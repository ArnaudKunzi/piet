#!/usr/bin/env python3
# BUG-112(6) ctest gate: omitted (collinear) coef-map terms now land at their
# Stata-natural design-matrix position instead of being tail-appended.
#
# `add_omitted` (apps/cmds/common.cpp) gained an optional `full_names` param —
# the pre-collinearity-drop column order, threaded from a new `full_names`
# field on FeolsResult/OlsResult (captured at the one point in each estimator
# where that order still exists, before `compact_columns` — src/core/
# collinear.h — mutates it away). This gate is only discriminating if the
# DROPPED column sits in the MIDDLE of the full order: x1/x2/x3 with x2 an
# exact scalar multiple of x1 (collinear_keep_mask drops the LATER of a
# collinear pair, so x2 is omitted) — the old tail-append behavior and the
# new natural-position behavior produce visibly DIFFERENT key orders for this
# fixture (a collinear column already at the tail would look identical either
# way and prove nothing).
#
# Exercises all 3 add_omitted call sites: cmd_ols (cmds_linear.cpp), run_feols
# and run_feols_multi (both common.cpp, single-y vs --y y1,y2 batch).
#
# Self-contained (builds its own CSV fixture; no external data dependency).
# usage: validate_bug112_addomitted_order.py <cobra-binary>
import json, os, subprocess, sys, tempfile

from _cobra_bin import resolve_cobra  # noqa: E402  (sibling module; see it for why)

COBRA = resolve_cobra()
fails = []


def run_ok(args, label):
    r = subprocess.run([COBRA] + args, capture_output=True, text=True)
    if r.returncode != 0:
        print(f"  cobra failed ({label}):", " ".join(args), "\n", r.stderr, file=sys.stderr)
        fails.append(label)
        return None
    return json.loads(r.stdout)


def check(name, cond):
    print(("  ok   " if cond else "  FAIL ") + name)
    if not cond:
        fails.append(name)


work = tempfile.mkdtemp(prefix="bug112_addomitted_order_")
csv_path = os.path.join(work, "d.csv")
hbin = os.path.join(work, "d.hbin")

# Equal-scale duplicate x2 = x1 (original-order tie); x3 varies
# independently. 30 rows — comfortably n > k for both ols and feols.
rows = []
for i in range(1, 31):
    x1 = 0.3 * i
    x2 = x1
    x3 = 0.15 * i * i
    y1 = 5.0 + 0.4 * x1 + 0.1 * x3 + 0.01 * i
    y2 = 2.0 - 0.2 * x1 + 0.05 * x3 + 0.02 * i
    rows.append((i, x1, x2, x3, y1, y2))
with open(csv_path, "w") as f:
    f.write("id,x1,x2,x3,y1,y2\n")
    for r in rows:
        f.write(",".join(str(v) for v in r) + "\n")

conv = subprocess.run([COBRA, "convert", csv_path, hbin], capture_output=True, text=True)
if conv.returncode != 0:
    print("convert failed:", conv.stderr, file=sys.stderr)
    sys.exit(1)

NATURAL = ["(intercept)", "x1", "x2", "x3"]


def check_natural_order(coef, label):
    names = list(coef.keys())
    check(f"{label}: 4 coef entries (intercept+x1+x2+x3, x2 omitted-not-dropped)",
          len(names) == 4)
    check(f"{label}: natural design-matrix order {NATURAL}, got {names}",
          names == NATURAL)
    check(f"{label}: x2 (the omitted/collinear term) is coef 0.0, not survivor-fit",
          coef.get("x2") == 0.0)


# --- ols ---
j_ols = run_ok(["ols", "--data", hbin, "--y", "y1", "--x", "x1,x2,x3"], "ols")
if j_ols is not None:
    check("ols: omitted lists x2", j_ols.get("omitted") == ["x2"])
    check_natural_order(j_ols.get("coef", {}), "ols coef")

# --- feols (single-y path: run_feols) ---
j_feols = run_ok(["feols", "--data", hbin, "--y", "y1", "--x", "x1,x2,x3"], "feols single")
if j_feols is not None:
    check("feols: omitted lists x2", j_feols.get("omitted") == ["x2"])
    check_natural_order(j_feols.get("coef", {}), "feols single coef")

# --- feols --y y1,y2 (multi-outcome batch path: run_feols_multi) ---
j_multi = run_ok(["feols", "--data", hbin, "--y", "y1,y2", "--x", "x1,x2,x3"], "feols multi")
if j_multi is not None:
    outcomes = j_multi.get("outcomes", [])
    check("feols multi: 2 outcomes", len(outcomes) == 2)
    for idx, oc in enumerate(outcomes):
        check(f"feols multi[{idx}]: omitted lists x2", oc.get("omitted") == ["x2"])
        check_natural_order(oc.get("coef", {}), f"feols multi[{idx}] coef")

# --- full-rank control: no omitted term ⇒ byte-identical shape (no "omitted"
# key at all), full_names threading must be a strict no-op on the happy path.
j_full_rank = run_ok(["ols", "--data", hbin, "--y", "y1", "--x", "x1,x3"], "ols full-rank")
if j_full_rank is not None:
    check("ols full-rank: no 'omitted' key", "omitted" not in j_full_rank)
    check("ols full-rank: coef order unaffected", list(j_full_rank.get("coef", {}).keys()) ==
          ["(intercept)", "x1", "x3"])

if fails:
    print("FAILED:", ", ".join(fails))
    sys.exit(1)
print("PASSED")
