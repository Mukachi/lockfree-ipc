// Tests for include/lfipc/bench/environment.hpp. Spec: docs/design/004-environment.md
//
// Builds a fake /proc and /sys tree for a machine in bench mode, checks every
// field is read, then breaks each bench-mode condition in turn (S3, S5).
#include <lfipc/bench/environment.hpp>

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

namespace {

int failures = 0;

void check(bool cond, const char* what) {
    std::printf("%s  %s\n", cond ? "pass" : "FAIL", what);
    if (!cond) ++failures;
}

namespace fs = std::filesystem;
using namespace lfipc::bench;

const std::string root = fs::absolute("environment_test_root").string();

void put(const std::string& path, const std::string& content) {
    fs::create_directories(fs::path(root + path).parent_path());
    std::ofstream(root + path) << content << '\n';
}

// A two-socket machine, 8 CPUs, housekeeping 0-1 and 4-5, reserved 2-3 and 6-7, in bench mode.
void good_tree() {
    fs::remove_all(root);
    put("/sys/class/dmi/id/sys_vendor", "HP");
    put("/sys/class/dmi/id/product_name", "ProLiant DL380 Gen9");
    put("/sys/class/dmi/id/bios_version", "P89");
    put("/sys/class/dmi/id/bios_date", "08/29/2024");
    std::string ci;
    for (int c = 0; c < 8; ++c)
        ci += "processor\t: " + std::to_string(c) + "\nmodel name\t: Test CPU @ 2.20GHz\nmicrocode\t: 0xb000041\n"
              "physical id\t: " + std::to_string(c / 4) + "\nsiblings\t: 4\ncpu cores\t: 2\n\n";
    put("/proc/cpuinfo", ci);
    put("/sys/devices/system/cpu/online", "0-7");
    put("/sys/devices/system/node/node0/cpulist", "0-3");
    put("/sys/devices/system/node/node0/meminfo", "Node 0 MemTotal:       2097152 kB");
    put("/sys/devices/system/node/node1/cpulist", "4-7");
    put("/sys/devices/system/node/node1/meminfo", "Node 1 MemTotal:       1048576 kB");
    put("/sys/devices/system/node/possible", "0-1");  // not a node directory
    put("/proc/sys/kernel/osrelease", "7.0.0-38-generic");
    put("/proc/sys/kernel/version", "#38-Ubuntu SMP");
    put("/proc/cmdline", "ro quiet");
    put("/sys/kernel/mm/transparent_hugepage/enabled", "always [madvise] never");
    put("/proc/sys/kernel/numa_balancing", "0");
    put("/sys/devices/system/cpu/vulnerabilities/spectre_v2", "Mitigation: Retpolines");
    put("/sys/devices/system/cpu/vulnerabilities/meltdown", "Not affected");
    put("/sys/fs/cgroup/system.slice/cpuset.cpus", "0-1,4-5");
    put("/sys/fs/cgroup/user.slice/cpuset.cpus", "0-1,4-5");
    put("/proc/self/cgroup", "0::/bench.slice/run-u1.scope");
    put("/proc/self/status", "Name:\tx\nCpus_allowed_list:\t0-7\nMems_allowed_list:\t0-1");
    for (int c = 0; c < 8; ++c) {
        const std::string b = "/sys/devices/system/cpu/cpu" + std::to_string(c);
        put(b + "/cpufreq/scaling_governor", "performance");
        const char* names[] = {"POLL", "C1", "C1E", "C3", "C6"};
        for (int s = 0; s < 5; ++s) {
            put(b + "/cpuidle/state" + std::to_string(s) + "/name", names[s]);
            put(b + "/cpuidle/state" + std::to_string(s) + "/disable", "0");
        }
    }
    put("/sys/devices/system/cpu/intel_pstate/no_turbo", "0");
    for (int p = 0; p < 2; ++p) {
        const std::string u = "/sys/devices/system/cpu/intel_uncore_frequency/package_0" + std::to_string(p) + "_die_0" +
                              std::to_string(p);
        put(u + "/min_freq_khz", "2800000");
        put(u + "/max_freq_khz", "2800000");
    }
}

std::vector<std::string> failures_now() {
    auto e = capture_environment(env_reader{root});
    e.clean = true;  // the build flag is compile-time; tested separately
    return bench_mode_failures(e);
}

bool only(const std::vector<std::string>& f, const std::string& needle) {
    return f.size() == 1 && f[0].find(needle) != std::string::npos;
}

}  // namespace

