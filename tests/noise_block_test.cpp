// Tests for include/lfipc/bench/noise_block.hpp. Spec: docs/design/005-noise-floor.md
//
// Writes noise_block.json, a short sample on one CPU (2 on dev-lnx01) and a refused one, in run
// order (environment, noise, clock), for check_noise_json.py.
#include "test_machine.hpp"

#include <lfipc/bench/noise_block.hpp>

#include <cstdio>
#include <cstdlib>
#include <fstream>

int main() {
    using namespace lfipc::bench;
    const int cpu = lfipc::test::pick_cpus(1).value().front();
    auto samples = sample_noise({cpu, -1}, 0.2);
    const double tpn = calibrate(3, 50).ticks_per_ns;

    // Classes from a known histogram: boundaries at 10 us, 100 us, 1 ms.
    histogram<> h;
    const auto us = [tpn](double x) { return static_cast<ticks>(x * 1e3 * tpn); };
    for (double x : {2.0, 9.0, 50.0, 99.0, 500.0, 2000.0, 5000.0}) h.record(us(x));
    const auto c = classify_stops(h, tpn);
    const bool classes = c.to_10us == 2 && c.to_100us == 2 && c.to_1ms == 1 && c.over_1ms == 2;
    std::printf("%s  stop classes split at 10 us, 100 us and 1 ms\n", classes ? "pass" : "FAIL");

    json_writer w;
    w.begin_object();
    w.key("environment");
    w.begin_object();
    w.end_object();
    write_noise_block(w, samples, tpn);
    w.key("clock");
    w.begin_object();
    w.end_object();
    w.end_object();
    std::ofstream("noise_block.json") << w.str() << '\n';
    const bool ok = samples[0]->ok && !samples[1]->ok;
    std::printf("%s  sampled CPU %d, refused CPU -1\n", ok ? "pass" : "FAIL", cpu);
    std::printf("%s\n", classes && ok ? "all passed" : "FAILED");
    return classes && ok ? EXIT_SUCCESS : EXIT_FAILURE;
}
