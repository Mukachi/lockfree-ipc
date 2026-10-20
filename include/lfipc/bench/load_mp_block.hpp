// JSON block for a sweep with several producers and consumers.
// Design: docs/design/010-mpmc-queue.md
#pragma once

#include <lfipc/bench/cross_cpu.hpp>
#include <lfipc/bench/histogram_block.hpp>
#include <lfipc/bench/json.hpp>
#include <lfipc/bench/load_mp.hpp>

#include <memory>
#include <string_view>
#include <vector>

namespace lfipc::bench {

// Writes `"one_way": {...}` into the object the caller has open: note 003's
// load block (one mode, the same run fields), with the producer and consumer
// CPU lists, every producer → consumer pair's offset bound (one-way crosses
// each of them), and the messages sent and received. `producer_cpu` is the
// first producer, whose node the queue must be on (note 008, Review).
inline void write_mp_load_block(json_writer& w, std::string_view sut, const std::vector<cross_cpu_pair>& bounds,
                                const std::vector<std::unique_ptr<mp_load_result>>& runs, double ticks_per_ns) {
    const auto ns = [ticks_per_ns](double t) { return t / ticks_per_ns; };
    const mp_load_result& first = *runs.front();
    w.key("one_way");
    w.begin_object();
    w.member("mode", "one_way");
    w.member("sut", sut);
    w.member("producer_cpu", first.producer_cpus.front());
    w.key("producer_cpus");
    w.begin_array();
    for (int c : first.producer_cpus) w.value(c);
    w.end_array();
    w.key("consumer_cpus");
    w.begin_array();
    for (int c : first.consumer_cpus) w.value(c);
    w.end_array();
    w.key("one_way_offset_bounds");
    w.begin_array();
    for (const auto& b : bounds) {
        w.begin_object();
        w.member("producer_cpu", b.a);
        w.member("consumer_cpu", b.b);
        w.member("ok", b.ok && !b.disagreement_proven());
        w.member("theta_lo_ns", ns(static_cast<double>(b.theta_lo())));
        w.member("theta_hi_ns", ns(static_cast<double>(b.theta_hi())));
        w.member("round_trips", b.round_trips);
        w.member("converged", b.converged);
        w.end_object();
    }
    w.end_array();

    w.key("runs");
    w.begin_array();
    for (const auto& r : runs) {
        w.begin_object();
        w.member("rate_per_s", r->rate_per_s);
        w.member("interval_ns", ns(r->interval_ticks));  // one producer's
        w.member("messages", r->messages);
        w.member("warmup_discarded", r->warmup_discarded);
        w.member("sent", r->sent);
        w.member("received", r->received);
        w.member("memory_node", r->memory_node);
        w.member("ok", r->ok);
        if (!r->ok) w.member("reason", r->reason);
        write_histogram_block(w, r->lateness, ticks_per_ns, "lateness");
        if (r->ok) {
            w.member("out_of_order", r->out_of_order);
            w.member("negative", r->negative);
            write_histogram_block(w, r->latency, ticks_per_ns, "one_way");
        }
        w.end_object();
    }
    w.end_array();
    w.end_object();
}

}  // namespace lfipc::bench
