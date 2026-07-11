/* io.c - shared sysfs/procfs read/write helpers for libzenctl
 *
 * Every domain implementation goes through these. They enforce:
 *  - trailing-newline stripping on reads
 *  - no-whitespace writes (kernel string parsers reject leading/trailing
 *    spaces; trailing newlines are tolerated but not required, so we do
 *    not add one)
 *  - consistent errno -> zenctl_err_t mapping
 *
 * Test redirection: if ZENCTL_SYSFS_PREFIX is set in the environment,
 * any path starting with "/sys/" or "/proc/" is prefixed with it. This
 * lets the unit tests point the library at a throwaway fixture tree
 * (e.g. ZENCTL_SYSFS_PREFIX=/tmp/zenctl-mock) without touching the
 * real kernel surface. The feature is a no-op in production.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <stdint.h>

#include "zenctl/internal.h"

/* Resolve a sysfs/procfs path against ZENCTL_SYSFS_PREFIX.
 * Returns a pointer to either `path` (unchanged) or `buf` (rewritten
 * as "<prefix><path>"). `buf` must be at least strlen(path)+256 bytes. */
static const char *zenctl__resolve_path(const char *path,
                                        char *buf, size_t bufsz)
{
    if (!path) return path;
    const char *prefix = getenv("ZENCTL_SYSFS_PREFIX");
    if (!prefix || !*prefix) return path;
    if (strncmp(path, "/sys/", 5) == 0 || strncmp(path, "/proc/", 6) == 0) {
        int n = snprintf(buf, bufsz, "%s%s", prefix, path);
        if (n < 0 || (size_t)n >= bufsz) return path; /* overflow: fall back */
        return buf;
    }
    return path;
}

void zenctl__set_err(zenctl_err_t *err, int code,
                     const char *msg, const char *ctx)
{
    if (!err) return;
    err->code = code;
    snprintf(err->message, sizeof(err->message), "%s",
             msg ? msg : zenctl_strerror(code));
    snprintf(err->context, sizeof(err->context), "%s",
             ctx ? ctx : "");
    err->recoverable = (code != ZENCTL_ERR_INTERNAL);
}

int zenctl__errno_to_code(int e)
{
    switch (e) {
    case 0:        return ZENCTL_OK;
    case EACCES:
    case EPERM:    return ZENCTL_ERR_EPERM;
    case ENOENT:   return ZENCTL_ERR_ENOENT;
    case EINVAL:   return ZENCTL_ERR_EINVAL;
    case EIO:      return ZENCTL_ERR_EIO;
    case ENOMEM:   return ZENCTL_ERR_NOMEM;
    case ERANGE:   return ZENCTL_ERR_ERANGE;
    case ENOTSUP:   return ZENCTL_ERR_ENOTSUP;
    default:       return ZENCTL_ERR_INTERNAL;
    }
}

int zenctl__read_file_string(const char *path, char *buf, size_t bufsize,
                             zenctl_err_t *err)
{
    if (!path || !buf || bufsize == 0) {
        zenctl__set_err(err, ZENCTL_ERR_EINVAL,
                        "NULL path or buf", "zenctl__read_file_string");
        return -1;
    }

    char rp[4096];
    const char *rpath = zenctl__resolve_path(path, rp, sizeof(rp));

    FILE *f = fopen(rpath, "r");
    if (!f) {
        zenctl__set_err(err, zenctl__errno_to_code(errno),
                        strerror(errno), path);
        return -1;
    }

    size_t n = fread(buf, 1, bufsize - 1, f);
    int ferr = ferror(f);
    int saved = errno;
    fclose(f);
    if (ferr) {
        zenctl__set_err(err, zenctl__errno_to_code(saved),
                        "read error", path);
        return -1;
    }

    buf[n] = '\0';
    /* strip trailing CR/LF */
    while (n > 0 && (buf[n - 1] == '\n' || buf[n - 1] == '\r'))
        buf[--n] = '\0';
    return 0;
}

