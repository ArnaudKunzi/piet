#!/usr/bin/env python3
# ctest gate for contract A §35: an EXCLUDED INSTRUMENT that the FE absorption
# annihilates must be silently OMITTED (ivreghdfe parity), not turned into a hard
# "collinear" error.
#
# Real trigger, measured on the kitchensink panel:
#     ivreghdfe y x2 (x1 = age firmsize), absorb(bench_id)
# `firmsize` is time-invariant by construction, so absorbing an id FE zeroes the
# whole column; W'W = [exog | IVs]'[...] then has a zero row/col. cobra used to
# raise EngineError("collinear", "W'W not positive-definite (collinear
# instruments?)") and refuse the model, while Stata drops the instrument and fits.
#
# The engine has NO warning channel on the success path (stderr is discarded there),
# so the omission is reported as DATA — a JSON `omitted` array — never as a message.
#
# Three properties are asserted, all on a self-contained synthetic panel (no
# committed fixture needed, so this gate cannot false-fail on a fresh worktree):
#   1. the two-instrument spec SUCCEEDS and reports omitted == ["ziv"];
#   2. every surviving number is IDENTICAL to the single-instrument spec that drops
#      the dead instrument by hand — the omission must not perturb the fit;
#   3. dropping below kendog surviving instruments still HARD-ERRORS
#      (under_identified) — that error is correct and must stay reachable.
#
# usage: validate_iv_omit.py <cobra-binary>
import csv
import json
import os
import subprocess
import sys
import tempfile

from _cobra_bin import resolve_cobra  # noqa: E402  (sibling module; see it for why)

COBRA = resolve_cobra()

fails = []


def check(name, cond):
    print(("  ok   " if cond else "  FAIL ") + name)
    if not cond:
        fails.append(name)


def run(args, expect_ok=True):
    r = subprocess.run([COBRA] + args, capture_output=True, text=True)
    if expect_ok and r.returncode != 0:
        print("  cobra failed:", " ".join(args), "\n", r.stdout, r.stderr, file=sys.stderr)
        sys.exit(1)
    return r


print("validate_iv_omit:")

# --- synthetic panel -----------------------------------------------------------
# 60 units x 6 periods. `zgood` varies within unit (a live instrument for the
# endogenous x1); `ziv` is CONSTANT within unit, so absorbing the unit FE
# annihilates it exactly. Deterministic (no RNG) so the gate is reproducible.
NG, NT = 60, 6
rows = []
for i in range(NG):
    zdead = 1.0 + (i % 7)          # time-INVARIANT -> dies under absorb(id)
    for t in range(NT):
        zgood = ((i * 7 + t * 13) % 11) / 3.0
        u = ((i * 5 + t * 3) % 9) / 4.0
        x2 = ((i * 3 + t * 11) % 13) / 5.0
        x1 = 0.8 * zgood + 0.3 * zdead + 0.5 * u
        y = 1.5 * x1 - 0.7 * x2 + 2.0 * u + 0.1 * i
        rows.append(dict(id=i, t=t, y=y, x1=x1, x2=x2, zgood=zgood, ziv=zdead))

tmpdir = tempfile.mkdtemp(prefix="cobra_iv_omit_")
csv_path = os.path.join(tmpdir, "iv_omit.csv")
with open(csv_path, "w", newline="") as fh:
    w = csv.DictWriter(fh, fieldnames=["id", "t", "y", "x1", "x2", "zgood", "ziv"])
    w.writeheader()
    w.writerows(rows)

hbin_path = os.path.join(tmpdir, "iv_omit.hbin")
conv = subprocess.run([COBRA, "convert", csv_path, hbin_path], capture_output=True, text=True)
if conv.returncode != 0:
    print("  convert failed:", conv.stdout, conv.stderr, file=sys.stderr)
    sys.exit(1)

base = ["iv", "--data", hbin_path, "--y", "y", "--x", "x2", "--endog", "x1",
        "--fe", "id", "--vcov", "iid", "--tidy"]

# 1. two instruments, one of them dead under the FE -> succeeds, reports omission
both = run(base + ["--instruments", "zgood,ziv"])
check("two-instrument spec exits 0 (was: hard 'collinear' error)", both.returncode == 0)
d_both = json.loads(both.stdout) if both.returncode == 0 else {}
check("omitted == ['ziv']", d_both.get("omitted") == ["ziv"])

# 2. equality with the hand-pruned single-instrument spec
one = run(base + ["--instruments", "zgood"])
d_one = json.loads(one.stdout)
check("single-instrument spec reports NO omitted key", "omitted" not in d_one)
for key in ("coef", "se", "tstat", "n_obs", "dof_resid", "first_stage_F"):
    check("surviving %s identical to the zgood-only spec" % key,
          d_both.get(key) == d_one.get(key))

# 3. under-identification after the omission still hard-errors
dead_only = run(base + ["--instruments", "ziv"], expect_ok=False)
check("instrument set that dies entirely still errors", dead_only.returncode != 0)
try:  # the error envelope goes to STDERR on the failure path
    err = json.loads(dead_only.stderr).get("error", {}).get("code")
except Exception:
    err = None
check("error code is under_identified (got %r)" % err, err == "under_identified")

# 4. no FE -> nothing to annihilate; response shape unchanged (no omitted key)
nofe = run(["iv", "--data", hbin_path, "--y", "y", "--x", "x2", "--endog", "x1",
            "--instruments", "zgood,ziv", "--vcov", "iid", "--tidy"])
check("no-FE path emits no omitted key", "omitted" not in json.loads(nofe.stdout))

print("FAILED" if fails else "PASSED")
sys.exit(1 if fails else 0)
