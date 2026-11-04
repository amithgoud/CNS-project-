/*
 * http.h - Minimal hand-written HTTP/1.1: request parser and response
 * builder. No HTTP library is used; everything is plain text over the
 * TCP socket, exactly as the protocol defines it.
 *
 * A request on the wire looks like:
 *
 *   POST /echo HTTP/1.1\r\n                 <- request line
 *   Host: localhost:8080\r\n                <- headers
 *   Content-Type: application/x-www-form-urlencoded\r\n
 *   Content-Length: 13\r\n
 *   \r\n                                    <- blank line ends headers
 *   message=hello                           <- body (Content-Length bytes)
 *
 * and a response:
 *
 *   HTTP/1.1 200 OK\r\n                     <- status line
 *   Content-Type: text/html; charset=utf-8\r\n
 *   Content-Length: 1234\r\n
 *   Connection: close\r\n
 *   \r\n
 *   <!DOCTYPE html>...
 */
#ifndef HTTP_H
#define HTTP_H

#include "config.h"
#include "server.h"

#include <stddef.h>

#define HTTP_MAX_HEADERS 32

typedef struct {
    char *name;
    char *value;
} http_header_t;

/*
 * A parsed request. All char* fields point into raw[], which holds the
 * bytes received from the socket (the parser writes NULs into it to cut
 * it into strings), so no other allocation is needed.
 */
typedef struct {
    char         *method;       /* "GET", "POST", ... */
    char         *path;         /* "/echo" (without the query string) */
    char         *query;        /* "a=1&b=2" or NULL if no '?' */
    char         *version;      /* "HTTP/1.1" */
    http_header_t headers[HTTP_MAX_HEADERS];
    int           header_count;
    char         *body;         /* NUL-terminated, body_len bytes */
    size_t        body_len;
    char          raw[MAX_HEADER_SIZE + MAX_BODY_SIZE + 1];
} http_request_t;

/*
 * Read and parse one request from the socket.
 *   0   success, *req is filled in
 *   >0  the request was bad; the value is the HTTP status to reply with
 *       (400, 408, 413, 431, 501, 505)
 *   -1  the client closed or went silent before sending anything; there
 *       is no one to reply to
 */
int http_read_request(const conn_t *c, http_request_t *req);

/* Case-insensitive header lookup (header names are case-insensitive
 * per RFC 9110). Returns NULL if absent. */
const char *http_get_header(const http_request_t *req, const char *name);

/*
 * Look up `key` in URL-encoded form data ("a=1&b=hello+world") and copy its
 * decoded value into out (always NUL-terminated, truncated to outsz-1).
 * Works for both query strings and POST bodies. Returns 1 if found.
 */
int http_form_get(const char *data, const char *key, char *out, size_t outsz);

/*
 * Find cookie `name` in the "Cookie: a=1; b=2" request header and copy its
 * value into out. Returns 1 if found and it fit in outsz, else 0.
 */
int http_get_cookie(const http_request_t *req, const char *name,
                    char *out, size_t outsz);

/* "OK", "Not Found", ... */
const char *http_status_text(int status);

/*
 * Send a complete response: status line, standard headers (Date, Server,
 * Content-Type, Content-Length, Connection: close), any extra_headers
 * (each line ending in \r\n, may be NULL), blank line, body.
 * Returns status on success, -1 if sending failed.
 */
int http_send_response(const conn_t *c, int status, const char *content_type,
                       const char *extra_headers,
                       const void *body, size_t body_len);

/* 302 Found with a Location header (the browser then GETs that URL). */
int http_send_redirect(const conn_t *c, const char *location, const char *extra_headers);

/* Small HTML error page. message may be NULL for a default text. */
int http_send_error(const conn_t *c, int status, const char *extra_headers,
                    const char *message);

#endif /* HTTP_H */
