#!/usr/bin/env python3
"""PERFSWEEP bench fixtures (v2 -- k-heavy). Seed 42, deterministic.

v1 lesson: with k=3 every hoisted buffer is a handful of doubles, so the bench
had no dynamic range to measure. The hoisted buffers scale as np (= k + nuisance)
or k^2, so the fixture carries 40 regressors: qreg's Ai is then 41x41 doubles
(13.5 KB) copied twice per interior-point iteration, ivml/zicount's tp/tm are
np-sized and allocated 2*np times per Newton step.

  panel_stress : ng x T panels, few regressors -- xtlogit's per-panel lambda
                 allocates 10 vectors sized T and (T+1)^2, so T drives it.
  xsec_stress  : one cross-section, 40 regressors -- everything else.
"""
import numpy as np, pandas as pd, sys
rng = np.random.default_rng(42)
out = sys.argv[1]

# ---- panel_stress: allocation count = 10 * ng * iters, buffer size ~ (T+1)^2
NG, T = 20000, 40
N = NG * T
pid   = np.repeat(np.arange(NG, dtype=np.float64), T)
alpha = np.repeat(rng.normal(0, 1.0, NG), T)
x1 = rng.normal(0,1,N); x2 = rng.normal(0,1,N); x3 = rng.normal(0,1,N)
xb = 0.5*x1 - 0.3*x2 + 0.2*x3
ybin = (alpha + xb + rng.logistic(0,1,N) > 0).astype(np.float64)
ycnt = rng.poisson(np.exp(np.clip(0.3 + 0.2*alpha + 0.3*x1, -5, 5))).astype(np.float64)
panel = pd.DataFrame({"h_pid": pid, "tvar": np.tile(np.arange(T,dtype=np.float64),NG),
                      "ybin": ybin, "y_int": ycnt, "x1": x1, "x2": x2, "x3": x3})
panel.to_pickle(f"{out}/panel_stress.pkl")
m = panel.groupby("h_pid")["ybin"].mean()
print(f"panel_stress: N={N} ng={NG} T={T} informative={int(((m>0)&(m<1)).sum())}/{NG}")

# ---- xsec_stress: 40 regressors
M, K = 150_000, 40
W = rng.normal(0, 1, (M, K))
beta = np.linspace(0.25, -0.25, K) / np.sqrt(K)      # keep the index tame
z1 = rng.normal(0,1,M); z2 = rng.normal(0,1,M)
endog = 0.6*z1 + 0.4*z2 + 0.3*W[:,0] + rng.normal(0,1,M)
lin = 0.4*endog + W @ beta
y_cont = lin + rng.normal(0,1,M)
y_bin  = (lin + rng.logistic(0,1,M) > 0).astype(np.float64)
y_cens = np.maximum(0.0, y_cont)
cnt  = rng.poisson(np.exp(np.clip(0.5 + W[:,:3] @ np.array([0.3,-0.2,0.1]), -5, 5)))
infl = rng.random(M) < 1.0/(1.0+np.exp(-(-0.5 + 0.2*W[:,3])))
y_zi = np.where(infl, 0, cnt).astype(np.float64)
d = {"y_cont": y_cont, "y_bin": y_bin, "y_cens": y_cens, "y_zi": y_zi,
     "endog": endog, "z1": z1, "z2": z2,
     "grp": (np.arange(M) % 800).astype(np.float64),
     "gfac": (np.arange(M) % 200).astype(np.float64),
     "gfac20": (np.arange(M) % 20).astype(np.float64)}
for j in range(K): d[f"w{j+1}"] = W[:, j]
pd.DataFrame(d).to_pickle(f"{out}/xsec_stress.pkl")
print(f"xsec_stress: N={M} K={K} y_bin_mean={y_bin.mean():.4f} "
      f"zi_zerofrac={(y_zi==0).mean():.4f} cens_zerofrac={(y_cens==0).mean():.4f}")
