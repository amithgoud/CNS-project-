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
 *   GET  /student     student dashboard: my questions   (students only)
 *   GET/POST /ask       ask a new question                (students only)
 *   GET  /query?id=N  one question and its progress     (its owner, or teachers)
 *   GET  /teacher     review queue + reviewed questions (teachers only)
 *   GET/POST /review    approve or edit one draft         (teachers only)
 *   GET  /api/queries   my questions as plain text      (students; CLI client)
 *   GET  /api/query?id=N one question as plain text     (owner or teachers)
 *
 * Access control is decided here, in one place, from the route table:
 * a handler marked ACCESS_TEACHER is never even called for a student.
 */
#include "router.h"
#include "auth.h"
#include "config.h"
#include "llm.h"
#include "log.h"
#include "strbuf.h"
#include "template.h"
#include "tls.h"
#include "util.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define HTML "text/html; charset=utf-8"
#define TEXT "text/plain; charset=utf-8"

#define ALLOW_GET  1
#define ALLOW_POST 2

#define ACCESS_PUBLIC  0    /* anyone */
#define ACCESS_STUDENT 1    /* logged in with role student */
#define ACCESS_TEACHER 2    /* logged in with role teacher */
#define ACCESS_USER    3    /* logged in, any role */

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
static int send_page(const conn_t *c, int status, const session_t *s, const char *title,
                     const char *page_tpl, const tpl_var_t *vars, size_t nvars,
                     const char *head_extra)
{
    strbuf_t content, nav, page;
    sb_init(&content);
    sb_init(&nav);
    sb_init(&page);

    int rc;
    if (template_render(&content, page_tpl, vars, nvars) < 0) {
        rc = http_send_error(c, 500, NULL, "Could not load page template.");
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
        rc = http_send_error(c, 500, NULL, "Could not load page layout.");
        goto out;
    }

    rc = http_send_response(c, status, HTML, NULL, page.data, page.len);
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

static void meta_refresh(char *out, size_t outsz, int seconds)
{
    snprintf(out, outsz, "<meta http-equiv=\"refresh\" content=\"%d\">", seconds);
}

/* Parse a positive id from form data / query string; 0 if missing or bad. */
static long long form_id(const char *data, const char *key)
{
    char buf[24];
    if (!http_form_get(data, key, buf, sizeof buf))
        return 0;
    char *end;
    long long id = strtoll(buf, &end, 10);
    return (end != buf && *end == '\0' && id > 0) ? id : 0;
}

/* Remove '\r' (browsers send textarea line breaks as CRLF) and trim
 * surrounding whitespace, in place. Returns the start of the text. */
static char *normalize_text(char *s)
{
    char *w = s;
    for (char *r = s; *r; r++)
        if (*r != '\r')
            *w++ = *r;
    *w = '\0';

    while (isspace((unsigned char)*s))
        s++;
    char *end = s + strlen(s);
    while (end > s && isspace((unsigned char)end[-1]))
        *--end = '\0';
    return s;
}

/* ------------------------------------------------------------------ */
/* Formatting helpers for questions                                   */
/* ------------------------------------------------------------------ */

static void fmt_time(long long t, char *out, size_t outsz)
{
    time_t tt = (time_t)t;
    struct tm tm;
    localtime_r(&tt, &tm);
    strftime(out, outsz, "%d %b %Y, %H:%M", &tm);
}

/* What each state means in plain words. Students are not told an AI
 * draft exists until a teacher has checked it. */
static const char *state_label(qstate_t st, role_t viewer)
{
    int teacher = (viewer == ROLE_TEACHER);
    switch (st) {
    case Q_PENDING_LLM:      return "Waiting for AI draft";
    case Q_LLM_ANSWERED:     return teacher ? "Needs review" : "Being reviewed by a teacher";
    case Q_TEACHER_APPROVED: return teacher ? "Approved, not yet read" : "Answer ready";
    case Q_TEACHER_EDITED:   return teacher ? "Edited, not yet read" : "Answer ready";
    case Q_DELIVERED:        return teacher ? "Delivered" : "Answered";
    default:                 return "Unknown";
    }
}

static void state_badge(strbuf_t *sb, qstate_t st, role_t viewer)
{
    sb_printf(sb, "<span class=\"state state-%d\" title=\"%s\">%s</span>",
              (int)st, qstate_name(st), state_label(st, viewer));
}

/* First `max` bytes of text, cut on a UTF-8 character boundary
 * (continuation bytes look like 10xxxxxx), with newlines and tabs turned
 * into spaces. HTML-escaped when html != 0. */
static void excerpt(strbuf_t *sb, const char *text, size_t max, int html)
{
    size_t n = strlen(text);
    int cut = n > max;
    if (cut) {
        n = max;
        while (n > 0 && ((unsigned char)text[n] & 0xC0) == 0x80)
            n--;
    }

    char *copy = malloc(n + 1);
    if (!copy)
        return;
    for (size_t i = 0; i < n; i++)
        copy[i] = (text[i] == '\n' || text[i] == '\r' || text[i] == '\t') ? ' ' : text[i];
    copy[n] = '\0';

    if (html)
        sb_append_html(sb, copy);
    else
        sb_puts(sb, copy);
    free(copy);
    if (cut)
        sb_puts(sb, html ? "&hellip;" : "...");
}

/* Progress bar for a question: 4 steps, everything up to the current
 * state marked done. */
static void build_stepper(strbuf_t *sb, qstate_t st)
{
    static const char *const labels[4] = {
        "Submitted", "AI draft written", "Teacher reviewed", "Delivered",
    };
    static const char *const codes[4] = {
        "PENDING_LLM", "LLM_ANSWERED", "TEACHER_APPROVED / TEACHER_EDITED", "DELIVERED",
    };
    int cur = st == Q_PENDING_LLM  ? 0
            : st == Q_LLM_ANSWERED ? 1
            : st == Q_DELIVERED    ? 3
            : 2;

    sb_puts(sb, "<ol class=\"stepper\">");
    for (int i = 0; i < 4; i++)
        sb_printf(sb, "<li class=\"%s\"><span class=\"dot\">%d</span>"
                      "<span class=\"label\">%s</span><code>%s</code></li>",
                  i < cur ? "done" : i == cur ? "done current" : "todo",
                  i + 1, labels[i], codes[i]);
    sb_puts(sb, "</ol>");
}

/*
 * Load question `id` for the viewer s. Students may only see their own.
 * Returns 1 with *q filled. Otherwise sends the error response, stores
 * the result of sending it in *sent, and returns 0.
 */
static int load_query_for(const conn_t *c, const session_t *s, long long id,
                          query_t *q, int *sent)
{
    if (id <= 0) {
        *sent = http_send_error(c, 400, NULL, "Missing or invalid question number.");
        return 0;
    }

    int rc = db_query_get(id, q);
    if (rc < 0) {
        *sent = http_send_error(c, 500, NULL, "Could not load the question.");
        return 0;
    }
    if (rc == 0) {
        *sent = http_send_error(c, 404, NULL, "There is no question with that number.");
        return 0;
    }

    if (s->user.role != ROLE_TEACHER && q->student_id != s->user.id) {
        log_warn("[conn %lu] %s:%u access denied: %s tried to open query #%lld of %s",
                 c->id, c->ip, c->port, s->user.username, id, q->student_name);
        query_free(q);
        *sent = http_send_error(c, 403, NULL, "This question belongs to another student.");
        return 0;
    }
    return 1;
}

/*
 * DELIVERED means: the teacher-checked answer has been written to the
 * student's own connection. We record it only after the response was sent
 * successfully (send() returned), so a student whose connection dropped
 * mid-response still sees the question as "Answer ready".
 */
static void record_delivery(const conn_t *c, long long id, qstate_t was, const char *student)
{
    if (db_query_mark_delivered(id) == 1)
        log_info("[conn %lu] %s:%u query #%lld: %s -> DELIVERED (answer sent to %s)",
                 c->id, c->ip, c->port, id, qstate_name(was), student);
}

/* ------------------------------------------------------------------ */
/* Demo pages (step 2)                                                */
/* ------------------------------------------------------------------ */

static int h_home(const conn_t *c, const http_request_t *req, const session_t *s)
{
    (void)req;
    return send_page(c, 200, s, "Home", "home.html", NULL, 0, NULL);
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
    int rc = send_page(c, 200, s, "Echo", "echo.html", vars, 1, NULL);
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

    char port[8], id[24], nhdr[16], transport[128], tlsdesc[96];
    snprintf(port, sizeof port, "%u", c->port);
    snprintf(id, sizeof id, "%lu", c->id);
    snprintf(nhdr, sizeof nhdr, "%d", req->header_count);
    tls_describe(c, tlsdesc, sizeof tlsdesc);
    if (tlsdesc[0])
        snprintf(transport, sizeof transport, "HTTPS - encrypted with %s", tlsdesc);
    else
        snprintf(transport, sizeof transport, "HTTP - plain TCP, not encrypted");

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
        { "transport",    transport },
    };
    int rc = send_page(c, 200, s, "Inspect request", "inspect.html",
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
    char uptime[64], active[16], max[16], accepted[24], rejected[24], limited[24];
    char port[8], id[24], now[32], rate[64];
    snprintf(uptime, sizeof uptime, "%ldh %02ldm %02lds",
             up / 3600, (up / 60) % 60, up % 60);
    snprintf(active, sizeof active, "%d", st.active);
    snprintf(max, sizeof max, "%d", st.max);
    snprintf(accepted, sizeof accepted, "%lu", st.accepted);
    snprintf(rejected, sizeof rejected, "%lu", st.rejected);
    snprintf(limited, sizeof limited, "%lu", st.rate_limited);
    snprintf(port, sizeof port, "%u", c->port);
    snprintf(id, sizeof id, "%lu", c->id);
    if (st.rate_limit > 0)
        snprintf(rate, sizeof rate, "%.0f requests/s per IP (burst %.0f)",
                 st.rate_limit, 2 * st.rate_limit);
    else
        snprintf(rate, sizeof rate, "off");

    time_t t = time(NULL);
    struct tm tm;
    localtime_r(&t, &tm);
    strftime(now, sizeof now, "%H:%M:%S", &tm);

    long long counts[Q_STATE_COUNT];
    char qc[Q_STATE_COUNT][24];
    db_query_count_by_state(counts);    /* all zero on error */
    for (int i = 0; i < Q_STATE_COUNT; i++)
        snprintf(qc[i], sizeof qc[i], "%lld", counts[i]);

    llm_status_t ls;
    llm_get_status(&ls);
    char llm_queued[16], llm_busy[32];
    snprintf(llm_queued, sizeof llm_queued, "%d", ls.queued);
    snprintf(llm_busy, sizeof llm_busy, "%d / %d", ls.busy, ls.workers);

    tpl_var_t vars[] = {
        { "uptime",       uptime },
        { "active",       active },
        { "max",          max },
        { "accepted",     accepted },
        { "rejected",     rejected },
        { "rate_limited", limited },
        { "rate_limit",   rate },
        { "transport",    st.https ? "HTTPS (TLS)" : "HTTP (plain TCP)" },
        { "client_ip",    c->ip },
        { "client_port",  port },
        { "conn_id",      id },
        { "now",          now },
        { "q_pending",    qc[Q_PENDING_LLM] },
        { "q_llm",        qc[Q_LLM_ANSWERED] },
        { "q_approved",   qc[Q_TEACHER_APPROVED] },
        { "q_edited",     qc[Q_TEACHER_EDITED] },
        { "q_delivered",  qc[Q_DELIVERED] },
        { "llm_provider", ls.provider },
        { "llm_model",    ls.model },
        { "llm_endpoint", ls.endpoint },
        { "llm_queued",   llm_queued },
        { "llm_busy",     llm_busy },
    };
    char head[96];
    meta_refresh(head, sizeof head, 5);
    return send_page(c, 200, s, "Server status", "status.html",
                     vars, sizeof vars / sizeof vars[0], head);
}

/* ------------------------------------------------------------------ */
/* Login, registration, logout (step 3)                               */
/* ------------------------------------------------------------------ */

static int render_login(const conn_t *c, int status, const char *error,
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
    int rc = send_page(c, status, NULL, "Log in", "login.html", vars, 2, NULL);
    sb_free(&alert);
    return rc;
}

static int h_login(const conn_t *c, const http_request_t *req, const session_t *s)
{
    if (s)      /* already logged in */
        return http_send_redirect(c, dashboard_path(s->user.role), NULL);

    if (strcmp(req->method, "GET") == 0) {
        char flag[8];
        const char *notice = NULL;
        if (http_form_get(req->query, "expired", flag, sizeof flag))
            notice = "Your session has expired. Please log in again.";
        else if (http_form_get(req->query, "loggedout", flag, sizeof flag))
            notice = "You have been logged out.";
        return render_login(c, 200, NULL, notice, NULL);
    }

    char username[64], password[PASSWORD_MAX + 2];
    http_form_get(req->body, "username", username, sizeof username);
    http_form_get(req->body, "password", password, sizeof password);

    if (!*username || !*password)
        return render_login(c, 400, "Please enter your username and password.",
                            NULL, username);

    session_t ns;
    int rc = auth_login(username, password, &ns);
    if (rc < 0)
        return http_send_error(c, 500, NULL, "Login failed because of a server error.");
    if (rc == 0) {
        log_warn("[conn %lu] %s:%u failed login attempt", c->id, c->ip, c->port);
        /* 401: the credentials were wrong. Same message for unknown user
         * and wrong password, so it does not reveal which usernames exist. */
        return render_login(c, 401, "Invalid username or password.", NULL, username);
    }

    log_info("[conn %lu] %s:%u login: %s (%s)", c->id, c->ip, c->port,
             ns.user.username, role_name(ns.user.role));

    /* Post/Redirect/Get: answer the POST with a redirect so refreshing the
     * dashboard does not re-submit the password. The cookie rides on it. */
    char cookie[256];
    auth_cookie_header(&ns, cookie, sizeof cookie);
    return http_send_redirect(c, dashboard_path(ns.user.role), cookie);
}

static int render_register(const conn_t *c, int status, const char *error,
                           const char *username)
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
    int rc = send_page(c, status, NULL, "Register", "register.html", vars, 4, NULL);
    sb_free(&alert);
    return rc;
}

