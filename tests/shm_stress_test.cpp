// Two-process stress test for include/lfipc/shm.hpp. Spec: docs/design/008-shm-transport.md (S1)
//
// The parent creates a segment and forks; the child attaches and checks every
// message: in order, none lost, duplicated or torn. 64-byte messages derive
// every field from their sequence number, so a torn read is detectable.
#include <lfipc/shm.hpp>

#include <sys/wait.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <type_traits>

namespace {

int failures = 0;

void check(bool cond, const std::string& what) {
    std::printf("%s  %s\n", cond ? "pass" : "FAIL", what.c_str());
    if (!cond) ++failures;
}

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

#if defined(__SANITIZE_THREAD__) || defined(__SANITIZE_ADDRESS__)
constexpr std::uint64_t n = 1'000'000;
#else
constexpr std::uint64_t n = 20'000'000;
#endif

// Child exit status: 0 all good, 1 cannot attach, 2 a message out of order, lost, duplicated or torn.
template <typename T>
void across_processes(const char* type, std::uint64_t type_id) {
    using q = lfipc::shm_spsc<T, 1024>;
    const std::string name = "/lfipc-stress-" + std::to_string(::getpid()) + "-" + type;
    std::string err;
    auto producer = q::create(name, type_id, err);
    if (!producer) {
        check(false, std::string(type) + ": create: " + err);
        return;
    }
    const pid_t child = ::fork();
    if (child == 0) {
        std::string e;
        auto consumer = q::attach(name, type_id, e);
        if (!consumer) std::_Exit(1);
        T v{};
        for (std::uint64_t expected = 0; expected < n;) {
            if (!consumer->ring().try_pop(v)) continue;
            bool good;
            if constexpr (std::is_same_v<T, wide>) good = v.seq == expected && v.consistent();
            else good = v == expected;
            if (!good) std::_Exit(2);
            ++expected;
        }
        std::_Exit(0);
    }
    // If the consumer exits early (it found a bad message), the ring fills and
    // stays full: check for that while pushes fail, so the test fails, not hangs.
    int status = 0;
    bool reaped = false;
    for (std::uint64_t i = 0; i < n && !reaped; ++i) {
        T v;
        if constexpr (std::is_same_v<T, wide>) v = wide::make(i);
        else v = i;
        for (std::uint64_t spins = 0; !producer->ring().try_push(v); ++spins) {
            if (spins % (1u << 20) == 0 && ::waitpid(child, &status, WNOHANG) == child) {
                reaped = true;
                break;
            }
        }
    }
    if (!reaped) ::waitpid(child, &status, 0);
    const int code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    check(code == 0, std::string("S1 ") + std::to_string(n) + " " + type +
                         " messages from one process to another, in order, none lost, duplicated or torn" +
                         (code == 0 ? "" : " (child exit " + std::to_string(code) + ")"));
}

}  // namespace

int main() {
    across_processes<std::uint64_t>("8-byte", 1);
    across_processes<wide>("64-byte", 2);
    std::printf("%s\n", failures ? "FAILED" : "all passed");
    return failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
