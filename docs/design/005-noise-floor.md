# 005 — Noise-floor meter

**Status:** Measured · **Updated:** 2026-10-18

## Proposal

Note 003's reviewed run showed that from p99.9 up, the load tail was the producer CPU being
stopped (for up to about 1 ms), not the system under test. Any queue measured on this machine
would show the same tail. Before a queue result is published, the machine's own floor has to be
measured, attributed to causes, and stated next to the result, and we need data to decide whether
boot-time isolation is required.

This component measures how often, and for how long, a thread spinning alone on a reserved CPU
is stopped; publishes that as every run's floor; and, separately, attributes the stops to their
causes with the kernel's own tracer.

It does:

- measure, from user space, the stops a benchmark thread actually sees on each CPU it uses;
- publish a `noise` block in every run's JSON, measured on the run's CPUs just before measuring;
- characterise all reserved CPUs, repeated, in a dedicated program (`noise_bench`);
- attribute stops to interrupts, softirqs, threads, NMIs and firmware with `rtla osnoise`, as a
  diagnostic run;
- give the evidence for the boot-time isolation decision.

It does not:

- change any setting (`bench-on`, or a boot change Shep makes);
- subtract the floor from any result. It is published beside it.

## Design

**Spinner.** A thread pinned to one CPU reads the TSC in a tight loop with `stamp()` (no fences:
the fastest read, about 10 ns per iteration). Any gap between consecutive reads longer than a
threshold is a stop: its start and length go into a pre-allocated array, its length into a note
002 histogram. Proposed threshold: **1 µs**, about 100 loop iterations, so loop jitter never
counts but a timer tick (typically 1–3 µs) does.

**Per CPU, published:** time stopped as a fraction, stop count per second, stops by class
(1–10 µs, 10–100 µs, 100 µs–1 ms, > 1 ms), longest stop, and the stop-length histogram. Also the
CPU's interrupt, softirq and involuntary-context-switch counts over the same window, from
`/proc/interrupts`, `/proc/softirqs` and `getrusage`: coarse causes, readable without root.

**In every run (S1).** Before measuring, each benchmark program spins for 2 s on each CPU it is
about to use and writes the `noise` block after `environment`. A result then carries the floor it
was measured on.

**`noise_bench`.** Every reserved CPU, 10 s each, repeated 5 times, so run-to-run spread is
visible (note 003's A/B found it as large as the effect being tested). About 35 minutes for 40
CPUs; or a chosen subset.

**Attribution (diagnostic, root).** `sudo rtla osnoise top -c <cpus> -d <duration>` runs the
kernel's osnoise tracer: on each CPU it reports noise and its sources (IRQ, softirq, thread, NMI,
and "hardware" noise the kernel cannot see, typically firmware System Management Interrupts).
Its output is saved beside the run, not parsed into the JSON. This is the only way to tell an SMI
from a kernel stop, and it needs root, so it stays outside the published path.

**The isolation decision.** With this kernel, boot-time isolation needs only boot parameters:
`CONFIG_NO_HZ_FULL` and `CONFIG_RCU_NOCB_CPU` are built in; `CONFIG_HZ=1000` means a busy CPU is
ticked 1,000 times a second, and note 003's 4,700 stops over 5 µs in 5 s on CPU 2 (about 940 a
second) point at the tick. The decision is made in this note's Review, comparing `noise_bench`
and `rtla osnoise` before and after `isolcpus=` `nohz_full=` `rcu_nocbs=` on the reserved CPUs.

**Output.**

```json
"noise": {
  "threshold_ns": 1000, "seconds_per_cpu": 2,
  "cpus": [ { "cpu": 2, "stopped_fraction": 0.0, "stops_per_s": 0,
              "by_class": { "1-10us": 0, "10-100us": 0, "100us-1ms": 0, "over_1ms": 0 },
              "longest_ns": 0, "iterations": 0, "iteration_min_ns": 0, "overflow": false,
              "interrupts": { "LOC": 0 }, "softirqs": { "TIMER": 0 }, "involuntary_switches": 0,
              "stop_lengths": { } } ]
}
```

Values shown are placeholders.

## Spec

- S1. Every run's JSON has a `noise` block for each CPU it measures on, taken before measuring.
- S2. The spinner's own loop never registers as a stop: on a CPU with nothing else running, a
  stop below the threshold is never recorded, and the loop iteration is published.
