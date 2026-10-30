# Open items

Every item a note's Review leaves open, one row each. Each gets one decision: **fix** (a task;
closed by its commit), **limit** (stated where it qualifies a figure; closed) or **parked**
(changes no published conclusion; closed, with what would reopen it). A note does not reach
Measured while an item it raised has no decision.

Decided with Shep 2026-10-30: every item below as written.

| # | Item | Note | Touches a published figure | Decision |
|---|---|---|---|---|
| L1 | Cross-socket round trips run at two speeds (~100 ns apart), changing run to run; cross-CPU clock convergence unstable across sockets | 001 | No: no cross-socket figure is published | parked: reopen when a measured path crosses sockets |
| L2 | Histogram: the realistic p99.9 is ~19 ns above hot | 002 | `record()` cost, p99.9 only | parked: reopen if `record()` changes |
| L3 | SPSC cross-core p50 shifts run to run (median 27 ns), not turbo | 006 | SPSC p50 | limit: published as ranges over runs |
| L4 | Siblings: the rest of the round-trip mechanism and the layout effect's (~6 ns) | 012 | Siblings section | parked: reopen if a deployment puts a queue's ends on one core |
| L5 | rtla counts ~1,000 IRQ/s in its one-sibling run (noise_bench sees none) | 005 | No | parked: diagnostic only |
