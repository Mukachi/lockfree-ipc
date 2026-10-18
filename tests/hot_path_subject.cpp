// Subjects for check_hot_path.py, which disassembles this binary. The real
// overhead loop sits behind a fixed, unmangled symbol (S6); the cross-CPU
// threads are found by their lambda names (S8). Counts come from the command
// line so the compiler cannot specialise the loops. Spec: docs/design/001-clock.md
#include <lfipc/bench/cross_cpu.hpp>
#include <lfipc/bench/overhead.hpp>

#include <cstdlib>

extern "C" [[gnu::noinline]] lfipc::bench::overhead lfipc_s6_overhead(std::size_t samples,
                                                                     std::size_t warmup) {
    return lfipc::bench::measure_overhead(samples, warmup, 0);
}

int main(int argc, char** argv) {
    const std::size_t n = argc > 1 ? std::strtoul(argv[1], nullptr, 10) : 1000;
    const bool ok = lfipc_s6_overhead(n, n / 10).samples == n &&
                    lfipc::bench::measure_pair(0, 1, n, n / 10).ok;
    return ok ? 0 : 1;
}
