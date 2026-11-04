/*
 * ratelimit.c - Per-IP token buckets in a small fixed-size hash table.
 *
 * The table is shared by all connections, so it is protected by a mutex.
 * Its size is fixed (RATE_LIMIT_TABLE), so a flood of different IPs cannot
 * make it grow: when the probe window is full, the least recently seen
 * entry is replaced.
 */
#include "ratelimit.h"
#include "config.h"

#include <pthread.h>
#include <string.h>
#include <time.h>

#define PROBE 16    /* slots examined per lookup (linear probing) */

typedef struct {
    uint32_t ip;
    int      used;
    double   tokens;
    double   last;      /* seconds, monotonic clock */
} bucket_t;

static bucket_t        table[RATE_LIMIT_TABLE];
static pthread_mutex_t rl_lock = PTHREAD_MUTEX_INITIALIZER;
static double          rate = 0.0;
static double          burst = 0.0;

void ratelimit_init(double rate_per_sec, double burst_size)
{
    pthread_mutex_lock(&rl_lock);
    rate  = rate_per_sec;
    burst = burst_size < 1.0 ? 1.0 : burst_size;
    memset(table, 0, sizeof table);
    pthread_mutex_unlock(&rl_lock);
}

double ratelimit_rate(void)
{
    return rate;
}

static double now_sec(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

int ratelimit_allow(uint32_t ip)
{
    if (rate <= 0.0)
        return 1;

    double now = now_sec();
    int allowed;

    pthread_mutex_lock(&rl_lock);

    size_t h = (size_t)((ip * 2654435761u) % RATE_LIMIT_TABLE);
    bucket_t *found = NULL, *empty = NULL, *oldest = NULL;
    for (int i = 0; i < PROBE; i++) {
        bucket_t *b = &table[(h + (size_t)i) % RATE_LIMIT_TABLE];
        if (b->used && b->ip == ip) {
            found = b;
            break;
        }
        if (!b->used) {
            if (!empty)
                empty = b;
        } else if (!oldest || b->last < oldest->last) {
            oldest = b;
        }
    }

    bucket_t *b = found ? found : empty ? empty : oldest;
    if (!found) {               /* new IP: start with a full bucket */
        b->ip     = ip;
        b->used   = 1;
        b->tokens = burst;
        b->last   = now;
    }

    /* Refill for the time since the last request, capped at burst. */
    b->tokens += (now - b->last) * rate;
    if (b->tokens > burst)
        b->tokens = burst;
    b->last = now;

    if (b->tokens >= 1.0) {
        b->tokens -= 1.0;
        allowed = 1;
    } else {
        allowed = 0;
    }

    pthread_mutex_unlock(&rl_lock);
    return allowed;
}
