# lockfree-ipc

Bounded lock-free queues for C++20 on Linux (single-producer, multi-producer, and across processes
through shared memory), and the measurement harness that produced every figure below. Each
design choice was measured against the one before it and against established queues; the losing
variants stay in the repository with their numbers. Written from scratch against public
documentation.

## Results

One-way latency from each message's intended send time, in ns, as ranges over three runs on a
dedicated, isolated machine (conditions below). Full tables, round trips, refusals and every
run's conditions: [RESULTS.md](RESULTS.md). These blocks are generated from the committed JSON in
`results/` by `python run.py report`; the test suite fails if they disagree.

<!-- results:spsc-summary -->
| Queue | Rate | p50 | p99 | p99.9 |
|---|---|---|---|---|
| **v3** | 1M/s | 156–167 | 229–241 | 250–264 |
| **v3** | 2M/s | 168–187 | 203–223 | 240–257 |
| moodycamel | 1M/s | 215–299 | 274–342 | 301–360 |
| moodycamel | 2M/s | 218–271 | 279–320 | 298–345 |
| Boost | 1M/s | 189–211 | 219–287 | 281–294 |
| Boost | 2M/s | 182–205 | 267–289 | 274–297 |

One-way latency in ns, CPUs 2 → 3 (separate cores), range over 3 runs. Full tables and conditions: [RESULTS.md](RESULTS.md).
<!-- /results -->

<!-- results:mpmc-summary -->
| Queue | Producers → consumers | Rate | p50 | p99 | p99.9 |
|---|---|---|---|---|---|
| **v2** | 2 → 1 | 1M/s | 210–238 | 250–276 | 271–303 |
| **v2** | 2 → 1 | 2M/s | 205–220 | 244–252 | 258–275 |
| **v2** | 4 → 4 | 1M/s | 276–290 | 305–323 | 312–331 |
| **v2** | 4 → 4 | 2M/s | 282–307 | 322–349 | 347–378 |
| moodycamel | 2 → 1 | 1M/s | 296–313 | 602–658 | 682–720 |
| moodycamel | 2 → 1 | 2M/s | 271–300 | 595–638 | 701–719 |
| moodycamel | 4 → 4 | 1M/s | 400–466 | 752–833 | 955–1,030 |
| moodycamel | 4 → 4 | 2M/s | 415–456 | 790–883 | 1,012–1,044 |
| Boost | 2 → 1 | 1M/s | 461–468 | 498–594 | 505–598 |
| Boost | 2 → 1 | 2M/s | 376–471 | 499–645 | 529–820 |
| Boost | 4 → 4 | 1M/s | 618–673 | 896–957 | 975–1,037 |
| Boost | 4 → 4 | 2M/s | 734–830 | 1,307–1,510 | 1,625–2,002 |
| rigtorp | 2 → 1 | 1M/s | 217–243 | 254–301 | 303–320 |
| rigtorp | 2 → 1 | 2M/s | 200–228 | 247–285 | 263–308 |
| rigtorp | 4 → 4 | 1M/s | 268–274 | 295–351 | 301–390 |
| rigtorp | 4 → 4 | 2M/s | 258–269 | 306–348 | 324–390 |

MPMC one-way latency in ns at the aggregate rate, range over 3 runs; v2 is this repo's queue. moodycamel is measured through its token-free API. Full tables and conditions: [RESULTS.md](RESULTS.md).
<!-- /results -->

<!-- results:noise-floor -->
Noise floor on the reserved CPUs: at most 0.0002% of time stopped, longest stop 18.3 µs.
<!-- /results -->

## What is here

| | |
|---|---|
| [`spsc.hpp`](include/lfipc/spsc.hpp) | `spsc_ring<T, N>`: single producer, single consumer. Each side caches the other's counter; counters and slots 128 bytes apart. [Note 006](docs/design/006-spsc-queue.md), [011](docs/design/011-spsc-padded-slots.md) |
| [`mpmc.hpp`](include/lfipc/mpmc.hpp) | `mpmc_ring<T, N>`: Vyukov's bounded multi-producer, multi-consumer queue, each slot on its own 128 bytes. [Note 010](docs/design/010-mpmc-queue.md) |
| [`shm.hpp`](include/lfipc/shm.hpp) | `shm_spsc<T, N>`: the SPSC ring in a named shared-memory segment, with layout validation, stale-segment recovery and peer liveness. [Note 008](docs/design/008-shm-transport.md) |
| [`bench/`](include/lfipc/bench) | The harness: TSC clock, log-linear histogram, open-loop load generator, environment capture, noise-floor meter. [Notes 001–005](docs/README.md) |
| [`run.py`](run.py) | Validates published runs and generates RESULTS.md and the blocks above. [Note 007](docs/design/007-results-report.md) |

Header-only; `try_push` and `try_pop` never block, allocate or make a system call, and the
compiled code is checked for that in the test suite (no call, fence or locked instruction in the
SPSC ring; exactly one `lock cmpxchg` in the MPMC ring).

