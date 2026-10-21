/*
 * Player object (see player.h).
 */
#include "player.h"

#include <errno.h>
#include <string.h>

#include "logger.h"

void player_init(Player *p, Server *srv, int fd, const char *peer)
{
    memset(p, 0, sizeof *p);
    p->srv   = srv;
    p->fd    = fd;
    p->peer  = peer;
    p->state = ST_CONNECTED;
    pthread_mutex_init(&p->send_lock, NULL);
}

void player_destroy(Player *p)
{
    pthread_mutex_destroy(&p->send_lock);
}

int player_send(Player *p, MsgType type, const uint8_t *payload, size_t len)
{
    /* The client thread (REGISTER_OK, ERROR...), the lobby (WAITING) and the
     * match thread (MATCH_START, GAME_OVER...) may all send to this client.
     * Without the lock, the bytes of two messages could interleave on the wire. */
    pthread_mutex_lock(&p->send_lock);
    ProtoStatus st = proto_send(p->fd, type, payload, len);
    int saved_errno = errno;   /* unlock/log could overwrite errno */
    pthread_mutex_unlock(&p->send_lock);

    if (st != PROTO_OK) {
        log_event(&p->srv->log, LOG_ERROR, "%s send %s: %s", p->peer, proto_type_name(type), strerror(saved_errno));
        return -1;
    }
    return 0;
}

const char *player_state_name(PlayerState st)
{
    switch (st) {
    case ST_CONNECTED:  return "CONNECTED";
    case ST_REGISTERED: return "REGISTERED";
    case ST_WAITING:    return "WAITING";
    case ST_PLAYING:    return "PLAYING";
    }
    return "?";
}
