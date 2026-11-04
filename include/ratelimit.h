/*
 * ratelimit.h - Per-IP rate limiting with a token bucket (bonus step).
 *
 * Each client IP has a bucket that holds up to `burst` tokens and refills
 * at `rate` tokens per second. Every new connection costs one token; with
 * an empty bucket the server answers 429 Too Many Requests. Short bursts
 * (a page plus its CSS) are fine; sustained flooding is cut off.
 */
#ifndef RATELIMIT_H
#define RATELIMIT_H

#include <stdint.h>

/* rate <= 0 disables limiting. */
void   ratelimit_init(double rate_per_sec, double burst);
double ratelimit_rate(void);

/* 1 if this IPv4 address (network byte order) may proceed, 0 if limited. */
int    ratelimit_allow(uint32_t ip);

#endif /* RATELIMIT_H */
