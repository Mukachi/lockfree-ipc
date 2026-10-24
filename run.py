#!/usr/bin/env python3
"""Results report: RESULTS.md and the README's figures, generated from committed JSON.

Design: docs/design/007-results-report.md

  python run.py report          write RESULTS.md and fill the README's result blocks
  python run.py report --check  regenerate in memory; exit 1 naming any difference

Only files listed in results/published.toml are read, and each is validated
first: environment checker, published and clean, the component's blocks
present, and one commit, machine and kernel command line per set. Figures are
min-max over a set's runs; refusals are shown, never dropped. Standard library
only.
"""
import argparse
import json
import re
import subprocess
import sys
import tomllib
from pathlib import Path

SETS = ("noise", "clock", "histogram", "load", "spsc", "shm", "mpmc", "siblings")
BLOCK = re.compile(r"(<!-- results:(?P<name>[a-z0-9-]+) -->\n)(?P<body>.*?)(<!-- /results -->)", re.S)
MEAN = re.compile(r"\b(mean|average|avg)\b", re.I)
QUEUES = ("spsc_v1", "spsc_v2", "spsc_v3", "spsc_v4", "moodycamel", "boost")
QUEUE_LABEL = {"spsc_v1": "v1", "spsc_v2": "v2", "spsc_v3": "**v3**", "spsc_v4": "v4", "moodycamel": "moodycamel",
               "boost": "Boost"}
MPMC_QUEUES = ("mpmc_v1", "mpmc_v2", "moodycamel", "boost", "rigtorp", "spsc_v3")
MPMC_LABEL = {"mpmc_v1": "v1", "mpmc_v2": "**v2**", "moodycamel": "moodycamel", "boost": "Boost", "rigtorp": "rigtorp",
              "spsc_v3": "SPSC v3 (reference)"}


class ReportError(Exception):
    pass


# ---------------------------------------------------------------- formatting


def ns(v):
    """A latency in ns: whole ns below 10 µs, else µs with one decimal."""
    if v is None:
        return "–"
    return f"{v:,.0f}" if v < 10_000 else f"{v / 1000:,.1f} µs"


def ns_unit(v):
    """As ns(), with the unit always written."""
    return ns(v) if v >= 10_000 else f"{ns(v)} ns"


def rng(values, fmt=ns):
    """min–max over runs, one value if they agree once formatted."""
    vals = [v for v in values if v is not None]
    if not vals:
        return "–"
    lo, hi = fmt(min(vals)), fmt(max(vals))
    if fmt is ns and min(vals) < 10_000 <= max(vals):  # one unit: "9,345–11.9 µs" reads as µs throughout
        lo = f"{min(vals) / 1000:,.1f}"
        hi = hi.removesuffix(" µs")
        return lo if lo == hi else f"{lo}–{hi} µs"
    return lo if lo == hi else f"{lo}–{hi}"


def pct(v):
    return f"{100 * v:.4f}%"


def table(header, rows):
    out = ["| " + " | ".join(header) + " |", "|" + "|".join("---" for _ in header) + "|"]
    out += ["| " + " | ".join(str(c) for c in r) + " |" for r in rows]
    return "\n".join(out)


def rate(r):
    return f"{r / 1e6:g}M/s" if r >= 1e6 else f"{r / 1e3:g}k/s"


# ---------------------------------------------------------------- validation


