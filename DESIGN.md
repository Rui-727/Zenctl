# zenctl: Project Planning Document

全 + ctl. "Whole control." A C library and CLI that exposes every
hardware control on Linux.

`libzenctl.so` and `/usr/bin/zenctl`.

---

## 1. Vision

`zenctl` is a library-first hardware control surface for Linux. The
library is the real product. The CLI is a thin wrapper.

You give it a link. Your program reads and writes every tuneable
hardware parameter the kernel exposes. CPU governors, NUMA policy, NVMe
queue depth, GPU power limits, fan curves, ACPI sleep states, USB
autosuspend, EFI variables. All from one library, one tool.

The target user is a sysadmin or DevOps engineer who needs scriptable
hardware tuning for performance, power, bare-metal provisioning, or
infrastructure automation. Secondary users are daemons that embed
`libzenctl` to automate hardware decisions.

---

## 2. Hardware domains

All 10 domains are in scope from day one. No deferred domains.

| # | Domain | Controls |
|---|--------|----------|
| 1 | CPU | Frequency scaling, governors, core on/off, C-states, P-states, SMT |
| 2 | Memory | Hugepages (static + THP), NUMA policy, memory overcommit |
| 3 | Storage | I/O schedulers, NVMe queue depth, disk power management, write cache |
| 4 | Network | RX/TX ring buffers, offload flags (TSO/GRO/LRO), IRQ affinity, queues |
| 5 | GPU | Clock frequencies, fan curves, power limits, performance levels |
| 6 | Thermal | Fan control, thermal zone thresholds, cooling device states, sensor reads |
| 7 | Power | ACPI sleep states (S-states), runtime PM, battery charge thresholds |
| 8 | PCIe / IOMMU | ASPM power states, IOMMU groups, ACS overrides, link speed |
| 9 | USB / Bluetooth / Wireless | USB autosuspend, Bluetooth power, wireless regulatory domain, TX power |
| 10 | Firmware / BIOS | ACPI/WMI-exposed settings, DMI info reads, EFI variable access |

---

## 3. Architecture

```
┌─────────────────────────────────────┐
│           /usr/bin/zenctl           │  ← thin CLI wrapper
│         (argument parsing only)     │
└───────────────┬─────────────────────┘
                │ calls
┌───────────────▼─────────────────────┐
│            libzenctl.so             │  ← the real product
│                                     │
│  ┌──────────┐  ┌──────────────────┐ │
│  │ Typed    │  │ Generic key-value│ │  ← dual API surface
│  │ Domain   │  │ fallback API     │ │
│  │ API      │  │                  │ │
│  └────┬─────┘  └────────┬─────────┘ │
│       └────────┬─────────┘          │
│          ┌─────▼──────┐             │
│          │  Backend   │             │
│          │  Router    │             │
│          └──┬──┬──┬───┘             │
└─────────────┼──┼──┼─────────────── ┘
     ┌─────────┘  │  └──────────┐
     ▼            ▼             ▼
  sysfs       procfs +     ioctl +
 /sys/...     /proc/...   ACPI/WMI +
                          vendor-specific
```

### 3.1 Kernel interface strategy

`libzenctl` uses whatever interface the feature needs, in this
priority order:

1. sysfs (`/sys/...`). Preferred for everything it covers.
2. procfs (`/proc/...`). For interfaces not yet in sysfs.
3. ioctl. For kernel subsystems unreachable via fs interfaces (DRM
   for GPU, ethtool for network).
4. ACPI / WMI. For firmware-exposed controls (thermal, BIOS settings,
   battery thresholds).
5. Vendor-specific. DMI strings, EFI variables, vendor kernel modules
   where needed.

Capability detection runs at init. The library probes which
interfaces are available on the running kernel and hardware, then
marks features as `ZENCTL_CAP_AVAILABLE`, `ZENCTL_CAP_UNAVAILABLE`, or
`ZENCTL_CAP_READONLY`. Callers check capability before use. There is
no hard kernel version floor. Support degrades gracefully per feature.

---

## 4. Library API

### 4.1 Two API layers (both shipped)

Layer 1: typed domain API. Preferred for compiled callers. Each
domain has a handle type, an open/close lifecycle, and explicit typed
functions.

```c
zenctl_cpu_t *cpu = zenctl_cpu_open(0, &err);
zenctl_cpu_set_governor(cpu, "performance", &err);
zenctl_cpu_set_freq_max(cpu, 3600000, &err);
zenctl_cpu_close(cpu);

zenctl_thermal_t *tz = zenctl_thermal_open("thermal_zone0", &err);
int32_t temp = zenctl_thermal_get_temp(tz, &err);
zenctl_thermal_close(tz);
```

Layer 2: generic key-value API. For scripting, dynamic access, and
keys not known at compile time.

