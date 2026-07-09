/* mem.c - memory domain implementation
 *
 * Implements the API declared in include/zenctl/mem.h against the
 * sysfs/procfs surface documented in docs/KERNEL_CPU_MEM.md.
 *
 * Unit conventions:
 *   - kernel kB files are converted to bytes at the API
 *   - kernel page counts stay as page counts (no conversion)
 *   - THP mode is an enum, not a string
 *   - NUMA cpumask is forwarded as the kernel's CPU list string
 */
#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <dirent.h>
#include <unistd.h>

#include "zenctl/internal.h"

/* ── Hugepages (default size, via /proc/sys/vm) ──────────────────── */

int zenctl_mem_get_nr_hugepages(int64_t *out, zenctl_err_t *err)
{
    if (!out) {
        zenctl__set_err(err, ZENCTL_ERR_EINVAL,
                        "NULL out", "zenctl_mem_get_nr_hugepages");
        return -1;
    }
    return zenctl__read_file_i64("/proc/sys/vm/nr_hugepages", out, err);
}

int zenctl_mem_set_nr_hugepages(int64_t pages, zenctl_err_t *err)
{
    if (pages < 0) {
        zenctl__set_err(err, ZENCTL_ERR_EINVAL,
                        "page count must be >= 0",
                        "zenctl_mem_set_nr_hugepages");
        return -1;
    }
    return zenctl__write_file_i64("/proc/sys/vm/nr_hugepages", pages, err);
}

int zenctl_mem_get_hugepage_size(int64_t *out_bytes, zenctl_err_t *err)
{
    if (!out_bytes) {
        zenctl__set_err(err, ZENCTL_ERR_EINVAL,
                        "NULL out_bytes", "zenctl_mem_get_hugepage_size");
        return -1;
    }

    FILE *f = fopen("/proc/meminfo", "r");
    if (!f) {
        zenctl__set_err(err, zenctl__errno_to_code(errno),
                        strerror(errno), "/proc/meminfo");
        return -1;
    }

    char line[256];
    int64_t kb = 0;
    while (fgets(line, sizeof(line), f)) {
        if (sscanf(line, "Hugepagesize: %ld kB", (long *)&kb) == 1)
            break;
    }
    fclose(f);

    if (kb == 0) {
        zenctl__set_err(err, ZENCTL_ERR_ENOENT,
                        "Hugepagesize not found in /proc/meminfo",
                        "/proc/meminfo");
        return -1;
    }
    *out_bytes = kb * 1024;
    return 0;
}

/* ── Per-size hugepages (via /sys/kernel/mm/hugepages/) ──────────── */

static int hugepages_path(int64_t size_kb, const char *attr,
                          char *buf, size_t bufsz, zenctl_err_t *err)
{
    if (size_kb <= 0) {
        zenctl__set_err(err, ZENCTL_ERR_EINVAL,
                        "size_kb must be > 0", "hugepages_path");
        return -1;
    }
    snprintf(buf, bufsz,
             "/sys/kernel/mm/hugepages/hugepages-%ldkB/%s",
             (long)size_kb, attr);
    return 0;
}

int zenctl_mem_get_nr_hugepages_size(int64_t size_kb, int64_t *out,
                                     zenctl_err_t *err)
{
    if (!out) {
        zenctl__set_err(err, ZENCTL_ERR_EINVAL,
                        "NULL out", "zenctl_mem_get_nr_hugepages_size");
        return -1;
    }
    char path[160];
    if (hugepages_path(size_kb, "nr_hugepages", path, sizeof(path), err) != 0)
        return -1;
    return zenctl__read_file_i64(path, out, err);
}

int zenctl_mem_set_nr_hugepages_size(int64_t size_kb, int64_t pages,
                                     zenctl_err_t *err)
{
    if (pages < 0) {
        zenctl__set_err(err, ZENCTL_ERR_EINVAL,
                        "page count must be >= 0",
                        "zenctl_mem_set_nr_hugepages_size");
        return -1;
    }
    char path[160];
    if (hugepages_path(size_kb, "nr_hugepages", path, sizeof(path), err) != 0)
        return -1;
    return zenctl__write_file_i64(path, pages, err);
}

