// Cost of a start()/stop() pair. Design: docs/design/001-clock.md
#pragma once

#include <lfipc/bench/clock.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <sched.h>
#include <vector>

namespace lfipc::bench {

// Nearest-rank percentile of sorted data, p in (0, 100].
inline ticks percentile_sorted(const std::vector<ticks>& sorted, double p) {
    if (sorted.empty()) return 0;
    // p * n first, and a small guard: 99.9 / 100 * 1000 is 999.0000000000001 in
    // floating point, and a bare ceil() would step to the next rank.
    const double exact = p * static_cast<double>(sorted.size()) / 100.0;
    auto rank = static_cast<std::size_t>(std::ceil(exact - 1e-9));
    rank = std::clamp<std::size_t>(rank, 1, sorted.size());
    return sorted[rank - 1];
}

struct overhead {
    int cpu = -1;
    std::size_t samples = 0;
    std::size_t warmup_discarded = 0;
    ticks p50 = 0, p99 = 0, p999 = 0, max = 0;
};

inline overhead summarise_overhead(std::vector<ticks> deltas) {
    std::sort(deltas.begin(), deltas.end());
    overhead o;
    o.samples = deltas.size();
    o.p50 = percentile_sorted(deltas, 50.0);
    o.p99 = percentile_sorted(deltas, 99.0);
    o.p999 = percentile_sorted(deltas, 99.9);
    o.max = deltas.empty() ? 0 : deltas.back();
    return o;
}

// Pins the calling thread to the CPU it is on (or to `cpu` if given), runs
// `warmup` discarded pairs, then records `samples` back-to-back pairs.
// Storage is allocated before measuring.
inline overhead measure_overhead(std::size_t samples = 1'000'000, std::size_t warmup = 100'000,
                                 int cpu = -1) {
    if (cpu < 0) cpu = sched_getcpu();
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(cpu, &set);
    sched_setaffinity(0, sizeof(set), &set);

    std::vector<ticks> deltas(samples);
    for (std::size_t i = 0; i < warmup; ++i) {
        const ticks a = start();
        const ticks b = stop();
        deltas[i % samples] = b - a;
    }
    for (std::size_t i = 0; i < samples; ++i) {
        const ticks a = start();
        const ticks b = stop();
        deltas[i] = b - a;
    }
    auto o = summarise_overhead(std::move(deltas));
    o.cpu = cpu;
    o.warmup_discarded = warmup;
    return o;
}

}  // namespace lfipc::bench
