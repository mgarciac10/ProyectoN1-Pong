/*
 * PongServer entry point.
 * Usage: ./server <PORT> <LogFile>
 *
 * Phase 1: iterative TCP echo server. It serves ONE client at a time:
 * the next client waits in the kernel's queue until the current one leaves.
 */
#include <arpa/inet.h>   /* htons, htonl, ntohs, inet_ntop */
#include <errno.h>
#include <netinet/in.h>  /* struct sockaddr_in, INADDR_ANY */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>  /* socket, setsockopt, bind, listen, accept, recv, send */
#include <unistd.h>      /* close */

#define BACKLOG  10      /* max pending connections queued by the kernel */
#define BUF_SIZE 1024

static void print_usage(const char *prog)
{
    fprintf(stderr, "Usage: %s <PORT> <LogFile>\n", prog);
}

/*
 * Creates a TCP socket listening on 0.0.0.0:<port>.
 * Returns the listening file descriptor, or -1 on error.
 */
static int create_listen_socket(uint16_t port)
{
    /* 1. socket(): ask the OS for an IPv4 (AF_INET) TCP (SOCK_STREAM) endpoint.
     *    Like open() for files, it returns a file descriptor (a small int). */
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        perror("socket");
        return -1;
    }

    /* Let us restart the server right away on the same port. Without this,
     * bind() fails with "Address already in use" for ~1 minute after a restart
     * (old connections linger in the TIME_WAIT state). */
    int yes = 1;
    if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof yes) < 0) {
        perror("setsockopt");
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
        perror("bind");
        close(fd);
        return -1;
    }

    /* 3. listen(): turn it into a passive socket. From now on the KERNEL
     *    completes incoming connections and queues them (up to BACKLOG). */
    if (listen(fd, BACKLOG) < 0) {
        perror("listen");
        close(fd);
        return -1;
    }
    return fd;
}

/*
 * send() may write fewer bytes than requested, so keep calling it until
 * everything is sent. Returns 0 on success, -1 on error.
 */
static int send_all(int fd, const char *buf, size_t len)
{
    size_t sent = 0;
    while (sent < len) {
        ssize_t n = send(fd, buf + sent, len - sent, 0);
        if (n < 0) {
            if (errno == EINTR)   /* interrupted by a signal: just retry */
                continue;
            return -1;
        }
        sent += (size_t)n;
    }
    return 0;
}

/* Echoes everything the client sends until it disconnects. */
static void serve_client(int client_fd)
{
    char buf[BUF_SIZE];

    for (;;) {
        /* recv() blocks until data arrives. It returns:
         *   > 0  number of bytes read (maybe fewer than sent: TCP is a byte stream)
         *   = 0  the peer closed the connection
         *   < 0  error */
        ssize_t n = recv(client_fd, buf, sizeof buf, 0);
        if (n > 0) {
            printf("  received %zd bytes: %.*s", n, (int)n, buf);
            if (buf[n - 1] != '\n')
                printf("\n");
            if (send_all(client_fd, buf, (size_t)n) < 0) {
                perror("send");
                return;
            }
        } else if (n == 0) {
            printf("  client closed the connection\n");
            return;
        } else {
            if (errno == EINTR)
                continue;
            perror("recv");
            return;
        }
    }
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

    const char *log_path = argv[2];
    (void)log_path; /* the logger arrives in Phase 2 */

    int listen_fd = create_listen_socket((uint16_t)port);
    if (listen_fd < 0)
        return EXIT_FAILURE;

    printf("PongServer listening on port %ld\n", port);

    /* Iterative server: accept a client, serve it until it leaves, repeat. */
    for (;;) {
        struct sockaddr_in client_addr;
        socklen_t addr_len = sizeof client_addr;

        /* 4. accept(): take the next connection from the kernel's queue
         *    (blocks if there is none). Returns a NEW socket just for this
         *    client; listen_fd keeps listening for others. */
        int client_fd = accept(listen_fd, (struct sockaddr *)&client_addr, &addr_len);
        if (client_fd < 0) {
            if (errno == EINTR)
                continue;
            perror("accept");
            continue;
        }

        char ip[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &client_addr.sin_addr, ip, sizeof ip);
        printf("Client connected from %s:%d\n", ip, ntohs(client_addr.sin_port));

        serve_client(client_fd);

        /* 5. close(): release the connection socket. */
        close(client_fd);
        printf("Client %s:%d disconnected\n", ip, ntohs(client_addr.sin_port));
    }

    /* Not reached for now; a clean shutdown comes in a later phase. */
    close(listen_fd);
    return EXIT_SUCCESS;
}
