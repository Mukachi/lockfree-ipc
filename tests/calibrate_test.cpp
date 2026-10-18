// Tests for include/lfipc/bench/calibrate.hpp. Spec: docs/design/001-clock.md
#include <lfipc/bench/calibrate.hpp>

#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace {

int failures = 0;

void check(bool cond, const char* what) {
    std::printf("%s  %s\n", cond ? "pass" : "FAIL", what);
    if (!cond) ++failures;
}

bool near(double a, double b, double tol) { return std::fabs(a - b) <= tol; }

}  // namespace

int main() {
    using namespace lfipc::bench;

    // Threshold logic, on synthetic window rates.
    const auto steady = evaluate_calibration({2.2, 2.2, 2.2000022, 2.2}, 10.0);
    check(steady.ok, "accepts 1 ppm spread under a 10 ppm limit");
    check(near(steady.spread_ppm, 1.0, 0.01), "spread computed as (max - min) / median in ppm");
    check(near(steady.ticks_per_ns, 2.2, 1e-12), "median of an even count");

    const auto noisy = evaluate_calibration({2.2, 2.2001}, 10.0);
    check(!noisy.ok && !noisy.reason.empty(), "S2 refuses ~45 ppm spread, with a reason");
    check(!evaluate_calibration({}, 10.0).ok, "refuses zero windows");
    check(!evaluate_calibration({0.0, 0.0}, 10.0).ok, "refuses a non-positive rate");

    // This machine: short run, reported and sanity-checked.
    const auto c = calibrate(5, 50);
    std::printf("info  ticks_per_ns %.6f  spread %.3f ppm  %s%s\n", c.ticks_per_ns, c.spread_ppm,
                c.ok ? "ok" : "refused: ", c.reason.c_str());
    check(c.window_rates.size() == 5, "one rate per window");
    check(c.ticks_per_ns > 0.0, "positive tick rate");

    std::printf("%s\n", failures ? "FAILED" : "all passed");
    return failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
