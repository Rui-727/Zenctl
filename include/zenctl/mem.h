/* mem.h - memory domain API
 *
 * Covers hugepages (legacy /proc/sys/vm plus per-size sysfs), THP,
 * NUMA topology, and the /proc/sys/vm tunables (swappiness,
 * overcommit, vfs_cache_pressure). Per docs/KERNEL_CPU_MEM.md.
 *
 * Memory quantities in the API are bytes unless noted; page counts
 * are pages. The kernel exposes some as kB and some as pages; this
 * layer converts at the boundary.
 */
#ifndef ZENCTL_MEM_H
#define ZENCTL_MEM_H

#include "zenctl.h"

/* ── Hugepages ───────────────────────────────────────────────────── */
/*
 * The "default size" hugepage pool is the size reported by
 * /proc/meminfo:Hugepagesize (commonly 2 MiB on x86-64). Per-size
 * access uses /sys/kernel/mm/hugepages/hugepages-<size>kB/.
 */

int zenctl_mem_get_nr_hugepages(int64_t *out, zenctl_err_t *err);
int zenctl_mem_set_nr_hugepages(int64_t pages, zenctl_err_t *err);

/* Default hugepage size in bytes (read from /proc/meminfo). */
int zenctl_mem_get_hugepage_size(int64_t *out_bytes, zenctl_err_t *err);

/* Per-size hugepages. `size_kb` is the page size in kB (e.g. 2048,
 * 1048576). The path /sys/kernel/mm/hugepages/hugepages-<size>kB/
 * must exist. */
int zenctl_mem_get_nr_hugepages_size(int64_t size_kb, int64_t *out,
                                     zenctl_err_t *err);
int zenctl_mem_set_nr_hugepages_size(int64_t size_kb, int64_t pages,
                                     zenctl_err_t *err);

/* ── Transparent Huge Pages ──────────────────────────────────────── */

typedef enum {
    ZENCTL_THP_ALWAYS  = 0,
    ZENCTL_THP_MADVISE = 1,
    ZENCTL_THP_NEVER   = 2,
} zenctl_thp_mode_t;

int zenctl_mem_get_thp(zenctl_thp_mode_t *out, zenctl_err_t *err);
int zenctl_mem_set_thp(zenctl_thp_mode_t mode, zenctl_err_t *err);

/* ── NUMA topology ───────────────────────────────────────────────── */
/*
 * On non-NUMA systems /sys/devices/system/node/ does not exist and
 * zenctl_mem_numa_node_count returns 0 with no error.
 *
 * `cpumask` is the kernel's CPU list format ("0-3,8-11" or "" on
 * memory-only nodes). `mem_total` / `mem_free` are in bytes.
 */

typedef struct {
    int      node_id;
    int64_t  mem_total;   /* bytes */
    int64_t  mem_free;    /* bytes */
    char     cpumask[256];/* e.g. "0-3,8-11" or "" */
} zenctl_numa_node_t;

int zenctl_mem_numa_node_count(int *out, zenctl_err_t *err);
int zenctl_mem_numa_get_node(int node_id, zenctl_numa_node_t *out,
                             zenctl_err_t *err);

/* ── VM tunables ─────────────────────────────────────────────────── */

int zenctl_mem_get_swappiness(int *out, zenctl_err_t *err);
int zenctl_mem_set_swappiness(int val, zenctl_err_t *err);  /* 0..200 */

int zenctl_mem_get_overcommit(int *out, zenctl_err_t *err);
int zenctl_mem_set_overcommit(int val, zenctl_err_t *err);  /* 0,1,2 */

int zenctl_mem_get_vfs_cache_pressure(int *out, zenctl_err_t *err);
int zenctl_mem_set_vfs_cache_pressure(int val, zenctl_err_t *err); /* >= 0 */

#endif /* ZENCTL_MEM_H */
