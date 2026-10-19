"""Load JSON: exact fields per mode, published and refused rates. Design: docs/design/003-open-loop-load.md

Usage: check_load_json.py load_block.json

The file holds a one-way and a round-trip block (note 003, amended: one mode
per run). A published rate carries only its mode's latency; an echo run's
one-way figure is never written. A refused rate has its reason and the
lateness histogram that refused it, and nothing else (S3). The one-way
offset bound for the pair is present (S7). Across the two blocks, the round
trip is never below the one-way trip at the same rate.
"""
import json
import sys

LOAD_FIELDS = {"mode", "sut", "producer_cpu", "consumer_cpu", "one_way_offset_bound", "runs"}
BOUND_FIELDS = {"ok", "theta_lo_ns", "theta_hi_ns", "round_trips", "converged"}
RUN_COMMON = {"rate_per_s", "interval_ns", "messages", "warmup_discarded", "memory_node", "ok", "lateness"}
RUN_OK = {"one_way": RUN_COMMON | {"out_of_order", "negative", "one_way"},
          "round_trip": RUN_COMMON | {"out_of_order", "negative", "round_trip"}}
RUN_REFUSED = RUN_COMMON | {"reason"}
HIST_FIELDS = {"sub_bucket_bits", "max_bits", "count", "overflow", "min_ns", "max_ns", "percentiles_ns", "buckets"}

failures = 0


def check(cond, what):
    global failures
    print(("pass" if cond else "FAIL") + "  " + what)
    if not cond:
        failures += 1


d = json.load(open(sys.argv[1]))
for mode in ("one_way", "round_trip"):
    load = d[mode]
    check(set(load) == LOAD_FIELDS and load["mode"] == mode, f"{mode}: exactly the agreed fields, mode recorded")
    b = load["one_way_offset_bound"]
    check(set(b) == BOUND_FIELDS and b["theta_lo_ns"] <= b["theta_hi_ns"], f"{mode}: S7 offset bound present")
    runs = load["runs"]
    published = [r for r in runs if r["ok"]]
    refused = [r for r in runs if not r["ok"]]
    check(len(published) > 0 and len(refused) > 0, f"{mode}: a published and a refused rate to check")
    check(all(set(r) == RUN_OK[mode] for r in published), f"{mode}: published rate carries only its mode's latency")
    check(all(set(r) == RUN_REFUSED and r["reason"] for r in refused), f"{mode}: S3 refused rate, reason and lateness only")
    hists = [r[k] for r in published for k in ("lateness", mode)] + [r["lateness"] for r in refused]
    check(all(set(h) == HIST_FIELDS for h in hists), f"{mode}: every histogram has the histogram block's fields")

ow = {r["rate_per_s"]: r for r in d["one_way"]["runs"] if r["ok"]}
rt = {r["rate_per_s"]: r for r in d["round_trip"]["runs"] if r["ok"]}
common = sorted(set(ow) & set(rt))
check(common and all(rt[k]["round_trip"]["percentiles_ns"]["p50"] >= ow[k]["one_way"]["percentiles_ns"]["p50"]
                     for k in common), "round trip p50 not below one-way p50 at the same rate")

print("FAILED" if failures else "all passed")
sys.exit(1 if failures else 0)
