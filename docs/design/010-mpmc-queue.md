# 010 — MPMC queue

**Status:** Measured · **Updated:** 2026-10-20

## Proposal

A bounded multi-producer, multi-consumer queue for fixed-size messages between threads. In a
trading system the common shape is fan-in: several feed handlers or strategies feeding one
gateway or risk check. Fan-out to a pool of workers is the other. Note 006's ring cannot serve
either: two producers on it race on `tail`.

As in note 006, it is built in measured steps against baselines, and the first figure to
publish is the price of generality: MPMC against SPSC v3 on the same single pair.

It does:

- `try_push` and `try_pop` of a trivially copyable `T`, never blocking, never allocating;
- keep a layout with no pointers, so the ring can later live in shared memory (note 008's
  transport, unchanged);
- extend note 003's generator to several producers and consumers;
- publish one-way latency per variant and baseline, per shape (producers → consumers) and rate.

It does not:

- promise one global order across producers beyond what the algorithm gives (below);
- put the queue in shared memory yet (a later step, reusing note 008);
- block, wait or wake; a full queue returns `false`;
- measure round trip (see Design, Measurement).

## Design

**Algorithm: Dmitry Vyukov's bounded MPMC queue** (published on 1024cores.net), the standard
public design. Each slot carries a sequence number beside the data. Two positions,
`enqueue_pos` and `dequeue_pos`, 64-bit, on their own lines.

- Push: read `enqueue_pos`; if the slot's sequence equals it, the slot is free for this lap:
  claim it with a CAS on `enqueue_pos`, write the data, then store the slot's sequence as
  `pos + 1` with release. If the sequence is behind, the ring is full: return `false`. If ahead,
  another producer took it: reload and retry.
- Pop: the mirror: a slot whose sequence is `pos + 1` holds data; claim it with a CAS on
  `dequeue_pos`, read, then store the sequence as `pos + capacity` with release, freeing it for
  the next lap.

Producers contend only with producers (on `enqueue_pos`), consumers only with consumers; a
producer and a consumer meet only on a slot's sequence, as in the SPSC ring.

