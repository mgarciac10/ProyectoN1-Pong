/*
 * PongServer entry point.
 * Usage: ./server <PORT> <LogFile>
 *
 * The main thread only accepts connections; every client is served by its
 * own thread, which runs a session (see session.c) speaking PongProtocol.
 */
#include <arpa/inet.h>   /* htons, htonl, ntohs, inet_ntop */
#include <errno.h>
#include <netinet/in.h>  /* struct sockaddr_in, INADDR_ANY */
#include <pthread.h>     /* pthread_create, pthread_detach */
#include <signal.h>      /* signal, SIGPIPE */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>  /* socket, setsockopt, bind, listen, accept, recv, send */
#include <unistd.h>      /* close */

#include "logger.h"
#include "server.h"
#include "session.h"
#include "sockopts.h"

#define BACKLOG  10      /* max pending connections queued by the kernel */

/* Everything a client thread needs to know about its client. */
typedef struct {
    Server *srv;         /* shared server context */
    int     fd;          /* connection socket returned by accept() */
    char    addr[32];    /* "ip:port", used in every log line */
} Client;

/* Prepares the shared server context (see server.h). 0 on success, -1 on error. */
static int server_init(Server *srv, const char *log_path)
{
    memset(srv, 0, sizeof *srv);
    pthread_mutex_init(&srv->nicks_lock, NULL);
    pthread_mutex_init(&srv->game_lock, NULL);
    pthread_cond_init(&srv->players_released, NULL);
    return logger_init(&srv->log, log_path);
}

static void server_destroy(Server *srv)
{
    logger_close(&srv->log);
    pthread_cond_destroy(&srv->players_released);
    pthread_mutex_destroy(&srv->game_lock);
    pthread_mutex_destroy(&srv->nicks_lock);
}

static void print_usage(const char *prog)
{
    fprintf(stderr, "Usage: %s <PORT> <LogFile>\n", prog);
}

/*
 * Creates a TCP socket listening on 0.0.0.0:<port>.
 * Returns the listening file descriptor, or -1 on error.
 */
static int create_listen_socket(Logger *lg, uint16_t port)
{
    /* 1. socket(): ask the OS for an IPv4 (AF_INET) TCP (SOCK_STREAM) endpoint.
     *    Like open() for files, it returns a file descriptor (a small int). */
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        log_event(lg, LOG_ERROR, "socket: %s", strerror(errno));
        return -1;
    }

    /* Let us restart the server right away on the same port. Without this,
     * bind() fails with "Address already in use" for ~1 minute after a restart
     * (old connections linger in the TIME_WAIT state). */
    int yes = 1;
    if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof yes) < 0) {
        log_event(lg, LOG_ERROR, "setsockopt: %s", strerror(errno));
        close(fd);
        return -1;
    }

    /* 2. bind(): attach the socket to a local address (IP + port). */
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof addr);
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY); /* 0.0.0.0 = every network interface */
    addr.sin_port        = htons(port);       /* host -> network byte order */

    if (bind(fd, (struct sockaddr *)&addr, sizeof addr) < 0) {
        log_event(lg, LOG_ERROR, "bind: %s", strerror(errno));
        close(fd);
        return -1;
    }

    /* 3. listen(): turn it into a passive socket. From now on the KERNEL
     *    completes incoming connections and queues them (up to BACKLOG). */
    if (listen(fd, BACKLOG) < 0) {
        log_event(lg, LOG_ERROR, "listen: %s", strerror(errno));
        close(fd);
        return -1;
    }
    return fd;
}

/*
 * Body of every client thread. pthread_create() requires exactly this
 * signature: takes a void* and returns a void*.
 */
static void *client_thread(void *arg)
{
    Client *client = arg; /* void* -> Client*: we know what we passed in */

    configure_client_socket(&client->srv->log, client->fd, client->addr);
    session_run(client->srv, client->fd, client->addr);

    close(client->fd);
    log_event(&client->srv->log, LOG_INFO, "%s disconnected", client->addr);
    free(client); /* the thread owns the Client: it was malloc'ed in main() */
    return NULL;
}

int main(int argc, char *argv[])
{
    /* When stdout is redirected to a file, C buffers ~4 KB before writing.
     * Line buffering makes every '\n' reach the output immediately. */
    setvbuf(stdout, NULL, _IOLBF, 0);

    /* argv[0] is the program name, so we expect exactly 3 entries. */
    if (argc != 3) {
        print_usage(argv[0]);
        return EXIT_FAILURE;
    }

    /* Ports are 16-bit numbers; 1-1023 need root, so we accept 1024-65535. */
    char *end;
    long port = strtol(argv[1], &end, 10);
    if (*end != '\0' || port < 1024 || port > 65535) {
        fprintf(stderr, "Invalid port '%s' (expected 1024-65535)\n", argv[1]);
        return EXIT_FAILURE;
    }

    /* All the state shared between threads lives here, NOT in global variables.
     * It is on main()'s stack, which is safe because main() never returns while
     * client threads are running (the accept loop below is infinite). */
    Server srv;
    if (server_init(&srv, argv[2]) < 0)
        return EXIT_FAILURE;

    /* By default, send() on a connection the peer already closed raises
     * SIGPIPE, which KILLS the whole process (every client!). Ignoring it
     * makes send() just return -1 with errno = EPIPE, which we handle. */
    signal(SIGPIPE, SIG_IGN);

    int listen_fd = create_listen_socket(&srv.log, (uint16_t)port);
    if (listen_fd < 0) {
        server_destroy(&srv);
        return EXIT_FAILURE;
    }

    log_event(&srv.log, LOG_INFO, "PongServer listening on port %ld (log file: %s)", port, argv[2]);

    /* The main thread only accepts; each client gets its own thread. */
    for (;;) {
        struct sockaddr_in client_addr;
        socklen_t addr_len = sizeof client_addr;

        /* 4. accept(): take the next connection from the kernel's queue
         *    (blocks if there is none). Returns a NEW socket just for this
         *    client; listen_fd keeps listening for others. */
        int client_fd = accept(listen_fd, (struct sockaddr *)&client_addr, &addr_len);
        if (client_fd < 0) {
            if (errno != EINTR)
                log_event(&srv.log, LOG_ERROR, "accept: %s", strerror(errno));
            continue;
        }

        /* Heap-allocate the client's data: it must outlive this loop
         * iteration, because the new thread keeps using it. */
        Client *client = malloc(sizeof *client);
        if (client == NULL) {
            log_event(&srv.log, LOG_ERROR, "malloc failed, dropping connection");
            close(client_fd);
            continue;
        }
        client->srv = &srv;
        client->fd  = client_fd;

        char ip[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &client_addr.sin_addr, ip, sizeof ip);
        snprintf(client->addr, sizeof client->addr, "%s:%d", ip, ntohs(client_addr.sin_port));

        log_event(&srv.log, LOG_INFO, "%s connected", client->addr);

        /* 5. Start a thread running client_thread(client). */
        pthread_t tid;
        int err = pthread_create(&tid, NULL, client_thread, client);
        if (err != 0) {
            log_event(&srv.log, LOG_ERROR, "pthread_create: %s", strerror(err));
            close(client_fd);
            free(client);
            continue;
        }

        /* Detached: nobody waits for this thread; its resources are freed
         * automatically when it finishes. */
        pthread_detach(tid);
    }

    /* Not reached for now; a clean shutdown comes in a later phase. */
    close(listen_fd);
    server_destroy(&srv);
    return EXIT_SUCCESS;
}
