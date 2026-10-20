// Tests for include/lfipc/mpmc.hpp. Spec: docs/design/010-mpmc-queue.md
#include <lfipc/bench/load.hpp>  // message: the measured 16-byte payload
#include <lfipc/mpmc.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <memory>
#include <string>
#include <thread>
#include <type_traits>
#include <vector>

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

std::uint64_t seq_of(std::uint64_t v) { return v; }
std::uint64_t seq_of(const wide& w) { return w.seq; }
bool consistent(std::uint64_t) { return true; }
bool consistent(const wide& w) { return w.consistent(); }
template <typename T>
T make(std::uint64_t s) {
    if constexpr (std::is_same_v<T, wide>) return wide::make(s);
    else return s;
}

// Messages per shape; fewer under sanitizers, which GCC flags.
#ifndef LFIPC_STRESS_N
#if defined(__SANITIZE_THREAD__) || defined(__SANITIZE_ADDRESS__)
#define LFIPC_STRESS_N 250'000
#else
#define LFIPC_STRESS_N 5'000'000
#endif
#endif
constexpr std::uint64_t stress_n = LFIPC_STRESS_N;

// Producer index in the top 8 bits of the sequence number, as the generator
// does (note 010).
constexpr int producer_shift = 56;

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

    // Every fill level, many laps: the sequence numbers must stay in step.
    std::uint64_t in = 0, out = 0;
    for (int round = 0; round < 100; ++round) {
        const std::size_t fill = static_cast<std::size_t>(round) % (N + 1);
        for (std::size_t i = 0; i < fill; ++i) ok = ok && q->try_push(in++);
        for (std::size_t i = 0; i < fill; ++i) ok = ok && q->try_pop(v) && v == out++;
    }
    check(ok && !q->try_pop(v), std::string(name) + ": 100 rounds at every fill level, FIFO across laps");
}

// S2: positions started just below 2^64, across the wrap.
template <template <typename, std::size_t> class Q>
void wrap(const char* name) {
    constexpr std::size_t N = 8;
    const std::uint64_t start = std::numeric_limits<std::uint64_t>::max() - 3;
    auto q = std::make_unique<Q<std::uint64_t, N>>(start);
    std::uint64_t v = 0;
    bool ok = !q->try_pop(v);
    for (std::uint64_t i = 0; i < N; ++i) ok = ok && q->try_push(i);
    ok = ok && !q->try_push(99);
    for (std::uint64_t i = 0; i < N; ++i) ok = ok && q->try_pop(v) && v == i;
    ok = ok && !q->try_pop(v);
    for (std::uint64_t lap = 0; lap < 3; ++lap) {
        for (std::uint64_t i = 0; i < N; ++i) ok = ok && q->try_push(i);
        ok = ok && !q->try_push(99);
        for (std::uint64_t i = 0; i < N; ++i) ok = ok && q->try_pop(v) && v == i;
    }
    check(ok && !q->try_pop(v), std::string(name) + ": S2 full and empty exact across the 2^64 wrap");
}

