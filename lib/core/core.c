/* core.c - libzenctl core: context, error, key-value API
 *
 * The context holds capability state and the backend router. The
 * key-value API dispatches to domain backends based on the key prefix.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#include "zenctl/zenctl.h"

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
    int placeholder; /* capability cache will go here */
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

int zenctl_get(zenctl_ctx_t *ctx, const char *key,
               zenctl_val_t *out, zenctl_err_t *err)
{
    (void)ctx; (void)key; (void)out;
    if (err) {
        err->code = ZENCTL_ERR_ENOTSUP;
        snprintf(err->message, sizeof(err->message), "key not implemented");
        snprintf(err->context, sizeof(err->context), "zenctl_get(%s)", key ? key : "NULL");
        err->recoverable = true;
    }
    return -1;
}

int zenctl_set(zenctl_ctx_t *ctx, const char *key,
               const char *value, zenctl_err_t *err)
{
    (void)ctx; (void)key; (void)value;
    if (err) {
        err->code = ZENCTL_ERR_ENOTSUP;
        snprintf(err->message, sizeof(err->message), "key not implemented");
        snprintf(err->context, sizeof(err->context), "zenctl_set(%s)", key ? key : "NULL");
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

zenctl_cap_t zenctl_query_cap(zenctl_ctx_t *ctx, const char *key)
{
    (void)ctx; (void)key;
    return ZENCTL_CAP_UNAVAILABLE;
}
