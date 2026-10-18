# 003 — Open-loop load generator

**Status:** Agreed · **Updated:** 2026-10-18

## Proposal

A queue's latency only means something at a stated offered load, and only if the load generator
does not wait for the system it is measuring. A closed-loop generator (send, wait for the reply,
send the next) stops sending while the system stalls, so the stall delays one message instead of
every message that should have been sent during it. The tail then looks better than it is. This
is coordinated omission.

The generator here is open-loop: the send schedule is fixed before the run, and each message's
latency is measured from when it *should* have been sent, not from when it was.

It does:

- send at a fixed offered rate, from a schedule computed before the run;
- stamp each message with its intended send time;
- record latency into a note 002 histogram, one per receiving thread;
- measure its own lateness against the schedule, and refuse a run where it could not keep up;
- run a sweep of rates, giving latency percentiles against offered load.

It does not:

- implement any queue. It drives a system under test (SUT) through two calls, `send` and
  `try_receive`;
- handle several producers or consumers yet (one of each, on given CPUs);
- go over a network.

## Design

**The clock question.** A one-way latency is the consumer's TSC read minus the
producer's, on different CPUs. Note 001 shows the TSCs agree, but it can only bound the offset to
the cache-line round trip: about ±100 ns on one socket, ±200 ns across. That is the size of the
latencies being measured. Intel TSCs on one board are in fact driven from one clock and synced by
firmware, and the kernel checks it at boot, but our own evidence does not go below the bound.

| Option | Measures | Clock dependence | Cost |
|---|---|---|---|
| A. One-way only | consumer stamp − intended send time | relies on TSC sync below what note 001 proves | simplest |
| B. Round trip only | the consumer echoes each message back on a second SUT instance; producer stamp − intended send time | one CPU's TSC, no sync assumption | measures two hops; halving it assumes symmetry |
| **C. Both** | one-way and round trip, published side by side | one-way's offset bound stated next to it | two queues per run |

Chosen: **C**. The round trip is the number that needs no clock assumption; the one-way is the
number readers expect, published with note 001's bound for that CPU pair beside it.

**Schedule.** Intended send times are `t_i = t_0 + i·T` in ticks, for an offered rate of 1/T.
Constant spacing, not Poisson: constant spacing makes the generator's lateness easy to read, and
Poisson arrivals can be added later as a second schedule. Computed before the run, into a
pre-faulted array.

**Producer.** For each message: spin on `start()` until `t_i`, then send `{sequence, t_i}`. If it
is already past `t_i`, it sends at once and does not skip: the late message's latency still
counts from `t_i`. Its lateness (send time − `t_i`) goes into a separate histogram.

**Consumer.** Spins on `try_receive`, stamps each message with `stop()`, and records
`stamp − t_i`. In option C, it also echoes the message back for the round trip.

**Refusal.** If the producer's lateness p99 exceeds T (it fell more than one interval behind for
over 1% of messages), the generator could not sustain the rate and the run refuses: the latencies
would partly measure the generator.

**Warm-up.** The first 10% of messages, and at least 100 ms, are sent and discarded.

**Validation against a known SUT.** A test SUT with a fixed injected delay, and one that stalls
for 1 ms every 100 ms. The stall test must show the stall across the tail, which a closed-loop
generator would hide; the test runs both to prove the difference.

**Output.** A `load` block per rate in the run's JSON, after the `clock` block: offered rate,
messages, warm-up discarded, producer and consumer CPUs, the lateness histogram, the latency
histogram(s), and for one-way the offset bound for that CPU pair from the clock block.

## Spec

- S1. Latency is measured from the intended send time, never the actual one.
- S2. A late producer never skips a message.
- S3. A run whose producer lateness p99 exceeds the send interval refuses, with the reason.
- S4. On the stall SUT, the open-loop generator's p99.9 reflects the stall (≥ 0.5 ms), and a
  closed-loop run on the same SUT does not. Both are in the test output.
- S5. On the fixed-delay SUT, every recorded latency is ≥ the injected delay.
- S6. The hot path (producer spin and send, consumer receive and record) makes no allocation and no
  system call, checked in the compiled code as in note 001.
- S7. Each one-way histogram is published with the clock block's offset bound for its CPU pair.
- S8. Compiles for aarch64 with warnings as errors.

## Tasks

- [x] Schedule and producer/consumer loops, with a test SUT
- [x] Lateness histogram and refusal
- [x] Round trip
- [ ] JSON `load` block
- [ ] Tests S1–S8, including the stall and fixed-delay SUTs
- [ ] First run under bench-on; results reviewed here

