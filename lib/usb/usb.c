/* usb.c - USB domain implementation.
 *
 * Wraps /sys/bus/usb/devices/<id>/ sysfs attributes and the USBDEVFS
 * ioctls on /dev/bus/usb/<bus>/<dev>. See docs/KERNEL_USB_BT_FW.md
 * section 1.
 *
 * Sysfs text I/O goes through the shared zenctl__read_file_string /
 * zenctl__write_file_string helpers (lib/core/io.c). The USBDEVFS_RESET
 * ioctl is issued directly on /dev/bus/usb/<bus>/<dev>.
 */
#define _POSIX_C_SOURCE 200809L

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/usbdevice_fs.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include "zenctl/zenctl.h"
#include "zenctl/internal.h"
#include "zenctl/usb.h"

#include "util.h"

#define USB_SYSFS_BASE "/sys/bus/usb/devices"

struct zenctl_usb {
    char *dev_path;   /* e.g. "1-2" or "2-1.3" or "usb1" */
    char *sysfs_dir;  /* /sys/bus/usb/devices/<dev_path> */
    int   busnum;     /* -1 if unknown */
    int   devnum;     /* -1 if unknown */
};

/* Validate a USB device path. Allowed: alnum, '-', '.', must start
 * with a digit or "usb" + digit. Reject anything with '/' or ':'
 * (interface entries). */
static bool dev_path_valid(const char *p)
{
    if (!p || !*p) return false;
    for (const char *c = p; *c; c++) {
        if (*c == '/' || *c == ':') return false;
        if (!(isalnum((unsigned char)*c) || *c == '-' || *c == '.')) return false;
    }
    if (isdigit((unsigned char)p[0])) return true;
    if (strncmp(p, "usb", 3) == 0 && isdigit((unsigned char)p[3])) return true;
    return false;
}

/* Read busnum / devnum into the handle. Returns 0 on success (values
 * stay -1 if files are absent). */
static int read_bus_dev_nums(zenctl_usb_t *u)
{
    char *path = zenctl_util_path_join(u->sysfs_dir, "busnum");
    if (!path) return -1;
    char buf[32] = {0};
    if (zenctl__read_file_string(path, buf, sizeof(buf), NULL) == 0)
        u->busnum = atoi(buf);
    free(path);

    path = zenctl_util_path_join(u->sysfs_dir, "devnum");
    if (!path) return -1;
    buf[0] = '\0';
    if (zenctl__read_file_string(path, buf, sizeof(buf), NULL) == 0)
        u->devnum = atoi(buf);
    free(path);
    return 0;
}

zenctl_usb_t *zenctl_usb_open(const char *dev_path, zenctl_err_t *err)
{
    if (!dev_path_valid(dev_path)) {
        char ctx[128];
        snprintf(ctx, sizeof(ctx), "zenctl_usb_open(%s)",
                 dev_path ? dev_path : "NULL");
        zenctl__set_err(err, ZENCTL_ERR_EINVAL,
                        "invalid USB device path", ctx);
        return NULL;
    }

    zenctl_usb_t *u = calloc(1, sizeof(*u));
    if (!u) {
        zenctl__set_err(err, ZENCTL_ERR_NOMEM, "calloc failed", "zenctl_usb_open");
        return NULL;
    }
    u->dev_path = strdup(dev_path);
    if (!u->dev_path) {
        free(u);
        zenctl__set_err(err, ZENCTL_ERR_NOMEM, "strdup failed", "zenctl_usb_open");
        return NULL;
    }
    u->sysfs_dir = zenctl_util_path_join(USB_SYSFS_BASE, dev_path);
    if (!u->sysfs_dir) {
        free(u->dev_path); free(u);
        zenctl__set_err(err, ZENCTL_ERR_NOMEM, "path join failed", "zenctl_usb_open");
        return NULL;
    }

    /* Verify the directory exists. */
    DIR *d = opendir(u->sysfs_dir);
    if (!d) {
        zenctl__set_err(err, zenctl__errno_to_code(errno),
                        strerror(errno), u->sysfs_dir);
        free(u->sysfs_dir);
        free(u->dev_path);
        free(u);
        return NULL;
    }
    closedir(d);

    u->busnum = -1;
    u->devnum = -1;
    read_bus_dev_nums(u);
    return u;
}

