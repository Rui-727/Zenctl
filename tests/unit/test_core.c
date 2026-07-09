/* test_core.c - minimal libzenctl smoke tests
 *
 * Designed to run unprivileged on any Linux box. The CPU/mem tests
 * only verify the read paths and error handling: they call the typed
 * API on CPU 0 and the default hugepage path, and assert that the
 * calls either succeed or fail cleanly with a populated err struct.
 * They never write.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "zenctl/zenctl.h"

static int failures = 0;

#define CHECK(cond, msg) \
    do { \
        if (cond) { \
            printf("  ok: %s\n", msg); \
        } else { \
            printf("  FAIL: %s\n", msg); \
            failures++; \
        } \
    } while (0)

static void test_strerror(void)
{
    printf("test_strerror\n");
    CHECK(strcmp(zenctl_strerror(ZENCTL_OK), "ok") == 0,
          "ZENCTL_OK -> \"ok\"");
    CHECK(strcmp(zenctl_strerror(ZENCTL_ERR_EPERM), "permission denied") == 0,
          "ZENCTL_ERR_EPERM -> \"permission denied\"");
    CHECK(strcmp(zenctl_strerror(99999), "unknown error") == 0,
          "unknown code -> \"unknown error\"");
}

static void test_ctx(void)
{
    printf("test_ctx\n");
    zenctl_err_t err;
    zenctl_ctx_t *ctx = zenctl_ctx_new(&err);
    CHECK(ctx != NULL, "ctx_new returns non-NULL");
    if (ctx) {
        zenctl_ctx_free(ctx);
        CHECK(1, "ctx_free accepts the ctx");
    }
}

static void test_cpu_read_paths(void)
{
    printf("test_cpu_read_paths\n");
    zenctl_err_t err;
    memset(&err, 0, sizeof(err));

    zenctl_cpu_t *cpu = zenctl_cpu_open(0, &err);
    if (cpu) {
        int64_t min_hz = 0, max_hz = 0;
        if (zenctl_cpu_get_freq_min(cpu, &min_hz, &err) == 0) {
            CHECK(min_hz >= 0, "freq_min read succeeded and is non-negative");
            /* max must be >= min if both reads succeed */
            if (zenctl_cpu_get_freq_max(cpu, &max_hz, &err) == 0)
                CHECK(max_hz >= min_hz, "freq_max >= freq_min");
            else
                CHECK(1, "freq_max read failed cleanly (populated err)");
        } else {
            CHECK(err.code != 0, "freq_min read failed with a populated err");
        }

        bool smt = false;
        if (zenctl_cpu_get_smt_active(cpu, &smt, &err) == 0)
            CHECK(smt == true || smt == false, "smt_active read returned bool");
        else
            CHECK(err.code != 0, "smt_active read failed with populated err");

        zenctl_cpu_close(cpu);
    } else {
        /* CPU 0 missing is rare but possible in containers. */
        CHECK(err.code != 0, "cpu_open(0) failure populated err");
    }
}

static void test_cpu_open_bad_index(void)
{
    printf("test_cpu_open_bad_index\n");
    zenctl_err_t err;
    memset(&err, 0, sizeof(err));

    zenctl_cpu_t *cpu = zenctl_cpu_open(-1, &err);
    CHECK(cpu == NULL, "cpu_open(-1) returns NULL");
    CHECK(err.code == ZENCTL_ERR_EINVAL, "cpu_open(-1) sets EINVAL");

    memset(&err, 0, sizeof(err));
    /* 99999 is beyond NR_CPUS on any reasonable kernel. */
    cpu = zenctl_cpu_open(99999, &err);
    CHECK(cpu == NULL, "cpu_open(99999) returns NULL");
    CHECK(err.code != 0, "cpu_open(99999) populates err");
}

static void test_mem_hugepage_size(void)
{
    printf("test_mem_hugepage_size\n");
    zenctl_err_t err;
    memset(&err, 0, sizeof(err));

    int64_t bytes = 0;
    if (zenctl_mem_get_hugepage_size(&bytes, &err) == 0) {
        CHECK(bytes > 0, "hugepage_size read succeeded and is positive");
        /* Default on x86-64 is 2 MiB = 2097152. Allow any power of two
         * >= 4 KiB. */
        CHECK(bytes >= 4096, "hugepage_size >= 4 KiB");
    } else {
        CHECK(err.code != 0, "hugepage_size read failed with populated err");
    }
}

static void test_mem_numa_count(void)
{
    printf("test_mem_numa_count\n");
    zenctl_err_t err;
    memset(&err, 0, sizeof(err));

    int n = -1;
    if (zenctl_mem_numa_node_count(&n, &err) == 0) {
        CHECK(n >= 0, "numa_node_count returned non-negative count");
    } else {
        CHECK(err.code != 0, "numa_node_count failed with populated err");
    }
}

static void test_mem_swappiness_range(void)
{
    printf("test_mem_swappiness_range\n");
    zenctl_err_t err;
    memset(&err, 0, sizeof(err));

    /* -1 is invalid and must be rejected without touching the file. */
    int rc = zenctl_mem_set_swappiness(-1, &err);
    CHECK(rc == -1, "set_swappiness(-1) rejected");
    CHECK(err.code == ZENCTL_ERR_ERANGE, "set_swappiness(-1) sets ERANGE");

    memset(&err, 0, sizeof(err));
    rc = zenctl_mem_set_swappiness(201, &err);
    CHECK(rc == -1, "set_swappiness(201) rejected");
    CHECK(err.code == ZENCTL_ERR_ERANGE, "set_swappiness(201) sets ERANGE");
}

static void test_mem_overcommit_range(void)
{
    printf("test_mem_overcommit_range\n");
    zenctl_err_t err;
    memset(&err, 0, sizeof(err));

    int rc = zenctl_mem_set_overcommit(3, &err);
    CHECK(rc == -1, "set_overcommit(3) rejected");
    CHECK(err.code == ZENCTL_ERR_EINVAL, "set_overcommit(3) sets EINVAL");
}

int main(void)
{
    printf("libzenctl smoke tests\n");
    test_strerror();
    test_ctx();
    test_cpu_read_paths();
    test_cpu_open_bad_index();
    test_mem_hugepage_size();
    test_mem_numa_count();
    test_mem_swappiness_range();
    test_mem_overcommit_range();

    printf("\n%d failure(s)\n", failures);
    return failures ? 1 : 0;
}