```c
zenctl_val_t val;
zenctl_get(ctx, "cpu.0.governor", &val, &err);
zenctl_set(ctx, "cpu.0.governor", "performance", &err);

zenctl_key_iter_t *it = zenctl_keys(ctx, "cpu.0", &err);
while (zenctl_key_iter_next(it, &key)) { ... }
zenctl_key_iter_free(it);
```

### 4.2 Error model

Every fallible call takes a `zenctl_err_t *err` out-parameter:

```c
typedef struct {
    int          code;
    char         message[256];
    char         context[256];
    bool         recoverable;
} zenctl_err_t;
```

`zenctl_strerror(code)` returns a static string for any error code.
Callers check `err.code != ZENCTL_OK` after every call.

### 4.3 Capability query

```c
zenctl_cap_t cap = zenctl_query_cap(ctx, "cpu.0.c_states");
/* ZENCTL_CAP_AVAILABLE | ZENCTL_CAP_READONLY | ZENCTL_CAP_UNAVAILABLE */
```

---

## 5. CLI

### 5.1 Relationship to library

The CLI does argument parsing, output formatting, and privilege
checks. Every hardware operation is a call into `libzenctl`. No sysfs
or ioctl access happens in the CLI binary.

### 5.2 Command structure

```
zenctl <domain> <subcommand> [target] [options]
zenctl profile <save|load|list|delete> <name>
zenctl caps [domain]
```

Examples:

```bash
# read
zenctl cpu get governor --cpu 0
zenctl thermal get temp --zone thermal_zone0
zenctl net get ring-buffer --iface eth0

# write
zenctl cpu set governor performance --cpu all
zenctl storage set scheduler mq-deadline --dev sda
zenctl gpu set power-limit 150 --dev /dev/dri/card0

# dry run (prints what would change, touches nothing)
zenctl cpu set freq-max 4000MHz --cpu all --dry-run

# json output
zenctl cpu get --all --json

# profiles
zenctl profile save server-perf
zenctl profile load server-perf
zenctl profile list --json
```

### 5.3 Output formats

Default: structured human-readable table (styled after `lsblk` /
`lspci`).

`--json`: machine-parseable JSON. Always has a `"status"` field
(`"ok"` or `"error"`) and a `"data"` or `"error"` key.

### 5.4 Safety rules

| Operation | Behaviour |
|---|---|
| Any read | Executes immediately, no prompt |
| Write, low risk | Executes with a confirmation line to stderr |
| Write, destructive or irreversible | Requires `--confirm` flag or interactive Y/N prompt |
| Any write with `--dry-run` | Prints planned changes only, returns 0 |
| Root not present | Exits with `ZENCTL_ERR_EPERM` and a clear message |

Destructive operations: disabling CPU cores, changing IOMMU groups,
writing EFI variables, modifying ACPI S-states, disabling USB devices.

---

## 6. Profiles

Profiles save and restore a named snapshot of hardware state.

- Stored in `/etc/zenctl/profiles/<name>.toml` (system) or
  `~/.config/zenctl/profiles/<name>.toml` (user, read-only domains
  only).
- Format: TOML, human-editable.
- `zenctl profile save <name>` captures current live state of all
  available domains.
- `zenctl profile load <name>` restores, respecting `--dry-run` and
  `--confirm`.
- Profiles are versioned with a `schema_version` field for forward
  compatibility.
- Partial profiles are valid. A profile need not cover every domain.

---

## 7. Dependencies

No restriction on dependencies. Each domain uses the best available
library.

| Domain | Dependencies |
|---|---|
| CPU, Memory, Power | sysfs/procfs direct (no lib needed) |
| Storage | `libudev` for device enumeration |
| Network | `libethtool` or raw ethtool ioctl |
| GPU | `libdrm`, vendor libs (NVML for NVIDIA, ROCm for AMD) where available |
| Thermal | sysfs + `libsensors` (lm-sensors) |
| PCIe | `libpci` (pciutils) |
| USB / Bluetooth | `libusb`, `libbluetooth` |
| Firmware / ACPI | `libacpi`, direct EFI variable access via `/sys/firmware/efi/efivars` |
| BIOS / WMI | `libwmi` or direct WMI sysfs interface |

All dependencies are detected at build time. Optional deps are
disabled via Makefile variables (e.g. `ZENCTL_NO_LIBDRM=1`).

---

## 8. Build and distribution

### 8.1 Build system

GNU Make. No CMake, no Meson, no autotools. A single `Makefile` at
the repo root.

```
make            # build libzenctl.so + zenctl binary
make lib        # build libzenctl.so only
make cli        # build zenctl only
make test       # build and run unit tests
make install    # install to PREFIX (default /usr/local)
make uninstall
make clean
```

### 8.2 Installed layout

