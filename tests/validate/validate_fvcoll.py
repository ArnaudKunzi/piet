#!/usr/bin/env python3
# ctest gate for Stata-matching collinear-column omission in factor-variable
# interactions. `regress price i.foreign##i.rep78` on sysuse auto is rank
# deficient: foreign cars have rep78 in {3,4,5} only, so 1.foreign equals the sum
# of 1.foreign#{3,4,5}.rep78 EXACTLY (on top of the structurally-empty cell
# 1.foreign#2.rep78). Stata omits the LATER collinear term, 1.foreign#5.rep78,
# and reports it with coefficient 0. This test confirms cobra (a) omits exactly
# that term, (b) reports it with coef 0, and (c) matches every surviving Stata
# coefficient to ~1e-9 — see fvauto_golden.do for the golden.
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
HBIN = need(os.path.join(VDIR, "auto.hbin"))
fails = []

# Stata golden: surviving + omitted coefs (key -> value). Omitted term has value 0.
g = {}
with open(need(os.path.join(VDIR, "fvauto_golden.csv"))) as f:
    next(f)
    for line in f:
        line = line.strip()
        if not line or "," not in line: continue
        k, v = line.rsplit(",", 1)
        g[k.strip()] = float(v)

r = subprocess.run([COBRA, "feols", "--y", "price", "--x", "i(foreign)##i(rep78)",
                    "--data", HBIN, "--no-provenance"], capture_output=True, text=True)
if r.returncode != 0:
    print(f"cobra feols exited {r.returncode}: {r.stderr[:300]}")
    sys.exit(1)
d = json.loads(r.stdout)
coef = d["coef"]
omitted = d.get("omitted", [])


def chk(label, cond, detail=""):
    print(f"  {'ok ' if cond else 'FAIL'} {label}{('  ' + detail) if detail else ''}")
    if not cond: fails.append(label)


def chk_close(label, got, want, tol=1e-9):
    ok = abs(got) < 1e-7 if want == 0 else abs(got - want) / abs(want) <= tol
    chk(label, ok, f"got={got:.10g} want={want:.10g}")


# (a) the omitted set is exactly {1.foreign#5.rep78} (BUG-101: cobra now names
#     factor terms in Stata `level.var` convention, matching Stata exactly).
chk("omitted set == {1.foreign#5.rep78}", omitted == ["1.foreign#5.rep78"],
    f"omitted={omitted}")

# (b) the omitted term is present in coef with value 0 (Stata-style)
chk("omitted term reported coef 0", abs(coef.get("1.foreign#5.rep78", 1.0)) < 1e-12,
    f"coef={coef.get('1.foreign#5.rep78')}")

# (c) the sample/dof match Stata (N=69 after listwise drop of 5 missing rep78;
#     rank 8 -> 7 slope df, residual df 61).
chk("n_obs == 69", d["n_obs"] == 69, f"n_obs={d['n_obs']}")
chk("dof_resid == 61", d["dof_resid"] == 61, f"dof_resid={d['dof_resid']}")

# (d) every Stata coefficient (surviving + the omitted 0) matches cobra to ~1e-9.
for k, want in g.items():
    key = "(intercept)" if k == "_cons" else k
    if key not in coef:
        chk(f"coef[{k}] present", False, "MISSING from cobra output")
        continue
    chk_close(f"coef[{k}]", coef[key], want)

print(f"\n{'FAILED: ' + ', '.join(fails) if fails else 'PASSED (collinear-omission vs Stata regress golden)'}")
sys.exit(1 if fails else 0)
