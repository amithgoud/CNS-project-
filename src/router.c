/*
 * router.c - Page handlers and the routing table.
 *
 *   GET  /            home page
 *   GET  /echo        form; the message comes back via the query string
 *   POST /echo        same form, but the message travels in the body
 *   GET  /inspect     shows the exact request the browser sent
 *   GET  /status      live server counters, auto-refreshed by meta-refresh
 *   GET  /static/...  CSS and other static files
 *   GET/POST /login     log in (sets the session cookie)
 *   GET/POST /register  create a student account
 *   POST /logout      end the session (clears the cookie)
 *   GET  /student     student dashboard   (students only)
 *   GET  /teacher     teacher dashboard   (teachers only)
 *
 * Access control is decided here, in one place, from the route table:
 * a handler marked ACCESS_TEACHER is never even called for a student.
 */
#include "router.h"
#include "auth.h"
#include "config.h"
#include "log.h"
#include "strbuf.h"
#include "template.h"
#include "util.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define HTML "text/html; charset=utf-8"

#define ALLOW_GET  1
#define ALLOW_POST 2

#define ACCESS_PUBLIC  0    /* anyone */
#define ACCESS_STUDENT 1    /* logged in with role student */
#define ACCESS_TEACHER 2    /* logged in with role teacher */

/* s is the logged-in user's session, or NULL for a visitor. */
typedef int (*handler_fn)(const conn_t *c, const http_request_t *req,
                          const session_t *s);

typedef struct {
    const char *path;
    handler_fn  fn;
    int         methods;    /* ALLOW_GET | ALLOW_POST */
    int         access;     /* ACCESS_* */
} route_t;

static const char *dashboard_path(role_t role)
{
    return role == ROLE_TEACHER ? "/teacher" : "/student";
}

/* Right-hand side of the top bar: who is logged in, or login links. */
static void build_account_nav(strbuf_t *sb, const session_t *s)
{
    if (!s) {
        sb_puts(sb, "<a href=\"/login\">Log in</a>"
                    "<a class=\"button small\" href=\"/register\">Register</a>");
        return;
    }
    sb_puts(sb, "<span class=\"who\">");
    sb_append_html(sb, s->user.username);
    sb_printf(sb, " <span class=\"role-tag\">%s</span></span>"
                  "<a href=\"%s\">Dashboard</a>"
                  "<form method=\"post\" action=\"/logout\" class=\"inline\">"
                  "<button type=\"submit\" class=\"link-button\">Log out</button></form>",
              role_name(s->user.role), dashboard_path(s->user.role));
}

/*
 * Render templates/<page_tpl> with vars, wrap it in templates/layout.html
 * and send it. head_extra is raw HTML for <head> (e.g. meta-refresh).
 */
static int send_page(int fd, int status, const session_t *s, const char *title,
                     const char *page_tpl, const tpl_var_t *vars, size_t nvars,
                     const char *head_extra)
{
    strbuf_t content, nav, page;
    sb_init(&content);
    sb_init(&nav);
    sb_init(&page);

    int rc;
    if (template_render(&content, page_tpl, vars, nvars) < 0) {
        rc = http_send_error(fd, 500, NULL, "Could not load page template.");
        goto out;
    }
    build_account_nav(&nav, s);

    tpl_var_t layout_vars[] = {
        { "title",       title },
        { "content",     sb_cstr(&content) },
        { "head_extra",  head_extra ? head_extra : "" },
        { "account_nav", sb_cstr(&nav) },
    };
    if (template_render(&page, "layout.html", layout_vars, 4) < 0) {
        rc = http_send_error(fd, 500, NULL, "Could not load page layout.");
        goto out;
    }

    rc = http_send_response(fd, status, HTML, NULL, page.data, page.len);
out:
    sb_free(&content);
    sb_free(&nav);
    sb_free(&page);
    return rc;
}

/* Append an escaped message box (or nothing if msg is NULL). */
static void alert_html(strbuf_t *sb, const char *cls, const char *msg)
{
    if (!msg)
        return;
    sb_printf(sb, "<div class=\"alert %s\">", cls);
    sb_append_html(sb, msg);
    sb_puts(sb, "</div>");
}

/* ------------------------------------------------------------------ */
/* Demo pages (step 2)                                                */
/* ------------------------------------------------------------------ */

static int h_home(const conn_t *c, const http_request_t *req, const session_t *s)
{
    (void)req;
    return send_page(c->fd, 200, s, "Home", "home.html", NULL, 0, NULL);
}

