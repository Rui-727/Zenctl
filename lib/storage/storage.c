/* storage.c - storage domain implementation
 *
 * Reads and writes the block-layer sysfs tree under
 * /sys/block/<dev>/{queue,device,stat,device/power}.
 *
 * Path safety: the device name is validated to reject path
 * traversal and over-long names so it cannot escape the
 * /sys/block/<prefix>.
 *
 * String writes are sent verbatim with no trailing newline (the
 * kernel's sysfs parsers tolerate but do not require newlines, and
 * some reject leading/trailing whitespace). Numeric writes use the
 * shared zenctl__write_file_i64 helper.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <ctype.h>
#include <unistd.h>
#include <limits.h>

#include "zenctl/internal.h"
#include "zenctl/storage.h"

/* ── Internal helpers ────────────────────────────────────────────── */

#define ZENCTL_SYS_BLOCK  "/sys/block"
#define ZENCTL_PATH_MAX   512

struct zenctl_storage {
    char dev_name[256];
};

static int storage_validate_dev(const char *dev_name, zenctl_err_t *err)
{
    size_t n;
    if (!dev_name || !*dev_name) {
        zenctl__set_err(err, ZENCTL_ERR_EINVAL,
                        "device name is required", "zenctl_storage_open");
        return -1;
    }
    n = strlen(dev_name);
    if (n >= 256) {
        zenctl__set_err(err, ZENCTL_ERR_EINVAL,
                        "device name too long", "zenctl_storage_open");
        return -1;
    }
    if (strchr(dev_name, '/') || strstr(dev_name, "..") ||
        strchr(dev_name, '\n')) {
        zenctl__set_err(err, ZENCTL_ERR_EINVAL,
                        "device name has invalid characters",
                        "zenctl_storage_open");
        return -1;
    }
    return 0;
}

static void storage_path(char *buf, size_t bufsz,
                         const zenctl_storage_t *st, const char *suffix)
{
    snprintf(buf, bufsz, "%s/%s%s", ZENCTL_SYS_BLOCK, st->dev_name, suffix);
}

/* Read a sysfs string into a caller-provided buffer. The shared helper
 * already strips a trailing newline. */
static int storage_read_str(const char *path, char *buf, size_t bufsz,
                            zenctl_err_t *err)
{
    return zenctl__read_file_string(path, buf, bufsz, err);
}

static int storage_read_i64(const char *path, int64_t *out, zenctl_err_t *err)
{
    return zenctl__read_file_i64(path, out, err);
}

static int storage_write_str(const char *path, const char *value,
                             zenctl_err_t *err)
{
    return zenctl__write_file_string(path, value, err);
}

static int storage_write_i64(const char *path, int64_t v, zenctl_err_t *err)
{
    return zenctl__write_file_i64(path, v, err);
}

/* ── Lifecycle ───────────────────────────────────────────────────── */

zenctl_storage_t *zenctl_storage_open(const char *dev_name, zenctl_err_t *err)
{
    char path[ZENCTL_PATH_MAX];
    zenctl_storage_t *st;

    if (storage_validate_dev(dev_name, err) != 0)
        return NULL;

    snprintf(path, sizeof(path), "%s/%s", ZENCTL_SYS_BLOCK, dev_name);
    if (access(path, F_OK) != 0) {
        zenctl__set_err(err, zenctl__errno_to_code(errno),
                        "block device not found", path);
        return NULL;
    }

    st = calloc(1, sizeof(*st));
    if (!st) {
        zenctl__set_err(err, ZENCTL_ERR_NOMEM, "calloc failed",
                        "zenctl_storage_open");
        return NULL;
    }
    snprintf(st->dev_name, sizeof(st->dev_name), "%s", dev_name);
    return st;
}

void zenctl_storage_close(zenctl_storage_t *st)
{
    free(st);
}

/* ── I/O scheduler ───────────────────────────────────────────────── */

int zenctl_storage_get_scheduler(zenctl_storage_t *st, char **out,
                                 zenctl_err_t *err)
{
    char path[ZENCTL_PATH_MAX];
    char buf[256];
    char *start, *end;

    if (!st || !out) {
        zenctl__set_err(err, ZENCTL_ERR_EINVAL, "NULL st or out",
                        "zenctl_storage_get_scheduler");
        return -1;
    }
    *out = NULL;
    storage_path(path, sizeof(path), st, "/queue/scheduler");
    if (storage_read_str(path, buf, sizeof(buf), err) < 0)
        return -1;

    start = strchr(buf, '[');
    if (!start) {
        zenctl__set_err(err, ZENCTL_ERR_EIO,
                        "no active scheduler in sysfs output", path);
        return -1;
    }
    start++;
    end = strchr(start, ']');
    if (!end) {
        zenctl__set_err(err, ZENCTL_ERR_EIO,
                        "malformed scheduler sysfs output", path);
        return -1;
    }
    *end = '\0';
    *out = strdup(start);
    if (!*out) {
        zenctl__set_err(err, ZENCTL_ERR_NOMEM, "strdup failed", path);
        return -1;
    }
    return 0;
}

