# 011 — SPSC v4: padded slots

**Status:** Agreed · **Updated:** 2026-10-20

## Proposal

Note 010 found that padding each MPMC slot to its own 128 bytes (v2) beat packed slots in every
shape, 1 → 1 included, where no second producer exists. The likely cause: packed, the consumer
reading slot k and the producer writing k + 1 share a cache line whenever the queue is nearly
empty. SPSC v3 packs its 16-byte slots four to a line, so it may pay the same cost. v4 is v3 with
each slot padded to 128 bytes, measured against v3 with note 006's harness. A follow-up to notes
006 and 010; it changes nothing else.

The prediction is uncertain, and the reason is worth stating. MPMC's consumer polls the slot's
own sequence number, on the slot's line, so a packed neighbour is contended on every poll. SPSC
v3's consumer polls the producer's counter, on its own line, and with cached counters only when
its copy says the ring is empty. Its slot lines are touched once per message on each side. v4 may
therefore gain less than MPMC v2 did, or nothing; and packing has a benefit padding loses: when
the consumer falls behind, one line transfer carries up to four messages.

It does:

- add v4 as a named variant of `spsc_ring`, v1–v3 unchanged;
- measure it beside v1–v3 and the baselines, same pairs, rates and modes;
- if it wins, make it `spsc_ring`'s default, and carry the layout change into shared memory.

It does not:

- try 64-byte slot padding as well (a second variant to separate the adjacent-line prefetcher's
  part; only if v4's result calls for it);
- re-measure note 008's shared-memory figures or note 010's 1 → 1 reference, which were taken with
  v3 and stay labelled so.

## Design

**Layout.** `spsc_ring` gains a `SlotAlign` parameter, as `mpmc_ring` has: 0 packs the slots
(v1–v3), 128 gives each its own 128 bytes. For the 16-byte message a 1,024-slot ring grows from
16 KB to 128 KB.

| Variant | Change |
|---|---|
| `v4` | v3 with each slot on its own 128 bytes |

**Measurement.** `spsc_bench` gains v4 and the whole SPSC set is re-measured: three runs, published
together, since a set shares one commit (note 007). Pairs 2 → 3 and 2 → 46, rates 100k–10M/s,
one-way and round trip in separate runs, as in note 006. About 25 minutes a run.

**If v4 is chosen.** `spsc_ring`'s default becomes v4. The shared-memory header (note 008) records
`separation` but not slot alignment, so a v3 segment and a v4 attacher with the same separation
would pass validation with different layouts. The header gains `slot_align`, its `version` goes
to 2, and a mismatch is refused with both values named, like the other fields. If v3 stays, none
of this is needed.

## Spec

- S1. v4 passes note 006's S1 and S2 tests: the two-thread stress test with 8- and 64-byte
  messages, natively, under TSan and ASan, and on ARM in CI; full and empty exact across the 2^64
  wrap.
- S2. v4's `try_push` and `try_pop` pass note 006's S3 check (no call, lock or fence).
- S3. v4 is standard-layout; its size is 256 bytes plus 128 per slot.
- S4. v4 is measured beside v1–v3 and the baselines in one `spsc_bench`, at least three runs,
  published via `run.py report`.
- S5. If chosen: `spsc_ring`'s default is v4 (checked at compile time), the shared-memory header
  records slot alignment, and a slot-alignment mismatch on attach is refused naming both values.

## Tasks

- [ ] `SlotAlign` in `spsc_ring`, v4 alias; S1–S3 tests and the hot-path subject
- [ ] v4 in `spsc_bench`, `queues.hpp` and `run.py`
- [ ] Three bench-on runs; choice recorded here
- [ ] If v4 is chosen: default, shared-memory header version 2, S5 test

## Review

| Date | Stage | Finding |
|---|---|---|
| 2026-10-20 | Draft | Written for review |
| 2026-10-20 | Agreed | Approved by Shep: v4 = v3 with each slot on its own 128 bytes via a `SlotAlign` parameter; whole SPSC set re-measured with v4, three runs; if chosen, the default and a shared-memory header version 2 with `slot_align`; earlier v3 figures stay labelled v3; no 64-byte variant unless the result calls for it |
