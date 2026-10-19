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
#include <cstdio>
#include <string>
#include <thread>
#include <type_traits>
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
    std::string reason;  // empty when ok; histograms are filled even on a lateness refusal
    double rate_per_s = 0.0;
    double interval_ticks = 0.0;   // T
    std::size_t messages = 0;
    std::size_t warmup_discarded = 0;
    int producer_cpu = -1, consumer_cpu = -1;
    std::uint64_t out_of_order = 0;   // received sequence numbers that were not the next expected
    std::uint64_t negative = 0;       // receive stamp before intended time; recorded as 0
    histogram<> latency;   // receive stamp - intended send time
    histogram<> lateness;  // producer's send - intended send time
    bool has_round_trip = false;
    histogram<> round_trip;  // echo's arrival back at the producer - intended send time
};

// The refusal decision alone, so it is testable (S3): the generator could not
// sustain the rate if its lateness p99 exceeds the send interval. p99 is a
// bucket's upper edge, so this errs towards refusing.
inline check_result evaluate_lateness(const histogram<>& lateness, double interval_ticks, double ticks_per_ns) {
    const auto p99 = lateness.percentile(99.0);
    if (!p99) return {false, "too few messages to judge producer lateness: p99 needs at least 1,000"};
    if (static_cast<double>(*p99) > interval_ticks) {
        char buf[160];
        std::snprintf(buf, sizeof(buf),
                      "producer lateness p99 %.0f ns exceeds the send interval %.0f ns: "
                      "the generator could not sustain the rate",
                      static_cast<double>(*p99) / ticks_per_ns, interval_ticks / ticks_per_ns);
        return {false, buf};
    }
    return {true, {}};
}

// Warm-up: 10% of the measured count, and at least 100 ms of messages.
inline std::size_t load_warmup(double rate_per_s, std::size_t messages) {
    return std::max(messages / 10, static_cast<std::size_t>(std::ceil(0.1 * rate_per_s)));
}

namespace detail {

// Offsets from t0 for every message, computed before the run into a
// pre-faulted array. Called by the producer after pinning, so the array's
// pages are first touched, and so placed, on the producer's NUMA node. Not
// inlined: it allocates, and note 003's S6 check allows exactly this call in
// the producer's thread body, outside its loop.
[[gnu::noinline]] inline void fill_schedule(std::vector<ticks>& offset, std::size_t n, double interval_ticks) {
    offset.resize(n);
    for (std::size_t i = 0; i < n; ++i)
        offset[i] = static_cast<ticks>(std::llround(static_cast<double>(i) * interval_ticks));
}

// Return path for a one-way run: nothing comes back.
struct no_return {
    bool send(const message&) { return true; }
    bool try_receive(message&) { return false; }
};

// Start-up and stop signals between producer and consumer. Lock-free atomics
// only, standard layout, so it can sit in memory shared between processes
// (note 008) as well as on a stack.
struct load_sync {
    std::atomic<int> ready{0}, failed{0};
    std::atomic<ticks> t0{0};  // 0 until go; then the schedule's start
    std::atomic<bool> abort{false};
};
static_assert(std::is_standard_layout_v<load_sync> && std::atomic<ticks>::is_always_lock_free);

// Fills r's description of the run; false if the configuration is invalid.
inline bool prepare_load(const load_config& c, load_result& r, bool echo) {
    r = {};
    r.has_round_trip = echo;
    r.rate_per_s = c.rate_per_s;
    r.interval_ticks = c.ticks_per_ns * 1e9 / c.rate_per_s;
    r.messages = c.messages;
    r.warmup_discarded = load_warmup(c.rate_per_s, c.messages);
    r.producer_cpu = c.producer_cpu;
    r.consumer_cpu = c.consumer_cpu;
    if (c.ticks_per_ns <= 0.0 || c.rate_per_s <= 0.0 || c.messages == 0) {
        r.reason = "invalid load configuration";
        return false;
    }
    return true;
}

// The consumer: records stamp - intended for every message (S1) and, with
// Echo, sends each one back. Inlined, so the thread version's compiled thread
// body (note 003, S6) is the loop itself, and the process version (note 008)
// runs the identical code.
template <bool Echo, load_sut Sut, load_sut Back>
[[gnu::always_inline]] inline void consumer_loop(Sut& sut, Back& back, load_result& r, std::size_t warm,
                                                 std::size_t n) {
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
        if constexpr (Echo) {
            while (!back.send(m)) detail::cpu_relax();
        }
        ++expected;
    }
}

