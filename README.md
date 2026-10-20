# lockfree-ipc

Lock-free SPSC/MPMC queues and shared-memory transport for Linux, with a measurement harness
that reports latency percentiles against the machine's own noise floor.

Work in progress. Written from scratch against public documentation.

## Results

<!-- results:spsc-summary -->
| Queue | Rate | p50 | p99 | p99.9 |
|---|---|---|---|---|
| **v3** | 1M/s | 162–190 | 226–251 | 256–267 |
| **v3** | 2M/s | 160–168 | 246–257 | 259–297 |
| moodycamel | 1M/s | 219–223 | 286–293 | 301–342 |
| moodycamel | 2M/s | 218–225 | 284–289 | 303–340 |
| Boost | 1M/s | 200–226 | 249–281 | 256–292 |
| Boost | 2M/s | 189–223 | 253–278 | 261–295 |

One-way latency in ns, CPUs 2 → 3 (separate cores), range over 3 runs. Full tables and conditions: [RESULTS.md](RESULTS.md).
<!-- /results -->

<!-- results:mpmc-summary -->
| Queue | Producers → consumers | Rate | p50 | p99 | p99.9 |
|---|---|---|---|---|---|
| **v2** | 2 → 1 | 1M/s | 213–242 | 255–281 | 269–303 |
| **v2** | 2 → 1 | 2M/s | 211–233 | 252–270 | 272–299 |
| **v2** | 4 → 4 | 1M/s | 294–319 | 327–351 | 334–358 |
| **v2** | 4 → 4 | 2M/s | 270–287 | 313–331 | 327–355 |
| moodycamel | 2 → 1 | 1M/s | 320–327 | 624–641 | 696–745 |
| moodycamel | 2 → 1 | 2M/s | 310–330 | 596–626 | 659–739 |
| moodycamel | 4 → 4 | 1M/s | 438–458 | 810–865 | 982–1,074 |
| moodycamel | 4 → 4 | 2M/s | 406–459 | 743–867 | 890–1,064 |
| Boost | 2 → 1 | 1M/s | 381–477 | 480–543 | 487–591 |
| Boost | 2 → 1 | 2M/s | 401–443 | 517–679 | 586–818 |
| Boost | 4 → 4 | 1M/s | 600–690 | 837–952 | 933–1,041 |
| Boost | 4 → 4 | 2M/s (2/3 runs) | 708–820 | 1,194–1,813 | 1,410–2,781 |
| rigtorp | 2 → 1 | 1M/s | 217–248 | 306–318 | 318–338 |
| rigtorp | 2 → 1 | 2M/s | 212–232 | 278–311 | 314–329 |
| rigtorp | 4 → 4 | 1M/s | 270–286 | 318–372 | 338–403 |
| rigtorp | 4 → 4 | 2M/s | 267–275 | 326–340 | 351–374 |

MPMC one-way latency in ns at the aggregate rate, range over 3 runs; v2 is this repo's queue. moodycamel is measured through its token-free API. Full tables and conditions: [RESULTS.md](RESULTS.md).
<!-- /results -->

<!-- results:noise-floor -->
Noise floor on the reserved CPUs: at most 0.0003% of time stopped, longest stop 23.1 µs.
<!-- /results -->
