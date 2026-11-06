#!/usr/bin/env python3
# BUG-060 ctest gate: E6 CLI unknown-flag rejection net (opt()/has()/opt_all()
# consumed-argv tracking + dispatch_guard in apps/cecon_cli.cpp).
#
# VERIFIED (PLAYBOOK.md P4_OPTIONS / pre-wave audit): an unrecognized `--flag`
# was silently ignored by every command (Stata itself errors r(198) for the
# same mistake). This gate pins the fix: any argv token shaped like a flag
# ("--xxx") that no opt()/has()/opt_all() call in the dispatched command ever
# looked up is now rejected — bad_input, naming the flag — while every REAL
# flag a normal invocation supplies (including ones read only deep inside a
# conditional branch, e.g. --cluster mattering only under --vcov cluster)
# keeps working exactly as before. cmd_eval, whose --gen/--egen/... mini-language
# is read by its own scanner rather than opt(), used to be exempt from the net;
# BUG-1181 wired it in, and the arms at the end pin that.
#
# Self-contained (builds its own CSV fixture; no external data dependency).
# usage: validate_e6_unknown_flag.py <cobra-binary>
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


work = tempfile.mkdtemp(prefix="e6_unknown_flag_")
csv_path = os.path.join(work, "e6.csv")
hbin = os.path.join(work, "e6.hbin")

# 20 rows, 4 clusters of 5 — enough for a real --vcov cluster --cluster run.
rows = []
for i in range(1, 21):
    cl = (i - 1) // 5 + 1  # 1..4
    x = 0.4 * i
    y = 3.0 + 0.6 * x + (0.3 * cl)
    rows.append((i, x, y, cl))
with open(csv_path, "w") as f:
    f.write("id,x,y,cl\n")
    for r in rows:
        f.write(",".join(str(v) for v in r) + "\n")

conv = run_raw(["convert", csv_path, hbin])
if conv.returncode != 0:
    print("convert failed:", conv.stderr, file=sys.stderr)
    sys.exit(1)

base = ["feols", "--data", hbin, "--y", "y", "--x", "x"]

# --- Valid invocations must be UNAFFECTED: every real flag a normal user
# supplies keeps working, including a flag combo only meaningful/read together
# (--cluster is only consulted downstream when --vcov cluster is also given,
# but opt() looks it up unconditionally, so it's never wrongly rejected). ---
j_plain = run_ok(base, "plain feols: runs")
check("plain feols: runs (rc 0)", j_plain is not None and j_plain.get("estimator") == "feols")

j_cluster = run_ok(base + ["--vcov", "cluster", "--cluster", "cl"], "feols vcov=cluster: runs")
check("feols --vcov cluster --cluster cl: runs (rc 0) — a real flag combo",
      j_cluster is not None and str(j_cluster.get("vcov_type") or "").startswith("cluster"))

outpath = os.path.join(work, "out.json")
r_out = run_raw(base + ["--keepif", "id>0", "--out", outpath])
check("feols --keepif id>0 --out FILE: runs (rc 0) — real flags stay accepted together",
      r_out.returncode == 0 and os.path.exists(outpath))

# A trailing valid GLOBAL flag (--tidy, read in main() before dispatch) must
# never be mistaken for an unrecognized per-command flag.
j_tidy = run_ok(base + ["--tidy"], "feols --tidy: runs")
check("feols --tidy: runs (rc 0) — global flag stays accepted", j_tidy is not None)

# --- Bogus flag must be REJECTED, loudly, naming the flag ---
r_bad = run_raw(base + ["--floob", "xyz"])
check("feols --floob xyz: rejected (rc != 0)", r_bad.returncode != 0)
try:
    err = json.loads(r_bad.stderr)
    check("feols --floob: structured error code 'bad_input'",
          err.get("error", {}).get("code") == "bad_input")
    check("feols --floob: error names the bogus flag ('--floob')",
          "--floob" in err.get("error", {}).get("message", "") or
          err.get("error", {}).get("detail") == "--floob")
except (json.JSONDecodeError, ValueError):
    check("feols --floob: stderr is structured JSON", False)

# A bogus flag appended AFTER an otherwise-complete, valid invocation must
# ALSO be rejected — this is exactly the audited scenario (regress/summarize
# ran fine to completion; the option itself just silently vanished).
r_bad2 = run_raw(base + ["--vcov", "cluster", "--cluster", "cl", "--floob", "xyz"])
check("feols (valid+extra) --floob xyz: rejected (rc != 0), not silently run-and-ignored",
      r_bad2.returncode != 0)

# A different, flag-free command (cmd_info takes no options at all beyond the
# positional path) rejects a bogus flag too — the net isn't feols-specific.
r_info_bad = run_raw(["info", hbin, "--floob"])
check("info <file> --floob: rejected (rc != 0)", r_info_bad.returncode != 0)

# --- BUG-1181: `eval` is no longer exempt. An unknown flag on it was silently
# ignored -- `cobra eval ... --store-type float --gen g2=1/3` on an engine that
# predates --store-type wrote doubles at rc 0. Its ordered ops, read by its own
# scanner, must stay accepted next to its opt()-read flags, also under the
# options-table drift check (every op is declared in options_table()).
ev_out = os.path.join(work, "ev.hbin")
ev_ok = ["eval", "--data", hbin, "--gen", "z=x*2", "--egen", "m=mean(y), by(cl)",
         "--bygen", "s=sum(x), by(cl), sort(id)", "--keepif", "id>2", "--drop", "m",
         "--summarize", "z", "--detail", "--gen-strict", "--store-type", "double",
         "--out", ev_out]
r_ev = run_raw(ev_ok)
check("eval with ordered ops + --summarize --detail + --out: runs (rc 0)",
      r_ev.returncode == 0 and os.path.exists(ev_out))
strict_env = dict(os.environ, COBRA_OPTIONS_STRICT="1")
r_ev_strict = subprocess.run([COBRA] + ev_ok, capture_output=True, text=True, env=strict_env)
check("eval ops under COBRA_OPTIONS_STRICT: rc 0, no options-table drift",
      r_ev_strict.returncode == 0 and "options-table drift" not in r_ev_strict.stderr)
r_ev_ts = run_raw(["eval", "--data", hbin, "--gen", "lx=L.x", "--tsset", "id",
                   "--tsdelta", "1", "--out", ev_out])
check("eval --gen L.x --tsset --tsdelta: runs (rc 0)", r_ev_ts.returncode == 0)
r_ev_bad = run_raw(["eval", "--data", hbin, "--gen", "z=x*2", "--floob", "xyz",
                    "--out", ev_out])
check("eval --floob xyz: rejected (rc != 0), not silently ignored", r_ev_bad.returncode != 0)
try:
    # the one-shot `wrote <out>` notice may precede the error object on stderr
    err = json.loads(r_ev_bad.stderr[r_ev_bad.stderr.find("{"):])
    check("eval --floob: structured error code 'bad_input' naming the flag",
          err.get("error", {}).get("code") == "bad_input" and
          "--floob" in err.get("error", {}).get("message", ""))
except (json.JSONDecodeError, ValueError):
    check("eval --floob: stderr is structured JSON", False)

if fails:
    print("FAILED:", ", ".join(fails))
    sys.exit(1)
print("PASSED")
