// Cross-CPU TSC agreement. Design: docs/design/001-clock.md
#pragma once

#include <lfipc/bench/clock.hpp>

#include <algorithm>
#include <atomic>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <limits>
#include <sched.h>
#include <string>
#include <thread>
#include <vector>

namespace lfipc::bench {

inline constexpr std::size_t default_round_trips = 10'000;
inline constexpr std::size_t default_cross_cpu_warmup = 1'000;

struct cross_cpu_pair {
    int a = -1, b = -1;
    std::size_t round_trips = 0;
    std::size_t warmup_discarded = 0;
    // d1 = min(tB_recv - tA_send), d2 = min(tA_recv - tB_send), in ticks.
    // B's offset from A lies in [-d2, d1].
    std::int64_t d1 = 0, d2 = 0;
    // Minimums over the first and second halves, for the convergence check.
    std::int64_t d1_half[2] = {0, 0}, d2_half[2] = {0, 0};
    bool converged = false;
    bool ok = false;
    std::string reason;  // empty when ok

    std::int64_t theta_lo() const noexcept { return -d2; }
    std::int64_t theta_hi() const noexcept { return d1; }
    // A negative bound excludes zero offset: the two TSCs disagree.
    bool disagreement_proven() const noexcept { return d1 < 0 || d2 < 0; }
};

// The bounds alone, from recorded round trips, so the arithmetic is testable.
// The first `warmup` round trips are discarded. Converged when neither bound's
// second-half minimum beats its first-half minimum by more than one tick.
inline cross_cpu_pair summarise_cross_cpu(const std::vector<ticks>& a_send,
                                          const std::vector<ticks>& b_recv,
                                          const std::vector<ticks>& b_send,
                                          const std::vector<ticks>& a_recv, std::size_t warmup) {
    cross_cpu_pair p;
    const std::size_t n = a_send.size();
    if (b_recv.size() != n || b_send.size() != n || a_recv.size() != n) {
        p.reason = "round-trip arrays differ in length";
        return p;
    }
    if (n < warmup + 2) {
        p.reason = "fewer than two round trips after warm-up";
        return p;
    }
    p.warmup_discarded = warmup;
    p.round_trips = n - warmup;
    const std::size_t mid = warmup + p.round_trips / 2;
    constexpr auto inf = std::numeric_limits<std::int64_t>::max();
    std::int64_t d1h[2] = {inf, inf}, d2h[2] = {inf, inf};
    for (std::size_t i = warmup; i < n; ++i) {
        const int h = i < mid ? 0 : 1;
        // Unsigned difference, then signed: correct for offsets far below 2^63.
        d1h[h] = std::min(d1h[h], static_cast<std::int64_t>(b_recv[i] - a_send[i]));
        d2h[h] = std::min(d2h[h], static_cast<std::int64_t>(a_recv[i] - b_send[i]));
    }
    std::copy(d1h, d1h + 2, p.d1_half);
    std::copy(d2h, d2h + 2, p.d2_half);
    p.d1 = std::min(d1h[0], d1h[1]);
    p.d2 = std::min(d2h[0], d2h[1]);
    p.converged = d1h[0] - d1h[1] <= 1 && d2h[0] - d2h[1] <= 1;
    p.ok = true;
    return p;
}

namespace detail {

inline bool pin_to(int cpu) noexcept {
    if (cpu < 0 || cpu >= CPU_SETSIZE) return false;
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(cpu, &set);
    return sched_setaffinity(0, sizeof(set), &set) == 0;
}

[[gnu::always_inline]] inline void cpu_relax() noexcept {
#if LFIPC_CLOCK_TSC
    _mm_pause();
#endif
}

// Receive-side read (S8): the lfence waits for the preceding load to complete,
// so the read cannot move before the load that saw the other CPU's write.
[[gnu::always_inline]] inline ticks after_load() noexcept {
#if LFIPC_CLOCK_TSC
    _mm_lfence();
    return __rdtsc();
#else
    return fallback_now();
#endif
}

// One flag per cache line, so each hand-off moves exactly one line.
struct alignas(64) flag_line {
    std::atomic<std::uint64_t> v{0};
};

inline constexpr std::uint64_t stop_signal = std::numeric_limits<std::uint64_t>::max();

}  // namespace detail

// Ping-pong between CPU a and CPU b, each on its own pinned thread. The
// caller's affinity is not changed. Send side: start() before the store (S8).
// Timestamps are kept in registers until the hand-off is done, so the stores
// to the arrays stay off the measured path.
inline cross_cpu_pair measure_pair(int a, int b, std::size_t round_trips = default_round_trips,
                                   std::size_t warmup = default_cross_cpu_warmup) {
    const std::size_t n = warmup + round_trips;
    std::vector<ticks> a_send(n), a_recv(n), b_recv(n), b_send(n);
    detail::flag_line ping, pong;  // ping written by A, pong by B
    std::atomic<int> b_state{0};   // 0 starting, 1 ready, -1 could not pin
    bool a_pinned = false;

    std::thread tb([&] {
        if (!detail::pin_to(b)) {
            b_state.store(-1, std::memory_order_release);
            return;
        }
        b_state.store(1, std::memory_order_release);
        for (std::size_t i = 0; i < n; ++i) {
            std::uint64_t v;
            while ((v = ping.v.load(std::memory_order_acquire)) != i + 1) {
                if (v == detail::stop_signal) return;
                detail::cpu_relax();
            }
            const ticks recv = detail::after_load();
            const ticks send = start();
            pong.v.store(i + 1, std::memory_order_release);
            b_recv[i] = recv;
            b_send[i] = send;
        }
    });
    std::thread ta([&] {
        a_pinned = detail::pin_to(a);
        int s;
        while ((s = b_state.load(std::memory_order_acquire)) == 0) detail::cpu_relax();
        if (!a_pinned || s < 0) {
            ping.v.store(detail::stop_signal, std::memory_order_release);
            return;
        }
        for (std::size_t i = 0; i < n; ++i) {
            const ticks send = start();
            ping.v.store(i + 1, std::memory_order_release);
            while (pong.v.load(std::memory_order_acquire) != i + 1) detail::cpu_relax();
            const ticks recv = detail::after_load();
            a_send[i] = send;
            a_recv[i] = recv;
        }
    });
    ta.join();
    tb.join();

    cross_cpu_pair p;
    if (!a_pinned) {
        p.reason = "cannot pin to CPU " + std::to_string(a);
    } else if (b_state.load() < 0) {
        p.reason = "cannot pin to CPU " + std::to_string(b);
    } else {
        p = summarise_cross_cpu(a_send, b_recv, b_send, a_recv, warmup);
    }
    p.a = a;
    p.b = b;
    return p;
}

inline constexpr const char* online_cpus_path = "/sys/devices/system/cpu/online";

// Kernel CPU list format, e.g. "0-3,8,10-11". Empty on malformed input.
inline std::vector<int> parse_cpu_list(const std::string& s) {
    std::vector<int> cpus;
    const char* p = s.data();
    const char* end = p + s.size();
    while (end > p && (end[-1] == '\n' || end[-1] == ' ')) --end;
    while (p < end) {
        int lo, hi;
        auto r = std::from_chars(p, end, lo);
        if (r.ec != std::errc{}) return {};
        p = r.ptr;
        hi = lo;
        if (p < end && *p == '-') {
            r = std::from_chars(p + 1, end, hi);
            if (r.ec != std::errc{} || hi < lo) return {};
            p = r.ptr;
        }
        for (int c = lo; c <= hi; ++c) cpus.push_back(c);
        if (p < end) {
            if (*p != ',') return {};
            ++p;
        }
    }
    return cpus;
}

inline std::vector<int> online_cpus(const std::string& path = online_cpus_path) {
    std::ifstream f(path);
    std::string s;
    std::getline(f, s);
    return parse_cpu_list(s);
}

// CPU a against every other online CPU (S5).
inline std::vector<cross_cpu_pair> measure_cross_cpu(int a = 0,
                                                     std::size_t round_trips = default_round_trips,
                                                     std::size_t warmup = default_cross_cpu_warmup) {
    std::vector<cross_cpu_pair> pairs;
    for (int b : online_cpus())
        if (b != a) pairs.push_back(measure_pair(a, b, round_trips, warmup));
    return pairs;
}

}  // namespace lfipc::bench
