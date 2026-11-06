#!/usr/bin/env python3
"""Phase-D1 golden set driver — Eval Harness Gen (BETA campaign Phase D).

Gates piet_native's D1 wirings (ivregress 2sls/gmm/liml, ivreghdfe(absorb),
reg3 3sls+sure, reghdfe vce(cluster A B) 2-way, boottest engine-parity) against
a golden key hand-derived from a REAL StataBE 19.5 run
(internal capture d1-oracle-stata.log).

AUTHORED GOLDEN-FIRST: this driver + d1_expected.json exist before the D1
wiring lands on op/piet. Every `oracle_kind:"golden"` case is EXPECTED to fail
RED against the current build (see RED-baseline.txt) and must turn GREEN once
the maintainer lands the corresponding wiring. Standalone script — NOT wired
into run_eval.py's default gating set (see README.md "wiring" section for
what the maintainer must do to promote it into the standing gate).

--- Design note: why each case is an ISOLATED piet_native invocation --------
`piet_native --do FILE` halts the ENTIRE file at the first block whose type
is "error" (piet/native/src/doexec.cpp's per-line loop: `if (b.value("type",
"")=="error") { out.stoppedAtLine=...; return out; }`), and this is NOT
prevented by wrapping the failing command in `cap noi` — empirically verified
live (2026-07-15) against the current op-piet build: `cap noi ivregress ...`
still sets stopped_at_reason and halts, because dispatcher.cpp's capture-
prefix arm forwards the inner error block verbatim (to satisfy `noisily`'s
"show the output" contract) and doexec.cpp's halt-check does not distinguish
a capture-wrapped error from a top-level one. Concretely: a single do-file
containing all 11 D1 cases back-to-back would stop dead at whichever case is
LEAST supported today and never even reach the others — worse, once the maintainer
lands the D1 verbs in some partial/unordered sequence (e.g. reg3 before
ivregress), a monolithic script would make an already-GREEN-worthy case
falsely report FAIL merely because an earlier, still-unwired case halted the
script first. Running each case as its own `sysuse fatalities, clear` +
one-case do-file makes every case's PASS/FAIL independent of every other
case's wiring status — the correct property for a golden set spanning
multiple, independently-landing features. d1_cases.do (checked in alongside
this driver) is a human-readable REFERENCE mirroring the same commands in one
script (oracle-log style, `cap noi`-wrapped) for manual spot-checking once
several verbs have landed; it is not what this driver executes.

Usage:
  python3 run_d1_eval.py [--piet-bin PATH] [--only case_id,case_id] [--json OUT]
  PIET_BIN=/path/to/piet_native python3 run_d1_eval.py

Exit codes: 0 all gating cases GREEN · 1 at least one RED · 2 harness error
(bad PIET_BIN, bad JSON, etc).
"""
from __future__ import annotations

import argparse
import json
import math
import os
import subprocess
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent                 # tests/eval/piet_d1
ROOT = Path(__file__).resolve().parents[3]              # repo root (piet_d1 -> eval -> tests -> root)
EXPECTED_PATH = HERE / "d1_expected.json"

DEFAULT_PIET_BIN = ROOT / "piet" / "native" / "build" / "piet_native"
RUN_TIMEOUT_S = 60

# Constant-term spelling aliases — see d1_expected.json's _meta.coeftable_name_aliases.
_NAME_ALIASES = {"_cons": ("_cons", "(intercept)")}


def _aliases_for(name: str) -> tuple:
    return _NAME_ALIASES.get(name, (name,))


# ───────────────────────── piet_native invocation ─────────────────────────
def resolve_piet_bin(cli_arg: "str | None") -> Path:
    cand = cli_arg or os.environ.get("PIET_BIN") or str(DEFAULT_PIET_BIN)
    p = Path(cand)
    if not p.exists():
        print(f"ERROR: piet_native binary not found at {p}", file=sys.stderr)
        sys.exit(2)
    return p


