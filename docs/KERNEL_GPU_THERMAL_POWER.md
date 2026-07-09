# Linux kernel GPU, thermal, and power control interfaces

A precise map of every sysfs, ioctl, and (where relevant) library
interface the Linux kernel exposes for GPU, thermal, and power
control. This is the spec the Zenctl `lib/gpu`, `lib/thermal`, and
`lib/power` implementations code against.

Conventions used below:

- **Path**: exact filesystem path. `N` is a card number, `X` is a
  thermal-zone or cooling-device index, `Y` is a trip-point or sensor
  index.
- **Read**: what a `cat` returns. Newline-terminated unless noted.
- **Write**: what an `echo` must send. Quoted strings must match
  exactly (case-insensitive for some files — noted where relevant).
- **Perms**: `R` = read-only, `W` = write-only, `RW` = read-write,
  followed by the kernel-ABI requirement (Required / Optional). Sysfs
  files are owned `root:root` by default; world-readable unless noted.
- **Units**: millidegree Celsius (m°C), millivolts (mV), microvolts
  (µV), microamps (µA), microwatts (µW), microjoules (µJ),
  milliwatts (mW), milliseconds (ms), microseconds (µs), RPM, Hz,
  MHz, percent, bytes.
- **Since**: kernel version the file was introduced in, when the ABI
  doc records one.
- **Quirks**: format gotchas the implementation must obey.

The canonical ABI references are:

- `Documentation/ABI/testing/sysfs-class-thermal`
- `Documentation/ABI/testing/sysfs-class-hwmon`
- `Documentation/ABI/testing/sysfs-class-power`
- `Documentation/ABI/testing/sysfs-power`
- `Documentation/ABI/testing/sysfs-devices-power`
- `Documentation/ABI/testing/sysfs-class-drm`
- `Documentation/ABI/testing/sysfs-driver-intel-i915-hwmon`
- `Documentation/ABI/testing/sysfs-driver-intel-xe-hwmon`
- `Documentation/driver-api/thermal/sysfs-api.rst`
- `Documentation/admin-guide/pm/sleep-states.rst`
- `Documentation/gpu/amdgpu/thermal.rst`
- `Documentation/gpu/drm-uapi.rst`
- `include/uapi/drm/drm.h` and `include/uapi/drm/drm_mode.h`

This document reflects Linux v6.20-rc1 (torvalds/master, post-6.19).

---

## GPU

### DRM core: device nodes and sysfs layout

DRM exposes three character-device flavours per GPU under `/dev/dri/`:

| Node            | Pattern            | Purpose                                          |
|-----------------|--------------------|--------------------------------------------------|
| Primary node    | `/dev/dri/cardN`   | Legacy + KMS + render; requires auth or DRM-Master |
| Control node    | `/dev/dri/controlD`| Unused (placeholder, never wired up)             |
| Render node     | `/dev/dri/renderDN`| Unprivileged GPU compute; no modesetting         |

Render nodes exist only when the driver sets `DRIVER_RENDER`. The
render-node number is offset by 128 from the primary node
(`renderD128` for `card0`). Only `DRM_RENDER_ALLOW` ioctls are
permitted on render nodes (no `GEM_OPEN`, no modesetting).

Sysfs: every DRM device appears as a class device under
`/sys/class/drm/`:

```
/sys/class/drm/
├── card0/                       → primary class device
│   ├── device/                  → PCI (or platform) parent
│   │   ├── uevent
│   │   ├── vendor → 0x<vendor_id>
│   │   ├── device → 0x<device_id>
│   │   ├── subsystem_vendor
│   │   ├── subsystem_device
│   │   ├── revision
│   │   ├── class → 0x030000   (VGA, 0x030200 = 3D controller)
│   │   ├── irq → <IRQ>
│   │   ├── resource
│   │   ├── resource0 … resourceN   (BARs)
│   │   ├── rom
│   │   ├── msi_irqs/
│   │   ├── power/                  (see Power section)
│   │   ├── hwmon/                  (symlink → hwmon class, if any)
│   │   └── … driver-specific sysfs
│   ├── dev → <major>:<minor>     (226:0 for card0)
│   ├── device → symlink to ../../devices/.../0000:00:02.0
│   ├── subsystem → ../../../../class/drm
│   ├── uevent
│   └── boot_display → 1 if device drove the boot display (6.20+)
├── card0-DP-1/, card0-HDMI-A-1/, card0-eDP-1/, …   (one per connector)
│   ├── status → connected | disconnected | unknown
│   ├── enabled → enabled | disabled
│   ├── modes → "1920x1080\n1680x1050\n…"
│   ├── dpms → On | Off
│   └── connector_id (debug)
├── controlD64/  (unused)
├── renderD128/
│   ├── device/ → same parent as card0
│   └── dev → 226:128
└── version   (no — this is ioctl-only, see below)
```

Connector subdirs (`card0-<type>-<n>`) are created per registered
connector. Type names include `VGA`, `DVI-I`, `DVI-D`, `DVI-A`,
`Composite`, `SVIDEO`, `LVDS`, `Component`, `DIN`, `DP`,
`HDMI-A`, `HDMI-B`, `TV`, `eDP`, `Virtual`, `DSI`, `DPAux`,
`Writeback`, `SPI`, `USB`, `Mipi`.

Connector sysfs files:

| File                | Perms | Read format                       | Notes                                  |
|---------------------|-------|-----------------------------------|----------------------------------------|
| `status`            | R     | `connected`/`disconnected`/`unknown` | Refreshed on hotplug uevent        |
| `enabled`           | R     | `enabled`/`disabled`              | Reflects atomic commit                 |
| `modes`             | R     | newline-separated `WxH`           | Empty if disconnected                  |
| `dpms`              | R     | `On`/`Off`                        | Legacy; modern drivers use atomic props|
| `connector_id`      | R     | decimal u32                       | Matches KMS object ID                  |

Card-level `boot_display` (since v6.20, sysfs-class-drm ABI):
present and contains `1` if a display connected to this device was
used to show the boot sequence. Missing or `0` otherwise.

The PCI `class` file reads as `0x030000` (VGA), `0x030200` (3D
controller, common for dGPUs in headless compute mode), or
`0x038000` (other display).

### DRM ioctls (overview)

All DRM ioctls use magic `'d'` and are issued via `ioctl(fd,
DRM_IOCTL_*, &arg)`. The fd is obtained by opening
`/dev/dri/cardN` (or `renderD128`). Macro forms:

```c
#define DRM_IO(nr)              _IO('d', nr)
#define DRM_IOR(nr,type)        _IOR('d', nr, type)
#define DRM_IOW(nr,type)        _IOW('d', nr, type)
#define DRM_IOWR(nr,type)       _IOWR('d', nr, type)
```

The six ioctls called out in the task spec:

#### DRM_IOCTL_VERSION  (0x00, RW)

`#define DRM_IOCTL_VERSION DRM_IOWR(0x00, struct drm_version)`

Returns driver name and version. Idiomatic entry point; libdrm's
`drmGetVersion()` wraps this.

```c
struct drm_version {
    int version_major;
    int version_minor;
    int version_patchlevel;
    __kernel_size_t name_len;
    char __user *name;          // e.g. "amdgpu", "i915", "nouveau", "xe", "nvidia-drm"
    __kernel_size_t date_len;
    char __user *date;
    __kernel_size_t desc_len;
    char __user *desc;
};
```

Usage: call with all `*_len=0` and pointers NULL to learn sizes,
then allocate and call again. Driver-name strings Zenctl must
recognise: `amdgpu`, `radeon`, `nouveau`, `i915`, `xe`, `nvidia-drm`,
`vmwgfx`, `virtio_gpu`, `mgag200`, `ast`, `cirrus`, `bochs`, `qxl`,
`panfrost`, `panthor`, `lima`, `msm`, `vc4`, `v3d`, `etnaviv`,
`imx`, `tegra`, `omapdrm`, `exynos`, `rockchip`, `meson`, `mediatek`,
`komeda`, `display`, `mxsfb`, `pl111`, `stm`, `sun4i`, `zynqmp`,
`tidss`, `kmb`, `simpledrm`, `vkms`, `udl`, `gma500`.

#### DRM_IOCTL_MODE_GETRESOURCES  (0xA0, RW)

`#define DRM_IOCTL_MODE_GETRESOURCES DRM_IOWR(0xA0, struct drm_mode_card_res)`

Enumerates KMS object IDs (framebuffers, CRTCs, connectors, encoders)
plus framebuffer-size limits. Caller sets pointer fields to NULL and
`count_*` to 0 on first call to learn counts, then re-issues with
allocated arrays.

```c
struct drm_mode_card_res {
    __u64 fb_id_ptr;
    __u64 crtc_id_ptr;
    __u64 connector_id_ptr;
    __u64 encoder_id_ptr;
    __u32 count_fbs;
    __u32 count_crtcs;
    __u32 count_connectors;
    __u32 count_encoders;
    __u32 min_width, max_width;
    __u32 min_height, max_height;
};
```

#### DRM_IOCTL_MODE_GETCONNECTOR  (0xA7, RW)

`#define DRM_IOCTL_MODE_GETCONNECTOR DRM_IOWR(0xA7, struct drm_mode_get_connector)`

Returns connector state, modes, encoders, and atomic properties.
Two-call pattern: query with `count_*=0` to learn sizes, then call
again with allocated `__u32` arrays for `encoders_ptr`/`props_ptr`,
`__u64` array for `prop_values_ptr`, and `struct drm_mode_modeinfo`
array for `modes_ptr`.

```c
struct drm_mode_get_connector {
    __u64 encoders_ptr;          // array of encoder object IDs
    __u64 modes_ptr;             // array of struct drm_mode_modeinfo
    __u64 props_ptr;             // array of property IDs
    __u64 prop_values_ptr;       // array of property values (__u64)
    __u32 count_modes, count_props, count_encoders;
    __u32 encoder_id, connector_id;
    __u32 connector_type;        // DRM_MODE_CONNECTOR_*
    __u32 connector_type_id;     // per-type index (unstable)
    __u32 connection;            // DRM_MODE_CONNECTED=1, DISCONNECTED=2, UNKNOWN=3
    __u32 mm_width, mm_height;   // physical size of attached sink
    __u32 subpixel;              // DRM_MODE_SUBPIXEL_*
    __u32 pad;
};
```

Connector type constants (`DRM_MODE_CONNECTOR_Unknown=0`, `_VGA=1`,
`_DVII=2`, `_DVID=3`, `_DVIA=4`, `_Composite=5`, `_SVIDEO=6`,
`_LVDS=7`, `_Component=8`, `_NinePinDIN=9`, `_DisplayPort=10`,
`_HDMIA=11`, `_HDMIB=12`, `_TV=13`, `_eDP=14`, `_VIRTUAL=15`,
`_DSI=16`, `_DPI=17`, `_WRITEBACK=18`, `_SPI=19`, `_USB=20`).

