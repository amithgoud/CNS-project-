/*
 * strbuf.h - Growable string buffer.
 *
 * Used to build HTTP responses and HTML pages without fixed-size arrays,
 * which removes a whole class of buffer overflows: every append checks
 * and grows the capacity first. If an allocation ever fails the buffer
 * is marked "oom" and later appends become no-ops, so callers only need
 * to check once at the end.
 */
#ifndef STRBUF_H
#define STRBUF_H

#include <stddef.h>

#ifdef __GNUC__
#define SB_PRINTF_FMT(a, b) __attribute__((format(printf, a, b)))
#else
#define SB_PRINTF_FMT(a, b)
#endif

typedef struct {
    char  *data;    /* always NUL-terminated once anything is appended */
    size_t len;     /* bytes used, excluding the terminating NUL */
    size_t cap;     /* bytes allocated */
    int    oom;     /* set if an allocation failed */
} strbuf_t;

void        sb_init(strbuf_t *sb);
void        sb_free(strbuf_t *sb);
int         sb_append(strbuf_t *sb, const char *s, size_t n);
int         sb_puts(strbuf_t *sb, const char *s);
int         sb_printf(strbuf_t *sb, const char *fmt, ...) SB_PRINTF_FMT(2, 3);

/* Append s with & < > " ' replaced by HTML entities. Every piece of
 * user-supplied text must go through this before it is put in a page,
 * otherwise a student could inject <script> into the teacher's browser
 * (cross-site scripting). */
int         sb_append_html(strbuf_t *sb, const char *s);

/* Contents as a C string ("" if nothing was appended yet). */
const char *sb_cstr(const strbuf_t *sb);

#endif /* STRBUF_H */
