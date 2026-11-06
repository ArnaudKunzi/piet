#!/usr/bin/env python3
"""Differential test: reproduce the 건강심리 prep (egen/bysort/winsor/gen) through
`cecon eval` and diff against Stata's constructed variables in d2_golden.dta.

d2_golden.dta holds the inputs (h_pid wv la visits age h_g6) plus Stata's outputs
(c_*). We feed cecon the inputs, run the same ops in order, and compare c_* values
element-wise (row order is preserved by both).
"""
import argparse
import subprocess
import sys
import os

import numpy as np
import pandas as pd

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "..", "tools"))
from hbin_read import read_hbin  # noqa: E402

OPS = [
    "--xtset", "h_pid,wv",
    "--bygen", "c_first_in = wv[1], by(h_pid), sort(wv)",
    "--bygen", "c_lag_la = la[_n-1], by(h_pid), sort(wv)",
    "--bygen", "c_n = _n, by(h_pid), sort(wv)",
    "--bygen", "c_N = _N, by(h_pid), sort(wv)",
    "--egen", "c_base_la = max(cond(wv==c_first_in, la, .)), by(h_pid)",
    "--egen", "c_onset = min(cond(la==1, wv, .)), by(h_pid)",
    "--egen", "c_ever_la = max(la), by(h_pid)",
    "--egen", "c_sum_la = sum(la), by(h_pid)",
    "--egen", "c_cnt_v = count(visits), by(h_pid)",
    "--egen", "c_mean_v = mean(visits), by(h_pid)",
    "--egen", "c_sd_v = sd(visits), by(h_pid)",
    "--egen", "c_med_la = median(la), by(wv)",
    "--gen", "c_gvar = cond(c_ever_la==1 & c_base_la==0 & !missing(c_onset), c_onset, 0)",
    "--winsor", "c_visits_w = visits,1,99",
    "--gen", "c_age_z_tmp = age",  # placeholder; age_z needs summarize scalars -> see below
    "--gen", "c_ln_visits = ln(1+visits)",
    "--gen", "c_edu_hi = cond(missing(h_g6), ., h_g6>=4)",
    "--gen", "c_inrange_v = inrange(visits,1,10)",
    "--gen", "c_inlist_e = inlist(h_g6,4,5,6)",
]
CCOLS = ["c_first_in", "c_lag_la", "c_n", "c_N", "c_base_la", "c_onset", "c_ever_la",
         "c_sum_la", "c_cnt_v", "c_mean_v", "c_sd_v", "c_med_la", "c_gvar",
         "c_visits_w", "c_age_z", "c_ln_visits", "c_edu_hi", "c_inrange_v", "c_inlist_e"]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--golden", default="d2_golden.dta")
    ap.add_argument("--cecon", required=True)
    ap.add_argument("--tohbin", required=True)
    ap.add_argument("--py", required=True)
    args = ap.parse_args()

    g = pd.read_stata(args.golden)
    for c in ["h_pid", "wv", "la", "visits", "age", "h_g6"]:
        g[c] = g[c].astype("float64")
    n = len(g)
    print(f"golden rows={n}")

    inp = g[["h_pid", "wv", "la", "visits", "age", "h_g6"]].copy()
    inp.to_parquet("d2_inputs.parquet")
    subprocess.run([args.py, args.tohbin, "d2_inputs.parquet", "d2_inputs.hbin"],
                   check=True, capture_output=True)

    # age_z needs summarize mean/sd of age -> compute via cecon's own summarize, then gen.
    smj = subprocess.run([args.cecon, "eval", "--data", "d2_inputs.hbin",
                          "--summarize", "age", "--detail"], capture_output=True, text=True, check=True)
    import json
    sm = json.loads(smj.stdout)["summarize"]["age"]
    amean, asd = sm["mean"], sm["sd"]
    ops = list(OPS)
    # replace the placeholder with the real standardize gen
    idx = ops.index("c_age_z_tmp = age")
    ops[idx] = f"c_age_z = (age - {amean!r})/{asd!r}"

    cmd = [args.cecon, "eval", "--data", "d2_inputs.hbin"] + ops + ["--out", "d2_cecon.hbin"]
    subprocess.run(cmd, check=True, capture_output=True)

    _, cc = read_hbin("d2_cecon.hbin")

    print(f"{'column':14s} {'maxabsdiff':>12s} {'maxreldiff':>12s} {'nanmismatch':>11s}  status")
    worst = 0.0
    fail = False
    for c in CCOLS:
        st = g[c].to_numpy(np.float64)
        ce = cc[c].astype(np.float64)
        nan_mis = int((np.isnan(st) != np.isnan(ce)).sum())
        both = ~np.isnan(st) & ~np.isnan(ce)
        ad = np.abs(ce[both] - st[both])
        mad = float(ad.max()) if both.any() else 0.0
        mrd = float((ad / (np.abs(st[both]) + 1e-30)).max()) if both.any() else 0.0
        ok = mrd < 1e-9 and nan_mis == 0
        worst = max(worst, mrd)
        if not ok:
            fail = True
        print(f"{c:14s} {mad:12.3e} {mrd:12.3e} {nan_mis:11d}  {'ok' if ok else 'FAIL'}")
    print(f"\noverall max rel diff = {worst:.3e}  ->  {'FAIL' if fail else 'PASS'}")
    return 1 if fail else 0


if __name__ == "__main__":
    sys.exit(main())
