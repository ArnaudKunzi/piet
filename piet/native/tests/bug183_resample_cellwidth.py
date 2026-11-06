#!/usr/bin/env python3
"""BUG-183 regression — the resampling tables' coefficient cells must use the
COEFFICIENT-TABLE width rule, not the one-character-wider `display` rule.

Why this file exists at all. BUG-183 was filed LATENT: the two rules coincide
below |1|, and above it they coincide whenever the 9th character would be a
trailing zero (which `%g` strips), so the defect only prints when

    |value| >= 1  AND  the value's 8th significant digit is nonzero.

The P6 answerbook happens to miss that window on every cell (its jackknife CI
upper is `1.099479`, exactly the trailing-zero case), which is why eight waves
of a green gate never saw this. A regression test that reuses the answerbook
would therefore be vacuous. The fixture below is built specifically to LAND in
the window: y = 2.70196*x + 1.4142136 + noise gives a slope of 2.70358419..., a
value >= 1 whose 8th significant digit is nonzero AND which is nowhere near a
half-way rounding boundary (the multiplier was chosen by searching for exactly
that, so the leg cannot be flipped by a 1-ulp change).

Oracle: live StataNow 19.5 (2026-08-06), same CSV, same command:

       slope |   2.703584   .0083324   324.46   0.000     2.687153    2.720015
       icept |   1.401988   .0773999    18.11   0.000     1.249358    1.554617

Stata prints the slope as `2.703584` — 7 significant digits, 8 characters. The
full-precision value is 2.70358419, so the `display` rule renders it
`2.7035842` (9 characters), and that is what PIET printed before this fix.

This asserts the WIDTH RULE, which is what the fix changed: every coefficient /
std.err. / CI cell is at most 8 characters excluding the sign, and the slope
cell equals Stata's byte for byte. It deliberately does NOT pin the last digit
of every OTHER cell: our intercept reads 1.401987 where Stata reads 1.401988
(our value is 1.40198749, i.e. the two sit either side of the half-way point of
the last displayed digit) and t reads 324.47 vs 324.46 likewise. Those are
TOLERANCE.md section 1 boundary-flip adjudications, a separate numeric question,
and must be neither laundered into nor blocked by a formatting gate.

Hand-invoked, like its neighbours in this directory (piet2_gate/, eval_diff/) --
the cobra root CMakeLists.txt is deliberately PIET-free, so this is not a ctest
row. Run:

    COBRA_BIN=<...>/build/cobra \\
        python3 piet/native/tests/bug183_resample_cellwidth.py \\
        [path/to/piet_native]

Exits 0 = PASS, 1 = FAIL, 2 = SKIPPED (a binary is missing -- an honest skip,
never a silent pass).
"""
import json
import math
import os
import subprocess
import sys
import tempfile


def _pin_gate_settings(env, _parents=3):  # BUG-388 (2026-09-02): never the user's live settings.json
    """Point the spawned piet_native at a GATE-OWNED settings file (tools/engine_arm.py::pin_settings)."""
    import sys as _s, tempfile as _t
    from pathlib import Path as _P
    _tools = str(_P(__file__).resolve().parents[_parents] / "tools")
    if _tools not in _s.path:
        _s.path.insert(0, _tools)
    import engine_arm
    engine_arm.pin_settings(env, _t.mkdtemp(prefix="piet_gate_settings_"))

HERE = os.path.dirname(os.path.abspath(__file__))
NATIVE_ROOT = os.path.dirname(HERE)              # piet/native
REPO = os.path.dirname(os.path.dirname(NATIVE_ROOT))

piet_bin = sys.argv[1] if len(sys.argv) > 1 else os.path.join(NATIVE_ROOT, "build", "piet_native")
cobra_bin = os.environ.get("COBRA_BIN", os.path.join(REPO, "build", "cobra"))

# Capability gate, stated honestly (never a silent pass).
for label, path in (("piet_native", piet_bin), ("cobra", cobra_bin)):
    if not os.path.isfile(path):
        print(f"SKIPPED: {label} not found at {path} — build it, or pass the path/COBRA_BIN.")
        sys.exit(2)
print(f"engine arm: COBRA_BIN={cobra_bin}")
print(f"piet arm:   {piet_bin}")

work = tempfile.mkdtemp(prefix="bug183_")
csv = os.path.join(work, "rs183.csv")
do = os.path.join(work, "rs183.do")

