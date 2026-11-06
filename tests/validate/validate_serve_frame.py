#!/usr/bin/env python3
# contract_A §26 ctest gate: `cobra serve` RESIDENT-FRAME session (MAXPERF Stage 1b).
#
# Drives one long-lived `cobra serve` process over the stdin/stdout line-JSON
# protocol through a scripted session and pins the §26 contract:
#   use  -> frame.vars (rows/cols/type/n_missing/n_levels)
#   run eval --gen (MUTATOR)      -> resident mutates in place; frame.added=[z]
#   run feols       (READ-ONLY)   -> reads the resident state; numbers match the
#                                    one-shot `cobra feols --data <snapshot>` (PARITY LAW ①)
#   run <bad flag>                -> nonzero exit AND the session is left INTACT
#                                    (the very next run still sees z, unchanged)
#   stateless {"argv":[...]}      -> a §25 request interleaves without clobbering the resident
#   snapshot -> a durable .hbin whose values equal the resident state
#   run keepif (STRUCTURAL)       -> frame.structure=true, rows shrink; snapshot still faithful
#   drop -> frees the frame; a subsequent run -> exit 3 "no frame in session"
#
# Self-contained (builds its own CSV fixture; no external data dependency).
# usage: validate_serve_frame.py <cobra-binary>
import json, os, subprocess, sys, tempfile

from _cobra_bin import resolve_cobra  # noqa: E402  (sibling module; see it for why)

COBRA = resolve_cobra()
fails = []


def check(name, cond):
    print(("  ok   " if cond else "  FAIL ") + name)
    if not cond:
        fails.append(name)


def run_raw(args, **kw):
    return subprocess.run([COBRA] + args, capture_output=True, text=True, **kw)


work = tempfile.mkdtemp(prefix="serve_frame_")
csv_path = os.path.join(work, "f.csv")
hbin = os.path.join(work, "f.hbin")
snap = os.path.join(work, "snap.hbin")
snap2 = os.path.join(work, "snap2.hbin")

# 20 rows; y is an exact linear function of x so feols coef is deterministic. `w`
# carries ONE missing value (".") so n_missing is exercised.
rows = []
for i in range(1, 21):
    x = 0.5 * i
    y = 3.0 + 2.0 * x
    w = "." if i == 3 else str(1.0 * i)
    rows.append((i, x, y, w))
with open(csv_path, "w") as fh:
    fh.write("id,x,y,w\n")
    for r in rows:
        fh.write(",".join(str(v) for v in r) + "\n")

conv = run_raw(["convert", csv_path, hbin])
if conv.returncode != 0:
    print("convert failed:", conv.stderr, file=sys.stderr)
    sys.exit(1)

# ---- drive one serve process through the whole session ---------------------------
proc = subprocess.Popen([COBRA, "serve"], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                        text=True, bufsize=1)


def frame_req(obj):
    proc.stdin.write(json.dumps(obj) + "\n")
    proc.stdin.flush()
    line = proc.stdout.readline()
    return json.loads(line)


handshake = json.loads(proc.stdout.readline())
check("handshake announces serve:1", handshake.get("serve") == 1)

# --- use ---
r = frame_req({"id": 1, "frame": {"op": "use", "path": hbin}})
check("use: exit 0", r.get("exit") == 0)
fr = r.get("frame", {})
check("use: frame.rows == 20", fr.get("rows") == 20)
check("use: frame.cols == 4", fr.get("cols") == 4)
vars_by = {v["name"]: v for v in fr.get("vars", [])}
check("use: vars carry id,x,y,w", set(["id", "x", "y", "w"]).issubset(vars_by.keys()))
check("use: x is f64 with 0 missing", vars_by.get("x", {}).get("type") == "f64" and
      vars_by.get("x", {}).get("n_missing") == 0)
check("use: w reports its 1 missing value", vars_by.get("w", {}).get("n_missing") == 1)

# --- run eval --gen (mutator): z = x + 2, persists into the resident ---
r = frame_req({"id": 2, "frame": {"op": "run", "argv": ["eval", "--gen", "z=x+2"]}})
check("gen: exit 0", r.get("exit") == 0)
fr = r.get("frame", {})
check("gen: z reported as added", "z" in fr.get("added", []))
check("gen: not a structural change", fr.get("structure") is False)
vd = {v["name"]: v for v in fr.get("vars_delta", [])}
check("gen: vars_delta has z as f64", vd.get("z", {}).get("type") == "f64")

# --- run feols (read-only): reads the resident state incl. the just-added z ---
r = frame_req({"id": 3, "frame": {"op": "run", "argv": ["feols", "--y", "y", "--x", "x"]}})
check("feols(resident): exit 0", r.get("exit") == 0)
feols_resident = None
try:
    feols_resident = json.loads(r.get("stdout", ""))