/* Self-registration creates STUDENT accounts only. Teacher accounts are
 * made on the server with: ./sqms_server --add-user teacher <name> */
static int h_register(const conn_t *c, const http_request_t *req, const session_t *s)
{
    if (s)
        return http_send_redirect(c, dashboard_path(s->user.role), NULL);
    if (strcmp(req->method, "GET") == 0)
        return render_register(c, 200, NULL, NULL);

    char username[64], password[PASSWORD_MAX + 2], confirm[PASSWORD_MAX + 2];
    http_form_get(req->body, "username", username, sizeof username);
    http_form_get(req->body, "password", password, sizeof password);
    http_form_get(req->body, "confirm", confirm, sizeof confirm);

    if (strcmp(password, confirm) != 0)
        return render_register(c, 400, "The two passwords do not match.", username);

    long long id = auth_register(username, password, ROLE_STUDENT);
    switch (id) {
    case AUTH_ERR_USERNAME:
        return render_register(c, 400, "That username is not allowed. "
                               "Use 3-32 lowercase letters, digits or underscores.", username);
    case AUTH_ERR_PASSWORD:
        return render_register(c, 400, "Password must be 6-128 characters long.", username);
    case AUTH_ERR_EXISTS:
        return render_register(c, 400, "That username is already taken.", username);
    case AUTH_ERR_INTERNAL:
        return http_send_error(c, 500, NULL, "Could not create the account.");
    default:
        break;
    }
    log_info("[conn %lu] %s:%u registered student '%s' (id %lld)",
             c->id, c->ip, c->port, username, id);

    /* Log the new student straight in. */
    session_t ns;
    if (auth_login(username, password, &ns) != 1)
        return http_send_redirect(c, "/login", NULL);

    char cookie[256];
    auth_cookie_header(&ns, cookie, sizeof cookie);
    return http_send_redirect(c, "/student", cookie);
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
    return http_send_redirect(c, "/login?loggedout=1", auth_clear_cookie_header());
}

