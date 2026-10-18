"""Histogram JSON: fields, 10-sample rule, bucket totals, no mean. Design: docs/design/002-histogram.md

Usage: check_histogram_json.py histogram_full.json histogram_empty.json [other run JSON ...]

S5: a percentile is null exactly when fewer than 10 samples lie above its rank.
S6: no key anywhere in any file given looks like a mean.
"""
import json
import math
import re
import sys

FIELDS = {"sub_bucket_bits", "max_bits", "count", "overflow", "min_ns", "max_ns", "percentiles_ns", "buckets"}
PERCENTILES = {"p50": 50.0, "p90": 90.0, "p99": 99.0, "p999": 99.9, "p9999": 99.99, "p99999": 99.999}
MEAN = re.compile(r"mean|avg|average", re.IGNORECASE)

failures = 0


def check(cond, what):
    global failures
    print(("pass" if cond else "FAIL") + "  " + what)
    if not cond:
        failures += 1


def keys(node):
    if isinstance(node, dict):
        for k, v in node.items():
            yield k
            yield from keys(v)
    elif isinstance(node, list):
        for v in node:
            yield from keys(v)


def rank(p, n):
    # Same rule as rank.hpp.
    return min(max(math.ceil(p * n / 100.0 - 1e-9), 1), n)


def check_block(h, name):
    check(set(h) == FIELDS, f"{name}: exactly the agreed fields")
    check(set(h["percentiles_ns"]) == set(PERCENTILES), f"{name}: the published percentile set")
    n = h["count"]
    nulls_ok = all((v is None) == (n == 0 or n - rank(PERCENTILES[k], n) < 10)
                   for k, v in h["percentiles_ns"].items())
    check(nulls_ok, f"{name}: S5 null exactly when fewer than 10 samples lie above the rank")
    total = sum(c for _, c in h["buckets"]) + h["overflow"]
    check(total == n, f"{name}: buckets plus overflow equal count ({total} vs {n})")
    edges = [e for e, _ in h["buckets"]]
    check(edges == sorted(edges) and len(set(edges)) == len(edges), f"{name}: bucket edges ascending, unique")
    vals = [v for v in h["percentiles_ns"].values() if v is not None]
    check(vals == sorted(vals) and all(h["min_ns"] <= v <= h["max_ns"] for v in vals),
          f"{name}: percentiles ordered and within min..max")


full = json.load(open(sys.argv[1]))["histogram"]
empty = json.load(open(sys.argv[2]))["histogram"]
check_block(full, "full")
check_block(empty, "empty")
check(full["percentiles_ns"]["p99999"] is not None, "full: p99.999 published at 2M samples")
check(all(v is None for v in empty["percentiles_ns"].values()), "empty: every percentile null")

for path in sys.argv[1:]:
    found = [k for k in keys(json.load(open(path))) if MEAN.search(k)]
    check(not found, f"S6 no mean in {path}" + (f": {found}" if found else ""))

print("FAILED" if failures else "all passed")
sys.exit(1 if failures else 0)