except ValueError:
    pass
check("feols(resident): parses, estimator=feols",
      feols_resident is not None and feols_resident.get("estimator") == "feols")
check("feols(resident): read-only run reports no column changes",
      r.get("frame", {}).get("changed") == [] and r.get("frame", {}).get("added") == [])

# --- a BAD run (unknown flag) on a MUTATOR must fail AND leave the session intact ---
# `duplicates tag` goes through the unknown-flag net (unlike eval's own hand-rolled
# scanner, which silently ignores unknown flags): it STAGES a new column 'newq', then
# --floob makes check_unconsumed_flags throw AFTER the stage. The atomic
# commit-on-success rule must then DISCARD the swap (newq must not appear).
r_bad = frame_req({"id": 4, "frame": {"op": "run",
                   "argv": ["duplicates", "tag", "--vars", "x", "--gen", "newq", "--floob", "z"]}})
check("bad run: nonzero exit", r_bad.get("exit", 0) != 0)
check("bad run: no frame swap reported (frame omitted)", "frame" not in r_bad)
# state intact: z still present (mean 7.25), and the staged 'newq' must be gone (cols still 5).
r = frame_req({"id": 5, "frame": {"op": "run", "argv": ["eval", "--summarize", "z"]}})
log = json.loads(r.get("stdout", "{}"))
zmean = log.get("summarize", {}).get("z", {}).get("mean")
# z = x + 2, x = 0.5*i for i=1..20 -> mean(x)=5.25 -> mean(z)=7.25
check("state intact after bad run: z still summarizes (mean 7.25)",
      zmean is not None and abs(zmean - 7.25) < 1e-9)
check("state intact after bad run: staged 'newq' discarded (n_cols still 5)",
      log.get("n_cols") == 5)

# --- a STATELESS §25 request interleaves without clobbering the resident ---
proc.stdin.write(json.dumps({"id": 6, "argv": ["info", hbin]}) + "\n")
proc.stdin.flush()
r_stateless = json.loads(proc.stdout.readline())
check("stateless info: exit 0, no frame member", r_stateless.get("exit") == 0 and "frame" not in r_stateless)
r = frame_req({"id": 7, "frame": {"op": "run", "argv": ["eval", "--summarize", "z"]}})
check("resident survived the stateless request (z still present)",
      "z" in json.loads(r.get("stdout", "{}")).get("summarize", {}))

# --- snapshot: durable .hbin equal to the resident state ---
r = frame_req({"id": 8, "frame": {"op": "snapshot", "path": snap}})
check("snapshot: exit 0", r.get("exit") == 0)
check("snapshot: file written", os.path.exists(snap))
# one-shot read of the snapshot: z present with the right mean
one = run_raw(["eval", "--data", snap, "--summarize", "z"])
osumm = json.loads(one.stdout) if one.returncode == 0 else {}
check("snapshot values faithful: z mean 7.25 on disk",
      abs(osumm.get("summarize", {}).get("z", {}).get("mean", -1) - 7.25) < 1e-9)

# --- PARITY LAW ①: frame-run feols numbers == one-shot feols on the materialized state ---
one_feols = run_raw(["feols", "--data", snap, "--y", "y", "--x", "x"])
oj = json.loads(one_feols.stdout) if one_feols.returncode == 0 else {}
if feols_resident and oj:
    cb = feols_resident.get("coef", {}).get("x")
    ob = oj.get("coef", {}).get("x")
    check("PARITY: resident feols coef[x] == one-shot feols coef[x] on snapshot",
          cb is not None and ob is not None and abs(cb - ob) < 1e-12)
else:
    check("PARITY: both feols runs produced coef", False)

# --- STRUCTURAL run: keep_if shrinks rows; structure flag set; snapshot still faithful ---
r = frame_req({"id": 9, "frame": {"op": "run", "argv": ["eval", "--keepif", "x>5"]}})
check("keepif: exit 0", r.get("exit") == 0)
fr = r.get("frame", {})
check("keepif: flagged structural", fr.get("structure") is True)
# x = 0.5*i > 5 -> i > 10 -> i in 11..20 -> 10 rows
check("keepif: rows shrank to 10", fr.get("rows") == 10)
r = frame_req({"id": 10, "frame": {"op": "snapshot", "path": snap2}})
check("snapshot after structural change: exit 0", r.get("exit") == 0)
info2 = run_raw(["info", snap2])
check("post-keepif snapshot has 10 rows on disk", "rows=10" in info2.stdout)

