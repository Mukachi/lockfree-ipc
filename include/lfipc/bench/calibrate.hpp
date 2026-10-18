// TSC-to-nanosecond calibration. Design: docs/design/001-clock.md
#pragma once

#include <lfipc/bench/clock.hpp>

#include <algorithm>
#include <ctime>
#include <string>
#include <vector>

namespace lfipc::bench {

struct calibration {
    double ticks_per_ns = 0.0;   // median across windows
    double spread_ppm = 0.0;     // (max - min) / median, in parts per million
    std::vector<double> window_rates;
    bool ok = false;
    std::string reason;          // empty when ok
};

inline constexpr int default_calibration_windows = 10;
inline constexpr long default_window_ms = 100;
inline constexpr double default_max_spread_ppm = 10.0;

// The decision alone, from per-window rates, so the threshold logic is testable.
inline calibration evaluate_calibration(std::vector<double> rates, double max_spread_ppm) {
    calibration c;
    c.window_rates = rates;
    if (rates.empty()) {
        c.reason = "no calibration windows";
        return c;
    }
    std::sort(rates.begin(), rates.end());
    const std::size_t n = rates.size();
    c.ticks_per_ns = n % 2 ? rates[n / 2] : (rates[n / 2 - 1] + rates[n / 2]) / 2.0;
    if (c.ticks_per_ns <= 0.0) {
        c.reason = "non-positive tick rate";
        return c;
    }
    c.spread_ppm = (rates.back() - rates.front()) / c.ticks_per_ns * 1e6;
    if (c.spread_ppm > max_spread_ppm) {
        c.reason = "calibration spread " + std::to_string(c.spread_ppm) + " ppm exceeds " +
                   std::to_string(max_spread_ppm) + " ppm";
        return c;
    }
    c.ok = true;
    return c;
}

namespace detail {

struct paired_read {
    ticks tsc;     // midpoint of the two TSC reads around clock_gettime
    long long ns;  // CLOCK_MONOTONIC_RAW
};

inline long long raw_ns() noexcept {
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC_RAW, &ts);
    return static_cast<long long>(ts.tv_sec) * 1'000'000'000LL + ts.tv_nsec;
}

// Bracket clock_gettime between two TSC reads and keep the tightest of several
// attempts, so an interrupt during one attempt does not skew the pairing.
inline paired_read pair_reads(int attempts = 16) noexcept {
    paired_read best{};
    ticks best_width = ~ticks{0};
    for (int i = 0; i < attempts; ++i) {
        const ticks a = start();
        const long long ns = raw_ns();
        const ticks b = stop();
        if (b - a < best_width) {
            best_width = b - a;
            best = {a + (b - a) / 2, ns};
        }
    }
    return best;
}

}  // namespace detail

// Measures ticks per nanosecond over several windows, sleeping through each.
// Sleeping lets the core enter idle states, which an invariant TSC must count through.
inline calibration calibrate(int windows = default_calibration_windows,
                             long window_ms = default_window_ms,
                             double max_spread_ppm = default_max_spread_ppm) {
    std::vector<double> rates;
    rates.reserve(static_cast<std::size_t>(windows));
    const timespec pause{window_ms / 1000, (window_ms % 1000) * 1'000'000L};
    for (int w = 0; w < windows; ++w) {
        const auto p0 = detail::pair_reads();
        nanosleep(&pause, nullptr);
        const auto p1 = detail::pair_reads();
        rates.push_back(static_cast<double>(p1.tsc - p0.tsc) / static_cast<double>(p1.ns - p0.ns));
    }
    auto c = evaluate_calibration(std::move(rates), max_spread_ppm);
    if (c.ok && !clock_is_tsc) {
        c.ok = false;
        c.reason = "not x86-64: steady_clock fallback produces no published measurements";
    }
    return c;
}

}  // namespace lfipc::bench