Quirks: the same `(type, type_id)` can vary across reboots depending
on driver probe order. Use `connector_id` for stable identification
within a single boot.

#### DRM_IOCTL_MODE_GETPROPERTY  (0xAA, RW)

`#define DRM_IOCTL_MODE_GETPROPERTY DRM_IOWR(0xAA, struct drm_mode_get_property)`

Returns metadata for one KMS property by ID. Needed to translate
the numeric property IDs returned by `GETCONNECTOR`/`OBJ_GETPROPERTIES`
into human-readable names and to learn the value range / enum
mappings.

```c
struct drm_mode_get_property {
    __u64 values_ptr;            // for RANGE: 2 u64s [min,max]; for BLOB: 1 u32 length
    __u64 enum_blob_ptr;         // array of struct drm_mode_property_enum
    __u32 prop_id;               // set by caller
    __u32 flags;                 // DRM_MODE_PROP_*
    char name[DRM_PROP_NAME_LEN]; // 32 bytes, NUL-terminated
    __u32 count_values;
    __u32 count_enum_blobs;
};
```

Property flag bits (`DRM_MODE_PROP_*`):
- `RANGE      = 1<<1` — value is in `[values[0], values[1]]`
- `IMMUTABLE  = 1<<2` — read-only, cannot be set
- `ENUM       = 1<<3` — value is one of the listed enum entries
- `BLOB       = 1<<4` — value is a blob ID
- `BITMASK    = 1<<5` — value is a bitmask of enum entries
- `SIGNED_RANGE= 1<<6` — like RANGE but signed

Standard property names Zenctl must recognise (see
`Documentation/gpu/kms-properties.csv`): `ACTIVE`, `MODE_ID`,
`FB_ID`, `CRTC_ID`, `SRC_X`, `SRC_Y`, `SRC_W`, `SRC_H`, `CRTC_X`,
`CRTC_Y`, `CRTC_W`, `CRTC_H`, `rotation`, `zpos`, `pixel blend mode`,
`alpha`, `IN_FORMATS`, `BG_COLOR`, `GAMMA_LUT`, `DEGAMMA_LUT`,
`CTM`, `VRR_ENABLED`, `HDR_OUTPUT_METADATA`, `COLOR_RANGE`,
`COLOR_ENCODING`, `max bpc`, `Broadcast RGB`, `link-status`,
`non-desktop`, `panel orientation`, `Content Protection`, `HDCP Content Protection`,
`subconnector`, `EDID`, `DPMS`, `scaling mode`, `dithering`,
`underscan`, `brightness`, `contrast`, `saturation`, `hue`.

#### DRM_IOCTL_MODE_OBJ_GETPROPERTIES  (0xB9, RW)

`#define DRM_IOCTL_MODE_OBJ_GETPROPERTIES DRM_IOWR(0xB9, struct drm_mode_obj_get_properties)`

Returns the (prop_id, value) pairs currently attached to any KMS
object (CRTC, connector, plane, etc.). This is the atomic-mode-setting
equivalent of `MODE_GETCONNECTOR`'s property array — it works on any
object type.

```c
struct drm_mode_obj_get_properties {
    __u64 props_ptr;            // __u32 array of property IDs
    __u64 prop_values_ptr;      // __u64 array of values
    __u32 count_props;          // in: capacity, out: actual
    __u32 obj_id;               // object ID (from GETRESOURCES/GETCONNECTOR/etc.)
    __u32 obj_type;             // DRM_MODE_OBJECT_*
};
```

Object type constants (`DRM_MODE_OBJECT_*`):
`CRTC=0xcccccccc`, `CONNECTOR=0xc0c0c0c0`, `ENCODER=0xe0e0e0e0`,
`MODE=0xdededede`, `PROPERTY=0xb0b0b0b0`, `FB=0xfbfbfbfb`,
`BLOB=0xbbbbbbbb`, `PLANE=0xeeeeeeee`, `COLOROP=0xfafafafa`,
`ANY=0` (wildcard).

#### DRM_IOCTL_MODE_ATOMIC  (0xBC, RW)

`#define DRM_IOCTL_MODE_ATOMIC DRM_IOWR(0xBC, struct drm_mode_atomic)`

The single entry point for atomic mode setting. Takes a list of
`(object, [(prop_id, value)])` tuples and either commits them or
validates them (TEST_ONLY). Requires `DRM_CLIENT_CAP_ATOMIC=1` to
have been set on the fd (via `DRM_IOCTL_SET_CLIENT_CAP`).

```c
struct drm_mode_atomic {
    __u32 flags;
    __u32 count_objs;
    __u64 objs_ptr;             // __u32 array of object IDs
    __u64 count_props_ptr;      // __u32 array: per-object prop count
    __u64 props_ptr;            // __u32 array: all prop IDs flattened
    __u64 prop_values_ptr;      // __u64 array: all values flattened
    __u64 reserved;             // must be 0
    __u64 user_data;            // echoed in vblank event
};
```

Flag bits (`DRM_MODE_ATOMIC_*`):
- `PAGE_FLIP_EVENT    = 1<<0` — emit `DRM_EVENT_FLIP_COMPLETE` on commit
- `PAGE_FLIP_ASYNC    = 1<<1` — non-blocking flip (may tear); requires `DRM_CAP_ATOMIC_ASYNC_PAGE_FLIP`
- `TEST_ONLY          = 1<<2` — validate only, do not commit
- `NONBLOCK           = 1<<3` — return `-EBUSY` instead of blocking if a previous flip is pending
- `ALLOW_MODESET      = 1<<4` — permit changes that require a full modeset (else rejected)

Usage: build parallel arrays. For each object, push its ID into
`objs_ptr`, its property count into `count_props_ptr`, and the
prop IDs and values into the flattened `props_ptr`/`prop_values_ptr`.
Then issue the ioctl. On success with `PAGE_FLIP_EVENT`, a
`drm_event_vblank` will be readable on the fd after the vblank.

Quirks:
- Test-only commits are atomic with respect to the current state but
  do not block; use them for capability probing.
- Any non-`ALLOW_MODESET` commit that would change `MODE_ID` on an
  active CRTC, change `CRTC_ID` on an enabled connector, or change
  the active topology returns `-EINVAL`.
- The fd must be DRM-Master (or have been granted master) to commit
  page-flips on a primary node; render nodes do not allow this.
- `user_data` is opaque to the kernel and echoed unchanged in the
  flip-complete event; use it to match commits to events.

### DRM client usage stats: `fdinfo`

In addition to sysfs, every DRM driver may export per-client stats
through `fops->show_fdinfo()`, readable via `cat /proc/<pid>/fdinfo/<fd>`
or `drm-stats` tooling. Format (per `Documentation/gpu/drm-usage-stats.rst`):

- One `key: value` pair per line.
- Standard keys prefixed `drm-`; driver-specific keys prefixed
  `<driver_name>-`.
- Mandatory: `drm-driver: <name>`.
- Optional identification: `drm-pdev: <aaaa:bb.cc.d>` (PCI slot),
  `drm-client-id: <uint>`, `drm-client-name: <str>`.
- Optional utilisation: `drm-engine-<name>: <uint> ns`,
  `drm-cycles-<name>: <uint>`, `drm-total-cycles-<name>: <uint>`,
  `drm-maxfreq-<name>: <uint> [Hz|MHz|KHz]`,
  `drm-engine-capacity-<name>: <uint>`.
- Optional memory: `drm-total-<region>:`, `drm-shared-<region>:`,
  `drm-resident-<region>:`, `drm-purgeable-<region>:`,
  `drm-active-<region>:`, all `<uint> [KiB|MiB]`. The region name
  `memory` is reserved for system memory; driver names like `vram`
  and `gtt` are common for dGPUs.

Engine time values may be non-monotonic but must catch up to the
previous larger value within a reasonable period; clients are
expected to clamp.

### AMDGPU (amdgpu driver) sysfs

AMDGPU exposes a rich set of power, thermal, and clock control files
under `/sys/class/drm/cardN/device/`. See
`Documentation/gpu/amdgpu/thermal.rst` and `driver-misc.rst`.

#### Power-state controls

| File                                   | Perms | Read                                           | Write                                          |
|----------------------------------------|-------|------------------------------------------------|------------------------------------------------|
| `power_dpm_state`                      | RW    | `performance` / `balanced` / `battery`         | same strings                                   |
| `power_dpm_force_performance_level`   | RW    | `auto`/`low`/`high`/`manual`/`profile_standard`/`profile_peak`/`profile_min_sclk`/`profile_min_mclk`/`profile_exit` | one of the above |
| `pp_power_profile_mode`                | RW    | numbered table: `0 BOOTUP_DEFAULT\n1 3D_FULL_SCREEN\n…\nN CUSTOM` | `<N>` to select, `N CUSTOM` to enable custom; with `manual` DPM level only |
| `pp_table`                             | RW    | binary PowerPlay table                         | binary blob (advanced; uses SMU firmware format) |
| `pp_od_clk_voltage`                    | RW    | `OD_SCLK:\n0: <MHz> <mV>\n1: …\nOD_MCLK:\n…\nOD_VDDC_CURVE:\n…\nOD_RANGE:\n…` | `s <idx> <MHz> <mV>` / `m <idx> <MHz> <mV>` / `vc <idx> <MHz> <mV>` / `c` commit; requires `manual` DPM level |
| `pp_dpm_sclk`                          | RW    | newline list `0: <MHz> *\n1: <MHz>` (asterisk = active) | `<N>` to pick a level |
| `pp_dpm_mclk`                          | RW    | same as sclk                                   | same                                           |
| `pp_dpm_socclk`                        | RW    | same                                           | same                                           |
| `pp_dpm_fclk`                          | RW    | same                                           | same                                           |
| `pp_dpm_dcefclk`                       | RW    | same                                           | same                                           |
| `pp_dpm_pcie`                          | RW    | `0: <GT/s> *` list                             | pick level                                     |
| `pm_policy`                            | RW    | numbered list of policies, `*` on active       | `<N>` to select                                |

DPM (Dynamic Power Management) state semantics:
- `power_dpm_state` is a **hint** for the workload profile; modern
  ASICs largely ignore it in favour of `pp_power_profile_mode`.
- `power_dpm_force_performance_level`:
  - `auto`            — fully automatic clock/power gating
  - `low`             — force lowest clocks
  - `high`            — force highest clocks
  - `manual`          — locks DPM, enables `pp_od_clk_voltage` and `pp_power_profile_mode` CUSTOM
  - `profile_*`       — fixed-point profiles, mutually exclusive with each other and with `manual`

Quirk: writing `pp_od_clk_voltage` while DPM level is not `manual`
returns `-EINVAL`. The `c` (commit) line is required for changes to
take effect; without it, edits live in a staging buffer that is
discarded on level change.

#### Utilisation and metrics

