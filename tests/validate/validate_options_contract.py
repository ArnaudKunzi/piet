#!/usr/bin/env python3
# B.0.3 option-forwarding contract gate (CONTRACT-engine.md §24). Pins the three
# guarantees that kill the PIET->cobra silent-skew class (BUG-056/060/104/110):
#
#   1. BUFFERED DISPATCH: an unrecognized flag rejects with exit 3 AND an EMPTY
#      stdout — the would-be result never leaks (BUG-110). Previously the result
#      was printed to stdout AND an error to stderr with exit 3, and PIET trusted
#      the stdout regardless of the exit.
#   2. `cobra options --json`: the accepted-flag table PIET pre-validates against
#      is well-formed and declares the B.0.3 skew-fix flags.
#   3. TABLE DRIFT: under COBRA_OPTIONS_STRICT=1, every flag a real invocation
#      CONSUMES must be declared in options_table() — a consumed-but-undeclared
#      flag throws (so `cobra options` can never silently under-report to PIET).
#
# Self-contained (builds its own CSV fixture). usage: validate_options_contract.py <cobra-binary>
import json, os, subprocess, sys, tempfile

from _cobra_bin import resolve_cobra  # noqa: E402  (sibling module; see it for why)

COBRA = resolve_cobra()
fails = []


def run(args, env=None):
    e = dict(os.environ)
    if env:
        e.update(env)
    return subprocess.run([COBRA] + args, capture_output=True, text=True, env=e)


def check(name, cond):
    print(("  ok   " if cond else "  FAIL ") + name)
    if not cond:
        fails.append(name)


work = tempfile.mkdtemp(prefix="opt_contract_")
csv_path = os.path.join(work, "d.csv")
hbin = os.path.join(work, "d.hbin")
with open(csv_path, "w") as f:
    f.write("id,x,y\n")
    for i in range(1, 21):
        f.write(f"{i},{0.4 * i},{3.0 + 0.6 * (0.4 * i)}\n")
if run(["convert", csv_path, hbin]).returncode != 0:
    print("convert failed", file=sys.stderr)
    sys.exit(1)

base = ["feols", "--data", hbin, "--y", "y", "--x", "x"]

# 1. Buffered dispatch: unknown flag -> exit 3 AND empty stdout (the BUG-110 fix).
r = run(base + ["--bogusflag", "9"])
check("unknown flag: rejected (rc != 0)", r.returncode != 0)
check("unknown flag: NO result leaked to stdout (buffered dispatch, BUG-110)", r.stdout.strip() == "")
# and appended after an otherwise-complete valid invocation (the audited scenario)
r2 = run(base + ["--vcov", "iid", "--bogusflag", "9"])
check("valid+unknown flag: rejected, stdout still empty", r2.returncode != 0 and r2.stdout.strip() == "")
# a valid run still emits its result to stdout on the clean path
rv = run(base + ["--tidy"])
check("valid feols --tidy: rc 0 + non-empty stdout result", rv.returncode == 0 and rv.stdout.strip() != "")

# 2. cobra options --json well-formed + declares the B.0.3 skew-fix flags.
ro = run(["options", "--json"])
check("cobra options --json: rc 0", ro.returncode == 0)
try:
    tbl = json.loads(ro.stdout)
    cmds = tbl.get("commands", {})
    check("options --json: has contract_version", "contract_version" in tbl)
    check("options --json: covers feols/logit/probit/margins",
          all(k in cmds for k in ("feols", "logit", "probit", "margins")))

    def flags_of(cmd):
        return {e["flag"] for e in cmds.get(cmd, [])}

    check("options: logit declares --level (D4)", "--level" in flags_of("logit"))
    check("options: probit declares --level (D4)", "--level" in flags_of("probit"))
    check("options: margins declares --dydx (D4)", "--dydx" in flags_of("margins"))
    check("options: every command lists globals --data/--tidy",
          all("--data" in flags_of(c) and "--tidy" in flags_of(c) for c in cmds))
except (json.JSONDecodeError, ValueError):
    check("options --json: parseable JSON", False)

# 3. Table drift: real invocations under strict mode must not consume an
#    undeclared flag (would throw code 'internal'). Exercise a spread of flags.
STRICT = {"COBRA_OPTIONS_STRICT": "1"}
drift_runs = [
    (base + ["--vcov", "cluster", "--cluster", "id", "--tidy"], "feols cluster"),
    (base + ["--level", "90", "--no-intercept"], "feols level+nocons"),
    (["logit", "--data", hbin, "--y", "id", "--x", "x", "--level", "90"], "logit level"),
]
for args, lbl in drift_runs:
    rs = run(args, env=STRICT)
    # rc may be 0 (ran) or a legit non-drift error, but NEVER the drift 'internal' throw.
    drift = "options-table drift" in (rs.stderr or "")
    check(f"no table drift under strict: {lbl}", not drift)

