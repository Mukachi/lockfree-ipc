// Tests for include/lfipc/bench/placement.hpp. Design: docs/design/008-shm-transport.md (Review)
#include <lfipc/bench/placement.hpp>

#include <array>
#include <cstdio>
#include <cstdlib>
#include <string>

namespace {

int failures = 0;

void check(bool cond, const std::string& what) {
    std::printf("%s  %s\n", cond ? "pass" : "FAIL", what.c_str());
    if (!cond) ++failures;
}

using namespace lfipc::bench;
using block = std::array<std::uint64_t, 1024>;  // 8 KB: two pages, first touched on construction

}  // namespace

int main() {
    const auto cpus = online_cpus();
    int far = -1;  // a CPU on another node than CPU 2, if there is one
    for (int c : cpus)
        if (cpu_node(c) != cpu_node(2)) {
            far = c;
            break;
        }
    std::printf("info  CPU 2 on node %d; first CPU on another node: %d (node %d)\n", cpu_node(2), far,
                far >= 0 ? cpu_node(far) : -1);
    check(cpu_node(2) >= 0, "a CPU's node is read from sysfs");

    auto here = make_on_cpu<block>(2);
    check(here && node_of(here->data()) == cpu_node(2), "made on CPU 2: its pages are on CPU 2's node");
    if (far >= 0) {
        auto there = make_on_cpu<block>(far);
        check(there && node_of(there->data()) == cpu_node(far), "made on a far CPU: its pages are on that node");

        load_result r;
        r.ok = true;
        apply_placement(r, there->data(), 2);
        check(!r.ok && r.reason.find("queue memory on node") != std::string::npos && r.memory_node == cpu_node(far),
              "a queue on the far node refuses the run, naming both nodes: " + r.reason);
    }
    load_result ok;
    ok.ok = true;
    apply_placement(ok, here->data(), 2);
    check(ok.ok && ok.memory_node == cpu_node(2), "a queue on the producer's node is accepted, its node recorded");
    check(!make_on_cpu<block>(-1), "an unpinnable CPU gives nothing");

    std::printf("%s\n", failures ? "FAILED" : "all passed");
    return failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
