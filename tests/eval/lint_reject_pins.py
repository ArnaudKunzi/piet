#!/usr/bin/env python3
"""Reject-pin expiry lint (MI-228 #2, EVAL-BACKLOG-ADJUDICATE 2026-07-22).

Every `oracle_kind: "reject"` manifest case asserts the engine MUST refuse a
specific input (rc != 0, optionally a specific error code/message). A landed
fix can silently satisfy the input a case meant to reject -- the case then
starts passing (rc == 0) and reads as an ordinary green, with nothing left
asserting the rejection ever happened (precedent: d5c estat -- a reject pin
outlived the feature it guarded and stayed silently green for 19/21 legs
until MI-216 caught it; that class is silent-wrong, not merely uncovered).

This walks every reject case against the CURRENT binary (same engine wiring
as run_eval.py: manifest, data dir, prep materialization) and flags any that
no longer reject -- a stale pin, not a passing test. It also separately notes
cases that still reject but whose `expect_error` code/message no longer
matches (a softer drift signal -- still honestly erroring, just not the
error the manifest documents).

Usage: COBRA_BIN=<cobra> /opt/homebrew/bin/python3 tests/eval/lint_reject_pins.py
Exit: 0 if no reject case silently started passing; 1 if any stale pin found
(drift alone does not fail the gate).
"""
import sys

import run_eval as E


def main():
    cases = E.load_manifest(E.EVAL / "manifest.json")
    reject_cases = [c for c in cases if c.get("oracle_kind") == "reject"]
    stale, drifted = [], []
    for case in reject_cases:
        overrides = E.run_prep(case["prep"], E.DATA_DIR) if case.get("prep") else None
        run = E.run_cobra(case["command"], case.get("args", []), E.DATA_DIR, overrides=overrides)
        checks = E.reject_checks(case, run)
        rc_check = next(c for c in checks if c["q"] == "rc_nonzero")
        if not rc_check["ok"]:
            stale.append({"id": case["id"], "command": case["command"], "rc": run["rc"]})
            continue
        failed = [c for c in checks if not c["ok"]]
        if failed:
            drifted.append({"id": case["id"], "command": case["command"], "failed": failed})

    print(f"reject-pin lint: {len(reject_cases)} reject cases scanned")
    print(f"  stale (engine now ACCEPTS a pinned-reject input): {len(stale)}")
    for s in stale:
        print(f"    STALE {s['id']} ({s['command']}) rc={s['rc']} -- was pinned to reject, now succeeds")
    print(f"  drifted (still rejects, but expect_error no longer matches): {len(drifted)}")
    for d in drifted:
        for f in d["failed"]:
            print(f"    DRIFT {d['id']} ({d['command']}) {f['q']}: {f.get('note', '')}")
    if not stale and not drifted:
        print("  all reject pins fresh")
    sys.exit(1 if stale else 0)


if __name__ == "__main__":
    main()
