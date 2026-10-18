// Tests for include/lfipc/bench/load_block.hpp. Spec: docs/design/003-open-loop-load.md
//
// Writes load_block.json from synthetic runs, one published and one refused,
// for check_load_json.py, so both kinds of rate are checked.
#include <lfipc/bench/load_block.hpp>

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

int main() {
    using namespace lfipc::bench;

    cross_cpu_pair pair;
    pair.ok = true;
    pair.a = 2;
    pair.b = 3;
    pair.d1 = 200;
    pair.d2 = 180;
    pair.round_trips = 100'000;
    pair.converged = true;

    std::vector<std::unique_ptr<load_result>> runs;
    auto good = std::make_unique<load_result>();
    good->ok = true;
    good->rate_per_s = 1e5;
    good->interval_ticks = 20'000;
    good->messages = 20'000;
    good->warmup_discarded = 10'000;
    good->has_round_trip = true;
    for (int i = 0; i < 20'000; ++i) {
        good->lateness.record(100 + i % 7);
        good->latency.record(400 + i % 50);
        good->round_trip.record(800 + i % 90);
    }
    runs.push_back(std::move(good));

    auto bad = std::make_unique<load_result>();
    bad->rate_per_s = 1e6;
    bad->interval_ticks = 2'000;
    bad->messages = 20'000;
    bad->warmup_discarded = 100'000;
    bad->has_round_trip = true;
    bad->reason = "producer lateness p99 5000 ns exceeds the send interval 1000 ns";
    for (int i = 0; i < 20'000; ++i) bad->lateness.record(i % 50 ? 100 : 10'000);
    runs.push_back(std::move(bad));

    json_writer w;
    w.begin_object();
    write_load_block(w, "synthetic", pair, runs, 2.0);
    w.end_object();
    std::ofstream("load_block.json") << w.str() << '\n';

    const bool ok = w.str().find("\"theta_hi_ns\": 100") != std::string::npos &&
                    w.str().find("\"theta_lo_ns\": -90") != std::string::npos;
    std::printf("%s  offset bound converted to ns\n", ok ? "pass" : "FAIL");
    std::printf("%s\n", ok ? "all passed" : "FAILED");
    return ok ? EXIT_SUCCESS : EXIT_FAILURE;
}
