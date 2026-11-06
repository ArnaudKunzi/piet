#!/usr/bin/env python3
"""PERFSWEEP paired bench. ratio-only reporting per [[bargrade-two-tier-measurement]].
Arms alternate A/B/A/B/A/B so machine drift cancels; wall = median of 3 per arm.
Usage: bench.py --a BIN_A --b BIN_B --only name1,name2  [--reps 3]
       bench.py --a BIN --only ...            (single-arm baseline)
"""
import argparse, json, statistics, subprocess, sys, time, os

SP = os.path.dirname(os.path.abspath(__file__))
PANEL = f"{SP}/panel_stress.hbin"
XSEC  = f"{SP}/xsec_stress.hbin"
BIG   = f"{SP}/panel_big.hbin"
SURV  = f"{SP}/surv_stress.hbin"

CMDS = {
 "xtlogit":  ["xtlogit","--data",PANEL,"--y","ybin","--x","x1,x2,x3","--ivar","h_pid"],
 "xtlogit_big": ["xtlogit","--data",BIG,"--y","ybin","--x","x1,x2,x3","--ivar","h_pid"],
 "xtpois_big":  ["xtpois","--data",BIG,"--y","y_int","--x","x1,x2,x3","--ivar","h_pid"],
 "xtpois":   ["xtpois","--data",PANEL,"--y","y_int","--x","x1,x2,x3","--ivar","h_pid"],
 "nbreg":    ["nbreg","--data",XSEC,"--y","y_zi","--x","w1,w2,w3,w4,w5,w6,w7,w8,w9,w10,w11,w12,w13,w14,w15,w16,w17,w18,w19,w20,w21,w22,w23,w24,w25,w26,w27,w28,w29,w30,w31,w32,w33,w34,w35,w36,w37,w38,w39,w40"],
 "zip":      ["zip","--data",XSEC,"--y","y_zi","--x","w1,w2,w3,w4,w5,w6,w7,w8,w9,w10,w11,w12,w13,w14,w15,w16,w17,w18,w19,w20,w21,w22,w23,w24,w25,w26,w27,w28,w29,w30,w31,w32,w33,w34,w35,w36,w37,w38,w39,w40","--inflate","_cons"],
 "zinb":     ["zinb","--data",XSEC,"--y","y_zi","--x","w1,w2,w3,w4,w5,w6,w7,w8,w9,w10,w11,w12,w13,w14,w15,w16,w17,w18,w19,w20,w21,w22,w23,w24,w25,w26,w27,w28,w29,w30,w31,w32,w33,w34,w35,w36,w37,w38,w39,w40","--inflate","_cons"],
 "ivprobit": ["ivprobit","--data",XSEC,"--y","y_bin","--x","w1,w2,w3,w4,w5,w6,w7,w8,w9,w10,w11,w12,w13,w14,w15,w16,w17,w18,w19,w20,w21,w22,w23,w24,w25,w26,w27,w28,w29,w30,w31,w32,w33,w34,w35,w36,w37,w38,w39,w40","--endog","endog","--instruments","z1,z2"],
 "ivtobit":  ["ivtobit","--data",XSEC,"--y","y_cens","--x","w1,w2,w3,w4,w5,w6,w7,w8,w9,w10,w11,w12,w13,w14,w15,w16,w17,w18,w19,w20,w21,w22,w23,w24,w25,w26,w27,w28,w29,w30,w31,w32,w33,w34,w35,w36,w37,w38,w39,w40","--endog","endog","--instruments","z1,z2","--ll","0"],
 "mixed":    ["mixed","--data",XSEC,"--y","y_cont","--x","w1,w2,w3,w4,w5,w6,w7,w8,w9,w10,w11,w12,w13,w14,w15,w16,w17,w18,w19,w20,w21,w22,w23,w24,w25,w26,w27,w28,w29,w30,w31,w32,w33,w34,w35,w36,w37,w38,w39,w40","--group","grp"],
 "qreg":     ["qreg","--data",XSEC,"--y","y_cont","--x","w1,w2,w3,w4,w5,w6,w7,w8,w9,w10,w11,w12,w13,w14,w15,w16,w17,w18,w19,w20,w21,w22,w23,w24,w25,w26,w27,w28,w29,w30,w31,w32,w33,w34,w35,w36,w37,w38,w39,w40","--tau","0.5"],

 "tobit":   ["tobit","--data",XSEC,"--y","y_cens","--x","w1,w2,w3,w4,w5,w6,w7,w8,w9,w10,w11,w12,w13,w14,w15,w16,w17,w18,w19,w20,w21,w22,w23,w24,w25,w26,w27,w28,w29,w30,w31,w32,w33,w34,w35,w36,w37,w38,w39,w40","--ll","0"],
 "mixed40": ["mixed","--data",XSEC,"--y","y_cont","--x","w1,w2,w3,w4,w5,w6,w7,w8,w9,w10,w11,w12,w13,w14,w15,w16,w17,w18,w19,w20,w21,w22,w23,w24,w25,w26,w27,w28,w29,w30,w31,w32,w33,w34,w35,w36,w37,w38,w39,w40","--group","grp"],
 "ologit":  ["ologit","--data",XSEC,"--y","gfac20","--x","w1,w2,w3,w4,w5,w6"],
 "mlogit":  ["mlogit","--data",XSEC,"--y","gfac20","--x","w1,w2,w3,w4,w5,w6"],
 "streg":   ["streg","--data",SURV,"--x","v1,v2,v3,v4,v5,v6,v7,v8,v9,v10,v11,v12"],
 "chow":     ["chow","--data",XSEC,"--y","y_cont","--x","w1,w2,w3","--by","gfac"],
 "chow20":   ["chow","--data",XSEC,"--y","y_cont","--x","w1,w2,w3","--by","gfac20"],
}

def run(binpath, argv):
    t = time.perf_counter()
    p = subprocess.run([binpath]+argv, capture_output=True, text=True)
    return time.perf_counter()-t, p.returncode, p.stdout, p.stderr

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--a", required=True); ap.add_argument("--b")
    ap.add_argument("--only", default=",".join(CMDS)); ap.add_argument("--reps", type=int, default=3)
    ap.add_argument("--json"); a = ap.parse_args()
    names = [n for n in a.only.split(",") if n in CMDS]
    res = {}
    for n in names:
        argv = CMDS[n]
        wa, wb = [], []
        rc_a = rc_b = 0; err = ""
        for r in range(a.reps):
            w, rc, so, se = run(a.a, argv); wa.append(w); rc_a = rc or rc_a
            if rc and not err: err = se.strip()[:160]
            if a.b:
                w, rc, so, se = run(a.b, argv); wb.append(w); rc_b = rc or rc_b
                if rc and not err: err = se.strip()[:160]
        ma = statistics.median(wa)
        row = {"a_median_s": round(ma,3), "rc_a": rc_a}
        if a.b:
            mb = statistics.median(wb)
            row.update({"b_median_s": round(mb,3), "rc_b": rc_b, "ratio_b_over_a": round(mb/ma,4)})
        if err: row["err"] = err
        res[n] = row
        line = f"{n:10s} A={ma:7.3f}s rc={rc_a}"
        if a.b: line += f"  B={mb:7.3f}s rc={rc_b}  ratio={mb/ma:.4f}"
        if err: line += f"  ERR: {err}"
        print(line, flush=True)
    if a.json: json.dump(res, open(a.json,"w"), indent=1)

main()