def validate(root, name, files):
    """Load a set's files, or raise naming the file and the reason (S3, S4)."""
    runs = []
    for f in files:
        path = root / "results" / f
        if not path.is_file():
            raise ReportError(f"{name}: {f}: missing")
        check = subprocess.run([sys.executable, "-I", str(root / "tests" / "check_environment_json.py"), str(path)],
                               capture_output=True, text=True)
        if check.returncode != 0:
            fails = [l for l in check.stdout.splitlines() if l.startswith("FAIL")]
            raise ReportError(f"{name}: {f}: environment check failed: {fails[:2]}")
        d = json.loads(path.read_text())
        e = d["environment"]
        if not (e["published"] and e["ok"] and e["bench_mode"]["ok"]):
            raise ReportError(f"{name}: {f}: not a published run (refused or outside results/)")
        if not e["build"]["clean"]:
            raise ReportError(f"{name}: {f}: built from a tree with uncommitted changes")
        need = {"noise": ["noise_bench"], "clock": ["clock"], "histogram": ["clock", "record_cost"],
                "load": ["load"], "spsc": ["spsc"], "shm": ["shm"], "mpmc": ["mpmc"],
                "siblings": ["siblings"]}[name]
        missing = [k for k in need if k not in d]
        if missing:
            raise ReportError(f"{name}: {f}: no {missing} block")
        if name == "clock" and not d["clock"]["ok"]:
            raise ReportError(f"{name}: {f}: clock refused: {d['clock'].get('reason')}")
        if name == "load" and set(d["load"]) != {"one_way", "round_trip"}:
            raise ReportError(f"{name}: {f}: load block is not one mode per run (note 003, amended)")
        if name in ("load", "spsc", "shm", "mpmc", "siblings"):
            check_placement(name, f, d)
        if name == "siblings":
            for p in d["siblings"]["pairs"]:
                for k, c in p["cells"].items():
                    if not {"one_way", "round_trip"} <= set(c):
                        raise ReportError(f"{name}: {f}: cell {k} is not one mode per run (note 003, amended)")
                offsets = {c["ring_page_offset"] for k, c in p["cells"].items() if k != "R"}
                if len(offsets) > 1:
                    raise ReportError(f"{name}: {f}: cells A-D do not share one layout (note 012, S1)")
                if len({c[s] for c in p["cells"].values() for s in ("producer_speculation", "consumer_speculation")}) > 1:
                    raise ReportError(f"{name}: {f}: speculation controls differ between cells (note 012, S2)")
        if name == "mpmc":
            for sh in d["mpmc"]["shapes"]:
                for q, blocks in sh["queues"].items():
                    if set(blocks) != {"one_way"}:
                        raise ReportError(f"{name}: {f}: {q} is not one-way only (note 010)")
                    if any(r["ok"] and r["received"] != r["sent"] for r in blocks["one_way"]["runs"]):
                        raise ReportError(f"{name}: {f}: {q}: a published run lost messages")
        if name == "shm":
            for p in d["shm"]["pairs"]:
                for t, blocks in p["transports"].items():
                    if set(blocks) != {"one_way", "round_trip"}:
                        raise ReportError(f"{name}: {f}: {t} is not one mode per run (note 003, amended)")
        if name == "spsc":
            for p in d["spsc"]["pairs"]:
                for q, blocks in p["queues"].items():
                    if set(blocks) != {"one_way", "round_trip"}:
                        raise ReportError(f"{name}: {f}: {q} is not one mode per run (note 003, amended)")
        runs.append((f, d))
    if not runs:
        raise ReportError(f"{name}: no runs listed")
    for key, get in (("commit", lambda e: e["build"]["commit"]),
                     ("machine", lambda e: (e["machine"]["product"], e["cpu"]["model"], e["cpu"]["microcode"])),
                     ("kernel command line", lambda e: e["kernel"]["cmdline"])):
        seen = {get(d["environment"]) for _, d in runs}
        if len(seen) > 1:
            raise ReportError(f"{name}: runs mix more than one {key}")
    return runs


