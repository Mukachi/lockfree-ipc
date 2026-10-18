"""S3: the clock block parses and has exactly the agreed fields. Design: docs/design/001-clock.md

Usage: check_clock_json.py clock_ok.json clock_refused.json
"""
import json
import sys

CHECK_FIELDS = {"ok", "source", "invariant_tsc", "kernel_clocksource"}
OK_FIELDS = CHECK_FIELDS | {"ticks_per_ns", "calibration_spread_ppm", "overhead", "cross_cpu"}
REFUSED_FIELDS = CHECK_FIELDS | {"reason"}
OVERHEAD_FIELDS = {"cpu", "samples", "warmup_discarded", "p50_ns", "p99_ns", "p999_ns", "max_ns"}
PAIR_FIELDS = {"a", "b", "theta_lo_ns", "theta_hi_ns", "gain_hi_ticks", "gain_lo_ticks", "round_trips",
               "warmup_discarded", "converged"}

failures = 0


def check(cond, what):
    global failures
    print(("pass" if cond else "FAIL") + "  " + what)
    if not cond:
        failures += 1


def load(path):
    with open(path) as f:
        return json.load(f)["clock"]


ok = load(sys.argv[1])
refused = load(sys.argv[2])

if ok["ok"]:
    check(set(ok) == OK_FIELDS, "S3 published block has exactly the agreed fields")
    check(set(ok["overhead"]) == OVERHEAD_FIELDS, "S3 overhead fields")
    check(len(ok["cross_cpu"]) > 0, "cross-CPU pairs present")
    check(all(set(p) == PAIR_FIELDS for p in ok["cross_cpu"]), "S3 cross-CPU pair fields")
    o = ok["overhead"]
    check(o["p50_ns"] <= o["p99_ns"] <= o["p999_ns"] <= o["max_ns"], "S4 overhead percentiles ordered")
    check(all(p["theta_lo_ns"] <= p["theta_hi_ns"] for p in ok["cross_cpu"]), "cross-CPU bounds ordered")
    tpn = ok["ticks_per_ns"]

    def flag_agrees(p):
        # Converged when neither gain exceeds 1% of the width (cross_cpu.hpp).
        width = round((p["theta_hi_ns"] - p["theta_lo_ns"]) * tpn)
        return p["converged"] == (100 * max(p["gain_hi_ticks"], p["gain_lo_ticks"]) <= width)
    check(all(flag_agrees(p) for p in ok["cross_cpu"]), "converged flag agrees with the gains and the 1% rule")
else:
    print("info  healthy run refused (not x86-64?): " + ok["reason"])
    check(set(ok) == REFUSED_FIELDS, "refused block has exactly the refusal fields")

check(refused["ok"] is False and set(refused) == REFUSED_FIELDS, "S1 refusal block has exactly the refusal fields")
check(isinstance(refused["reason"], str) and refused["reason"] != "", "S1 refusal carries a reason")

print("FAILED" if failures else "all passed")
sys.exit(1 if failures else 0)
