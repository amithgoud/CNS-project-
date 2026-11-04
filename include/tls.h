/*
 * tls.h - Optional HTTPS support with OpenSSL (bonus step).
 *
 * Built only with "make HTTPS=1" (defines SQMS_TLS and links -lssl
 * -lcrypto). Without it these functions are stubs and tls_init() fails
 * with a clear message, so the normal build needs no OpenSSL.
 *
 * TLS sits between TCP and HTTP: after the TCP three-way handshake the
 * client and server run a TLS handshake (certificate, key exchange), and
 * from then on every HTTP byte is encrypted. The HTTP code does not
 * change at all; it just reads and writes through SSL_read/SSL_write.
 */
#ifndef TLS_H
#define TLS_H

#include "server.h"

#include <stddef.h>
#include <sys/types.h>

/* Load certificate + key and create the TLS context. 0 on success. */
int  tls_init(const char *cert_file, const char *key_file);
void tls_cleanup(void);

/* 1 if the server is running HTTPS. */
int  tls_enabled(void);

/* Run the TLS handshake on an accepted connection; sets c->tls. 0 on success. */
int  tls_accept(conn_t *c);

/* Send close_notify and free the TLS state. */
void tls_close(conn_t *c);

/* Same contract as recv()/send(): -1 with errno = EAGAIN on timeout. */
ssize_t tls_recv(const conn_t *c, void *buf, size_t len);
ssize_t tls_send(const conn_t *c, const void *buf, size_t len);

/* "TLSv1.3, TLS_AES_256_GCM_SHA384" for display, or "" if not TLS. */
void tls_describe(const conn_t *c, char *out, size_t outsz);

#endif /* TLS_H */
