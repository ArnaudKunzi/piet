* Run from tests/validate: every path below is relative to it.
* Goldens for cecon logit / probit / margins / hausman.
clear all
set more off
set type double
capture log close
log using "glm_golden.log", replace text
use "csdid_input.dta", clear
xtset h_pid wv
gen byte treated = (gvar>0 & wv>=gvar) if !missing(gvar)
recast double treated
gen byte y01 = (visits_w > 0) if !missing(visits_w)
gen long y_int = round(visits_w)

* ---- logit (iid + robust) ----
logit y01 treated age_z edu_hi
matrix b = e(b)
di "LOGIT_B treated=" %20.15g b[1,1] " age_z=" %20.15g b[1,2] " edu_hi=" %20.15g b[1,3] " cons=" %20.15g b[1,4]
di "LOGIT_SE treated=" %20.15g _se[treated] " age_z=" %20.15g _se[age_z] " edu_hi=" %20.15g _se[edu_hi]
di "LOGIT_LL=" %20.15g e(ll)
margins, dydx(*) post
di "LOGIT_AME treated=" %20.15g _b[treated] " age_z=" %20.15g _b[age_z] " edu_hi=" %20.15g _b[edu_hi]
di "LOGIT_AME_SE treated=" %20.15g _se[treated] " age_z=" %20.15g _se[age_z] " edu_hi=" %20.15g _se[edu_hi]

* ---- probit ----
probit y01 treated age_z edu_hi
di "PROBIT_B treated=" %20.15g _b[treated] " age_z=" %20.15g _b[age_z] " edu_hi=" %20.15g _b[edu_hi] " cons=" %20.15g _b[_cons]
di "PROBIT_SE treated=" %20.15g _se[treated] " age_z=" %20.15g _se[age_z] " edu_hi=" %20.15g _se[edu_hi]
di "PROBIT_LL=" %20.15g e(ll)
margins, dydx(*) post
di "PROBIT_AME treated=" %20.15g _b[treated] " age_z=" %20.15g _b[age_z] " edu_hi=" %20.15g _b[edu_hi]

* ---- poisson margins ----
poisson y_int treated age_z edu_hi
margins, dydx(*) post
di "POIS_AME treated=" %20.15g _b[treated] " age_z=" %20.15g _b[age_z] " edu_hi=" %20.15g _b[edu_hi]

* ---- hausman FE vs RE ----
quietly xtreg visits_w treated age_z edu_hi, fe
estimates store fe
quietly xtreg visits_w treated age_z edu_hi, re
estimates store re
hausman fe re
di "HAUSMAN_CHI2=" %20.15g r(chi2) " DF=" r(df) " P=" %20.15g r(p)

log close
