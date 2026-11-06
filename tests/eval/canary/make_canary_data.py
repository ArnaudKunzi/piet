#!/usr/bin/env python3
"""Build the deterministic synthetic canaries for `random-check`.

The strongest anti-mock canary is **noise-free closed-form** data: y is an exact
linear combination of x with KNOWN integer coefficients, so OLS must recover those
coefficients to machine precision (SE ~ 1e-12 ≈ 0). No mock/fixture can produce the
exact answer for a freshly-built synthetic dataset, and no static fixture's
coefficient survives changing the build.

Reproducible by construction: the synthetic is rebuilt every run from a public
in-tree fixture (`auto.hbin`) via `cobra eval --gen`, so nothing binary is committed
(same policy as the eval `prep` mechanism). Deterministic — no RNG here.

Usage:
    python make_canary_data.py --cobra <bin> --data-dir <eval/data> --out-dir <cache>
    # or import: build_all(cobra_bin, data_dir, out_dir) -> {id: Path}
"""
import argparse
import os
import subprocess
from pathlib import Path

EVAL = Path(__file__).resolve().parent.parent          # tests/eval/
ROOT = Path(os.environ.get("COBRA_ROOT", EVAL.parent.parent))  # repo root (tests/eval -> tests -> root)


def _resolve_data_dir():
    """Canary source fixtures live in tests/validate (symlinked as tests/eval/data
    inside op-eval; empty on a bare main checkout). Honor COBRA_EVAL_DATA, then walk
    candidates; first with auto.hbin wins (PC-024). No hardcoded user paths."""
    cands = []
    env = os.environ.get("COBRA_EVAL_DATA")
    if env:
        cands.append(Path(env))
    cands += [EVAL / "data", EVAL.parent / "validate", ROOT / "tests" / "validate"]
    for c in cands:
        try:
            if (c / "auto.hbin").exists():
                return c
        except OSError:
            continue
    return EVAL / "data"


DEFAULT_DATA = _resolve_data_dir()
DEFAULT_COBRA = Path(os.environ.get("COBRA_BIN", ROOT / "build" / "cobra"))

# Each synthetic is (id, source fixture, gen exprs, the exact coefficients OLS must
# recover). Keep coefficients distinct integers so a column-swap is caught.
SYNTH = {
    "synth_linear": {
        "source": "auto",
        "gen": [
            "cf_x1=mpg",
            "cf_x2=weight/1000.0",
            "cf_y=10.0*mpg - 3.0*(weight/1000.0) + 7.0",
        ],
        # regress cf_y cf_x1 cf_x2  ->  exact recovery
        "expect": {"cf_x1": 10.0, "cf_x2": -3.0, "(intercept)": 7.0},
    },
}


def build_one(cobra_bin, data_dir, out_dir, name, spec):
    src = Path(data_dir) / (spec["source"] + ".hbin")
    if not src.exists():
        raise FileNotFoundError(f"canary source fixture missing: {src}")
    out = Path(out_dir) / (name + ".hbin")
    cmd = [str(cobra_bin), "eval", "--data", str(src)]
    for g in spec["gen"]:
        cmd += ["--gen", g]
    cmd += ["--out", str(out)]
    p = subprocess.run(cmd, capture_output=True, text=True, timeout=120)
    if p.returncode != 0:
        raise RuntimeError(f"build {name} failed: {p.stderr.strip()[:300]}")
    return out


def build_all(cobra_bin=DEFAULT_COBRA, data_dir=DEFAULT_DATA, out_dir=None):
    out_dir = Path(out_dir or "/tmp/cobra_canary")
    out_dir.mkdir(parents=True, exist_ok=True)
    return {name: build_one(cobra_bin, data_dir, out_dir, name, spec)
            for name, spec in SYNTH.items()}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--cobra", default=str(DEFAULT_COBRA))
    ap.add_argument("--data-dir", default=str(DEFAULT_DATA))
    ap.add_argument("--out-dir", default="/tmp/cobra_canary")
    a = ap.parse_args()
    built = build_all(Path(a.cobra), Path(a.data_dir), a.out_dir)
    for name, path in built.items():
        print(f"built {name} -> {path}")


if __name__ == "__main__":
    main()
