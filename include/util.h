/*
 * util.h - Small helpers shared by several modules.
 */
#ifndef UTIL_H
#define UTIL_H

#include <stddef.h>

/*
 * Read a whole file into a freshly malloc'd, NUL-terminated buffer.
 * Fails (returns NULL) if the file is missing or larger than max_size.
 * On success *out_len is set to the file size; the caller frees the buffer.
 */
char *read_whole_file(const char *path, size_t max_size, size_t *out_len);

#endif /* UTIL_H */
