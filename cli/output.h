/* output.h - CLI output helpers (table + JSON)
 *
 * Two modes:
 *   - table: human-readable, lsblk/lspci styled, colored when TTY
 *   - JSON : {"status":"ok","data":...} / {"status":"error","error":{...}}
 *
 * Helpers either print immediately (table mode) or build a JSON object
 * stream. The JSON helpers do proper string escaping. The caller is
 * responsible for the surrounding structure.
 */
#ifndef ZENCTL_CLI_OUTPUT_H
#define ZENCTL_CLI_OUTPUT_H

#include <stdbool.h>
#include <stdint.h>

/* ── Mode / TTY ──────────────────────────────────────────────────── */

bool out_is_tty(void);

/* ANSI color codes. out_color() emits them only when stdout is a TTY. */
#define OUT_DIM    "\033[2m"
#define OUT_GREEN  "\033[32m"
#define OUT_RED    "\033[31m"
#define OUT_YELLOW "\033[33m"
#define OUT_BOLD   "\033[1m"
#define OUT_RESET  "\033[0m"

void out_color(const char *code);     /* emit code if TTY */
void out_colorln(const char *code, const char *s); /* emit code+s+reset+\n if TTY, else s+\n */

/* ── Table mode ──────────────────────────────────────────────────── */

/* Print "label: value\n" with the label dim and value plain/green. */
void out_kv(const char *label, const char *value);
void out_kv_int(const char *label, int64_t value);

/* Print a simple two-column table. cols[] is a NULL-terminated list of
 * column headers in alternating (label, value) pairs, e.g.
 *   out_kv_row("CPU", "0", "GOVERNOR", "powersave", NULL);
 * Pads the label column to a fixed width and joins values with spaces. */
void out_kv_row(const char *first_label, ...);

/* Aligned header + row helpers for fixed column tables.
 *   const char *h[] = {"CPU","GOVERNOR","FREQ"};
 *   out_table_header(h, 3);
 *   const char *r[] = {"0","powersave","3600000"};
 *   out_table_row(r, 3);
 * Column widths are recomputed each call from the longest string seen
 * so far across header + rows in the same logical table. The caller is
 * expected to call out_table_reset() before each new table. */
void out_table_reset(void);
void out_table_header(const char *cols[], int ncols);
void out_table_row(const char *cols[], int ncols);

/* ── JSON mode ───────────────────────────────────────────────────── */

/* Emit a JSON-escaped string with surrounding double quotes. */
void out_json_escape(const char *s);

/* Standard envelope helpers.
 *   out_json_ok_begin();   ->  {"status":"ok","data":
 *   <emit data>
 *   out_json_ok_end();     ->  }\n
 *
 * For errors:
 *   out_json_error(code, msg);  ->  {"status":"error","error":{"code":N,"message":"..."}}\n
 */
void out_json_ok_begin(void);
void out_json_ok_end(void);
void out_json_error(int code, const char *message);

/* JSON value emitters. Each one prints a leading comma if *first is
 * false, then the key:value pair, and sets *first=false. Use them
 * inside an object opened by out_json_ok_begin() (or a nested object).
 *
 *   bool first = true;
 *   out_json_ok_begin();
 *   out_json_field_string(&first, "cpu", "0");
 *   out_json_field_int(&first, "freq", 3600000);
 *   out_json_field_bool(&first, "online", true);
 *   out_json_ok_end();
 */
void out_json_field_string(bool *first, const char *key, const char *val);
void out_json_field_int(bool *first, const char *key, int64_t val);
void out_json_field_bool(bool *first, const char *key, bool val);
void out_json_field_double(bool *first, const char *key, double val);

/* Open / close nested objects and arrays inside the data envelope.
 *   out_json_field_object_begin(&first, "link")
 *     -> , "link":{
 *   out_json_object_end()
 *     -> }
 *   out_json_field_array_begin(&first, "cpus")
 *     -> , "cpus":[
 *   out_json_array_end()
 *     -> ]
 */
void out_json_field_object_begin(bool *first, const char *key);
void out_json_field_array_begin(bool *first, const char *key);
void out_json_object_end(void);
void out_json_array_end(void);

/* Low-level primitives for composing arbitrary JSON structures.
 * out_json_separator() emits a comma if *first is false and sets
 * *first=false. Use it between array elements or between fields you
 * manage manually. */
void out_json_separator(bool *first);
void out_json_open_object(void);   /* { */
void out_json_close_object(void);  /* } */
void out_json_open_array(void);    /* [ */
void out_json_close_array(void);   /* ] */

/* Inside an array, the comma goes between elements. Track first with
 * a local bool. */
void out_json_array_string(bool *first, const char *val);
void out_json_array_int(bool *first, int64_t val);

/* ── Error helpers (table mode only) ─────────────────────────────── */

/* Print an error to stderr (no color, no JSON). */
void out_err(const char *msg);
/* Print an error to stderr with the zenctl error code mapped to text. */
void out_err_code(int code, const char *msg);

/* ── Dry-run helper ──────────────────────────────────────────────── */

/* Print a "[DRY-RUN] <msg>" line. In JSON mode the caller is expected
 * to emit a JSON object instead. */
void out_dryrun(const char *msg);

#endif /* ZENCTL_CLI_OUTPUT_H */
