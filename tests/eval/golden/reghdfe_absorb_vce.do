* ============================================================================
* reghdfe_absorb_vce.do — Stata truth for reghdfe absorb()+vce(cluster) with the
* reconstructed _cons row (COBRA feols GAP-B / 689b53c).  StataNow BE 19.5 + reghdfe.
*
* Purpose: pin the _cons value+SE (the field the 689b53c fix ADDS), n_clusters,
* absorbed dims, and the R2-family/F/RMSE for the clustered FE estimator — closing
* the coeftable-only blind spot for reghdfe absorb()+vce(cluster). Two variants:
*   (A) POOLED single-FE  : reghdfe y x1 x2, absorb(id)   vce(cluster id)
*        == cobra feols --y y --x x1,x2 --fe id   --vcov cluster --cluster id
*   (B) TWO-WAY FE (interaction of FE) : reghdfe y x1 x2, absorb(id t) vce(cluster id)
*        == cobra feols --y y --x x1,x2 --fe id,t --vcov cluster --cluster id
*
* Data: tests/validate/panel_synth.hbin (committed) via its committed lossless CSV
*   tests/eval/golden/panel_synth_full.csv (id=100 units x t=8 periods, N=800).
*   (%.17g round-trips float64 exactly; same CSV the xtreg_fe_scalars golden uses.)
*
* NO-GAMING CROSS-CHECK 1: reghdfe absorb(id) with IID SE must reproduce the
*   ALREADY-COMMITTED xtreg_fe_scalars.csv within-estimator numbers (r2_w/r2_b/
*   r2_o/sigma_e/_b[x1]/_b[x2]) — proving the reghdfe route == the xtreg,fe route
*   the existing golden pinned, and that the CSV is bit-faithful to the hbin.
* NO-GAMING CROSS-CHECK 2: reghdfe reports _cons; xtreg,fe reports the SAME _cons
*   (both are the grand-mean intercept). We display xtreg,fe _cons alongside to
*   prove they agree — the fix reconstructs exactly this quantity.
*
* Run:  cd tests/eval/golden && StataBE -b do reghdfe_absorb_vce.do
* Truth transcribed (%21.15e) into reghdfe_absorb_vce.csv by the Eval curator,
* validated against this log, then committed (Constitution Art 10 / §10).
* ============================================================================
clear all
set more off
set type double
capture log close
log using "reghdfe_absorb_vce.log", replace text

import delimited using "`c(pwd)'/panel_synth_full.csv", clear case(preserve) asdouble
xtset id t

* ---- CROSS-CHECK: reghdfe absorb(id) IID == the committed xtreg_fe_scalars ----
* (existing golden: r2_w=2.373685e-01-ish; here we just prove agreement.)
reghdfe y x1 x2, absorb(id) vce(unadjusted)
display "XCHECK_REGHDFE_IID_BEGIN"
display %21.15e e(r2_within) " = e(r2_within)"
display %21.15e _b[x1]       " = _b[x1]"
display %21.15e _b[x2]       " = _b[x2]"
display %21.15e _b[_cons]    " = _b[_cons] (reghdfe reconstructed intercept)"
display "XCHECK_REGHDFE_IID_END"

xtreg y x1 x2, fe
display "XCHECK_XTREGFE_BEGIN"
display %21.15e _b[x1]    " = xtreg,fe _b[x1]  (must match reghdfe)"
display %21.15e _b[_cons] " = xtreg,fe _b[_cons] (must match reghdfe _cons)"
display "XCHECK_XTREGFE_END"

* ============================================================================
* VARIANT A — POOLED single-FE, clustered on id
* ============================================================================
reghdfe y x1 x2, absorb(id) vce(cluster id)
display "GOLDEN_A_BEGIN"
display %21.15e _b[x1]         " = A coeftable[x1].b"
display %21.15e _se[x1]        " = A coeftable[x1].se"
display %21.15e _b[x2]         " = A coeftable[x2].b"
display %21.15e _se[x2]        " = A coeftable[x2].se"
display %21.15e _b[_cons]      " = A coeftable[_cons].b"
display %21.15e _se[_cons]     " = A coeftable[_cons].se"
display %21.15e e(r2_within)   " = A r2_within"
display %21.15e e(r2)          " = A r2"
display %21.15e e(rmse)        " = A rmse"
display %21.15e e(F)           " = A F"
display %21.15e e(N)           " = A n_obs"
display %21.15e e(N_clust)     " = A n_clusters"
display %21.15e e(df_r)        " = A dof_resid (reghdfe: clusters-1)"
display "GOLDEN_A_END"

* ============================================================================
* VARIANT B — TWO-WAY FE (interaction of FE id x t), clustered on id
* ============================================================================
reghdfe y x1 x2, absorb(id t) vce(cluster id)
display "GOLDEN_B_BEGIN"
display %21.15e _b[x1]         " = B coeftable[x1].b"
display %21.15e _se[x1]        " = B coeftable[x1].se"
display %21.15e _b[x2]         " = B coeftable[x2].b"
display %21.15e _se[x2]        " = B coeftable[x2].se"
display %21.15e _b[_cons]      " = B coeftable[_cons].b"
display %21.15e _se[_cons]     " = B coeftable[_cons].se"
display %21.15e e(r2_within)   " = B r2_within"
display %21.15e e(rmse)        " = B rmse"
display %21.15e e(F)           " = B F"
display %21.15e e(N)           " = B n_obs"
display %21.15e e(N_clust)     " = B n_clusters"
display "GOLDEN_B_END"

log close
