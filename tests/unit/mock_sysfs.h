/* mock_sysfs.h - in-process sysfs/procfs fixture manager
 *
 * Creates a temp directory, exports its path via the ZENCTL_SYSFS_PREFIX
 * env var (so libzenctl's io.c redirects /sys/... and /proc/... reads
 * to it), and lets tests drop fixture files and directories inside.
 *
 * Note: a few libzenctl domains bypass io.c (they call access(),
 * opendir(), readlink(), open() directly). Those calls are redirected
 * by mock_preload.c, a LD_PRELOAD shim that the Makefile injects at
 * test time. mock_sysfs.c itself only manages the fixture tree and
 * the env var.
 */
#ifndef ZENCTL_TEST_MOCK_SYSFS_H
#define ZENCTL_TEST_MOCK_SYSFS_H

/* Create a fresh temp directory and set ZENCTL_SYSFS_PREFIX to it.
 * Returns 0 on success, -1 on error (errno set). Idempotent: calling
 * it twice without an intervening cleanup() is undefined. */
int mock_sysfs_init(void);

/* Tear down the fixture tree and clear ZENCTL_SYSFS_PREFIX. Safe to
 * call after a failed init(). */
void mock_sysfs_cleanup(void);

/* Return the prefix path currently in use (NULL if not initialised).
 * The string is owned by the mock; do not free. */
const char *mock_sysfs_prefix(void);

/* Create a file inside the mock tree. `rel_path` is relative to the
 * prefix (e.g. "sys/devices/system/cpu/cpu0/cpufreq/scaling_governor").
 * Creates parent directories as needed. `content` is NUL-terminated;
 * the bytes written are strlen(content). Returns 0 / -1. */
int mock_sysfs_create_file(const char *rel_path, const char *content);

/* Create a file inside the mock tree from raw bytes. */
int mock_sysfs_create_file_bin(const char *rel_path,
                               const void *data, size_t len);

/* Create a directory inside the mock tree. Parent dirs are created as
 * needed. Returns 0 / -1. */
int mock_sysfs_create_dir(const char *rel_path);

/* Create a symbolic link inside the mock tree. `target` is the link's
 * target (stored verbatim, can be relative). Returns 0 / -1. */
int mock_sysfs_create_symlink(const char *rel_path, const char *target);

/* Read a file from the mock tree into `buf`. At most `sz-1` bytes are
 * read; the buffer is NUL-terminated. Returns the number of bytes read
 * (excluding NUL) on success, -1 on error. Useful for verifying what
 * the library wrote. */
int mock_sysfs_read_file(const char *rel_path, char *buf, size_t sz);

/* Read a file from the mock tree into a freshly malloc'd buffer.
 * *out_size receives the byte count. Caller frees. Returns 0 / -1. */
int mock_sysfs_read_file_bin(const char *rel_path,
                             char **out_buf, size_t *out_size);

/* Remove a file or empty directory from the mock tree. Returns 0 / -1. */
int mock_sysfs_remove(const char *rel_path);

#endif /* ZENCTL_TEST_MOCK_SYSFS_H */
