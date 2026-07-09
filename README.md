# zenctl

Hardware control for Linux. One C library, one CLI, every tuneable the kernel exposes.

## What it is

`zenctl` is a library-first hardware control surface for Linux. The library, `libzenctl.so`, is the real product. The CLI, `/usr/bin/zenctl`, is a thin wrapper that parses arguments, formats output, and checks privilege. Every hardware operation the CLI performs is a call into the library. The binary itself never touches sysfs, procfs, ioctl, or EFI directly.

The target user is a sysadmin or DevOps engineer who needs scriptable hardware tuning for performance, power, bare-metal provisioning, or infrastructure automation. Secondary users are daemons that embed `libzenctl` to make hardware decisions without shelling out to `cat` and `echo`.

CPU governors, NUMA policy, NVMe queue depth, GPU power limits, fan curves, ACPI sleep states, USB autosuspend, EFI variables. All from one library, one tool. Twelve domains in one API: cpu, mem, storage, net, gpu, thermal, power, pcie, usb, bt, wireless, firmware.

## Why

Linux exposes hundreds of hardware controls through a tangle of sysfs files, procfs tunables, ioctls, ACPI/WMI methods, and vendor-specific paths. Today, automating these means writing brittle shell scripts that `cat` and `echo` paths that change between kernels, or pulling in a different library per subsystem (libdrm for GPU, libpci for PCIe, libusb for USB, libbluetooth for BT, raw ethtool ioctls for NICs).

`zenctl` puts all of that behind one C API and one CLI. One error model. One capability system. One place to add a new kernel interface. Scripts call `zenctl cpu set governor performance` instead of `echo performance > /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor`. Programs link one library instead of five.

The library detects what the running kernel exposes and reports it. Callers do not need to know which kernel version added which sysfs file. The capability query returns `available`, `unavailable`, or `readonly` for any key, and the caller can degrade or skip cleanly.

## How it works

```
+-------------------------------------+
|           /usr/bin/zenctl           |  <- thin CLI wrapper
|         (argument parsing only)     |
+-----------------+-------------------+
                  | calls
+-----------------v-------------------+
|            libzenctl.so             |  <- the real product
|                                     |
|  +----------+  +------------------+ |
|  | Typed    |  | Generic key-value| |  <- dual API surface
|  | Domain   |  | fallback API     | |
|  | API      |  |                  | |
|  +----+-----+  +--------+---------+ |
|       +--------v---------+         |
|       |  Backend Router  |         |
|       +--+--+--+---------+         |
+----------+--+--+-------------------+
   +------+  |  +------+
   v         v       v
 sysfs    procfs   ioctl
/sys/... /proc/... + ACPI/WMI
                  + vendor-specific
```

Kernel interfaces are used in this priority order: sysfs, procfs, ioctl, ACPI/WMI, vendor-specific. Capability detection runs at context creation. The library probes which interfaces the running kernel and hardware expose, then marks each feature as `ZENCTL_CAP_AVAILABLE`, `ZENCTL_CAP_UNAVAILABLE`, or `ZENCTL_CAP_READONLY`. There is no hard kernel version floor; support degrades gracefully per feature.

## Quick start

```
make
sudo make install
zenctl --version
zenctl caps cpu
sudo zenctl cpu set governor performance --cpu all
```

Build requires `cc`, GNU Make, and the standard C library. Optional dependencies (libdrm, libpci, libusb, libbluetooth, libsensors, libudev) are detected at build time; disable any with `ZENCTL_NO_<NAME>=1`.

## Domains

All twelve domains ship from day one. No deferred domains.

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

The implementation splits domain 9 into three CLI surfaces (`usb`, `bt`, `wireless`) for clarity, so the CLI has twelve domains total.

## CLI examples

```
# read
zenctl cpu get governor --cpu 0
zenctl thermal get temp --zone thermal_zone0
zenctl net get ring-rx --iface eth0
zenctl pcie get link-speed --addr 0000:01:00.0
zenctl gpu get temp --dev 0
zenctl usb enumerate

# write
sudo zenctl cpu set governor performance --cpu all
sudo zenctl storage set scheduler mq-deadline --dev sda
sudo zenctl gpu set fan-pwm 128 --dev /dev/dri/card0
sudo zenctl net set ring-rx 4096 --iface eth0
sudo zenctl thermal set policy step_wise --zone thermal_zone0

# destructive: needs --confirm or interactive Y/N
sudo zenctl cpu set online off --cpu 4 --confirm
sudo zenctl usb reset --dev 1-2 --confirm
sudo zenctl firmware efi delete BootOrder <guid> --confirm

# dry run (prints what would change, touches nothing)
zenctl cpu set freq-max 4000000 --cpu all --dry-run

# json output
zenctl caps cpu --json
zenctl profile list --json

# profiles
sudo zenctl profile save server-perf
sudo zenctl profile load server-perf
zenctl profile list
sudo zenctl profile delete server-perf --confirm
```

