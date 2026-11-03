/*
 * util.c - Small helpers shared by several modules.
 */
#include "util.h"
#include "strbuf.h"

#include <stdio.h>
#include <stdlib.h>

char *read_whole_file(const char *path, size_t max_size, size_t *out_len)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return NULL;

    strbuf_t sb;
    sb_init(&sb);

    char chunk[4096];
    size_t n;
    while ((n = fread(chunk, 1, sizeof chunk, f)) > 0) {
        if (sb.len + n > max_size) {
            fclose(f);
            sb_free(&sb);
            return NULL;
        }
        sb_append(&sb, chunk, n);
    }

    int read_error = ferror(f);
    fclose(f);
    if (read_error || sb.oom) {
        sb_free(&sb);
        return NULL;
    }

    if (!sb.data) {                 /* empty file: return "" */
        sb.data = calloc(1, 1);
        if (!sb.data)
            return NULL;
    }
    *out_len = sb.len;
    return sb.data;                 /* ownership passes to the caller */
}
