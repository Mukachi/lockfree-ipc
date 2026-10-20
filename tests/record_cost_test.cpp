// Tests for include/lfipc/bench/record_cost.hpp. Spec: docs/design/002-histogram.md (S7)
#include "test_machine.hpp"

#include <lfipc/bench/record_cost.hpp>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <string>

namespace {

int failures = 0;

void check(bool cond, const char* what) {
    std::printf("%s  %s\n", cond ? "pass" : "FAIL", what);
    if (!cond) ++failures;
}

}  // namespace

int main() {
    using namespace lfipc::bench;

    // Value cases are what the note says.
    const auto hot = record_cost_values("hot", 1000);
    const auto real = record_cost_values("realistic", 100'000);
    const auto worst = record_cost_values("worst", 100'000);
    check(std::all_of(hot.begin(), hot.end(), [](ticks v) { return v == 300; }), "hot: always 300 ticks");
    std::size_t below = 0;
    for (ticks v : real) below += v < 1024;
    check(below > 20'000 && below < 80'000, "realistic straddles 1,024 ticks");
    ticks top = 0;
    for (ticks v : worst) top = std::max(top, v);
    check(top < (ticks{1} << 40) && top > (ticks{1} << 39), "worst spans the range, none in overflow");

    // The record case really records: every timed call lands in the histogram.
    {
        histogram<> h;
        const auto o = detail::time_window<true>(h, real, 5'000);
        check(h.count() == real.size() + 5'000, "every warm-up and measured call recorded");
        check(o.samples == real.size() && o.warmup_discarded == 5'000, "samples and warm-up reported");
        check(o.p50 > 0 && o.p50 <= o.p99 && o.p99 <= o.p999 && o.p999 <= o.max, "percentiles ordered");
        histogram<> g;
        detail::time_window<false>(g, real, 5'000);
        check(g.count() == 0, "the bare case records nothing");
    }

    // Whole measurement, short, reported.
    const auto cases = measure_record_cost(lfipc::test::pick_cpus(1).value().front(), 100'000, 10'000);
    check(cases.size() == 4 && cases[0].name == "none" && cases[3].name == "worst", "four cases, bare read first");
    for (const auto& c : cases)
        std::printf("info  %-9s cpu %d  p50 %llu  p99 %llu  p99.9 %llu ticks\n", c.name.c_str(), c.cost.cpu,
                    (unsigned long long)c.cost.p50, (unsigned long long)c.cost.p99,
                    (unsigned long long)c.cost.p999);
    check(measure_record_cost(-1, 10, 1).empty(), "unpinnable CPU gives no result");

    std::printf("%s\n", failures ? "FAILED" : "all passed");
    return failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
