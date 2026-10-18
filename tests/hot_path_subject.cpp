// Subjects for check_hot_path.py, which disassembles this binary. The real
// overhead loop sits behind a fixed, unmangled symbol (S6); the cross-CPU
// threads are found by their lambda names (S8). The histogram's record() sits
// behind its own symbol (note 002, S1). The load generator's threads are found
// by their lambda names inside run_load_impl, run with the test mailbox (note 003, S6). Counts come from the command
// line so the compiler cannot specialise the loops. Spec: docs/design/001-clock.md
#include <lfipc/bench/cross_cpu.hpp>
#include <lfipc/bench/histogram.hpp>
#include <lfipc/bench/load.hpp>

#include "test_suts.hpp"

#include <memory>
#include <lfipc/bench/overhead.hpp>

#include <cstdlib>

extern "C" [[gnu::noinline]] lfipc::bench::overhead lfipc_s6_overhead(std::size_t samples,
                                                                     std::size_t warmup) {
    return lfipc::bench::measure_overhead(samples, warmup, 0);
}

extern "C" [[gnu::noinline]] void lfipc_hist_record(lfipc::bench::histogram<>& h, lfipc::bench::ticks v) {
    h.record(v);
}

int main(int argc, char** argv) {
    const std::size_t n = argc > 1 ? std::strtoul(argv[1], nullptr, 10) : 1000;
    {
        // Both generator variants, so both are compiled into this binary.
        lfipc::bench::load_config c;
        c.ticks_per_ns = 2.2;
        c.messages = 1'000;
        c.rate_per_s = 1e5;
        auto r = std::make_unique<lfipc::bench::load_result>();
        lfipc::test::plain out, back;
        lfipc::bench::run_load(out, c, *r);
        lfipc::bench::run_load(out, back, c, *r);
    }
    lfipc::bench::histogram<> h;
    lfipc_hist_record(h, n);
    const bool ok = h.count() == 1 && lfipc_s6_overhead(n, n / 10).samples == n &&
                    lfipc::bench::measure_pair(0, 1, n, n / 10).ok;
    return ok ? 0 : 1;
}
