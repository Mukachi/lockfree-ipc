# 006 — SPSC queue

**Status:** Measured · **Updated:** 2026-10-19

## Proposal

A bounded single-producer, single-consumer queue for fixed-size messages between two threads:
the first of the repo's queues, and the one every later piece (shared-memory transport, the
integration repo's ITCH → risk → FIX path) builds on.

It is built in measured steps. Each optimisation is a separate variant, measured with the Step 0
harness against the one before, so every design choice in the final queue has a number behind
it. The same harness measures two established SPSC queues as baselines.

It does:

- `try_push` and `try_pop` of a trivially copyable `T`, never blocking, never allocating;
- keep its state in a layout with no pointers, so the same ring can later live in shared memory;
- publish one-way and round-trip latency per variant and baseline, at a sweep of rates, with the
  run's environment and noise floor.

It does not:

- support more than one producer or consumer (MPMC is a later note);
- go across processes yet. The layout allows it; the transport (mapping, naming, lifetime) is a
  later note;
- block or wait. A full queue returns `false`; the caller decides.
- batch or hand out in-place slots (`claim`/`publish`). Possible later variants, each measured.

## Design

**Ring.** Capacity a power of two, set at compile time. Slots are a plain array of `T`, packed.
Two counters: `tail` (next slot to write; written only by the producer) and `head` (next slot to
read; written only by the consumer). Both are 64-bit and never wrap in practice; the slot index
is `counter & (capacity − 1)`, and the queue is full when `tail − head == capacity`. No slot is
wasted to tell full from empty.

**Ordering.** The producer writes the slot, then stores `tail` with release. The consumer loads
`tail` with acquire, reads the slot, then stores `head` with release; the producer loads `head`
with acquire. On x86 these are plain moves (no `lock` prefix, no fence); on ARM they are
`ldar`/`stlr`. The ARM build is where a missing ordering would show, which is why correctness is
also to be run there (CI, to come).

**Variants, each measured against the previous:**

| Variant | Change | What it should show |
|---|---|---|
| `v1` | Lamport's ring as above: `head` and `tail` on separate 64-byte lines | the baseline cost of two lines moving between cores on every message |
| `v2` | each side keeps a cached copy of the other's counter, reloading it only when the ring looks full (producer) or empty (consumer) | fewer cross-core line transfers: the shared counter is read once per many messages, not once per message |
| `v3` | `head`, `tail` and the slots separated by 128 bytes, not 64 | whether Broadwell's adjacent-line prefetcher (which fetches lines in 128-byte pairs) makes 64-byte separation false sharing in practice |

The final queue is the best measured variant; the losing variants stay in the repo, measured.
**Chosen: v3** (Review, 2026-10-19). `spsc_ring<T, N>` defaults to it.

**Shared-memory ready.** `T` must be trivially copyable (`static_assert`). The queue's state is
standard-layout, holds no pointers, and puts the slots inline, so it can be placed in a mapped
region and used from two processes with no change.

**Baselines,** fetched by CMake into `build/` and pinned (versions recorded in the run JSON):
moodycamel `ReaderWriterQueue` (v1.0.7 today) and `boost::lockfree::spsc_queue` (Boost 1.92.0
today). Each is wrapped in the load generator's `send`/`try_receive` interface, nothing else.

**Measurement.** The SUT for note 003's generator, one-way and round trip in the same run:

- CPU pairs: 2 → 3 (different cores, same socket) and 2 → 46 (hyper-thread siblings, one core).
  Cross-socket is not measured: no node-1 CPU is reserved, and note 001's two-speed effect is
  unexplained;
- rates 100k, 1M, 2M, 5M, 10M/s, 10 s each. A rate the generator cannot sustain is refused, as
  in note 003, so the highest published rate is also the pair's sustainable rate under this
  harness;
- the generator's 16-byte message (`seq`, `intended`). Larger messages are a later step.