## Findings

- **SPSC.** Cached counters and 128-byte separation (Broadwell's adjacent-line prefetcher fetches
  lines in pairs) give the lowest or tied latency on separate cores against moodycamel's
  `ReaderWriterQueue` and Boost's `spsc_queue`. On hyper-thread siblings Boost's queue is a few ns
  faster. Padding each slot as well helps only at 10M messages/s, and hurts on siblings, where the
  larger ring no longer fits the shared L1 (note 011).
- **MPMC.** Padding each slot beats packing in every shape, one producer and one consumer
  included: packed, the consumer polling a slot shares its line with the slot being written next.
  At 1 → 1 it matches the SPSC ring's median. It is not lock-free in the strict sense: a producer
  stalled between claiming a slot and publishing it holds that slot. Against rigtorp's
  `MPMCQueue` (the same algorithm) neither dominates; moodycamel's `ConcurrentQueue` (measured
  without producer tokens) and Boost's `queue` are slower (note 010).
- **Processes.** Across two processes on separate cores the ring costs nothing measurable over
  two threads one-way, and a little round trip. On hyper-thread siblings, one-way the difference is mostly where the ring sits in
  its page; round trip, the second address space, with more memory-ordering machine clears and
  TLB misses explaining part of it (notes 008, 012).
- **The harness, too, was measured.** Its first queue results were wrong twice: no benchmark
  controlled NUMA placement (the queues sat on the far node), and the generator's own producer and
  consumer results shared a cache line (note 008, Review). Later, a FIX codec benchmark in a
  sister repository ran twice as slow as its code: the machine setup had disabled every idle state
  but polling, so each measured CPU's idle hyper-thread sibling spun and took half the core
  (note 004, Review). Each was found from the data, fixed, and every set re-measured.

## How it is measured

- **Open loop.** Messages are sent on a fixed schedule; latency runs from each message's intended
  send time, so a stall delays every message due during it (no coordinated omission). A run is
  refused if the generator falls behind its own schedule (note 003).
- **Clock.** The TSC, read with `lfence`/`rdtscp` fencing, calibrated against
  `CLOCK_MONOTONIC_RAW` each run; cross-CPU offsets bounded and published with every one-way
  figure (note 001).
- **Percentiles only**, from a log-linear histogram, after a discarded warm-up; one-way and round
  trip in separate runs; ranges over at least three runs (notes 002, 006).
- **The machine.** HPE ProLiant DL380 Gen9, 2 × Xeon E5-2699 v4, Ubuntu 26.04, kernel 7.0.
  Benchmarks run on node 0's CPUs 2–21 and their siblings, isolated at boot (`nohz_full`,
  `isolcpus`), with services, kernel threads and interrupts fenced off them, the performance
  governor, idle states deeper than C1 off (so idle hyper-threads halt rather than spin), the uncore clock pinned, automatic NUMA balancing off, and every
  benchmark's memory bound to the measured node. Periodic firmware SMIs were removed in the BIOS.
  The noise floor is measured in every run (note 005). The setup scripts: [tools/machine](tools/machine).
- **Every run records its conditions** (hardware, microcode, kernel command line, power, fence,
  placement, build commit and flags) and is refused for publication unless they are met and the
  build is clean (note 004).

## Build and test

Linux; C++20; tested with GCC 15.2, CMake 4.2 and Python 3.14 (3.11 or later for `run.py`).

```sh
cmake -S . -B build -G Ninja
cmake --build build
ctest --test-dir build
```

The baseline queues (moodycamel, Boost.Lockfree, rigtorp) are fetched into `build/` by CMake,
pinned by commit or checksum; `-DLFIPC_BASELINES=OFF` builds without them. The test suite also
runs under ThreadSanitizer and AddressSanitizer/UBSan, and on aarch64 (correctness only: the
clock falls back to `steady_clock` and refuses to publish), in CI on every push. Checks that need
a quiet, dedicated machine carry the CTest label `quiet` (`ctest -LE quiet` skips them).

## Running the benchmarks

```sh
build/spsc_bench out.json 10      # every SPSC queue, both CPU pairs, 100k-10M msg/s, 10 s per rate
build/mpmc_bench out.json 10      # every MPMC queue and shape
```

Each writes one JSON file: the environment, the noise floor, the clock, then the results. Run
without a path, a benchmark writes into `results/`, and refuses unless the machine is in bench
mode and the build is clean; given a path elsewhere, it runs anywhere and marks the file
unpublished. The CPU numbers are this machine's (node 0, CPUs 2–21 reserved); another machine
needs them changed in the benchmark source. To publish runs: list them in
`results/published.toml`, then `python run.py report`.

## Design notes

Every component has a design note written and agreed before its code: proposal, design, spec,
tasks, and a review log with every finding, including the wrong turns. Index:
[docs/README.md](docs/README.md).

## Licence

[MIT](LICENSE).