## Review

| Date | Stage | Finding |
|---|---|---|
| 2026-10-18 | Draft | Written for review |
| 2026-10-18 | Agreed | Approved by Shep: option C (one-way and round trip side by side), constant spacing, refusal at lateness p99 > T, warm-up 10% and at least 100 ms |
| 2026-10-18 | Built (partial) | Schedule and loops (`load.hpp`), test SUTs (`tests/test_suts.hpp`: one-slot mailbox, fixed delay, receive stall, send stall). Tests pass, also under TSan and ASan/UBSan, and compile for aarch64: every message once and in order; a 5 µs injected delay floors every latency (S5); a 1 ms producer hold shows in latency counted from the intended time, and nothing is skipped (S1, S2). Finding, not bench-on: producer lateness p99 ≈ 12 µs and p99.9 ≈ 340 µs at both 100k/s and 1M/s and on two CPU pairs, so not the generator's speed. A thread spinning alone on CPU 2 is stopped 0.6% of the time (822 gaps > 5 µs in 5 s, longest 0.5 ms, 34 involuntary context switches); CPU 30 the same. The CPUs are not isolated (no `isolcpus`/`nohz_full`), and user tasks run on them. Under S3 every run at 1M/s would refuse. The open-loop design is doing its job: it counts every message due during a stop |
| 2026-10-18 | Built (partial) | CPU fence agreed with Shep (option A): `bench-on` now limits system.slice, user.slice and init.scope to housekeeping CPUs 0–1, 22–45, 66–87 via `AllowedCPUs`, and moves the unbound workqueue mask and every movable interrupt there; benchmarks run outside the fence with `sudo bench-run`. Reserved: 2–21 and siblings 46–65. Spinning on CPU 2: stopped 0.28% of time (was 0.60%), involuntary switches 7 in 5 s (was 34), longest 252 µs (was 496), gaps > 5 µs 742 (was 822). Producer lateness through `bench-run`: p99 104–108 ns at 100k/s and 1M/s on CPUs 2,3 and 4,5 (was ≈ 12 µs); late by > T 0.21% at 100k/s, 0.68–0.76% at 1M/s. S3 now passes, with thin margin at 1M/s. p99.9 still 130–150 µs: per-CPU kernel work and the timer tick, which only boot-time isolation removes. Still reachable and movable: unbound kernel threads (kthreadd, RCU, khugepaged, ksmd, kipmi) |
| 2026-10-18 | Built (partial) | Kernel threads (kthreadd and children) also moved to housekeeping by `bench-on`, restored by `bench-off`. A/B on CPU 2, fence on, 5 s each: threads moved, 4,627 / 4,717 gaps > 5 µs, 0.74% / 0.77% stopped, 6 switches, longest ≤ 258 µs; threads on all CPUs, 742 and 577 gaps, 0.28% and 0.56% stopped, 7 and 44 switches, longest 252 and 722 µs. Moving them removes the long stops and adds many short ones (cause unverified). Run-to-run spread with the same settings is as large as the effect. Producer lateness the same in both; S3 passes in both. Kept, decided with Shep, for the shorter worst case. Characterising this properly, and whether boot-time isolation is needed, belongs to the noise-floor meter |
| 2026-10-18 | Built (partial) | Refusal built (`evaluate_lateness`, S3): tested on synthetic lateness (0.5% late accepted, 2% refused with p99 and T in the reason, too few to judge refused). Outside bench mode every live test run now refuses for lateness (p99 11–126 µs against T of 1–10 µs), as expected on an unfenced machine; the tests check message correctness separately and accept a lateness refusal as a finished run. Histograms are filled even when a run refuses |
| 2026-10-18 | Built (partial) | Round trip built, as agreed with Shep: the consumer echoes each message on a second SUT; the producer takes echoes while it waits (and while a send is refused, so neither side can block the other) and records arrival − intended on its own TSC. One-way and round trip come from the same run; the echo work is in both. Tests: every message measured both ways, in order; with a 5 µs delay each way every round trip is ≥ 10 µs and every one-way ≥ 5 µs. Not bench-on, CPUs 2→3: round trip p50 389 ns, one-way 195 ns, almost exactly half; with symmetric paths that puts the 2/3 TSC offset far inside note 001's ±100 ns bound (supporting evidence, not proof). Under TSan the run falls ≈ 200 ms behind and both p50s land in one bucket, so the test checks round trip ≥ one-way, not >. Clean under TSan and ASan/UBSan, compiles for aarch64 |
