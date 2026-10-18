"""Load JSON: exact fields for published and refused rates. Design: docs/design/003-open-loop-load.md

Usage: check_load_json.py load_block.json

A published rate has lateness, one-way and round-trip histograms; a refused one
has its reason and the lateness histogram that refused it, and nothing else (S3).
The one-way offset bound for the CPU pair is present (S7).
"""
import json
import sys

LOAD_FIELDS = {"sut", "producer_cpu", "consumer_cpu", "one_way_offset_bound", "runs"}
BOUND_FIELDS = {"ok", "theta_lo_ns", "theta_hi_ns", "round_trips", "converged"}
RUN_COMMON = {"rate_per_s", "interval_ns", "messages", "warmup_discarded", "ok", "lateness"}
RUN_OK = RUN_COMMON | {"out_of_order", "negative", "one_way", "round_trip"}
RUN_REFUSED = RUN_COMMON | {"reason"}
HIST_FIELDS = {"sub_bucket_bits", "max_bits", "count", "overflow", "min_ns", "max_ns", "percentiles_ns", "buckets"}

failures = 0


def check(cond, what):
    global failures
    print(("pass" if cond else "FAIL") + "  " + what)
    if not cond:
        failures += 1


load = json.load(open(sys.argv[1]))["load"]
check(set(load) == LOAD_FIELDS, "load block has exactly the agreed fields")
b = load["one_way_offset_bound"]
check(set(b) == BOUND_FIELDS and b["theta_lo_ns"] <= b["theta_hi_ns"], "S7 offset bound for the pair present")

runs = load["runs"]
published = [r for r in runs if r["ok"]]
refused = [r for r in runs if not r["ok"]]
check(len(runs) > 0, f"rates present ({len(published)} published, {len(refused)} refused)")
check(all(set(r) == RUN_OK for r in published), "published rate: exactly the agreed fields")
check(all(set(r) == RUN_REFUSED for r in refused), "S3 refused rate: reason and lateness only, no latency")
check(all(r["reason"] for r in refused), "S3 refused rate carries a reason")
hists = [r[k] for r in published for k in ("lateness", "one_way", "round_trip")] + [r["lateness"] for r in refused]
check(all(set(h) == HIST_FIELDS for h in hists), "every histogram has the histogram block's fields")
check(all(r["round_trip"]["percentiles_ns"]["p50"] >= r["one_way"]["percentiles_ns"]["p50"] for r in published),
      "round trip p50 not below one-way p50")

print("FAILED" if failures else "all passed")
sys.exit(1 if failures else 0)
