#!/usr/bin/env python3
"""Phase-D4+D5 golden set driver — Eval Harness Gen (BETA campaign Phase D).

Gates piet_native's D4 wirings (newey, prais twostep/corc, dfuller, pperron,
wntestq, tsfill) and D5 wirings (sdtest, estat durbinalt, estat firststage +
the endogenous/overid honest-rejects, margins dydx-after-logit + the
at()-after-logit honest-reject, the hausman estimates-store idiom, chow,
mkspline) against d45_expected.json — hand-derived from the real StataBE
19.5 oracle (internal capture
d45-oracle-stata.log).

Same architecture as tests/eval/piet_d1 / piet_d2d3 (see those READMEs for
the shared rationale): every case is an ISOLATED `piet_native --do`
invocation with a fully self-contained do_body. UNLIKE those legs, the D4/D5
wirings were already landed (@ff9d06a) at authoring time — this leg is the
independent Eval-owned regression net (current-status, not RED-first).

Extensions over the piet_d2d3 driver:
  * scalar keys are jget-style paths ("classic.F", "firststage[beertax].F",
    "coefs[beertax].diff") — dots descend dicts, [name] matches list
    elements by their "name" field, [N] is an index;
  * a resolved single-element list is unwrapped before numeric compare
    (estat durbinalt emits per-lag one-element arrays);
  * exact_scalars may pin lists/strings (group_sizes, case labels) — raw
    == compare, no unwrap when the expected value is itself a list;
  * n_obs is only checked when the case declares it (hausman/firststage
    blocks carry none/0 — live-verified);
  * oracle_kind "mechanical": completion + display_sequence (ordered count
    outputs, for tsfill) and/or summarize_check (spline vars materialized).

Standalone leg — NOT wired into run_eval.py's default gating set.

Usage:
  python3 run_d45_eval.py [--piet-bin PATH] [--only case_id,...] [--json OUT]
  PIET_BIN=/path/to/piet_native python3 run_d45_eval.py

Exit codes: 0 all cases GREEN · 1 at least one RED · 2 harness error.
"""
from __future__ import annotations

import argparse
import json
import math
import os
import re
import subprocess
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent                 # tests/eval/piet_d45
ROOT = Path(__file__).resolve().parents[3]              # repo root
EXPECTED_PATH = HERE / "d45_expected.json"

DEFAULT_PIET_BIN = ROOT / "piet" / "native" / "build" / "piet_native"
RUN_TIMEOUT_S = 120

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
    body = "\n".join(do_lines) + "\n"
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


# ───────────────────────── jget-style path resolver ─────────────────────────
_TOK = re.compile(r"[^.\[\]]+|\[[^\]]*\]")


def jget(obj, q: str):
    """Resolve 'classic.F' / 'firststage[beertax].F' / 'coefs[0].diff'. Raises KeyError."""
    cur = obj
    for tok in _TOK.findall(q):
        if tok.startswith("["):
            key = tok[1:-1]
            if isinstance(cur, list):
                if re.fullmatch(r"-?\d+", key):
                    cur = cur[int(key)]
                else:
                    hit = next((e for e in cur if isinstance(e, dict) and e.get("name") == key), None)
                    if hit is None:
                        raise KeyError(q)
                    cur = hit
            elif isinstance(cur, dict):
                cur = cur[key]
            else:
                raise KeyError(q)
        else:
            if not isinstance(cur, dict) or tok not in cur:
                raise KeyError(q)
            cur = cur[tok]
    return cur


def resolve_scalar(block: dict, path: str, want):
    """jget + single-element-list unwrap (skipped when `want` is itself a list)."""
    got = jget(block, path)
    if isinstance(got, list) and not isinstance(want, list) and len(got) == 1:
        got = got[0]
    return got


# ───────────────────────── block lookup ─────────────────────────
def last_estimation_block(blocks: list) -> "dict | None":
    for b in reversed(blocks or []):
        if isinstance(b, dict) and b.get("type") == "estimation":
            return b
    return None


def coeftable_lookup(coeftable, name: str) -> "dict | None":
    if not isinstance(coeftable, list):
        return None
    for alias in _aliases_for(name):
        for row in coeftable:
            if isinstance(row, dict) and row.get("name") == alias:
                return row
    return None


def _completion_gate(run: dict) -> "list | None":
    if run["stdout_json"] is None:
        return [{"q": "json_parse", "ok": False,
                 "note": f"stdout did not parse as JSON ({(run.get('stderr') or '')[:150]})"}]
    j = run["stdout_json"]
    if j.get("stopped_at_reason"):
        return [{"q": "ran_to_completion", "ok": False,
                 "note": f"stopped_at_line={j.get('stopped_at_line')} reason={j.get('stopped_at_reason')!r}"}]
    return None


