# 004 — Environment capture

**Status:** Measured · **Updated:** 2026-10-18

## Proposal

A latency figure is only reproducible with the conditions it was measured under: hardware,
firmware, kernel, CPU and memory placement, power settings, the code that ran. Today those live
in the Review tables, typed by hand. This component reads them from the machine at run time and
writes them into every run's JSON, so a result carries its own conditions and they cannot drift
from what actually ran.

It also checks that a run meant for `results/` really was taken in bench mode, and refuses one
that was not, the way note 001 refuses a bad clock.

It does:

- read hardware, firmware, kernel, NUMA, power and placement settings from `/proc` and `/sys`;
- record the git commit the binary was built from, and whether the tracked tree was clean;
- check the bench-mode conditions and refuse a published run that fails any of them;
- write an `environment` block before any measurement.

It does not:

- change any setting (that is `bench-on`'s job);
- capture anything during measurement;
- judge whether the machine is quiet enough. That is the noise-floor meter (note 005).

## Design

**Fields**, all read at run time. A value that cannot be read is `null`, never guessed or left out.

| Group | Fields | Source |
|---|---|---|
| Machine | vendor, product, BIOS version and date | `/sys/class/dmi/id/` |
| CPU | model, microcode, sockets, cores per socket, threads per core, online CPUs | `/proc/cpuinfo`, `/sys/devices/system/cpu/` |
| NUMA | each node's CPU list and memory | `/sys/devices/system/node/` |
| Kernel | release, version, command line, transparent huge pages, automatic NUMA balancing, CPU vulnerability mitigations | `/proc/sys/kernel/osrelease` and `version`, `/proc/cmdline`, `/sys/kernel/mm/`, `/sys/devices/system/cpu/vulnerabilities/` |
| Power | governor and turbo per reserved CPU, idle states enabled, uncore min/max per socket | `cpufreq`, `intel_pstate`, `cpuidle`, `intel_uncore_frequency` |
| Placement | fence (`AllowedCPUs` of the system and user slices), the run's own cgroup and CPU affinity | `/sys/fs/cgroup/`, `/proc/self/` |
| Build | git commit, tracked tree clean, compiler and version, build type and flags | embedded at build time; see below |
| Run | program, arguments, UTC start time | the process |

Mitigations are included because some (retpolines, IBRS) change the cost of system calls and
indirect branches, and the kernel's `old_microcode` check is reported this way.

**Build provenance.** The commit and a clean/dirty flag are embedded by CMake at build time from
`git rev-parse HEAD` and `git status --porcelain --untracked-files=no` (untracked files, such as a
new results file, do not count). CMake re-reads them on every build, so a rebuild after a commit
updates them. Reading git at run time instead would describe the tree, not the binary.

**Bench-mode check.** A published run requires all of:

1. governor `performance` on every reserved CPU;
2. idle states deeper than C1 disabled on every reserved CPU;
3. uncore minimum equal to maximum on both sockets;
4. the fence in place: system and user slices limited to the housekeeping CPUs;
5. the run itself in `bench.slice` (started by `bench-run`), with affinity covering its CPUs;
6. a clean tracked tree at build time;
7. automatic NUMA balancing off (note 005: it stops a thread for ~0.6 ms at a time).

Any failure refuses the run: `ok: false` and a reason naming every failed condition, written
after the `environment` block, and no measurement is made. A run written somewhere other than
`results/` (a smoke test) is not refused, but is marked `published: false`.

**Testability.** Every path is read under a root prefix, `/` by default, so tests run the parser
and every refusal against a fake `/proc` and `/sys` tree.

**Output.** An `environment` block first in each run's JSON, before `clock`:

```json
"environment": {
  "published": true, "ok": true,
  "bench_mode": { "ok": true, "failed": [] },
  "machine": { "vendor": "HP", "product": "ProLiant DL380 Gen9", "bios": "P89", "bios_date": "08/29/2024" },
  "cpu": { "model": "...", "microcode": "0xb000041", "sockets": 2, "cores_per_socket": 22,
           "threads_per_core": 2, "online": "0-87" },
  "numa": [ { "node": 0, "cpus": "0-21,44-65", "memory_mb": 0 } ],
  "kernel": { "release": "...", "version": "...", "cmdline": "...", "thp": "madvise", "numa_balancing": "0",
              "vulnerabilities": { "spectre_v2": "..." } },
  "power": { "governor": "performance", "turbo": true, "idle_states_enabled": ["POLL", "C1"],
             "uncore_khz": [ { "socket": 0, "min": 2800000, "max": 2800000 } ] },
  "placement": { "fence": "0-1,22-45,66-87", "cgroup": "/bench.slice/...", "affinity": "0-87" },
  "build": { "commit": "...", "clean": true, "compiler": "gcc 15.2.0", "build_type": "Release",
             "flags": "..." },
  "run": { "program": "load_bench", "args": [], "start_utc": "2026-10-18T21:38:58Z" }
}
```

Values shown are placeholders. A refused run adds `reason` after `ok`, naming every failed
condition. `bench_mode` is written on every run, published or not, so a smoke run shows what
was missing.

## Spec

- S1. Every run's JSON starts with an `environment` block containing all the fields above.
- S2. Every value is read from the system at run time, or embedded at build time for `build`;
  an unreadable value is `null`.
- S3. A run written to `results/` that fails any bench-mode condition refuses, names every failed
  condition, and makes no measurement.
- S4. The build commit and clean flag describe the binary, not the tree at run time.
- S5. The parser and every refusal are tested against a fake `/proc` and `/sys` tree.
- S6. Compiles for aarch64 with warnings as errors; fields that do not exist there are `null`.

## Tasks

- [x] Reader under a root prefix, and the fake tree for tests
- [x] Build provenance from CMake
- [x] Bench-mode check and refusal
- [x] JSON `environment` block, added to `clock_bench`, `histogram_bench`, `load_bench`
- [x] Tests S1–S6
- [x] A run under bench-on and one without; results reviewed here

## Review

| Date | Stage | Finding |
|---|---|---|
| 2026-10-18 | Draft | Written for review |
| 2026-10-18 | Agreed | Approved by Shep: refuse unpublished-condition runs to `results/`, refuse a dirty build, provenance at build time. Kernel release and version read from `/proc/sys/kernel/` rather than `uname()`, so tests can fake them |
| 2026-10-18 | Built | `environment.hpp` (reader under a root prefix, bench-mode check, block), `run_file.hpp` (shared by the three benchmark programs: run file name, environment first, refusal, write), `cmake/build_info.cmake` (commit and clean flag at every build, rewritten only on change). Added to the agreed fields: `bench_mode` (ok and the failed list) on every run, so an unpublished run shows what was missing. Fake-tree test: every field read; a bench-mode tree passes; each condition broken alone is named alone; idle states on housekeeping CPUs are not checked; an empty tree reads as nulls and fails. Live, outside bench mode: a run to `results/` refuses in 21 ms with all nine failures named and nothing measured; a run elsewhere goes ahead, `published: false`. `check_environment_json.py` checks exact fields, refused-iff-published-and-failing, and that a refused file has nothing after the block. A fresh build directory builds and passes (the generated header is ordered before its users). Compiles for aarch64; clean under TSan and ASan/UBSan |
| 2026-10-18 | Measured | Without bench mode: `clock_bench` to `results/` refused in 21 ms, all nine failures named, nothing measured (file deleted). Under bench-on with `bench-run`: `results/clock-dev-lnx01-20261018T215708Z.json`, published, every condition met; reserved CPUs derived as 2–21,46–65; cgroup `bench.slice`; commit a277b03, clean. Only `POLL` is enabled on reserved CPUs: `bench-on` disables C1 too, stricter than assumed. Finding from the block: clock overhead max 3.0 ms on CPU 1 (5–254 µs before the fence). CPU 1 is housekeeping, so the fence now crowds every service, kernel thread and interrupt onto it; notes 001 and 002 measure on CPU 1. Spec: S1–S6 pass |
| 2026-10-18 | Measured | Correction: from about 21:18 to 22:09 a deadlocked test process of Claude's (an ASan `load_test` built before the `closed_loop` fix, plus a TSan run left by a `timeout` that did not take effect) kept two threads spinning, pinned to CPUs 2 and 3. Under bench-on the fence moved them onto the housekeeping CPUs. So the 3.0 ms overhead max on CPU 1 at 21:57 coincided with two stray spinning threads on the housekeeping CPUs, and its cause is not established; the earlier rows attributing it to the fence's housekeeping load overstate the evidence. The move to CPU 2 stands on design grounds: the fence concentrates displaced load on housekeeping CPUs, so measurements belong on reserved ones. Runs on reserved CPUs in that window (load 21:38, clock and histogram 22:02) were away from the stray threads and stand. Leftover processes are now checked before every measured run |
| 2026-10-19 | Measured | Amended with Shep: `kernel.numa_balancing` recorded, and a seventh bench-mode condition requires it off (note 005 traced the rare long stops to it). `bench-on` saves and clears it; `bench-off` restores it through the existing saved-settings file. Tests: on, and unreadable, each refuse alone |
