// Environment capture and the bench-mode check. Design: docs/design/004-environment.md
#pragma once

#include <lfipc/bench/cross_cpu.hpp>  // parse_cpu_list
#include <lfipc/bench/json.hpp>
#include <lfipc/build_info.hpp>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace lfipc::bench {

using opt_str = std::optional<std::string>;

// Reads files under a root prefix: "" for the real system, a directory of
// fake /proc and /sys files in tests (S5).
struct env_reader {
    std::string root;

    // Whole file, trailing whitespace trimmed; empty optional if unreadable.
    opt_str read(const std::string& path) const {
        std::ifstream f(root + path);
        if (!f) return std::nullopt;
        std::stringstream ss;
        ss << f.rdbuf();
        std::string s = ss.str();
        while (!s.empty() && (s.back() == '\n' || s.back() == ' ' || s.back() == '\t')) s.pop_back();
        return s;
    }
    std::optional<long long> read_int(const std::string& path) const {
        const auto s = read(path);
        if (!s || s->empty()) return std::nullopt;
        try {
            return std::stoll(*s);
        } catch (...) {
            return std::nullopt;
        }
    }
    // Names in a directory, sorted; empty if it does not exist.
    std::vector<std::string> list(const std::string& dir) const {
        std::vector<std::string> out;
        std::error_code ec;
        for (const auto& e : std::filesystem::directory_iterator(root + dir, ec)) out.push_back(e.path().filename().string());
        std::sort(out.begin(), out.end());
        return out;
    }
};

struct numa_node {
    int node = -1;
    opt_str cpus;
    std::optional<long long> memory_mb;
};

struct uncore_socket {
    int socket = -1;
    std::optional<long long> min_khz, max_khz;
};

struct environment {
    // Machine
    opt_str vendor, product, bios, bios_date;
    // CPU
    opt_str model, microcode, online;
    std::optional<long long> sockets, cores_per_socket, threads_per_core;
    // NUMA
    std::vector<numa_node> numa;
    // Kernel
    opt_str release, version, cmdline, thp, numa_balancing, perf_event_paranoid;
    std::map<std::string, std::string> vulnerabilities;
    // Placement: fence is the housekeeping set; reserved is online minus fence.
    opt_str fence_system, fence_user, cgroup, affinity;
    opt_str memory_policy;  // set by the benchmark program (run_file), not read from files
    std::vector<int> reserved;
    // Power, over the reserved CPUs
    opt_str governor;  // the common value, or "mixed: ..." if they differ
    std::optional<bool> turbo;
    std::vector<std::string> idle_states_enabled;  // union over reserved CPUs
    // Union over reserved CPUs of the states disabled: checked (condition 2),
    // not written; bench_mode.ok certifies it (note 004, Review 2026-10-22).
    std::vector<std::string> idle_states_disabled;
    std::vector<uncore_socket> uncore;
    // Build (embedded) and run
    std::string commit = LFIPC_GIT_COMMIT;
    bool clean = LFIPC_GIT_CLEAN;
    std::string compiler = std::string("gcc ") + __VERSION__;
    std::string build_type = LFIPC_BUILD_TYPE;
    std::string flags = LFIPC_BUILD_FLAGS;
    std::string program, start_utc;
    std::vector<std::string> args;
};

namespace detail {

// First value of a "key : value" line in /proc/cpuinfo.
inline opt_str cpuinfo_field(const std::string& text, const std::string& key) {
    std::istringstream in(text);
    for (std::string line; std::getline(in, line);) {
        const auto colon = line.find(':');
        if (colon == std::string::npos) continue;
        std::string k = line.substr(0, colon);
        while (!k.empty() && (k.back() == ' ' || k.back() == '\t')) k.pop_back();
        if (k == key) {
            const auto v = line.find_first_not_of(" \t", colon + 1);
            return v == std::string::npos ? std::string() : line.substr(v);
        }
    }
    return std::nullopt;
}

inline std::optional<long long> to_int(const opt_str& s) {
    if (!s) return std::nullopt;
    try {
        return std::stoll(*s);
    } catch (...) {
        return std::nullopt;
    }
}

// "always [madvise] never" -> "madvise".
inline opt_str bracketed(const opt_str& s) {
    if (!s) return std::nullopt;
    const auto a = s->find('['), b = s->find(']');
    if (a == std::string::npos || b == std::string::npos || b < a) return s;
    return s->substr(a + 1, b - a - 1);
}

// Prints CPUs as the kernel does: "0-1,22-45".
inline std::string cpu_list_string(const std::vector<int>& cpus) {
    std::string out;
    for (std::size_t i = 0; i < cpus.size();) {
        std::size_t j = i;
        while (j + 1 < cpus.size() && cpus[j + 1] == cpus[j] + 1) ++j;
        if (!out.empty()) out += ',';
        out += std::to_string(cpus[i]);
        if (j > i) out += '-' + std::to_string(cpus[j]);
        i = j + 1;
    }
    return out;
}

}  // namespace detail

