// Tests for include/lfipc/bench/noise.hpp. Spec: docs/design/005-noise-floor.md
#include "test_machine.hpp"

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

int main(int argc, char** argv) {
    namespace t = lfipc::test;
    const double tpn = calibrate(3, 50).ticks_per_ns;
    auto r = std::make_unique<noise_result>();
    const int cpu = t::pick_cpus(1).value().front();

    // S4: inject twenty 100 us stops; each is found, at the right length. A
    // quiet check: on a shared machine a real stop can land on an injected one.
    if (t::quiet_run(argc, argv)) {
        const auto hold = static_cast<ticks>(100'000 * tpn);
        measure_noise(cpu, 0.5, tpn, *r, default_noise_threshold_ns, default_stop_capacity,
                      inject{100'000, 100'000, 20, hold});
        std::size_t found = 0, exact = 0;
        for (const auto& e : r->events) {
            if (e.length < hold * 9 / 10 || e.length > hold * 2) continue;
            ++found;
            exact += e.length >= hold && e.length - hold <= static_cast<ticks>(1'000 * tpn);
        }
        std::printf("info  injected 20 x 100 us on CPU %d: %zu found near 100 us, %zu within 1 us of it\n", cpu,
                    found, exact);
        check(found >= 20, "S4 every injected stop is recorded");
        check(exact >= 18, "S4 injected stops measured within 1 us (two may coincide with a real stop)");
        std::printf("%s\n", failures ? "FAILED" : "all passed");
        return failures ? EXIT_FAILURE : EXIT_SUCCESS;
    }

    // Plain spin: S2, the loop itself never counts.
    measure_noise(cpu, 0.5, tpn, *r);
    std::printf("info  cpu %d, 0.5 s: %llu iterations, shortest gap %.1f ns, %llu stops, %.3f%% stopped, longest %.1f us\n",
                cpu, (unsigned long long)r->iterations, r->iteration_min / tpn, (unsigned long long)r->stops,
                100.0 * static_cast<double>(r->stopped) / static_cast<double>(r->window), r->lengths.max() / tpn / 1e3);
    check(r->ok && r->iterations > 1'000'000, "spinner runs");
    check(r->iteration_min * 10 < r->threshold, "S2 the loop's own iteration is far below the threshold");
    bool all_above = true;
    for (const auto& e : r->events) all_above = all_above && e.length > r->threshold;
    check(all_above && r->lengths.count() == r->stops, "S2, S3 every recorded stop is above the threshold, all counted");

    // S3: capacity of 4, twenty injected stops: all counted, overflow reported.
    {
        measure_noise(cpu, 0.3, tpn, *r, default_noise_threshold_ns, 4,
                      inject{100'000, 100'000, 20, static_cast<ticks>(20'000 * tpn)});
        check(r->stops >= 20 && r->events.size() == 4 && r->overflow && r->lengths.count() == r->stops,
              "S3 beyond capacity: every stop counted and in the histogram, overflow reported");
    }

    // Coarse causes: parsing one CPU's column, and the live counters.
    {
        const char* irq =
            "           CPU0       CPU1       CPU2\n"
            "  0:         40          0          0   IO-APIC    2-edge      timer\n"
            " 24:          1          5        900   PCI-MSI 1-edge      nvme0q1\n"
            "LOC:       1000       2000       3000   Local timer interrupts\n"
            "RES:          7          8          9   Rescheduling interrupts\n"
            "ERR:          0\n"
            "MIS:          0\n";
        const auto c2 = per_cpu_counts(irq, 2);
        check(c2.size() == 4 && c2.at("LOC") == 3000 && c2.at("24") == 900 && c2.at("0") == 0 && !c2.count("ERR"),
              "per-CPU column parsed, single-column rows skipped");
        check(per_cpu_counts(irq, 7).empty(), "a CPU not in the header gives nothing");
        const auto d = counter_delta({{"LOC", 10}, {"RES", 5}}, {{"LOC", 1010}, {"RES", 5}, {"TLB", 2}});
        check(d.size() == 2 && d.at("LOC") == 1000 && d.at("TLB") == 2, "delta keeps only the types that moved");

        // CPU 0: the boot CPU always ticks (it cannot be nohz_full), so the
        // counter wiring is testable there on any kernel configuration.
        const auto allowed = t::allowed_cpus();
        if (allowed.empty() || allowed.front() != 0) {
            t::skip("live: timer interrupts counted on CPU 0", "CPU 0 is not in this process's affinity mask");
        } else {
            measure_noise(0, 0.5, tpn, *r);
            const auto loc = r->interrupts.count("LOC") ? r->interrupts.at("LOC") : 0;
            std::printf("info  cpu 0, 0.5 s: %llu stops; LOC %llu; interrupt types %zu, softirq types %zu, "
                        "involuntary switches %ld\n",
                        (unsigned long long)r->stops, (unsigned long long)loc, r->interrupts.size(),
                        r->softirqs.size(), r->involuntary_switches);
#if defined(__x86_64__)
            check(r->ok && loc > 0, "live: local timer interrupts counted on the spinning CPU (CPU 0)");
#else
            // LOC is x86's row; other architectures name their timer interrupt differently.
            check(r->ok && !r->interrupts.empty(), "live: interrupts counted on the spinning CPU (CPU 0)");
#endif
        }
    }

    measure_noise(-1, 0.1, tpn, *r);
    check(!r->ok && !r->reason.empty(), "unpinnable CPU refuses with a reason");

    std::printf("%s\n", failures ? "FAILED" : "all passed");
    return failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
