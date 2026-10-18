// Clock reads and startup checks. Design: docs/design/001-clock.md
#pragma once

#include <cstdint>
#include <fstream>
#include <string>

#if defined(__x86_64__)
#include <cpuid.h>
#include <x86intrin.h>
#define LFIPC_CLOCK_TSC 1
#else
#include <chrono>
#define LFIPC_CLOCK_TSC 0
#endif

namespace lfipc::bench {

using ticks = std::uint64_t;

#if LFIPC_CLOCK_TSC

inline constexpr bool clock_is_tsc = true;

// Start of a timed region: earlier instructions complete before the read,
// later ones do not begin before it.
[[gnu::always_inline]] inline ticks start() noexcept {
    _mm_lfence();
    const ticks t = __rdtsc();
    _mm_lfence();
    return t;
}

// End of a timed region: rdtscp waits for earlier instructions; the fence
// keeps later ones from moving above the read.
[[gnu::always_inline]] inline ticks stop() noexcept {
    unsigned aux;
    const ticks t = __rdtscp(&aux);
    _mm_lfence();
    return t;
}

// Bare read for timestamps carried across threads in a message. No fences,
// so the path being measured is not distorted.
[[gnu::always_inline]] inline ticks stamp() noexcept { return __rdtsc(); }

#else

// Non-x86 fallback so correctness tests build on ARM. check_clock() refuses
// it, so it never produces published measurements.
inline constexpr bool clock_is_tsc = false;

inline ticks fallback_now() noexcept {
    return static_cast<ticks>(std::chrono::steady_clock::now().time_since_epoch().count());
}
inline ticks start() noexcept { return fallback_now(); }
inline ticks stop() noexcept { return fallback_now(); }
inline ticks stamp() noexcept { return fallback_now(); }

#endif

struct check_result {
    bool ok;
    std::string reason;  // empty when ok
};

// CPUID leaf 0x80000007, EDX bit 8: TSC rate is constant across P-, C- and T-states.
inline bool cpu_has_invariant_tsc() noexcept {
#if LFIPC_CLOCK_TSC
    unsigned a, b, c, d;
    if (!__get_cpuid(0x80000000u, &a, &b, &c, &d) || a < 0x80000007u) return false;
    __get_cpuid(0x80000007u, &a, &b, &c, &d);
    return (d >> 8) & 1u;
#else
    return false;
#endif
}

inline constexpr const char* kernel_clocksource_path =
    "/sys/devices/system/clocksource/clocksource0/current_clocksource";

// Empty string if the file cannot be read.
inline std::string read_clocksource(const std::string& path = kernel_clocksource_path) {
    std::ifstream f(path);
    std::string s;
    f >> s;
    return s;
}

// The decision alone, separated from the system reads so every refusal path is testable.
inline check_result evaluate_clock(bool is_tsc, bool invariant_tsc, const std::string& clocksource) {
    if (!is_tsc) return {false, "not x86-64: steady_clock fallback produces no published measurements"};
    if (!invariant_tsc) return {false, "CPU lacks invariant TSC (CPUID 0x80000007 EDX bit 8)"};
    if (clocksource.empty()) return {false, "cannot read kernel clocksource"};
    if (clocksource != "tsc") return {false, "kernel clocksource is '" + clocksource + "', not 'tsc'"};
    return {true, {}};
}

inline check_result check_clock(const std::string& clocksource_path = kernel_clocksource_path) {
    return evaluate_clock(clock_is_tsc, cpu_has_invariant_tsc(), read_clocksource(clocksource_path));
}

}  // namespace lfipc::bench
