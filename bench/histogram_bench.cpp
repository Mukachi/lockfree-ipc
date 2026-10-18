// Measures histogram record() cost and writes the run JSON. Design: docs/design/002-histogram.md
//
// Usage: histogram_bench [output.json]
// Default output: results/histogram-<host>-<UTC time>.json in the source tree.
// Exit status: 0 published, 1 refused (reason in the JSON), 2 cannot write.
#include <lfipc/bench/clock_block.hpp>
#include <lfipc/bench/noise_block.hpp>
#include <lfipc/bench/record_cost.hpp>
#include <lfipc/bench/run_file.hpp>

#include <cstdio>

int main(int argc, char** argv) {
    using namespace lfipc::bench;
    run_file run(argc, argv, LFIPC_RESULTS_DIR, "histogram");
    if (run.refused) return run.finish(false, "histogram_bench");

    // The floor on the CPU whose percentiles are published, before measuring.
    const auto noise = sample_noise({clock_options{}.overhead_cpu}, 2.0);
    auto clk = run_clock();
    std::vector<record_cost_case> cost;
    if (clk.ok) {
        cost = measure_record_cost();
        if (cost.empty()) {
            clk.ok = false;
            clk.reason = "cannot pin to CPU 2 for record cost";
        }
    }
    if (clk.cal.ok) write_noise_block(run.w, noise, clk.cal.ticks_per_ns);
    write_clock_block(run.w, clk);
    if (clk.ok) write_record_cost_block(run.w, cost, clk.cal.ticks_per_ns);
    if (!clk.ok) std::fprintf(stderr, "histogram_bench: refused: %s\n", clk.reason.c_str());
    return run.finish(clk.ok, "histogram_bench");
}
