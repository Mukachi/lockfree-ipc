# 012 — Processes on hyper-thread siblings

**Status:** Agreed · **Updated:** 2026-10-20

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

- [ ] Cells A–D on the shared-memory segment (two mappings in one process; the inherited mapping
      across `fork`), speculation status recorded; tests that each cell runs and delivers every
      message
- [ ] `bench/siblings_bench`, its JSON block; RESULTS.md section only if S5 applies
- [ ] Three bench-on runs
- [ ] `perf stat` diagnostic run for the cells that differ; Review

## Review

| Date | Stage | Finding |
|---|---|---|
| 2026-10-20 | Draft | Written for review |
| 2026-10-20 | Agreed | Approved by Shep: cells A–D on one segment layout (threads one or two mappings; processes attaching by name or inheriting the mapping), note 006's thread version as the layout reference, 2 → 3 control for A and C, rates 100k–5M/s, one-way and round trip separately, three runs; speculation status recorded; a `perf stat` diagnostic run by Shep; published figures change only if S5 applies |