- S3. Stops are counted, not sampled: every gap above the threshold in the window is recorded,
  up to a stated capacity; overflowing it is reported, not silent.
- S4. A known injected stop (the spinner held in a timed busy-wait between two reads) is recorded
  with the right length, within 1 µs.
- S5. The spinner's loop makes no system call or allocation, checked in the compiled code.
- S6. `noise_bench` reports each CPU's figures for each repeat, and their spread.
- S7. Compiles for aarch64 with warnings as errors.

## Tasks

- [x] Spinner and stop recording, with S2–S5 tests
- [x] Coarse cause counters (interrupts, softirqs, context switches)
- [x] JSON `noise` block, added to the three benchmark programs
- [x] `noise_bench`
- [x] Reviewed runs: `noise_bench` and `rtla osnoise` under bench-on
- [ ] Isolation decision, from those runs; if taken, the same runs again after the boot change

## Review

| Date | Stage | Finding |
|---|---|---|
| 2026-10-18 | Draft | Written for review |
| 2026-10-18 | Agreed | Approved by Shep: 1 µs threshold, a 2 s noise sample per CPU in every run, `noise_bench` on all 40 reserved CPUs (10 s × 5 repeats), `rtla osnoise` output saved beside the run. S4's injected stop is a timed busy-wait, not a sleep: wake-up latency would break the 1 µs check |
| 2026-10-18 | Built (partial) | Spinner (`noise.hpp`): `spin_window` reads with `stamp()` and records every gap above the threshold into a pre-faulted array and a histogram; counts carry on past capacity. Tests: shortest gap 5.5 ns against a 1 µs threshold (S2); all 20 injected 100 µs stops found, all within 1 µs (S4); capacity 4 with 20 stops: all counted, overflow reported (S3). S5 checked in the compiled code: the loop has TSC reads and no call; fails on a hook that calls `clock_gettime`. The first test run was invalid: Claude's stray test threads were spinning on CPUs 2 and 3 (52% stopped, 3.4 ms longest); after removing them, CPU 2 unfenced: 0.9% stopped, longest 460 µs, 570 stops in 0.5 s |
| 2026-10-18 | Built (partial) | Coarse causes: the measured CPU's column of `/proc/interrupts` and `/proc/softirqs` (non-zero types), and involuntary switches, read by the spinning thread before and after the window, outside the loop. First sample, CPU 2 unfenced, 0.5 s: 524 stops, 501 local timer interrupts (LOC), 6 involuntary switches: nearly every stop above 1 µs is a timer tick at `CONFIG_HZ=1000`. One sample, not bench-on |
| 2026-10-18 | Built (partial) | JSON block (`noise_block.hpp`) in all three programs, between `environment` and `clock`. Sampled CPUs: those whose percentiles are published (CPU 2 for clock and histogram, 2 and 3 for load); not the cross-CPU partners, whose bounds are minimums and would add 3 minutes per run. Taken first; threshold set from a quick 150 ms calibration, figures converted with the clock block's. Classes come from the stop-length histogram (at most 0.2% boundary error). Fields as built: `stops`, `iterations`, `iteration_min_ns`, `overflow`, per-type `interrupts` and `softirqs`, `stop_lengths`. Smoke run, unfenced, CPU 2, 2 s: 1.0% stopped, 2,439 stops (1,983 under 10 µs, 438 to 100 µs, 18 to 1 ms), 2,007 LOC, longest 722 µs |
| 2026-10-18 | Built | `bench/noise_bench`: environment, clock, then every reserved CPU (read from the live fence) for 10 s, 5 repeats, each repeat sweeping all CPUs so a machine-wide change shows as repeat-to-repeat spread; per-repeat noise blocks and a per-CPU min/max summary (S6). Event capacity 100,000 per sample (≈ 12,000 expected), overflow reported. Optional CPU list, seconds and repeats for smoke runs. Smoke run, unfenced, CPUs 2 and 4, 0.5 s × 2: 0.78–0.92% stopped, 1,050–1,116 stops/s, longest 459–496 µs, 1,004–1,006 timer interrupts/s. All tasks but the reviewed runs and the isolation decision done |
| 2026-10-18 | Measured | Reviewed runs: `results/noise-dev-lnx01-20261018T222021Z.json` (commit 7af484d, clean, every bench-mode condition met; 40 reserved CPUs × 10 s × 5 repeats, 200 samples, no overflow) and `results/osnoise-dev-lnx01-20261018T225348Z.txt` (`rtla osnoise top -c 2-21,46-65 -d 5m -q`). Per reserved CPU: 0.66–0.74% stopped; ~1,018 stops/s; LOC ~1,010/s; 99.1% of stops 1–10 µs, 0.1% 10–100 µs, 0.8% (≈ 8/s, typically ~400 µs) 100 µs–1 ms, none over 1 ms; longest 254–624 µs; involuntary switches 0.4/s, NMI 0.13/s, softirqs ~10/s. Uniform across CPUs; repeat 5 lower (median 0.66% against 0.73%). rtla: ~1.3% noise, ~1,000 IRQ/s (the tick), 36 NMI, 75 thread events per CPU in 299 s, ~377 'HW' (no OS cause) noise events/s, and a max single noise of 425–429 µs on every CPU: a machine-wide stall, the signature of an SMI. Reading: the short stops are the tick, which `nohz_full` would remove. The ~400 µs stops at ~8/s match note 003's load tail (≈ 0.3% of messages at 1M/s, i.e. p99.9 ≈ 160 µs); if they are SMIs, boot isolation will not remove them, and HPE firmware power/monitoring settings are the lever. Next: count SMIs (MSR_SMI_COUNT via turbostat) before the isolation decision. Spec: S1–S7 pass |
| 2026-10-18 | Measured | SMIs confirmed: `turbostat --show SMI --interval 10` under bench-on read 7,128, the sum of MSR_SMI_COUNT over 88 logical CPUs; one SMI stops every CPU, so 81 SMIs in 10 s, ≈ 8/s, the rate of the ~400 µs stops. The long stops are HPE firmware, not the kernel; boot isolation cannot remove them. HPE's low-latency guidance for ProLiant lists BIOS (RBSU) options that stop periodic SMIs for processor power and utilisation monitoring and for memory pre-failure notification; on Gen9 the monitoring option is reported to sit in a hidden Service Options menu (Ctrl+A in RBSU). Exact labels to be confirmed on this ROM (P89, 08/29/2024). Decision with Shep pending: BIOS change first, then `noise_bench` and the SMI count again, then the isolation decision |
| 2026-10-18 | Measured | BIOS change by Shep (RBSU, reboot at 23:41): periodic-SMI sources disabled. SMI count under bench-on afterwards: 0 in 10 s (was 81). Exact settings changed to be recorded here. Next: `noise_bench` and `rtla osnoise` again, then the isolation decision |
| 2026-10-19 | Measured | After the BIOS change: `results/noise-dev-lnx01-20261019T091800Z.json` (commit 1490985, clean, all conditions met, 200 samples, no overflow) and `results/osnoise-dev-lnx01-20261019T095127Z.txt`. Stopped 0.43–0.45% per CPU (was 0.66–0.74%); 100 µs–1 ms stops 0.1/s (was 8.2/s); typical longest per sample 116 µs (was 384), one outlier 510 µs; ticks unchanged at ~1,006/s. rtla: noise 0.67% (was 1.27%), max single noise 23–86 µs (was 425–429 on every CPU). rtla still reports ~175 'HW' events/s per CPU with SMIs at zero; suspected sibling interference (rtla spins on both hyper-threads of each core at once; noise_bench samples one CPU at a time), unverified. The floor is now the 1 kHz tick: 99.9% of stops, ~4.4 µs each. At 1M msg/s it delays ~0.5% of messages by a few µs, which would dominate a queue's p99.9. Proposed to Shep: `load_bench` now as the post-BIOS baseline, then `nohz_full`, `rcu_nocbs`, `isolcpus=managed_irq,domain` on 2–21,46–65, then `noise_bench` and `load_bench` again |
| 2026-10-19 | Measured | BIOS settings changed by Shep: Processor Power and Utilization Monitoring → Disabled; Memory Pre-Failure Notification → Disabled. Power Profile left at Custom (the OS controls C-states; `bench-on` disables the deep ones at run time, as `environment` records) |
