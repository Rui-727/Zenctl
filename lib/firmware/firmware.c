/* firmware.c - firmware / BIOS domain implementation.
 *
 * Wraps DMI/SMBIOS (/sys/class/dmi/id/), efivarfs
 * (/sys/firmware/efi/efivars/), ACPI tables
 * (/sys/firmware/acpi/tables/), and the firmware-attributes class
 * (/sys/class/firmware-attributes/<vendor>/). See
 * docs/KERNEL_USB_BT_FW.md section 4.
 *
 * The sysfs I/O helpers are local static functions; the USB/BT
 * modules have their own (lib/usb/internal.c) but we keep firmware
 * self-contained to avoid cross-module coupling.
 */
#define _GNU_SOURCE
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/fs.h>
#include <linux/magic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <sys/types.h>
#include <unistd.h>

#include "zenctl/zenctl.h"
#include "zenctl/firmware.h"

#define DMI_ID_BASE        "/sys/class/dmi/id"
#define EFIVARS_BASE       "/sys/firmware/efi/efivars"
#define ACPI_TABLES_BASE   "/sys/firmware/acpi/tables"
#define FWATTR_BASE        "/sys/class/firmware-attributes"

/* ── Error helper ───────────────────────────────────────────────── */

static void set_err_from_errno(zenctl_err_t *err, int e, const char *ctx)
{
    if (!err) return;
    int code;
    switch (e) {
    case 0:           code = ZENCTL_OK;           break;
    case EPERM:
    case EACCES:      code = ZENCTL_ERR_EPERM;   break;
    case ENOENT:      code = ZENCTL_ERR_ENOENT;   break;
    case EINVAL:      code = ZENCTL_ERR_EINVAL;   break;
    case ERANGE:
    case EOVERFLOW:   code = ZENCTL_ERR_ERANGE;   break;
    case ENOMEM:      code = ZENCTL_ERR_NOMEM;    break;
    case ENOTSUP:      code = ZENCTL_ERR_ENOTSUP;  break;
    default:          code = ZENCTL_ERR_EIO;      break;
    }
    err->code = code;
    snprintf(err->message, sizeof(err->message), "%s", strerror(e));
    if (ctx)
        snprintf(err->context, sizeof(err->context), "%s", ctx);
    else
        err->context[0] = '\0';
    err->recoverable = (code != ZENCTL_ERR_INTERNAL);
}

static void set_err(zenctl_err_t *err, int code, const char *msg, const char *ctx)
{
    if (!err) return;
    err->code = code;
    snprintf(err->message, sizeof(err->message), "%s", msg);
    if (ctx)
        snprintf(err->context, sizeof(err->context), "%s", ctx);
    else
        err->context[0] = '\0';
    err->recoverable = (code != ZENCTL_ERR_INTERNAL);
}

/* ── File I/O helpers ───────────────────────────────────────────── */

static int read_text(const char *path, char **out, zenctl_err_t *err)
{
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) { set_err_from_errno(err, errno, path); return -1; }
    size_t cap = 256, len = 0;
    char *buf = malloc(cap);
    if (!buf) { close(fd); set_err(err, ZENCTL_ERR_NOMEM, "oom", path); return -1; }
    for (;;) {
        if (len + 1 >= cap) {
            size_t ncap = cap * 2;
            char *n = realloc(buf, ncap);
            if (!n) { free(buf); close(fd); set_err(err, ZENCTL_ERR_NOMEM, "oom", path); return -1; }
            buf = n; cap = ncap;
        }
        ssize_t r = read(fd, buf + len, cap - len - 1);
        if (r < 0) {
            if (errno == EINTR) continue;
            int e = errno; free(buf); close(fd);
            set_err_from_errno(err, e, path);
            return -1;
        }
        if (r == 0) break;
        len += (size_t)r;
    }
    close(fd);
    buf[len] = '\0';
    while (len > 0 && (buf[len - 1] == '\n' || buf[len - 1] == '\r'))
        buf[--len] = '\0';
    *out = buf;
    return 0;
}

static int read_binary(const char *path, uint8_t **out, size_t *out_size, zenctl_err_t *err)
{
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) { set_err_from_errno(err, errno, path); return -1; }
    size_t cap = 256, len = 0;
    uint8_t *buf = malloc(cap);
    if (!buf) { close(fd); set_err(err, ZENCTL_ERR_NOMEM, "oom", path); return -1; }
    for (;;) {
        if (len == cap) {
            size_t ncap = cap * 2;
            uint8_t *n = realloc(buf, ncap);
            if (!n) { free(buf); close(fd); set_err(err, ZENCTL_ERR_NOMEM, "oom", path); return -1; }
            buf = n; cap = ncap;
        }
        ssize_t r = read(fd, buf + len, cap - len);
        if (r < 0) {
            if (errno == EINTR) continue;
            int e = errno; free(buf); close(fd);
            set_err_from_errno(err, e, path);
            return -1;
        }
        if (r == 0) break;
        len += (size_t)r;
    }
    close(fd);
    *out = buf;
    *out_size = len;
    return 0;
}

