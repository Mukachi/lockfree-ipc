// Tests for include/lfipc/bench/load_mp.hpp. Spec: docs/design/010-mpmc-queue.md (S5)
#include "test_machine.hpp"

#include <lfipc/bench/calibrate.hpp>
#include <lfipc/bench/load.hpp>
#include <lfipc/bench/load_mp.hpp>
#include <lfipc/mpmc.hpp>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <vector>

namespace {

int failures = 0;

void check(bool cond, const std::string& what) {
    std::printf("%s  %s\n", cond ? "pass" : "FAIL", what.c_str());
    if (!cond) ++failures;
}

using namespace lfipc::bench;
namespace t = lfipc::test;

// The MPMC ring as a SUT, with hooks a test can set: one producer held up
// once, one producer slowed on every send, one message dropped.
struct hooked {
    lfipc::mpmc_v1<message, 1024> q;
    std::atomic<std::uint64_t> stall_seq{~std::uint64_t{0}};  // held up once, on this sequence number (read by every producer)
    ticks stall = 0;
    std::uint64_t slow_producer = ~std::uint64_t{0};  // every send by this producer waits `slow`
    ticks slow = 0;
    std::uint64_t drop_seq = ~std::uint64_t{0};  // accepted and not delivered
    bool send(const message& m) {
        if (m.seq == stall_seq.load(std::memory_order_relaxed)) {
            const ticks until = start() + stall;
            while (start() < until) {
            }
            stall_seq.store(~std::uint64_t{0}, std::memory_order_relaxed);  // only producer 0 sends it
        }
        if ((m.seq >> producer_shift) == slow_producer) {
            const ticks until = start() + slow;
            while (start() < until) {
            }
        }
        if (m.seq == drop_seq) return true;
        return q.try_push(m);
    }
    bool try_receive(message& m) { return q.try_pop(m); }
};

bool finished(const mp_load_result& r) {
    if (!r.ok) std::printf("info  refused: %s\n", r.reason.c_str());
    return r.ok || r.reason.rfind("producer lateness", 0) == 0;
}

std::string shape(const mp_load_config& c) {
    std::string s;
    for (int p : c.producer_cpus) s += (s.empty() ? "" : ",") + std::to_string(p);
    s += " -> ";
    bool first = true;
    for (int p : c.consumer_cpus) {
        s += (first ? "" : ",") + std::to_string(p);
        first = false;
    }
    return s;
}

// The first P + C CPUs this machine offers, or nothing (reported as a skip).
bool pick(mp_load_config& c, std::size_t P, std::size_t C, const std::string& what) {
    const auto cpus = t::pick_cpus(P + C);
    if (!cpus) {
        t::skip(what, "needs " + std::to_string(P + C) + " CPUs to pin");
        return false;
    }
    c.producer_cpus.assign(cpus->begin(), cpus->begin() + static_cast<std::ptrdiff_t>(P));
    c.consumer_cpus.assign(cpus->begin() + static_cast<std::ptrdiff_t>(P), cpus->end());
    return true;
}

}  // namespace