def check_placement(name, f, d):
    """Every load run's queue lived on its producer's NUMA node (note 008, Review)."""
    node_of_cpu = {}
    for n in d["environment"]["numa"]:
        for part in (n["cpus"] or "").split(","):
            lo, _, hi = part.partition("-")
            if lo:
                for c in range(int(lo), int(hi or lo) + 1):
                    node_of_cpu[c] = n["node"]
    if name == "load":
        blocks = list(d["load"].values())
    elif name == "spsc":
        blocks = [b for p in d["spsc"]["pairs"] for q in p["queues"].values() for b in q.values()]
    elif name == "mpmc":
        blocks = [b for sh in d["mpmc"]["shapes"] for q in sh["queues"].values() for b in q.values()]
    elif name == "siblings":
        blocks = [c[m] for p in d["siblings"]["pairs"] for c in p["cells"].values() for m in ("one_way", "round_trip")]
    else:
        blocks = [b for p in d["shm"]["pairs"] for t in p["transports"].values() for b in t.values()]
    for b in blocks:
        want = node_of_cpu.get(b["producer_cpu"])
        for r in b["runs"]:
            if "memory_node" not in r:
                raise ReportError(f"{name}: {f}: runs do not record where the queue lived (before note 008's fix)")
            if r["memory_node"] != want:
                raise ReportError(f"{name}: {f}: a queue on node {r['memory_node']}, producer CPU "
                                  f"{b['producer_cpu']} on node {want}")


def load_sets(root):
    manifest = root / "results" / "published.toml"
    if not manifest.is_file():
        raise ReportError("results/published.toml missing")
    m = tomllib.loads(manifest.read_text())
    m = {k: v for k, v in m.items() if k != "notes"}
    unknown = set(m) - set(SETS)
    if unknown:
        raise ReportError(f"published.toml: unknown sets {sorted(unknown)}")
    return {name: (m[name].get("note", ""), validate(root, name, m[name]["runs"])) for name in SETS if name in m}


# ---------------------------------------------------------------- sections


def conditions(sets):
    first = next(iter(sets.values()))[1][0][1]["environment"]
    lines = ["## Conditions", "",
             f"{first['machine']['vendor']} {first['machine']['product']}, BIOS {first['machine']['bios']} "
             f"({first['machine']['bios_date']}); {first['cpu']['sockets']} × {first['cpu']['model']}, "
             f"{first['cpu']['cores_per_socket']} cores per socket, {first['cpu']['threads_per_core']} threads per core, "
             f"microcode {first['cpu']['microcode']}; kernel {first['kernel']['release']}.", ""]
    rows = []
    for name, (note, runs) in sets.items():
        e = runs[0][1]["environment"]
        starts = sorted(d["environment"]["run"]["start_utc"] for _, d in runs)
        iso = "nohz_full" in e["kernel"]["cmdline"]
        rows.append([f"{name} (note {note})", len(runs), starts[0][:16].replace("T", " ") + " UTC",
                     e["build"]["commit"][:7], e["power"]["governor"], " ".join(e["power"]["idle_states_enabled"]),
                     "on" if e["power"]["turbo"] else "off",
                     "off" if e["kernel"].get("numa_balancing") == "0" else "on",
                     "yes" if iso else "no", e["placement"]["reserved"] or "–"])
    lines += [table(["Set", "Runs", "First run", "Commit", "Governor", "Idle states enabled", "Turbo",
                     "NUMA balancing", "Boot isolation", "Reserved CPUs"], rows), "",
              "Every run met note 004's bench-mode conditions (each file's `environment.bench_mode`, checked "
              "before use) and was built clean; compiler and flags are in each file's `environment.build`. "
              "Ranges are min–max over a set's runs.", ""]
    return lines


