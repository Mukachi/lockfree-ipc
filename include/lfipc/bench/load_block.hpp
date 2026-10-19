// JSON block for a load sweep. Design: docs/design/003-open-loop-load.md
#pragma once

#include <lfipc/bench/cross_cpu.hpp>
#include <lfipc/bench/histogram_block.hpp>
#include <lfipc/bench/json.hpp>
#include <lfipc/bench/load.hpp>

#include <memory>
#include <string_view>
#include <vector>

namespace lfipc::bench {

// Writes `"<key>": {...}` (default "load") into the object the caller has open. `pair` is the
// producer/consumer CPU pair measured with note 001's method: the one-way
// figures are published beside its offset bound (S7). A refused rate keeps its
// reason and the lateness histogram that refused it, and no latency results.
//
// One mode per block (note 003, amended): runs without echo publish one-way
// latency only; runs with echo publish round trip only. An echo run's one-way
// figure is not written: the echo traffic inflates it (note 006, Review).
inline void write_load_block(json_writer& w, std::string_view sut, const cross_cpu_pair& pair,
                             const std::vector<std::unique_ptr<load_result>>& runs, double ticks_per_ns,
                             std::string_view key = "load") {
    const auto ns = [ticks_per_ns](double t) { return t / ticks_per_ns; };
    const bool round_trip = !runs.empty() && runs.front()->has_round_trip;
    w.key(key);
    w.begin_object();
    w.member("mode", round_trip ? "round_trip" : "one_way");
    w.member("sut", sut);
    w.member("producer_cpu", pair.a);
    w.member("consumer_cpu", pair.b);
    w.key("one_way_offset_bound");
    w.begin_object();
    w.member("ok", pair.ok && !pair.disagreement_proven());
    w.member("theta_lo_ns", ns(static_cast<double>(pair.theta_lo())));
    w.member("theta_hi_ns", ns(static_cast<double>(pair.theta_hi())));
    w.member("round_trips", pair.round_trips);
    w.member("converged", pair.converged);
    w.end_object();

    w.key("runs");
    w.begin_array();
    for (const auto& r : runs) {
        w.begin_object();
        w.member("rate_per_s", r->rate_per_s);
        w.member("interval_ns", ns(r->interval_ticks));
        w.member("messages", r->messages);
        w.member("warmup_discarded", r->warmup_discarded);
        w.member("ok", r->ok);
        if (!r->ok) w.member("reason", r->reason);
        write_histogram_block(w, r->lateness, ticks_per_ns, "lateness");
        if (r->ok) {
            w.member("out_of_order", r->out_of_order);
            w.member("negative", r->negative);
            if (r->has_round_trip) write_histogram_block(w, r->round_trip, ticks_per_ns, "round_trip");
            else write_histogram_block(w, r->latency, ticks_per_ns, "one_way");
        }
        w.end_object();
    }
    w.end_array();
    w.end_object();
}

}  // namespace lfipc::bench