static int write_text_trunc(const char *path, const char *data, size_t len, zenctl_err_t *err)
{
    int fd = open(path, O_WRONLY | O_CLOEXEC | O_TRUNC);
    if (fd < 0) { set_err_from_errno(err, errno, path); return -1; }
    size_t off = 0;
    while (off < len) {
        ssize_t w = write(fd, data + off, len - off);
        if (w < 0) {
            if (errno == EINTR) continue;
            int e = errno; close(fd);
            set_err_from_errno(err, e, path);
            return -1;
        }
        off += (size_t)w;
    }
    if (close(fd) < 0) { set_err_from_errno(err, errno, path); return -1; }
    return 0;
}

static int write_binary_create(const char *path, const uint8_t *data, size_t len, zenctl_err_t *err)
{
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) { set_err_from_errno(err, errno, path); return -1; }
    size_t off = 0;
    while (off < len) {
        ssize_t w = write(fd, data + off, len - off);
        if (w < 0) {
            if (errno == EINTR) continue;
            int e = errno; close(fd);
            set_err_from_errno(err, e, path);
            return -1;
        }
        off += (size_t)w;
    }
    if (close(fd) < 0) { set_err_from_errno(err, errno, path); return -1; }
    return 0;
}

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

static char **list_dir(const char *dir, zenctl_err_t *err)
{
    DIR *d = opendir(dir);
    if (!d) { set_err_from_errno(err, errno, dir); return NULL; }
    size_t cap = 16, n = 0;
    char **arr = calloc(cap, sizeof(char *));
    if (!arr) { closedir(d); set_err(err, ZENCTL_ERR_NOMEM, "oom", dir); return NULL; }
    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0) continue;
        char *copy = strdup(de->d_name);
        if (!copy) { for (size_t i = 0; i < n; i++) free(arr[i]); free(arr); closedir(d); set_err(err, ZENCTL_ERR_NOMEM, "oom", dir); return NULL; }
        if (n + 1 >= cap) {
            size_t ncap = cap * 2;
            char **na = realloc(arr, ncap * sizeof(char *));
            if (!na) { free(copy); for (size_t i = 0; i < n; i++) free(arr[i]); free(arr); closedir(d); set_err(err, ZENCTL_ERR_NOMEM, "oom", dir); return NULL; }
            arr = na; cap = ncap;
        }
        arr[n++] = copy;
    }
    closedir(d);
    if (n + 1 > cap) {
        char **na = realloc(arr, (n + 1) * sizeof(char *));
        if (!na) { for (size_t i = 0; i < n; i++) free(arr[i]); free(arr); set_err(err, ZENCTL_ERR_NOMEM, "oom", dir); return NULL; }
        arr = na;
    }
    arr[n] = NULL;
    return arr;
}

static int cmp_str(const void *a, const void *b)
{
    const char *const *pa = a; const char *const *pb = b;
    return strcmp(*pa, *pb);
}