/* ── Transparent Huge Pages ──────────────────────────────────────── */
/*
 * /sys/kernel/mm/transparent_hugepage/enabled
 * Read format: "[always] madvise never" (active mode in brackets).
 * Write: one of "always" / "madvise" / "never" (exact match).
 */

static const char *const THP_PATH =
    "/sys/kernel/mm/transparent_hugepage/enabled";

int zenctl_mem_get_thp(zenctl_thp_mode_t *out, zenctl_err_t *err)
{
    if (!out) {
        zenctl__set_err(err, ZENCTL_ERR_EINVAL,
                        "NULL out", "zenctl_mem_get_thp");
        return -1;
    }
    char buf[128];
    if (zenctl__read_file_string(THP_PATH, buf, sizeof(buf), err) != 0)
        return -1;

    char *b = strchr(buf, '[');
    char *e = strchr(buf, ']');
    if (!b || !e || e < b) {
        zenctl__set_err(err, ZENCTL_ERR_EIO,
                        "could not parse THP enabled (no bracketed mode)",
                        THP_PATH);
        return -1;
    }
    *e = '\0';
    const char *mode = b + 1;
    if (strcmp(mode, "always") == 0)       *out = ZENCTL_THP_ALWAYS;
    else if (strcmp(mode, "madvise") == 0) *out = ZENCTL_THP_MADVISE;
    else if (strcmp(mode, "never") == 0)   *out = ZENCTL_THP_NEVER;
    else {
        zenctl__set_err(err, ZENCTL_ERR_EINVAL,
                        "unknown THP mode in enabled file", mode);
        return -1;
    }
    return 0;
}

int zenctl_mem_set_thp(zenctl_thp_mode_t mode, zenctl_err_t *err)
{
    const char *s;
    switch (mode) {
    case ZENCTL_THP_ALWAYS:  s = "always";  break;
    case ZENCTL_THP_MADVISE: s = "madvise"; break;
    case ZENCTL_THP_NEVER:   s = "never";   break;
    default:
        zenctl__set_err(err, ZENCTL_ERR_EINVAL,
                        "invalid THP mode", "zenctl_mem_set_thp");
        return -1;
    }
    return zenctl__write_file_string(THP_PATH, s, err);
}

/* ── NUMA topology ───────────────────────────────────────────────── */

static const char *const NODE_DIR = "/sys/devices/system/node";

/* Returns 1 if name matches "node<N>", 0 otherwise. */
static int is_node_name(const char *name)
{
    if (strncmp(name, "node", 4) != 0) return 0;
    char *end = NULL;
    long n = strtol(name + 4, &end, 10);
    if (end == name + 4 || *end != '\0' || n < 0) return 0;
    return 1;
}

int zenctl_mem_numa_node_count(int *out, zenctl_err_t *err)
{
    if (!out) {
        zenctl__set_err(err, ZENCTL_ERR_EINVAL,
                        "NULL out", "zenctl_mem_numa_node_count");
        return -1;
    }
    *out = 0;

    DIR *d = opendir(NODE_DIR);
    if (!d) {
        if (errno == ENOENT) {
            /* Non-NUMA system. Not an error. */
            return 0;
        }
        zenctl__set_err(err, zenctl__errno_to_code(errno),
                        strerror(errno), NODE_DIR);
        return -1;
    }

    int n = 0;
    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        if (is_node_name(de->d_name)) n++;
    }
    closedir(d);
    *out = n;
    return 0;
}

