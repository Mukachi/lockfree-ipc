// JSON block for noise samples. Design: docs/design/005-noise-floor.md
#pragma once

#include <lfipc/bench/calibrate.hpp>
#include <lfipc/bench/histogram_block.hpp>
#include <lfipc/bench/json.hpp>
#include <lfipc/bench/noise.hpp>

#include <memory>
#include <vector>

namespace lfipc::bench {

// Stop counts by class, from the stop-length histogram. A bucket is counted in
// the class holding its upper edge; buckets are at most 0.2% wide, so only
// stops within 0.2% of a class boundary can land one class up.
struct stop_classes {
    std::uint64_t to_10us = 0, to_100us = 0, to_1ms = 0, over_1ms = 0;
};

inline stop_classes classify_stops(const histogram<>& h, double ticks_per_ns) {
    stop_classes c;
    const double us = 1e3 * ticks_per_ns;
    h.for_each_bucket([&](ticks upper, std::uint64_t n) {
        const double u = static_cast<double>(upper);
        if (u <= 10 * us) c.to_10us += n;
        else if (u <= 100 * us) c.to_100us += n;
        else if (u <= 1000 * us) c.to_1ms += n;
        else c.over_1ms += n;
    });
    return c;
}

// Samples each CPU in turn, before any measurement (S1).
inline std::vector<std::unique_ptr<noise_result>> sample_noise(const std::vector<int>& cpus, double seconds) {
    const double tpn = calibrate(3, 50).ticks_per_ns;  // sets the threshold only
    std::vector<std::unique_ptr<noise_result>> out;
    for (int cpu : cpus) {
        auto r = std::make_unique<noise_result>();
        measure_noise(cpu, seconds, tpn, *r);
        out.push_back(std::move(r));
    }
    return out;
}

inline void write_counter_map(json_writer& w, const char* key, const counter_map& m) {
    w.key(key);
    w.begin_object();
    for (const auto& [k, v] : m) w.member(k, v);
    w.end_object();
}

// Writes `"noise": {...}`, converting with the run's own ticks_per_ns.
inline void write_noise_block(json_writer& w, const std::vector<std::unique_ptr<noise_result>>& samples,
                              double ticks_per_ns) {
    const auto ns = [ticks_per_ns](double t) { return t / ticks_per_ns; };
    w.key("noise");
    w.begin_object();
    w.member("threshold_ns", samples.empty() ? 0.0 : ns(static_cast<double>(samples[0]->threshold)));
    w.member("seconds_per_cpu", samples.empty() ? 0.0 : ns(static_cast<double>(samples[0]->window)) / 1e9);
    w.key("cpus");
    w.begin_array();
    for (const auto& r : samples) {
        w.begin_object();
        w.member("cpu", r->cpu);
        w.member("ok", r->ok);
        if (!r->ok) {
            w.member("reason", r->reason);
            w.end_object();
            continue;
        }
        const double secs = ns(static_cast<double>(r->window)) / 1e9;
        w.member("stopped_fraction", static_cast<double>(r->stopped) / static_cast<double>(r->window));
        w.member("stops", r->stops);
        w.member("stops_per_s", static_cast<double>(r->stops) / secs);
        const auto c = classify_stops(r->lengths, ticks_per_ns);
        w.key("by_class");
        w.begin_object();
        w.member("1-10us", c.to_10us);
        w.member("10-100us", c.to_100us);
        w.member("100us-1ms", c.to_1ms);
        w.member("over_1ms", c.over_1ms);
        w.end_object();
        w.member("longest_ns", ns(static_cast<double>(r->lengths.max())));
        w.member("iterations", r->iterations);
        w.member("iteration_min_ns", ns(static_cast<double>(r->iteration_min)));
        w.member("overflow", r->overflow);
        write_counter_map(w, "interrupts", r->interrupts);
        write_counter_map(w, "softirqs", r->softirqs);
        w.member("involuntary_switches", r->involuntary_switches);
        write_histogram_block(w, r->lengths, ticks_per_ns, "stop_lengths");
        w.end_object();
    }
    w.end_array();
    w.end_object();
}

}  // namespace lfipc::bench