/* ------------------------------------------------------------------ */
/* Student pages (step 4)                                             */
/* ------------------------------------------------------------------ */

/* Session details shown on the dashboards. */
typedef struct {
    char expires[64];
    char remaining[32];
    char token_preview[24];
    char ttl[16];
} session_view_t;

static void session_view(const session_t *s, session_view_t *v)
{
    time_t exp = (time_t)s->expires_at;
    struct tm tm;
    localtime_r(&exp, &tm);
    strftime(v->expires, sizeof v->expires, "%H:%M:%S", &tm);

    long left = (long)(s->expires_at - (long long)time(NULL));
    if (left < 0)
        left = 0;
    snprintf(v->remaining, sizeof v->remaining, "%ld min %02ld s", left / 60, left % 60);

    /* Only show the start of the token; it is a secret. */
    snprintf(v->token_preview, sizeof v->token_preview, "%.12s...", s->token);
    snprintf(v->ttl, sizeof v->ttl, "%d", SESSION_TTL_SEC);
}

/* Student dashboard: list of my questions. While any is still open the
 * page polls the server with meta-refresh. */
static int h_student(const conn_t *c, const http_request_t *req, const session_t *s)
{
    (void)req;
    query_t *list;
    int n = db_query_list_by_student(s->user.id, STUDENT_LIST_LIMIT, &list);
    if (n < 0)
        return http_send_error(c, 500, NULL, "Could not load your questions.");

    strbuf_t rows;
    sb_init(&rows);
    int open = 0;
    for (int i = 0; i < n; i++) {
        const query_t *q = &list[i];
        char when[64];
        if (!qstate_has_answer(q->state))
            open++;
        fmt_time(q->updated_at, when, sizeof when);
        sb_printf(&rows, "<tr><td class=\"num\">#%lld</td><td><a href=\"/query?id=%lld\">",
                  q->id, q->id);
        excerpt(&rows, q->question ? q->question : "", 90, 1);
        sb_puts(&rows, "</a></td><td>");
        state_badge(&rows, q->state, ROLE_STUDENT);
        sb_printf(&rows, "</td><td class=\"muted\">%s</td></tr>\n", when);
    }
    if (n == 0)
        sb_puts(&rows, "<tr><td colspan=\"4\" class=\"empty\">"
                       "You have not asked any questions yet.</td></tr>");
    query_list_free(list, n);

    char head[96] = "", note[160] = "";
    if (open > 0) {
        meta_refresh(head, sizeof head, POLL_LIST_SEC);
        snprintf(note, sizeof note, "%d question%s still open. This page asks the server "
                 "for updates every %d seconds.", open, open == 1 ? "" : "s", POLL_LIST_SEC);
    } else if (n > 0) {
        snprintf(note, sizeof note, "All your questions have been answered.");
    }

    session_view_t sv;
    session_view(s, &sv);

    tpl_var_t vars[] = {
        { "username",      s->user.username },
        { "role",          role_name(s->user.role) },
        { "rows",          sb_cstr(&rows) },
        { "refresh_note",  note },
        { "expires",       sv.expires },
        { "remaining",     sv.remaining },
        { "token_preview", sv.token_preview },
        { "cookie_name",   SESSION_COOKIE_NAME },
        { "ttl",           sv.ttl },
    };
    int rc = send_page(c, 200, s, "My questions", "student.html",
                       vars, sizeof vars / sizeof vars[0], open > 0 ? head : NULL);
    sb_free(&rows);
    return rc;
}

