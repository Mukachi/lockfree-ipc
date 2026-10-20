// Tests for bench/queues.hpp: every queue, through its adapter, passes the
// same two-thread sequence check. Spec: docs/design/006-spsc-queue.md
#include "../bench/queues.hpp"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <thread>

namespace {

int failures = 0;

void check(bool cond, const std::string& what) {
    std::printf("%s  %s\n", cond ? "pass" : "FAIL", what.c_str());
    if (!cond) ++failures;
}

#if defined(__SANITIZE_THREAD__) || defined(__SANITIZE_ADDRESS__)
constexpr std::uint64_t n = 200'000;
#else
constexpr std::uint64_t n = 5'000'000;
#endif

using namespace lfipc::bench;

template <typename Sut>
void through(const char* name, std::size_t expect_capacity) {
    auto s = std::make_unique<Sut>();
    // Capacity as wrapped: fill until send refuses, then drain.
    std::size_t filled = 0;
    while (filled < 4 * queue_capacity && s->send(message{filled, 0})) ++filled;
    message m{};
    std::size_t drained = 0;
    bool order = true;
    while (s->try_receive(m)) order = order && m.seq == drained++;
    check(filled == expect_capacity && drained == filled && order,
          std::string(name) + ": holds " + std::to_string(filled) + ", drains in order");

    std::uint64_t bad = 0;
    std::thread consumer([&] {
        message v{};
        for (std::uint64_t expected = 0; expected < n;) {
            if (!s->try_receive(v)) continue;
            bad += v.seq != expected || v.intended != expected * 3;
            ++expected;
        }
    });
    for (std::uint64_t i = 0; i < n; ++i)
        while (!s->send(message{i, i * 3})) {
        }
    consumer.join();
    check(bad == 0, std::string(name) + ": " + std::to_string(n) + " messages across threads, in order");
}

}  // namespace

int main() {
    through<ring_sut<lfipc::spsc_v1>>("spsc v1", queue_capacity);
    through<ring_sut<lfipc::spsc_v2>>("spsc v2", queue_capacity);
    through<ring_sut<lfipc::spsc_v3>>("spsc v3", queue_capacity);
    through<ring_sut<lfipc::spsc_v4>>("spsc v4", queue_capacity);
#if LFIPC_HAVE_BASELINES
    // moodycamel rounds its storage up, so it may hold more than asked.
    {
        auto s = std::make_unique<moodycamel_sut>();
        std::size_t filled = 0;
        while (filled < 4 * queue_capacity && s->send(message{filled, 0})) ++filled;
        std::printf("info  moodycamel holds %zu without allocating, for %zu asked\n", filled, queue_capacity);
        through<moodycamel_sut>("moodycamel", filled);
    }
    through<boost_sut>("boost", queue_capacity);
#else
    std::printf("info  baselines left out under ThreadSanitizer (see bench/queues.hpp)\n");
#endif
    std::printf("%s\n", failures ? "FAILED" : "all passed");
    return failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