def noise_section(note, runs):
    out = [f"## Noise floor (note {note})", ""]
    for f, d in runs:
        nb = d["noise_bench"]
        s = nb["summary"]
        cls = {"1-10us": 0, "10-100us": 0, "100us-1ms": 0, "over_1ms": 0}
        for r in nb["runs"]:
            for c in r["noise"]["cpus"]:
                if c["ok"]:
                    for k in cls:
                        cls[k] += c["by_class"][k]
        secs = nb["repeats"] * nb["seconds_per_cpu"] * len(s)
        out += [f"CPUs {nb['cpus']}, {nb['seconds_per_cpu']:g} s each, {nb['repeats']} repeats "
                f"({secs:,.0f} CPU-seconds); a stop is a gap over {r['noise']['threshold_ns']:.0f} ns between "
                f"consecutive TSC reads of a thread spinning alone.", "",
                table(["Time stopped (per CPU)", "Stops per second", "Longest stop", "1–10 µs", "10–100 µs",
                       "100 µs–1 ms", "> 1 ms"],
                      [[f"{pct(min(x['stopped_fraction_min_max'][0] for x in s))}–"
                        f"{pct(max(x['stopped_fraction_min_max'][1] for x in s))}",
                        f"{min(x['stops_per_s_min_max'][0] for x in s):g}–{max(x['stops_per_s_min_max'][1] for x in s):g}",
                        ns(max(x["longest_ns_min_max"][1] for x in s)),
                        cls["1-10us"], cls["10-100us"], cls["100us-1ms"], cls["over_1ms"]]]), ""]
    return out


def clock_section(note, runs):
    c = [d["clock"] for _, d in runs]
    o = [x["overhead"] for x in c]
    unconv = [sum(not p["converged"] for p in x["cross_cpu"]) for x in c]
    disagree = sum(p["theta_lo_ns"] > 0 or p["theta_hi_ns"] < 0 for x in c for p in x["cross_cpu"])
    sib = [p for x in c for p in x["cross_cpu"] if p["b"] == 44]
    return [f"## Clock (note {note})", "",
            table(["TSC ticks per ns", "Calibration spread", "Overhead CPU", "Samples", "Warm-up discarded"],
                  [[rng([x["ticks_per_ns"] for x in c], lambda v: f"{v:.7f}"),
                    rng([x["calibration_spread_ppm"] for x in c], lambda v: f"{v:.2f}") + " ppm",
                    o[0]["cpu"], f"{o[0]['samples']:,}", f"{o[0]['warmup_discarded']:,}"]]), "",
            "Cost of a `start()`/`stop()` pair:", "",
            table(["p50", "p99", "p99.9", "max"],
                  [[rng([x["p50_ns"] for x in o]), rng([x["p99_ns"] for x in o]), rng([x["p999_ns"] for x in o]),
                    rng([x["max_ns"] for x in o])]]), "",
            f"Cross-CPU agreement, CPU 0 against every other CPU ({len(c[0]['cross_cpu'])} pairs per run): "
            f"{'no pair proves disagreement' if disagree == 0 else f'{disagree} pair-runs prove disagreement'}; "
            f"hyper-thread sibling (CPU 44) offset within {rng([p['theta_lo_ns'] for p in sib])} to "
            f"{rng([p['theta_hi_ns'] for p in sib])} ns; unconverged pairs per run {rng(unconv, str)}.", ""]


def histogram_section(note, runs):
    rows = []
    for case in ("none", "hot", "realistic", "worst"):
        cs = [d["record_cost"][case] for _, d in runs]
        rows.append([case, cs[0]["values"], rng([x["p50_ns"] for x in cs]), rng([x["p99_ns"] for x in cs]),
                     rng([x["p999_ns"] for x in cs]), rng([x["max_ns"] for x in cs])])
    c0 = runs[0][1]["record_cost"]["none"]
    return [f"## Histogram record cost (note {note})", "",
            f"`start(); record(v); stop()` against bare `start(); stop()` (`none`), unsubtracted; CPU {c0['cpu']}, "
            f"{c0['samples']:,} samples per case, {c0['warmup_discarded']:,} warm-up discarded.", "",
            table(["Case", "Values", "p50", "p99", "p99.9", "max"], rows), ""]


