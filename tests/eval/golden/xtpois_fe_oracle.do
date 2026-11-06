* xtpois_fe + xtpois_fe_keepif oracle re-derivation (BUG-227 tranche 1, WS-X 2026-08-07).
* xtpois.csv / xtpois_keepif.csv were self-captured from cobra; the generation-time
* Stata xcheck (tests/validate/xtpois_golden.do) used a DIFFERENT regressor set
* (included `female`), so it never covered this spec.
* Spec (tests/eval/manifest.json):
*   xtpois_fe:        cobra xtpois --data {xtpois_in} --y y_int --x treated,age_z,edu_hi --ivar h_pid
*   xtpois_fe_keepif: same + --keepif age_z>0
* Stata equivalent: xtpoisson y_int treated age_z edu_hi, fe   (conditional FE
* Poisson, default ML iid SEs), xtset h_pid.
* Data transport: tests/validate/xtpois_in.hbin -> xtpois_in_export.dta via
*   `cobra export-dta` (round-trip-gated transport).
* Run:  StataBE -b do xtpois_fe_oracle.do -> xtpois_fe_oracle.log
clear all
set more off
set type double
capture log close
log using "xtpois_fe_oracle.log", replace text
display "stata_version=" c(stata_version) " born=" c(born_date)
use "../../validate/xtpois_in_export.dta", clear
xtset h_pid

xtpoisson y_int treated age_z edu_hi, fe
display "XTPOIS_FE_ORACLE_BEGIN"
display "ORACLE|xtpois.csv|coeftable[treated].b|" %21.15e _b[treated]
display "ORACLE|xtpois.csv|coeftable[treated].se|" %21.15e _se[treated]
display "ORACLE|xtpois.csv|coeftable[age_z].b|" %21.15e _b[age_z]
display "ORACLE|xtpois.csv|coeftable[age_z].se|" %21.15e _se[age_z]
display "ORACLE|xtpois.csv|coeftable[edu_hi].b|" %21.15e _b[edu_hi]
display "ORACLE|xtpois.csv|coeftable[edu_hi].se|" %21.15e _se[edu_hi]
display "ORACLE|xtpois.csv|loglik|" %21.15e e(ll)
display "ORACLE|xtpois.csv|n_obs|" %21.15e e(N)
display "XTPOIS_FE_ORACLE_END"

xtpoisson y_int treated age_z edu_hi if age_z>0, fe
display "XTPOIS_FE_KEEPIF_ORACLE_BEGIN"
display "ORACLE|xtpois_keepif.csv|coeftable[treated].b|" %21.15e _b[treated]
display "ORACLE|xtpois_keepif.csv|coeftable[treated].se|" %21.15e _se[treated]
display "ORACLE|xtpois_keepif.csv|coeftable[age_z].b|" %21.15e _b[age_z]
display "ORACLE|xtpois_keepif.csv|coeftable[age_z].se|" %21.15e _se[age_z]
display "ORACLE|xtpois_keepif.csv|coeftable[edu_hi].b|" %21.15e _b[edu_hi]
display "ORACLE|xtpois_keepif.csv|coeftable[edu_hi].se|" %21.15e _se[edu_hi]
display "XTPOIS_FE_KEEPIF_ORACLE_END"
log close
