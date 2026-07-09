/* storage.h - storage domain API
 *
 * Block-device controls: I/O scheduler, queue depth, read-ahead,
 * write cache, TRIM limits, rotational flag, I/O stats, runtime PM.
 * Backed by /sys/block/<dev>/queue/, /sys/block/<dev>/device/,
 * /sys/block/<dev>/stat, and /sys/block/<dev>/device/power/control.
 */
#ifndef ZENCTL_STORAGE_H
#define ZENCTL_STORAGE_H

#include "zenctl.h"

typedef struct zenctl_storage zenctl_storage_t;

/* Open by block device name (e.g. "sda", "nvme0n1", "dm-0").
 * Returns NULL on error and fills *err. */
zenctl_storage_t *zenctl_storage_open(const char *dev_name, zenctl_err_t *err);
void              zenctl_storage_close(zenctl_storage_t *st);

/* I/O scheduler.
 * - get_scheduler:   returns the active scheduler name (no brackets).
 * - set_scheduler:   writes one of the listed names.
 * - list_schedulers: returns the raw /queue/scheduler string
 *   (space-separated list with the active entry in [brackets]). */
int zenctl_storage_get_scheduler(zenctl_storage_t *st, char **out, zenctl_err_t *err);
int zenctl_storage_set_scheduler(zenctl_storage_t *st, const char *sched, zenctl_err_t *err);
int zenctl_storage_list_schedulers(zenctl_storage_t *st, char **out, zenctl_err_t *err);

/* Queue depth. Backed by /sys/block/<dev>/device/queue_depth
 * (SCSI hardware queue depth, RW; NVMe multipath QD policy, RO). */
int zenctl_storage_get_queue_depth(zenctl_storage_t *st, int *out, zenctl_err_t *err);
int zenctl_storage_set_queue_depth(zenctl_storage_t *st, int depth, zenctl_err_t *err);

/* Read-ahead in kB. /sys/block/<dev>/queue/read_ahead_kb. */
int zenctl_storage_get_read_ahead(zenctl_storage_t *st, int64_t *out_kb, zenctl_err_t *err);
int zenctl_storage_set_read_ahead(zenctl_storage_t *st, int64_t kb, zenctl_err_t *err);

/* Write cache. Backed by /sys/block/<dev>/device/cache_type (SCSI)
 * or /sys/block/<dev>/queue/write_cache (NVMe, virtio). */
typedef enum {
    ZENCTL_CACHE_WRITE_BACK = 0,
    ZENCTL_CACHE_WRITE_THROUGH = 1,
    ZENCTL_CACHE_NONE = 2,
    ZENCTL_CACHE_WRITE_BACK_FUA = 3,
} zenctl_cache_type_t;

int zenctl_storage_get_cache_type(zenctl_storage_t *st, zenctl_cache_type_t *out, zenctl_err_t *err);
int zenctl_storage_set_cache_type(zenctl_storage_t *st, zenctl_cache_type_t type, zenctl_err_t *err);

/* TRIM/discard max bytes per request. /sys/block/<dev>/queue/discard_max_bytes. */
int zenctl_storage_get_discard_max(zenctl_storage_t *st, int64_t *out_bytes, zenctl_err_t *err);

/* Rotational flag. /sys/block/<dev>/queue/rotational. */
int zenctl_storage_get_rotational(zenctl_storage_t *st, bool *out, zenctl_err_t *err);

/* I/O stats — the 17 fields from /sys/block/<dev>/stat. */
typedef struct {
    int64_t reads_completed;
    int64_t reads_merged;
    int64_t sectors_read;
    int64_t time_reading_ms;
    int64_t writes_completed;
    int64_t writes_merged;
    int64_t sectors_written;
    int64_t time_writing_ms;
    int64_t ios_in_progress;
    int64_t time_io_ms;
    int64_t weighted_time_io_ms;
    int64_t discards_completed;
    int64_t discards_merged;
    int64_t sectors_discarded;
    int64_t time_discarding_ms;
    int64_t flushes_completed;
    int64_t time_flushing_ms;
} zenctl_io_stats_t;

int zenctl_storage_get_io_stats(zenctl_storage_t *st, zenctl_io_stats_t *out, zenctl_err_t *err);

/* Runtime PM. /sys/block/<dev>/device/power/control. */
int zenctl_storage_get_power_control(zenctl_storage_t *st, char **out, zenctl_err_t *err);
int zenctl_storage_set_power_control(zenctl_storage_t *st, const char *mode, zenctl_err_t *err);

#endif /* ZENCTL_STORAGE_H */