# ───────────────────────── golden grader ─────────────────────────
def grade_golden(case: dict, run: dict, tol: dict) -> tuple:
    fail = _completion_gate(run)
    if fail:
        return False, fail
    block = last_estimation_block(run["stdout_json"].get("blocks", []))
    if block is None:
        return False, [{"q": "estimation_block_present", "ok": False,
                        "note": "no block with type=='estimation' in output"}]
    ok_all = True
    checks = []

    if "n_obs" in case:
        n_obs = block.get("n_obs")
        ok = (n_obs == case["n_obs"])
        ok_all &= ok
        checks.append({"q": "n_obs", "ok": ok, "got": n_obs, "want": case["n_obs"]})

    # BUG-399 (2026-09-02): a `margins` block carries its rows in `margins[]`, keyed by `name`
    # (dydx) or `at` (predictive margins), with the estimate in `margin`. Present them to the
    # same lookup as coeftable rows so an at() case can be pinned like any other golden.
    rows = block.get("coeftable")
    if not isinstance(rows, list) and isinstance(block.get("margins"), list):
        rows = [dict(m, name=m.get("name") or m.get("at"), b=m.get("b", m.get("margin")))
                for m in block["margins"] if isinstance(m, dict)]
    for name, exp in case.get("coefs", {}).items():
        row = coeftable_lookup(rows, name)
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

    for path, want in case.get("required_scalars", {}).items():
        try:
            got = resolve_scalar(block, path, want)
        except (KeyError, IndexError):
            ok_all = False
            checks.append({"q": f"required_scalar.{path}", "ok": False,
                           "note": "path absent (FATAL — live-verified schema field)"})
            continue
        ok = close(got, want, tol["scalar_rtol"], tol["scalar_atol"])
        ok_all &= ok
        checks.append({"q": f"required_scalar.{path}", "ok": ok, "got": got, "want": want})

    for path, want in case.get("exact_scalars", {}).items():
        try:
            got = resolve_scalar(block, path, want)
        except (KeyError, IndexError):
            ok_all = False
            checks.append({"q": f"exact_scalar.{path}", "ok": False, "note": "path absent (FATAL)"})
            continue
        ok = (got == want)
        ok_all &= ok
        checks.append({"q": f"exact_scalar.{path}", "ok": ok, "got": got, "want": want})

    for path, want in case.get("scalars", {}).items():
        try:
            got = resolve_scalar(block, path, want)
        except (KeyError, IndexError):
            checks.append({"q": f"scalar.{path}", "ok": False,
                           "note": "path absent (secondary — schema note, non-fatal)"})
            continue
        ok = close(got, want, tol["scalar_rtol"], tol["scalar_atol"])
        ok_all &= ok
        checks.append({"q": f"scalar.{path}", "ok": ok, "got": got, "want": want})

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


# ───────────────────────── mechanical grader ─────────────────────────
def grade_mechanical(case: dict, run: dict) -> tuple:
    fail = _completion_gate(run)
    if fail:
        return False, fail
    blocks = run["stdout_json"].get("blocks", [])
    ok_all = True
    checks = []

    seq = case.get("display_sequence")
    if seq:
        texts = [b.get("text", "") for b in blocks
                 if isinstance(b, dict) and b.get("type") == "display"]
        # ordered subsequence match
        it = iter(texts)
        matched = all(any(want == t for t in it) for want in seq)
        ok_all &= matched
        checks.append({"q": f"display_sequence{seq}", "ok": matched,
                       "note": "" if matched else f"display texts seen: {texts}"})

    sc = case.get("summarize_check")
    if sc:
        summ = None
        for b in reversed(blocks):
            if isinstance(b, dict) and b.get("type") == "summarize":
                summ = b
                break
        if summ is None:
            ok_all = False
            checks.append({"q": "summarize_block_present", "ok": False, "note": "no summarize block"})
        else:
            stats = summ.get("stats", {})
            for var in sc["vars"]:
                st = stats.get(var)
                ok = isinstance(st, dict) and st.get("N") == sc["n"]
                ok_all &= ok
                checks.append({"q": f"summarize[{var}].N", "ok": ok,
                               "got": (st or {}).get("N") if isinstance(st, dict) else None,
                               "want": sc["n"]})
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
    print(f"[d45] {engine_arm.describe_arm(_cobra)}")
    engine_arm.pin_settings(os.environ, tempfile.mkdtemp(prefix="piet_d45_settings_"))  # BUG-388: never the user's live settings.json

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
        run = run_do(piet_bin, case["do_body"])
        if kind == "golden":
            ok, checks = grade_golden(case, run, tol)
        elif kind == "reject":
            ok, checks = grade_reject(case, run)
        elif kind == "mechanical":
            ok, checks = grade_mechanical(case, run)
        else:
            ok, checks = False, [{"q": "unknown_oracle_kind", "ok": False, "note": kind}]
        results.append({"id": cid, "oracle_kind": kind, "passed": ok, "checks": checks})

    width = max(len(r["id"]) for r in results)
    print(f"\n{'CASE':{width}}  KIND        RESULT")
    print("-" * (width + 26))
    for r in results:
        mark = "GREEN" if r["passed"] else "RED  "
        print(f"{r['id']:{width}}  {r['oracle_kind']:10s}  {mark}")
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
    print(f"\n{n_pass}/{len(results)} D4+D5 cases GREEN\n")

    if args.json:
        Path(args.json).write_text(json.dumps(results, indent=2, default=str))

    sys.exit(0 if n_pass == len(results) else 1)


if __name__ == "__main__":
    main()