# 4. P-QE wave (rdrobust/rddensity/rdplot/binscatter/eventstudyinteract/
#    did_imputation/sdid): an earlier scaffold arc declared only these seven
#    commands' Stata-level options(), not the cobra CLI flags (--y/--x/--ivar/
#    ...) their handlers actually read — including the options several of them
#    read ONLY to refuse by name (--fuzzy/--covs/--cluster on the RD trio,
#    --controls/--absorb on binscatter, --weight/--cluster on did_imputation,
#    --x/--method on sdid). Nothing drove these commands through the CLI under
#    COBRA_OPTIONS_STRICT, so a wrong row was invisible to every gate. This
#    builds one synthetic panel that is simultaneously a valid RD running-
#    variable sample (cutoff 0) and a valid staggered-adoption panel (two
#    treated cohorts at t=4/t=7 plus a never-treated block), and asserts a
#    real, successful (rc==0) run of each command consumes only declared
#    flags — dispatch_guard only calls check_unconsumed_flags after rc==0
#    (apps/cecon_cli.cpp), so a nonzero rc here would just as often mean "row
#    regressed" as "estimator objected"; requiring rc==0 keeps the two apart.
#    NOTE (superseded 2026-08-06 by BUG-182 — see section 5): a reject-by-name
#    flag used to be unreachable here because its handler throws before
#    returning. main() now runs the drift half from the throwing path too, so
#    section 5 below covers those flags by measurement instead of by inspection.
pqe_n_units, pqe_n_periods = 60, 10
pqe_csv = os.path.join(work, "pqe_panel.csv")
pqe_hbin = os.path.join(work, "pqe_panel.hbin")
with open(pqe_csv, "w") as f:
    f.write("id,t,running,x1,y,gvar,trt,F2,F3,L2,L3,never_treat,cluster\n")
    for uid in range(1, pqe_n_units + 1):
        gvar = 4 if uid <= 15 else (7 if uid <= 30 else None)
        for t in range(1, pqe_n_periods + 1):
            noise = ((uid * 37 + t * 101) % 23) / 23.0 - 0.5
            running = ((uid * 7 + t * 13) % 100) - 50.0
            x1 = ((uid * 11 + t * 5) % 40) / 4.0
            trt = 1 if (gvar is not None and t >= gvar) else 0
            ev = (t - gvar) if gvar is not None else None
            F2, F3 = int(ev == -2), int(ev == -3)
            L2, L3 = int(ev == 2), int(ev == 3)
            never = int(gvar is None)
            y = 3.0 + 0.15 * uid + 0.2 * t + 2.5 * trt + noise + 0.05 * running
            gvar_s = "" if gvar is None else str(gvar)
            f.write(f"{uid},{t},{running},{x1},{y},{gvar_s},{trt},"
                    f"{F2},{F3},{L2},{L3},{never},{uid}\n")
if run(["convert", pqe_csv, pqe_hbin]).returncode != 0:
    print("pqe panel convert failed", file=sys.stderr)
    sys.exit(1)

