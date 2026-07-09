/* mock_preload.c - LD_PRELOAD shim that redirects sysfs/procfs paths.
 *
 * libzenctl's io.c already redirects /sys/... and /proc/... paths
 * through ZENCTL_SYSFS_PREFIX, but several domain implementations
 * bypass io.c (they call access(), opendir(), readlink(), open()
 * directly). This shim intercepts those libc entry points so the
 * library sees the mock fixture tree no matter which libc call it
 * uses.
 *
 * The shim is a no-op when ZENCTL_SYSFS_PREFIX is unset (production
 * builds, smoke tests against real /sys).
 *
 * Linked as a separate shared object (libzenctl_mockpreload.so) and
 * injected via LD_PRELOAD by `make test`.
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <sys/types.h>

/* Linux efivarfs magic, used by lib/firmware/firmware.c to gate EFI
 * access. We lie about it when the lookup is for the efivars path so
 * the EFI tests can run against a tmpfs-backed fixture. */
#define ZENCTL_EFIVARFS_MAGIC 0xde5e81e4u

static const char *get_prefix(void)
{
    return getenv("ZENCTL_SYSFS_PREFIX");
}

/* If `path` starts with /sys/ or /proc/, copy "<prefix><path>" into
 * `out` and return 1. Otherwise return 0 and leave `out` untouched.
 * Returns 0 (no redirect) when the prefix is unset or the result
 * wouldn't fit in `sz`. */
static int redirect_path(const char *path, char *out, size_t sz)
{
    if (!path) return 0;
    const char *p = get_prefix();
    if (!p || !p[0]) return 0;
    if (strncmp(path, "/sys/", 5) == 0 || strcmp(path, "/sys") == 0 ||
        strncmp(path, "/proc/", 6) == 0 || strcmp(path, "/proc") == 0) {
        int n = snprintf(out, sz, "%s%s", p, path);
        if (n < 0 || (size_t)n >= sz) return 0;
        return 1;
    }
    return 0;
}

/* ---- access ---- */
typedef int (*access_fn)(const char *, int);
int access(const char *path, int mode)
{
    static access_fn real;
    if (!real) real = (access_fn)dlsym(RTLD_NEXT, "access");
    char buf[4096];
    if (redirect_path(path, buf, sizeof(buf))) return real(buf, mode);
    return real(path, mode);
}

/* ---- fopen / fopen64 ---- */
typedef FILE *(*fopen_fn)(const char *, const char *);
FILE *fopen(const char *path, const char *mode)
{
    static fopen_fn real;
    if (!real) real = (fopen_fn)dlsym(RTLD_NEXT, "fopen");
    char buf[4096];
    if (redirect_path(path, buf, sizeof(buf))) return real(buf, mode);
    return real(path, mode);
}

#ifdef __LP64__
typedef FILE *(*fopen64_fn)(const char *, const char *);
FILE *fopen64(const char *path, const char *mode)
{
    static fopen64_fn real;
    if (!real) real = (fopen64_fn)dlsym(RTLD_NEXT, "fopen64");
    char buf[4096];
    if (redirect_path(path, buf, sizeof(buf))) return real(buf, mode);
    return real(path, mode);
}
#endif

/* ---- opendir ---- */
typedef DIR *(*opendir_fn)(const char *);
DIR *opendir(const char *path)
{
    static opendir_fn real;
    if (!real) real = (opendir_fn)dlsym(RTLD_NEXT, "opendir");
    char buf[4096];
    if (redirect_path(path, buf, sizeof(buf))) return real(buf);
    return real(path);
}

/* ---- readlink ---- */
typedef ssize_t (*readlink_fn)(const char *, char *, size_t);
ssize_t readlink(const char *path, char *buf, size_t sz)
{
    static readlink_fn real;
    if (!real) real = (readlink_fn)dlsym(RTLD_NEXT, "readlink");
    char tmp[4096];
    if (redirect_path(path, tmp, sizeof(tmp))) return real(tmp, buf, sz);
    return real(path, buf, sz);
}