def load_rows(blocks, mode, n_runs):
    """One row per rate: published count, then p50/p99/p99.9/p99.99 ranges over published runs."""
    rates = sorted({r["rate_per_s"] for b in blocks for r in b["runs"]})
    rows = []
    for rt in rates:
        rs = [r for b in blocks for r in b["runs"] if r["rate_per_s"] == rt]
        ok = [r for r in rs if r["ok"]]
        if not ok:
            why = "generator ceiling" if all("producer lateness" in r.get("reason", "") for r in rs) else "refused"
            rows.append([rate(rt), f"0/{n_runs}", f"refused ({why})", "", "", ""])
            continue
        p = [r[mode]["percentiles_ns"] for r in ok]
        rows.append([rate(rt), f"{len(ok)}/{n_runs}", rng([x["p50"] for x in p]), rng([x["p99"] for x in p]),
                     rng([x["p999"] for x in p]), rng([x["p9999"] for x in p])])
    return rows


def load_section(note, runs):
    b0 = runs[0][1]["load"]["one_way"]
    out = [f"## Mailbox load (note {note})", "",
           f"{b0['sut']}, CPUs {b0['producer_cpu']} → {b0['consumer_cpu']}; open-loop, latency from each message's "
           f"intended send time; one-way and round trip in separate runs. Offset bound for the pair: "
           f"{rng([d['load']['one_way']['one_way_offset_bound']['theta_lo_ns'] for _, d in runs])} to "
           f"{rng([d['load']['one_way']['one_way_offset_bound']['theta_hi_ns'] for _, d in runs])} ns.", ""]
    for mode, title in (("one_way", "One-way"), ("round_trip", "Round trip")):
        out += [f"{title}:", "",
                table(["Rate", "Published", "p50", "p99", "p99.9", "p99.99"],
                      load_rows([d["load"][mode] for _, d in runs], mode, len(runs))), ""]
    return out


def spsc_block(runs, pair, queue, mode):
    out = []
    for _, d in runs:
        for p in d["spsc"]["pairs"]:
            if (p["producer_cpu"], p["consumer_cpu"]) == pair:
                out.append(p["queues"][queue][mode])
    return out


def spsc_section(note, runs):
    s = runs[0][1]["spsc"]
    pairs = [(p["producer_cpu"], p["consumer_cpu"]) for p in s["pairs"]]
    present = [q for q in QUEUES if q in s["pairs"][0]["queues"]]  # v4 from note 011 on
    v4 = "; v4: + each slot on its own 128 bytes (note 011)" if "spsc_v4" in present else ""
    out = [f"## SPSC queues (note {note})", "",
           f"Capacity {s['capacity']}, {s['message_bytes']}-byte messages, {s['seconds_per_rate']:g} s per rate, "
           f"warm-up 10% and at least 100 ms discarded; baselines moodycamel {s['baselines']['moodycamel']}, "
           f"Boost {s['baselines']['boost']}. v1: Lamport's ring; v2: + cached counters; v3 (the chosen "
           f"queue, `spsc_ring`'s default): + 128-byte separation{v4}. A refused rate is one the open-loop "
           f"generator could not sustain.", ""]
    for pair in pairs:
        where = "hyper-thread siblings" if pair[1] - pair[0] == 44 else "different cores, same socket"
        for mode, title in (("one_way", "one-way"), ("round_trip", "round trip")):
            rows = []
            for q in present:
                for r in load_rows(spsc_block(runs, pair, q, mode), mode, len(runs)):
                    rows.append([QUEUE_LABEL[q]] + r)
            out += [f"### {pair[0]} → {pair[1]} ({where}), {title}", "",
                    table(["Queue", "Rate", "Published", "p50", "p99", "p99.9", "p99.99"], rows), ""]
    return out


