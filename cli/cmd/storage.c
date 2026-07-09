/* cmd_storage.c - storage domain commands
 *
 * Subcommands:
 *   get  scheduler | queue-depth | read-ahead | cache-type | stats | power
 *   set  scheduler | read-ahead | cache-type | power
 *   list
 *
 * The library does not yet expose block-device enumeration; `list`
 * scans /sys/block/ directly (read-only directory listing).
 */
#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>

#include "zenctl/zenctl.h"
#include "zenctl/storage.h"
#include "output.h"
#include "cmd_util.h"

/* ── helpers ─────────────────────────────────────────────────────── */

static int get_dev_arg(int argc, char **argv, const char **out_dev)
{
    return cmd_opt_str(argc, argv, "--dev", out_dev);
}

static const char *cache_type_str(zenctl_cache_type_t t)
{
    switch (t) {
    case ZENCTL_CACHE_WRITE_BACK:      return "write back";
    case ZENCTL_CACHE_WRITE_THROUGH:   return "write through";
    case ZENCTL_CACHE_NONE:            return "none";
    case ZENCTL_CACHE_WRITE_BACK_FUA:  return "write back (fua)";
    default:                           return "unknown";
    }
}

static int cache_type_parse(const char *s, zenctl_cache_type_t *out)
{
    if (strcmp(s, "write_back")     == 0 ||
        strcmp(s, "write-back")     == 0 ||
        strcmp(s, "write back")     == 0) { *out = ZENCTL_CACHE_WRITE_BACK; return 0; }
    if (strcmp(s, "write_through")  == 0 ||
        strcmp(s, "write-through")  == 0 ||
        strcmp(s, "write through")  == 0) { *out = ZENCTL_CACHE_WRITE_THROUGH; return 0; }
    if (strcmp(s, "none")           == 0) { *out = ZENCTL_CACHE_NONE; return 0; }
    return -1;
}

/* ── get scheduler ───────────────────────────────────────────────── */

static int st_get_scheduler(int argc, char **argv, bool json)
{
    const char *dev = NULL;
    if (get_dev_arg(argc, argv, &dev) <= 0) {
        cmd_print_err(json, NULL, "missing --dev <device>");
        return 1;
    }
    zenctl_err_t err;
    zenctl_storage_t *st = zenctl_storage_open(dev, &err);
    if (!st) {
        cmd_print_err(json, &err, "cannot open device");
        return 1;
    }
    char *cur = NULL, *list = NULL;
    int rc1 = zenctl_storage_get_scheduler(st, &cur, &err);
    int rc2 = zenctl_storage_list_schedulers(st, &list, NULL);
    zenctl_storage_close(st);
    if (rc1 != 0) {
        cmd_print_err(json, &err, "cannot read scheduler");
        free(cur); free(list);
        return 1;
    }
    if (json) {
        out_json_ok_begin();
        out_json_open_object();
        bool f = true;
        out_json_field_string(&f, "device", dev);
        out_json_field_string(&f, "scheduler", cur);
        if (rc2 == 0) out_json_field_string(&f, "available", list);
        out_json_close_object();
        out_json_ok_end();
    } else {
        out_kv("Device", dev);
        out_kv("Scheduler", cur);
        if (rc2 == 0) out_kv("Available", list);
    }
    free(cur); free(list);
    return 0;
}

