/*
 * main.c - Entry point for the Student Query Management System server.
 *
 * Usage:
 *   ./sqms_server [port]                          run the server
 *   ./sqms_server --add-user <student|teacher> <username>
 *                                                 create an account (the
 *                                                 password is read from stdin)
 */
#include "auth.h"
#include "config.h"
#include "db.h"
#include "llm.h"
#include "log.h"
#include "server.h"

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>

static void on_shutdown_signal(int sig)
{
    (void)sig;
    server_request_stop();   /* only sets a sig_atomic_t flag */
}

static int install_signal_handlers(void)
{
    struct sigaction sa;

    /* Writing to a socket whose peer has closed raises SIGPIPE, whose
     * default action terminates the process. One student closing their
     * browser mid-response must not kill the server, so ignore it; send()
     * then just fails with EPIPE and the worker cleans up. */
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = SIG_IGN;
    sigemptyset(&sa.sa_mask);
    if (sigaction(SIGPIPE, &sa, NULL) < 0)
        return -1;

    /* Ctrl+C / kill: request a graceful shutdown. sa_flags deliberately
     * omits SA_RESTART so the blocking accept() returns EINTR and the
     * accept loop notices the stop flag. */
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_shutdown_signal;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    if (sigaction(SIGINT, &sa, NULL) < 0 || sigaction(SIGTERM, &sa, NULL) < 0)
        return -1;

    return 0;
}

/* Read one line from stdin. When stdin is a terminal, echo is switched
 * off so the password is not shown; when piped (scripts), it just reads. */
static int read_password(const char *prompt, char *buf, size_t size)
{
    int tty = isatty(STDIN_FILENO);
    struct termios old, quiet;

    if (tty) {
        fputs(prompt, stderr);
        tcgetattr(STDIN_FILENO, &old);
        quiet = old;
        quiet.c_lflag &= ~(tcflag_t)ECHO;
        tcsetattr(STDIN_FILENO, TCSAFLUSH, &quiet);
    }
    char *r = fgets(buf, (int)size, stdin);
    if (tty) {
        tcsetattr(STDIN_FILENO, TCSAFLUSH, &old);
        fputc('\n', stderr);
    }
    if (!r)
        return -1;
    buf[strcspn(buf, "\r\n")] = '\0';
    return 0;
}

static int add_user(const char *role_str, const char *username)
{
    role_t role;
    char pw[PASSWORD_MAX + 2], again[PASSWORD_MAX + 2];

    if (role_parse(role_str, &role) < 0) {
        fprintf(stderr, "role must be 'student' or 'teacher'\n");
        return EXIT_FAILURE;
    }
    if (read_password("Password: ", pw, sizeof pw) < 0)
        return EXIT_FAILURE;
    if (isatty(STDIN_FILENO)) {
        if (read_password("Repeat password: ", again, sizeof again) < 0)
            return EXIT_FAILURE;
        if (strcmp(pw, again) != 0) {
            fprintf(stderr, "Passwords do not match.\n");
            return EXIT_FAILURE;
        }
    }

    long long id = auth_register(username, pw, role);
    switch (id) {
    case AUTH_ERR_USERNAME:
        fprintf(stderr, "Invalid username: use %d-%d lowercase letters, digits or '_'.\n",
                USERNAME_MIN, USERNAME_MAX);
        return EXIT_FAILURE;
    case AUTH_ERR_PASSWORD:
        fprintf(stderr, "Password must be %d-%d characters.\n", PASSWORD_MIN, PASSWORD_MAX);
        return EXIT_FAILURE;
    case AUTH_ERR_EXISTS:
        fprintf(stderr, "User '%s' already exists.\n", username);
        return EXIT_FAILURE;
    case AUTH_ERR_INTERNAL:
        fprintf(stderr, "Could not create the user (see server.log).\n");
        return EXIT_FAILURE;
    default:
        log_info("created %s account '%s' (id %lld)", role_name(role), username, id);
        return EXIT_SUCCESS;
    }
}

static void usage(const char *prog)
{
    fprintf(stderr,
            "usage: %s [port]\n"
            "       %s --add-user <student|teacher> <username>\n",
            prog, prog);
}

int main(int argc, char **argv)
{
    unsigned short port = DEFAULT_PORT;
    int adding_user = (argc > 1 && strcmp(argv[1], "--add-user") == 0);

    if (adding_user) {
        if (argc != 4) {
            usage(argv[0]);
            return EXIT_FAILURE;
        }
    } else if (argc > 2) {
        usage(argv[0]);
        return EXIT_FAILURE;
    } else if (argc == 2) {
        char *end;
        long p = strtol(argv[1], &end, 10);
        if (*argv[1] == '\0' || *end != '\0' || p < 1 || p > 65535) {
            usage(argv[0]);
            return EXIT_FAILURE;
        }
        port = (unsigned short)p;
    }

    if (log_init(LOG_FILE_PATH) < 0) {
        perror("log_init: " LOG_FILE_PATH);
        return EXIT_FAILURE;
    }
    if (db_open(DB_PATH) < 0) {
        log_close();
        return EXIT_FAILURE;
    }

    int rc;
    if (adding_user) {
        rc = add_user(argv[2], argv[3]);
    } else if (install_signal_handlers() < 0) {
        perror("sigaction");
        rc = EXIT_FAILURE;
    } else if (llm_init() < 0) {
        /* Before server_run: libcurl's global init must happen while
         * this is still the only thread. */
        rc = EXIT_FAILURE;
    } else {
        rc = server_run(port) == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
        llm_shutdown();         /* stop workers before closing the DB */
    }

    db_close();
    log_close();
    return rc;
}
