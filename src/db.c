/*
 * db.c - SQLite storage layer.
 *
 * Concurrency: the server has one SQLite connection shared by all worker
 * threads, guarded by db_lock. Each function holds the lock for the whole
 * prepare -> bind -> step -> finalize sequence, so two threads never
 * interleave statements on the connection and sqlite3_errmsg() /
 * sqlite3_last_insert_rowid() always describe *our* statement.
 * Database work is tiny compared with network I/O, so one lock is plenty.
 */
#include "db.h"
#include "log.h"

#include <pthread.h>
#include <sqlite3.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

static sqlite3        *db = NULL;
static pthread_mutex_t db_lock = PTHREAD_MUTEX_INITIALIZER;

static const char SCHEMA[] =
    "PRAGMA foreign_keys = ON;"
    "CREATE TABLE IF NOT EXISTS users ("
    "  id          INTEGER PRIMARY KEY AUTOINCREMENT,"
    "  username    TEXT    NOT NULL UNIQUE,"
    "  pass_hash   TEXT    NOT NULL,"
    "  role        TEXT    NOT NULL CHECK (role IN ('student', 'teacher')),"
    "  created_at  INTEGER NOT NULL"
    ");"
    "CREATE TABLE IF NOT EXISTS sessions ("
    "  token       TEXT    PRIMARY KEY,"
    "  user_id     INTEGER NOT NULL REFERENCES users(id) ON DELETE CASCADE,"
    "  expires_at  INTEGER NOT NULL"
    ");"
    "CREATE INDEX IF NOT EXISTS idx_sessions_expiry ON sessions(expires_at);";

int db_open(const char *path)
{
    int rc = sqlite3_open_v2(path, &db,
                             SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE |
                             SQLITE_OPEN_FULLMUTEX, NULL);
    if (rc != SQLITE_OK) {
        log_error("db: cannot open %s: %s", path, db ? sqlite3_errmsg(db) : "out of memory");
        sqlite3_close(db);
        db = NULL;
        return -1;
    }
    sqlite3_busy_timeout(db, 5000);

    /* Fixed SQL with no user input, so sqlite3_exec is fine here. */
    char *err = NULL;
    if (sqlite3_exec(db, SCHEMA, NULL, NULL, &err) != SQLITE_OK) {
        log_error("db: schema: %s", err ? err : "?");
        sqlite3_free(err);
        sqlite3_close(db);
        db = NULL;
        return -1;
    }
    log_info("db: opened %s", path);
    return 0;
}

void db_close(void)
{
    pthread_mutex_lock(&db_lock);
    sqlite3_close(db);
    db = NULL;
    pthread_mutex_unlock(&db_lock);
}

const char *role_name(role_t role)
{
    return role == ROLE_TEACHER ? "teacher" : "student";
}

int role_parse(const char *s, role_t *out)
{
    if (s && strcmp(s, "student") == 0) { *out = ROLE_STUDENT; return 0; }
    if (s && strcmp(s, "teacher") == 0) { *out = ROLE_TEACHER; return 0; }
    return -1;
}

/* Copy a text column into a fixed buffer, truncating safely. */
static void copy_text(sqlite3_stmt *st, int col, char *out, size_t outsz)
{
    const unsigned char *t = sqlite3_column_text(st, col);
    snprintf(out, outsz, "%s", t ? (const char *)t : "");
}

/* Columns col, col+1, col+2 must be id, username, role. */
static void read_user(sqlite3_stmt *st, int col, user_t *u)
{
    char role[16];
    u->id = sqlite3_column_int64(st, col);
    copy_text(st, col + 1, u->username, sizeof u->username);
    copy_text(st, col + 2, role, sizeof role);
    if (role_parse(role, &u->role) < 0)
        u->role = ROLE_STUDENT;
}

static int prepare(const char *sql, sqlite3_stmt **st)
{
    if (sqlite3_prepare_v2(db, sql, -1, st, NULL) != SQLITE_OK) {
        log_error("db: prepare: %s", sqlite3_errmsg(db));
        return -1;
    }
    return 0;
}

