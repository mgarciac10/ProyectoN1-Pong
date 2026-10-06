/*
 * PongServer entry point.
 * Usage: ./server <PORT> <LogFile>
 */
#include <stdio.h>
#include <stdlib.h>

static void print_usage(const char *prog)
{
    fprintf(stderr, "Usage: %s <PORT> <LogFile>\n", prog);
}

int main(int argc, char *argv[])
{
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

    printf("PongServer starting on port %ld, logging to '%s'\n", port, log_path);
    return EXIT_SUCCESS;
}