static bool path_is_dir(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

/* ── DMI / SMBIOS ───────────────────────────────────────────────── */

static bool dmi_field_valid(const char *f)
{
    /* Whitelist of /sys/class/dmi/id/ attribute names. Anything else
     * is rejected before we touch the filesystem. */
    static const char *const ok[] = {
        "bios_vendor", "bios_version", "bios_date", "bios_release",
        "ec_firmware_release", "sys_vendor",
        "product_name", "product_version", "product_sku", "product_family",
        "product_uuid", "product_serial",
        "board_vendor", "board_name", "board_version",
        "board_serial", "board_asset_tag",
        "chassis_vendor", "chassis_type", "chassis_version",
        "chassis_serial", "chassis_asset_tag",
        "uevent",
        NULL
    };
    if (!f || !*f) return false;
    for (size_t i = 0; ok[i]; i++)
        if (strcmp(f, ok[i]) == 0) return true;
    return false;
}

int zenctl_fw_dmi_get(const char *field, char **out, zenctl_err_t *err)
{
    if (!field || !out) { set_err(err, ZENCTL_ERR_EINVAL, "NULL argument", "zenctl_fw_dmi_get"); return -1; }
    if (!dmi_field_valid(field)) {
        set_err(err, ZENCTL_ERR_EINVAL, "unknown DMI field", field);
        return -1;
    }
    char *path = path_join(DMI_ID_BASE, field);
    if (!path) { set_err(err, ZENCTL_ERR_NOMEM, "oom", "zenctl_fw_dmi_get"); return -1; }
    int rc = read_text(path, out, err);
    free(path);
    return rc;
}

/* ── efivarfs helpers ───────────────────────────────────────────── */

static bool efivars_mounted(zenctl_err_t *err)
{
    struct statfs sb;
    if (statfs(EFIVARS_BASE, &sb) < 0) {
        set_err_from_errno(err, errno, EFIVARS_BASE);
        return false;
    }
    if (sb.f_type != EFIVARFS_MAGIC) {
        set_err(err, ZENCTL_ERR_ENOTSUP,
                "efivarfs not mounted at " EFIVARS_BASE,
                "efivars_mounted");
        return false;
    }
    return true;
}

/* Validate an EFI variable name: must not contain '/' or '-'. (UEFI
 * names are UCS-2 internally; the filename is the UTF-8 form. A
 * literal '-' would conflict with the name/GUID separator.) */
static bool efi_name_valid(const char *name)
{
    if (!name || !*name) return false;
    if (strlen(name) > 200) return false;
    for (const char *c = name; *c; c++) {
        if (*c == '/' || *c == '-') return false;
    }
    return true;
}

/* Validate a GUID string of form "xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx"
 * (36 chars, hex digits and dashes). */
static bool efi_guid_valid(const char *g)
{
    if (!g || strlen(g) != 36) return false;
    for (int i = 0; i < 36; i++) {
        if (i == 8 || i == 13 || i == 18 || i == 23) {
            if (g[i] != '-') return false;
        } else {
            if (!isxdigit((unsigned char)g[i])) return false;
        }
    }
    return true;
}

/* Build "<NAME>-<GUID>" filename (heap-allocated, caller frees). */
static char *efi_filename(const char *name, const char *guid)
{
    size_t n = strlen(name), g = strlen(guid);
    char *s = malloc(n + 1 + g + 1);
    if (!s) return NULL;
    memcpy(s, name, n);
    s[n] = '-';
    memcpy(s + n + 1, guid, g);
    s[n + 1 + g] = '\0';
    return s;
}

/* Parse "<NAME>-<GUID>" into separate heap-allocated strings. Returns
 * 0 on success, -1 if the entry doesn't match the expected pattern. */
static int efi_parse_entry(const char *entry, char **out_name, char **out_guid)
{
    size_t len = strlen(entry);
    if (len < 37) return -1;  /* 1 char name + '-' + 36-char GUID */
    /* GUID is the last 36 chars; the dash before it is at len - 37. */
    if (entry[len - 37] != '-') return -1;
    const char *guid = entry + len - 36;
    if (!efi_guid_valid(guid)) return -1;
    char *g = strdup(guid);
    if (!g) return -1;
    char *n = malloc(len - 36);
    if (!n) { free(g); return -1; }
    memcpy(n, entry, len - 37);
    n[len - 37] = '\0';
    *out_name = n;
    *out_guid = g;
    return 0;
}

/* Get/clear the FS_IMMUTABLE_FL on a file. Returns the current flags
 * via *cur_flags when get_flags is non-NULL. Returns 0 on success.
 * Errors are reported through *err but immutable-flag handling is
 * best-effort: if the file doesn't support the ioctl (e.g. on
 * tmpfs), we return 0 with *cur_flags = 0 and *supported = false. */
static int fs_get_flags(const char *path, long *cur_flags, bool *supported, zenctl_err_t *err)
{
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) { set_err_from_errno(err, errno, path); return -1; }
    long fl = 0;
    if (ioctl(fd, FS_IOC_GETFLAGS, &fl) < 0) {
        int e = errno;
        close(fd);
        if (e == ENOTTY || e == ENOSYS || e == EINVAL) {
            *cur_flags = 0;
            *supported = false;
            return 0;
        }
        set_err_from_errno(err, e, "FS_IOC_GETFLAGS");
        return -1;
    }
    close(fd);
    *cur_flags = fl;
    *supported = true;
    return 0;
}

static int fs_set_flags(const char *path, long flags, zenctl_err_t *err)
{
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) { set_err_from_errno(err, errno, path); return -1; }
    if (ioctl(fd, FS_IOC_SETFLAGS, &flags) < 0) {
        int e = errno; close(fd);
        set_err_from_errno(err, e, "FS_IOC_SETFLAGS");
        return -1;
    }
    close(fd);
    return 0;
}

/* ── EFI variable API ───────────────────────────────────────────── */

