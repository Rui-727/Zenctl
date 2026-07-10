/* test_nvml.c - unit tests for the dlopen-based NVML shim.
 *
 * The sandbox does not have libnvidia-ml.so.1 installed, so these
 * tests exercise the negative paths: nvml_init() returns -1, every
 * getter returns -1, and shutdown() is safe to call without init.
 * The positive path (real NVML present) is exercised on real NVIDIA
 * hardware by the smoke tests, not here.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "harness.h"

/* Forward declarations from lib/gpu/nvml.c (private header not
 * installed; the test links against the .o directly). */
int  nvml_init(void);
void nvml_shutdown(void);
int  nvml_get_temp(int gpu_index, int64_t *out);
int  nvml_get_fan_speed(int gpu_index, int *out_percent);
int  nvml_get_power(int gpu_index, int64_t *out);
int  nvml_get_clock(int gpu_index, int64_t *out_mhz);
int  nvml_get_utilization(int gpu_index, int *out_percent);
int  nvml_get_power_limit(int gpu_index, int32_t *out);
int  nvml_set_power_limit(int gpu_index, int32_t mw);

static void test_nvml_not_installed(void)
{
    /* On a build host without libnvidia-ml.so.1, init must fail. */
    int rc = nvml_init();
    OK(rc == -1, "nvml_init() returns -1 when libnvidia-ml.so.1 is absent");

    /* Getters must fail without a successful init. */
    int64_t v64 = 0;
    int vint = 0;
    int32_t v32 = 0;

    OK(nvml_get_temp(0, &v64) == -1, "nvml_get_temp fails without init");
    OK(nvml_get_fan_speed(0, &vint) == -1, "nvml_get_fan_speed fails without init");
    OK(nvml_get_power(0, &v64) == -1, "nvml_get_power fails without init");
    OK(nvml_get_clock(0, &v64) == -1, "nvml_get_clock fails without init");
    OK(nvml_get_utilization(0, &vint) == -1, "nvml_get_utilization fails without init");
    OK(nvml_get_power_limit(0, &v32) == -1, "nvml_get_power_limit fails without init");
    OK(nvml_set_power_limit(0, 100000) == -1, "nvml_set_power_limit fails without init");

    /* NULL out must also fail. */
    OK(nvml_get_temp(0, NULL) == -1, "nvml_get_temp(NULL) fails");
    OK(nvml_get_fan_speed(0, NULL) == -1, "nvml_get_fan_speed(NULL) fails");
    OK(nvml_get_power(0, NULL) == -1, "nvml_get_power(NULL) fails");
    OK(nvml_get_clock(0, NULL) == -1, "nvml_get_clock(NULL) fails");
    OK(nvml_get_utilization(0, NULL) == -1, "nvml_get_utilization(NULL) fails");
    OK(nvml_get_power_limit(0, NULL) == -1, "nvml_get_power_limit(NULL) fails");

    /* Negative index must fail (init is still -1 from above). */
    OK(nvml_get_temp(-1, &v64) == -1, "nvml_get_temp(-1) fails");
    OK(nvml_set_power_limit(-1, 0) == -1, "nvml_set_power_limit(-1) fails");
}

static void test_nvml_shutdown_safe(void)
{
    /* shutdown() is a no-op when init() was never called or failed. */
    nvml_shutdown();
    nvml_shutdown();
    OK(1, "nvml_shutdown() is safe to call without init");
}

int test_nvml_suite(void)
{
    SUITE_START("NVML dlopen shim");
    test_nvml_not_installed();
    test_nvml_shutdown_safe();
    SUITE_END();
    return SUITE_FAILURES();
}