int zenctl_mem_numa_get_node(int node_id, zenctl_numa_node_t *out,
                             zenctl_err_t *err)
{
    if (!out) {
        zenctl__set_err(err, ZENCTL_ERR_EINVAL,
                        "NULL out", "zenctl_mem_numa_get_node");
        return -1;
    }
    if (node_id < 0) {
        zenctl__set_err(err, ZENCTL_ERR_EINVAL,
                        "node_id must be >= 0", "zenctl_mem_numa_get_node");
        return -1;
    }

    char path[256];
    snprintf(path, sizeof(path), "%s/node%d", NODE_DIR, node_id);
    if (access(path, F_OK) != 0) {
        zenctl__set_err(err, zenctl__errno_to_code(errno),
                        "NUMA node directory does not exist", path);
        return -1;
    }

    memset(out, 0, sizeof(*out));
    out->node_id = node_id;

    /* Parse meminfo for MemTotal and MemFree (lines in kB). */
    snprintf(path, sizeof(path), "%s/node%d/meminfo", NODE_DIR, node_id);
    FILE *f = fopen(path, "r");
    if (!f) {
        zenctl__set_err(err, zenctl__errno_to_code(errno),
                        strerror(errno), path);
        return -1;
    }
    char line[512];
    while (fgets(line, sizeof(line), f)) {
        long v = 0; int n = 0;
        if (sscanf(line, "Node %d MemTotal: %ld kB", &n, &v) == 2)
            out->mem_total = (int64_t)v * 1024;
        else if (sscanf(line, "Node %d MemFree: %ld kB", &n, &v) == 2)
            out->mem_free = (int64_t)v * 1024;
    }
    fclose(f);

    /* Read cpulist. Memory-only nodes may have an empty (but present)
     * cpulist; the file itself is always present. A read failure here
     * is not fatal — leave cpumask empty and continue. */
    snprintf(path, sizeof(path), "%s/node%d/cpulist", NODE_DIR, node_id);
    zenctl_err_t tmp;
    if (zenctl__read_file_string(path, out->cpumask,
                                 sizeof(out->cpumask), &tmp) != 0) {
        out->cpumask[0] = '\0';
    }
    return 0;
}

/* ── VM tunables ─────────────────────────────────────────────────── */

int zenctl_mem_get_swappiness(int *out, zenctl_err_t *err)
{
    if (!out) {
        zenctl__set_err(err, ZENCTL_ERR_EINVAL,
                        "NULL out", "zenctl_mem_get_swappiness");
        return -1;
    }
    int64_t v;
    if (zenctl__read_file_i64("/proc/sys/vm/swappiness", &v, err) != 0)
        return -1;
    *out = (int)v;
    return 0;
}

int zenctl_mem_set_swappiness(int val, zenctl_err_t *err)
{
    if (val < 0 || val > 200) {
        zenctl__set_err(err, ZENCTL_ERR_ERANGE,
                        "swappiness must be 0..200",
                        "zenctl_mem_set_swappiness");
        return -1;
    }
    return zenctl__write_file_i64("/proc/sys/vm/swappiness", (int64_t)val, err);
}

int zenctl_mem_get_overcommit(int *out, zenctl_err_t *err)
{
    if (!out) {
        zenctl__set_err(err, ZENCTL_ERR_EINVAL,
                        "NULL out", "zenctl_mem_get_overcommit");
        return -1;
    }
    int64_t v;
    if (zenctl__read_file_i64("/proc/sys/vm/overcommit_memory", &v, err) != 0)
        return -1;
    *out = (int)v;
    return 0;
}

int zenctl_mem_set_overcommit(int val, zenctl_err_t *err)
{
    if (val < 0 || val > 2) {
        zenctl__set_err(err, ZENCTL_ERR_EINVAL,
                        "overcommit must be 0, 1, or 2",
                        "zenctl_mem_set_overcommit");
        return -1;
    }
    return zenctl__write_file_i64("/proc/sys/vm/overcommit_memory",
                                  (int64_t)val, err);
}

int zenctl_mem_get_vfs_cache_pressure(int *out, zenctl_err_t *err)
{
    if (!out) {
        zenctl__set_err(err, ZENCTL_ERR_EINVAL,
                        "NULL out", "zenctl_mem_get_vfs_cache_pressure");
        return -1;
    }
    int64_t v;
    if (zenctl__read_file_i64("/proc/sys/vm/vfs_cache_pressure", &v, err) != 0)
        return -1;
    *out = (int)v;
    return 0;
}

int zenctl_mem_set_vfs_cache_pressure(int val, zenctl_err_t *err)
{
    if (val < 0) {
        zenctl__set_err(err, ZENCTL_ERR_EINVAL,
                        "vfs_cache_pressure must be >= 0",
                        "zenctl_mem_set_vfs_cache_pressure");
        return -1;
    }
    return zenctl__write_file_i64("/proc/sys/vm/vfs_cache_pressure",
                                  (int64_t)val, err);
}
