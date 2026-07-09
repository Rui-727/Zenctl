/* wireless.c - Wireless (Wi-Fi / cfg80211 / nl80211) domain implementation.
 *
 * For v1 the only knobs we actually implement end-to-end are rfkill
 * soft-block (power) and basic enumeration. The nl80211-dependent
 * controls — regdomain set, TX power, 802.11 power-save — are stubbed
 * to ZENCTL_ERR_ENOTSUP. The TODOs are tracked in
 * docs/KERNEL_USB_BT_FW.md section 3.
 */
#define _POSIX_C_SOURCE 200809L

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "zenctl/zenctl.h"
#include "zenctl/internal.h"
#include "zenctl/wireless.h"

#include "rfkill.h"
#include "util.h"

#define WL_SYSFS_BASE "/sys/class/ieee80211"

struct zenctl_wireless {
    char *phy;        /* e.g. "phy0" */
    char *sysfs_dir;  /* /sys/class/ieee80211/phy0 */
    int   index;      /* phy index parsed from name, -1 if unknown */
};

static bool phy_name_valid(const char *p)
{
    if (!p || strncmp(p, "phy", 3) != 0) return false;
    if (p[3] == '\0') return false;
    for (const char *c = p + 3; *c; c++)
        if (!isdigit((unsigned char)*c)) return false;
    return true;
}

zenctl_wireless_t *zenctl_wireless_open(const char *phy, zenctl_err_t *err)
{
    if (!phy_name_valid(phy)) {
        char ctx[128];
        snprintf(ctx, sizeof(ctx), "zenctl_wireless_open(%s)",
                 phy ? phy : "NULL");
        zenctl__set_err(err, ZENCTL_ERR_EINVAL,
                        "phy must be 'phy<N>'", ctx);
        return NULL;
    }
    zenctl_wireless_t *wl = calloc(1, sizeof(*wl));
    if (!wl) {
        zenctl__set_err(err, ZENCTL_ERR_NOMEM, "calloc failed", "zenctl_wireless_open");
        return NULL;
    }
    wl->phy = strdup(phy);
    if (!wl->phy) {
        free(wl);
        zenctl__set_err(err, ZENCTL_ERR_NOMEM, "strdup failed", "zenctl_wireless_open");
        return NULL;
    }
    wl->sysfs_dir = zenctl_util_path_join(WL_SYSFS_BASE, phy);
    if (!wl->sysfs_dir) {
        free(wl->phy); free(wl);
        zenctl__set_err(err, ZENCTL_ERR_NOMEM, "path join failed", "zenctl_wireless_open");
        return NULL;
    }
    wl->index = atoi(phy + 3);

    DIR *d = opendir(wl->sysfs_dir);
    if (!d) {
        zenctl__set_err(err, zenctl__errno_to_code(errno),
                        strerror(errno), wl->sysfs_dir);
        free(wl->sysfs_dir);
        free(wl->phy);
        free(wl);
        return NULL;
    }
    closedir(d);
    return wl;
}

void zenctl_wireless_close(zenctl_wireless_t *wl)
{
    if (!wl) return;
    free(wl->sysfs_dir);
    free(wl->phy);
    free(wl);
}

int zenctl_wireless_get_regdomain(zenctl_wireless_t *wl, char **out, zenctl_err_t *err)
{
    (void)wl; (void)out;
    /* The current regdomain is only exposed via nl80211
     * (NL80211_CMD_GET_REG). /sys/class/regulatory/ contains hint
     * subdirs, not the live value. */
    zenctl__set_err(err, ZENCTL_ERR_ENOTSUP,
                    "regdomain read requires nl80211 (TODO)",
                    "zenctl_wireless_get_regdomain");
    return -1;
}

int zenctl_wireless_set_regdomain(zenctl_wireless_t *wl, const char *country, zenctl_err_t *err)
{
    (void)wl;
    if (!country || strlen(country) != 2 ||
        !isalpha((unsigned char)country[0]) ||
        !isalpha((unsigned char)country[1])) {
        zenctl__set_err(err, ZENCTL_ERR_EINVAL,
                        "country must be an ISO 3166-1 alpha-2 code",
                        "zenctl_wireless_set_regdomain");
        return -1;
    }
    /* Requires NL80211_CMD_REQ_SET_REG. */
    zenctl__set_err(err, ZENCTL_ERR_ENOTSUP,
                    "regdomain set requires nl80211 (TODO)",
                    "zenctl_wireless_set_regdomain");
    return -1;
}