## Spec

- S1. FIFO with no loss and no duplication: a two-thread stress test checks every sequence
  number, natively and under ThreadSanitizer.
- S2. Full and empty are exact at capacity, including counters started just below 2^64, so the
  subtraction is right across a wrap.
- S3. `try_push` and `try_pop` make no call, allocation or system call, and on x86 contain no
  locked instruction or fence: checked in the compiled code.
- S4. `T` is trivially copyable; the queue is standard-layout with no pointer members.
- S5. Every variant and baseline is measured with the same harness, pairs and rates, with
  `environment` and `noise` blocks in each run.
- S6. Compiles for aarch64 with warnings as errors.

## Tasks

- [x] `include/lfipc/spsc.hpp`: v1, with S1–S4 tests
- [x] v2 (cached counters) and v3 (128-byte separation), same tests
- [x] Baselines fetched and wrapped
- [x] `bench/spsc_bench`: every queue × pair × rate
- [x] Reviewed run under bench-on; the choice of final variant recorded here
- [x] Turbo: measure its effect on run-to-run p50 before any decision
- [x] Generator cost cross-core (~200 ns per message caps 2→3 at ~5M/s): measure before any decision
- [x] Three bench-on runs with one-way and round trip measured separately; publish ranges

## Review

| Date | Stage | Finding |
|---|---|---|
| 2026-10-19 | Draft | Written for review |
| 2026-10-19 | Agreed | Approved by Shep: measured variants v1–v3, in-process now with a shared-memory-ready layout, moodycamel and Boost.Lockfree baselines, pairs 2→3 and 2→46, rates 100k–10M/s, 16-byte messages. Noted for later: `run.py report --check` does not exist yet and is needed before README numbers |
| 2026-10-19 | Built (partial) | v1 (`spsc_ring<T, N, 64>`): tail at offset 0, head at 64, slots at 128. Tests: empty, full at exactly capacity, FIFO, 100 rounds at every fill level; S2 across the 2^64 counter wrap; S1 20M messages across two threads, 8- and 64-byte (the 64-byte one detects torn reads), 1M under TSan and ASan; S4 standard layout. Mutants: a full check off by one fails every test; `tail` stored relaxed is a TSan data race on the slot read (natively undetectable on x86, which keeps stores in order); `tail` stored seq_cst fails S3 (`xchg`). S3 in the compiled code: `try_push` is 13 instructions, plain moves only; the parser now keeps `lock` prefixes visible. ARM build: release/acquire become `stlr`/`ldar` |
| 2026-10-19 | Built (partial) | v2 and v3: a `Cached` template parameter; each side's copy of the other's counter sits on its own counter's line. Same tests, all pass (20M messages each, 1M under TSan and ASan). v3 places head at 128 bytes and slots at 256. S3 refined: `xchg` counts only with a memory operand; `xchg %ax,%ax` is the assembler's 2-byte padding nop (found after a `ret` in v3 `try_pop`). The seq_cst mutant is still caught in all three |
| 2026-10-19 | Built (partial) | Baselines fetched by CMake into `build/_deps`, behind `LFIPC_BASELINES` (on by default): moodycamel `readerwriterqueue` v1.0.7 pinned to commit 8b21766 (released 2025-04-28); Boost 1.92.0 from `boost-1.92.0-cmake.tar.xz` (108 MB, released 2026-08-12), pinned by SHA-256, built for `lockfree` only. Their headers are SYSTEM, so our warnings-as-errors apply to our code only. Adapters in `bench/queues.hpp`: send/try_receive and nothing else. Capacities as wrapped, all asked for 1024: our rings and Boost (compile-time capacity) hold exactly 1024; moodycamel rounds up and holds 2,044 without allocating. At the measured rates no queue nears full, so capacity does not enter latency. All five pass the same two-thread sequence check (5M messages; 200k under ASan). TSan cannot vouch for either baseline: moodycamel marks its queue `no_sanitize("thread")`, and Boost.Lockfree uses standalone fences TSan does not model (GCC refuses them under `-Werror=tsan`). Under TSan the adapter test checks our rings only and says so |
| 2026-10-19 | Built | `bench/spsc_bench`: noise on CPUs 2, 3, 46; clock; each pair's offset bound measured directly; then for each pair and rate the five queues back to back (fresh queues per run, one each way); a load block per queue in an `spsc` block that records capacity, message size, seconds per rate and baseline versions (defined once in CMake). A refused rate is a result, not a failure. Note 003 change within its design: the producer fills its own schedule (`detail::fill_schedule`) after pinning and before reporting ready, so the array (880 MB at 10M/s × 10 s) is first-touched on the producer's node; S6 allows exactly that named setup call, and still fails on a SUT making a system call. Smoke run, outside bench mode, 0.2 s per rate: 2→46 published every rate for every queue (one-way p50 ≈ 50 ns); 2→3 refused almost everything with the same producer lateness (p99 ≈ 12 µs, max ≈ 17 µs) for every queue and rate, so not the queue. Unfenced, NUMA balancing on, 0.3 s fresh threads; to be judged on the bench-on run |
| 2026-10-19 | Built | First bench-on run: `results/spsc-dev-lnx01-20261019T142448Z.json` (commit bd8fe7d, clean, all conditions met, noise samples on 2, 3, 46: zero stops). The smoke run's 12 µs lateness on 2→3 is gone: it was the unfenced environment. One-way p50 / p99 / p99.9, 2→3: at 1M/s v1 198/279/311, v2 156/235/264, v3 192/248/261, moodycamel 297/341/358, Boost 223/289/335; at 2M/s v1 209/351/405, v2 165/304/372, v3 185/215/238, moodycamel 282/472/514, Boost 259/396/429. 2→46 (siblings): every queue p50 60–75, p99.9 92–110 ns, differences within a few ns. Reading: cached counters take ~40 ns off p50 cross-core; 128-byte separation buys the tail (p99.9 238 against 372 at 2M/s) at ~25–35 ns of p50; v3 beats both baselines at every percentile cross-core. 5M and 10M/s on 2→3 are refused for all queues at producer lateness ≈ 200 ns: the generator's own per-message cost cross-core, not the queues', so the highest published rate measures the harness there; the note's claim needs that qualification. Maxima of 1.4–2 µs at ≥ 1M/s for every queue and pair, absent at 100k/s: consistent with a rare residual tick landing on dense messages; three outliers of 10–22 µs. One run: final variant to be chosen only if the ranking holds over repeats |
| 2026-10-19 | Built | Repeats: `results/spsc-dev-lnx01-20261019T143849Z.json`, `results/spsc-dev-lnx01-20261019T144947Z.json` (same commit, all conditions met). Over three runs, 2→3: v2 beats v1 at p50 by ~30–40 ns every run; v2 against v3 at p50 flips (noise); v3 beats v2 at 2M/s every run, p99 214–251 against 304–362, p99.9 238–283 against 372–431; at 100k–1M/s their tails are roughly equal; v3 beats moodycamel and Boost at p50 and p99.9 every run. 2→46: all five within a few ns, no consistent ranking (moodycamel 5–10 ns slower). Run 3's p50s sit ~30–40 ns above runs 1–2 for v1 and v2, as large as the v2/v3 p50 difference; suspected turbo (on, recorded), unverified. 5M/s refusals change from run to run: the generator's ceiling cross-core is at the margin there. Proposed to Shep: v3 as the final queue |
| 2026-10-19 | Measured | Decided with Shep: v3 is the final queue; `spsc_ring<T, N>`'s defaults are v3's (a static_assert keeps them equal); v1 and v2 stay, named and measured. Spec: S1–S4 by tests and the compiled code, S5 by three bench-on runs, S6 by the aarch64 build. Two follow-ups agreed, measure before deciding: turbo's effect on run-to-run p50 (runs differed by 30–40 ns), and the generator's ~200 ns per message cross-core, which caps the measurable rate on 2→3 at ~5M/s |
| 2026-10-19 | Measured | Generator cost (scratch experiment, bench-on, v3, 2→3, 2 s per rate): one-way-only runs sustain 8M/s (lateness p99 92 ns turbo on, 80 off), with echo 6M/s; the generator costs ~125 ns per message cross-core, and the echo pick-up about a quarter of its capacity. The 5M/s refusals in spsc_bench were the echo mode at the margin over 10 s. Unexpected: the echo inflates the one-way figures themselves. Turbo on, one-way p50 158–167 ns one-way-only against 177–216 with echo; p99 189–209 against 204–306. Note 006's one-way numbers (measured with echo, per note 003's option C) overstate one-way latency by ~20–50 ns at p50 and up to ~100 at p99; the round trip is unaffected. Decision for Shep pending |
| 2026-10-19 | Measured | Turbo off (`results/spsc-dev-lnx01-20261019T154134Z.json`, `...155312Z.json`, commit c0c3f48, clean, `turbo: false`): everything slower, one-way p50 +30–50 ns cross-core and ~+15 ns on siblings, tails +30–50 ns. Run-to-run p50 spread cross-core: median 27 ns turbo on (3 runs) against 14 off (2 runs), but the largest spread is 53 on against 62 off: turbo is not the cause of the shifts, which remain unexplained. v3's choice holds turbo off (p99.9 at 2M/s 278–291 against v2 404–486). Recommendation to Shep: keep turbo on; publish queue figures from at least three runs, as ranges. A first turbo-off attempt refused correctly: the binary was built before the v3 commit and recorded a dirty tree |
| 2026-10-19 | Measured | Decided with Shep: turbo stays on (off costs 30–50 ns and does not remove run-to-run shifts); queue figures are published from at least three runs, as ranges; one-way and round trip are measured in separate runs (note 003 amended). `spsc_bench` now runs each queue at each rate twice, back to back; each queue's entry holds a `one_way` and a `round_trip` block. Runtime ~20 min per run. The five earlier result files stand as recorded: their round trips are valid, their one-way figures were measured with echo on |
| 2026-10-19 | Measured | Publishable runs, one-way and round trip separate, turbo on: `results/spsc-dev-lnx01-20261019T163730Z.json`, `...165847Z.json`, `...172005Z.json` (commit bda41f5, clean, all conditions met). Ranges over the three runs, one-way 2→3, v3 p50 / p99 / p99.9: 100k/s 168–196 / 224–256 / 263–282; 1M/s 157–187 / 220–249 / 260–272; 2M/s 159–175 / 208–251 / 252–276; 5M/s 156–171 / 211–244 / 253–276. v3 is lowest or tied at p99 at every rate; at p99.9 its range is lowest or overlaps the lowest (moodycamel 265–281 at 1M/s overlaps); at 2M/s moodycamel 342–380, Boost 280–309. Without echo every queue sustains 5M/s one-way; 10M/s is refused for all (generator ceiling ~8M/s). Round trip 2→3: v3 best or tied (p99.9 517–552 at 100k/s); at 5M/s only v3 (2/3) and v2 (1/3) published. 2→46: all five within ~10 ns at every percentile. The choice of v3 holds with clean one-way figures |
| 2026-10-19 | Measured | Superseded pending re-measurement: these runs did not control where memory lived; queues and shared lines were on the far NUMA node (note 008, Review). Every set is being re-measured with memory bound to the measured node and its placement recorded |
| 2026-10-19 | Measured | Superseded again before publication: the thread-version generator shared one result struct between producer and consumer, whose histograms shared a cache line (note 008, Review). Fixed; re-measured next |
