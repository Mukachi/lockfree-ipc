// Every SPSC queue (note 006) through the open-loop generator (note 003), one-way
// and round trip, at a sweep of rates on two CPU pairs; and the run JSON.
// Design: docs/design/006-spsc-queue.md
//
// Usage: spsc_bench [output.json [seconds_per_rate]]
// Default output: results/spsc-<host>-<UTC time>.json; 10 s per rate.
// For each pair and rate the five queues run back to back, so queues compared
// at one rate are measured close together in time. A refused rate is a result
// (the generator or the queue could not sustain it), not a failure.
// Exit status: 0 published, 1 refused (bench mode or clock; reason in the JSON),
// 2 cannot write.
#include "queues.hpp"

#include <lfipc/bench/clock_block.hpp>
#include <lfipc/bench/load_block.hpp>
#include <lfipc/bench/noise_block.hpp>
#include <lfipc/bench/run_file.hpp>

#include <cstdio>
#include <cstdlib>
#include <memory>
#include <vector>

namespace {

using namespace lfipc::bench;

using runner = void (*)(const load_config&, load_result&);

// Fresh queues for every run: one each way.
template <typename Sut>
void run_queue(const load_config& c, load_result& r) {
    auto out = std::make_unique<Sut>();
    auto back = std::make_unique<Sut>();
    run_load(*out, *back, c, r);
}

struct queue_entry {
    const char* name;
    const char* sut;
    runner run;
};

const queue_entry queues[] = {
    {"spsc_v1", "lfipc spsc v1: Lamport ring, counters on separate 64-byte lines", run_queue<ring_sut<lfipc::spsc_v1>>},
    {"spsc_v2", "lfipc spsc v2: v1 plus cached counters", run_queue<ring_sut<lfipc::spsc_v2>>},
    {"spsc_v3", "lfipc spsc v3: v2 with 128-byte separation", run_queue<ring_sut<lfipc::spsc_v3>>},
    {"moodycamel", "moodycamel::ReaderWriterQueue " LFIPC_RWQ_VERSION, run_queue<moodycamel_sut>},
    {"boost", "boost::lockfree::spsc_queue " LFIPC_BOOST_VERSION, run_queue<boost_sut>},
};
constexpr std::size_t queue_count = sizeof(queues) / sizeof(queues[0]);

struct cpu_pair {
    int producer, consumer;
};

}  // namespace

int main(int argc, char** argv) {
    run_file run(argc, argv, LFIPC_RESULTS_DIR, "spsc");
    if (run.refused) return run.finish(false, "spsc_bench");

    const double seconds_per_rate = argc > 2 ? std::atof(argv[2]) : 10.0;
    const cpu_pair pairs[] = {{2, 3}, {2, 46}};  // different cores; hyper-thread siblings
    const double rates[] = {1e5, 1e6, 2e6, 5e6, 1e7};

    // The floor on every CPU used, before measuring.
    const auto noise = sample_noise({2, 3, 46}, 2.0);
    const auto clk = run_clock();
    if (!clk.ok) {
        write_clock_block(run.w, clk);
        std::fprintf(stderr, "spsc_bench: clock refused: %s\n", clk.reason.c_str());
        return run.finish(false, "spsc_bench");
    }

    // results[pair][queue] holds one load_result per rate, in rate order.
    std::vector<cross_cpu_pair> bounds;
    std::vector<std::vector<std::vector<std::unique_ptr<load_result>>>> results(std::size(pairs));
    for (std::size_t p = 0; p < std::size(pairs); ++p) {
        bounds.push_back(measure_pair(pairs[p].producer, pairs[p].consumer));
        results[p].resize(queue_count);
        for (const double rate : rates) {
            for (std::size_t q = 0; q < queue_count; ++q) {
                load_config c;
                c.rate_per_s = rate;
                c.messages = static_cast<std::size_t>(rate * seconds_per_rate);
                c.producer_cpu = pairs[p].producer;
                c.consumer_cpu = pairs[p].consumer;
                c.ticks_per_ns = clk.cal.ticks_per_ns;
                auto r = std::make_unique<load_result>();
                queues[q].run(c, *r);
                std::printf("cpus %d->%-2d %9.0f/s %-10s %s\n", pairs[p].producer, pairs[p].consumer, rate,
                            queues[q].name, r->ok ? "ok" : r->reason.c_str());
                std::fflush(stdout);
                results[p][q].push_back(std::move(r));
            }
        }
    }

    write_noise_block(run.w, noise, clk.cal.ticks_per_ns);
    write_clock_block(run.w, clk);
    run.w.key("spsc");
    run.w.begin_object();
    run.w.member("capacity", queue_capacity);
    run.w.member("message_bytes", sizeof(message));
    run.w.member("seconds_per_rate", seconds_per_rate);
    run.w.key("baselines");
    run.w.begin_object();
    run.w.member("moodycamel", LFIPC_RWQ_VERSION);
    run.w.member("boost", LFIPC_BOOST_VERSION);
    run.w.end_object();
    run.w.key("pairs");
    run.w.begin_array();
    for (std::size_t p = 0; p < std::size(pairs); ++p) {
        run.w.begin_object();
        run.w.member("producer_cpu", pairs[p].producer);
        run.w.member("consumer_cpu", pairs[p].consumer);
        run.w.key("queues");
        run.w.begin_object();
        for (std::size_t q = 0; q < queue_count; ++q)
            write_load_block(run.w, queues[q].sut, bounds[p], results[p][q], clk.cal.ticks_per_ns, queues[q].name);
        run.w.end_object();
        run.w.end_object();
    }
    run.w.end_array();
    run.w.end_object();
    return run.finish(true, "spsc_bench");
}