// The producer sends message i at base + offset[i], spinning until then, and
// never skips a late one (S2). With Echo, while it waits it takes echoes and
// records arrival - intended: a round trip on one CPU's TSC. `abort` is read
// only when a send is refused or no echo is waiting, never on the timed path:
// it stops a producer whose consumer has gone (a process that died, note 008).
template <bool Echo, load_sut Sut, load_sut Back>
[[gnu::always_inline]] inline void producer_loop(Sut& sut, Back& back, load_result& r, load_sync& sync,
                                                 const std::vector<ticks>& offset, ticks base, std::size_t warm,
                                                 std::size_t n) {
    std::uint64_t echoes = 0;
    // Take one echo if there is one. Called while waiting, so the consumer
    // is never stuck sending back into a full return path.
    const auto poll = [&] {
        message e{};
        if (!back.try_receive(e)) return false;
        const ticks now = stop();
        if (e.seq >= warm) r.round_trip.record(now >= e.intended ? now - e.intended : 0);
        ++echoes;
        return true;
    };
    for (std::size_t i = 0; i < n; ++i) {
        const ticks due = base + offset[i];
        while (start() < due) {
            if (!Echo || !poll()) detail::cpu_relax();
        }
        while (!sut.send(message{i, due})) {
            if (!Echo || !poll()) {
                if (sync.abort.load(std::memory_order_relaxed)) return;
                detail::cpu_relax();
            }
        }
        const ticks sent = start();
        if (i >= warm) r.lateness.record(sent - due);
    }
    if constexpr (Echo) {
        while (echoes < n) {
            if (!poll()) {
                if (sync.abort.load(std::memory_order_relaxed)) return;
                detail::cpu_relax();
            }
        }
    }
}

// Waits for both sides, then starts the schedule 1 ms from now, so both are
// spinning on it first; or aborts if either could not pin.
inline void start_schedule(load_sync& sync, double ticks_per_ns) {
    while (sync.ready.load(std::memory_order_acquire) < 2) detail::cpu_relax();
    if (sync.failed.load()) sync.abort.store(true);
    sync.t0.store(start() + static_cast<ticks>(ticks_per_ns * 1e6), std::memory_order_release);
}

// The verdict once both sides are done.
inline void finish_load(const load_config& c, load_result& r, const load_sync& sync) {
    if (sync.failed.load()) {
        r.reason = "cannot pin producer CPU " + std::to_string(c.producer_cpu) + " or consumer CPU " +
                   std::to_string(c.consumer_cpu);
        return;
    }
    const auto lat = evaluate_lateness(r.lateness, r.interval_ticks, c.ticks_per_ns);
    if (!lat.ok) {
        r.reason = lat.reason;
        return;
    }
    r.ok = true;
}

// Producer and consumer as two pinned threads. Histograms are constructed
// before the threads start, so their pages are mapped.
template <bool Echo, load_sut Sut, load_sut Back>
void run_load_impl(Sut& sut, Back& back, const load_config& c, load_result& r) {
    if (!prepare_load(c, r, Echo)) return;
    const std::size_t warm = r.warmup_discarded;
    const std::size_t n = warm + c.messages;
    std::vector<ticks> offset;  // filled by the producer (fill_schedule) before it reports ready
    load_sync sync;

    std::thread consumer([&] {
        if (!detail::pin_to(c.consumer_cpu)) sync.failed.fetch_add(1);
        sync.ready.fetch_add(1, std::memory_order_release);
        while (sync.t0.load(std::memory_order_acquire) == 0) detail::cpu_relax();
        if (sync.abort.load()) return;
        consumer_loop<Echo>(sut, back, r, warm, n);
    });
    std::thread producer([&] {
        if (!detail::pin_to(c.producer_cpu)) sync.failed.fetch_add(1);
        detail::fill_schedule(offset, n, r.interval_ticks);
        sync.ready.fetch_add(1, std::memory_order_release);
        ticks base;
        while ((base = sync.t0.load(std::memory_order_acquire)) == 0) detail::cpu_relax();
        if (sync.abort.load()) return;
        producer_loop<Echo>(sut, back, r, sync, offset, base, warm, n);
    });

    start_schedule(sync, c.ticks_per_ns);
    producer.join();
    consumer.join();
    finish_load(c, r, sync);
}

}  // namespace detail

// One-way only.
template <load_sut Sut>
void run_load(Sut& sut, const load_config& c, load_result& r) {
    detail::no_return none;
    detail::run_load_impl<false>(sut, none, c, r);
}

// One-way and round trip in the same run (note 003, option C).
template <load_sut Sut, load_sut Back>
void run_load(Sut& sut, Back& back, const load_config& c, load_result& r) {
    detail::run_load_impl<true>(sut, back, c, r);
}

}  // namespace lfipc::bench
