// Tests for include/lfipc/shm.hpp. Spec: docs/design/008-shm-transport.md (S2-S4)
#include <lfipc/shm.hpp>

#include <sys/wait.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>

namespace {

int failures = 0;

void check(bool cond, const std::string& what) {
    std::printf("%s  %s\n", cond ? "pass" : "FAIL", what.c_str());
    if (!cond) ++failures;
}

const std::string base = "/lfipc-test-" + std::to_string(::getpid());

struct pair16 {
    std::uint64_t a, b;
};

using q8 = lfipc::shm_spsc<std::uint64_t, 8>;

}  // namespace

int main() {
    std::string err;
    const std::string name = base + "-a";

    // Create, attach (a second mapping of the same memory), use, clean up.
    {
        auto c = q8::create(name, 42, err);
        const bool created = c && c->is_creator();
        check(created, "create: " + (created ? std::string("ok") : err));
        auto a = q8::attach(name, 42, err);
        const bool attached = a && !a->is_creator();
        check(attached, "attach: " + (attached ? std::string("ok") : err));
        bool ok = a && c && &a->ring() != &c->ring();  // two mappings, two addresses
        for (std::uint64_t i = 0; i < 8; ++i) ok = ok && c->ring().try_push(i);
        std::uint64_t v = 0;
        for (std::uint64_t i = 0; i < 8; ++i) ok = ok && a->ring().try_pop(v) && v == i;
        check(ok, "a message pushed through one mapping is popped through the other");
        check(c->header().creator_cpu >= 0 && c->header().creator_pid == ::getpid(), "header records creator");
    }
    {
        const bool gone = !q8::attach(name, 42, err, std::chrono::milliseconds(0)) && err.find("No such file") != std::string::npos;
        check(gone, "the creator unlinks the name when destroyed");
    }

    // S2: every layout mismatch refused, naming it.
    {
        auto c = q8::create(name, 42, err);
        // Each condition is evaluated before its message is built: argument
        // order is unspecified, and the message reads `err`.
        const auto refused = [&](bool attached, const char* want, const char* what) {
            const bool ok = !attached && err.find(want) != std::string::npos;
            check(ok, std::string(what) + ": " + err);
        };
        refused(lfipc::shm_spsc<std::uint64_t, 16>::attach(name, 42, err) != nullptr, "capacity 8 (expected 16)",
                "S2 capacity mismatch");
        refused(q8::attach(name, 43, err) != nullptr, "type_id 42 (expected 43)", "S2 type tag mismatch");
        refused(lfipc::shm_spsc<pair16, 8>::attach(name, 42, err) != nullptr, "type_size 8 (expected 16)",
                "S2 type size mismatch");
        refused(lfipc::shm_spsc<std::uint64_t, 8, 64>::attach(name, 42, err) != nullptr,
                "separation 128 (expected 64)", "S2 separation mismatch");
        c->header().version = 99;
        refused(q8::attach(name, 42, err) != nullptr, "version 99 (expected 1)", "S2 version mismatch");
        c->header().version = lfipc::shm_version;

        // S2: attaching before ready waits, then succeeds; without ready, times out.
        c->header().state.store(lfipc::shm_initialising);
        std::thread later([&] {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            c->header().state.store(lfipc::shm_ready, std::memory_order_release);
        });
        const auto t0 = std::chrono::steady_clock::now();
        auto a = q8::attach(name, 42, err);
        const auto waited = std::chrono::steady_clock::now() - t0;
        later.join();
        check(a && waited >= std::chrono::milliseconds(40), "S2 attach before ready waits, then succeeds");
        c->header().state.store(lfipc::shm_initialising);
        refused(q8::attach(name, 42, err, std::chrono::milliseconds(20)) != nullptr, "not ready",
                "S2 never ready: attach times out with the reason");
        c->header().state.store(lfipc::shm_ready);

        // S3: a live segment is never replaced.
        refused(q8::create(name, 42, err) != nullptr, "is running", "S3 creating over a live segment fails");
    }

    // S3: a segment whose creator died without cleaning up is replaced.
    {
        const pid_t child = ::fork();
        if (child == 0) {
            std::string e;
            auto c = q8::create(name, 42, e);
            std::_Exit(c ? 0 : 1);  // no destructor: the name is left behind
        }
        int status = 0;
        ::waitpid(child, &status, 0);
        check(WIFEXITED(status) && WEXITSTATUS(status) == 0, "child created a segment and died without unlinking");
        auto again = q8::create(name, 42, err);
        const bool replaced = again != nullptr;
        check(replaced, "S3 stale segment (creator gone) replaced: " + (replaced ? std::string("ok") : err));
    }

    // S4: a killed attacher is reported gone.
    {
        auto c = q8::create(name + "-live", 42, err);
        const pid_t child = ::fork();
        if (child == 0) {
            std::string e;
            auto a = q8::attach(name + "-live", 42, e);
            if (!a) std::_Exit(1);
            ::pause();  // until killed
            std::_Exit(0);
        }
        while (c->header().attacher_pid.load() != child) std::this_thread::sleep_for(std::chrono::milliseconds(1));
        check(c->peer_alive(), "S4 a running attacher is alive");
        ::kill(child, SIGKILL);
        ::waitpid(child, nullptr, 0);
        check(!c->peer_alive(), "S4 a killed attacher is reported gone");
    }

    std::printf("%s\n", failures ? "FAILED" : "all passed");
    return failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