/* ---- stat / lstat ---- */
typedef int (*stat_fn)(const char *, struct stat *);
int stat(const char *path, struct stat *st)
{
    static stat_fn real;
    if (!real) real = (stat_fn)dlsym(RTLD_NEXT, "stat");
    char buf[4096];
    if (redirect_path(path, buf, sizeof(buf))) return real(buf, st);
    return real(path, st);
}
int lstat(const char *path, struct stat *st)
{
    static stat_fn real;
    if (!real) real = (stat_fn)dlsym(RTLD_NEXT, "lstat");
    char buf[4096];
    if (redirect_path(path, buf, sizeof(buf))) return real(buf, st);
    return real(path, st);
}

#ifdef __LP64__
typedef int (*stat64_fn)(const char *, struct stat64 *);
int stat64(const char *path, struct stat64 *st)
{
    static stat64_fn real;
    if (!real) real = (stat64_fn)dlsym(RTLD_NEXT, "stat64");
    char buf[4096];
    if (redirect_path(path, buf, sizeof(buf))) return real(buf, st);
    return real(path, st);
}
int lstat64(const char *path, struct stat64 *st)
{
    static stat64_fn real;
    if (!real) real = (stat64_fn)dlsym(RTLD_NEXT, "lstat64");
    char buf[4096];
    if (redirect_path(path, buf, sizeof(buf))) return real(buf, st);
    return real(path, st);
}
#endif

/* ---- statfs: redirect, and lie about magic for efivars ---- */
typedef int (*statfs_fn)(const char *, struct statfs *);
int statfs(const char *path, struct statfs *sf)
{
    static statfs_fn real;
    if (!real) real = (statfs_fn)dlsym(RTLD_NEXT, "statfs");
    char buf[4096];
    if (redirect_path(path, buf, sizeof(buf))) {
        int rc = real(buf, sf);
        if (rc == 0) {
            /* firmware.c checks f_type == EFIVARFS_MAGIC to decide
             * whether efivarfs is mounted. The mock lives on tmpfs,
             * so lie about the magic for the efivars path only. */
            if (strcmp(path, "/sys/firmware/efi/efivars") == 0)
                sf->f_type = ZENCTL_EFIVARFS_MAGIC;
        }
        return rc;
    }
    return real(path, sf);
}

/* ---- open / open64 / openat ---- */
typedef int (*open_fn)(const char *, int, ...);
int open(const char *path, int flags, ...)
{
    static open_fn real;
    if (!real) real = (open_fn)dlsym(RTLD_NEXT, "open");
    mode_t mode = 0;
    if (flags & O_CREAT) {
        va_list ap; va_start(ap, flags);
        mode = va_arg(ap, mode_t);
        va_end(ap);
    }
    char buf[4096];
    if (redirect_path(path, buf, sizeof(buf))) return real(buf, flags, mode);
    return real(path, flags, mode);
}

#ifdef __LP64__
typedef int (*open64_fn)(const char *, int, ...);
int open64(const char *path, int flags, ...)
{
    static open64_fn real;
    if (!real) real = (open64_fn)dlsym(RTLD_NEXT, "open64");
    mode_t mode = 0;
    if (flags & O_CREAT) {
        va_list ap; va_start(ap, flags);
        mode = va_arg(ap, mode_t);
        va_end(ap);
    }
    char buf[4096];
    if (redirect_path(path, buf, sizeof(buf))) return real(buf, flags, mode);
    return real(path, flags, mode);
}
#endif

typedef int (*openat_fn)(int, const char *, int, ...);
int openat(int dirfd, const char *path, int flags, ...)
{
    static openat_fn real;
    if (!real) real = (openat_fn)dlsym(RTLD_NEXT, "openat");
    mode_t mode = 0;
    if (flags & O_CREAT) {
        va_list ap; va_start(ap, flags);
        mode = va_arg(ap, mode_t);
        va_end(ap);
    }
    char buf[4096];
    if (redirect_path(path, buf, sizeof(buf)))
        return real(dirfd, buf, flags, mode);
    return real(dirfd, path, flags, mode);
}
