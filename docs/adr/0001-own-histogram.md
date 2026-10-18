# 0001 — Own histogram instead of HdrHistogram

**Status:** Proposed · **Date:** 2026-10-18

## Context

Benchmarks need a latency histogram with bounded error, fixed memory and O(1) recording (design
note 002). HdrHistogram is the established design for this, with a C implementation
(HdrHistogram_c) and a C++ port. The alternatives:

1. **Use HdrHistogram_c**, fetched by CMake into `build/`.
2. **Write our own** log-linear histogram, the same idea cut down to what the notes need.
3. **Store every sample and sort**, as the clock overhead does.

Option 3 is ruled out by size: ten minutes at 1 million messages a second is 4.8 GB.

## Decision

Write our own (option 2), following HdrHistogram's bucket scheme.

- The measurement harness stays free of third-party code. Third-party code in this repo is
  limited to baselines being compared against.
- The hot path must be checkable the way note 001's is: no allocation, system call or page fault
  in `record()`, verified in tests and in the compiled code. That is easier to guarantee in about
  200 lines we own than in a general library with features we do not use (auto-resizing,
  coordinated-omission correction, compressed log formats).
- The error bound and percentile rule (upper bucket edge, null below 10 samples) are ours to
  specify and test exactly.

## Consequences

- Easier: one dependency-free header; every behaviour is in the Spec and tested.
- Harder: we carry the correctness risk HdrHistogram has already paid down. The exhaustive and
  sorted-copy tests in note 002 are what pay it down here.
- Cost: a few hundred lines and their tests. No HdrHistogram log-format compatibility: our raw
  buckets are in our own JSON.
