/* util.c - private filesystem helpers for the USB / BT / wireless modules. */
#define _POSIX_C_SOURCE 200809L

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "zenctl/internal.h"
#include "util.h"

char *zenctl_util_path_join(const char *dir, const char *name)
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

char **zenctl_util_list_dir(const char *dir, zenctl_err_t *err)
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
