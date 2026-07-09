/* rfkill.c - private rfkill helpers for the USB / BT / wireless modules.
 *
 * Wraps /sys/class/rfkill/rfkill<N>/{name,type,soft}. See
 * docs/KERNEL_USB_BT_FW.md section 3.9. Not part of the public API.
 */
#define _POSIX_C_SOURCE 200809L

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "zenctl/internal.h"
#include "rfkill.h"

#define RFKILL_BASE "/sys/class/rfkill"

/* Heap-allocated "<dir>/<name>". Caller frees. */
static char *path_join(const char *dir, const char *name)
{
    size_t dlen = strlen(dir), nlen = strlen(name);
    int sep = (dlen > 0 && dir[dlen - 1] != '/');
    char *s = malloc(dlen + nlen + (sep ? 2 : 1));
    if (!s) return NULL;
    memcpy(s, dir, dlen);
    size_t off = dlen;
    if (sep) s[off++] = '/';
    memcpy(s + off, name, nlen);
    off += nlen;
    s[off] = '\0';
    return s;
}

/* NULL-terminated array of entry names (heap-allocated). Caller frees
 * each entry and the array. */
static char **list_dir(const char *dir, zenctl_err_t *err)
{
    DIR *d = opendir(dir);
    if (!d) {
        zenctl__set_err(err, zenctl__errno_to_code(errno),
                        strerror(errno), dir);
        return NULL;
    }
    size_t cap = 16, n = 0;
    char **arr = calloc(cap, sizeof(char *));
    if (!arr) {
        closedir(d);
        zenctl__set_err(err, ZENCTL_ERR_NOMEM, "out of memory", dir);
        return NULL;
    }
    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0)
            continue;
        char *copy = strdup(de->d_name);
        if (!copy) {
            for (size_t i = 0; i < n; i++) free(arr[i]);
            free(arr);
            closedir(d);
            zenctl__set_err(err, ZENCTL_ERR_NOMEM, "out of memory", dir);
            return NULL;
        }
        if (n + 1 >= cap) {
            size_t ncap = cap * 2;
            char **narr = realloc(arr, ncap * sizeof(char *));
            if (!narr) {
                free(copy);
                for (size_t i = 0; i < n; i++) free(arr[i]);
                free(arr);
                closedir(d);
                zenctl__set_err(err, ZENCTL_ERR_NOMEM, "out of memory", dir);
                return NULL;
            }
            arr = narr;
            cap = ncap;
        }
        arr[n++] = copy;
    }
    closedir(d);
    if (n + 1 > cap) {
        char **narr = realloc(arr, (n + 1) * sizeof(char *));
        if (!narr) {
            for (size_t i = 0; i < n; i++) free(arr[i]);
            free(arr);
            zenctl__set_err(err, ZENCTL_ERR_NOMEM, "out of memory", dir);
            return NULL;
        }
        arr = narr;
    }
    arr[n] = NULL;
    return arr;
}

int zenctl_rfkill_find(const char *want_name, const char *want_type, zenctl_err_t *err)
{
    char **names = list_dir(RFKILL_BASE, err);
    if (!names) return -1;

    int found = -1;
    for (size_t i = 0; names[i]; i++) {
        const char *entry = names[i];
        if (strncmp(entry, "rfkill", 6) != 0) continue;
        char *end = NULL;
        long idx = strtol(entry + 6, &end, 10);
        if (!end || *end != '\0' || idx < 0) continue;

        char base[256];
        snprintf(base, sizeof(base), "%s/%s", RFKILL_BASE, entry);

        if (want_type) {
            char *tpath = path_join(base, "type");
            if (!tpath) continue;
            char tval[64] = {0};
            int rc = zenctl__read_file_string(tpath, tval, sizeof(tval), NULL);
            free(tpath);
            if (rc != 0) continue;
            if (strcmp(tval, want_type) != 0) continue;
        }

        if (want_name) {
            char *npath = path_join(base, "name");
            if (!npath) continue;
            char nval[128] = {0};
            int rc = zenctl__read_file_string(npath, nval, sizeof(nval), NULL);
            free(npath);
            if (rc != 0) continue;
            if (strcmp(nval, want_name) != 0) continue;
        }

        found = (int)idx;
        break;
    }

    for (size_t i = 0; names[i]; i++) free(names[i]);
    free(names);

    if (found < 0) {
        char ctx[256];
        snprintf(ctx, sizeof(ctx),
                 "zenctl_rfkill_find(name=%s type=%s)",
                 want_name ? want_name : "*",
                 want_type ? want_type : "*");
        zenctl__set_err(err, ZENCTL_ERR_ENOENT,
                        "no matching rfkill device", ctx);
    }
    return found;
}

int zenctl_rfkill_get_soft(int idx, bool *out, zenctl_err_t *err)
{
    char path[128];
    snprintf(path, sizeof(path), "%s/rfkill%d/soft", RFKILL_BASE, idx);
    char buf[16] = {0};
    if (zenctl__read_file_string(path, buf, sizeof(buf), err) != 0)
        return -1;
    *out = (atoi(buf) != 0);
    return 0;
}

int zenctl_rfkill_set_soft(int idx, bool blocked, zenctl_err_t *err)
{
    char path[128];
    snprintf(path, sizeof(path), "%s/rfkill%d/soft", RFKILL_BASE, idx);
    return zenctl__write_file_string(path, blocked ? "1" : "0", err);
}
