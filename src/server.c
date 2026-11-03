/*
 * server.c - Multi-threaded TCP server.
 *
 * Socket lifecycle on the server side:
 *
 *   socket() -> setsockopt(SO_REUSEADDR) -> bind() -> listen()
 *        -> loop { accept() -> spawn worker thread }
 *
 *   worker:  setsockopt(SO_RCVTIMEO/SO_SNDTIMEO)
 *        -> recv() HTTP request -> route -> send() HTTP response
 *        -> shutdown(SHUT_WR) -> close()
 *
 * ---------------------------------------------------------------------
 * Why thread-per-connection?
 * ---------------------------------------------------------------------
 * Each accepted connection gets its own detached POSIX thread that uses
 * ordinary blocking recv()/send(). We chose this model because:
 *
 *  1. The code for one connection reads top-to-bottom like a single-client
 *     server, which makes the HTTP logic easy to follow and explain. An
 *     event loop (select/poll/epoll) would force every handler to be
 *     written as a state machine.
 *  2. A slow client (or a slow LLM call, which can take seconds) blocks
 *     only its own thread; all other students keep being served.
 *  3. Our load is small (a classroom: tens of clients), so the cost of one
 *     thread per connection (~8 MB virtual stack, a few KB actually used)
 *     is irrelevant.
 *
 * The weakness of this model is unbounded thread creation under heavy
 * load, so we cap concurrent connections at MAX_CONNECTIONS with a
 * mutex-protected counter. Connections beyond the cap get an immediate
 * "503 Service Unavailable" instead of being left hanging.
 * A fixed thread pool would be the next step for a larger deployment.
 */
#include "server.h"
#include "config.h"
#include "http.h"
#include "log.h"
#include "router.h"

#include <arpa/inet.h>
#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

/* ---------------------------------------------------------------------
 * Shared state, protected by conn_lock.
 * These counters are read and written by the accept thread and by every
 * worker, so all access goes through the mutex. conn_cv lets shutdown
 * wait until the last worker finishes.
 * ------------------------------------------------------------------- */
static pthread_mutex_t conn_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  conn_cv   = PTHREAD_COND_INITIALIZER;
static int             active_conns = 0;
static unsigned long   total_accepted = 0;
static unsigned long   total_rejected = 0;
static time_t          start_time;

/* Set from the signal handler; sig_atomic_t makes that write safe. */
static volatile sig_atomic_t stop_requested = 0;

void server_request_stop(void)
{
    stop_requested = 1;
}

void server_get_stats(server_stats_t *out)
{
    pthread_mutex_lock(&conn_lock);
    out->active   = active_conns;
    out->max      = MAX_CONNECTIONS;
    out->accepted = total_accepted;
    out->rejected = total_rejected;
    out->started  = start_time;
    pthread_mutex_unlock(&conn_lock);
}

/* Reserve a connection slot. Returns 1 on success, 0 if the server is full. */
static int acquire_slot(void)
{
    int ok = 0;
    pthread_mutex_lock(&conn_lock);
    if (active_conns < MAX_CONNECTIONS) {
        active_conns++;
        total_accepted++;
        ok = 1;
    } else {
        total_rejected++;
    }
    pthread_mutex_unlock(&conn_lock);
    return ok;
}

static void release_slot(void)
{
    pthread_mutex_lock(&conn_lock);
    active_conns--;
    pthread_cond_broadcast(&conn_cv);
    pthread_mutex_unlock(&conn_lock);
}

ssize_t send_all(int fd, const void *buf, size_t len)
{
    const char *p = buf;
    size_t left = len;

    while (left > 0) {
        /* MSG_NOSIGNAL: if the peer has gone away, return EPIPE instead of
         * raising SIGPIPE (which would kill the whole process). SIGPIPE is
         * also ignored in main(), so this is belt and braces. */
        ssize_t n = send(fd, p, left, MSG_NOSIGNAL);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        p    += n;
        left -= (size_t)n;
    }
    return (ssize_t)len;
}