| File                  | Perms | Read                       | Units     |
|-----------------------|-------|----------------------------|-----------|
| `gpu_busy_percent`    | R     | `0`–`100`                  | percent   |
| `mem_busy_percent`    | R     | `0`–`100`                  | percent   |
| `gpu_metrics`         | R     | binary blob                | see `amdgpu_pm.c` |
| `pcie_bw`             | R     | `<aggressor> <victim>`     | MB/s × 100 (usec-windowed) |
| `smartshift_apu_power`| R     | `<uint>`                   | mW        |
| `smartshift_dgpu_power`| R    | `<uint>`                   | mW        |
| `smartshift_bias`     | RW    | `-100`–`100`               | percent bias toward APU/dGPU |
| `unique_id`           | R     | 16-hex-digit serial        | n/a       |
| `mem_info_vram_total` | R     | `<bytes>`                  | bytes     |
| `mem_info_vram_used`  | R     | `<bytes>`                  | bytes     |
| `mem_info_vis_vram_total`| R  | `<bytes>`                  | bytes     |
| `mem_info_vis_vram_used`| R   | `<bytes>`                  | bytes     |
| `mem_info_gtt_total`  | R     | `<bytes>`                  | bytes     |
| `mem_info_gtt_used`   | R     | `<bytes>`                  | bytes     |
| `pcie_replay_count`   | R     | `<uint>`                   | count     |

`gpu_busy_percent` and `mem_busy_percent` are the simplest utilisation
signals; they read the SMU's averaged busy counters. `gpu_metrics` is
a binary struct (`amdgpu_gpu_metrics` header + arrays) defined in
`include/uapi/drm/amdgpu_drm.h`; versioned by `format_revision` and
`content_revision` fields. SmartShift is present only on specific APU
+ dGPU combinations.

#### Fan control

| File                                | Perms | Read            | Write                   | Units/Range        |
|-------------------------------------|-------|-----------------|-------------------------|--------------------|
| `fan_curve`                         | RW    | multi-line `<temp°C> <PWM>` table | `0 <temp> <PWM>` lines, then `c` (commit) | °C, 0–255 PWM |
| `acoustic_limit_rpm_threshold`      | RW    | `<rpm>`         | `<rpm>`                 | RPM                |
| `acoustic_target_rpm_threshold`     | RW    | `<rpm>`         | `<rpm>`                 | RPM                |
| `fan_target_temperature`            | RW    | `<temp>`        | `<temp>`                | °C                 |
| `fan_minimum_pwm`                   | RW    | `<pwm>`         | `<pwm>`                 | 0–255              |
| `fan_zero_rpm_enable`               | RW    | `0`/`1`         | `0`/`1`                 | bool               |
| `fan_zero_rpm_stop_temperature`     | RW    | `<temp>`        | `<temp>`                | °C                 |

`fan_curve` requires `power_dpm_force_performance_level=manual`.
Each line is `0 <temp_in_C> <pwm_0_to_255>`; the leading `0` is a
flag (1 = disable this point). End with `c` on its own line to
commit, e.g. `echo -e "0 40 80\n0 60 150\n0 80 220\nc" > fan_curve`.

#### Product information (FRU EEPROM)

| File              | Perms | Read                          |
|-------------------|-------|-------------------------------|
| `product_name`    | R     | ASCII string                  |
| `product_number`  | R     | ASCII string                  |
| `serial_number`   | R     | ASCII string                  |
| `fru_id`          | R     | ASCII string                  |
| `manufacturer`    | R     | ASCII string                  |
| `board_info`      | R     | ASCII multiline, optional     |

Only present on cards with a FRU EEPROM (server / workstation
boards). `0400` permission (root-only) on most fields.

#### AMDGPU hwmon (under `device/hwmon/hwmonX/`)

AMDGPU registers an hwmon device whose `name` reads `amdgpu`. Files
actually populated vary by ASIC family; common ones:

| File            | Perms | Units              | Notes                                |
|-----------------|-------|--------------------|--------------------------------------|
| `name`          | R     | string `amdgpu`    |                                      |
| `temp1_input`   | R     | m°C                | Edge temperature                     |
| `temp1_crit`    | RW    | m°C                | Edge critical threshold              |
| `temp2_input`   | R     | m°C                | Junction (Hotspot) temperature       |
| `temp2_crit`    | RW    | m°C                | Junction critical threshold          |
| `temp3_input`   | R     | m°C                | Memory temperature                   |
| `temp3_crit`    | RW    | m°C                | Memory critical threshold            |
| `temp1_emergency`, `temp2_emergency`, `temp3_emergency` | R | m°C | Hardware shutdown thresholds |
| `pwm1`          | RW    | 0–255              | Manual fan PWM (when `pwm1_enable=1`)|
| `pwm1_enable`   | RW    | 0/1/2              | 0=off/full-speed, 1=manual, 2=auto   |
| `fan1_input`    | R     | RPM                | Current fan speed                    |
| `fan1_target`   | RW    | RPM                | Target RPM (auto mode)               |
| `fan1_max`      | R     | RPM                | Max PWM RPM                          |
| `fan1_min`      | R     | RPM                | Min PWM RPM                          |
| `power1_average`| R     | µW                 | Average GPU power                    |
| `power1_cap`    | RW    | µW                 | Power limit (TGP)                    |
| `power1_cap_max`| R     | µW                 | Maximum supported power limit        |
| `power1_cap_min`| R     | µW                 | Minimum supported power limit        |
| `in0_input`     | R     | mV                 | Northbridge voltage (SoC)            |
| `in1_input`     | R     | mV                 | PLX voltage (multi-GPU boards)       |
| `freq1_input`   | R     | kHz                | Current SCLK                         |
| `freq2_input`   | R     | kHz                | Current MCLK                         |
| `pwm1_mode`     | R     | 0/1                | 0=DC, 1=PWM (read-only on amdgpu)    |

#### AMDGPU debugfs (GFXOFF, not sysfs)

GFXOFF lets the RLC firmware power down the GFX engine when idle.
These are debugfs files under `/sys/kernel/debug/dri/N/`, **not**
regular sysfs:

| File                   | Perms | Read                                | Write          |
|------------------------|-------|-------------------------------------|----------------|
| `amdgpu_gfxoff`        | RW    | `1`=enabled / `0`=disabled          | `0` or `1`     |
| `amdgpu_gfxoff_status` | R     | `0`=in GFXOFF / `1`=leaving / `2`=out / `3`=entering | n/a |
| `amdgpu_gfxoff_count`  | R     | `uint64_t` entry count since boot   | n/a (vangogh)  |
| `amdgpu_gfxoff_residency` | RW | last-interval % × 100              | `1`=start log, `0`=stop (vangogh) |

Read these as raw binary (`xxd -l1 -p`) since debugfs uses `uint32_t`
binary format, not ASCII. Returns `-EINVAL` on unsupported ASICs.

### Intel i915 / xe sysfs

i915 (legacy driver, cards `<card0>`..`<cardN>`) and xe (new driver,
gradually replacing i915 on Intel dGPUs and recent integrated
graphics) expose both driver-specific sysfs under
`/sys/class/drm/cardN/device/` and an hwmon device under
`device/hwmon/hwmonM/`.

#### i915 RPS (Render Performance Scaling) files

The `gt` (graphics tile) directories hold the RPS files. For
multi-tile platforms each `gtN/` has its own copy:

```
/sys/class/drm/cardN/device/
├── gt/                       (i915 only)
│   ├── gt0/
│   │   ├── rps_act_freq_mhz     R   current effective SCLK, MHz
│   │   ├── rps_cur_freq_mhz     R   current requested SCLK, MHz
│   │   ├── rps_boost_freq_mhz   RW  boost frequency, MHz
│   │   ├── rps_max_freq_mhz     RW  max frequency, MHz
│   │   ├── rps_min_freq_mhz     RW  min frequency, MHz
│   │   ├── rps_rp_freq_mhz      R   "RP0/RP1/RPn" lines: max/non-turbo/min, MHz
│   │   ├── rps_pm_mask          RW  PM interrupt mask (debug)
│   │   └── throttle_reason_status  R  bitmask of frequency throttling reasons
│   └── gt1/  (multi-tile only)
└── …
```

RPS frequency constraints: writing `rps_min_freq_mhz` and
`rps_max_freq_mhz` clamps the driver's clock selector. Writes must
satisfy `min <= cur <= max`, `min >= RPn`, `max <= RP0`. Out-of-range
writes return `-EINVAL`.

The `throttle_reason_status` bitmask (i915):
- bit 0: `PL1` (sustained power limit)
- bit 1: `PL2` (burst power limit)
- bit 2: `PL4` (peak power limit)
- bit 3: `thermal`
- bit 4: `prochot`
- bit 5: `ratl` (reliability)
- bit 6: `vr_thermalert` (voltage regulator thermal alert)
- bit 7: `vr_tdc` (voltage regulator current limit)

#### i915 RC6 (Render Standby)

Older kernels exposed `rc6_enable` and `rc6_residency_ms` directly
under `device/`. On modern kernels these moved into the `gt/gtN/`
directory:

| File                       | Perms | Read             | Notes                                  |
|----------------------------|-------|------------------|----------------------------------------|
| `gt/gtN/rc6_enable`        | R     | `0`/`1`/mask     | Bitmask of enabled RC6 modes           |
| `gt/gtN/rc6_residency_ms`  | R     | `<ms>`           | Time spent in RC6 since boot           |

`rc6_enable` bitmask: bit 0 = RC6 (render standby), bit 1 = RC6p
(deep, removed on recent gens), bit 2 = RC6pp (deepest, removed on
recent gens). `rc6_residency_ms` resets on driver reload. The
percentage of time in RC6 over an interval is
`(rc6_after - rc6_before) / interval_ms × 100`. If the kernel was
booted with `i915.enable_rc6=0` or the platform does not support
RC6, `rc6_enable` reads `0` and `rc6_residency_ms` stays at 0.

#### i915 / xe hwmon (under `device/hwmon/hwmonM/`)

Both i915 (since v6.2) and xe (since v6.5) expose an hwmon device.
The i915 device `name` is `i915` (card-level) or `i915_gtN`
(per-tile); the xe device `name` is `xe` (card-level) or `xe_gtN`.

i915 hwmon files (per `Documentation/ABI/testing/sysfs-driver-intel-i915-hwmon`):

| File                  | Perms | Units | Notes                                            |
|-----------------------|-------|-------|--------------------------------------------------|
| `name`                | R     | str   | `i915` / `i915_gtN`                              |
| `in0_input`           | R     | mV    | Current voltage                                  |
| `power1_max`          | RW    | µW    | PL1 sustained power limit; `0` disables          |
| `power1_rated_max`    | R     | µW    | Default TDP                                      |
| `power1_max_interval` | RW    | ms    | PL1 averaging window (Tau)                       |
| `power1_crit`         | RW    | µW    | PL1 critical (I1) limit (client products)        |
| `curr1_crit`          | RW    | mA    | PL1 critical (I1) limit (server products)        |
| `energy1_input`       | R     | µJ    | Cumulative energy                                |
| `fan1_input`          | R     | RPM   | Fan speed (since v6.12, where present)           |
| `temp1_input`         | R     | m°C   | GPU package temperature (since v6.12)            |

