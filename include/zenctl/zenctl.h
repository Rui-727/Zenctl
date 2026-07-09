/* zenctl.h - master include for libzenctl
 *
 * libzenctl is a C library that exposes every hardware control on
 * Linux through a single API. Two layers: typed domain API (cpu.h,
 * net.h, etc.) and a generic key-value fallback.
 *
 * Include this header to get everything:
 *   #include <zenctl/zenctl.h>
 */
#ifndef ZENCTL_H
#define ZENCTL_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ── Error model ─────────────────────────────────────────────────── */

#define ZENCTL_OK           0
#define ZENCTL_ERR_EPERM    1
#define ZENCTL_ERR_ENOENT   2
#define ZENCTL_ERR_EINVAL   3
#define ZENCTL_ERR_EIO      4
#define ZENCTL_ERR_ENOTSUP  5
#define ZENCTL_ERR_ERANGE   6
#define ZENCTL_ERR_NOMEM    7
#define ZENCTL_ERR_INTERNAL 8

typedef struct {
    int          code;
    char         message[256];
    char         context[256];
    bool         recoverable;
} zenctl_err_t;

const char *zenctl_strerror(int code);

/* ── Capability ──────────────────────────────────────────────────── */

typedef enum {
    ZENCTL_CAP_UNAVAILABLE = 0,
    ZENCTL_CAP_AVAILABLE   = 1,
    ZENCTL_CAP_READONLY    = 2,
} zenctl_cap_t;

/* ── Context ─────────────────────────────────────────────────────── */

typedef struct zenctl_ctx zenctl_ctx_t;

zenctl_ctx_t *zenctl_ctx_new(zenctl_err_t *err);
void          zenctl_ctx_free(zenctl_ctx_t *ctx);

/* ── Generic key-value API ───────────────────────────────────────── */

typedef struct {
    enum {
        ZENCTL_VAL_STRING,
        ZENCTL_VAL_INT,
        ZENCTL_VAL_BOOL,
        ZENCTL_VAL_DOUBLE,
    } type;
    union {
        char    *s;
        int64_t  i;
        bool     b;
        double   d;
    } v;
} zenctl_val_t;

void zenctl_val_free(zenctl_val_t *val);

int zenctl_get(zenctl_ctx_t *ctx, const char *key,
               zenctl_val_t *out, zenctl_err_t *err);
int zenctl_set(zenctl_ctx_t *ctx, const char *key,
               const char *value, zenctl_err_t *err);

/* ── Key iteration ───────────────────────────────────────────────── */

typedef struct zenctl_key_iter zenctl_key_iter_t;

zenctl_key_iter_t *zenctl_keys(zenctl_ctx_t *ctx, const char *domain_prefix,
                               zenctl_err_t *err);
int  zenctl_key_iter_next(zenctl_key_iter_t *it, char **key_out);
void zenctl_key_iter_free(zenctl_key_iter_t *it);

/* ── Capability query ────────────────────────────────────────────── */

zenctl_cap_t zenctl_query_cap(zenctl_ctx_t *ctx, const char *key);

/* ── Domain headers ──────────────────────────────────────────────── */

#include "cpu.h"
#include "mem.h"
#include "storage.h"
#include "net.h"
#include "gpu.h"
#include "thermal.h"
#include "power.h"
#include "pcie.h"
#include "usb.h"
#include "bt.h"
#include "wireless.h"
#include "firmware.h"

#ifdef __cplusplus
}
#endif

#endif /* ZENCTL_H */
