"""Noise JSON: exact fields per CPU, classes add up. Design: docs/design/005-noise-floor.md

Usage: check_noise_json.py run.json [run.json ...]

S1: the noise block comes after environment and before clock, with every field
for each sampled CPU; the stop classes sum to the stop count, which the
stop-length histogram also holds.
"""
import json
import sys

TOP = {"threshold_ns", "seconds_per_cpu", "cpus"}
CPU_OK = {"cpu", "ok", "stopped_fraction", "stops", "stops_per_s", "by_class", "longest_ns", "iterations",
          "iteration_min_ns", "overflow", "interrupts", "softirqs", "involuntary_switches", "stop_lengths"}
CPU_REFUSED = {"cpu", "ok", "reason"}
CLASSES = {"1-10us", "10-100us", "100us-1ms", "over_1ms"}

failures = 0


def check(cond, what):
    global failures
    print(("pass" if cond else "FAIL") + "  " + what)
    if not cond:
        failures += 1


for path in sys.argv[1:]:
    d = json.load(open(path))
    keys = list(d)
    check(keys.index("noise") == keys.index("environment") + 1 and keys.index("noise") < keys.index("clock"),
          f"{path}: S1 noise after environment, before clock")
    n = d["noise"]
    check(set(n) == TOP and n["cpus"], f"{path}: noise block fields, at least one CPU")
    for c in n["cpus"]:
        name = f"{path}: cpu {c['cpu']}"
        if not c["ok"]:
            check(set(c) == CPU_REFUSED and c["reason"], f"{name}: refused with a reason")
            continue
        check(set(c) == CPU_OK and set(c["by_class"]) == CLASSES, f"{name}: exactly the agreed fields")
        check(sum(c["by_class"].values()) == c["stops"] == c["stop_lengths"]["count"],
              f"{name}: classes sum to the stop count, as does the histogram")
        check(0 <= c["stopped_fraction"] < 1 and c["iteration_min_ns"] < n["threshold_ns"] / 10,
              f"{name}: fraction in range; S2 loop iteration far below the threshold")

print("FAILED" if failures else "all passed")
sys.exit(1 if failures else 0)