int zenctl_storage_set_scheduler(zenctl_storage_t *st, const char *sched,
                                 zenctl_err_t *err)
{
    char path[ZENCTL_PATH_MAX];

    if (!st || !sched || !*sched) {
        zenctl__set_err(err, ZENCTL_ERR_EINVAL, "NULL st or sched",
                        "zenctl_storage_set_scheduler");
        return -1;
    }
    if (strchr(sched, '/') || strchr(sched, '\n') || strchr(sched, ' ') ||
        strlen(sched) >= 128) {
        zenctl__set_err(err, ZENCTL_ERR_EINVAL,
                        "invalid scheduler name",
                        "zenctl_storage_set_scheduler");
        return -1;
    }
    storage_path(path, sizeof(path), st, "/queue/scheduler");
    return storage_write_str(path, sched, err);
}

int zenctl_storage_list_schedulers(zenctl_storage_t *st, char **out,
                                   zenctl_err_t *err)
{
    char path[ZENCTL_PATH_MAX];
    char buf[256];

    if (!st || !out) {
        zenctl__set_err(err, ZENCTL_ERR_EINVAL, "NULL st or out",
                        "zenctl_storage_list_schedulers");
        return -1;
    }
    *out = NULL;
    storage_path(path, sizeof(path), st, "/queue/scheduler");
    if (storage_read_str(path, buf, sizeof(buf), err) < 0)
        return -1;
    *out = strdup(buf);
    if (!*out) {
        zenctl__set_err(err, ZENCTL_ERR_NOMEM, "strdup failed", path);
        return -1;
    }
    return 0;
}

/* ── Queue depth ─────────────────────────────────────────────────── */

int zenctl_storage_get_queue_depth(zenctl_storage_t *st, int *out,
                                   zenctl_err_t *err)
{
    char path[ZENCTL_PATH_MAX];
    int64_t v;

    if (!st || !out) {
        zenctl__set_err(err, ZENCTL_ERR_EINVAL, "NULL st or out",
                        "zenctl_storage_get_queue_depth");
        return -1;
    }
    storage_path(path, sizeof(path), st, "/device/queue_depth");
    if (storage_read_i64(path, &v, err) < 0)
        return -1;
    if (v < 0 || v > INT_MAX) {
        zenctl__set_err(err, ZENCTL_ERR_ERANGE,
                        "queue_depth out of range", path);
        return -1;
    }
    *out = (int)v;
    return 0;
}

int zenctl_storage_set_queue_depth(zenctl_storage_t *st, int depth,
                                   zenctl_err_t *err)
{
    char path[ZENCTL_PATH_MAX];

    if (!st) {
        zenctl__set_err(err, ZENCTL_ERR_EINVAL, "NULL st",
                        "zenctl_storage_set_queue_depth");
        return -1;
    }
    if (depth < 1) {
        zenctl__set_err(err, ZENCTL_ERR_EINVAL,
                        "queue depth must be >= 1",
                        "zenctl_storage_set_queue_depth");
        return -1;
    }
    storage_path(path, sizeof(path), st, "/device/queue_depth");
    return storage_write_i64(path, (int64_t)depth, err);
}

/* ── Read-ahead ──────────────────────────────────────────────────── */

int zenctl_storage_get_read_ahead(zenctl_storage_t *st, int64_t *out_kb,
                                  zenctl_err_t *err)
{
    char path[ZENCTL_PATH_MAX];

    if (!st || !out_kb) {
        zenctl__set_err(err, ZENCTL_ERR_EINVAL, "NULL st or out_kb",
                        "zenctl_storage_get_read_ahead");
        return -1;
    }
    storage_path(path, sizeof(path), st, "/queue/read_ahead_kb");
    return storage_read_i64(path, out_kb, err);
}

int zenctl_storage_set_read_ahead(zenctl_storage_t *st, int64_t kb,
                                  zenctl_err_t *err)
{
    char path[ZENCTL_PATH_MAX];

    if (!st) {
        zenctl__set_err(err, ZENCTL_ERR_EINVAL, "NULL st",
                        "zenctl_storage_set_read_ahead");
        return -1;
    }
    if (kb < 0) {
        zenctl__set_err(err, ZENCTL_ERR_EINVAL,
                        "read-ahead must be >= 0",
                        "zenctl_storage_set_read_ahead");
        return -1;
    }
    storage_path(path, sizeof(path), st, "/queue/read_ahead_kb");
    return storage_write_i64(path, kb, err);
}