int zenctl_wireless_get_txpower(zenctl_wireless_t *wl, int32_t *out_mBm, zenctl_err_t *err)
{
    (void)wl; (void)out_mBm;
    zenctl__set_err(err, ZENCTL_ERR_ENOTSUP,
                    "txpower read requires nl80211 (TODO)",
                    "zenctl_wireless_get_txpower");
    return -1;
}

int zenctl_wireless_set_txpower(zenctl_wireless_t *wl, int32_t mBm, zenctl_err_t *err)
{
    (void)wl; (void)mBm;
    zenctl__set_err(err, ZENCTL_ERR_ENOTSUP,
                    "txpower set requires nl80211 (TODO)",
                    "zenctl_wireless_set_txpower");
    return -1;
}

int zenctl_wireless_get_power_save(zenctl_wireless_t *wl, bool *out, zenctl_err_t *err)
{
    (void)wl; (void)out;
    zenctl__set_err(err, ZENCTL_ERR_ENOTSUP,
                    "power_save read requires nl80211 (TODO)",
                    "zenctl_wireless_get_power_save");
    return -1;
}

int zenctl_wireless_set_power_save(zenctl_wireless_t *wl, bool on, zenctl_err_t *err)
{
    (void)wl; (void)on;
    zenctl__set_err(err, ZENCTL_ERR_ENOTSUP,
                    "power_save set requires nl80211 (TODO)",
                    "zenctl_wireless_set_power_save");
    return -1;
}

int zenctl_wireless_get_rfkill_blocked(zenctl_wireless_t *wl, bool *out, zenctl_err_t *err)
{
    if (!wl || !out) {
        zenctl__set_err(err, ZENCTL_ERR_EINVAL, "NULL wl or out",
                        "zenctl_wireless_get_rfkill_blocked");
        return -1;
    }
    /* rfkill name for a cfg80211 PHY is usually the phy name (e.g.
     * "phy0"); fall back to any wlan rfkill on this system. */
    int idx = zenctl_rfkill_find(wl->phy, "wlan", NULL);
    if (idx < 0) idx = zenctl_rfkill_find(NULL, "wlan", err);
    if (idx < 0) return -1;
    return zenctl_rfkill_get_soft(idx, out, err);
}

int zenctl_wireless_set_rfkill_blocked(zenctl_wireless_t *wl, bool blocked, zenctl_err_t *err)
{
    if (!wl) {
        zenctl__set_err(err, ZENCTL_ERR_EINVAL, "NULL wl",
                        "zenctl_wireless_set_rfkill_blocked");
        return -1;
    }
    int idx = zenctl_rfkill_find(wl->phy, "wlan", NULL);
    if (idx < 0) idx = zenctl_rfkill_find(NULL, "wlan", err);
    if (idx < 0) return -1;
    return zenctl_rfkill_set_soft(idx, blocked, err);
}

static int cmp_str(const void *a, const void *b)
{
    const char *const *pa = a;
    const char *const *pb = b;
    return strcmp(*pa, *pb);
}

int zenctl_wireless_enumerate(char ***out_list, int *out_count, zenctl_err_t *err)
{
    if (!out_list || !out_count) {
        zenctl__set_err(err, ZENCTL_ERR_EINVAL, "NULL out_list or out_count",
                        "zenctl_wireless_enumerate");
        return -1;
    }
    char **entries = zenctl_util_list_dir(WL_SYSFS_BASE, err);
    if (!entries) return -1;

    size_t cap = 8, n = 0;
    char **out = calloc(cap, sizeof(char *));
    if (!out) {
        for (size_t i = 0; entries[i]; i++) free(entries[i]);
        free(entries);
        zenctl__set_err(err, ZENCTL_ERR_NOMEM, "out of memory", "zenctl_wireless_enumerate");
        return -1;
    }
    for (size_t i = 0; entries[i]; i++) {
        if (!phy_name_valid(entries[i])) continue;
        if (n + 1 >= cap) {
            size_t ncap = cap * 2;
            char **narr = realloc(out, ncap * sizeof(char *));
            if (!narr) {
                for (size_t j = 0; j < n; j++) free(out[j]);
                free(out);
                for (size_t j = 0; entries[j]; j++) free(entries[j]);
                free(entries);
                zenctl__set_err(err, ZENCTL_ERR_NOMEM, "out of memory", "zenctl_wireless_enumerate");
                return -1;
            }
            out = narr; cap = ncap;
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
