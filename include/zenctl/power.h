/* power.h - power domain API
 *
 * System sleep states, runtime PM, battery, AC adapter, and wake
 * sources via the sysfs power_supply class and /sys/power.
 */
#ifndef ZENCTL_POWER_H
#define ZENCTL_POWER_H

#include "zenctl.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ACPI sleep states */
typedef enum {
    ZENCTL_SLEEP_FREEZE   = 0,
    ZENCTL_SLEEP_MEM      = 1,  /* suspend to RAM */
    ZENCTL_SLEEP_DISK     = 2,  /* hibernate */
    ZENCTL_SLEEP_STANDBY  = 3,
} zenctl_sleep_state_t;

int zenctl_power_get_supported_states(char **out, zenctl_err_t *err);
int zenctl_power_suspend(zenctl_sleep_state_t state, zenctl_err_t *err);

/* mem_sleep mode (s2idle, shallow, deep) */
int zenctl_power_get_mem_sleep_mode(char **out, zenctl_err_t *err);
int zenctl_power_set_mem_sleep_mode(const char *mode, zenctl_err_t *err);

/* Runtime PM for a device */
int zenctl_power_get_runtime_pm(const char *dev_path, char **out, zenctl_err_t *err);
int zenctl_power_set_runtime_pm(const char *dev_path, const char *mode, zenctl_err_t *err);

/* Battery */
typedef struct {
    char    name[64];
    char    status[32];      /* Charging, Discharging, Full, Unknown */
    int     capacity;        /* percent */
    int64_t charge_full;     /* uAh */
    int64_t charge_full_design;
    int64_t charge_now;
    int64_t current_now;     /* uA */
    int64_t voltage_now;     /* uV */
    int     cycle_count;
    char    technology[32];  /* Li-ion, Li-poly, etc. */
} zenctl_battery_t;

int zenctl_power_battery_count(int *out, zenctl_err_t *err);
int zenctl_power_battery_get(int index, zenctl_battery_t *out, zenctl_err_t *err);

/* Charge thresholds (ThinkPads and others) */
int zenctl_power_battery_get_charge_start(int index, int *out_percent, zenctl_err_t *err);
int zenctl_power_battery_set_charge_start(int index, int percent, zenctl_err_t *err);
int zenctl_power_battery_get_charge_end(int index, int *out_percent, zenctl_err_t *err);
int zenctl_power_battery_set_charge_end(int index, int percent, zenctl_err_t *err);

/* AC adapter */
int zenctl_power_ac_online(bool *out, zenctl_err_t *err);

/* Wake sources */
int zenctl_power_get_wakeup_devices(char ***out_list, int *out_count, zenctl_err_t *err);

/* Toggle a wake source. Writing a device name to /proc/acpi/wakeup
 * toggles its enabled state, so to set an absolute state we first
 * read the current state and only write if it differs. Returns 0 on
 * success (including when the device is already in the desired
 * state), -1 on error (ZENCTL_ERR_ENOENT if the device is not listed
 * in /proc/acpi/wakeup, ZENCTL_ERR_EINVAL on a NULL or malformed
 * device name). */
int zenctl_power_set_wakeup(const char *device, bool enabled, zenctl_err_t *err);

#ifdef __cplusplus
}
#endif

#endif /* ZENCTL_POWER_H */