static int st_set_scheduler(int argc, char **argv, bool json, bool dry_run)
{
    const char *dev = NULL;
    if (get_dev_arg(argc, argv, &dev) <= 0) {
        cmd_print_err(json, NULL, "missing --dev <device>");
        return 1;
    }
    if (cmd_positional_count(argc, argv) < 1) {
        cmd_print_err(json, NULL, "missing scheduler name");
        return 1;
    }
    const char *sched = cmd_positional(argc, argv, 0);

    if (dry_run) {
        if (json) {
            out_json_ok_begin();
            out_json_open_object();
            bool f = true;
            out_json_field_bool(&f, "dry_run", true);
            out_json_field_string(&f, "action", "set_scheduler");
            out_json_field_string(&f, "device", dev);
            out_json_field_string(&f, "scheduler", sched);
            out_json_close_object();
            out_json_ok_end();
        } else {
            char msg[256];
            snprintf(msg, sizeof(msg),
                     "Would set scheduler='%s' on %s", sched, dev);
            out_dryrun(msg);
        }
        return 0;
    }

    zenctl_err_t err;
    zenctl_storage_t *st = zenctl_storage_open(dev, &err);
    if (!st) {
        cmd_print_err(json, &err, "cannot open device");
        return 1;
    }
    int rc = zenctl_storage_set_scheduler(st, sched, &err);
    zenctl_storage_close(st);
    if (rc != 0) {
        cmd_print_err(json, &err, "set scheduler failed");
        return 1;
    }
    if (json) {
        out_json_ok_begin();
        out_json_open_object();
        bool f = true;
        out_json_field_string(&f, "device", dev);
        out_json_field_string(&f, "scheduler", sched);
        out_json_close_object();
        out_json_ok_end();
    } else {
        printf("%s: scheduler=%s\n", dev, sched);
    }
    return 0;
}

/* ── get queue-depth ─────────────────────────────────────────────── */

static int st_get_queue_depth(int argc, char **argv, bool json)
{
    const char *dev = NULL;
    if (get_dev_arg(argc, argv, &dev) <= 0) {
        cmd_print_err(json, NULL, "missing --dev <device>");
        return 1;
    }
    zenctl_err_t err;
    zenctl_storage_t *st = zenctl_storage_open(dev, &err);
    if (!st) {
        cmd_print_err(json, &err, "cannot open device");
        return 1;
    }
    int qd = 0;
    int rc = zenctl_storage_get_queue_depth(st, &qd, &err);
    zenctl_storage_close(st);
    if (rc != 0) {
        cmd_print_err(json, &err, "cannot read queue_depth");
        return 1;
    }
    if (json) {
        out_json_ok_begin();
        out_json_open_object();
        bool f = true;
        out_json_field_string(&f, "device", dev);
        out_json_field_int(&f, "queue_depth", qd);
        out_json_close_object();
        out_json_ok_end();
    } else {
        char b[16]; snprintf(b, sizeof(b), "%d", qd);
        out_kv("Device", dev);
        out_kv("Queue depth", b);
    }
    return 0;
}

/* ── get / set read-ahead ────────────────────────────────────────── */

static int st_get_read_ahead(int argc, char **argv, bool json)
{
    const char *dev = NULL;
    if (get_dev_arg(argc, argv, &dev) <= 0) {
        cmd_print_err(json, NULL, "missing --dev <device>");
        return 1;
    }
    zenctl_err_t err;
    zenctl_storage_t *st = zenctl_storage_open(dev, &err);
    if (!st) {
        cmd_print_err(json, &err, "cannot open device");
        return 1;
    }
    int64_t ra = 0;
    int rc = zenctl_storage_get_read_ahead(st, &ra, &err);
    zenctl_storage_close(st);
    if (rc != 0) {
        cmd_print_err(json, &err, "cannot read read-ahead");
        return 1;
    }
    if (json) {
        out_json_ok_begin();
        out_json_open_object();
        bool f = true;
        out_json_field_string(&f, "device", dev);
        out_json_field_int(&f, "read_ahead_kb", ra);
        out_json_close_object();
        out_json_ok_end();
    } else {
        char b[32]; snprintf(b, sizeof(b), "%lld kB", (long long)ra);
        out_kv("Device", dev);
        out_kv("Read-ahead", b);
    }
    return 0;
}

