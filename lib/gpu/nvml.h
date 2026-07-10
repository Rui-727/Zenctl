/* nvml.h - private NVIDIA NVML support via dlopen.
 *
 * NVML (NVIDIA Management Library) is the proprietary library shipped
 * with the NVIDIA driver. It can't be linked at build time (proprietary,
 * not always installed), so we dlopen() it lazily at first use.
 *
 * Not part of the public API; consumed only by lib/gpu/gpu.c.
 */
#ifndef ZENCTL_GPU_NVML_H
#define ZENCTL_GPU_NVML_H

#include <stdint.h>

/* Initialize: dlopen("libnvidia-ml.so.1", RTLD_LAZY), dlsym the
 * functions, call nvmlInit_v2(). Returns 0 if NVML is available and
 * initialised, -1 if not. Idempotent: subsequent calls are no-ops
 * once init has succeeded. Thread-unsafe by design (callers gate
 * NVML access through the per-gpu-handle hot path). */
int nvml_init(void);

/* Shutdown: call nvmlShutdown(), dlclose(). Safe to call without
 * prior init() or after init() failed. */
void nvml_shutdown(void);

/* All getters return 0 on success, -1 on failure (NVML not loaded,
 * GPU index out of range, or NVML call returned an error). */

/* Temperature in millidegrees C. NVML reports degrees C; we multiply
 * by 1000 to match the hwmon temp1_input convention. */
int nvml_get_temp(int gpu_index, int64_t *out);

/* Fan speed percent 0-100 (not PWM 0-255). */
int nvml_get_fan_speed(int gpu_index, int *out_percent);

/* Power usage in microwatts. NVML reports milliwatts; we multiply by
 * 1000 to match the hwmon power1_* convention. */
int nvml_get_power(int gpu_index, int64_t *out);

/* SM clock frequency in MHz. */
int nvml_get_clock(int gpu_index, int64_t *out_mhz);

/* GPU utilization percent 0-100. */
int nvml_get_utilization(int gpu_index, int *out_percent);

/* Power management limit in milliwatts. */
int nvml_get_power_limit(int gpu_index, int32_t *out);

/* Set power management limit in milliwatts. */
int nvml_set_power_limit(int gpu_index, int32_t mw);

#endif /* ZENCTL_GPU_NVML_H */
