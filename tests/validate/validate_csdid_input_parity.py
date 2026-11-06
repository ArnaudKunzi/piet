#!/usr/bin/env python3
# BUG-040/043 ctest gate: csdid input-validation parity (ivar()/cluster() collision
# + cluster/time nesting).
#
# BUG-040 — Black-box Stata probe (StataBE 19.5 + csdid v1.81, observed behaviour
# only) on the fatalities tour
# recipe found:
#   ivar(X) ... cluster(X)  [same variable NAME given to both options]
#     -> Stata r(198) "X may not be both target and by()"; cobra used to run it
#        and answer (Doctrine-1 input-parity violation; the fatalities tour hits
#        this exact form: xtset statenum year + csdid ..., ivar(statenum) ...
#        cluster(statenum)).
#   ivar(X) ... cluster(Y)  [Y a genuinely different variable, by name AND data]
#     -> Stata runs normally (clustered SEs). Unaffected — must stay accepted.
#   ivar(X) ... cluster(X2) [X2 a byte-identical COPY of X under a different name]
#     -> Stata STILL runs normally (clustered SEs). The check is a NAME/token
#        match, not a data/value match — probed explicitly to rule out a
#        value-based rule.
#   xtset presence/absence and method()/comparison() choice do not change any of
#   the above (probed both ways) — the collision is purely --ivar-string ==
#   --cluster-string, independent of everything else.
#
# BUG-043 — Black-box Stata probe (same StataBE/csdid version; 5
# synthetic-panel arms + xtset on/off, 2026-07-31) found the real rule is NOT
# "cluster()==time()" as the bug title suggests but the GENERAL panel-clustering
# nesting requirement: --cluster must be CONSTANT WITHIN each --ivar unit ->
# r(498) "panels are not nested within clusters". Confirmed:
#   cluster(T)      [T = the literal --time column]            -> r(498)
#   cluster(Tcopy)  [Tcopy = a byte-identical COPY of T, new name] -> r(498)
#     (value-based, persists under renaming — unlike BUG-040's name-token rule)
#   cluster(Z)      [Z varies within a unit, Z != time]         -> r(498)
#     (the trigger is the nesting property, not equality to --time specifically)
#   cluster(region) [constant within each unit]                 -> accepted
#   xtset presence/absence does not change the outcome (probed both ways).
#
# Self-contained (builds its own CSV fixture; no external data dependency).
# usage: validate_csdid_input_parity.py <cobra-binary>
import json, os, subprocess, sys, tempfile

from _cobra_bin import resolve_cobra  # noqa: E402  (sibling module; see it for why)

COBRA = resolve_cobra()
fails = []


def run_raw(args):
    return subprocess.run([COBRA] + args, capture_output=True, text=True)


def run_ok(args, label):
    r = run_raw(args)
    if r.returncode != 0:
        print(f"  cobra failed ({label}):", " ".join(args), "\n", r.stderr, file=sys.stderr)
        fails.append(label)
        return None
    return json.loads(r.stdout)


def check(name, cond):
    print(("  ok   " if cond else "  FAIL ") + name)
    if not cond:
        fails.append(name)


work = tempfile.mkdtemp(prefix="csdid_input_parity_")
csv_path = os.path.join(work, "panel.csv")
hbin = os.path.join(work, "panel.hbin")

# 10 units x 5 periods. Units 1-6 never treated; 7-8 treated at g=3; 9-10 at g=4.
# id2/region are, respectively, a value-identical copy of id (different NAME) and
# a genuinely different clustering variable (2 groups of 5 units), both constant
# within a unit (nested — BUG-040 fixtures). t2 is a byte-identical copy of t
# under a different name (varies within a unit — BUG-043 nesting-violation
# fixture, renamed to rule out a name-token rule same as BUG-040's id2 probe).
rows = []
cohort = {1: 0, 2: 0, 3: 0, 4: 0, 5: 0, 6: 0, 7: 3, 8: 3, 9: 4, 10: 4}
for uid in range(1, 11):
    g = cohort[uid]
    for t in range(1, 6):
        bump = 2.0 if (g > 0 and t >= g) else 0.0
        y = 0.3 * t + 0.1 * uid + bump
        x = 0.05 * uid
        rows.append((uid, t, y, x, g, uid, 1 if uid <= 5 else 2, t))

