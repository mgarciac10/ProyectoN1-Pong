/*
 * Thread-safe logger: every event goes to the console AND to the log file.
 */
#ifndef LOGGER_H   /* "include guard": avoids defining things twice if */
#define LOGGER_H   /* this header is #included from several places     */

#include <pthread.h>
#include <stdio.h>

typedef enum {
    LOG_INFO,
    LOG_WARN,
    LOG_ERROR
} LogLevel;

/* A logger: the open log file plus the lock that serializes the writers.
 * It lives inside the Server context (server.h), not in a global variable. */
typedef struct {
    FILE            *file;
    pthread_mutex_t  lock;   /* one thread at a time writes a log line */
} Logger;

/* Opens (or creates) the log file in append mode. Returns 0 on success, -1 on error. */
int logger_init(Logger *lg, const char *path);

/* Closes the log file. */
void logger_close(Logger *lg);

/* printf-style logging, e.g. log_event(lg, LOG_INFO, "%s connected", addr).
 * Adds a timestamp and the level; a newline is added automatically. */
void log_event(Logger *lg, LogLevel level, const char *fmt, ...);

#endif
