/*
 * http.c - Hand-written HTTP/1.1 request parser and response builder.
 */
#include "http.h"
#include "server.h"
#include "strbuf.h"

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <time.h>

/* ------------------------------------------------------------------ */
/* Parsing                                                            */
/* ------------------------------------------------------------------ */

/* A method is a token of upper-case letters ("GET", "POST", ...). */
static int valid_method(const char *m)
{
    if (!*m)
        return 0;
    for (; *m; m++)
        if (!isupper((unsigned char)*m))
            return 0;
    return 1;
}

const char *http_get_header(const http_request_t *req, const char *name)
{
    for (int i = 0; i < req->header_count; i++)
        if (strcasecmp(req->headers[i].name, name) == 0)
            return req->headers[i].value;
    return NULL;
}

/* recv() wrapper: map errors to "what should the caller do".
 * Returns bytes read (>0), 0 on peer close, -1 timeout, -2 other error. */
static ssize_t recv_some(const conn_t *c, char *buf, size_t len)
{
    for (;;) {
        ssize_t n = conn_recv(c, buf, len);
        if (n >= 0)
            return n;
        if (errno == EINTR)
            continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK)
            return -1;          /* SO_RCVTIMEO expired */
        return -2;              /* ECONNRESET etc. */
    }
}

int http_read_request(const conn_t *c, http_request_t *req)
{
    char  *raw = req->raw;
    size_t used = 0;
    char  *hdr_end = NULL;

    req->header_count = 0;
    req->query = NULL;
    req->body = NULL;
    req->body_len = 0;

    /* --- 1. Receive until the blank line that ends the headers. -------
     * TCP is a byte stream, not a message protocol: one request may
     * arrive split over several recv() calls, so we keep reading until
     * we see "\r\n\r\n". Any bytes after it are the start of the body. */
    while (!hdr_end) {
        if (used >= MAX_HEADER_SIZE)
            return 431;                         /* headers too large */

        ssize_t n = recv_some(c, raw + used, MAX_HEADER_SIZE - used);
        if (n == 0)
            return used == 0 ? -1 : 400;        /* closed mid-request */
        if (n == -1)
            return used == 0 ? -1 : 408;        /* went silent mid-request */
        if (n < 0)
            return -1;

        used += (size_t)n;
        raw[used] = '\0';
        hdr_end = strstr(raw, "\r\n\r\n");
    }

    char  *body_start = hdr_end + 4;
    size_t have_body  = used - (size_t)(body_start - raw);
    *hdr_end = '\0';            /* raw is now just request line + headers */

    /* --- 2. Request line: METHOD SP target SP version ------------------ */
    char *headers_start = NULL;
    char *eol = strstr(raw, "\r\n");
    if (eol) {
        *eol = '\0';
        headers_start = eol + 2;
    }

    req->method = raw;
    char *sp = strchr(raw, ' ');
    if (!sp)
        return 400;
    *sp = '\0';
    req->path = sp + 1;
    sp = strchr(req->path, ' ');
    if (!sp)
        return 400;
    *sp = '\0';
    req->version = sp + 1;

    if (!valid_method(req->method) || req->path[0] != '/')
        return 400;
    if (strncmp(req->version, "HTTP/", 5) != 0)
        return 400;
    if (strcmp(req->version, "HTTP/1.1") != 0 &&
        strcmp(req->version, "HTTP/1.0") != 0)
        return 505;

    char *qmark = strchr(req->path, '?');
    if (qmark) {
        *qmark = '\0';
        req->query = qmark + 1;
    }

    /* --- 3. Headers: "Name: value" per line ---------------------------- */
    char *line = headers_start;
    while (line && *line) {
        eol = strstr(line, "\r\n");
        if (eol)
            *eol = '\0';

        char *colon = strchr(line, ':');
        /* RFC 9112: no whitespace allowed between name and colon. */
        if (!colon || colon == line || colon[-1] == ' ' || colon[-1] == '\t')
            return 400;
        *colon = '\0';

        char *val = colon + 1;
        while (*val == ' ' || *val == '\t')
            val++;
        char *end = val + strlen(val);
        while (end > val && (end[-1] == ' ' || end[-1] == '\t'))
            *--end = '\0';

        if (req->header_count >= HTTP_MAX_HEADERS)
            return 431;
        req->headers[req->header_count].name  = line;
        req->headers[req->header_count].value = val;
        req->header_count++;

        line = eol ? eol + 2 : NULL;
    }

    /* --- 4. Body length from Content-Length ---------------------------- */
    if (http_get_header(req, "Transfer-Encoding"))
        return 501;             /* chunked uploads are not supported */

    size_t clen = 0;
    const char *cl = http_get_header(req, "Content-Length");
    if (cl) {
        char *endp;
        if (!isdigit((unsigned char)*cl))
            return 400;
        errno = 0;
        unsigned long long v = strtoull(cl, &endp, 10);
        if (errno != 0 || *endp != '\0')
            return 400;
        if (v > MAX_BODY_SIZE)
            return 413;         /* refuse before reading a single body byte */
        clen = (size_t)v;
    }

    /* Some clients (curl) send "Expect: 100-continue" and wait for the
     * server's go-ahead before uploading the body. */
    const char *expect = http_get_header(req, "Expect");
    if (clen > 0 && have_body == 0 && expect &&
        strcasecmp(expect, "100-continue") == 0) {
        static const char cont[] = "HTTP/1.1 100 Continue\r\n\r\n";
        conn_send_all(c, cont, sizeof cont - 1);
    }

    /* --- 5. Receive the rest of the body ------------------------------- */
    while (have_body < clen) {
        ssize_t n = recv_some(c, body_start + have_body, clen - have_body);
        if (n == 0)
            return 400;         /* body shorter than Content-Length */
        if (n == -1)
            return 408;
        if (n < 0)
            return -1;
        have_body += (size_t)n;
    }

    /* Fits: body_start is at most MAX_HEADER_SIZE into raw and
     * clen <= MAX_BODY_SIZE, and raw has one spare byte for the NUL. */
    body_start[clen] = '\0';
    req->body     = body_start;
    req->body_len = clen;
    return 0;
}