void zenctl_usb_close(zenctl_usb_t *usb)
{
    if (!usb) return;
    free(usb->sysfs_dir);
    free(usb->dev_path);
    free(usb);
}

/* Helper: read a sysfs text attribute into a heap-allocated string. */
static int read_attr_str(zenctl_usb_t *usb, const char *attr,
                         char **out, zenctl_err_t *err)
{
    if (!usb || !out) {
        zenctl__set_err(err, ZENCTL_ERR_EINVAL, "NULL usb or out", attr);
        return -1;
    }
    char *path = zenctl_util_path_join(usb->sysfs_dir, attr);
    if (!path) {
        zenctl__set_err(err, ZENCTL_ERR_NOMEM, "path join failed", attr);
        return -1;
    }
    char buf[512] = {0};
    int rc = zenctl__read_file_string(path, buf, sizeof(buf), err);
    free(path);
    if (rc != 0) return rc;
    char *s = strdup(buf);
    if (!s) {
        zenctl__set_err(err, ZENCTL_ERR_NOMEM, "strdup failed", attr);
        return -1;
    }
    *out = s;
    return 0;
}

/* Helper: read a sysfs hex string attribute as int. */
static int read_attr_hex(zenctl_usb_t *usb, const char *attr,
                         int *out, zenctl_err_t *err)
{
    if (!usb || !out) {
        zenctl__set_err(err, ZENCTL_ERR_EINVAL, "NULL usb or out", attr);
        return -1;
    }
    char *path = zenctl_util_path_join(usb->sysfs_dir, attr);
    if (!path) {
        zenctl__set_err(err, ZENCTL_ERR_NOMEM, "path join failed", attr);
        return -1;
    }
    char buf[32] = {0};
    int rc = zenctl__read_file_string(path, buf, sizeof(buf), err);
    free(path);
    if (rc != 0) return rc;
    *out = (int)strtol(buf, NULL, 16);
    return 0;
}

/* Helper: write a sysfs text attribute. */
static int write_attr_str(zenctl_usb_t *usb, const char *attr,
                          const char *value, zenctl_err_t *err)
{
    if (!usb) {
        zenctl__set_err(err, ZENCTL_ERR_EINVAL, "NULL usb", attr);
        return -1;
    }
    char *path = zenctl_util_path_join(usb->sysfs_dir, attr);
    if (!path) {
        zenctl__set_err(err, ZENCTL_ERR_NOMEM, "path join failed", attr);
        return -1;
    }
    int rc = zenctl__write_file_string(path, value, err);
    free(path);
    return rc;
}

int zenctl_usb_get_vendor_id(zenctl_usb_t *usb, int *out, zenctl_err_t *err)
{
    return read_attr_hex(usb, "idVendor", out, err);
}

int zenctl_usb_get_product_id(zenctl_usb_t *usb, int *out, zenctl_err_t *err)
{
    return read_attr_hex(usb, "idProduct", out, err);
}

int zenctl_usb_get_manufacturer(zenctl_usb_t *usb, char **out, zenctl_err_t *err)
{
    return read_attr_str(usb, "manufacturer", out, err);
}

int zenctl_usb_get_product(zenctl_usb_t *usb, char **out, zenctl_err_t *err)
{
    return read_attr_str(usb, "product", out, err);
}

int zenctl_usb_get_speed(zenctl_usb_t *usb, char **out, zenctl_err_t *err)
{
    return read_attr_str(usb, "speed", out, err);
}

int zenctl_usb_get_power_control(zenctl_usb_t *usb, char **out, zenctl_err_t *err)
{
    return read_attr_str(usb, "power/control", out, err);
}