static int st_set_read_ahead(int argc, char **argv, bool json, bool dry_run)
{
    const char *dev = NULL;
    if (get_dev_arg(argc, argv, &dev) <= 0) {
        cmd_print_err(json, NULL, "missing --dev <device>");
        return 1;
    }
    if (cmd_positional_count(argc, argv) < 1) {
        cmd_print_err(json, NULL, "missing KB value");
        return 1;
    }
    const char *s = cmd_positional(argc, argv, 0);
    char *e = NULL;
    long long kb = strtoll(s, &e, 10);
    if (e == s || *e != '\0' || kb < 0) {
        cmd_print_err(json, NULL, "read-ahead must be a non-negative integer (kB)");
        return 1;
    }

    if (dry_run) {
        if (json) {
            out_json_ok_begin();
            out_json_open_object();
            bool f = true;
            out_json_field_bool(&f, "dry_run", true);
            out_json_field_string(&f, "action", "set_read_ahead");
            out_json_field_string(&f, "device", dev);
            out_json_field_int(&f, "read_ahead_kb", (int64_t)kb);
            out_json_close_object();
            out_json_ok_end();
        } else {
            char msg[256];
            snprintf(msg, sizeof(msg),
                     "Would set read_ahead_kb=%lld on %s", kb, dev);
            out_dryrun(msg);
        }
        return 0;
    }

    zenctl_err_t err;
    zenctl_storage_t *st = zenctl_storage_open(dev, &err);
    if (!st) {
        cmd_print_err(json, &err, "cannot open device");
        return 1;
    }
    int rc = zenctl_storage_set_read_ahead(st, (int64_t)kb, &err);
    zenctl_storage_close(st);
    if (rc != 0) {
        cmd_print_err(json, &err, "set read-ahead failed");
        return 1;
    }
    if (json) {
        out_json_ok_begin();
        out_json_open_object();
        bool f = true;
        out_json_field_string(&f, "device", dev);
        out_json_field_int(&f, "read_ahead_kb", (int64_t)kb);
        out_json_close_object();
        out_json_ok_end();
    } else {
        printf("%s: read_ahead_kb=%lld\n", dev, kb);
    }
    return 0;
}

/* ── get / set cache-type ────────────────────────────────────────── */

static int st_get_cache_type(int argc, char **argv, bool json)
{
    const char *dev = NULL;
    if (get_dev_arg(argc, argv, &dev) <= 0) {
        cmd_print_err(json, NULL, "missing --dev <device>");
        return 1;
    }
    zenctl_err_t err;
    zenctl_storage_t *st = zenctl_storage_open(dev, &err);
    if (!st) {
        cmd_print_err(json, &err, "cannot open device");
        return 1;
    }
    zenctl_cache_type_t t;
    int rc = zenctl_storage_get_cache_type(st, &t, &err);
    zenctl_storage_close(st);
    if (rc != 0) {
        cmd_print_err(json, &err, "cannot read cache_type");
        return 1;
    }
    if (json) {
        out_json_ok_begin();
        out_json_open_object();
        bool f = true;
        out_json_field_string(&f, "device", dev);
        out_json_field_string(&f, "cache_type", cache_type_str(t));
        out_json_field_int(&f, "cache_type_id", (int)t);
        out_json_close_object();
        out_json_ok_end();
    } else {
        out_kv("Device", dev);
        out_kv("Cache type", cache_type_str(t));
    }
    return 0;
}

