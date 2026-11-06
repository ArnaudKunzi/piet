#!/usr/bin/env python3
"""Validate cecon FE-Poisson (PPML) vs pyfixest fepois, + an offset-invariance self-test.

The MLE is unique, so coefficients match pyfixest to ~1e-10 regardless of demean
nondeterminism; SEs are cross-checked loosely. The offset path is checked by the
invariance property: adding a constant to log-exposure must leave slopes unchanged
(it is absorbed by the FE / intercept).

Build the test .hbin first (needs a log-exposure column), e.g.:
  python - <<'PY'
  import pandas as pd, numpy as np
  df = pd.read_parquet(SRC).dropna(); df = df[df['pop']>0]
  df['log_pop']=np.log(df['pop']); df['log_pop_p5']=df['log_pop']+5
  df.to_parquet('/tmp/dery_off.parquet')
  PY
  python tools/to_hbin.py /tmp/dery_off.parquet dery_off.hbin --factor sigungu5,year
"""
import argparse
import json
import subprocess
import sys

import pandas as pd


def cecon_fepois(cecon, hbin, y, x, fe, cluster, offset=None):
    cmd = [cecon, "fepois", "--data", hbin, "--y", y, "--x", x, "--fe", fe,
           "--vcov", "cluster", "--cluster", cluster]
    if offset:
        cmd += ["--offset", offset]
    return json.loads(subprocess.run(cmd, capture_output=True, text=True, check=True).stdout)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--parquet", required=True)
    ap.add_argument("--hbin", required=True)
    ap.add_argument("--cecon", required=True)
    ap.add_argument("--y", required=True)
    ap.add_argument("--x", required=True)
    ap.add_argument("--fe", required=True)
    ap.add_argument("--cluster", required=True)
    ap.add_argument("--offset", default="")          # log-exposure column
    ap.add_argument("--offset-shift", default="")     # same + a constant, for the invariance test
    ap.add_argument("--rtol-coef", type=float, default=1e-6)
    args = ap.parse_args()

    xs = args.x.split(",")
    fes = args.fe.split(",")
    cols = [args.y] + xs + fes + [args.cluster]
    df = pd.read_parquet(args.parquet, columns=sorted(set(cols))).dropna()

    c0 = cecon_fepois(args.cecon, args.hbin, args.y, args.x, args.fe, args.cluster)

    import pyfixest as pf
    fml = f"{args.y} ~ " + "+".join(xs) + " | " + "+".join(fes)
    m = pf.fepois(fml, data=df, vcov={"CRV1": args.cluster})
    coef_py, se_py = m.coef().to_dict(), m.se().to_dict()

    print(f"\n=== {fml}  [cluster={args.cluster}] ===")
    print(f"  n_obs cecon={c0['n_obs']} pyfixest={m._N}  irls_iters={c0['irls_iters']} "
          f"converged={c0['converged']}")
    fail = False
    mrc = mrs = 0.0
    for nm in xs:
        rb = abs(c0["coef"][nm] - coef_py[nm]) / (abs(coef_py[nm]) + 1e-30)
        rs = abs(c0["se"][nm] - se_py[nm]) / (abs(se_py[nm]) + 1e-30)
        mrc, mrs = max(mrc, rb), max(mrs, rs)
        flag = "" if rb <= args.rtol_coef else "  <<FAIL"
        if flag:
            fail = True
        print(f"  {nm:8s} beta {c0['coef'][nm]:13.7g} (py {coef_py[nm]:13.7g}, r={rb:.1e})  "
              f"se {c0['se'][nm]:11.6g} (py {se_py[nm]:11.6g}, r={rs:.1e}){flag}")
    print(f"  max rel: coef={mrc:.2e} (gate {args.rtol_coef:.0e}), se={mrs:.2e} (loose)")

    if args.offset and args.offset_shift:
        ca = cecon_fepois(args.cecon, args.hbin, args.y, args.x, args.fe, args.cluster, args.offset)
        cb = cecon_fepois(args.cecon, args.hbin, args.y, args.x, args.fe, args.cluster, args.offset_shift)
        inv = max(abs(ca["coef"][nm] - cb["coef"][nm]) for nm in xs)
        ok = inv < 1e-9
        print(f"  offset invariance ({args.offset} vs {args.offset_shift}): "
              f"max |dbeta|={inv:.2e}  {'ok' if ok else 'FAIL'}")
        if not ok:
            fail = True

    print("RESULT:", "FAIL" if fail else "PASS")
    return 1 if fail else 0


if __name__ == "__main__":
    sys.exit(main())
