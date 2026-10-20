// Note 003's open-loop generator across two processes, over note 008's
// shared-memory ring. Design: docs/design/008-shm-transport.md (Measurement)
#pragma once

#include <lfipc/bench/load.hpp>
#include <lfipc/bench/placement.hpp>
#include <lfipc/shm.hpp>

#include <sched.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

#include <atomic>
#include <cstdint>
#include <fstream>
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

// What a cross-process run saw (note 012): where each process has the ring,
// and each process's speculation controls (/proc/<pid>/status).
struct xproc_info {
    std::uintptr_t producer_ring = 0, consumer_ring = 0;
    std::string producer_speculation, consumer_speculation;
};

// The "Speculation..." lines of /proc/<pid>/status, joined by "; ".
inline std::string speculation_status(pid_t pid) {
    std::ifstream f("/proc/" + std::to_string(pid) + "/status");
    std::string line, out;
    while (std::getline(f, line))
        if (line.rfind("Speculation", 0) == 0) {
            for (auto& ch : line)
                if (ch == '\t') ch = ' ';
            out += (out.empty() ? "" : "; ") + line;
        }
    return out;
}

namespace detail {

// Shared between parent and child, mapped before fork: the start-up signals,
// and the child's results as plain words.
struct xproc_shared {
    load_sync sync;
    std::atomic<int> child_stage{0};  // 1 attached and pinned, 2 results written
    std::uint64_t out_of_order = 0, negative = 0;
    std::uint64_t consumer_ring = 0;  // the ring's address in the child
    std::uint64_t latency[histogram<>::state_words];
};
static_assert(std::is_standard_layout_v<xproc_shared>);

}  // namespace detail

// Producer in this process, consumer in a forked child that attaches to the
// segments by name (its own mappings, as an unrelated process would), or with
// `inherit` uses the mappings it inherits across fork, at the same addresses
// (note 012, cell D). `info`, if given, receives the ring's address in each
// process and each process's speculation controls. The same
// producer_loop and consumer_loop as the thread version. This process pins to
// the producer CPU for the run and creates the segments there, so their pages
// are first touched on the producer's node; its affinity is restored after.
template <bool Echo>
void run_load_xproc(const load_config& c, load_result& r, bool inherit = false, xproc_info* info = nullptr) {
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
        // Consumer process. Its own mappings of the segments, by name, or the
        // ones inherited from the parent.
        std::string e;
        std::unique_ptr<shm_queue> in_own, out_own;
        if (!inherit) {
            in_own = shm_queue::attach(base + "-fwd", shm_message_type_id, e);
            if (in_own && Echo) out_own = shm_queue::attach(base + "-back", shm_message_type_id, e);
        }
        shm_queue* in = inherit ? fwd.get() : in_own.get();
        shm_queue* out = inherit ? back.get() : out_own.get();
        const bool ok = in && (!Echo || out) && detail::pin_to(c.consumer_cpu);
        if (!ok) sync.failed.fetch_add(1);
        if (in) shared->consumer_ring = reinterpret_cast<std::uintptr_t>(&in->ring());
        shared->child_stage.store(1, std::memory_order_release);
        sync.ready.fetch_add(1, std::memory_order_release);
        if (!ok) std::_Exit(1);
        while (sync.t0.load(std::memory_order_acquire) == 0) detail::cpu_relax();
        if (sync.abort.load()) std::_Exit(0);
        auto local = std::make_unique<load_result>();  // histograms built before the loop
        shm_queue_sut in_sut{in};
        if constexpr (Echo) {
            shm_queue_sut out_sut{out};
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
    if (info) {
        // Before the schedule starts: the child is attached (or has failed) and
        // waiting for t0.
        while (shared->child_stage.load(std::memory_order_acquire) < 1 && !sync.abort.load()) detail::cpu_relax();
        info->producer_ring = reinterpret_cast<std::uintptr_t>(&fwd->ring());
        info->consumer_ring = static_cast<std::uintptr_t>(shared->consumer_ring);
        info->producer_speculation = speculation_status(::getpid());
        info->consumer_speculation = speculation_status(child);
    }
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
    if (r.ok || r.reason.rfind("producer lateness", 0) == 0) apply_placement(r, &fwd->ring(), c.producer_cpu);
    ::munmap(mem, sizeof(detail::xproc_shared));
}

}  // namespace lfipc::bench
