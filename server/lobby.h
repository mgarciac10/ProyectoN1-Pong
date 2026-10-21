/*
 * Lobby: the list of AVAILABLE players (README 2.3.7, rule 2).
 *
 * A registered player becomes available with JOIN (it is added to the list and
 * receives WAITING). Any other player sees the list with LIST and chooses its
 * opponent with PLAY_WITH: if the chosen player is still available, the match
 * starts at once. All functions must be called with srv->game_lock held.
 */
#ifndef LOBBY_H
#define LOBBY_H

#include <stddef.h>
#include <stdint.h>

#include "player.h"

/* REGISTERED -> WAITING: add `p` to the list of available players and send it WAITING. */
void lobby_join(Player *p);

/* An available player leaves (QUIT or disconnection): take it out of the list,
 * so nobody can ever choose a ghost. Does nothing if `p` is not in the list. */
void lobby_cancel(Player *p);

/* Builds the USER_LIST payload for `p` (everyone available except `p`) in `out`
 * (`max` bytes). Returns the payload length. Nicks that do not fit are left out. */
size_t lobby_list(Player *p, uint8_t *out, size_t max);

/* `p` chooses `nick` as its opponent. If that player is available (and is not
 * `p` itself), both leave the list and the match starts: returns 0. Otherwise -1. */
int lobby_play_with(Player *p, const char *nick);

#endif /* LOBBY_H */