int zenctl_usb_set_power_control(zenctl_usb_t *usb, const char *mode, zenctl_err_t *err)
{
    if (!mode || (strcmp(mode, "on") != 0 && strcmp(mode, "auto") != 0)) {
        zenctl__set_err(err, ZENCTL_ERR_EINVAL,
                        "mode must be 'on' or 'auto'", "zenctl_usb_set_power_control");
        return -1;
    }
    return write_attr_str(usb, "power/control", mode, err);
}

int zenctl_usb_get_autosuspend_delay(zenctl_usb_t *usb, int *out_ms, zenctl_err_t *err)
{
    if (!usb || !out_ms) {
        zenctl__set_err(err, ZENCTL_ERR_EINVAL, "NULL usb or out", "zenctl_usb_get_autosuspend_delay");
        return -1;
    }
    char *path = zenctl_util_path_join(usb->sysfs_dir, "power/autosuspend_delay_ms");
    if (!path) {
        zenctl__set_err(err, ZENCTL_ERR_NOMEM, "path join failed", "autosuspend_delay_ms");
        return -1;
    }
    char buf[32] = {0};
    int rc = zenctl__read_file_string(path, buf, sizeof(buf), err);
    free(path);
    if (rc != 0) return rc;
    *out_ms = atoi(buf);
    return 0;
}

int zenctl_usb_set_autosuspend_delay(zenctl_usb_t *usb, int ms, zenctl_err_t *err)
{
    if (!usb) {
        zenctl__set_err(err, ZENCTL_ERR_EINVAL, "NULL usb", "zenctl_usb_set_autosuspend_delay");
        return -1;
    }
    char buf[32];
    snprintf(buf, sizeof(buf), "%d", ms);
    return write_attr_str(usb, "power/autosuspend_delay_ms", buf, err);
}

int zenctl_usb_get_runtime_status(zenctl_usb_t *usb, char **out, zenctl_err_t *err)
{
    return read_attr_str(usb, "power/runtime_status", out, err);
}

int zenctl_usb_get_authorized(zenctl_usb_t *usb, bool *out, zenctl_err_t *err)
{
    if (!usb || !out) {
        zenctl__set_err(err, ZENCTL_ERR_EINVAL, "NULL usb or out", "zenctl_usb_get_authorized");
        return -1;
    }
    char *path = zenctl_util_path_join(usb->sysfs_dir, "authorized");
    if (!path) {
        zenctl__set_err(err, ZENCTL_ERR_NOMEM, "path join failed", "authorized");
        return -1;
    }
    char buf[16] = {0};
    int rc = zenctl__read_file_string(path, buf, sizeof(buf), err);
    free(path);
    if (rc != 0) return rc;
    *out = (atoi(buf) != 0);
    return 0;
}

int zenctl_usb_set_authorized(zenctl_usb_t *usb, bool auth, zenctl_err_t *err)
{
    return write_attr_str(usb, "authorized", auth ? "1" : "0", err);
}

int zenctl_usb_get_wakeup(zenctl_usb_t *usb, bool *out, zenctl_err_t *err)
{
    if (!usb || !out) {
        zenctl__set_err(err, ZENCTL_ERR_EINVAL, "NULL usb or out", "zenctl_usb_get_wakeup");
        return -1;
    }
    char *path = zenctl_util_path_join(usb->sysfs_dir, "power/wakeup");
    if (!path) {
        zenctl__set_err(err, ZENCTL_ERR_NOMEM, "path join failed", "wakeup");
        return -1;
    }
    char buf[32] = {0};
    int rc = zenctl__read_file_string(path, buf, sizeof(buf), err);
    free(path);
    if (rc != 0) return rc;
    /* Empty string = device has no wakeup capability. */
    *out = (strcmp(buf, "enabled") == 0);
    return 0;
}

int zenctl_usb_set_wakeup(zenctl_usb_t *usb, bool on, zenctl_err_t *err)
{
    return write_attr_str(usb, "power/wakeup", on ? "enabled" : "disabled", err);
}

