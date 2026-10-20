// What a test may assume about the machine it runs on, so the suite runs on
// dev-lnx01, a 2-vCPU CI runner, or dev-lnx01 under the bench fence.
// Benchmarks do not use this; their CPUs are fixed. Design: docs/design/009-ci.md
#pragma once

#include <lfipc/bench/cross_cpu.hpp>  // parse_cpu_list

#include <sched.h>

#include <cstdio>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>
#include <vector>

namespace lfipc::test {

// Returned by a test that could check nothing on this machine (CTest
// SKIP_RETURN_CODE).
inline constexpr int skip_code = 77;

// Checks that hold only on a quiet, dedicated machine (timing accuracy, clock
// health) run when the test is started with the argument "quiet", registered
// in CTest under the label `quiet`; everything else runs without it. CI runs
// `ctest -LE quiet`.
inline bool quiet_run(int argc, char** argv) { return argc > 1 && std::string(argv[1]) == "quiet"; }

// A check this machine cannot offer: reported, never counted as a pass.
inline void skip(const std::string& what, const std::string& why) {
    std::printf("skip  %s: %s\n", what.c_str(), why.c_str());
}

// CPUs this process may run on.
inline std::vector<int> allowed_cpus() {
    std::vector<int> out;
    cpu_set_t set;
    CPU_ZERO(&set);
    if (sched_getaffinity(0, sizeof(set), &set) != 0) return out;
    for (int c = 0; c < CPU_SETSIZE; ++c)
        if (CPU_ISSET(c, &set)) out.push_back(c);
    return out;
}

// n distinct CPUs to pin to. The kernel's isolated CPUs come first (on
// dev-lnx01, 2-21 and 46-65, so the tests pin 2, 3, ... as they always have),
// then the rest of the allowed set. Nothing if fewer than n are allowed.
inline std::optional<std::vector<int>> pick_cpus(std::size_t n) {
    const auto allowed = allowed_cpus();
    std::ifstream f("/sys/devices/system/cpu/isolated");
    const auto isolated = bench::parse_cpu_list(std::string(std::istreambuf_iterator<char>(f), {}));
    std::vector<int> out;
    for (int c : isolated)
        for (int a : allowed)
            if (a == c && out.size() < n) out.push_back(c);
    for (int a : allowed) {
        if (out.size() == n) break;
        bool taken = false;
        for (int c : out) taken = taken || c == a;
        if (!taken) out.push_back(a);
    }
    if (out.size() < n) return std::nullopt;
    return out;
}

}  // namespace lfipc::test
