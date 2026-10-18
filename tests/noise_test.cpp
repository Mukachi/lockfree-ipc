// Tests for include/lfipc/bench/noise.hpp. Spec: docs/design/005-noise-floor.md
#include <lfipc/bench/calibrate.hpp>
#include <lfipc/bench/noise.hpp>

#include <cstdio>
#include <cstdlib>
#include <memory>

namespace {

int failures = 0;

void check(bool cond, const char* what) {
    std::printf("%s  %s\n", cond ? "pass" : "FAIL", what);
    if (!cond) ++failures;
}

using namespace lfipc::bench;

// Holds the spinner in a busy-wait of `hold` ticks at the given iterations.
struct inject {
    std::uint64_t first, every, count;
    ticks hold;
    void operator()(std::uint64_t n) const noexcept {
        if (n < first || (n - first) % every != 0 || (n - first) / every >= count) return;
        const ticks until = start() + hold;
        while (start() < until) {
        }
    }
};

}  // namespace

int main() {
    const double tpn = calibrate(3, 50).ticks_per_ns;
    auto r = std::make_unique<noise_result>();

    // Plain spin: S2, the loop itself never counts.
    measure_noise(2, 0.5, tpn, *r);
    std::printf("info  cpu 2, 0.5 s: %llu iterations, shortest gap %.1f ns, %llu stops, %.3f%% stopped, longest %.1f us\n",
                (unsigned long long)r->iterations, r->iteration_min / tpn, (unsigned long long)r->stops,
                100.0 * static_cast<double>(r->stopped) / static_cast<double>(r->window), r->lengths.max() / tpn / 1e3);
    check(r->ok && r->iterations > 1'000'000, "spinner runs");
    check(r->iteration_min * 10 < r->threshold, "S2 the loop's own iteration is far below the threshold");
    bool all_above = true;
    for (const auto& e : r->events) all_above = all_above && e.length > r->threshold;
    check(all_above && r->lengths.count() == r->stops, "S2, S3 every recorded stop is above the threshold, all counted");

    // S4: inject twenty 100 us stops; each is found, at the right length.
    {
        const auto hold = static_cast<ticks>(100'000 * tpn);
        measure_noise(2, 0.5, tpn, *r, default_noise_threshold_ns, default_stop_capacity,
                      inject{100'000, 100'000, 20, hold});
        std::size_t found = 0, exact = 0;
        for (const auto& e : r->events) {
            if (e.length < hold * 9 / 10 || e.length > hold * 2) continue;
            ++found;
            exact += e.length >= hold && e.length - hold <= static_cast<ticks>(1'000 * tpn);
        }
        std::printf("info  injected 20 x 100 us: %zu found near 100 us, %zu within 1 us of it\n", found, exact);
        check(found >= 20, "S4 every injected stop is recorded");
        check(exact >= 18, "S4 injected stops measured within 1 us (two may coincide with a real stop)");
    }

    // S3: capacity of 4, twenty injected stops: all counted, overflow reported.
    {
        measure_noise(2, 0.3, tpn, *r, default_noise_threshold_ns, 4,
                      inject{100'000, 100'000, 20, static_cast<ticks>(20'000 * tpn)});
        check(r->stops >= 20 && r->events.size() == 4 && r->overflow && r->lengths.count() == r->stops,
              "S3 beyond capacity: every stop counted and in the histogram, overflow reported");
    }

    measure_noise(-1, 0.1, tpn, *r);
    check(!r->ok && !r->reason.empty(), "unpinnable CPU refuses with a reason");

    std::printf("%s\n", failures ? "FAILED" : "all passed");
    return failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
