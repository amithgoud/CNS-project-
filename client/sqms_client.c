/*
 * sqms_client.c - Command-line client for SQMS, built on raw TCP sockets.
 *
 * Shows the other side of the client-server model: the client resolves
 * the server's address (getaddrinfo), opens a TCP connection (socket +
 * connect, which performs the three-way handshake), writes an HTTP
 * request by hand, reads the response until the server closes the
 * connection, and parses the status line and headers itself.
 *
 * It keeps the session cookie in a file (default ./.sqms_cookie), just as
 * a browser keeps it in its cookie jar.
 *
 * Usage:
 *   sqms_client [-H host] [-p port] [-c cookiefile] [-v] <command> [args]
 *
 *   get PATH                 send GET PATH and print the raw response
 *   register USER PASS       create a student account (and log in)
 *   login USER PASS          log in and save the session cookie
 *   logout                   end the session
 *   ask "QUESTION"           submit a question; prints its number
 *   list                     list my questions
 *   show ID                  show one question (and its answer)
 *   wait ID [STATE] [SECS]   poll every second until ID reaches STATE
 *                            (default LLM_ANSWERED, timeout 60 s)
 *
 *   -v  verbose: print the request and response headers, and timings
 */
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#define MAX_RESPONSE   (4 * 1024 * 1024)
#define TIMEOUT_SEC    30
#define COOKIE_NAME    "sqms_session"

static const char *host = "127.0.0.1";
static const char *port = "8080";
static const char *cookie_file = ".sqms_cookie";
static int verbose = 0;
static char cookie[200];            /* "sqms_session=<token>" or "" */

/* ------------------------------------------------------------------ */
/* Growable buffer                                                    */
/* ------------------------------------------------------------------ */

typedef struct {
    char  *data;
    size_t len, cap;
} buf_t;

static void buf_append(buf_t *b, const char *s, size_t n)
{
    if (b->len + n + 1 > b->cap) {
        size_t cap = b->cap ? b->cap : 1024;
        while (cap < b->len + n + 1)
            cap *= 2;
        char *p = realloc(b->data, cap);
        if (!p) {
            fprintf(stderr, "out of memory\n");
            exit(1);
        }
        b->data = p;
        b->cap = cap;
    }
    memcpy(b->data + b->len, s, n);
    b->len += n;
    b->data[b->len] = '\0';
}

static void buf_printf(buf_t *b, const char *fmt, ...)
{
    va_list ap, ap2;
    va_start(ap, fmt);
    va_copy(ap2, ap);
    int n = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    if (n > 0) {
        char *tmp = malloc((size_t)n + 1);
        if (!tmp) {
            fprintf(stderr, "out of memory\n");
            exit(1);
        }
        vsnprintf(tmp, (size_t)n + 1, fmt, ap2);
        buf_append(b, tmp, (size_t)n);
        free(tmp);
    }
    va_end(ap2);
}

/* application/x-www-form-urlencoded: keep A-Z a-z 0-9 - _ . ~, space -> '+',
 * everything else -> %XX. */
static void url_encode(buf_t *b, const char *s)
{
    static const char hex[] = "0123456789ABCDEF";
    for (; *s; s++) {
        unsigned char ch = (unsigned char)*s;
        if ((ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') ||
            (ch >= '0' && ch <= '9') || ch == '-' || ch == '_' || ch == '.' || ch == '~') {
            buf_append(b, (const char *)&ch, 1);
        } else if (ch == ' ') {
            buf_append(b, "+", 1);
        } else {
            char enc[3] = { '%', hex[ch >> 4], hex[ch & 15] };
            buf_append(b, enc, 3);
        }
    }
}

static double now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1e6;
}

/* ------------------------------------------------------------------ */
/* Cookie jar                                                         */
/* ------------------------------------------------------------------ */

static void load_cookie(void)
{
    FILE *f = fopen(cookie_file, "r");
    cookie[0] = '\0';
    if (!f)
        return;
    if (fgets(cookie, sizeof cookie, f))
        cookie[strcspn(cookie, "\r\n")] = '\0';
    fclose(f);
}