```
/usr/local/lib/libzenctl.so
/usr/local/lib/libzenctl.so.1         (soname symlink)
/usr/local/lib/libzenctl.so.1.0.0     (versioned)
/usr/local/include/zenctl/zenctl.h    (master header)
/usr/local/include/zenctl/cpu.h
/usr/local/include/zenctl/net.h
... (one header per domain)
/usr/local/lib/pkgconfig/zenctl.pc    (pkg-config file)
/usr/local/bin/zenctl
/usr/local/share/man/man1/zenctl.1
/usr/local/share/man/man3/libzenctl.3
```

### 8.3 pkg-config

A `zenctl.pc` file is installed so downstream projects can do:

```bash
gcc $(pkg-config --cflags --libs zenctl) myapp.c
```

---

## 9. Testing

### 9.1 Unit tests (mandatory, mocked kernel)

- Mock layer replaces all sysfs/procfs/ioctl calls with in-process
  fakes.
- Tests are compiled into a separate `zenctl-test` binary.
- Every public library function has at minimum: happy path, error
  path, capability-unavailable path.
- Tests run without root, without hardware, in CI.

### 9.2 Integration tests (optional, real hardware)

- Guarded by `ZENCTL_INTEGRATION=1` environment variable.
- Require root.
- Read-only by default. Destructive writes require
  `ZENCTL_INTEGRATION_WRITE=1`.
- Designed to run on a dedicated test machine or VM with passthrough
  hardware.
- Results are not part of CI gates. They are manual verification.

### 9.3 Test framework

Plain C with a minimal hand-rolled test harness. No external framework
dependency. Test output is TAP-compatible for CI integration.

---

## 10. Security model

| Concern | Decision |
|---|---|
| Privilege required | Root (`uid == 0`) for all write operations |
| Read operations | Unprivileged where kernel allows; root where sysfs requires it |
| No SUID binary | `zenctl` does not ship setuid. Caller invokes with sudo/root. |
| No privilege-separated daemon | Out of scope for v1 |
| Bounds validation | All numeric writes are range-checked against kernel-reported min/max before writing |
| Dry-run | Available on every write subcommand. Never touches hardware. |
| Audit logging | All writes log to syslog (`LOG_AUTH` facility) with uid, command, and values |

---

## 11. Project structure

```
zenctl/
├── Makefile
├── README.md
├── LICENSE
│
├── include/
│   └── zenctl/
│       ├── zenctl.h       # master include
│       ├── cpu.h
│       ├── mem.h
│       ├── storage.h
│       ├── net.h
│       ├── gpu.h
│       ├── thermal.h
│       ├── power.h
│       ├── pcie.h
│       ├── usb.h
│       └── firmware.h
│
├── lib/                   # libzenctl source
│   ├── core/              # context, capability, error, key-value API
│   ├── cpu/
│   ├── mem/
│   ├── storage/
│   ├── net/
│   ├── gpu/
│   ├── thermal/
│   ├── power/
│   ├── pcie/
│   ├── usb/
│   └── firmware/
│
├── cli/                   # zenctl binary source
│   ├── main.c
│   ├── output.c           # table + JSON formatting
│   ├── profile.c          # profile save/load
│   └── cmd/               # one .c file per domain subcommand
│
├── tests/
│   ├── unit/
│   │   ├── harness.h      # minimal TAP harness
│   │   ├── mock_sysfs.c
│   │   └── test_cpu.c ... (one per domain)
│   └── integration/
│       └── run_integration.sh
│
├── man/
│   ├── zenctl.1
│   └── libzenctl.3
│
└── zenctl.pc.in           # pkg-config template
```

---

## 12. Out of scope for v1

- Privilege-separated daemon mode (for non-root callers via Unix socket)
- Language bindings (Python, Rust)
- Distro packages (`.deb`, `.rpm`)
- Watch/monitor mode (`zenctl thermal watch --zone all --interval 1s`)
- Systemd unit generation from profiles
- Hardware event callbacks / udev integration

---

## 13. Key decisions

| Decision | Choice |
|---|---|
| Primary artifact | `libzenctl.so`, C library |
| CLI relationship | Thin wrapper, no direct kernel access |
| API shape | Typed domain API + generic key-value fallback |
| Kernel interfaces | sysfs + procfs + ioctl + ACPI/WMI + vendor |
| Kernel version support | Runtime capability detection, graceful degradation |
| Permission model | Root required for writes. Dry-run available everywhere. |
| Destructive ops | Require `--confirm` or interactive prompt |
| Output | Human table (default) + `--json` |
| Persistence | Stateless ops + optional TOML profiles |
| Dependencies | Unrestricted. Build-time feature flags for optional deps. |
| Error model | Structured `zenctl_err_t` (code + message + context + recoverable) |
| Distribution | `.so` + headers + `pkg-config` + GNU Make install |
| Testing | Unit (mocked, mandatory) + integration (real hardware, optional) |
| Language bindings | None in v1 |
| Name | `zenctl` (全 + ctl) |