xe hwmon files (per `Documentation/ABI/testing/sysfs-driver-intel-xe-hwmon`):

| File                     | Perms | Units | Notes                                          |
|--------------------------|-------|-------|------------------------------------------------|
| `name`                   | R     | str   | `xe` / `xe_gtN`                                |
| `in0_input`              | R     | mV    | Package voltage (since v6.8)                   |
| `power1_max`             | RW    | µW    | Card PL1 sustained; `0` disables               |
| `power1_cap`             | RW    | µW    | Card PL2 burst (since v6.15)                   |
| `power1_max_interval`    | RW    | ms    | Card PL1 Tau                                   |
| `power1_cap_interval`    | RW    | ms    | Card PL2 Tau (since v6.15)                     |
| `power1_rated_max`       | R     | µW    | Card default TDP                               |
| `power1_crit`            | RW    | µW    | Card PL1 critical (since v6.15)                |
| `power2_max`             | RW    | µW    | Package PL1 sustained (since v6.8)             |
| `power2_cap`             | RW    | µW    | Package PL2 burst (since v6.15)                |
| `power2_max_interval`    | RW    | ms    | Package PL1 Tau                                |
| `power2_cap_interval`    | RW    | ms    | Package PL2 Tau (since v6.15)                  |
| `power2_rated_max`       | R     | µW    | Package default TDP                            |
| `energy1_input`          | R     | µJ    | Card cumulative energy                         |
| `energy2_input`          | R     | µJ    | Package cumulative energy                      |
| `curr1_crit`             | RW    | mA    | Card I1 critical (since v6.15)                 |
| `temp1_input`            | R     | m°C   | (i915-only, not present on xe yet)             |
| `temp[2-5]_input`        | R     | m°C   | Package (2), VRAM (3), memory controller (4), GPU PCIe (5). `temp2_*` since v6.15; `temp3..5` since v7.0. |
| `temp[2-5]_max`          | R     | m°C   | Per-channel max limit (since v7.0)             |
| `temp[2-5]_crit`         | R     | m°C   | Per-channel critical (since v7.0)              |
| `temp[2-5]_emergency`    | R     | m°C   | Per-channel shutdown (since v7.0)              |
| `temp[6-21]_input`, `_crit`, `_emergency` | R | m°C | Per-VRAM-channel temperature + limits (since v7.0) |
| `fan1_input`             | R     | RPM   | Fan 1 (since v6.16)                            |
| `fan2_input`             | R     | RPM   | Fan 2 (since v6.16)                            |
| `fan3_input`             | R     | RPM   | Fan 3 (since v6.16)                            |

Quirk: PL1/PL2 power limits are reactive. The card runs at full
boost until the rolling average (over `*_interval` ms) crosses the
limit, then the power controller throttles. Reading `0` from
`power1_max` means "limit disabled"; writing `0` disables it.
Writes greater than TDP (`*_rated_max`) are rejected.

### NVIDIA

NVIDIA's open `nvidia-drm` kernel module is KMS-only — it does NOT
expose clock, power, or thermal control via sysfs. To read or
control these on the proprietary driver stack, use the **NVML**
userspace library (`libnvidia-ml.so`, package `nvidia-ml-dev` or
`cuda-toolkit`). NVML talks to the driver via an `ioctl` on
`/dev/nvidiactl` and `/dev/nvidiaN`; the protocol is closed but
the NVML API is documented and stable across releases.

Key NVML API functions (from `nvml.h`), grouped:

- **Lifecycle / enumeration**: `nvmlInit_v2()`, `nvmlShutdown()`,
  `nvmlDeviceGetCount_v2()`, `nvmlDeviceGetHandleByIndex_v2()`,
  `nvmlDeviceGetHandleByUUID()`,
  `nvmlDeviceGetHandleByPciBusId_v2()`, `nvmlDeviceGetIndex()`.
- **Identity**: `nvmlDeviceGetName()`, `nvmlDeviceGetUUID()`,
  `nvmlDeviceGetPciInfo_v3()`,
  `nvmlSystemGetDriverVersion()`,
  `nvmlSystemGetNVMLVersion()`,
  `nvmlSystemGetCudaDriverVersion_v2()`.
- **Temperature**: `nvmlDeviceGetTemperature()` (°C; sensor enum
  `NVML_TEMPERATURE_GPU=0`, `_MEM=1`),
  `nvmlDeviceGetTemperatureThreshold()` (enum: `SHUTDOWN`,
  `SLOWDOWN`, `GPU_MAX`, `ACCELERATION`, …),
  `nvmlDeviceGetThermalSettings()`.
- **Fan**: `nvmlDeviceGetNumFans()`,
  `nvmlDeviceGetFanSpeed_v2()` / `GetCurrentFanSpeed_v2()` (0–100
  percent, per index),
  `nvmlDeviceSetFanSpeed_v2()` (root),
  `nvmlDeviceGetFanControlPolicy_v2()` /
  `nvmlDeviceSetFanControlPolicy()` (`FAN_POLICY_TEMPERATURE_CLAMPING`,
  `_MANUAL`, `_DRIVEN_BY_EXTERNAL_SYSTEM`).
- **Power / energy**: `nvmlDeviceGetPowerUsage()` (mW),
  `nvmlDeviceGetPowerManagementMode()` (0/1),
  `nvmlDeviceGetPowerManagementLimit()` /
  `GetPowerManagementDefaultLimit()` /
  `GetPowerManagementLimitConstraints()` (mW),
  `nvmlDeviceSetPowerManagementLimit()` (root; mW),
  `nvmlDeviceGetTotalEnergyConsumption()` (mJ since driver load).
- **Clocks**: `nvmlDeviceGetClockInfo()`,
  `nvmlDeviceGetMaxClockInfo()`,
  `nvmlDeviceGetApplicationsClock()`,
  `nvmlDeviceSetApplicationsClocks()` /
  `ResetApplicationsClocks()`,
  `nvmlDeviceGetDefaultApplicationsClocks()`,
  `nvmlDeviceGetMaxCustomerBoostClock()`,
  `nvmlDeviceSetGpuLockedClocks()` /
  `ResetGpuLockedClocks()`,
  `nvmlDeviceSetMemoryLockedClocks()` /
  `ResetMemoryLockedClocks()`. All clocks in MHz. Clock-type enum:
  `GRAPHICS=0`, `SM=1`, `MEM=2`, `VIDEO=3`.
- **Performance state**: `nvmlDeviceGetPerformanceState()` (P0 =
  max, P15 = min), `nvmlDeviceGetPersistenceMode()` /
  `SetPersistenceMode()`, `nvmlDeviceGetComputeMode()` /
  `SetComputeMode()` (`DEFAULT`, `EXCLUSIVE_THREAD`,
  `EXCLUSIVE_PROCESS`, `PROHIBITED`), `nvmlDeviceGetMPSMode()` /
  `SetMPSMode()`.
- **Utilisation / memory**: `nvmlDeviceGetGpuUtilization()` (% SM,
  % memory), `nvmlDeviceGetMemoryUtilization()`,
  `nvmlDeviceGetMemoryInfo()` (`total`, `used`, `free`,
  `reserved` in bytes), `nvmlDeviceGetMemoryBusWidth()` (bits),
  `nvmlDeviceGetEncoderUtilization()` /
  `GetDecoderUtilization()` (% + sampling period µs),
  `nvmlDeviceGetProcessUtilization()` (per-PID),
  `nvmlDeviceGetPcieThroughput()` (KB/s; TX/RX),
  `nvmlDeviceGetPcieReplayCounter()`.
- **ECC / accounting**: `nvmlDeviceGetMemoryErrorCounter()`,
  `nvmlDeviceGetTotalEccErrors()`, `nvmlDeviceGetAccountingMode()`
  / `SetAccountingMode()`, `nvmlDeviceGetAccountingPids()`,
  `nvmlDeviceGetAccountingStats()`.
- **Generic query**: `nvmlDeviceGetFieldValues_v2()` (single call
  for many metrics; preferred for telemetry).
- **Events**: `nvmlEventSetCreate()`,
  `nvmlDeviceRegisterEvents()` (bitmask:
  `CLOCK_CHANGE`, `PSTATE`, `CRITICAL_XID`,
  `POWER_SOURCE_CHANGE`, `SINGLE_BIT_ECC_ERROR`,
  `DOUBLE_BIT_ECC_ERROR`, `NVLINK_REPLAY_ERROR`),
  `nvmlEventSetWait()`.
- **Topology**: `nvmlSystemGetTopologyGpuSet()` (NUMA-affinity GPU
  list).

NVML unit conventions:
- Power: milliwatts (mW), not microwatts. Multiply by 1000 when
  comparing to hwmon `power1_*` which uses µW.
- Energy: millijoules (mJ).
- Temperatures: degrees Celsius (not millidegrees).
- Clocks: megahertz (MHz), not kilohertz.
- Throughput: kilobytes/sec (KB/s), not bytes/sec.

Quirks:
- NVML requires root or `CAP_SYS_ADMIN` for most write operations;
  reads work unprivileged.
- Persistence mode must be enabled (via `nvidia-persistenced` or
  `nvmlDeviceSetPersistenceMode`) to keep driver state loaded when
  no clients are open; otherwise cold-start latency per process.
- The open `nvidia-drm` module exposes sysfs but only the DRM
  attributes (modesetting, prime). For thermal/power, NVML is the
  only stable interface.
- `nvidia-smi` is the bundled CLI; it is a thin wrapper around
  NVML and is useful for command-line validation but not for
  programmatic use in Zenctl (use NVML directly).
- Some NVML functions are stubbed on consumer (GeForce) cards —
  e.g. `nvmlDeviceGetPowerManagementLimitConstraints` may return
  `NVML_ERROR_NOT_SUPPORTED`. Always check return codes.

### Other DRM drivers (briefly)

- **nouveau** (open NVIDIA): hwmon at
  `/sys/class/drm/cardN/device/hwmon/hwmonM/` with `temp1_input`,
  `pwm1`, `pwm1_enable`, `fan1_input`. Power-state control via
  `/sys/kernel/debug/dri/N/pstate` (debugfs, not sysfs).
- **radeon** (legacy AMD): hwmon present on most ASICs; same files
  as nouveau. Power state via `power_dpm_state` (similar to amdgpu).
- **panfrost / panthor / msm / vc4 / v3d**: clock control via
  devfreq (`/sys/class/devfreq/<dev>/`); no driver-specific thermal
  sysfs.
- **virtio_gpu, vmwgfx, qxl**: no thermal/power interface.

For any GPU not in the explicit list above, the only guaranteed
interfaces are: DRM ioctls, the `gpu_busy_percent`-style files (if
the driver provides them), and the standard PCI `device/` attributes
(`vendor`, `device`, `class`, `revision`, `irq`, `resource`, `power/`).