# --- contract_A §31: post-estimation by handle (session estimation store) ---------
# The `est` member is ADDITIVE on the §26 response, so this block is guarded by a
# day-1 probe exactly like serve_session's §27 legs: a binary without §31 answers
# `postest` through the generic unknown-op path (exit 2, "unknown frame op"), and
# must SKIP rather than RED -- otherwise every branch that has not yet merged the
# engine change fails ctest. Set COBRA_REQUIRE_POSTEST=1 (release time) to make the
# absence itself a failure, so the skip can never quietly become permanent.
probe = frame_req({"id": 20, "frame": {"op": "postest", "est": "est:0:__probe__",
                                       "argv": ["lincom", "--exp", "_cons"]}})
postest_live = probe.get("exit") == 3 and "no stored model" in (probe.get("stderr") or "")
if not postest_live:
    msg = (f"contract_A §31 (postest) not present on this binary "
           f"(probe exit={probe.get('exit')!r}) -- §31 checks SKIPPED")
    if os.environ.get("COBRA_REQUIRE_POSTEST"):
        check("§31 required by COBRA_REQUIRE_POSTEST but absent: " + msg, False)
    else:
        print("  skip  " + msg)
else:
    # A frame-run post-estimation call from the FULL specification fits and MINTS.
    # This is the path PIET already takes today (session_state.cpp routes lincom
    # resident: --data present, no --out, not a mutation), so it is live from the
    # merge even while the postest CONSUMER side is still unwired.
    r = frame_req({"id": 21, "frame": {"op": "run", "argv": ["lincom", "--y", "y", "--x", "x",
                                                             "--exp", "x"]}})
    check("§31 mint: frame-run lincom exit 0", r.get("exit") == 0)
    est = r.get("est")
    check("§31 mint: response carries an 'est' object", isinstance(est, dict))
    est = est if isinstance(est, dict) else {}
    handle = est.get("handle")
    check("§31 mint: est.handle is a non-empty string",
          isinstance(handle, str) and len(handle) > 0)
    check("§31 mint: est carries k / n_obs / vcov_type / fits",
          all(k in est for k in ("k", "n_obs", "vcov_type", "fits")))
    # This block runs AFTER the structural keepif above (x>5 -> i in 11..20), so the
    # resident carries 10 rows, not the original 20. Asserting the post-filter count
    # is deliberate: it proves the mint fitted against the CURRENT resident state
    # rather than against the file the session was opened from.
    check("§31 mint: est.n_obs == 10 (the post-keepif resident, not the original file)",
          est.get("n_obs") == 10)
    mint_out = json.loads(r.get("stdout", "{}") or "{}")
    mint_b = (mint_out.get("coefs") or [{}])[0].get("b")

    # Spend the handle: same numbers, NO new fit, and no `frame` member (no data read).
    r2 = frame_req({"id": 22, "frame": {"op": "postest", "est": handle,
                                        "argv": ["lincom", "--exp", "x"]}})
    check("§31 postest: exit 0", r2.get("exit") == 0)
    est2 = r2.get("est") if isinstance(r2.get("est"), dict) else {}
    check("§31 postest: est.fits did NOT climb (the re-fit is gone)",
          est2.get("fits") is not None and est2.get("fits") == est.get("fits"))
    check("§31 postest: handle echoed", est2.get("handle") == handle)
    check("§31 postest: no 'frame' member (no data was read)", "frame" not in r2)
    post_b = (json.loads(r2.get("stdout", "{}") or "{}").get("coefs") or [{}])[0].get("b")
    check("§31 postest: b identical to the minting call",
          mint_b is not None and post_b == mint_b)

    # An unresolvable handle is an explicit error with NO stdout -- never a re-fit.
    r3 = frame_req({"id": 23, "frame": {"op": "postest", "est": "est:ffffffff:999999",
                                        "argv": ["lincom", "--exp", "x"]}})
    check("§31 unknown handle: exit 3, empty stdout",
          r3.get("exit") == 3 and not r3.get("stdout"))

    # The resident frame is untouched by all of the above.
    r4 = frame_req({"id": 24, "frame": {"op": "run", "argv": ["eval", "--summarize", "z"]}})
    check("§31: resident intact after mint+postest (z still present)",
          "z" in json.loads(r4.get("stdout", "{}") or "{}").get("summarize", {}))

# --- drop, then a run must report no frame in session (exit 3) ---
r = frame_req({"id": 11, "frame": {"op": "drop"}})
check("drop: exit 0", r.get("exit") == 0)
r = frame_req({"id": 12, "frame": {"op": "run", "argv": ["eval", "--summarize", "x"]}})
check("run after drop: exit 3 (no frame in session)", r.get("exit") == 3)

# --- clean shutdown ---
proc.stdin.write(json.dumps({"argv": ["shutdown"]}) + "\n")
proc.stdin.flush()
proc.stdin.close()
proc.wait(timeout=10)

if fails:
    print("FAILED:", ", ".join(fails))
    sys.exit(1)
print("PASSED")