def run_do(piet_bin: Path, do_lines: list) -> dict:
    """Run `sysuse fatalities, clear` + do_lines as one --do invocation.

    Returns {"rc", "stdout_json" (parsed dict or None), "stderr", "raw_rc"}.
    `sysuse fatalities` resolves via piet_native's OWN compiled-in dev-tree
    path (dispatcher.cpp devTreeSampleFatalitiesCsv() uses __FILE__ baked in
    at compile time) — NOT dependent on this process's cwd, verified live —
    so no cwd/data-dir plumbing is needed here regardless of which worktree
    piet_bin was built in.
    """
    body = "sysuse fatalities, clear\n" + "\n".join(do_lines) + "\n"
    with tempfile.NamedTemporaryFile("w", suffix=".do", delete=False) as f:
        f.write(body)
        do_path = f.name
    try:
        p = subprocess.run([str(piet_bin), "--do", do_path], capture_output=True,
                            text=True, timeout=RUN_TIMEOUT_S, stdin=subprocess.DEVNULL)
    except subprocess.TimeoutExpired:
        return {"rc": None, "stdout_json": None, "stderr": "TIMEOUT", "raw_rc": None,
                "do_path": do_path, "do_body": body}
    finally:
        pass
    parsed = None
    if p.stdout.strip().startswith("{"):
        try:
            parsed = json.loads(p.stdout)
        except json.JSONDecodeError:
            parsed = None
    return {"rc": p.returncode, "stdout_json": parsed, "stderr": p.stderr,
            "raw_stdout": p.stdout, "do_path": do_path, "do_body": body}


# ───────────────────────── numeric compare ─────────────────────────
def close(got, want, rtol, atol) -> bool:
    if got is None or want is None:
        return got == want
    try:
        got = float(got)
        want = float(want)
    except (TypeError, ValueError):
        return got == want
    if not (math.isfinite(got) and math.isfinite(want)):
        return got == want
    return abs(got - want) <= atol or abs(got - want) <= rtol * abs(want)


# ───────────────────────── block/coeftable lookup ─────────────────────────
def last_estimation_block(blocks: list) -> "dict | None":
    for b in reversed(blocks or []):
        if isinstance(b, dict) and b.get("type") == "estimation":
            return b
    return None


def coeftable_lookup(coeftable: list, name: str) -> "dict | None":
    if not isinstance(coeftable, list):
        return None
    for alias in _aliases_for(name):
        for row in coeftable:
            if isinstance(row, dict) and row.get("name") == alias:
                return row
    return None


def find_equation_block(top_block: dict, depvar: str) -> "dict | None":
    """Multi-equation (reg3) lookup — tries 'outcomes' (documented current
    plan, estimation_m4.cpp ~152-165), then a flat 'equations' key, matching
    each entry's 'depvar' field against the requested equation name."""
    for key in ("outcomes", "equations"):
        arr = top_block.get(key)
        if isinstance(arr, list):
            for eq in arr:
                if isinstance(eq, dict) and eq.get("depvar") == depvar:
                    return eq
    return None


def flat_qualified_coeftable_row(top_block: dict, depvar: str, coefname: str) -> "dict | None":
    """Fallback for a possible flat 'coeftable' with colon-qualified names
    (Stata e(b)-matrix style, 'depvar:varname'), in case the eventual reg3
    wiring does not use the 'outcomes'/'equations' shape at all."""
    ct = top_block.get("coeftable")
    if not isinstance(ct, list):
        return None
    qualified = f"{depvar}:{coefname}"
    for row in ct:
        if isinstance(row, dict) and row.get("name") in (qualified, coefname):
            return row
    return None


