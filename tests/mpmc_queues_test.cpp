// Tests for bench/mpmc_queues.hpp: every MPMC queue, through its adapter,
// delivers every message exactly once, each producer's in order.
// Spec: docs/design/010-mpmc-queue.md
#include "../bench/mpmc_queues.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace {

int failures = 0;

void check(bool cond, const std::string& what) {
    std::printf("%s  %s\n", cond ? "pass" : "FAIL", what.c_str());
    if (!cond) ++failures;
}

#if defined(__SANITIZE_THREAD__) || defined(__SANITIZE_ADDRESS__)
constexpr std::uint64_t n = 200'000;
#else
constexpr std::uint64_t n = 2'000'000;
#endif

using namespace lfipc::bench;

template <typename Sut>
void through(const char* name, int producers, int consumers) {
    auto s = std::make_unique<Sut>();
    const std::uint64_t per = n / static_cast<std::uint64_t>(producers), total = per * static_cast<std::uint64_t>(producers);
    std::atomic<int> done{0};
    struct result {
        std::vector<std::uint8_t> seen;
        std::uint64_t out_of_order = 0, bad = 0;
    };
    std::vector<result> res(static_cast<std::size_t>(consumers));
    std::vector<std::thread> threads;
    for (int c = 0; c < consumers; ++c)
        threads.emplace_back([&, c] {
            result& r = res[static_cast<std::size_t>(c)];
            r.seen.assign(total, 0);
            std::vector<std::uint64_t> next(static_cast<std::size_t>(producers), 0);
            message m{};
            for (;;) {
                if (!s->try_receive(m)) {
                    if (done.load(std::memory_order_acquire) == producers && !s->try_receive(m)) break;
                    std::this_thread::yield();
                    continue;
                }
                const std::uint64_t p = m.seq >> producer_shift, i = m.seq & index_mask;
                if (p >= static_cast<std::uint64_t>(producers) || i >= per || m.intended != i * 3) {
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
            for (std::uint64_t i = 0; i < per; ++i)
                while (!s->send(message{(static_cast<std::uint64_t>(p) << producer_shift) | i, i * 3}))
                    std::this_thread::yield();
            done.fetch_add(1, std::memory_order_release);
        });
    for (auto& t : threads) t.join();
    std::uint64_t once = 0, ooo = 0, bad = 0;
    for (std::uint64_t k = 0; k < total; ++k) {
        int c = 0;
        for (const auto& r : res) c += r.seen[k];
        once += c == 1;
    }
    for (const auto& r : res) {
        ooo += r.out_of_order;
        bad += r.bad;
    }
    check(once == total && ooo == 0 && bad == 0,
          std::string(name) + ": " + std::to_string(producers) + " x " + std::to_string(consumers) + ", " +
              std::to_string(total) + " messages, each exactly once, per-producer order (out of order " +
              std::to_string(ooo) + ", bad " + std::to_string(bad) + ")");
}

template <typename Sut>
void all(const char* name) {
    {
        // Capacity as wrapped: fill until send refuses, then drain in order.
        auto s = std::make_unique<Sut>();
        std::size_t filled = 0;
        while (filled < 8 * queue_capacity && s->send(message{filled, 0})) ++filled;
        message m{};
        std::size_t drained = 0;
        bool order = true;
        while (s->try_receive(m)) order = order && m.seq == drained++;
        std::printf("info  %s holds %zu for %zu asked\n", name, filled, queue_capacity);
        check(filled >= queue_capacity && drained == filled && order,
              std::string(name) + ": holds at least the capacity asked, drains in order");
    }
    through<Sut>(name, 4, 1);
    through<Sut>(name, 4, 4);
}

}  // namespace

int main() {
    // A broken queue can leave a thread spinning for ever: fail, not hang.
    std::thread([] {
        std::this_thread::sleep_for(std::chrono::seconds(120));
        std::printf("FAIL  not finished within 120 s\n");
        std::fflush(stdout);
        std::_Exit(EXIT_FAILURE);
    }).detach();

    all<ring_sut<lfipc::mpmc_v1>>("mpmc v1");
    all<ring_sut<lfipc::mpmc_v2>>("mpmc v2");
#if LFIPC_HAVE_BASELINES
    all<moodycamel_mpmc_sut>("moodycamel");
    all<boost_mpmc_sut>("boost");
    all<rigtorp_sut>("rigtorp");
#else
    std::printf("info  baselines left out under ThreadSanitizer (see bench/queues.hpp)\n");
#endif
    std::printf("%s\n", failures ? "FAILED" : "all passed");
    return failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
