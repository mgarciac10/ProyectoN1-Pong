/*
 * Thread-safe logger: every event goes to the console AND to the log file.
 */
#ifndef LOGGER_H   /* "include guard": avoids defining things twice if */
#define LOGGER_H   /* this header is #included from several places     */

typedef enum {
    LOG_INFO,
    LOG_WARN,
    LOG_ERROR
} LogLevel;

/* Opens (or creates) the log file in append mode. Returns 0 on success, -1 on error. */
int logger_init(const char *path);

/* Closes the log file. */
void logger_close(void);

/* printf-style logging, e.g. log_event(LOG_INFO, "%s connected", addr).
 * Adds a timestamp and the level; a newline is added automatically. */
void log_event(LogLevel level, const char *fmt, ...);

#endif
