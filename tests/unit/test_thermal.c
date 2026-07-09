/* test_thermal.c - thermal domain unit tests against a mock sysfs tree. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "zenctl/zenctl.h"
#include "harness.h"
#include "mock_sysfs.h"

static void test_thermal_zone(void)
{
    /* Build /sys/class/thermal/thermal_zone0 with temp, type, policy,
     * 3 trip points, and 2 cooling-device symlinks. */
    mock_sysfs_create_dir("sys/class/thermal/thermal_zone0");
    mock_sysfs_create_file("sys/class/thermal/thermal_zone0/temp", "45000");
    mock_sysfs_create_file("sys/class/thermal/thermal_zone0/type", "acpitz");
    mock_sysfs_create_file("sys/class/thermal/thermal_zone0/policy",
                           "step_wise");
    mock_sysfs_create_file(
        "sys/class/thermal/thermal_zone0/available_policies",
        "step_wise fair-share");

    /* Trip points 0..2 */
    mock_sysfs_create_file(
        "sys/class/thermal/thermal_zone0/trip_point_0_temp", "27000");
    mock_sysfs_create_file(
        "sys/class/thermal/thermal_zone0/trip_point_0_type", "active");
    mock_sysfs_create_file(
        "sys/class/thermal/thermal_zone0/trip_point_0_hyst", "2");

    mock_sysfs_create_file(
        "sys/class/thermal/thermal_zone0/trip_point_1_temp", "60000");
    mock_sysfs_create_file(
        "sys/class/thermal/thermal_zone0/trip_point_1_type", "passive");
    mock_sysfs_create_file(
        "sys/class/thermal/thermal_zone0/trip_point_1_hyst", "5");

    mock_sysfs_create_file(
        "sys/class/thermal/thermal_zone0/trip_point_2_temp", "100000");
    mock_sysfs_create_file(
        "sys/class/thermal/thermal_zone0/trip_point_2_type", "critical");
    mock_sysfs_create_file(
        "sys/class/thermal/thermal_zone0/trip_point_2_hyst", "0");

    /* Cooling devices: cdev0 and cdev1 symlinks pointing at
     * cooling_device0 and cooling_device1. The kernel exposes them
     * as symlinks; thermal.c reads them with readlink. */
    mock_sysfs_create_symlink(
        "sys/class/thermal/thermal_zone0/cdev0",
        "../../virtual/thermal/cooling_device0");
    mock_sysfs_create_symlink(
        "sys/class/thermal/thermal_zone0/cdev1",
        "../../virtual/thermal/cooling_device1");

    /* And the cooling devices themselves (zone_cdev_index extracts
     * the trailing N from the basename of the symlink target). */
    mock_sysfs_create_dir("sys/class/thermal/cooling_device0");
    mock_sysfs_create_file("sys/class/thermal/cooling_device0/type",
                           "Processor");
    mock_sysfs_create_file("sys/class/thermal/cooling_device0/cur_state",
                           "0");
    mock_sysfs_create_file("sys/class/thermal/cooling_device0/max_state",
                           "10");

    mock_sysfs_create_dir("sys/class/thermal/cooling_device1");
    mock_sysfs_create_file("sys/class/thermal/cooling_device1/type",
                           "intel_powerclamp");
    mock_sysfs_create_file("sys/class/thermal/cooling_device1/cur_state",
                           "0");
    mock_sysfs_create_file("sys/class/thermal/cooling_device1/max_state",
                           "50");

    zenctl_err_t err;
    memset(&err, 0, sizeof(err));
    zenctl_thermal_t *tz = zenctl_thermal_open("thermal_zone0", &err);
    OK(tz != NULL, "thermal_open(\"thermal_zone0\") succeeds");
    if (!tz) return;

    /* temp */
    int64_t t = 0;
    memset(&err, 0, sizeof(err));
    OK(zenctl_thermal_get_temp(tz, &t, &err) == 0, "get_temp returns 0");
    OK(t == 45000, "get_temp returns 45000 (mC)");

    /* type */
    char *s = NULL;
    memset(&err, 0, sizeof(err));
    OK(zenctl_thermal_get_type(tz, &s, &err) == 0, "get_type returns 0");
    OK(s && strcmp(s, "acpitz") == 0, "get_type returns \"acpitz\"");
    free(s);

    /* policy */
    memset(&err, 0, sizeof(err));
    s = NULL;
    OK(zenctl_thermal_get_policy(tz, &s, &err) == 0, "get_policy returns 0");
    OK(s && strcmp(s, "step_wise") == 0,
       "get_policy returns \"step_wise\"");
    free(s);

    /* trip count */
    int count = -1;
    memset(&err, 0, sizeof(err));
    OK(zenctl_thermal_get_trip_count(tz, &count, &err) == 0,
       "get_trip_count returns 0");
    OK(count == 3, "get_trip_count returns 3");

    /* trip 0 values */
    zenctl_trip_t trip;
    memset(&trip, 0, sizeof(trip));
    memset(&err, 0, sizeof(err));
    OK(zenctl_thermal_get_trip(tz, 0, &trip, &err) == 0,
       "get_trip(0) returns 0");
    OK(trip.temp == 27000, "trip 0 temp == 27000");
    OK(strcmp(trip.type, "active") == 0, "trip 0 type == \"active\"");
    OK(trip.hysteresis == 2000, "trip 0 hysteresis == 2000 mC (2 C -> mC)");

    /* trip 2 (critical) */
    memset(&trip, 0, sizeof(trip));
    memset(&err, 0, sizeof(err));
    OK(zenctl_thermal_get_trip(tz, 2, &trip, &err) == 0,
       "get_trip(2) returns 0");
    OK(trip.temp == 100000, "trip 2 temp == 100000");
    OK(strcmp(trip.type, "critical") == 0, "trip 2 type == \"critical\"");

    /* cooling count */
    int ccount = -1;
    memset(&err, 0, sizeof(err));
    OK(zenctl_thermal_get_cooling_count(tz, &ccount, &err) == 0,
       "get_cooling_count returns 0");
    OK(ccount == 2, "get_cooling_count returns 2");

    /* cooling device 0 */
    zenctl_cooling_t cdev;
    memset(&cdev, 0, sizeof(cdev));
    memset(&err, 0, sizeof(err));
    OK(zenctl_thermal_get_cooling(tz, 0, &cdev, &err) == 0,
       "get_cooling(0) returns 0");
    OK(strcmp(cdev.type, "Processor") == 0,
       "cooling device 0 type == \"Processor\"");
    OK(cdev.max_state == 10, "cooling device 0 max_state == 10");
    OK(cdev.cur_state == 0, "cooling device 0 cur_state == 0");

    /* cooling device 1 */
    memset(&cdev, 0, sizeof(cdev));
    memset(&err, 0, sizeof(err));
    OK(zenctl_thermal_get_cooling(tz, 1, &cdev, &err) == 0,
       "get_cooling(1) returns 0");
    OK(strcmp(cdev.type, "intel_powerclamp") == 0,
       "cooling device 1 type == \"intel_powerclamp\"");
    OK(cdev.max_state == 50, "cooling device 1 max_state == 50");

    zenctl_thermal_close(tz);
}

static void test_thermal_open_missing(void)
{
    zenctl_err_t err;
    memset(&err, 0, sizeof(err));
    zenctl_thermal_t *tz = zenctl_thermal_open("thermal_zone9", &err);
    OK(tz == NULL, "thermal_open(\"thermal_zone9\") returns NULL");
    OK(err.code == ZENCTL_ERR_ENOENT,
       "thermal_open(missing) sets ZENCTL_ERR_ENOENT");
}

static void test_thermal_open_bad_name(void)
{
    zenctl_err_t err;
    memset(&err, 0, sizeof(err));
    zenctl_thermal_t *tz = zenctl_thermal_open("not_a_zone", &err);
    OK(tz == NULL, "thermal_open(\"not_a_zone\") rejected");
    OK(err.code == ZENCTL_ERR_EINVAL,
       "thermal_open(\"not_a_zone\") sets ZENCTL_ERR_EINVAL");
}

int test_thermal_suite(void)
{
    SUITE_START("thermal domain");
    test_thermal_zone();
    test_thermal_open_missing();
    test_thermal_open_bad_name();
    SUITE_END();
    return SUITE_FAILURES();
}