int zenctl_fw_efi_enumerate(zenctl_efi_var_t **out_list, int *out_count, zenctl_err_t *err)
{
    if (!out_list || !out_count) { set_err(err, ZENCTL_ERR_EINVAL, "NULL argument", "zenctl_fw_efi_enumerate"); return -1; }
    if (!efivars_mounted(err)) return -1;

    char **entries = list_dir(EFIVARS_BASE, err);
    if (!entries) return -1;

    size_t cap = 16, n = 0;
    zenctl_efi_var_t *arr = calloc(cap, sizeof(*arr));
    if (!arr) {
        for (size_t i = 0; entries[i]; i++) free(entries[i]);
        free(entries);
        set_err(err, ZENCTL_ERR_NOMEM, "oom", "zenctl_fw_efi_enumerate");
        return -1;
    }

    for (size_t i = 0; entries[i]; i++) {
        char *name = NULL, *guid = NULL;
        if (efi_parse_entry(entries[i], &name, &guid) != 0) continue;

        /* Read the file header (4-byte attributes prefix). */
        char *fpath = path_join(EFIVARS_BASE, entries[i]);
        if (!fpath) { free(name); free(guid); continue; }
        uint8_t *raw = NULL;
        size_t raw_sz = 0;
        if (read_binary(fpath, &raw, &raw_sz, NULL) == 0) {
            if (n + 1 >= cap) {
                size_t ncap = cap * 2;
                zenctl_efi_var_t *na = realloc(arr, ncap * sizeof(*arr));
                if (!na) { free(raw); free(fpath); free(name); free(guid); continue; }
                arr = na; cap = ncap;
            }
            zenctl_efi_var_t *v = &arr[n];
            memset(v, 0, sizeof(*v));
            snprintf(v->name, sizeof(v->name), "%s", name);
            snprintf(v->guid, sizeof(v->guid), "%s", guid);
            v->attributes = (raw_sz >= 4)
                ? (uint32_t)raw[0] | ((uint32_t)raw[1] << 8) |
                  ((uint32_t)raw[2] << 16) | ((uint32_t)raw[3] << 24)
                : 0;
            v->data_size = (raw_sz >= 4) ? (raw_sz - 4) : 0;
            n++;
            free(raw);
        }
        free(fpath);
        free(name);
        free(guid);
    }

    for (size_t i = 0; entries[i]; i++) free(entries[i]);
    free(entries);

    *out_list = arr;
    *out_count = (int)n;
    return 0;
}

int zenctl_fw_efi_get(const char *name, const char *guid,
                      uint8_t **out_data, size_t *out_size, zenctl_err_t *err)
{
    if (!efi_name_valid(name) || !efi_guid_valid(guid) || !out_data || !out_size) {
        set_err(err, ZENCTL_ERR_EINVAL, "invalid name/guid/out",
                "zenctl_fw_efi_get");
        return -1;
    }
    if (!efivars_mounted(err)) return -1;

    char *fname = efi_filename(name, guid);
    if (!fname) { set_err(err, ZENCTL_ERR_NOMEM, "oom", "zenctl_fw_efi_get"); return -1; }
    char *fpath = path_join(EFIVARS_BASE, fname);
    free(fname);
    if (!fpath) { set_err(err, ZENCTL_ERR_NOMEM, "oom", "zenctl_fw_efi_get"); return -1; }

    uint8_t *raw = NULL;
    size_t raw_sz = 0;
    int rc = read_binary(fpath, &raw, &raw_sz, err);
    free(fpath);
    if (rc != 0) return rc;

    if (raw_sz < 4) {
        free(raw);
        set_err(err, ZENCTL_ERR_EIO, "efivar file shorter than attribute prefix",
                "zenctl_fw_efi_get");
        return -1;
    }
    /* Strip the 4-byte attributes prefix. */
    size_t data_sz = raw_sz - 4;
    uint8_t *data = malloc(data_sz ? data_sz : 1);
    if (!data) { free(raw); set_err(err, ZENCTL_ERR_NOMEM, "oom", "zenctl_fw_efi_get"); return -1; }
    if (data_sz) memcpy(data, raw + 4, data_sz);
    free(raw);
    *out_data = data;
    *out_size = data_sz;
    return 0;
}

/* Best-effort immutable-flag handling: if the file exists and has
 * FS_IMMUTABLE_FL set, clear it before the write and re-apply it
 * afterwards. Returns 0 on success (including when the file does not
 * exist or the filesystem doesn't support the ioctl). On error sets
 * *err. */
static int efi_maybe_clear_immutable(const char *fpath, bool *was_cleared, zenctl_err_t *err)
{
    *was_cleared = false;
    long fl = 0;
    bool supported = false;
    if (fs_get_flags(fpath, &fl, &supported, err) != 0) return -1;
    if (!supported) return 0;
    if (!(fl & FS_IMMUTABLE_FL)) return 0;
    long newfl = fl & ~FS_IMMUTABLE_FL;
    if (fs_set_flags(fpath, newfl, err) != 0) return -1;
    *was_cleared = true;
    return 0;
}

