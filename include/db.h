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

/* ------------------------------------------------------------------ */
/* Queries (student questions)                                        */
/* ------------------------------------------------------------------ */

/*
 * Lifecycle of a question. The order matters: a later state never goes
 * back to an earlier one, so "state >= Q_TEACHER_APPROVED" means "has a
 * teacher-checked answer".
 *
 *   PENDING_LLM -> LLM_ANSWERED -> TEACHER_APPROVED | TEACHER_EDITED -> DELIVERED
 */
typedef enum {
    Q_PENDING_LLM,
    Q_LLM_ANSWERED,
    Q_TEACHER_APPROVED,
    Q_TEACHER_EDITED,
    Q_DELIVERED,
    Q_STATE_COUNT
} qstate_t;

typedef struct {
    long long id;
    long long student_id;
    char      student_name[USERNAME_MAX + 1];
    long long teacher_id;                   /* 0 until a teacher reviews it */
    char      teacher_name[USERNAME_MAX + 1];
    qstate_t  state;
    char     *question;                     /* heap strings; NULL if empty */
    char     *llm_answer;
    char     *final_answer;
    long long created_at;
    long long updated_at;
} query_t;

const char *qstate_name(qstate_t s);    /* "PENDING_LLM", ... (as stored) */
int         qstate_has_answer(qstate_t s);  /* teacher-checked answer exists */

/* Free the heap strings inside one query / an array from db_query_list_*. */
void query_free(query_t *q);
void query_list_free(query_t *list, int n);

/* Insert a new question in state PENDING_LLM. Returns its id or -1. */
long long db_query_create(long long student_id, const char *question);

/* Load one question. 1 found, 0 not found, -1 error. */
int db_query_get(long long id, query_t *out);

/* A student's questions, newest first, at most max. Returns the count
 * (and a malloc'd array in *out, free with query_list_free) or -1. */
int db_query_list_by_student(long long student_id, int max, query_t **out);

/* Number of questions in each state, for the status page. 0 on success. */
int db_query_count_by_state(long long counts[Q_STATE_COUNT]);

/* Ids of questions in a state, oldest first, at most max. Returns count or -1. */
int db_query_ids_by_state(qstate_t state, long long *ids, int max);

/* Store the LLM draft and move PENDING_LLM -> LLM_ANSWERED. The WHERE
 * clause only matches PENDING_LLM, so a state can never move backwards.
 * Returns 1 if updated, 0 if the question was not PENDING_LLM, -1 error. */
int db_query_set_llm_answer(long long id, const char *answer);

#endif /* DB_H */
