/*
 * log.h - Thread-safe logging to a file (and mirrored to stderr).
 *
 * Many worker threads log at the same time, so every write is done
 * while holding a mutex. Without it, lines from different threads could
 * be interleaved in the middle of a line.
 */
#ifndef LOG_H
#define LOG_H

#ifdef __GNUC__
#define LOG_PRINTF_FMT(a, b) __attribute__((format(printf, a, b)))
#else
#define LOG_PRINTF_FMT(a, b)
#endif

/* Open (append to) the log file. Returns 0 on success, -1 on error. */
int  log_init(const char *path);

/* Flush and close the log file. */
void log_close(void);

/* Write one timestamped line: "YYYY-mm-dd HH:MM:SS [LEVEL] message". */
void log_write(const char *level, const char *fmt, ...) LOG_PRINTF_FMT(2, 3);

#define log_info(...)  log_write("INFO",  __VA_ARGS__)
#define log_warn(...)  log_write("WARN",  __VA_ARGS__)
#define log_error(...) log_write("ERROR", __VA_ARGS__)

#endif /* LOG_H */
