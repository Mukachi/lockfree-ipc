// The cells of note 012: v3 between two ends on one segment layout, varying
// only address space (threads or processes) and virtual address (one mapping
// or two), plus note 006's thread version as the layout reference.
// Benchmark glue. Design: docs/design/012-siblings.md
#pragma once

#include <lfipc/bench/load.hpp>
#include <lfipc/bench/load_xproc.hpp>
#include <lfipc/bench/placement.hpp>
#include <lfipc/spsc.hpp>

#include <sched.h>
#include <unistd.h>

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>

namespace lfipc::bench {

// The producer pushes through one mapping of the segment, the consumer pops
// through another, or the same one.
struct shm_mapped_sut {
    shm_queue* tx;
    shm_queue* rx;
    bool send(const message& m) { return tx->ring().try_push(m); }
    bool try_receive(message& m) { return rx->ring().try_pop(m); }
};
static_assert(load_sut<shm_mapped_sut>);

// v3 on the heap, as note 006 measures it (queues.hpp's ring_sut, without the
// baselines that file pulls in, so this builds in sanitizer builds too).
struct v3_sut {
    spsc_v3<message, 1024> q;
    bool send(const message& m) { return q.try_push(m); }
    bool try_receive(message& m) { return q.try_pop(m); }
};

// Cells A and B: two threads of this process on the segment. The segments are
// created by this thread pinned to the producer CPU (first touch on its node,
// as the process cells do); with `two_mappings` the consumer side maps them a
// second time, by name, at another address.
template <bool Echo>
void run_threads_on_segment(const load_config& c, load_result& r, bool two_mappings, xproc_info& info) {
    cpu_set_t saved;
    CPU_ZERO(&saved);
    sched_getaffinity(0, sizeof(saved), &saved);
    if (!detail::pin_to(c.producer_cpu)) {
        r = {};
        r.reason = "cannot pin producer CPU " + std::to_string(c.producer_cpu);
        return;
    }
    static std::atomic<int> serial{0};
    const std::string base = "/lfipc-sib-" + std::to_string(::getpid()) + "-" + std::to_string(serial++);
    std::string err;
    auto fwd = shm_queue::create(base + "-fwd", shm_message_type_id, err);
    auto back = fwd && Echo ? shm_queue::create(base + "-back", shm_message_type_id, err) : nullptr;
    std::unique_ptr<shm_queue> fwd2, back2;
    if (two_mappings && fwd) fwd2 = shm_queue::attach(base + "-fwd", shm_message_type_id, err);
    if (two_mappings && back) back2 = shm_queue::attach(base + "-back", shm_message_type_id, err);
    sched_setaffinity(0, sizeof(saved), &saved);
    if (!fwd || (Echo && !back) || (two_mappings && (!fwd2 || (Echo && !back2)))) {
        r = {};
        r.reason = "cannot create or map segment: " + err;
        return;
    }
    // The consumer's side: the second mapping, or the same one.
    shm_queue* fwd_rx = two_mappings ? fwd2.get() : fwd.get();
    shm_queue* back_tx = two_mappings ? back2.get() : back.get();
    shm_mapped_sut out{fwd.get(), fwd_rx};
    if constexpr (Echo) {
        shm_mapped_sut ret{back_tx, back.get()};
        run_load(out, ret, c, r);
    } else {
        run_load(out, c, r);
    }
    apply_placement(r, &fwd->ring(), c.producer_cpu);
    info.producer_ring = reinterpret_cast<std::uintptr_t>(&fwd->ring());
    info.consumer_ring = reinterpret_cast<std::uintptr_t>(&fwd_rx->ring());
    info.producer_speculation = info.consumer_speculation = speculation_status(::getpid());
}

// The reference: note 006's thread version, the ring at the start of a fresh
// page (placement.hpp), as spsc_bench runs it.
template <bool Echo>
void run_threads_reference(const load_config& c, load_result& r, xproc_info& info) {
    auto out = make_on_cpu<v3_sut>(c.producer_cpu);
    auto back = Echo ? make_on_cpu<v3_sut>(c.producer_cpu) : nullptr;
    if (!out || (Echo && !back)) {
        r = {};
        r.reason = "cannot pin to producer CPU " + std::to_string(c.producer_cpu) + " to place the queue";
        return;
    }
    if constexpr (Echo) run_load(*out, *back, c, r);
    else run_load(*out, c, r);
    apply_placement(r, out.get(), c.producer_cpu);
    info.producer_ring = info.consumer_ring = reinterpret_cast<std::uintptr_t>(&out->q);
    info.producer_speculation = info.consumer_speculation = speculation_status(::getpid());
}

// One cell, one mode.
enum class cell { A, B, C, D, R };

inline void run_cell(cell k, bool echo, const load_config& c, load_result& r, xproc_info& info) {
    switch (k) {
        case cell::A: echo ? run_threads_on_segment<true>(c, r, false, info) : run_threads_on_segment<false>(c, r, false, info); break;
        case cell::B: echo ? run_threads_on_segment<true>(c, r, true, info) : run_threads_on_segment<false>(c, r, true, info); break;
        case cell::C: echo ? run_load_xproc<true>(c, r, false, &info) : run_load_xproc<false>(c, r, false, &info); break;
        case cell::D: echo ? run_load_xproc<true>(c, r, true, &info) : run_load_xproc<false>(c, r, true, &info); break;
        case cell::R: echo ? run_threads_reference<true>(c, r, info) : run_threads_reference<false>(c, r, info); break;
    }
}

}  // namespace lfipc::bench
