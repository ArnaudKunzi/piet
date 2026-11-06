#!/usr/bin/env python3
"""G12 exotica leg — factor-variable coefficient rows under value labels.

Gates piet_native's coeftable row naming when a numeric factor variable
carries a value label (BUG-102). Standalone script — NOT wired into
run_eval.py's default gating set (the cobra-facing manifest cannot reach this
dimension at all: value labels do not survive into hbin, so a cobra cell would
compare two identical label-free runs and read GREEN vacuously).

Oracle = real StataBE 19.5; source + full log committed at
tests/eval/golden/g12_factor_valuelabel.{do,log}.

  python3 run_g12_eval.py [--piet-bin PATH] [--only ID,ID] [--json OUT]
  PIET_BIN=/path/to/piet_native python3 run_g12_eval.py

Resolution order for the binary: --piet-bin > $PIET_BIN >
<repo_root>/piet/native/build/piet_native.

Exit 0 iff every ASSERTED check passes. The label-carriage observation (where
the display stub text lives in the row object) is REPORTED, never asserted —
see _meta.what_is_REPORTED_ONLY in g12_expected.json.
"""
import argparse
import json
import math
import os
import subprocess
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent                  # tests/eval/piet_g12
ROOT = Path(__file__).resolve().parents[3]              # repo root
DEFAULT_PIET_BIN = ROOT / "piet" / "native" / "build" / "piet_native"
EXPECTED = HERE / "g12_expected.json"
RUN_TIMEOUT_S = 120

# piet_native emits `(intercept)` where Stata's e(b) says `_cons`. Pre-existing
# alias, unrelated to BUG-102.
ALIASES = {"_cons": ["_cons", "(intercept)"]}


def aliases_for(name):
    return ALIASES.get(name, [name])


def resolve_piet_bin(cli_arg):
    p = Path(cli_arg) if cli_arg else Path(os.environ.get("PIET_BIN", DEFAULT_PIET_BIN))
    if not p.exists():
        print(f"ERROR: piet_native binary not found at {p}", file=sys.stderr)
        sys.exit(2)
    return p


def run_do(piet_bin, do_lines):
    """One isolated `piet_native --do` invocation per case.

    Isolated on purpose: piet_native halts the whole file at the first block
    whose type is "error" (doexec.cpp per-line loop), so batching cases would
    let one failure mask every case after it.
    """
    body = "\n".join(do_lines) + "\n"
    with tempfile.NamedTemporaryFile("w", suffix=".do", delete=False) as f:
        f.write(body)
        do_path = f.name
    try:
        p = subprocess.run([str(piet_bin), "--do", do_path], capture_output=True,
                           text=True, timeout=RUN_TIMEOUT_S, stdin=subprocess.DEVNULL)
    except subprocess.TimeoutExpired:
        return {"blocks": None, "stderr": "TIMEOUT", "do_body": body}
    parsed = None
    out = p.stdout.strip()
    # the binary prints a scratch-workdir banner before the JSON
    brace = out.find("{")
    if brace >= 0:
        try:
            parsed = json.loads(out[brace:])
        except json.JSONDecodeError:
            parsed = None
    return {"blocks": (parsed or {}).get("blocks"), "raw": out,
            "stderr": p.stderr, "do_body": body}


def last_estimation_block(blocks):
    for b in reversed(blocks or []):
        if isinstance(b, dict) and b.get("type") == "estimation":
            return b
    return None


def close(got, want, rtol, atol):
    if got is None or want is None:
        return got == want
    try:
        got, want = float(got), float(want)
    except (TypeError, ValueError):
        return got == want
    if not (math.isfinite(got) and math.isfinite(want)):
        return got == want
    return abs(got - want) <= atol or abs(got - want) <= rtol * abs(want)


def lookup(coeftable, name):
    for alias in aliases_for(name):
        for row in coeftable:
            if isinstance(row, dict) and row.get("name") == alias:
                return row
    return None


