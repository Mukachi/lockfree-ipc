# lockfree-ipc

Lock-free SPSC/MPMC queues and shared-memory transport for Linux, with a measurement harness
that reports latency percentiles against the machine's own noise floor.

Work in progress. Written from scratch against public documentation.

## Results

<!-- results:spsc-summary -->
| Queue | Rate | p50 | p99 | p99.9 |
|---|---|---|---|---|
| **v3** | 1M/s | 157–187 | 220–249 | 260–272 |
| **v3** | 2M/s | 159–175 | 208–251 | 252–276 |
| moodycamel | 1M/s | 175–199 | 252–259 | 265–281 |
| moodycamel | 2M/s | 218–287 | 292–359 | 342–380 |
| Boost | 1M/s | 225–249 | 255–299 | 271–312 |
| Boost | 2M/s | 228–243 | 259–300 | 280–309 |

One-way latency in ns, CPUs 2 → 3 (separate cores), range over 3 runs. Full tables and conditions: [RESULTS.md](RESULTS.md).
<!-- /results -->

<!-- results:noise-floor -->
Noise floor on the reserved CPUs: at most 0.0003% of time stopped, longest stop 23.8 µs.
<!-- /results -->
