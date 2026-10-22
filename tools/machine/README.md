# Machine setup

How the benchmark machine (HPE ProLiant DL380 Gen9, 2 × Xeon E5-2699 v4, Ubuntu 26.04) was set up
for every published run. A record of this machine, not a portable installer: the CPU lists are
its own (node 0's cores 2–21 and their hyper-thread siblings 46–65 reserved, the rest
housekeeping). Every run's JSON records the conditions these produce, and a run to `results/` is
refused unless they hold (design note 004).

| File | Installed at | What it does |
|---|---|---|
| [`99-bench-isolation.cfg`](99-bench-isolation.cfg) | `/etc/default/grub.d/` | Boot-time isolation of the reserved CPUs: `nohz_full`, `rcu_nocbs`, `isolcpus=managed_irq,domain` (note 005). Then `update-grub` and reboot |
| [`bench-on`](bench-on) | `/usr/local/bin/` | Performance governor, every idle state left available on the reserved CPUs and housekeeping CPUs capped at C1 (see below), uncore clock pinned at its maximum, services and user programs fenced off the reserved CPUs (systemd `AllowedCPUs`), unbound kernel work, interrupts and kernel threads moved off them, automatic NUMA balancing off, `perf_event_paranoid` 2 (a benchmark may count its own CPU events). Saves what it changes, before changing it |
| [`bench-off`](bench-off) | `/usr/local/bin/` | Restores what `bench-on` saved, idle states included (until 2026-10-22 it enabled every idle state, C3 too, which the kernel disables at boot on this CPU) |
| [`bench-run`](bench-run) | `/usr/local/bin/` | Starts a benchmark outside the fence, as the calling user, so it can pin to the reserved CPUs |

A measured run:

```sh
sudo sh -c 'bench-on && bench-run build/spsc_bench; bench-off'
```

**BIOS** (no script): Processor Power and Utilization Monitoring and Memory Pre-Failure
Notification disabled. Both cause periodic firmware SMIs (~400 µs each, ~8 a second), which were
the load generator's tail before they were removed (note 005).

**Known hazard.** `bench-on` saves the current settings unconditionally. Run twice without
`bench-off` between (an interrupted run, then a new one), it saves the fenced settings as the
originals, and `bench-off` then restores the fence. A reboot restores the kernel defaults. A guard
at the top of `bench-on` (refuse if `/run/bench.saved` exists) would prevent it.

**Changed 2026-10-21: C1 kept.** Until then `bench-on` ran `cpupower idle-set -D 2`. C1's exit
latency is exactly 2 µs, so C1 was disabled too and only POLL remained: every idle reserved CPU,
the measured CPUs' hyper-thread siblings included, spun in the kernel's poll loop and took a share
of the core. A compute-bound benchmark (the FIX codec in `fix-order-gateway`) ran about 2× slower;
the SPSC queue's latency, bound by cache-line transfers, did not move; the MPMC figures moved by up
to ~15%. Now `-D 3`: C1 (halt) stays, deeper states stay off. The measured CPUs never go idle, so
C1's exit latency does not reach them. Recorded in `lockfree-ipc` notes 004 and 005.

**Changed 2026-10-22: deep idle states left on for the reserved CPUs.** Housekeeping CPUs stay
capped at C1. The reason first given, that the C1 cap made a compute-bound thread's branch
prediction slow, was wrong and is withdrawn: the FIX codec's two speeds reproduce with no
`bench-on` setting applied, and come from the branch predictor's two stable states for that code
(`fix-order-gateway` note 009, Review 2026-10-22). The change stays because it is harmless: the
measured threads spin and never idle, so deep states add no wake-up latency to them. No published
figure depends on it. Recorded in `lockfree-ipc` note 004.
