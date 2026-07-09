/* thermal.h - thermal domain API
 *
 * Thermal zone, cooling device, and hwmon sensor access via the
 * sysfs thermal and hwmon classes.
 */
#ifndef ZENCTL_THERMAL_H
#define ZENCTL_THERMAL_H

#include "zenctl.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct zenctl_thermal zenctl_thermal_t;

/* Open by zone name, e.g. "thermal_zone0" */
zenctl_thermal_t *zenctl_thermal_open(const char *zone_name, zenctl_err_t *err);
void              zenctl_thermal_close(zenctl_thermal_t *tz);

/* Temperature (millidegrees C) */
int zenctl_thermal_get_temp(zenctl_thermal_t *tz, int64_t *out, zenctl_err_t *err);

/* Zone info */
int zenctl_thermal_get_type(zenctl_thermal_t *tz, char **out, zenctl_err_t *err);
int zenctl_thermal_get_policy(zenctl_thermal_t *tz, char **out, zenctl_err_t *err);
int zenctl_thermal_set_policy(zenctl_thermal_t *tz, const char *policy, zenctl_err_t *err);
int zenctl_thermal_list_policies(zenctl_thermal_t *tz, char **out, zenctl_err_t *err);

/* Zone mode */
int zenctl_thermal_get_mode(zenctl_thermal_t *tz, bool *enabled, zenctl_err_t *err);
int zenctl_thermal_set_mode(zenctl_thermal_t *tz, bool enabled, zenctl_err_t *err);

/* Trip points */
typedef struct {
    int      index;
    int64_t  temp;        /* millidegrees C */
    char     type[32];    /* active, passive, hot, critical */
    int64_t  hysteresis;  /* millidegrees C */
} zenctl_trip_t;

int zenctl_thermal_get_trip_count(zenctl_thermal_t *tz, int *out, zenctl_err_t *err);
int zenctl_thermal_get_trip(zenctl_thermal_t *tz, int index, zenctl_trip_t *out, zenctl_err_t *err);

/* Cooling devices */
typedef struct {
    int  index;
    char type[64];
    int  cur_state;
    int  max_state;
} zenctl_cooling_t;

int zenctl_thermal_get_cooling_count(zenctl_thermal_t *tz, int *out, zenctl_err_t *err);
int zenctl_thermal_get_cooling(zenctl_thermal_t *tz, int index, zenctl_cooling_t *out, zenctl_err_t *err);
int zenctl_thermal_set_cooling_state(zenctl_thermal_t *tz, int index, int state, zenctl_err_t *err);

/* hwmon sensors (standalone, not tied to a thermal zone) */
typedef struct {
    char    name[64];
    char    label[64];
    int64_t temp;       /* millidegrees C */
    int64_t temp_max;   /* millidegrees C, or -1 if not set */
    int64_t temp_crit;  /* millidegrees C, or -1 if not set */
} zenctl_hwmon_temp_t;

int zenctl_thermal_hwmon_count(int *out, zenctl_err_t *err);
int zenctl_thermal_hwmon_get_temp(int hwmon_index, int sensor_index,
                                  zenctl_hwmon_temp_t *out, zenctl_err_t *err);

/* Fan control via hwmon */
int zenctl_thermal_hwmon_get_fan_pwm(int hwmon_index, int fan_index, int *out, zenctl_err_t *err);
int zenctl_thermal_hwmon_set_fan_pwm(int hwmon_index, int fan_index, int pwm, zenctl_err_t *err);
int zenctl_thermal_hwmon_get_fan_auto(int hwmon_index, int fan_index, bool *out, zenctl_err_t *err);
int zenctl_thermal_hwmon_set_fan_auto(int hwmon_index, int fan_index, bool auto_mode, zenctl_err_t *err);

#ifdef __cplusplus
}
#endif

#endif /* ZENCTL_THERMAL_H */