static int render_ask(const conn_t *c, int status, const session_t *s,
                      const char *error, const char *question)
{
    strbuf_t alert;
    sb_init(&alert);
    alert_html(&alert, "error", error);

    char rules[96], maxlen[16];
    snprintf(rules, sizeof rules, "Between %d and %d characters.",
             MIN_QUESTION_LEN, MAX_QUESTION_LEN);
    snprintf(maxlen, sizeof maxlen, "%d", MAX_QUESTION_LEN);

    tpl_var_t vars[] = {
        { "alert",    sb_cstr(&alert) },
        { "question", question ? question : "" },
        { "rules",    rules },
        { "maxlen",   maxlen },
    };
    int rc = send_page(c, status, s, "Ask a question", "ask.html", vars, 4, NULL);
    sb_free(&alert);
    return rc;
}

/* Ask a question. The form is on its own page (not the dashboard) so the
 * dashboard's auto-refresh cannot wipe out a half-typed question. */
static int h_ask(const conn_t *c, const http_request_t *req, const session_t *s)
{
    if (strcmp(req->method, "GET") == 0)
        return render_ask(c, 200, s, NULL, NULL);

    /* +2: one byte to detect "too long", one for the NUL. */
    char buf[MAX_QUESTION_LEN + 2];
    http_form_get(req->body, "question", buf, sizeof buf);
    char *q = normalize_text(buf);

    size_t len = strlen(q);
    if (len < MIN_QUESTION_LEN)
        return render_ask(c, 400, s, "Please write a little more: your question is too short.", q);
    if (len > MAX_QUESTION_LEN)
        return render_ask(c, 400, s, "Your question is too long. Please shorten it.", q);

    long long id = db_query_create(s->user.id, q);
    if (id < 0)
        return http_send_error(c, 500, NULL, "Could not save your question.");

    log_info("[conn %lu] %s:%u query #%lld created by %s -> %s",
             c->id, c->ip, c->port, id, s->user.username, qstate_name(Q_PENDING_LLM));

    /* Hand the question to the background LLM workers and answer the
     * browser immediately; the draft is written while the student waits
     * on the (auto-refreshing) question page. */
    llm_enqueue(id);

    /* Post/Redirect/Get: go to the question's own page. */
    char loc[64];
    snprintf(loc, sizeof loc, "/query?id=%lld&new=1", id);
    return http_send_redirect(c, loc, NULL);
}

