// Every MPMC queue (note 010) through the open-loop generator, one-way, at a
// sweep of rates and shapes (producers -> consumers); and the run JSON.
// Design: docs/design/010-mpmc-queue.md
//
// Usage: mpmc_bench [output.json [seconds_per_rate]]
// Default output: results/mpmc-<host>-<UTC time>.json; 10 s per rate.
// For each shape and rate the queues run back to back, so queues compared at
// one rate are measured close together in time. 1 -> 1 runs note 003's
// generator (as notes 006 and 008) and adds SPSC v3 as the reference; the
// other shapes run the generator for several producers and consumers. A
// refused rate is a result, not a failure.
// Exit status: 0 published, 1 refused (bench mode or clock; reason in the JSON),
// 2 cannot write.
#include "mpmc_queues.hpp"

#include <lfipc/bench/clock_block.hpp>
#include <lfipc/bench/load_mp_block.hpp>
#include <lfipc/bench/noise_block.hpp>
#include <lfipc/bench/placement.hpp>
#include <lfipc/bench/run_file.hpp>

#include <cstdio>
#include <cstdlib>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace {

using namespace lfipc::bench;

using runner = void (*)(const mp_load_config&, mp_load_result&);

// A fresh queue for every run, constructed on the first producer's CPU, so on
// its node (placement.hpp). The memory policy binds the whole run to that node
// as well, which covers queues that allocate their slots (moodycamel, rigtorp).
template <typename Sut>
void run_queue(const mp_load_config& c, mp_load_result& r) {
    const int cpu = c.producer_cpus.front();
    auto q = make_on_cpu<Sut>(cpu);
    if (!q) {
        r.ok = false;
        r.reason = "cannot pin to producer CPU " + std::to_string(cpu) + " to place the queue";
        return;
    }
    if (c.producer_cpus.size() == 1 && c.consumer_cpus.size() == 1) {
        // Note 003's generator, unchanged, as in notes 006 and 008.
        load_config one;
        one.rate_per_s = c.rate_per_s;
        one.messages = c.messages;
        one.producer_cpu = cpu;
        one.consumer_cpu = c.consumer_cpus.front();
        one.ticks_per_ns = c.ticks_per_ns;
        static_cast<load_result&>(r) = {};
        run_load(*q, one, r);
        r.producer_cpus = c.producer_cpus;
        r.consumer_cpus = c.consumer_cpus;
        r.sent = r.messages + r.warmup_discarded;
        // It returns only once every message has arrived (or it could not pin).
        r.received = r.reason.rfind("cannot pin", 0) == 0 ? 0 : r.sent;
    } else {
        run_load_mp(*q, c, r);
    }
    apply_placement(r, q.get(), cpu);
}

struct queue_entry {
    const char* name;
    const char* sut;
    runner run;
    bool spsc;  // single producer and consumer only: 1 -> 1
};

const queue_entry queues[] = {
    {"mpmc_v1", "lfipc mpmc v1: Vyukov's bounded queue, slots packed", run_queue<ring_sut<lfipc::mpmc_v1>>, false},
    {"mpmc_v2", "lfipc mpmc v2: v1 with each slot on its own 128 bytes", run_queue<ring_sut<lfipc::mpmc_v2>>, false},
    {"moodycamel", "moodycamel::ConcurrentQueue " LFIPC_CQ_VERSION ", implicit producers (no tokens)",
     run_queue<moodycamel_mpmc_sut>, false},
    {"boost", "boost::lockfree::queue " LFIPC_BOOST_VERSION ", fixed pool", run_queue<boost_mpmc_sut>, false},
    {"rigtorp", "rigtorp::MPMCQueue " LFIPC_RIGTORP_VERSION, run_queue<rigtorp_sut>, false},
    {"spsc_v3", "lfipc spsc v3 (reference)", run_queue<ring_sut<lfipc::spsc_v3>>, true},
};

struct shape {
    std::vector<int> producers, consumers;
};

}  // namespace

