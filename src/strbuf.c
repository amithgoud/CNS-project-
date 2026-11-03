/*
 * strbuf.c - Implementation of the growable string buffer.
 */
#include "strbuf.h"

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void sb_init(strbuf_t *sb)
{
    sb->data = NULL;
    sb->len  = 0;
    sb->cap  = 0;
    sb->oom  = 0;
}

void sb_free(strbuf_t *sb)
{
    free(sb->data);
    sb_init(sb);
}

/* Make room for `extra` more bytes plus the terminating NUL. */
static int sb_reserve(strbuf_t *sb, size_t extra)
{
    if (sb->oom)
        return -1;
    if (extra > SIZE_MAX - sb->len - 1) {     /* size_t overflow guard */
        sb->oom = 1;
        return -1;
    }

    size_t need = sb->len + extra + 1;
    if (need <= sb->cap)
        return 0;

    size_t cap = sb->cap ? sb->cap : 256;
    while (cap < need) {
        if (cap > SIZE_MAX / 2) {
            cap = need;
            break;
        }
        cap *= 2;
    }

    char *p = realloc(sb->data, cap);
    if (!p) {
        sb->oom = 1;
        return -1;
    }
    sb->data = p;
    sb->cap  = cap;
    return 0;
}

int sb_append(strbuf_t *sb, const char *s, size_t n)
{
    if (sb_reserve(sb, n) < 0)
        return -1;
    memcpy(sb->data + sb->len, s, n);
    sb->len += n;
    sb->data[sb->len] = '\0';
    return 0;
}

int sb_puts(strbuf_t *sb, const char *s)
{
    return sb_append(sb, s, strlen(s));
}

int sb_printf(strbuf_t *sb, const char *fmt, ...)
{
    va_list ap, ap2;

    /* First pass measures, second pass writes into the reserved space. */
    va_start(ap, fmt);
    va_copy(ap2, ap);
    int n = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);

    if (n < 0) {
        va_end(ap2);
        sb->oom = 1;
        return -1;
    }
    if (sb_reserve(sb, (size_t)n) < 0) {
        va_end(ap2);
        return -1;
    }
    vsnprintf(sb->data + sb->len, (size_t)n + 1, fmt, ap2);
    va_end(ap2);
    sb->len += (size_t)n;
    return 0;
}

int sb_append_html(strbuf_t *sb, const char *s)
{
    const char *run = s;    /* start of the current run of safe chars */

    for (; *s; s++) {
        const char *ent;
        switch (*s) {
        case '&':  ent = "&amp;";  break;
        case '<':  ent = "&lt;";   break;
        case '>':  ent = "&gt;";   break;
        case '"':  ent = "&quot;"; break;
        case '\'': ent = "&#39;";  break;
        default:   continue;
        }
        sb_append(sb, run, (size_t)(s - run));
        sb_puts(sb, ent);
        run = s + 1;
    }
    sb_append(sb, run, (size_t)(s - run));
    return sb->oom ? -1 : 0;
}

const char *sb_cstr(const strbuf_t *sb)
{
    return sb->data ? sb->data : "";
}
