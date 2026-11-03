/*
 * log.c - Implementation of the thread-safe logger declared in log.h.
 */
#include "log.h"

#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <time.h>

static FILE *log_fp = NULL;

/* Serialises access to log_fp and stderr across worker threads. */
static pthread_mutex_t log_lock = PTHREAD_MUTEX_INITIALIZER;

int log_init(const char *path)
{
    log_fp = fopen(path, "a");
    return log_fp ? 0 : -1;
}

void log_close(void)
{
    pthread_mutex_lock(&log_lock);
    if (log_fp) {
        fclose(log_fp);
        log_fp = NULL;
    }
    pthread_mutex_unlock(&log_lock);
}

void log_write(const char *level, const char *fmt, ...)
{
    /* Format the timestamp and message outside the lock to keep the
     * critical section as short as possible. localtime_r() is the
     * thread-safe variant of localtime(). */
    char ts[32];
    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    strftime(ts, sizeof ts, "%Y-%m-%d %H:%M:%S", &tm);

    char msg[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);   /* truncates, never overflows */
    va_end(ap);

    pthread_mutex_lock(&log_lock);
    if (log_fp) {
        fprintf(log_fp, "%s [%s] %s\n", ts, level, msg);
        fflush(log_fp);
    }
    fprintf(stderr, "%s [%s] %s\n", ts, level, msg);
    pthread_mutex_unlock(&log_lock);
}
