// The clock flow end to end, and its JSON block. Design: docs/design/001-clock.md
#pragma once

#include <lfipc/bench/calibrate.hpp>
#include <lfipc/bench/clock.hpp>
#include <lfipc/bench/cross_cpu.hpp>
#include <lfipc/bench/json.hpp>
#include <lfipc/bench/overhead.hpp>

#include <cstdio>
#include <string>
#include <vector>

namespace lfipc::bench {

struct clock_options {
    std::string clocksource_path = kernel_clocksource_path;
    int calibration_windows = default_calibration_windows;
    long window_ms = default_window_ms;
    double max_spread_ppm = default_max_spread_ppm;
    std::size_t overhead_samples = 1'000'000;
    std::size_t overhead_warmup = 100'000;
    int overhead_cpu = 1;  // not CPU 0: its interrupt load would inflate the tail
    int reference_cpu = 0;
    std::size_t round_trips = default_round_trips;
    std::size_t cross_cpu_warmup = default_cross_cpu_warmup;
};

struct clock_report {
    bool ok = false;
    std::string reason;  // empty when ok
    bool invariant_tsc = false;
    std::string kernel_clocksource;
    calibration cal;
    overhead ovh;
    std::vector<cross_cpu_pair> cross_cpu;
};

// The cross-CPU decision alone, so every refusal path is testable: a pair that
// could not be measured (S5) or that proves disagreement (S9) refuses the run.
inline check_result evaluate_cross_cpu(const std::vector<cross_cpu_pair>& pairs, double ticks_per_ns) {
    for (const auto& p : pairs) {
        const std::string name = "cross-CPU " + std::to_string(p.a) + " vs " + std::to_string(p.b);
        if (!p.ok) return {false, name + ": " + p.reason};
        if (p.disagreement_proven()) {
            char bounds[96];
            std::snprintf(bounds, sizeof(bounds), "[%.1f, %.1f] ns", p.theta_lo() / ticks_per_ns,
                          p.theta_hi() / ticks_per_ns);
            return {false, name + ": TSCs disagree, offset bounds " + bounds + " exclude zero"};
        }
    }
    return {true, {}};
}

// The flowchart: checks, calibration, overhead, cross-CPU. Stops at the first
// refusal, leaving the later results empty.
inline clock_report run_clock(const clock_options& opt = {}) {
    clock_report r;
    r.invariant_tsc = cpu_has_invariant_tsc();
    r.kernel_clocksource = read_clocksource(opt.clocksource_path);
    const auto chk = evaluate_clock(clock_is_tsc, r.invariant_tsc, r.kernel_clocksource);
    if (!chk.ok) {
        r.reason = chk.reason;
        return r;
    }
    r.cal = calibrate(opt.calibration_windows, opt.window_ms, opt.max_spread_ppm);
    if (!r.cal.ok) {
        r.reason = r.cal.reason;
        return r;
    }
    // measure_overhead pins without reporting failure, so check the CPU first.
    if (!detail::pin_to(opt.overhead_cpu)) {
        r.reason = "cannot pin to overhead CPU " + std::to_string(opt.overhead_cpu);
        return r;
    }
    const auto ovh = measure_overhead(opt.overhead_samples, opt.overhead_warmup, opt.overhead_cpu);
    auto cross = measure_cross_cpu(opt.reference_cpu, opt.round_trips, opt.cross_cpu_warmup);
    const auto agree = evaluate_cross_cpu(cross, r.cal.ticks_per_ns);
    if (!agree.ok) {
        r.reason = agree.reason;
        return r;
    }
    r.ovh = ovh;
    r.cross_cpu = std::move(cross);
    r.ok = true;
    return r;
}

// Writes `"clock": {...}` into the object the caller has open. Ticks are
// converted to ns here, at report time.
inline void write_clock_block(json_writer& w, const clock_report& r) {
    w.key("clock");
    w.begin_object();
    w.member("ok", r.ok);
    if (!r.ok) w.member("reason", r.reason);
    w.member("source", clock_is_tsc ? "rdtsc" : "steady_clock");
    w.member("invariant_tsc", r.invariant_tsc);
    w.member("kernel_clocksource", r.kernel_clocksource);
    if (r.ok) {
        const double tpn = r.cal.ticks_per_ns;
        const auto ns = [tpn](double t) { return t / tpn; };
        w.member("ticks_per_ns", tpn);
        w.member("calibration_spread_ppm", r.cal.spread_ppm);

        w.key("overhead");
        w.begin_object();
        w.member("cpu", r.ovh.cpu);
        w.member("samples", r.ovh.samples);
        w.member("warmup_discarded", r.ovh.warmup_discarded);
        w.member("p50_ns", ns(static_cast<double>(r.ovh.p50)));
        w.member("p99_ns", ns(static_cast<double>(r.ovh.p99)));
        w.member("p999_ns", ns(static_cast<double>(r.ovh.p999)));
        w.member("max_ns", ns(static_cast<double>(r.ovh.max)));
        w.end_object();

        w.key("cross_cpu");
        w.begin_array();
        for (const auto& p : r.cross_cpu) {
            w.begin_object();
            w.member("a", p.a);
            w.member("b", p.b);
            w.member("theta_lo_ns", ns(static_cast<double>(p.theta_lo())));
            w.member("theta_hi_ns", ns(static_cast<double>(p.theta_hi())));
            w.member("round_trips", p.round_trips);
            w.member("warmup_discarded", p.warmup_discarded);
            w.member("converged", p.converged);
            w.end_object();
        }
        w.end_array();
    }
    w.end_object();
}

}  // namespace lfipc::bench
