// Note 012's cells through the open-loop generator, one-way and round trip,
// on hyper-thread siblings (2 -> 46) and, for A and C, on separate cores
// (2 -> 3) as the control; and the run JSON.
// Design: docs/design/012-siblings.md
//
// Usage: siblings_bench [output.json [seconds_per_rate]]
//        siblings_bench --cell A|B|C|D|R one_way|round_trip <rate> <seconds> [producer consumer]
// Default output: results/siblings-<host>-<UTC time>.json; 10 s per rate.
// --cell runs one cell for a counter run under perf (note 012): no JSON,
// nothing written to results/; it prints the messages sent and the latency.
// For each pair and rate the cells run back to back, one-way then round trip.
// Exit status: 0 published, 1 refused (bench mode or clock; reason in the JSON),
// 2 cannot write.
#include "siblings.hpp"

#include <lfipc/bench/clock_block.hpp>
#include <lfipc/bench/load_block.hpp>
#include <lfipc/bench/noise_block.hpp>
#include <lfipc/bench/run_file.hpp>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <memory>
#include <vector>

namespace {

using namespace lfipc::bench;

struct cell_entry {
    cell k;
    const char* name;
    const char* sut;
};

const cell_entry cells[] = {
    {cell::A, "A", "v3 on the segment, two threads, one mapping"},
    {cell::B, "B", "v3 on the segment, two threads, two mappings"},
    {cell::C, "C", "v3 on the segment, two processes, the child attaching by name"},
    {cell::D, "D", "v3 on the segment, two processes, the child using the inherited mapping"},
    {cell::R, "R", "v3 on the heap, two threads (note 006's layout)"},
};

struct pair_entry {
    int producer, consumer;
    std::vector<cell> cells;
};

// One cell, for perf: memory bound to the producer's node as run_file does.
int diagnose(int argc, char** argv) {
    if (argc < 6) {
        std::fprintf(stderr, "usage: siblings_bench --cell A|B|C|D|R one_way|round_trip <rate> <seconds> [producer consumer]\n");
        return 2;
    }
    const char* which = std::strchr("ABCDR", argv[2][0]);
    const bool echo = std::string(argv[3]) == "round_trip";
    if (!which || argv[2][1] != '\0' || (!echo && std::string(argv[3]) != "one_way")) {
        std::fprintf(stderr, "siblings_bench: unknown cell or mode\n");
        return 2;
    }
    load_config c;
    c.rate_per_s = std::atof(argv[4]);
    c.messages = static_cast<std::size_t>(c.rate_per_s * std::atof(argv[5]));
    c.producer_cpu = argc > 7 ? std::atoi(argv[6]) : 2;
    c.consumer_cpu = argc > 7 ? std::atoi(argv[7]) : 46;
    std::string err;
    if (!bind_memory_to_node(cpu_node(c.producer_cpu), err)) {
        std::fprintf(stderr, "siblings_bench: %s\n", err.c_str());
        return 1;
    }
    c.ticks_per_ns = calibrate().ticks_per_ns;
    auto r = std::make_unique<load_result>();
    xproc_info info;
    run_cell(static_cast<cell>(which - "ABCDR"), echo, c, *r, info);
    const auto& h = echo ? r->round_trip : r->latency;
    const auto ns = [&](double q) { return static_cast<double>(h.percentile(q).value_or(0)) / c.ticks_per_ns; };
    std::printf("cell %c %s %.0f/s on %d -> %d: %s; sent %zu (warm-up %zu); p50 %.0f ns, p99 %.0f ns, p99.9 %.0f ns; "
                "memory node %d\n",
                *which, argv[3], c.rate_per_s, c.producer_cpu, c.consumer_cpu, r->ok ? "ok" : r->reason.c_str(),
                r->messages + r->warmup_discarded, r->warmup_discarded, ns(50), ns(99), ns(99.9), r->memory_node);
    return r->ok ? 0 : 1;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc > 1 && std::string(argv[1]) == "--cell") return diagnose(argc, argv);
    run_file run(argc, argv, LFIPC_RESULTS_DIR, "siblings");
    if (run.refused) return run.finish(false, "siblings_bench");

