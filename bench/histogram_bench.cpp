// Measures histogram record() cost and writes the run JSON. Design: docs/design/002-histogram.md
//
// Usage: histogram_bench [output.json]
// Default output: results/histogram-<host>-<UTC time>.json in the source tree.
// Exit status: 0 published, 1 refused (reason in the JSON), 2 cannot write.
#include <lfipc/bench/clock_block.hpp>
#include <lfipc/bench/record_cost.hpp>

#include <cstdio>
#include <ctime>
#include <fstream>
#include <string>
#include <unistd.h>

namespace {

std::string default_path() {
    char host[256] = "unknown";
    gethostname(host, sizeof(host) - 1);
    char stamp[32];
    const std::time_t now = std::time(nullptr);
    std::tm utc;
    gmtime_r(&now, &utc);
    std::strftime(stamp, sizeof(stamp), "%Y%m%dT%H%M%SZ", &utc);
    return std::string(LFIPC_RESULTS_DIR) + "/histogram-" + host + "-" + stamp + ".json";
}

}  // namespace

int main(int argc, char** argv) {
    using namespace lfipc::bench;
    const std::string path = argc > 1 ? argv[1] : default_path();

    auto clk = run_clock();
    std::vector<record_cost_case> cost;
    if (clk.ok) {
        cost = measure_record_cost();
        if (cost.empty()) {
            clk.ok = false;
            clk.reason = "cannot pin to CPU 1 for record cost";
        }
    }

    json_writer w;
    w.begin_object();
    write_clock_block(w, clk);
    if (clk.ok) write_record_cost_block(w, cost, clk.cal.ticks_per_ns);
    w.end_object();

    std::ofstream f(path);
    f << w.str() << '\n';
    f.close();
    if (!f) {
        std::fprintf(stderr, "histogram_bench: cannot write %s\n", path.c_str());
        return 2;
    }
    if (!clk.ok) {
        std::fprintf(stderr, "histogram_bench: refused: %s\nwrote %s\n", clk.reason.c_str(), path.c_str());
        return 1;
    }
    std::printf("wrote %s\n", path.c_str());
    return 0;
}
