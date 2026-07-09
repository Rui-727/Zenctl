/* cmd_util.h - small shared helpers for domain command implementations
 *
 * These are internal to the CLI; not part of any public surface.
 */
#ifndef ZENCTL_CLI_CMD_UTIL_H
#define ZENCTL_CLI_CMD_UTIL_H

#include <stdbool.h>
#include <stdint.h>
#include <dirent.h>

#include "zenctl/zenctl.h"

/* Print a zenctl error in the right mode. In JSON mode emits a JSON
 * error object to stdout; in table mode prints to stderr. `fallback`
 * is used if err is NULL or has an empty message. */
void cmd_print_err(bool json, const zenctl_err_t *err, const char *fallback);

/* Convert a zenctl_err_t code into a short symbolic name (EPERM, ENOENT,
 * EINVAL, EIO, ENOTSUP, ERANGE, NOMEM, INTERNAL, or UNKNOWN). */
const char *cmd_err_name(int code);

/* Format a frequency in Hz as a friendly human string: "3.60 GHz",
 * "2.40 MHz", "1.00 kHz", or "100 Hz". */
void cmd_format_hz(int64_t hz, char *buf, size_t bufsz);

/* Open /sys/devices/system/cpu and count cpuN entries. Returns 1 if
 * the directory cannot be opened (best-effort). */
int cmd_count_cpus(void);

/* Scan argv[0..argc) for the option `name`. If found, returns the
 * following argument via *out_value and returns 1. Returns 0 if not
 * found. Returns -1 if `name` is the last arg (no value). */
int cmd_opt_str(int argc, char **argv, const char *name, const char **out_value);

/* Scan argv for `name` taking an int value. *out is set to the parsed
 * integer. Returns 1 if found, 0 if not present, -1 on parse error or
 * missing value. */
int cmd_opt_int(int argc, char **argv, const char *name, long *out);

/* Count positional args (those not starting with --). */
int cmd_positional_count(int argc, char **argv);

/* Get the i-th positional argument (0-based) or NULL if out of range. */
const char *cmd_positional(int argc, char **argv, int i);

/* Scan a directory and call cb(name) for each entry that is a regular
 * directory entry (skips "." and ".."). Returns the count of accepted
 * entries or -1 on opendir failure. The `filter` predicate (if non-NULL)
 * returns 1 to accept, 0 to skip. */
typedef int (*cmd_name_filter_fn)(const char *name);
int cmd_list_dir(const char *path, cmd_name_filter_fn filter,
                 char ***out_list, int *out_count);

/* Free a list returned by cmd_list_dir. */
void cmd_free_list(char **list, int count);

#endif /* ZENCTL_CLI_CMD_UTIL_H */
