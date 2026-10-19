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
#include <lfipc/bench/placement.hpp>
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
    std::vector<std::unique_ptr<load_result>> runs, runs_rt;  // one-way only; with echo
    bool all_ok = clk.ok;
    if (clk.ok) {
        // The pair actually used, measured directly: tighter than combining two
        // bounds against CPU 0 from the clock block.
        pair = measure_pair(producer, consumer);
        // Each rate twice: one-way only, then with echo (note 003, amended).
        for (const double rate : rates) {
            for (int mode = 0; mode < 2; ++mode) {
                load_config c;
                c.rate_per_s = rate;
                c.messages = static_cast<std::size_t>(rate * seconds_per_rate);
                c.producer_cpu = producer;
                c.consumer_cpu = consumer;
                c.ticks_per_ns = clk.cal.ticks_per_ns;
                // On the producer's CPU, so on its node (placement.hpp).
                auto out = make_on_cpu<lfipc::test::plain>(producer);
                auto back = make_on_cpu<lfipc::test::plain>(producer);
                auto r = std::make_unique<load_result>();
                if (mode) run_load(*out, *back, c, *r);
                else run_load(*out, c, *r);
                apply_placement(*r, out.get(), producer);
                std::printf("rate %.0f/s %s: %s\n", rate, mode ? "round trip" : "one-way", r->ok ? "ok" : r->reason.c_str());
                all_ok = all_ok && r->ok;
                (mode ? runs_rt : runs).push_back(std::move(r));
            }
        }
    }

    if (clk.ok) write_noise_block(run.w, noise, clk.cal.ticks_per_ns);
    write_clock_block(run.w, clk);
    if (clk.ok) {
        run.w.key("load");
        run.w.begin_object();
        write_load_block(run.w, "one-slot mailbox (tests/test_suts.hpp)", pair, runs, clk.cal.ticks_per_ns, "one_way");
        write_load_block(run.w, "one-slot mailbox (tests/test_suts.hpp), echoed on a second", pair, runs_rt,
                         clk.cal.ticks_per_ns, "round_trip");
        run.w.end_object();
    }
    if (!clk.ok) std::fprintf(stderr, "load_bench: clock refused: %s\n", clk.reason.c_str());
    return run.finish(all_ok, "load_bench");
}