/* One question with its progress. Students may only open their own
 * questions; teachers may open any. */
static int h_query(const conn_t *c, const http_request_t *req, const session_t *s)
{
    query_t q;
    int sent;
    if (!load_query_for(c, s, form_id(req->query, "id"), &q, &sent))
        return sent;

    int is_teacher = (s->user.role == ROLE_TEACHER);

    /* The owner opening an approved answer receives it with this response,
     * so show it as delivered; the DB is updated once sending succeeded. */
    qstate_t was = q.state;
    int delivering = !is_teacher &&
                     (q.state == Q_TEACHER_APPROVED || q.state == Q_TEACHER_EDITED);
    if (delivering)
        q.state = Q_DELIVERED;

    strbuf_t steps, answer, alert, badge;
    sb_init(&steps);
    sb_init(&answer);
    sb_init(&alert);
    sb_init(&badge);

    build_stepper(&steps, q.state);
    state_badge(&badge, q.state, s->user.role);

    char flag[8];
    if (http_form_get(req->query, "new", flag, sizeof flag))
        alert_html(&alert, "info", "Your question has been sent and saved. "
                   "You can leave this page; the answer will appear under My questions.");

    if (qstate_has_answer(q.state) && q.final_answer) {
        sb_puts(&answer, "<section class=\"card answer\"><h2>Answer</h2><div class=\"text\">");
        sb_append_html(&answer, q.final_answer);
        sb_puts(&answer, "</div><p class=\"muted\">Checked by teacher ");
        sb_append_html(&answer, q.teacher_name[0] ? q.teacher_name : "(unknown)");
        if (was == Q_TEACHER_EDITED || (was == Q_DELIVERED && q.llm_answer &&
                                        strcmp(q.llm_answer, q.final_answer) != 0))
            sb_puts(&answer, ", who edited the AI draft");
        sb_puts(&answer, ".</p></section>");
    } else if (!is_teacher) {
        sb_printf(&answer, "<section class=\"card pending\"><h2>Answer</h2>"
                           "<p class=\"muted\">No answer yet. An AI model writes a first draft, "
                           "then a teacher checks it before it reaches you. This page asks the "
                           "server for updates every %d seconds.</p></section>", POLL_QUERY_SEC);
    }

    if (is_teacher) {
        sb_puts(&answer, "<section class=\"card draft\"><h2>AI draft</h2>");
        if (q.llm_answer) {
            sb_puts(&answer, "<div class=\"text\">");
            sb_append_html(&answer, q.llm_answer);
            sb_puts(&answer, "</div>");
        } else {
            sb_puts(&answer, "<p class=\"muted\">Not generated yet.</p>");
        }
        if (q.state == Q_LLM_ANSWERED)
            sb_printf(&answer, "<p><a class=\"button\" href=\"/review?id=%lld\">"
                               "Review this answer</a></p>", q.id);
        sb_puts(&answer, "</section>");
    }

    char title[32], created[64], updated[64], head[96];
    snprintf(title, sizeof title, "Question #%lld", q.id);
    fmt_time(q.created_at, created, sizeof created);
    fmt_time(q.updated_at, updated, sizeof updated);
    meta_refresh(head, sizeof head, POLL_QUERY_SEC);

    tpl_var_t vars[] = {
        { "title",     title },
        { "alert",     sb_cstr(&alert) },
        { "student",   q.student_name },
        { "created",   created },
        { "updated",   updated },
        { "badge",     sb_cstr(&badge) },
        { "question",  q.question ? q.question : "" },
        { "steps",     sb_cstr(&steps) },
        { "answer",    sb_cstr(&answer) },
        { "back_link", is_teacher ? "/teacher" : "/student" },
    };
    /* Keep polling until there is a teacher-checked answer. */
    int rc = send_page(c, 200, s, title, "query.html", vars, sizeof vars / sizeof vars[0],
                       qstate_has_answer(q.state) ? NULL : head);

    if (delivering && rc == 200)
        record_delivery(c, q.id, was, q.student_name);

    sb_free(&steps);
    sb_free(&answer);
    sb_free(&alert);
    sb_free(&badge);
    query_free(&q);
    return rc;
}

/* ------------------------------------------------------------------ */
/* Teacher pages (step 6)                                             */
/* ------------------------------------------------------------------ */

