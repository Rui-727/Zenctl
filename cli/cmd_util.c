/* cmd_util.c - shared helpers for domain command implementations */
#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <stdint.h>
#include <ctype.h>
#include <dirent.h>

#include "zenctl/zenctl.h"
#include "output.h"
#include "cmd_util.h"

void cmd_print_err(bool json, const zenctl_err_t *err, const char *fallback)
{
    const char *msg = (err && err->message[0]) ? err->message : fallback;
    char buf[768];
    if (err && err->context[0]) {
        snprintf(buf, sizeof(buf), "%s: %s", msg, err->context);
        msg = buf;
    }
    /* CLI-detected errors (err == NULL) default to EINVAL; library
     * errors propagate the library's code. */
    int code = err ? err->code : ZENCTL_ERR_EINVAL;
    if (json)
        out_json_error(code, msg);
    else
        out_err_code(code, msg);
}

const char *cmd_err_name(int code)
{
    switch (code) {
    case ZENCTL_OK:           return "OK";
    case ZENCTL_ERR_EPERM:    return "EPERM";
    case ZENCTL_ERR_ENOENT:   return "ENOENT";
    case ZENCTL_ERR_EINVAL:   return "EINVAL";
    case ZENCTL_ERR_EIO:      return "EIO";
    case ZENCTL_ERR_ENOTSUP:  return "ENOTSUP";
    case ZENCTL_ERR_ERANGE:   return "ERANGE";
    case ZENCTL_ERR_NOMEM:    return "ENOMEM";
    case ZENCTL_ERR_INTERNAL: return "INTERNAL";
    default:                  return "UNKNOWN";
    }
}

void cmd_format_hz(int64_t hz, char *buf, size_t bufsz)
{
    if (hz >= 1000000000) {
        double g = (double)hz / 1e9;
        snprintf(buf, bufsz, "%.2f GHz", g);
    } else if (hz >= 1000000) {
        double m = (double)hz / 1e6;
        snprintf(buf, bufsz, "%.2f MHz", m);
    } else if (hz >= 1000) {
        double k = (double)hz / 1e3;
        snprintf(buf, bufsz, "%.2f kHz", k);
    } else {
        snprintf(buf, bufsz, "%lld Hz", (long long)hz);
    }
}

int cmd_count_cpus(void)
{
    DIR *d = opendir("/sys/devices/system/cpu");
    if (!d) return 1;
    int n = 0;
    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        if (strncmp(de->d_name, "cpu", 3) != 0) continue;
        char *e = NULL;
        long idx = strtol(de->d_name + 3, &e, 10);
        if (e == de->d_name + 3 || *e != '\0' || idx < 0) continue;
        n++;
    }
    closedir(d);
    return n > 0 ? n : 1;
}

int cmd_opt_str(int argc, char **argv, const char *name, const char **out_value)
{
    for (int i = 0; i < argc; i++) {
        if (strcmp(argv[i], name) == 0) {
            if (i + 1 >= argc)
                return -1;
            *out_value = argv[i + 1];
            return 1;
        }
        /* Also accept --name=value form */
        size_t nl = strlen(name);
        if (strncmp(argv[i], name, nl) == 0 && argv[i][nl] == '=') {
            *out_value = argv[i] + nl + 1;
            return 1;
        }
    }
    return 0;
}

int cmd_opt_int(int argc, char **argv, const char *name, long *out)
{
    const char *v = NULL;
    int rc = cmd_opt_str(argc, argv, name, &v);
    if (rc <= 0) return rc;
    char *e = NULL;
    long n = strtol(v, &e, 10);
    if (e == v || *e != '\0') return -1;
    *out = n;
    return 1;
}

int cmd_positional_count(int argc, char **argv)
{
    int n = 0;
    for (int k = 0; k < argc; k++) {
        if (argv[k][0] == '-') {
            /* Skip this option; if it's not in --opt=val form, also
             * skip the next arg as the value. By the time we get
             * here main.c has already filtered out the boolean
             * global flags, so the remaining --foo options all take
             * a value. */
            if (strchr(argv[k], '=') == NULL && k + 1 < argc)
                k++;
            continue;
        }
        n++;
    }
    return n;
}

const char *cmd_positional(int argc, char **argv, int i)
{
    int n = 0;
    for (int k = 0; k < argc; k++) {
        if (argv[k][0] == '-') {
            if (strchr(argv[k], '=') == NULL && k + 1 < argc)
                k++;
            continue;
        }
        if (n == i) return argv[k];
        n++;
    }
    return NULL;
}

int cmd_list_dir(const char *path, cmd_name_filter_fn filter,
                 char ***out_list, int *out_count)
{
    *out_list = NULL;
    *out_count = 0;
    DIR *d = opendir(path);
    if (!d) return -1;

    int cap = 16, n = 0;
    char **arr = malloc(cap * sizeof(char *));
    if (!arr) { closedir(d); return -1; }

    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        if (strcmp(de->d_name, ".") == 0)  continue;
        if (strcmp(de->d_name, "..") == 0) continue;
        if (filter && !filter(de->d_name)) continue;
        if (n == cap) {
            cap *= 2;
            char **na = realloc(arr, cap * sizeof(char *));
            if (!na) {
                for (int i = 0; i < n; i++) free(arr[i]);
                free(arr);
                closedir(d);
                return -1;
            }
            arr = na;
        }
        char *copy = strdup(de->d_name);
        if (!copy) {
            for (int i = 0; i < n; i++) free(arr[i]);
            free(arr);
            closedir(d);
            return -1;
        }
        arr[n++] = copy;
    }
    closedir(d);
    *out_list = arr;
    *out_count = n;
    return n;
}

void cmd_free_list(char **list, int count)
{
    if (!list) return;
    for (int i = 0; i < count; i++) free(list[i]);
    free(list);
}
