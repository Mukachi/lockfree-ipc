// Open-loop sweep against the one-slot test mailbox, and the run JSON.
// Design: docs/design/003-open-loop-load.md
//
// Usage: load_bench [output.json]
// Default output: results/load-<host>-<UTC time>.json in the source tree.
// Exit status: 0 every rate published, 1 a refusal (reasons in the JSON), 2 cannot write.
#include "../tests/test_suts.hpp"

#include <lfipc/bench/clock_block.hpp>
#include <lfipc/bench/load_block.hpp>

#include <cstdio>
#include <ctime>
#include <fstream>
#include <memory>
#include <string>
#include <unistd.h>
#include <vector>

namespace {

std::string default_path() {
    char host[256] = "unknown";
    gethostname(host, sizeof(host) - 1);
    char stamp[32];
    const std::time_t now = std::time(nullptr);
    std::tm utc;
    gmtime_r(&now, &utc);
    std::strftime(stamp, sizeof(stamp), "%Y%m%dT%H%M%SZ", &utc);
    return std::string(LFIPC_RESULTS_DIR) + "/load-" + host + "-" + stamp + ".json";
}

}  // namespace

int main(int argc, char** argv) {
    using namespace lfipc::bench;
    const std::string path = argc > 1 ? argv[1] : default_path();
    constexpr int producer = 2, consumer = 3;
    const double rates[] = {1e5, 2.5e5, 5e5, 1e6};
    constexpr double seconds_per_rate = 10.0;

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

    json_writer w;
    w.begin_object();
    write_clock_block(w, clk);
    if (clk.ok) write_load_block(w, "one-slot mailbox (tests/test_suts.hpp), echoed on a second", pair, runs,
                                 clk.cal.ticks_per_ns);
    w.end_object();

    std::ofstream f(path);
    f << w.str() << '\n';
    f.close();
    if (!f) {
        std::fprintf(stderr, "load_bench: cannot write %s\n", path.c_str());
        return 2;
    }
    if (!clk.ok) std::fprintf(stderr, "load_bench: clock refused: %s\n", clk.reason.c_str());
    std::printf("wrote %s\n", path.c_str());
    return all_ok ? 0 : 1;
}
