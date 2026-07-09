/* io.c - shared sysfs/procfs read/write helpers for libzenctl
 *
 * Every domain implementation goes through these. They enforce:
 *  - trailing-newline stripping on reads
 *  - no-whitespace writes (kernel string parsers reject leading/trailing
 *    spaces; trailing newlines are tolerated but not required, so we do
 *    not add one)
 *  - consistent errno -> zenctl_err_t mapping
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#include "zenctl/internal.h"

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

    FILE *f = fopen(path, "r");
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

    FILE *f = fopen(path, "w");
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
