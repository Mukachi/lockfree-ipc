# 005 — Noise-floor meter

**Status:** Agreed · **Updated:** 2026-10-18

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
              "longest_ns": 0, "interrupts": 0, "softirqs": 0, "involuntary_switches": 0,
              "histogram": { } } ]
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
- [ ] Coarse cause counters (interrupts, softirqs, context switches)
- [ ] JSON `noise` block, added to the three benchmark programs
- [ ] `noise_bench`
- [ ] Reviewed runs: `noise_bench` and `rtla osnoise` under bench-on
- [ ] Isolation decision, from those runs; if taken, the same runs again after the boot change

## Review

| Date | Stage | Finding |
|---|---|---|
| 2026-10-18 | Draft | Written for review |
| 2026-10-18 | Agreed | Approved by Shep: 1 µs threshold, a 2 s noise sample per CPU in every run, `noise_bench` on all 40 reserved CPUs (10 s × 5 repeats), `rtla osnoise` output saved beside the run. S4's injected stop is a timed busy-wait, not a sleep: wake-up latency would break the 1 µs check |
| 2026-10-18 | Built (partial) | Spinner (`noise.hpp`): `spin_window` reads with `stamp()` and records every gap above the threshold into a pre-faulted array and a histogram; counts carry on past capacity. Tests: shortest gap 5.5 ns against a 1 µs threshold (S2); all 20 injected 100 µs stops found, all within 1 µs (S4); capacity 4 with 20 stops: all counted, overflow reported (S3). S5 checked in the compiled code: the loop has TSC reads and no call; fails on a hook that calls `clock_gettime`. The first test run was invalid: Claude's stray test threads were spinning on CPUs 2 and 3 (52% stopped, 3.4 ms longest); after removing them, CPU 2 unfenced: 0.9% stopped, longest 460 µs, 570 stops in 0.5 s |
