#!/usr/bin/env python3
# ctest gate for the Tier-4 stats/tabulation commands: run each cobra command on
# the auto.hbin fixture and check headline numbers against the Stata goldens
# (tests/validate/t4_*_golden.csv, produced by t4_*_golden.do via ivreg2-free Stata).
import json, subprocess, sys, os

from _cobra_bin import resolve_cobra  # noqa: E402  (sibling module; see it for why)

COBRA = resolve_cobra()
# FULL FUNCTIONAL CHECK (2026-08-05): $COBRA_VALIDATE_DIR wins over the
# CMake-configured argv[2]. The goldens and .hbin fixtures this reads are
# gitignored, so in a fresh worktree argv[2] points at that worktree's own
# EMPTY tests/validate and the test fails for a reason unrelated to the
# code under test — every P7 agent hit this and none could work around it.
# The env var is the same override name the C++ validators already honour
# (CMakeLists.txt COBRA_VALIDATE_DIR compile definition); these three were
# simply never wired to it.
VDIR = (os.environ.get("COBRA_VALIDATE_DIR")
        or (sys.argv[2] if len(sys.argv) > 2 else None)
        or os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from _fixtures import need  # noqa: E402 (GA sweep: honest SKIP on absent fixtures)
HBIN = need(os.path.join(VDIR, "auto.hbin"))
fails = []

def gold(cmd):
    g = {}
    with open(need(os.path.join(VDIR, f"t4_{cmd}_golden.csv"))) as f:
        next(f)
        for line in f:
            line = line.strip()
            if not line or "," not in line: continue
            k, v = line.rsplit(",", 1)  # keys may contain commas (e.g. C[mpg,price]); value is the last field
            try: g[k.strip()] = float(v)
            except ValueError: g[k.strip()] = v.strip()
    return g

def run(args):
    r = subprocess.run([COBRA] + args + ["--data", HBIN, "--no-provenance"],
                       capture_output=True, text=True)
    if r.returncode != 0:
        raise RuntimeError(f"{' '.join(args)} -> exit {r.returncode}: {r.stderr[:200]}")
    return json.loads(r.stdout)

def chk(label, got, want, tol=1e-6):
    if want == 0: ok = abs(got) < 1e-9
    else: ok = abs(got - want) / abs(want) <= tol
    print(f"  {'ok ' if ok else 'FAIL'} {label}: got={got:.10g} want={want:.10g}")
    if not ok: fails.append(label)

# ---- corr ----
g = gold("corr")
o = run(["corr", "--vars", "price,mpg,weight,length"])
chk("corr r[mpg,price]", o["r"][1][0], g["A.C[mpg,price]"])
chk("corr r[length,weight]", o["r"][3][2], g["A.C[length,weight]"])
o = run(["corr", "--vars", "price,mpg,weight", "--pairwise", "--sig"])
chk("pwcorr p[mpg,price]", o["p"][1][0], g["B.sig[mpg,price]"], 1e-5)
chk("pwcorr p[weight,mpg]", o["p"][2][1], g["B.sig[weight,mpg]"], 1e-5)

# ---- ttest ----
g = gold("ttest")
o = run(["ttest", "--var", "mpg", "--against", "21"])
chk("ttest1 t", o["t"], g["c1.t"]); chk("ttest1 p", o["p"], g["c1.p"])
o = run(["ttest", "--var", "mpg", "--by", "foreign"])
chk("ttest2 t", o["t"], g["c2.t"]); chk("ttest2 p", o["p"], g["c2.p"]); chk("ttest2 diff", o["mean_diff"], g["c2.mu_diff"])
o = run(["ttest", "--var", "mpg", "--by", "foreign", "--unequal"])
chk("ttestW t", o["t"], g["c3.t"]); chk("ttestW df", o["df"], g["c3.df_t"]); chk("ttestW p", o["p"], g["c3.p"])

# ---- sdtest ----
g = gold("sdtest")
o = run(["sdtest", "--var", "mpg", "--by", "foreign"])
chk("sdtest F", o["F"], g["A_F"]); chk("sdtest p", o["p"], g["A_p"])
o = run(["sdtest", "--var", "mpg", "--against", "5"])
chk("sdtest1 chi2", o["chi2"], g["B_chi2"]); chk("sdtest1 p", o["p"], g["B_p"])

# ---- vif ----
g = gold("vif")
o = run(["vif", "--y", "price", "--x", "mpg,weight,length,turn"])
vif = {c["name"]: c["b"] for c in o["coefs"]}
chk("vif mpg", vif["mpg"], g["vif_mpg"])
chk("vif weight", vif["weight"], g["vif_weight"])
chk("vif mean", o["mean_vif"], g["mean_vif"])

# ---- tabstat ----
g = gold("tabstat")
o = run(["tabstat", "--vars", "price,mpg,weight", "--stats", "mean,sd,min,max,n,p50"])
chk("tabstat price.mean", o["total"]["price"]["mean"], g["T1.Mean.price"])
chk("tabstat price.p50", o["total"]["price"]["p50"], g["T1.p50.price"])
chk("tabstat weight.sd", o["total"]["weight"]["sd"], g["T1.SD.weight"])
o = run(["tabstat", "--vars", "price,mpg", "--by", "foreign", "--stats", "mean,sd,n"])
chk("tabstat g2.mpg.mean", o["groups"][1]["stats"]["mpg"]["mean"], g["T2.g2.Mean.mpg"])

# ---- tabulate ----
g = gold("tabulate")
o = run(["tabulate", "--vars", "rep78"])
lv = {int(l["value"]): l for l in o["levels"]}
chk("tab1 freq[3]", lv[3]["freq"], g["ow_freq_3"]); chk("tab1 pct[3]", lv[3]["percent"], g["ow_pct_3"])
o = run(["tabulate", "--vars", "foreign,rep78"])
chk("tab2 chi2", o["chi2"], g["tw_chi2"]); chk("tab2 p", o["p"], g["tw_p"], 1e-5)
chk("tab2 V", o["cramers_v"], g["tw_CramersV"]); chk("tab2 gamma", o["gamma"], g["tw_gamma"])
chk("tab2 taub", o["taub"], g["tw_taub"]); chk("tab2 lr", o["lr_chi2"], g["tw_chi2_lr"])

# ---- codebook ----
g = gold("codebook")
o = run(["codebook", "--vars", "price,rep78,gear_ratio"])
e = {x["name"]: x for x in o["entries"]}
chk("cb price.n_unique", e["price"]["n_unique"], g["price_n_unique"])
chk("cb rep78.n_missing", e["rep78"]["n_missing"], g["rep78_n_missing"])
chk("cb gear.mean", e["gear_ratio"]["mean"], g["gear_ratio_mean"])
chk("cb gear.p50", e["gear_ratio"]["p50"], g["gear_ratio_p50"])

print(f"\n{'FAILED: ' + ', '.join(fails) if fails else 'PASSED (all Tier-4 vs Stata goldens)'}")
sys.exit(1 if fails else 0)
