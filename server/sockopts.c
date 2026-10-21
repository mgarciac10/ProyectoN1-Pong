/*
 * Socket options applied to every accepted client connection (see sockopts.h).
 */
#include "sockopts.h"

#include <errno.h>
#include <netinet/in.h>   /* IPPROTO_TCP */
#include <netinet/tcp.h>  /* TCP_NODELAY, TCP_KEEPIDLE, TCP_USER_TIMEOUT... (Linux) */
#include <string.h>
#include <sys/socket.h>   /* setsockopt, SOL_SOCKET, SO_KEEPALIVE */

#include "logger.h"

/* Values chosen in README 2.3.7 (Liveness): a dead client is detected in ~15 s. */
#define KEEPALIVE_IDLE_S      5       /* idle seconds before the first probe */
#define KEEPALIVE_INTERVAL_S  2       /* seconds between probes */
#define KEEPALIVE_PROBES      3       /* unanswered probes before giving up */
#define USER_TIMEOUT_MS       10000   /* max time with sent data left unacknowledged */

static void set_option(Logger *lg, int fd, int level, int name, int value,
                       const char *label, const char *peer)
{
    if (setsockopt(fd, level, name, &value, sizeof value) < 0)
        log_event(lg, LOG_WARN, "%s setsockopt(%s) failed: %s", peer, label, strerror(errno));
}

void configure_client_socket(Logger *lg, int fd, const char *peer)
{
    set_option(lg, fd, IPPROTO_TCP, TCP_NODELAY,      1,                    "TCP_NODELAY",      peer);

    set_option(lg, fd, SOL_SOCKET,  SO_KEEPALIVE,     1,                    "SO_KEEPALIVE",     peer);
    set_option(lg, fd, IPPROTO_TCP, TCP_KEEPIDLE,     KEEPALIVE_IDLE_S,     "TCP_KEEPIDLE",     peer);
    set_option(lg, fd, IPPROTO_TCP, TCP_KEEPINTVL,    KEEPALIVE_INTERVAL_S, "TCP_KEEPINTVL",    peer);
    set_option(lg, fd, IPPROTO_TCP, TCP_KEEPCNT,      KEEPALIVE_PROBES,     "TCP_KEEPCNT",      peer);

    set_option(lg, fd, IPPROTO_TCP, TCP_USER_TIMEOUT, USER_TIMEOUT_MS,      "TCP_USER_TIMEOUT", peer);
}