// Where benchmark memory lives. Design: docs/design/008-shm-transport.md (Review)
//
// Pages are placed on the NUMA node of the CPU that first touches them. An
// object constructed by an unpinned main thread lands wherever that thread
// runs; with boot isolation that is a housekeeping CPU, mostly on node 1, so a
// queue between two node-0 CPUs would be homed on the far socket. Benchmarks
// construct shared memory with make_on_cpu, and check it with node_of.
#pragma once

#include <lfipc/bench/cross_cpu.hpp>  // detail::pin_to
#include <lfipc/bench/load.hpp>

#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <filesystem>
#include <memory>
#include <new>
#include <string>
#include <thread>
#include <utility>

namespace lfipc::bench {

// Destroys the object and returns its pages.
template <typename T>
struct unmap_deleter {
    std::size_t bytes = 0;
    void operator()(T* p) const noexcept {
        p->~T();
        ::munmap(p, bytes);
    }
};
template <typename T>
using placed_ptr = std::unique_ptr<T, unmap_deleter<T>>;

// Constructs a T in fresh pages, on a thread pinned to `cpu`, so the pages are
// first touched, and placed, on that CPU's node. Fresh pages matter: `new`
// goes through glibc's per-thread arenas, which pass from one thread to the
// next, so a pinned thread can be handed pages another node's thread touched
// (placement_test showed it). nullptr if the CPU cannot be pinned.
template <typename T, typename... A>
placed_ptr<T> make_on_cpu(int cpu, A&&... args) {
    static_assert(alignof(T) <= 4096, "mmap gives page alignment");
    const std::size_t bytes = (sizeof(T) + 4095) / 4096 * 4096;
    placed_ptr<T> out(nullptr, unmap_deleter<T>{bytes});
    std::thread t([&] {
        if (!detail::pin_to(cpu)) return;
        void* mem = ::mmap(nullptr, bytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (mem == MAP_FAILED) return;
        out.reset(new (mem) T(std::forward<A>(args)...));  // the first touch
    });
    t.join();
    return out;
}

// The NUMA node holding the page at `p` (move_pages with no target only asks);
// -1 if unknown.
inline int node_of(const void* p) {
    void* pages[1] = {const_cast<void*>(p)};
    int status[1] = {-1};
    if (::syscall(SYS_move_pages, 0, 1, pages, nullptr, status, 0) != 0) return -1;
    return status[0] >= 0 ? status[0] : -1;
}

// The NUMA node of `cpu`, from sysfs (cpuN/nodeM); -1 if unknown.
inline int cpu_node(int cpu) {
    std::error_code ec;
    const std::string dir = "/sys/devices/system/cpu/cpu" + std::to_string(cpu);
    for (const auto& e : std::filesystem::directory_iterator(dir, ec)) {
        const std::string n = e.path().filename().string();
        if (n.rfind("node", 0) == 0 && n.size() > 4) return std::stoi(n.substr(4));
    }
    return -1;
}

// Binds every page this process allocates from now on, on any thread, and in
// any child it forks, to `node` (MPOL_BIND): `numactl --membind` from inside.
// Benchmarks call it first, so nothing they measure with lands on another node
// by accident of which thread touched it first.
inline bool bind_memory_to_node(int node, std::string& error) {
    if (node < 0 || node >= 64) {
        error = "no NUMA node to bind to";
        return false;
    }
    const unsigned long mask = 1UL << node;
    constexpr int mpol_bind = 2;  // MPOL_BIND, <linux/mempolicy.h>
    if (::syscall(SYS_set_mempolicy, mpol_bind, &mask, 65) != 0) {
        error = "set_mempolicy: " + std::string(std::strerror(errno));
        return false;
    }
    return true;
}

// This process's memory policy, as recorded in the environment block:
// "bind <nodes>", "default", or "mode <n> <nodes>".
inline std::string memory_policy() {
    int mode = -1;
    unsigned long mask = 0;
    if (::syscall(SYS_get_mempolicy, &mode, &mask, 65, nullptr, 0) != 0) return "unknown";
    std::string nodes;
    for (int n = 0; n < 64; ++n)
        if (mask >> n & 1) nodes += (nodes.empty() ? "" : ",") + std::to_string(n);
    if (mode == 0) return "default";
    if (mode == 2) return "bind " + nodes;
    return "mode " + std::to_string(mode) + " " + nodes;
}

// Records where a run's queue lived, and refuses the run if not on the
// producer's node: a far-socket queue measures the interconnect, not the queue.
inline void apply_placement(load_result& r, const void* queue, int producer_cpu) {
    r.memory_node = node_of(queue);
    const int want = cpu_node(producer_cpu);
    if (r.memory_node != want) {
        r.ok = false;
        r.reason = "queue memory on node " + std::to_string(r.memory_node) + ", producer CPU " +
                   std::to_string(producer_cpu) + " on node " + std::to_string(want);
    }
}

}  // namespace lfipc::bench