/* ── Write cache ─────────────────────────────────────────────────── */

static const char *cache_type_to_str(zenctl_cache_type_t t)
{
    switch (t) {
    case ZENCTL_CACHE_WRITE_BACK:      return "write back";
    case ZENCTL_CACHE_WRITE_THROUGH:   return "write through";
    case ZENCTL_CACHE_NONE:            return "none";
    /* FUA is a per-command flag, not a separate cache mode in sysfs.
     * Map to "write back" so the setter actually does something. */
    case ZENCTL_CACHE_WRITE_BACK_FUA:  return "write back";
    default:                           return NULL;
    }
}

static int cache_str_to_type(const char *s, zenctl_cache_type_t *out)
{
    /* SCSI sd cache_type strings */
    if (strcmp(s, "write back") == 0)             { *out = ZENCTL_CACHE_WRITE_BACK; return 0; }
    if (strcmp(s, "write through") == 0)          { *out = ZENCTL_CACHE_WRITE_THROUGH; return 0; }
    if (strcmp(s, "none") == 0)                   { *out = ZENCTL_CACHE_NONE; return 0; }
    if (strcmp(s, "write back, no read (daft)") == 0) {
        /* Broken mode (WCE=1 RCD=1). Surface as plain write back. */
        *out = ZENCTL_CACHE_WRITE_BACK;
        return 0;
    }
    return -1;
}

int zenctl_storage_get_cache_type(zenctl_storage_t *st,
                                  zenctl_cache_type_t *out, zenctl_err_t *err)
{
    char path[ZENCTL_PATH_MAX];
    char buf[64];
    zenctl_cache_type_t t;

    if (!st || !out) {
        zenctl__set_err(err, ZENCTL_ERR_EINVAL, "NULL st or out",
                        "zenctl_storage_get_cache_type");
        return -1;
    }

    /* SCSI sd: /device/cache_type. NVMe / virtio: /queue/write_cache. */
    storage_path(path, sizeof(path), st, "/device/cache_type");
    if (storage_read_str(path, buf, sizeof(buf), NULL) >= 0) {
        if (cache_str_to_type(buf, &t) == 0) {
            *out = t;
            return 0;
        }
        zenctl__set_err(err, ZENCTL_ERR_EIO,
                        "unrecognised cache_type string", path);
        return -1;
    }

    storage_path(path, sizeof(path), st, "/queue/write_cache");
    if (storage_read_str(path, buf, sizeof(buf), err) < 0)
        return -1;
    if (cache_str_to_type(buf, &t) == 0) {
        *out = t;
        return 0;
    }
    zenctl__set_err(err, ZENCTL_ERR_EIO,
                    "unrecognised write_cache string", path);
    return -1;
}

int zenctl_storage_set_cache_type(zenctl_storage_t *st,
                                  zenctl_cache_type_t type, zenctl_err_t *err)
{
    char path[ZENCTL_PATH_MAX];
    const char *s;

    if (!st) {
        zenctl__set_err(err, ZENCTL_ERR_EINVAL, "NULL st",
                        "zenctl_storage_set_cache_type");
        return -1;
    }
    s = cache_type_to_str(type);
    if (!s) {
        zenctl__set_err(err, ZENCTL_ERR_EINVAL,
                        "invalid cache type enum",
                        "zenctl_storage_set_cache_type");
        return -1;
    }

    /* Try SCSI first, then NVMe/virtio. */
    storage_path(path, sizeof(path), st, "/device/cache_type");
    if (storage_write_str(path, s, NULL) == 0)
        return 0;

    storage_path(path, sizeof(path), st, "/queue/write_cache");
    return storage_write_str(path, s, err);
}

/* ── Discard max ─────────────────────────────────────────────────── */

int zenctl_storage_get_discard_max(zenctl_storage_t *st,
                                   int64_t *out_bytes, zenctl_err_t *err)
{
    char path[ZENCTL_PATH_MAX];

    if (!st || !out_bytes) {
        zenctl__set_err(err, ZENCTL_ERR_EINVAL, "NULL st or out_bytes",
                        "zenctl_storage_get_discard_max");
        return -1;
    }
    storage_path(path, sizeof(path), st, "/queue/discard_max_bytes");
    return storage_read_i64(path, out_bytes, err);
}

/* ── Rotational ──────────────────────────────────────────────────── */

