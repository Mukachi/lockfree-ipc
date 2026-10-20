// Tests for include/lfipc/spsc.hpp. Spec: docs/design/006-spsc-queue.md
#include <lfipc/bench/load.hpp>  // message
#include <lfipc/spsc.hpp>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <memory>
#include <string>
#include <thread>
#include <type_traits>

namespace {

int failures = 0;

void check(bool cond, const std::string& what) {
    std::printf("%s  %s\n", cond ? "pass" : "FAIL", what.c_str());
    if (!cond) ++failures;
}

// 64 bytes whose fields are all derived from `seq`, so a torn read (part of
// one message, part of another) is detectable.
struct wide {
    std::uint64_t seq;
    std::uint64_t f[7];
    static wide make(std::uint64_t s) {
        wide w{s, {}};
        for (int i = 0; i < 7; ++i) w.f[i] = s * 0x9E3779B97F4A7C15ULL + static_cast<std::uint64_t>(i);
        return w;
    }
    bool consistent() const {
        const wide expected = make(seq);
        return std::memcmp(this, &expected, sizeof(wide)) == 0;
    }
};

// Iterations for the threaded tests; fewer under sanitizers, which GCC flags.
#ifndef LFIPC_STRESS_N
#if defined(__SANITIZE_THREAD__) || defined(__SANITIZE_ADDRESS__)
#define LFIPC_STRESS_N 1'000'000
#else
#define LFIPC_STRESS_N 20'000'000
#endif
#endif
constexpr std::uint64_t stress_n = LFIPC_STRESS_N;

template <template <typename, std::size_t> class Q>
void single_thread(const char* name) {
    constexpr std::size_t N = 8;
    auto q = std::make_unique<Q<std::uint64_t, N>>();
    std::uint64_t v = 0;
    bool ok = !q->try_pop(v);
    for (std::uint64_t i = 0; i < N; ++i) ok = ok && q->try_push(i);
    ok = ok && !q->try_push(99);  // full at exactly N
    for (std::uint64_t i = 0; i < N; ++i) ok = ok && q->try_pop(v) && v == i;
    ok = ok && !q->try_pop(v);
    check(ok, std::string(name) + ": empty, full at exactly capacity, FIFO, empty again");

    // Wrap the slot index many times, at every fill level.
    std::uint64_t next_in = 0, next_out = 0;
    bool wrap = true;
    for (int round = 0; round < 100; ++round) {
        const std::size_t fill = 1 + static_cast<std::size_t>(round) % N;
        for (std::size_t i = 0; i < fill; ++i) wrap = wrap && q->try_push(next_in++);
        for (std::size_t i = 0; i < fill; ++i) wrap = wrap && q->try_pop(v) && v == next_out++;
    }
    check(wrap && !q->try_pop(v), std::string(name) + ": 100 rounds at every fill level, order kept");
}

// S2: counters started just below 2^64, so tail and head wrap mid-test.
template <template <typename, std::size_t> class Q>
void counter_wrap(const char* name) {
    constexpr std::size_t N = 8;
    const std::uint64_t start = std::numeric_limits<std::uint64_t>::max() - 4;
    auto q = std::make_unique<Q<std::uint64_t, N>>(start);
    std::uint64_t v = 0;
    bool ok = true;
    for (int round = 0; round < 4; ++round) {
        for (std::uint64_t i = 0; i < N; ++i) ok = ok && q->try_push(i);
        ok = ok && !q->try_push(99);
        for (std::uint64_t i = 0; i < N; ++i) ok = ok && q->try_pop(v) && v == i;
        ok = ok && !q->try_pop(v);
    }
    check(ok, std::string(name) + ": S2 full and empty exact across the 2^64 counter wrap");
}

// S1: two threads, every message checked in order; `wide` also checks for torn reads.
template <template <typename, std::size_t> class Q, typename T>
void stress(const char* name, const char* type) {
    constexpr std::size_t N = 1024;
    auto q = std::make_unique<Q<T, N>>();
    std::uint64_t bad = 0, received = 0;
    std::thread consumer([&] {
        T v{};
        for (std::uint64_t expected = 0; expected < stress_n;) {
            if (!q->try_pop(v)) continue;
            if constexpr (std::is_same_v<T, wide>) {
                if (v.seq != expected || !v.consistent()) ++bad;
            } else {
                if (v != expected) ++bad;
            }
            ++expected;
            ++received;
        }
    });
    for (std::uint64_t i = 0; i < stress_n; ++i) {
        T v;
        if constexpr (std::is_same_v<T, wide>) v = wide::make(i);
        else v = i;
        while (!q->try_push(v)) {
        }
    }
    consumer.join();
    check(bad == 0 && received == stress_n,
          std::string(name) + ": S1 " + std::to_string(stress_n) + " " + type +
              " messages across threads, in order, none lost, duplicated or torn");
}

template <template <typename, std::size_t> class Q>
void all(const char* name) {
    // S4: shared-memory-ready layout.
    static_assert(std::is_standard_layout_v<Q<std::uint64_t, 8>>);
    static_assert(std::is_standard_layout_v<Q<wide, 8>>);
    check(alignof(Q<std::uint64_t, 8>) >= Q<std::uint64_t, 8>::separation,
          std::string(name) + ": S4 standard layout, counters separated by " +
              std::to_string(Q<std::uint64_t, 8>::separation) + " bytes");
    single_thread<Q>(name);
    counter_wrap<Q>(name);
    stress<Q, std::uint64_t>(name, "8-byte");
    stress<Q, wide>(name, "64-byte");
}

}  // namespace

int main() {
    all<lfipc::spsc_v1>("v1");
    all<lfipc::spsc_v2>("v2");
    all<lfipc::spsc_v3>("v3");
    all<lfipc::spsc_v4>("v4");

    // S3 (note 011): v4's size is the counters' 256 bytes plus 128 per slot;
    // v3's slots stay packed.
    static_assert(sizeof(lfipc::spsc_v4<lfipc::bench::message, 1024>) == 256 + 1024 * 128);
    static_assert(sizeof(lfipc::spsc_v3<lfipc::bench::message, 1024>) == 256 + 1024 * 16);
    check(true, "v4: 128 bytes per slot; v3 packed, unchanged");
    std::printf("%s\n", failures ? "FAILED" : "all passed");
    return failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
