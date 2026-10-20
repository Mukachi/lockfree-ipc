// Open-loop load from several producers to several consumers.
// Design: docs/design/010-mpmc-queue.md (Generator)
#pragma once

#include <lfipc/bench/load.hpp>

#include <atomic>
#include <cmath>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace lfipc::bench {

// A message's producer index sits in the top 8 bits of its sequence number;
// the rest counts that producer's messages. The message stays 16 bytes.
inline constexpr int producer_shift = 56;
inline constexpr std::uint64_t index_mask = (std::uint64_t{1} << producer_shift) - 1;
inline constexpr std::size_t max_producers = 256;

struct mp_load_config {
    double rate_per_s = 1e6;           // aggregate, all producers together
    std::size_t messages = 1'000'000;  // measured, all producers together
    std::vector<int> producer_cpus{2};
    std::vector<int> consumer_cpus{3};
    double ticks_per_ns = 0.0;
};

// load_result's fields, for the run as a whole: `latency` merges every
// consumer's, `lateness` every producer's; `interval_ticks` is one producer's
// send interval (P / rate); `producer_cpu` and `consumer_cpu` are the first of
// each list.
struct mp_load_result : load_result {
    std::vector<int> producer_cpus, consumer_cpus;
    std::uint64_t sent = 0, received = 0;  // warm-up included
};

namespace detail {

// One consumer's results, on lines of its own (note 008's false-sharing
// lesson): its histograms, its count, and the next index it expects from each
// producer.
struct alignas(128) mp_consumer {
    load_result r;
    std::uint64_t received = 0;
    std::vector<std::uint64_t> next;
    char pad[128];
};

// A consumer among several: records receive - intended for every message
// past its producer's warm-up, and counts any message that arrives before an
// earlier one from the same producer. It stops on an empty queue once every
// producer has finished: what is left then is final. `done` is read only on
// an empty poll, never on the timed path.
template <load_sut Sut>
[[gnu::always_inline]] inline void mp_consumer_loop(Sut& sut, mp_consumer& me, std::size_t warm,
                                                    const std::atomic<int>& done, int producers) {
    message m{};
    for (;;) {
        if (!sut.try_receive(m)) {
            if (done.load(std::memory_order_acquire) < producers) {
                cpu_relax();
                continue;
            }
            if (!sut.try_receive(m)) return;
        }
        const ticks now = stop();
        ++me.received;
        const std::uint64_t p = m.seq >> producer_shift, i = m.seq & index_mask;
        if (p >= me.next.size() || i < me.next[p]) {
            ++me.r.out_of_order;
        } else {
            me.next[p] = i + 1;
        }
        if (i >= warm) {
            if (now < m.intended) {
                ++me.r.negative;
                me.r.latency.record(0);
            } else {
                me.r.latency.record(now - m.intended);
            }
        }
    }
}

}  // namespace detail

