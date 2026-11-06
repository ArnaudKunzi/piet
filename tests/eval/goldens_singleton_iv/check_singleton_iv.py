#!/usr/bin/env python3
"""Golden regression: cobra `iv` singleton-FE drop (contract 1.7.0 §21a).

Closes the coverage gap that the standard 56/56 eval harness does NOT cover:
no pre-existing golden exercises iv+FE+singleton, so 56/56 alone does not verify
the new default singleton-drop against Stata.

Self-contained: rebuilds the input .hbin from the committed CSV via `cobra convert`
(no dependency on the gitignored tests/validate fixtures), runs cobra `iv` twice
(default = drops singletons, and --keep-singletons), and compares against the
Stata `ivregress 2sls y w i.firm (x = z), small` oracle captured in
singleton_iv_oracle.log (StataNow BE 19.5).

Usage:
    python3 check_singleton_iv.py --cobra /path/to/cobra
Exit 0 = pass. Non-zero = mismatch.
"""
import argparse, json, subprocess, sys, tempfile, os
from pathlib import Path

HERE = Path(__file__).resolve().parent
CSV = HERE / "singleton_iv.csv"

# Stata oracle (ivregress 2sls y w i.firm (x = z), small; StataNow BE 19.5).
# Display precision is Stata's 7-sig-fig `di`; tolerance set accordingly.
ORACLE = {
    "default": {"n_obs": 40, "n_dropped_singletons": 5,
                "x_b": 1.310801, "x_se": 0.2910784,
                "w_b": -0.37605187, "w_se": 0.28054615},
    "keep":    {"n_obs": 45, "n_dropped_singletons": 0,
                "x_b": 1.310801, "x_se": 0.2910784,
                "w_b": -0.37605187, "w_se": 0.28054615},
}
RTOL = 1e-6  # limited by Stata's 7-sig-fig display of the oracle


def run(cobra, hbin, keep_singletons):
    cmd = [cobra, "iv", "--data", hbin, "--y", "y", "--endog", "x",
           "--instruments", "z", "--x", "w", "--fe", "firm", "--vcov", "iid", "--tidy"]
    if keep_singletons:
        cmd.append("--keep-singletons")
    p = subprocess.run(cmd, capture_output=True, text=True)
    if p.returncode != 0:
        sys.exit(f"cobra iv failed: {p.stderr[:300]}")
    return json.loads(p.stdout)


def coef(d, name, field):
    row = next(r for r in d["coeftable"] if r["name"] == name)
    return row[field]


def check(label, d, want):
    fails = []
    if d.get("n_obs") != want["n_obs"]:
        fails.append(f"n_obs {d.get('n_obs')} != {want['n_obs']}")
    if d.get("n_dropped_singletons") != want["n_dropped_singletons"]:
        fails.append(f"n_dropped_singletons {d.get('n_dropped_singletons')} != {want['n_dropped_singletons']}")
    for nm in ("x", "w"):
        for f in ("b", "se"):
            got = coef(d, nm, f)
            exp = want[f"{nm}_{f}"]
            rel = abs(got - exp) / abs(exp)
            mark = "ok" if rel <= RTOL else "FAIL"
            print(f"  [{mark}] {label} {nm}.{f}: got={got:.10g} want={exp:.7g} rel={rel:.1e}")
            if rel > RTOL:
                fails.append(f"{nm}.{f} rel={rel:.1e}")
    return fails


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--cobra", required=True)
    a = ap.parse_args()
    with tempfile.TemporaryDirectory() as td:
        hbin = os.path.join(td, "singleton_iv.hbin")
        c = subprocess.run([a.cobra, "convert", str(CSV), hbin], capture_output=True, text=True)
        if c.returncode != 0:
            sys.exit(f"convert failed: {c.stderr[:300]}")
        fails = []
        print("cobra iv singleton-FE drop vs Stata ivregress 2sls oracle:")
        fails += check("default", run(a.cobra, hbin, False), ORACLE["default"])
        fails += check("keep   ", run(a.cobra, hbin, True), ORACLE["keep"])
    if fails:
        print("FAILED:", "; ".join(fails))
        sys.exit(1)
    print("PASS — iv singleton-drop matches Stata (default N=40 drop 5; keep N=45).")


if __name__ == "__main__":
    main()
