/*
 * auth.c - Accounts, login and cookie-based sessions (see auth.h).
 */
#include "auth.h"
#include "config.h"
#include "crypto.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#define STR_(x) #x
#define STR(x)  STR_(x)

/*
 * Hash checked when the username does not exist. Without it a login for
 * an unknown user would return instantly while a wrong password takes
 * ~0.1 s of PBKDF2, letting anyone find valid usernames by timing.
 */
static const char DUMMY_HASH[] =
    "pbkdf2_sha256$" STR(PBKDF2_ITERATIONS)
    "$00000000000000000000000000000000"
    "$0000000000000000000000000000000000000000000000000000000000000000";

int auth_valid_username(const char *u)
{
    size_t n = strlen(u);
    if (n < USERNAME_MIN || n > USERNAME_MAX)
        return 0;
    for (; *u; u++)
        if (!(islower((unsigned char)*u) || isdigit((unsigned char)*u) || *u == '_'))
            return 0;
    return 1;
}

long long auth_register(const char *username, const char *password, role_t role)
{
    size_t plen = strlen(password);
    char hash[PASSWORD_HASH_MAX];

    if (!auth_valid_username(username))
        return AUTH_ERR_USERNAME;
    if (plen < PASSWORD_MIN || plen > PASSWORD_MAX)
        return AUTH_ERR_PASSWORD;
    if (password_hash(password, hash, sizeof hash) < 0)
        return AUTH_ERR_INTERNAL;

    long long id = db_create_user(username, hash, role);
    if (id == DB_ERR_EXISTS)
        return AUTH_ERR_EXISTS;
    return id > 0 ? id : AUTH_ERR_INTERNAL;
}

static int create_session(const user_t *u, session_t *out)
{
    unsigned char raw[SESSION_TOKEN_BYTES];
    time_t now = time(NULL);

    /* The token is the only thing that proves who the browser is, so it
     * must be unguessable: 256 bits from the kernel's CSPRNG. */
    if (random_bytes(raw, sizeof raw) < 0)
        return -1;
    hex_encode(raw, sizeof raw, out->token);

    db_session_purge(now);              /* housekeeping: drop expired rows */

    out->user = *u;
    out->expires_at = (long long)now + SESSION_TTL_SEC;
    if (db_session_create(out->token, u->id, out->expires_at) < 0)
        return -1;
    return 1;
}

int auth_login(const char *username, const char *password, session_t *out)
{
    user_t u;
    char stored[PASSWORD_HASH_MAX];

    int found = 0;
    if (auth_valid_username(username)) {
        found = db_find_user(username, &u, stored, sizeof stored);
        if (found < 0)
            return -1;
    }

    if (!found) {
        password_verify(password, DUMMY_HASH);  /* same cost as a real check */
        return 0;
    }
    if (!password_verify(password, stored))
        return 0;

    return create_session(&u, out);
}

/* A valid token is exactly 64 lowercase hex characters. */
static int token_well_formed(const char *t)
{
    size_t n = 0;
    for (; t[n]; n++)
        if (!isdigit((unsigned char)t[n]) && !(t[n] >= 'a' && t[n] <= 'f'))
            return 0;
    return n == 2 * SESSION_TOKEN_BYTES;
}

int auth_session_from_request(const http_request_t *req, session_t *out)
{
    char token[2 * SESSION_TOKEN_BYTES + 2];

    if (!http_get_cookie(req, SESSION_COOKIE_NAME, token, sizeof token))
        return AUTH_SESSION_NONE;
    if (!token_well_formed(token))
        return AUTH_SESSION_INVALID;

    int rc = db_session_lookup(token, (long long)time(NULL),
                               &out->user, &out->expires_at);
    if (rc != 1)
        return AUTH_SESSION_INVALID;

    memcpy(out->token, token, sizeof out->token);
    return AUTH_SESSION_OK;
}

void auth_logout(const char *token)
{
    db_session_delete(token);
}

void auth_cookie_header(const session_t *s, char *out, size_t outsz)
{
    /* HttpOnly: JavaScript cannot read the cookie (limits XSS damage).
     * SameSite=Lax: the browser does not send it on cross-site POSTs,
     *   which blocks cross-site request forgery on our forms.
     * Max-Age: the browser forgets it when the server-side session expires.
     * (Secure is added in the HTTPS bonus step; over plain HTTP it would
     *  stop the cookie from being sent at all.) */
    snprintf(out, outsz,
             "Set-Cookie: " SESSION_COOKIE_NAME "=%s; Path=/; Max-Age=%d; "
             "HttpOnly; SameSite=Lax\r\n",
             s->token, SESSION_TTL_SEC);
}

const char *auth_clear_cookie_header(void)
{
    return "Set-Cookie: " SESSION_COOKIE_NAME "=; Path=/; Max-Age=0; "
           "HttpOnly; SameSite=Lax\r\n";
}