inline environment capture_environment(const env_reader& rd, int argc = 0, char** argv = nullptr) {
    environment e;
    const std::string dmi = "/sys/class/dmi/id/";
    e.vendor = rd.read(dmi + "sys_vendor");
    e.product = rd.read(dmi + "product_name");
    e.bios = rd.read(dmi + "bios_version");
    e.bios_date = rd.read(dmi + "bios_date");

    if (const auto ci = rd.read("/proc/cpuinfo")) {
        e.model = detail::cpuinfo_field(*ci, "model name");
        e.microcode = detail::cpuinfo_field(*ci, "microcode");
        const auto cores = detail::to_int(detail::cpuinfo_field(*ci, "cpu cores"));
        const auto siblings = detail::to_int(detail::cpuinfo_field(*ci, "siblings"));
        e.cores_per_socket = cores;
        if (cores && siblings && *cores > 0) e.threads_per_core = *siblings / *cores;
        std::set<std::string> ids;
        std::istringstream in(*ci);
        for (std::string line; std::getline(in, line);)
            if (line.rfind("physical id", 0) == 0) ids.insert(line.substr(line.find(':') + 1));
        if (!ids.empty()) e.sockets = static_cast<long long>(ids.size());
    }
    e.online = rd.read("/sys/devices/system/cpu/online");

    for (const auto& n : rd.list("/sys/devices/system/node")) {
        if (n.rfind("node", 0) != 0 || n.size() < 5 || !std::isdigit(static_cast<unsigned char>(n[4]))) continue;
        numa_node nn;
        nn.node = std::stoi(n.substr(4));
        nn.cpus = rd.read("/sys/devices/system/node/" + n + "/cpulist");
        if (const auto mi = rd.read("/sys/devices/system/node/" + n + "/meminfo")) {
            const auto p = mi->find("MemTotal:");
            if (p != std::string::npos) {
                try {
                    nn.memory_mb = std::stoll(mi->substr(p + 9)) / 1024;
                } catch (...) {
                }
            }
        }
        e.numa.push_back(nn);
    }
    std::sort(e.numa.begin(), e.numa.end(), [](const numa_node& a, const numa_node& b) { return a.node < b.node; });

    e.release = rd.read("/proc/sys/kernel/osrelease");
    e.version = rd.read("/proc/sys/kernel/version");
    e.cmdline = rd.read("/proc/cmdline");
    e.thp = detail::bracketed(rd.read("/sys/kernel/mm/transparent_hugepage/enabled"));
    e.numa_balancing = rd.read("/proc/sys/kernel/numa_balancing");
    e.perf_event_paranoid = rd.read("/proc/sys/kernel/perf_event_paranoid");
    const std::string vul = "/sys/devices/system/cpu/vulnerabilities/";
    for (const auto& v : rd.list(vul))
        if (const auto s = rd.read(vul + v)) e.vulnerabilities[v] = *s;

    // Placement. The cpuset files exist only while bench-on's fence is set.
    e.fence_system = rd.read("/sys/fs/cgroup/system.slice/cpuset.cpus");
    e.fence_user = rd.read("/sys/fs/cgroup/user.slice/cpuset.cpus");
    if (const auto cg = rd.read("/proc/self/cgroup")) {
        const auto p = cg->find("::");
        e.cgroup = p == std::string::npos ? *cg : cg->substr(p + 2);
    }
    if (const auto st = rd.read("/proc/self/status")) {
        const auto p = st->find("Cpus_allowed_list:");
        if (p != std::string::npos) {
            const auto v = st->find_first_not_of(" \t", p + 18);
            e.affinity = st->substr(v, st->find('\n', v) - v);
        }
    }
    if (e.online && e.fence_user) {
        const auto online = parse_cpu_list(*e.online);
        const auto house = parse_cpu_list(*e.fence_user);
        const std::set<int> h(house.begin(), house.end());
        for (int c : online)
            if (!h.count(c)) e.reserved.push_back(c);
    }

    // Power, over the reserved CPUs (all online CPUs if there is no fence).
    std::vector<int> power_cpus = e.reserved;
    if (power_cpus.empty() && e.online) power_cpus = parse_cpu_list(*e.online);
    std::set<std::string> governors, idle, idle_off;
    for (int c : power_cpus) {
        const std::string base = "/sys/devices/system/cpu/cpu" + std::to_string(c);
        governors.insert(rd.read(base + "/cpufreq/scaling_governor").value_or("unknown"));
        for (const auto& st : rd.list(base + "/cpuidle")) {
            if (st.rfind("state", 0) != 0) continue;
            const auto name = rd.read(base + "/cpuidle/" + st + "/name").value_or(st);
            if (rd.read_int(base + "/cpuidle/" + st + "/disable").value_or(0) == 0) idle.insert(name);
            else idle_off.insert(name);
        }
    }
    if (governors.size() == 1 && *governors.begin() != "unknown") {
        e.governor = *governors.begin();
    } else if (!governors.empty()) {
        std::string g = "mixed:";
        for (const auto& x : governors) g += " " + x;
        e.governor = g;
    }
    e.idle_states_enabled.assign(idle.begin(), idle.end());
    e.idle_states_disabled.assign(idle_off.begin(), idle_off.end());
    if (const auto nt = rd.read_int("/sys/devices/system/cpu/intel_pstate/no_turbo")) e.turbo = *nt == 0;
    else if (const auto b = rd.read_int("/sys/devices/system/cpu/cpufreq/boost")) e.turbo = *b != 0;
    const std::string unc = "/sys/devices/system/cpu/intel_uncore_frequency/";
    for (const auto& p : rd.list(unc)) {
        if (p.rfind("package_", 0) != 0) continue;
        uncore_socket u;
        u.socket = std::stoi(p.substr(8, 2));
        u.min_khz = rd.read_int(unc + p + "/min_freq_khz");
        u.max_khz = rd.read_int(unc + p + "/max_freq_khz");
        e.uncore.push_back(u);
    }

    if (argc > 0 && argv) {
        e.program = std::filesystem::path(argv[0]).filename().string();
        for (int i = 1; i < argc; ++i) e.args.emplace_back(argv[i]);
    }
    char stamp[32];
    const std::time_t now = std::time(nullptr);
    std::tm utc;
    gmtime_r(&now, &utc);
    std::strftime(stamp, sizeof(stamp), "%Y-%m-%dT%H:%M:%SZ", &utc);
    e.start_utc = stamp;
    return e;
}

