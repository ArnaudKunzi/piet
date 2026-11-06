#!/usr/bin/env python3
# BUG-112(8) ctest gate: `ib#.` naming a base level that is absent from the data
# is no longer SILENT — the response carries a top-level "warnings" array — while
# the estimates themselves are unchanged.
#
# ORACLE (StataBE 19.5, 2026-07-31, `sysuse auto`, rep78 in {1..5}; 5 arms —
# regress×ib99. / regress×ib2. / regress×ibn. / logit×ib99. / fvset base 99):
#   * every arm returns _rc == 0 — Stata does NOT reject an absent base, so a
#     hard-error design was ruled out by measurement, not by taste;
#   * the absent-base arms emit `note: 99b.rep78 identifies no observations in
#     the sample.` — i.e. warn-and-proceed;
#   * the COEFFICIENTS of `ib99.rep78` equal those of `ibn.rep78` exactly
#     (1.rep78=-1348.5, 2.=54.625, 3.=516.2333, 4.=158.5, _cons=5913) — Stata
#     keeps 99 as an empty base column and omits the last level for collinearity,
#     cobra re-bases to the last level; same estimates, different term inventory.
#     The inventory gap is a SEPARATE filing; this gate pins the note + the
#     estimate-invariance, which is exactly what item (8) asked for.
#
# Discriminating by construction: arm 1 fails if the warning is not produced
# (the pre-fix behavior), arms 3-5 fail if the key leaks onto clean responses
# (which is what would force a re-capture of the 127-golden corpus).
#
# usage: validate_bug112_ibwarn.py <cobra-binary>
import json, os, subprocess, sys, tempfile

from _cobra_bin import resolve_cobra  # noqa: E402  (sibling module; see it for why)

COBRA = resolve_cobra()
HERE = os.path.dirname(os.path.abspath(__file__))
AUTO = os.path.join(HERE, "auto.hbin")
fails = []


def run(args, label, expect_rc=0):
    r = subprocess.run([COBRA] + args, capture_output=True, text=True)
    if r.returncode != expect_rc:
        print(f"  cobra rc={r.returncode} (expected {expect_rc}) [{label}]:",
              " ".join(args), "\n", r.stderr, file=sys.stderr)
        fails.append(label + " (rc)")
        return None
    try:
        return json.loads(r.stdout)
    except json.JSONDecodeError:
        print(f"  unparseable stdout [{label}]:", r.stdout[:400], file=sys.stderr)
        fails.append(label + " (json)")
        return None


def check(name, cond):
    print(("  ok   " if cond else "  FAIL ") + name)
    if not cond:
        fails.append(name)


def one_warning(j, label):
    """The response carries exactly one warning; return it (or None + a FAIL)."""
    w = j.get("warnings")
    check(f"{label}: 'warnings' key present", isinstance(w, list))
    if not isinstance(w, list):
        return None
    check(f"{label}: exactly one warning, got {len(w)}", len(w) == 1)
    return w[0] if w else None


# ── fixture: rep in {1..5}, mirroring auto's rep78 level set ───────────────────
work = tempfile.mkdtemp(prefix="bug112_ibwarn_")
csv_path = os.path.join(work, "d.csv")
hbin = os.path.join(work, "d.hbin")
rows = []
for i in range(60):
    rep = (i % 5) + 1                      # levels 1..5, all present, none is 99
    x = 0.25 * i
    y = 3.0 + 0.7 * rep + 0.4 * x + 0.05 * (i % 7)
    d = 1 if (i % 3 == 0) else 0           # binary outcome for the logit arm
    z = 0.5 * x + 0.3 * (i % 4)            # instrument for the IV arm
    e = 0.2 * x + 0.1 * (i % 5)            # endogenous regressor for the IV arm
    rows.append((i, rep, x, y, d, z, e))
with open(csv_path, "w") as f:
    f.write("id,rep,x,y,d,z,e\n")
    for r in rows:
        f.write(",".join(str(v) for v in r) + "\n")
conv = subprocess.run([COBRA, "convert", csv_path, hbin], capture_output=True, text=True)
if conv.returncode != 0:
    print("convert failed:", conv.stderr, file=sys.stderr)
    sys.exit(1)

# ── arm 1: absent base warns, and does NOT reject (oracle: _rc == 0) ──────────
j_absent = run(["feols", "--data", hbin, "--y", "y", "--x", "ib99.rep,x"], "feols ib99")
if j_absent is not None:
    w = one_warning(j_absent, "feols ib99")
    if w is not None:
        check(f"feols ib99: warning names the REQUESTED base 99, got {w!r}", "99" in w)
        check(f"feols ib99: warning names the variable, got {w!r}", "rep" in w)
        check(f"feols ib99: warning names the base actually USED (5), got {w!r}",
              "5" in w.split(";")[-1])
        check(f"feols ib99: Stata's wording is mirrored, got {w!r}",
              "identifies no observations in the sample" in w)

