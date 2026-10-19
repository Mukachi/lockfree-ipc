// Note 006's SPSC ring in a named shared-memory segment, between two processes.
// Design: docs/design/008-shm-transport.md
#pragma once

#include <lfipc/spsc.hpp>

#include <fcntl.h>
#include <sched.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <new>
#include <string>
#include <thread>

namespace lfipc {

inline constexpr std::uint64_t shm_magic = 0x6C6669706373686DULL;  // "lfipcshm"
inline constexpr std::uint32_t shm_version = 1;

enum shm_state : std::uint32_t { shm_initialising = 1, shm_ready = 2 };

// At the start of every segment. The creator writes every field, constructs
// the ring, then stores `state = ready` with release; an attacher reads them
// only after loading `ready` with acquire.
struct shm_header {
    std::uint64_t magic;
    std::uint32_t version;
    std::atomic<std::uint32_t> state;
    std::uint64_t type_size, type_align, capacity, separation, type_id;
    std::int32_t creator_pid;
    std::atomic<std::int32_t> attacher_pid;
    std::int32_t creator_cpu;
};
static_assert(std::is_standard_layout_v<shm_header>);
static_assert(std::atomic<std::uint32_t>::is_always_lock_free && std::atomic<std::int32_t>::is_always_lock_free);

// True if `pid` names a running process (EPERM: it exists, not ours to signal).
inline bool pid_alive(std::int32_t pid) {
    if (pid <= 0) return false;
    return ::kill(pid, 0) == 0 || errno == EPERM;
}

// One spsc_ring<T, N, Separation, Cached> in a named segment under /dev/shm.
// `type_id` is a caller-chosen tag for T: two programs agreeing on the size of
// T but not its meaning are refused (S2).
template <typename T, std::size_t N, std::size_t Separation = 128, bool Cached = true>
class shm_spsc {
public:
    using ring_type = spsc_ring<T, N, Separation, Cached>;
    static constexpr std::size_t ring_offset =
        (sizeof(shm_header) + alignof(ring_type) - 1) / alignof(ring_type) * alignof(ring_type);
    static constexpr std::size_t total_size = ring_offset + sizeof(ring_type);

    // Creates `name` (e.g. "/feed-to-strategy") and constructs the ring. If the
    // name exists: replaced only if it is an lfipc segment whose creator has
    // gone (S3); otherwise nullptr and `error` says why. Never removes a live
    // segment, or one still being initialised.
    static std::unique_ptr<shm_spsc> create(const std::string& name, std::uint64_t type_id, std::string& error) {
        for (int attempt = 0; attempt < 2; ++attempt) {
            const int fd = ::shm_open(name.c_str(), O_CREAT | O_EXCL | O_RDWR, 0600);
            if (fd >= 0) return construct(name, type_id, fd, error);
            if (errno != EEXIST) return fail(error, name + ": shm_open: " + std::strerror(errno));
            if (attempt == 1 || !remove_if_stale(name, error)) return nullptr;
        }
        return nullptr;
    }

