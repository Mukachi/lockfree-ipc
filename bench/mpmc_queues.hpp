// Every MPMC queue measured in note 010, wrapped in the load generator's
// send/try_receive interface (note 003) and nothing else. Benchmark glue, not
// library code: the baselines are third-party and fetched into build/_deps.
// Design: docs/design/010-mpmc-queue.md
#pragma once

#include "queues.hpp"  // queue_capacity, ring_sut, LFIPC_HAVE_BASELINES

#include <lfipc/bench/load.hpp>
#include <lfipc/bench/load_mp.hpp>  // producer_shift, index_mask
#include <lfipc/mpmc.hpp>

#if LFIPC_HAVE_BASELINES
#include <boost/lockfree/queue.hpp>
#include <concurrentqueue.h>
#include <rigtorp/MPMCQueue.h>
#endif

#include <cstddef>

namespace lfipc::bench {

static_assert(load_sut<ring_sut<mpmc_v1>> && load_sut<ring_sut<mpmc_v2>>);

#if LFIPC_HAVE_BASELINES
// The most producers any shape uses (note 010: 4 -> 1, 4 -> 4), with room.
inline constexpr std::size_t mpmc_max_producers = 8;

// moodycamel::ConcurrentQueue through its token-free API: each producer
// thread gets an implicit sub-queue. Its README says explicit producer tokens
// are "almost always faster", but they are per-thread state this interface
// does not carry; the figures may understate it slightly (note 010). Storage
// is preallocated for the producers; try_enqueue never allocates.
struct moodycamel_mpmc_sut {
    moodycamel::ConcurrentQueue<message> q{queue_capacity, 0, mpmc_max_producers};
    bool send(const message& m) { return q.try_enqueue(m); }
    bool try_receive(message& m) { return q.try_dequeue(m); }
};

// Boost.Lockfree's queue on a fixed pool of queue_capacity nodes.
struct boost_mpmc_sut {
    boost::lockfree::queue<message, boost::lockfree::capacity<queue_capacity>> q;
    bool send(const message& m) { return q.bounded_push(m); }
    bool try_receive(message& m) { return q.pop(m); }
};

// rigtorp::MPMCQueue: the same Vyukov design, slots padded to a cache line,
// capacity set at run time.
struct rigtorp_sut {
    rigtorp::MPMCQueue<message> q{queue_capacity};
    bool send(const message& m) { return q.try_push(m); }
    bool try_receive(message& m) { return q.try_pop(m); }
};

static_assert(load_sut<moodycamel_mpmc_sut> && load_sut<boost_mpmc_sut> && load_sut<rigtorp_sut>);
#endif

}  // namespace lfipc::bench