def shm_section(note, runs):
    s = runs[0][1]["shm"]
    pairs = [(p["producer_cpu"], p["consumer_cpu"]) for p in s["pairs"]]
    out = [f"## Shared-memory transport (note {note})", "",
           f"v3 between two processes (a `shm_open` segment; the consumer is a forked process that attaches by "
           f"name) against v3 between two threads, measured back to back in the same run. Capacity "
           f"{s['capacity']}, {s['message_bytes']}-byte messages, {s['seconds_per_rate']:g} s per rate.", ""]
    for pair in pairs:
        where = "hyper-thread siblings" if pair[1] - pair[0] == 44 else "different cores, same socket"
        for mode, title in (("one_way", "one-way"), ("round_trip", "round trip")):
            rows = []
            for t in ("threads", "processes"):
                blocks = [p["transports"][t][mode] for _, d in runs for p in d["shm"]["pairs"]
                          if (p["producer_cpu"], p["consumer_cpu"]) == pair]
                rows += [[t] + r for r in load_rows(blocks, mode, len(runs))]
            out += [f"### {pair[0]} → {pair[1]} ({where}), {title}", "",
                    table(["Transport", "Rate", "Published", "p50", "p99", "p99.9", "p99.99"], rows), ""]
            if pair[1] - pair[0] == 44 and mode == "round_trip":
                out += ["On siblings this compares more than threads with processes: the process version's ring "
                        "starts 128 bytes into a page (after the segment header), the thread version's at offset 0. "
                        "Note 012 separates the two: the one-way difference is mostly that layout, the round-trip "
                        "difference the second address space (see Hyper-thread siblings below).", ""]
    return out


def mpmc_section(note, runs):
    s = runs[0][1]["mpmc"]
    out = [f"## MPMC queues (note {note})", "",
           f"One-way latency from each message's intended send time, by shape (producers → consumers) and "
           f"aggregate rate; capacity {s['capacity']}, {s['message_bytes']}-byte messages, "
           f"{s['seconds_per_rate']:g} s per rate. Each producer sends at its share of the rate, offset so arrivals "
           f"are evenly spaced; a run is refused if any producer falls behind, or if any message is lost. "
           f"v1: Vyukov's bounded queue, slots packed; v2: each slot on its own 128 bytes. Baselines: "
           f"moodycamel `ConcurrentQueue` {s['baselines']['moodycamel']} through its token-free API (explicit "
           f"producer tokens, which its README calls almost always faster, need per-thread state the harness "
           f"does not carry), FIFO per producer only and not linearizable; Boost `lockfree::queue` "
           f"{s['baselines']['boost']}, the only one lock-free in the strict sense; rigtorp `MPMCQueue` "
           f"{s['baselines']['rigtorp']}, the same design as ours, slots padded to 64 bytes. v2 is the chosen "
           f"queue, `mpmc_ring`'s default. At 1 → 1, SPSC v3 is the reference.", ""]
    for i, sh in enumerate(s["shapes"]):
        cpus = sh["queues"][next(iter(sh["queues"]))]["one_way"]
        rows = []
        for q in MPMC_QUEUES:
            if q not in sh["queues"]:
                continue
            blocks = [d["mpmc"]["shapes"][i]["queues"][q]["one_way"] for _, d in runs]
            rows += [[MPMC_LABEL[q]] + r for r in load_rows(blocks, "one_way", len(runs))]
        out += [f"### {sh['producers']} → {sh['consumers']} (CPUs {', '.join(map(str, cpus['producer_cpus']))} → "
                f"{', '.join(map(str, cpus['consumer_cpus']))})", "",
                table(["Queue", "Rate", "Published", "p50", "p99", "p99.9", "p99.99"], rows), ""]
    return out


SIBLING_CELLS = {"R": "threads, ring at page offset 0 (note 006's layout)",
                 "A": "threads, one mapping", "B": "threads, two mappings",
                 "C": "processes, child attaches by name", "D": "processes, inherited mapping"}