pqe_runs = [
    (["rdrobust", "--data", pqe_hbin, "--y", "y", "--x", "running", "--c", "0",
      "--p", "1", "--q", "2", "--h", "12", "--b", "18", "--kernel", "triangular",
      "--nnmatch", "3", "--level", "95", "--bwselect", "mserd", "--vce", "nn",
      "--masspoints", "adjust", "--keepif", "running > -49", "--tidy"], "rdrobust"),
    (["rddensity", "--data", pqe_hbin, "--x", "running", "--c", "0", "--p", "1",
      "--q", "2", "--h", "20", "--kernel", "triangular", "--bwselect", "comb",
      "--vce", "jackknife", "--keepif", "running > -49", "--tidy"], "rddensity"),
    (["rdplot", "--data", pqe_hbin, "--y", "y", "--x", "running", "--c", "0",
      "--p", "1", "--h", "20", "--nbins", "10,10", "--kernel", "uniform",
      "--binselect", "esmv", "--keepif", "running > -49", "--tidy"], "rdplot"),
    (["binscatter", "--data", pqe_hbin, "--y", "y", "--x", "x1",
      "--nquantiles", "10", "--tidy"], "binscatter"),
    # gfx-binscatter served --controls/--absorb (residualization, binscatter's
    # ado); they moved here from section 5's refuse-by-name rows.
    (["binscatter", "--data", pqe_hbin, "--y", "y", "--x", "x1",
      "--nquantiles", "10", "--controls", "t", "--keepif", "id > 0",
      "--tidy"], "binscatter --controls"),
    (["binscatter", "--data", pqe_hbin, "--y", "y", "--x", "x1",
      "--nquantiles", "10", "--absorb", "id", "--linetype", "qfit",
      "--tidy"], "binscatter --absorb"),
    (["eventstudyinteract", "--data", pqe_hbin, "--y", "y", "--rel", "F2,F3,L2,L3",
      "--absorb", "id,t", "--cohort", "gvar", "--control_cohort", "never_treat",
      "--cluster", "cluster", "--vce", "cluster", "--level", "95",
      "--keepif", "id > 0", "--tidy"], "eventstudyinteract"),
    (["did_imputation", "--data", pqe_hbin, "--y", "y", "--ivar", "id", "--time", "t",
      "--gvar", "gvar", "--horizons", "0/3", "--pretrends", "2", "--autosample",
      "--level", "95", "--keepif", "id > 0", "--tidy"], "did_imputation"),
    (["sdid", "--data", pqe_hbin, "--y", "y", "--ivar", "id", "--time", "t",
      "--treated", "trt", "--vce", "bootstrap", "--reps", "20", "--seed", "1",
      "--level", "95", "--keepif", "id > 0", "--tidy"], "sdid"),
]
for args, lbl in pqe_runs:
    rs = run(args, env=STRICT)
    check(f"pqe strict-gate coverage: {lbl} runs to completion (rc 0, no drift)",
          rs.returncode == 0)

# 5. BUG-182 — the REFUSE-BY-NAME flags, the ones section 4 could not reach.
#    Refusing an option by name is how this codebase satisfies gate (1) for
#    anything it does not implement (silently ignoring an option answers a
#    different question), so the declarations backing the honesty mechanism are
#    exactly the ones that most need a gate. Every one of these paths THROWS,
#    and dispatch_guard's rc==0 gate unwound past — main() now runs
#    check_options_table_drift() from the catch around dispatch_command, so a
#    consumed-but-undeclared refuse-by-name flag surfaces as the 'internal'
#    drift error instead of vanishing.
#
#    Each row asserts BOTH halves, which is what makes the check sharp:
#      (a) the honest refusal still fires, naming the flag  -> the product
#          behaviour is unchanged and the flag really was consumed;
#      (b) stderr carries NO "options-table drift"          -> and, because (a)
#          proves the drift check ran on this exact path, (b) is now a real
#          measurement of the declaration rather than a vacuous pass.
#
#    RED certificate (recorded 2026-08-06, reproduce by deleting {"--fuzzy",true}
#    from the rdrobust row of apps/cmds/options_table.cpp and rebuilding):
#      post-fix  -> code 'internal', "options-table drift: '--fuzzy' consumed by
#                   'rdrobust' but not declared in options_table()"   [RED]
#      pre-fix   -> code 'bad_input', the ordinary refusal              [BLIND]
#    i.e. the seeded defect is invisible to the old wiring and caught by the new
#    one. Non-strict output is byte-identical in both builds (the shipped path
#    never runs the drift half).
refuse_by_name = [
    (["rdrobust", "--data", pqe_hbin, "--y", "y", "--x", "running", "--c", "0",
      "--fuzzy", "trt"], "--fuzzy", "rdrobust"),
    (["rdrobust", "--data", pqe_hbin, "--y", "y", "--x", "running", "--c", "0",
      "--covs", "x1"], "--covs", "rdrobust"),
    (["rdrobust", "--data", pqe_hbin, "--y", "y", "--x", "running", "--c", "0",
      "--cluster", "cluster"], "--cluster", "rdrobust"),
    (["rddensity", "--data", pqe_hbin, "--x", "running", "--c", "0",
      "--fuzzy", "trt"], "--fuzzy", "rddensity"),
    (["rddensity", "--data", pqe_hbin, "--x", "running", "--c", "0",
      "--covs", "x1"], "--covs", "rddensity"),
    (["rddensity", "--data", pqe_hbin, "--x", "running", "--c", "0",
      "--cluster", "cluster"], "--cluster", "rddensity"),
    (["rdplot", "--data", pqe_hbin, "--y", "y", "--x", "running", "--c", "0",
      "--fuzzy", "trt"], "--fuzzy", "rdplot"),
    (["rdplot", "--data", pqe_hbin, "--y", "y", "--x", "running", "--c", "0",
      "--covs", "x1"], "--covs", "rdplot"),
    (["rdplot", "--data", pqe_hbin, "--y", "y", "--x", "running", "--c", "0",
      "--cluster", "cluster"], "--cluster", "rdplot"),
    (["did_imputation", "--data", pqe_hbin, "--y", "y", "--ivar", "id",
      "--time", "t", "--gvar", "gvar", "--weight", "x1"], "--weight", "did_imputation"),
    (["did_imputation", "--data", pqe_hbin, "--y", "y", "--ivar", "id",
      "--time", "t", "--gvar", "gvar", "--cluster", "cluster"], "--cluster", "did_imputation"),
    (["sdid", "--data", pqe_hbin, "--y", "y", "--ivar", "id", "--time", "t",
      "--treated", "trt", "--x", "x1"], "--x", "sdid"),
    (["sdid", "--data", pqe_hbin, "--y", "y", "--ivar", "id", "--time", "t",
      "--treated", "trt", "--method", "sc"], "--method", "sdid"),
]
n_refused = 0
for args, flag, cmd in refuse_by_name:
    rs = run(args, env=STRICT)
    err = rs.stderr or ""
    drift = "options-table drift" in err
    # (a) the honest refusal fired and named the flag
    refused = rs.returncode != 0 and flag in err and not drift
    check(f"BUG-182 refuse-by-name reached under strict: {cmd} {flag} "
          f"(honest refusal, no drift)", refused)
    if refused:
        n_refused += 1
