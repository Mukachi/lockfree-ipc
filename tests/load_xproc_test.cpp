// Tests for include/lfipc/bench/load_xproc.hpp. Spec: docs/design/008-shm-transport.md
#include <lfipc/bench/calibrate.hpp>
#include <lfipc/bench/load_xproc.hpp>

#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <memory>
#include <string>
#include <thread>

namespace {

int failures = 0;

void check(bool cond, const std::string& what) {
    std::printf("%s  %s\n", cond ? "pass" : "FAIL", what.c_str());
    if (!cond) ++failures;
}

using namespace lfipc::bench;

// Off the fence, lateness refusals are the machine's verdict, not ours (note 003).
bool finished(const load_result& r) {
    if (!r.ok && r.reason.rfind("producer lateness", 0) == 0) std::printf("info  refused: %s\n", r.reason.c_str());
    return r.ok || r.reason.rfind("producer lateness", 0) == 0;
}

}  // namespace

int main() {
    const double tpn = calibrate(3, 50).ticks_per_ns;
    load_config c;
    c.ticks_per_ns = tpn;
    c.rate_per_s = 1e6;
    c.messages = 200'000;
    auto r = std::make_unique<load_result>();

    run_load_xproc<false>(c, *r);
    std::printf("info  one-way across processes: p50 %.0f ns, p99 %.0f ns\n",
                r->latency.percentile(50).value_or(0) / tpn, r->latency.percentile(99).value_or(0) / tpn);
    check(finished(*r) && !r->has_round_trip, "one-way run completes across two processes");
    check(r->latency.count() == c.messages && r->lateness.count() == c.messages && r->out_of_order == 0 &&
              r->negative == 0,
          "every message measured in the child and returned to the parent, in order");

    run_load_xproc<true>(c, *r);
    std::printf("info  round trip across processes: p50 %.0f ns\n", r->round_trip.percentile(50).value_or(0) / tpn);
    check(finished(*r) && r->has_round_trip && r->round_trip.count() == c.messages && r->out_of_order == 0,
          "round-trip run completes, every echo back in the parent");

    // A consumer killed mid-run: the parent must stop promptly, not hang. The
    // child is found through /proc (its pid is not exposed) and killed 0.3 s
    // into what would be a 10 s run.
    {
        c.rate_per_s = 1e5;
        c.messages = 1'000'000;
        std::thread killer([] {
            std::this_thread::sleep_for(std::chrono::milliseconds(300));
            std::ifstream kids("/proc/self/task/" + std::to_string(::getpid()) + "/children");
            for (pid_t k; kids >> k;) ::kill(k, SIGKILL);
        });
        const auto t0 = std::chrono::steady_clock::now();
        run_load_xproc<false>(c, *r);
        const auto took = std::chrono::steady_clock::now() - t0;
        killer.join();
        check(!r->ok && r->reason.find("ended early") != std::string::npos && took < std::chrono::seconds(3),
              "a consumer killed mid-run ends the run promptly with the reason: " + r->reason);
        c.rate_per_s = 1e6;
    }

    c.consumer_cpu = -1;
    c.messages = 10'000;
    run_load_xproc<false>(c, *r);
    check(!r->ok && r->reason.find("cannot pin") != std::string::npos,
          "an unpinnable consumer refuses, without hanging: " + r->reason);

    std::printf("%s\n", failures ? "FAILED" : "all passed");
    return failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