int main() {
    const double tpn = calibrate(3, 50).ticks_per_ns;
    auto r = std::make_unique<mp_load_result>();
    mp_load_config c;
    c.ticks_per_ns = tpn;

    // Every shape the bench measures: all messages arrive once, each
    // producer's in order, counted on both sides.
    for (const auto& [P, C] : {std::pair<std::size_t, std::size_t>{2, 1}, {4, 1}, {2, 2}, {4, 4}}) {
        const std::string name = std::to_string(P) + " -> " + std::to_string(C);
        if (!pick(c, P, C, name + ": run completes")) continue;
        auto sut = std::make_unique<hooked>();
        c.rate_per_s = 1e6;
        c.messages = 200'000;
        run_load_mp(*sut, c, *r);
        std::printf("info  %s on CPUs %s: p50 %.0f ns, p99 %.0f ns\n", name.c_str(), shape(c).c_str(),
                    r->latency.percentile(50).value_or(0) / tpn, r->latency.percentile(99).value_or(0) / tpn);
        check(finished(*r) && r->received == r->sent && r->out_of_order == 0 && r->negative == 0 &&
                  r->latency.count() == c.messages && r->lateness.count() == c.messages,
              name + ": every message once, per-producer order, measured counts exclude warm-up");
    }

    if (pick(c, 2, 1, "held-up producer, late producer, lost message")) {
        // S1 per producer: producer 0 held up 1 ms on one message. Measured
        // from the intended time, the messages due during the hold show it.
        {
            auto sut = std::make_unique<hooked>();
            c.rate_per_s = 1e6;
            c.messages = 200'000;
            sut->stall_seq = (load_warmup(c.rate_per_s, c.messages) + 1) / 2 + 50'000;  // producer 0, past warm-up
            sut->stall = static_cast<ticks>(1e6 * tpn);
            run_load_mp(*sut, c, *r);
            check(finished(*r) && r->latency.max() >= sut->stall * 9 / 10,
                  "S5 a held-up producer shows in latency from the intended time");
            check(r->lateness.max() >= sut->stall * 9 / 10 && r->received == r->sent && r->out_of_order == 0,
                  "S5 its lateness shows the hold; no message skipped");
        }
        // A producer slower than its interval refuses the run, naming it.
        {
            auto sut = std::make_unique<hooked>();
            c.rate_per_s = 1e5;  // each producer's interval 20 us
            c.messages = 2'000;
            sut->slow_producer = 1;
            sut->slow = static_cast<ticks>(40'000 * tpn);
            run_load_mp(*sut, c, *r);
            check(!r->ok && r->reason.rfind("producer lateness", 0) == 0 &&
                      r->reason.find("(producer 1)") != std::string::npos,
                  "S5 a late producer refuses the run, naming it: " + r->reason);
        }
        // A message accepted and never delivered refuses the run.
        {
            auto sut = std::make_unique<hooked>();
            c.rate_per_s = 1e6;
            c.messages = 20'000;
            sut->drop_seq = (std::uint64_t{1} << producer_shift) | 1'000;  // producer 1's 1,000th
            run_load_mp(*sut, c, *r);
            check(!r->ok && r->received + 1 == r->sent && r->reason.find("received") != std::string::npos,
                  "S5 a lost message refuses the run: " + r->reason);
        }
    }

    // 1 -> 1 through both generators on the same queue, reported (not
    // asserted here: the bench review compares them, note 010 S5).
    if (pick(c, 1, 1, "1 -> 1 against note 003's generator")) {
        auto a = std::make_unique<hooked>(), b = std::make_unique<hooked>();
        c.rate_per_s = 1e6;
        c.messages = 200'000;
        run_load_mp(*a, c, *r);
        const double mp50 = r->latency.percentile(50).value_or(0) / tpn;
        load_config lc;
        lc.ticks_per_ns = tpn;
        lc.rate_per_s = c.rate_per_s;
        lc.messages = c.messages;
        lc.producer_cpu = c.producer_cpus[0];
        lc.consumer_cpu = c.consumer_cpus[0];
        auto one = std::make_unique<load_result>();
        run_load(*b, lc, *one);
        std::printf("info  1 -> 1 at 1M/s, MPMC v1: p50 %.0f ns (this generator), %.0f ns (note 003's)\n", mp50,
                    one->latency.percentile(50).value_or(0) / tpn);
        check(finished(*r) && r->received == r->sent && r->out_of_order == 0, "1 -> 1 run completes");
    }

    // Refusals.
    {
        hooked sut;
        mp_load_config bad = c;
        bad.producer_cpus.clear();
        run_load_mp(sut, bad, *r);
        check(!r->ok && r->reason == "invalid load configuration", "no producer: invalid configuration");
        bad = c;
        bad.consumer_cpus = {-1};
        bad.messages = 1'000;
        run_load_mp(sut, bad, *r);
        check(!r->ok && r->reason.find("cannot pin") != std::string::npos,
              "an unpinnable consumer refuses, without hanging: " + r->reason);
    }

    std::printf("%s\n", failures ? "FAILED" : "all passed");
    return failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
