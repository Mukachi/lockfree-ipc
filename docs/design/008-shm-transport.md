# 008 — Shared-memory transport

**Status:** Measured · **Updated:** 2026-10-20

## Proposal

Note 006's queue works between threads. A trading system runs its parts as separate processes
(feed handler, strategy, gateway) so one can restart, crash or be deployed without the others.
This component puts the same ring, v3 unchanged, in shared memory between two processes, and
measures whether crossing a process boundary costs anything.

The prediction is that it costs nothing in the steady state: the two cores move the same cache
lines whether the threads share an address space or not. The only differences are page tables
(each process maps the segment separately) and setup. The measurement will say.

It does:

- create a named segment holding a header and one `spsc_ring<T, N>`, and attach to it from
  another process;
- refuse to attach to a segment whose layout differs (type size, capacity, separation, version);
- tell a peer that has gone from one that is slow, outside the hot path;
- clean up the name, including one left by a crashed run;
- measure v3 across two processes with note 003's generator, against v3 across two threads.

It does not:

- change the ring. `try_push` and `try_pop` are note 006's, byte for byte;
- block or wake (no futex): both sides spin, as in note 006;
- carry more than one queue per segment, or variable-size messages (later notes);
- reserve huge pages: the ring for 16-byte messages is ~16 KB, five ordinary pages.

## Design

**Naming: `shm_open` under `/dev/shm`.** Unrelated processes find the segment by name, which is
how separately deployed processes meet. The alternative, `memfd_create` with the descriptor passed
over a Unix socket, cleans up by itself but needs a socket and a related or cooperating process;
it is not taken.

**Segment layout.** A header, then the ring:

| Field | Purpose |
|---|---|
| `magic`, `version` | it is ours, and this layout version |
| `type_size`, `type_align`, `capacity`, `separation`, `type_id` | the attacher's `spsc_ring<T, N>` matches the creator's; `type_id` is a caller-chosen 64-bit tag for `T` |
| `state` | `initialising` → `ready`, stored with release by the creator after constructing the ring |
| `creator_pid`, `attacher_pid` | liveness checks and stale-segment recovery |

**Lifetime.** The creator `shm_open`s with `O_CREAT | O_EXCL`, sizes, maps, constructs the ring
with placement `new`, then sets `state = ready`. The attacher maps, waits for `ready` (acquire),
validates the header, and uses the ring through `std::launder`. Formally, C++ has no rule for an
object created in one process and used in another; this is the standard practice, it relies on
the ring being standard-layout with lock-free, address-free atomics (note 006, S4), and it is
checked by the stress test below rather than assumed. The creator unlinks the name on destruction.

**Stale segments.** If `O_EXCL` fails, the existing segment is replaced only if its
`creator_pid` no longer exists (`kill(pid, 0)` fails with `ESRCH`); otherwise creation fails with
the reason. No live segment is ever removed.

**Liveness, outside the hot path.** `peer_alive()` checks the other side's pid. A consumer that
sees an empty ring cannot tell a slow producer from a dead one; the caller decides how often to
ask.

**NUMA.** Pages are placed on first touch. The creator constructs (touches) the ring, so it should
run on the node both sides use; the benchmarks create from a thread pinned to a reserved node-0
CPU. The header records the creator's CPU.

**Measurement, across processes.** Note 003's generator runs producer and consumer as threads.
Across processes: the parent creates the segment and a second shared region for control (`t0`,
ready flags) and results, then `fork`s. The child pins to the consumer CPU and runs the consumer
loop; the parent runs the producer. Each records its own histograms; the child copies its
counters into the results region before exiting. Same pairs (2→3, 2→46), rates and modes
(one-way and round trip in separate runs) as note 006, so the cross-process table sits beside
v3's cross-thread one.

**What TSan cannot do.** ThreadSanitizer sees one process; it cannot check two. Cross-process
correctness rests on the stress test and on ARM (CI, to come: note 009), not on TSan.

## Spec

- S1. A second process receives every message in order, none lost, duplicated or torn (8- and
  64-byte messages, two-process stress test).
- S2. Attaching to a segment with a different type size, alignment, capacity, separation, type tag
  or version fails with the mismatch named; attaching before `ready` waits, then succeeds.
- S3. A stale segment (creator gone) is replaced; a live one is never removed, and creating over it
  fails with the reason.
