/*
 * config.h - Compile-time tunables for the Student Query Management System.
 *
 * Every limit that protects the server from misbehaving or malicious
 * clients lives here so it is easy to find and explain in the report.
 */
#ifndef CONFIG_H
#define CONFIG_H

/* TCP port the server listens on when none is given on the command line. */
#define DEFAULT_PORT          8080

/*
 * Length of the kernel's queue of fully-established connections that
 * have completed the TCP three-way handshake but have not yet been
 * returned by accept(). Linux silently caps this at net.core.somaxconn.
 */
#define LISTEN_BACKLOG        128

/*
 * Maximum number of connections served at the same time. With the
 * thread-per-connection model this is also the maximum number of worker
 * threads, so it bounds memory use (each thread has its own stack).
 */
#define MAX_CONNECTIONS       64

/*
 * recv()/send() timeout in seconds (SO_RCVTIMEO / SO_SNDTIMEO). A client
 * that opens a connection and then goes silent is disconnected after
 * this long instead of holding a worker thread forever.
 */
#define SOCKET_TIMEOUT_SEC    15

/* On Ctrl+C, how long to wait for in-flight connections to finish. */
#define SHUTDOWN_GRACE_SEC    5

/*
 * HTTP request size limits. The request line plus all headers must fit
 * in MAX_HEADER_SIZE bytes (else 431); the body may be at most
 * MAX_BODY_SIZE bytes (else 413). Together they bound the memory one
 * client can make us allocate.
 */
#define MAX_HEADER_SIZE       8192
#define MAX_BODY_SIZE         16384

/* Largest file served from STATIC_DIR. */
#define MAX_STATIC_FILE       (1024 * 1024)

/* Directories, relative to where the server is started. */
#define TEMPLATE_DIR          "templates"
#define STATIC_DIR            "static"

/* Value of the "Server:" response header. */
#define SERVER_NAME           "SQMS/1.0"

/* Log file (connections, requests, state changes). */
#define LOG_FILE_PATH         "server.log"

/* SQLite database file (users, sessions, queries). */
#define DB_PATH               "sqms.db"

/*
 * Sessions. HTTP is stateless: every request arrives on a fresh TCP
 * connection with no memory of earlier ones. After login the server
 * hands the browser a random token in a cookie; the browser sends it
 * back with every request and the server looks it up in the sessions
 * table. The session (and the cookie) expire after SESSION_TTL_SEC.
 * Set it to e.g. 60 to demonstrate expiry quickly.
 */
#define SESSION_COOKIE_NAME   "sqms_session"
#define SESSION_TTL_SEC       3600
#define SESSION_TOKEN_BYTES   32        /* 256 random bits -> 64 hex chars */

/*
 * Passwords are stored as PBKDF2-HMAC-SHA256 with a random 16-byte salt.
 * The salt makes identical passwords hash differently; the iteration
 * count makes each guess slow for an attacker who steals the database.
 */
#define PBKDF2_ITERATIONS     100000
#define PASSWORD_SALT_BYTES   16

/* Account field limits. */
#define USERNAME_MIN          3
#define USERNAME_MAX          32
#define PASSWORD_MIN          6
#define PASSWORD_MAX          128

#endif /* CONFIG_H */
