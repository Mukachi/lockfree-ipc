"""S1: the environment block has exactly the agreed fields. Design: docs/design/004-environment.md

Usage: check_environment_json.py environment.json [run JSON ...]

Every file given must start with an environment block; the first is the test
fixture, the rest are run files whose block must have the same shape.
"""
import json
import sys

TOP = {"published", "ok", "bench_mode", "machine", "cpu", "numa", "kernel", "power", "placement", "build", "run"}
SUB = {
    "bench_mode": {"ok", "failed"},
    "machine": {"vendor", "product", "bios", "bios_date"},
    "cpu": {"model", "microcode", "sockets", "cores_per_socket", "threads_per_core", "online"},
    "kernel": {"release", "version", "cmdline", "thp", "numa_balancing", "perf_event_paranoid", "vulnerabilities"},
    "power": {"governor", "turbo", "idle_states_enabled", "uncore_khz"},
    "placement": {"fence", "reserved", "cgroup", "affinity", "memory_policy"},
    "build": {"commit", "clean", "compiler", "build_type", "flags"},
    "run": {"program", "args", "start_utc"},
}

# Fields added after runs were published; a run recorded before them lacks them.
OPTIONAL = {"kernel": {"perf_event_paranoid"}}  # added 2026-10-22

failures = 0


def check(cond, what):
    global failures
    print(("pass" if cond else "FAIL") + "  " + what)
    if not cond:
        failures += 1


for path in sys.argv[1:]:
    d = json.load(open(path))
    check(next(iter(d)) == "environment", f"{path}: environment block comes first")
    e = d["environment"]
    refused = not e["ok"]
    check(set(e) == TOP | ({"reason"} if refused else set()), f"{path}: exactly the agreed top-level fields")
    check(all(set(e[k]) in (v, v - OPTIONAL.get(k, set())) for k, v in SUB.items()),
          f"{path}: exactly the agreed fields in each group")
    check(all(set(n) == {"node", "cpus", "memory_mb"} for n in e["numa"]), f"{path}: numa node fields")
    check(e["ok"] == (not e["published"] or e["bench_mode"]["ok"]), f"{path}: refused exactly when published and failing")
    check(e["bench_mode"]["ok"] == (len(e["bench_mode"]["failed"]) == 0), f"{path}: bench_mode ok matches its failures")
    if refused:
        check(list(d) == ["environment"], f"{path}: S3 a refused run measures nothing")

print("FAILED" if failures else "all passed")
sys.exit(1 if failures else 0)
