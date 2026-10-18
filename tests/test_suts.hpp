// Systems under test for the load generator's tests. Not queues for
// measurement: they exist to give the generator known behaviour.
// Design: docs/design/003-open-loop-load.md
#pragma once

#include <lfipc/bench/clock.hpp>
#include <lfipc/bench/load.hpp>

#include <atomic>
#include <cstdint>

namespace lfipc::test {

using bench::message;
using bench::ticks;

// One slot. send() fails while it is full.
template <typename T>
class mailbox {
public:
    bool put(const T& v) {
        if (full_.load(std::memory_order_acquire)) return false;
        slot_ = v;
        full_.store(true, std::memory_order_release);
        return true;
    }
    const T* peek() const { return full_.load(std::memory_order_acquire) ? &slot_ : nullptr; }
    bool take(T& out) {
        if (!full_.load(std::memory_order_acquire)) return false;
        out = slot_;
        full_.store(false, std::memory_order_release);
        return true;
    }

private:
    alignas(64) std::atomic<bool> full_{false};
    alignas(64) T slot_{};
};

struct plain {
    mailbox<message> box;
    bool send(const message& m) { return box.put(m); }
    bool try_receive(message& m) { return box.take(m); }
};

// Each message becomes visible only `delay` ticks after it was sent.
struct fixed_delay {
    struct item {
        message m;
        ticks sent;
    };
    explicit fixed_delay(ticks d) : delay(d) {}
    ticks delay;
    mailbox<item> box;
    bool send(const message& m) { return box.put({m, bench::start()}); }
    bool try_receive(message& m) {
        const item* p = box.peek();
        if (!p || bench::start() < p->sent + delay) return false;
        item it{};
        box.take(it);
        m = it.m;
        return true;
    }
};

// The receiving side stops for `length` ticks every `every` ticks.
struct stalling {
    stalling(ticks every_, ticks length_) : every(every_), length(length_) {}
    ticks every, length, next = 0;
    mailbox<message> box;
    bool send(const message& m) { return box.put(m); }
    bool try_receive(message& m) {
        const ticks now = bench::start();
        if (next == 0) next = now + every;
        if (now >= next) {
            while (bench::start() < next + length) {
            }
            next += every;
        }
        return box.take(m);
    }
};

// The sending side blocks for `length` ticks once, on message `at`. A
// measurement from the actual send time would hide this (S1).
struct send_stall {
    send_stall(std::uint64_t at_, ticks length_) : at(at_), length(length_) {}
    std::uint64_t at;
    ticks length;
    mailbox<message> box;
    bool send(const message& m) {
        if (m.seq == at) {
            const ticks until = bench::start() + length;
            while (bench::start() < until) {
            }
            at = ~std::uint64_t{0};
        }
        return box.put(m);
    }
    bool try_receive(message& m) { return box.take(m); }
};

static_assert(bench::load_sut<plain> && bench::load_sut<fixed_delay> && bench::load_sut<stalling> &&
              bench::load_sut<send_stall>);

}  // namespace lfipc::test
