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
#include <stdlib.h>
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
    "CREATE INDEX IF NOT EXISTS idx_sessions_expiry ON sessions(expires_at);"
    "CREATE TABLE IF NOT EXISTS queries ("
    "  id           INTEGER PRIMARY KEY AUTOINCREMENT,"
    "  student_id   INTEGER NOT NULL REFERENCES users(id) ON DELETE CASCADE,"
    "  question     TEXT    NOT NULL,"
    "  state        TEXT    NOT NULL DEFAULT 'PENDING_LLM' CHECK (state IN"
    "                 ('PENDING_LLM', 'LLM_ANSWERED', 'TEACHER_APPROVED',"
    "                  'TEACHER_EDITED', 'DELIVERED')),"
    "  llm_answer   TEXT,"
    "  final_answer TEXT,"
    "  teacher_id   INTEGER REFERENCES users(id),"
    "  created_at   INTEGER NOT NULL,"
    "  updated_at   INTEGER NOT NULL"
    ");"
    "CREATE INDEX IF NOT EXISTS idx_queries_student ON queries(student_id, id);"
    "CREATE INDEX IF NOT EXISTS idx_queries_state   ON queries(state, id);";

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

/* ------------------------------------------------------------------ */
/* Queries                                                            */
/* ------------------------------------------------------------------ */

static const char *const QSTATE_NAMES[Q_STATE_COUNT] = {
    "PENDING_LLM", "LLM_ANSWERED", "TEACHER_APPROVED", "TEACHER_EDITED", "DELIVERED",
};

const char *qstate_name(qstate_t s)
{
    return ((int)s >= 0 && (int)s < Q_STATE_COUNT) ? QSTATE_NAMES[s] : "UNKNOWN";
}

static qstate_t qstate_parse(const char *s)
{
    for (int i = 0; i < Q_STATE_COUNT; i++)
        if (strcmp(s, QSTATE_NAMES[i]) == 0)
            return (qstate_t)i;
    return Q_PENDING_LLM;
}

int qstate_has_answer(qstate_t s)
{
    return s >= Q_TEACHER_APPROVED;
}

void query_free(query_t *q)
{
    free(q->question);
    free(q->llm_answer);
    free(q->final_answer);
    q->question = q->llm_answer = q->final_answer = NULL;
}

void query_list_free(query_t *list, int n)
{
    for (int i = 0; i < n; i++)
        query_free(&list[i]);
    free(list);
}

/* Heap copy of a text column, or NULL if the column is NULL. */
static char *dup_text(sqlite3_stmt *st, int col)
{
    const unsigned char *t = sqlite3_column_text(st, col);
    return t ? strdup((const char *)t) : NULL;
}

/* Every query SELECT uses these columns in this order (see read_query). */
#define QUERY_COLUMNS \
    "SELECT q.id, q.student_id, su.username, q.teacher_id, tu.username, "  \
    "q.state, q.question, q.llm_answer, q.final_answer, "                  \
    "q.created_at, q.updated_at "                                          \
    "FROM queries q JOIN users su ON su.id = q.student_id "                \
    "LEFT JOIN users tu ON tu.id = q.teacher_id "

static void read_query(sqlite3_stmt *st, query_t *q)
{
    char state[32];

    memset(q, 0, sizeof *q);
    q->id         = sqlite3_column_int64(st, 0);
    q->student_id = sqlite3_column_int64(st, 1);
    copy_text(st, 2, q->student_name, sizeof q->student_name);
    q->teacher_id = sqlite3_column_int64(st, 3);    /* NULL reads as 0 */
    copy_text(st, 4, q->teacher_name, sizeof q->teacher_name);
    copy_text(st, 5, state, sizeof state);
    q->state        = qstate_parse(state);
    q->question     = dup_text(st, 6);
    q->llm_answer   = dup_text(st, 7);
    q->final_answer = dup_text(st, 8);
    q->created_at   = sqlite3_column_int64(st, 9);
    q->updated_at   = sqlite3_column_int64(st, 10);
}

