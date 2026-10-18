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
- [ ] Lateness histogram and refusal
- [ ] Round trip
- [ ] JSON `load` block
- [ ] Tests S1–S8, including the stall and fixed-delay SUTs
- [ ] First run under bench-on; results reviewed here

## Review

| Date | Stage | Finding |
|---|---|---|
| 2026-10-18 | Draft | Written for review |
| 2026-10-18 | Agreed | Approved by Shep: option C (one-way and round trip side by side), constant spacing, refusal at lateness p99 > T, warm-up 10% and at least 100 ms |
| 2026-10-18 | Built (partial) | Schedule and loops (`load.hpp`), test SUTs (`tests/test_suts.hpp`: one-slot mailbox, fixed delay, receive stall, send stall). Tests pass, also under TSan and ASan/UBSan, and compile for aarch64: every message once and in order; a 5 µs injected delay floors every latency (S5); a 1 ms producer hold shows in latency counted from the intended time, and nothing is skipped (S1, S2). Finding, not bench-on: producer lateness p99 ≈ 12 µs and p99.9 ≈ 340 µs at both 100k/s and 1M/s and on two CPU pairs, so not the generator's speed. A thread spinning alone on CPU 2 is stopped 0.6% of the time (822 gaps > 5 µs in 5 s, longest 0.5 ms, 34 involuntary context switches); CPU 30 the same. The CPUs are not isolated (no `isolcpus`/`nohz_full`), and user tasks run on them. Under S3 every run at 1M/s would refuse. The open-loop design is doing its job: it counts every message due during a stop |