static int efi_maybe_restore_immutable(const char *fpath, bool restore, zenctl_err_t *err)
{
    if (!restore) return 0;
    long fl = 0;
    bool supported = false;
    if (fs_get_flags(fpath, &fl, &supported, err) != 0) return -1;
    if (!supported) return 0;
    long newfl = fl | FS_IMMUTABLE_FL;
    if (fl == newfl) return 0;
    return fs_set_flags(fpath, newfl, err);
}

int zenctl_fw_efi_set(const char *name, const char *guid, uint32_t attributes,
                      const uint8_t *data, size_t size, zenctl_err_t *err)
{
    if (!efi_name_valid(name) || !efi_guid_valid(guid)) {
        set_err(err, ZENCTL_ERR_EINVAL, "invalid name/guid",
                "zenctl_fw_efi_set");
        return -1;
    }
    if (size > 0 && !data) {
        set_err(err, ZENCTL_ERR_EINVAL, "data is NULL but size > 0",
                "zenctl_fw_efi_set");
        return -1;
    }
    if (!efivars_mounted(err)) return -1;

    char *fname = efi_filename(name, guid);
    if (!fname) { set_err(err, ZENCTL_ERR_NOMEM, "oom", "zenctl_fw_efi_set"); return -1; }
    char *fpath = path_join(EFIVARS_BASE, fname);
    free(fname);
    if (!fpath) { set_err(err, ZENCTL_ERR_NOMEM, "oom", "zenctl_fw_efi_set"); return -1; }

    /* Build the payload: 4-byte LE attributes + data. */
    size_t total = 4 + size;
    uint8_t *buf = malloc(total);
    if (!buf) { free(fpath); set_err(err, ZENCTL_ERR_NOMEM, "oom", "zenctl_fw_efi_set"); return -1; }
    buf[0] = (uint8_t)(attributes & 0xff);
    buf[1] = (uint8_t)((attributes >> 8) & 0xff);
    buf[2] = (uint8_t)((attributes >> 16) & 0xff);
    buf[3] = (uint8_t)((attributes >> 24) & 0xff);
    if (size) memcpy(buf + 4, data, size);

    /* If the file already exists and is immutable, clear the flag
     * for the duration of the write. */
    bool was_cleared = false;
    if (efi_maybe_clear_immutable(fpath, &was_cleared, err) != 0) {
        free(buf); free(fpath); return -1;
    }

    int rc = write_binary_create(fpath, buf, total, err);

    /* Restore the immutable flag if we cleared it. Best-effort: don't
     * clobber the write's error. */
    if (was_cleared) {
        zenctl_err_t tmp;
        efi_maybe_restore_immutable(fpath, true, &tmp);
    }

    free(buf);
    free(fpath);
    return rc;
}

int zenctl_fw_efi_delete(const char *name, const char *guid, zenctl_err_t *err)
{
    if (!efi_name_valid(name) || !efi_guid_valid(guid)) {
        set_err(err, ZENCTL_ERR_EINVAL, "invalid name/guid",
                "zenctl_fw_efi_delete");
        return -1;
    }
    if (!efivars_mounted(err)) return -1;

    char *fname = efi_filename(name, guid);
    if (!fname) { set_err(err, ZENCTL_ERR_NOMEM, "oom", "zenctl_fw_efi_delete"); return -1; }
    char *fpath = path_join(EFIVARS_BASE, fname);
    free(fname);
    if (!fpath) { set_err(err, ZENCTL_ERR_NOMEM, "oom", "zenctl_fw_efi_delete"); return -1; }

    /* Clear immutable flag if set, so the delete-by-zero-attr write
     * succeeds. */
    bool was_cleared = false;
    if (efi_maybe_clear_immutable(fpath, &was_cleared, err) != 0) {
        free(fpath); return -1;
    }

    /* Delete by writing 4 bytes of zero attributes and no data. */
    uint8_t zero[4] = {0, 0, 0, 0};
    int rc = write_text_trunc(fpath, (const char *)zero, 4, err);

    /* If the variable didn't get deleted (some firmware needs an
     * explicit unlink), try unlink() as a fallback. */
    if (rc == 0) {
        struct stat st;
        if (stat(fpath, &st) == 0) {
            if (unlink(fpath) < 0) {
                set_err_from_errno(err, errno, "unlink efivar");
                rc = -1;
            }
        }
    }

    /* Best-effort: don't restore the immutable flag if we deleted the
     * variable. If the delete failed and we cleared the flag, restore
     * it. */
    if (rc != 0 && was_cleared) {
        zenctl_err_t tmp;
        efi_maybe_restore_immutable(fpath, true, &tmp);
    }

    free(fpath);
    return rc;
}

