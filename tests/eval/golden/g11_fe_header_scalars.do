* ============================================================================
* g11_fe_header_scalars.do — Stata truth for reghdfe FULL-MODEL header R2/adj-R2
* Eval Harness §10 golden source.  StataNow BE 19.5 + reghdfe.
* COUNCIL-20260710 golden batch #1 — G11 (PLAYBOOK.md §7.2 item 11).
*
* Purpose: close the blind spot RAW-correctness.md's "Goldens blind-spot review"
* item 2 flags: the already-committed xtreg_fe_scalars.csv / reghdfe_absorb_vce_*
* goldens pin r2_within/r2_between/r2_overall but deliberately NEVER pinned the
* FULL-MODEL (FE-included) e(r2)/e(r2_a) reghdfe reports in its header -- because
* cobra's `feols --fe` currently emits its top-level "r2"/"adj_r2" fields as
* ALIASES of r2_within/an ad-hoc within-adjustment, NOT the full model R2.
* CONFIRMED live: `cobra feols --data panel_synth.hbin --y y --x x1,x2 --fe id
* --vcov cluster --cluster id --format tidy` -> r2==r2_within==2.3736852...e-01
* and adj_r2==1.2701640...e-01 -- both far below what a full-model (FE-included)
* R2 on a 100-group panel must be (FE alone should explain most of the id-level
* variance). This golden pins Stata's TRUE e(r2)/e(r2_a) so the mislabeling
* cannot keep shipping.
*
* Model: reghdfe y x1 x2, absorb(id) vce(unadjusted)   (POOLED single-FE, matches
*   reghdfe_absorb_vce.do's XCHECK_REGHDFE_IID block exactly modulo vce type)
*   == cobra feols --data panel_synth.hbin --y y --x x1,x2 --fe id --vcov iid
* Data: tests/validate/panel_synth.hbin (committed) via the ALREADY-COMMITTED
*   lossless CSV tests/eval/golden/panel_synth_full.csv (id=100 x t=8, N=800) --
*   same fixture xtreg_fe_scalars.do / reghdfe_absorb_vce.do already use.
*
* NO-GAMING CROSS-CHECK: r2_within here must reproduce the ALREADY-COMMITTED
* reghdfe_absorb_vce_A.csv r2_within (2.373685277923090e-01) and
* xtreg_fe_scalars.csv r2_within (same value) -- proving this is the identical
* model/data those goldens already pin, just reading two NEW e()-scalars
* (e(r2), e(r2_a)) off the same estimation.
*
* Run:  cd tests/eval/golden && StataBE -b do g11_fe_header_scalars.do
* Truth values transcribed (%21.15e) into g11_fe_header_scalars.csv by the
* Eval curator, validated against this log, then committed (Constitution Art 10).
* ============================================================================
clear all
set more off
set type double
capture log close
log using "g11_fe_header_scalars.log", replace text

import delimited using "`c(pwd)'/panel_synth_full.csv", clear case(preserve) asdouble
xtset id t

reghdfe y x1 x2, absorb(id) vce(unadjusted)
display "XCHECK_BEGIN"
display %21.15e e(r2_within) " = e(r2_within) want 2.373685277923090e-01 (reghdfe_absorb_vce_A.csv)"
display %21.15e _b[x1]       " = _b[x1]       want 5.411122024932790e-01 (reghdfe_absorb_vce_A.csv)"
display "XCHECK_END"

display "GOLDEN_BEGIN"
display %21.15e e(r2)          " = e(r2)          FULL-MODEL R2 (FE included) -- cobra r2 SHOULD equal this, currently equals r2_within instead"
display %21.15e e(r2_a)        " = e(r2_a)        FULL-MODEL adjusted R2 -- cobra adj_r2 SHOULD equal this"
display %21.15e e(r2_within)   " = e(r2_within)    (cobra r2_within -- already correctly pinned elsewhere, repeated here for completeness)"
display %21.15e e(N)           " = e(N)           want 8.000000000000000e+02"
display %21.15e e(df_a)        " = e(df_a)        absorbed dof (N_g)"
display "GOLDEN_END"

* ---- TWO-WAY FE variant, for completeness (matches reghdfe_absorb_vce.do's variant B) ----
reghdfe y x1 x2, absorb(id t) vce(unadjusted)
display "GOLDEN_B_BEGIN"
display %21.15e e(r2)          " = B e(r2)          FULL-MODEL R2, two-way FE"
display %21.15e e(r2_a)        " = B e(r2_a)        FULL-MODEL adjusted R2, two-way FE"
display %21.15e e(r2_within)   " = B e(r2_within)"
display %21.15e e(N)           " = B e(N)"
display "GOLDEN_B_END"

log close
