/*
 * template.h - Tiny HTML template engine for files in TEMPLATE_DIR.
 *
 *   {{name}}    replaced by the variable's value, HTML-escaped (safe for
 *               user input)
 *   {{{name}}}  replaced by the raw value (only for HTML the server
 *               built itself)
 *
 * Unknown variables expand to nothing.
 */
#ifndef TEMPLATE_H
#define TEMPLATE_H

#include "strbuf.h"

#include <stddef.h>

typedef struct {
    const char *key;
    const char *value;
} tpl_var_t;

/* Render TEMPLATE_DIR/name and append the result to out. 0 on success. */
int template_render(strbuf_t *out, const char *name,
                    const tpl_var_t *vars, size_t nvars);

#endif /* TEMPLATE_H */
