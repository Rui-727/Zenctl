/* output.c - CLI output helpers (table + JSON)
 *
 * Implements the helpers declared in output.h. The JSON emitter is a
 * minimal hand-rolled one (no external dep). String escaping follows
 * RFC 8259: ", \, and control characters < 0x20 are escaped.
 */
#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <unistd.h>
#include <termios.h>
#include <sys/ioctl.h>

#include "output.h"

/* ── TTY detection ───────────────────────────────────────────────── */

static int g_tty_cached = -1;

bool out_is_tty(void)
{
    if (g_tty_cached < 0)
        g_tty_cached = isatty(STDOUT_FILENO) ? 1 : 0;
    return g_tty_cached == 1;
}

void out_color(const char *code)
{
    if (out_is_tty())
        fputs(code, stdout);
}

void out_colorln(const char *code, const char *s)
{
    if (out_is_tty())
        printf("%s%s%s\n", code, s, OUT_RESET);
    else
        printf("%s\n", s);
}

/* ── Table mode ──────────────────────────────────────────────────── */

void out_kv(const char *label, const char *value)
{
    if (out_is_tty())
        printf("%s%s:%s %s\n", OUT_DIM, label, OUT_RESET, value);
    else
        printf("%s: %s\n", label, value);
}

void out_kv_int(const char *label, int64_t value)
{
    char buf[32];
    snprintf(buf, sizeof(buf), "%lld", (long long)value);
    out_kv(label, buf);
}

/* Fixed-width table support. Tracks the maximum column width seen so
 * the caller can interleave headers and rows. */
static int  g_widths[8];
static int  g_ncols;

void out_table_reset(void)
{
    g_ncols = 0;
    for (int i = 0; i < 8; i++) g_widths[i] = 0;
}

static void track_widths(const char *cols[], int ncols)
{
    if (ncols > 8) ncols = 8;
    for (int i = 0; i < ncols; i++) {
        int w = (int)strlen(cols[i]);
        if (w > g_widths[i]) g_widths[i] = w;
    }
    if (ncols > g_ncols) g_ncols = ncols;
}

void out_table_header(const char *cols[], int ncols)
{
    track_widths(cols, ncols);
    if (out_is_tty())
        fputs(OUT_BOLD, stdout);
    for (int i = 0; i < ncols; i++) {
        fputs(cols[i], stdout);
        int pad = g_widths[i] - (int)strlen(cols[i]);
        for (int k = 0; k < pad; k++) putchar(' ');
        if (i + 1 < ncols) fputs("  ", stdout);
    }
    if (out_is_tty())
        fputs(OUT_RESET, stdout);
    putchar('\n');
}

void out_table_row(const char *cols[], int ncols)
{
    track_widths(cols, ncols);
    for (int i = 0; i < ncols; i++) {
        if (out_is_tty() && i == 0)
            fputs(OUT_GREEN, stdout);
        fputs(cols[i], stdout);
        int pad = g_widths[i] - (int)strlen(cols[i]);
        for (int k = 0; k < pad; k++) putchar(' ');
        if (out_is_tty() && i == 0)
            fputs(OUT_RESET, stdout);
        if (i + 1 < ncols) fputs("  ", stdout);
    }
    putchar('\n');
}

/* ── JSON mode ───────────────────────────────────────────────────── */

void out_json_escape(const char *s)
{
    if (!s) {
        fputs("null", stdout);
        return;
    }
    putchar('"');
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        unsigned char c = *p;
        switch (c) {
        case '"':  fputs("\\\"", stdout); break;
        case '\\': fputs("\\\\", stdout); break;
        case '\b': fputs("\\b", stdout);  break;
        case '\f': fputs("\\f", stdout);  break;
        case '\n': fputs("\\n", stdout);  break;
        case '\r': fputs("\\r", stdout);  break;
        case '\t': fputs("\\t", stdout);  break;
        default:
            if (c < 0x20)
                printf("\\u%04x", c);
            else
                putchar(c);
        }
    }
    putchar('"');
}

void out_json_ok_begin(void)
{
    fputs("{\"status\":\"ok\",\"data\":", stdout);
}

void out_json_ok_end(void)
{
    fputs("}\n", stdout);
}

void out_json_error(int code, const char *message)
{
    fputs("{\"status\":\"error\",\"error\":{\"code\":", stdout);
    printf("%d", code);
    fputs(",\"message\":", stdout);
    out_json_escape(message ? message : "");
    fputs("}}\n", stdout);
}

static void emit_comma(bool *first)
{
    if (*first) {
        *first = false;
    } else {
        putchar(',');
    }
}

void out_json_field_string(bool *first, const char *key, const char *val)
{
    emit_comma(first);
    out_json_escape(key);
    putchar(':');
    out_json_escape(val);
}

void out_json_field_int(bool *first, const char *key, int64_t val)
{
    emit_comma(first);
    out_json_escape(key);
    printf(":%lld", (long long)val);
}

void out_json_field_bool(bool *first, const char *key, bool val)
{
    emit_comma(first);
    out_json_escape(key);
    printf(":%s", val ? "true" : "false");
}

void out_json_field_double(bool *first, const char *key, double val)
{
    emit_comma(first);
    out_json_escape(key);
    printf(":%g", val);
}

void out_json_field_object_begin(bool *first, const char *key)
{
    emit_comma(first);
    out_json_escape(key);
    fputs(":{", stdout);
    *first = true;   /* reset for the inner object */
}

void out_json_field_array_begin(bool *first, const char *key)
{
    emit_comma(first);
    out_json_escape(key);
    fputs(":[", stdout);
    *first = true;   /* reset for the inner array */
}

void out_json_object_end(void)
{
    putchar('}');
}

void out_json_array_end(void)
{
    putchar(']');
}

void out_json_separator(bool *first)
{
    emit_comma(first);
}

void out_json_open_object(void)
{
    putchar('{');
}

void out_json_close_object(void)
{
    putchar('}');
}

void out_json_open_array(void)
{
    putchar('[');
}

void out_json_close_array(void)
{
    putchar(']');
}

void out_json_array_string(bool *first, const char *val)
{
    emit_comma(first);
    out_json_escape(val);
}

void out_json_array_int(bool *first, int64_t val)
{
    emit_comma(first);
    printf("%lld", (long long)val);
}

/* ── Error helpers ───────────────────────────────────────────────── */

void out_err(const char *msg)
{
    if (out_is_tty())
        fprintf(stderr, "%serror:%s %s\n", OUT_RED, OUT_RESET, msg);
    else
        fprintf(stderr, "error: %s\n", msg);
}

void out_err_code(int code, const char *msg)
{
    /* Include the symbolic code name for diagnostics. */
    const char *name = "UNKNOWN";
    switch (code) {
    case 1: name = "EPERM"; break;
    case 2: name = "ENOENT"; break;
    case 3: name = "EINVAL"; break;
    case 4: name = "EIO"; break;
    case 5: name = "ENOTSUP"; break;
    case 6: name = "ERANGE"; break;
    case 7: name = "ENOMEM"; break;
    case 8: name = "INTERNAL"; break;
    default: break;
    }
    if (out_is_tty())
        fprintf(stderr, "%serror [%s]:%s %s\n", OUT_RED, name, OUT_RESET, msg);
    else
        fprintf(stderr, "error [%s]: %s\n", name, msg);
}

void out_dryrun(const char *msg)
{
    if (out_is_tty())
        printf("%s[DRY-RUN]%s %s\n", OUT_YELLOW, OUT_RESET, msg);
    else
        printf("[DRY-RUN] %s\n", msg);
}
