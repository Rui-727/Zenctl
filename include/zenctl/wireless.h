/* wireless.h - Wireless (Wi-Fi / cfg80211 / nl80211) domain API
 *
 * Wraps the wireless PHY sysfs (/sys/class/ieee80211/phy<N>/), the
 * rfkill interface, and (TODO) nl80211 for TX power, power-save, and
 * regdomain. See docs/KERNEL_USB_BT_FW.md section 3.
 *
 * For v1, regdomain reads from /sys/class/regulatory/ are not
 * available; the regdomain is exposed via a stable per-phy file at
 * /sys/class/ieee80211/phy<N>/regdomain (kernel 6.x) — if absent,
 * returns ZENCTL_ERR_ENOTSUP. Setting a regdomain requires nl80211
 * (TODO); v1 returns ZENCTL_ERR_ENOTSUP.
 *
 * TX power and 802.11 power-save setting require nl80211; v1 returns
 * ZENCTL_ERR_ENOTSUP for the setters and reads TX power from the
 * sysfs file when present.
 */
#ifndef ZENCTL_WIRELESS_H
#define ZENCTL_WIRELESS_H

#include "zenctl.h"

typedef struct zenctl_wireless zenctl_wireless_t;

/* Open by phy name, e.g. "phy0". The name is the directory under
 * /sys/class/ieee80211/. Returns NULL and sets *err on failure. */
zenctl_wireless_t *zenctl_wireless_open(const char *phy, zenctl_err_t *err);
void               zenctl_wireless_close(zenctl_wireless_t *wl);

/* Regulatory domain. get returns an ISO 3166-1 alpha-2 string (e.g.
 * "US", "DE") when the kernel exposes one; set requires nl80211 and
 * is not implemented in v1. */
int zenctl_wireless_get_regdomain(zenctl_wireless_t *wl, char **out, zenctl_err_t *err);
int zenctl_wireless_set_regdomain(zenctl_wireless_t *wl, const char *country, zenctl_err_t *err);

/* TX power in mBm (100 * dBm). Reading returns the current value when
 * the kernel exposes it (per-phy sysfs file). Setting requires
 * nl80211 (TODO); v1 returns ZENCTL_ERR_ENOTSUP. */
int zenctl_wireless_get_txpower(zenctl_wireless_t *wl, int32_t *out_mBm, zenctl_err_t *err);
int zenctl_wireless_set_txpower(zenctl_wireless_t *wl, int32_t mBm, zenctl_err_t *err);

/* 802.11 power-save. Reading returns the current state when exposed
 * via sysfs. Setting requires nl80211 (TODO); v1 returns
 * ZENCTL_ERR_ENOTSUP. */
int zenctl_wireless_get_power_save(zenctl_wireless_t *wl, bool *out, zenctl_err_t *err);
int zenctl_wireless_set_power_save(zenctl_wireless_t *wl, bool on, zenctl_err_t *err);

/* rfkill soft-block. The PHY's rfkill device is located by name match
 * against /sys/class/rfkill/rfkill<N>/name (which is usually the phy
 * name, "phy0"). */
int zenctl_wireless_get_rfkill_blocked(zenctl_wireless_t *wl, bool *out, zenctl_err_t *err);
int zenctl_wireless_set_rfkill_blocked(zenctl_wireless_t *wl, bool blocked, zenctl_err_t *err);

/* Enumerate phy names (phy0, phy1, ...) under /sys/class/ieee80211/.
 * Heap-allocated strings; caller frees each and the array. */
int zenctl_wireless_enumerate(char ***out_list, int *out_count, zenctl_err_t *err);

#endif /* ZENCTL_WIRELESS_H */
