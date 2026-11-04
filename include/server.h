/*
 * server.h - TCP server core: listening socket, accept loop, worker
 * threads, and the per-connection I/O functions every other module uses.
 */
#ifndef SERVER_H
#define SERVER_H

#include <netinet/in.h>
#include <stddef.h>
#include <sys/types.h>
#include <time.h>

/* Everything a worker thread knows about its TCP connection. */
typedef struct {
    int            fd;
    char           ip[INET_ADDRSTRLEN];   /* client address, e.g. "10.0.0.5" */
    unsigned short port;                  /* client's ephemeral port */
    unsigned long  id;                    /* sequential, for readable logs */
    void          *tls;                   /* OpenSSL SSL* when HTTPS, else NULL */
} conn_t;

/* Snapshot of server counters, shown on the /status page. */
typedef struct {
    int           active;       /* connections being served right now */
    int           max;          /* MAX_CONNECTIONS */
    unsigned long accepted;     /* total accepted since start */
    unsigned long rejected;     /* turned away because the server was full */
    unsigned long rate_limited; /* turned away by the per-IP rate limit */
    time_t        started;      /* server start time */
    int           https;        /* 1 if serving HTTPS */
    double        rate_limit;   /* requests/second per IP, 0 = off */
} server_stats_t;

/*
 * Create the listening socket, run the accept loop until
 * server_request_stop() is called, then wait for active connections to
 * finish. Returns 0 on clean shutdown, -1 if the socket could not be set up.
 */
int  server_run(unsigned short port);

/* Ask the accept loop to stop. Async-signal-safe (called from SIGINT). */
void server_request_stop(void);

/* Copy the current counters (taken under the connection mutex). */
void server_get_stats(server_stats_t *out);

/*
 * Connection I/O. These hide whether the connection is plain TCP or TLS,
 * so the HTTP code is identical for HTTP and HTTPS.
 *
 * conn_recv: like recv(): >0 bytes, 0 when the peer closed, -1 on error
 *            with errno set (EAGAIN/EWOULDBLOCK means the timeout expired).
 * conn_send_all: send the whole buffer, looping over partial writes
 *            (send() may write fewer bytes than asked when the socket
 *            buffer is full). Returns len on success, -1 on error.
 */
ssize_t conn_recv(const conn_t *c, void *buf, size_t len);
ssize_t conn_send_all(const conn_t *c, const void *buf, size_t len);

#endif /* SERVER_H */