# ───────────────────────── per-case graders ─────────────────────────
def grade_golden_single(case: dict, run: dict, tol: dict) -> tuple:
    checks = []
    if run["stdout_json"] is None:
        return False, [{"q": "json_parse", "ok": False, "note": "stdout did not parse as JSON"}]
    j = run["stdout_json"]
    if j.get("stopped_at_reason"):
        return False, [{"q": "ran_to_completion", "ok": False,
                        "note": f"stopped_at_line={j.get('stopped_at_line')} reason={j.get('stopped_at_reason')!r}"}]
    block = last_estimation_block(j.get("blocks", []))
    if block is None:
        return False, [{"q": "estimation_block_present", "ok": False,
                        "note": "no block with type=='estimation' in output"}]
    ok_all = True
    n_obs = block.get("n_obs")
    ok = (n_obs == case["n_obs"])
    ok_all &= ok
    checks.append({"q": "n_obs", "ok": ok, "got": n_obs, "want": case["n_obs"]})
    for name, exp in case.get("coefs", {}).items():
        row = coeftable_lookup(block.get("coeftable"), name)
        if row is None:
            ok_all = False
            checks.append({"q": f"coeftable[{name}]", "ok": False, "note": "row not found (tried aliases)"})
            continue
        for field in ("b", "se"):
            got = row.get(field)
            want = exp[field]
            ok = close(got, want, tol["coef_rtol"], tol["coef_atol"])
            ok_all &= ok
            checks.append({"q": f"coeftable[{name}].{field}", "ok": ok, "got": got, "want": want})
    for field, want in case.get("scalars", {}).items():
        got = block.get(field)
        if got is None:
            checks.append({"q": f"scalar.{field}", "ok": False, "note": "field absent from block (schema note, not necessarily a real miss — see d1_expected.json _meta)"})
            # Scalars are a secondary check per _meta — do not fail the whole case on a missing field alone.
            continue
        ok = close(got, want, tol["scalar_rtol"], tol["scalar_atol"])
        ok_all &= ok
        checks.append({"q": f"scalar.{field}", "ok": ok, "got": got, "want": want})
    for field, want in case.get("scalar_dof", {}).items():
        got = block.get(field)
        if got is not None:
            ok = (got == want)
            ok_all &= ok
            checks.append({"q": f"dof.{field}", "ok": ok, "got": got, "want": want})
    if "n_clusters" in case:
        got = block.get("n_clusters")
        if got is not None:
            ok = (got == case["n_clusters"]) or (isinstance(got, (list, dict)) and case["n_clusters"] in (got if isinstance(got, list) else got.values()))
            ok_all &= ok
            checks.append({"q": "n_clusters", "ok": ok, "got": got, "want": case["n_clusters"]})
    return ok_all, checks


def grade_golden_multi(case: dict, run: dict, tol: dict) -> tuple:
    if run["stdout_json"] is None:
        return False, [{"q": "json_parse", "ok": False, "note": "stdout did not parse as JSON"}]
    j = run["stdout_json"]
    if j.get("stopped_at_reason"):
        return False, [{"q": "ran_to_completion", "ok": False,
                        "note": f"stopped_at_line={j.get('stopped_at_line')} reason={j.get('stopped_at_reason')!r}"}]
    block = last_estimation_block(j.get("blocks", []))
    if block is None:
        return False, [{"q": "estimation_block_present", "ok": False,
                        "note": "no block with type=='estimation' in output"}]
    ok_all = True
    checks = []
    n_obs = block.get("n_obs")
    ok = (n_obs is None) or (n_obs == case["n_obs"])   # top-level n_obs may be absent for multi-eq; not fatal
    if n_obs is not None:
        ok_all &= ok
        checks.append({"q": "n_obs", "ok": ok, "got": n_obs, "want": case["n_obs"]})
    for depvar, eqexp in case.get("equations", {}).items():
        eq = find_equation_block(block, depvar)
        for name, exp in eqexp.get("coefs", {}).items():
            row = None
            if eq is not None:
                row = coeftable_lookup(eq.get("coeftable"), name)
            if row is None:
                row = flat_qualified_coeftable_row(block, depvar, name)
            if row is None:
                ok_all = False
                checks.append({"q": f"{depvar}.coeftable[{name}]", "ok": False,
                               "note": "row not found in 'outcomes'/'equations'/flat-qualified coeftable (tried all 3 shapes)"})
                continue
            for field in ("b", "se"):
                got = row.get(field)
                want = exp[field]
                ok = close(got, want, tol["coef_rtol"], tol["coef_atol"])
                ok_all &= ok
                checks.append({"q": f"{depvar}.coeftable[{name}].{field}", "ok": ok, "got": got, "want": want})
        for field, want in eqexp.get("scalars", {}).items():
            got = (eq or {}).get(field)
            if got is None:
                checks.append({"q": f"{depvar}.scalar.{field}", "ok": False,
                               "note": "field absent — secondary check, documented possible engine-data gap (see d1_expected.json case notes); not fatal alone"})
                continue
            ok = close(got, want, tol["scalar_rtol"], tol["scalar_atol"])
            ok_all &= ok
            checks.append({"q": f"{depvar}.scalar.{field}", "ok": ok, "got": got, "want": want})
    return ok_all, checks