# Vacuity guard: if the invocations stopped throwing (an estimator started
# ACCEPTING one of these, or the fixture stopped reaching the refusal), every
# row above would still "pass" the not-drift half while measuring nothing.
check("BUG-182 vacuity guard: all 13 refuse-by-name paths actually threw",
      n_refused == len(refuse_by_name))

# 6. AER scope-2 B2 wave (nnmatch/oaxaca): the same blind spot as section 4,
#    found again on 2026-09-09. `apps/cmds/cmds_nnmatch.cpp` consumes --level
#    via opt(), but the nnmatch row of options_table() was written before
#    --level existed and never grew it, so `cobra options --json` under-reported
#    to PIET exactly the BUG-110 way this file exists to prevent. Every gate in
#    the battery was GREEN because NOTHING drove either B2 command through the
#    CLI under COBRA_OPTIONS_STRICT: the eval cases and the freeze stanza all
#    take the default level, and this file had no arm for either name.
#
#    RED certificate (recorded 2026-09-09, reproduce by deleting {"--level",true}
#    from the nnmatch row of apps/cmds/options_table.cpp and rebuilding):
#      pre-fix  -> rc 3, code 'internal', "options-table drift: '--level'
#                  consumed by 'nnmatch' but not declared in options_table()"
#      post-fix -> rc 0                                                  [GREEN]
#    Every flag each row declares that a real invocation can carry is driven
#    here, so the next flag added to a handler without its row fails the same way.
b2_csv = os.path.join(work, "b2.csv")
b2_hbin = os.path.join(work, "b2.hbin")
with open(b2_csv, "w") as f:
    f.write("id,t,g,x1,x2,y\n")
    for i in range(1, 121):
        x1 = ((i * 7) % 23) / 23.0
        x2 = ((i * 13) % 17) / 17.0
        t = (i // 3) % 2
        g = i % 2
        y = 1.0 + 2.0 * x1 - 0.5 * x2 + 0.3 * g + 0.25 * t + ((i * 11) % 31) / 31.0 - 0.5
        f.write(f"{i},{t},{g},{x1},{x2},{y}\n")
if run(["convert", b2_csv, b2_hbin]).returncode != 0:
    print("b2 fixture convert failed", file=sys.stderr)
    sys.exit(1)

b2_runs = [
    (["nnmatch", "--data", b2_hbin, "--y", "y", "--t", "t", "--x", "x1,x2",
      "--fv", "g", "--nn", "2", "--metric", "ivariance", "--stat", "atet",
      "--vce-nn", "3", "--dtolerance", "1e-8", "--level", "90",
      "--keepif", "id > 1", "--tidy"], "nnmatch"),
    (["oaxaca", "--data", b2_hbin, "--y", "y", "--x", "x1,x2", "--by", "t",
      "--pooled", "--level", "90", "--keepif", "id > 1", "--tidy"], "oaxaca"),
]
for args, lbl in b2_runs:
    rs = run(args, env=STRICT)
    check(f"B2 strict-gate coverage: {lbl} runs to completion (rc 0, no drift)",
          rs.returncode == 0 and "options-table drift" not in (rs.stderr or ""))

if fails:
    print("FAILED:", ", ".join(fails))
    sys.exit(1)
print("PASSED")