# ── arm 2: the estimates are UNCHANGED — this is a signal-only fix ────────────
j_ibn = run(["feols", "--data", hbin, "--y", "y", "--x", "ibn.rep,x"], "feols ibn")
if j_absent is not None and j_ibn is not None:
    check("feols ib99 coef == feols ibn coef (fallback numerics untouched)",
          j_absent.get("coef") == j_ibn.get("coef"))
    check("feols ib99 se == feols ibn se",
          j_absent.get("se") == j_ibn.get("se"))

# ── arms 3-5: clean responses carry NO 'warnings' key (absent-when-empty) ─────
# This is what keeps the 127-golden corpus byte-identical; if the key ever became
# unconditional these arms fail before any golden re-capture is even considered.
if j_ibn is not None:
    check("feols ibn: no 'warnings' key (no absent base)", "warnings" not in j_ibn)
j_ib2 = run(["feols", "--data", hbin, "--y", "y", "--x", "ib2.rep,x"], "feols ib2")
if j_ib2 is not None:
    check("feols ib2 (present base): no 'warnings' key", "warnings" not in j_ib2)
j_plain = run(["feols", "--data", hbin, "--y", "y", "--x", "i.rep,x"], "feols i.")
if j_plain is not None:
    check("feols i. (default base): no 'warnings' key", "warnings" not in j_plain)

# ── arm 6: same kernel, every OTHER wired family ─────────────────────────────
# Measured reachability, not assumed: `ols` and `logit` take BARE column names in
# --x and reject `i.`/`ib#.` outright ("column not found: i.rep"), so the factor
# kernel is unreachable from them and they are correctly absent here. The four
# families below all parse fv terms and are the ones add_warnings is wired into.
j_fepois = run(["fepois", "--data", hbin, "--y", "d", "--x", "ib99.rep,x"], "fepois ib99")
if j_fepois is not None:
    one_warning(j_fepois, "fepois ib99")
j_iv = run(["iv", "--data", hbin, "--y", "y", "--x", "ib99.rep",
            "--endog", "e", "--instruments", "z"], "iv ib99")
if j_iv is not None:
    one_warning(j_iv, "iv ib99")
j_fmulti = run(["feols", "--data", hbin, "--y", "y,d", "--x", "ib99.rep,x"], "feols multi ib99")
if j_fmulti is not None:
    one_warning(j_fmulti, "feols multi ib99")
j_wb = run(["wildboot", "--data", hbin, "--y", "y", "--x", "ib99.rep,x",
            "--param", "x", "--cluster", "rep", "--reps", "20"], "wildboot ib99")
if j_wb is not None:
    one_warning(j_wb, "wildboot ib99")

# ── arm 7: two absent bases ⇒ two warnings (per-part, not per-response) ───────
j_two = run(["feols", "--data", hbin, "--y", "y", "--x", "ib99.rep,ib98.rep"], "feols two-absent")
if j_two is not None:
    w = j_two.get("warnings")
    check("feols two absent bases: 2 warnings", isinstance(w, list) and len(w) == 2)

# ── arm 8: oracle tie-back on the real auto fixture (the numbers in the log) ──
# The StataBE run above used `sysuse auto`; tests/validate/auto.hbin is the same
# data. Pinning the coefficients here is what ties this gate to the oracle rather
# than to cobra's own opinion.
if os.path.exists(AUTO):
    j_auto = run(["feols", "--data", AUTO, "--y", "price", "--x", "ib99.rep78"], "auto ib99")
    if j_auto is not None:
        one_warning(j_auto, "auto ib99")
        coef = j_auto.get("coef", {})
        ORACLE = {"1.rep78": -1348.5, "2.rep78": 54.625, "3.rep78": 516.2333333333333,
                  "4.rep78": 158.5, "(intercept)": 5913.0}
        for k, v in ORACLE.items():
            got = coef.get(k)
            check(f"auto ib99: {k} == StataBE {v} (got {got})",
                  got is not None and abs(got - v) < 1e-6)
else:
    print(f"  skip  auto.hbin absent at {AUTO} — oracle tie-back arm not run")

if fails:
    print("FAILED:", ", ".join(fails))
    sys.exit(1)
print("PASSED")
