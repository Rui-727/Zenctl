/* internal.h - shared internal helpers for libzenctl
 *
 * These helpers are NOT part of the public API. They exist so every
 * domain implementation (cpu.c, mem.c, ...) reads and writes sysfs /
 * procfs the same way, with consistent error handling and unit
 * conventions. The double-underscore prefix marks them private.
 */
#ifndef ZENCTL_INTERNAL_H
#define ZENCTL_INTERNAL_H

#include <stddef.h>
#include <stdint.h>

#include "zenctl.h"

/* Fill an error struct. NULL err is ignored. */
void zenctl__set_err(zenctl_err_t *err, int code,
                     const char *msg, const char *ctx);

/* Map a libc errno value to the nearest zenctl error code. */
int  zenctl__errno_to_code(int e);

/* Read the entire contents of `path` into `buf` (NUL-terminated).
 * Trailing CR/LF is stripped. `bufsize` is the total buffer size
 * including the NUL byte. Returns 0 on success, -1 on error. */
int  zenctl__read_file_string(const char *path, char *buf, size_t bufsize,
                              zenctl_err_t *err);

/* Write the exact string `value` (no added whitespace) to `path`.
 * Returns 0 on success, -1 on error. */
int  zenctl__write_file_string(const char *path, const char *value,
                               zenctl_err_t *err);

/* Read a base-10 signed 64-bit integer from `path`. Returns 0 / -1. */
int  zenctl__read_file_i64(const char *path, int64_t *out, zenctl_err_t *err);

/* Write a signed 64-bit integer to `path` as base-10 ASCII. */
int  zenctl__write_file_i64(const char *path, int64_t val, zenctl_err_t *err);

#endif /* ZENCTL_INTERNAL_H */
