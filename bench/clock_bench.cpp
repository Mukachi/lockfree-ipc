// Runs the clock flow and writes the run JSON. Design: docs/design/001-clock.md
//
// Usage: clock_bench [output.json]
// Default output: results/clock-<host>-<UTC time>.json in the source tree.
// Exit status: 0 published, 1 refused (reason in the JSON), 2 cannot write.
#include <lfipc/bench/clock_block.hpp>
#include <lfipc/bench/noise_block.hpp>
#include <lfipc/bench/run_file.hpp>

#include <cstdio>

int main(int argc, char** argv) {
    using namespace lfipc::bench;
    run_file run(argc, argv, LFIPC_RESULTS_DIR, "clock");
    if (run.refused) return run.finish(false, "clock_bench");

    // The floor on the CPU whose percentiles are published, before measuring.
    const auto noise = sample_noise({clock_options{}.overhead_cpu}, 2.0);
    const auto r = run_clock();
    if (r.ok) write_noise_block(run.w, noise, r.cal.ticks_per_ns);
    write_clock_block(run.w, r);
    if (!r.ok) std::fprintf(stderr, "clock_bench: refused: %s\n", r.reason.c_str());
    return run.finish(r.ok, "clock_bench");
}
