/*
 * One session = everything the server does for ONE connected client:
 * read its messages, check them against the state machine of README 2.3.7,
 * and answer. It runs inside the client's own thread.
 */
#ifndef SESSION_H
#define SESSION_H

#include "server.h"

/*
 * Serves the client connected on `fd` until it leaves (QUIT, disconnection or
 * protocol error). `peer` is the client's "ip:port", used in every log line.
 * It does NOT close `fd`: the caller (the client thread) does.
 */
void session_run(Server *srv, int fd, const char *peer);

#endif /* SESSION_H */