/* Teacher dashboard: the review queue and recently reviewed questions.
 * It has no form, so it can safely poll with meta-refresh. */
static int h_teacher(const conn_t *c, const http_request_t *req, const session_t *s)
{
    query_t *queue = NULL, *done = NULL;
    int nq = db_query_list_review_queue(TEACHER_LIST_LIMIT, &queue);
    int nd = db_query_list_reviewed(TEACHER_LIST_LIMIT, &done);
    if (nq < 0 || nd < 0) {
        if (nq > 0) query_list_free(queue, nq);
        if (nd > 0) query_list_free(done, nd);
        return http_send_error(c, 500, NULL, "Could not load the review queue.");
    }

    strbuf_t qrows, drows, alert;
    sb_init(&qrows);
    sb_init(&drows);
    sb_init(&alert);

    for (int i = 0; i < nq; i++) {
        const query_t *q = &queue[i];
        char when[64];
        fmt_time(q->created_at, when, sizeof when);
        sb_printf(&qrows, "<tr><td class=\"num\">#%lld</td><td>", q->id);
        sb_append_html(&qrows, q->student_name);
        sb_printf(&qrows, "</td><td><a href=\"/review?id=%lld\">", q->id);
        excerpt(&qrows, q->question ? q->question : "", 90, 1);
        sb_printf(&qrows, "</a></td><td class=\"muted\">%s</td>"
                          "<td><a class=\"button small\" href=\"/review?id=%lld\">Review</a></td></tr>\n",
                  when, q->id);
    }
    if (nq == 0)
        sb_puts(&qrows, "<tr><td colspan=\"5\" class=\"empty\">"
                        "Nothing to review right now.</td></tr>");

    for (int i = 0; i < nd; i++) {
        const query_t *q = &done[i];
        char when[64];
        fmt_time(q->updated_at, when, sizeof when);
        sb_printf(&drows, "<tr><td class=\"num\">#%lld</td><td>", q->id);
        sb_append_html(&drows, q->student_name);
        sb_printf(&drows, "</td><td><a href=\"/query?id=%lld\">", q->id);
        excerpt(&drows, q->question ? q->question : "", 70, 1);
        sb_puts(&drows, "</a></td><td>");
        state_badge(&drows, q->state, ROLE_TEACHER);
        sb_puts(&drows, "</td><td>");
        sb_append_html(&drows, q->teacher_name);
        sb_printf(&drows, "</td><td class=\"muted\">%s</td></tr>\n", when);
    }
    if (nd == 0)
        sb_puts(&drows, "<tr><td colspan=\"6\" class=\"empty\">"
                        "No reviewed questions yet.</td></tr>");

    query_list_free(queue, nq);
    query_list_free(done, nd);

    long long done_id = form_id(req->query, "done");
    if (done_id > 0) {
        char msg[160];
        snprintf(msg, sizeof msg, "Answer to question #%lld sent. It becomes DELIVERED "
                 "when the student opens it.", done_id);
        alert_html(&alert, "info", msg);
    }

    long long counts[Q_STATE_COUNT];
    db_query_count_by_state(counts);
    char c_pending[24], c_review[24], c_waiting[24], c_delivered[24];
    snprintf(c_pending, sizeof c_pending, "%lld", counts[Q_PENDING_LLM]);
    snprintf(c_review, sizeof c_review, "%lld", counts[Q_LLM_ANSWERED]);
    snprintf(c_waiting, sizeof c_waiting, "%lld",
             counts[Q_TEACHER_APPROVED] + counts[Q_TEACHER_EDITED]);
    snprintf(c_delivered, sizeof c_delivered, "%lld", counts[Q_DELIVERED]);

    session_view_t sv;
    session_view(s, &sv);

    tpl_var_t vars[] = {
        { "username",      s->user.username },
        { "role",          role_name(s->user.role) },
        { "alert",         sb_cstr(&alert) },
        { "queue_rows",    sb_cstr(&qrows) },
        { "done_rows",     sb_cstr(&drows) },
        { "c_pending",     c_pending },
        { "c_review",      c_review },
        { "c_waiting",     c_waiting },
        { "c_delivered",   c_delivered },
        { "poll",          "10" },
        { "expires",       sv.expires },
        { "remaining",     sv.remaining },
        { "token_preview", sv.token_preview },
        { "cookie_name",   SESSION_COOKIE_NAME },
        { "ttl",           sv.ttl },
    };
    char head[96];
    meta_refresh(head, sizeof head, POLL_LIST_SEC);
    int rc = send_page(c, 200, s, "Review queue", "teacher.html",
                       vars, sizeof vars / sizeof vars[0], head);
    sb_free(&qrows);
    sb_free(&drows);
    sb_free(&alert);
    return rc;
}

static int render_review(const conn_t *c, int status, const session_t *s,
                         const query_t *q, const char *answer, const char *error)
{
    strbuf_t alert;
    sb_init(&alert);
    alert_html(&alert, "error", error);

    char title[32], created[64], idbuf[24], maxlen[16];
    snprintf(title, sizeof title, "Review question #%lld", q->id);
    snprintf(idbuf, sizeof idbuf, "%lld", q->id);
    snprintf(maxlen, sizeof maxlen, "%d", MAX_ANSWER_LEN);
    fmt_time(q->created_at, created, sizeof created);

    tpl_var_t vars[] = {
        { "title",    title },
        { "id",       idbuf },
        { "alert",    sb_cstr(&alert) },
        { "student",  q->student_name },
        { "created",  created },
        { "question", q->question ? q->question : "" },
        { "answer",   answer ? answer : "" },
        { "maxlen",   maxlen },
    };
    int rc = send_page(c, status, s, title, "review.html",
                       vars, sizeof vars / sizeof vars[0], NULL);
    sb_free(&alert);
    return rc;
}

