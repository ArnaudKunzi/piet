#!/usr/bin/env python3
"""Self-contained public smoke test for a fresh clone.

Drives the built PIET binary through its block protocol with the bundled
sample (demos/fatalities/fatalities.csv): import, summarize, regress, save,
reload, regress again. The regression is checked against an independent
normal-equations solve written here, and the reloaded run must reproduce the
first one exactly. Nothing here needs Stata, private fixtures or a network.

Usage (from the repository root, after scripts/build-macos.sh built both
executables):  python3 tests/public_smoke.py
Environment: PIET_BIN / COBRA_BIN override the binary locations.
"""
import csv
import json
import os
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
ENGINE = Path(os.environ.get("COBRA_BIN", ROOT / "build" / "cobra"))
APP = Path(os.environ.get("PIET_BIN", ROOT / "piet" / "native" / "build" / "piet_native"))
SAMPLE = ROOT / "demos" / "fatalities" / "fatalities.csv"
DEPVAR, REGRESSORS = "fatal", ["beertax", "unemp"]


def run(cmds, folder):
    env = dict(os.environ, COBRA_BIN=str(ENGINE), PIET_SETTINGS_FILE=str(folder / "settings.json"))
    req = {"kind": "dispatch", "cmds": cmds, "workdir": str(folder), "cwd": str(folder)}
    p = subprocess.run([str(APP), "--run-block"], input=json.dumps(req), text=True,
                       capture_output=True, env=env, timeout=180)
    if p.returncode:
        raise AssertionError(f"piet_native exit {p.returncode}: {p.stderr[-2000:]} {p.stdout[-2000:]}")
    result = json.loads(p.stdout)
    assert result.get("ok"), result
    return result["block_lists"]


def errors(blocks):
    return [b for b in blocks if b.get("type") == "error"]


def estimation(blocks):
    est = [b for b in blocks if b.get("type") == "estimation"]
    assert len(est) == 1, f"expected one estimation block, got {len(est)}"
    return est[0]


def solve(a, b):
    """Gauss-Jordan with partial pivoting: an independent reference for OLS."""
    m = [list(row) + [v] for row, v in zip(a, b)]
    n = len(b)
    for k in range(n):
        j = max(range(k, n), key=lambda j: abs(m[j][k]))
        m[k], m[j] = m[j], m[k]
        pivot = m[k][k]
        assert abs(pivot) > 1e-12, "singular normal equations"
        m[k] = [v / pivot for v in m[k]]
        for j in range(n):
            if j != k:
                s = m[j][k]
                m[j] = [x - s * y for x, y in zip(m[j], m[k])]
    return [row[-1] for row in m]


def reference_ols():
    rows = list(csv.DictReader(SAMPLE.open()))
    xs = [[1.0] + [float(r[v]) for v in REGRESSORS] for r in rows]
    ys = [float(r[DEPVAR]) for r in rows]
    k = len(REGRESSORS) + 1
    xtx = [[sum(x[i] * x[j] for x in xs) for j in range(k)] for i in range(k)]
    xty = [sum(x[i] * y for x, y in zip(xs, ys)) for i in range(k)]
    return len(rows), solve(xtx, xty)


def main():
    assert ENGINE.is_file(), f"engine not built: {ENGINE}"
    assert APP.is_file(), f"app not built: {APP}"
    assert SAMPLE.is_file(), f"sample missing: {SAMPLE}"
    with tempfile.TemporaryDirectory(prefix="piet-public-smoke-") as d:
        folder = Path(d)
        (folder / "fatalities.csv").write_bytes(SAMPLE.read_bytes())
        reg = f"regress {DEPVAR} {' '.join(REGRESSORS)}"
        lists = run(["import delimited fatalities.csv, clear", "summarize", reg,
                     "save roundtrip.dta, replace", "clear", "use roundtrip.dta", reg], folder)
        assert len(lists) == 7, f"expected 7 block lists, got {len(lists)}"
        for i, blocks in enumerate(lists):
            assert not errors(blocks), f"command {i} produced an error: {errors(blocks)}"
        first, again = estimation(lists[2]), estimation(lists[6])
        assert first["coeftable"] == again["coeftable"], "reloaded .dta changed the fit"
        n, beta = reference_ols()
        assert first["n_obs"] == n, f"N {first['n_obs']} != {n}"
        got = {row["name"]: row["b"] for row in first["coeftable"]}
        names = ["(intercept)"] + REGRESSORS
        for name, ref in zip(names, beta):
            assert name in got, f"coefficient {name} missing from {sorted(got)}"
            tol = 1e-8 * max(1.0, abs(ref))
            assert abs(got[name] - ref) <= tol, f"{name}: {got[name]!r} vs reference {ref!r}"
    print(f"public smoke: OK (N={n}, {len(names)} coefficients match an independent OLS to 1e-8, "
          f".dta roundtrip reproduces the fit)")


if __name__ == "__main__":
    try:
        main()
    except AssertionError as e:
        print(f"public smoke: FAIL — {e}", file=sys.stderr)
        sys.exit(1)
