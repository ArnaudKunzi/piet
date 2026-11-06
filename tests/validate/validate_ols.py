#!/usr/bin/env python3
"""Validate cecon pooled OLS against numpy (exact oracle) and pyfixest (cross-check).

Usage:
  validate_ols.py --parquet PANEL.parquet --hbin PANEL.hbin \
      --y y_all --x pm25,tmaxb,tminb,precd,wsd --cecon ./build/cecon

numpy is the gating oracle (we control its formulas to match cecon exactly).
pyfixest is an independent cross-check; small 4th-digit SE differences across
dof conventions are expected and only warned about.
"""
import argparse
import json
import subprocess
import sys

import numpy as np
import pandas as pd


def numpy_ols(y, X):
    XtX = X.T @ X
    bread = np.linalg.inv(XtX)
    beta = bread @ (X.T @ y)
    e = y - X @ beta
    n, k = X.shape
    sigma2 = (e @ e) / (n - k)
    se_classic = np.sqrt(np.diag(sigma2 * bread))
    meat = (X * e[:, None]).T @ (X * e[:, None])
    V = (n / (n - k)) * bread @ meat @ bread
    se_hc1 = np.sqrt(np.diag(V))
    return beta, se_classic, se_hc1


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--parquet", required=True)
    ap.add_argument("--hbin", required=True)
    ap.add_argument("--y", required=True)
    ap.add_argument("--x", required=True)
    ap.add_argument("--cecon", required=True)
    ap.add_argument("--rtol-beta", type=float, default=1e-8)
    ap.add_argument("--rtol-se", type=float, default=1e-6)
    args = ap.parse_args()

    xs = args.x.split(",")
    cols = [args.y] + xs
    df = pd.read_parquet(args.parquet, columns=cols).dropna()
    y = df[args.y].to_numpy(dtype=float)
    Xc = np.column_stack([np.ones(len(df))] + [df[c].to_numpy(dtype=float) for c in xs])
    names = ["(intercept)"] + xs

    beta_np, sec_np, seh_np = numpy_ols(y, Xc)

    # --- cecon ---
    out = subprocess.run(
        [args.cecon, "ols", "--data", args.hbin, "--y", args.y, "--x", args.x],
        capture_output=True, text=True, check=True,
    )
    res = json.loads(out.stdout)

    # --- pyfixest cross-check (optional) ---
    py = None
    try:
        import pyfixest as pf
        fml = f"{args.y} ~ " + " + ".join(xs)
        m = pf.feols(fml, data=df, vcov="hetero")
        py = {"coef": m.coef().to_dict(), "se": m.se().to_dict()}
    except Exception as ex:  # noqa: BLE001
        print(f"(pyfixest cross-check skipped: {ex})")

    # --- compare ---
    print(f"n_obs: cecon={res['n_obs']} numpy={len(df)}")
    print(f"{'param':<14}{'cecon beta':>16}{'numpy beta':>16}{'cecon se_hc1':>16}"
          f"{'numpy se_hc1':>16}")
    max_rb = max_rs = 0.0
    fail = False
    for j, nm in enumerate(names):
        cb = res["coef"][nm]
        cs = res["se_hc1"][nm]
        rb = abs(cb - beta_np[j]) / (abs(beta_np[j]) + 1e-30)
        rsc = abs(res["se_classic"][nm] - sec_np[j]) / (abs(sec_np[j]) + 1e-30)
        rsh = abs(cs - seh_np[j]) / (abs(seh_np[j]) + 1e-30)
        max_rb = max(max_rb, rb)
        max_rs = max(max_rs, rsc, rsh)
        print(f"{nm:<14}{cb:>16.8g}{beta_np[j]:>16.8g}{cs:>16.8g}{seh_np[j]:>16.8g}")
        if rb > args.rtol_beta or rsc > args.rtol_se or rsh > args.rtol_se:
            fail = True
            print(f"    !! rel err beta={rb:.2e} se_classic={rsc:.2e} se_hc1={rsh:.2e}")

    print(f"\nmax rel err: beta={max_rb:.2e} (tol {args.rtol_beta:.0e}), "
          f"se={max_rs:.2e} (tol {args.rtol_se:.0e})")

    if py is not None:
        worst = 0.0
        for nm in names:
            key = "Intercept" if nm == "(intercept)" else nm
            if key in py["coef"]:
                worst = max(worst, abs(res["coef"][nm] - py["coef"][key]) /
                            (abs(py["coef"][key]) + 1e-30))
        print(f"pyfixest cross-check: max rel beta err = {worst:.2e} "
              f"({'ok' if worst < 1e-3 else 'WARN'})")

    print("RESULT:", "FAIL" if fail else "PASS")
    sys.exit(1 if fail else 0)


if __name__ == "__main__":
    main()
