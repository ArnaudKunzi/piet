#!/usr/bin/env python3
"""Phase-D2+D3 golden set driver — Eval Harness Gen (BETA campaign Phase D).

Gates piet_native's D2 wirings (tobit ul/ll, heckman twostep + MLE
honest-reject, qreg median/quantile, mediate engine-parity) and D3 wirings
(poisson [+offset/robust], ppmlhdfe absorb, nbreg, xtpoisson fe, xtlogit fe,
drdid dripw) against d2d3_expected.json — hand-derived from real StataBE 19.5
oracle logs (see that file's _meta.oracle_logs).

Same architecture as tests/eval/piet_d1/run_d1_eval.py, for the same reasons
(see that driver's module docstring for the halt-at-first-error rationale):
every case runs as its OWN isolated `piet_native --do` invocation with a
fully self-contained do_body (dataset load + fixture gens + one command),
so each case's PASS/FAIL is independent of every other verb's wiring status.
D2 wirings are landed (GREEN expected); D3 wirings are in flight (RED
expected until they land — RED-baseline.txt records that state).

Standalone leg — NOT wired into run_eval.py's default gating set (that
ratification is the maintainer's at merge; see README.md).

Usage:
  python3 run_d2d3_eval.py [--piet-bin PATH] [--only case_id,...] [--json OUT]
  PIET_BIN=/path/to/piet_native python3 run_d2d3_eval.py
  DRDID_DTA=/path/to/drdid2x2.dta   (optional override for the d3h fixture)

Exit codes: 0 all cases GREEN · 1 at least one RED · 2 harness error.
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

HERE = Path(__file__).resolve().parent                 # tests/eval/piet_d2d3
ROOT = Path(__file__).resolve().parents[3]              # repo root
EXPECTED_PATH = HERE / "d2d3_expected.json"

DEFAULT_PIET_BIN = ROOT / "piet" / "native" / "build" / "piet_native"
RUN_TIMEOUT_S = 120

_NAME_ALIASES = {"_cons": ("_cons", "(intercept)")}
_CONS_SPELLINGS = ("_cons", "(intercept)")


def _aliases_for(name: str, case: dict) -> tuple:
    ca = case.get("coef_aliases", {})
    if name in ca:
        return tuple(ca[name])
    return _NAME_ALIASES.get(name, (name,))


# ───────────────────────── fixture path resolution ─────────────────────────
def resolve_drdid_dta() -> "str | None":
    """{DRDID_DTA} -> the drdid 2x2 fixture.

    Preference order: env override > the IN-LEG format-converted copy
    (drdid2x2_v118.dta — see below) > tests/validate/drdid2x2.dta > the main
    worktree original.

    Why the in-leg copy exists (2026-07-15): the ORIGINAL oracle fixture
    (<repo>/tests/validate/drdid2x2.dta, the exact file the Stata drdid oracle
    ran on) is legacy .dta FORMAT VERSION 114, which the engine's dta reader
    does not support — live-verified: `use <it>` fails with "unsupported
    legacy format version 114 (supported: 115; use Stata to `saveold`/`save`
    into 115/117/118)" BEFORE the drdid command is ever reached, which would
    leave d3h RED for a fixture-loading reason even after the drdid wiring
    lands (the same wrong-RED trap as piet_d1's original boottest fixture).
    drdid2x2_v118.dta is a pandas round-trip of the original into version
    118 — verified BIT-IDENTICAL column-by-column against the v114 source at
    conversion time (600 obs, 6 vars: id/t/Y/x1/x2/treat) — so the pinned
    oracle ATT/SE apply to it unchanged.
    """
    cands = []
    env = os.environ.get("DRDID_DTA")
    if env:
        cands.append(Path(env))
    cands.append(HERE / "drdid2x2_v118.dta")
    cands.append(ROOT / "tests" / "validate" / "drdid2x2.dta")
    for c in cands:
        try:
            if c.exists():
                return str(c)
        except OSError:
            continue
    return None


# ───────────────────────── piet_native invocation ─────────────────────────
def resolve_piet_bin(cli_arg: "str | None") -> Path:
    cand = cli_arg or os.environ.get("PIET_BIN") or str(DEFAULT_PIET_BIN)
    p = Path(cand)
    if not p.exists():
        print(f"ERROR: piet_native binary not found at {p}", file=sys.stderr)
        sys.exit(2)
    return p


def run_do(piet_bin: Path, do_lines: list) -> dict:
    """Run do_lines (already fully self-contained) as one --do invocation."""
    dta = resolve_drdid_dta()
    body_lines = []
    for ln in do_lines:
        if "{DRDID_DTA}" in ln:
            if dta is None:
                return {"rc": None, "stdout_json": None,
                        "stderr": "HARNESS: drdid2x2.dta fixture not found (tried $DRDID_DTA, "
                                  "tests/validate/, main worktree)", "raw_stdout": ""}
            ln = ln.replace("{DRDID_DTA}", f'"{dta}"')
        body_lines.append(ln)
    body = "\n".join(body_lines) + "\n"
    with tempfile.NamedTemporaryFile("w", suffix=".do", delete=False) as f:
        f.write(body)
        do_path = f.name
    try:
        p = subprocess.run([str(piet_bin), "--do", do_path], capture_output=True,
                            text=True, timeout=RUN_TIMEOUT_S, stdin=subprocess.DEVNULL)
    except subprocess.TimeoutExpired:
        return {"rc": None, "stdout_json": None, "stderr": "TIMEOUT", "raw_stdout": ""}
    parsed = None
    if p.stdout.strip().startswith("{"):
        try:
            parsed = json.loads(p.stdout)
        except json.JSONDecodeError:
            parsed = None
    return {"rc": p.returncode, "stdout_json": parsed, "stderr": p.stderr,
            "raw_stdout": p.stdout}


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


def coeftable_lookup(coeftable, name: str, case: dict) -> "dict | None":
    if not isinstance(coeftable, list):
        return None
    for alias in _aliases_for(name, case):
        for row in coeftable:
            if isinstance(row, dict) and row.get("name") == alias:
                return row
    return None


def find_equation_block(top_block: dict, depvar: str) -> "dict | None":
    for key in ("outcomes", "equations"):
        arr = top_block.get(key)
        if isinstance(arr, list):
            for eq in arr:
                if isinstance(eq, dict) and eq.get("depvar") == depvar:
                    return eq
    return None


# ───────────────────────── golden grader (unified) ─────────────────────────
def _completion_gate(run: dict) -> "list | None":
    """Returns a failure check-list if the run did not complete, else None."""
    if run["stdout_json"] is None:
        return [{"q": "json_parse", "ok": False,
                 "note": f"stdout did not parse as JSON ({(run.get('stderr') or '')[:150]})"}]
    j = run["stdout_json"]
    if j.get("stopped_at_reason"):
        return [{"q": "ran_to_completion", "ok": False,
                 "note": f"stopped_at_line={j.get('stopped_at_line')} reason={j.get('stopped_at_reason')!r}"}]
    return None


def grade_golden(case: dict, run: dict, tol: dict) -> tuple:
    fail = _completion_gate(run)
    if fail:
        return False, fail
    block = last_estimation_block(run["stdout_json"].get("blocks", []))
    if block is None:
        return False, [{"q": "estimation_block_present", "ok": False,
                        "note": "no block with type=='estimation' in output"}]
    ct = tol.copy()
    ct.update(case.get("tol_override", {}))
    ok_all = True
    checks = []

    # n_obs — exact, fatal
    n_obs = block.get("n_obs")
    ok = (n_obs == case["n_obs"])
    ok_all &= ok
    checks.append({"q": "n_obs", "ok": ok, "got": n_obs, "want": case["n_obs"]})

    def check_coef_rows(coefs: dict, table, prefix: str):
        nonlocal ok_all
        for name, exp in coefs.items():
            row = coeftable_lookup(table, name, case)
            if row is None and case.get("att_fallback") and name == "treat":
                # drdid fallback: top-level att/se_att (or att/se) scalar pair
                att = block.get("att")
                se_att = block.get("se_att", block.get("se"))
                if isinstance(att, (int, float)):
                    row = {"b": att, "se": se_att}
            if row is None:
                ok_all = False
                checks.append({"q": f"{prefix}coeftable[{name}]", "ok": False,
                               "note": "row not found (tried aliases"
                                       + (", att fallback" if case.get("att_fallback") else "") + ")"})
                continue
            for field in ("b", "se"):
                got = row.get(field)
                want = exp[field]
                ok = close(got, want, ct["coef_rtol"], ct["coef_atol"])
                ok_all &= ok
                checks.append({"q": f"{prefix}coeftable[{name}].{field}", "ok": ok,
                               "got": got, "want": want})

    # top-level coefs
    if "coefs" in case:
        check_coef_rows(case["coefs"], block.get("coeftable"), "")

    # multi-equation coefs
    for depvar, eqexp in case.get("equations", {}).items():
        eq = find_equation_block(block, depvar)
        table = eq.get("coeftable") if eq is not None else None
        check_coef_rows(eqexp.get("coefs", {}), table, f"{depvar}.")

    # forbid_cons: conditional-likelihood estimators must NOT fabricate a constant
    if case.get("forbid_cons"):
        found = None
        for sp in _CONS_SPELLINGS:
            r = coeftable_lookup(block.get("coeftable"), sp, {})
            if r is not None:
                found = sp
                break
        ok = found is None
        ok_all &= ok
        checks.append({"q": "no_constant_row", "ok": ok,
                       "note": "" if ok else f"conditional-FE estimator fabricated a constant row ({found})"})

    # required scalars — fatal, absent or mismatched
    for field, want in case.get("required_scalars", {}).items():
        got = block.get(field)
        if got is None:
            ok_all = False
            checks.append({"q": f"required_scalar.{field}", "ok": False, "note": "field absent (FATAL — live-verified schema field)"})
            continue
        ok = close(got, want, ct["scalar_rtol"], ct["scalar_atol"])
        ok_all &= ok
        checks.append({"q": f"required_scalar.{field}", "ok": ok, "got": got, "want": want})

    # exact scalars — fatal, integer equality
    for field, want in case.get("exact_scalars", {}).items():
        got = block.get(field)
        ok = (got == want)
        ok_all &= ok
        checks.append({"q": f"exact_scalar.{field}", "ok": ok, "got": got, "want": want})

    # secondary scalars — absent = note (non-fatal); present-but-wrong = fatal
    for field, want in case.get("scalars", {}).items():
        got = block.get(field)
        if got is None:
            checks.append({"q": f"scalar.{field}", "ok": False,
                           "note": "field absent (secondary — schema note, non-fatal; see _meta.check_severity_model)"})
            continue
        ok = close(got, want, ct["scalar_rtol"], ct["scalar_atol"])
        ok_all &= ok
        checks.append({"q": f"scalar.{field}", "ok": ok, "got": got, "want": want})

    return ok_all, checks


# ───────────────────────── reject grader ─────────────────────────
def grade_reject(case: dict, run: dict) -> tuple:
    checks = []
    if run["stdout_json"] is None:
        return False, [{"q": "json_parse", "ok": False, "note": "stdout did not parse as JSON"}]
    j = run["stdout_json"]
    reason = j.get("stopped_at_reason") or ""
    ok_stop = bool(reason)
    checks.append({"q": "run_stopped", "ok": ok_stop,
                   "note": "" if ok_stop else "run completed — the unsupported form was silently accepted"})
    ok_all = ok_stop
    for sub in case.get("expect_stop_contains", []):
        ok = sub in reason
        ok_all &= ok
        checks.append({"q": f"stop_reason_contains[{sub!r}]", "ok": ok,
                       "note": "" if ok else f"reason={reason[:160]!r}"})
    want_code = case.get("expect_error_code")
    if want_code:
        got_code = None
        for b in reversed(j.get("blocks", [])):
            if isinstance(b, dict) and b.get("type") == "error":
                got_code = b.get("code")
                break
        ok = got_code == want_code
        ok_all &= ok
        checks.append({"q": "error.code", "ok": ok, "got": got_code, "want": want_code})
    return ok_all, checks


# ───────────────────────── engine-parity grader (mediate) ─────────────────────────
def canon(obj) -> str:
    return json.dumps(obj, sort_keys=True)


def grade_engine_parity(case: dict, piet_bin: Path) -> tuple:
    run1 = run_do(piet_bin, case["do_body"])
    run2 = run_do(piet_bin, case["do_body"])
    checks = []
    for tag, run in (("run1", run1), ("run2", run2)):
        fail = _completion_gate(run)
        checks.append({"q": f"{tag}.ran_to_completion", "ok": fail is None,
                       "note": "" if fail is None else fail[0].get("note", "")[:200]})
    if not (checks[0]["ok"] and checks[1]["ok"]):
        return False, checks
    b1 = last_estimation_block(run1["stdout_json"].get("blocks", []))
    b2 = last_estimation_block(run2["stdout_json"].get("blocks", []))
    if b1 is None or b2 is None:
        checks.append({"q": "estimation_block_present", "ok": False, "note": "missing on at least one run"})
        return False, checks
    ok_all = True

    # (2) treatment_levels tag — the continuous(#L0 #L1) parse proof
    want_levels = case["treatment_levels"]
    got_levels = b1.get("treatment_levels")
    ok = got_levels == want_levels
    ok_all &= ok
    checks.append({"q": "treatment_levels", "ok": ok, "got": got_levels, "want": want_levels})

    # (3) bootstrap options genuinely forwarded
    boot = b1.get("bootstrap") or {}
    ok = boot.get("reps") == case["bootstrap_reps_requested"]
    ok_all &= ok
    checks.append({"q": "bootstrap.reps_forwarded", "ok": ok,
                   "got": boot.get("reps"), "want": case["bootstrap_reps_requested"]})
    ok = boot.get("seed") == case["bootstrap_seed"]
    ok_all &= ok
    checks.append({"q": "bootstrap.seed_forwarded", "ok": ok,
                   "got": boot.get("seed"), "want": case["bootstrap_seed"]})

    # (4) full-block fixed-seed reproducibility, byte-identical canonical JSON
    ok = canon(b1) == canon(b2)
    ok_all &= ok
    note = ""
    if not ok:
        diffs = [k for k in set(b1) | set(b2) if canon(b1.get(k)) != canon(b2.get(k))]
        note = f"fields differing across identical-seed runs: {sorted(diffs)}"
    checks.append({"q": "full_block_parity", "ok": ok, "note": note})
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
    print(f"[d2d3] {engine_arm.describe_arm(_cobra)}")
    engine_arm.pin_settings(os.environ, tempfile.mkdtemp(prefix="piet_d2d3_settings_"))  # BUG-388: never the user's live settings.json

    piet_bin = resolve_piet_bin(args.piet_bin)
    expected = json.loads(EXPECTED_PATH.read_text())
    tp = expected["_meta"]["tolerance_policy"]
    tol = {"coef_rtol": tp["coef_rtol"], "coef_atol": tp["coef_atol"],
           "scalar_rtol": tp["scalar_rtol"], "scalar_atol": tp["scalar_atol"]}
    cases = expected["cases"]

    only = set(args.only.split(",")) if args.only else None
    results = []
    for cid, case in cases.items():
        if only and cid not in only:
            continue
        kind = case["oracle_kind"]
        if kind == "engine_parity":
            ok, checks = grade_engine_parity(case, piet_bin)
        else:
            run = run_do(piet_bin, case["do_body"])
            if kind == "golden":
                ok, checks = grade_golden(case, run, tol)
            elif kind == "reject":
                ok, checks = grade_reject(case, run)
            else:
                ok, checks = False, [{"q": "unknown_oracle_kind", "ok": False, "note": kind}]
        results.append({"id": cid, "oracle_kind": kind, "passed": ok, "checks": checks})

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
    print(f"\n{n_pass}/{len(results)} D2+D3 cases GREEN\n")

    if args.json:
        Path(args.json).write_text(json.dumps(results, indent=2, default=str))

    sys.exit(0 if n_pass == len(results) else 1)


if __name__ == "__main__":
    main()