/* ── ACPI tables ────────────────────────────────────────────────── */

int zenctl_fw_acpi_list_tables(char ***out_list, int *out_count, zenctl_err_t *err)
{
    if (!out_list || !out_count) { set_err(err, ZENCTL_ERR_EINVAL, "NULL argument", "zenctl_fw_acpi_list_tables"); return -1; }
    char **entries = list_dir(ACPI_TABLES_BASE, err);
    if (!entries) return -1;

    /* Filter out subdirectories ("dynamic", "data"). ACPI table
     * files are 4-char names like DSDT, SSDT, FACP, APIC. */
    size_t cap = 8, n = 0;
    char **out = calloc(cap, sizeof(char *));
    if (!out) {
        for (size_t i = 0; entries[i]; i++) free(entries[i]);
        free(entries);
        set_err(err, ZENCTL_ERR_NOMEM, "oom", "zenctl_fw_acpi_list_tables");
        return -1;
    }
    for (size_t i = 0; entries[i]; i++) {
        char *p = path_join(ACPI_TABLES_BASE, entries[i]);
        if (!p) continue;
        bool isdir = path_is_dir(p);
        free(p);
        if (isdir) continue;
        if (n + 1 >= cap) {
            size_t ncap = cap * 2;
            char **na = realloc(out, ncap * sizeof(char *));
            if (!na) {
                for (size_t j = 0; j < n; j++) free(out[j]);
                free(out);
                for (size_t j = 0; entries[j]; j++) free(entries[j]);
                free(entries);
                set_err(err, ZENCTL_ERR_NOMEM, "oom", "zenctl_fw_acpi_list_tables");
                return -1;
            }
            out = na; cap = ncap;
        }
        out[n] = entries[i];
        entries[i] = NULL;
        n++;
    }
    for (size_t i = 0; entries[i]; i++) free(entries[i]);
    free(entries);

    qsort(out, n, sizeof(char *), cmp_str);

    *out_list = out;
    *out_count = (int)n;
    return 0;
}

int zenctl_fw_acpi_get_table(const char *name, uint8_t **out_data, size_t *out_size, zenctl_err_t *err)
{
    if (!name || !*name || !out_data || !out_size) {
        set_err(err, ZENCTL_ERR_EINVAL, "NULL argument", "zenctl_fw_acpi_get_table");
        return -1;
    }
    /* Reject path traversal. ACPI table names are short ASCII alnum. */
    for (const char *c = name; *c; c++) {
        if (*c == '/' || !isalnum((unsigned char)*c)) {
            set_err(err, ZENCTL_ERR_EINVAL, "invalid ACPI table name", name);
            return -1;
        }
    }
    char *path = path_join(ACPI_TABLES_BASE, name);
    if (!path) { set_err(err, ZENCTL_ERR_NOMEM, "oom", "zenctl_fw_acpi_get_table"); return -1; }
    int rc = read_binary(path, out_data, out_size, err);
    free(path);
    return rc;
}

/* ── firmware-attributes (BIOS settings) ────────────────────────── */

/* Find the first vendor directory under /sys/class/firmware-attributes/. */
static char *fwattr_find_vendor(zenctl_err_t *err)
{
    char **entries = list_dir(FWATTR_BASE, err);
    if (!entries) return NULL;

    char *vendor = NULL;
    for (size_t i = 0; entries[i]; i++) {
        char *p = path_join(FWATTR_BASE, entries[i]);
        if (!p) continue;
        bool isdir = path_is_dir(p);
        free(p);
        if (isdir) {
            vendor = strdup(entries[i]);
            break;
        }
    }
    for (size_t i = 0; entries[i]; i++) free(entries[i]);
    free(entries);

    if (!vendor) {
        set_err(err, ZENCTL_ERR_ENOTSUP,
                "no /sys/class/firmware-attributes/<vendor>/ registered",
                "fwattr_find_vendor");
    }
    return vendor;
}

/* Read the list of setting names (alphabetical) under
 * <vendor>/attributes/. A directory entry is treated as a setting if
 * it contains a "type" file. Returns a NULL-terminated array (caller
 * frees each entry and the array) or NULL on error. */
