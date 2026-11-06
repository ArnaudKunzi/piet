#!/usr/bin/env python3
"""Validate cecon 2SLS/IV against a deterministic numpy oracle (gate) + pyfixest (loose).

Example:
  validate_iv.py --parquet P.parquet --hbin P.hbin --cecon ./build/cecon \
      --y y_all --x tmaxb,tminb,precd,wsd --endog pm25 --instruments wq \
      --fe sigungu5,year --cluster sigungu5
"""
import argparse
import json
import subprocess
import sys

import numpy as np
import pandas as pd


def demean(M, fe_codes, tol=1e-13, maxit=2000):
    M = M.astype(float).copy()
    if not fe_codes:
        return M
    single = len(fe_codes) == 1
    scale = np.sqrt((M * M).mean(axis=0))
    scale[scale == 0] = 1.0
    for _ in range(maxit):
        worst = 0.0
        for codes, K in fe_codes:
            cnt = np.bincount(codes, minlength=K)
            for j in range(M.shape[1]):
                means = np.bincount(codes, weights=M[:, j], minlength=K) / cnt
                M[:, j] -= means[codes]
                worst = max(worst, np.abs(means).max() / scale[j])
        if single or worst < tol:
            break
    return M


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--parquet", required=True)
    ap.add_argument("--hbin", required=True)
    ap.add_argument("--cecon", required=True)
    ap.add_argument("--y", required=True)
    ap.add_argument("--x", default="")
    ap.add_argument("--endog", required=True)
    ap.add_argument("--instruments", required=True)
    ap.add_argument("--fe", default="")
    ap.add_argument("--cluster", default="")
    ap.add_argument("--rtol-beta", type=float, default=1e-7)
    ap.add_argument("--rtol-se", type=float, default=1e-6)
    args = ap.parse_args()

    exog = args.x.split(",") if args.x else []
    endog = args.endog.split(",")
    inst = args.instruments.split(",")
    fes = args.fe.split(",") if args.fe else []
    cl = args.cluster.split(",") if args.cluster else []
    cols = [args.y] + exog + endog + inst + fes + cl
    df = pd.read_parquet(args.parquet, columns=sorted(set(cols))).dropna()
    N = len(df)

    fe_codes = []
    for f in fes:
        c, _ = pd.factorize(df[f].values)
        fe_codes.append((c.astype(np.int64), int(c.max()) + 1))

    def dm(names):
        if not names:
            return np.empty((N, 0))
        return demean(np.column_stack([df[c].to_numpy(float) for c in names]), fe_codes)

    yd = demean(df[args.y].to_numpy(float).reshape(-1, 1), fe_codes)[:, 0]
    Xe = dm(exog)
    Xen = dm(endog)
    Z = dm(inst)
    W = np.column_stack([Xe, Z]) if exog else Z
    X = np.column_stack([Xe, Xen]) if exog else Xen
    breadW = np.linalg.inv(W.T @ W)
    Xhat_en = W @ (breadW @ (W.T @ Xen))
    Xhat = np.column_stack([Xe, Xhat_en]) if exog else Xhat_en
    bread = np.linalg.inv(Xhat.T @ Xhat)
    beta = bread @ (Xhat.T @ yd)
    e = yd - X @ beta
    names = exog + endog  # cecon order: exog then endog

    res = run = subprocess.run(
        [args.cecon, "iv", "--data", args.hbin, "--y", args.y]
        + (["--x", args.x] if exog else [])
        + ["--endog", args.endog, "--instruments", args.instruments]
        + (["--fe", args.fe] if fes else [])
        + (["--vcov", "twoway" if len(cl) > 1 else "cluster", "--cluster", args.cluster]
           if cl else ["--vcov", "iid"]),
        capture_output=True, text=True, check=True)
    res = json.loads(run.stdout)

    if cl:
        gcodes = [pd.factorize(df[c].values)[0].astype(np.int64) for c in cl]
        Gs = [int(g.max()) + 1 for g in gcodes]
        def meat(g, G):
            S = np.zeros((G, Xhat.shape[1]))
            for j in range(Xhat.shape[1]):
                S[:, j] = np.bincount(g, weights=Xhat[:, j] * e, minlength=G)
            return S.T @ S
        if len(cl) == 1:
            M = meat(gcodes[0], Gs[0])
        else:
            pair, _ = pd.factorize(gcodes[0] * Gs[1] + gcodes[1])
            M = (meat(gcodes[0], Gs[0]) + meat(gcodes[1], Gs[1])
                 - meat(pair, int(pair.max()) + 1))
        Gmin = res["n_clusters"]
        k_adj = res["cluster_dof_k"]
        factor = (Gmin / (Gmin - 1)) * ((N - 1) / (N - k_adj))
        Vexp = factor * bread @ M @ bread
    else:
        sigma2 = (e @ e) / res["dof_resid"]
        Vexp = sigma2 * bread
    se_exp = np.sqrt(np.diag(Vexp))

    label = (f"{args.y} ~ {'+'.join(exog) if exog else '.'} + ({'+'.join(endog)} ~ "
             f"{'+'.join(inst)})" + (f" | {'+'.join(fes)}" if fes else "")
             + f"  [{'+'.join(cl) if cl else 'iid'}]")
    print(f"\n=== {label} ===")
    print(f"  n_obs={res['n_obs']}  vcov={res['vcov_type']}  first_stage_F={res['first_stage_F']}")
    fail = False
    mrb = mrs = 0.0
    for j, nm in enumerate(names):
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
    return 1 if fail else 0


if __name__ == "__main__":
    sys.exit(main())
