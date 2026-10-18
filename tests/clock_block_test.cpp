// Tests for include/lfipc/bench/clock_block.hpp. Spec: docs/design/001-clock.md
//
// Runs the clock flow twice, healthy and with a forced refusal, and writes both
// blocks to clock_ok.json and clock_refused.json in the working directory.
// check_clock_json.py then checks that each parses and has exactly the agreed fields (S3).
#include <lfipc/bench/clock_block.hpp>

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>

namespace {

int failures = 0;

void check(bool cond, const char* what) {
    std::printf("%s  %s\n", cond ? "pass" : "FAIL", what);
    if (!cond) ++failures;
}

using namespace lfipc::bench;

void write(const clock_report& r, const char* path) {
    json_writer w;
    w.begin_object();
    write_clock_block(w, r);
    w.end_object();
    std::ofstream(path) << w.str() << '\n';
}

bool has(const std::string& s, const char* needle) { return s.find(needle) != std::string::npos; }

}  // namespace

int main() {
    // Short settings: this checks the flow and the block, not the numbers.
    clock_options opt;
    opt.calibration_windows = 3;
    opt.window_ms = 20;
    opt.overhead_samples = 100'000;
    opt.overhead_warmup = 10'000;
    opt.round_trips = 1'000;
    opt.cross_cpu_warmup = 100;

    const auto ok = run_clock(opt);
    std::printf("info  %s\n", ok.ok ? "flow published" : ok.reason.c_str());
    if (clock_is_tsc) {
        check(ok.ok && ok.reason.empty(), "healthy run publishes");
        check(ok.cross_cpu.size() + 1 == online_cpus().size(), "cross-CPU block covers every other CPU");
        check(ok.ovh.cpu == 1, "overhead measured on CPU 1");
    } else {
        check(!ok.ok, "S7 fallback refuses");
    }
    write(ok, "clock_ok.json");

    // S1: forced refusal through a fake clocksource file.
    const char* fake = "clock_block_test_clocksource.tmp";
    std::ofstream(fake) << "hpet\n";
    opt.clocksource_path = fake;
    const auto refused = run_clock(opt);
    std::remove(fake);
    check(!refused.ok && !refused.reason.empty(), "S1 refusal carries a reason");
    if (clock_is_tsc) check(has(refused.reason, "hpet"), "S1 reason names the clocksource");
    check(refused.cross_cpu.empty() && refused.ovh.samples == 0, "S1 refusal produces no results");
    write(refused, "clock_refused.json");

    // An unpinnable overhead CPU refuses rather than measuring somewhere else.
    opt.clocksource_path = kernel_clocksource_path;
    opt.overhead_cpu = -1;
    if (clock_is_tsc) check(!run_clock(opt).ok, "unpinnable overhead CPU refuses");

    // S5 and S9: the cross-CPU decision, on synthetic pairs.
    cross_cpu_pair good;
    good.ok = true;
    good.a = 0;
    good.b = 1;
    good.d1 = 300;
    good.d2 = 200;
    auto disagree = good;
    disagree.b = 2;
    disagree.d2 = -50;
    auto unmeasured = good;
    unmeasured.b = 3;
    unmeasured.ok = false;
    unmeasured.reason = "cannot pin to CPU 3";
    check(evaluate_cross_cpu({good, good}, 2.0).ok, "agreeing pairs accepted");
    const auto d = evaluate_cross_cpu({good, disagree}, 2.0);
    check(!d.ok && has(d.reason, "0 vs 2") && has(d.reason, "[25.0, 150.0] ns"),
          "S9 proven disagreement refuses, naming the CPUs and the bounds");
    const auto u = evaluate_cross_cpu({good, unmeasured}, 2.0);
    check(!u.ok && has(u.reason, "cannot pin to CPU 3"), "S5 unmeasured pair refuses");

    json_writer w;
    w.begin_object();
    write_clock_block(w, refused);
    w.end_object();
    check(has(w.str(), "\"ok\": false") && !has(w.str(), "ticks_per_ns"), "refusal block has no results");

    std::printf("%s\n", failures ? "FAILED" : "all passed");
    return failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
