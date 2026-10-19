"""Tests for run.py report (design note 007, S1-S7).

Usage: run_py_test.py <repo root>

Builds a scratch repo from real published files, then checks that report and
--check behave, and that every refusal in the Spec fires: a tampered README
number or RESULTS.md, a missing file, an unpublished run, a dirty build, a set
mixing commits, and the word "average" reaching generated text.
"""
import json
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

SRC = Path(sys.argv[1]).resolve()
failures = 0


def check(cond, what):
    global failures
    print(("pass" if cond else "FAIL") + "  " + what)
    if not cond:
        failures += 1


SPSC = ["spsc-dev-lnx01-20261019T163730Z.json", "spsc-dev-lnx01-20261019T165847Z.json",
        "spsc-dev-lnx01-20261019T172005Z.json"]
NOISE = "noise-dev-lnx01-20261019T121236Z.json"
LOAD = "load-dev-lnx01-20261019T174123Z.json"


def manifest(spsc=SPSC, noise=NOISE, load=LOAD):
    q = lambda xs: "[" + ", ".join(f'"{x}"' for x in xs) + "]"
    return (f'[noise]\nnote = "005"\nruns = {q([noise])}\n\n[clock]\nnote = "001"\nruns = {q(spsc)}\n\n'
            f'[load]\nnote = "003"\nruns = {q([load])}\n\n[spsc]\nnote = "006"\nruns = {q(spsc)}\n')


README = "# x\n\n<!-- results:spsc-summary -->\n<!-- /results -->\n\n<!-- results:noise-floor -->\n<!-- /results -->\n"


def edit(root, name, change):
    path = root / "results" / name
    d = json.loads(path.read_text())
    change(d)
    path.write_text(json.dumps(d))


def scratch():
    root = Path(tempfile.mkdtemp(prefix="run_py_test_"))
    (root / "results").mkdir()
    (root / "tests").mkdir()
    shutil.copy(SRC / "run.py", root)
    shutil.copy(SRC / "tests" / "check_environment_json.py", root / "tests")
    for f in SPSC + [NOISE, LOAD]:
        shutil.copy(SRC / "results" / f, root / "results")
        # These real files predate note 008's placement fix: add the two fields
        # the report now requires, so the copies exercise run.py's logic. They
        # are test fixtures here, never published.
        def upgrade(d):
            d["environment"]["placement"]["memory_policy"] = "bind 0"
            blocks = []
            if "load" in d:
                blocks = list(d["load"].values())
            if "spsc" in d:
                blocks = [b for p in d["spsc"]["pairs"] for q in p["queues"].values() for b in q.values()]
            for b in blocks:
                for r in b["runs"]:
                    r["memory_node"] = 0
        edit(root, f, upgrade)
    (root / "results" / "published.toml").write_text(manifest())
    (root / "README.md").write_text(README)
    return root


def run(root, *args):
    p = subprocess.run([sys.executable, "-I", str(root / "run.py"), "report", "--root", str(root), *args],
                       capture_output=True, text=True)
    return p.returncode, p.stdout + p.stderr


root = scratch()
try:
    # S1, S2: generate, then check passes; outputs only from the manifest's files.
    code, out = run(root)
    check(code == 0, "report writes RESULTS.md and the README blocks")
    code, out = run(root, "--check")
    check(code == 0, "S2 --check passes on fresh output")
    readme = (root / "README.md").read_text()
    check("| **v3** | 1M/s |" in readme and "Noise floor on the reserved CPUs" in readme, "S1 README blocks filled")
    results = (root / "RESULTS.md").read_text()
    check(all(f"## {h}" in results for h in ("Conditions", "Noise floor", "Clock", "Mailbox load", "SPSC queues"))
          and "## Histogram" not in results, "S1 RESULTS.md has exactly the manifest's sets")

    # S2: a tampered README number, then a tampered RESULTS.md.
    (root / "README.md").write_text(readme.replace("| **v3** | 1M/s | ", "| **v3** | 1M/s | 1", 1))
    code, out = run(root, "--check")
    check(code == 1 and "README.md block 'spsc-summary'" in out, "S2 a changed README number fails, naming the block")
    (root / "README.md").write_text(readme)
    (root / "RESULTS.md").write_text(results.replace("| 2.1974", "| 2.1975", 1))
    code, out = run(root, "--check")
    check(code == 1 and "RESULTS.md" in out, "S2 a changed RESULTS.md fails")
    (root / "RESULTS.md").write_text(results)
    check(run(root, "--check")[0] == 0, "restored: --check passes again")

    # S3: missing, unpublished, dirty.
    (root / "results" / "published.toml").write_text(manifest(load="load-missing.json"))
    code, out = run(root, "--check")
    check(code == 1 and "load-missing.json: missing" in out, "S3 a missing file stops the report")
    (root / "results" / "published.toml").write_text(manifest())

    shutil.copy(root / "results" / LOAD, root / "results" / "load-unpub.json")
    edit(root, "load-unpub.json", lambda d: d["environment"].update(published=False))
    (root / "results" / "published.toml").write_text(manifest(load="load-unpub.json"))
    code, out = run(root, "--check")
    check(code == 1 and "not a published run" in out, "S3 an unpublished run stops the report")

    shutil.copy(root / "results" / LOAD, root / "results" / "load-dirty.json")
    edit(root, "load-dirty.json", lambda d: d["environment"]["build"].update(clean=False))
    (root / "results" / "published.toml").write_text(manifest(load="load-dirty.json"))
    code, out = run(root, "--check")
    check(code == 1 and "uncommitted changes" in out, "S3 a dirty build stops the report")
    (root / "results" / "published.toml").write_text(manifest())

    # S4: a set mixing commits.
    shutil.copy(root / "results" / SPSC[2], root / "results" / "spsc-other.json")
    edit(root, "spsc-other.json", lambda d: d["environment"]["build"].update(commit="0" * 40))
    (root / "results" / "published.toml").write_text(manifest(spsc=SPSC[:2] + ["spsc-other.json"]))
    code, out = run(root, "--check")
    check(code == 1 and "mix more than one commit" in out, "S4 a set mixing commits stops the report")
    (root / "results" / "published.toml").write_text(manifest())

    # S6: no mean in generated text (the mailbox's description is printed).
    shutil.copy(root / "results" / LOAD, root / "results" / "load-avg.json")
    edit(root, "load-avg.json", lambda d: d["load"]["one_way"].update(sut="average mailbox"))
    (root / "results" / "published.toml").write_text(manifest(load="load-avg.json"))
    code, out = run(root, "--check")
    check(code == 1 and "(S6)" in out, "S6 'average' reaching generated text stops the report")
finally:
    shutil.rmtree(root)

print("FAILED" if failures else "all passed")
sys.exit(1 if failures else 0)