long long db_query_create(long long student_id, const char *question)
{
    sqlite3_stmt *st = NULL;
    long long rc = -1;
    sqlite3_int64 now = (sqlite3_int64)time(NULL);

    pthread_mutex_lock(&db_lock);
    if (prepare("INSERT INTO queries (student_id, question, state, created_at, updated_at) "
                "VALUES (?, ?, 'PENDING_LLM', ?, ?)", &st) < 0)
        goto out;

    sqlite3_bind_int64(st, 1, student_id);
    sqlite3_bind_text(st, 2, question, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(st, 3, now);
    sqlite3_bind_int64(st, 4, now);

    if (sqlite3_step(st) == SQLITE_DONE)
        rc = sqlite3_last_insert_rowid(db);
    else
        log_error("db: create query: %s", sqlite3_errmsg(db));
out:
    sqlite3_finalize(st);
    pthread_mutex_unlock(&db_lock);
    return rc;
}

int db_query_get(long long id, query_t *out)
{
    sqlite3_stmt *st = NULL;
    int rc = -1;

    pthread_mutex_lock(&db_lock);
    if (prepare(QUERY_COLUMNS "WHERE q.id = ?", &st) < 0)
        goto out;

    sqlite3_bind_int64(st, 1, id);

    int s = sqlite3_step(st);
    if (s == SQLITE_ROW) {
        read_query(st, out);
        rc = 1;
    } else if (s == SQLITE_DONE) {
        rc = 0;
    } else {
        log_error("db: get query: %s", sqlite3_errmsg(db));
    }
out:
    sqlite3_finalize(st);
    pthread_mutex_unlock(&db_lock);
    return rc;
}

/*
 * Run QUERY_COLUMNS + where_sql and collect up to max rows. where_sql
 * must end in "LIMIT ?". If has_arg, it also contains one other "?"
 * before the LIMIT, bound to arg.
 */
static int list_queries(const char *where_sql, int has_arg, long long arg,
                        int max, query_t **out)
{
    sqlite3_stmt *st = NULL;
    int n = -1;
    char sql[1024];

    *out = NULL;
    query_t *list = calloc((size_t)max, sizeof *list);
    if (!list)
        return -1;
    snprintf(sql, sizeof sql, "%s%s", QUERY_COLUMNS, where_sql);

    pthread_mutex_lock(&db_lock);
    if (prepare(sql, &st) < 0)
        goto out;

    int idx = 1;
    if (has_arg)
        sqlite3_bind_int64(st, idx++, arg);
    sqlite3_bind_int(st, idx, max);

    n = 0;
    int s = SQLITE_DONE;
    while (n < max && (s = sqlite3_step(st)) == SQLITE_ROW)
        read_query(st, &list[n++]);
    if (n < max && s != SQLITE_DONE) {
        log_error("db: list queries: %s", sqlite3_errmsg(db));
        query_list_free(list, n);
        list = NULL;
        n = -1;
    }
out:
    sqlite3_finalize(st);
    pthread_mutex_unlock(&db_lock);
    if (n < 0)
        free(list);
    else
        *out = list;
    return n;
}

int db_query_list_by_student(long long student_id, int max, query_t **out)
{
    return list_queries("WHERE q.student_id = ? ORDER BY q.id DESC LIMIT ?",
                        1, student_id, max, out);
}

int db_query_list_review_queue(int max, query_t **out)
{
    /* Oldest first: first come, first served. */
    return list_queries("WHERE q.state = 'LLM_ANSWERED' ORDER BY q.id ASC LIMIT ?",
                        0, 0, max, out);
}

int db_query_list_reviewed(int max, query_t **out)
{
    return list_queries("WHERE q.state IN ('TEACHER_APPROVED', 'TEACHER_EDITED', 'DELIVERED') "
                        "ORDER BY q.updated_at DESC, q.id DESC LIMIT ?",
                        0, 0, max, out);
}

int db_query_review(long long id, long long teacher_id, const char *final_answer,
                    int edited)
{
    sqlite3_stmt *st = NULL;
    int rc = -1;

    pthread_mutex_lock(&db_lock);
    /* "AND state = 'LLM_ANSWERED'" makes the check and the update one
     * atomic step: if two teachers submit at once, only the first UPDATE
     * matches the row; the second changes 0 rows and is told so. */
    if (prepare("UPDATE queries SET final_answer = ?, teacher_id = ?, state = ?, "
                "updated_at = ? WHERE id = ? AND state = 'LLM_ANSWERED'", &st) < 0)
        goto out;

    sqlite3_bind_text(st, 1, final_answer, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(st, 2, teacher_id);
    sqlite3_bind_text(st, 3, qstate_name(edited ? Q_TEACHER_EDITED : Q_TEACHER_APPROVED),
                      -1, SQLITE_STATIC);
    sqlite3_bind_int64(st, 4, (sqlite3_int64)time(NULL));
    sqlite3_bind_int64(st, 5, id);

    if (sqlite3_step(st) == SQLITE_DONE)
        rc = sqlite3_changes(db) > 0 ? 1 : 0;
    else
        log_error("db: review query: %s", sqlite3_errmsg(db));
out:
    sqlite3_finalize(st);
    pthread_mutex_unlock(&db_lock);
    return rc;
}

int db_query_mark_delivered(long long id)
{
    sqlite3_stmt *st = NULL;
    int rc = -1;

    pthread_mutex_lock(&db_lock);
    if (prepare("UPDATE queries SET state = 'DELIVERED', updated_at = ? "
                "WHERE id = ? AND state IN ('TEACHER_APPROVED', 'TEACHER_EDITED')", &st) < 0)
        goto out;

    sqlite3_bind_int64(st, 1, (sqlite3_int64)time(NULL));
    sqlite3_bind_int64(st, 2, id);

    if (sqlite3_step(st) == SQLITE_DONE)
        rc = sqlite3_changes(db) > 0 ? 1 : 0;
    else
        log_error("db: mark delivered: %s", sqlite3_errmsg(db));
out:
    sqlite3_finalize(st);
    pthread_mutex_unlock(&db_lock);
    return rc;
}

int db_query_count_by_state(long long counts[Q_STATE_COUNT])
{
    sqlite3_stmt *st = NULL;
    int rc = -1;

    for (int i = 0; i < Q_STATE_COUNT; i++)
        counts[i] = 0;

    pthread_mutex_lock(&db_lock);
    if (prepare("SELECT state, COUNT(*) FROM queries GROUP BY state", &st) < 0)
        goto out;

    int s;
    while ((s = sqlite3_step(st)) == SQLITE_ROW) {
        const unsigned char *name = sqlite3_column_text(st, 0);
        if (name)
            counts[qstate_parse((const char *)name)] = sqlite3_column_int64(st, 1);
    }
    if (s == SQLITE_DONE)
        rc = 0;
    else
        log_error("db: count queries: %s", sqlite3_errmsg(db));
out:
    sqlite3_finalize(st);
    pthread_mutex_unlock(&db_lock);
    return rc;
}

int db_query_ids_by_state(qstate_t state, long long *ids, int max)
{
    sqlite3_stmt *st = NULL;
    int n = -1;

    pthread_mutex_lock(&db_lock);
    if (prepare("SELECT id FROM queries WHERE state = ? ORDER BY id LIMIT ?", &st) < 0)
        goto out;

    sqlite3_bind_text(st, 1, qstate_name(state), -1, SQLITE_STATIC);
    sqlite3_bind_int(st, 2, max);

    n = 0;
    int s = SQLITE_DONE;
    while (n < max && (s = sqlite3_step(st)) == SQLITE_ROW)
        ids[n++] = sqlite3_column_int64(st, 0);
    if (n < max && s != SQLITE_DONE) {
        log_error("db: ids by state: %s", sqlite3_errmsg(db));
        n = -1;
    }
out:
    sqlite3_finalize(st);
    pthread_mutex_unlock(&db_lock);
    return n;
}

int db_query_set_llm_answer(long long id, const char *answer)
{
    sqlite3_stmt *st = NULL;
    int rc = -1;

    pthread_mutex_lock(&db_lock);
    if (prepare("UPDATE queries SET llm_answer = ?, state = 'LLM_ANSWERED', updated_at = ? "
                "WHERE id = ? AND state = 'PENDING_LLM'", &st) < 0)
        goto out;

    sqlite3_bind_text(st, 1, answer, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(st, 2, (sqlite3_int64)time(NULL));
    sqlite3_bind_int64(st, 3, id);

    if (sqlite3_step(st) == SQLITE_DONE)
        rc = sqlite3_changes(db) > 0 ? 1 : 0;
    else
        log_error("db: set llm answer: %s", sqlite3_errmsg(db));
out:
    sqlite3_finalize(st);
    pthread_mutex_unlock(&db_lock);
    return rc;
}