// The bench-mode conditions (note 004), each failure named. Empty when all hold.
inline std::vector<std::string> bench_mode_failures(const environment& e) {
    std::vector<std::string> f;
    if (!e.fence_system || !e.fence_user || *e.fence_system != *e.fence_user || e.reserved.empty())
        f.push_back("fence not in place: system and user slices not limited to the same housekeeping CPUs");
    if (e.governor != "performance") f.push_back("governor is '" + e.governor.value_or("unknown") + "', not 'performance'");
    // Every idle state available on the reserved CPUs: the measured threads spin
    // and never idle, while an idle hyper-thread sibling must reach a deep
    // state to give its core's shared resources back (capped at C1, it halved
    // the branch prediction a compute-bound thread could use; note 004,
    // Review 2026-10-22).
    for (const auto& st : e.idle_states_disabled)
        f.push_back("idle state " + st + " disabled on a reserved CPU");
    if (e.uncore.empty()) f.push_back("uncore frequency not readable");
    for (const auto& u : e.uncore)
        if (!u.min_khz || !u.max_khz || *u.min_khz != *u.max_khz)
            f.push_back("uncore on socket " + std::to_string(u.socket) + " not pinned (min != max)");
    if (!e.cgroup || e.cgroup->find("/bench.slice/") == std::string::npos)
        f.push_back("not started by bench-run (cgroup '" + e.cgroup.value_or("unknown") + "')");
    if (e.affinity && !e.reserved.empty()) {
        const auto a = parse_cpu_list(*e.affinity);
        const std::set<int> as(a.begin(), a.end());
        if (!std::all_of(e.reserved.begin(), e.reserved.end(), [&](int c) { return as.count(c) > 0; }))
            f.push_back("affinity " + *e.affinity + " does not cover the reserved CPUs");
    }
    // Automatic NUMA balancing stops a thread for ~0.6 ms at a time to scan its
    // memory (note 005); bench-on turns it off.
    if (e.numa_balancing != "0")
        f.push_back("automatic NUMA balancing is '" + e.numa_balancing.value_or("unknown") + "', not off");
    if (!e.clean) f.push_back("built from a tree with uncommitted changes to tracked files");
    return f;
}

