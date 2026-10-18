// Tests for include/lfipc/bench/cross_cpu.hpp. Spec: docs/design/001-clock.md
#include <lfipc/bench/calibrate.hpp>
#include <lfipc/bench/cross_cpu.hpp>

#include <cstdio>
#include <cstdlib>

namespace {

int failures = 0;

void check(bool cond, const char* what) {
    std::printf("%s  %s\n", cond ? "pass" : "FAIL", what);
    if (!cond) ++failures;
}

using lfipc::bench::ticks;

struct trips {
    std::vector<ticks> a_send, b_recv, b_send, a_recv;
};

// Round trips with B's clock offset by theta and a one-way latency of
// `latency + jitter`, jitter cycling 0..9.
trips synthetic(std::size_t n, long long theta, long long latency) {
    trips t;
    for (std::size_t i = 0; i < n; ++i) {
        const long long jitter = static_cast<long long>(i % 10);
        const long long as = 1'000'000 + 1000 * static_cast<long long>(i);
        const long long br = as + latency + jitter + theta;
        const long long bs = br + 5;
        const long long ar = bs - theta + latency + jitter;
        t.a_send.push_back(static_cast<ticks>(as));
        t.b_recv.push_back(static_cast<ticks>(br));
        t.b_send.push_back(static_cast<ticks>(bs));
        t.a_recv.push_back(static_cast<ticks>(ar));
    }
    return t;
}

lfipc::bench::cross_cpu_pair summarise(const trips& t, std::size_t warmup) {
    return lfipc::bench::summarise_cross_cpu(t.a_send, t.b_recv, t.b_send, t.a_recv, warmup);
}

}  // namespace

int main() {
    using namespace lfipc::bench;

    // Bounds arithmetic: offset +50, latency 100. d1 = 150, d2 = 50.
    {
        const auto p = summarise(synthetic(1000, 50, 100), 0);
        check(p.ok && p.d1 == 150 && p.d2 == 50, "d1 and d2 are the minimum one-way differences");
        check(p.theta_lo() <= 50 && 50 <= p.theta_hi(), "true offset lies in [-d2, d1]");
        check(!p.disagreement_proven(), "offset within latency is not proven disagreement");
        check(p.converged, "jitter repeats in both halves: converged");
    }
    // Offset larger than latency: d2 negative, interval excludes zero.
    {
        const auto p = summarise(synthetic(1000, 200, 100), 0);
        check(p.d2 < 0 && p.disagreement_proven(), "offset beyond latency is proven disagreement");
        check(p.theta_lo() > 0, "interval excludes zero");
    }
    // Warm-up is discarded: a too-small sample in it does not count.
    {
        auto t = synthetic(1000, 0, 100);
        t.b_recv[3] = t.a_send[3] + 1;
        const auto with = summarise(t, 0);
        const auto without = summarise(t, 10);
        check(with.d1 == 1 && without.d1 == 100, "warm-up samples are discarded");
        check(without.round_trips == 990 && without.warmup_discarded == 10, "counts reported");
    }
    // Convergence: a new minimum in the second half more than one tick lower.
    {
        auto t = synthetic(1000, 0, 100);
        t.b_recv[900] = t.a_send[900] + 98;
        check(!summarise(t, 0).converged, "second-half minimum 2 ticks lower: not converged");
        t.b_recv[900] = t.a_send[900] + 99;
        check(summarise(t, 0).converged, "second-half minimum 1 tick lower: converged");
    }
    // Input validation.
    {
        auto t = synthetic(10, 0, 100);
        t.a_recv.pop_back();
        check(!summarise(t, 0).ok, "mismatched lengths refused");
        check(!summarise(synthetic(10, 0, 100), 9).ok, "fewer than two round trips refused");
    }

    // CPU list parsing.
    check(parse_cpu_list("0-87\n").size() == 88, "range parsed");
    check((parse_cpu_list("0,2-4,7") == std::vector<int>{0, 2, 3, 4, 7}), "mixed list parsed");
    check(parse_cpu_list("").empty() && parse_cpu_list("0-x").empty() && parse_cpu_list("3-1").empty(),
          "empty and malformed lists give nothing");

    // Pinning failure is reported, not hung on.
    {
        const auto p = measure_pair(0, -1, 100, 10);
        check(!p.ok && !p.reason.empty(), "unpinnable CPU refused with a reason");
    }

    // This machine: CPU 0 against every other online CPU (S5), reported.
    const auto cpus = online_cpus();
    const auto c = calibrate(5, 50);
    const auto ns = [&](std::int64_t t) { return static_cast<double>(t) / c.ticks_per_ns; };
    const auto pairs = measure_cross_cpu(0);
    std::size_t ok = 0, converged = 0, disagree = 0;
    for (const auto& p : pairs) {
        if (!p.ok) {
            std::printf("info  0 vs %d: %s\n", p.b, p.reason.c_str());
            continue;
        }
        ++ok;
        converged += p.converged;
        disagree += p.disagreement_proven();
        std::printf("info  0 vs %2d  theta [%7.1f, %7.1f] ns  width %6.1f ns%s\n", p.b,
                    ns(p.theta_lo()), ns(p.theta_hi()), ns(p.d1 + p.d2),
                    p.converged ? "" : "  not converged");
    }
    std::printf("info  %zu pairs, %zu converged, %zu proven disagreement\n", pairs.size(), converged,
                disagree);
    check(!cpus.empty() && pairs.size() == cpus.size() - 1, "CPU 0 against every other online CPU");
    check(ok == pairs.size(), "every pair measured");
    // Sanity check for S8: unfenced reads could show false disagreement here.
    check(disagree == 0, "no pair proves disagreement on this machine");

    std::printf("%s\n", failures ? "FAILED" : "all passed");
    return failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
