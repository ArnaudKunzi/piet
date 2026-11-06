#!/usr/bin/env python3
"""Validate cecon feols (HDFE + cluster/iid SE) on real data.

Gating oracle = a deterministic, high-precision numpy within-estimator. pyfixest
is a LOOSE cross-check only: its parallel demeaner (default fixef_tol=1e-8) is
non-deterministic across process launches and wobbles ~1e-6 in the SEs, so it is
unsuitable as a tight gate. cecon, by contrast, is bit-reproducible and matches
the numpy oracle to ~1e-13.

The SE gate uses cecon's reported small-sample params (cluster_dof_k / dof_resid)
to build the expected SE from the oracle's raw sandwich; the *convention* behind
those params is what the (loose) pyfixest cross-check confirms.

Example:
  validate_feols.py --parquet P.parquet --hbin P.hbin --cecon ./build/cecon \
      --y y_all --x pm25,tmaxb,tminb,precd,wsd --fe sigungu5,year --cluster sigungu5
"""
import argparse
import json
import subprocess
import sys

import numpy as np
import pandas as pd


def demean(v, fe_codes, tol=1e-13, maxit=2000):
    v = v.astype(float).copy()
    if not fe_codes:
        return v
    single = len(fe_codes) == 1
    scale = np.sqrt(np.mean(v * v)) or 1.0
    for _ in range(maxit):
        worst = 0.0
        for codes, K in fe_codes:
            means = np.bincount(codes, weights=v, minlength=K) / np.bincount(codes, minlength=K)
            v -= means[codes]
            worst = max(worst, np.abs(means).max() / scale)
        if single or worst < tol:
            break
    return v


def run_cecon(cecon, hbin, y, x, fe, cluster):
    cmd = [cecon, "feols", "--data", hbin, "--y", y, "--x", x]
    if fe:
        cmd += ["--fe", fe]
    cmd += (["--vcov", "cluster", "--cluster", cluster] if cluster else ["--vcov", "iid"])
    out = subprocess.run(cmd, capture_output=True, text=True, check=True)
    return json.loads(out.stdout)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--parquet", required=True)
    ap.add_argument("--hbin", required=True)
    ap.add_argument("--cecon", required=True)
    ap.add_argument("--y", required=True)
    ap.add_argument("--x", required=True)
    ap.add_argument("--fe", default="")
    ap.add_argument("--cluster", default="")
    ap.add_argument("--rtol-beta", type=float, default=1e-7)
    ap.add_argument("--rtol-se", type=float, default=1e-6)
    args = ap.parse_args()

    xs = args.x.split(",")
    fes = args.fe.split(",") if args.fe else []
    cols = [args.y] + xs + fes + ([args.cluster] if args.cluster else [])
    df = pd.read_parquet(args.parquet, columns=sorted(set(cols))).dropna()
    N = len(df)

    fe_codes = []
    for f in fes:
        c, _ = pd.factorize(df[f].values)
        fe_codes.append((c.astype(np.int64), int(c.max()) + 1))

    yd = demean(df[args.y].to_numpy(float), fe_codes)
    Xd = np.column_stack([demean(df[c].to_numpy(float), fe_codes) for c in xs])
    if not fes:  # pooled: add intercept
        Xd = np.column_stack([np.ones(N), Xd])
    bread = np.linalg.inv(Xd.T @ Xd)
    beta = bread @ (Xd.T @ yd)
    e = yd - Xd @ beta
    if not fes:
        beta = beta[1:]  # drop intercept for comparison to slope coefs

    res = run_cecon(args.cecon, args.hbin, args.y, args.x, args.fe, args.cluster)

    # Expected SE from the oracle's raw sandwich + cecon's reported dof params.
    if args.cluster:
        g, _ = pd.factorize(df[args.cluster].values)
        G = int(g.max()) + 1
        S = np.zeros((G, Xd.shape[1]))
        for j in range(Xd.shape[1]):
            S[:, j] = np.bincount(g, weights=Xd[:, j] * e, minlength=G)
        raw = bread @ (S.T @ S) @ bread
        k_adj = res["cluster_dof_k"]
        factor = (G / (G - 1)) * ((N - 1) / (N - k_adj))
        Vexp = factor * raw
    else:
        sigma2 = (e @ e) / res["dof_resid"]
        Vexp = sigma2 * bread
    se_exp = np.sqrt(np.diag(Vexp))
    if not fes:
        se_exp = se_exp[1:]

    label = (f"{args.y} ~ {'+'.join(xs)}"
             + (f" | {'+'.join(fes)}" if fes else "")
             + f"  [{'cluster='+args.cluster if args.cluster else 'iid'}]")
    print(f"\n=== {label} ===")
    print(f"  n_obs={res['n_obs']}  vcov={res['vcov_type']}  "
          f"cluster_dof_k={res.get('cluster_dof_k')}  nested={res.get('nested_fe')}")

    fail = False
    mrb = mrs = 0.0
    for j, nm in enumerate(xs):
        cb, cs = res["coef"][nm], res["se"][nm]
        rb = abs(cb - beta[j]) / (abs(beta[j]) + 1e-30)
        rs = abs(cs - se_exp[j]) / (abs(se_exp[j]) + 1e-30)
        mrb, mrs = max(mrb, rb), max(mrs, rs)
        flag = "" if (rb <= args.rtol_beta and rs <= args.rtol_se) else "  <<FAIL"
        if flag:
            fail = True
        print(f"  {nm:8s} beta {cb:14.8g} (oracle {beta[j]:14.8g}, r={rb:.1e})  "
              f"se {cs:12.7g} (oracle {se_exp[j]:12.7g}, r={rs:.1e}){flag}")
    print(f"  max rel err vs numpy oracle: beta={mrb:.2e} se={mrs:.2e}  -> "
          f"{'FAIL' if fail else 'PASS'}")

    # Loose, informational pyfixest cross-check (non-deterministic; do not gate).
    try:
        import pyfixest as pf
        fml = f"{args.y} ~ " + "+".join(xs) + (f" | {'+'.join(fes)}" if fes else "")
        vcov = {"CRV1": args.cluster} if args.cluster else "iid"
        m = pf.feols(fml, data=df, vcov=vcov)
        cse, pse = m.coef().to_dict(), m.se().to_dict()
        wb = max(abs(res["coef"][nm] - cse[nm]) / (abs(cse[nm]) + 1e-30) for nm in xs)
        ws = max(abs(res["se"][nm] - pse[nm]) / (abs(pse[nm]) + 1e-30) for nm in xs)
        print(f"  pyfixest cross-check (loose): beta={wb:.1e} se={ws:.1e} "
              f"({'ok' if wb < 1e-5 and ws < 1e-3 else 'WARN'})")
    except Exception as ex:  # noqa: BLE001
        print(f"  (pyfixest cross-check skipped: {ex})")

    return 1 if fail else 0


if __name__ == "__main__":
    sys.exit(main())