def grade_no_oracle(case: dict, run: dict) -> tuple:
    if run["stdout_json"] is None:
        return False, [{"q": "json_parse", "ok": False, "note": "stdout did not parse as JSON"}]
    j = run["stdout_json"]
    if j.get("stopped_at_reason"):
        return False, [{"q": "ran_to_completion", "ok": False,
                        "note": f"stopped_at_line={j.get('stopped_at_line')} reason={j.get('stopped_at_reason')!r} "
                                "(EXPECTED today — no wiring yet; this case has no numeric oracle so it is graded "
                                "structurally only, see d1_expected.json no_stata_oracle_reason)"}]
    block = last_estimation_block(j.get("blocks", []))
    if block is None:
        return False, [{"q": "estimation_block_present", "ok": False, "note": "no estimation block"}]
    req = case["structural_check"]["require_coeftable_row"]
    row = coeftable_lookup(block.get("coeftable"), req)
    if row is None:
        return False, [{"q": f"coeftable[{req}]", "ok": False, "note": "row not found"}]
    se = row.get("se")
    ok = isinstance(se, (int, float)) and math.isfinite(se) and se > 0
    return ok, [{"q": f"coeftable[{req}].se>0", "ok": ok, "got": se}]


def grade_boottest_parity(case: dict, piet_bin: Path) -> tuple:
    run1 = run_do(piet_bin, case["do_body"])
    run2 = run_do(piet_bin, case["do_body"])
    checks = []
    for tag, run in (("run1", run1), ("run2", run2)):
        ran_ok = run["stdout_json"] is not None and not run["stdout_json"].get("stopped_at_reason")
        checks.append({"q": f"{tag}.ran_to_completion", "ok": ran_ok,
                       "note": "" if ran_ok else str((run["stdout_json"] or {}).get("stopped_at_reason") or run.get("stderr", ""))[:200]})
    if not (checks[0]["ok"] and checks[1]["ok"]):
        checks.append({"q": "engine_parity", "ok": False,
                       "note": "boottest not supported today (both runs failed identically) — RED expected, "
                               "no p-value exists yet to compare; NOT a pass (see d1_expected.json grading note)"})
        return False, checks
    b1 = last_estimation_block(run1["stdout_json"].get("blocks", []))
    b2 = last_estimation_block(run2["stdout_json"].get("blocks", []))
    if b1 is None or b2 is None:
        checks.append({"q": "estimation_block_present", "ok": False, "note": "missing on at least one run"})
        return False, checks
    ok_all = True
    reps_ok = b1.get("reps") == case["reps_requested"]
    ok_all &= reps_ok
    checks.append({"q": "reps_forwarded", "ok": reps_ok, "got": b1.get("reps"), "want": case["reps_requested"]})
    # required_fields: must be PRESENT in both runs' boottest block — guards
    # against a vacuous "parity" where every parity field is simply absent.
    for field in case.get("required_fields", []):
        present = field in b1 and field in b2
        ok_all &= present
        checks.append({"q": f"required.{field}_present", "ok": present,
                       "note": "" if present else f"absent (run1 keys: {sorted(b1.keys())})"})
    for field in case["parity_fields"]:
        if field not in b1:
            continue
        ok = b1.get(field) == b2.get(field)
        ok_all &= ok
        checks.append({"q": f"parity.{field}", "ok": ok, "run1": b1.get(field), "run2": b2.get(field)})
    return ok_all, checks


