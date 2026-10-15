/*
 * Matchmaking queue (README 2.3.7, rule 2): first come, first served.
 * There is at most ONE waiting player: as soon as a second one joins, both
 * are paired. Both functions must be called with game_lock held.
 */
#ifndef LOBBY_H
#define LOBBY_H

#include "player.h"

/* A REGISTERED player asks for a match: it becomes WAITING (and receives
 * WAITING), or it is paired with the waiting player and a match starts. */
void lobby_join(Player *p);

/* A WAITING player leaves (QUIT or disconnection): take it out of the queue,
 * so nobody is ever paired with a ghost. */
void lobby_cancel(Player *p);

#endif /* LOBBY_H */
