/*
 * crypto.c - SHA-256, HMAC-SHA256, PBKDF2 and password hashing.
 *
 * Stored password format:
 *
 *   pbkdf2_sha256$100000$<32 hex chars of salt>$<64 hex chars of hash>
 *
 * Keeping the algorithm, iteration count and salt next to the hash means
 * old hashes keep working if PBKDF2_ITERATIONS is raised later.
 */
#include "crypto.h"
#include "config.h"

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define SALT_MAX 32

/* ------------------------------------------------------------------ */
/* Randomness and hex                                                 */
/* ------------------------------------------------------------------ */

int random_bytes(void *buf, size_t n)
{
    int fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return -1;

    unsigned char *p = buf;
    while (n > 0) {
        ssize_t r = read(fd, p, n);
        if (r < 0) {
            if (errno == EINTR)
                continue;
            close(fd);
            return -1;
        }
        if (r == 0) {
            close(fd);
            return -1;
        }
        p += r;
        n -= (size_t)r;
    }
    close(fd);
    return 0;
}

void hex_encode(const unsigned char *in, size_t n, char *out)
{
    static const char digits[] = "0123456789abcdef";
    for (size_t i = 0; i < n; i++) {
        out[2 * i]     = digits[in[i] >> 4];
        out[2 * i + 1] = digits[in[i] & 0x0f];
    }
    out[2 * n] = '\0';
}

