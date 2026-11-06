* ============================================================================
* mi231_level90.do — StataBE truth for logit/probit level(90) z-Wald CI
* Eval Harness §10 golden source.  StataNow BE 19.5, sysuse auto.
* MSG-MI-231 (maintainer->eval-harness, 2026-07-15) — B.0.3 option-contract campaign.
*
* Purpose: pin the level(90) coeftable CI (b ± invnormal(1-alpha/2)*se) for
* logit and probit, same model as the committed logit_scalars.csv /
* probit_scalars.csv IID goldens (foreign mpg weight) so this is a same-model
* companion, not a new model. Default vce (iid) — the b/se here MUST match
* logit_scalars.csv/probit_scalars.csv's own coefficients (no separate golden
* pins those directly, so this file also serves as that record).
*
* Run:  cd tests/eval/golden && StataBE -b do mi231_level90.do
* Truth values transcribed (%21.15e) into mi231_level90_logit.csv /
* mi231_level90_probit.csv by the Eval curator, validated against this log,
* then committed (Constitution Art 10).
* ============================================================================
clear all
set more off
set type double
capture log close
log using "mi231_level90.log", replace text

sysuse auto, clear

* ============================================================================
* LOGIT — level(90), default (iid) vce
* ============================================================================
logit foreign mpg weight, level(90)
display "LOGIT_LEVEL90_BEGIN"
display %21.15e e(N)        " = e(N)"
display %21.15e _b[mpg]     " = b_mpg"
display %21.15e _se[mpg]    " = se_mpg"
display %21.15e r(table)[1,1] " = b_mpg (r(table) row1, cross-check)"
display %21.15e r(table)[2,1] " = se_mpg (r(table) row2, cross-check)"
display %21.15e r(table)[3,1] " = z_mpg"
display %21.15e r(table)[4,1] " = p_mpg"
display %21.15e r(table)[5,1] " = ci_lo_mpg (90% CI)"
display %21.15e r(table)[6,1] " = ci_hi_mpg (90% CI)"
display %21.15e _b[weight]  " = b_weight"
display %21.15e _se[weight] " = se_weight"
display %21.15e r(table)[5,2] " = ci_lo_weight (90% CI)"
display %21.15e r(table)[6,2] " = ci_hi_weight (90% CI)"
display %21.15e _b[_cons]   " = b_cons"
display %21.15e _se[_cons]  " = se_cons"
display %21.15e r(table)[5,3] " = ci_lo_cons (90% CI)"
display %21.15e r(table)[6,3] " = ci_hi_cons (90% CI)"
display "LOGIT_LEVEL90_END"

* ============================================================================
* PROBIT — level(90), default (iid) vce
* ============================================================================
probit foreign mpg weight, level(90)
display "PROBIT_LEVEL90_BEGIN"
display %21.15e e(N)        " = e(N)"
display %21.15e _b[mpg]     " = b_mpg"
display %21.15e _se[mpg]    " = se_mpg"
display %21.15e r(table)[3,1] " = z_mpg"
display %21.15e r(table)[4,1] " = p_mpg"
display %21.15e r(table)[5,1] " = ci_lo_mpg (90% CI)"
display %21.15e r(table)[6,1] " = ci_hi_mpg (90% CI)"
display %21.15e _b[weight]  " = b_weight"
display %21.15e _se[weight] " = se_weight"
display %21.15e r(table)[5,2] " = ci_lo_weight (90% CI)"
display %21.15e r(table)[6,2] " = ci_hi_weight (90% CI)"
display %21.15e _b[_cons]   " = b_cons"
display %21.15e _se[_cons]  " = se_cons"
display %21.15e r(table)[5,3] " = ci_lo_cons (90% CI)"
display %21.15e r(table)[6,3] " = ci_hi_cons (90% CI)"
display "PROBIT_LEVEL90_END"

log close
