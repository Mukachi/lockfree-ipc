// Tests for include/lfipc/bench/clock.hpp. Spec: docs/design/001-clock.md
#include <lfipc/bench/clock.hpp>

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

std::string write_temp(const std::string& content) {
    const std::string path = "clock_test_clocksource.tmp";
    std::ofstream(path) << content;
    return path;
}

}  // namespace

int main() {
    using namespace lfipc::bench;

    // S1: every refusal path, forced.
    check(!evaluate_clock(false, true, "tsc").ok, "S1 refuses non-x86 fallback");
    check(!evaluate_clock(true, false, "tsc").ok, "S1 refuses CPU without invariant TSC");
    check(!evaluate_clock(true, true, "hpet").ok, "S1 refuses clocksource hpet");
    check(!evaluate_clock(true, true, "").ok, "S1 refuses unreadable clocksource");
    check(!evaluate_clock(true, true, "hpet").reason.empty(), "S1 refusal carries a reason");
    check(evaluate_clock(true, true, "tsc").ok, "accepts invariant TSC with clocksource tsc");

    // Clocksource file reading, against fake files.
    check(read_clocksource(write_temp("tsc\n")) == "tsc", "reads clocksource file");
    check(!check_clock(write_temp("kvm-clock\n")).ok, "S1 refuses via fake sysfs file");
    check(read_clocksource("/nonexistent/clocksource").empty(), "missing file reads as empty");
    std::remove("clock_test_clocksource.tmp");

    // S7: the fallback must refuse.
    if (!clock_is_tsc) check(!check_clock().ok, "S7 fallback refuses published measurements");

    // Reads move forward on one thread.
    bool ordered = true;
    ticks prev = stamp();
    for (int i = 0; i < 1'000'000; ++i) {
        const ticks a = start();
        const ticks b = stop();
        if (b < a || a < prev) ordered = false;
        prev = b;
    }
    check(ordered, "start/stop/stamp never go backwards on one thread");

    // This machine, reported but not asserted: CI virtual machines may not use tsc.
    const auto here = check_clock();
    std::printf("info  this machine: %s%s\n", here.ok ? "ok" : "refused: ", here.reason.c_str());

    std::printf("%s\n", failures ? "FAILED" : "all passed");
    return failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
