/* test_mem.c - memory domain unit tests against a mock /proc and /sys tree. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "zenctl/zenctl.h"
#include "harness.h"
#include "mock_sysfs.h"

static void test_swappiness(void)
{
    mock_sysfs_create_file("proc/sys/vm/swappiness", "60");

    int v = -1;
    zenctl_err_t err;
    memset(&err, 0, sizeof(err));
    int rc = zenctl_mem_get_swappiness(&v, &err);
    OK(rc == 0, "get_swappiness returns 0");
    OK(v == 60, "get_swappiness returns 60 from mock file");

    /* set_swappiness writes the value as a base-10 string */
    memset(&err, 0, sizeof(err));
    rc = zenctl_mem_set_swappiness(100, &err);
    OK(rc == 0, "set_swappiness(100) returns 0");
    char buf[32];
    int n = mock_sysfs_read_file("proc/sys/vm/swappiness", buf, sizeof(buf));
    OK(n >= 0, "swappiness file exists after write");
    OK(strcmp(buf, "100") == 0, "swappiness file contains \"100\"");

    /* range validation */
    memset(&err, 0, sizeof(err));
    rc = zenctl_mem_set_swappiness(-1, &err);
    OK(rc == -1, "set_swappiness(-1) rejected");
    OK(err.code == ZENCTL_ERR_ERANGE, "set_swappiness(-1) sets ERANGE");

    memset(&err, 0, sizeof(err));
    rc = zenctl_mem_set_swappiness(201, &err);
    OK(rc == -1, "set_swappiness(201) rejected");
    OK(err.code == ZENCTL_ERR_ERANGE, "set_swappiness(201) sets ERANGE");

    /* boundaries: 0 and 200 must be accepted */
    memset(&err, 0, sizeof(err));
    rc = zenctl_mem_set_swappiness(0, &err);
    OK(rc == 0, "set_swappiness(0) accepted (lower bound)");

    memset(&err, 0, sizeof(err));
    rc = zenctl_mem_set_swappiness(200, &err);
    OK(rc == 0, "set_swappiness(200) accepted (upper bound)");
}

static void test_thp(void)
{
    mock_sysfs_create_file(
        "sys/kernel/mm/transparent_hugepage/enabled",
        "[always] madvise never");

    zenctl_thp_mode_t mode = (zenctl_thp_mode_t)999;
    zenctl_err_t err;
    memset(&err, 0, sizeof(err));
    int rc = zenctl_mem_get_thp(&mode, &err);
    OK(rc == 0, "get_thp returns 0");
    OK(mode == ZENCTL_THP_ALWAYS, "get_thp returns ZENCTL_THP_ALWAYS");

    /* Switch the fixture to madvise */
    mock_sysfs_create_file(
        "sys/kernel/mm/transparent_hugepage/enabled",
        "always [madvise] never");
    memset(&err, 0, sizeof(err));
    rc = zenctl_mem_get_thp(&mode, &err);
    OK(rc == 0, "get_thp returns 0 (second call)");
    OK(mode == ZENCTL_THP_MADVISE, "get_thp returns ZENCTL_THP_MADVISE");

    /* And never */
    mock_sysfs_create_file(
        "sys/kernel/mm/transparent_hugepage/enabled",
        "always madvise [never]");
    memset(&err, 0, sizeof(err));
    rc = zenctl_mem_get_thp(&mode, &err);
    OK(rc == 0, "get_thp returns 0 (third call)");
    OK(mode == ZENCTL_THP_NEVER, "get_thp returns ZENCTL_THP_NEVER");

    /* set_thp writes the canonical name */
    memset(&err, 0, sizeof(err));
    rc = zenctl_mem_set_thp(ZENCTL_THP_ALWAYS, &err);
    OK(rc == 0, "set_thp(ALWAYS) returns 0");
    char buf[64];
    int n = mock_sysfs_read_file(
        "sys/kernel/mm/transparent_hugepage/enabled",
        buf, sizeof(buf));
    OK(n >= 0, "enabled file exists after set_thp");
    OK(strcmp(buf, "always") == 0, "set_thp writes \"always\"");
}

static void test_nr_hugepages(void)
{
    mock_sysfs_create_file("proc/sys/vm/nr_hugepages", "1024");

    int64_t pages = 0;
    zenctl_err_t err;
    memset(&err, 0, sizeof(err));
    int rc = zenctl_mem_get_nr_hugepages(&pages, &err);
    OK(rc == 0, "get_nr_hugepages returns 0");
    OK(pages == 1024, "get_nr_hugepages returns 1024");

    /* set_nr_hugepages writes the integer */
    memset(&err, 0, sizeof(err));
    rc = zenctl_mem_set_nr_hugepages(2048, &err);
    OK(rc == 0, "set_nr_hugepages(2048) returns 0");
    char buf[32];
    int n = mock_sysfs_read_file("proc/sys/vm/nr_hugepages", buf, sizeof(buf));
    OK(n >= 0, "nr_hugepages file exists after write");
    OK(strcmp(buf, "2048") == 0, "nr_hugepages file contains \"2048\"");

    /* negative input rejected without touching the file */
    memset(&err, 0, sizeof(err));
    rc = zenctl_mem_set_nr_hugepages(-1, &err);
    OK(rc == -1, "set_nr_hugepages(-1) rejected");
    OK(err.code == ZENCTL_ERR_EINVAL,
       "set_nr_hugepages(-1) sets ZENCTL_ERR_EINVAL");
}

static void test_overcommit(void)
{
    /* The library accepts 0, 1, 2. Test each read value. */
    zenctl_err_t err;
    int v;

    mock_sysfs_create_file("proc/sys/vm/overcommit_memory", "0");
    memset(&err, 0, sizeof(err));
    OK(zenctl_mem_get_overcommit(&v, &err) == 0, "get_overcommit returns 0");
    OK(v == 0, "get_overcommit reads 0");

    mock_sysfs_create_file("proc/sys/vm/overcommit_memory", "1");
    memset(&err, 0, sizeof(err));
    OK(zenctl_mem_get_overcommit(&v, &err) == 0, "get_overcommit returns 0 (1)");
    OK(v == 1, "get_overcommit reads 1");

    mock_sysfs_create_file("proc/sys/vm/overcommit_memory", "2");
    memset(&err, 0, sizeof(err));
    OK(zenctl_mem_get_overcommit(&v, &err) == 0, "get_overcommit returns 0 (2)");
    OK(v == 2, "get_overcommit reads 2");

    /* set */
    memset(&err, 0, sizeof(err));
    OK(zenctl_mem_set_overcommit(1, &err) == 0, "set_overcommit(1) returns 0");
    char buf[32];
    int n = mock_sysfs_read_file("proc/sys/vm/overcommit_memory",
                                 buf, sizeof(buf));
    OK(n >= 0, "overcommit_memory file exists after write");
    OK(strcmp(buf, "1") == 0, "overcommit_memory file contains \"1\"");

    /* reject out-of-range value (3 is not a valid kernel mode) */
    memset(&err, 0, sizeof(err));
    OK(zenctl_mem_set_overcommit(3, &err) == -1, "set_overcommit(3) rejected");
    OK(err.code == ZENCTL_ERR_EINVAL,
       "set_overcommit(3) sets ZENCTL_ERR_EINVAL");
}

int test_mem_suite(void)
{
    SUITE_START("memory domain");
    test_swappiness();
    test_thp();
    test_nr_hugepages();
    test_overcommit();
    SUITE_END();
    return SUITE_FAILURES();
}