int main(int argc, char** argv) {
    run_file run(argc, argv, LFIPC_RESULTS_DIR, "mpmc");
    if (run.refused) return run.finish(false, "mpmc_bench");

    const double seconds_per_rate = argc > 2 ? std::atof(argv[2]) : 10.0;
    // Node 0's reserved cores, producers from CPU 2 upward, then consumers.
    const shape shapes[] = {
        {{2}, {3}}, {{2, 3}, {4}}, {{2, 3, 4, 5}, {6}}, {{2, 3}, {4, 5}}, {{2, 3, 4, 5}, {6, 7, 8, 9}},
    };
    const double rates[] = {1e5, 1e6, 2e6, 5e6, 1e7};

    // The floor on every CPU used, before measuring.
    const auto noise = sample_noise({2, 3, 4, 5, 6, 7, 8, 9}, 2.0);
    const auto clk = run_clock();
    if (!clk.ok) {
        write_clock_block(run.w, clk);
        std::fprintf(stderr, "mpmc_bench: clock refused: %s\n", clk.reason.c_str());
        return run.finish(false, "mpmc_bench");
    }

    // Offset bounds for every producer -> consumer pair any shape uses, once.
    std::map<std::pair<int, int>, cross_cpu_pair> bound;
    for (const auto& s : shapes)
        for (int p : s.producers)
            for (int c : s.consumers)
                if (!bound.count({p, c})) bound[{p, c}] = measure_pair(p, c);

    // results[shape][queue] holds one result per rate, in rate order; empty
    // for a queue the shape does not take.
    std::vector<std::vector<std::vector<std::unique_ptr<mp_load_result>>>> results(std::size(shapes));
    for (std::size_t s = 0; s < std::size(shapes); ++s) {
        const bool one = shapes[s].producers.size() == 1 && shapes[s].consumers.size() == 1;
        results[s].resize(std::size(queues));
        for (const double rate : rates) {
            for (std::size_t q = 0; q < std::size(queues); ++q) {
                if (queues[q].spsc && !one) continue;
                mp_load_config c;
                c.rate_per_s = rate;
                c.messages = static_cast<std::size_t>(rate * seconds_per_rate);
                c.producer_cpus = shapes[s].producers;
                c.consumer_cpus = shapes[s].consumers;
                c.ticks_per_ns = clk.cal.ticks_per_ns;
                auto r = std::make_unique<mp_load_result>();
                queues[q].run(c, *r);
                std::printf("%zu -> %zu %9.0f/s %-10s %s\n", c.producer_cpus.size(), c.consumer_cpus.size(), rate,
                            queues[q].name, r->ok ? "ok" : r->reason.c_str());
                std::fflush(stdout);
                results[s][q].push_back(std::move(r));
            }
        }
    }

    write_noise_block(run.w, noise, clk.cal.ticks_per_ns);
    write_clock_block(run.w, clk);
    run.w.key("mpmc");
    run.w.begin_object();
    run.w.member("capacity", queue_capacity);
    run.w.member("message_bytes", sizeof(message));
    run.w.member("seconds_per_rate", seconds_per_rate);
    run.w.key("baselines");
    run.w.begin_object();
    run.w.member("moodycamel", LFIPC_CQ_VERSION);
    run.w.member("boost", LFIPC_BOOST_VERSION);
    run.w.member("rigtorp", LFIPC_RIGTORP_VERSION);
    run.w.end_object();
    run.w.key("shapes");
    run.w.begin_array();
    for (std::size_t s = 0; s < std::size(shapes); ++s) {
        std::vector<cross_cpu_pair> bounds;
        for (int p : shapes[s].producers)
            for (int c : shapes[s].consumers) bounds.push_back(bound[{p, c}]);
        run.w.begin_object();
        run.w.member("producers", shapes[s].producers.size());
        run.w.member("consumers", shapes[s].consumers.size());
        run.w.key("queues");
        run.w.begin_object();
        for (std::size_t q = 0; q < std::size(queues); ++q) {
            if (results[s][q].empty()) continue;
            run.w.key(queues[q].name);
            run.w.begin_object();
            write_mp_load_block(run.w, queues[q].sut, bounds, results[s][q], clk.cal.ticks_per_ns);
            run.w.end_object();
        }
        run.w.end_object();
        run.w.end_object();
    }
    run.w.end_array();
    run.w.end_object();
    return run.finish(true, "mpmc_bench");
}
