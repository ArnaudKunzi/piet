* ============================================================================
* header_scalars.do — Stata truth for regression HEADER SCALARS (OLS, no FE)
* Eval Harness §10 golden source.  StataNow BE 19.5, sysuse auto.
*
* Purpose: pin the centered R-squared (and Root MSE / N / dof) that a no-FE OLS
* must report, so the engine cannot ship an UNCENTERED R-squared.  This is the
* discrimination golden for the 2026-06-19 feols-no-FE R^2 bug (MSG-EV-010):
* cobra `feols --vcov iid` on a no-FE model emitted r2_within = 1 - SSR/sum(y^2)
* (uncentered, 0.97702236) where Stata's centered e(r2) = 0.66139040.
*
* Model: regress mpg weight length   (auto.dta, n = 74, no fixed effects)
* The proof that 0.97702236 is the UNCENTERED R^2 is computed at the bottom.
*
* Run:  StataBE -b do header_scalars.do   →  header_scalars.log
* Truth values are transcribed (at %21.15e) into header_scalars.csv by the
* Eval curator (Opus), validated against this log, then committed.  Haiku never
* writes goldens; only Eval commits (Constitution §10, make-goldens pipeline).
* ============================================================================
clear all
set more off
capture log close
log using "header_scalars.log", replace text

sysuse auto, clear

* ---- the no-FE OLS model (PIET `regress` routes this to feols) ----
regress mpg weight length

display "GOLDEN_BEGIN"
display %21.15e e(r2)    " = e(r2)    centered R-squared   <- THE discriminator"
display %21.15e e(r2_a)  " = e(r2_a)  adjusted R-squared"
display %21.15e e(rmse)  " = e(rmse)  Root MSE = sqrt(e(rss)/e(df_r))"
display %21.15e e(N)     " = e(N)     number of observations"
display %21.15e e(F)     " = e(F)     model F(df_m, df_r)"
display %21.15e e(df_m)  " = e(df_m)  model dof"
display %21.15e e(df_r)  " = e(df_r)  residual dof  (cobra dof_resid)"
display %21.15e e(mss)   " = e(mss)   model sum of squares"
display %21.15e e(rss)   " = e(rss)   residual sum of squares (cobra sigma2 = rss/df_r)"
display "GOLDEN_END"

* ---- PROOF the buggy 0.97702236 is the UNCENTERED R^2 (no-tautology anchor) ----
scalar rss_saved = e(rss)
gen double mpg2 = mpg^2
quietly summarize mpg2, meanonly
scalar uncentered_tss = r(sum)          // = sum(mpg^2), the uncentered TSS
display "UNCENTERED_PROOF_BEGIN"
display %21.15e (1 - rss_saved/uncentered_tss) " = 1 - SSR/sum(y^2) == cobra buggy r2_within (0.97702236)"
display %21.15e uncentered_tss                 " = uncentered TSS = sum(mpg^2)"
display "UNCENTERED_PROOF_END"

log close
