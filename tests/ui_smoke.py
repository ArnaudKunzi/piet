#!/usr/bin/env python3
"""Run the app's built-in UI journey (--ui-smoke) and judge its verdict file.

The journey opens the real window and the real webview, loads the bundled sample,
runs a regression, drives the editor and exercises the file routes. It needs a
desktop session, so CI runs it on the hosted runners and a headless box cannot.

Usage: python3 tests/ui_smoke.py [path to piet_native or PIET.exe]
"""
import json
import os
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
EXE = ".exe" if os.name == "nt" else ""
TIMEOUT_SECONDS = 300


def main():
    app = Path(sys.argv[1]) if len(sys.argv) > 1 else ROOT / "piet" / "native" / "build" / f"piet_native{EXE}"
    if not app.is_file():
        print(f"ui smoke: FAIL: app not built: {app}", file=sys.stderr)
        return 1
    with tempfile.TemporaryDirectory(prefix="piet-ui-smoke-") as d:
        verdict_path = Path(d) / "verdict.json"
        env = dict(os.environ, PIET_UI_SMOKE_OUT=str(verdict_path))
        try:
            subprocess.run([str(app), "--ui-smoke"], env=env, timeout=TIMEOUT_SECONDS,
                           stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        except subprocess.TimeoutExpired:
            print(f"ui smoke: FAIL: no result after {TIMEOUT_SECONDS}s", file=sys.stderr)
            return 1
        if not verdict_path.is_file():
            print("ui smoke: FAIL: the app wrote no verdict", file=sys.stderr)
            return 1
        verdict = json.loads(verdict_path.read_text(encoding="utf-8"))
    steps = verdict.get("steps", [])
    failed = [s for s in steps if not s.get("pass")]
    for step in failed:
        print(f"  FAIL {step.get('step')}: {step.get('detail')}", file=sys.stderr)
    if verdict.get("verdict") != "GREEN" or failed:
        print(f"ui smoke: FAIL: verdict {verdict.get('verdict')}, {len(failed)} of {len(steps)} steps failed",
              file=sys.stderr)
        return 1
    print(f"ui smoke: OK ({len(steps)} steps, PIET {verdict.get('piet_version')})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