static int st_set_cache_type(int argc, char **argv, bool json, bool dry_run)
{
    const char *dev = NULL;
    if (get_dev_arg(argc, argv, &dev) <= 0) {
        cmd_print_err(json, NULL, "missing --dev <device>");
        return 1;
    }
    if (cmd_positional_count(argc, argv) < 1) {
        cmd_print_err(json, NULL, "missing cache type");
        return 1;
    }
    const char *s = cmd_positional(argc, argv, 0);
    zenctl_cache_type_t t;
    if (cache_type_parse(s, &t) != 0) {
        cmd_print_err(json, NULL,
                      "cache type must be write_back|write_through|none");
        return 1;
    }

    if (dry_run) {
        if (json) {
            out_json_ok_begin();
            out_json_open_object();
            bool f = true;
            out_json_field_bool(&f, "dry_run", true);
            out_json_field_string(&f, "action", "set_cache_type");
            out_json_field_string(&f, "device", dev);
            out_json_field_string(&f, "cache_type", s);
            out_json_close_object();
            out_json_ok_end();
        } else {
            char msg[256];
            snprintf(msg, sizeof(msg),
                     "Would set cache_type='%s' on %s", s, dev);
            out_dryrun(msg);
        }
        return 0;
    }

    zenctl_err_t err;
    zenctl_storage_t *st = zenctl_storage_open(dev, &err);
    if (!st) {
        cmd_print_err(json, &err, "cannot open device");
        return 1;
    }
    int rc = zenctl_storage_set_cache_type(st, t, &err);
    zenctl_storage_close(st);
    if (rc != 0) {
        cmd_print_err(json, &err, "set cache_type failed");
        return 1;
    }
    if (json) {
        out_json_ok_begin();
        out_json_open_object();
        bool f = true;
        out_json_field_string(&f, "device", dev);
        out_json_field_string(&f, "cache_type", s);
        out_json_close_object();
        out_json_ok_end();
    } else {
        printf("%s: cache_type=%s\n", dev, s);
    }
    return 0;
}

/* ── get stats ───────────────────────────────────────────────────── */

static int st_get_stats(int argc, char **argv, bool json)
{
    const char *dev = NULL;
    if (get_dev_arg(argc, argv, &dev) <= 0) {
        cmd_print_err(json, NULL, "missing --dev <device>");
        return 1;
    }
    zenctl_err_t err;
    zenctl_storage_t *st = zenctl_storage_open(dev, &err);
    if (!st) {
        cmd_print_err(json, &err, "cannot open device");
        return 1;
    }
    zenctl_io_stats_t s;
    int rc = zenctl_storage_get_io_stats(st, &s, &err);
    zenctl_storage_close(st);
    if (rc != 0) {
        cmd_print_err(json, &err, "cannot read I/O stats");
        return 1;
    }
    if (json) {
        out_json_ok_begin();
        out_json_open_object();
        bool f = true;
        out_json_field_string(&f, "device", dev);
        out_json_field_object_begin(&f, "stats");
        bool sf = true;
        out_json_field_int(&sf, "reads_completed",     s.reads_completed);
        out_json_field_int(&sf, "reads_merged",        s.reads_merged);
        out_json_field_int(&sf, "sectors_read",        s.sectors_read);
        out_json_field_int(&sf, "time_reading_ms",     s.time_reading_ms);
        out_json_field_int(&sf, "writes_completed",    s.writes_completed);
        out_json_field_int(&sf, "writes_merged",       s.writes_merged);
        out_json_field_int(&sf, "sectors_written",     s.sectors_written);
        out_json_field_int(&sf, "time_writing_ms",     s.time_writing_ms);
        out_json_field_int(&sf, "ios_in_progress",     s.ios_in_progress);
        out_json_field_int(&sf, "time_io_ms",          s.time_io_ms);
        out_json_field_int(&sf, "weighted_time_io_ms", s.weighted_time_io_ms);
        out_json_field_int(&sf, "discards_completed",  s.discards_completed);
        out_json_field_int(&sf, "discards_merged",     s.discards_merged);
        out_json_field_int(&sf, "sectors_discarded",   s.sectors_discarded);
        out_json_field_int(&sf, "time_discarding_ms",  s.time_discarding_ms);
        out_json_field_int(&sf, "flushes_completed",   s.flushes_completed);
        out_json_field_int(&sf, "time_flushing_ms",    s.time_flushing_ms);
        out_json_close_object();
        out_json_close_object();
        out_json_ok_end();
    } else {
        out_kv("Device", dev);
        char b[32];
        snprintf(b, sizeof(b), "%lld", (long long)s.reads_completed);
        out_kv("Reads completed", b);
        snprintf(b, sizeof(b), "%lld", (long long)s.sectors_read);
        out_kv("Sectors read", b);
        snprintf(b, sizeof(b), "%lld ms", (long long)s.time_reading_ms);
        out_kv("Time reading", b);
        snprintf(b, sizeof(b), "%lld", (long long)s.writes_completed);
        out_kv("Writes completed", b);
        snprintf(b, sizeof(b), "%lld", (long long)s.sectors_written);
        out_kv("Sectors written", b);
        snprintf(b, sizeof(b), "%lld ms", (long long)s.time_writing_ms);
        out_kv("Time writing", b);
        snprintf(b, sizeof(b), "%lld", (long long)s.ios_in_progress);
        out_kv("I/Os in progress", b);
        snprintf(b, sizeof(b), "%lld", (long long)s.discards_completed);
        out_kv("Discards completed", b);
        snprintf(b, sizeof(b), "%lld", (long long)s.flushes_completed);
        out_kv("Flushes completed", b);
    }
    return 0;
}

