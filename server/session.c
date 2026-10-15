/*
 * Per-client session (see session.h).
 *
 * Implements the server side of the state machine in README 2.3.7:
 *
 *     CONNECTED --REGISTER--> REGISTERED --JOIN--> WAITING --> PLAYING
 *
 * The client thread handles REGISTER, JOIN and QUIT. Pairing is done by the
 * lobby, and the match itself by its own thread (game.c). INPUT arrives in phase 6.
 *
 * The player's state is shared with other threads (the lobby and the match
 * thread change it), so it is always read and written under game_lock.
 */
#include "session.h"

#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>     /* strcasecmp */
#include <sys/socket.h>  /* shutdown, recv, setsockopt */
#include <sys/time.h>    /* struct timeval */
#include <time.h>        /* time */

#include "game.h"
#include "lobby.h"
#include "logger.h"
#include "player.h"
#include "protocol.h"

/* ============================================================ nickname registry
 *
 * The nickname must be unique among CONNECTED players (README rule 1), compared
 * ignoring upper/lower case. The registry is shared by all client threads, so
 * every access is protected by a mutex. Without it, two clients registering
 * "ana" at the same moment could both find the nick free and both succeed
 * (a classic race condition: check-then-act). The mutex makes "look for the
 * nick + insert it" ONE indivisible step.
 */
typedef struct NickNode {
    char             nick[NICK_MAX + 1];
    struct NickNode *next;
} NickNode;

static NickNode        *registry_head = NULL;
static pthread_mutex_t  registry_lock = PTHREAD_MUTEX_INITIALIZER;

typedef enum { REG_OK, REG_TAKEN, REG_NO_MEMORY } RegResult;

static RegResult registry_add(const char *nick)
{
    RegResult result = REG_OK;

    pthread_mutex_lock(&registry_lock);
    for (NickNode *n = registry_head; n != NULL; n = n->next) {
        if (strcasecmp(n->nick, nick) == 0) {
            result = REG_TAKEN;
            break;
        }
    }
    if (result == REG_OK) {
        NickNode *node = malloc(sizeof *node);
        if (node == NULL) {
            result = REG_NO_MEMORY;
        } else {
            snprintf(node->nick, sizeof node->nick, "%s", nick);
            node->next = registry_head;
            registry_head = node;
        }
    }
    pthread_mutex_unlock(&registry_lock);
    return result;
}

static void registry_remove(const char *nick)
{
    pthread_mutex_lock(&registry_lock);
    NickNode **link = &registry_head;      /* pointer to the pointer that points to n */
    while (*link != NULL) {
        if (strcasecmp((*link)->nick, nick) == 0) {
            NickNode *dead = *link;
            *link = dead->next;            /* unlink it from the list... */
            free(dead);                    /* ...and give the memory back */
            break;
        }
        link = &(*link)->next;
    }
    pthread_mutex_unlock(&registry_lock);
}

/* ================================================================ session data */

/* What to do with the connection after handling a message. */
typedef enum { ACTION_KEEP, ACTION_CLOSE } Action;

/* The player's state, read under game_lock (other threads may change it). */
static PlayerState current_state(Player *s)
{
    pthread_mutex_lock(&game_lock);
    PlayerState st = s->state;
    pthread_mutex_unlock(&game_lock);
    return st;
}

/* Which messages does the table in README 2.3.7 allow in each state? */
static int allowed_in_state(PlayerState st, uint8_t type)
{
    switch (st) {
    case ST_CONNECTED:  return type == MSG_REGISTER || type == MSG_QUIT;
    case ST_REGISTERED: return type == MSG_JOIN     || type == MSG_QUIT;
    case ST_WAITING:    return type == MSG_QUIT;
    case ST_PLAYING:    return type == MSG_INPUT    || type == MSG_QUIT;
    }
    return 0;
}

/* ================================================================== sending */

/* Every send goes through player_send(): other threads may write to this
 * client too (WAITING from the lobby, MATCH_START from the match thread). */

static int send_empty(Player *s, MsgType type)
{
    if (player_send(s, type, NULL, 0) < 0)
        return -1;
    log_event(LOG_INFO, "%s SEND %s", s->peer, proto_type_name(type));
    return 0;
}