// Producers and consumers as pinned threads. Producer k sends at rate / P,
// offset k / rate from the shared start, so the combined arrivals are evenly
// spaced at `rate`; it runs note 003's producer loop, tagging its messages.
// Each thread records into its own results, merged after the run. The run is
// refused if any thread could not pin, if any producer's lateness p99 exceeds
// its interval, or if fewer messages arrived than were sent. One-way only.
template <load_sut Sut>
void run_load_mp(Sut& sut, const mp_load_config& c, mp_load_result& r) {
    r.producer_cpus = c.producer_cpus;
    r.consumer_cpus = c.consumer_cpus;
    const auto P = static_cast<int>(c.producer_cpus.size()), C = static_cast<int>(c.consumer_cpus.size());
    if (P < 1 || C < 1 || c.producer_cpus.size() > max_producers || c.messages < c.producer_cpus.size() ||
        c.ticks_per_ns <= 0.0 || c.rate_per_s <= 0.0) {
        r.ok = false;
        r.reason = "invalid load configuration";
        return;
    }
    const auto Pz = static_cast<std::size_t>(P);
    const std::size_t per = c.messages / Pz;  // measured, per producer
    const std::size_t warm = (load_warmup(c.rate_per_s, c.messages) + Pz - 1) / Pz;
    const std::size_t n = warm + per;
    r.ok = false;
    r.reason.clear();
    r.has_round_trip = false;
    r.rate_per_s = c.rate_per_s;
    r.interval_ticks = c.ticks_per_ns * 1e9 * P / c.rate_per_s;
    r.messages = per * Pz;
    r.warmup_discarded = warm * Pz;
    r.producer_cpu = c.producer_cpus.front();
    r.consumer_cpu = c.consumer_cpus.front();
    r.out_of_order = r.negative = 0;
    r.latency = {};
    r.lateness = {};
    r.round_trip = {};
    r.sent = n * Pz;
    r.received = 0;
    const double phase = c.ticks_per_ns * 1e9 / c.rate_per_s;  // ticks between producers' starts

    // Every thread's results are constructed here, before any thread starts,
    // so their pages are mapped (and placed by the run's memory policy).
    std::vector<std::unique_ptr<detail::consumer_results>> prod;
    std::vector<std::unique_ptr<detail::mp_consumer>> cons;
    for (int k = 0; k < P; ++k) prod.push_back(std::make_unique<detail::consumer_results>());
    for (int k = 0; k < C; ++k) {
        cons.push_back(std::make_unique<detail::mp_consumer>());
        cons.back()->next.assign(Pz, 0);
    }
    std::vector<std::vector<ticks>> offsets(Pz);
    std::vector<ticks> phase_of(Pz);  // producer k starts k / rate after t0; computed here, not in its thread
    for (std::size_t k = 0; k < Pz; ++k) phase_of[k] = static_cast<ticks>(std::llround(static_cast<double>(k) * phase));
    detail::load_sync sync;
    alignas(128) std::atomic<int> done{0};
    detail::no_return none;

    std::vector<std::thread> threads;
    for (int k = 0; k < C; ++k)
        threads.emplace_back([&, k] {
            if (!detail::pin_to(c.consumer_cpus[static_cast<std::size_t>(k)])) sync.failed.fetch_add(1);
            sync.ready.fetch_add(1, std::memory_order_release);
            while (sync.t0.load(std::memory_order_acquire) == 0) detail::cpu_relax();
            if (sync.abort.load()) return;
            detail::mp_consumer_loop(sut, *cons[static_cast<std::size_t>(k)], warm, done, P);
        });
    for (int k = 0; k < P; ++k)
        threads.emplace_back([&, k] {
            const auto kz = static_cast<std::size_t>(k);
            if (!detail::pin_to(c.producer_cpus[kz])) sync.failed.fetch_add(1);
            detail::fill_schedule(offsets[kz], n, r.interval_ticks);
            sync.ready.fetch_add(1, std::memory_order_release);
            ticks t0;
            while ((t0 = sync.t0.load(std::memory_order_acquire)) == 0) detail::cpu_relax();
            if (sync.abort.load()) return;
            detail::producer_loop<false>(sut, none, prod[kz]->r, sync, offsets[kz], t0 + phase_of[kz], warm, n,
                                         static_cast<std::uint64_t>(k) << producer_shift);
            done.fetch_add(1, std::memory_order_release);
        });
    detail::start_schedule(sync, c.ticks_per_ns, P + C);
    for (auto& t : threads) t.join();

    std::string late;
    for (int k = 0; k < P; ++k) {
        const auto& h = prod[static_cast<std::size_t>(k)]->r.lateness;
        const auto lat = evaluate_lateness(h, r.interval_ticks, c.ticks_per_ns);
        if (!lat.ok && late.empty()) late = lat.reason + " (producer " + std::to_string(k) + ")";
        r.lateness.merge(h);
    }
    for (const auto& m : cons) {
        r.latency.merge(m->r.latency);
        r.out_of_order += m->r.out_of_order;
        r.negative += m->r.negative;
        r.received += m->received;
    }
    if (sync.failed.load()) {
        r.reason = "cannot pin every producer and consumer CPU";
    } else if (r.received != r.sent) {
        r.reason = "received " + std::to_string(r.received) + " of " + std::to_string(r.sent) + " messages sent";
    } else if (!late.empty()) {
        r.reason = late;
    } else {
        r.ok = true;
    }
}

}  // namespace lfipc::bench