int zenctl__write_file_string(const char *path, const char *value,
                              zenctl_err_t *err)
{
    if (!path || !value) {
        zenctl__set_err(err, ZENCTL_ERR_EINVAL,
                        "NULL path or value", "zenctl__write_file_string");
        return -1;
    }

    char rp[4096];
    const char *rpath = zenctl__resolve_path(path, rp, sizeof(rp));

    FILE *f = fopen(rpath, "w");
    if (!f) {
        zenctl__set_err(err, zenctl__errno_to_code(errno),
                        strerror(errno), path);
        return -1;
    }

    size_t len = strlen(value);
    size_t w = fwrite(value, 1, len, f);
    int saved = (w != len) ? errno : 0;
    if (fclose(f) != 0 && saved == 0)
        saved = errno;

    if (w != len || saved != 0) {
        zenctl__set_err(err, zenctl__errno_to_code(saved),
                        "write error", path);
        return -1;
    }
    return 0;
}

int zenctl__read_file_i64(const char *path, int64_t *out, zenctl_err_t *err)
{
    char buf[64];
    if (zenctl__read_file_string(path, buf, sizeof(buf), err) != 0)
        return -1;

    errno = 0;
    char *end = NULL;
    long long v = strtoll(buf, &end, 10);
    if (errno != 0 || end == buf) {
        zenctl__set_err(err, ZENCTL_ERR_EINVAL,
                        "not a base-10 integer", path);
        return -1;
    }
    *out = (int64_t)v;
    return 0;
}

int zenctl__write_file_i64(const char *path, int64_t val, zenctl_err_t *err)
{
    char buf[32];
    snprintf(buf, sizeof(buf), "%lld", (long long)val);
    return zenctl__write_file_string(path, buf, err);
}

int zenctl__read_file_binary(const char *path, uint8_t **out, size_t *out_len,
                             zenctl_err_t *err)
{
    if (!path || !out || !out_len) {
        zenctl__set_err(err, ZENCTL_ERR_EINVAL,
                        "NULL path or out", "zenctl__read_file_binary");
        return -1;
    }
    *out = NULL;
    *out_len = 0;

    char rp[4096];
    const char *rpath = zenctl__resolve_path(path, rp, sizeof(rp));

    FILE *f = fopen(rpath, "rb");
    if (!f) {
        zenctl__set_err(err, zenctl__errno_to_code(errno),
                        strerror(errno), path);
        return -1;
    }

    /* Grow a heap buffer to fit the whole file. Used for EFI variable
     * payloads (small) and ACPI tables (often tens of KB). */
    size_t cap = 256, len = 0;
    uint8_t *buf = malloc(cap);
    if (!buf) {
        int saved = errno;
        fclose(f);
        zenctl__set_err(err, ZENCTL_ERR_NOMEM, "malloc failed", path);
        (void)saved;
        return -1;
    }
    for (;;) {
        if (len == cap) {
            size_t ncap = cap * 2;
            uint8_t *nb = realloc(buf, ncap);
            if (!nb) {
                int saved = errno;
                free(buf);
                fclose(f);
                zenctl__set_err(err, ZENCTL_ERR_NOMEM, "realloc failed", path);
                (void)saved;
                return -1;
            }
            buf = nb;
            cap = ncap;
        }
        size_t r = fread(buf + len, 1, cap - len, f);
        if (r == 0) break;
        len += r;
    }
    int ferr = ferror(f);
    int saved = errno;
    fclose(f);
    if (ferr) {
        free(buf);
        zenctl__set_err(err, zenctl__errno_to_code(saved),
                        "read error", path);
        return -1;
    }

    /* Add a trailing NUL (one byte past len) so callers that want to
     * treat the buffer as a C string can. The NUL is NOT counted in
     * *out_len. */
    uint8_t *nb = realloc(buf, len + 1);
    if (!nb) {
        /* Original buffer is still valid; just NUL-terminate in place
         * if there's room (there always is, since cap >= len). */
        buf[len] = '\0';
        *out = buf;
        *out_len = len;
        return 0;
    }
    buf = nb;
    buf[len] = '\0';
    *out = buf;
    *out_len = len;
    return 0;
}