static int send_error(Player *s, ErrorCode code)
{
    uint8_t payload[1] = { (uint8_t)code };
    if (player_send(s, MSG_ERROR, payload, sizeof payload) < 0)
        return -1;
    log_event(LOG_INFO, "%s SEND ERROR code=%d (%s)", s->peer, (int)code, proto_error_name((uint8_t)code));
    return 0;
}

/*
 * Rejects the last message with ERROR(code). README 2.3.5: errors 1 and 2 mean
 * the byte stream can no longer be trusted, so the connection is closed; errors
 * 3, 4 and 5 are reported and the connection stays open.
 */
static Action reply_error(Player *s, ErrorCode code, const char *why)
{
    log_event(LOG_WARN, "%s rejected: %s", s->peer, why);
    int send_failed = send_error(s, code) < 0;
    int fatal = (code == ERR_BAD_VERSION || code == ERR_BAD_MESSAGE);
    return (send_failed || fatal) ? ACTION_CLOSE : ACTION_KEEP;
}

/*
 * Closing a socket that still has UNREAD incoming data makes Linux send a RST
 * (an abrupt reset) instead of a normal FIN, and the peer may lose the ERROR we
 * just sent. So, when the SERVER decides to close: tell the peer we are done
 * writing (shutdown), then read and discard what it still sends, for at most
 * ~3 seconds, and only then let the caller close().
 */
static void lingering_close(int fd)
{
    shutdown(fd, SHUT_WR);                      /* sends our FIN after the pending data */

    struct timeval tv = { .tv_sec = 1, .tv_usec = 0 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);

    char   junk[512];
    size_t total = 0;
    time_t deadline = time(NULL) + 2;
    while (total < 4096 && time(NULL) <= deadline) {
        ssize_t n = recv(fd, junk, sizeof junk, 0);
        if (n <= 0)
            break;                               /* EOF, error or timeout */
        total += (size_t)n;
    }
}

/* ================================================================== handlers */

static Action handle_register(Player *s, const ProtoHeader *hdr, const uint8_t *payload)
{
    char nick[NICK_MAX + 1];
    char email[EMAIL_MAX + 1];
    const char *why = "";

    DecodeStatus ds = proto_decode_register(payload, hdr->length, nick, email, &why);
    if (ds == DECODE_BAD_LAYOUT)
        return reply_error(s, ERR_BAD_MESSAGE, why);
    if (ds == DECODE_BAD_FIELD)
        return reply_error(s, ERR_INVALID_FIELD, why);

    /* The fields passed validation, so it is safe to write them in the log
     * (a nick has only letters/digits/_/-; an e-mail only printable ASCII). */
    log_event(LOG_INFO, "%s REGISTER nick=%s email=%s", s->peer, nick, email);

    RegResult r = registry_add(nick);
    if (r == REG_TAKEN) {
        char msg[64];
        snprintf(msg, sizeof msg, "nickname '%s' is already in use", nick);
        return reply_error(s, ERR_NICK_TAKEN, msg);
    }
    if (r == REG_NO_MEMORY) {
        log_event(LOG_ERROR, "%s out of memory while registering", s->peer);
        return ACTION_CLOSE;
    }

    snprintf(s->nick, sizeof s->nick, "%s", nick);
    snprintf(s->email, sizeof s->email, "%s", email);
    pthread_mutex_lock(&game_lock);
    s->state = ST_REGISTERED;
    pthread_mutex_unlock(&game_lock);
    log_event(LOG_INFO, "%s registered as '%s'", s->peer, s->nick);

    return send_empty(s, MSG_REGISTER_OK) < 0 ? ACTION_CLOSE : ACTION_KEEP;
}

static Action handle_join(Player *s, const ProtoHeader *hdr)
{
    if (hdr->length != 0)
        return reply_error(s, ERR_BAD_MESSAGE, "JOIN must have an empty payload");

    /* The lobby answers: WAITING now, or MATCH_START (sent by the new match
     * thread) if somebody was already waiting. */
    pthread_mutex_lock(&game_lock);
    if (s->state == ST_REGISTERED)
        lobby_join(s);
    pthread_mutex_unlock(&game_lock);
    return ACTION_KEEP;
}

static Action handle_quit(Player *s, const ProtoHeader *hdr)
{
    if (hdr->length != 0)
        return reply_error(s, ERR_BAD_MESSAGE, "QUIT must have an empty payload");

    log_event(LOG_INFO, "%s QUIT", s->peer);
    return ACTION_CLOSE;
}