long long db_create_user(const char *username, const char *pass_hash, role_t role)
{
    sqlite3_stmt *st = NULL;
    long long rc = -1;

    pthread_mutex_lock(&db_lock);
    if (prepare("INSERT INTO users (username, pass_hash, role, created_at) "
                "VALUES (?, ?, ?, ?)", &st) < 0)
        goto out;

    sqlite3_bind_text(st, 1, username, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 2, pass_hash, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 3, role_name(role), -1, SQLITE_STATIC);
    sqlite3_bind_int64(st, 4, (sqlite3_int64)time(NULL));

    int s = sqlite3_step(st);
    if (s == SQLITE_DONE)
        rc = sqlite3_last_insert_rowid(db);
    else if ((s & 0xff) == SQLITE_CONSTRAINT)
        rc = DB_ERR_EXISTS;                     /* UNIQUE(username) */
    else
        log_error("db: create user: %s", sqlite3_errmsg(db));
out:
    sqlite3_finalize(st);
    pthread_mutex_unlock(&db_lock);
    return rc;
}

int db_find_user(const char *username, user_t *u, char *hash, size_t hashsz)
{
    sqlite3_stmt *st = NULL;
    int rc = -1;

    pthread_mutex_lock(&db_lock);
    if (prepare("SELECT id, username, role, pass_hash FROM users "
                "WHERE username = ?", &st) < 0)
        goto out;

    sqlite3_bind_text(st, 1, username, -1, SQLITE_TRANSIENT);

    int s = sqlite3_step(st);
    if (s == SQLITE_ROW) {
        read_user(st, 0, u);
        copy_text(st, 3, hash, hashsz);
        rc = 1;
    } else if (s == SQLITE_DONE) {
        rc = 0;
    } else {
        log_error("db: find user: %s", sqlite3_errmsg(db));
    }
out:
    sqlite3_finalize(st);
    pthread_mutex_unlock(&db_lock);
    return rc;
}

int db_session_create(const char *token, long long user_id, long long expires_at)
{
    sqlite3_stmt *st = NULL;
    int rc = -1;

    pthread_mutex_lock(&db_lock);
    if (prepare("INSERT INTO sessions (token, user_id, expires_at) "
                "VALUES (?, ?, ?)", &st) < 0)
        goto out;

    sqlite3_bind_text(st, 1, token, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(st, 2, user_id);
    sqlite3_bind_int64(st, 3, expires_at);

    if (sqlite3_step(st) == SQLITE_DONE)
        rc = 0;
    else
        log_error("db: create session: %s", sqlite3_errmsg(db));
out:
    sqlite3_finalize(st);
    pthread_mutex_unlock(&db_lock);
    return rc;
}

int db_session_lookup(const char *token, long long now, user_t *u,
                      long long *expires_at)
{
    sqlite3_stmt *st = NULL;
    int rc = -1;

    pthread_mutex_lock(&db_lock);
    if (prepare("SELECT u.id, u.username, u.role, s.expires_at "
                "FROM sessions s JOIN users u ON u.id = s.user_id "
                "WHERE s.token = ? AND s.expires_at > ?", &st) < 0)
        goto out;

    sqlite3_bind_text(st, 1, token, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(st, 2, now);

    int s = sqlite3_step(st);
    if (s == SQLITE_ROW) {
        read_user(st, 0, u);
        *expires_at = sqlite3_column_int64(st, 3);
        rc = 1;
    } else if (s == SQLITE_DONE) {
        rc = 0;
    } else {
        log_error("db: lookup session: %s", sqlite3_errmsg(db));
    }
out:
    sqlite3_finalize(st);
    pthread_mutex_unlock(&db_lock);
    return rc;
}

/* Run a one-parameter statement that returns no rows. */
static int exec_simple(const char *sql, const char *text, long long num, int is_text)
{
    sqlite3_stmt *st = NULL;
    int rc = -1;

    pthread_mutex_lock(&db_lock);
    if (prepare(sql, &st) < 0)
        goto out;

    if (is_text)
        sqlite3_bind_text(st, 1, text, -1, SQLITE_TRANSIENT);
    else
        sqlite3_bind_int64(st, 1, num);

    if (sqlite3_step(st) == SQLITE_DONE)
        rc = sqlite3_changes(db);
    else
        log_error("db: %s", sqlite3_errmsg(db));
out:
    sqlite3_finalize(st);
    pthread_mutex_unlock(&db_lock);
    return rc;
}

int db_session_delete(const char *token)
{
    return exec_simple("DELETE FROM sessions WHERE token = ?", token, 0, 1);
}

int db_session_purge(long long now)
{
    return exec_simple("DELETE FROM sessions WHERE expires_at <= ?", NULL, now, 0);
}
