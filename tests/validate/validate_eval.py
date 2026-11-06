#!/usr/bin/env python3
"""Differential test for cecon's data-layer expression evaluator (generate/keep/
summarize) against numpy on the real .do formulas (ln, Fourier harmonics, square,
mixed funcs) + a `keep if !missing(...)` filter.

Compares cecon's `summarize` stats (mean/sd/min/max) to numpy — no hbin reader
needed. Build the input hbin first from the full panel's raw columns:
  to_hbin.py <panel.parquet> genchk.hbin   # needs pop, doy, t2m_mean_C, PM10_mean
"""
import argparse
import json
import subprocess
import sys

import numpy as np
import pandas as pd


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--parquet", required=True)  # the same raw columns as the hbin
    ap.add_argument("--hbin", required=True)
    ap.add_argument("--cecon", required=True)
    args = ap.parse_args()

    gens = [
        ("lpop", "ln(pop)", lambda d: np.log(d["pop"])),
        ("sin1", "sin(2*_pi*1*doy/365.25)", lambda d: np.sin(2 * np.pi * d["doy"] / 365.25)),
        ("cos1", "cos(2*_pi*1*doy/365.25)", lambda d: np.cos(2 * np.pi * d["doy"] / 365.25)),
        ("t2m_sq", "t2m_mean_C^2", lambda d: d["t2m_mean_C"] ** 2),
        ("mix", "ln(pop)+exp(t2m_mean_C/100)-sqrt(abs(doy))",
         lambda d: np.log(d["pop"]) + np.exp(d["t2m_mean_C"] / 100) - np.sqrt(np.abs(d["doy"]))),
    ]
    keepif = "!missing(PM10_mean) & pop>0"

    cmd = [args.cecon, "eval", "--data", args.hbin]
    for nm, expr, _ in gens:
        cmd += ["--gen", f"{nm}={expr}"]
    cmd += ["--keepif", keepif, "--summarize", ",".join(nm for nm, _, _ in gens)]
    ce = json.loads(subprocess.run(cmd, capture_output=True, text=True, check=True).stdout)

    df = pd.read_parquet(args.parquet)
    m = df["PM10_mean"].notna() & (df["pop"] > 0)
    d = {c: df.loc[m, c].to_numpy(float) for c in ["pop", "doy", "t2m_mean_C"]}

    print(f"n_rows cecon={ce['n_rows']} numpy={int(m.sum())}  "
          f"({'match' if ce['n_rows'] == int(m.sum()) else 'MISMATCH'})")
    worst = 0.0
    for nm, _, fn in gens:
        v = fn(d)
        cs = ce["summarize"][nm]
        for stat, val in [("mean", np.nanmean(v)), ("sd", np.nanstd(v, ddof=1)),
                          ("min", np.nanmin(v)), ("max", np.nanmax(v))]:
            worst = max(worst, abs(cs[stat] - val) / (abs(val) + 1e-30))
    ok = worst < 1e-10 and ce["n_rows"] == int(m.sum())
    print(f"max rel err vs numpy = {worst:.2e}  -> {'PASS' if ok else 'FAIL'}")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