---

## Thermal

The thermal subsystem (`Documentation/driver-api/thermal/sysfs-api.rst`)
manages thermal zones (sensors + trip points + cooling devices). All
files live under `/sys/class/thermal/`. A thermal zone can be backed
by ACPI, DT, or a platform driver.

### Thermal zones: `/sys/class/thermal/thermal_zoneX/`

Each thermal zone has these attributes (per
`Documentation/ABI/testing/sysfs-class-thermal`):

| File                          | Perms | Read                                          | Write                          | Units / Notes                          |
|-------------------------------|-------|-----------------------------------------------|--------------------------------|----------------------------------------|
| `type`                        | R     | short lowercase string, e.g. `acpitz`, `x86_pkg_temp`, `iwlwifi_1`, `soc_dts0` | n/a | Required. |
| `temp`                        | R     | `<int>`                                       | n/a                            | m°C. Required.                         |
| `mode`                        | RW    | `enabled` / `disabled`                        | `enabled` / `disabled`         | Optional. `disabled` lets userspace take over. |
| `policy`                      | RW    | active governor name                          | governor name (must be in `available_policies`) | Required. |
| `available_policies`          | R     | space-separated list                          | n/a                            | Required.                              |
| `trip_point_Y_temp`           | R     | `<int>`                                       | n/a (some drivers allow set via `set_trip_temp` op) | m°C. Optional. |
| `trip_point_Y_type`           | R     | `critical` / `hot` / `passive` / `active[0-*]` | n/a                          | Optional.                              |
| `trip_point_Y_hyst`           | RW    | `<int>`                                       | `<int>`                        | **°C** (NOT m°C — historical quirk). Optional. |
| `cdevY`                       | R     | symlink to `/sys/class/thermal/cooling_deviceZ` | n/a                          | Optional, per binding.                 |
| `cdevY_trip_point`            | R     | `<int>` (index into trip_point_*) or `-1`     | n/a                            | Optional.                              |
| `cdevY_weight`                | RW    | `<int>`                                       | `<int>`                        | Relative influence in fair_share / power_allocator. Optional. |
| `emul_temp`                   | W     | n/a                                           | `<int>` (m°C); `0` disables    | Optional. Debug only — see warning.    |
| `sustainable_power`           | RW    | `<int>`                                       | `<int>`                        | mW. Power_allocator governor input.    |
| `k_po`, `k_pu`, `k_i`, `k_d`  | RW    | `<int>`                                       | `<int>`                        | Power_allocator PID gains.             |
| `integral_cutoff`             | RW    | `<int>`                                       | `<int>`                        | m°C. Optional.                         |
| `slope`, `offset`             | RW    | `<int>`                                       | `<int>`                        | Linear-extrapolation constants. Optional. |

Trip-point types (per `Documentation/driver-api/thermal/sysfs-api.rst`):

- `critical` — kernel triggers orderly shutdown / emergency restart
  on crossing. Cannot be disabled.
- `hot` — driver's `.hot` callback fires; usually triggers graceful
  userspace shutdown but does not auto-power-off.
- `passive` — below `active`; uses processor/cooling throttling
  rather than a fan. The kernel uses `passive_delay` (set at zone
  registration) as the polling interval for passive trips.
- `active[0-*]` — typically mapped to fan cooling; index is the fan
  stage.

`trip_point_Y_hyst` quirk: unlike `temp` and `*_temp` fields which
are m°C, hysteresis is reported in **whole degrees Celsius** (°C),
matching the original ACPI `_PSV` semantics. Implementation must
divide by 1000 when comparing to `temp`.

`emul_temp` WARNING (per ABI doc): enabling this on production
systems is dangerous — userspace can disable thermal policy by
flooding the node with low values. Treat as root-only debug.

`mode` semantics:
- `enabled` (default) — kernel governor drives cooling devices.
- `disabled` — kernel will not act on trip crossings; userspace
  is responsible for monitoring `temp` and operating cooling devices
  via `cooling_device*/cur_state`. Sensor reads still work.

### Cooling devices: `/sys/class/thermal/cooling_deviceX/`

A cooling device is anything the kernel can throttle: a CPU P-state
cluster, a fan, a GPU clock domain, an LCD backlight, etc.

| File                              | Perms | Read                                          | Write                | Units / Notes                                |
|-----------------------------------|-------|-----------------------------------------------|----------------------|----------------------------------------------|
| `type`                            | R     | `Processor`, `Fan`, `LCD`, `intel_powerclamp`, `Processor`, `thermal-cpufreq-0`, etc. | n/a | Required. |
| `max_state`                       | R     | `<int>`                                       | n/a                  | Required.                                    |
| `cur_state`                       | RW    | `<int>` in `[0, max_state]`                   | `<int>`              | `0` = no cooling, `max_state` = max cooling. Required. |
| `stats/reset`                     | W     | n/a                                           | any value            | Required (if stats dir exists).              |
| `stats/time_in_state_ms`          | R     | `<state> <ms>` per line, one per state        | n/a                  | Required.                                    |
| `stats/total_trans`               | R     | `<int>`                                       | n/a                  | Total state-change count.                    |
| `stats/trans_table`               | R     | 2D matrix, rows = from-state, cols = to-state | n/a                  | Returns `-EFBIG` if bigger than PAGE_SIZE.   |

The `cur_state` write is the primary control point. For CPU
cooling devices (`thermal-cpufreq-*`), `cur_state` maps to a
cpufreq P-state; for fan cooling devices, it maps to a PWM level
(generally linear). Negative or out-of-range writes return
`-EINVAL`.

For per-CPU thermal cooling, the `type` is typically
`thermal-cpufreq-<policy>` and `max_state` equals the number of
P-states minus one.

### Thermal governors

Selectable via `policy`. Available governors (the exact set depends
on `CONFIG_THERMAL_GOV_*`):

| Governor        | Behaviour                                                                                              | Required trip points                              |
|-----------------|--------------------------------------------------------------------------------------------------------|---------------------------------------------------|
| `step_wise`     | Default. Increments/decrements cooling by one step per polling interval when trip is crossed/clear.    | Any                                               |
| `fair_share`    | Distributes cooling across all bound cdevs weighted by `cdevY_weight`.                                 | Any                                               |
| `bang_bang`     | Two-state (full-on / full-off) for fan control. Required for `active` trips where hysteresis matters.  | `active`                                          |
| `user_space`    | Does nothing in kernel; emits `THERMAL_TRIP` uevents so userspace decides.                              | Any                                               |
| `power_allocator` | PID controller using `sustainable_power` and `k_p/k_i/k_d`. Best for zones with power-aware cdevs.   | Two passive trips: switch_on + desired temperature |

Writing the policy name updates `policy` immediately. If the new
governor refuses the zone (e.g. `power_allocator` requires two
passive trips), the write returns `-EINVAL` and the previous
governor stays.

### Thermal uevents

When a trip is crossed, the thermal core emits a kobject uevent of
form:

```
THERMAL_TRIP=thermal_zoneX trip_point_Y_type
```

Plus `NAME=<type>` for the zone. Userspace listens via libudev (or
`udevadm monitor --kernel`). The `user_space` governor emits these
without acting; other governors also emit them as side effects of
their actions.

Critical-trip events are NOT emitted as uevents — the kernel goes
straight to power-off / emergency restart (per section 5 of
`sysfs-api.rst`).

### hwmon (`/sys/class/hwmon/hwmonN/`)

Hwmon is the generic hardware-monitoring class. Many thermal
sensors, fan controllers, voltage monitors, and GPUs expose
themselves as hwmon devices. The full ABI is in
`Documentation/ABI/testing/sysfs-class-hwmon` and
`Documentation/hwmon/sysfs-interface.rst`. Mandatory file: `name` —
short lowercase string identifying the chip. Per-channel files use
`tempN_*`, `fanN_*`, `pwmN_*`, `inN_*`, `currN_*`, `powerN_*`,
`energyN_*`, `humidityN_*`, `freqN_*`, `intrusionN_*`. Channel
index starts at 1.

#### Temperature

| File             | Perms | Units | Notes                                          |
|------------------|-------|-------|------------------------------------------------|
| `tempN_input`    | R     | m°C   | Current reading                                |
| `tempN_max`      | RW    | m°C   | High limit                                     |
| `tempN_max_hyst` | RW    | m°C   | Absolute value, not delta (per ABI)            |
| `tempN_max_alarm`| R     | 0/1   | High-limit alarm                               |
| `tempN_crit`     | RW    | m°C   | Critical limit                                 |
| `tempN_crit_hyst`| RW    | m°C   | Absolute value                                 |
| `tempN_crit_alarm`| R    | 0/1   | Critical-limit alarm                           |
| `tempN_lcrit`    | RW    | m°C   | Low critical limit                             |
| `tempN_min`      | RW    | m°C   | Low limit                                      |
| `tempN_emergency`| RW    | m°C   | Emergency max (above `crit`)                   |
| `tempN_label`    | R     | str   | Sensor label, e.g. "CPU", "DIMM A1"            |
| `tempN_lowest`, `tempN_highest` | R | m°C | Historical min/max since reset             |
| `tempN_type`     | RW    | 1–6   | Sensor type (1=CPU diode, 2=transistor, 3=thermal diode, 4=thermistor, 5=AMD AMDSI, 6=Intel PECI) |
| `tempN_offset`   | RW    | m°C   | Per-sensor offset                             |
| `tempN_beep`     | RW    | 0/1   | Beep on alarm                                 |

#### Fan

| File           | Perms | Units | Notes                                  |
|----------------|-------|-------|----------------------------------------|
| `fanN_input`   | R     | RPM   | Current fan speed                      |
| `fanN_min`     | RW    | RPM   | Min                                    |
| `fanN_max`     | RW    | RPM   | Max                                    |
| `fanN_div`     | RW    | 1–128 | Power-of-two divisor                   |
| `fanN_pulses`  | RW    | 1–4   | Tach pulses per revolution             |
| `fanN_target`  | RW    | RPM   | Target speed                           |
| `fanN_label`   | R     | str   | Channel label                          |
| `fanN_fault`   | R     | 0/1   | Fan failure                            |
| `fanN_alarm`   | R     | 0/1   | Alarm                                  |
| `fanN_beep`    | RW    | 0/1   | Beep on alarm                          |

#### PWM (fan control output)

