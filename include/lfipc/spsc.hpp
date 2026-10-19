// Bounded single-producer, single-consumer queue. Design: docs/design/006-spsc-queue.md
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace lfipc {

// A ring of `Capacity` slots (a power of two) holding trivially copyable `T`.
// `tail` is the next slot to write and is written only by the producer;
// `head` is the next slot to read and is written only by the consumer. Both
// are 64-bit counters that never wrap in practice; a slot is `counter &
// (Capacity - 1)`, and the ring is full when tail - head == Capacity, so no
// slot is given up to tell full from empty.
//
// The state is standard-layout with no pointers and the slots inline, so the
// whole object can later be placed in shared memory (S4). `Separation` is the
// distance kept between the producer's counter, the consumer's counter and
// the slots: 64 bytes (one cache line) or 128 (an adjacent-line pair).
template <typename T, std::size_t Capacity, std::size_t Separation = 64>
class spsc_ring {
    static_assert(std::is_trivially_copyable_v<T>, "T must be trivially copyable (S4)");
    static_assert(Capacity >= 2 && (Capacity & (Capacity - 1)) == 0, "Capacity must be a power of two");
    static_assert(Separation == 64 || Separation == 128, "Separation is one line or an adjacent pair");
    static_assert(std::atomic<std::uint64_t>::is_always_lock_free, "counters must be lock-free to share memory");

public:
    static constexpr std::size_t capacity = Capacity;
    static constexpr std::size_t separation = Separation;

    // `start` sets both counters, so tests can begin just below 2^64 (S2).
    explicit spsc_ring(std::uint64_t start = 0) noexcept : tail_(start), head_(start) {}
    spsc_ring(const spsc_ring&) = delete;
    spsc_ring& operator=(const spsc_ring&) = delete;

    // Producer only. False when full; never blocks.
    bool try_push(const T& v) noexcept {
        const std::uint64_t t = tail_.load(std::memory_order_relaxed);  // only we write it
        if (t - head_.load(std::memory_order_acquire) == Capacity) return false;
        slots_[t & mask] = v;
        tail_.store(t + 1, std::memory_order_release);
        return true;
    }

    // Consumer only. False when empty; never blocks.
    bool try_pop(T& out) noexcept {
        const std::uint64_t h = head_.load(std::memory_order_relaxed);  // only we write it
        if (h == tail_.load(std::memory_order_acquire)) return false;
        out = slots_[h & mask];
        head_.store(h + 1, std::memory_order_release);
        return true;
    }

private:
    static constexpr std::uint64_t mask = Capacity - 1;

    alignas(Separation) std::atomic<std::uint64_t> tail_;
    alignas(Separation) std::atomic<std::uint64_t> head_;
    alignas(Separation) T slots_[Capacity];
};

// v1: Lamport's ring, counters on separate 64-byte lines (note 006).
template <typename T, std::size_t Capacity>
using spsc_v1 = spsc_ring<T, Capacity, 64>;

}  // namespace lfipc
