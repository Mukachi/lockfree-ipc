// Open-loop sweep against the one-slot test mailbox, and the run JSON.
// Design: docs/design/003-open-loop-load.md
//
// Usage: load_bench [output.json]
// Default output: results/load-<host>-<UTC time>.json in the source tree.
// Exit status: 0 every rate published, 1 a refusal (reasons in the JSON), 2 cannot write.
#include "../tests/test_suts.hpp"

#include <lfipc/bench/clock_block.hpp>
#include <lfipc/bench/load_block.hpp>
#include <lfipc/bench/noise_block.hpp>
#include <lfipc/bench/run_file.hpp>

#include <cstdio>
#include <memory>
#include <vector>

int main(int argc, char** argv) {
    using namespace lfipc::bench;
    run_file run(argc, argv, LFIPC_RESULTS_DIR, "load");
    if (run.refused) return run.finish(false, "load_bench");
    constexpr int producer = 2, consumer = 3;
    const double rates[] = {1e5, 2.5e5, 5e5, 1e6};
    constexpr double seconds_per_rate = 10.0;

    // The floor on the producer and consumer CPUs, before measuring.
    const auto noise = sample_noise({producer, consumer}, 2.0);
    const auto clk = run_clock();
    cross_cpu_pair pair;
    std::vector<std::unique_ptr<load_result>> runs;
    bool all_ok = clk.ok;
    if (clk.ok) {
        // The pair actually used, measured directly: tighter than combining two
        // bounds against CPU 0 from the clock block.
        pair = measure_pair(producer, consumer);
        for (const double rate : rates) {
            load_config c;
            c.rate_per_s = rate;
            c.messages = static_cast<std::size_t>(rate * seconds_per_rate);
            c.producer_cpu = producer;
            c.consumer_cpu = consumer;
            c.ticks_per_ns = clk.cal.ticks_per_ns;
            lfipc::test::plain out, back;
            auto r = std::make_unique<load_result>();
            run_load(out, back, c, *r);
            std::printf("rate %.0f/s: %s\n", rate, r->ok ? "ok" : r->reason.c_str());
            all_ok = all_ok && r->ok;
            runs.push_back(std::move(r));
        }
    }

    if (clk.ok) write_noise_block(run.w, noise, clk.cal.ticks_per_ns);
    write_clock_block(run.w, clk);
    if (clk.ok)
        write_load_block(run.w, "one-slot mailbox (tests/test_suts.hpp), echoed on a second", pair, runs,
                         clk.cal.ticks_per_ns);
    if (!clk.ok) std::fprintf(stderr, "load_bench: clock refused: %s\n", clk.reason.c_str());
    return run.finish(all_ok, "load_bench");
}