    // Attaches to `name`, waiting up to `wait` for its creator to finish
    // initialising, then validates the header against this T, N and layout (S2).
    static std::unique_ptr<shm_spsc> attach(const std::string& name, std::uint64_t type_id, std::string& error,
                                            std::chrono::milliseconds wait = std::chrono::milliseconds(1000)) {
        const int fd = ::shm_open(name.c_str(), O_RDWR, 0);
        if (fd < 0) return fail(error, name + ": shm_open: " + std::strerror(errno));
        const auto deadline = std::chrono::steady_clock::now() + wait;
        struct stat st {};
        // Size first (the creator truncates just after creating), then ready.
        while (::fstat(fd, &st) == 0 && static_cast<std::size_t>(st.st_size) < sizeof(shm_header)) {
            if (std::chrono::steady_clock::now() > deadline) {
                ::close(fd);
                return fail(error, name + ": not initialised within the wait");
            }
            std::this_thread::sleep_for(std::chrono::microseconds(100));
        }
        void* base = ::mmap(nullptr, static_cast<std::size_t>(st.st_size), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        ::close(fd);
        if (base == MAP_FAILED) return fail(error, name + ": mmap: " + std::strerror(errno));
        std::unique_ptr<shm_spsc> s(new shm_spsc(name, static_cast<std::byte*>(base),
                                                 static_cast<std::size_t>(st.st_size), false));
        shm_header& h = s->header();
        while (h.state.load(std::memory_order_acquire) != shm_ready) {
            if (std::chrono::steady_clock::now() > deadline) return fail(error, name + ": not ready within the wait");
            std::this_thread::sleep_for(std::chrono::microseconds(100));
        }
        std::string mismatch;
        const auto expect = [&](const char* field, std::uint64_t got, std::uint64_t want) {
            if (got != want)
                mismatch += std::string(mismatch.empty() ? "" : ", ") + field + " " + std::to_string(got) +
                            " (expected " + std::to_string(want) + ")";
        };
        if (h.magic != shm_magic) return fail(error, name + ": not an lfipc segment");
        expect("version", h.version, shm_version);
        expect("type_size", h.type_size, sizeof(T));
        expect("type_align", h.type_align, alignof(T));
        expect("capacity", h.capacity, N);
        expect("separation", h.separation, Separation);
        expect("type_id", h.type_id, type_id);
        expect("segment size", s->size_, total_size);
        if (!mismatch.empty()) return fail(error, name + ": layout mismatch: " + mismatch);
        h.attacher_pid.store(static_cast<std::int32_t>(::getpid()), std::memory_order_release);
        return s;
    }

    shm_spsc(const shm_spsc&) = delete;
    shm_spsc& operator=(const shm_spsc&) = delete;

    ~shm_spsc() {
        ::munmap(base_, size_);
        if (creator_) ::shm_unlink(name_.c_str());
    }

    // The ring, through std::launder: it was constructed by the creator, maybe
    // in another process (note 008, Lifetime).
    ring_type& ring() noexcept { return *std::launder(reinterpret_cast<ring_type*>(base_ + ring_offset)); }
    shm_header& header() noexcept { return *std::launder(reinterpret_cast<shm_header*>(base_)); }

    // Whether the other side's process is running (S4). Not for the hot path:
    // it is a system call. The creator's peer is the last attacher.
    bool peer_alive() {
        const auto& h = header();
        return pid_alive(creator_ ? h.attacher_pid.load(std::memory_order_acquire) : h.creator_pid);
    }

    bool is_creator() const noexcept { return creator_; }

private:
    shm_spsc(std::string name, std::byte* base, std::size_t size, bool creator)
        : name_(std::move(name)), base_(base), size_(size), creator_(creator) {}

    static std::unique_ptr<shm_spsc> fail(std::string& error, std::string why) {
        error = std::move(why);
        return nullptr;
    }

    static std::unique_ptr<shm_spsc> construct(const std::string& name, std::uint64_t type_id, int fd,
                                               std::string& error) {
        if (::ftruncate(fd, static_cast<off_t>(total_size)) != 0) {
            const int e = errno;
            ::close(fd);
            ::shm_unlink(name.c_str());
            return fail(error, name + ": ftruncate: " + std::strerror(e));
        }
        void* base = ::mmap(nullptr, total_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        ::close(fd);
        if (base == MAP_FAILED) {
            const int e = errno;
            ::shm_unlink(name.c_str());
            return fail(error, name + ": mmap: " + std::strerror(e));
        }
        auto* b = static_cast<std::byte*>(base);
        // Placement new, header then ring: these writes are the first touch,
        // so the pages land on the creator's NUMA node (note 008, NUMA).
        auto* h = new (b) shm_header{};
        h->magic = shm_magic;
        h->version = shm_version;
        h->state.store(shm_initialising, std::memory_order_relaxed);
        h->type_size = sizeof(T);
        h->type_align = alignof(T);
        h->capacity = N;
        h->separation = Separation;
        h->type_id = type_id;
        h->creator_pid = static_cast<std::int32_t>(::getpid());
        h->attacher_pid.store(0, std::memory_order_relaxed);
        h->creator_cpu = ::sched_getcpu();
        new (b + ring_offset) ring_type();
        h->state.store(shm_ready, std::memory_order_release);
        return std::unique_ptr<shm_spsc>(new shm_spsc(name, b, total_size, true));
    }

    // Removes `name` only if it is an lfipc segment whose creator is gone.
    static bool remove_if_stale(const std::string& name, std::string& error) {
        const int fd = ::shm_open(name.c_str(), O_RDONLY, 0);
        if (fd < 0) {
            if (errno == ENOENT) return true;  // gone meanwhile: try again
            error = name + ": exists, and cannot be opened: " + std::strerror(errno);
            return false;
        }
        struct stat st {};
        const bool sized = ::fstat(fd, &st) == 0 && static_cast<std::size_t>(st.st_size) >= sizeof(shm_header);
        void* base = sized ? ::mmap(nullptr, sizeof(shm_header), PROT_READ, MAP_SHARED, fd, 0) : MAP_FAILED;
        ::close(fd);
        if (base == MAP_FAILED) {
            error = name + ": exists and is being initialised, or is not an lfipc segment";
            return false;
        }
        const auto* h = std::launder(reinterpret_cast<const shm_header*>(base));
        const bool ours = h->magic == shm_magic;
        const std::int32_t pid = h->creator_pid;
        ::munmap(base, sizeof(shm_header));
        if (!ours) {
            error = name + ": exists and is not an lfipc segment";
            return false;
        }
        if (pid_alive(pid)) {
            error = name + ": exists and its creator (pid " + std::to_string(pid) + ") is running";
            return false;
        }
        ::shm_unlink(name.c_str());  // stale: its creator is gone
        return true;
    }

    std::string name_;
    std::byte* base_;
    std::size_t size_;
    bool creator_;
};

}  // namespace lfipc