# ───────────────────────── main ─────────────────────────
def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--piet-bin", default=None)
    ap.add_argument("--only", default="", help="comma list of case ids")
    ap.add_argument("--json", default="", help="optional path to dump machine-readable results")
    args = ap.parse_args()

    # Declare the engine arm rather than letting the spawned piet_native infer
    # one — without this the run silently grades against the pinned ARCHIVE
    # (last release) instead of the engine main just built, and says nothing
    # about it. See tools/engine_arm.py.
    sys.path.insert(0, str(ROOT / "tools"))
    import engine_arm
    _cobra = engine_arm.declare(engine_arm.resolve_cobra())
    print(f"[d1] {engine_arm.describe_arm(_cobra)}")
    engine_arm.pin_settings(os.environ, tempfile.mkdtemp(prefix="piet_d1_settings_"))  # BUG-388: never the user's live settings.json

    piet_bin = resolve_piet_bin(args.piet_bin)
    expected = json.loads(EXPECTED_PATH.read_text())
    tol_policy = expected["_meta"]["tolerance_policy"]
    tol = {"coef_rtol": tol_policy["coef_rtol"], "coef_atol": tol_policy["coef_atol"],
           "scalar_rtol": tol_policy["scalar_rtol"], "scalar_atol": tol_policy["scalar_atol"]}
    cases = expected["cases"]

    only = set(args.only.split(",")) if args.only else None
    results = []
    for cid, case in cases.items():
        if only and cid not in only:
            continue
        kind = case["oracle_kind"]
        if kind == "engine_parity":
            ok, checks = grade_boottest_parity(case, piet_bin)
        else:
            run = run_do(piet_bin, case["do_body"])
            if kind == "golden" and "equations" in case:
                ok, checks = grade_golden_multi(case, run, tol)
            elif kind == "golden":
                ok, checks = grade_golden_single(case, run, tol)
            elif kind == "no_oracle":
                ok, checks = grade_no_oracle(case, run)
            else:
                ok, checks = False, [{"q": "unknown_oracle_kind", "ok": False, "note": kind}]
        results.append({"id": cid, "oracle_kind": kind, "passed": ok, "checks": checks})

    # ---- print PASS/FAIL table ----
    width = max(len(r["id"]) for r in results)
    print(f"\n{'CASE':{width}}  KIND            RESULT")
    print("-" * (width + 30))
    for r in results:
        mark = "GREEN" if r["passed"] else "RED  "
        print(f"{r['id']:{width}}  {r['oracle_kind']:14s}  {mark}")
        if not r["passed"]:
            gap = cases[r["id"]].get("known_engine_gap")
            if gap:
                print(f"      [KNOWN ENGINE GAP — tracked, not mysterious] {gap}")
            for c in r["checks"]:
                if not c["ok"]:
                    extra = ""
                    if "got" in c and "want" in c:
                        extra = f" got={c['got']!r} want={c['want']!r}"
                    print(f"      - {c['q']}{extra}  {c.get('note','')}")
    n_pass = sum(r["passed"] for r in results)
    print(f"\n{n_pass}/{len(results)} D1 cases GREEN\n")

    if args.json:
        Path(args.json).write_text(json.dumps(results, indent=2, default=str))

    sys.exit(0 if n_pass == len(results) else 1)


if __name__ == "__main__":
    main()