/* Apply recv/send timeouts so a silent client cannot hold a thread forever. */
static void set_socket_timeouts(int fd)
{
    struct timeval tv;
    tv.tv_sec  = SOCKET_TIMEOUT_SEC;
    tv.tv_usec = 0;
    if (setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv) < 0)
        log_warn("setsockopt(SO_RCVTIMEO): %s", strerror(errno));
    if (setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv) < 0)
        log_warn("setsockopt(SO_SNDTIMEO): %s", strerror(errno));
}

/*
 * Close a connection without losing the response we just sent.
 *
 * If close() is called while unread data from the client is still in the
 * socket's receive buffer (e.g. the rest of a too-large upload we refused),
 * Linux sends a TCP RST instead of a FIN, and the client may throw away
 * our response before reading it. So we first send FIN with
 * shutdown(SHUT_WR), then briefly read and discard whatever the client
 * still sends until it closes its side, and only then close().
 */
static void close_gracefully(int fd)
{
    char sink[1024];
    size_t drained = 0;
    struct timeval tv = { 1, 0 };

    shutdown(fd, SHUT_WR);
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    while (drained < 64 * 1024) {
        ssize_t n = recv(fd, sink, sizeof sink, 0);
        if (n <= 0)
            break;              /* client's FIN (0), timeout or error */
        drained += (size_t)n;
    }
    close(fd);
}

/* Worker thread: serve one HTTP request on this connection, then close. */
static void *connection_thread(void *arg)
{
    conn_t *c = arg;

    log_info("[conn %lu] %s:%u connected", c->id, c->ip, c->port);

    /* ~24 KB: on the heap rather than the thread's stack. */
    http_request_t *req = malloc(sizeof *req);
    if (!req) {
        log_error("[conn %lu] out of memory", c->id);
        http_send_error(c->fd, 500, NULL, "Server out of memory.");
    } else {
        int rc = http_read_request(c->fd, req);
        if (rc == 0) {
            int status = router_handle(c, req);
            if (status < 0)
                log_warn("[conn %lu] \"%s %s\" -> response not delivered",
                         c->id, req->method, req->path);
            else
                log_info("[conn %lu] \"%s %s\" -> %d %s", c->id,
                         req->method, req->path, status, http_status_text(status));
        } else if (rc > 0) {
            log_warn("[conn %lu] bad request -> %d %s",
                     c->id, rc, http_status_text(rc));
            http_send_error(c->fd, rc, NULL, NULL);
        } else {
            /* Normal for browsers: they often open a spare connection in
             * advance and close it unused. */
            log_info("[conn %lu] closed or idle before sending a request", c->id);
        }
        free(req);
    }

    close_gracefully(c->fd);
    log_info("[conn %lu] %s:%u disconnected", c->id, c->ip, c->port);
    free(c);
    release_slot();
    return NULL;
}

/* socket() -> setsockopt(SO_REUSEADDR) -> bind() -> listen(). */
static int create_listen_socket(unsigned short port)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);       /* IPv4, TCP */
    if (fd < 0) {
        log_error("socket: %s", strerror(errno));
        return -1;
    }

    /* SO_REUSEADDR lets us restart the server immediately. Without it,
     * bind() fails with EADDRINUSE while old connections from the previous
     * run sit in TCP's TIME_WAIT state (up to 2*MSL, ~60s on Linux). */
    int yes = 1;
    if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof yes) < 0) {
        log_error("setsockopt(SO_REUSEADDR): %s", strerror(errno));
        close(fd);
        return -1;
    }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof addr);
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);  /* all local interfaces */
    addr.sin_port        = htons(port);        /* host -> network byte order */

    if (bind(fd, (struct sockaddr *)&addr, sizeof addr) < 0) {
        log_error("bind port %u: %s", port, strerror(errno));
        close(fd);
        return -1;
    }

    /* From here the kernel completes three-way handshakes on our behalf
     * and queues the connections until we accept() them. */
    if (listen(fd, LISTEN_BACKLOG) < 0) {
        log_error("listen: %s", strerror(errno));
        close(fd);
        return -1;
    }
    return fd;
}

