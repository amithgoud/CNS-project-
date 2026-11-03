/*
 * auth.h - Accounts, login and cookie-based sessions.
 *
 * Flow:
 *   1. POST /login with username + password.
 *   2. Server verifies the salted hash, creates a random 256-bit token,
 *      stores (token, user, expiry) in the sessions table and replies
 *      302 + "Set-Cookie: sqms_session=<token>; HttpOnly; SameSite=Lax".
 *   3. The browser sends "Cookie: sqms_session=<token>" on every later
 *      request (each on a new TCP connection); the server looks the token
 *      up to find out who is asking.
 */
#ifndef AUTH_H
#define AUTH_H

#include "db.h"
#include "http.h"

#include <stddef.h>

typedef struct {
    user_t    user;
    char      token[2 * SESSION_TOKEN_BYTES + 1];
    long long expires_at;           /* Unix time */
} session_t;

/* Error codes from auth_register(). */
#define AUTH_ERR_INTERNAL   (-1)
#define AUTH_ERR_USERNAME   (-2)    /* invalid format */
#define AUTH_ERR_PASSWORD   (-3)    /* too short / too long */
#define AUTH_ERR_EXISTS     (-4)    /* username taken */

/* Results of auth_session_from_request(). */
#define AUTH_SESSION_NONE    0      /* no cookie sent */
#define AUTH_SESSION_OK      1      /* *out filled in */
#define AUTH_SESSION_INVALID 2      /* cookie sent but expired / unknown */

/* 3-32 chars of a-z, 0-9, '_'. */
int auth_valid_username(const char *username);

/* Create an account. Returns the new user id (>0) or an AUTH_ERR_*. */
long long auth_register(const char *username, const char *password, role_t role);

/* Check credentials and start a session. 1 ok, 0 bad credentials, -1 error. */
int auth_login(const char *username, const char *password, session_t *out);

/* Find the session belonging to this request's cookie. */
int auth_session_from_request(const http_request_t *req, session_t *out);

/* End a session (server side). */
void auth_logout(const char *token);

/* "Set-Cookie: ...\r\n" header that hands the session token to the browser. */
void auth_cookie_header(const session_t *s, char *out, size_t outsz);

/* "Set-Cookie: ...\r\n" header that deletes the cookie (Max-Age=0). */
const char *auth_clear_cookie_header(void);

#endif /* AUTH_H */
