# 008 — Shared-memory transport

**Status:** Agreed · **Updated:** 2026-10-19

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
correctness rests on the stress test and on ARM (CI, to come), not on TSan.

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
- [ ] Cross-process generator (fork, shared control and results)
- [ ] `bench/shm_bench`, published through the manifest; reviewed runs

## Review

| Date | Stage | Finding |
|---|---|---|
| 2026-10-19 | Draft | Written for review |
| 2026-10-19 | Agreed | Approved by Shep: named segments (`shm_open`), layout validation in the header, stale-segment replacement only when the creator is gone, fork-based cross-process measurement published beside v3's thread results |
| 2026-10-19 | Built (partial) | `shm.hpp`: `shm_spsc<T, N, Separation, Cached>`, header then ring at the ring's alignment (320 bytes for 8 × 8-byte slots). A segment is removed only with proof it is stale: our magic and a creator pid that no longer exists; too small, still initialising or not ours is never touched, which closes the race with a creator mid-setup. Tests: create/attach through two mappings; S2 each mismatch refused and named (capacity, type tag, type size, separation, version; a separation mismatch also names the segment size, which differs too); attach waits for ready, times out without it; S3 a live segment is refused naming its pid, a dead creator's is replaced; S4 a killed attacher is reported gone. No segment is left in /dev/shm. Clean under TSan and ASan, builds for aarch64 |
| 2026-10-19 | Built (partial) | S1: 20M messages each, 8- and 64-byte, parent to forked child through the segment, in order, none lost, duplicated or torn (1M under TSan and ASan). S5: the ring reached through the segment compiles to the same plain moves (`check_hot_path.py`, subjects `lfipc_spsc_shm_push/pop`). A broken producer (skipping one message) first made the test hang, not fail: the child exited on the gap and the producer spun on a full ring. The producer now checks for the child's exit while pushes fail; the broken run fails in 14 ms. The hang left a segment behind, removed by hand |