/*
 * Echo: the web version of step 1's echo server. The same form is shown
 * twice, once with method="get" and once with method="post", so you can
 * see the two ways form data travels over HTTP.
 */
static int h_echo(const conn_t *c, const http_request_t *req, const session_t *s)
{
    const char *data = NULL;
    const char *via  = NULL;
    char message[2048];

    if (strcmp(req->method, "POST") == 0) {
        data = req->body;
        via  = "POST - in the request body, after the headers";
    } else if (req->query) {
        data = req->query;
        via  = "GET - in the URL, after the '?'";
    }

    strbuf_t result;
    sb_init(&result);

    if (data && http_form_get(data, "message", message, sizeof message)) {
        sb_puts(&result, "<section class=\"card result\"><h2>The server echoed back</h2>"
                         "<p class=\"echo\">");
        sb_append_html(&result, message);
        sb_puts(&result, "</p><table class=\"kv\"><tr><th>Sent with</th><td>");
        sb_append_html(&result, via);
        sb_puts(&result, "</td></tr><tr><th>As it travelled (URL-encoded)</th><td><code>");
        sb_append_html(&result, data);
        sb_printf(&result, "</code></td></tr><tr><th>Size</th><td>%zu bytes</td></tr>"
                           "</table></section>", strlen(data));
    }

    tpl_var_t vars[] = { { "result", sb_cstr(&result) } };
    int rc = send_page(c->fd, 200, s, "Echo", "echo.html", vars, 1, NULL);
    sb_free(&result);
    return rc;
}

/* Show the request exactly as the server parsed it. */
static int h_inspect(const conn_t *c, const http_request_t *req, const session_t *s)
{
    strbuf_t rows;
    sb_init(&rows);
    for (int i = 0; i < req->header_count; i++) {
        sb_puts(&rows, "<tr><th>");
        sb_append_html(&rows, req->headers[i].name);
        sb_puts(&rows, "</th><td><code>");
        sb_append_html(&rows, req->headers[i].value);
        sb_puts(&rows, "</code></td></tr>\n");
    }

    char port[8], id[24], nhdr[16];
    snprintf(port, sizeof port, "%u", c->port);
    snprintf(id, sizeof id, "%lu", c->id);
    snprintf(nhdr, sizeof nhdr, "%d", req->header_count);

    tpl_var_t vars[] = {
        { "method",       req->method },
        { "path",         req->path },
        { "query",        req->query ? req->query : "(none)" },
        { "version",      req->version },
        { "client_ip",    c->ip },
        { "client_port",  port },
        { "conn_id",      id },
        { "header_count", nhdr },
        { "header_rows",  sb_cstr(&rows) },
    };
    int rc = send_page(c->fd, 200, s, "Inspect request", "inspect.html",
                       vars, sizeof vars / sizeof vars[0], NULL);
    sb_free(&rows);
    return rc;
}

/* Live counters. The page reloads itself every 5 seconds via
 * <meta http-equiv="refresh">: polling with no JavaScript. */
static int h_status(const conn_t *c, const http_request_t *req, const session_t *s)
{
    (void)req;
    server_stats_t st;
    server_get_stats(&st);

    long up = (long)difftime(time(NULL), st.started);
    char uptime[64], active[16], max[16], accepted[24], rejected[24];
    char port[8], id[24], now[32];
    snprintf(uptime, sizeof uptime, "%ldh %02ldm %02lds",
             up / 3600, (up / 60) % 60, up % 60);
    snprintf(active, sizeof active, "%d", st.active);
    snprintf(max, sizeof max, "%d", st.max);
    snprintf(accepted, sizeof accepted, "%lu", st.accepted);
    snprintf(rejected, sizeof rejected, "%lu", st.rejected);
    snprintf(port, sizeof port, "%u", c->port);
    snprintf(id, sizeof id, "%lu", c->id);

    time_t t = time(NULL);
    struct tm tm;
    localtime_r(&t, &tm);
    strftime(now, sizeof now, "%H:%M:%S", &tm);

    tpl_var_t vars[] = {
        { "uptime",      uptime },
        { "active",      active },
        { "max",         max },
        { "accepted",    accepted },
        { "rejected",    rejected },
        { "client_ip",   c->ip },
        { "client_port", port },
        { "conn_id",     id },
        { "now",         now },
    };
    return send_page(c->fd, 200, s, "Server status", "status.html",
                     vars, sizeof vars / sizeof vars[0],
                     "<meta http-equiv=\"refresh\" content=\"5\">");
}

