// Tests for include/lfipc/bench/placement.hpp. Design: docs/design/008-shm-transport.md (Review)
#include "test_machine.hpp"

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
    namespace t = lfipc::test;
    const int cpu = t::pick_cpus(1).value().front();
    int far = -1;  // an allowed CPU on another node, if there is one
    for (int c : t::allowed_cpus())
        if (cpu_node(c) != cpu_node(cpu)) {
            far = c;
            break;
        }
    std::printf("info  CPU %d on node %d; first CPU on another node: %d (node %d)\n", cpu, cpu_node(cpu), far,
                far >= 0 ? cpu_node(far) : -1);
    check(cpu_node(cpu) >= 0, "a CPU's node is read from sysfs");

    auto here = make_on_cpu<block>(cpu);
    check(here && node_of(here->data()) == cpu_node(cpu), "made on the test CPU: its pages are on that CPU's node");
    if (far < 0) {
        t::skip("pages placed on a far node; a queue there refuses the run", "no allowed CPU on another NUMA node");
    } else {
        auto there = make_on_cpu<block>(far);
        check(there && node_of(there->data()) == cpu_node(far), "made on a far CPU: its pages are on that node");

        load_result r;
        r.ok = true;
        apply_placement(r, there->data(), cpu);
        check(!r.ok && r.reason.find("queue memory on node") != std::string::npos && r.memory_node == cpu_node(far),
              "a queue on the far node refuses the run, naming both nodes: " + r.reason);
    }
    load_result ok;
    ok.ok = true;
    apply_placement(ok, here->data(), cpu);
    check(ok.ok && ok.memory_node == cpu_node(cpu), "a queue on the producer's node is accepted, its node recorded");
    check(!make_on_cpu<block>(-1), "an unpinnable CPU gives nothing");

    std::printf("%s\n", failures ? "FAILED" : "all passed");
    return failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
