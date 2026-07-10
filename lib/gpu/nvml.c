/* nvml.c - NVIDIA NVML support via dlopen.
 *
 * The proprietary NVML library is loaded at runtime via dlopen(). All
 * symbols we use are resolved once at init() time into a static
 * function-pointer table. If the library is not installed (or any
 * required symbol is missing), init() returns -1 and every getter
 * returns -1 immediately.
 *
 * NVML returns temperatures in degrees C, power in milliwatts, and
 * clock / fan / utilization in their natural units. We normalise to
 * the units the rest of libzenctl uses (millidegrees, microwatts).
 */
#define _GNU_SOURCE

#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "nvml.h"

/* NVML enums we care about (from nvml.h). */
#define NVML_SUCCESS           0
#define NVML_TEMPERATURE_GPU   0
#define NVML_CLOCK_SM          0

/* nvmlDevice_t is an opaque pointer in NVML; model it as void*. */
typedef void *nvmlDevice_t;

/* nvmlUtilization_t (from nvml.h) - first two fields are all we use. */
typedef struct {
    unsigned int gpu;
    unsigned int memory;
} nvmlUtilization_t;

/* Function pointer types for each dlsym'd entry point. nvmlReturn_t
 * is an int in the NVML ABI. */
typedef int (*nvmlInit_v2_fn)(void);
typedef int (*nvmlShutdown_fn)(void);
typedef int (*nvmlDeviceGetCount_v2_fn)(unsigned int *);
typedef int (*nvmlDeviceGetHandleByIndex_v2_fn)(unsigned int, nvmlDevice_t *);
typedef int (*nvmlDeviceGetTemperature_fn)(nvmlDevice_t, unsigned int, unsigned int *);
typedef int (*nvmlDeviceGetFanSpeed_fn)(nvmlDevice_t, unsigned int *);
typedef int (*nvmlDeviceGetPowerUsage_fn)(nvmlDevice_t, unsigned int *);
typedef int (*nvmlDeviceGetClockInfo_fn)(nvmlDevice_t, unsigned int, unsigned int *);
typedef int (*nvmlDeviceGetUtilizationRates_fn)(nvmlDevice_t, nvmlUtilization_t *);
typedef int (*nvmlDeviceGetPowerManagementLimit_fn)(nvmlDevice_t, unsigned int *);
typedef int (*nvmlDeviceSetPowerManagementLimit_fn)(nvmlDevice_t, unsigned int);

struct nvml_fns {
    nvmlInit_v2_fn                       init;
    nvmlShutdown_fn                      shutdown;
    nvmlDeviceGetCount_v2_fn             get_count;
    nvmlDeviceGetHandleByIndex_v2_fn     get_handle;
    nvmlDeviceGetTemperature_fn          get_temp;
    nvmlDeviceGetFanSpeed_fn             get_fan;
    nvmlDeviceGetPowerUsage_fn           get_power;
    nvmlDeviceGetClockInfo_fn            get_clock;
    nvmlDeviceGetUtilizationRates_fn     get_util;
    nvmlDeviceGetPowerManagementLimit_fn get_plimit;
    nvmlDeviceSetPowerManagementLimit_fn set_plimit;
};

static void *nvml_handle = NULL;
static int   nvml_initialized = 0;
static struct nvml_fns nvml_fns;

/* Resolve all required symbols. Returns 0 if every symbol we use is
 * present, -1 otherwise (treats a partial NVML as "not available"). */
static int load_symbols(void)
{
    memset(&nvml_fns, 0, sizeof(nvml_fns));
    nvml_fns.init       = (nvmlInit_v2_fn)dlsym(nvml_handle, "nvmlInit_v2");
    nvml_fns.shutdown   = (nvmlShutdown_fn)dlsym(nvml_handle, "nvmlShutdown");
    nvml_fns.get_count  = (nvmlDeviceGetCount_v2_fn)dlsym(nvml_handle, "nvmlDeviceGetCount_v2");
    nvml_fns.get_handle = (nvmlDeviceGetHandleByIndex_v2_fn)dlsym(nvml_handle, "nvmlDeviceGetHandleByIndex_v2");
    nvml_fns.get_temp   = (nvmlDeviceGetTemperature_fn)dlsym(nvml_handle, "nvmlDeviceGetTemperature");
    nvml_fns.get_fan    = (nvmlDeviceGetFanSpeed_fn)dlsym(nvml_handle, "nvmlDeviceGetFanSpeed");
    nvml_fns.get_power  = (nvmlDeviceGetPowerUsage_fn)dlsym(nvml_handle, "nvmlDeviceGetPowerUsage");
    nvml_fns.get_clock  = (nvmlDeviceGetClockInfo_fn)dlsym(nvml_handle, "nvmlDeviceGetClockInfo");
    nvml_fns.get_util   = (nvmlDeviceGetUtilizationRates_fn)dlsym(nvml_handle, "nvmlDeviceGetUtilizationRates");
    nvml_fns.get_plimit = (nvmlDeviceGetPowerManagementLimit_fn)dlsym(nvml_handle, "nvmlDeviceGetPowerManagementLimit");
    nvml_fns.set_plimit = (nvmlDeviceSetPowerManagementLimit_fn)dlsym(nvml_handle, "nvmlDeviceSetPowerManagementLimit");
    if (!nvml_fns.init || !nvml_fns.shutdown || !nvml_fns.get_count ||
        !nvml_fns.get_handle || !nvml_fns.get_temp || !nvml_fns.get_fan ||
        !nvml_fns.get_power || !nvml_fns.get_clock || !nvml_fns.get_util ||
        !nvml_fns.get_plimit || !nvml_fns.set_plimit)
        return -1;
    return 0;
}

