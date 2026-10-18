// Cost of histogram record(), side by side with a bare clock read.
// Design: docs/design/002-histogram.md (S7)
#pragma once

#include <lfipc/bench/clock.hpp>
#include <lfipc/bench/cross_cpu.hpp>
#include <lfipc/bench/histogram.hpp>
#include <lfipc/bench/json.hpp>
#include <lfipc/bench/overhead.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <random>
#include <string>
#include <vector>

namespace lfipc::bench {

struct record_cost_case {
    std::string name;
    std::string values;  // how the values were generated
    overhead cost;       // percentiles of the timed window, in ticks
};

namespace detail {

// Times start(); [record(v);] stop() per sample. Values are loaded before
// start() and each delta is stored after stop(), so neither is in the window.
template <bool Record>
overhead time_window(histogram<>& h, const std::vector<ticks>& vals, std::size_t warmup) {
    const std::size_t n = vals.size();
    std::vector<ticks> deltas(n);
    for (std::size_t i = 0; i < warmup + n; ++i) {
        const std::size_t j = i % n;
        const ticks v = vals[j];
        const ticks a = start();
        if constexpr (Record) h.record(v);
        const ticks b = stop();
        deltas[j] = b - a;  // warm-up entries are overwritten by the measured pass
    }
    auto o = summarise_overhead(std::move(deltas));
    o.warmup_discarded = warmup;
    return o;
}

}  // namespace detail

inline std::vector<ticks> record_cost_values(const std::string& name, std::size_t n, std::uint64_t seed = 1) {
    std::mt19937_64 rng(seed);
    std::vector<ticks> v(n);
    if (name == "hot") {
        std::fill(v.begin(), v.end(), ticks{300});
    } else if (name == "realistic") {
        std::lognormal_distribution<double> ln(std::log(900.0), 0.3);
        for (auto& x : v) x = static_cast<ticks>(ln(rng));
    } else {  // worst
        for (auto& x : v) x = rng() >> (64 - histogram<>::max_bits);
    }
    return v;
}

// The bare read first, then each record case, all on `cpu`. Empty if `cpu`
// cannot be pinned.
inline std::vector<record_cost_case> measure_record_cost(int cpu = 1, std::size_t samples = 1'000'000,
                                                         std::size_t warmup = 100'000) {
    if (!detail::pin_to(cpu)) return {};
    std::vector<record_cost_case> out;
    histogram<> h;  // constructed before timing, so its pages are mapped (S1)
    {
        const std::vector<ticks> unused(samples);
        out.push_back({"none", "no record: start(); stop()", detail::time_window<false>(h, unused, warmup)});
    }
    const struct {
        const char* name;
        const char* values;
    } cases[] = {{"hot", "always 300 ticks"},
                 {"realistic", "lognormal, median 900 ticks, sigma 0.3"},
                 {"worst", "uniform over 0 to 2^40 ticks"}};
    for (const auto& c : cases) {
        const auto vals = record_cost_values(c.name, samples);
        out.push_back({c.name, c.values, detail::time_window<true>(h, vals, warmup)});
    }
    for (auto& c : out) c.cost.cpu = cpu;
    return out;
}

inline void write_record_cost_block(json_writer& w, const std::vector<record_cost_case>& cases, double ticks_per_ns) {
    const auto ns = [ticks_per_ns](ticks t) { return static_cast<double>(t) / ticks_per_ns; };
    w.key("record_cost");
    w.begin_object();
    for (const auto& c : cases) {
        w.key(c.name);
        w.begin_object();
        w.member("values", c.values);
        w.member("cpu", c.cost.cpu);
        w.member("samples", c.cost.samples);
        w.member("warmup_discarded", c.cost.warmup_discarded);
        w.member("p50_ns", ns(c.cost.p50));
        w.member("p99_ns", ns(c.cost.p99));
        w.member("p999_ns", ns(c.cost.p999));
        w.member("max_ns", ns(c.cost.max));
        w.end_object();
    }
    w.end_object();
}

}  // namespace lfipc::bench
