#!/usr/bin/env python3
# ctest gate for the feols --fe panel R² (Stata xtreg,fe / areg between & overall R²),
# end-to-end via the CLI. Regression guard for the commit-3217393 side-effect: adding
# the reconstructed reghdfe "_cons" row inflated r.names from k to k+1, which tripped
# the plain-slope guard in add_panel_r2() (`r.names.size()!=k`) and SILENTLY dropped
# r2_between / r2_overall. The pre-existing unit test (test_xtreg) covers xtreg,re only
# and never asserted these three panel R², which is why the regression shipped.
#
# This asserts, on the committed panel_synth.hbin fixture, that a single-FE feols emits
# ALL THREE panel R² (within/between/overall) AND still carries the reconstructed _cons
# row unchanged. Golden values are the Eval-diagnosed references.
#
# usage: validate_panel_r2.py <cobra-binary>
import json, subprocess, sys, os

from _cobra_bin import resolve_cobra  # noqa: E402  (sibling module; see it for why)

COBRA = resolve_cobra()
# Fixture lives next to this script (tests/validate/panel_synth.hbin), committed.
HERE = os.path.dirname(os.path.abspath(__file__))
DATA = os.path.join(HERE, "panel_synth.hbin")

fails = []


def run(args):
    r = subprocess.run([COBRA] + args, capture_output=True, text=True)
    if r.returncode != 0:
        print("  cobra failed:", " ".join(args), "\n", r.stderr, file=sys.stderr)
        sys.exit(1)
    return json.loads(r.stdout)


def check(name, cond):
    print(("  ok   " if cond else "  FAIL ") + name)
    if not cond:
        fails.append(name)


def rel(got, want):
    return abs(got - want) / (abs(want) + 1e-30)


print("validate_panel_r2:")

# The Eval-provided fix-verify handle:
#   cobra feols --data panel_synth.hbin --y y --x x1,x2 --fe id --vcov iid --tidy
out = run(["feols", "--data", DATA, "--y", "y", "--x", "x1,x2",
           "--fe", "id", "--vcov", "iid", "--tidy"])

# 1. All three panel R² must be PRESENT (the regression dropped between/overall).
check("r2_within present", "r2_within" in out)
check("r2_between present", "r2_between" in out)
check("r2_overall present", "r2_overall" in out)

# 2. Golden values (Eval-diagnosed). within is unchanged by the _cons row (Frisch-Waugh);
#    between/overall are the reconstructed correlations of xb=x·β vs y.
if "r2_within" in out:
    check("r2_within ~= 0.2373685 (rel<1e-6)", rel(out["r2_within"], 0.23736852779230821) < 1e-6)
if "r2_between" in out:
    check("r2_between ~= 7.671e-4 (rel<1e-3)", rel(out["r2_between"], 7.671e-4) < 1e-3)
if "r2_overall" in out:
    check("r2_overall ~= 0.07955 (rel<1e-3)", rel(out["r2_overall"], 0.07955) < 1e-3)

# 3. The reconstructed reghdfe _cons row must STILL be present and unchanged
#    (its value must not have shifted; the guard fix must not perturb it).
coef = out.get("coef", {})
check("_cons row present in coef", "_cons" in coef)
if "_cons" in coef:
    check("_cons ~= 2.20495 (rel<1e-6)", rel(coef["_cons"], 2.2049501377388587) < 1e-6)

# 4. Sanity: the slope coefs are the plain requested predictors (not factor-expanded),
#    i.e. the panel-R² path is genuinely the one exercised.
check("slope x1 present", "x1" in coef)
check("slope x2 present", "x2" in coef)

print("FAILED" if fails else "PASSED")
sys.exit(1 if fails else 0)