/* ------------------------------------------------------------------ */
/* Login, registration, logout (step 3)                               */
/* ------------------------------------------------------------------ */

static int render_login(int fd, int status, const char *error,
                        const char *notice, const char *username)
{
    strbuf_t alert;
    sb_init(&alert);
    alert_html(&alert, "error", error);
    alert_html(&alert, "info", notice);

    tpl_var_t vars[] = {
        { "alert",    sb_cstr(&alert) },
        { "username", username ? username : "" },
    };
    int rc = send_page(fd, status, NULL, "Log in", "login.html", vars, 2, NULL);
    sb_free(&alert);
    return rc;
}

static int h_login(const conn_t *c, const http_request_t *req, const session_t *s)
{
    if (s)      /* already logged in */
        return http_send_redirect(c->fd, dashboard_path(s->user.role), NULL);

    if (strcmp(req->method, "GET") == 0) {
        char flag[8];
        const char *notice = NULL;
        if (http_form_get(req->query, "expired", flag, sizeof flag))
            notice = "Your session has expired. Please log in again.";
        else if (http_form_get(req->query, "loggedout", flag, sizeof flag))
            notice = "You have been logged out.";
        return render_login(c->fd, 200, NULL, notice, NULL);
    }

    char username[64], password[PASSWORD_MAX + 2];
    http_form_get(req->body, "username", username, sizeof username);
    http_form_get(req->body, "password", password, sizeof password);

    if (!*username || !*password)
        return render_login(c->fd, 400, "Please enter your username and password.",
                            NULL, username);

    session_t ns;
    int rc = auth_login(username, password, &ns);
    if (rc < 0)
        return http_send_error(c->fd, 500, NULL, "Login failed because of a server error.");
    if (rc == 0) {
        log_warn("[conn %lu] %s:%u failed login attempt", c->id, c->ip, c->port);
        /* 401: the credentials were wrong. Same message for unknown user
         * and wrong password, so it does not reveal which usernames exist. */
        return render_login(c->fd, 401, "Invalid username or password.", NULL, username);
    }

    log_info("[conn %lu] %s:%u login: %s (%s)", c->id, c->ip, c->port,
             ns.user.username, role_name(ns.user.role));

    /* Post/Redirect/Get: answer the POST with a redirect so refreshing the
     * dashboard does not re-submit the password. The cookie rides on it. */
    char cookie[256];
    auth_cookie_header(&ns, cookie, sizeof cookie);
    return http_send_redirect(c->fd, dashboard_path(ns.user.role), cookie);
}

static int render_register(int fd, int status, const char *error, const char *username)
{
    strbuf_t alert;
    sb_init(&alert);
    alert_html(&alert, "error", error);

    char limits[160];
    snprintf(limits, sizeof limits,
             "%d-%d characters: lowercase letters, digits or underscore",
             USERNAME_MIN, USERNAME_MAX);
    char pw_limits[64];
    snprintf(pw_limits, sizeof pw_limits, "At least %d characters", PASSWORD_MIN);

    tpl_var_t vars[] = {
        { "alert",           sb_cstr(&alert) },
        { "username",        username ? username : "" },
        { "username_rules",  limits },
        { "password_rules",  pw_limits },
    };
    int rc = send_page(fd, status, NULL, "Register", "register.html", vars, 4, NULL);
    sb_free(&alert);
    return rc;
}

/* Self-registration creates STUDENT accounts only. Teacher accounts are
 * made on the server with: ./sqms_server --add-user teacher <name> */
