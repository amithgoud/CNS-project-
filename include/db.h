/*
 * db.h - SQLite storage. Every query uses a prepared statement with bound
 * parameters (sqlite3_bind_*), so user input is never pasted into SQL
 * text and SQL injection is impossible.
 */
#ifndef DB_H
#define DB_H

#include "config.h"

#include <stddef.h>

typedef enum {
    ROLE_STUDENT,
    ROLE_TEACHER
} role_t;

typedef struct {
    long long id;
    char      username[USERNAME_MAX + 1];
    role_t    role;
} user_t;

#define DB_ERR_EXISTS (-2)

/* Open (creating if needed) the database and its tables. 0 on success. */
int  db_open(const char *path);
void db_close(void);

const char *role_name(role_t role);              /* "student" / "teacher" */
int         role_parse(const char *s, role_t *out); /* 0 on success */

/* Insert a user. Returns the new id, DB_ERR_EXISTS if the username is
 * taken, or -1 on error. */
long long db_create_user(const char *username, const char *pass_hash, role_t role);

/* Look up a user by name; copies the stored hash into hash.
 * Returns 1 found, 0 not found, -1 error. */
int db_find_user(const char *username, user_t *u, char *hash, size_t hashsz);

/* Sessions: token -> user, valid while expires_at > now (Unix seconds). */
int db_session_create(const char *token, long long user_id, long long expires_at);
int db_session_lookup(const char *token, long long now, user_t *u,
                      long long *expires_at);   /* 1 valid, 0 not, -1 error */
int db_session_delete(const char *token);
int db_session_purge(long long now);             /* delete expired sessions */

#endif /* DB_H */
