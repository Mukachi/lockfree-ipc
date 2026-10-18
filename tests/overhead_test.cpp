// Tests for include/lfipc/bench/overhead.hpp. Spec: docs/design/001-clock.md
#include <lfipc/bench/calibrate.hpp>
#include <lfipc/bench/overhead.hpp>

#include <cstdio>
#include <cstdlib>
#include <numeric>

namespace {

int failures = 0;

void check(bool cond, const char* what) {
    std::printf("%s  %s\n", cond ? "pass" : "FAIL", what);
    if (!cond) ++failures;
}

}  // namespace

int main() {
    using namespace lfipc::bench;

    // Nearest-rank percentiles on 1..1000.
    std::vector<ticks> v(1000);
    std::iota(v.begin(), v.end(), ticks{1});
    check(percentile_sorted(v, 50.0) == 500, "p50 of 1..1000 is 500");
    check(percentile_sorted(v, 99.0) == 990, "p99 of 1..1000 is 990");
    check(percentile_sorted(v, 99.9) == 999, "p99.9 of 1..1000 is 999");
    check(percentile_sorted(v, 100.0) == 1000, "p100 is the max");
    check(percentile_sorted({}, 50.0) == 0, "empty input gives 0");

    // S4: summary carries percentiles and max, in order.
    const auto s = summarise_overhead({5, 1, 4, 2, 3});
    check(s.p50 == 3 && s.max == 5 && s.samples == 5, "summary sorts and reports p50 and max");

    // This machine, reported and sanity-checked.
    const auto c = calibrate(5, 50);
    const auto o = measure_overhead();
    const auto ns = [&](ticks t) { return static_cast<double>(t) / c.ticks_per_ns; };
    std::printf("info  cpu %d, %zu samples, %zu warm-up discarded\n", o.cpu, o.samples, o.warmup_discarded);
    std::printf("info  ticks p50 %llu  p99 %llu  p99.9 %llu  max %llu\n",
                (unsigned long long)o.p50, (unsigned long long)o.p99, (unsigned long long)o.p999,
                (unsigned long long)o.max);
    std::printf("info  ns    p50 %.1f  p99 %.1f  p99.9 %.1f  max %.1f\n", ns(o.p50), ns(o.p99),
                ns(o.p999), ns(o.max));
    check(o.samples == 1'000'000, "one million samples");
    check(o.p50 > 0 && o.p50 <= o.p99 && o.p99 <= o.p999 && o.p999 <= o.max, "percentiles ordered");

    std::printf("%s\n", failures ? "FAILED" : "all passed");
    return failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