    const double seconds_per_rate = argc > 2 ? std::atof(argv[2]) : 10.0;
    const pair_entry pairs[] = {
        {2, 46, {cell::A, cell::B, cell::C, cell::D, cell::R}},  // hyper-thread siblings
        {2, 3, {cell::A, cell::C}},                              // control: separate cores
    };
    const double rates[] = {1e5, 1e6, 2e6, 5e6};

    const auto noise = sample_noise({2, 3, 46}, 2.0);
    const auto clk = run_clock();
    if (!clk.ok) {
        write_clock_block(run.w, clk);
        std::fprintf(stderr, "siblings_bench: clock refused: %s\n", clk.reason.c_str());
        return run.finish(false, "siblings_bench");
    }

    write_noise_block(run.w, noise, clk.cal.ticks_per_ns);
    write_clock_block(run.w, clk);
    run.w.key("siblings");
    run.w.begin_object();
    run.w.member("capacity", shm_queue::ring_type::capacity);
    run.w.member("message_bytes", sizeof(message));
    run.w.member("seconds_per_rate", seconds_per_rate);
    run.w.key("pairs");
    run.w.begin_array();
    for (const auto& p : pairs) {
        const auto bound = measure_pair(p.producer, p.consumer);
        // results[cell][mode]: one result per rate; info from the last one-way run.
        std::vector<std::vector<std::unique_ptr<load_result>>> results[2];
        results[0].resize(p.cells.size());
        results[1].resize(p.cells.size());
        std::vector<xproc_info> info(p.cells.size());
        for (const double rate : rates) {
            for (std::size_t i = 0; i < p.cells.size(); ++i) {
                for (int mode = 0; mode < 2; ++mode) {
                    load_config c;
                    c.rate_per_s = rate;
                    c.messages = static_cast<std::size_t>(rate * seconds_per_rate);
                    c.producer_cpu = p.producer;
                    c.consumer_cpu = p.consumer;
                    c.ticks_per_ns = clk.cal.ticks_per_ns;
                    auto r = std::make_unique<load_result>();
                    xproc_info seen;
                    run_cell(p.cells[i], mode == 1, c, *r, seen);
                    if (mode == 0) info[i] = seen;
                    std::printf("cpus %d->%-2d %9.0f/s cell %s %-10s %s\n", p.producer, p.consumer, rate,
                                cells[static_cast<int>(p.cells[i])].name, mode ? "round trip" : "one-way",
                                r->ok ? "ok" : r->reason.c_str());
                    std::fflush(stdout);
                    results[mode][i].push_back(std::move(r));
                }
            }
        }
        run.w.begin_object();
        run.w.member("producer_cpu", p.producer);
        run.w.member("consumer_cpu", p.consumer);
        run.w.key("cells");
        run.w.begin_object();
        for (std::size_t i = 0; i < p.cells.size(); ++i) {
            const auto& e = cells[static_cast<int>(p.cells[i])];
            run.w.key(e.name);
            run.w.begin_object();
            run.w.member("ring_page_offset", info[i].producer_ring % 4096);
            run.w.member("same_address", info[i].producer_ring == info[i].consumer_ring);
            run.w.member("producer_speculation", info[i].producer_speculation);
            run.w.member("consumer_speculation", info[i].consumer_speculation);
            write_load_block(run.w, e.sut, bound, results[0][i], clk.cal.ticks_per_ns, "one_way");
            write_load_block(run.w, e.sut, bound, results[1][i], clk.cal.ticks_per_ns, "round_trip");
            run.w.end_object();
        }
        run.w.end_object();
        run.w.end_object();
    }
    run.w.end_array();
    run.w.end_object();
    return run.finish(true, "siblings_bench");
}
