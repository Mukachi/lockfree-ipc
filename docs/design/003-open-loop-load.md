# 003 — Open-loop load generator

**Status:** Measured · **Updated:** 2026-10-18

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

Chosen: **C**, amended 2026-10-19: both are published, but from **separate runs** at each rate,
one-way only and with echo. In the same run, the echo traffic slowed the one-way path by ~20–50 ns
at p50 (note 006, Review); each block now carries one `mode`. The round trip is the number that needs no clock assumption; the one-way is the
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
histogram(s), and for one-way the offset bound for that CPU pair. The clock block only bounds
CPU 0 against each CPU; combining two of those would double the width, so the pair is measured
directly with note 001's method.

## Spec

- S1. Latency is measured from the intended send time, never the actual one.
- S2. A late producer never skips a message.
- S3. A run whose producer lateness p99 exceeds the send interval refuses, with the reason.
- S4. On the stall SUT, the open-loop generator's p99.9 reflects the stall (≥ 0.5 ms), and a
  closed-loop run on the same SUT does not. Both are in the test output.
- S5. On the fixed-delay SUT, every recorded latency is ≥ the injected delay.
- S6. The hot path (producer spin and send, consumer receive and record) makes no allocation and no
  system call, checked in the compiled code as in note 001.
- S7. Each one-way histogram is published with the offset bound for its CPU pair, measured
  directly with note 001's method.
- S8. Compiles for aarch64 with warnings as errors.

## Tasks

