/* test_kv_api.c - generic key-value API and core-function tests.
 *
 * Covers:
 *   - zenctl_get / zenctl_set with valid-and-unknown keys (ENOTSUP)
 *   - zenctl_get with NULL out (EINVAL)
 *   - zenctl_val_free in all states (NULL, string, int, bool, double)
 *   - zenctl_strerror for every error code
 *   - zenctl_ctx_new / zenctl_ctx_free lifecycle
 *   - zenctl_keys (key iterator) returns NULL with ENOTSUP
 *
 * The malformed-key error paths (NULL, empty, too-long, no-domain)
 * are exercised in test_errors.c section 8.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "zenctl/zenctl.h"
#include "harness.h"

/* ── Generic key-value API: valid keys, ENOTSUP ──────────────────── */

static void test_kv_valid_unknown_keys(void)
{
    zenctl_ctx_t *ctx = zenctl_ctx_new(NULL);
    OK(ctx != NULL, "ctx_new for kv-api tests");

    zenctl_err_t err;
    zenctl_val_t out;
    memset(&out, 0, sizeof(out));

    /* Every well-formed key currently returns ENOTSUP because the
     * domain dispatcher is not wired up yet. The point of these
     * tests is to lock in the contract: a well-formed key never
     * returns EINVAL, only ENOTSUP. */
    memset(&err, 0, sizeof(err));
    OK(zenctl_get(ctx, "mem.swappiness", &out, &err) == -1,
       "zenctl_get(\"mem.swappiness\") returns -1 (not implemented)");
    OK(err.code == ZENCTL_ERR_ENOTSUP,
       "zenctl_get(\"mem.swappiness\") sets ZENCTL_ERR_ENOTSUP");
    OK(err.recoverable == true,
       "zenctl_get ENOTSUP marks err.recoverable = true");

    memset(&err, 0, sizeof(err));
    OK(zenctl_get(ctx, "cpu.0.governor", &out, &err) == -1,
       "zenctl_get(\"cpu.0.governor\") returns -1");
    OK(err.code == ZENCTL_ERR_ENOTSUP,
       "zenctl_get(\"cpu.0.governor\") sets ZENCTL_ERR_ENOTSUP");

    memset(&err, 0, sizeof(err));
    OK(zenctl_set(ctx, "mem.swappiness", "60", &err) == -1,
       "zenctl_set(\"mem.swappiness\", \"60\") returns -1");
    OK(err.code == ZENCTL_ERR_ENOTSUP,
       "zenctl_set(\"mem.swappiness\") sets ZENCTL_ERR_ENOTSUP");

    memset(&err, 0, sizeof(err));
    OK(zenctl_set(ctx, "cpu.0.online", "1", &err) == -1,
       "zenctl_set(\"cpu.0.online\", \"1\") returns -1");
    OK(err.code == ZENCTL_ERR_ENOTSUP,
       "zenctl_set(\"cpu.0.online\") sets ZENCTL_ERR_ENOTSUP");

    zenctl_ctx_free(ctx);
}

/* ── zenctl_get with NULL out ────────────────────────────────────── */

static void test_kv_get_null_out(void)
{
    zenctl_ctx_t *ctx = zenctl_ctx_new(NULL);
    zenctl_err_t err;
    memset(&err, 0, sizeof(err));
    OK(zenctl_get(ctx, "mem.swappiness", NULL, &err) == -1,
       "zenctl_get(NULL out) returns -1");
    OK(err.code == ZENCTL_ERR_EINVAL,
       "zenctl_get(NULL out) sets ZENCTL_ERR_EINVAL");
    zenctl_ctx_free(ctx);
}

/* ── zenctl_val_free in every state ──────────────────────────────── */

static void test_val_free(void)
{
    /* NULL is a no-op (must not crash). */
    zenctl_val_free(NULL);
    OK(1, "zenctl_val_free(NULL) does not crash");

    /* String value: frees v.s and zeroes the struct. */
    zenctl_val_t v;
    memset(&v, 0, sizeof(v));
    v.type = ZENCTL_VAL_STRING;
    v.v.s = strdup("hello");
    OK(v.v.s != NULL, "strdup for val_free string test");
    zenctl_val_free(&v);
    OK(v.v.s == NULL, "val_free zeroes v.s");
    OK(v.type == 0, "val_free zeroes type");

    /* Int value: no allocation, just zeroes the struct. */
    memset(&v, 0, sizeof(v));
    v.type = ZENCTL_VAL_INT;
    v.v.i = 42;
    zenctl_val_free(&v);
    OK(v.v.i == 0, "val_free zeroes int val");
    OK(v.type == 0, "val_free zeroes type after int");

    /* Bool value. */
    memset(&v, 0, sizeof(v));
    v.type = ZENCTL_VAL_BOOL;
    v.v.b = true;
    zenctl_val_free(&v);
    OK(v.v.b == false, "val_free zeroes bool val");

    /* Double value. */
    memset(&v, 0, sizeof(v));
    v.type = ZENCTL_VAL_DOUBLE;
    v.v.d = 3.14;
    zenctl_val_free(&v);
    OK(v.v.d == 0.0, "val_free zeroes double val");

    /* String with NULL pointer (defensive). */
    memset(&v, 0, sizeof(v));
    v.type = ZENCTL_VAL_STRING;
    v.v.s = NULL;
    zenctl_val_free(&v);
    OK(v.type == 0, "val_free handles STRING with NULL pointer");
}