int main(int argc, char** argv) {
    good_tree();
    const auto e = capture_environment(env_reader{root}, argc, argv);

    // S2, S5: every field read from the tree.
    check(e.vendor == "HP" && e.product == "ProLiant DL380 Gen9" && e.bios == "P89" && e.bios_date == "08/29/2024",
          "machine fields");
    check(e.model == "Test CPU @ 2.20GHz" && e.microcode == "0xb000041" && e.sockets == 2 && e.cores_per_socket == 2 &&
              e.threads_per_core == 2 && e.online == "0-7",
          "cpu fields, sockets counted from physical ids");
    check(e.numa.size() == 2 && e.numa[0].cpus == "0-3" && e.numa[0].memory_mb == 2048 && e.numa[1].memory_mb == 1024,
          "numa nodes, memory in MB, non-node entries skipped");
    check(e.release == "7.0.0-38-generic" && e.thp == "madvise" && e.numa_balancing == "0" && e.vulnerabilities.size() == 2 &&
              e.vulnerabilities.at("spectre_v2") == "Mitigation: Retpolines",
          "kernel fields, THP selection, vulnerabilities");
    check(e.governor == "performance" && e.turbo == true &&
              e.idle_states_enabled == std::vector<std::string>{"C1", "C1E", "C3", "C6", "POLL"} &&
              e.idle_states_disabled.empty() && e.uncore.size() == 2 &&
              e.uncore[1].socket == 1 && e.uncore[1].min_khz == 2800000,
          "power fields over the reserved CPUs");
    check(e.fence_user == "0-1,4-5" && e.reserved == std::vector<int>{2, 3, 6, 7} &&
              e.cgroup == "/bench.slice/run-u1.scope" && e.affinity == "0-7",
          "placement: fence, reserved CPUs derived, cgroup, affinity");
    check(!e.program.empty() && e.start_utc.size() == 20 && e.start_utc.back() == 'Z', "run fields");

    // S3: in bench mode, nothing fails; break each condition in turn.
    check(failures_now().empty(), "S3 bench-mode tree passes every condition");
    put("/sys/devices/system/cpu/cpu3/cpufreq/scaling_governor", "schedutil");
    check(only(failures_now(), "governor is 'mixed: performance schedutil'"), "S3 one reserved CPU's governor wrong");
    good_tree();
    put("/sys/devices/system/cpu/cpu6/cpuidle/state4/disable", "1");
    check(only(failures_now(), "idle state C6 disabled"), "S3 a deep idle state disabled on one reserved CPU");
    good_tree();
    put("/sys/devices/system/cpu/cpu1/cpuidle/state4/disable", "1");
    check(failures_now().empty(), "idle states on a housekeeping CPU are not checked");
    good_tree();
    put("/sys/devices/system/cpu/intel_uncore_frequency/package_01_die_01/min_freq_khz", "1200000");
    check(only(failures_now(), "uncore on socket 1 not pinned"), "S3 uncore not pinned on one socket");
    good_tree();
    fs::remove(root + "/sys/fs/cgroup/user.slice/cpuset.cpus");
    {
        const auto f = failures_now();
        check(!f.empty() && f[0].find("fence not in place") != std::string::npos, "S3 no fence");
    }
    good_tree();
    put("/proc/sys/kernel/numa_balancing", "1");
    check(only(failures_now(), "automatic NUMA balancing is '1'"), "S3 NUMA balancing on");
    good_tree();
    fs::remove(root + "/proc/sys/kernel/numa_balancing");
    check(only(failures_now(), "automatic NUMA balancing is 'unknown'"), "S3 NUMA balancing unreadable");
    good_tree();
    put("/proc/self/cgroup", "0::/user.slice/user-1000.slice/session-13.scope");
    check(only(failures_now(), "not started by bench-run"), "S3 not started by bench-run");
    good_tree();
    put("/proc/self/status", "Cpus_allowed_list:\t0-1,4-5");
    check(only(failures_now(), "does not cover the reserved CPUs"), "S3 affinity inside the fence");
    good_tree();
    {
        auto x = capture_environment(env_reader{root});
        x.clean = false;
        check(only(bench_mode_failures(x), "uncommitted changes"), "S3 dirty build");
    }

    // S2: a missing file is null, never guessed.
    fs::remove_all(root);
    const auto empty = capture_environment(env_reader{root});
    check(!empty.vendor && !empty.model && !empty.sockets && empty.numa.empty() && !empty.governor && !empty.turbo &&
              !empty.fence_user && empty.reserved.empty(),
          "S2 unreadable values are null");
    check(!bench_mode_failures(empty).empty(), "an unreadable system fails the bench-mode check");

    // Published means under the results directory.
    fs::create_directories(root + "/results");
    check(is_published(root + "/results/x.json", root + "/results") &&
              !is_published(root + "/results-old/x.json", root + "/results") &&
              !is_published("/tmp/x.json", root + "/results"),
          "published only when written under the results directory");

    // The block: refused only when published and failing.
    {
        good_tree();
        const auto g = capture_environment(env_reader{root});
        json_writer w;
        w.begin_object();
        write_environment_block(w, g, true, {"governor is 'schedutil', not 'performance'"});
        w.end_object();
        check(w.str().find("\"ok\": false") != std::string::npos &&
                  w.str().find("\"reason\": \"not in bench mode: governor") != std::string::npos,
              "S3 published and failing: refused, reason names the failure");
        json_writer v;
        v.begin_object();
        write_environment_block(v, g, false, {"governor is 'schedutil', not 'performance'"});
        v.end_object();
        check(v.str().find("\"published\": false,\n    \"ok\": true") != std::string::npos,
              "unpublished and failing: not refused, failures still listed");
        std::ofstream("environment.json") << v.str() << '\n';
    }
    fs::remove_all(root);

    std::printf("%s\n", failures ? "FAILED" : "all passed");
    return failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
