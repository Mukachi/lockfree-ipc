// Tests for include/lfipc/bench/histogram_block.hpp. Spec: docs/design/002-histogram.md
//
// Writes histogram_full.json (2M samples, with overflow) and histogram_empty.json
// in the working directory. check_histogram_json.py then checks fields, the
// 10-sample rule, bucket totals and that no mean appears (S5, S6).
#include <lfipc/bench/histogram_block.hpp>

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <random>
#include <string>

namespace {

int failures = 0;

void check(bool cond, const char* what) {
    std::printf("%s  %s\n", cond ? "pass" : "FAIL", what);
    if (!cond) ++failures;
}

using namespace lfipc::bench;

std::string block(const histogram<>& h) {
    json_writer w;
    w.begin_object();
    write_histogram_block(w, h, 2.0);
    w.end_object();
    return w.str();
}

}  // namespace

int main() {
    histogram<> full;
    std::mt19937_64 rng(7);
    std::lognormal_distribution<double> ln(6.0, 1.5);
    for (int i = 0; i < 2'000'000; ++i) full.record(static_cast<ticks>(ln(rng)));
    full.record(ticks{1} << 45);  // one overflow
    const auto f = block(full);
    std::ofstream("histogram_full.json") << f << '\n';
    std::ofstream("histogram_empty.json") << block(histogram<>()) << '\n';

    // Spot checks here; the full field check is in check_histogram_json.py.
    check(f.find("\"overflow\": 1") != std::string::npos, "overflow published");
    check(f.find("\"p99999\": null") == std::string::npos, "p99.999 published at 2M samples");
    const auto e = block(histogram<>());
    check(e.find("\"p50\": null") != std::string::npos && e.find("\"buckets\": []") != std::string::npos,
          "empty histogram: null percentiles, no buckets");
    // One bucket per line, on one line each.
    check(f.find("[1, ") != std::string::npos || f.find("[2, ") != std::string::npos, "buckets written inline");

    std::printf("%s\n", failures ? "FAILED" : "all passed");
    return failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