# The fixture, generated the same way the oracle log above was: 200 rows,
# deterministic, no RNG anywhere (jackknife is deterministic, so the comparison
# to Stata is exact rather than distributional).
with open(csv, "w") as f:
    f.write("y,x\n")
    for i in range(1, 201):
        x = (i % 17) + math.sin(i) / 3.0
        y = 2.70196 * x + math.cos(i * 1.7) * 0.8 + 1.4142136
        f.write(f"{y:.10f},{x:.10f}\n")
with open(do, "w") as f:
    f.write(f'import delimited "{csv}", clear\n')
    f.write("jackknife slope=_b[x] icept=_b[_cons]: regress y x\n")

env = dict(os.environ, COBRA_BIN=cobra_bin)
_pin_gate_settings(env)  # BUG-388
r = subprocess.run([piet_bin, "--do", do], capture_output=True, text=True, env=env, cwd=work)
payload = None
for line in r.stdout.splitlines():
    if line.startswith("{"):
        payload = json.loads(line)
if payload is None:
    print("FAIL: piet_native produced no JSON payload")
    print(r.stdout[-2000:], r.stderr[-2000:])
    sys.exit(1)

lines = [b["text"] for b in payload["blocks"] if b.get("type") == "display"]
errors = [b for b in payload["blocks"] if b.get("type") == "error"]
fails = []


def check(name, cond, detail=""):
    print(("  ok   " if cond else "  FAIL ") + name + ((" | " + detail) if detail and not cond else ""))
    if not cond:
        fails.append(name)


check("jackknife ran without an error block", not errors, str(errors)[:300])

rows = {}
for ln in lines:
    if "|" not in ln:
        continue
    label, _, rest = ln.partition("|")
    label = label.strip()
    if label in ("slope", "icept"):
        rows[label] = rest.split()

check("both jackknife rows rendered", set(rows) == {"slope", "icept"}, str(sorted(rows)))

if set(rows) == {"slope", "icept"}:
    # (1) The oracle cell, byte for byte. This is the assertion the fix moves:
    #     pre-fix this read "2.7035842".
    check("slope coefficient cell == Stata's `2.703584`",
          rows["slope"][0] == "2.703584", f"got {rows['slope'][0]!r}")

    # (2) The RULE, over every numeric cell of both rows: a coefficient-table
    #     cell never exceeds 8 characters excluding the sign. Stated as the rule
    #     rather than as five more pinned literals, so a future value change in
    #     the fixture cannot quietly turn this leg into a no-op.
    for label, cells in sorted(rows.items()):
        for idx in (0, 1, 4, 5):        # coef, std.err., CI lo, CI hi (t and P>|t| are %.2f/%.3f)
            cell = cells[idx]
            body = cell[1:] if cell.startswith("-") else cell
            check(f"{label} cell[{idx}] within the coef-table width (<=8 excl. sign)",
                  len(body) <= 8, f"{cell!r} is {len(body)} chars")

    # (3) VACUITY GUARD. If the fixture ever stops landing in the exposure
    #     window (|v| >= 1 and a nonzero 8th significant digit), every check
    #     above would still pass while measuring nothing at all — which is
    #     precisely how this defect survived eight green waves on the P6
    #     answerbook. Assert the fixture still discriminates: the `display`
    #     rule must produce a DIFFERENT string here than the table rule does.
    # Recover the full-precision value from the payload rather than re-deriving it.
    resample = [b for b in payload["blocks"] if b.get("type") == "resample"]
    check("resample block present (source of the full-precision value)", bool(resample))
    if resample:
        by_name = {row["name"]: row for row in resample[0]["rows"]}
        b = by_name["slope"]["b"]
        nine = f"{b:.8g}"       # what the `display` rule (%9.0g family) yields
        eight = f"{b:.7g}"      # what the coefficient-table rule yields
        check("vacuity guard: fixture still lands in the exposure window "
              "(display rule and table rule disagree here)",
              nine != eight, f"both render as {eight!r} — pick a new fixture value")
        check("vacuity guard: the printed cell is the TABLE rule, not the display rule",
              rows["slope"][0] == eight and rows["slope"][0] != nine,
              f"printed {rows['slope'][0]!r}, table rule {eight!r}, display rule {nine!r}")
if fails:
    print("FAILED: " + ", ".join(fails))
    sys.exit(1)
print("PASSED")
