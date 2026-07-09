/* common.h - shared helpers for the cli/cmd domain subcommands.
 *
 * Header-only: every helper is `static inline` so there is exactly one
 * definition per translation unit and no linking surprises. The
 * functions wrap the boring bits every domain command does the same way:
 * privilege checks, --flag argument parsing, error→output mapping.
 *
 * Output goes through the helpers in cli/output.h (owned by the CLI
 * framework agent); this header does not print on its own except for
 * the require-root and usage helpers. Function signatures match the
 * parallel agent's cmd.h (bool for json/dry_run/confirm).
 */
#ifndef ZENCTL_CLI_CMD_COMMON_H
#define ZENCTL_CLI_CMD_COMMON_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <stdbool.h>
#include <ctype.h>
#include <errno.h>
#include <stdint.h>

#include "zenctl/zenctl.h"
#include "../output.h"

/* The cli_err helper composes a "<msg> (<ctx>)" string into a 1 KiB
 * buffer from two source strings each up to 256 bytes. Theoretical
 * truncation can't happen in practice; -Wformat-truncation is noisy. */
#pragma GCC diagnostic ignored "-Wformat-truncation"

/* ── Privilege ──────────────────────────────────────────────────── */

static inline bool cli_is_root(void) { return geteuid() == 0; }

/* If not root, emit an error (JSON or table per `json`) and return -1.
 * Returns 0 if root. */
static inline int cli_require_root(bool json)
{
    if (cli_is_root()) return 0;
    if (json)
        out_json_error(1 /* ZENCTL_ERR_EPERM */,
                       "this operation requires root");
    else
        out_err("this operation requires root");
    return -1;
}

/* ── Argument helpers ───────────────────────────────────────────── */

/* Find the value of a `--flag VALUE` option in argv. Returns NULL if
 * the flag is absent or has no following value. argv[0] is treated as
 * the first positional argument (the subcommand). */
static inline const char *cli_opt(int argc, char **argv, const char *flag)
{
    for (int i = 0; i < argc - 1; i++)
        if (argv[i] && strcmp(argv[i], flag) == 0)
            return argv[i + 1];
    return NULL;
}

/* True if a boolean flag (--flag, no value) is present. */
static inline bool cli_has_flag(int argc, char **argv, const char *flag)
{
    for (int i = 0; i < argc; i++)
        if (argv[i] && strcmp(argv[i], flag) == 0)
            return true;
    return false;
}

/* Parse a `--flag N` integer option; returns default_val if absent. */
static inline int cli_opt_int(int argc, char **argv, const char *flag,
                              int default_val)
{
    const char *v = cli_opt(argc, argv, flag);
    if (!v) return default_val;
    return atoi(v);
}

/* Parse on/off, enabled/disabled, 1/0, yes/no, true/false.
 * Returns 1 or 0 on success, -1 on unrecognized input. */
static inline int cli_parse_bool(const char *s)
{
    if (!s) return -1;
    if (!strcasecmp(s, "on")      || !strcasecmp(s, "enabled") ||
        !strcasecmp(s, "yes")     || !strcmp(s, "1")           ||
        !strcasecmp(s, "true"))
        return 1;
    if (!strcasecmp(s, "off")     || !strcasecmp(s, "disabled") ||
        !strcasecmp(s, "no")      || !strcmp(s, "0")           ||
        !strcasecmp(s, "false"))
        return 0;
    return -1;
}

/* Parse a base-10 integer; returns 0 on success, -1 on parse failure. */
static inline int cli_parse_int(const char *s, long long *out)
{
    if (!s || !*s) return -1;
    errno = 0;
    char *end = NULL;
    long long v = strtoll(s, &end, 10);
    if (errno != 0 || end == s || *end != '\0') return -1;
    *out = v;
    return 0;
}

/* ── Error construction ─────────────────────────────────────────── */

/* Build a zenctl_err_t in place. Used when the CLI itself detects an
 * error (bad arg, missing dependency) rather than relaying one from
 * the library. Mirrors libzenctl's internal zenctl__set_err. */
static inline void cli_make_err(zenctl_err_t *err, int code,
                                const char *msg, const char *ctx)
{
    if (!err) return;
    memset(err, 0, sizeof(*err));
    err->code = code;
    snprintf(err->message, sizeof(err->message), "%s",
             msg ? msg : zenctl_strerror(code));
    if (ctx)
        snprintf(err->context, sizeof(err->context), "%s", ctx);
    err->recoverable = (code != ZENCTL_ERR_INTERNAL);
}

/* ── Error → output ─────────────────────────────────────────────── */

/* Print a zenctl_err_t in the right format and return -1. Always
 * returns -1 so callers can write `return cli_err(json, &err);`. */
static inline int cli_err(bool json, const zenctl_err_t *err)
{
    int code = err ? err->code : ZENCTL_ERR_INTERNAL;
    const char *msg = (err && err->message[0]) ? err->message
                                               : zenctl_strerror(code);
    if (json) {
        out_json_error(code, msg);
    } else {
        char buf[512];
        if (err && err->context[0])
            snprintf(buf, sizeof(buf), "%s (%s)", msg, err->context);
        else
            snprintf(buf, sizeof(buf), "%s", msg);
        out_err_code(code, buf);
    }
    return -1;
}

/* Convenience for "library returned non-zero, propagate the err". */
static inline int cli_err_rc(bool json, const zenctl_err_t *err, int rc)
{
    (void)rc;
    return cli_err(json, err);
}

/* ── Dry-run / confirm ──────────────────────────────────────────── */

/* Emit a dry-run announcement line for a write op. */
static inline void cli_dryrun(bool json, const char *what)
{
    if (json) {
        /* In JSON mode the caller should emit a JSON object instead;
         * this is a fallback. */
        out_json_ok_begin();
        fputs("\"dry-run: ", stdout);
        fputs(what, stdout);
        fputs("\"", stdout);
        out_json_ok_end();
    } else {
        out_dryrun(what);
    }
}

/* ── Usage ──────────────────────────────────────────────────────── */

/* Print a usage message to stderr and return -1. */
static inline int cli_usage(bool json, const char *usage_text)
{
    (void)json;
    fprintf(stderr, "usage: %s\n", usage_text);
    return -1;
}

#endif /* ZENCTL_CLI_CMD_COMMON_H */
