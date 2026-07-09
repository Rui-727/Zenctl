/* gpu.h - gpu domain API
 *
 * GPU control surface covering DRM sysfs, hwmon, and vendor-specific
 * interfaces. v1 implements the common sysfs/hwmon surface and the
 * AMDGPU-specific controls. NVML (NVIDIA proprietary) and i915-specific
 * controls are stubbed for later.
 */
#ifndef ZENCTL_GPU_H
#define ZENCTL_GPU_H

#include "zenctl.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct zenctl_gpu zenctl_gpu_t;

/* Open by card index (0, 1, ...) or by DRM device path.
 * The path form accepts either /dev/dri/cardN or
 * /sys/class/drm/cardN. */
zenctl_gpu_t *zenctl_gpu_open(int card_index, zenctl_err_t *err);
zenctl_gpu_t *zenctl_gpu_open_path(const char *drm_path, zenctl_err_t *err);
void          zenctl_gpu_close(zenctl_gpu_t *gpu);

/* Driver info */
int zenctl_gpu_get_driver(zenctl_gpu_t *gpu, char **out, zenctl_err_t *err);
int zenctl_gpu_get_vendor(zenctl_gpu_t *gpu, char **out, zenctl_err_t *err);

/* Temperature (via hwmon, millidegrees C) */
int zenctl_gpu_get_temp(zenctl_gpu_t *gpu, int64_t *out_millideg, zenctl_err_t *err);

/* Fan control (via hwmon) */
int zenctl_gpu_get_fan_pwm(zenctl_gpu_t *gpu, int *out, zenctl_err_t *err);
int zenctl_gpu_set_fan_pwm(zenctl_gpu_t *gpu, int pwm, zenctl_err_t *err);
int zenctl_gpu_get_fan_auto(zenctl_gpu_t *gpu, bool *out, zenctl_err_t *err);
int zenctl_gpu_set_fan_auto(zenctl_gpu_t *gpu, bool auto_mode, zenctl_err_t *err);

/* Power (via hwmon, microwatts) */
int zenctl_gpu_get_power(zenctl_gpu_t *gpu, int64_t *out_microwatts, zenctl_err_t *err);

/* Clock frequency (via hwmon, MHz) */
int zenctl_gpu_get_freq(zenctl_gpu_t *gpu, int64_t *out_mhz, zenctl_err_t *err);

/* AMDGPU-specific (only works if driver == amdgpu) */
int zenctl_gpu_amdgpu_get_power_profile(zenctl_gpu_t *gpu, char **out, zenctl_err_t *err);
int zenctl_gpu_amdgpu_set_power_profile(zenctl_gpu_t *gpu, const char *profile, zenctl_err_t *err);
int zenctl_gpu_amdgpu_get_dpm_state(zenctl_gpu_t *gpu, char **out, zenctl_err_t *err);
int zenctl_gpu_amdgpu_get_busy_percent(zenctl_gpu_t *gpu, int *out, zenctl_err_t *err);

#ifdef __cplusplus
}
#endif

#endif /* ZENCTL_GPU_H */
