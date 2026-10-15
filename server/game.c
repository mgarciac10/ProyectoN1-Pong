/*
 * Matches (see game.h).
 *
 * Lifetime of a Match:
 *   1. match_create() (in the thread of the player who joined second)
 *      allocates it and starts its match thread.
 *   2. The match thread sends MATCH_START to both players and runs one loop
 *      iteration per tick until the match ends.
 *   3. match_end() puts the players back in REGISTERED, sends GAME_OVER, drops
 *      the match's references to the players and frees the Match.
 * The match thread is the ONLY one that sends match messages (MATCH_START,
 * STATE, GAME_OVER), so they always arrive in the right order.
 */
#include "game.h"

#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>  /* shutdown */
#include <time.h>        /* nanosleep */

#include "lobby.h"
#include "logger.h"

#define TICKS_PER_SECOND 30                       /* README 2.3.6 */
#define TICK_NS          (1000000000L / TICKS_PER_SECOND)

/* GAME_OVER reasons (README 2.3.4). */
#define REASON_SCORE         0
#define REASON_OPPONENT_LEFT 1

pthread_mutex_t game_lock = PTHREAD_MUTEX_INITIALIZER;

/* Signalled every time a match drops its references to its players
 * (a leaving client thread waits on it in game_player_leave). */
static pthread_cond_t players_released = PTHREAD_COND_INITIALIZER;

typedef struct Match {
    Player *p[2];      /* p[0] = left, p[1] = right. Fixed after creation. */
    int     gone[2];   /* [game_lock] 1 once that player quit or disconnected */
} Match;

/* ============================================================ match messages */

static void send_match_start(Match *m, int side)
{
    Player *me  = m->p[side];
    Player *opp = m->p[1 - side];

    /* side:u8, opp_len:u8, opp_nick (README 2.3.4) */
    uint8_t payload[2 + NICK_MAX];
    size_t  n = strlen(opp->nick);
    payload[0] = (uint8_t)side;
    payload[1] = (uint8_t)n;
    memcpy(payload + 2, opp->nick, n);

    if (player_send(me, MSG_MATCH_START, payload, 2 + n) == 0)
        log_event(LOG_INFO, "%s SEND MATCH_START side=%d opponent=%s", me->peer, side, opp->nick);
}

static void send_game_over(Player *p, int winner, int reason)
{
    uint8_t payload[2] = { (uint8_t)winner, (uint8_t)reason };
    if (player_send(p, MSG_GAME_OVER, payload, sizeof payload) == 0)
        log_event(LOG_INFO, "%s SEND GAME_OVER winner=%d reason=%d (%s)", p->peer, winner, reason,
                  reason == REASON_SCORE ? "score" : "opponent left");
}

/* ================================================================ ending */

static void match_end(Match *m, int winner, int reason)
{
    int notify[2];

    /* 1. Back to REGISTERED (they may JOIN again) BEFORE sending GAME_OVER:
     *    a client that reacts to GAME_OVER with JOIN must find the right state. */
    pthread_mutex_lock(&game_lock);
    for (int side = 0; side < 2; side++) {
        Player *p = m->p[side];
        notify[side] = !m->gone[side];
        p->match = NULL;
        if (notify[side])
            p->state = ST_REGISTERED;
    }
    pthread_mutex_unlock(&game_lock);

    log_event(LOG_INFO, "match '%s' vs '%s' over: winner=%s (%s)", m->p[0]->nick, m->p[1]->nick,
              m->p[winner]->nick, reason == REASON_SCORE ? "score" : "opponent left");

    /* 2. Tell the players who are still connected. They cannot vanish meanwhile:
     *    our references (refs) keep their client threads alive. */
    for (int side = 0; side < 2; side++)
        if (notify[side])
            send_game_over(m->p[side], winner, reason);

    /* 3. Drop our references and wake up any client thread waiting to finish. */
    pthread_mutex_lock(&game_lock);
    m->p[0]->refs--;
    m->p[1]->refs--;
    pthread_cond_broadcast(&players_released);
    pthread_mutex_unlock(&game_lock);

    free(m);
}

/* ========================================================== match thread */

static void sleep_one_tick(void)
{
    struct timespec ts = { .tv_sec = 0, .tv_nsec = TICK_NS };
    nanosleep(&ts, NULL);
}

static void *match_thread(void *arg)
{
    Match *m = arg;

    send_match_start(m, 0);
    send_match_start(m, 1);

    /* One iteration per tick (~33 ms). Phase 6 adds here: apply the inputs,
     * move ball and paddles, detect goals, send STATE to both players. */
    int gone[2];
    for (;;) {
        pthread_mutex_lock(&game_lock);
        gone[0] = m->gone[0];
        gone[1] = m->gone[1];
        pthread_mutex_unlock(&game_lock);

        if (gone[0] || gone[1])
            break;
        sleep_one_tick();
    }

    /* The player who stayed wins (README rule 6). */
    match_end(m, gone[0] ? 1 : 0, REASON_OPPONENT_LEFT);
    return NULL;
}

/* ============================================================ public API */

/* Pairing failed (out of memory or threads): undo it and close both
 * connections, so the clients are not left waiting forever. */
static void abort_pairing(Player *left, Player *right)
{
    Player *both[2] = { left, right };
    for (int i = 0; i < 2; i++) {
        both[i]->state = ST_REGISTERED;
        shutdown(both[i]->fd, SHUT_RDWR);  /* its session sees the connection end */
    }
}

void match_create(Player *left, Player *right)
{
    Match *m = calloc(1, sizeof *m);
    if (m == NULL) {
        log_event(LOG_ERROR, "out of memory creating a match");
        abort_pairing(left, right);
        return;
    }
    m->p[0] = left;
    m->p[1] = right;

    for (int side = 0; side < 2; side++) {
        Player *p = m->p[side];
        p->state = ST_PLAYING;
        p->match = m;
        p->side  = side;
        p->refs++;           /* this match now uses p */
    }

    pthread_t tid;
    int err = pthread_create(&tid, NULL, match_thread, m);
    if (err != 0) {
        log_event(LOG_ERROR, "pthread_create (match): %s", strerror(err));
        for (int side = 0; side < 2; side++) {
            m->p[side]->match = NULL;
            m->p[side]->refs--;
        }
        free(m);
        abort_pairing(left, right);
        return;
    }
    pthread_detach(tid);
    log_event(LOG_INFO, "match started: '%s' (left, %s) vs '%s' (right, %s)",
              left->nick, left->peer, right->nick, right->peer);
}

void game_player_leave(Player *p)
{
    pthread_mutex_lock(&game_lock);

    if (p->state == ST_WAITING) {
        lobby_cancel(p);
    } else if (p->state == ST_PLAYING && p->match != NULL) {
        /* The match thread notices it on its next tick and ends the match. */
        p->match->gone[p->side] = 1;
        log_event(LOG_INFO, "%s '%s' left the match", p->peer, p->nick);
    }

    /* Wait until no match uses p. pthread_cond_wait releases game_lock while
     * sleeping, so the match thread can take it and finish. */
    while (p->refs > 0)
        pthread_cond_wait(&players_released, &game_lock);

    pthread_mutex_unlock(&game_lock);
}
