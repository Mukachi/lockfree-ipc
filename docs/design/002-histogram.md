# 002 — Histogram

**Status:** Measured · **Updated:** 2026-10-18

## Proposal

Every benchmark records one latency per message and publishes percentiles. A run at 1 million
messages a second for ten minutes is 600 million samples: 4.8 GB as a sorted array. The histogram
records each sample in constant time and fixed memory, with a known worst-case error, so long runs
and deep tails (p99.99 and beyond) can be published honestly.

It does:

- record a value in ticks in O(1), with no allocation, system call or page fault on the hot path;
- keep min, max and count exactly;
- bound the error of every reported percentile, and report it on the safe side (never below the
  true value);
- merge per-thread histograms after a run, so recording needs no atomics;
- publish a JSON block: percentiles in ns, and the raw buckets so a run can be re-analysed or
  merged later.

It does not:

- correct for coordinated omission. That belongs to the open-loop load generator, which records
  latency from each message's intended send time;
- convert to ns while recording (conversion happens at report time, as in note 001);
- record from several threads into one histogram. One histogram per thread, merged afterwards.

## Design

**Buckets: log-linear, as in HdrHistogram.** Values below 2^k ticks get one bucket each, so they
are exact. Above that, each power of two is split into 2^(k−1) equal buckets. The bucket width
is then at most 2^(1−k) of the value, which bounds the relative error.

| k | Exact up to | Worst error above | Counters (to 2^40 ticks) | Memory |
|---|---|---|---|---|
| 8 | 256 ticks (116 ns) | 0.78% | 4,352 | 34 KB |
| **10** | **1,024 ticks (466 ns)** | **0.20%** | **16,384** | **128 KB** |
| 11 | 2,048 ticks (932 ns) | 0.10% | 31,744 | 248 KB |

Proposed default: **k = 10**. Same-socket queue latencies (a few hundred ns) are recorded exactly;
the tail is within 0.2%. 128 KB is half of the E5-2699 v4's 256 KB L2, but recording touches only
the cache lines where values fall, which in a latency run is a small cluster. k = 11 would fill
L2. `k` and the range are template parameters, so a benchmark can choose differently and the
JSON records which it used.

**Range.** Up to 2^40 ticks (about 500 s on dev-lnx01). Larger values go to an overflow counter;
count and max stay exact, so nothing is lost.

**Recording.** Index from the position of the top set bit (`std::countl_zero`) and the next
k−1 bits, then one increment. No branches apart from the overflow clamp, and no division.

**Memory.** Counters are allocated and written once at construction, so every page is mapped
before the first sample. A freshly allocated zero page is mapped on first write, and that fault
would land in the measured path.

**Percentiles.** Nearest rank, as in note 001's overhead. The reported value is the bucket's upper
edge, so it is never below the true percentile and at most 0.2% above it (k = 10). A percentile is
published only if at least 10 samples lie above its rank, i.e. count × (1 − p) ≥ 10;
otherwise it is `null`. A p99.99 from 5,000 samples is one sample, not a percentile.

Published set: min, p50, p90, p99, p99.9, p99.99, p99.999, max. Never a mean.

**Merging.** Counter-by-counter addition; min, max, count and overflow combine exactly. Requires
the same k and range.

**Output.**

```json
"histogram": {
  "sub_bucket_bits": 10,
  "max_bits": 40,
  "count": 0,
  "overflow": 0,
  "min_ns": 0, "max_ns": 0,
  "percentiles_ns": { "p50": 0, "p90": 0, "p99": 0, "p999": 0, "p9999": null, "p99999": null },
  "buckets": [ [0, 0] ]
}
```

`buckets` lists only non-empty buckets, as `[upper edge in ticks, count]`. Ticks, not ns, so
re-analysis uses the run's own `ticks_per_ns` from the clock block. Values shown are placeholders.

