// v3 across two processes over shared memory (note 008), against v3 across two
// threads, in the same run; and the run JSON.
// Design: docs/design/008-shm-transport.md
//
// Usage: shm_bench [output.json [seconds_per_rate]]
// Default output: results/shm-<host>-<UTC time>.json; 10 s per rate.
// For each pair, rate and mode, threads then processes run back to back, so
// the two are compared under the same conditions. One-way and round trip are
// separate runs (note 003, amended). A refused rate is a result, not a failure.
// Exit status: 0 published, 1 refused (bench mode or clock), 2 cannot write.
#include "queues.hpp"

#include <lfipc/bench/clock_block.hpp>
#include <lfipc/bench/load_block.hpp>
#include <lfipc/bench/load_xproc.hpp>
#include <lfipc/bench/noise_block.hpp>
#include <lfipc/bench/placement.hpp>
#include <lfipc/bench/run_file.hpp>

#include <cstdio>
#include <cstdlib>
#include <memory>
#include <vector>

namespace {

using namespace lfipc::bench;

// Queues constructed on the producer's CPU, so on its node, as the process
// version's segments are (placement.hpp).
void run_threads(const load_config& c, load_result& r, bool echo) {
    using sut = ring_sut<lfipc::spsc_v3>;
    auto out = make_on_cpu<sut>(c.producer_cpu);
    auto back = echo ? make_on_cpu<sut>(c.producer_cpu) : nullptr;
    if (!out || (echo && !back)) {
        r = {};
        r.reason = "cannot pin to producer CPU " + std::to_string(c.producer_cpu) + " to place the queue";
        return;
    }
    if (echo) run_load(*out, *back, c, r);
    else run_load(*out, c, r);
    apply_placement(r, out.get(), c.producer_cpu);
}

void run_processes(const load_config& c, load_result& r, bool echo) {
    if (echo) run_load_xproc<true>(c, r);
    else run_load_xproc<false>(c, r);
}

struct transport {
    const char* name;
    const char* sut;
    void (*run)(const load_config&, load_result&, bool);
};

const transport transports[] = {
    {"threads", "lfipc spsc v3, two threads of one process", run_threads},
    {"processes", "lfipc spsc v3 in a shm_open segment, two processes (fork; consumer attaches by name)",
     run_processes},
};

struct cpu_pair {
    int producer, consumer;
};

}  // namespace

int main(int argc, char** argv) {
    run_file run(argc, argv, LFIPC_RESULTS_DIR, "shm");
    if (run.refused) return run.finish(false, "shm_bench");

    const double seconds_per_rate = argc > 2 ? std::atof(argv[2]) : 10.0;
    const cpu_pair pairs[] = {{2, 3}, {2, 46}};
    const double rates[] = {1e5, 1e6, 2e6, 5e6, 1e7};

    const auto noise = sample_noise({2, 3, 46}, 2.0);
    const auto clk = run_clock();
    if (!clk.ok) {
        write_clock_block(run.w, clk);
        std::fprintf(stderr, "shm_bench: clock refused: %s\n", clk.reason.c_str());
        return run.finish(false, "shm_bench");
    }

    // results[mode][pair][transport]: one load_result per rate; mode 0 one-way, 1 echo.
    std::vector<cross_cpu_pair> bounds;
    std::vector<std::vector<std::vector<std::unique_ptr<load_result>>>> results[2];
    for (auto& m : results) m.resize(std::size(pairs));
    for (std::size_t p = 0; p < std::size(pairs); ++p) {
        bounds.push_back(measure_pair(pairs[p].producer, pairs[p].consumer));
        for (auto& m : results) m[p].resize(std::size(transports));
        for (const double rate : rates) {
            for (int mode = 0; mode < 2; ++mode) {
                for (std::size_t t = 0; t < std::size(transports); ++t) {
                    load_config c;
                    c.rate_per_s = rate;
                    c.messages = static_cast<std::size_t>(rate * seconds_per_rate);
                    c.producer_cpu = pairs[p].producer;
                    c.consumer_cpu = pairs[p].consumer;
                    c.ticks_per_ns = clk.cal.ticks_per_ns;
                    auto r = std::make_unique<load_result>();
                    transports[t].run(c, *r, mode == 1);
                    std::printf("cpus %d->%-2d %9.0f/s %-10s %-9s %s\n", pairs[p].producer, pairs[p].consumer, rate,
                                mode ? "round trip" : "one-way", transports[t].name, r->ok ? "ok" : r->reason.c_str());
                    std::fflush(stdout);
                    results[mode][p][t].push_back(std::move(r));
                }
            }
        }
    }

    write_noise_block(run.w, noise, clk.cal.ticks_per_ns);
    write_clock_block(run.w, clk);
    run.w.key("shm");
    run.w.begin_object();
    run.w.member("capacity", queue_capacity);
    run.w.member("message_bytes", sizeof(message));
    run.w.member("seconds_per_rate", seconds_per_rate);
    run.w.key("pairs");
    run.w.begin_array();
    for (std::size_t p = 0; p < std::size(pairs); ++p) {
        run.w.begin_object();
        run.w.member("producer_cpu", pairs[p].producer);
        run.w.member("consumer_cpu", pairs[p].consumer);
        run.w.key("transports");
        run.w.begin_object();
        for (std::size_t t = 0; t < std::size(transports); ++t) {
            run.w.key(transports[t].name);
            run.w.begin_object();
            write_load_block(run.w, transports[t].sut, bounds[p], results[0][p][t], clk.cal.ticks_per_ns, "one_way");
            write_load_block(run.w, transports[t].sut, bounds[p], results[1][p][t], clk.cal.ticks_per_ns, "round_trip");
            run.w.end_object();
        }
        run.w.end_object();
        run.w.end_object();
    }
    run.w.end_array();
    run.w.end_object();
    return run.finish(true, "shm_bench");
}
