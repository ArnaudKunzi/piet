#!/usr/bin/env python3
# ctest gate for factor-variable interactions (i. / c. / # / ##): run cobra feols
# with interaction terms on the fvint.hbin fixture and check every coefficient
# against the Stata golden (fvint_golden.csv from fvint_golden.do).
import json, subprocess, sys, os

from _cobra_bin import resolve_cobra  # noqa: E402  (sibling module; see it for why)

COBRA = resolve_cobra()
# FULL FUNCTIONAL CHECK (2026-08-05): $COBRA_VALIDATE_DIR wins over the
# CMake-configured argv[2]. The goldens and .hbin fixtures this reads are
# gitignored, so in a fresh worktree argv[2] points at that worktree's own
# EMPTY tests/validate and the test fails for a reason unrelated to the
# code under test — every P7 agent hit this and none could work around it.
# The env var is the same override name the C++ validators already honour
# (CMakeLists.txt COBRA_VALIDATE_DIR compile definition); these three were
# simply never wired to it.
VDIR = (os.environ.get("COBRA_VALIDATE_DIR")
        or (sys.argv[2] if len(sys.argv) > 2 else None)
        or os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from _fixtures import need  # noqa: E402 (GA sweep: honest SKIP on absent fixtures)
HBIN = need(os.path.join(VDIR, "fvint.hbin"))
fails = []

g = {}
with open(need(os.path.join(VDIR, "fvint_golden.csv"))) as f:
    next(f)
    for line in f:
        line = line.strip()
        if not line or "," not in line: continue
        k, v = line.rsplit(",", 1)
        g[k.strip()] = float(v)

def run(xspec):
    r = subprocess.run([COBRA, "feols", "--y", "y", "--x", xspec,
                        "--data", HBIN, "--no-provenance"],
                       capture_output=True, text=True)
    if r.returncode != 0:
        raise RuntimeError(f"feols --x {xspec} -> exit {r.returncode}: {r.stderr[:200]}")
    return json.loads(r.stdout)["coef"]

def chk(label, got, want, tol=1e-6):
    ok = abs(got) < 1e-9 if want == 0 else abs(got - want) / abs(want) <= tol
    print(f"  {'ok ' if ok else 'FAIL'} {label}: got={got:.10g} want={want:.10g}")
    if not ok: fails.append(label)

# BUG-101: cobra factor-term names are Stata-convention `level.var` (e.g. 1.treat,
# 2.g3, 1.treat#1.post) — NOT the old `var==level`. The golden CSV keys carry a
# cN. spec-prefix over that same convention.

# (1) i(treat)##i(post)
c = run("i(treat)##i(post)")
chk("c1 1.treat",           c["1.treat"],             g["c1.1.treat"])
chk("c1 1.post",            c["1.post"],              g["c1.1.post"])
chk("c1 1.treat#1.post",    c["1.treat#1.post"],      g["c1.1.treat#1.post"])
chk("c1 _cons",             c["(intercept)"],         g["c1._cons"])

# (2) c(x)##c(x) — x and x squared
c = run("c(x)##c(x)")
chk("c2 x",   c["x"],     g["c2.x"])
chk("c2 x#x", c["x#x"],   g["c2.x#x"])

# (3) i(g3)##c(x) — group main + slope interactions
c = run("i(g3)##c(x)")
chk("c3 2.g3",     c["2.g3"],      g["c3.2.g3"])
chk("c3 3.g3",     c["3.g3"],      g["c3.3.g3"])
chk("c3 x",        c["x"],         g["c3.x"])
chk("c3 2.g3#x",   c["2.g3#x"],    g["c3.2.g3#x"])
chk("c3 3.g3#x",   c["3.g3#x"],    g["c3.3.g3#x"])

# (4) i(treat)#i(post) — interaction only
c = run("i(treat)#i(post)")
chk("c4 1.treat#1.post", c["1.treat#1.post"], g["c4.1.treat#1.post"])

# (5) i(g3)##i(post) — 3x2 fully populated
c = run("i(g3)##i(post)")
chk("c5 2.g3",           c["2.g3"],             g["c5.2.g3"])
chk("c5 3.g3",           c["3.g3"],             g["c5.3.g3"])
chk("c5 1.post",         c["1.post"],           g["c5.1.post"])
chk("c5 2.g3#1.post",    c["2.g3#1.post"],      g["c5.2.g3#1.post"])
chk("c5 3.g3#1.post",    c["3.g3#1.post"],      g["c5.3.g3#1.post"])

print(f"\n{'FAILED: ' + ', '.join(fails) if fails else 'PASSED (factor-var i./c./#/## vs Stata golden)'}")
sys.exit(1 if fails else 0)
