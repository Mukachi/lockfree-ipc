// Tests for bench/siblings.hpp: every cell of note 012 runs, delivers every
// message, and has the layout and mappings the note says.
// Spec: docs/design/012-siblings.md
#include "../bench/siblings.hpp"
#include "test_machine.hpp"

#include <lfipc/bench/calibrate.hpp>

#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>

namespace {

int failures = 0;

void check(bool cond, const std::string& what) {
    std::printf("%s  %s\n", cond ? "pass" : "FAIL", what.c_str());
    if (!cond) ++failures;
}

using namespace lfipc::bench;

bool finished(const load_result& r) {
    if (!r.ok) std::printf("info  refused: %s\n", r.reason.c_str());
    return r.ok || r.reason.rfind("producer lateness", 0) == 0;
}

}  // namespace

int main() {
    namespace t = lfipc::test;
    const auto cpus = t::pick_cpus(2);
    if (!cpus) {
        t::skip("siblings_test", "needs two CPUs to pin producer and consumer");
        return t::skip_code;
    }
    load_config c;
    c.ticks_per_ns = calibrate(3, 50).ticks_per_ns;
    c.rate_per_s = 1e5;
    c.messages = 20'000;
    c.producer_cpu = (*cpus)[0];
    c.consumer_cpu = (*cpus)[1];
    auto r = std::make_unique<load_result>();

    const char* names = "ABCDR";
    std::uintptr_t offset[5] = {};
    for (int k = 0; k < 5; ++k) {
        const std::string name = std::string("cell ") + names[k];
        for (const bool echo : {false, true}) {
            xproc_info info;
            run_cell(static_cast<cell>(k), echo, c, *r, info);
            check(finished(*r) && r->latency.count() == c.messages && r->out_of_order == 0 &&
                      (!echo || r->round_trip.count() == c.messages),
                  name + (echo ? ", round trip" : ", one-way") + ": every message delivered, in order");
            if (echo) continue;
            offset[k] = info.producer_ring % 4096;
            const bool same = info.producer_ring == info.consumer_ring;
            std::printf("info  %s: ring at page offset %zu; producer %#zx, consumer %#zx; %s\n", name.c_str(),
                        static_cast<std::size_t>(offset[k]), static_cast<std::size_t>(info.producer_ring),
                        static_cast<std::size_t>(info.consumer_ring), info.consumer_speculation.c_str());
            check(info.consumer_ring % 4096 == offset[k], name + ": both ends see the ring at one page offset");
            check(same == (k == 0 || k == 3 || k == 4),
                  name + (same ? ": both ends at one address" : ": the ends at different addresses"));
            check(!info.producer_speculation.empty() && !info.consumer_speculation.empty(),
                  name + ": speculation status recorded for both ends");
        }
    }
    check(offset[0] == offset[1] && offset[1] == offset[2] && offset[2] == offset[3],
          "S1 cells A-D: the ring at one page offset (" + std::to_string(offset[0]) + ")");
    check(offset[4] != offset[0], "the reference's ring at another page offset (" + std::to_string(offset[4]) + ")");

    std::printf("%s\n", failures ? "FAILED" : "all passed");
    return failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
