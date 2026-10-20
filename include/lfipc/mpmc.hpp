// Bounded multi-producer, multi-consumer queue. Design: docs/design/010-mpmc-queue.md
#pragma once

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace lfipc {

namespace detail {

// A slot: its sequence number, then the data. Packed (SlotAlign 0) a slot is
// as large as the two need; padded, each slot has its own SlotAlign bytes.
template <typename T, std::size_t SlotAlign>
struct alignas(SlotAlign == 0 ? std::max(alignof(std::atomic<std::uint64_t>), alignof(T)) : SlotAlign) mpmc_slot {
    std::atomic<std::uint64_t> seq;
    T data;
};

}  // namespace detail

// Dmitry Vyukov's bounded MPMC queue (1024cores.net). A ring of `Capacity`
// slots (a power of two) holding trivially copyable `T`, each with a sequence
// number. Two 64-bit positions that never wrap in practice: `enqueue_pos`,
// claimed by producers with a CAS, and `dequeue_pos`, claimed by consumers.
//
// For position p, slot p & (Capacity - 1) is free for this lap when its
// sequence is p, and holds data when its sequence is p + 1. A producer claims
// p, writes, and stores sequence p + 1 (release); a consumer claims p, reads,
// and stores p + Capacity, freeing the slot for the next lap. Producers
// contend only with producers, consumers with consumers; a producer and a
// consumer meet only on a slot's sequence.
//
// Each producer's messages arrive in its order, and each consumer sees each
// producer's messages in increasing order. Not lock-free in the strict sense:
// a producer stalled between its CAS and its sequence store holds its slot,
// and consumers see the queue empty there until it finishes (note 010).
//
// Standard-layout, no pointers, slots inline: the object can later be placed
// in shared memory (S4). The positions and the slots are 128 bytes apart (note
// 006's v3). `SlotAlign` 0 packs the slots; 128 gives each its own pair of
// lines (v2).
//
// The default is the measured choice, v2 (note 010, Review): packed, the
// consumer reading one slot and a producer writing the next share a line
// whenever the queue is nearly empty. v1 stays below, named, with its results.
template <typename T, std::size_t Capacity, std::size_t SlotAlign = 128>
class mpmc_ring {
    static_assert(std::is_trivially_copyable_v<T>, "T must be trivially copyable (S4)");
    static_assert(Capacity >= 2 && (Capacity & (Capacity - 1)) == 0, "Capacity must be a power of two");
    static_assert(SlotAlign == 0 || SlotAlign == 64 || SlotAlign == 128, "slots packed, or one line or a pair each");
    static_assert(std::atomic<std::uint64_t>::is_always_lock_free, "positions must be lock-free to share memory");

public:
    static constexpr std::size_t capacity = Capacity;
    static constexpr std::size_t slot_align = SlotAlign;

    // `start` sets both positions and the slots to match, so tests can begin
    // just below 2^64 (S2).
    explicit mpmc_ring(std::uint64_t start = 0) noexcept : enqueue_pos_(start), dequeue_pos_(start) {
        for (std::uint64_t i = 0; i < Capacity; ++i)
            slots_[(start + i) & mask].seq.store(start + i, std::memory_order_relaxed);
    }
    mpmc_ring(const mpmc_ring&) = delete;
    mpmc_ring& operator=(const mpmc_ring&) = delete;

    // Any producer. False when full; never blocks.
    bool try_push(const T& v) noexcept {
        std::uint64_t pos = enqueue_pos_.load(std::memory_order_relaxed);
        slot* s;
        for (;;) {
            s = &slots_[pos & mask];
            const auto d = static_cast<std::int64_t>(s->seq.load(std::memory_order_acquire) - pos);
            if (d == 0) {
                // Free for this lap: claim it. On failure pos is reloaded.
                if (enqueue_pos_.compare_exchange_weak(pos, pos + 1, std::memory_order_relaxed)) break;
            } else if (d < 0) {
                return false;  // last lap's data not yet taken: full
            } else {
                pos = enqueue_pos_.load(std::memory_order_relaxed);  // another producer took it
            }
        }
        s->data = v;
        s->seq.store(pos + 1, std::memory_order_release);
        return true;
    }

    // Any consumer. False when empty; never blocks.
    bool try_pop(T& out) noexcept {
        std::uint64_t pos = dequeue_pos_.load(std::memory_order_relaxed);
        slot* s;
        for (;;) {
            s = &slots_[pos & mask];
            const auto d = static_cast<std::int64_t>(s->seq.load(std::memory_order_acquire) - (pos + 1));
            if (d == 0) {
                if (dequeue_pos_.compare_exchange_weak(pos, pos + 1, std::memory_order_relaxed)) break;
            } else if (d < 0) {
                return false;  // not yet written this lap: empty
            } else {
                pos = dequeue_pos_.load(std::memory_order_relaxed);  // another consumer took it
            }
        }
        out = s->data;
        s->seq.store(pos + Capacity, std::memory_order_release);
        return true;
    }

private:
    using slot = detail::mpmc_slot<T, SlotAlign>;
    static constexpr std::uint64_t mask = Capacity - 1;

    alignas(128) std::atomic<std::uint64_t> enqueue_pos_;
    alignas(128) std::atomic<std::uint64_t> dequeue_pos_;
    alignas(128) slot slots_[Capacity];
};

// v1: slots packed (note 010).
template <typename T, std::size_t Capacity>
using mpmc_v1 = mpmc_ring<T, Capacity, 0>;

// v2: each slot on its own 128 bytes, against false sharing between neighbours.
// The final queue: the same as mpmc_ring's defaults.
template <typename T, std::size_t Capacity>
using mpmc_v2 = mpmc_ring<T, Capacity, 128>;
static_assert(std::is_same_v<mpmc_ring<int, 8>, mpmc_v2<int, 8>>, "the default ring is v2");

}  // namespace lfipc