| File                        | Perms | Format                                           | Notes                                       |
|-----------------------------|-------|--------------------------------------------------|---------------------------------------------|
| `pwmN`                      | RW    | `0`–`255` integer                                | 255 = 100%.                                 |
| `pwmN_enable`               | RW    | `0` = no control (full speed) / `1` = manual (use `pwmN`) / `2+` = automatic (chip-dependent) | The task spec's "0=disabled, 1=manual, 2=auto" matches; some chips use 3, 4 for distinct auto modes. |
| `pwmN_mode`                 | RW    | `0` = DC mode / `1` = PWM mode                   | Direct current vs pulse-width output.       |
| `pwmN_freq`                 | RW    | `<int>` Hz                                       | Only when `pwmN_mode=1`.                    |
| `pwmN_auto_channels_temp`   | RW    | bitmask: bit 0 = temp1, bit 1 = temp2, etc.      | Which temp channels drive this PWM in auto. |
| `pwmN_auto_pointZ_pwm`      | RW    | `0`–`255`                                        | PWM at auto trip Z.                         |
| `pwmN_auto_pointZ_temp`     | RW    | m°C                                              | Temp at auto trip Z.                        |
| `pwmN_auto_pointZ_temp_hyst`| RW    | m°C                                              | Hysteresis for trip Z.                      |
| `tempN_auto_pointZ_pwm`     | RW    | `0`–`255`                                        | Alt. binding when trips belong to a temp channel rather than a PWM. |

Quirk: `pwmN_enable` semantics vary by chip. The kernel ABI defines
0/1/2 as the minimum, but drivers (e.g. `it87`, `nct6775`,
`amdgpu`) may use 3, 4, 5 for distinct automatic control modes
(three-point curve, smart fan, etc.). Always read the per-driver
doc.

#### Voltage / current / power / energy / frequency

| File             | Perms | Units | Notes                          |
|------------------|-------|-------|--------------------------------|
| `inN_input`      | R     | mV    | Voltage                        |
| `inN_min`, `inN_max` | RW | mV    | Limits                         |
| `inN_alarm`      | R     | 0/1   | Alarm                          |
| `inN_label`      | R     | str   | Channel label                  |
| `currN_input`    | R     | mA    | Current                        |
| `currN_max`, `currN_crit` | RW | mA | Limits                |
| `powerN_input`   | R     | µW    | Instantaneous power            |
| `powerN_average` | R     | µW    | Averaged power                 |
| `powerN_average_interval` | RW | ms | Averaging window      |
| `powerN_average_highest`, `_lowest`, `_max` | R | µW | Stats |
| `powerN_acc`, `powerN_cap`, `powerN_cap_max`, `powerN_cap_min` | RW | µW | Caps |
| `powerN_alarm`   | R     | 0/1   |                                |
| `energyN_input`  | R     | µJ    | Cumulative energy              |
| `freqN_input`    | R     | Hz    | Frequency                      |
| `freqN_label`    | R     | str   | Channel label                  |
| `humidityN_input`| R     | ‰ (milli-percent) | Relative humidity   |

#### Other hwmon files

| File                | Perms | Notes                                          |
|---------------------|-------|------------------------------------------------|
| `name`              | R     | Mandatory chip name.                           |
| `label`             | R     | Optional device-level label.                   |
| `update_interval`   | RW    | ms. Some chips only.                           |
| `update_interval_us`| RW    | µs. Sub-millisecond control. v6.x+             |
| `intrusion0_alarm`  | RW    | Chassis intrusion; write `0` to clear.         |
| `intrusion0_beep`   | RW    | Beep on intrusion.                             |

### libsensors interface

`libsensors` (`libsensors5`, `sensors.h`) is the userspace library
that wraps hwmon discovery. Key API:

```c
int sensors_init(FILE *input);            // NULL = use /etc/sensors3.conf
void sensors_cleanup(void);
int sensors_parse_chip_name(const char *name, sensors_chip_name *res);
void sensors_free_chip_name(sensors_chip_name *name);
const sensors_chip_name *sensors_get_detected_chips(int *nr);
const sensors_feature *sensors_get_features(const sensors_chip_name *name, int *nr);
const sensors_subfeature *sensors_get_all_subfeatures(const sensors_chip_name *name, int featurenr, int *nr);
int sensors_get_value(const sensors_chip_name *name, int subfeat_nr, double *value);
int sensors_set_value(const sensors_chip_name *name, int subfeat_nr, double value);
int sensors_do_chip_sets(const sensors_chip_name *name);   // commit writes
const char *sensors_get_label(const sensors_chip_name *name, int featurenr);
int sensors_get_adapter_name(int bus_nr, const char **name);
```

`libsensors` resolves chip-name → sysfs path mapping
(`/sys/class/hwmon/hwmonN/...`), applies the linear transforms
defined in `/etc/sensors3.conf`, and returns SI values (°C, V, A, W,
RPM, Hz). Zenctl can either use `libsensors` for high-level access
or read `/sys/class/hwmon/` directly for raw values — the latter is
preferred when fine-grained control over which hwmon device matters.
The `sensors` CLI is the bundled command-line tool; `sensors -u`
dumps every subfeature value (useful for debugging).

---

## Power

### System sleep states

`/sys/power/state` and `/sys/power/mem_sleep` together expose the
sleep-state matrix. Per `Documentation/admin-guide/pm/sleep-states.rst`
and `Documentation/ABI/testing/sysfs-power`:

#### `/sys/power/state`

- **Perms**: R (read lists available states); W (write triggers).
- **Read**: space-separated list of supported state strings. Typical:
  `freeze mem` (most laptops), `freeze standby mem disk` (full set).
  Selected variant in `[brackets]`.
- **Write**: one of the supported state strings.
- **States**:
  - `freeze` — suspend-to-idle (s2idle). Pure software, always
    available if `CONFIG_SUSPEND=y`.
  - `standby` — power-on suspend (ACPI S1). Available only if
    platform registers it.
  - `mem` — interpreted per `mem_sleep` (see below). On most modern
    ACPI laptops this resolves to `deep` (S3) or `s2idle` (S0ix).
  - `disk` — hibernation. Writes a snapshot to swap, then either
    powers off, reboots, or enters S4 per `/sys/power/disk`.
- **Quirk**: writing to `/sys/power/state` blocks until resume. The
  fd must be opened O_RDWR; only one writer at a time. The kernel
  will reject writes from a context that cannot sleep (e.g. an
  ioctl in atomic context).

#### `/sys/power/mem_sleep`

- **Perms**: RW.
- **Read**: space-separated list of variants. Always includes
  `s2idle`; `shallow` (ACPI S1) and `deep` (S3) appear when
  supported. Active variant in `[brackets]`, e.g.
  `s2idle [deep]`.
- **Write**: one of `s2idle`, `shallow`, `deep`.
- **Quirk**: the default is platform-dependent. On modern Intel
  laptops the default is often `s2idle` (S0ix / Modern Standby)
  even when `deep` (S3) is supported; this is controlled by ACPI
  tables and the kernel cmdline `mem_sleep_default=`. To force S3:
  `echo deep > /sys/power/mem_sleep` then `echo mem >
  /sys/power/state`.

#### `/sys/power/disk` (hibernation mode)

- **Perms**: RW.
- **Read**: space-separated list of options. Active in `[brackets]`.
  Options: `platform`, `shutdown`, `reboot`, `suspend`,
  `test_resume` (where supported). Also legacy `firmware`,
  `testproc`, `test` (older kernels).
- **Write**: one of the option strings.
- **Semantics** (what to do after writing the snapshot to swap):
  - `platform` — ACPI S4 (firmware-managed low-power state).
  - `shutdown` — power off (default; works on all systems).
  - `reboot` — reboot the system (diagnostic).
  - `suspend` — hybrid suspend: enter `mem_sleep` state; on wakeup,
    discard the image; if wakeup fails, restore from image.
  - `test_resume` — diagnostic: simulate resume without power-off.

#### `/sys/power/image_size`

- **Perms**: RW.
- **Read**: current image-size limit in bytes. Default ~2/5 of RAM.
- **Write**: non-negative integer in bytes. `0` means "smallest
  possible".
- **Quirk**: best-effort. The kernel may exceed the limit if it
  cannot free enough memory.

#### `/sys/power/reserved_size`, `/sys/power/resume_offset`, `/sys/power/resume`

- `reserved_size` (RW): bytes reserved for driver `->freeze`
  allocations during hibernation. Default 1 MB. Write non-negative
  integer bytes.
- `resume_offset` (RW): byte offset into the resume device (for
  swap files). Default `0`.
- `resume` (RW): write `major:minor` of the swap device to trigger
  a late-resume attempt; read returns the configured resume device.
  Mostly used after a manual hibernate to read the image back.

#### `/sys/power/wakeup_count`

- **Perms**: RW.
- **Read**: current count of registered wakeup events. Blocks if
  events are in flight.
- **Write**: only succeeds if the written value equals the current
  count. On success, a subsequent transition to a sleep state is
  aborted if a wakeup event arrives between the write and the
  actual sleep.
- **Use**: race-free suspend. Pattern:
  1. Read `wakeup_count` → `N`.
  2. Write `N` back to `wakeup_count`.
  3. If write succeeds, write `mem` to `/sys/power/state` immediately.
  4. If a wakeup happens between (2) and (3), the suspend aborts
     cleanly without losing the event.

#### `/sys/power/autosleep`

- **Perms**: RW.
- **Read**: last successfully written state string, or `off`.
- **Write**: any state from `/sys/power/state`, or `off` to disable.
  When set, the kernel auto-enters that state whenever no wakeup
  sources are active. Mostly used on Android-style systems.

#### `/sys/power/wake_lock`, `/sys/power/wake_unlock`

- **Perms**: RW.
- **Write**: `<name>` to take a wakeup-source lock; `<name> <ns>`
  to take a timeout-based lock. Wakeup sources block autosleep and
  cause `wakeup_count` reads to return nonzero.
- **Read**: space-separated list of active (wake_lock) or inactive
  (wake_unlock) sources created via this interface.

#### `/sys/power/sync_on_suspend`, `/sys/power/pm_async`, `/sys/power/pm_print_times`, `/sys/power/pm_debug_messages`

All RW. Defaults: `sync_on_suspend=1`, `pm_async=1`, `pm_print_times=0`,
`pm_debug_messages=0`. Write `0`/`1` to toggle. `sync_on_suspend=0`
skips the `sync()` call during suspend (improves latency, risks
filesystem corruption if a crash occurs during sleep). `pm_async=0`
forces synchronous suspend/resume. `pm_print_times=1` prints
per-device suspend/resume timing to dmesg. `pm_debug_messages=1`
enables PM-core debug messages.

#### `/sys/power/pm_trace`, `/sys/power/pm_trace_dev_match`

- **Perms**: RW / R respectively.
- **Write**: `1` to start saving PM event hashes into the RTC.
- **Quirk**: enabling this **destroys the RTC time** on the next
  resume. Use only for debugging suspend hangs. `pm_trace_dev_match`
  reads return the device name matching the hash in the RTC.

#### `/sys/power/pm_wakeup_irq`

- **Perms**: R. Read: IRQ number of the most recent wakeup interrupt.

#### `/sys/power/suspend_stats/`

Directory of suspend statistics (since v5.3). All files are R.

- `success`, `fail` — counts of suspend cycles by outcome.
- `failed_freeze`, `failed_prepare`, `failed_suspend`,
  `failed_suspend_late`, `failed_suspend_noirq`,
  `failed_resume`, `failed_resume_early`, `failed_resume_noirq`
  — per-stage failure counts.
