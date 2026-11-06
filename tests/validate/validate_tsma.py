#!/usr/bin/env python3
"""Differential test: cecon tssmooth_ma vs Stata's exact pm10_ma15 (golden file
tsma_golden.csv produced by tsma_golden.do). Element-wise over the full panel.

Steps:
  1. read tsma_golden.csv (geo_id, date, PM10_mean, stata pm10_ma15)
  2. build a raw .hbin (geo_id, date, PM10_mean) and run:
       cecon eval --xtset geo_id,date --tsma "pm10_ma15=PM10_mean,7,1,7" --out cecon_ma.hbin
  3. read cecon_ma.hbin, align by (geo_id,date), diff pm10_ma15 element-wise
"""
import argparse
import subprocess
import sys
import os

import numpy as np
import pandas as pd

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "..", "tools"))
from hbin_read import read_hbin  # noqa: E402


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--golden", default="tsma_golden.dta")
    ap.add_argument("--cecon", required=True)
    ap.add_argument("--tohbin", required=True)  # path to to_hbin.py
    ap.add_argument("--py", required=True)       # venv python for to_hbin
    args = ap.parse_args()

    g = pd.read_stata(args.golden)  # full double precision, no CSV rounding
    # date is a Stata %td (datetime64 after read_stata); convert to epoch days (f64)
    if np.issubdtype(g["date"].dtype, np.datetime64):
        g["date"] = (g["date"] - pd.Timestamp("1960-01-01")).dt.days.astype("float64")
    else:
        g["date"] = g["date"].astype("float64")
    g["geo_id"] = g["geo_id"].astype("float64")
    g = g.rename(columns={"pm10_ma15": "stata_ma"})
    n = len(g)
    print(f"golden rows={n}")

    raw = g[["geo_id", "date", "PM10_mean"]].copy()
    raw.to_parquet("tsma_raw.parquet")
    subprocess.run([args.py, args.tohbin, "tsma_raw.parquet", "tsma_raw.hbin"],
                   check=True, capture_output=True)

    subprocess.run([args.cecon, "eval", "--data", "tsma_raw.hbin",
                    "--xtset", "geo_id,date",
                    "--tsma", "pm10_ma15=PM10_mean,7,1,7",
                    "--out", "tsma_cecon.hbin"], check=True, capture_output=True)

    _, cols = read_hbin("tsma_cecon.hbin")
    m = g.reset_index(drop=True)
    m["cecon_ma"] = cols["pm10_ma15"]  # cecon preserves input row order

    # --- Deterministic double-precision oracle: time-aware ±[lags,leads] window
    #     MA over PM10_mean, computed per panel with searchsorted (matches cecon's
    #     algorithm exactly, in float64). This is the GATE. ---
    lags, leads = 7, 7
    oracle = np.full(len(m), np.nan)
    for _, idx in m.groupby("geo_id").groups.items():
        sub = m.loc[idx]
        order = np.argsort(sub["date"].to_numpy(), kind="stable")
        d = sub["date"].to_numpy()[order]
        x = sub["PM10_mean"].to_numpy(np.float64)[order]
        csum = np.concatenate([[0.0], np.cumsum(x)])
        lo = np.searchsorted(d, d - lags, side="left")
        hi = np.searchsorted(d, d + leads, side="right")
        ma = (csum[hi] - csum[lo]) / (hi - lo)
        oracle[np.asarray(idx)[order]] = ma

    gate = np.abs(m["cecon_ma"].to_numpy() - oracle)
    grel = gate / (np.abs(oracle) + 1e-30)
    print(f"GATE  cecon vs numpy double oracle: max abs={np.nanmax(gate):.3e} "
          f"rel={np.nanmax(grel):.3e}")

    # --- Loose cross-check vs Stata (its tssmooth ma stores single precision) ---
    both = m["stata_ma"].notna() & m["cecon_ma"].notna()
    nan_mismatch = int((m["stata_ma"].isna() != m["cecon_ma"].isna()).sum())
    sd = np.abs(m.loc[both, "cecon_ma"] - m.loc[both, "stata_ma"])
    print(f"x-chk cecon vs Stata (float32 tssmooth): max abs={sd.max():.3e} "
          f"(expected ~1e-5; Stata stores ma as float)  NaN-pattern mismatch={nan_mismatch}")

    ok = np.nanmax(grel) < 1e-12 and nan_mismatch == 0 and sd.max() < 1e-3
    print("RESULT:", "PASS" if ok else "FAIL")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
