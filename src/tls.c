/*
 * tls.c - Optional HTTPS support with OpenSSL (see tls.h).
 */
#include "tls.h"
#include "log.h"

#include <stdio.h>

#ifdef SQMS_TLS

#include <errno.h>
#include <limits.h>
#include <openssl/err.h>
#include <openssl/ssl.h>

static SSL_CTX *ctx = NULL;

/* Log and clear OpenSSL's per-thread error queue. */
static void log_ssl_errors(const char *what)
{
    unsigned long e;
    char buf[256];
    int any = 0;
    while ((e = ERR_get_error()) != 0) {
        ERR_error_string_n(e, buf, sizeof buf);
        log_warn("tls: %s: %s", what, buf);
        any = 1;
    }
    if (!any)
        log_warn("tls: %s", what);
}

int tls_init(const char *cert_file, const char *key_file)
{
    ctx = SSL_CTX_new(TLS_server_method());
    if (!ctx) {
        log_ssl_errors("SSL_CTX_new");
        return -1;
    }
    /* TLS 1.0 and 1.1 are obsolete and insecure. */
    SSL_CTX_set_min_proto_version(ctx, TLS1_2_VERSION);

    if (SSL_CTX_use_certificate_chain_file(ctx, cert_file) != 1 ||
        SSL_CTX_use_PrivateKey_file(ctx, key_file, SSL_FILETYPE_PEM) != 1 ||
        SSL_CTX_check_private_key(ctx) != 1) {
        log_ssl_errors("loading certificate/key");
        log_error("tls: could not load %s and %s (create them with: make cert)",
                  cert_file, key_file);
        SSL_CTX_free(ctx);
        ctx = NULL;
        return -1;
    }
    log_info("tls: HTTPS enabled with certificate %s", cert_file);
    return 0;
}

void tls_cleanup(void)
{
    SSL_CTX_free(ctx);
    ctx = NULL;
}

int tls_enabled(void)
{
    return ctx != NULL;
}

int tls_accept(conn_t *c)
{
    SSL *ssl = SSL_new(ctx);
    if (!ssl)
        return -1;
    SSL_set_fd(ssl, c->fd);

    /* The TLS handshake runs in the worker thread, not the accept loop,
     * so one slow handshake never delays other clients. The socket's
     * SO_RCVTIMEO bounds how long it can take. */
    if (SSL_accept(ssl) != 1) {
        char what[64];
        snprintf(what, sizeof what, "[conn %lu] handshake failed", c->id);
        log_ssl_errors(what);
        SSL_free(ssl);
        return -1;
    }
    c->tls = ssl;
    log_info("[conn %lu] TLS established: %s, %s", c->id,
             SSL_get_version(ssl), SSL_get_cipher(ssl));
    return 0;
}

void tls_close(conn_t *c)
{
    if (!c->tls)
        return;
    SSL_shutdown((SSL *)c->tls);        /* send close_notify alert */
    SSL_free((SSL *)c->tls);
    c->tls = NULL;
    ERR_clear_error();
}

/* Map an SSL_read/SSL_write failure onto recv()-style errno values. */
static ssize_t map_error(SSL *ssl, int n)
{
    int err = SSL_get_error(ssl, n);
    ERR_clear_error();
    if (err == SSL_ERROR_ZERO_RETURN)
        return 0;                       /* peer sent close_notify */
    if (err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE) {
        errno = EAGAIN;                 /* SO_RCVTIMEO / SO_SNDTIMEO expired */
        return -1;
    }
    if (err == SSL_ERROR_SYSCALL && (errno == EAGAIN || errno == EWOULDBLOCK))
        return -1;
    errno = ECONNRESET;
    return -1;
}

ssize_t tls_recv(const conn_t *c, void *buf, size_t len)
{
    SSL *ssl = c->tls;
    int n = SSL_read(ssl, buf, len > INT_MAX ? INT_MAX : (int)len);
    return n > 0 ? n : map_error(ssl, n);
}

ssize_t tls_send(const conn_t *c, const void *buf, size_t len)
{
    SSL *ssl = c->tls;
    int n = SSL_write(ssl, buf, len > INT_MAX ? INT_MAX : (int)len);
    return n > 0 ? n : map_error(ssl, n);
}

void tls_describe(const conn_t *c, char *out, size_t outsz)
{
    if (!c->tls) {
        snprintf(out, outsz, "%s", "");
        return;
    }
    snprintf(out, outsz, "%s, %s", SSL_get_version((SSL *)c->tls),
             SSL_get_cipher((SSL *)c->tls));
}

#else /* !SQMS_TLS: build without OpenSSL */

#include <errno.h>

int tls_init(const char *cert_file, const char *key_file)
{
    (void)cert_file;
    (void)key_file;
    log_error("tls: this binary was built without HTTPS support; "
              "rebuild with: make clean && make HTTPS=1");
    return -1;
}

void tls_cleanup(void) {}
int  tls_enabled(void) { return 0; }
int  tls_accept(conn_t *c) { (void)c; return -1; }
void tls_close(conn_t *c) { (void)c; }

ssize_t tls_recv(const conn_t *c, void *buf, size_t len)
{
    (void)c; (void)buf; (void)len;
    errno = ENOTSUP;
    return -1;
}

ssize_t tls_send(const conn_t *c, const void *buf, size_t len)
{
    (void)c; (void)buf; (void)len;
    errno = ENOTSUP;
    return -1;
}

void tls_describe(const conn_t *c, char *out, size_t outsz)
{
    (void)c;
    snprintf(out, outsz, "%s", "");
}

#endif /* SQMS_TLS */
