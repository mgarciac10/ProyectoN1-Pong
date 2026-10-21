#include "logger.h"

#include <stdarg.h>   /* va_list: functions with a variable number of arguments */
#include <time.h>

static const char *level_name(LogLevel level)
{
    switch (level) {
    case LOG_INFO:  return "INFO";
    case LOG_WARN:  return "WARN";
    case LOG_ERROR: return "ERROR";
    }
    return "?";
}

int logger_init(Logger *lg, const char *path)
{
    pthread_mutex_init(&lg->lock, NULL);
    lg->file = fopen(path, "a"); /* "a" = append: keep previous runs' lines */
    if (lg->file == NULL) {
        perror(path);
        return -1;
    }
    return 0;
}

void logger_close(Logger *lg)
{
    if (lg->file != NULL) {
        fclose(lg->file);
        lg->file = NULL;
    }
    pthread_mutex_destroy(&lg->lock);
}

void log_event(Logger *lg, LogLevel level, const char *fmt, ...)
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

    /* 3. Write the line to both outputs while holding the lock. Without it,
     *    two threads logging at the same moment could interleave their text. */
    pthread_mutex_lock(&lg->lock);

    printf("%s [%s] %s\n", timestamp, level_name(level), message);
    if (lg->file != NULL) {
        fprintf(lg->file, "%s [%s] %s\n", timestamp, level_name(level), message);
        fflush(lg->file); /* write it now, so the file is up to date even if we crash */
    }

    pthread_mutex_unlock(&lg->lock);
}
