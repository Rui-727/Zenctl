/* test_power.c - power domain unit tests against a mock sysfs tree. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "zenctl/zenctl.h"
#include "harness.h"
#include "mock_sysfs.h"

static void test_supported_states(void)
{
    mock_sysfs_create_file("sys/power/state", "freeze mem disk");

    char *s = NULL;
    zenctl_err_t err;
    memset(&err, 0, sizeof(err));
    int rc = zenctl_power_get_supported_states(&s, &err);
    OK(rc == 0, "get_supported_states returns 0");
    OK(s && strcmp(s, "freeze mem disk") == 0,
       "get_supported_states returns \"freeze mem disk\"");
    free(s);

    /* suspend() validates the state against the file then writes.
     * The write overwrites the file, so we have to restore the
     * fixture between suspend calls. */
    memset(&err, 0, sizeof(err));
    rc = zenctl_power_suspend(ZENCTL_SLEEP_FREEZE, &err);
    OK(rc == 0, "suspend(FREEZE) returns 0 against mock");
    char buf[64];
    int n = mock_sysfs_read_file("sys/power/state", buf, sizeof(buf));
    OK(n >= 0, "state file exists after suspend write");
    OK(strcmp(buf, "freeze") == 0, "state file contains \"freeze\"");

    /* Restore the file and test suspend(DISK). */
    mock_sysfs_create_file("sys/power/state", "freeze mem disk");
    memset(&err, 0, sizeof(err));
    rc = zenctl_power_suspend(ZENCTL_SLEEP_DISK, &err);
    OK(rc == 0, "suspend(DISK) returns 0 when disk is supported");

    /* Test that an unsupported state is rejected. Rewrite the fixture
     * to only have "freeze mem". */
    mock_sysfs_create_file("sys/power/state", "freeze mem");
    memset(&err, 0, sizeof(err));
    rc = zenctl_power_suspend(ZENCTL_SLEEP_DISK, &err);
    OK(rc == -1, "suspend(DISK) rejected when not in state file");
    OK(err.code == ZENCTL_ERR_ENOTSUP,
       "suspend(unsupported) sets ZENCTL_ERR_ENOTSUP");

    /* STANDBY is never in the standard state list on x86 but we
     * don't assert that here; just check the bad-state path. */
    memset(&err, 0, sizeof(err));
    rc = zenctl_power_suspend((zenctl_sleep_state_t)999, &err);
    OK(rc == -1, "suspend(invalid enum) rejected");
    OK(err.code == ZENCTL_ERR_EINVAL,
       "suspend(invalid enum) sets ZENCTL_ERR_EINVAL");
}

static void test_battery_and_ac(void)
{
    /* Build /sys/class/power_supply/{BAT0,AC0}. */
    mock_sysfs_create_dir("sys/class/power_supply/BAT0");
    mock_sysfs_create_file("sys/class/power_supply/BAT0/type", "Battery");
    mock_sysfs_create_file("sys/class/power_supply/BAT0/status", "Charging");
    mock_sysfs_create_file("sys/class/power_supply/BAT0/capacity", "72");
    mock_sysfs_create_file("sys/class/power_supply/BAT0/technology",
                           "Li-ion");
    mock_sysfs_create_file("sys/class/power_supply/BAT0/cycle_count", "42");
    mock_sysfs_create_file("sys/class/power_supply/BAT0/charge_full",
                           "50000000");
    mock_sysfs_create_file("sys/class/power_supply/BAT0/charge_full_design",
                           "60000000");
    mock_sysfs_create_file("sys/class/power_supply/BAT0/charge_now",
                           "36000000");
    mock_sysfs_create_file("sys/class/power_supply/BAT0/current_now",
                           "1500000");
    mock_sysfs_create_file("sys/class/power_supply/BAT0/voltage_now",
                           "12500000");

    mock_sysfs_create_dir("sys/class/power_supply/AC0");
    mock_sysfs_create_file("sys/class/power_supply/AC0/type", "Mains");
    mock_sysfs_create_file("sys/class/power_supply/AC0/online", "1");

    /* battery count */
    int n = -1;
    zenctl_err_t err;
    memset(&err, 0, sizeof(err));
    int rc = zenctl_power_battery_count(&n, &err);
    OK(rc == 0, "battery_count returns 0");
    OK(n == 1, "battery_count returns 1 (BAT0)");

    /* battery get */
    zenctl_battery_t bat;
    memset(&bat, 0, sizeof(bat));
    memset(&err, 0, sizeof(err));
    rc = zenctl_power_battery_get(0, &bat, &err);
    OK(rc == 0, "battery_get(0) returns 0");
    OK(strcmp(bat.name, "BAT0") == 0, "battery name == \"BAT0\"");
    OK(strcmp(bat.status, "Charging") == 0, "battery status == \"Charging\"");
    OK(bat.capacity == 72, "battery capacity == 72");
    OK(strcmp(bat.technology, "Li-ion") == 0,
       "battery technology == \"Li-ion\"");
    OK(bat.cycle_count == 42, "battery cycle_count == 42");
    OK(bat.charge_full == 50000000LL, "battery charge_full == 50000000");
    OK(bat.charge_full_design == 60000000LL,
       "battery charge_full_design == 60000000");
    OK(bat.charge_now == 36000000LL, "battery charge_now == 36000000");
    OK(bat.current_now == 1500000LL, "battery current_now == 1500000");
    OK(bat.voltage_now == 12500000LL, "battery voltage_now == 12500000");

    /* out-of-range battery index */
    memset(&err, 0, sizeof(err));
    rc = zenctl_power_battery_get(5, &bat, &err);
    OK(rc == -1, "battery_get(5) returns -1 (out of range)");
    OK(err.code == ZENCTL_ERR_ENOENT,
       "battery_get(out-of-range) sets ZENCTL_ERR_ENOENT");

    /* AC online */
    bool online = false;
    memset(&err, 0, sizeof(err));
    rc = zenctl_power_ac_online(&online, &err);
    OK(rc == 0, "ac_online returns 0");
    OK(online == true, "ac_online returns true (AC0/online==1)");

    /* Flip AC to offline and verify. */
    mock_sysfs_create_file("sys/class/power_supply/AC0/online", "0");
    online = true;
    memset(&err, 0, sizeof(err));
    rc = zenctl_power_ac_online(&online, &err);
    OK(rc == 0, "ac_online returns 0 (second call)");
    OK(online == false, "ac_online returns false (AC0/online==0)");
}

int test_power_suite(void)
{
    SUITE_START("power domain");
    test_supported_states();
    test_battery_and_ac();
    SUITE_END();
    return SUITE_FAILURES();
}