- `last_failed_dev`, `last_failed_errno`, `last_failed_step` —
  most recent failure's device path, errno string, and stage name.
- `last_hw_sleep`, `total_hw_sleep`, `max_hw_sleep` — µs in
  hardware sleep state (last cycle / since boot / max the hardware
  can report). `last_hw_sleep` is the authoritative "how long was
  the hardware actually asleep" counter — useful for verifying
  that an s2idle attempt actually entered S0ix rather than just
  freezing user space.

#### `/sys/power/hibernate_compression_threads`

- **Perms**: RW. Default 3. Min 1 (since v6.x).
- **Write**: number of threads for hibernation-image compression
  and decompression. Takes effect on next hibernation.

### Runtime PM: `/sys/devices/.../power/`

Every device in `/sys/devices/` has a `power/` subdirectory with
runtime-PM controls (per
`Documentation/ABI/testing/sysfs-devices-power`).

| File                                   | Perms | Read                                          | Write                          | Notes                                                |
|----------------------------------------|-------|-----------------------------------------------|--------------------------------|------------------------------------------------------|
| `control`                              | RW    | `auto` / `on`                                 | `auto` / `on`                  | `on` disables runtime PM for this device; `auto` allows it. Default `auto`. |
| `runtime_status`                       | R     | `suspended` / `suspending` / `resuming` / `active` / `error` / `unsupported` | n/a | Reflects current runtime PM state. `unsupported` = runtime PM disabled. |
| `runtime_usage`                        | R     | `<int>`                                       | n/a                            | PM-usage count. `0` = idle.                          |
| `runtime_enabled`                      | R     | `enabled` / `disabled` / `forbidden` / combination | n/a                      | Whether runtime PM is enabled.                       |
| `runtime_active_kids`                  | R     | `<int>`                                       | n/a                            | Number of active children.                           |
| `runtime_active_time`                  | R     | `<int>` ms                                    | n/a                            | Cumulative time in active state.                     |
| `runtime_suspended_time`               | R     | `<int>` ms                                    | n/a                            | Cumulative time suspended.                           |
| `autosuspend_delay_ms`                 | RW    | `<int>` ms (or absent)                        | `<int>` ms (or negative)       | Delay before autosuspending an idle device. Negative disables autosuspend. Values >= 1000 round up to nearest second. Returns `-EIO` if unsupported. |
| `async`                                | RW    | `enabled` / `disabled`                        | `enabled` / `disabled`         | Allow async suspend/resume.                          |
| `wakeup`                               | RW    | `enabled` / `disabled`                        | `enabled` / `disabled`         | Present only if device can wake the system.          |
| `wakeup_count`                         | R     | `<int>`                                       | n/a                            | Signaled wakeup events.                                |
| `wakeup_active_count`, `wakeup_abort_count`, `wakeup_expire_count`, `wakeup_active` | R | `<int>` / `0/1` | n/a | Processed / aborted / expired / in-progress wakeups. |
| `wakeup_total_time_ms`, `wakeup_max_time_ms`, `wakeup_last_time_ms`, `wakeup_prevent_sleep_time_ms` | R | `<int>` ms | n/a | Wakeup timing stats. All present only if wakeup-capable. |
| `pm_qos_resume_latency_us`             | RW    | `<int>` µs or `n/a`                           | `<int>` µs or `n/a`            | Max resume latency. `0` = any. `n/a` = userspace forbids any latency. |
| `pm_qos_latency_tolerance_us`          | RW    | `<int>` µs / `any` / `auto`                   | same                           | Active-state latency tolerance.                      |
| `pm_qos_no_power_off`                  | RW    | `0` / `1`                                     | `0` / `1`                      | Forbid powering off the device entirely.             |
| `pm_qos_flags`                         | R     | bitmask string                                | n/a                            | PM QoS flags (e.g. `no_power_off`).                  |

#### Runtime-PM write semantics

- `echo on > control` — increment the device's usage count and
  resume it. Prevents autosuspend. Equivalent to a driver-level
  `pm_runtime_get_sync()`.
- `echo auto > control` — decrement the usage count and allow
  autosuspend. If the count reaches zero and the autosuspend delay
  elapses, the device suspends.
- Writing `on` while the device is already suspended causes it to
  be resumed immediately. Writing `on` while the device is active
  increments the count and prevents further autosuspend.

#### Wakeup-source semantics

- `power/wakeup` is only present if the device's bus or driver has
  registered a wakeup capability (`device_set_wakeup_capable(dev,
  true)`).
- Writing `enabled` arms the device as a wakeup source; `disabled`
  disarms. The actual arming happens at the next suspend cycle.
- All `wakeup_*` stats files are present only when the device is
  wakeup-capable. They are empty if wakeup is disabled.

### ACPI device power state

For ACPI device nodes (under `/sys/devices/LNXSYSTM*` and
`/sys/bus/acpi/devices/`), two extra files appear:

| File                 | Perms | Read                                                    | Notes                                  |
|----------------------|-------|---------------------------------------------------------|----------------------------------------|
| `power_state`        | R     | `D0` / `D1` / `D2` / `D3hot` / `D3cold`                 | Logical power state per ACPI spec.     |
| `real_power_state`   | R     | same                                                    | Actual `_PSC` value (may differ if shared power resources are ON for other devices). |

Only present for ACPI devices that expose power-management methods.

### ACPI fan performance states

For ACPI fan devices (`PNP0C0B`, `INT3404`), `/sys/bus/acpi/devices/<ACPIID>:NN/`
exposes:

| File                    | Perms | Read                                                | Notes                                  |
|-------------------------|-------|-----------------------------------------------------|----------------------------------------|
| `state0` … `stateN`     | R     | `control_percent:trip_point_index:speed_rpm:noise_level_mdb:power_mw` | One per _FPS state. |
| `status`                | R     | current state                                       |                                        |
| `fine_grain_control`    | R     | `0` / `1`                                           | Whether _FIF advertises fine-grain control. |
| `fan_speed_rpm`         | R     | `<int>` RPM                                         | From _FST; current fan speed.          |

`control_percent` is the value to write to the corresponding
`cooling_device*/cur_state` to select that performance state. When
`fine_grain_control=1`, intermediate values are also accepted.

### Power supply class: `/sys/class/power_supply/<NAME>/`

Per `Documentation/ABI/testing/sysfs-class-power`. `<NAME>` is
typically `BAT0`, `BAT1`, `BATT`, `macsmc-battery` for batteries
and `AC`, `ACAD`, `ADP1` for mains adapters. USB chargers appear
with type `USB` and may be named e.g. `ucs1002-source-psy`.

Common files for all supply types:

| File                | Perms | Read                                      | Notes                          |
|---------------------|-------|-------------------------------------------|--------------------------------|
| `type`              | R     | `Battery` / `UPS` / `Mains` / `USB` / `Wireless` | Required.                |
| `name`              | R     | supply name string                        |                                |
| `manufacturer`      | R     | string                                    | Optional.                      |
| `model_name`        | R     | string                                    | Optional.                      |
| `serial_number`     | R     | string                                    | Optional.                      |
| `technology`        | R     | `Unknown` / `NiMH` / `Li-ion` / `Li-poly` / `LiFe` / `NiCd` / `LiMn` | Battery only. |
| `online`            | R / RW | `0` = Offline / `1` = Online Fixed / `2` = Online Programmable | USB supplies may be RW (voltage/current control). |
| `present`           | R     | `0` / `1`                                 | Battery only.                  |
| `health`            | R     | `Unknown` / `Good` / `Overheat` / `Dead` / `Over voltage` / `Unspecified failure` / `Cold` / `Watchdog timer expire` / `Safety timer expire` | Battery. |
| `scope`             | R     | `System` / `Device`                       |                                |
| `uevent`            | R     | `POWER_SUPPLY_NAME=…\nPOWER_SUPPLY_TYPE=…` etc. |                            |

#### Battery files

| File                          | Perms | Units / Format                              | Notes                                  |
|-------------------------------|-------|---------------------------------------------|----------------------------------------|
| `status`                      | R/RW  | `Unknown` / `Charging` / `Discharging` / `Not charging` / `Full` | Some supplies allow write to enable/disable charging. |
| `capacity`                    | R     | `0`–`100` percent                           | Fine-grain state of charge.            |
| `capacity_level`              | R     | `Unknown` / `Critical` / `Low` / `Normal` / `High` / `Full` | Coarse.                  |
| `capacity_alert_min`          | RW    | `0`–`100` percent                           | Kernel emits uevent below this.        |
| `capacity_alert_max`          | RW    | `0`–`100` percent                           | Kernel emits uevent above this.        |
| `capacity_error_margin`       | R     | percent                                     | Fuel-gauge uncertainty.                |
| `charge_now`                  | R     | µAh                                         | Coulomb counter.                       |
| `charge_full`                 | R     | µAh                                         | Learned full capacity (last full).     |
| `charge_full_design`          | R     | µAh                                         | Nameplate design capacity.             |
| `energy_now`, `energy_full`, `energy_full_design` | R | µWh                            | Alternate units (some chips).          |
| `current_now`                 | R     | µA; negative = discharging, positive = charging |                                  |
| `current_avg`                 | R     | µA                                          | Smoothed.                              |
| `current_max`                 | R     | µA                                          | Max charge current.                    |
| `voltage_now`                 | R     | µV                                          |                                        |
| `voltage_avg`                 | R     | µV                                          |                                        |
| `voltage_max`, `voltage_min`  | R     | µV                                          | Limits.                                |
| `voltage_max_design`, `voltage_min_design` | R | µV                                    | Design limits.                         |
| `temp`                        | R     | 1/10 °C                                     | **NOT m°C** — historical quirk.        |
| `temp_alert_min`, `temp_alert_max` | R | 1/10 °C                                |                                        |
| `temp_ambient`                | R     | 1/10 °C                                     | Optional.                              |
| `cycle_count`                 | R     | count                                       | `0` = unavailable.                     |
| `state_of_health`             | R     | `0`–`100` percent                           | Since v6.x. Vendor-specific algorithm. |
| `internal_resistance`         | R     | µΩ                                          | Since v6.x.                            |
| `time_to_empty_now`           | R     | seconds                                     |                                        |
| `time_to_full_now`            | R     | seconds                                     |                                        |
| `precharge_current`           | R     | µA                                          |                                        |
| `charge_term_current`         | R     | µA                                          | Current that ends charging.            |
| `constant_charge_current`, `_current_max` | R | µA                              |                                        |
| `constant_charge_voltage`, `_voltage_max` | R | µV                              |                                        |

#### Battery charge thresholds