with open(csv_path, "w") as f:
    f.write("id,t,y,x,gvar,id2,region,t2\n")
    for r in rows:
        f.write(",".join(str(v) for v in r) + "\n")

conv = run_raw(["convert", csv_path, hbin])
if conv.returncode != 0:
    print("convert failed:", conv.stderr, file=sys.stderr)
    sys.exit(1)

base = ["csdid", "--data", hbin, "--y", "y", "--x", "x",
        "--ivar", "id", "--time", "t", "--gvar", "gvar", "--method", "dripw"]

# --- Accepted forms: must all succeed (rc 0), unaffected by the fix ---
j_noclu = run_ok(base, "no cluster: runs")
check("no cluster: runs", j_noclu is not None and j_noclu.get("ok") is True)

j_diff = run_ok(base + ["--cluster", "region"], "cluster = different var: runs")
check("cluster = different var (region): runs",
      j_diff is not None and j_diff.get("ok") is True)

j_copy = run_ok(base + ["--cluster", "id2"], "cluster = value-identical copy, diff name: runs")
check("cluster = value-identical copy of ivar under a DIFFERENT name (id2): runs "
      "(name-based check, not value-based — parity with the Stata probe)",
      j_copy is not None and j_copy.get("ok") is True)

# Accepted forms must be numerically identical to each other where comparable
# (no-cluster vs distinct-cluster point estimates: same ATTs, cluster only
# changes SEs) — spot check ATT vector unaffected by --cluster choice.
if j_noclu and j_diff:
    check("point estimates (att) unaffected by --cluster (region) choice",
          [c["att"] for c in j_noclu["cells"]] == [c["att"] for c in j_diff["cells"]])
if j_noclu and j_copy:
    check("point estimates (att) unaffected by --cluster (id2) choice",
          [c["att"] for c in j_noclu["cells"]] == [c["att"] for c in j_copy["cells"]])

# --- Rejected form: ivar and cluster name the SAME variable ---
r_same = run_raw(base + ["--cluster", "id"])
check("cluster == ivar (same name 'id'): rejected (rc != 0)", r_same.returncode != 0)
try:
    err = json.loads(r_same.stderr)
    check("cluster == ivar: structured error code 'bad_input'",
          err.get("error", {}).get("code") == "bad_input")
    check("cluster == ivar: error names the colliding variable ('id')",
          "id" in err.get("error", {}).get("message", "") or
          err.get("error", {}).get("detail") == "id")
except (json.JSONDecodeError, ValueError):
    check("cluster == ivar: stderr is structured JSON", False)

# --- BUG-043: --cluster not nested within --ivar (varies within a unit) ---
r_time = run_raw(base + ["--cluster", "t"])
check("cluster = time (literal 't'): rejected (rc != 0)", r_time.returncode != 0)
try:
    err = json.loads(r_time.stderr)
    check("cluster = time: structured error code 'bad_input'",
          err.get("error", {}).get("code") == "bad_input")
    check("cluster = time: error names the colliding variable ('t')",
          err.get("error", {}).get("detail") == "t")
except (json.JSONDecodeError, ValueError):
    check("cluster = time: stderr is structured JSON", False)

r_t2 = run_raw(base + ["--cluster", "t2"])
check("cluster = value-identical copy of time under a DIFFERENT name (t2): "
      "rejected (rc != 0) — value/nesting-based, persists under renaming, "
      "parity with the Stata probe", r_t2.returncode != 0)
try:
    err = json.loads(r_t2.stderr)
    check("cluster = t2: structured error code 'bad_input'",
          err.get("error", {}).get("code") == "bad_input")
except (json.JSONDecodeError, ValueError):
    check("cluster = t2: stderr is structured JSON", False)

if fails:
    print("FAILED:", ", ".join(fails))
    sys.exit(1)
print("PASSED")