static int h_register(const conn_t *c, const http_request_t *req, const session_t *s)
{
    if (s)
        return http_send_redirect(c->fd, dashboard_path(s->user.role), NULL);
    if (strcmp(req->method, "GET") == 0)
        return render_register(c->fd, 200, NULL, NULL);

    char username[64], password[PASSWORD_MAX + 2], confirm[PASSWORD_MAX + 2];
    http_form_get(req->body, "username", username, sizeof username);
    http_form_get(req->body, "password", password, sizeof password);
    http_form_get(req->body, "confirm", confirm, sizeof confirm);

    if (strcmp(password, confirm) != 0)
        return render_register(c->fd, 400, "The two passwords do not match.", username);

    long long id = auth_register(username, password, ROLE_STUDENT);
    switch (id) {
    case AUTH_ERR_USERNAME:
        return render_register(c->fd, 400, "That username is not allowed. "
                               "Use 3-32 lowercase letters, digits or underscores.", username);
    case AUTH_ERR_PASSWORD:
        return render_register(c->fd, 400, "Password must be 6-128 characters long.", username);
    case AUTH_ERR_EXISTS:
        return render_register(c->fd, 400, "That username is already taken.", username);
    case AUTH_ERR_INTERNAL:
        return http_send_error(c->fd, 500, NULL, "Could not create the account.");
    default:
        break;
    }
    log_info("[conn %lu] %s:%u registered student '%s' (id %lld)",
             c->id, c->ip, c->port, username, id);

    /* Log the new student straight in. */
    session_t ns;
    if (auth_login(username, password, &ns) != 1)
        return http_send_redirect(c->fd, "/login", NULL);

    char cookie[256];
    auth_cookie_header(&ns, cookie, sizeof cookie);
    return http_send_redirect(c->fd, "/student", cookie);
}

/* POST only: a GET link to /logout could be triggered by any <img> tag on
 * another site. */
static int h_logout(const conn_t *c, const http_request_t *req, const session_t *s)
{
    (void)req;
    if (s) {
        auth_logout(s->token);
        log_info("[conn %lu] %s:%u logout: %s", c->id, c->ip, c->port,
                 s->user.username);
    }
    return http_send_redirect(c->fd, "/login?loggedout=1", auth_clear_cookie_header());
}

/* ------------------------------------------------------------------ */
/* Dashboards                                                         */
/* ------------------------------------------------------------------ */

static int render_dashboard(const conn_t *c, const session_t *s,
                            const char *title, const char *tpl)
{
    char expires[64], remaining[32], token_preview[24];
    time_t exp = (time_t)s->expires_at;
    struct tm tm;
    localtime_r(&exp, &tm);
    strftime(expires, sizeof expires, "%H:%M:%S", &tm);

    long left = (long)(s->expires_at - (long long)time(NULL));
    if (left < 0)
        left = 0;
    snprintf(remaining, sizeof remaining, "%ld min %02ld s", left / 60, left % 60);

    /* Only show the start of the token; it is a secret. */
    snprintf(token_preview, sizeof token_preview, "%.12s...", s->token);

    char ttl[16];
    snprintf(ttl, sizeof ttl, "%d", SESSION_TTL_SEC);

    tpl_var_t vars[] = {
        { "username",      s->user.username },
        { "role",          role_name(s->user.role) },
        { "expires",       expires },
        { "remaining",     remaining },
        { "token_preview", token_preview },
        { "cookie_name",   SESSION_COOKIE_NAME },
        { "ttl",           ttl },
    };
    return send_page(c->fd, 200, s, title, tpl, vars, sizeof vars / sizeof vars[0], NULL);
}

static int h_student(const conn_t *c, const http_request_t *req, const session_t *s)
{
    (void)req;
    return render_dashboard(c, s, "Student dashboard", "student.html");
}

static int h_teacher(const conn_t *c, const http_request_t *req, const session_t *s)
{
    (void)req;
    return render_dashboard(c, s, "Teacher dashboard", "teacher.html");
}

/* ------------------------------------------------------------------ */
/* Static files                                                       */
/* ------------------------------------------------------------------ */

/* Only plain file names like "style.css": no '/', no leading '.', so a
 * request such as /static/../src/main.c cannot escape STATIC_DIR. */
static int safe_filename(const char *s)
{
    if (!*s || *s == '.')
        return 0;
    for (; *s; s++)
        if (!isalnum((unsigned char)*s) && *s != '.' && *s != '-' && *s != '_')
            return 0;
    return 1;
}

static const char *mime_type(const char *name)
{
    const char *dot = strrchr(name, '.');
    if (!dot)                       return "application/octet-stream";
    if (strcmp(dot, ".css") == 0)   return "text/css; charset=utf-8";
    if (strcmp(dot, ".html") == 0)  return HTML;
    if (strcmp(dot, ".txt") == 0)   return "text/plain; charset=utf-8";
    if (strcmp(dot, ".png") == 0)   return "image/png";
    if (strcmp(dot, ".jpg") == 0)   return "image/jpeg";
    if (strcmp(dot, ".svg") == 0)   return "image/svg+xml";
    if (strcmp(dot, ".ico") == 0)   return "image/x-icon";
    return "application/octet-stream";
}