/* Start a detached worker for an accepted connection. Returns 0 on success. */
static int spawn_worker(conn_t *c)
{
    pthread_attr_t attr;
    pthread_t tid;
    sigset_t block, old;
    int rc;

    /* Detached: the thread's resources are freed automatically when it
     * returns, so the accept loop never has to pthread_join() it. */
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);

    /* A signal sent to the process may be delivered to any thread that does
     * not block it. Workers block SIGINT/SIGTERM (they inherit the mask in
     * effect at pthread_create) so Ctrl+C always lands on the main thread
     * and interrupts its accept() call. */
    sigemptyset(&block);
    sigaddset(&block, SIGINT);
    sigaddset(&block, SIGTERM);
    pthread_sigmask(SIG_BLOCK, &block, &old);
    rc = pthread_create(&tid, &attr, connection_thread, c);
    pthread_sigmask(SIG_SETMASK, &old, NULL);

    pthread_attr_destroy(&attr);
    return rc;
}

/* On shutdown, wait (bounded) for in-flight connections to finish. */
static void wait_for_workers(void)
{
    struct timespec deadline;
    clock_gettime(CLOCK_REALTIME, &deadline);
    deadline.tv_sec += SHUTDOWN_GRACE_SEC;

    pthread_mutex_lock(&conn_lock);
    while (active_conns > 0) {
        if (pthread_cond_timedwait(&conn_cv, &conn_lock, &deadline) == ETIMEDOUT)
            break;
    }
    if (active_conns > 0)
        log_warn("shutdown: %d connection(s) still open, exiting anyway",
                 active_conns);
    pthread_mutex_unlock(&conn_lock);
}

int server_run(unsigned short port)
{
    unsigned long next_id = 1;

    start_time = time(NULL);

    int lfd = create_listen_socket(port);
    if (lfd < 0)
        return -1;

    log_info("listening on http://0.0.0.0:%u/ (max %d concurrent connections)",
             port, MAX_CONNECTIONS);

    while (!stop_requested) {
        struct sockaddr_in peer;
        socklen_t peer_len = sizeof peer;

        /* Blocks until a client has completed the three-way handshake.
         * Returns a NEW socket for that client; lfd keeps listening. */
        int cfd = accept(lfd, (struct sockaddr *)&peer, &peer_len);
        if (cfd < 0) {
            if (errno == EINTR || errno == ECONNABORTED)
                continue;       /* signal (re-check stop flag) or client gave up */
            log_error("accept: %s", strerror(errno));
            continue;
        }

        conn_t *c = calloc(1, sizeof *c);
        if (!c) {
            log_error("out of memory, dropping connection");
            close(cfd);
            continue;
        }
        c->fd   = cfd;
        c->port = ntohs(peer.sin_port);
        c->id   = next_id++;
        inet_ntop(AF_INET, &peer.sin_addr, c->ip, sizeof c->ip);

        if (!acquire_slot()) {
            log_warn("[conn %lu] %s:%u rejected: %d connections active",
                     c->id, c->ip, c->port, MAX_CONNECTIONS);
            /* Short send timeout so a stuck client cannot block the accept
             * loop; no graceful drain here for the same reason. */
            set_socket_timeouts(cfd);
            http_send_error(cfd, 503, "Retry-After: 5\r\n",
                            "Too many students are connected right now. "
                            "Please try again in a few seconds.");
            close(cfd);
            free(c);
            continue;
        }

        set_socket_timeouts(cfd);

        int rc = spawn_worker(c);
        if (rc != 0) {
            log_error("pthread_create: %s", strerror(rc));
            close(cfd);
            free(c);
            release_slot();
        }
    }

    log_info("shutdown requested, closing listening socket");
    close(lfd);
    wait_for_workers();
    log_info("server stopped");
    return 0;
}
