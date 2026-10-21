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

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>  /* shutdown */
#include <time.h>        /* clock_gettime, clock_nanosleep */

#include "lobby.h"
#include "logger.h"

#define TICKS_PER_SECOND 30                       /* README 2.3.6 */
#define TICK_NS          (1000000000L / TICKS_PER_SECOND)

/* Game constants (README 2.3.6): the same values the client uses. */
#define FIELD_W        800
#define FIELD_H        600
#define PADDLE_W       10
#define PADDLE_H       80
#define PADDLE_X_LEFT  20
#define PADDLE_X_RIGHT 770
#define BALL_SIZE      10
#define WIN_SCORE      5

/* Tuning that only the server knows (units per tick; x30 = units per second). */
#define PADDLE_SPEED     9.0   /* 270 units/s */
#define BALL_SPEED_START 8.0
#define BALL_SPEED_MAX   16.0  /* must stay < PADDLE_W + BALL_SIZE, or the ball could skip over a paddle */
#define BALL_SPEEDUP     1.06  /* the ball gets 6% faster at every paddle hit */
#define BALL_MAX_VY      9.0
#define SERVE_DELAY_TICKS 20   /* ball waits ~0.66 s in the centre after each goal */

/* GAME_OVER reasons (README 2.3.4). */
#define REASON_SCORE         0
#define REASON_OPPONENT_LEFT 1

typedef struct Match {
    Server *srv;       /* shared server context (game_lock, logger...) */
    Player *p[2];      /* p[0] = left, p[1] = right. Fixed after creation. */
    int     gone[2];   /* [game_lock] 1 once that player quit or disconnected */
    int     dir[2];    /* [game_lock] last INPUT of each player: 0 stop, 1 up, 2 down */

    /* Simulation: touched ONLY by the match thread, so it needs no lock. */
    double  bx, by;    /* ball's top-left corner */
    double  vx, vy;    /* ball velocity, units per tick */
    double  py[2];     /* paddles' top edge */
    int     score[2];
    int     serve_delay;   /* ticks left before the ball starts moving */
    unsigned seed;         /* for rand_r (thread-safe, unlike rand) */
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
        log_event(&me->srv->log, LOG_INFO, "%s SEND MATCH_START side=%d opponent=%s", me->peer, side, opp->nick);
}

static void send_game_over(Player *p, int winner, int reason)
{
    uint8_t payload[2] = { (uint8_t)winner, (uint8_t)reason };
    if (player_send(p, MSG_GAME_OVER, payload, sizeof payload) == 0)
        log_event(&p->srv->log, LOG_INFO, "%s SEND GAME_OVER winner=%d reason=%d (%s)", p->peer, winner, reason,
                reason == REASON_SCORE ? "score" : "opponent left");
}

/* ============================================================== simulation */

/* Puts the ball in the centre and serves it toward `toward` (0 = left, 1 = right). */
static void serve(Match *m, int toward)
{
    m->bx = (FIELD_W - BALL_SIZE) / 2.0;
    m->by = (FIELD_H - BALL_SIZE) / 2.0;
    m->vx = (toward == 0) ? -BALL_SPEED_START : BALL_SPEED_START;
    m->vy = (double)((int)(rand_r(&m->seed) % 9) - 4);   /* -4 .. +4 */
    m->serve_delay = SERVE_DELAY_TICKS;
}

static void match_init_game(Match *m)
{
    m->seed = (unsigned)time(NULL) ^ (unsigned)(size_t)m;
    m->py[0] = m->py[1] = (FIELD_H - PADDLE_H) / 2.0;
    m->score[0] = m->score[1] = 0;
    serve(m, (int)(rand_r(&m->seed) % 2));
}

/* The ball hits a paddle whose top edge is `paddle_y`: bounce it and send it
 * back faster, at an angle that depends on WHERE it hit (centre = straight,
 * edges = steep). `new_vx_sign` is the new horizontal direction. */
static void bounce_on_paddle(Match *m, double paddle_y, int new_vx_sign)
{
    double ball_c   = m->by + BALL_SIZE / 2.0;
    double paddle_c = paddle_y + PADDLE_H / 2.0;
    double offset   = (ball_c - paddle_c) / (PADDLE_H / 2.0 + BALL_SIZE / 2.0);  /* about -1 .. +1 */
    if (offset < -1.0) offset = -1.0;
    if (offset >  1.0) offset =  1.0;

    double speed = (m->vx < 0 ? -m->vx : m->vx) * BALL_SPEEDUP;
    if (speed > BALL_SPEED_MAX) speed = BALL_SPEED_MAX;

    m->vx = new_vx_sign * speed;
    m->vy = offset * BALL_MAX_VY;
}

