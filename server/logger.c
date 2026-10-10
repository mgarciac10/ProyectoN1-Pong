#include "logger.h"

#include <pthread.h>
#include <stdarg.h>   /* va_list: functions with a variable number of arguments */
#include <stdio.h>
#include <time.h>

/* "static" globals: shared by every thread, but only visible inside this file. */
static FILE *log_file = NULL;

/* Only one thread at a time may write a log line. Without this lock, two
 * threads logging at the same moment could interleave their text. */
static pthread_mutex_t log_mutex = PTHREAD_MUTEX_INITIALIZER;

static const char *level_name(LogLevel level)
{
    switch (level) {
    case LOG_INFO:  return "INFO";
    case LOG_WARN:  return "WARN";
    case LOG_ERROR: return "ERROR";
    }
    return "?";
}

int logger_init(const char *path)
{
    log_file = fopen(path, "a"); /* "a" = append: keep previous runs' lines */
    if (log_file == NULL) {
        perror(path);
        return -1;
    }
    return 0;
}

void logger_close(void)
{
    if (log_file != NULL) {
        fclose(log_file);
        log_file = NULL;
    }
}

void log_event(LogLevel level, const char *fmt, ...)
{
    /* 1. Build the timestamp, e.g. "2026-10-10 14:03:11".
     *    localtime_r is the thread-safe version of localtime. */
    char timestamp[32];
    time_t now = time(NULL);
    struct tm tm_now;
    localtime_r(&now, &tm_now);
    strftime(timestamp, sizeof timestamp, "%Y-%m-%d %H:%M:%S", &tm_now);

    /* 2. Format the caller's message, exactly like printf would. */
    char message[512];
    va_list args;
    va_start(args, fmt);
    vsnprintf(message, sizeof message, fmt, args);
    va_end(args);

    /* 3. Write the line to both outputs while holding the lock. */
    pthread_mutex_lock(&log_mutex);

    printf("%s [%s] %s\n", timestamp, level_name(level), message);
    if (log_file != NULL) {
        fprintf(log_file, "%s [%s] %s\n", timestamp, level_name(level), message);
        fflush(log_file); /* write it now, so the file is up to date even if we crash */
    }

    pthread_mutex_unlock(&log_mutex);
}
