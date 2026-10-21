/*
 * Player: the server-side object for ONE connected client.
 *
 * It lives inside its client thread (session.c), but other threads use it too:
 * the lobby lists it and pairs it, and its match thread sends it MATCH_START /
 * STATE / GAME_OVER. Three rules keep that safe:
 *   1. Fields marked [game_lock] are read or written only while holding
 *      srv->game_lock (see server.h).
 *   2. Every message to the client goes through player_send(), which holds
 *      send_lock: two threads never write to the same socket at the same time.
 *   3. While refs > 0, a match is still using this Player, so its client thread
 *      must not finish (the memory and the socket would disappear under the match).
 */
#ifndef PLAYER_H
#define PLAYER_H

#include <pthread.h>
#include <stddef.h>
#include <stdint.h>

#include "protocol.h"
#include "server.h"

struct Match;  /* defined in game.c; here we only need pointers to it */

/* Server's view of the client (README 2.3.7). */
typedef enum { ST_CONNECTED, ST_REGISTERED, ST_WAITING, ST_PLAYING } PlayerState;

typedef struct Player {
    Server          *srv;                  /* shared server context */
    int              fd;
    const char      *peer;                 /* "ip:port", for the log */
    char             nick[NICK_MAX + 1];   /* written once, when registering */
    char             email[EMAIL_MAX + 1];
    pthread_mutex_t  send_lock;            /* one writer at a time on fd */

    /* [game_lock] */
    PlayerState      state;
    struct Match    *match;                /* current match, or NULL */
    int              side;                 /* 0 = left, 1 = right (while match != NULL) */
    int              refs;                 /* matches still using this Player */
    struct Player   *next_waiting;         /* next available player in the lobby list */
} Player;

void player_init(Player *p, Server *srv, int fd, const char *peer);
void player_destroy(Player *p);

/* Sends one message to the client under send_lock. 0 on success, -1 on error
 * (already logged). The caller logs what it sent. */
int player_send(Player *p, MsgType type, const uint8_t *payload, size_t len);

const char *player_state_name(PlayerState st);

#endif /* PLAYER_H */
