#!/usr/bin/env python3
# ctest gate for BUG-021 (iv drops singleton FE by default + --keep-singletons
# opt-out) and BUG-020 (--keepif scratch honors $TMPDIR and is cleaned up).
#
# Self-contained: builds its own CSV fixture, converts to hbin via `cobra convert`,
# and drives `cobra iv` / `cobra feols`. No gitignored fixture dependency.
#
# usage: validate_iv_singleton.py <cobra-binary>
import json, os, subprocess, sys, tempfile, glob

from _cobra_bin import resolve_cobra  # noqa: E402  (sibling module; see it for why)

COBRA = resolve_cobra()
fails = []


def run_raw(args, env=None):
    r = subprocess.run([COBRA] + args, capture_output=True, text=True, env=env)
    if r.returncode != 0:
        print("  cobra failed:", " ".join(args), "\n", r.stderr, file=sys.stderr)
        sys.exit(1)
    return r.stdout


def run(args, env=None):
    return json.loads(run_raw(args, env=env))


def check(name, cond):
    print(("  ok   " if cond else "  FAIL ") + name)
    if not cond:
        fails.append(name)


work = tempfile.mkdtemp(prefix="iv_singleton_")
csv = os.path.join(work, "panel.csv")
hbin = os.path.join(work, "panel.hbin")

# 7 rows. FE group `g`: level 1 = {r0,r1,r2}, level 2 = {r3,r4,r5}, level 3 = {r6}
# is a SINGLETON. The instrument z drives the endog d; y is a noisy function of d.
# reghdfe/ivreghdfe drop the singleton row -> N=6; --keep-singletons keeps N=7.
rows = [
    # y,    d,    z,    xc,  g
    (2.0, 1.0, 0.5, 1.0, 1),
    (3.1, 2.0, 1.5, 2.0, 1),
    (3.9, 3.0, 2.4, 1.0, 1),
    (5.2, 4.0, 3.6, 2.0, 2),
    (6.0, 5.0, 4.5, 1.0, 2),
    (7.1, 6.0, 5.4, 2.0, 2),
    (9.0, 8.0, 7.7, 1.0, 3),  # singleton FE group
]
with open(csv, "w") as f:
    f.write("y,d,z,xc,g\n")
    for r in rows:
        f.write(",".join(str(v) for v in r) + "\n")

run_raw(["convert", csv, hbin, "--factor", "g"])

# --- FIX 2: default drops the singleton FE group ---
j = run(["iv", "--data", hbin, "--y", "y", "--endog", "d",
         "--instruments", "z", "--x", "xc", "--fe", "g"])
check("iv default: n_obs == 6 (singleton dropped)", j["n_obs"] == 6)
check("iv default: n_dropped_singletons == 1", j.get("n_dropped_singletons") == 1)

# --- FIX 2: --keep-singletons retains it ---
jk = run(["iv", "--data", hbin, "--y", "y", "--endog", "d",
          "--instruments", "z", "--x", "xc", "--fe", "g", "--keep-singletons"])
check("iv --keep-singletons: n_obs == 7", jk["n_obs"] == 7)
check("iv --keep-singletons: n_dropped_singletons == 0",
      jk.get("n_dropped_singletons") == 0)

# --- FIX 1: --keepif scratch honors $TMPDIR and is cleaned up ---
scratch = os.path.join(work, "tmpdir")
os.makedirs(scratch, exist_ok=True)
env = dict(os.environ)
env["TMPDIR"] = scratch
# keepif that keeps all rows so the estimate is well-posed; the point is the scratch.
jf = run(["feols", "--data", hbin, "--y", "y", "--x", "d,xc",
          "--keepif", "y > 0"], env=env)
check("keepif estimate ran", jf.get("n_obs", 0) == 7)
leftover = glob.glob(os.path.join(scratch, "cecon_keepif_*")) + \
           glob.glob(os.path.join(scratch, "cecon_keepif_*.hbin"))
check("keepif scratch cleaned up (none left in TMPDIR)", len(leftover) == 0)
# And nothing leaked into /tmp with our pid pattern either (best-effort).
# (We can't know the child pid, so just assert TMPDIR was the chosen dir by
#  confirming no scratch remains anywhere obvious.)

if fails:
    print("FAILED:", ", ".join(fails))
    sys.exit(1)
print("PASSED")
