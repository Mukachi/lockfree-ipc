# 012 — Processes on hyper-thread siblings

**Status:** Measured · **Updated:** 2026-10-20

## Proposal

Note 008 found one difference between v3 across threads and v3 across processes: on hyper-thread
siblings (CPUs 2 → 46, one core), processes are consistently slower, one-way p50 +8–17 ns at
100k–2M/s and round trip ~+20 ns, converging at 5–10M/s. On separate cores there is none. The
cause is unknown. This note finds it, or narrows it to what can be stated honestly. It is an
investigation: no queue changes, and nothing is published to the README unless it explains a
published figure.

It matters only when a queue's two ends share a core, but that is a real deployment choice
(producer and consumer on one core's two threads to share L1 and L2), and an unexplained 10–20 ns
on a ~60 ns path is a gap in the record.

## Design

**What differs between note 008's two runs.** Three things, confounded:

1. *Address space.* Two processes have two page tables (two CR3/PCID values). On one core the two
   threads translate through the shared TLBs with different tags, so the same physical page needs
   two entries. Threads share one.
2. *Virtual address.* Each process maps the segment where `mmap` chooses; the threads use one
   address. L1D is indexed by bits within the page offset, so this should not matter on its own,
   but it is cheap to rule out.
3. *Layout.* The segment puts its header first, so the ring starts 128 bytes into a page; the
   thread version's ring starts at offset 0 (a fresh page from `make_on_cpu`). The ring's slots,
   counters and the generator's other hot data then fall at different page offsets, which changes
   4K aliasing between loads and earlier stores, and which L1 sets they share. Found while
   drafting: note 008's comparison did not hold this constant.

A fourth candidate is checked, not varied: *STIBP*. This kernel reports `STIBP: conditional`:
branch prediction is isolated between siblings only for tasks that request it (`prctl` or
seccomp). The processes should not have; each run records `Speculation_Store_Bypass` and
`SpeculationIndirectBranch` from `/proc/<pid>/status` for both ends.

**Experiment: one layout, a 2 × 2 of address space and virtual address.** Every cell uses the same
shared-memory segment (same header, ring at the same page offset), v3, the generator of note 008,
pair 2 → 46, so only the named factor changes:

| Cell | Ends | Mappings |
|---|---|---|
| A | two threads | one mapping, the same address |
| B | two threads | two mappings of the segment, different addresses |
| C | two processes | the child attaches by name: different addresses (note 008 as built) |
| D | two processes | the child uses the mapping inherited across `fork`: the same address |

C against A is note 008's difference without the layout confound. B isolates the virtual address
in one address space; D isolates the address space at one virtual address. Plus the reference
from note 006 (threads, ring at page offset 0) to measure the layout effect by itself.

Rates 100k, 1M, 2M and 5M/s (where note 008's gap appeared and closed), one-way and round trip in
separate runs, 10 s each, three runs: about 25 minutes a run. One extra pair, 2 → 3, for cells A
and C only, as a control: the gap should stay absent on separate cores.

**Then the mechanism, with counters.** For the cells that differ, a diagnostic run under
`perf stat -C 2,46` (root: `perf_event_paranoid` is 4 here) at 1M/s, counting the Broadwell events
that each candidate would move: `dtlb_load_misses.miss_causes_a_walk` and `.stlb_hit`,
`dtlb_store_misses.miss_causes_a_walk` (address space), `ld_blocks_partial.address_alias` (4K
aliasing: layout), `machine_clears.memory_ordering` and `.count`, cycles and instructions.
Counts per message, not published as latency figures; recorded in Review as the explanation.

## Spec

- S1. Cells A–D and the reference run in one benchmark, same layout across A–D (checked: the
  ring's page offset is recorded per cell and must agree), same pairs, rates and modes; three
  bench-mode runs.
- S2. Each run records, for both ends of every cell, the speculation-control status from
  `/proc/<pid>/status`, and the run is flagged if they differ between cells.
- S3. The 2 → 3 control: A and C overlap, as in note 008.
- S4. Review states which factor carries the gap (address space, virtual address, layout, or none
  of them), with the counters that support it, or states plainly that it remains unexplained and
  what was ruled out.
- S5. If the gap turns out to be layout, not processes, note 008's Review is amended to say so and
  its published threads-vs-processes table gets a footnote. No other published figure changes.

## Tasks

- [x] Cells A–D on the shared-memory segment (two mappings in one process; the inherited mapping
      across `fork`), speculation status recorded; tests that each cell runs and delivers every
      message
- [x] `bench/siblings_bench`, its JSON block; RESULTS.md section only if S5 applies
- [x] Three bench-on runs
- [x] `perf stat` diagnostic run for the cells that differ; Review

## Review

| Date | Stage | Finding |
|---|---|---|
| 2026-10-20 | Draft | Written for review |
| 2026-10-20 | Agreed | Approved by Shep: cells A–D on one segment layout (threads one or two mappings; processes attaching by name or inheriting the mapping), note 006's thread version as the layout reference, 2 → 3 control for A and C, rates 100k–5M/s, one-way and round trip separately, three runs; speculation status recorded; a `perf stat` diagnostic run by Shep; published figures change only if S5 applies |
| 2026-10-20 | Built | `run_load_xproc` gains `inherit` (the child uses the mappings it inherits across `fork`: cell D) and `xproc_info` (each process's ring address and the `Speculation*` lines of its `/proc/<pid>/status`). `bench/siblings.hpp`: cells A and B as two threads on the segment through one or two mappings (the second mapped by name in the same process), C and D through `run_load_xproc`, R as note 006's heap ring; all through note 003's loops. `siblings_test`: every cell, one-way and round trip, delivers every message in order; A–D put the ring 128 bytes into its page, R at offset 0; A and D's ends share one address, B and C's do not; every process reports `SpeculationIndirectBranch: conditional enabled` (no STIBP requested, the same in every cell). `bench/siblings_bench`: 2 → 46 cells A–D and R, 2 → 3 cells A and C, rates 100k–5M/s, one-way and round trip, about 11 minutes a run. Clean under TSan and ASan; builds for aarch64 |
| 2026-10-20 | Measured | Runs `results/siblings-dev-lnx01-20261020T202834Z.json`, `...203933Z.json`, `...205032Z.json` (commit 287369b, clean, bench mode, memory bound to node 0, every ring on node 0, zero noise stops, none out of order; every process `SpeculationIndirectBranch: conditional enabled`), published as a RESULTS.md section (S5 applies). 2 → 46, ns: **virtual address: no effect** (A = B, C = D at every rate and mode). **One-way: mostly layout.** R (page offset 0) against A (offset 128): p50 58 against 64–65 at 1M/s, 64–65 against 71 at 100k/s; processes add 0–3 ns at p50 (C 64–66), ~4 at p99.9 (98 against 94). **Round trip: the address space.** C and D against A and B: p50 137–144 against 123–124 at 1M/s, +13–20 ns from 100k to 2M/s, ~10 at 5M/s; layout adds ~0–5 ns (R 123–124). Control 2 → 3: A and C overlap (S3). Counters (`perf stat -C 2,46`, 1M/s, 11M messages per cell, two event sets of four so no multiplexing; diagnostic, not published): on the producer in round trip, memory-ordering machine clears per message R 0.021, A 0.014, C 0.080, and STLB hits R 0.109, A 0.217, C 0.431; page walks ≈0 per message in every cell; 4K-alias load blocks no higher in C (0.022 against 0.035); L2 misses negligible. The producer polls the echo ring its sibling writes; with two address spaces it takes ~5× the ordering clears and twice the L1 dTLB misses (two page tables, two entries per page). At ~100+ cycles a clear (estimated, not measured), the extra 0.07 per message is ~3–5 ns on average: part of the 13–20 ns, not all. The one-way layout effect is not explained by these counters (R and A equal on every event, or R higher). Ruled out: STIBP, the virtual address, page walks, 4K aliasing, L2 misses. Left open, by choice: the rest of the round-trip mechanism and the layout effect's, given their size. Note 008 amended (S5). The counter run also exposed a hazard in the machine scripts, not the repo: `bench-on` run twice without `bench-off` saves the fenced settings as the originals, so `bench-off` then restores the fence (NUMA balancing left off, IRQs fenced). Reported to Shep with a guard for `bench-on`; a reboot restores the defaults |
| 2026-10-21 | Measured | Republished: every set re-measured with C1 kept (idle siblings halt instead of spinning; note 004, Review 2026-10-21), commit c279cc4, published via `run.py report`; `results/siblings-dev-lnx01-20261021T173927Z.json`, `...185420Z.json`, `...200915Z.json`. Conclusions hold: R against A one-way ~6–7 ns at p50 (layout); C and D against A and B round trip +14–18 ns at p50 (address space); A = B and C ≈ D (virtual address no effect). The siblings' own sibling CPUs were busy here in both conditions, so this note's figures were the least affected |
