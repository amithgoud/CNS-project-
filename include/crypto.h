/*
 * crypto.h - Randomness, SHA-256 and salted password hashing.
 *
 * SHA-256, HMAC and PBKDF2 are implemented in crypto.c (from FIPS 180-4,
 * RFC 2104 and RFC 8018) so the project needs no crypto library.
 */
#ifndef CRYPTO_H
#define CRYPTO_H

#include <stddef.h>

/* Room for "pbkdf2_sha256$<iter>$<salt hex>$<hash hex>". */
#define PASSWORD_HASH_MAX 160

/* Fill buf with n bytes from the kernel's CSPRNG (/dev/urandom). 0 on success. */
int  random_bytes(void *buf, size_t n);

/* Write 2*n lowercase hex characters plus a NUL to out. */
void hex_encode(const unsigned char *in, size_t n, char *out);

/* SHA-256 of len bytes. */
void sha256(const void *data, size_t len, unsigned char out[32]);

/* Hash a password with a fresh random salt. 0 on success. */
int  password_hash(const char *password, char *out, size_t outsz);

/* 1 if password matches the stored hash, else 0. */
int  password_verify(const char *password, const char *stored);

#endif /* CRYPTO_H */
