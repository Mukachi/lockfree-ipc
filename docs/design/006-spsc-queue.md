# 006 — SPSC queue

**Status:** Agreed · **Updated:** 2026-10-19

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
- [ ] `bench/spsc_bench`: every queue × pair × rate
- [ ] Reviewed run under bench-on; the choice of final variant recorded here

## Review

| Date | Stage | Finding |
|---|---|---|
| 2026-10-19 | Draft | Written for review |
| 2026-10-19 | Agreed | Approved by Shep: measured variants v1–v3, in-process now with a shared-memory-ready layout, moodycamel and Boost.Lockfree baselines, pairs 2→3 and 2→46, rates 100k–10M/s, 16-byte messages. Noted for later: `run.py report --check` does not exist yet and is needed before README numbers |
| 2026-10-19 | Built (partial) | v1 (`spsc_ring<T, N, 64>`): tail at offset 0, head at 64, slots at 128. Tests: empty, full at exactly capacity, FIFO, 100 rounds at every fill level; S2 across the 2^64 counter wrap; S1 20M messages across two threads, 8- and 64-byte (the 64-byte one detects torn reads), 1M under TSan and ASan; S4 standard layout. Mutants: a full check off by one fails every test; `tail` stored relaxed is a TSan data race on the slot read (natively undetectable on x86, which keeps stores in order); `tail` stored seq_cst fails S3 (`xchg`). S3 in the compiled code: `try_push` is 13 instructions, plain moves only; the parser now keeps `lock` prefixes visible. ARM build: release/acquire become `stlr`/`ldar` |
| 2026-10-19 | Built (partial) | v2 and v3: a `Cached` template parameter; each side's copy of the other's counter sits on its own counter's line. Same tests, all pass (20M messages each, 1M under TSan and ASan). v3 places head at 128 bytes and slots at 256. S3 refined: `xchg` counts only with a memory operand; `xchg %ax,%ax` is the assembler's 2-byte padding nop (found after a `ret` in v3 `try_pop`). The seq_cst mutant is still caught in all three |
| 2026-10-19 | Built (partial) | Baselines fetched by CMake into `build/_deps`, behind `LFIPC_BASELINES` (on by default): moodycamel `readerwriterqueue` v1.0.7 pinned to commit 8b21766 (released 2025-04-28); Boost 1.92.0 from `boost-1.92.0-cmake.tar.xz` (108 MB, released 2026-08-12), pinned by SHA-256, built for `lockfree` only. Their headers are SYSTEM, so our warnings-as-errors apply to our code only. Adapters in `bench/queues.hpp`: send/try_receive and nothing else. Capacities as wrapped, all asked for 1024: our rings and Boost (compile-time capacity) hold exactly 1024; moodycamel rounds up and holds 2,044 without allocating. At the measured rates no queue nears full, so capacity does not enter latency. All five pass the same two-thread sequence check (5M messages; 200k under ASan). TSan cannot vouch for either baseline: moodycamel marks its queue `no_sanitize("thread")`, and Boost.Lockfree uses standalone fences TSan does not model (GCC refuses them under `-Werror=tsan`). Under TSan the adapter test checks our rings only and says so |