def siblings_section(note, runs):
    s = runs[0][1]["siblings"]
    off = {k: c["ring_page_offset"] for k, c in s["pairs"][0]["cells"].items()}
    out = [f"## Hyper-thread siblings (note {note})", "",
           f"Why v3 across processes is slower than across threads on hyper-thread siblings (note 008). Cells A–D "
           f"use one shared-memory segment layout (ring at page offset {off['A']}) and vary only the address space "
           f"(threads or processes) and the virtual address (one mapping or two); R is note 006's thread version "
           f"(ring at page offset {off['R']}). Capacity {s['capacity']}, {s['message_bytes']}-byte messages, "
           f"{s['seconds_per_rate']:g} s per rate; every process's speculation controls identical (no STIBP). "
           f"Findings (note 012, Review): the virtual address makes no difference (A = B, C = D); one-way, the "
           f"ring's page offset costs ~6 ns (R against A) and processes add 0–3 ns at p50; round trip, processes "
           f"add 13–20 ns at p50 (C, D against A, B), with ~5× the memory-ordering machine clears and 2× the "
           f"STLB hits on the producer, which together explain only part of it.", ""]
    for i, p in enumerate(s["pairs"]):
        pair = (p["producer_cpu"], p["consumer_cpu"])
        where = "hyper-thread siblings" if pair[1] - pair[0] == 44 else "different cores, same socket: control"
        for mode, title in (("one_way", "one-way"), ("round_trip", "round trip")):
            rows = []
            for k in ("R", "A", "B", "C", "D"):
                if k in p["cells"]:
                    blocks = [d["siblings"]["pairs"][i]["cells"][k][mode] for _, d in runs]
                    rows += [[f"{k}: {SIBLING_CELLS[k]}"] + r for r in load_rows(blocks, mode, len(runs))]
            out += [f"### {pair[0]} → {pair[1]} ({where}), {title}", "",
                    table(["Cell", "Rate", "Published", "p50", "p99", "p99.9", "p99.99"], rows), ""]
    return out


SECTIONS = {"noise": noise_section, "clock": clock_section, "histogram": histogram_section,
            "load": load_section, "spsc": spsc_section, "shm": shm_section, "mpmc": mpmc_section,
            "siblings": siblings_section}


def results_md(sets):
    lines = ["# Results", "",
             "Generated by `python run.py report` from the runs listed in `results/published.toml`. "
             "Do not edit: `python run.py report --check` fails if this file and the JSON disagree. "
             "Latencies in ns unless marked µs; percentiles only.", ""]
    if not sets:
        lines += ["No published results: every set is being re-measured. The earlier runs did not control where "
                  "their memory lived, and the queues were on the far NUMA node (design note 008, Review).", ""]
        return "\n".join(lines).rstrip() + "\n"
    lines += conditions(sets)
    for name in SETS:
        if name in sets:
            note, runs = sets[name]
            lines += SECTIONS[name](note, runs)
    return "\n".join(lines).rstrip() + "\n"


# ---------------------------------------------------------------- README blocks