/* ── get / set power ─────────────────────────────────────────────── */

static int st_get_power(int argc, char **argv, bool json)
{
    const char *dev = NULL;
    if (get_dev_arg(argc, argv, &dev) <= 0) {
        cmd_print_err(json, NULL, "missing --dev <device>");
        return 1;
    }
    zenctl_err_t err;
    zenctl_storage_t *st = zenctl_storage_open(dev, &err);
    if (!st) {
        cmd_print_err(json, &err, "cannot open device");
        return 1;
    }
    char *mode = NULL;
    int rc = zenctl_storage_get_power_control(st, &mode, &err);
    zenctl_storage_close(st);
    if (rc != 0) {
        cmd_print_err(json, &err, "cannot read power control");
        return 1;
    }
    if (json) {
        out_json_ok_begin();
        out_json_open_object();
        bool f = true;
        out_json_field_string(&f, "device", dev);
        out_json_field_string(&f, "power", mode);
        out_json_close_object();
        out_json_ok_end();
    } else {
        out_kv("Device", dev);
        out_kv("Power control", mode);
    }
    free(mode);
    return 0;
}

static int st_set_power(int argc, char **argv, bool json, bool dry_run)
{
    const char *dev = NULL;
    if (get_dev_arg(argc, argv, &dev) <= 0) {
        cmd_print_err(json, NULL, "missing --dev <device>");
        return 1;
    }
    if (cmd_positional_count(argc, argv) < 1) {
        cmd_print_err(json, NULL, "missing power mode (auto|on)");
        return 1;
    }
    const char *mode = cmd_positional(argc, argv, 0);
    if (strcmp(mode, "auto") != 0 && strcmp(mode, "on") != 0) {
        cmd_print_err(json, NULL, "power mode must be 'auto' or 'on'");
        return 1;
    }

    if (dry_run) {
        if (json) {
            out_json_ok_begin();
            out_json_open_object();
            bool f = true;
            out_json_field_bool(&f, "dry_run", true);
            out_json_field_string(&f, "action", "set_power");
            out_json_field_string(&f, "device", dev);
            out_json_field_string(&f, "power", mode);
            out_json_close_object();
            out_json_ok_end();
        } else {
            char msg[256];
            snprintf(msg, sizeof(msg),
                     "Would set power='%s' on %s", mode, dev);
            out_dryrun(msg);
        }
        return 0;
    }

    zenctl_err_t err;
    zenctl_storage_t *st = zenctl_storage_open(dev, &err);
    if (!st) {
        cmd_print_err(json, &err, "cannot open device");
        return 1;
    }
    int rc = zenctl_storage_set_power_control(st, mode, &err);
    zenctl_storage_close(st);
    if (rc != 0) {
        cmd_print_err(json, &err, "set power failed");
        return 1;
    }
    if (json) {
        out_json_ok_begin();
        out_json_open_object();
        bool f = true;
        out_json_field_string(&f, "device", dev);
        out_json_field_string(&f, "power", mode);
        out_json_close_object();
        out_json_ok_end();
    } else {
        printf("%s: power=%s\n", dev, mode);
    }
    return 0;
}