/*
 * Review one draft. GET shows the question and the AI draft in an editable
 * box. POST stores the final answer: unchanged text -> TEACHER_APPROVED,
 * changed text -> TEACHER_EDITED.
 */
static int h_review(const conn_t *c, const http_request_t *req, const session_t *s)
{
    int is_post = (strcmp(req->method, "POST") == 0);
    long long id = form_id(is_post ? req->body : req->query, "id");

    query_t q;
    int rc;
    if (!load_query_for(c, s, id, &q, &rc))
        return rc;

    if (q.state != Q_LLM_ANSWERED) {
        /* Not reviewable (no draft yet, or already reviewed): show it. */
        char loc[64];
        snprintf(loc, sizeof loc, "/query?id=%lld", q.id);
        if (is_post) {
            rc = http_send_error(c, 409, NULL, q.state == Q_PENDING_LLM
                                 ? "This question has no AI draft yet."
                                 : "This question has already been reviewed by another teacher.");
        } else {
            rc = http_send_redirect(c, loc, NULL);
        }
        query_free(&q);
        return rc;
    }

    if (!is_post) {
        rc = render_review(c, 200, s, &q, q.llm_answer, NULL);
        query_free(&q);
        return rc;
    }

    char buf[MAX_ANSWER_LEN + 2];
    http_form_get(req->body, "answer", buf, sizeof buf);
    char *answer = normalize_text(buf);
    size_t len = strlen(answer);

    if (len == 0) {
        rc = render_review(c, 400, s, &q, answer, "The answer cannot be empty.");
    } else if (len > MAX_ANSWER_LEN) {
        rc = render_review(c, 400, s, &q, answer, "The answer is too long.");
    } else {
        /* Compare with the draft (normalised the same way) to decide
         * between APPROVED and EDITED. */
        char *draft = q.llm_answer ? strdup(q.llm_answer) : NULL;
        int edited = !draft || strcmp(normalize_text(draft), answer) != 0;
        free(draft);

        int r = db_query_review(q.id, s->user.id, answer, edited);
        if (r == 1) {
            log_info("[conn %lu] %s:%u query #%lld: LLM_ANSWERED -> %s by %s",
                     c->id, c->ip, c->port, q.id,
                     qstate_name(edited ? Q_TEACHER_EDITED : Q_TEACHER_APPROVED),
                     s->user.username);
            char loc[64];
            snprintf(loc, sizeof loc, "/teacher?done=%lld", q.id);
            rc = http_send_redirect(c, loc, NULL);
        } else if (r == 0) {
            /* Another teacher's UPDATE matched first. */
            rc = http_send_error(c, 409, NULL,
                                 "This question was reviewed by another teacher a moment ago.");
        } else {
            rc = http_send_error(c, 500, NULL, "Could not save the answer.");
        }
    }
    query_free(&q);
    return rc;
}

/* ------------------------------------------------------------------ */
/* Plain-text API for the command-line client (step 7)                */
/* ------------------------------------------------------------------ */

/* One line per question: "#id<TAB>STATE<TAB>question". */
static int h_api_queries(const conn_t *c, const http_request_t *req, const session_t *s)
{
    (void)req;
    query_t *list;
    int n = db_query_list_by_student(s->user.id, STUDENT_LIST_LIMIT, &list);
    if (n < 0)
        return http_send_error(c, 500, NULL, "Could not load your questions.");

    strbuf_t out;
    sb_init(&out);
    for (int i = 0; i < n; i++) {
        sb_printf(&out, "#%lld\t%s\t", list[i].id, qstate_name(list[i].state));
        excerpt(&out, list[i].question ? list[i].question : "", 70, 0);
        sb_puts(&out, "\n");
    }
    if (n == 0)
        sb_puts(&out, "(no questions yet)\n");
    query_list_free(list, n);

    int rc = http_send_response(c, 200, TEXT, NULL, sb_cstr(&out), out.len);
    sb_free(&out);
    return rc;
}

/* One question as "key: value" lines, then the texts. Same rules as the
 * web page, including recording delivery for the owner. */