**What it guarantees, stated plainly.** Messages from one producer are delivered in that
producer's order; with one consumer that order is seen directly, and with several each consumer
sees each producer's messages in increasing order. It is **not lock-free in the strict sense**: a
producer stalled between its CAS and its sequence store (preempted, or hit by an SMI) holds that
slot, and consumers see the queue as empty at that point until it finishes, even if later slots
are full. Under the bench fence such stalls are rare (note 005's floor), but they would show in a
consumer's tail as a stall of another thread. rigtorp's queue (below) shares this property;
moodycamel's per-producer sub-queues and Boost's linked list do not.

**Rejected: `fetch_add` tickets** (claim a position unconditionally, then wait for its turn). They
avoid CAS retries under contention, but a producer that claimed a position must wait if the ring
is full, so `try_push` could no longer return `false` without blocking. Not taken.

**Variants, each measured against the previous:**

| Variant | Change | What it should show |
|---|---|---|
| `v1` | Vyukov as above; positions 128 bytes apart (note 006's v3 lesson); slots packed (16-byte message + 8-byte sequence = 24 bytes) | the cost of the CAS and of the sequence handshake |
| `v2` | each slot padded to its own 128 bytes | whether producers writing neighbouring slots at once (and consumers reading them) false-share; costs 5× the memory |

**Chosen: v2** (Review, 2026-10-20). `mpmc_ring<T, N>` defaults to it.

**Baselines,** fetched by CMake into `build/` and pinned, as in note 006:

| Queue | Version (checked 20 Oct 2026) | Note |
|---|---|---|
| moodycamel `ConcurrentQueue` | v1.0.5 (`9afb997`, 2026-04-03) | per-producer sub-queues; FIFO per producer only, no order across producers, "not linearizable" and "only eventually consistent" (its README); `try_enqueue` never allocates but can fail under contention even when pre-sized. Weaker semantics, published with that stated |
| `boost::lockfree::queue` | Boost 1.92.0 (already fetched) | Michael–Scott linked queue on a fixed-size node pool; lock-free in the strict sense |
| rigtorp `MPMCQueue` | v1.0 (`11ffb3c`, 2020; last commit 2023) | the same Vyukov design, widely used; a check on our implementation as much as a competitor |

SPSC v3 joins at 1 → 1 only, as the reference.

**Generator, several producers and consumers.** Note 003's generator, generalised; the
one-producer, one-consumer case is the existing code path, unchanged.

- *Producers.* Each runs note 003's open-loop loop at `rate / P`, scheduled from one `t0` with
  producer `k` offset by `k / rate`, so the combined arrivals are evenly spaced at `rate`. Each
  records its own lateness; a run is refused if any producer's p99 lateness exceeds its interval.
- *Message.* Still 16 bytes (`seq`, `intended`), so figures compare with notes 006 and 008; the
  producer's index goes in the top 8 bits of `seq`.
- *Consumers.* Each records `receive − intended` into its own 128-byte-aligned results (note 008's
  false-sharing lesson), merged after the run. Each checks that every producer's sequence numbers
  arrive in increasing order. A consumer stops when the queue is empty and every producer has
  finished; it reads that flag only on an empty poll, never on the timed path.
- *Counts.* The run fails unless the consumers' received total equals the number sent.
  Exactly-once delivery per message is the stress test's job (S1), not the benchmark's: a shared
  bitmap would put a contended write on the measured path.
- *Round trip: not measured.* An echo would need a return queue per producer and gives a figure
  dominated by the return path. One-way relies on cross-CPU TSC agreement, which note 001 bounds.

**Measurement.**

- Shapes (producers → consumers), all on node 0's reserved cores, producers from CPU 2 upward,
  then consumers: 1 → 1 (2 → 3, as note 006), 2 → 1, 4 → 1, 2 → 2, 4 → 4. Hyper-thread siblings
  are not used (the siblings question is separate).
- Aggregate rates 100k, 1M, 2M, 5M/s, 10 s each; 10M/s only where sustained. A refused rate is
  published as refused.
- Queues at capacity 1024. About 17 minutes per run of the whole matrix; three runs, published as
  ranges via `run.py report` (a new `mpmc` set).

## Spec

- S1. Every message is delivered exactly once, and each consumer sees each producer's messages in
  order: stress test with P × C threads (1×2, 2×1, 2×2, 4×4, 8×1), 8- and 64-byte messages, a
  bitmap of every sequence number; natively, under TSan, and on ARM in CI.
- S2. Full and empty are exact at capacity, including positions started just below 2^64.
- S3. `try_push` and `try_pop` make no call, allocation or system call; on x86 each contains
  exactly one locked instruction (the CAS) and no fence: checked in the compiled code.
- S4. `T` is trivially copyable; the queue is standard-layout with no pointer members.
- S5. The generalised generator: a producer held up shows in latency from the intended time
  (note 003's S1 per producer); the consumers' totals and per-producer order are checked; a late
  producer refuses the run; at 1 → 1 its figures for SPSC v3 overlap note 006's published ranges.
- S6. Every variant and baseline is measured with the same harness, shapes and rates, with
  `environment` and `noise` blocks; at least three runs published via `run.py report`.
- S7. Compiles for aarch64 with warnings as errors (CI).

## Tasks

- [x] `include/lfipc/mpmc.hpp`: v1, with S1–S4 tests
- [x] v2 (padded slots), same tests
- [x] Generator for P producers and C consumers, with S5 tests
- [x] Baselines fetched and wrapped
- [x] `bench/mpmc_bench` and the `mpmc` set in `run.py`
- [x] Three bench-on runs; variant chosen and recorded here

## Review

| Date | Stage | Finding |
|---|---|---|
| 2026-10-20 | Draft | Written for review |
| 2026-10-20 | Agreed | Approved by Shep: Vyukov's bounded queue (not strictly lock-free, stated), v1 packed and v2 padded slots, baselines moodycamel ConcurrentQueue v1.0.5, Boost.Lockfree queue 1.92.0 and rigtorp MPMCQueue v1.0 with SPSC v3 at 1 → 1, generator for several producers and consumers, one-way only, shapes 1→1, 2→1, 4→1, 2→2, 4→4, rates 100k–5M/s |
| 2026-10-20 | Built (partial) | `mpmc_ring<T, N, SlotAlign>`; `mpmc_v1` packs slots (24 bytes for the 16-byte message), `mpmc_v2` gives each 128; positions and slots 128 bytes apart. Tests: empty, full at exactly capacity, FIFO at every fill level over 100 rounds; S2 across the 2^64 wrap; S4 standard layout and the slot sizes; S1 5M messages per shape (1×2, 2×1, 2×2, 4×4, 8×1 with 8-byte messages; 2×2 and 4×4 with 64-byte ones, which detect torn reads), each consumer marking a bitmap combined after: every message exactly once, per-producer order, none torn; 250k under TSan and ASan, clean. The first stress test hung, not failed, on a broken queue (consumers waited for lost messages): consumers now stop on an empty queue once every producer has finished, and a 120 s watchdog fails the test. Mutants, each caught: CAS replaced by a plain store, producers or consumers (the position moves backwards and the queue livelocks; caught by the watchdog); slot freed one lap early (fill-level and wrap tests); publish stored relaxed (TSan race); publish stored seq_cst (S3, `xchg`). S3 in the compiled code: `try_push` 20 instructions, one `lock cmpxchg`, plain moves for release and acquire; same for `try_pop` and v2 (`check_hot_path.py`, subjects `lfipc_mpmc_<variant>_<op>`). Builds for aarch64 |
| 2026-10-20 | Built (partial) | CI on `da0e942`, all four jobs green; `mpmc_test` passed on aarch64 (5.6 s), its first run on a weakly ordered CPU. A limit worth stating: on the 2-vCPU runners the x86-64 release run took 0.8 s against 9 s on dev-lnx01. With up to 16 threads on two CPUs, most threads take turns rather than run at once, so the runners exercise far fewer true races than dev-lnx01; ARM's evidence is two CPUs running in parallel, not more |
| 2026-10-20 | Built (partial) | Generator, `load_mp.hpp`: `run_load_mp(sut, mp_load_config, mp_load_result)`. Producers run note 003's `producer_loop`, which gains a `tag` OR'd into every sequence number (the producer's index in the top 8 bits; 0 and folded away in the existing path, whose compiled loops still pass note 003's S6). Each producer's interval is P / rate, its start k / rate after `t0`, computed before the threads start. Consumers run `mp_consumer_loop`: per-producer order, stop on an empty queue once every producer has finished (read only on an empty poll). Every thread records into its own 128-byte-aligned results, merged after. Refusals: a CPU that cannot be pinned, any producer's lateness p99 over its interval (the reason names the producer), fewer messages received than sent. `check_hot_path.py` now also checks the new thread bodies (no calls but pinning and the schedule fill); its first version called `llround` in the producer's thread for the start offset, moved out. Tests (`load_mp_test`): 2 → 1, 4 → 1, 2 → 2 and 4 → 4 deliver every message once, in per-producer order, counts excluding warm-up; S5: producer 0 held 1 ms shows in latency from the intended time and in its lateness, nothing skipped; a producer slowed past its interval refuses the run, naming it; a message accepted and dropped refuses it ("received 119999 of 120000"); invalid and unpinnable configurations refuse without hanging. 1 → 1 at 1M/s on MPMC v1 off the fence: p50 164 ns with this generator, 163 ns with note 003's. Shapes needing more CPUs than the machine offers are skipped and reported (on the 2-vCPU runners, all but 1 → 1). Clean under TSan and ASan; builds for aarch64 |
| 2026-10-20 | Built (partial) | Baselines fetched by commit (moodycamel `ConcurrentQueue` `9afb997`, rigtorp `MPMCQueue` `11ffb3c`, header-only, their own CMake projects not added; Boost's queue from the existing Boost 1.92.0 fetch) and wrapped in `bench/mpmc_queues.hpp`: moodycamel through its token-free API (implicit producers, storage preallocated for 8 producers; `try_enqueue` never allocates). Its README says explicit producer tokens are "almost always faster (though not necessarily by a huge factor)"; tokens are per-thread state the `send`/`try_receive` interface does not carry, so its figures may understate it slightly, and the published table says so. Boost: `bounded_push` on a fixed pool of 1024 nodes. rigtorp: capacity 1024 at run time, slots padded to a cache line. `mpmc_queues_test`: each queue holds exactly 1024 when filled from one thread and drains in order; with 4 → 1 and 4 → 4 unpinned threads, 2M messages each, every message exactly once, per-producer order. As for note 006, the baselines are left out of sanitizer builds |
| 2026-10-20 | Built | `bench/mpmc_bench`: five shapes × rates 100k–10M/s × queues back to back (v1, v2, moodycamel, Boost, rigtorp; SPSC v3 at 1 → 1 only), a fresh queue per run made on the first producer's CPU and checked on its node; 1 → 1 runs note 003's generator unchanged, the other shapes `run_load_mp`. Noise on CPUs 2–9, the clock, and the offset bound of every producer → consumer pair used (13 pairs, measured once). JSON block per queue and shape: `load_mp_block.hpp`, note 003's run fields plus the CPU lists, every pair's bound, and messages sent and received. `run.py`: an `mpmc` set (one-way only per queue, placement checked, any published run that lost messages refused) rendered as one table per shape. Smoke run outside bench mode at 0.05 s per rate: structure complete; almost every rate above 100k/s refused on producer lateness (p99 ~11 µs), as `load_test` is off the fence. All tasks but the reviewed runs done |
| 2026-10-20 | Measured | Reviewed runs `results/mpmc-dev-lnx01-20261020T151545Z.json`, `...154543Z.json`, `...161548Z.json` (commit 8943c99, clean, bench mode, memory bound to node 0, every queue on node 0, zero noise stops on CPUs 2–9; 390 measurements, none lost a message or saw one out of order), published via `run.py report`. One-way ns, ranges over the runs. **v2 beats v1 in every shape**, 1 → 1 included, where no second producer exists: at 1M/s p50 147 against 183–184, p99.9 265–291 against 326–337; at 4 → 4 and 5M/s v1's tail collapses (p99.9 up to 74 µs), v2's holds (478–620). The likely cause, not separately tested: packed, the consumer reading slot k and the producer writing k + 1 share a line whenever the queue is nearly empty, which at these rates is almost always: producer–consumer false sharing, not producer–producer. **Chosen: v2**, now `mpmc_ring`'s default (128 KB per 1,024 slots against v1's 24 KB). **Generality costs nothing measurable at 1 → 1:** v2 p50 145–150 against SPSC v3's 154–183, p99.9 overlapping; SPSC v3 alone sustains 10M/s. **Against rigtorp** (same design, 64-byte slots): v2's p50 is ~35 ns lower at 1 → 1, its p99.9 lower at 2 → 1 (269–303 against 318–338) and overlapping at 4 → 4 1M/s (334–358 against 338–403), and it holds 4 → 4 at 5M/s in 3/3 runs against 1/3; rigtorp has the tighter p99.9 at 1 → 1 (254–260 against 260–291) and the lower p50 at 4 → 4 1M/s (270–286 against 294–319). Neither dominates; v2 is ahead more often. moodycamel (token-free) and Boost: p50 1.5–2.5× and p99 2–3× v2's, refused above 2M/s in most shapes. 10M/s is refused at the generator's ceiling in every shape but 1 → 1 SPSC v3 (and v2 at 2 → 1 in 1/3). S5: SPSC v3 at 1 → 1 (1M/s p50 161–183, p99 215–248, p99.9 247–271) overlaps note 006's published ranges (162–184, 214–265, 244–307). Spec: S1–S7 pass. Shep: the figures can be improved over time. Follow-up agreed: SPSC v4, v3 with each slot padded, since SPSC v3's 16-byte slots sit four to a line and may pay the same neighbouring-slot sharing |
| 2026-10-21 | Measured | Republished: every set re-measured with C1 kept (idle siblings halt instead of spinning; note 004, Review 2026-10-21), commit c279cc4, published via `run.py report`; `results/mpmc-dev-lnx01-20261021T170206Z.json`, `...181649Z.json`, `...193143Z.json`. v2 against v1: lower in 27 of 40 comparisons (shape, rate, p50 or p99.9), overlapping in 13, never higher: v2 stays. v2 against rigtorp: lower in 9, higher in 7, overlapping in 20; neither dominates, and the pattern is now sharper than "v2 ahead more often": v2 has the lower median at 1 → 1 (by ~30 ns) and the lower tails at 2 → 1, 4 → 1 and saturated 2 → 2; rigtorp has the tighter p99.9 at 1 → 1 (by ~20–40 ns) and the lower median at 4 → 4. moodycamel and Boost remain well behind |
