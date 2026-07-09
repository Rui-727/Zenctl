/* mock_sysfs.c - in-process sysfs/procfs fixture manager.
 *
 * Creates a temp directory under /tmp, exports it via the
 * ZENCTL_SYSFS_PREFIX env var, and lets tests populate it with files
 * and directories that mimic the kernel surface. The library's io.c
 * resolves /sys/... and /proc/... paths against the prefix; the
 * LD_PRELOAD shim (mock_preload.c) covers the library functions that
 * call access()/opendir()/readlink()/open() directly.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "mock_sysfs.h"

static char g_prefix[4096] = {0};

/* Forward declarations of helpers. */
static int mkdir_p(const char *path);

static void join3(const char *a, const char *b, char *out, size_t sz)
{
    /* Join a + "/" + b into out, collapsing any duplicate slash between
     * them. The caller passes a fixed-size buffer; if the result would
     * be truncated, we silently truncate (callers use 4096-byte buffers
     * for paths that are <200 chars in practice). */
    size_t alen = strlen(a);
    size_t blen = strlen(b);
    int need_sep = (alen > 0 && a[alen - 1] != '/');
    size_t total = alen + (need_sep ? 1 : 0) + blen;
    if (total >= sz) total = sz - 1;
    size_t off = 0;
    if (off < total) {
        size_t n = (alen < total - off) ? alen : total - off;
        memcpy(out + off, a, n); off += n;
    }
    if (need_sep && off < total) {
        out[off++] = '/';
    }
    if (off < total) {
        size_t n = (blen < total - off) ? blen : total - off;
        memcpy(out + off, b, n); off += n;
    }
    out[off] = '\0';
}

/* Split `rel_path` into a parent directory and a final component,
 * writing the parent (relative to the prefix) into parent_buf. Returns
 * the length of the parent string, or -1 if rel_path has no slash. */
static int split_parent(const char *rel_path, char *parent_buf, size_t bufsz)
{
    const char *slash = strrchr(rel_path, '/');
    if (!slash) return -1;
    size_t n = (size_t)(slash - rel_path);
    if (n >= bufsz) return -1;
    memcpy(parent_buf, rel_path, n);
    parent_buf[n] = '\0';
    return (int)n;
}

int mock_sysfs_init(void)
{
    char tmpl[64];
    snprintf(tmpl, sizeof(tmpl), "/tmp/zenctl-mock-XXXXXX");
    char *dir = mkdtemp(tmpl);
    if (!dir) return -1;
    snprintf(g_prefix, sizeof(g_prefix), "%s", dir);
    if (setenv("ZENCTL_SYSFS_PREFIX", g_prefix, 1) != 0)
        return -1;
    return 0;
}

static void rmtree(const char *path)
{
    /* Best-effort recursive delete using system("rm -rf"). The path is
     * always a fresh mkdtemp() directory we created, so this is safe. */
    char cmd[8192];
    snprintf(cmd, sizeof(cmd), "rm -rf -- '%s'", path);
    int rc = system(cmd);
    (void)rc;
}

void mock_sysfs_cleanup(void)
{
    if (g_prefix[0]) {
        rmtree(g_prefix);
        g_prefix[0] = '\0';
    }
    unsetenv("ZENCTL_SYSFS_PREFIX");
}

const char *mock_sysfs_prefix(void)
{
    return g_prefix[0] ? g_prefix : NULL;
}

static int mkdir_p_under_prefix(const char *rel_dir)
{
    char full[4096];
    if (!g_prefix[0]) { errno = ENOENT; return -1; }
    if (rel_dir[0] == '\0') return 0;
    join3(g_prefix, rel_dir, full, sizeof(full));
    return mkdir_p(full);
}