| File                                | Perms | Format                | Notes                                              |
|-------------------------------------|-------|-----------------------|----------------------------------------------------|
| `charge_control_start_threshold`    | RW    | `0`–`100` percent     | Battery percentage below which charging begins.    |
| `charge_control_end_threshold`      | RW    | `0`–`100` percent     | Battery percentage above which charging stops.     |
| `charge_type`                       | RW    | `Standard` / `Fast` / `Trickle` / `Adaptive` / `Custom` / `Long Life` / `Bypass` / `Unknown` / `N/A` | Active charging algorithm. |
| `charge_types`                      | R     | `Fast [Standard] Long_Life` (active in brackets) | List of supported charge types (since v6.x). Spaces in names are replaced by `_`. |
| `charge_behaviour`                  | RW    | `auto` / `inhibit-charge` / `inhibit-charge-awake` / `force-discharge` | Charging behaviour. Multiple values may be OR'd with ` ` (space). |

Threshold quirk: hardware support is patchy. Writing `80` may be
silently rounded to `75` or `80` depending on the EC. Always
read-back to confirm the actual threshold set. Both thresholds may
be 0–100, but the driver will reject `end < start`. On ThinkPads
the `thinkpad_acpi` driver also exposes `/sys/devices/platform/thinkpad_acpi/charge_stop_threshold`
and `charge_start_threshold` as aliases.

`charge_type=Custom` activates the start/end threshold logic. With
`Standard` or `Fast`, thresholds may be ignored.

#### USB / charger files

| File                          | Perms | Format                  | Notes                                  |
|-------------------------------|-------|-------------------------|----------------------------------------|
| `usb_type`                    | R / RW | `Unknown` / `SDP` / `DCP` / `CDP` / `ACA` / `C` / `PD` / `PD_DRP` / `PD_PPS` / `BrickID` / `PD_SPR_AVS` / `PD_PPS_SPR_AVS` | For chargers, RO; for source controllers, RW. |
| `input_current_limit`         | RW    | µA                      | IBUS limit.                            |
| `input_voltage_limit`         | RW    | µV                      | VBUS limit.                            |
| `input_power_limit`           | RW    | µW                      | Power limit (preferred over I/V limit). |
| `charge_control_limit`        | RW    | µA                      | IBAT limit (battery side).             |
| `charge_control_limit_max`    | R     | µA                      | Max legal value.                       |

### AC adapter

AC adapters appear as `type=Mains` power supplies. The single
relevant file is `online` (R, `0` / `1`; `1` = mains present,
`0` = on battery. RW on some programmable supplies but typically
RO for AC).

Detection pattern for "on AC vs battery":
1. Enumerate `/sys/class/power_supply/*/type`.
2. Find all `Mains` supplies; if any has `online=1`, system is on AC.
3. Find all `Battery` supplies; aggregate `capacity` and `status`.

### PM QoS: system-wide

In addition to the per-device `pm_qos_*` files, the kernel exposes
system-wide PM QoS constraints via `/proc/sys/kernel/` (sysctls):

| Path                                  | Default | Notes                                          |
|---------------------------------------|---------|------------------------------------------------|
| `/proc/sys/kernel/cpu_latency_ns`     | 0       | Max CPU wakeup latency. `0` = no constraint.   |
| `/proc/sys/kernel/network_latency_ns` | 0       | Max network round-trip latency.                |
| `/proc/sys/kernel/perf_cpu_time_max_percent` | 25 | Perf-event sampling budget.              |

For network throughput and `pm_qos` flags, see
`/sys/kernel/debug/pm_qos/` (debugfs).

### Wake sources (system-wide view)

`/sys/power/wake_lock` and `/sys/power/wake_unlock` were covered
above. The complete list of registered wakeup sources is in
`/proc/acpi/wakeup` (ACPI-only; legacy).

Format per line:
```
  Device  S-state   Status  Sysfs node
  GLAN      S4    *disabled  pci:0000:00:1f.6
  PEG0      S4    *enabled   pci:0000:00:01.0
  PWRB      S4    *enabled   button:PNP0C0C:00
```

Toggle with `echo GLAN > /proc/acpi/wakeup` (toggles `enabled` ↔
`disabled`). `*` indicates the current state. This interface is
deprecated in favour of per-device `power/wakeup` files but is
still widely used for ACPI-only wake sources.

The per-device `power/wakeup` files under `/sys/devices/.../` are
the modern equivalent and cover non-ACPI wake sources (PCIe PME,
USB remote wakeup, GPIO, etc.).

### Wakeup events via netlink

The kernel emits wakeup-source events as kobject uevents. Listen
via `udevadm monitor --kernel` or libudev. For power events in
general (AC online/offline, battery capacity crossings), the
`power_supply` and `acpi` subsystems emit uevents of form:

```
POWER_SUPPLY_NAME=BAT0
POWER_SUPPLY_TYPE=Battery
POWER_SUPPLY_CAPACITY=80
```

Plus `ACTION=change` and `SUBSYSTEM=power_supply`. The ACPI
button events (power, sleep, lid) come via the input subsystem
(`/dev/input/eventN`) and are also dispatched by `acpid` /
`systemd-logind`.

---

## Cross-references

### Kernel source files backing each section

GPU:
- `drivers/gpu/drm/drm_ioctl.c` — DRM ioctl dispatch;
  `drm_auth.c` — DRM-Master and auth;
  `drm_mode_config.c` — KMS mode config;
  `drm_atomic.c` — atomic commit implementation;
  `drm_sysfs.c` — `/sys/class/drm/` setup;
  `drm_connector.c` — connector sysfs;
  `drm_blend.c`, `drm_color_mgmt.c` — plane / color properties.
- `drivers/gpu/drm/amd/pm/amdgpu_pm.c` — AMDGPU power/thermal sysfs;
  `amd/amdgpu/amdgpu_vram_mgr.c` — VRAM accounting;
  `amd/amdgpu/amdgpu_fru_eeprom.c` — FRU product info.
- `drivers/gpu/drm/i915/gt/intel_rc6.c` — i915 RC6;
  `i915/gt/intel_rps.c` — i915 RPS;
  `i915/i915_hwmon.c` — i915 hwmon.
- `drivers/gpu/drm/xe/xe_hwmon.c` — xe hwmon.
- UAPI headers in `include/uapi/drm/`: `drm.h`, `drm_mode.h`,
  `amdgpu_drm.h`, `i915_drm.h`, `nouveau_drm.h`.

Thermal:
- `drivers/thermal/thermal_core.c` — thermal-zone registration and
  governor dispatch;
  `thermal_sysfs.c` — `/sys/class/thermal/` files;
  `thermal_hwmon.c` — hwmon bridge.
- Governor implementations: `gov_step_wise.c`, `gov_fair_share.c`,
  `gov_bang_bang.c`, `gov_user_space.c`, `gov_power_allocator.c`.
- `drivers/hwmon/hwmon.c` — hwmon class core.
- `Documentation/hwmon/sysfs-interface.rst` — hwmon ABI spec.

Power:
- `kernel/power/main.c` — `/sys/power/state`, `mem_sleep`, `disk`,
  `image_size`, `autosleep`, `wake_lock`, `wakeup_count`.
- `kernel/power/suspend.c` — suspend-to-RAM and s2idle;
  `hibernate.c` — hibernation; `qos.c` — PM QoS.
- `drivers/base/power/sysfs.c` — `/sys/devices/.../power/` files;
  `runtime.c` — runtime PM core; `wakeup.c` — wakeup sources.
- `drivers/acpi/proc.c` — `/proc/acpi/wakeup`; `drivers/acpi/fan.c`
  — ACPI fan driver.
- `drivers/power/supply/power_supply_sysfs.c` —
  `/sys/class/power_supply/` files; `charge_controller.c` —
  charge_control_*_threshold.

### Open issues / runtime gotchas

1. **pwmN_enable semantics vary by chip**: AMDGPU, nct6775, it87,
   and others extend `0/1/2` with vendor-specific 3/4/5 modes. Read
   the per-driver ABI doc before writing.
2. **trip_point_Y_hyst is in °C, not m°C**: every other thermal
   temperature file is m°C. Divide hyst by 1000 before comparing
   to `temp`.
3. **power_supply `temp` is in 1/10 °C**, not m°C. Multiply by 100
   if you need m°C for interop with thermal-zone temps.
4. **AMDGPU `pp_od_clk_voltage` and `fan_curve` require `manual`
   DPM level**; `fan_curve` also requires a trailing `c` commit
   line. Without it, edits live in a staging buffer that is
   discarded on level change.
5. **NVIDIA exposes nothing via sysfs**: NVML is the only stable
   interface. NVML uses mW/mJ/°C/MHz (not µW/µJ/m°C/kHz like
   hwmon); multiply by 1000 when comparing. NVML writes require
   root or `CAP_SYS_ADMIN`.
6. **Hibernation `image_size` is best-effort**: the kernel may
   exceed the limit if it cannot free enough memory.
7. **`mem_sleep` default is platform-dependent**: modern Intel
   laptops often default to `s2idle` (S0ix) even when S3 is
   supported. Force `deep` if S3 behaviour is required.
8. **`/proc/acpi/wakeup` toggling is deprecated but still works**:
   prefer per-device `power/wakeup` files; ACPI-only wake sources
   are not all represented there.
9. **`charge_control_*_threshold` may be silently rounded**: read
   back after write. `charge_type=Custom` activates thresholds;
   other charge types may ignore them. Hardware support is patchy.
10. **i915 `rps_*` and `rc6_*` files moved to `gt/gtN/`**: on
    multi-tile platforms each tile has its own copy. Legacy
    top-level paths may still exist as symlinks on older kernels.
11. **xe is the successor to i915**: on Intel dGPUs (Arc,
    Battlemage) and future integrated graphics, the `xe` driver
    replaces i915. The hwmon file set differs (PL1/PL2 split,
    multiple temp channels for package/VRAM/MC/PCIe/VRAM-channels).
12. **DRM ioctl privilege**: `DRM_IOCTL_VERSION`, `GETRESOURCES`,
    `GETCONNECTOR` work unprivileged. `MODE_ATOMIC` and
    `MODE_SETCRTC` require DRM-Master (compositor privilege).
    Render nodes (`/dev/dri/renderD128`) only allow
    `DRM_RENDER_ALLOW` ioctls. `DRM_CLIENT_CAP_ATOMIC=1` must be
    set on the fd before using `MODE_ATOMIC` or seeing the full
    plane/property set on connectors.
13. **`/sys/class/drm/cardN/device/hwmon/` is a directory, not a
    single device**: AMDGPU, i915, and xe may each expose multiple
    hwmon subdirs (card-level + per-tile). Always iterate.
14. **`pwm1_enable=0` on AMDGPU means "fan at full speed"**, not
    "PWM disabled / fan off" — matches the ABI's generic wording
    "no fan speed control (i.e. fan at full speed)".
15. **`emul_temp` is dangerous on production systems**: a runaway
    userspace process can disable thermal protection by flooding
    it with low values. Treat as root-only debug.
16. **`runtime_status=unsupported`** means runtime PM is disabled
    for the device; writes to `control` and `autosuspend_delay_ms`
    have no effect.