int nvml_init(void)
{
    if (nvml_initialized) return 0;
    /* The SONAME matches what the NVIDIA driver package installs. */
    nvml_handle = dlopen("libnvidia-ml.so.1", RTLD_LAZY);
    if (!nvml_handle) return -1;
    if (load_symbols() != 0) {
        dlclose(nvml_handle);
        nvml_handle = NULL;
        return -1;
    }
    if (nvml_fns.init() != NVML_SUCCESS) {
        dlclose(nvml_handle);
        nvml_handle = NULL;
        return -1;
    }
    nvml_initialized = 1;
    return 0;
}

void nvml_shutdown(void)
{
    if (!nvml_initialized) return;
    nvml_fns.shutdown();
    dlclose(nvml_handle);
    nvml_handle = NULL;
    nvml_initialized = 0;
    memset(&nvml_fns, 0, sizeof(nvml_fns));
}

/* Resolve a GPU index into an nvmlDevice_t handle. Returns 0 on
 * success, -1 on failure (out of range or NVML error). */
static int get_handle(int gpu_index, nvmlDevice_t *out)
{
    if (gpu_index < 0) return -1;
    unsigned int count = 0;
    if (nvml_fns.get_count(&count) != NVML_SUCCESS) return -1;
    if ((unsigned int)gpu_index >= count) return -1;
    if (nvml_fns.get_handle((unsigned int)gpu_index, out) != NVML_SUCCESS)
        return -1;
    return 0;
}

int nvml_get_temp(int gpu_index, int64_t *out)
{
    if (!nvml_initialized || !out) return -1;
    nvmlDevice_t dev;
    if (get_handle(gpu_index, &dev) != 0) return -1;
    unsigned int temp = 0;
    if (nvml_fns.get_temp(dev, NVML_TEMPERATURE_GPU, &temp) != NVML_SUCCESS)
        return -1;
    *out = (int64_t)temp * 1000;  /* C -> millidegrees C */
    return 0;
}

int nvml_get_fan_speed(int gpu_index, int *out_percent)
{
    if (!nvml_initialized || !out_percent) return -1;
    nvmlDevice_t dev;
    if (get_handle(gpu_index, &dev) != 0) return -1;
    unsigned int speed = 0;
    if (nvml_fns.get_fan(dev, &speed) != NVML_SUCCESS) return -1;
    *out_percent = (int)speed;
    return 0;
}

int nvml_get_power(int gpu_index, int64_t *out)
{
    if (!nvml_initialized || !out) return -1;
    nvmlDevice_t dev;
    if (get_handle(gpu_index, &dev) != 0) return -1;
    unsigned int mw = 0;
    if (nvml_fns.get_power(dev, &mw) != NVML_SUCCESS) return -1;
    *out = (int64_t)mw * 1000;  /* mW -> microwatts to match hwmon */
    return 0;
}

int nvml_get_clock(int gpu_index, int64_t *out_mhz)
{
    if (!nvml_initialized || !out_mhz) return -1;
    nvmlDevice_t dev;
    if (get_handle(gpu_index, &dev) != 0) return -1;
    unsigned int mhz = 0;
    if (nvml_fns.get_clock(dev, NVML_CLOCK_SM, &mhz) != NVML_SUCCESS)
        return -1;
    *out_mhz = (int64_t)mhz;
    return 0;
}

int nvml_get_utilization(int gpu_index, int *out_percent)
{
    if (!nvml_initialized || !out_percent) return -1;
    nvmlDevice_t dev;
    if (get_handle(gpu_index, &dev) != 0) return -1;
    nvmlUtilization_t util;
    memset(&util, 0, sizeof(util));
    if (nvml_fns.get_util(dev, &util) != NVML_SUCCESS) return -1;
    *out_percent = (int)util.gpu;
    return 0;
}

int nvml_get_power_limit(int gpu_index, int32_t *out)
{
    if (!nvml_initialized || !out) return -1;
    nvmlDevice_t dev;
    if (get_handle(gpu_index, &dev) != 0) return -1;
    unsigned int mw = 0;
    if (nvml_fns.get_plimit(dev, &mw) != NVML_SUCCESS) return -1;
    *out = (int32_t)mw;
    return 0;
}

int nvml_set_power_limit(int gpu_index, int32_t mw)
{
    if (!nvml_initialized) return -1;
    if (mw < 0) return -1;
    nvmlDevice_t dev;
    if (get_handle(gpu_index, &dev) != 0) return -1;
    if (nvml_fns.set_plimit(dev, (unsigned int)mw) != NVML_SUCCESS) return -1;
    return 0;
}