static char **fwattr_list_settings(const char *vendor, int *out_count, zenctl_err_t *err)
{
    char *apath = NULL;
    char *vpath = path_join(FWATTR_BASE, vendor);
    if (!vpath) { set_err(err, ZENCTL_ERR_NOMEM, "oom", "fwattr_list_settings"); return NULL; }
    apath = path_join(vpath, "attributes");
    free(vpath);
    if (!apath) { set_err(err, ZENCTL_ERR_NOMEM, "oom", "fwattr_list_settings"); return NULL; }

    char **entries = list_dir(apath, err);
    free(apath);
    if (!entries) return NULL;

    size_t cap = 8, n = 0;
    char **out = calloc(cap, sizeof(char *));
    if (!out) {
        for (size_t i = 0; entries[i]; i++) free(entries[i]);
        free(entries);
        set_err(err, ZENCTL_ERR_NOMEM, "oom", "fwattr_list_settings");
        return NULL;
    }
    for (size_t i = 0; entries[i]; i++) {
        char *p = NULL;
        {
            char *base = path_join(FWATTR_BASE, vendor);
            if (!base) continue;
            char *attrs = path_join(base, "attributes");
            free(base);
            if (!attrs) continue;
            p = path_join(attrs, entries[i]);
            free(attrs);
            if (!p) continue;
        }
        /* Must be a directory AND have a "type" file. */
        bool isdir = path_is_dir(p);
        free(p);
        if (!isdir) continue;

        char *t = NULL;
        {
            char *base = path_join(FWATTR_BASE, vendor);
            if (!base) continue;
            char *attrs = path_join(base, "attributes");
            free(base);
            if (!attrs) continue;
            char *entp = path_join(attrs, entries[i]);
            free(attrs);
            if (!entp) continue;
            t = path_join(entp, "type");
            free(entp);
            if (!t) continue;
        }
        bool has_type = (access(t, F_OK) == 0);
        free(t);
        if (!has_type) continue;

        if (n + 1 >= cap) {
            size_t ncap = cap * 2;
            char **na = realloc(out, ncap * sizeof(char *));
            if (!na) {
                for (size_t j = 0; j < n; j++) free(out[j]);
                free(out);
                for (size_t j = 0; entries[j]; j++) free(entries[j]);
                free(entries);
                set_err(err, ZENCTL_ERR_NOMEM, "oom", "fwattr_list_settings");
                return NULL;
            }
            out = na; cap = ncap;
        }
        out[n] = entries[i];
        entries[i] = NULL;
        n++;
    }

    for (size_t i = 0; entries[i]; i++) free(entries[i]);
    free(entries);

    qsort(out, n, sizeof(char *), cmp_str);

    *out_count = (int)n;
    return out;
}

int zenctl_fw_bios_setting_count(int *out, zenctl_err_t *err)
{
    if (!out) { set_err(err, ZENCTL_ERR_EINVAL, "NULL argument", "zenctl_fw_bios_setting_count"); return -1; }
    char *vendor = fwattr_find_vendor(err);
    if (!vendor) return -1;
    int count = 0;
    char **list = fwattr_list_settings(vendor, &count, err);
    free(vendor);
    if (!list) return -1;
    for (int i = 0; i < count; i++) free(list[i]);
    free(list);
    *out = count;
    return 0;
}

static void copy_field(char *dst, size_t dst_sz, const char *src)
{
    if (!src) { if (dst_sz) dst[0] = '\0'; return; }
    snprintf(dst, dst_sz, "%s", src);
}

int zenctl_fw_bios_get_setting(int index, zenctl_bios_setting_t *out, zenctl_err_t *err)
{
    if (!out || index < 0) { set_err(err, ZENCTL_ERR_EINVAL, "invalid index/out", "zenctl_fw_bios_get_setting"); return -1; }
    memset(out, 0, sizeof(*out));

    char *vendor = fwattr_find_vendor(err);
    if (!vendor) return -1;

    int count = 0;
    char **list = fwattr_list_settings(vendor, &count, err);
    if (!list) { free(vendor); return -1; }

    if (index >= count) {
        for (int i = 0; i < count; i++) free(list[i]);
        free(list);
        free(vendor);
        set_err(err, ZENCTL_ERR_ERANGE, "BIOS setting index out of range",
                "zenctl_fw_bios_get_setting");
        return -1;
    }

    const char *sname = list[index];
    copy_field(out->name, sizeof(out->name), sname);

    /* Build /sys/class/firmware-attributes/<vendor>/attributes/<name>/<field>. */
    char *base = path_join(FWATTR_BASE, vendor);
    free(vendor);
    if (!base) {
        for (int i = 0; i < count; i++) free(list[i]);
        free(list);
        set_err(err, ZENCTL_ERR_NOMEM, "oom", "zenctl_fw_bios_get_setting");
        return -1;
    }
    char *attrs = path_join(base, "attributes");
    free(base);
    if (!attrs) {
        for (int i = 0; i < count; i++) free(list[i]);
        free(list);
        set_err(err, ZENCTL_ERR_NOMEM, "oom", "zenctl_fw_bios_get_setting");
        return -1;
    }
    char *spath = path_join(attrs, sname);
    free(attrs);
    if (!spath) {
        for (int i = 0; i < count; i++) free(list[i]);
        free(list);
        set_err(err, ZENCTL_ERR_NOMEM, "oom", "zenctl_fw_bios_get_setting");
        return -1;
    }

    static const char *const fields[] = {"type", "current_value", "default_value", "display_name"};
    char *dst_ptrs[] = {out->type, out->current_value, out->default_value, out->display_name};
    size_t dst_sizes[] = {sizeof(out->type), sizeof(out->current_value), sizeof(out->default_value), sizeof(out->display_name)};

    for (size_t k = 0; k < sizeof(fields) / sizeof(fields[0]); k++) {
        char *fp = path_join(spath, fields[k]);
        if (!fp) continue;
        char *val = NULL;
        if (read_text(fp, &val, NULL) == 0) {
            copy_field(dst_ptrs[k], dst_sizes[k], val);
            free(val);
        }
        free(fp);
    }

    free(spath);
    for (int i = 0; i < count; i++) free(list[i]);
    free(list);
    return 0;
}