int zenctl_usb_reset(zenctl_usb_t *usb, zenctl_err_t *err)
{
    if (!usb) {
        zenctl__set_err(err, ZENCTL_ERR_EINVAL, "NULL usb", "zenctl_usb_reset");
        return -1;
    }
    if (usb->busnum < 0 || usb->devnum < 0) {
        /* Re-attempt to populate busnum/devnum in case they were not
         * available at open time. */
        read_bus_dev_nums(usb);
    }
    if (usb->busnum < 0 || usb->devnum < 0) {
        char ctx[128];
        snprintf(ctx, sizeof(ctx), "zenctl_usb_reset(%s)", usb->dev_path);
        zenctl__set_err(err, ZENCTL_ERR_ENOENT,
                        "busnum/devnum not available for device", ctx);
        return -1;
    }

    char devpath[128];
    snprintf(devpath, sizeof(devpath), "/dev/bus/usb/%03d/%03d",
             usb->busnum, usb->devnum);

    int fd = open(devpath, O_WRONLY | O_CLOEXEC);
    if (fd < 0) {
        zenctl__set_err(err, zenctl__errno_to_code(errno),
                        strerror(errno), devpath);
        return -1;
    }
    int rc = ioctl(fd, USBDEVFS_RESET, 0);
    int saved = errno;
    close(fd);
    if (rc < 0) {
        zenctl__set_err(err, zenctl__errno_to_code(saved),
                        strerror(saved), "USBDEVFS_RESET");
        return -1;
    }
    return 0;
}

/* Filter predicate: keep entries that look like USB device paths
 * (usbN or N-P[.Q.R]) and skip interface entries (contain ':'). */
static bool is_usb_device_entry(const char *name)
{
    if (!name || !*name) return false;
    if (strchr(name, ':')) return false;
    if (strncmp(name, "usb", 3) == 0 && isdigit((unsigned char)name[3])) {
        for (const char *c = name + 4; *c; c++)
            if (!isdigit((unsigned char)*c)) return false;
        return true;
    }
    if (!isdigit((unsigned char)name[0])) return false;
    bool last_was_sep = false;
    for (const char *c = name; *c; c++) {
        if (isdigit((unsigned char)*c)) {
            last_was_sep = false;
        } else if (*c == '-' || *c == '.') {
            if (last_was_sep) return false;
            last_was_sep = true;
        } else {
            return false;
        }
    }
    return !last_was_sep;
}

static int cmp_str(const void *a, const void *b)
{
    const char *const *pa = a;
    const char *const *pb = b;
    return strcmp(*pa, *pb);
}

int zenctl_usb_enumerate(char ***out_list, int *out_count, zenctl_err_t *err)
{
    if (!out_list || !out_count) {
        zenctl__set_err(err, ZENCTL_ERR_EINVAL, "NULL out_list or out_count",
                        "zenctl_usb_enumerate");
        return -1;
    }
    char **entries = zenctl_util_list_dir(USB_SYSFS_BASE, err);
    if (!entries) return -1;

    size_t cap = 8, n = 0;
    char **out = calloc(cap, sizeof(char *));
    if (!out) {
        for (size_t i = 0; entries[i]; i++) free(entries[i]);
        free(entries);
        zenctl__set_err(err, ZENCTL_ERR_NOMEM, "out of memory", "zenctl_usb_enumerate");
        return -1;
    }
    for (size_t i = 0; entries[i]; i++) {
        if (!is_usb_device_entry(entries[i])) continue;
        if (n + 1 >= cap) {
            size_t ncap = cap * 2;
            char **narr = realloc(out, ncap * sizeof(char *));
            if (!narr) {
                for (size_t j = 0; j < n; j++) free(out[j]);
                free(out);
                for (size_t j = 0; entries[j]; j++) free(entries[j]);
                free(entries);
                zenctl__set_err(err, ZENCTL_ERR_NOMEM, "out of memory", "zenctl_usb_enumerate");
                return -1;
            }
            out = narr; cap = ncap;
        }
        out[n] = entries[i];   /* steal the allocation */
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
