// Tests for include/lfipc/bench/load.hpp. Spec: docs/design/003-open-loop-load.md
#include "test_suts.hpp"

#include <lfipc/bench/calibrate.hpp>
#include <lfipc/bench/load.hpp>

#include <cstdio>
#include <cstdlib>
#include <memory>

namespace {

int failures = 0;

void check(bool cond, const char* what) {
    std::printf("%s  %s\n", cond ? "pass" : "FAIL", what);
    if (!cond) ++failures;
}

using namespace lfipc::bench;
namespace t = lfipc::test;

// The run finished and every message was handled. Refusal for lateness is a
// verdict on the machine, not on correctness, so it counts as finished here.
bool finished(const load_result& r) {
    if (!r.ok && r.reason.rfind("producer lateness", 0) == 0) std::printf("info  refused: %s\n", r.reason.c_str());
    return r.ok || r.reason.rfind("producer lateness", 0) == 0;
}

void report(const char* name, const load_result& r, double tpn) {
    const auto ns = [tpn](std::optional<ticks> v) { return v ? static_cast<double>(*v) / tpn : -1.0; };
    std::printf("info  %-10s %zu msgs at %.0f/s: latency p50 %.0f p99 %.0f p99.9 %.0f max %.0f ns; "
                "lateness p99 %.0f ns\n",
                name, r.messages, r.rate_per_s, ns(r.latency.percentile(50)), ns(r.latency.percentile(99)),
                ns(r.latency.percentile(99.9)), static_cast<double>(r.latency.max()) / tpn,
                ns(r.lateness.percentile(99)));
}

}  // namespace

