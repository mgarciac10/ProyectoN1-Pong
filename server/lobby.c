/*
 * Lobby: list of available players (see lobby.h).
 *
 * The list is linked through Player->next_waiting and its head is
 * srv->waiting. Players are kept in the order they became available.
 */
#include "lobby.h"

#include <string.h>
#include <strings.h>   /* strcasecmp */

#include "game.h"
#include "logger.h"

/* Removes `p` from the list. Returns 1 if it was there. */
static int unlink_player(Server *srv, Player *p)
{
    for (Player **link = &srv->waiting; *link != NULL; link = &(*link)->next_waiting) {
        if (*link == p) {
            *link = p->next_waiting;
            p->next_waiting = NULL;
            return 1;
        }
    }
    return 0;
}

void lobby_join(Player *p)
{
    Server *srv = p->srv;

    /* Append at the end: the list shows players in the order they arrived. */
    Player **link = &srv->waiting;
    while (*link != NULL)
        link = &(*link)->next_waiting;
    *link = p;
    p->next_waiting = NULL;
    p->state = ST_WAITING;
    log_event(&srv->log, LOG_INFO, "%s '%s' is available (waiting to be chosen)", p->peer, p->nick);

    /* Sent while game_lock is held ON PURPOSE: nobody can choose this player
     * (and send it MATCH_START) before WAITING is on its way, so the client
     * always sees WAITING -> MATCH_START in that order. It is 4 bytes on an
     * idle socket, so send() returns immediately. */
    if (player_send(p, MSG_WAITING, NULL, 0) == 0)
        log_event(&srv->log, LOG_INFO, "%s SEND WAITING", p->peer);
}

void lobby_cancel(Player *p)
{
    if (unlink_player(p->srv, p))
        log_event(&p->srv->log, LOG_INFO, "%s '%s' left the list of available players", p->peer, p->nick);
}

size_t lobby_list(Player *p, uint8_t *out, size_t max)
{
    /* USER_LIST payload (README 2.3.4): count:u8, then count x (nick_len:u8, nick). */
    size_t  len   = 1;
    uint8_t count = 0;

    for (Player *q = p->srv->waiting; q != NULL && count < 255; q = q->next_waiting) {
        if (q == p)
            continue;                        /* nobody plays against themselves */
        size_t n = strlen(q->nick);
        if (len + 1 + n > max)
            break;                           /* does not fit in one message */
        out[len] = (uint8_t)n;
        memcpy(out + len + 1, q->nick, n);
        len += 1 + n;
        count++;
    }
    out[0] = count;
    return len;
}

int lobby_play_with(Player *p, const char *nick)
{
    Server *srv = p->srv;

    for (Player *q = srv->waiting; q != NULL; q = q->next_waiting) {
        if (q != p && strcasecmp(q->nick, nick) == 0) {
            unlink_player(srv, q);
            unlink_player(srv, p);           /* if p was available too */
            log_event(&srv->log, LOG_INFO, "%s '%s' chose '%s' as opponent", p->peer, p->nick, q->nick);
            match_create(q, p);              /* the one who was available plays on the left */
            return 0;
        }
    }
    return -1;
}
