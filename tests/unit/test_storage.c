/* test_storage.c - storage domain unit tests against a mock /sys/block tree. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "zenctl/zenctl.h"
#include "harness.h"
#include "mock_sysfs.h"

static void test_storage_open_and_read(void)
{
    /* Build a mock /sys/block/sda tree. */
    mock_sysfs_create_dir("sys/block/sda/queue");
    mock_sysfs_create_dir("sys/block/sda/device");
    mock_sysfs_create_file("sys/block/sda/queue/scheduler",
                           "mq-deadline kyber [bfq] none");
    mock_sysfs_create_file("sys/block/sda/queue/read_ahead_kb", "128");
    mock_sysfs_create_file("sys/block/sda/queue/rotational", "1");
    mock_sysfs_create_file("sys/block/sda/queue/discard_max_bytes", "2147483648");
    /* 17-field stat line (modern kernel) */
    mock_sysfs_create_file("sys/block/sda/stat",
        "100 5 2000 40 200 10 4000 80 0 120 160 "
        "3 1 64 8 7 14");

    zenctl_err_t err;
    memset(&err, 0, sizeof(err));
    zenctl_storage_t *st = zenctl_storage_open("sda", &err);
    OK(st != NULL, "storage_open(\"sda\") succeeds against mock tree");
    if (!st) return;

    /* scheduler: active entry is the bracketed one */
    char *sched = NULL;
    memset(&err, 0, sizeof(err));
    int rc = zenctl_storage_get_scheduler(st, &sched, &err);
    OK(rc == 0, "get_scheduler returns 0");
    OK(sched && strcmp(sched, "bfq") == 0,
       "get_scheduler returns \"bfq\" (bracketed entry)");
    free(sched);

    /* set_scheduler: writes the name verbatim */
    memset(&err, 0, sizeof(err));
    rc = zenctl_storage_set_scheduler(st, "mq-deadline", &err);
    OK(rc == 0, "set_scheduler(\"mq-deadline\") returns 0");
    char buf[128];
    int n = mock_sysfs_read_file("sys/block/sda/queue/scheduler",
                                 buf, sizeof(buf));
    OK(n >= 0, "scheduler file exists after write");
    OK(strcmp(buf, "mq-deadline") == 0,
       "scheduler file contains \"mq-deadline\"");

    /* set_scheduler rejects names with slashes / spaces / newlines */
    memset(&err, 0, sizeof(err));
    rc = zenctl_storage_set_scheduler(st, "bfq none", &err);
    OK(rc == -1, "set_scheduler(\"bfq none\") rejected");
    OK(err.code == ZENCTL_ERR_EINVAL,
       "set_scheduler(invalid) sets ZENCTL_ERR_EINVAL");

    /* read_ahead */
    int64_t ra = 0;
    memset(&err, 0, sizeof(err));
    rc = zenctl_storage_get_read_ahead(st, &ra, &err);
    OK(rc == 0, "get_read_ahead returns 0");
    OK(ra == 128, "get_read_ahead returns 128 (kB)");

    /* io_stats: 17 fields */
    zenctl_io_stats_t s;
    memset(&s, 0, sizeof(s));
    memset(&err, 0, sizeof(err));
    rc = zenctl_storage_get_io_stats(st, &s, &err);
    OK(rc == 0, "get_io_stats returns 0");
    OK(s.reads_completed == 100, "io_stats.reads_completed == 100");
    OK(s.reads_merged == 5, "io_stats.reads_merged == 5");
    OK(s.sectors_read == 2000, "io_stats.sectors_read == 2000");
    OK(s.writes_completed == 200, "io_stats.writes_completed == 200");
    OK(s.writes_merged == 10, "io_stats.writes_merged == 10");
    OK(s.sectors_written == 4000, "io_stats.sectors_written == 4000");
    OK(s.ios_in_progress == 0, "io_stats.ios_in_progress == 0");
    OK(s.weighted_time_io_ms == 160, "io_stats.weighted_time_io_ms == 160");
    OK(s.discards_completed == 3, "io_stats.discards_completed == 3");
    OK(s.flushes_completed == 7, "io_stats.flushes_completed == 7");
    OK(s.time_flushing_ms == 14, "io_stats.time_flushing_ms == 14");

    zenctl_storage_close(st);
}

static void test_storage_open_missing(void)
{
    zenctl_err_t err;
    memset(&err, 0, sizeof(err));
    zenctl_storage_t *st = zenctl_storage_open("nosuchdev", &err);
    OK(st == NULL, "storage_open(\"nosuchdev\") returns NULL");
    OK(err.code == ZENCTL_ERR_ENOENT,
       "storage_open(missing) sets ZENCTL_ERR_ENOENT");
}

static void test_storage_open_bad_name(void)
{
    zenctl_err_t err;
    memset(&err, 0, sizeof(err));
    zenctl_storage_t *st = zenctl_storage_open("..", &err);
    OK(st == NULL, "storage_open(\"..\") rejected");
    OK(err.code == ZENCTL_ERR_EINVAL,
       "storage_open(\"..\") sets ZENCTL_ERR_EINVAL");

    memset(&err, 0, sizeof(err));
    st = zenctl_storage_open("sda/..", &err);
    OK(st == NULL, "storage_open(\"sda/..\") rejected");

    memset(&err, 0, sizeof(err));
    st = zenctl_storage_open("", &err);
    OK(st == NULL, "storage_open(\"\") rejected");
}

int test_storage_suite(void)
{
    SUITE_START("storage domain");
    test_storage_open_and_read();
    test_storage_open_missing();
    test_storage_open_bad_name();
    SUITE_END();
    return SUITE_FAILURES();
}