/* Advances the game ONE tick with the players' directions `dir`.
 * Returns the winner's side if somebody reached WIN_SCORE, otherwise -1. */
static int step_game(Match *m, const int dir[2])
{
    /* 1. Paddles: INPUT only chose a direction, the server moves them. */
    for (int s = 0; s < 2; s++) {
        if (dir[s] == 1)      m->py[s] -= PADDLE_SPEED;
        else if (dir[s] == 2) m->py[s] += PADDLE_SPEED;
        if (m->py[s] < 0)                   m->py[s] = 0;
        if (m->py[s] > FIELD_H - PADDLE_H)  m->py[s] = FIELD_H - PADDLE_H;
    }

    /* 2. Short pause after each serve. */
    if (m->serve_delay > 0) {
        m->serve_delay--;
        return -1;
    }

    /* 3. Ball. */
    m->bx += m->vx;
    m->by += m->vy;

    if (m->by < 0) {                                  /* top wall */
        m->by = -m->by;
        m->vy = -m->vy;
    } else if (m->by > FIELD_H - BALL_SIZE) {         /* bottom wall */
        m->by = 2.0 * (FIELD_H - BALL_SIZE) - m->by;
        m->vy = -m->vy;
    }

    /* Left paddle: occupies x in [20, 30). Only a ball moving left can hit it. */
    if (m->vx < 0 &&
        m->bx < PADDLE_X_LEFT + PADDLE_W && m->bx + BALL_SIZE > PADDLE_X_LEFT &&
        m->by + BALL_SIZE > m->py[0] && m->by < m->py[0] + PADDLE_H) {
        m->bx = PADDLE_X_LEFT + PADDLE_W;
        bounce_on_paddle(m, m->py[0], +1);
    }
    /* Right paddle: occupies x in [770, 780). Only a ball moving right can hit it. */
    else if (m->vx > 0 &&
            m->bx + BALL_SIZE > PADDLE_X_RIGHT && m->bx < PADDLE_X_RIGHT + PADDLE_W &&
            m->by + BALL_SIZE > m->py[1] && m->by < m->py[1] + PADDLE_H) {
        m->bx = PADDLE_X_RIGHT - BALL_SIZE;
        bounce_on_paddle(m, m->py[1], -1);
    }

    /* 4. Goals: the ball left the field through a side. */
    int scorer = -1;
    if (m->bx + BALL_SIZE < 0)       scorer = 1;      /* went out on the left: right scores */
    else if (m->bx > FIELD_W)        scorer = 0;      /* went out on the right: left scores */

    if (scorer >= 0) {
        m->score[scorer]++;
        log_event(&m->srv->log, LOG_INFO, "goal: '%s' scores (%d-%d) in '%s' vs '%s'", m->p[scorer]->nick,
                  m->score[0], m->score[1], m->p[0]->nick, m->p[1]->nick);
        if (m->score[scorer] >= WIN_SCORE)
            return scorer;
        serve(m, 1 - scorer);                         /* serve toward the one who conceded */
    }
    return -1;
}

/* Position -> u16 for the wire (positions are integers, README 2.3.6). */
static uint16_t to_u16(double v)
{
    if (v < 0) v = 0;
    if (v > 65535) v = 65535;
    return (uint16_t)(v + 0.5);
}

static void put_u16(uint8_t *out, uint16_t v)
{
    out[0] = (uint8_t)(v >> 8);       /* big endian, like every field on the wire */
    out[1] = (uint8_t)(v & 0xFF);
}

/* STATE (README 2.3.4): ball_x, ball_y, left_y, right_y (u16) + score_l, score_r (u8).
 * Not logged: it is sent 30 times per second (README logger section). */
static void send_state(Match *m, const int gone[2])
{
    uint8_t payload[10];
    put_u16(payload + 0, to_u16(m->bx));
    put_u16(payload + 2, to_u16(m->by));
    put_u16(payload + 4, to_u16(m->py[0]));
    put_u16(payload + 6, to_u16(m->py[1]));
    payload[8] = (uint8_t)m->score[0];
    payload[9] = (uint8_t)m->score[1];

    for (int side = 0; side < 2; side++)
        if (!gone[side])
            player_send(m->p[side], MSG_STATE, payload, sizeof payload);
}

/* ================================================================ ending */

