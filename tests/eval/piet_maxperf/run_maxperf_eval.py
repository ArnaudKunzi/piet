#!/usr/bin/env python3
"""MAXPERF golden set driver — Eval Harness Gen, MSG-MI-237 items 1-3.

Gates 3 gate-① silent-wrong-answer / control-flow bugs found by the
2026-07-16/17 MAXPERF audit (internal note GATE23-analysis-do-audit-20260716.md
findings F1/F2/F3), ahead of the piet B.2.2 fix wave (MI-236):

  mp1_merge_highprecision_key    merge m:1 false "not unique" on f64 keys
                                  that agree only up to 6 significant digits
                                  (findDupKey, handlers_data_verbs.cpp:2010)
  mp2_collapse_factorby_precision  collapse (mean) ..., by(FACTOR) silently
                                  truncates the computed statistic to 6 sig
                                  digits via a CSV round-trip
                                  (handlers_data_verbs.cpp:771/:1001)
  mp3_foreach_continue_capture   foreach silently stops after iteration 1's
                                  CAPTURED error instead of continuing
                                  (loops.cpp runBody(), missing the same
                                  captured-error exemption doexec.cpp's own
                                  top-level halt-check already has)

AUTHORED GOLDEN-FIRST: every case is verified RED against the pre-B.2.2
baseline (see RED-baseline.txt) and must turn GREEN once the corresponding
B.2.2 fix lands. All 3 fixtures are fully self-contained synthetic data
(inline `set obs`/`gen`/`replace`) — no dependency on the soccer corpus or
any external file. Standalone leg — NOT wired into run_eval.py's default
gating set (the maintainer's call at merge time, same recipe as the piet_d1/d2d3/d45
siblings — see this dir's README.md).

Usage:
  python3 run_maxperf_eval.py [--binary PATH] [--only case_id,case_id] [--json OUT]
  PIET_BIN=/path/to/piet_native python3 run_maxperf_eval.py

`--binary` is the primary flag (MSG-MI-237's own wording, so the maintainer's gating
invocation matches this driver exactly); `--piet-bin` is accepted as an
alias for consistency with the sibling piet_d1/d2d3/d45 drivers.

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

HERE = Path(__file__).resolve().parent                  # tests/eval/piet_maxperf
ROOT = Path(__file__).resolve().parents[3]               # repo root
EXPECTED_PATH = HERE / "maxperf_expected.json"

DEFAULT_PIET_BIN = ROOT / "piet" / "native" / "build" / "piet_native"
RUN_TIMEOUT_S = 60


# ───────────────────────── piet_native invocation ─────────────────────────
def resolve_piet_bin(cli_arg: "str | None") -> Path:
    cand = cli_arg or os.environ.get("PIET_BIN") or str(DEFAULT_PIET_BIN)
    p = Path(cand)
    if not p.exists():
        print(f"ERROR: piet_native binary not found at {p}", file=sys.stderr)
        sys.exit(2)
    return p


def run_do(piet_bin: Path, do_lines: list, workdir: Path) -> dict:
    """Run do_lines as one --do invocation. Returns
    {"rc","stdout_json","stderr","raw_stdout","do_path","do_body"}."""
    body = "\n".join(do_lines) + "\n"
    do_path = workdir / "case.do"
    do_path.write_text(body)
    try:
        p = subprocess.run([str(piet_bin), "--do", str(do_path)], capture_output=True,
                            text=True, timeout=RUN_TIMEOUT_S, stdin=subprocess.DEVNULL)
    except subprocess.TimeoutExpired:
        return {"rc": None, "stdout_json": None, "stderr": "TIMEOUT",
                "do_path": str(do_path), "do_body": body}
    parsed = None
    if p.stdout.strip().startswith("{"):
        try:
            parsed = json.loads(p.stdout)
        except json.JSONDecodeError:
            parsed = None
    return {"rc": p.returncode, "stdout_json": parsed, "stderr": p.stderr,
            "raw_stdout": p.stdout, "do_path": str(do_path), "do_body": body}


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


# ───────────────────────── block helpers ─────────────────────────
def last_block_of_type(blocks: list, btype: str) -> "dict | None":
    for b in reversed(blocks or []):
        if isinstance(b, dict) and b.get("type") == btype:
            return b
    return None


def blocks_of_type(blocks: list, btype: str) -> list:
    return [b for b in (blocks or []) if isinstance(b, dict) and b.get("type") == btype]


def find_mutation(blocks: list, action: str) -> "dict | None":
    for b in reversed(blocks or []):
        if isinstance(b, dict) and b.get("type") == "data_mutation" and b.get("action") == action:
            return b
    return None


# ───────────────────────── per-case graders ─────────────────────────
def grade_merge_success(case: dict, run: dict) -> tuple:
    checks = []
    if run["stdout_json"] is None:
        return False, [{"q": "json_parse", "ok": False, "note": "stdout did not parse as JSON",
                        "raw_stdout": (run.get("raw_stdout") or "")[:500]}]
    j = run["stdout_json"]
    stopped = j.get("stopped_at_reason")
    ok_ran = stopped is None
    checks.append({"q": "ran_to_completion", "ok": ok_ran,
                   "got": stopped, "want": None,
                   "note": "" if ok_ran else "merge halted the do-file — this IS the bug (false 'not unique' rejects a genuinely-unique key)"})
    if not ok_ran:
        return False, checks

    ok_all = True

    mut = find_mutation(j.get("blocks", []), "merge")
    if mut is None:
        ok_all = False
        checks.append({"q": "merge_mutation_block_present", "ok": False,
                       "note": "no data_mutation block with action=='merge' found"})
    else:
        counts = mut.get("merge_counts") or {}
        for field, want in case["expected_merge_counts"].items():
            got = counts.get(field)
            ok = (got == want)
            ok_all &= ok
            checks.append({"q": f"merge_counts.{field}", "ok": ok, "got": got, "want": want})

    browse = last_block_of_type(j.get("blocks", []), "browse")
    if browse is None:
        ok_all = False
        checks.append({"q": "list_block_present", "ok": False,
                       "note": "no 'browse' block found (the trailing `list id payout` never ran)"})
        return ok_all, checks

    rows = browse.get("rows") or []
    ok = (len(rows) == case["expected_total_rows"])
    ok_all &= ok
    checks.append({"q": "row_count", "ok": ok, "got": len(rows), "want": case["expected_total_rows"]})

    atol = case["match_atol"]
    seen_counts = {}
    for idx, row in enumerate(rows):
        if not isinstance(row, list) or len(row) < 2:
            ok_all = False
            checks.append({"q": f"row[{idx}].shape", "ok": False, "note": f"unexpected row shape: {row!r}"})
            continue
        got_id, got_payout = row[0], row[1]
        match = None
        for exp_id, exp_payout in case["expected_pairs"]:
            if close(got_id, exp_id, 0.0, atol):
                match = (exp_id, exp_payout)
                break
        if match is None:
            ok_all = False
            checks.append({"q": f"row[{idx}].id_recognized", "ok": False,
                           "note": f"id={got_id!r} does not match any expected key"})
            continue
        exp_id, exp_payout = match
        ok = close(got_payout, exp_payout, 0.0, atol)
        ok_all &= ok
        checks.append({"q": f"row(id={exp_id}).payout", "ok": ok, "got": got_payout, "want": exp_payout})
        seen_counts[exp_id] = seen_counts.get(exp_id, 0) + 1

    for exp_id, _ in case["expected_pairs"]:
        got_n = seen_counts.get(exp_id, 0)
        ok = (got_n == case["expected_count_per_id"])
        ok_all &= ok
        checks.append({"q": f"count(id={exp_id})", "ok": ok, "got": got_n, "want": case["expected_count_per_id"]})

    return ok_all, checks


def grade_collapse_precision(case: dict, run: dict) -> tuple:
    checks = []
    if run["stdout_json"] is None:
        return False, [{"q": "json_parse", "ok": False, "note": "stdout did not parse as JSON",
                        "raw_stdout": (run.get("raw_stdout") or "")[:500]}]
    j = run["stdout_json"]
    stopped = j.get("stopped_at_reason")
    ok_ran = stopped is None
    checks.append({"q": "ran_to_completion", "ok": ok_ran, "got": stopped, "want": None})
    if not ok_ran:
        return False, checks

    browse = last_block_of_type(j.get("blocks", []), "browse")
    if browse is None:
        return False, [{"q": "list_block_present", "ok": False,
                        "note": "no 'browse' block found (the trailing `list grp val` never ran)"}]

    rows = browse.get("rows") or []
    tol = case["tolerance_policy"]
    ok_all = True
    want_groups = dict(case["expected_group_means"])
    seen = set()
    for idx, row in enumerate(rows):
        if not isinstance(row, list) or len(row) < 2:
            ok_all = False
            checks.append({"q": f"row[{idx}].shape", "ok": False, "note": f"unexpected row shape: {row!r}"})
            continue
        grp, val = row[0], row[1]
        if grp not in want_groups:
            ok_all = False
            checks.append({"q": f"row[{idx}].grp_recognized", "ok": False, "note": f"unexpected group {grp!r}"})
            continue
        want = want_groups[grp]
        ok = close(val, want, tol["rtol"], tol["atol"])
        ok_all &= ok
        checks.append({"q": f"mean[{grp}]", "ok": ok, "got": val, "want": want,
                       "rel_err": (abs(val - want) / abs(want)) if (isinstance(val, (int, float)) and want) else None})
        seen.add(grp)

    missing = set(want_groups) - seen
    if missing:
        ok_all = False
        checks.append({"q": "all_groups_present", "ok": False, "note": f"missing groups: {sorted(missing)}"})

    return ok_all, checks


def grade_display_sequence(case: dict, run: dict) -> tuple:
    checks = []
    if run["stdout_json"] is None:
        return False, [{"q": "json_parse", "ok": False, "note": "stdout did not parse as JSON",
                        "raw_stdout": (run.get("raw_stdout") or "")[:500]}]
    j = run["stdout_json"]
    blocks = j.get("blocks", [])

    got_seq = [b.get("text") for b in blocks_of_type(blocks, "display")]
    want_seq = case["expected_display_sequence"]
    ok_seq = (got_seq == want_seq)
    checks.append({"q": "display_sequence", "ok": ok_seq, "got": got_seq, "want": want_seq})

    n_captured = sum(1 for b in blocks if isinstance(b, dict) and b.get("type") == "error" and b.get("captured") is True)
    ok_n = (n_captured == case["expected_captured_error_count"])
    checks.append({"q": "captured_error_count", "ok": ok_n, "got": n_captured,
                   "want": case["expected_captured_error_count"]})

    stopped = j.get("stopped_at_reason")
    ok_ran = stopped is None
    checks.append({"q": "do_file_did_not_halt", "ok": ok_ran, "got": stopped, "want": None,
                   "note": "the do-file itself must not halt (doexec.cpp's top-level halt-check is already captured-aware) — only the LOOP's internal per-iteration halt is the bug under test"})

    return (ok_seq and ok_n and ok_ran), checks


GRADERS = {
    "merge_success": grade_merge_success,
    "collapse_precision": grade_collapse_precision,
    "display_sequence": grade_display_sequence,
}


# ───────────────────────── main ─────────────────────────
def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--binary", default=None, help="path to piet_native (primary flag, MSG-MI-237 wording)")
    ap.add_argument("--piet-bin", default=None, help="alias for --binary (piet_d1/d2d3/d45 sibling convention)")
    ap.add_argument("--only", default="", help="comma list of case ids")
    ap.add_argument("--json", default="", help="optional path to dump machine-readable results")
    args = ap.parse_args()

    # Declare the engine arm instead of letting the spawned piet_native infer
    # one; an undeclared run silently measures the pinned ARCHIVE (last
    # release) rather than the engine main just built. See tools/engine_arm.py.
    sys.path.insert(0, str(ROOT / "tools"))
    import engine_arm
    _cobra = engine_arm.declare(engine_arm.resolve_cobra())
    print(f"[maxperf] {engine_arm.describe_arm(_cobra)}")
    engine_arm.pin_settings(os.environ, tempfile.mkdtemp(prefix="piet_maxperf_settings_"))  # BUG-388: never the user's live settings.json

    piet_bin = resolve_piet_bin(args.binary or args.piet_bin)
    expected = json.loads(EXPECTED_PATH.read_text())
    cases = expected["_meta"]["cases"]

    only = set(args.only.split(",")) if args.only else None
    results = []
    with tempfile.TemporaryDirectory(prefix="piet_maxperf_") as tmpdir_str:
        tmpdir = Path(tmpdir_str)
        for cid, case in cases.items():
            if only and cid not in only:
                continue
            kind = case["kind"]
            grader = GRADERS.get(kind)
            if grader is None:
                results.append({"id": cid, "kind": kind, "passed": False,
                                "checks": [{"q": "unknown_kind", "ok": False, "note": kind}]})
                continue

            do_lines = case["do_body"]
            if "using_placeholder" in case:
                using_path = tmpdir / f"{cid}_using.hbin"
                token = case["using_placeholder"]
                do_lines = [line.replace(token, str(using_path)) for line in do_lines]

            case_workdir = tmpdir / cid
            case_workdir.mkdir(parents=True, exist_ok=True)
            run = run_do(piet_bin, do_lines, case_workdir)
            ok, checks = grader(case, run)
            results.append({"id": cid, "kind": kind, "passed": ok, "checks": checks})

    # ---- print PASS/FAIL table ----
    width = max((len(r["id"]) for r in results), default=10)
    print(f"\n{'CASE':{width}}  KIND                  RESULT")
    print("-" * (width + 34))
    for r in results:
        mark = "GREEN" if r["passed"] else "RED  "
        print(f"{r['id']:{width}}  {r['kind']:20s}  {mark}")
        if not r["passed"]:
            for c in r["checks"]:
                if not c["ok"]:
                    extra = ""
                    if "got" in c and "want" in c:
                        extra = f" got={c['got']!r} want={c['want']!r}"
                    note = c.get("note", "")
                    print(f"      - {c['q']}{extra}  {note}")
    n_pass = sum(r["passed"] for r in results)
    print(f"\n{n_pass}/{len(results)} MAXPERF (MI-237) cases GREEN\n")

    if args.json:
        Path(args.json).write_text(json.dumps(results, indent=2, default=str))

    sys.exit(0 if (results and n_pass == len(results)) else 1)


if __name__ == "__main__":
    main()
