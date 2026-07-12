/* core.c - libzenctl core: context, error, key-value API
 *
 * The context holds capability state and the backend router. The
 * key-value API dispatches to domain backends based on the key prefix.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>

#include "zenctl/zenctl.h"
#include "zenctl/internal.h"

/* ── Version ─────────────────────────────────────────────────────── */

/* The Makefile passes -DZENCTL_VERSION_MAJOR/MINOR/PATCH=N. Provide
 * fallbacks so the library still builds when those are absent (e.g.
 * when core.c is compiled standalone for a smoke test). */
#ifndef ZENCTL_VERSION_MAJOR
#define ZENCTL_VERSION_MAJOR 0
#endif
#ifndef ZENCTL_VERSION_MINOR
#define ZENCTL_VERSION_MINOR 1
#endif
#ifndef ZENCTL_VERSION_PATCH
#define ZENCTL_VERSION_PATCH 0
#endif

const char *zenctl_version(void)
{
    static char v[32];
    if (v[0] == '\0') {
        snprintf(v, sizeof(v), "%d.%d.%d",
                 ZENCTL_VERSION_MAJOR, ZENCTL_VERSION_MINOR,
                 ZENCTL_VERSION_PATCH);
    }
    return v;
}

/* ── Error strings ───────────────────────────────────────────────── */

const char *zenctl_strerror(int code)
{
    switch (code) {
    case ZENCTL_OK:           return "ok";
    case ZENCTL_ERR_EPERM:    return "permission denied";
    case ZENCTL_ERR_ENOENT:   return "not found";
    case ZENCTL_ERR_EINVAL:   return "invalid argument";
    case ZENCTL_ERR_EIO:      return "I/O error";
    case ZENCTL_ERR_ENOTSUP:  return "not supported";
    case ZENCTL_ERR_ERANGE:   return "out of range";
    case ZENCTL_ERR_NOMEM:    return "out of memory";
    case ZENCTL_ERR_INTERNAL: return "internal error";
    default:                  return "unknown error";
    }
}

/* ── Context ─────────────────────────────────────────────────────── */

struct zenctl_ctx {
    int cap_cache_init;  /* set to 1 after first capability probe */
};

zenctl_ctx_t *zenctl_ctx_new(zenctl_err_t *err)
{
    (void)err;
    zenctl_ctx_t *ctx = calloc(1, sizeof(*ctx));
    return ctx;
}

void zenctl_ctx_free(zenctl_ctx_t *ctx)
{
    free(ctx);
}

/* ── Generic key-value API ───────────────────────────────────────── */

void zenctl_val_free(zenctl_val_t *val)
{
    if (!val) return;
    if (val->type == ZENCTL_VAL_STRING && val->v.s)
        free(val->v.s);
    memset(val, 0, sizeof(*val));
}

/* Validate a generic KV key. Rules:
 *  - non-NULL, non-empty
 *  - shorter than ZENCTL_KV_KEY_MAX (4096)
 *  - contains at least one '.' separating the domain prefix from the
 *    rest (e.g. "mem.swappiness" or "cpu.0.governor")
 * Returns 1 if valid, 0 otherwise. */
#define ZENCTL_KV_KEY_MAX 4096
static int valid_kv_key(const char *key)
{
    if (!key || !*key) return 0;
    size_t n = strlen(key);
    if (n >= ZENCTL_KV_KEY_MAX) return 0;
    if (strchr(key, '.') == NULL) return 0;
    return 1;
}

int zenctl_get(zenctl_ctx_t *ctx, const char *key,
               zenctl_val_t *out, zenctl_err_t *err)
{
    (void)ctx;
    if (!valid_kv_key(key)) {
        zenctl__set_err(err, ZENCTL_ERR_EINVAL,
                        "invalid key (NULL, empty, too long, "
                        "or missing domain prefix)",
                        "zenctl_get");
        return -1;
    }
    if (!out) {
        zenctl__set_err(err, ZENCTL_ERR_EINVAL,
                        "NULL out", "zenctl_get");
        return -1;
    }
    /* TODO: dispatch to domain backends based on the key prefix.
     * For now every valid-but-unknown key returns ENOTSUP so callers
     * can distinguish "bad key" from "not implemented". */
    if (err) {
        err->code = ZENCTL_ERR_ENOTSUP;
        snprintf(err->message, sizeof(err->message), "key not implemented");
        snprintf(err->context, sizeof(err->context), "zenctl_get(%s)", key);
        err->recoverable = true;
    }
    return -1;
}

