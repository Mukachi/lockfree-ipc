// Noise-floor meter: how often, and for how long, a thread spinning alone on a
// CPU is stopped. Design: docs/design/005-noise-floor.md
#pragma once

#include <lfipc/bench/clock.hpp>
#include <lfipc/bench/cross_cpu.hpp>  // detail::pin_to
#include <lfipc/bench/histogram.hpp>

#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <thread>
#include <vector>

namespace lfipc::bench {

inline constexpr double default_noise_threshold_ns = 1000.0;
inline constexpr std::size_t default_stop_capacity = 1 << 20;

struct stop_event {
    ticks at;      // the read before the gap
    ticks length;  // gap between consecutive reads
};

struct noise_result {
    bool ok = false;
    std::string reason;
    int cpu = -1;
    ticks window = 0;           // spin time, in ticks
    ticks threshold = 0;        // a gap longer than this is a stop
    std::uint64_t iterations = 0;
    ticks iteration_min = 0;    // shortest gap seen: the loop's own cost (S2)
    std::uint64_t stops = 0;    // every stop, including any beyond capacity (S3)
    ticks stopped = 0;          // total length of all stops
    std::vector<stop_event> events;  // the first `capacity` stops, in order
    bool overflow = false;      // more stops than capacity (S3)
    histogram<> lengths;        // every stop's length
};

namespace detail {

struct no_hook {
    void operator()(std::uint64_t) const noexcept {}
};

// The measured loop (S5: no call, no system call, no allocation; checked in
// the compiled code). Reads the TSC with stamp(), the cheapest read, and
// records every gap above the threshold. `events` is pre-sized and pre-faulted
// by the caller. The hook runs once per iteration; tests use it to inject a
// stop of known length (S4); the default compiles away.
template <typename Hook>
[[gnu::noinline]] void spin_window(noise_result& r, std::size_t capacity, Hook hook) noexcept {
    ticks prev = stamp();
    const ticks end = prev + r.window;
    ticks min_gap = std::numeric_limits<ticks>::max();
    std::uint64_t n = 0, stops = 0;
    ticks stopped = 0;
    while (prev < end) {
        hook(n);
        const ticks now = stamp();
        const ticks gap = now - prev;
        min_gap = gap < min_gap ? gap : min_gap;
        if (gap > r.threshold) [[unlikely]] {
            if (stops < capacity) r.events[stops] = {prev, gap};
            ++stops;
            stopped += gap;
            r.lengths.record(gap);
        }
        prev = now;
        ++n;
    }
    r.iterations = n;
    r.iteration_min = min_gap;
    r.stops = stops;
    r.stopped = stopped;
}

}  // namespace detail

// Spins on `cpu` for `seconds` on a thread of its own, so the caller's
// affinity is unchanged.
template <typename Hook = detail::no_hook>
void measure_noise(int cpu, double seconds, double ticks_per_ns, noise_result& r,
                   double threshold_ns = default_noise_threshold_ns,
                   std::size_t capacity = default_stop_capacity, Hook hook = {}) {
    r.ok = false;
    r.reason.clear();
    r.cpu = cpu;
    r.window = static_cast<ticks>(seconds * 1e9 * ticks_per_ns);
    r.threshold = static_cast<ticks>(threshold_ns * ticks_per_ns);
    r.events.assign(capacity, stop_event{0, 0});  // written now, so pages are mapped
    r.lengths = histogram<>();
    bool pinned = false;
    std::thread t([&] {
        pinned = detail::pin_to(cpu);
        if (pinned) detail::spin_window(r, capacity, hook);
    });
    t.join();
    if (!pinned) {
        r.reason = "cannot pin to CPU " + std::to_string(cpu);
        r.events.clear();
        return;
    }
    r.overflow = r.stops > capacity;
    r.events.resize(r.stops < capacity ? r.stops : capacity);
    r.ok = true;
}

}  // namespace lfipc::bench