// A run counts as published when it writes under the results directory.
inline bool is_published(const std::string& output_path, const std::string& results_dir) {
    const auto canon = [](const std::string& p) {
        std::error_code ec;
        const auto c = std::filesystem::weakly_canonical(p, ec);
        return ec ? p : c.string();
    };
    const std::string r = canon(results_dir), o = canon(output_path);
    return o.size() > r.size() && o.compare(0, r.size(), r) == 0 && o[r.size()] == '/';
}

// Writes `"environment": {...}`. A published run that fails a condition is
// refused (ok false, with a reason naming every failure); the caller then
// measures nothing (S3).
inline void write_environment_block(json_writer& w, const environment& e, bool published,
                                    const std::vector<std::string>& failures) {
    const bool ok = !published || failures.empty();
    w.key("environment");
    w.begin_object();
    w.member("published", published);
    w.member("ok", ok);
    if (!ok) {
        std::string reason = "not in bench mode:";
        for (std::size_t i = 0; i < failures.size(); ++i) reason += (i ? "; " : " ") + failures[i];
        w.member("reason", reason);
    }
    w.key("bench_mode");
    w.begin_object();
    w.member("ok", failures.empty());
    w.key("failed");
    w.begin_array();
    for (const auto& x : failures) w.value(x);
    w.end_array();
    w.end_object();

    w.key("machine");
    w.begin_object();
    w.member("vendor", e.vendor);
    w.member("product", e.product);
    w.member("bios", e.bios);
    w.member("bios_date", e.bios_date);
    w.end_object();

    w.key("cpu");
    w.begin_object();
    w.member("model", e.model);
    w.member("microcode", e.microcode);
    w.member("sockets", e.sockets);
    w.member("cores_per_socket", e.cores_per_socket);
    w.member("threads_per_core", e.threads_per_core);
    w.member("online", e.online);
    w.end_object();

    w.key("numa");
    w.begin_array();
    for (const auto& n : e.numa) {
        w.begin_object();
        w.member("node", n.node);
        w.member("cpus", n.cpus);
        w.member("memory_mb", n.memory_mb);
        w.end_object();
    }
    w.end_array();

    w.key("kernel");
    w.begin_object();
    w.member("release", e.release);
    w.member("version", e.version);
    w.member("cmdline", e.cmdline);
    w.member("thp", e.thp);
    w.member("numa_balancing", e.numa_balancing);
    w.member("perf_event_paranoid", e.perf_event_paranoid);
    w.key("vulnerabilities");
    w.begin_object();
    for (const auto& [k, v] : e.vulnerabilities) w.member(k, v);
    w.end_object();
    w.end_object();

    w.key("power");
    w.begin_object();
    w.member("governor", e.governor);
    w.member("turbo", e.turbo);
    w.key("idle_states_enabled");
    w.begin_inline_array();
    for (const auto& x : e.idle_states_enabled) w.value(x);
    w.end_array();
    w.key("uncore_khz");
    w.begin_array();
    for (const auto& u : e.uncore) {
        w.begin_object();
        w.member("socket", u.socket);
        w.member("min", u.min_khz);
        w.member("max", u.max_khz);
        w.end_object();
    }
    w.end_array();
    w.end_object();

    w.key("placement");
    w.begin_object();
    w.member("fence", e.fence_user);
    w.member("reserved", e.reserved.empty() ? opt_str{} : opt_str{detail::cpu_list_string(e.reserved)});
    w.member("cgroup", e.cgroup);
    w.member("affinity", e.affinity);
    w.member("memory_policy", e.memory_policy);
    w.end_object();

    w.key("build");
    w.begin_object();
    w.member("commit", e.commit.empty() ? opt_str{} : opt_str{e.commit});
    w.member("clean", e.clean);
    w.member("compiler", e.compiler);
    w.member("build_type", e.build_type);
    w.member("flags", e.flags);
    w.end_object();

    w.key("run");
    w.begin_object();
    w.member("program", e.program);
    w.key("args");
    w.begin_inline_array();
    for (const auto& a : e.args) w.value(a);
    w.end_array();
    w.member("start_utc", e.start_utc);
    w.end_object();
    w.end_object();
}

}  // namespace lfipc::bench