static int hexval(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return c - 'A' + 10;
}

/* Decode n bytes of application/x-www-form-urlencoded text:
 * '+' is a space and "%XX" is the byte with hex value XX. */
static void url_decode_n(const char *src, size_t n, char *out, size_t outsz)
{
    size_t o = 0;
    for (size_t i = 0; i < n && o + 1 < outsz; i++) {
        char c = src[i];
        if (c == '+') {
            c = ' ';
        } else if (c == '%' && i + 2 < n &&
                   isxdigit((unsigned char)src[i + 1]) &&
                   isxdigit((unsigned char)src[i + 2])) {
            c = (char)(hexval(src[i + 1]) * 16 + hexval(src[i + 2]));
            i += 2;
        }
        out[o++] = c;
    }
    out[o] = '\0';
}

int http_form_get(const char *data, const char *key, char *out, size_t outsz)
{
    if (outsz == 0)
        return 0;
    out[0] = '\0';
    if (!data)
        return 0;

    size_t klen = strlen(key);
    const char *p = data;

    for (;;) {
        const char *amp = strchr(p, '&');
        size_t plen = amp ? (size_t)(amp - p) : strlen(p);

        if (plen >= klen && strncmp(p, key, klen) == 0) {
            if (plen == klen)
                return 1;                       /* "key" with no '=' */
            if (p[klen] == '=') {
                url_decode_n(p + klen + 1, plen - klen - 1, out, outsz);
                return 1;
            }
        }
        if (!amp)
            return 0;
        p = amp + 1;
    }
}

int http_get_cookie(const http_request_t *req, const char *name,
                    char *out, size_t outsz)
{
    const char *h = http_get_header(req, "Cookie");
    size_t nlen = strlen(name);

    if (outsz == 0)
        return 0;
    out[0] = '\0';
    if (!h)
        return 0;

    const char *p = h;
    for (;;) {
        while (*p == ' ' || *p == ';')
            p++;
        if (!*p)
            return 0;

        const char *semi = strchr(p, ';');
        size_t len = semi ? (size_t)(semi - p) : strlen(p);

        if (len > nlen && strncmp(p, name, nlen) == 0 && p[nlen] == '=') {
            size_t vlen = len - nlen - 1;
            if (vlen >= outsz)
                return 0;
            memcpy(out, p + nlen + 1, vlen);
            out[vlen] = '\0';
            return 1;
        }
        if (!semi)
            return 0;
        p = semi + 1;
    }
}

