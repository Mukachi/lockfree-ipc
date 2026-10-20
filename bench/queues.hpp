// Every SPSC queue measured in note 006, wrapped in the load generator's
// send/try_receive interface (note 003) and nothing else. Benchmark glue, not
// library code: the baselines are third-party and fetched into build/_deps.
// Design: docs/design/006-spsc-queue.md
#pragma once

#include <lfipc/bench/load.hpp>
#include <lfipc/spsc.hpp>

// The baselines are left out under ThreadSanitizer: moodycamel marks its queue
// no_sanitize("thread"), and Boost.Lockfree relies on standalone fences, which
// TSan does not model (GCC refuses them under -Werror=tsan). TSan can vouch
// for neither, so it checks our rings only.
#if !defined(__SANITIZE_THREAD__)
#define LFIPC_HAVE_BASELINES 1
#include <boost/lockfree/spsc_queue.hpp>
#include <readerwriterqueue.h>
#endif

#include <cstddef>

namespace lfipc::bench {

// The capacity every queue is given. Ours and Boost's (compile-time capacity)
// hold exactly this; moodycamel rounds its storage up and holds 2,044. At the
// rates measured the consumer keeps up and no queue nears full, so capacity
// does not enter the latency (note 006, Review).
inline constexpr std::size_t queue_capacity = 1024;

template <template <typename, std::size_t> class Ring>
struct ring_sut {
    Ring<message, queue_capacity> q;
    bool send(const message& m) { return q.try_push(m); }
    bool try_receive(message& m) { return q.try_pop(m); }
};

#if LFIPC_HAVE_BASELINES
// moodycamel::ReaderWriterQueue: try_enqueue never allocates; the constructor
// pre-allocates room for queue_capacity elements.
struct moodycamel_sut {
    moodycamel::ReaderWriterQueue<message> q{queue_capacity};
    bool send(const message& m) { return q.try_enqueue(m); }
    bool try_receive(message& m) { return q.try_dequeue(m); }
};

struct boost_sut {
    boost::lockfree::spsc_queue<message, boost::lockfree::capacity<queue_capacity>> q;
    bool send(const message& m) { return q.push(m); }
    bool try_receive(message& m) { return q.pop(m); }
};

static_assert(load_sut<moodycamel_sut> && load_sut<boost_sut>);
#endif

static_assert(load_sut<ring_sut<spsc_v1>> && load_sut<ring_sut<spsc_v2>> && load_sut<ring_sut<spsc_v3>> &&
              load_sut<ring_sut<spsc_v4>>);

}  // namespace lfipc::bench