- [x] Schedule and producer/consumer loops, with a test SUT
- [x] Lateness histogram and refusal
- [x] Round trip
- [x] JSON `load` block
- [x] Tests S1–S8, including the stall and fixed-delay SUTs
- [x] First run under bench-on; results reviewed here

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
| 2026-10-18 | Built (partial) | JSON block (`load_block.hpp`) and `bench/load_bench`: clock flow, the 2→3 pair measured directly, then 100k, 250k, 500k, 1M/s for 10 s of messages each against the one-slot mailbox, echoed. S7 reworded: the clock block has no 2→3 bound, and combining two CPU-0 bounds would double it. A refused rate keeps its reason and the lateness histogram that refused it, and no latency results. `check_load_json.py` checks both kinds on a synthetic file. Smoke run, unfenced: 49 s, all four rates refused for lateness (p99 11–13 µs), exit 1, file complete; pair bound −81 to +113 ns |
| 2026-10-18 | Built | S4: a test wrapper (`closed_loop`) makes any SUT closed-loop: one message in flight, stamped with its actual send time. Same stalling SUT (1 ms every 100 ms), 1M/s, not bench-on: open-loop p99 333 µs, p99.9 923 µs; closed-loop p99 246 ns, p99.9 265 ns. Both see the stall once, in the max (≈ 1 ms); the closed-loop run's own lateness (p99 367 µs) is the tell. S6 checked in the compiled code: each generator thread body, both variants, calls only `sched_setaffinity` (pinning, before the loop) and `__stack_chk_fail`; SUT, histogram and clock reads are inlined. Fails on a SUT that calls `clock_gettime`, in exactly the threads that call it. TSan found a deadlock in `closed_loop`: it set its in-flight flag after making the message visible, so a fast consumer could clear it first. Natively the window is nanoseconds and the test passed by luck; under TSan it hung every time. Fixed by setting the flag first. Every task but the reviewed run is done |
| 2026-10-18 | Measured | Reviewed run: `results/load-dev-lnx01-20261018T213858Z.json`. dev-lnx01, bench-on with CPU fence and kernel threads moved, `bench-run`, Release build, one-slot mailbox echoed on a second, producer CPU 2, consumer CPU 3, 10 s of messages per rate, warm-up 10% and at least 100 ms discarded. Pair 2/3 offset bound −92.8 to +84.2 ns (100,000 round trips, converged). 100k/s: one-way p50 223 ns, p99 241, p99.9 163 µs; round trip p50 433 ns, p99 1.16 µs. 250k/s: one-way 223 / 240 ns, p99.9 168 µs; round trip 385 ns / 1.15 µs. 500k/s: one-way 223 / 253 ns, p99.9 188 µs; round trip 409 ns / 1.57 µs. 1M/s refused: lateness p99 1,132 ns > T 1,000 ns. No message out of order; no negative latency. From p99.9 up, lateness, one-way and round trip agree to within 1% at every rate: the tail is the producer being stopped, counted open-loop, not the SUT. On this setup a queue's tail above p99 would measure the machine: the noise-floor meter must measure that floor before queue results are published, and boot-time isolation may be needed. Open: round-trip p99 (1.15–1.57 µs) far above one-way p99 (≈ 240 ns); suspected the producer's echo pick-up, unverified. Clock block: 19 of 87 pairs unconverged; gains show near-misses (a few ticks) and the two-speed effect (200–280 ticks). Spec: S1–S8 pass; S3 confirmed live by the 1M/s refusal |
| 2026-10-18 | Measured | Check after the fact: during the reviewed run (21:38) two stray test threads of Claude's were spinning, displaced by the fence onto the housekeeping CPUs. Producer and consumer ran on reserved CPUs 2 and 3, away from them; the result stands. See note 001's correction row |
| 2026-10-19 | Measured | Post-BIOS baseline, before boot isolation: `results/load-dev-lnx01-20261019T102427Z.json` (commit 1490985, clean, all conditions met). One-way p50 199 ns, p99 213 ns, p99.9 ≈ 4.1 µs at 100k–1M/s (was 163–188 µs); p99.99 6.5 µs at 100k/s, 77–181 µs at 250–500k/s, 1.9 ms at 1M/s; max 0.5–2.7 ms. 1M/s now publishes. Round trip p99 428–473 ns at 100–500k/s (was 1.15–1.57 µs): the open round-trip p99 item was the SMIs. p50 down 24 ns (223 → 199), cause unverified. p99.9 is now the tick; the rare long stops (≈ 0.1/s, up to ~2.6 ms here) remain, cause unknown |
| 2026-10-19 | Measured | With SMIs off and boot isolation: `results/load-dev-lnx01-20261019T113214Z.json`. One-way p50 214–222 ns, p99 234–253, p99.9 237 / 238 / 255 ns at 100k / 250k / 500k/s and 1.4 µs at 1M/s; p99.99 403 ns at 100k/s, 96 µs–2.2 ms above. The tail up to p99.9 is now the SUT, not the machine. Note 005 has the floor and the open items |
| 2026-10-19 | Measured | Final, SMIs off, boot isolation, NUMA balancing off: `results/load-dev-lnx01-20261019T124603Z.json`. One-way p50 216–225 ns, p99 238–278, p99.9 241–283, p99.99 243–286, p99.999 255–357 ns; max 0.43–0.45 µs at 100k–500k/s, 1.07 µs at 1M/s. Round trip p99.9 467–539 ns (2.1–2.4 µs before: the open round-trip item was NUMA balancing). The whole distribution is now the SUT; the run's own noise samples recorded zero stops |
| 2026-10-19 | Measured | Amended with Shep: one-way and round trip measured in separate runs at each rate (note 006 found the echo inflates the one-way figure by ~20–50 ns at p50, up to ~100 at p99). `write_load_block` writes one mode per block, with a `mode` field; an echo run's one-way figure is never written. `load_bench` and `spsc_bench` run each rate twice. Earlier result files keep their combined runs; their round trips stand, their one-way figures were measured with echo on |
| 2026-10-19 | Measured | Mailbox with one-way and round trip separate: `results/load-dev-lnx01-20261019T174123Z.json` (commit bda41f5). One-way p50 148–160 ns, p99 168–173, p99.9 174–178, p99.99 211–223 (with echo in the same run it had been 216–225 at p50: the echo added ~60 ns). Round trip p50 337–388, p99.9 367–435 |
| 2026-10-19 | Measured | Superseded pending re-measurement: these runs did not control where memory lived; queues and shared lines were on the far NUMA node (note 008, Review). Every set is being re-measured with memory bound to the measured node and its placement recorded |