## Library example

Link with `-lzenctl`. Include `<zenctl/zenctl.h>` to pull in every domain header.

```c
#include <stdio.h>
#include <zenctl/zenctl.h>

int main(void)
{
    zenctl_err_t err = {0};

    zenctl_cpu_t *cpu = zenctl_cpu_open(0, &err);
    if (!cpu) {
        fprintf(stderr, "open: %s\n", err.message);
        return 1;
    }

    if (zenctl_cpu_set_governor(cpu, "performance", &err) != ZENCTL_OK) {
        fprintf(stderr, "set governor: %s\n", err.message);
    }

    int64_t max_hz = 0;
    if (zenctl_cpu_get_freq_max(cpu, &max_hz, &err) == ZENCTL_OK) {
        printf("cpu0 max freq: %lld Hz\n", (long long)max_hz);
    }

    zenctl_cpu_close(cpu);
    return 0;
}
```

Build it:

```
cc -o set-perf set-perf.c $(pkg-config --cflags --libs zenctl)
```

For dynamic access (keys not known at compile time), use the generic API:

```c
zenctl_val_t val;
zenctl_get(ctx, "cpu.0.governor", &val, &err);
printf("%s\n", val.v.s);
zenctl_val_free(&val);

zenctl_set(ctx, "cpu.0.governor", "performance", &err);
```

## Profiles

A profile is a named snapshot of hardware state stored as TOML. System profiles live in `/etc/zenctl/profiles/`; user profiles in `~/.config/zenctl/profiles/`. User profiles are limited to read-only domains. Profiles are versioned via a `schema_version` field. Partial profiles are valid.

```
sudo zenctl profile save server-perf
sudo zenctl profile load server-perf --dry-run     # preview
sudo zenctl profile load server-perf               # apply
sudo zenctl profile delete server-perf --confirm
```

## Building from source

GNU Make. No CMake, no Meson, no autotools. One Makefile at the repo root.

```
make            # build libzenctl.so + zenctl binary
make lib        # build libzenctl.so only
make cli        # build zenctl only
make test       # build and run unit tests
make install    # install to PREFIX (default /usr/local)
make uninstall
make clean
```

`PREFIX` defaults to `/usr/local`. Override on the command line: `make install PREFIX=/opt/zenctl`.

Installed layout:

```
/usr/local/lib/libzenctl.so
/usr/local/lib/libzenctl.so.1
/usr/local/lib/libzenctl.so.1.0.0
/usr/local/include/zenctl/zenctl.h
/usr/local/include/zenctl/cpu.h
/usr/local/include/zenctl/net.h
... (one header per domain)
/usr/local/lib/pkgconfig/zenctl.pc
/usr/local/bin/zenctl
/usr/local/share/man/man1/zenctl.1
/usr/local/share/man/man3/libzenctl.3
```

## Project structure

```
zenctl/
├── Makefile
├── README.md
├── LICENSE
├── DESIGN.md
├── zenctl.pc.in           # pkg-config template
│
├── include/
│   └── zenctl/            # public headers
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
│       ├── bt.h
│       ├── wireless.h
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
│   ├── usb/               # also bt, wireless, rfkill
│   └── firmware/
│
├── cli/                   # zenctl binary source
│   ├── main.c
│   ├── output.c           # table + JSON formatting
│   ├── profile.c          # profile save/load
│   └── cmd/               # one .c file per domain subcommand
│
├── tests/
│   ├── unit/              # mocked sysfs, no hardware, no root
│   └── integration/       # real hardware, optional
│
├── man/
│   ├── zenctl.1
│   └── libzenctl.3
│
└── docs/                  # kernel ABI reference
    ├── KERNEL_CPU_MEM.md
    ├── KERNEL_STORAGE_NET_PCIE.md
    ├── KERNEL_GPU_THERMAL_POWER.md
    └── KERNEL_USB_BT_FW.md
```

## License

MIT. See [LICENSE](LICENSE) for the full text.