/* mkdir -p, relative or absolute path. */
static int mkdir_p(const char *path)
{
    char tmp[4096];
    snprintf(tmp, sizeof(tmp), "%s", path);
    size_t len = strlen(tmp);
    if (len == 0) return 0;
    if (tmp[len - 1] == '/') tmp[len - 1] = '\0';

    /* Walk the path creating each component. */
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            if (mkdir(tmp, 0755) != 0 && errno != EEXIST) return -1;
            *p = '/';
        }
    }
    if (mkdir(tmp, 0755) != 0 && errno != EEXIST) return -1;
    return 0;
}

int mock_sysfs_create_file_bin(const char *rel_path,
                               const void *data, size_t len)
{
    if (!g_prefix[0]) { errno = ENOENT; return -1; }
    char parent[4096];
    if (split_parent(rel_path, parent, sizeof(parent)) >= 0) {
        if (mkdir_p_under_prefix(parent) != 0) return -1;
    }
    char full[4096];
    join3(g_prefix, rel_path, full, sizeof(full));
    FILE *f = fopen(full, "w");
    if (!f) return -1;
    if (len > 0 && fwrite(data, 1, len, f) != len) {
        int e = errno; fclose(f); errno = e; return -1;
    }
    if (fclose(f) != 0) return -1;
    return 0;
}

int mock_sysfs_create_file(const char *rel_path, const char *content)
{
    size_t n = content ? strlen(content) : 0;
    return mock_sysfs_create_file_bin(rel_path, content ? content : "", n);
}

int mock_sysfs_create_dir(const char *rel_path)
{
    if (!g_prefix[0]) { errno = ENOENT; return -1; }
    return mkdir_p_under_prefix(rel_path);
}

int mock_sysfs_create_symlink(const char *rel_path, const char *target)
{
    if (!g_prefix[0]) { errno = ENOENT; return -1; }
    char parent[4096];
    if (split_parent(rel_path, parent, sizeof(parent)) >= 0) {
        if (mkdir_p_under_prefix(parent) != 0) return -1;
    }
    char full[4096];
    join3(g_prefix, rel_path, full, sizeof(full));
    if (symlink(target, full) != 0) return -1;
    return 0;
}

int mock_sysfs_read_file(const char *rel_path, char *buf, size_t sz)
{
    if (!g_prefix[0] || sz == 0) { errno = EINVAL; return -1; }
    char full[4096];
    join3(g_prefix, rel_path, full, sizeof(full));
    FILE *f = fopen(full, "r");
    if (!f) return -1;
    size_t n = fread(buf, 1, sz - 1, f);
    int e = errno;
    int ferr = ferror(f);
    fclose(f);
    if (ferr) { errno = e; return -1; }
    buf[n] = '\0';
    return (int)n;
}

int mock_sysfs_read_file_bin(const char *rel_path,
                             char **out_buf, size_t *out_size)
{
    if (!g_prefix[0]) { errno = EINVAL; return -1; }
    char full[4096];
    join3(g_prefix, rel_path, full, sizeof(full));
    FILE *f = fopen(full, "r");
    if (!f) return -1;

    size_t cap = 256, len = 0;
    char *buf = malloc(cap);
    if (!buf) { fclose(f); errno = ENOMEM; return -1; }
    for (;;) {
        if (len == cap) {
            size_t ncap = cap * 2;
            char *nb = realloc(buf, ncap);
            if (!nb) { free(buf); fclose(f); errno = ENOMEM; return -1; }
            buf = nb; cap = ncap;
        }
        size_t r = fread(buf + len, 1, cap - len, f);
        if (r == 0) break;
        len += r;
    }
    int e = errno;
    int ferr = ferror(f);
    fclose(f);
    if (ferr) { free(buf); errno = e; return -1; }

    *out_buf = buf;
    *out_size = len;
    return 0;
}

int mock_sysfs_remove(const char *rel_path)
{
    if (!g_prefix[0]) { errno = ENOENT; return -1; }
    char full[4096];
    join3(g_prefix, rel_path, full, sizeof(full));
    /* Try unlink first (works for files and symlinks); fall back to rmdir. */
    if (unlink(full) == 0) return 0;
    if (errno == EISDIR) return rmdir(full);
    return -1;
}
