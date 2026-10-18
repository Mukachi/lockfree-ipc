// Open-loop load generator. Design: docs/design/003-open-loop-load.md
#pragma once

#include <lfipc/bench/clock.hpp>
#include <lfipc/bench/cross_cpu.hpp>
#include <lfipc/bench/histogram.hpp>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string>
#include <thread>
#include <vector>

namespace lfipc::bench {

// What the generator sends. `intended` is the scheduled send time, which
// latency is measured from (S1).
struct message {
    std::uint64_t seq;
    ticks intended;
};

// A system under test: send() may refuse (full) and is retried; try_receive()
// returns false when nothing is there.
template <typename S>
concept load_sut = requires(S s, const message& in, message& out) {
    { s.send(in) } -> std::same_as<bool>;
    { s.try_receive(out) } -> std::same_as<bool>;
};

struct load_config {
    double rate_per_s = 1e6;       // offered rate
    std::size_t messages = 1'000'000;  // measured, after warm-up
    int producer_cpu = 2;
    int consumer_cpu = 3;
    double ticks_per_ns = 0.0;     // from the run's calibration
};

struct load_result {
    bool ok = false;
    std::string reason;
    double rate_per_s = 0.0;
    double interval_ticks = 0.0;   // T
    std::size_t messages = 0;
    std::size_t warmup_discarded = 0;
    int producer_cpu = -1, consumer_cpu = -1;
    std::uint64_t out_of_order = 0;   // received sequence numbers that were not the next expected
    std::uint64_t negative = 0;       // receive stamp before intended time; recorded as 0
    histogram<> latency;   // receive stamp - intended send time
    histogram<> lateness;  // producer's send - intended send time
};

// Warm-up: 10% of the measured count, and at least 100 ms of messages.
inline std::size_t load_warmup(double rate_per_s, std::size_t messages) {
    return std::max(messages / 10, static_cast<std::size_t>(std::ceil(0.1 * rate_per_s)));
}

// Runs one rate. The producer sends message i at t0 + i*T, spinning until then,
// and never skips a late one (S2); the consumer records stamp - intended (S1).
// Histograms are constructed before the threads start, so their pages are mapped.
template <load_sut Sut>
void run_load(Sut& sut, const load_config& c, load_result& r) {
    r = {};
    r.rate_per_s = c.rate_per_s;
    r.interval_ticks = c.ticks_per_ns * 1e9 / c.rate_per_s;
    r.messages = c.messages;
    r.warmup_discarded = load_warmup(c.rate_per_s, c.messages);
    r.producer_cpu = c.producer_cpu;
    r.consumer_cpu = c.consumer_cpu;
    if (c.ticks_per_ns <= 0.0 || c.rate_per_s <= 0.0 || c.messages == 0) {
        r.reason = "invalid load configuration";
        return;
    }
    const std::size_t warm = r.warmup_discarded;
    const std::size_t n = warm + c.messages;

    // Offsets from t0, computed before the run into a pre-faulted array.
    std::vector<ticks> offset(n);
    for (std::size_t i = 0; i < n; ++i) offset[i] = static_cast<ticks>(std::llround(static_cast<double>(i) * r.interval_ticks));

    std::atomic<int> ready{0}, failed{0};
    std::atomic<ticks> t0{0};  // 0 until go; then the schedule's start
    std::atomic<bool> abort{false};

    std::thread consumer([&] {
        if (!detail::pin_to(c.consumer_cpu)) failed.fetch_add(1);
        ready.fetch_add(1, std::memory_order_release);
        while (t0.load(std::memory_order_acquire) == 0) detail::cpu_relax();
        if (abort.load()) return;
        message m{};
        for (std::uint64_t expected = 0; expected < n;) {
            if (!sut.try_receive(m)) {
                detail::cpu_relax();
                continue;
            }
            const ticks now = stop();
            if (m.seq != expected) ++r.out_of_order;
            if (m.seq >= warm) {
                if (now < m.intended) {
                    ++r.negative;
                    r.latency.record(0);
                } else {
                    r.latency.record(now - m.intended);
                }
            }
            ++expected;
        }
    });
    std::thread producer([&] {
        if (!detail::pin_to(c.producer_cpu)) failed.fetch_add(1);
        ready.fetch_add(1, std::memory_order_release);
        ticks base;
        while ((base = t0.load(std::memory_order_acquire)) == 0) detail::cpu_relax();
        if (abort.load()) return;
        for (std::size_t i = 0; i < n; ++i) {
            const ticks due = base + offset[i];
            while (start() < due) detail::cpu_relax();
            while (!sut.send(message{i, due})) detail::cpu_relax();
            const ticks sent = start();
            if (i >= warm) r.lateness.record(sent - due);
        }
    });

    while (ready.load(std::memory_order_acquire) < 2) detail::cpu_relax();
    if (failed.load()) abort.store(true);
    // Start 1 ms from now, so both threads are spinning on the schedule first.
    t0.store(start() + static_cast<ticks>(c.ticks_per_ns * 1e6), std::memory_order_release);
    producer.join();
    consumer.join();

    if (failed.load()) {
        r.reason = "cannot pin producer CPU " + std::to_string(c.producer_cpu) + " or consumer CPU " +
                   std::to_string(c.consumer_cpu);
        return;
    }
    r.ok = true;
}

}  // namespace lfipc::bench
