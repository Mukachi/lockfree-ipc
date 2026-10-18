"""S6 and S8, checked in the compiled code. Design: docs/design/001-clock.md

Usage: check_hot_path.py <objdump> <hot_path_subject binary>
Checked against a deliberately broken subject: both checks fail on it.

Histogram S1 (note 002): record() contains no call or system call, so it
cannot allocate or enter the kernel, and exactly one conditional branch (the
overflow check), as the note's Design states.

Load S6 (note 003): each generator thread body (the run_load_impl lambdas'
_M_run, one-way and echo variants) calls only sched_setaffinity, for pinning
before its loop, and __stack_chk_fail, the stack protector's abort. The SUT,
the histogram and the clock reads are all inlined into the loops.

S6: in the overhead loop, every window from a start() read (rdtsc) to the
next stop() read (rdtscp) holds only fences and register moves: no call,
system call, division or floating-point conversion.
S8: in the cross-CPU threads, every rdtsc is fenced: going back from it, an
lfence comes before any memory access or branch (register-only moves the
compiler schedules in between are harmless). This rules out the unfenced
stamp(). Each thread also has a send read followed directly by an lfence, so
the read cannot move past the store.
"""
import re
import subprocess
import sys

WINDOW_ALLOWED = {"lfence", "mov", "shl", "or", "nop", "xchg"}

failures = 0


def check(cond, what):
    global failures
    print(("pass" if cond else "FAIL") + "  " + what)
    if not cond:
        failures += 1


PREFIXES = {"data16", "cs", "ds", "rex", "rex.W", "lock", "notrack", "bnd"}


def functions(objdump, binary):
    """{function name: [(mnemonic, operands)]} from objdump's AT&T output."""
    out = subprocess.run([objdump, "-d", "-C", "--no-show-raw-insn", binary],
                         check=True, capture_output=True, text=True).stdout
    funcs, name = {}, None
    for line in out.splitlines():
        m = re.match(r"^[0-9a-f]+ <(.*)>:$", line)
        if m:
            name = m[1]
            funcs[name] = []
            continue
        m = re.match(r"^\s+[0-9a-f]+:\s+(.*)$", line)
        if m and name is not None:
            words = m[1].split()
            while words and words[0] in PREFIXES:
                words.pop(0)
            if words:
                funcs[name].append((words[0], " ".join(words[1:])))
    return funcs


def fenced_before(ins, i):
    """True if, walking back from ins[i], an lfence comes before any memory access or branch."""
    for mnem, ops in reversed(ins[:i]):
        if mnem == "lfence":
            return True
        if "(" in ops or mnem.startswith("j") or mnem in ("call", "ret", "pause"):
            return False
    return False


def base(m):
    # Strip AT&T size suffixes so movq/movl/orq/shlq compare as mov/or/shl.
    for b in WINDOW_ALLOWED:
        if m == b or (m.startswith(b) and len(m) == len(b) + 1 and m[-1] in "bwlq"):
            return b
    return m


funcs = functions(sys.argv[1], sys.argv[2])

# S6
ov = funcs.get("lfipc_s6_overhead", [])
windows, bad = 0, []
for i, (m, _) in enumerate(ov):
    if m != "rdtsc":
        continue
    j = i + 1
    while j < len(ov) and ov[j][0] != "rdtscp":
        mnem, ops = ov[j]
        nop = mnem.startswith("nop") or mnem == "xchg"
        if not nop and (base(mnem) not in WINDOW_ALLOWED or "(" in ops):
            bad.append(f"{mnem} {ops}".strip())
        j += 1
    if j < len(ov):
        windows += 1
check(windows >= 2, f"S6 timed windows found in the overhead loop ({windows})")
check(not bad, "S6 only fences and register moves between start() and stop()"
      + (f": found {sorted(set(bad))}" if bad else ""))

# S8
lambdas = {n: ins for n, ins in funcs.items()
           if "measure_pair" in n and "lambda" in n and any(m == "rdtsc" for m, _ in ins)}
check(len(lambdas) >= 2, f"S8 both cross-CPU threads found ({len(lambdas)})")
for n, ins in lambdas.items():
    who = re.search(r"lambda\(\)#\d+", n)[0]
    reads = [i for i, (m, _) in enumerate(ins) if m == "rdtsc"]
    check(all(fenced_before(ins, i) for i in reads), f"S8 {who}: every read is fenced ({len(reads)} reads)")
    check(any(i + 1 < len(ins) and ins[i + 1][0] == "lfence" for i in reads),
          f"S8 {who}: send read has an lfence after it")

# Histogram S1
rec = funcs.get("lfipc_hist_record", [])
calls = [f"{m} {o}" for m, o in rec if m in ("call", "syscall", "sysenter", "int") or (m == "jmp" and "@plt" in o)]
check(len(rec) > 0, f"histogram record() found ({len(rec)} instructions)")
check(not calls, "histogram S1 record() makes no call or system call" + (f": found {calls}" if calls else ""))
branches = [m for m, _ in rec if m.startswith("j") and m != "jmp"]
check(len(branches) == 1, f"histogram record() has one conditional branch, the overflow check ({len(branches)}: {branches})")

# Load S6
ALLOWED_CALLS = {"sched_setaffinity@plt", "__stack_chk_fail@plt"}
bodies = {n: ins for n, ins in funcs.items() if "run_load_impl" in n and "lambda" in n and "_M_run" in n}
check(len(bodies) >= 4, f"load generator thread bodies found, both variants ({len(bodies)})")
for n, ins in sorted(bodies.items()):
    who = ("echo " if "run_load_impl<true" in n else "one-way ") + re.search(r"lambda\(\)#\d+", n)[0]
    targets = {re.search(r"<([^>]*)>", o)[1] for m, o in ins
               if (m == "call" or (m == "jmp" and "@plt" in o)) and "<" in o}
    extra = sorted(targets - ALLOWED_CALLS)
    check(not extra, f"load S6 {who}: no calls but pinning" + (f": found {extra}" if extra else ""))

print("FAILED" if failures else "all passed")
sys.exit(1 if failures else 0)