static int h_api_query(const conn_t *c, const http_request_t *req, const session_t *s)
{
    query_t q;
    int sent;
    if (!load_query_for(c, s, form_id(req->query, "id"), &q, &sent))
        return sent;

    int is_teacher = (s->user.role == ROLE_TEACHER);
    qstate_t was = q.state;
    int delivering = !is_teacher &&
                     (q.state == Q_TEACHER_APPROVED || q.state == Q_TEACHER_EDITED);
    if (delivering)
        q.state = Q_DELIVERED;

    char created[64], updated[64];
    fmt_time(q.created_at, created, sizeof created);
    fmt_time(q.updated_at, updated, sizeof updated);

    strbuf_t out;
    sb_init(&out);
    sb_printf(&out, "id: %lld\nstate: %s\nstudent: %s\nasked: %s\nupdated: %s\n",
              q.id, qstate_name(q.state), q.student_name, created, updated);
    if (q.teacher_name[0])
        sb_printf(&out, "teacher: %s\n", q.teacher_name);
    sb_printf(&out, "\nquestion:\n%s\n", q.question ? q.question : "");
    if (is_teacher && q.llm_answer)
        sb_printf(&out, "\nai draft:\n%s\n", q.llm_answer);
    if (qstate_has_answer(q.state) && q.final_answer)
        sb_printf(&out, "\nanswer:\n%s\n", q.final_answer);

    int rc = http_send_response(c, 200, TEXT, NULL, sb_cstr(&out), out.len);
    if (delivering && rc == 200)
        record_delivery(c, q.id, was, q.student_name);
    sb_free(&out);
    query_free(&q);
    return rc;
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
    if (strcmp(dot, ".txt") == 0)   return TEXT;
    if (strcmp(dot, ".png") == 0)   return "image/png";
    if (strcmp(dot, ".jpg") == 0)   return "image/jpeg";
    if (strcmp(dot, ".svg") == 0)   return "image/svg+xml";
    if (strcmp(dot, ".ico") == 0)   return "image/x-icon";
    return "application/octet-stream";
}

static int serve_static(const conn_t *c, const char *name)
{
    if (!safe_filename(name))
        return http_send_error(c, 404, NULL, "No such file.");

    char path[256];
    snprintf(path, sizeof path, "%s/%s", STATIC_DIR, name);

    size_t len;
    char *data = read_whole_file(path, MAX_STATIC_FILE, &len);
    if (!data)
        return http_send_error(c, 404, NULL, "No such file.");

    int rc = http_send_response(c, 200, mime_type(name), NULL, data, len);
    free(data);
    return rc;
}

/* ------------------------------------------------------------------ */
/* Dispatch                                                           */
/* ------------------------------------------------------------------ */

static const route_t routes[] = {
    { "/",            h_home,        ALLOW_GET,              ACCESS_PUBLIC  },
    { "/echo",        h_echo,        ALLOW_GET | ALLOW_POST, ACCESS_PUBLIC  },
    { "/inspect",     h_inspect,     ALLOW_GET,              ACCESS_PUBLIC  },
    { "/status",      h_status,      ALLOW_GET,              ACCESS_PUBLIC  },
    { "/login",       h_login,       ALLOW_GET | ALLOW_POST, ACCESS_PUBLIC  },
    { "/register",    h_register,    ALLOW_GET | ALLOW_POST, ACCESS_PUBLIC  },
    { "/logout",      h_logout,      ALLOW_POST,             ACCESS_PUBLIC  },
    { "/student",     h_student,     ALLOW_GET,              ACCESS_STUDENT },
    { "/ask",         h_ask,         ALLOW_GET | ALLOW_POST, ACCESS_STUDENT },
    { "/query",       h_query,       ALLOW_GET,              ACCESS_USER    },
    { "/teacher",     h_teacher,     ALLOW_GET,              ACCESS_TEACHER },
    { "/review",      h_review,      ALLOW_GET | ALLOW_POST, ACCESS_TEACHER },
    { "/api/queries", h_api_queries, ALLOW_GET,              ACCESS_STUDENT },
    { "/api/query",   h_api_query,   ALLOW_GET,              ACCESS_USER    },
};

/* 405 must list the allowed methods in an Allow header (RFC 9110). */
static int method_not_allowed(const conn_t *c, int methods)
{
    const char *allow = (methods == (ALLOW_GET | ALLOW_POST)) ? "Allow: GET, POST\r\n"
                      : (methods == ALLOW_POST)               ? "Allow: POST\r\n"
                                                              : "Allow: GET\r\n";
    return http_send_error(c, 405, allow,
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
            return http_send_redirect(c, "/login?expired=1",
                                      auth_clear_cookie_header());
        return http_send_redirect(c, "/login", NULL);
    }

    if (r->access == ACCESS_USER)
        return -2;          /* any role; the handler checks ownership */

    role_t need = (r->access == ACCESS_TEACHER) ? ROLE_TEACHER : ROLE_STUDENT;
    if (s->user.role != need) {
        /* Logged in, but the wrong role: 403 Forbidden. */
        log_warn("[conn %lu] %s:%u access denied: %s (%s) requested %s",
                 c->id, c->ip, c->port, s->user.username,
                 role_name(s->user.role), r->path);
        return http_send_error(c, 403, NULL,
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
            return method_not_allowed(c, ALLOW_GET);
        return serve_static(c, req->path + 8);
    }

    for (size_t i = 0; i < sizeof routes / sizeof routes[0]; i++) {
        const route_t *r = &routes[i];
        if (strcmp(req->path, r->path) != 0)
            continue;

        if (!(r->methods & m))
            return method_not_allowed(c, r->methods);

        /* Identify the user from the session cookie (if any). */
        session_t sess;
        int state = auth_session_from_request(req, &sess);
        const session_t *s = (state == AUTH_SESSION_OK) ? &sess : NULL;

        int denied = check_access(c, r, s, state);
        if (denied != -2)
            return denied;

        return r->fn(c, req, s);
    }

    return http_send_error(c, 404, NULL, "There is no page at this address.");
}