/*
 * Checks one valid-looking message against the session state and runs it.
 * Order of checks (README 2.3.5): header (already done by proto_recv)
 * -> state -> layout -> field rules.
 */
static Action handle_message(Player *s, const ProtoHeader *hdr, const uint8_t *payload)
{
    char hex[128];
    proto_format_frame(hdr, payload, hex, sizeof hex);
    log_event(LOG_INFO, "%s RECV %s [%s]", s->peer, proto_type_name(hdr->type), hex);

    PlayerState st = current_state(s);

    /* README rule 7: a late INPUT is silently discarded, never answered with an
     * error (it may legitimately cross a GAME_OVER already on its way). */
    if (hdr->type == MSG_INPUT && st != ST_PLAYING) {
        log_event(LOG_INFO, "%s INPUT ignored (state %s)", s->peer, player_state_name(st));
        return ACTION_KEEP;
    }

    if (!allowed_in_state(st, hdr->type)) {
        char msg[80];
        snprintf(msg, sizeof msg, "%s is not allowed in state %s",
                proto_type_name(hdr->type), player_state_name(st));
        return reply_error(s, ERR_INVALID_STATE, msg);
    }

    switch (hdr->type) {
    case MSG_REGISTER: return handle_register(s, hdr, payload);
    case MSG_JOIN:     return handle_join(s, hdr);
    case MSG_QUIT:     return handle_quit(s, hdr);
    default:
        /* MSG_INPUT while PLAYING (phase 6). proto_recv only lets client types through. */
        return ACTION_KEEP;
    }
}

/* ================================================================== main loop */

void session_run(int fd, const char *peer)
{
    /* The Player lives on this thread's stack. That is safe even though other
     * threads use it: game_player_leave() below does not return until no match
     * references it any more. */
    Player s;
    player_init(&s, fd, peer);

    int server_closes = 0;   /* did WE decide to end the connection? */

    for (;;) {
        ProtoHeader hdr;
        uint8_t     payload[PROTO_MAX_PAYLOAD];
        char        why[64];

        ProtoStatus st = proto_recv(fd, &hdr, payload, sizeof payload);

        if (st == PROTO_OK) {
            if (handle_message(&s, &hdr, payload) == ACTION_CLOSE) {
                server_closes = 1;
                break;
            }
            continue;
        }
        if (st == PROTO_CLOSED) {
            log_event(LOG_INFO, "%s closed the connection", peer);
            break;
        }
        if (st == PROTO_IO_ERROR) {
            /* Also what we see when keepalive / TCP_USER_TIMEOUT declare a client dead. */
            log_event(LOG_ERROR, "%s recv: %s", peer, strerror(errno));
            break;
        }

        /* Malformed header: the stream is out of sync, answer and close. */
        if (st == PROTO_BAD_VERSION) {
            snprintf(why, sizeof why, "unsupported protocol version %u", (unsigned)hdr.version);
            reply_error(&s, ERR_BAD_VERSION, why);
        } else if (st == PROTO_BAD_LENGTH) {
            snprintf(why, sizeof why, "payload length %u is above the limit of %d",
                    (unsigned)hdr.length, PROTO_MAX_PAYLOAD);
            reply_error(&s, ERR_BAD_MESSAGE, why);
        } else {  /* PROTO_BAD_TYPE */
            snprintf(why, sizeof why, "unknown message type 0x%02x", (unsigned)hdr.type);
            reply_error(&s, ERR_BAD_MESSAGE, why);
        }
        server_closes = 1;
        break;
    }

    /* Leave the queue or the match (the opponent gets GAME_OVER) and wait until
     * no match uses this Player. From here on, only this thread touches `s`. */
    game_player_leave(&s);

    /* The nickname is released as soon as the session ends, however the player
     * left. Do it BEFORE lingering_close(), which may wait up to ~3 s: the player
     * is already gone for the protocol, so its nick must be free right away. */
    if (s.nick[0] != '\0') {   /* set only after a successful REGISTER */
        registry_remove(s.nick);
        log_event(LOG_INFO, "%s nickname '%s' released", peer, s.nick);
    }

    if (server_closes)
        lingering_close(fd);

    player_destroy(&s);
}