static void match_end(Match *m, int winner, int reason)
{
    int notify[2];

    /* 1. Back to REGISTERED (they may JOIN again) BEFORE sending GAME_OVER:
     *    a client that reacts to GAME_OVER with JOIN must find the right state. */
    pthread_mutex_lock(&m->srv->game_lock);
    for (int side = 0; side < 2; side++) {
        Player *p = m->p[side];
        notify[side] = !m->gone[side];
        p->match = NULL;
        if (notify[side])
            p->state = ST_REGISTERED;
    }
    pthread_mutex_unlock(&m->srv->game_lock);

    log_event(&m->srv->log, LOG_INFO, "match '%s' vs '%s' over: winner=%s (%s)", m->p[0]->nick, m->p[1]->nick,
            m->p[winner]->nick, reason == REASON_SCORE ? "score" : "opponent left");

    /* 2. Tell the players who are still connected. They cannot vanish meanwhile:
     *    our references (refs) keep their client threads alive. */
    for (int side = 0; side < 2; side++)
        if (notify[side])
            send_game_over(m->p[side], winner, reason);

    /* 3. Drop our references and wake up any client thread waiting to finish. */
    pthread_mutex_lock(&m->srv->game_lock);
    m->p[0]->refs--;
    m->p[1]->refs--;
    pthread_cond_broadcast(&m->srv->players_released);
    pthread_mutex_unlock(&m->srv->game_lock);

    free(m);
}

/* ========================================================== match thread */

/* Adds one tick to `t` (a timespec). */
static void add_tick(struct timespec *t)
{
    t->tv_nsec += TICK_NS;
    if (t->tv_nsec >= 1000000000L) {
        t->tv_nsec -= 1000000000L;
        t->tv_sec++;
    }
}

static void *match_thread(void *arg)
{
    Match *m = arg;

    send_match_start(m, 0);
    send_match_start(m, 1);

    /* Absolute deadlines: each tick ends at start + n * 33 ms, so the time spent
     * simulating and sending does not accumulate as drift (a plain sleep(33 ms)
     * would make the game slower than 30 ticks/s). */
    struct timespec next;
    clock_gettime(CLOCK_MONOTONIC, &next);

    int gone[2];
    int dir[2];
    for (;;) {
        /* Take a snapshot of the shared fields; simulate and send OUTSIDE the lock. */
        pthread_mutex_lock(&m->srv->game_lock);
        gone[0] = m->gone[0];
        gone[1] = m->gone[1];
        dir[0]  = m->dir[0];
        dir[1]  = m->dir[1];
        pthread_mutex_unlock(&m->srv->game_lock);

        if (gone[0] || gone[1])
            break;

        int winner = step_game(m, dir);
        send_state(m, gone);          /* the last STATE shows the final score */

        if (winner >= 0) {
            match_end(m, winner, REASON_SCORE);
            return NULL;
        }

        add_tick(&next);
        while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &next, NULL) == EINTR)
            ;
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
        log_event(&left->srv->log, LOG_ERROR, "out of memory creating a match");
        abort_pairing(left, right);
        return;
    }
    m->p[0] = left;
    m->p[1] = right;
    m->srv  = left->srv;
    match_init_game(m);

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
        log_event(&left->srv->log, LOG_ERROR, "pthread_create (match): %s", strerror(err));
        for (int side = 0; side < 2; side++) {
            m->p[side]->match = NULL;
            m->p[side]->refs--;
        }
        free(m);
        abort_pairing(left, right);
        return;
    }
    pthread_detach(tid);
    log_event(&left->srv->log, LOG_INFO, "match started: '%s' (left, %s) vs '%s' (right, %s)",
            left->nick, left->peer, right->nick, right->peer);
}

void game_set_input(Player *p, uint8_t dir)
{
    pthread_mutex_lock(&p->srv->game_lock);
    if (p->state == ST_PLAYING && p->match != NULL)
        p->match->dir[p->side] = dir;
    pthread_mutex_unlock(&p->srv->game_lock);
}

void game_player_leave(Player *p)
{
    pthread_mutex_lock(&p->srv->game_lock);

    if (p->state == ST_WAITING) {
        lobby_cancel(p);
    } else if (p->state == ST_PLAYING && p->match != NULL) {
        /* The match thread notices it on its next tick and ends the match. */
        p->match->gone[p->side] = 1;
        log_event(&p->srv->log, LOG_INFO, "%s '%s' left the match", p->peer, p->nick);
    }

    /* Wait until no match uses p. pthread_cond_wait releases game_lock while
     * sleeping, so the match thread can take it and finish. */
    while (p->refs > 0)
        pthread_cond_wait(&p->srv->players_released, &p->srv->game_lock);

    pthread_mutex_unlock(&p->srv->game_lock);
}