int main() {
    const auto cal = calibrate(3, 50);
    const double tpn = cal.ticks_per_ns;
    load_config c;
    c.ticks_per_ns = tpn;
    auto r = std::make_unique<load_result>();  // two 128 KB histograms: keep off the stack

    check(load_warmup(1e6, 1'000'000) == 100'000 && load_warmup(1e6, 10'000'000) == 1'000'000 &&
              load_warmup(1e4, 1'000) == 1'000,
          "warm-up is 10% and at least 100 ms of messages");

    // Plain mailbox: everything arrives once, in order, and is counted.
    {
        t::plain sut;
        c.rate_per_s = 1e6;
        c.messages = 200'000;
        run_load(sut, c, *r);
        report("plain", *r, tpn);
        check(finished(*r), "plain run completes");
        check(r->out_of_order == 0 && r->negative == 0, "every message once, in order; no negative latency");
        check(r->latency.count() == c.messages && r->lateness.count() == c.messages,
              "measured count excludes warm-up, on both histograms");
    }

    // S5: a fixed 5 us delay is a floor under every latency.
    {
        const auto delay = static_cast<ticks>(5'000 * tpn);
        t::fixed_delay sut(delay);
        c.rate_per_s = 1e5;
        c.messages = 50'000;
        run_load(sut, c, *r);
        report("delay 5us", *r, tpn);
        check(finished(*r) && r->latency.min() >= delay, "S5 every latency is at least the injected delay");
    }

    // S1 and S2: the producer is held up 1 ms on one message. Measured from the
    // intended time, the messages due during the hold show it; none is skipped.
    {
        c.rate_per_s = 1e6;
        c.messages = 200'000;
        const auto hold = static_cast<ticks>(1e6 * tpn);
        t::send_stall sut(load_warmup(c.rate_per_s, c.messages) + 50'000, hold);
        run_load(sut, c, *r);
        report("send stall", *r, tpn);
        check(finished(*r) && r->latency.max() >= hold * 9 / 10, "S1 latency counts from the intended send time");
        check(r->lateness.max() >= hold * 9 / 10, "producer lateness shows the hold");
        check(r->out_of_order == 0 && r->latency.count() == c.messages, "S2 no message skipped");
    }

    // Round trip: every message comes back; a fixed delay each way floors every
    // round trip at twice the delay; the one-way run alongside is unaffected in
    // what it counts.
    {
        t::plain out, back;
        c.rate_per_s = 1e6;
        c.messages = 200'000;
        run_load(out, back, c, *r);
        report("rt plain", *r, tpn);
        const auto rt50 = r->round_trip.percentile(50), ow50 = r->latency.percentile(50);
        std::printf("info  rt plain   round trip p50 %.0f ns, one-way p50 %.0f ns\n", rt50 ? *rt50 / tpn : -1.0,
                    ow50 ? *ow50 / tpn : -1.0);
        check(finished(*r) && r->has_round_trip, "round-trip run completes");
        check(r->round_trip.count() == c.messages && r->latency.count() == c.messages && r->out_of_order == 0,
              "every message measured both ways, in order");
        // Not strictly above: when the run is far behind (under TSan, ~200 ms),
        // both land in the same 0.2%-wide bucket.
        check(rt50 && ow50 && *rt50 >= *ow50, "round trip p50 not below one-way p50");
    }
    {
        const auto delay = static_cast<ticks>(5'000 * tpn);
        t::fixed_delay out(delay), back(delay);
        c.rate_per_s = 1e5;
        c.messages = 50'000;
        run_load(out, back, c, *r);
        report("rt delay", *r, tpn);
        check(finished(*r) && r->round_trip.min() >= 2 * delay && r->latency.min() >= delay,
              "S5 round trip at least twice the delay, one-way at least once");
    }

    // S4: the receiver stalls 1 ms every 100 ms. Open-loop counts every message
    // due during a stall; closed-loop sends nothing during it and hides it.
    {
        c.rate_per_s = 1e6;
        c.messages = 300'000;  // 300 ms after warm-up: about three stalls
        const auto every = static_cast<ticks>(100e6 * tpn), length = static_cast<ticks>(1e6 * tpn);
        t::stalling open_sut(every, length);
        run_load(open_sut, c, *r);
        report("stall open", *r, tpn);
        const auto open999 = r->latency.percentile(99.9);
        t::closed_loop<t::stalling> closed_sut(every, length);
        run_load(closed_sut, c, *r);
        report("stall closed", *r, tpn);
        const auto closed999 = r->latency.percentile(99.9);
        const auto half_ms = static_cast<ticks>(0.5e6 * tpn);
        check(open999 && *open999 >= half_ms, "S4 open-loop p99.9 shows the 1 ms stall (>= 0.5 ms)");
        check(closed999 && *closed999 < half_ms, "S4 closed-loop p99.9 hides it (< 0.5 ms)");
    }

    // S3: the refusal decision, on synthetic lateness. T = 1,000 ticks.
    {
        histogram<> ok_h, late_h, few_h;
        for (int i = 0; i < 10'000; ++i) ok_h.record(i < 50 ? 5'000 : 100);    // 0.5% late by > T
        for (int i = 0; i < 10'000; ++i) late_h.record(i < 200 ? 5'000 : 100);  // 2% late by > T
        for (int i = 0; i < 500; ++i) few_h.record(100);
        check(evaluate_lateness(ok_h, 1'000, 2.0).ok, "S3 0.5% late by more than T: accepted");
        const auto late = evaluate_lateness(late_h, 1'000, 2.0);
        check(!late.ok && late.reason.find("exceeds the send interval 500 ns") != std::string::npos,
              "S3 2% late by more than T: refused, with p99 and T in the reason");
        check(!evaluate_lateness(few_h, 1'000, 2.0).ok, "S3 too few messages to judge: refused");
    }

    // An unpinnable CPU refuses.
    {
        t::plain sut;
        c.producer_cpu = -1;
        c.messages = 1'000;
        run_load(sut, c, *r);
        check(!r->ok && !r->reason.empty(), "unpinnable CPU refuses with a reason");
        c.producer_cpu = 2;
    }

    std::printf("%s\n", failures ? "FAILED" : "all passed");
    return failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