static void save_cookie(const char *value)
{
    /* 0600: the token is a secret, only the owner may read the file. */
    int fd = open(cookie_file, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) {
        perror(cookie_file);
        return;
    }
    FILE *f = fdopen(fd, "w");
    if (!f) {
        close(fd);
        return;
    }
    fprintf(f, "%s\n", value);
    fclose(f);
    snprintf(cookie, sizeof cookie, "%s", value);
}

static void clear_cookie(void)
{
    remove(cookie_file);
    cookie[0] = '\0';
}

/* ------------------------------------------------------------------ */
/* HTTP over a raw TCP socket                                         */
/* ------------------------------------------------------------------ */

typedef struct {
    int    status;
    buf_t  raw;                 /* whole response as received */
    char  *body;                /* points into raw */
    size_t body_len;
    char   location[512];
    char   set_cookie[200];     /* "sqms_session=<token>" if the server set one */
    int    cookie_cleared;      /* server sent sqms_session= with no value */
} response_t;

static void response_free(response_t *r)
{
    free(r->raw.data);
    memset(r, 0, sizeof *r);
}

/* getaddrinfo + socket + connect. The kernel performs the TCP three-way
 * handshake inside connect(). */
static int connect_to_server(void)
{
    struct addrinfo hints, *res, *ai;
    memset(&hints, 0, sizeof hints);
    hints.ai_family   = AF_UNSPEC;      /* IPv4 or IPv6 */
    hints.ai_socktype = SOCK_STREAM;    /* TCP */

    int rc = getaddrinfo(host, port, &hints, &res);
    if (rc != 0) {
        fprintf(stderr, "cannot resolve %s: %s\n", host, gai_strerror(rc));
        return -1;
    }

    int fd = -1, err = 0;
    for (ai = res; ai; ai = ai->ai_next) {
        fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (fd < 0) {
            err = errno;
            continue;
        }
        if (connect(fd, ai->ai_addr, ai->ai_addrlen) == 0)
            break;
        err = errno;
        close(fd);
        fd = -1;
    }
    freeaddrinfo(res);

    if (fd < 0) {
        fprintf(stderr, "cannot connect to %s:%s: %s\n", host, port, strerror(err));
        return -1;
    }

    struct timeval tv = { TIMEOUT_SEC, 0 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
    return fd;
}

static int send_all(int fd, const char *p, size_t len)
{
    while (len > 0) {
        ssize_t n = send(fd, p, len, MSG_NOSIGNAL);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        p += n;
        len -= (size_t)n;
    }
    return 0;
}

static void print_prefixed(const char *prefix, const char *text, size_t len)
{
    const char *p = text, *end = text + len;
    while (p < end) {
        const char *nl = memchr(p, '\n', (size_t)(end - p));
        size_t n = nl ? (size_t)(nl - p) : (size_t)(end - p);
        if (n > 0 && p[n - 1] == '\r')
            n--;
        fprintf(stderr, "%s%.*s\n", prefix, (int)n, p);
        p = nl ? nl + 1 : end;
    }
}

/* Parse status line and headers; set body. 0 on success. */
static int parse_response(response_t *r)
{
    char *raw = r->raw.data;
    if (!raw || sscanf(raw, "HTTP/%*d.%*d %d", &r->status) != 1)
        return -1;

    char *hdr_end = strstr(raw, "\r\n\r\n");
    if (!hdr_end)
        return -1;
    r->body = hdr_end + 4;
    r->body_len = r->raw.len - (size_t)(r->body - raw);

    char *line = strstr(raw, "\r\n") + 2;      /* skip status line */
    while (line < hdr_end) {
        char *eol = strstr(line, "\r\n");
        size_t len = (size_t)(eol - line);
        char *colon = memchr(line, ':', len);
        if (colon) {
            size_t nlen = (size_t)(colon - line);
            char *val = colon + 1;
            while (*val == ' ')
                val++;
            size_t vlen = (size_t)(eol - val);

            if (nlen == 8 && strncasecmp(line, "Location", 8) == 0) {
                snprintf(r->location, sizeof r->location, "%.*s", (int)vlen, val);
            } else if (nlen == 10 && strncasecmp(line, "Set-Cookie", 10) == 0 &&
                       strncmp(val, COOKIE_NAME "=", strlen(COOKIE_NAME) + 1) == 0) {
                size_t clen = strcspn(val, ";\r");
                if (clen == strlen(COOKIE_NAME) + 1)
                    r->cookie_cleared = 1;
                else
                    snprintf(r->set_cookie, sizeof r->set_cookie, "%.*s", (int)clen, val);
            } else if (nlen == 14 && strncasecmp(line, "Content-Length", 14) == 0) {
                size_t cl = (size_t)strtoull(val, NULL, 10);
                if (cl < r->body_len)
                    r->body_len = cl;
            }
        }
        line = eol + 2;
    }
    return 0;
}

/*
 * Send one request (one TCP connection, like the server's Connection:
 * close) and read the full response. form_body may be NULL.
 */
static int http_request(const char *method, const char *path, const char *form_body,
                        response_t *r)
{
    memset(r, 0, sizeof *r);

    double t0 = now_ms();
    int fd = connect_to_server();
    if (fd < 0)
        return -1;
    double t_conn = now_ms();

    buf_t req = { 0 };
    buf_printf(&req, "%s %s HTTP/1.1\r\n", method, path);
    buf_printf(&req, "Host: %s:%s\r\n", host, port);
    buf_printf(&req, "User-Agent: sqms_client/1.0\r\n");
    buf_printf(&req, "Accept: */*\r\n");
    buf_printf(&req, "Connection: close\r\n");
    if (cookie[0])
        buf_printf(&req, "Cookie: %s\r\n", cookie);
    if (form_body) {
        buf_printf(&req, "Content-Type: application/x-www-form-urlencoded\r\n");
        buf_printf(&req, "Content-Length: %zu\r\n", strlen(form_body));
    }
    buf_append(&req, "\r\n", 2);
    if (form_body)
        buf_append(&req, form_body, strlen(form_body));

    if (verbose) {
        fprintf(stderr, "* connected to %s:%s in %.1f ms\n", host, port, t_conn - t0);
        print_prefixed("> ", req.data, req.len);
    }

    int rc = send_all(fd, req.data, req.len);
    free(req.data);
    if (rc < 0) {
        fprintf(stderr, "send failed: %s\n", strerror(errno));
        close(fd);
        return -1;
    }

    /* The server closes the connection after the response
     * (Connection: close), so read until recv() returns 0. */
    char chunk[4096];
    for (;;) {
        ssize_t n = recv(fd, chunk, sizeof chunk, 0);
        if (n > 0) {
            if (r->raw.len + (size_t)n > MAX_RESPONSE) {
                fprintf(stderr, "response too large\n");
                break;
            }
            buf_append(&r->raw, chunk, (size_t)n);
            continue;
        }
        if (n < 0 && errno == EINTR)
            continue;
        if (n < 0)
            fprintf(stderr, "recv failed: %s\n", strerror(errno));
        break;
    }
    close(fd);

    if (parse_response(r) < 0) {
        fprintf(stderr, "invalid HTTP response from server\n");
        response_free(r);
        return -1;
    }

    if (verbose) {
        print_prefixed("< ", r->raw.data, (size_t)(r->body - r->raw.data - 2));
        fprintf(stderr, "* %zu bytes in %.1f ms, connection closed by server\n",
                r->raw.len, now_ms() - t0);
    }

    if (r->set_cookie[0])
        save_cookie(r->set_cookie);
    else if (r->cookie_cleared)
        clear_cookie();
    return 0;
}

static int is_login_redirect(const response_t *r)
{
    return r->status == 302 && strncmp(r->location, "/login", 6) == 0;
}

/* Print the human-readable message from an HTML error page or form alert. */
static void print_server_message(const response_t *r)
{
    const char *start = NULL, *end = NULL;
    const char *a = r->body ? strstr(r->body, "class=\"alert error\">") : NULL;
    if (a) {
        start = strchr(a, '>') + 1;
        end = strstr(start, "</div>");
    } else if (r->body && (a = strstr(r->body, "</h1><p>")) != NULL) {
        start = a + 8;
        end = strstr(start, "</p>");
    }
    if (!start || !end) {
        fprintf(stderr, "server answered HTTP %d\n", r->status);
        return;
    }

    /* Undo the server's HTML escaping for display. */
    fprintf(stderr, "server answered HTTP %d: ", r->status);
    for (const char *p = start; p < end; p++) {
        if (*p == '&') {
            static const struct { const char *ent; char ch; } ents[] = {
                { "&amp;", '&' }, { "&lt;", '<' }, { "&gt;", '>' },
                { "&quot;", '"' }, { "&#39;", '\'' },
            };
            int matched = 0;
            for (size_t i = 0; i < sizeof ents / sizeof ents[0]; i++) {
                size_t n = strlen(ents[i].ent);
                if (strncmp(p, ents[i].ent, n) == 0) {
                    fputc(ents[i].ch, stderr);
                    p += n - 1;
                    matched = 1;
                    break;
                }
            }
            if (matched)
                continue;
        }
        fputc(*p, stderr);
    }
    fputc('\n', stderr);
}

/* ------------------------------------------------------------------ */
/* Commands                                                           */
/* ------------------------------------------------------------------ */

static int cmd_get(const char *path)
{
    response_t r;
    if (http_request("GET", path, NULL, &r) < 0)
        return 1;
    fwrite(r.raw.data, 1, r.raw.len, stdout);
    int rc = r.status < 400 ? 0 : 1;
    response_free(&r);
    return rc;
}

static int cmd_login(const char *user, const char *pass, int registering)
{
    buf_t body = { 0 };
    buf_append(&body, "username=", 9);
    url_encode(&body, user);
    buf_append(&body, "&password=", 10);
    url_encode(&body, pass);
    if (registering) {
        buf_append(&body, "&confirm=", 9);
        url_encode(&body, pass);
    }

    response_t r;
    int rc = http_request("POST", registering ? "/register" : "/login", body.data, &r);
    free(body.data);
    if (rc < 0)
        return 1;

    if (r.status == 302 && r.set_cookie[0]) {
        printf("%s as %s (session cookie saved in %s)\n",
               registering ? "Registered and logged in" : "Logged in", user, cookie_file);
        rc = 0;
    } else {
        print_server_message(&r);
        rc = 1;
    }
    response_free(&r);
    return rc;
}

static int cmd_logout(void)
{
    response_t r;
    if (http_request("POST", "/logout", "", &r) < 0)
        return 1;
    clear_cookie();
    printf("Logged out\n");
    response_free(&r);
    return 0;
}

static int cmd_ask(const char *question)
{
    buf_t body = { 0 };
    buf_append(&body, "question=", 9);
    url_encode(&body, question);

    response_t r;
    int rc = http_request("POST", "/ask", body.data, &r);
    free(body.data);
    if (rc < 0)
        return 1;

    const char *id = strstr(r.location, "id=");
    if (r.status == 302 && id) {
        printf("Question #%lld submitted (state PENDING_LLM)\n", strtoll(id + 3, NULL, 10));
        rc = 0;
    } else if (is_login_redirect(&r)) {
        fprintf(stderr, "not logged in as a student: run 'login USER PASS' first\n");
        rc = 1;
    } else {
        print_server_message(&r);
        rc = 1;
    }
    response_free(&r);
    return rc;
}

/* GET a plain-text API path and print the body. */
static int cmd_api(const char *path)
{
    response_t r;
    if (http_request("GET", path, NULL, &r) < 0)
        return 1;

    int rc = 1;
    if (r.status == 200) {
        fwrite(r.body, 1, r.body_len, stdout);
        rc = 0;
    } else if (is_login_redirect(&r)) {
        fprintf(stderr, "not logged in: run 'login USER PASS' first\n");
    } else {
        print_server_message(&r);
    }
    response_free(&r);
    return rc;
}

static const char *const STATES[] = {
    "PENDING_LLM", "LLM_ANSWERED", "TEACHER_APPROVED", "TEACHER_EDITED", "DELIVERED",
};

static int state_rank(const char *s)
{
    for (int i = 0; i < 5; i++)
        if (strcmp(s, STATES[i]) == 0)
            return i == 3 ? 2 : i;      /* APPROVED and EDITED are the same stage */
    return -1;
}

/* Poll /api/query every second until the question reaches `target`. */
static int cmd_wait(long long id, const char *target, int timeout)
{
    int want = state_rank(target);
    if (want < 0) {
        fprintf(stderr, "unknown state '%s'\n", target);
        return 1;
    }

    char path[64];
    snprintf(path, sizeof path, "/api/query?id=%lld", id);
    double t0 = now_ms();

    for (int i = 0; i <= timeout; i++) {
        response_t r;
        if (http_request("GET", path, NULL, &r) < 0)
            return 1;
        if (r.status != 200) {
            if (is_login_redirect(&r))
                fprintf(stderr, "not logged in: run 'login USER PASS' first\n");
            else
                print_server_message(&r);
            response_free(&r);
            return 1;
        }

        char state[32] = "";
        const char *st = strstr(r.body, "state: ");
        if (st)
            sscanf(st + 7, "%31s", state);
        response_free(&r);

        if (state_rank(state) >= want) {
            printf("Question #%lld reached %s after %.1f s\n", id, state,
                   (now_ms() - t0) / 1000.0);
            return 0;
        }
        if (verbose)
            fprintf(stderr, "* #%lld is %s, polling again in 1 s\n", id, state);
        sleep(1);
    }
    fprintf(stderr, "timeout: question #%lld did not reach %s within %d s\n",
            id, target, timeout);
    return 2;
}

static void usage(void)
{
    fprintf(stderr,
        "usage: sqms_client [-H host] [-p port] [-c cookiefile] [-v] <command> [args]\n"
        "\n"
        "commands:\n"
        "  get PATH                 send GET PATH and print the raw HTTP response\n"
        "  register USER PASS       create a student account (and log in)\n"
        "  login USER PASS          log in and save the session cookie\n"
        "  logout                   end the session\n"
        "  ask \"QUESTION\"           submit a question\n"
        "  list                     list my questions\n"
        "  show ID                  show one question and its answer\n"
        "  wait ID [STATE] [SECS]   poll until ID reaches STATE (default LLM_ANSWERED, 60 s)\n"
        "\n"
        "options:\n"
        "  -H host   server host (default 127.0.0.1)\n"
        "  -p port   server port (default 8080)\n"
        "  -c file   cookie file (default .sqms_cookie)\n"
        "  -v        verbose: show request/response headers and timings\n");
}

int main(int argc, char **argv)
{
    int opt;
    while ((opt = getopt(argc, argv, "H:p:c:v")) != -1) {
        switch (opt) {
        case 'H': host = optarg; break;
        case 'p': port = optarg; break;
        case 'c': cookie_file = optarg; break;
        case 'v': verbose = 1; break;
        default:  usage(); return 1;
        }
    }
    if (optind >= argc) {
        usage();
        return 1;
    }

    load_cookie();

    const char *cmd = argv[optind];
    int nargs = argc - optind - 1;
    char **args = argv + optind + 1;

    if (strcmp(cmd, "get") == 0 && nargs == 1)
        return cmd_get(args[0]);
    if (strcmp(cmd, "register") == 0 && nargs == 2)
        return cmd_login(args[0], args[1], 1);
    if (strcmp(cmd, "login") == 0 && nargs == 2)
        return cmd_login(args[0], args[1], 0);
    if (strcmp(cmd, "logout") == 0 && nargs == 0)
        return cmd_logout();
    if (strcmp(cmd, "ask") == 0 && nargs == 1)
        return cmd_ask(args[0]);
    if (strcmp(cmd, "list") == 0 && nargs == 0)
        return cmd_api("/api/queries");
    if (strcmp(cmd, "show") == 0 && nargs == 1) {
        char path[64];
        snprintf(path, sizeof path, "/api/query?id=%lld", strtoll(args[0], NULL, 10));
        return cmd_api(path);
    }
    if (strcmp(cmd, "wait") == 0 && nargs >= 1 && nargs <= 3)
        return cmd_wait(strtoll(args[0], NULL, 10),
                        nargs >= 2 ? args[1] : "LLM_ANSWERED",
                        nargs >= 3 ? atoi(args[2]) : 60);

    usage();
    return 1;
}