static int serve_static(int fd, const char *name)
{
    if (!safe_filename(name))
        return http_send_error(fd, 404, NULL, "No such file.");

    char path[256];
    snprintf(path, sizeof path, "%s/%s", STATIC_DIR, name);

    size_t len;
    char *data = read_whole_file(path, MAX_STATIC_FILE, &len);
    if (!data)
        return http_send_error(fd, 404, NULL, "No such file.");

    int rc = http_send_response(fd, 200, mime_type(name), NULL, data, len);
    free(data);
    return rc;
}

/* ------------------------------------------------------------------ */
/* Dispatch                                                           */
/* ------------------------------------------------------------------ */

static const route_t routes[] = {
    { "/",         h_home,     ALLOW_GET,              ACCESS_PUBLIC  },
    { "/echo",     h_echo,     ALLOW_GET | ALLOW_POST, ACCESS_PUBLIC  },
    { "/inspect",  h_inspect,  ALLOW_GET,              ACCESS_PUBLIC  },
    { "/status",   h_status,   ALLOW_GET,              ACCESS_PUBLIC  },
    { "/login",    h_login,    ALLOW_GET | ALLOW_POST, ACCESS_PUBLIC  },
    { "/register", h_register, ALLOW_GET | ALLOW_POST, ACCESS_PUBLIC  },
    { "/logout",   h_logout,   ALLOW_POST,             ACCESS_PUBLIC  },
    { "/student",  h_student,  ALLOW_GET,              ACCESS_STUDENT },
    { "/teacher",  h_teacher,  ALLOW_GET,              ACCESS_TEACHER },
};

/* 405 must list the allowed methods in an Allow header (RFC 9110). */
static int method_not_allowed(int fd, int methods)
{
    const char *allow = (methods == (ALLOW_GET | ALLOW_POST)) ? "Allow: GET, POST\r\n"
                      : (methods == ALLOW_POST)               ? "Allow: POST\r\n"
                                                              : "Allow: GET\r\n";
    return http_send_error(fd, 405, allow,
                           "This page does not accept that request method.");
}

/*
 * Role-based access control. Returns -2 if the request may proceed,
 * otherwise sends the refusal and returns its status.
 */
static int check_access(const conn_t *c, const route_t *r,
                        const session_t *s, int session_state)
{
    if (r->access == ACCESS_PUBLIC)
        return -2;

    if (!s) {
        /* Not logged in: send the browser to the login page. If it sent a
         * stale cookie, say the session expired and delete the cookie. */
        if (session_state == AUTH_SESSION_INVALID)
            return http_send_redirect(c->fd, "/login?expired=1",
                                      auth_clear_cookie_header());
        return http_send_redirect(c->fd, "/login", NULL);
    }

    role_t need = (r->access == ACCESS_TEACHER) ? ROLE_TEACHER : ROLE_STUDENT;
    if (s->user.role != need) {
        /* Logged in, but the wrong role: 403 Forbidden. */
        log_warn("[conn %lu] %s:%u access denied: %s (%s) requested %s",
                 c->id, c->ip, c->port, s->user.username,
                 role_name(s->user.role), r->path);
        return http_send_error(c->fd, 403, NULL,
                               need == ROLE_TEACHER
                               ? "This page is only for teachers."
                               : "This page is only for students.");
    }
    return -2;
}

int router_handle(const conn_t *c, const http_request_t *req)
{
    int m = strcmp(req->method, "GET") == 0  ? ALLOW_GET
          : strcmp(req->method, "POST") == 0 ? ALLOW_POST
          : 0;

    if (strncmp(req->path, "/static/", 8) == 0) {
        if (m != ALLOW_GET)
            return method_not_allowed(c->fd, ALLOW_GET);
        return serve_static(c->fd, req->path + 8);
    }

    for (size_t i = 0; i < sizeof routes / sizeof routes[0]; i++) {
        const route_t *r = &routes[i];
        if (strcmp(req->path, r->path) != 0)
            continue;

        if (!(r->methods & m))
            return method_not_allowed(c->fd, r->methods);

        /* Identify the user from the session cookie (if any). */
        session_t sess;
        int state = auth_session_from_request(req, &sess);
        const session_t *s = (state == AUTH_SESSION_OK) ? &sess : NULL;

        int denied = check_access(c, r, s, state);
        if (denied != -2)
            return denied;

        return r->fn(c, req, s);
    }

    return http_send_error(c->fd, 404, NULL, "There is no page at this address.");
}