int zenctl_storage_get_rotational(zenctl_storage_t *st, bool *out,
                                  zenctl_err_t *err)
{
    char path[ZENCTL_PATH_MAX];
    int64_t v;

    if (!st || !out) {
        zenctl__set_err(err, ZENCTL_ERR_EINVAL, "NULL st or out",
                        "zenctl_storage_get_rotational");
        return -1;
    }
    storage_path(path, sizeof(path), st, "/queue/rotational");
    if (storage_read_i64(path, &v, err) < 0)
        return -1;
    *out = (v != 0);
    return 0;
}

/* ── I/O stats ───────────────────────────────────────────────────── */

int zenctl_storage_get_io_stats(zenctl_storage_t *st,
                                zenctl_io_stats_t *out, zenctl_err_t *err)
{
    char path[ZENCTL_PATH_MAX];
    char buf[1024];
    long long v[17];
    int n;

    if (!st || !out) {
        zenctl__set_err(err, ZENCTL_ERR_EINVAL, "NULL st or out",
                        "zenctl_storage_get_io_stats");
        return -1;
    }
    storage_path(path, sizeof(path), st, "/stat");
    /* Read the raw file directly — zenctl__read_file_string truncates
     * at 64 bytes which is too small for the 17-field stat line, and
     * we want to preserve any interior whitespace for sscanf. */
    {
        FILE *fp = fopen(path, "r");
        if (!fp) {
            zenctl__set_err(err, zenctl__errno_to_code(errno),
                            strerror(errno), path);
            return -1;
        }
        if (!fgets(buf, sizeof(buf), fp)) {
            int e = errno;
            fclose(fp);
            zenctl__set_err(err, zenctl__errno_to_code(e),
                            "cannot read stat file", path);
            return -1;
        }
        fclose(fp);
    }

    n = sscanf(buf,
        "%lld %lld %lld %lld %lld %lld %lld %lld %lld %lld %lld "
        "%lld %lld %lld %lld %lld %lld",
        &v[0], &v[1], &v[2], &v[3], &v[4], &v[5], &v[6], &v[7],
        &v[8], &v[9], &v[10], &v[11], &v[12], &v[13], &v[14],
        &v[15], &v[16]);
    if (n < 11) {
        zenctl__set_err(err, ZENCTL_ERR_EIO,
                        "cannot parse /sys/block/.../stat", path);
        return -1;
    }
    /* Older kernels have only 11 fields. Mid-era ones have 15 (no
     * flush_*). Modern ones have 17. Zero whatever is missing so the
     * caller gets a consistent snapshot. */
    for (int i = n; i < 17; i++)
        v[i] = 0;

    out->reads_completed      = v[0];
    out->reads_merged         = v[1];
    out->sectors_read         = v[2];
    out->time_reading_ms      = v[3];
    out->writes_completed     = v[4];
    out->writes_merged        = v[5];
    out->sectors_written      = v[6];
    out->time_writing_ms      = v[7];
    out->ios_in_progress      = v[8];
    out->time_io_ms           = v[9];
    out->weighted_time_io_ms  = v[10];
    out->discards_completed   = v[11];
    out->discards_merged      = v[12];
    out->sectors_discarded    = v[13];
    out->time_discarding_ms   = v[14];
    out->flushes_completed    = v[15];
    out->time_flushing_ms     = v[16];
    return 0;
}

/* ── Runtime PM ──────────────────────────────────────────────────── */

int zenctl_storage_get_power_control(zenctl_storage_t *st, char **out,
                                     zenctl_err_t *err)
{
    char path[ZENCTL_PATH_MAX];
    char buf[64];

    if (!st || !out) {
        zenctl__set_err(err, ZENCTL_ERR_EINVAL, "NULL st or out",
                        "zenctl_storage_get_power_control");
        return -1;
    }
    *out = NULL;
    storage_path(path, sizeof(path), st, "/device/power/control");
    if (storage_read_str(path, buf, sizeof(buf), err) < 0)
        return -1;
    *out = strdup(buf);
    if (!*out) {
        zenctl__set_err(err, ZENCTL_ERR_NOMEM, "strdup failed", path);
        return -1;
    }
    return 0;
}

int zenctl_storage_set_power_control(zenctl_storage_t *st, const char *mode,
                                     zenctl_err_t *err)
{
    char path[ZENCTL_PATH_MAX];

    if (!st || !mode || !*mode) {
        zenctl__set_err(err, ZENCTL_ERR_EINVAL, "NULL st or mode",
                        "zenctl_storage_set_power_control");
        return -1;
    }
    if (strcmp(mode, "on") != 0 && strcmp(mode, "auto") != 0) {
        zenctl__set_err(err, ZENCTL_ERR_EINVAL,
                        "mode must be 'on' or 'auto'",
                        "zenctl_storage_set_power_control");
        return -1;
    }
    storage_path(path, sizeof(path), st, "/device/power/control");
    return storage_write_str(path, mode, err);
}