def check_case(case, blocks, tol):
    """Returns (failures, notes). failures are ASSERTED; notes are REPORTED."""
    failures, notes = [], []
    block = last_estimation_block(blocks)
    if block is None:
        return ["no estimation block in output"], notes
    coeftable = block.get("coeftable")
    if not isinstance(coeftable, list):
        return ["estimation block has no coeftable array"], notes

    got_names = [r.get("name") for r in coeftable if isinstance(r, dict)]
    expected = case["expected_rows"]

    # ---- ASSERTED A: row-name layer == Stata e(b) colnames, label-invariant
    for name in expected:
        if lookup(coeftable, name) is None:
            failures.append(f"row name {name!r} absent (got {got_names!r})")

    # ---- ASSERTED B: numeric layer
    for name, want in expected.items():
        row = lookup(coeftable, name)
        if row is None:
            continue                      # already reported by the name check
        for field in ("b", "se"):
            if not close(row.get(field), want[field], tol["rtol"], tol["atol"]):
                failures.append(
                    f"{name}.{field}: got {row.get(field)!r} want {want[field]!r}")

    # ---- REPORTED C: where does the display stub text live?
    for name, want in expected.items():
        stub = want["stub"]
        if stub == "_cons":
            continue
        row = lookup(coeftable, name)
        if row is None:
            continue
        carriers = [k for k, v in row.items() if isinstance(v, str) and v == stub]
        if carriers:
            notes.append(f"{name}: stub {stub!r} carried in field(s) {carriers}")
        else:
            notes.append(f"{name}: stub {stub!r} NOT present in any row field "
                         f"(fields: {sorted(row.keys())})")
    return failures, notes


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--piet-bin")
    ap.add_argument("--only", help="comma-separated case ids")
    ap.add_argument("--json", help="write a machine-readable result file")
    args = ap.parse_args()

    # Declare the engine arm rather than letting the spawned piet_native infer
    # one — without this the run silently grades against the pinned ARCHIVE
    # (last release) instead of the engine main just built, and says nothing
    # about it. See tools/engine_arm.py.
    sys.path.insert(0, str(ROOT / "tools"))
    import engine_arm
    _cobra = engine_arm.declare(engine_arm.resolve_cobra())
    print(f"[g12] {engine_arm.describe_arm(_cobra)}")
    engine_arm.pin_settings(os.environ, tempfile.mkdtemp(prefix="piet_g12_settings_"))  # BUG-388: never the user's live settings.json

    piet_bin = resolve_piet_bin(args.piet_bin)
    spec = json.loads(EXPECTED.read_text())
    tol = spec["_meta"]["tolerance"]
    cases = spec["cases"]
    if args.only:
        want = {s.strip() for s in args.only.split(",")}
        cases = [c for c in cases if c["id"] in want]

    print(f"piet_g12 — binary: {piet_bin}")
    print(f"oracle:  tests/eval/golden/g12_factor_valuelabel.{{do,log}} (StataBE 19.5)\n")

    results, n_pass = [], 0
    for case in cases:
        run = run_do(piet_bin, case["do_body"])
        failures, notes = check_case(case, run["blocks"], tol)
        ok = not failures
        n_pass += ok
        print(f"[{'PASS' if ok else 'FAIL'}] {case['id']}")
        for f in failures:
            print(f"         ASSERT  {f}")
        for n in notes:
            print(f"         report  {n}")
        results.append({"id": case["id"], "pass": ok,
                        "failures": failures, "notes": notes})

    total = len(cases)
    print(f"\npiet_g12: {n_pass}/{total} asserted-GREEN")
    if args.json:
        Path(args.json).write_text(json.dumps(
            {"binary": str(piet_bin), "pass": n_pass, "total": total,
             "results": results}, indent=1))
    return 0 if n_pass == total else 1


if __name__ == "__main__":
    sys.exit(main())
