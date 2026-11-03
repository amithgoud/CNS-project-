/*
 * template.c - Tiny HTML template engine (see template.h).
 *
 * Templates are read from disk on every request. That is slower than
 * caching, but it means you can edit a page and just refresh the browser,
 * and it needs no locking between threads.
 */
#include "template.h"
#include "config.h"
#include "util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_TEMPLATE_SIZE (256 * 1024)

static const char *lookup(const tpl_var_t *vars, size_t n,
                          const char *key, size_t klen)
{
    for (size_t i = 0; i < n; i++)
        if (strlen(vars[i].key) == klen && strncmp(vars[i].key, key, klen) == 0)
            return vars[i].value;
    return NULL;
}

int template_render(strbuf_t *out, const char *name,
                    const tpl_var_t *vars, size_t nvars)
{
    char path[256];
    snprintf(path, sizeof path, "%s/%s", TEMPLATE_DIR, name);

    size_t len;
    char *src = read_whole_file(path, MAX_TEMPLATE_SIZE, &len);
    if (!src)
        return -1;

    const char *p = src;
    while (*p) {
        const char *open = strstr(p, "{{");
        if (!open) {
            sb_puts(out, p);
            break;
        }
        sb_append(out, p, (size_t)(open - p));

        int raw = (open[2] == '{');
        const char *ks = open + (raw ? 3 : 2);
        const char *close = strstr(ks, raw ? "}}}" : "}}");
        if (!close) {                       /* unterminated: copy as-is */
            sb_puts(out, open);
            break;
        }

        const char *ke = close;
        while (ks < ke && *ks == ' ')
            ks++;
        while (ke > ks && ke[-1] == ' ')
            ke--;

        const char *val = lookup(vars, nvars, ks, (size_t)(ke - ks));
        if (val) {
            if (raw)
                sb_puts(out, val);
            else
                sb_append_html(out, val);
        }
        p = close + (raw ? 3 : 2);
    }

    free(src);
    return out->oom ? -1 : 0;
}
