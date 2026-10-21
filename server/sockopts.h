/*
 * Socket options applied to every accepted client connection
 * (see README section 2.2 and "Liveness" in 2.3.7).
 */
#ifndef SOCKOPTS_H
#define SOCKOPTS_H

#include "logger.h"

/*
 * Configures a freshly accepted client socket:
 *   - TCP_NODELAY: small messages leave immediately (no Nagle delay).
 *   - SO_KEEPALIVE + TCP_KEEPIDLE/INTVL/CNT: the kernel probes a silent peer,
 *     so a client that vanished (lost Wi-Fi, crashed) is detected.
 *   - TCP_USER_TIMEOUT: same detection while we are SENDING data (keepalive
 *     does not act when there is unacknowledged data in flight).
 * A failing option is logged as a warning and the connection keeps working.
 * `peer` is the "ip:port" string used in the log.
 */
void configure_client_socket(Logger *lg, int fd, const char *peer);

#endif /* SOCKOPTS_H */