def readme_blocks(sets):
    blocks = {}
    if "spsc" in sets:
        runs = sets["spsc"][1]
        rows = []
        for q in ("spsc_v3", "moodycamel", "boost"):
            for rt in (1e6, 2e6):
                ok = [r for b in spsc_block(runs, (2, 3), q, "one_way") for r in b["runs"]
                      if r["rate_per_s"] == rt and r["ok"]]
                p = [r["one_way"]["percentiles_ns"] for r in ok]
                rows.append([QUEUE_LABEL[q], rate(rt), rng([x["p50"] for x in p]), rng([x["p99"] for x in p]),
                             rng([x["p999"] for x in p])])
        blocks["spsc-summary"] = (table(["Queue", "Rate", "p50", "p99", "p99.9"], rows) +
                                  f"\n\nOne-way latency in ns, CPUs 2 → 3 (separate cores), range over "
                                  f"{len(runs)} runs. Full tables and conditions: [RESULTS.md](RESULTS.md).\n")
    else:
        blocks["spsc-summary"] = ("SPSC figures are being re-measured after two fixes to the harness: memory "
                                  "placement, and false sharing in the load generator (design note 008, Review).\n")
    if "mpmc" in sets:
        runs = sets["mpmc"][1]
        shapes = runs[0][1]["mpmc"]["shapes"]
        rows = []
        for q in ("mpmc_v2", "moodycamel", "boost", "rigtorp"):
            for i, sh in enumerate(shapes):
                if (sh["producers"], sh["consumers"]) not in ((2, 1), (4, 4)):
                    continue
                for rt in (1e6, 2e6):
                    rs = [r for _, d in runs for r in d["mpmc"]["shapes"][i]["queues"][q]["one_way"]["runs"]
                          if r["rate_per_s"] == rt]
                    p = [r["one_way"]["percentiles_ns"] for r in rs if r["ok"]]
                    label = rate(rt) + ("" if len(p) == len(rs) else f" ({len(p)}/{len(rs)} runs)")
                    rows.append([MPMC_LABEL[q], f"{sh['producers']} → {sh['consumers']}", label,
                                 rng([x["p50"] for x in p]), rng([x["p99"] for x in p]), rng([x["p999"] for x in p])])
        blocks["mpmc-summary"] = (table(["Queue", "Producers → consumers", "Rate", "p50", "p99", "p99.9"], rows) +
                                  f"\n\nMPMC one-way latency in ns at the aggregate rate, range over {len(runs)} "
                                  f"runs; v2 is this repo's queue. moodycamel is measured through its token-free "
                                  f"API. Full tables and conditions: [RESULTS.md](RESULTS.md).\n")
    else:
        blocks["mpmc-summary"] = "MPMC figures: not yet measured (design note 010).\n"
    if "noise" in sets:
        s = sets["noise"][1][0][1]["noise_bench"]["summary"]
        blocks["noise-floor"] = (f"Noise floor on the reserved CPUs: at most "
                                 f"{pct(max(x['stopped_fraction_min_max'][1] for x in s))} of time stopped, longest "
                                 f"stop {ns_unit(max(x['longest_ns_min_max'][1] for x in s))}.\n")
    else:
        blocks["noise-floor"] = "Noise floor: being re-measured (design note 008, Review).\n"
    return blocks


def fill_readme(text, blocks):
    found = {m["name"] for m in BLOCK.finditer(text)}
    missing = sorted(set(blocks) - found)
    if missing:
        raise ReportError(f"README.md has no block for {missing}")
    return BLOCK.sub(lambda m: m.group(1) + blocks.get(m["name"], m["body"]) + m.group(4), text)


# ---------------------------------------------------------------- main


def report(root, check):
    sets = load_sets(root)
    results = results_md(sets)
    readme_path = root / "README.md"
    readme = readme_path.read_text()
    new_readme = fill_readme(readme, readme_blocks(sets))
    for text, what in ((results, "RESULTS.md"), (new_readme, "README.md")):
        found = MEAN.findall(text)
        if found:
            raise ReportError(f"{what}: generated text mentions {found} (S6)")
    if not check:
        (root / "RESULTS.md").write_text(results)
        readme_path.write_text(new_readme)
        print(f"wrote RESULTS.md and {len(readme_blocks(sets))} README block(s)")
        return 0
    diffs = []
    current = (root / "RESULTS.md").read_text() if (root / "RESULTS.md").is_file() else ""
    if current != results:
        diffs.append("RESULTS.md")
    old = {m["name"]: m["body"] for m in BLOCK.finditer(readme)}
    new = {m["name"]: m["body"] for m in BLOCK.finditer(new_readme)}
    diffs += [f"README.md block '{k}'" for k in new if old.get(k) != new[k]]
    if diffs:
        print("report --check: out of date: " + ", ".join(diffs) + " (run: python run.py report)")
        return 1
    print("report --check: RESULTS.md and README.md agree with the published JSON")
    return 0


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    sub = ap.add_subparsers(dest="cmd", required=True)
    rp = sub.add_parser("report", help="generate RESULTS.md and README blocks from published JSON")
    rp.add_argument("--check", action="store_true", help="compare only; exit 1 on any difference")
    rp.add_argument("--root", type=Path, default=Path(__file__).resolve().parent, help=argparse.SUPPRESS)
    args = ap.parse_args()
    try:
        return report(args.root, args.check)
    except ReportError as e:
        print(f"report: {e}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