/* ── list ────────────────────────────────────────────────────────── */

/* Filter for block device names: not "." or ".." and no slashes. */
static int block_dev_filter(const char *name)
{
    if (!name || !*name) return 0;
    if (strchr(name, '/')) return 0;
    if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) return 0;
    return 1;
}

static int st_list(int argc, char **argv, bool json)
{
    (void)argc; (void)argv;
    char **list = NULL;
    int n = 0;
    if (cmd_list_dir("/sys/block", block_dev_filter, &list, &n) < 0) {
        cmd_print_err(json, NULL, "cannot enumerate /sys/block");
        return 1;
    }

    if (json) {
        out_json_ok_begin();
        out_json_open_object();
        bool f = true;
        out_json_field_int(&f, "count", n);
        out_json_field_array_begin(&f, "devices");
        bool arr_first = true;
        for (int i = 0; i < n; i++) {
            out_json_array_string(&arr_first, list[i]);
        }
        out_json_close_array();
        out_json_close_object();
        out_json_ok_end();
    } else {
        out_table_reset();
        const char *h[] = { "DEVICE" };
        out_table_header(h, 1);
        for (int i = 0; i < n; i++) {
            const char *row[] = { list[i] };
            out_table_row(row, 1);
        }
    }
    cmd_free_list(list, n);
    return 0;
}

/* ── top-level dispatch ──────────────────────────────────────────── */

int cmd_storage(int argc, char **argv, bool json, bool dry_run, bool confirm)
{
    (void)confirm;
    if (argc < 1) {
        cmd_print_err(json, NULL, "missing subcommand");
        return 1;
    }
    const char *sub = argv[0];

    if (strcmp(sub, "list") == 0)
        return st_list(argc - 1, argv + 1, json);

    if (strcmp(sub, "get") == 0) {
        if (argc < 2) { cmd_print_err(json, NULL, "missing get target"); return 1; }
        const char *what = argv[1];
        int rargc = argc - 2; char **rargv = argv + 2;
        if (strcmp(what, "scheduler")    == 0) return st_get_scheduler(rargc, rargv, json);
        if (strcmp(what, "queue-depth")  == 0) return st_get_queue_depth(rargc, rargv, json);
        if (strcmp(what, "read-ahead")   == 0) return st_get_read_ahead(rargc, rargv, json);
        if (strcmp(what, "cache-type")   == 0) return st_get_cache_type(rargc, rargv, json);
        if (strcmp(what, "stats")        == 0) return st_get_stats(rargc, rargv, json);
        if (strcmp(what, "power")        == 0) return st_get_power(rargc, rargv, json);
        char buf[160];
        snprintf(buf, sizeof(buf), "unknown storage get target '%s'", what);
        cmd_print_err(json, NULL, buf);
        return 1;
    }
    if (strcmp(sub, "set") == 0) {
        if (argc < 2) { cmd_print_err(json, NULL, "missing set target"); return 1; }
        const char *what = argv[1];
        int rargc = argc - 2; char **rargv = argv + 2;
        if (strcmp(what, "scheduler")    == 0) return st_set_scheduler(rargc, rargv, json, dry_run);
        if (strcmp(what, "read-ahead")   == 0) return st_set_read_ahead(rargc, rargv, json, dry_run);
        if (strcmp(what, "cache-type")   == 0) return st_set_cache_type(rargc, rargv, json, dry_run);
        if (strcmp(what, "power")        == 0) return st_set_power(rargc, rargv, json, dry_run);
        char buf[160];
        snprintf(buf, sizeof(buf), "unknown storage set target '%s'", what);
        cmd_print_err(json, NULL, buf);
        return 1;
    }
    char buf[160];
    snprintf(buf, sizeof(buf), "unknown storage subcommand '%s'", sub);
    cmd_print_err(json, NULL, buf);
    return 1;
}