- S4. `peer_alive()` reports a killed peer as gone.
- S5. The ring's hot path is unchanged: note 006's S3 check applies to the shared-memory
  instantiation.
- S6. One-way and round trip across processes are published beside v3 across threads, same pairs
  and rates, at least three runs, via `run.py report`.
- S7. Compiles for aarch64 with warnings as errors.

## Tasks

- [x] `include/lfipc/shm.hpp`: segment, header, create/attach, stale recovery, liveness; S2–S4 tests
- [x] Two-process stress test (S1)
- [x] Cross-process generator (fork, shared control and results)
- [x] `bench/shm_bench`, published through the manifest; reviewed runs

## Review

| Date | Stage | Finding |
|---|---|---|
| 2026-10-19 | Draft | Written for review |
| 2026-10-19 | Agreed | Approved by Shep: named segments (`shm_open`), layout validation in the header, stale-segment replacement only when the creator is gone, fork-based cross-process measurement published beside v3's thread results |
| 2026-10-19 | Built (partial) | `shm.hpp`: `shm_spsc<T, N, Separation, Cached>`, header then ring at the ring's alignment (320 bytes for 8 × 8-byte slots). A segment is removed only with proof it is stale: our magic and a creator pid that no longer exists; too small, still initialising or not ours is never touched, which closes the race with a creator mid-setup. Tests: create/attach through two mappings; S2 each mismatch refused and named (capacity, type tag, type size, separation, version; a separation mismatch also names the segment size, which differs too); attach waits for ready, times out without it; S3 a live segment is refused naming its pid, a dead creator's is replaced; S4 a killed attacher is reported gone. No segment is left in /dev/shm. Clean under TSan and ASan, builds for aarch64 |
| 2026-10-19 | Built (partial) | S1: 20M messages each, 8- and 64-byte, parent to forked child through the segment, in order, none lost, duplicated or torn (1M under TSan and ASan). S5: the ring reached through the segment compiles to the same plain moves (`check_hot_path.py`, subjects `lfipc_spsc_shm_push/pop`). A broken producer (skipping one message) first made the test hang, not fail: the child exited on the gap and the producer spun on a full ring. The producer now checks for the child's exit while pushes fail; the broken run fails in 14 ms. The hang left a segment behind, removed by hand |
| 2026-10-19 | Built (partial) | Cross-process generator (`load_xproc.hpp`). Note 003's loops moved into `detail::producer_loop` and `consumer_loop` (always inlined) with a `load_sync` of lock-free atomics; the thread version calls them from its lambdas, unchanged in the compiled code (S6 still passes), and the process version calls the same code from parent and forked child. The child attaches by name (its own mappings); its latency histogram returns through the shared region as plain words (`histogram::export_state`/`import_state`, round-trip tested). The parent pins to the producer CPU and creates the segments there (first touch on node 0), restoring its affinity after. A watchdog reaps the child; `abort` is read only when a send is refused or no echo waits, so a dead consumer ends the run without touching the timed path. Tests: one-way and round trip complete across processes with every message returned in order; a consumer killed 0.3 s into a 10 s run ends it promptly ("consumer process ended early (status 9)"); an unpinnable consumer refuses. A first version unmapped the shared region before its last use and crashed; found under gdb, fixed. Clean under TSan and ASan, builds for aarch64 |
| 2026-10-19 | Built | `bench/shm_bench`: v3 between threads and between processes, back to back for each pair, rate and mode, in one run (so S6's comparison is under one set of conditions, not across sessions); noise on 2, 3, 46; clock. `run.py` knows an `shm` set (one block per mode per transport, validated) and renders threads and processes side by side. Smoke run outside bench mode: structure complete, no segment left. All tasks but the reviewed runs done |
| 2026-10-19 | Built | **Finding: memory placement was never controlled.** First `shm_bench` runs (`results/shm-dev-lnx01-20261019T204021Z.json`, `...204844Z.json`, `...205707Z.json`, kept, not published): one-way threads and processes overlap at 2→3 up to 5M/s (p50 162–187 against 163–200), but processes were faster on the round trip (p50 320–348 against 370–410) and alone sustained 10M/s, and slower on siblings by 15–20 ns. Cause, confirmed with `move_pages`: the thread version's queues were made by the unpinned main thread, which under boot isolation runs on housekeeping CPUs, 46 of 48 on node 1; the queue's pages were on node 1 while producer and consumer were on node 0. The process version's parent pinned to CPU 2 before creating its segments. The same flaw sits under every earlier published queue figure (notes 003 and 006, and note 001's cross-CPU flags, which were on the caller's stack). A second, non-obvious layer: pinning the allocating thread is not enough, because glibc's per-thread arenas pass between threads, so a pinned thread can receive pages another node's thread touched (`placement_test` showed a CPU-22 object on node 0). Fixes, agreed with Shep: (1) every benchmark process binds its memory to the measured node at start (`set_mempolicy(MPOL_BIND)`, in `run_file`), recorded as `environment.placement.memory_policy`; (2) `make_on_cpu` builds shared objects in fresh `mmap` pages on a pinned thread; the cross-CPU flags likewise, now 128-byte aligned; (3) every load run records `memory_node` and refuses if not the producer's node; `run.py` refuses to publish any run without both fields. The manifest is emptied: every set is re-measured |
| 2026-10-19 | Built | **Second finding: false sharing in the generator.** On clean placement (`results/shm-dev-lnx01-20261019T230909Z.json`, `...231731Z.json`, `...232553Z.json`, kept, not published), one-way threads and processes overlap at every rate (10M/s included), but round trip still favoured processes (2M/s p50 315–323 against 378–397; 5M/s 361–366 against 510–616) and siblings favoured threads by 10–20 ns. Cause: the thread version's producer and consumer wrote into one `load_result`; the consumer's latency histogram ends at byte 144 and the producer's lateness histogram starts there, and both store into bytes 128–191 on every message: the harness ping-ponged a line between the measured cores. The process version's consumer already used its own results. Fix: the consumer records into its own 128-byte-aligned results, merged after the run. A/B outside bench mode, round trip p50 at 2M/s: threads 359–368 before, 280–289 after, processes 268–283. Every thread-version figure (notes 003, 006, the shm threads column) carried this; load, spsc and shm are held back and re-measured. Noise, clock and histogram are unaffected and published |
| 2026-10-20 | Measured | Reviewed runs after both harness fixes: `results/shm-dev-lnx01-20261020T102449Z.json`, `...103309Z.json`, `...104128Z.json` (commit 531986e, clean, memory bound to node 0, every queue on node 0, zero noise stops), published via `run.py report`. Different cores (2→3): threads and processes overlap at every rate, one-way and round trip (one-way p50 at 1M/s 163–187 against 180–198; round trip p99.9 at 2M/s 435–501 against 441–443). Any p50 difference is within ~15 ns, inside the run-to-run spread: crossing the process boundary costs nothing measurable. The earlier round-trip advantage of processes was the generator's false sharing. Hyper-thread siblings (2→46): processes are consistently slower, one-way p50 +8–17 ns at 100k–2M/s, round trip ~+20 ns, converging at 5–10M/s. Real (every run), cause unknown; candidate: the two processes reaching the same lines through different page tables while sharing one core; a one-process, two-mapping test would separate it. Open; relevant only if a queue's ends share a core. Spec: S1–S7 pass |
| 2026-10-20 | Amended | Note 012 found that the siblings comparison above changed two things, not one: the process version's ring starts 128 bytes into a page (after the segment header), the thread version's at page offset 0. With the layout held constant, the one-way gap on siblings is mostly that layout (~6 ns at p50), processes adding 0–3 ns; the round-trip gap (+13–20 ns at p50) is the second address space. The virtual address plays no part. RESULTS.md's siblings table carries a note saying so; no figure changes |
| 2026-10-21 | Measured | Republished: every set re-measured with C1 kept (idle siblings halt instead of spinning; note 004, Review 2026-10-21), commit c279cc4, published via `run.py report`; `results/shm-dev-lnx01-20261021T173121Z.json`, `...184614Z.json`, `...200109Z.json`. On separate cores (2 → 3) one-way, threads and processes still overlap; round trip, processes now run a little higher: p50 313–334 against 289–308 at 1M/s (no longer overlapping), overlapping at 100k and 2M/s. "Overlap at every rate" (Measured, 2026-10-20) no longer holds for the round trip; a cost of ~5–8% at p50, consistent with note 012's address-space effect, which is larger on siblings. Siblings (2 → 46): processes slower, as note 012 explains |
