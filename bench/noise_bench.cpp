// The machine's noise floor on every reserved CPU, repeated, and the run JSON.
// Design: docs/design/005-noise-floor.md
//
// Usage: noise_bench [output.json [cpus [seconds [repeats]]]]
// Defaults: results/noise-<host>-<UTC time>.json; every reserved CPU (online
// minus bench-on's fence); 10 s per CPU; 5 repeats. Each repeat samples every
// CPU in turn, so a machine-wide change over the run shows as spread between
// repeats, not as one CPU.
// Exit status: 0 published, 1 refused (reason in the JSON), 2 cannot write.
#include <lfipc/bench/clock_block.hpp>
#include <lfipc/bench/noise_block.hpp>
#include <lfipc/bench/run_file.hpp>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <map>
#include <memory>
#include <vector>

namespace {

struct range {
    double lo = std::numeric_limits<double>::max(), hi = std::numeric_limits<double>::lowest();
    void add(double v) {
        lo = std::min(lo, v);
        hi = std::max(hi, v);
    }
};

struct cpu_summary {
    range stopped_fraction, stops_per_s, longest_ns, timer_per_s;
    int repeats_ok = 0;
};

}  // namespace

int main(int argc, char** argv) {
    using namespace lfipc::bench;
    run_file run(argc, argv, LFIPC_RESULTS_DIR, "noise");
    if (run.refused) return run.finish(false, "noise_bench");

    std::vector<int> cpus;
    if (argc > 2) cpus = parse_cpu_list(argv[2]);
    else cpus = capture_environment(env_reader{}).reserved;
    const double seconds = argc > 3 ? std::atof(argv[3]) : 10.0;
    const int repeats = argc > 4 ? std::atoi(argv[4]) : 5;
    if (cpus.empty() || seconds <= 0 || repeats <= 0) {
        std::fprintf(stderr, "noise_bench: no CPUs to measure (no fence, and none given)\n");
        return run.finish(false, "noise_bench");
    }
    constexpr std::size_t capacity = 100'000;  // ~12,000 stops expected in 10 s

    const auto clk = run_clock();
    write_clock_block(run.w, clk);
    if (!clk.ok) {
        std::fprintf(stderr, "noise_bench: clock refused: %s\n", clk.reason.c_str());
        return run.finish(false, "noise_bench");
    }
    const double tpn = clk.cal.ticks_per_ns;

    std::map<int, cpu_summary> summary;
    bool all_ok = true;
    run.w.key("noise_bench");
    run.w.begin_object();
    run.w.member("repeats", repeats);
    run.w.member("seconds_per_cpu", seconds);
    run.w.member("cpus", detail::cpu_list_string(cpus));
    run.w.key("runs");
    run.w.begin_array();
    for (int rep = 0; rep < repeats; ++rep) {
        std::vector<std::unique_ptr<noise_result>> samples;
        for (int cpu : cpus) {
            auto r = std::make_unique<noise_result>();
            measure_noise(cpu, seconds, tpn, *r, default_noise_threshold_ns, capacity);
            std::printf("repeat %d cpu %2d: %s%.3f%% stopped, %llu stops, longest %.0f us\n", rep + 1, cpu,
                        r->ok ? "" : "FAILED ", 100.0 * static_cast<double>(r->stopped) / static_cast<double>(r->window),
                        (unsigned long long)r->stops, r->lengths.max() / tpn / 1e3);
            std::fflush(stdout);
            all_ok = all_ok && r->ok && !r->overflow;
            if (r->ok) {
                auto& s = summary[cpu];
                const double secs = static_cast<double>(r->window) / tpn / 1e9;
                s.stopped_fraction.add(static_cast<double>(r->stopped) / static_cast<double>(r->window));
                s.stops_per_s.add(static_cast<double>(r->stops) / secs);
                s.longest_ns.add(r->lengths.max() / tpn);
                const auto loc = r->interrupts.find("LOC");
                s.timer_per_s.add(loc == r->interrupts.end() ? 0.0 : static_cast<double>(loc->second) / secs);
                ++s.repeats_ok;
            }
            r->events.clear();  // keep memory to one sample's worth
            r->events.shrink_to_fit();
            samples.push_back(std::move(r));
        }
        run.w.begin_object();
        run.w.member("repeat", rep + 1);
        write_noise_block(run.w, samples, tpn);
        run.w.end_object();
    }
    run.w.end_array();

    // S6: each CPU's spread across repeats.
    run.w.key("summary");
    run.w.begin_array();
    const auto spread = [&run](const char* key, const range& r) {
        run.w.key(key);
        run.w.begin_inline_array();
        run.w.value(r.lo);
        run.w.value(r.hi);
        run.w.end_array();
    };
    for (const auto& [cpu, s] : summary) {
        run.w.begin_object();
        run.w.member("cpu", cpu);
        run.w.member("repeats_ok", s.repeats_ok);
        spread("stopped_fraction_min_max", s.stopped_fraction);
        spread("stops_per_s_min_max", s.stops_per_s);
        spread("longest_ns_min_max", s.longest_ns);
        spread("timer_interrupts_per_s_min_max", s.timer_per_s);
        run.w.end_object();
    }
    run.w.end_array();
    run.w.end_object();
    return run.finish(all_ok, "noise_bench");
}
