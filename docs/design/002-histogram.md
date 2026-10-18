# 002 — Histogram

**Status:** Agreed · **Updated:** 2026-10-18

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
  "count": 0,
  "overflow": 0,
  "min_ns": 0, "max_ns": 0,
  "percentiles_ns": { "p50": 0, "p90": 0, "p99": 0, "p999": 0, "p9999": null, "p99999": null },
  "buckets": [ [0, 0] ]
}
```

`buckets` lists only non-empty buckets, as `[upper edge in ticks, count]`. Ticks, not ns, so
re-analysis uses the run's own `ticks_per_ns` from the clock block. Values shown are placeholders.

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
- [ ] `include/lfipc/bench/histogram.hpp`: record, merge, percentiles
- [ ] JSON `histogram` block
- [ ] Tests S1–S6, S8: exhaustive check of every value below 2^20 for S2; random and bimodal
      distributions against a sorted copy; merge; page faults counted with `getrusage`
- [ ] Record cost (S7) under bench-on; results reviewed here

## Review

| Date | Stage | Finding |
|---|---|---|
| 2026-10-18 | Draft | Written for review |
| 2026-10-18 | Agreed | Approved by Shep without changes: k = 10, upper-edge percentiles, 10-sample rule, own implementation (ADR 0001 accepted). Wording of the 10-sample rule aligned with its formula ("above", not "at or above") |
