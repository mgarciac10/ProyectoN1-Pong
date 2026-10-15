/*
 * Matchmaking queue (see lobby.h).
 */
#include "lobby.h"

#include "game.h"
#include "logger.h"

static Player *waiting = NULL;   /* [game_lock] the player waiting for an opponent */

void lobby_join(Player *p)
{
    if (waiting == NULL) {
        waiting  = p;
        p->state = ST_WAITING;
        log_event(LOG_INFO, "%s '%s' is waiting for an opponent", p->peer, p->nick);

        /* Sent while game_lock is held ON PURPOSE: nobody can pair this player
         * (and send it MATCH_START) before WAITING is on its way, so the client
         * always sees WAITING -> MATCH_START in that order. It is 4 bytes on an
         * idle socket, so send() returns immediately. */
        if (player_send(p, MSG_WAITING, NULL, 0) == 0)
            log_event(LOG_INFO, "%s SEND WAITING", p->peer);
        return;
    }

    /* Somebody is waiting: pair them. Whoever waited longer plays on the left. */
    Player *opponent = waiting;
    waiting = NULL;
    match_create(opponent, p);
}

void lobby_cancel(Player *p)
{
    if (waiting == p) {
        waiting = NULL;
        log_event(LOG_INFO, "%s '%s' left the queue", p->peer, p->nick);
    }
}