/* ── zenctl_strerror for every code ──────────────────────────────── */

static void test_strerror_all(void)
{
    OK(strcmp(zenctl_strerror(ZENCTL_OK), "ok") == 0,
       "strerror(OK) == \"ok\"");
    OK(strcmp(zenctl_strerror(ZENCTL_ERR_EPERM),
              "permission denied") == 0,
       "strerror(EPERM) == \"permission denied\"");
    OK(strcmp(zenctl_strerror(ZENCTL_ERR_ENOENT), "not found") == 0,
       "strerror(ENOENT) == \"not found\"");
    OK(strcmp(zenctl_strerror(ZENCTL_ERR_EINVAL),
              "invalid argument") == 0,
       "strerror(EINVAL) == \"invalid argument\"");
    OK(strcmp(zenctl_strerror(ZENCTL_ERR_EIO), "I/O error") == 0,
       "strerror(EIO) == \"I/O error\"");
    OK(strcmp(zenctl_strerror(ZENCTL_ERR_ENOTSUP),
              "not supported") == 0,
       "strerror(ENOTSUP) == \"not supported\"");
    OK(strcmp(zenctl_strerror(ZENCTL_ERR_ERANGE),
              "out of range") == 0,
       "strerror(ERANGE) == \"out of range\"");
    OK(strcmp(zenctl_strerror(ZENCTL_ERR_NOMEM),
              "out of memory") == 0,
       "strerror(NOMEM) == \"out of memory\"");
    OK(strcmp(zenctl_strerror(ZENCTL_ERR_INTERNAL),
              "internal error") == 0,
       "strerror(INTERNAL) == \"internal error\"");
    OK(strcmp(zenctl_strerror(99999), "unknown error") == 0,
       "strerror(unknown) == \"unknown error\"");
}

/* ── Context lifecycle ───────────────────────────────────────────── */

static void test_ctx_lifecycle(void)
{
    zenctl_err_t err;
    memset(&err, 0, sizeof(err));
    zenctl_ctx_t *ctx = zenctl_ctx_new(&err);
    OK(ctx != NULL, "ctx_new returns non-NULL");
    /* ctx_new currently ignores err; just make sure it doesn't crash
     * when err is non-NULL. */

    /* free(NULL) must be safe */
    zenctl_ctx_free(NULL);
    OK(1, "ctx_free(NULL) does not crash");

    if (ctx) zenctl_ctx_free(ctx);
    OK(1, "ctx_free accepts a real ctx");
}

/* ── Key iterator (stub returns NULL with ENOTSUP) ───────────────── */

static void test_key_iter(void)
{
    zenctl_ctx_t *ctx = zenctl_ctx_new(NULL);
    zenctl_err_t err;
    memset(&err, 0, sizeof(err));

    zenctl_key_iter_t *it = zenctl_keys(ctx, "mem", &err);
    OK(it == NULL, "zenctl_keys returns NULL (not implemented)");
    OK(err.code == ZENCTL_ERR_ENOTSUP,
       "zenctl_keys sets ZENCTL_ERR_ENOTSUP");

    /* next/free on a NULL iterator must not crash. zenctl_key_iter_free
     * is documented to accept NULL? It calls free(NULL) which is safe. */
    zenctl_key_iter_free(NULL);
    OK(1, "zenctl_key_iter_free(NULL) does not crash");

    /* next on NULL iterator returns 0 (no more keys) without crashing. */
    char *k = NULL;
    OK(zenctl_key_iter_next(NULL, &k) == 0,
       "zenctl_key_iter_next(NULL, ...) returns 0");

    zenctl_ctx_free(ctx);
}

/* ── zenctl_version ──────────────────────────────────────────────── */

static void test_version(void)
{
    const char *v = zenctl_version();
    OK(v != NULL, "zenctl_version returns non-NULL");
    /* The default version injected by the Makefile is 0.1.0. */
    OK(strcmp(v, "0.1.0") == 0,
       "zenctl_version returns \"0.1.0\" by default");
    OK(zenctl_version() == v,
       "zenctl_version returns the same pointer on repeat calls");
}

int test_kv_api_suite(void)
{
    SUITE_START("generic KV API + core");
    test_kv_valid_unknown_keys();
    test_kv_get_null_out();
    test_val_free();
    test_strerror_all();
    test_ctx_lifecycle();
    test_key_iter();
    test_version();
    SUITE_END();
    return SUITE_FAILURES();
}
