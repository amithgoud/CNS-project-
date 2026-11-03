/*
 * server.h - TCP server core: listening socket, accept loop and the
 * per-connection worker threads.
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
} conn_t;

/* Snapshot of server counters, shown on the /status page. */
typedef struct {
    int           active;       /* connections being served right now */
    int           max;          /* MAX_CONNECTIONS */
    unsigned long accepted;     /* total accepted since start */
    unsigned long rejected;     /* turned away because the server was full */
    time_t        started;      /* server start time */
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
 * Send the whole buffer, looping over partial writes. send() may write
 * fewer bytes than asked when the socket's send buffer is full, so a
 * single call is not enough. Returns len on success, -1 on error.
 */
ssize_t send_all(int fd, const void *buf, size_t len);

#endif /* SERVER_H */
