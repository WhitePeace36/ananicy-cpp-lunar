![Ananicy Cpp Logo](./assets/ananicy_logo.svg)

# Ananicy Cpp
[Ananicy](https://github.com/Nefelim4ag/Ananicy) rewritten in C++ for much lower CPU and memory usage.

_Beta status_

[![Packaging status](https://repology.org/badge/vertical-allrepos/ananicy-cpp.svg)](https://repology.org/project/ananicy-cpp/versions)

## What works

- [X] Rule and configuration loading
- [X] Process detection and scanning
- [X] Renicing
- [X] Changing CPU scheduler
- [X] Changing IO class and IO nice
- [X] Setting OOM score
- [X] Change default configuration file and rules directory using environment variables (`ANANICY_CPP_{CONF,CONFDIR}`)
- [X] CPU affinity (cpuset)
  - [X] Pin processes to specific CPU cores
  - [X] Full CPU topology detection (NUMA nodes, LLC domains, core types)
  - [X] Intel big.LITTLE / AMD heterogeneous core detection
  - [X] Named cpuset aliases (`big-cores`, `little-cores`, `llc-N`, `node-N`, etc.)
  - [X] AMD X3D V-Cache topology auto-detection
  - [X] `amd_x3d_vcache` kernel driver integration
- [X] Cgroups
  - [X] V1
  - [X] V2 (_Limited support, see #21_)
- [X] Autogroup
- [X] Systemd integration
  - [X] Unit file
  - [X] Hardening unit file
  - [X] Working sd_notify
- [X] CLI compatibility with Ananicy
- [ ] Something else... ?

## Installation

#### Dependencies

- [CMake](https://cmake.org/download/) 3.17+
- [fmtlib](https://github.com/fmtlib/fmt) 8.0+ _(can be automatically downloaded)_
- [spdlog](https://github.com/gabime/spdlog) 1.9+ _(can be automatically downloaded)_
- [nlohmann_json](https://github.com/nlohmann/json) 3.9+ _(can be automatically downloaded)_
- C++ compiler compatible with C++20 (`g++ >= 10` or `clang++ >= 10`).
  Notably, `<concepts>` and `std::jthread` are required. Work is in progress to add support to `g++ ~= 9`, see https://gitlab.com/ananicy-cpp/ananicy-cpp/-/issues/28 for more info.
- For the BPF process listener (the default, see `USE_BPF_PROC_IMPL` below):
    - [bpf](https://www.kernel.org) (bpftool), and a kernel with BTF (`/sys/kernel/btf/vmlinux`)
    - [libbpf](https://github.com/libbpf/libbpf)
    - [libelf](https://sourceware.org/elfutils/)
    - [clang](https://clang.llvm.org/) clang++ >= 10
- _Optional_:
    - libsystemd (if you want systemd support)

#### Debian/Ubuntu
Install the prerequisites: `sudo apt install git cmake build-essential g++ libsystemd-dev`

### Linux
It should be a fully drop-in replacement from Ananicy.
If you detect a difference in behavior (except for bugs in Ananicy that are fixed in `ananicy-cpp`),
please create an issue.

If you want pre-made community rules, you can use the rules from the original Ananicy project.
Simply copy them to your rules directory (by default, `/etc/ananicy.d`).

#### Available CMake options

- `CMAKE_BUILD_TYPE:STRING`: Either `None` (default), `Release`, or `Debug`.
The effects are mainly on the default compiler flags `ananicy-cpp` is built with.
- `USE_EXTERNAL_JSON:BOOL`: Use system nlohmann_json library,
- `USE_EXTERNAL_SPDLOG:BOOL`: Use system spdlog library,
- `USE_EXTERNAL_FMTLIB:BOOL`: Use system fmtlib,
- `ENABLE_SYSTEMD:BOOL`: Add systemd support,
- `USE_BPF_PROC_IMPL:BOOL`: Use BPF to get process events (default `ON`).
  Set it to `OFF` to use the netlink proc connector instead, which needs neither BTF nor
  a BPF toolchain (for older or minimal kernels). The listener is chosen at build time,
- `BPF_BUILD_LIBBPF:BOOL`: Build libbpf instead of using system libary,
- `STATIC:BOOL`: Build a static binary,

#### How processes are detected

At startup ananicy-cpp scans `/proc` once and applies the rules to every running process.
After that it reacts to process events (start, `exec`, rename) from one of two listeners,
chosen at build time with `USE_BPF_PROC_IMPL`:

- **BPF** (default): hooks the kernel's fork, exec and rename tracepoints. Needs a kernel
  with BTF.
- **netlink proc connector** (`-DUSE_BPF_PROC_IMPL=OFF`): works on older or minimal kernels.
  It needs `CAP_NET_ADMIN`. Root has it unless it was taken away (a container, or
  `CapabilityBoundingSet=` in the unit). Without it the kernel silently sends no events,
  so ananicy-cpp checks for it at startup and refuses to start without it.

Both need the daemon to run as root.

If events are lost because the daemon could not keep up (thousands of processes started at
once), it logs `process events dropped` / `Lost ... process events` and scans `/proc` again
(at most once per second), so the processes started in that window still get their rules.

#### Building

In order to actually build `ananicy-cpp`, open a terminal,
clone the repository (or download a release archive), and do the following commands:

- If using the `git` repo (development version):
  - `git clone https://gitlab.com/ananicy-cpp/ananicy-cpp.git`
  - `cd ananicy-cpp`
- If using release archive:
  - `wget https://gitlab.com/ananicy-cpp/ananicy-cpp/-/archive/v1.2.0/ananicy-cpp-v1.2.0.tar.gz`
  - `tar -xvf ananicy-cpp-v1.2.0.tar.gz`
  - `cd ananicy-cpp-v1.2.0`

```
cmake -B build \
  -DCMAKE_RELEASE_TYPE=Release \
  -D[Your Option Here] -D[Another Option] \
  -S .
cmake --build build --target ananicy-cpp
sudo cmake --install build --component Runtime
```

- If using install-deps.sh and configure.sh (BPF listener, the default)
  - `git clone https://gitlab.com/ananicy-cpp/ananicy-cpp.git`
  - `cd ananicy-cpp`
  - `./install-deps.sh && ./configure.sh && ./build.sh`
  - for the netlink listener instead: `env USE_BPF=OFF ./install-deps.sh && ./configure.sh --use_netlink_proc && ./build.sh`
  - `sudo cmake --install build/RelWithDebInfo --component Runtime`

#### Service

To enable the systemd service,
enable Systemd support with `-DENABLE_SYSTEMD=ON` on `cmake` invocation, and run this command _after_ installing `ananicy-cpp`:
```
systemctl enable --now ananicy-cpp.service
```
If you want to stop or disable it later, you can use:
```
systemctl (stop|disable) ananicy-cpp.service
```
You can have more information in `systemd`'s man page (`man systemd` on Linux)

#### Commands

| Command | What it does |
|---------|--------------|
| `ananicy-cpp start` | Run the daemon (this is what the service does). Needs root. |
| `ananicy-cpp dump rules` | Print all rules as JSON, with their `type` merged in. `cmdline_contains` rules are listed as `name [cmdline: [...]]`. |
| `ananicy-cpp dump types` / `dump cgroups` | Print the loaded types / cgroup definitions. |
| `ananicy-cpp dump proc` / `dump autogroup` | Print what ananicy-cpp sees for every running process / the autogroups. |
| `ananicy-cpp debug cgroups` | Print cgroup diagnostics for bug reports. |
| `ananicy-cpp --reload` | Ask the running daemon to reread `ananicy.conf` (the `apply_*` switches, `loglevel`, …). Rules, types and cgroups are **not** reloaded: restart the service after changing them. Fails (exit status 1) if no daemon is running. |
| `ananicy-cpp --version` / `--help` | Version / usage. |

An unknown command (e.g. `ananicy-cpp show rules`) prints the usage and exits with
status 1. It never starts the daemon.

#### Single instance and crashes

Only one daemon runs at a time. It holds a lock on `/dev/shm/AnanicyCppMutex`; a second
`start` exits with "Ananicy Cpp is already running!". The lock belongs to the running
process, so the kernel releases it when the daemon dies in any way (`kill -9`, the OOM
killer, a crash). The leftover file then does not block the next start, and
`Restart=always` in the service brings the daemon back on its own.

`ananicy-cpp --force-remove-semaphore` deletes that file. It is only needed for versions
before this change. Do not run it while the daemon is running, because a second instance
could then start next to it.

### Distro-specific
#### Arch-based distros

There is an AUR package named `ananicy-cpp`, which is maintained by me.
You can simply use your preferred AUR-helper to install it.

There is also binaries in `cachyos` and `chaotic-aur`, although `ananicy-cpp` is relatively fast to build.


## Configuration

### Global configuration

Global settings are defined in `/etc/ananicy.d/ananicy-cpp.conf` (or the path set via `ANANICY_CPP_CONF`).
Format is `key=value`, one per line.

| Option | Default | Description |
|--------|---------|-------------|
| `check_freq` | `60` | How often the main loop wakes up, in seconds. With `--manual-scanning`, a full `/proc` scan runs every `check_freq` seconds. Without it, process events are used and there is no periodic scan (see "How processes are detected") |
| `apply_nice` | `true` | Apply nice values from rules |
| `apply_sched` | `true` | Apply scheduling policy from rules |
| `apply_ionice` | `true` | Apply I/O nice values from rules |
| `apply_oom_score_adj` | `true` | Apply OOM score adjustments from rules |
| `apply_latnice` | `true` | Apply latency nice values from rules |
| `apply_cpuset` | `true` | Apply CPU affinity (cpuset) from rules |
| `cgroup_load` | `true` | Load cgroup definitions |
| `type_load` | `true` | Load type definitions |
| `rule_load` | `true` | Load rule definitions |
| `cgroup_realtime_workaround` | `true` | Enable cgroup realtime workaround |
| `log_applied_rule` | `false` | Log each applied rule |
| `loglevel` | `info` | Log level (`trace`, `debug`, `info`, `warn`, `error`, `critical`) |
| `x3d_mode` | `auto` | AMD X3D driver mode: `auto` (don't touch), `cache`, or `frequency` |

#### CPU topology detection

At startup, ananicy-cpp probes sysfs to build a full CPU topology:
- **NUMA nodes** and **LLC (last-level cache) domains** are identified and grouped.
- **Core types** (BigTurbo, Big, Little) are classified using CPU capacity values from sysfs.
  Five capacity sources are probed in priority order: `amd_pstate_prefcore_ranking`,
  `amd_pstate_highest_perf`, `acpi_cppc/highest_perf`, `cpu_capacity`, `cpuinfo_max_freq`.
- **big.LITTLE detection**: If the highest-capacity CPU exceeds 1.3x the lowest, the system
  is classified as heterogeneous and cores are split into Big/Little groups.
- **SMT** (simultaneous multithreading) status is detected.

This topology information drives the named cpuset aliases described above.

#### AMD X3D support

On AMD X3D CPUs (e.g., 7800X3D, 7950X3D, 9800X3D, 9950X3D), ananicy-cpp automatically detects
the V-Cache and frequency CCDs at startup by reading CPU topology and L3 cache sizes from sysfs.

The `x3d_mode` option controls the `amd_x3d_vcache` kernel driver (if present):
- `auto`: Do not change the driver mode (default).
- `cache`: Set the driver to prefer the V-Cache CCD for scheduling.
- `frequency`: Set the driver to prefer the high-frequency CCD for scheduling.

### Rules

Add rules in `/etc/ananicy.d`. This path can be overridden with `ANANICY_CPP_CONFDIR`
environment variable.
Rules are defined in files ending with `.rules`.
For instance, to add a rule for GCC, you could do the following:

- Create the `/etc/ananicy.d/10-compilers` folder.
- Create the `/etc/ananicy.d/10-compilers/gcc.rules` file
- Add `{"name": "gcc", "nice": 19, "latency_nice": 19, "sched": "batch", "ioclass": "idle"}` to the file.

To pin a game to the V-Cache CCD on an AMD X3D system:
~~~json
{"name": "game-x", "cpuset": "x3d-cache", "nice": -5}
~~~

To pin a workload to performance cores (Intel P-cores or AMD high-capacity cores):
~~~json
{"name": "my-game", "cpuset": "performance-cores", "nice": -5}
~~~

To restrict a background task to efficiency cores:
~~~json
{"name": "file-indexer", "cpuset": "efficiency-cores", "nice": 19, "sched": "idle"}
~~~

To pin to a specific LLC domain or NUMA node:
~~~json
{"name": "database", "cpuset": "node-0"}
~~~

Or to pin to specific cores directly:
~~~json
{"name": "my-app", "cpuset": "0-3,8-11"}
~~~

You can then (re)start `ananicy-cpp.service`.

#### Load order and duplicate rules

All `.rules`, `.types` and `.cgroups` files below the configuration directory
(subdirectories included) are loaded in **sorted path order**. `00-default/…` loads
before `50-community/…`, which loads before `99-local.rules`. Lines within a file are
read top to bottom.

If the same `name`, `type` or `cgroup` is defined more than once, the one loaded last
wins, and a warning names both files:

~~~
[warning] name rule 'gcc' from /etc/ananicy.d/99-local.rules replaces the one from /etc/ananicy.d/00-default/compilers.rules (the last one loaded wins)
~~~

To override a community rule, put your version in a file that sorts later, e.g.
`/etc/ananicy.d/99-local.rules`. `cmdline_contains` rules are not replaced this way:
several can exist for the same name (see below).

#### Rules for specific Java, Python, … programs

Rules match the program name, which is the file name of `argv[0]`. Every Java program
is therefore `java` and every Python script is `python3`. To tell them apart, add
`cmdline_contains` to a `name` rule. It can be a string or a list of strings, and every
string must appear somewhere in the full command line (the arguments joined by spaces,
case-sensitive) for the rule to apply:

~~~json
{"name": "java",    "cmdline_contains": "minecraft", "type": "Game"}
{"name": "java",    "cmdline_contains": ["-jar", "jenkins.war"], "nice": 10}
{"name": "python3", "cmdline_contains": "jupyter-lab", "nice": 5}
{"name": "java",    "nice": 0}
~~~

- `cmdline_contains` rules for a name are tried in load order (see "Load order and
  duplicate rules" above), and the
  first one that matches wins. If none matches, the plain rule with the same `name`
  is used (if there is one). Several rules for the same name can therefore coexist.
- `cmdline_contains` requires `name`. Without it (on its own, or on a `type`/`cgroup`
  line) the line is ignored with a warning.
- The command line is only read for names that have `cmdline_contains` rules, so other
  processes cost nothing extra.
- To see the exact command line ananicy-cpp matches against: `tr '\0' ' ' < /proc/<pid>/cmdline`.

#### Supported attributes

`nice`, `latency_nice`, `sched`, `ioclass`/`ionice` and `cpuset` are per-thread settings in
Linux. ananicy-cpp applies them to **every thread** of the process, so multi-threaded programs
(Java, games, browsers…) get them on all their threads, not only the main one.

If an attribute cannot be applied (missing permission, a value the kernel rejects, an unknown
`sched` or `ioclass` name), the error is logged and the rule's **other attributes are still
applied**. Only a process that has exited stops the rest of its rule.

- `nice: [-20-19]`: Set the nice value of the process.
A process with a higher nice value will be more "polite", and will get less
cpu time than processes with a lower nice value.
- `latency_nice: [-20-19]`: Set the latency_nice value of the process.
A process with a lower latency_nice value indicates the task to have the least
latency as compared to the task having a higher latency_nice.
A additonal kernel patch is needed see [latency_nice](https://lore.kernel.org/lkml/20221110175009.18458-1-vincent.guittot@linaro.org/)
- `sched: {"fifo", "rr", "normal", "batch", "idle"}`:
Set the scheduling policy.
  - `fifo` and `rr` (for round-robin) are realtime scheduling policies, and must only be used for
latency critical programs, like `Xorg` or `pulseaudio` for instance. Nice values are ignored,
  `rtprio` should be used instead.
  - `deadline`: Special realtime scheduling policy which _can't_ be set by `ananicy-cpp`.
  A rule with `"sched": "deadline"` logs a warning and leaves the process's current policy unchanged.
  - `normal` is well... normal, the default behavior for the current OS.
  Specifying this option can be useful if you want to force the child of a
  realtime process to have a normal scheduling policy.
  - `batch`: Very useful for compilers or other CPU-hungry, non-interactive programs,
  like compilers for instance. It can actually improve their performance with almost no cost
  on the rest of the system.
  - `idle`: Very, very low priority, even lower than a nice value of `19`.
  Useful for background, low priority stuff, like file indexer for instance.
- `rtprio: [0, 99]`: Sets the static priority of a process. Only relevant if the actual scheduling policy
of a process is a realtime one, i.e. `fifo`, `rr` or `deadline`.
A higher value means a higher priority.
- `ioclass: {"best-effort", "realtime", "idle", "none"}`:
Define the IO scheduling policy. By default, it is `best-effort`.
**Only the CFQ I/O scheduler supports `ioclass` and `ionice`, see [ioprio_set](https://man7.org/linux/man-pages/man2/ioprio_set.2.html)**.
  - `realtime` is to be used cautiously, as the process will have the absolute priority
  above all `best-effort` processes, and can [starve][wikipedia:starvation] them.
  This could prevent you from starting a shell, for instance.
    - `ionice: [0, 7]`. Lower value is higher priority.
  - `idle`: Process gets I/O resources after all other processes.
  This could [starve][wikipedia:starvation] the process.
    - `ionice` is completely ignored.
  - `none`: Reset I/O policy to system default, `ionice` must be `0`.
  - `best-effort`: Try to fairly share I/O resources between processes.
    - `ionice: [0, 7]`. Lower value is higher priority.
- `oom_score_adj: [-999, 999]`: Adjust the **O**ut **O**f **M**emory killer score of a process.
  Negative value decrease the score of the process, making it _less_ likely to be killed if available memory
  gets very low. It is recommended to use it on critical programs which must be killed last if you lack memory.
- `cpuset`: Pin the process to the specified CPU cores using Linux cpuset notation.
  Accepts ranges (`0-7`), comma-separated lists (`0,2,4`), or mixed (`0-3,8-11`).

  Named aliases are resolved at runtime based on auto-detected topology:

  | Alias | Description |
  |-------|-------------|
  | `big-cores` | High-performance cores (Big + BigTurbo) |
  | `little-cores` | Efficiency cores (Little) |
  | `turbo-cores` | Highest-capacity cores only (BigTurbo) |
  | `performance-cores` | `turbo-cores` if available, otherwise `big-cores` |
  | `efficiency-cores` | Same as `little-cores` |
  | `all-cores` | All online CPUs |
  | `x3d-cache` | CPUs on the LLC(s) with the largest L3 cache (works on any CPU) |
  | `x3d-frequency` | AMD X3D high-frequency CCD (X3D only) |
  | `llc-N` | CPUs sharing LLC domain N (e.g., `llc-0`, `llc-1`) |
  | `node-N` | CPUs on NUMA node N (e.g., `node-0`, `node-1`) |

  On systems without heterogeneous cores, `big-cores` and `all-cores` resolve to the same set.
- `cgroup`: Put the process in the specified cgroup. This can be any cgroup, including those created outside `ananicy-cpp`.
- `type`: Set the type of the rule. All options defined in the type will be used as if written explicitly in the rule,
although you can override each option if needed.


[wikipedia:starvation]: https://en.wikipedia.org/wiki/Starvation_(computer_science)

### Types

To avoid repeating yourself, you can add types.
It must be defined in a `.types` file.

The syntax is the following:
~~~json
{"type": "my_type", "nice": 19, "other_parameter": "value"}
~~~

It can then be used in any rule by simply adding the `type` property to the rule.
For instance, `{"name": "gcc", "type": "compiler"}`

Parameters can be overridden, for instance:
~~~json
{"type": "compiler", "nice": 19, "sched": "batch", "ioclass": "idle"}
~~~

~~~json
{"name": "gcc", "type": "compiler", "ioclass": "none", "ionice": 0}
~~~

### Cgroups

They are defined in `.cgroups` files.

`cgroup_v1` are recommended for proper functionality (see issue #21), they can be forced by
passing `systemd.unified_cgroup_hierarchy=0` to your kernel cmdline (by editing `/etc/default/grub` for instance).

Only one attribute is supported, `CPUQuota`:

~~~json
{"cgroup": "cpu80", "CPUQuota": 80}
~~~

`CPUQuota` is a percentage of the **whole machine** (all CPUs together), from 0 to 100.
`80` lets the processes in the cgroup use 80 % of all CPU time. On 4 CPUs that is 3.2 CPUs
(`cpu.max` = `320000 100000`). This is different from systemd's `CPUQuota=`, where 100 %
means one CPU.

One major limitations of `cgroups` right now is that they can only be created by the `root` user.
You can _not_ use a `CAP_SYS_CGROUP` capability thing like for other things, because it simply does not exist yet.
If it is ever implemented in the Linux kernel, please create an issue.

### Community rules

#### From the original project

You can use the original Ananicy's rules by copying the `00-default`, `00-types.types` and `00-cgroups.cgroups` to your `ananicy.d` directory (by default, in `/etc`).

## Why rewriting ananicy?
I mostly used Ananicy on older computers to improve interactivity. However, having Ananicy use megabytes of RAM
and a decent amount of CPU time troubled me. Thus I decided to rewrite it in C++, using an event based approach.
RAM usage is much lower (only a few thousands of bytes !), and CPU usage is almost always zero thanks to its
event-based implementation.

## License

Ananicy Cpp — Copyright (C) 2021 Antoine Viallon and contributors.
Modifications in this fork — Copyright (C) 2026 WhitePeace36.

This is a modified fork of [ananicy-cpp](https://gitlab.com/ananicy-cpp/ananicy-cpp).
It is free software: you can redistribute it and/or modify it under the terms of the
GNU General Public License as published by the Free Software Foundation, either version 3
of the License, or (at your option) any later version (`GPL-3.0-or-later`). See [LICENSE](LICENSE).
The list of changes relative to upstream is the git history of this repository.

Bundled third-party code keeps its own license:

- `include/utility/atomic_queue/` — [atomic_queue](https://github.com/max0x7ba/atomic_queue),
  Copyright (c) 2019 Maxim Egorushkin, MIT License (see `include/utility/atomic_queue/LICENSE`).
