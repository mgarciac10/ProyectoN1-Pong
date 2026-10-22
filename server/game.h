/*
 * Matches: creation, the match thread, and how a match ends.
 *
 * srv->game_lock (server.h) protects ALL the shared game state: the lobby, the
 * [game_lock] fields of every Player, and every Match. A single lock is
 * simple and enough for this project: the critical sections are tiny, and
 * sends to the network happen OUTSIDE it (except WAITING and USER_LIST, see
 * lobby.c and session.c).
 */
#ifndef GAME_H
#define GAME_H

#include <pthread.h>

#include "player.h"

/* Pairs two players: both become PLAYING and a match thread starts. The match
 * thread sends MATCH_START to both. Call with srv->game_lock held. */
void match_create(Player *left, Player *right);

/* Records the direction (0 = stop, 1 = up, 2 = down) requested by player `p`.
 * It only takes effect on the next tick. Silently ignored if `p` is not in a
 * match any more (an INPUT can cross a GAME_OVER). Takes srv->game_lock itself. */
void game_set_input(Player *p, uint8_t dir);

/* Called by a client thread when its session ends (QUIT, disconnection or
 * protocol error). Leaves the queue or the match (the opponent gets
 * GAME_OVER with reason 1), then BLOCKS until no match uses `p` any more.
 * After it returns, nobody else touches `p`. Takes srv->game_lock itself. */
void game_player_leave(Player *p);

#endif /* GAME_H */