int zenctl_fw_bios_set_setting(const char *name, const char *value,
                               const char *password, zenctl_err_t *err)
{
    if (!name || !*name || !value) {
        set_err(err, ZENCTL_ERR_EINVAL, "NULL argument", "zenctl_fw_bios_set_setting");
        return -1;
    }
    /* Reject path traversal in the setting name. */
    for (const char *c = name; *c; c++) {
        if (*c == '/') {
            set_err(err, ZENCTL_ERR_EINVAL, "invalid setting name", name);
            return -1;
        }
    }
    char *vendor = fwattr_find_vendor(err);
    if (!vendor) return -1;

    /* If a password is supplied, write it to the first authentication
     * role's current_password file. The role dir name varies by
     * vendor; we pick the first directory under authentication/. */
    if (password && *password) {
        char *base = path_join(FWATTR_BASE, vendor);
        if (!base) { free(vendor); set_err(err, ZENCTL_ERR_NOMEM, "oom", "fwattr_set"); return -1; }
        char *authdir = path_join(base, "authentication");
        free(base);
        if (!authdir) { free(vendor); set_err(err, ZENCTL_ERR_NOMEM, "oom", "fwattr_set"); return -1; }

        char **roles = list_dir(authdir, NULL);
        if (roles) {
            for (size_t i = 0; roles[i]; i++) {
                char *rp = path_join(authdir, roles[i]);
                if (!rp) continue;
                bool isdir = path_is_dir(rp);
                if (!isdir) { free(rp); continue; }
                char *cp = path_join(rp, "current_password");
                free(rp);
                if (!cp) continue;
                int rc = write_text_trunc(cp, password, strlen(password), NULL);
                free(cp);
                if (rc == 0) break;
            }
            for (size_t i = 0; roles[i]; i++) free(roles[i]);
            free(roles);
        }
        free(authdir);
    }

    /* Write the new value to attributes/<name>/current_value. */
    char *base = path_join(FWATTR_BASE, vendor);
    free(vendor);
    if (!base) { set_err(err, ZENCTL_ERR_NOMEM, "oom", "fwattr_set"); return -1; }
    char *attrs = path_join(base, "attributes");
    free(base);
    if (!attrs) { set_err(err, ZENCTL_ERR_NOMEM, "oom", "fwattr_set"); return -1; }
    char *spath = path_join(attrs, name);
    free(attrs);
    if (!spath) { set_err(err, ZENCTL_ERR_NOMEM, "oom", "fwattr_set"); return -1; }
    char *cval = path_join(spath, "current_value");
    free(spath);
    if (!cval) { set_err(err, ZENCTL_ERR_NOMEM, "oom", "fwattr_set"); return -1; }

    int rc = write_text_trunc(cval, value, strlen(value), err);
    free(cval);

    /* Clear the password file by writing an empty string. */
    if ((rc == 0) && password && *password) {
        char *v2 = fwattr_find_vendor(NULL);
        if (v2) {
            char *b2 = path_join(FWATTR_BASE, v2);
            free(v2);
            if (b2) {
                char *a2 = path_join(b2, "authentication");
                free(b2);
                if (a2) {
                    char **roles = list_dir(a2, NULL);
                    if (roles) {
                        for (size_t i = 0; roles[i]; i++) {
                            char *rp = path_join(a2, roles[i]);
                            if (!rp) continue;
                            bool isdir = path_is_dir(rp);
                            if (!isdir) { free(rp); continue; }
                            char *cp = path_join(rp, "current_password");
                            free(rp);
                            if (!cp) continue;
                            write_text_trunc(cp, "", 0, NULL);
                            free(cp);
                        }
                        for (size_t i = 0; roles[i]; i++) free(roles[i]);
                        free(roles);
                    }
                    free(a2);
                }
            }
        }
    }

    return rc;
}