/* ------------------------------------------------------------------ */
/* Responses                                                          */
/* ------------------------------------------------------------------ */

const char *http_status_text(int status)
{
    switch (status) {
    case 200: return "OK";
    case 302: return "Found";
    case 400: return "Bad Request";
    case 401: return "Unauthorized";
    case 403: return "Forbidden";
    case 404: return "Not Found";
    case 405: return "Method Not Allowed";
    case 408: return "Request Timeout";
    case 409: return "Conflict";
    case 413: return "Content Too Large";
    case 429: return "Too Many Requests";
    case 431: return "Request Header Fields Too Large";
    case 500: return "Internal Server Error";
    case 501: return "Not Implemented";
    case 503: return "Service Unavailable";
    case 505: return "HTTP Version Not Supported";
    default:  return "Unknown";
    }
}

int http_send_response(const conn_t *c, int status, const char *content_type,
                       const char *extra_headers,
                       const void *body, size_t body_len)
{
    /* HTTP dates are always GMT in RFC 1123 format. */
    char date[64];
    time_t now = time(NULL);
    struct tm tm;
    gmtime_r(&now, &tm);
    strftime(date, sizeof date, "%a, %d %b %Y %H:%M:%S GMT", &tm);

    strbuf_t out;
    sb_init(&out);

    /* Content-Length tells the client exactly where the body ends.
     * Connection: close means one request per TCP connection: after this
     * response we close the socket. That keeps each worker simple and
     * makes every request its own handshake when captured in Wireshark. */
    sb_printf(&out,
              "HTTP/1.1 %d %s\r\n"
              "Date: %s\r\n"
              "Server: " SERVER_NAME "\r\n"
              "Content-Type: %s\r\n"
              "Content-Length: %zu\r\n"
              "Connection: close\r\n"
              "Cache-Control: no-store\r\n"
              "X-Content-Type-Options: nosniff\r\n",
              status, http_status_text(status), date,
              content_type, body_len);
    if (extra_headers)
        sb_puts(&out, extra_headers);
    sb_puts(&out, "\r\n");                      /* end of headers */

    /* Headers and body go out in one buffer, so a small response
     * usually fits in a single TCP segment. */
    if (body_len > 0)
        sb_append(&out, body, body_len);

    int rc = -1;
    if (!out.oom && conn_send_all(c, out.data, out.len) >= 0)
        rc = status;
    sb_free(&out);
    return rc;
}

int http_send_redirect(const conn_t *c, const char *location, const char *extra_headers)
{
    /* A CR or LF inside a header value would let an attacker end the
     * header early and inject their own (response splitting). */
    if (strpbrk(location, "\r\n"))
        return http_send_error(c, 500, NULL, "Invalid redirect target.");

    strbuf_t hdr;
    sb_init(&hdr);
    sb_printf(&hdr, "Location: %s\r\n", location);
    if (extra_headers)
        sb_puts(&hdr, extra_headers);

    static const char body[] = "Redirecting...\n";
    int rc = hdr.oom ? -1
           : http_send_response(c, 302, "text/plain; charset=utf-8",
                                hdr.data, body, sizeof body - 1);
    sb_free(&hdr);
    return rc;
}

int http_send_error(const conn_t *c, int status, const char *extra_headers,
                    const char *message)
{
    const char *text = http_status_text(status);
    strbuf_t b;
    sb_init(&b);

    sb_printf(&b,
              "<!DOCTYPE html>\n<html lang=\"en\"><head><meta charset=\"utf-8\">"
              "<meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">"
              "<title>%d %s</title>"
              "<link rel=\"stylesheet\" href=\"/static/style.css\"></head>"
              "<body><main class=\"wrap\"><section class=\"card error\">"
              "<p class=\"status-code\">%d</p><h1>%s</h1><p>",
              status, text, status, text);
    sb_append_html(&b, message ? message : "The server could not handle this request.");
    sb_puts(&b, "</p><p><a class=\"button\" href=\"/\">Back to home</a></p>"
                "</section></main></body></html>\n");

    int rc;
    if (b.oom)
        rc = http_send_response(c, status, "text/plain; charset=utf-8",
                                extra_headers, text, strlen(text));
    else
        rc = http_send_response(c, status, "text/html; charset=utf-8",
                                extra_headers, b.data, b.len);
    sb_free(&b);
    return rc;
}