// S1: P producers, C consumers. Every message delivered exactly once (one
// bitmap per consumer, combined after), each consumer sees each producer's
// messages in increasing order, no torn message.
template <template <typename, std::size_t> class Q, typename T>
void stress(const char* name, int producers, int consumers) {
    auto q = std::make_unique<Q<T, 1024>>();
    const std::uint64_t per = stress_n / static_cast<std::uint64_t>(producers);
    const std::uint64_t total = per * static_cast<std::uint64_t>(producers);
    std::atomic<int> producers_done{0};
    struct result {
        std::vector<std::uint8_t> seen;
        std::uint64_t out_of_order = 0, torn = 0, bad = 0;
    };
    std::vector<result> res(static_cast<std::size_t>(consumers));
    std::vector<std::thread> threads;
    for (int c = 0; c < consumers; ++c)
        threads.emplace_back([&, c] {
            result& r = res[static_cast<std::size_t>(c)];
            r.seen.assign(total, 0);
            std::vector<std::uint64_t> next(static_cast<std::size_t>(producers), 0);
            T m{};
            // Stops on an empty queue once every producer has finished: what
            // is left then is final, so a lost message is counted, not waited for.
            for (;;) {
                if (!q->try_pop(m)) {
                    if (producers_done.load(std::memory_order_acquire) == producers && !q->try_pop(m)) break;
                    std::this_thread::yield();  // the suite may run on two CPUs (CI)
                    continue;
                }
                const std::uint64_t s = seq_of(m);
                const std::uint64_t p = s >> producer_shift, i = s & ((std::uint64_t{1} << producer_shift) - 1);
                if (!consistent(m)) ++r.torn;
                if (p >= static_cast<std::uint64_t>(producers) || i >= per) {
                    ++r.bad;
                    continue;
                }
                if (i < next[p]) ++r.out_of_order;
                next[p] = i + 1;
                r.seen[p * per + i] = 1;
            }
        });
    for (int p = 0; p < producers; ++p)
        threads.emplace_back([&, p] {
            for (std::uint64_t i = 0; i < per; ++i) {
                const T m = make<T>((static_cast<std::uint64_t>(p) << producer_shift) | i);
                while (!q->try_push(m)) std::this_thread::yield();
            }
            producers_done.fetch_add(1, std::memory_order_release);
        });
    for (auto& t : threads) t.join();

    std::uint64_t once = 0, twice = 0, never = 0, ooo = 0, torn = 0, bad = 0;
    for (std::uint64_t k = 0; k < total; ++k) {
        int n = 0;
        for (const auto& r : res) n += r.seen[k];
        once += n == 1;
        twice += n > 1;
        never += n == 0;
    }
    for (const auto& r : res) {
        ooo += r.out_of_order;
        torn += r.torn;
        bad += r.bad;
    }
    char buf[200];
    std::snprintf(buf, sizeof(buf),
                  "%s: S1 %d x %d, %llu %zu-byte messages: each exactly once, per-producer order, none torn "
                  "(missing %llu, duplicated %llu, out of order %llu, torn %llu, bad %llu)",
                  name, producers, consumers, (unsigned long long)total, sizeof(T), (unsigned long long)never,
                  (unsigned long long)twice, (unsigned long long)ooo, (unsigned long long)torn,
                  (unsigned long long)bad);
    check(once == total && ooo == 0 && torn == 0 && bad == 0, buf);
}

template <template <typename, std::size_t> class Q>
void all(const char* name) {
    single_thread<Q>(name);
    wrap<Q>(name);
    for (const auto& [p, c] : {std::pair{1, 2}, {2, 1}, {2, 2}, {4, 4}, {8, 1}}) stress<Q, std::uint64_t>(name, p, c);
    stress<Q, wide>(name, 2, 2);
    stress<Q, wide>(name, 4, 4);
}

}  // namespace

int main() {
    // A broken queue can look full for ever, leaving a producer spinning: fail
    // rather than hang.
    std::thread([] {
        std::this_thread::sleep_for(std::chrono::seconds(120));
        std::printf("FAIL  not finished within 120 s: a producer or consumer is stuck\n");
        std::fflush(stdout);
        std::_Exit(EXIT_FAILURE);
    }).detach();

    using lfipc::bench::message;
    using v1 = lfipc::mpmc_v1<message, 1024>;
    using v2 = lfipc::mpmc_v2<message, 1024>;

    // S4 and the layouts the note describes.
    static_assert(std::is_standard_layout_v<v1> && std::is_standard_layout_v<v2>);
    static_assert(sizeof(lfipc::detail::mpmc_slot<message, 0>) == 24, "v1: 16-byte message + 8-byte sequence, packed");
    static_assert(sizeof(lfipc::detail::mpmc_slot<message, 128>) == 128, "v2: one slot per 128 bytes");
    static_assert(sizeof(v1) == 256 + 1024 * 24 && sizeof(v2) == 256 + 1024 * 128);
    check(true, "S4 standard layout; slots 24 bytes packed (v1), 128 padded (v2); positions 128 apart");

    all<lfipc::mpmc_v1>("v1");
    all<lfipc::mpmc_v2>("v2");

    std::printf("%s\n", failures ? "FAILED" : "all passed");
    return failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
