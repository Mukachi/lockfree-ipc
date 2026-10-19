// Note 003's open-loop generator across two processes, over note 008's
// shared-memory ring. Design: docs/design/008-shm-transport.md (Measurement)
#pragma once

#include <lfipc/bench/load.hpp>
#include <lfipc/shm.hpp>

#include <sched.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

#include <atomic>
#include <cstdint>
#include <memory>
#include <new>
#include <string>
#include <thread>

namespace lfipc::bench {

// The final queue (note 006, v3) in a segment, for the generator.
using shm_queue = shm_spsc<message, 1024>;
inline constexpr std::uint64_t shm_message_type_id = 0x6C66697063303031ULL;  // tag for bench::message

struct shm_queue_sut {
    shm_queue* q;
    bool send(const message& m) { return q->ring().try_push(m); }
    bool try_receive(message& m) { return q->ring().try_pop(m); }
};
static_assert(load_sut<shm_queue_sut>);

namespace detail {

// Shared between parent and child, mapped before fork: the start-up signals,
// and the child's results as plain words.
struct xproc_shared {
    load_sync sync;
    std::atomic<int> child_stage{0};  // 1 attached and pinned, 2 results written
    std::uint64_t out_of_order = 0, negative = 0;
    std::uint64_t latency[histogram<>::state_words];
};
static_assert(std::is_standard_layout_v<xproc_shared>);

}  // namespace detail

// Producer in this process, consumer in a forked child that attaches to the
// segments by name (its own mappings, as an unrelated process would). The same
// producer_loop and consumer_loop as the thread version. This process pins to
// the producer CPU for the run and creates the segments there, so their pages
// are first touched on the producer's node; its affinity is restored after.
template <bool Echo>
void run_load_xproc(const load_config& c, load_result& r) {
    if (!detail::prepare_load(c, r, Echo)) return;
    const std::size_t warm = r.warmup_discarded;
    const std::size_t n = warm + c.messages;

    cpu_set_t saved;
    CPU_ZERO(&saved);
    sched_getaffinity(0, sizeof(saved), &saved);
    struct restore_affinity {
        cpu_set_t& set;
        ~restore_affinity() { sched_setaffinity(0, sizeof(set), &set); }
    } restore{saved};
    if (!detail::pin_to(c.producer_cpu)) {
        r.reason = "cannot pin producer CPU " + std::to_string(c.producer_cpu);
        return;
    }

    static std::atomic<int> serial{0};
    const std::string base = "/lfipc-load-" + std::to_string(::getpid()) + "-" + std::to_string(serial++);
    std::string err;
    auto fwd = shm_queue::create(base + "-fwd", shm_message_type_id, err);
    std::unique_ptr<shm_queue> back;
    if (fwd && Echo) back = shm_queue::create(base + "-back", shm_message_type_id, err);
    if (!fwd || (Echo && !back)) {
        r.reason = "cannot create segment: " + err;
        return;
    }

    void* mem = ::mmap(nullptr, sizeof(detail::xproc_shared), PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if (mem == MAP_FAILED) {
        r.reason = "cannot map shared control region";
        return;
    }
    auto* shared = new (mem) detail::xproc_shared();
    detail::load_sync& sync = shared->sync;

    const pid_t child = ::fork();
    if (child < 0) {
        ::munmap(mem, sizeof(detail::xproc_shared));
        r.reason = "fork failed";
        return;
    }
    if (child == 0) {
        // Consumer process. Its own mappings of the segments, by name.
        std::string e;
        auto in = shm_queue::attach(base + "-fwd", shm_message_type_id, e);
        std::unique_ptr<shm_queue> out;
        if (in && Echo) out = shm_queue::attach(base + "-back", shm_message_type_id, e);
        const bool ok = in && (!Echo || out) && detail::pin_to(c.consumer_cpu);
        if (!ok) sync.failed.fetch_add(1);
        shared->child_stage.store(1, std::memory_order_release);
        sync.ready.fetch_add(1, std::memory_order_release);
        if (!ok) std::_Exit(1);
        while (sync.t0.load(std::memory_order_acquire) == 0) detail::cpu_relax();
        if (sync.abort.load()) std::_Exit(0);
        auto local = std::make_unique<load_result>();  // histograms built before the loop
        shm_queue_sut in_sut{in.get()};
        if constexpr (Echo) {
            shm_queue_sut out_sut{out.get()};
            detail::consumer_loop<true>(in_sut, out_sut, *local, warm, n);
        } else {
            detail::no_return none;
            detail::consumer_loop<false>(in_sut, none, *local, warm, n);
        }
        shared->out_of_order = local->out_of_order;
        shared->negative = local->negative;
        local->latency.export_state(shared->latency);
        shared->child_stage.store(2, std::memory_order_release);
        std::_Exit(0);  // no destructors: the segments belong to the parent
    }

    // Producer, here. A watchdog reaps the child; if it exits early, abort
    // stops the producer at its next refused send or empty echo poll.
    int status = 0;
    std::atomic<bool> done{false};
    std::thread watchdog([&] {
        ::waitpid(child, &status, 0);
        if (!done.load()) sync.abort.store(true);
    });

    std::vector<ticks> offset;
    detail::fill_schedule(offset, n, r.interval_ticks);
    sync.ready.fetch_add(1, std::memory_order_release);
    detail::start_schedule(sync, c.ticks_per_ns);
    if (!sync.abort.load()) {
        const ticks base_t0 = sync.t0.load(std::memory_order_acquire);
        shm_queue_sut out_sut{fwd.get()};
        if constexpr (Echo) {
            shm_queue_sut back_sut{back.get()};
            detail::producer_loop<true>(out_sut, back_sut, r, sync, offset, base_t0, warm, n);
        } else {
            detail::no_return none;
            detail::producer_loop<false>(out_sut, none, r, sync, offset, base_t0, warm, n);
        }
    }
    done.store(true);
    watchdog.join();

    const bool child_ok = WIFEXITED(status) && WEXITSTATUS(status) == 0 &&
                          shared->child_stage.load(std::memory_order_acquire) == 2;
    if (child_ok) {
        r.out_of_order = shared->out_of_order;
        r.negative = shared->negative;
        r.latency.import_state(shared->latency);
    }
    // `sync` lives in the shared region: every use comes before the unmap.
    const bool aborted = sync.abort.load() && !sync.failed.load();
    if (aborted || (!child_ok && !sync.failed.load()))
        r.reason = "consumer process ended early (status " + std::to_string(status) + ")";
    else
        detail::finish_load(c, r, sync);
    ::munmap(mem, sizeof(detail::xproc_shared));
}

}  // namespace lfipc::bench