**Record cost (S7).** `record()` costs a few ns, below the clock's ~21 ns read overhead, so a
single call cannot be timed on its own, and note 001 forbids subtracting the overhead. Instead
`bench/histogram_bench` times `start(); record(v); stop()` and, in the same loop on the same CPU,
`start(); stop()` with no record. Both distributions are published side by side, unsubtracted.
1,000,000 samples per case after 100,000 warm-up, pinned to CPU 2 (as note 001's overhead),
values generated before timing. Three cases:

| Case | Values | What it shows |
|---|---|---|
| `hot` | always 300 ticks | one bucket, always in L1 |
| `realistic` | lognormal, median 900 ticks, σ 0.3 | latency-like spread straddling 2^k = 1,024 ticks |
| `worst` | uniform over 0 … 2^40 | a different bucket each time |

The whole histogram (128 KB) fits in the 256 KB L2, so `worst` measures L1 misses served from L2,
not misses to memory. The run's JSON also carries the full `clock` block (note 001, S3).

**Why not HdrHistogram itself:** see ADR 0001.

## Spec

- S1. `record()` makes no allocation and no system call, and causes no page fault after
  construction.
- S2. Every reported percentile is ≥ the exact nearest-rank value from a sorted copy, and at most
  2^(1−k) above it. Values below 2^k are exact.
- S3. Min, max, count and overflow are exact, including values beyond the range.
- S4. Merging histograms gives the same result as recording every sample into one.
- S5. A percentile with fewer than 10 samples above its rank is published as `null`.
- S6. No mean is published anywhere.
- S7. The cost of `record()` is measured and published as percentiles, like the clock's overhead.
- S8. Compiles for aarch64 with warnings as errors.

## Tasks

- [x] ADR 0001: own histogram vs HdrHistogram
- [x] `include/lfipc/bench/histogram.hpp`: record, merge, percentiles
- [x] JSON `histogram` block
- [x] Tests S1–S6, S8: exhaustive check of every value below 2^20 for S2; random and bimodal
      distributions against a sorted copy; merge; page faults counted with `getrusage`
- [x] Record cost (S7) under bench-on; results reviewed here

## Review

| Date | Stage | Finding |
|---|---|---|
| 2026-10-18 | Draft | Written for review |
| 2026-10-18 | Agreed | Approved by Shep without changes: k = 10, upper-edge percentiles, 10-sample rule, own implementation (ADR 0001 accepted). Wording of the 10-sample rule aligned with its formula ("above", not "at or above") |
| 2026-10-18 | Built (partial) | Header and tests. S2 checked on every value below 2^20, both sides of every power of two to 2^40, 1M random values, and percentiles against a sorted copy on uniform, lognormal and bimodal data (2M samples each). S1: 0 minor page faults recording 2M values across all buckets; the same meter counts 22 on counters allocated but never written, so it can see faults. Disassembly showed GCC turned the index's `max()` into two branches (a zero check before `bsr`, and a shortcut below 2^k). The second would mispredict when values straddle 1,024 ticks (466 ns), which is where cross-socket latencies sit. Fixed by taking the bit width of `v | 2^(k−1)`; `check_hot_path.py` now requires `record()` to have no call and exactly one conditional branch (overflow). `nearest_rank` moved to `rank.hpp`, shared with note 001's overhead |
| 2026-10-18 | Built (partial) | JSON block built (`histogram_block.hpp`). Added `max_bits` to the fields: without the range a reader cannot tell where overflow starts or whether two histograms can be merged. Buckets written one per line; 2M lognormal samples give 4,500 lines. `check_histogram_json.py` checks exact fields, S5 against the rank rule, buckets plus overflow equal count, and S6 (no key like mean/avg/average) across the histogram and clock files; it fails on a doctored block with `mean_ns` and a wrong null. JSON writer gained `null`, `optional` and one-line arrays; the clock's output is unchanged |
| 2026-10-18 | Agreed | Record-cost method agreed with Shep: side by side with the bare clock read, unsubtracted, not batched (batching would publish averages); three value cases |
| 2026-10-18 | Built | Record cost built: `record_cost.hpp`, `bench/histogram_bench`. Disassembly of the timed loop: between the reads, only the inlined `record()` (count, min/max by `cmov`, the overflow branch, index, increment); the value is loaded before `start()` and the delta stored after the closing fence. Short run, not bench-on, CPU 1: bare read p50 56 ticks; with `record()` hot 64, realistic 64 (p99.9 132), worst 68 (p99.9 144). All tasks but the reviewed bench-on run done |
| 2026-10-18 | Measured | Reviewed run: `results/histogram-dev-lnx01-20261018T201320Z.json`. dev-lnx01, bench-on (uncore pinned), CPU 1, 1,000,000 samples per case, 100,000 warm-up discarded, Release build. Bare `start(); stop()`: p50 21.4 ns, p99 23.2, p99.9 24.1, max 5.5 µs, identical to the clock block's overhead from the same run (two separate loops agree). With `record()`: hot p50 24.6 / p99 27.3 / p99.9 28.7 ns; realistic 24.6 / 27.3 / 42.8; worst 25.5 / 30.0 / 48.2. `record()` adds about 3 ns (7 ticks) at p50, 4–7 ns at p99. The worst-case p99.9 (+24 ns) is L1 misses served from L2, as predicted. The realistic p99.9 (+19 ns) is unexplained: its few hundred buckets should stay in L1. Open, small. Spec: S1–S8 pass (S6 also checked on this results file) |
| 2026-10-18 | Measured | Finding (note 004's reviewed run, `results/clock-dev-lnx01-20261018T215708Z.json`): with the CPU fence, CPU 1 is a housekeeping CPU and carries every service, kernel thread and movable interrupt; overhead max rose to 3.0 ms (p50–p99.9 unchanged at 21.4 / 23.2 / 24.1 ns). Measuring on CPU 1 no longer avoids interrupt load, it maximises it. Earlier results predate the fence and stand. Change of CPU proposed to Shep |
| 2026-10-18 | Measured | Approved by Shep: record cost moved from CPU 1 to CPU 2 with note 001's overhead. The reviewed run was on CPU 1 and stands as recorded |
