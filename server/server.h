/*
 * Server context.
 *
 * The server has NO global variables: all the state shared between threads
 * lives in this one struct. main() creates it (on its own stack: main never
 * returns while clients are served) and every module receives a pointer to it,
 * directly or through Player->srv. Each field says which lock protects it.
 */
#ifndef SERVER_H
#define SERVER_H

#include <pthread.h>

#include "logger.h"

struct NickNode;   /* session.c */
struct Player;     /* player.h  */

typedef struct Server {
    Logger           log;               /* console + log file (has its own lock) */

    /* Registry of connected nicknames (session.c). */
    struct NickNode *nicks;             /* [nicks_lock] linked list */
    pthread_mutex_t  nicks_lock;

    /* Game state (lobby.c, game.c). */
    pthread_mutex_t  game_lock;         /* protects every field marked [game_lock] */
    pthread_cond_t   players_released;  /* signalled when a match drops its players */
    struct Player   *waiting;           /* [game_lock] lobby: available players (list) */
} Server;

#endif /* SERVER_H */
