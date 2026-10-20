// Subjects for check_hot_path.py, which disassembles this binary. The real
// overhead loop sits behind a fixed, unmangled symbol (S6); the cross-CPU
// threads are found by their lambda names (S8). The histogram's record() sits
// behind its own symbol (note 002, S1). The load generator's threads are found
// by their lambda names inside run_load_impl, run with the test mailbox (note 003, S6), and
// inside run_load_mp, run with the MPMC ring (note 010).
// SPSC try_push/try_pop sit behind lfipc_spsc_<variant>_<op> (note 006, S3), MPMC's behind
// lfipc_mpmc_<variant>_<op> (note 010, S3). Counts come from the command
// line so the compiler cannot specialise the loops. Spec: docs/design/001-clock.md
#include <lfipc/bench/cross_cpu.hpp>
#include <lfipc/bench/histogram.hpp>
#include <lfipc/bench/load.hpp>
#include <lfipc/bench/load_mp.hpp>
#include <lfipc/bench/noise.hpp>
#include <lfipc/mpmc.hpp>
#include <lfipc/shm.hpp>
#include <lfipc/spsc.hpp>

#include "test_suts.hpp"

#include <memory>
#include <lfipc/bench/overhead.hpp>

#include <cstdlib>

extern "C" [[gnu::noinline]] lfipc::bench::overhead lfipc_s6_overhead(std::size_t samples,
                                                                     std::size_t warmup) {
    return lfipc::bench::measure_overhead(samples, warmup, 0);
}

extern "C" [[gnu::noinline]] void lfipc_hist_record(lfipc::bench::histogram<>& h, lfipc::bench::ticks v) {
    h.record(v);
}

#define LFIPC_SPSC_SUBJECT(V)                                                                         \
    using spsc_##V##_subject = lfipc::spsc_##V<lfipc::bench::message, 1024>;                           \
    extern "C" [[gnu::noinline]] bool lfipc_spsc_##V##_push(spsc_##V##_subject& q,                     \
                                                           const lfipc::bench::message& m) {           \
        return q.try_push(m);                                                                          \
    }                                                                                                  \
    extern "C" [[gnu::noinline]] bool lfipc_spsc_##V##_pop(spsc_##V##_subject& q, lfipc::bench::message& m) { \
        return q.try_pop(m);                                                                           \
    }
LFIPC_SPSC_SUBJECT(v1)
LFIPC_SPSC_SUBJECT(v2)
LFIPC_SPSC_SUBJECT(v3)

#define LFIPC_MPMC_SUBJECT(V)                                                                         \
    using mpmc_##V##_subject = lfipc::mpmc_##V<lfipc::bench::message, 1024>;                           \
    extern "C" [[gnu::noinline]] bool lfipc_mpmc_##V##_push(mpmc_##V##_subject& q,                     \
                                                           const lfipc::bench::message& m) {           \
        return q.try_push(m);                                                                          \
    }                                                                                                  \
    extern "C" [[gnu::noinline]] bool lfipc_mpmc_##V##_pop(mpmc_##V##_subject& q, lfipc::bench::message& m) { \
        return q.try_pop(m);                                                                           \
    }
LFIPC_MPMC_SUBJECT(v1)
LFIPC_MPMC_SUBJECT(v2)

// Note 008, S5: the ring reached through a shared-memory segment.
using shm_subject = lfipc::shm_spsc<lfipc::bench::message, 1024>;
extern "C" [[gnu::noinline]] bool lfipc_spsc_shm_push(shm_subject& s, const lfipc::bench::message& m) {
    return s.ring().try_push(m);
}
extern "C" [[gnu::noinline]] bool lfipc_spsc_shm_pop(shm_subject& s, lfipc::bench::message& m) {
    return s.ring().try_pop(m);
}

int main(int argc, char** argv) {
    const std::size_t n = argc > 1 ? std::strtoul(argv[1], nullptr, 10) : 1000;
    {
        // Both generator variants, so both are compiled into this binary.
        lfipc::bench::load_config c;
        c.ticks_per_ns = 2.2;
        c.messages = 1'000;
        c.rate_per_s = 1e5;
        auto r = std::make_unique<lfipc::bench::load_result>();
        lfipc::test::plain out, back;
        lfipc::bench::run_load(out, c, *r);
        lfipc::bench::run_load(out, back, c, *r);
    }
    {
        // The generator for several producers and consumers (note 010).
        struct mpmc_sut {
            lfipc::mpmc_v1<lfipc::bench::message, 1024> q;
            bool send(const lfipc::bench::message& m) { return q.try_push(m); }
            bool try_receive(lfipc::bench::message& m) { return q.try_pop(m); }
        };
        lfipc::bench::mp_load_config c;
        c.ticks_per_ns = 2.2;
        c.messages = 1'000;
        c.rate_per_s = 1e5;
        c.producer_cpus = {0, 1};
        c.consumer_cpus = {0};
        auto sut = std::make_unique<mpmc_sut>();
        auto r = std::make_unique<lfipc::bench::mp_load_result>();
        lfipc::bench::run_load_mp(*sut, c, *r);
    }
    {
        // The noise spinner's loop (note 005, S5), default hook.
        auto nr = std::make_unique<lfipc::bench::noise_result>();
        lfipc::bench::measure_noise(0, 0.01, 2.2, *nr);
    }
    {
        lfipc::bench::message m{1, 2};
        auto q1 = std::make_unique<spsc_v1_subject>();
        auto q2 = std::make_unique<spsc_v2_subject>();
        auto q3 = std::make_unique<spsc_v3_subject>();
        if (!lfipc_spsc_v1_push(*q1, m) || !lfipc_spsc_v1_pop(*q1, m)) return 1;
        if (!lfipc_spsc_v2_push(*q2, m) || !lfipc_spsc_v2_pop(*q2, m)) return 1;
        if (!lfipc_spsc_v3_push(*q3, m) || !lfipc_spsc_v3_pop(*q3, m)) return 1;
        auto m1 = std::make_unique<mpmc_v1_subject>();
        auto m2 = std::make_unique<mpmc_v2_subject>();
        if (!lfipc_mpmc_v1_push(*m1, m) || !lfipc_mpmc_v1_pop(*m1, m)) return 1;
        if (!lfipc_mpmc_v2_push(*m2, m) || !lfipc_mpmc_v2_pop(*m2, m)) return 1;
        std::string err;
        auto s = shm_subject::create("/lfipc-hot-path-" + std::to_string(::getpid()), 7, err);
        if (!s || !lfipc_spsc_shm_push(*s, m) || !lfipc_spsc_shm_pop(*s, m)) return 1;
    }
    lfipc::bench::histogram<> h;
    lfipc_hist_record(h, n);
    const bool ok = h.count() == 1 && lfipc_s6_overhead(n, n / 10).samples == n &&
                    lfipc::bench::measure_pair(0, 1, n, n / 10).ok;
    return ok ? 0 : 1;
}