static int hex_nibble(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* Decode exactly 2*n hex chars into n bytes. 0 on success. */
static int hex_decode(const char *hex, size_t n, unsigned char *out)
{
    for (size_t i = 0; i < n; i++) {
        int hi = hex_nibble(hex[2 * i]);
        int lo = hex_nibble(hex[2 * i + 1]);
        if (hi < 0 || lo < 0)
            return -1;
        out[i] = (unsigned char)(hi << 4 | lo);
    }
    return 0;
}

/* Compare in time that does not depend on where the first difference is,
 * so response timing leaks nothing about the stored hash. */
static int ct_equal(const unsigned char *a, const unsigned char *b, size_t n)
{
    unsigned char diff = 0;
    for (size_t i = 0; i < n; i++)
        diff |= a[i] ^ b[i];
    return diff == 0;
}

/* ------------------------------------------------------------------ */
/* SHA-256 (FIPS 180-4)                                               */
/* ------------------------------------------------------------------ */

typedef struct {
    uint32_t h[8];
    uint64_t len;           /* total bytes hashed */
    uint8_t  buf[64];
    size_t   buflen;
} sha256_ctx;

static const uint32_t K[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
};

#define ROR(x, n) (((x) >> (n)) | ((x) << (32 - (n))))

static void sha256_block(sha256_ctx *c, const uint8_t *p)
{
    uint32_t w[64];
    for (int i = 0; i < 16; i++)
        w[i] = (uint32_t)p[4 * i] << 24 | (uint32_t)p[4 * i + 1] << 16 |
               (uint32_t)p[4 * i + 2] << 8 | (uint32_t)p[4 * i + 3];
    for (int i = 16; i < 64; i++) {
        uint32_t s0 = ROR(w[i - 15], 7) ^ ROR(w[i - 15], 18) ^ (w[i - 15] >> 3);
        uint32_t s1 = ROR(w[i - 2], 17) ^ ROR(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }

    uint32_t a = c->h[0], b = c->h[1], cc = c->h[2], d = c->h[3];
    uint32_t e = c->h[4], f = c->h[5], g = c->h[6], h = c->h[7];

    for (int i = 0; i < 64; i++) {
        uint32_t S1  = ROR(e, 6) ^ ROR(e, 11) ^ ROR(e, 25);
        uint32_t ch  = (e & f) ^ (~e & g);
        uint32_t t1  = h + S1 + ch + K[i] + w[i];
        uint32_t S0  = ROR(a, 2) ^ ROR(a, 13) ^ ROR(a, 22);
        uint32_t maj = (a & b) ^ (a & cc) ^ (b & cc);
        uint32_t t2  = S0 + maj;
        h = g; g = f; f = e; e = d + t1;
        d = cc; cc = b; b = a; a = t1 + t2;
    }

    c->h[0] += a; c->h[1] += b; c->h[2] += cc; c->h[3] += d;
    c->h[4] += e; c->h[5] += f; c->h[6] += g; c->h[7] += h;
}

static void sha256_init(sha256_ctx *c)
{
    static const uint32_t iv[8] = {
        0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
        0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19,
    };
    memcpy(c->h, iv, sizeof iv);
    c->len = 0;
    c->buflen = 0;
}

static void sha256_update(sha256_ctx *c, const void *data, size_t n)
{
    const uint8_t *p = data;
    c->len += n;
    while (n > 0) {
        if (c->buflen == 0 && n >= 64) {        /* whole block: no copy */
            sha256_block(c, p);
            p += 64;
            n -= 64;
            continue;
        }
        size_t take = 64 - c->buflen;
        if (take > n)
            take = n;
        memcpy(c->buf + c->buflen, p, take);
        c->buflen += take;
        p += take;
        n -= take;
        if (c->buflen == 64) {
            sha256_block(c, c->buf);
            c->buflen = 0;
        }
    }
}

static void sha256_final(sha256_ctx *c, uint8_t out[32])
{
    uint64_t bits = c->len * 8;

    /* Padding: 0x80, zeros up to 56 mod 64, then the 64-bit length. */
    c->buf[c->buflen++] = 0x80;
    if (c->buflen > 56) {
        memset(c->buf + c->buflen, 0, 64 - c->buflen);
        sha256_block(c, c->buf);
        c->buflen = 0;
    }
    memset(c->buf + c->buflen, 0, 56 - c->buflen);
    for (int i = 0; i < 8; i++)
        c->buf[56 + i] = (uint8_t)(bits >> (56 - 8 * i));
    sha256_block(c, c->buf);

    for (int i = 0; i < 8; i++) {
        out[4 * i]     = (uint8_t)(c->h[i] >> 24);
        out[4 * i + 1] = (uint8_t)(c->h[i] >> 16);
        out[4 * i + 2] = (uint8_t)(c->h[i] >> 8);
        out[4 * i + 3] = (uint8_t)(c->h[i]);
    }
}

void sha256(const void *data, size_t len, unsigned char out[32])
{
    sha256_ctx c;
    sha256_init(&c);
    sha256_update(&c, data, len);
    sha256_final(&c, out);
}

/* ------------------------------------------------------------------ */
/* HMAC-SHA256 (RFC 2104) and PBKDF2 (RFC 8018)                       */
/* ------------------------------------------------------------------ */

/* Hash states after absorbing (key ^ ipad) and (key ^ opad). Computing
 * them once and copying per call halves the work inside PBKDF2. */
typedef struct {
    sha256_ctx inner;
    sha256_ctx outer;
} hmac_ctx;

static void hmac_init(hmac_ctx *hm, const uint8_t *key, size_t klen)
{
    uint8_t k0[64] = { 0 };
    uint8_t pad[64];

    if (klen > 64)
        sha256(key, klen, k0);
    else
        memcpy(k0, key, klen);

    for (int i = 0; i < 64; i++)
        pad[i] = k0[i] ^ 0x36;
    sha256_init(&hm->inner);
    sha256_update(&hm->inner, pad, 64);

    for (int i = 0; i < 64; i++)
        pad[i] = k0[i] ^ 0x5c;
    sha256_init(&hm->outer);
    sha256_update(&hm->outer, pad, 64);
}

/* out = HMAC(key, msg). msg and out may overlap. */
static void hmac_compute(const hmac_ctx *hm, const uint8_t *msg, size_t len,
                         uint8_t out[32])
{
    uint8_t inner_hash[32];
    sha256_ctx c = hm->inner;
    sha256_update(&c, msg, len);
    sha256_final(&c, inner_hash);

    c = hm->outer;
    sha256_update(&c, inner_hash, 32);
    sha256_final(&c, out);
}

/* PBKDF2-HMAC-SHA256 with a 32-byte output (a single block). */
static void pbkdf2_sha256(const char *password, const uint8_t *salt,
                          size_t salt_len, unsigned long iterations,
                          uint8_t out[32])
{
    hmac_ctx hm;
    uint8_t msg[SALT_MAX + 4];
    uint8_t u[32];

    hmac_init(&hm, (const uint8_t *)password, strlen(password));

    /* U1 = HMAC(P, salt || INT_BE(1)) */
    memcpy(msg, salt, salt_len);
    msg[salt_len]     = 0;
    msg[salt_len + 1] = 0;
    msg[salt_len + 2] = 0;
    msg[salt_len + 3] = 1;
    hmac_compute(&hm, msg, salt_len + 4, u);
    memcpy(out, u, 32);

    /* Ui = HMAC(P, Ui-1);  T = U1 ^ U2 ^ ... ^ Uc */
    for (unsigned long i = 1; i < iterations; i++) {
        hmac_compute(&hm, u, 32, u);
        for (int j = 0; j < 32; j++)
            out[j] ^= u[j];
    }
}

/* ------------------------------------------------------------------ */
/* Password hashing                                                   */
/* ------------------------------------------------------------------ */

#define HASH_PREFIX "pbkdf2_sha256$"

int password_hash(const char *password, char *out, size_t outsz)
{
    uint8_t salt[PASSWORD_SALT_BYTES];
    uint8_t dk[32];
    char salt_hex[2 * PASSWORD_SALT_BYTES + 1];
    char dk_hex[65];

    if (random_bytes(salt, sizeof salt) < 0)
        return -1;
    pbkdf2_sha256(password, salt, sizeof salt, PBKDF2_ITERATIONS, dk);
    hex_encode(salt, sizeof salt, salt_hex);
    hex_encode(dk, sizeof dk, dk_hex);

    int n = snprintf(out, outsz, HASH_PREFIX "%d$%s$%s",
                     PBKDF2_ITERATIONS, salt_hex, dk_hex);
    return (n > 0 && (size_t)n < outsz) ? 0 : -1;
}

int password_verify(const char *password, const char *stored)
{
    size_t plen = strlen(HASH_PREFIX);
    if (strncmp(stored, HASH_PREFIX, plen) != 0)
        return 0;

    const char *p = stored + plen;
    char *end;
    long iterations = strtol(p, &end, 10);
    if (end == p || *end != '$' || iterations < 1 || iterations > 10000000)
        return 0;

    const char *salt_hex = end + 1;
    const char *dollar = strchr(salt_hex, '$');
    if (!dollar)
        return 0;
    size_t salt_hex_len = (size_t)(dollar - salt_hex);
    if (salt_hex_len == 0 || salt_hex_len % 2 != 0 || salt_hex_len / 2 > SALT_MAX)
        return 0;

    uint8_t salt[SALT_MAX];
    uint8_t expected[32], actual[32];
    const char *hash_hex = dollar + 1;
    if (strlen(hash_hex) != 64 ||
        hex_decode(salt_hex, salt_hex_len / 2, salt) < 0 ||
        hex_decode(hash_hex, 32, expected) < 0)
        return 0;

    pbkdf2_sha256(password, salt, salt_hex_len / 2, (unsigned long)iterations, actual);
    return ct_equal(actual, expected, 32);
}
