#!/usr/bin/env python3
# ctest gate pinning contract A §35's new scalars to the Stata 19.5 ANSWER BOOK,
# end-to-end through the CLI, on the pangram wave fixture.
#
# Oracle: internal capture p2_stata.log (User-captured), the
# two model lines below. Every number here was read out of that log, not derived.
#
#   poisson ycount x1 x2 i.region, vce(cluster id)
#       Log pseudolikelihood = -5961.354      -> e(ll)
#       Wald chi2(6)         =  252.19        -> e(chi2) / e(df_m)
#       Pseudo R2            =   0.0108       -> e(r2_p)
#
#   xtreg y x1 x2 i.t, fe vce(cluster id)
#       Within/Between/Overall R2 = 0.4144 / 0.6150 / 0.4527
#       corr(u_i, Xb) = -0.1166
#       sigma_u = .31448074   sigma_e = .77958951   rho = .13995192
#
# FIXTURE NOTE: wave_fixture.dta is a generated pangram artifact (gitignored, rebuilt
# by the User's Stata runs) and is NOT part of cobra's own committed fixture set. When
# it is absent this gate prints a loud SKIP and exits 0 — its absence is a missing
# artifact, not a cobra regression. The mechanical invariants behind these same
# scalars are pinned fixture-free by tests/unit/test_fepois_ll.cpp, so a silent
# no-fixture run still leaves the feature covered.
#
# usage: validate_wave_oracle.py <cobra-binary>
import json
import os
import subprocess
import sys
import tempfile

from _cobra_bin import resolve_cobra  # noqa: E402  (sibling module; see it for why)

COBRA = resolve_cobra()
HERE = os.path.dirname(os.path.abspath(__file__))
FIXTURE = os.path.normpath(os.path.join(
    HERE, "..", "kitchensink", "pangram", "waves", "_artifacts", "wave_fixture.dta"))

print("validate_wave_oracle:")

if not os.path.exists(FIXTURE):
    print("  SKIP: pangram wave fixture absent (%s)" % FIXTURE)
    print("  SKIP: §35 oracle pins NOT exercised this run; mechanical invariants")
    print("  SKIP: remain covered by tests/unit/test_fepois_ll.cpp")
    print("SKIPPED")
    sys.exit(0)

fails = []


def check(name, cond):
    print(("  ok   " if cond else "  FAIL ") + name)
    if not cond:
        fails.append(name)


def run(args):
    r = subprocess.run([COBRA] + args, capture_output=True, text=True)
    if r.returncode != 0:
        print("  cobra failed:", " ".join(args), "\n", r.stdout, r.stderr, file=sys.stderr)
        sys.exit(1)
    return json.loads(r.stdout)


def near(got, want, tol):
    return got is not None and abs(got - want) <= tol


work = tempfile.mkdtemp(prefix="cobra_wave_oracle_")
hbin = os.path.join(work, "wave.hbin")
conv = subprocess.run([COBRA, "import-dta", "--data", FIXTURE, "--out", hbin],
                      capture_output=True, text=True)
if conv.returncode != 0:
    print("  import-dta failed:", conv.stdout, conv.stderr, file=sys.stderr)
    sys.exit(1)

# --- C1: poisson ycount x1 x2 i.region, vce(cluster id) -------------------------
p = run(["fepois", "--data", hbin, "--y", "ycount", "--x", "x1,x2,i.region",
         "--vcov", "cluster", "--cluster", "id", "--tidy"])
check("poisson n_obs == 4671", p.get("n_obs") == 4671)
# Stata prints ll to 3dp ("-5961.354"), so the tolerance is that display width.
check("e(ll) == -5961.354 (+-5e-4)", near(p.get("ll"), -5961.354, 5e-4))
# Pseudo R2 is printed to 4dp.
check("e(r2_p) == 0.0108 (+-5e-5)", near(p.get("r2_p"), 0.0108, 5e-5))
check("e(df_m) == 6", p.get("chi2_df") == 6)
check("chi2 is a Wald stat, not the LR",
      p.get("chi2") is not None
      and abs(p["chi2"] - 2 * (p["ll"] - p["ll_0"])) > 1.0)
# BUG-175: the ML cluster multiplier is the pure G/(G-1), NOT the regression CGM
# G/(G-1)*(N-1)/(N-K). Before that fix chi2 came out 251.8616 — low by exactly
# (N-1)/(N-K) = 4670/4664 — and every clustered SE was high by its square root.
# Stata prints the Wald to 2dp, so that is the tolerance.
check("e(chi2) == 252.19 (+-5e-3)", near(p.get("chi2"), 252.19, 5e-3))
# The SEs are the other half of the same factor: pin them so a regression on either
# symptom is caught. Stata prints 7 significant figures.
for nm, want in (("x1", .0108147), ("x2", .0102169), ("2.region", .0260368),
                 ("3.region", .0249853), ("4.region", .0236475),
                 ("5.region", .0258812), ("(intercept)", .022027)):
    got = p.get("se", {}).get(nm)
    check("se(%s) == %g (ML cluster factor)" % (nm, want),
          got is not None and abs(got - want) / want < 1e-5)

# --- C2: xtreg y x1 x2 i.t, fe vce(cluster id) ----------------------------------
x = run(["feols", "--data", hbin, "--y", "y", "--x", "x1,x2,i.t",
         "--fe", "id", "--vcov", "cluster", "--cluster", "id", "--tidy"])
check("xtreg n_obs == 4649", x.get("n_obs") == 4649)
check("r2_within == 0.4144 (+-5e-5)", near(x.get("r2_within"), 0.4144, 5e-5))
# BUG-156: these two were ABSENT on this model before §35 — the old between/overall
# helper reconstructed the predictor from the Dataset by variable name and bailed on
# any factor expansion, and `i.t` is a factor expansion.
check("r2_between == 0.6150 (+-5e-5)", near(x.get("r2_between"), 0.6150, 5e-5))
check("r2_overall == 0.4527 (+-5e-5)", near(x.get("r2_overall"), 0.4527, 5e-5))
check("sigma_u == .31448074 (+-5e-9)", near(x.get("sigma_u"), 0.31448074, 5e-9))
check("sigma_e == .77958951 (+-5e-9)", near(x.get("sigma_e"), 0.77958951, 5e-9))
check("rho == .13995192 (+-5e-9)", near(x.get("rho"), 0.13995192, 5e-9))
check("corr(u_i,Xb) == -0.1166 (+-5e-5)", near(x.get("corr_u_xb"), -0.1166, 5e-5))
# rho must be reconstructible from the two sigmas (guards a decoupled edit).
if x.get("sigma_u") is not None and x.get("sigma_e") is not None:
    su2, se2 = x["sigma_u"] ** 2, x["sigma_e"] ** 2
    check("rho == sigma_u^2/(sigma_u^2+sigma_e^2)",
          near(x.get("rho"), su2 / (su2 + se2), 1e-12))

# --- additivity: multi-FE must NOT emit the single-FE panel components ----------
m = run(["feols", "--data", hbin, "--y", "y", "--x", "x1,x2",
         "--fe", "id,t", "--vcov", "cluster", "--cluster", "id", "--tidy"])
for k in ("sigma_u", "sigma_e", "rho", "corr_u_xb"):
    check("multi-FE emits no %s" % k, k not in m)

print("FAILED" if fails else "PASSED")
sys.exit(1 if fails else 0)
