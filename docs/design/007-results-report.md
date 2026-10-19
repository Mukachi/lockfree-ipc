# 007 — Results report

**Status:** Draft · **Updated:** 2026-10-19

## Proposal

The rule is one number everywhere: the README, `RESULTS.md` and the committed JSON agree, and
`python run.py report --check` proves it. Today nothing enforces it. Every figure so far lives in
JSON files and in hand-typed Review rows.

`run.py report` generates `RESULTS.md` from the committed JSON, and the README's figures from the
same data. `run.py report --check` regenerates both in memory and fails if either differs from
what is committed. A figure cannot then be typed, rounded differently, or left stale.

It does:

- read only the result files a manifest names, and validate each before using it;
- write `RESULTS.md`: every published component's figures, with the conditions they were measured
  under;
- fill marked blocks in the README from the same data;
- check, without writing, that both are current.

It does not:

- run benchmarks (that stays `sudo bench-on && sudo bench-run …`);
- judge results. It reports what the files say, refusals included;
- draw charts. Tables only, for now.

## Design

**Manifest: `results/published.toml`.** `results/` holds every run, superseded ones included
(eight SPSC files, three of them the publishable set). Which files are published is a decision,
so it is written down, reviewable in a diff, not inferred from names or dates:

```toml
[spsc]
note = "006"
runs = ["spsc-dev-lnx01-20261019T163730Z.json",
        "spsc-dev-lnx01-20261019T165847Z.json",
        "spsc-dev-lnx01-20261019T172005Z.json"]
```

One table per component: `noise`, `clock`, `histogram`, `load`, `spsc`, each with the note it
belongs to and its runs.

**Validation, before any number is read.** Every listed file must: parse; pass the existing
checkers (`check_environment_json.py` and the component's own); be published (`published: true,
ok: true`); come from a clean build. A set of runs must share one commit, one machine and one
kernel command line: a range must not mix conditions. Any failure stops the report with the file
and the reason.

**Ranges.** A set's figure is the minimum–maximum over its runs, one value if they agree. A rate
refused in some runs shows how many published (e.g. "2/3"); a rate refused in all shows
"refused" and the reason's kind (generator ceiling, lateness). Nothing is dropped silently.
Percentiles only, never a mean.

**`RESULTS.md`,** generated, never edited by hand (a header says so):

1. Conditions: machine, CPU, microcode, kernel and command line (isolation), bench-mode settings
   (governor, idle states, uncore, fence, NUMA balancing, turbo), commit, compiler and flags, and
   for each table its pairs, rates, sample counts and warm-up discarded.
2. The noise floor (note 005), clock (001), histogram record cost (002), mailbox load (003).
3. SPSC (006): one-way and round trip, every queue, both pairs, every rate, p50 / p99 / p99.9 /
   p99.99, as ranges.

**README blocks.** The README's figures sit between markers:

```markdown
<!-- results:spsc-summary -->
...generated table...
<!-- /results -->
```

`report` replaces each block's contents; `--check` compares them. Prose outside the blocks is
untouched. Proposed README content: one table of v3 against moodycamel and Boost, one-way 2→3 at
1M/s and 2M/s, p50 / p99 / p99.9; and one line for the noise floor. Everything else in
`RESULTS.md`, linked.

**`run.py`** at the repo root: Python standard library only (`json`, `tomllib`, `argparse`);
`report` writes, `report --check` exits non-zero on any difference and prints which block or
section differs.

## Spec

- S1. `RESULTS.md` and the README blocks are generated only from files the manifest names.
- S2. `report --check` fails if `RESULTS.md` or any README block differs from what the committed
  JSON generates, naming the block.
- S3. A manifest file that is missing, unpublished, refused, from a dirty build, or failing its
  checker stops the report, naming the file and the reason.
- S4. A set mixing commits, machines or kernel command lines stops the report.
- S5. Figures are min–max over a set's runs; partial and full refusals are shown, never dropped.
- S6. No mean appears in `RESULTS.md` or the README blocks.
- S7. Standard library only; tested under CTest with a synthetic manifest and results, including a
  tampered README number that `--check` must catch.

## Tasks

- [ ] `results/published.toml` for the current published runs
- [ ] `run.py report` and `--check`, with S1–S7 tests
- [ ] `RESULTS.md` generated; README blocks added; `--check` passing
- [ ] `--check` added to CTest, so a stale number fails the build's tests

## Review

| Date | Stage | Finding |
|---|---|---|
| 2026-10-19 | Draft | Written for review |