int zenctl_set(zenctl_ctx_t *ctx, const char *key,
               const char *value, zenctl_err_t *err)
{
    (void)ctx;
    if (!valid_kv_key(key)) {
        zenctl__set_err(err, ZENCTL_ERR_EINVAL,
                        "invalid key (NULL, empty, too long, "
                        "or missing domain prefix)",
                        "zenctl_set");
        return -1;
    }
    if (!value) {
        zenctl__set_err(err, ZENCTL_ERR_EINVAL,
                        "NULL value", "zenctl_set");
        return -1;
    }
    if (err) {
        err->code = ZENCTL_ERR_ENOTSUP;
        snprintf(err->message, sizeof(err->message), "key not implemented");
        snprintf(err->context, sizeof(err->context), "zenctl_set(%s)", key);
        err->recoverable = true;
    }
    return -1;
}

/* ── Key iteration ───────────────────────────────────────────────── */

struct zenctl_key_iter {
    int done;
};

zenctl_key_iter_t *zenctl_keys(zenctl_ctx_t *ctx, const char *domain_prefix,
                               zenctl_err_t *err)
{
    (void)ctx; (void)domain_prefix;
    if (err) {
        err->code = ZENCTL_ERR_ENOTSUP;
        snprintf(err->message, sizeof(err->message), "key iteration not implemented");
        err->recoverable = true;
    }
    return NULL;
}

int zenctl_key_iter_next(zenctl_key_iter_t *it, char **key_out)
{
    (void)it; (void)key_out;
    return 0;
}

void zenctl_key_iter_free(zenctl_key_iter_t *it)
{
    free(it);
}

/* ── Capability query ────────────────────────────────────────────── */
/*
 * Maps a known KV key to its backing sysfs/procfs path. Returns NULL
 * for keys that are not in the table (the caller treats that as
 * ZENCTL_CAP_UNAVAILABLE). The table is deliberately small: it covers
 * the keys the unit-test capability suite exercises plus a handful of
 * obvious ones. Domain backends remain the source of truth for full
 * enumeration; this is a quick "does the surface exist?" probe.
 *
 * Note: returns a pointer to a static buffer; not thread-safe. The
 * capability query is a one-shot lookup, not a held reference.
 */
static const char *key_to_path(const char *key)
{
    static char path[256];

    if (strncmp(key, "cpu.", 4) == 0) {
        int idx;
        char attr[64];
        if (sscanf(key, "cpu.%d.%63s", &idx, attr) != 2) return NULL;
        if (idx < 0) return NULL;
        if (strcmp(attr, "governor") == 0)
            snprintf(path, sizeof(path),
                     "/sys/devices/system/cpu/cpu%d/cpufreq/scaling_governor", idx);
        else if (strcmp(attr, "freq_min") == 0)
            snprintf(path, sizeof(path),
                     "/sys/devices/system/cpu/cpu%d/cpufreq/scaling_min_freq", idx);
        else if (strcmp(attr, "freq_max") == 0)
            snprintf(path, sizeof(path),
                     "/sys/devices/system/cpu/cpu%d/cpufreq/scaling_max_freq", idx);
        else if (strcmp(attr, "online") == 0)
            snprintf(path, sizeof(path),
                     "/sys/devices/system/cpu/cpu%d/online", idx);
        else
            return NULL;
        return path;
    }

    if (strcmp(key, "mem.swappiness") == 0)
        return "/proc/sys/vm/swappiness";
    if (strcmp(key, "mem.nr_hugepages") == 0)
        return "/proc/sys/vm/nr_hugepages";
    if (strcmp(key, "mem.overcommit") == 0)
        return "/proc/sys/vm/overcommit_memory";
    if (strcmp(key, "mem.vfs_cache_pressure") == 0)
        return "/proc/sys/vm/vfs_cache_pressure";
    if (strcmp(key, "mem.thp") == 0)
        return "/sys/kernel/mm/transparent_hugepage/enabled";

    return NULL;
}

zenctl_cap_t zenctl_query_cap(zenctl_ctx_t *ctx, const char *key)
{
    (void)ctx;
    if (!key || !*key) return ZENCTL_CAP_UNAVAILABLE;

    const char *path = key_to_path(key);
    if (!path) return ZENCTL_CAP_UNAVAILABLE;

    /* Existence check first: a missing file is always UNAVAILABLE
     * regardless of permissions. */
    if (access(path, F_OK) != 0) return ZENCTL_CAP_UNAVAILABLE;

    /* If the file exists but is not writable, surface READONLY. Note
     * that root bypasses DAC, so this only reports READONLY when the
     * caller actually lacks write permission. */
    if (access(path, W_OK) != 0) return ZENCTL_CAP_READONLY;

    return ZENCTL_CAP_AVAILABLE;
}
