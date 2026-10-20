# lockfree-ipc

Lock-free SPSC/MPMC queues and shared-memory transport for Linux, with a measurement harness
that reports latency percentiles against the machine's own noise floor.

Work in progress. Written from scratch against public documentation.

## Results

<!-- results:spsc-summary -->
| Queue | Rate | p50 | p99 | p99.9 |
|---|---|---|---|---|
| **v3** | 1M/s | 162–184 | 214–265 | 244–307 |
| **v3** | 2M/s | 169–184 | 214–249 | 241–260 |
| moodycamel | 1M/s | 223–243 | 289–332 | 311–351 |
| moodycamel | 2M/s | 223–298 | 286–345 | 309–366 |
| Boost | 1M/s | 194–223 | 248–293 | 254–304 |
| Boost | 2M/s | 196–224 | 233–256 | 251–288 |

One-way latency in ns, CPUs 2 → 3 (separate cores), range over 3 runs. Full tables and conditions: [RESULTS.md](RESULTS.md).
<!-- /results -->

<!-- results:noise-floor -->
Noise floor on the reserved CPUs: at most 0.0003% of time stopped, longest stop 23.1 µs.
<!-- /results -->
