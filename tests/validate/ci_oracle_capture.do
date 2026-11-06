* Run from tests/validate: every path below is relative to it.
* WS-J EVAL-TOL-AUDIT / margins-CI job: fresh CI oracle capture for the
* families with zero existing ci_lo/ci_hi witness (poisson-via-margins,
* nbreg, tobit, heckman, xtpoisson-fe), plus a GLM margins at() CI.
* Re-runs the EXACT same specs as the existing goldens (xtpois_golden.do /
* tobit_golden.do / nbreg_golden.do / heckman_golden.do / posthest_golden.do)
* and reads Stata's own displayed 95% CI straight out of r(table)["ll"/"ul"].
clear all
set more off
set type double
capture log close
log using "ci_oracle_capture.log", replace text

local VAL "."

* ---------- xtpoisson, fe (the "poisson" family CI witness) --------------
use "`VAL'/csdid_input.dta", clear
xtset h_pid wv
gen byte treated = (gvar>0 & wv>=gvar) if !missing(gvar)
recast double treated
gen long y_int = round(visits_w)
qui replace y_int = . if missing(visits_w)
xtpoisson y_int treated female age_z edu_hi, fe
matrix tab_xtp = r(table)
di "XTPOIS_TREATED_B="  %22.15g tab_xtp[1,1]
di "XTPOIS_TREATED_SE=" %22.15g tab_xtp[2,1]
di "XTPOIS_TREATED_LL=" %22.15g tab_xtp[5,1]
di "XTPOIS_TREATED_UL=" %22.15g tab_xtp[6,1]

* ---------- tobit ----------------------------------------------------------
use "`VAL'/tobit_input.dta", clear
tobit visits_w age_z edu_hi treated, ll(0)
matrix tab_tob = r(table)
di "TOBIT_AGEZ_B="  %22.15g tab_tob[1,1]
di "TOBIT_AGEZ_SE=" %22.15g tab_tob[2,1]
di "TOBIT_AGEZ_LL=" %22.15g tab_tob[5,1]
di "TOBIT_AGEZ_UL=" %22.15g tab_tob[6,1]

* ---------- nbreg -----------------------------------------------------------
use "`VAL'/csdid_input.dta", clear
gen byte treated2 = (gvar>0 & wv>=gvar) if !missing(gvar)
recast double treated2
gen long y_int2 = round(visits_w)
drop if missing(y_int2)
nbreg y_int2 treated2 female age_z edu_hi
matrix tab_nb = r(table)
di "NBREG_TREATED_B="  %22.15g tab_nb[1,1]
di "NBREG_TREATED_SE=" %22.15g tab_nb[2,1]
di "NBREG_TREATED_LL=" %22.15g tab_nb[5,1]
di "NBREG_TREATED_UL=" %22.15g tab_nb[6,1]

* ---------- heckman (twostep) ----------------------------------------------
use "`VAL'/heckman_input.dta", clear
heckman y x1, select(S = z1 z2) twostep
matrix tab_hk = r(table)
di "HECKMAN_X1_B="  %22.15g tab_hk[1,1]
di "HECKMAN_X1_SE=" %22.15g tab_hk[2,1]
di "HECKMAN_X1_LL=" %22.15g tab_hk[5,1]
di "HECKMAN_X1_UL=" %22.15g tab_hk[6,1]

* ---------- GLM margins, at() — the BUG-194 GLM-at() site -----------------
use "`VAL'/csdid_input.dta", clear
gen byte treated3 = (gvar>0 & wv>=gvar) if !missing(gvar)
recast double treated3
gen long y_int3 = round(visits_w)
drop if missing(y_int3)
poisson y_int3 treated3 age_z edu_hi
margins, at(treated3=0 age_z=0 edu_hi=0) at(treated3=1 age_z=1 edu_hi=1) post
matrix tab_mp = r(table)
di "MARGINS_P1_B="  %22.15g tab_mp[1,1]
di "MARGINS_P1_SE=" %22.15g tab_mp[2,1]
di "MARGINS_P1_LL=" %22.15g tab_mp[5,1]
di "MARGINS_P1_UL=" %22.15g tab_mp[6,1]
di "MARGINS_P2_B="  %22.15g tab_mp[1,2]
di "MARGINS_P2_SE=" %22.15g tab_mp[2,2]
di "MARGINS_P2_LL=" %22.15g tab_mp[5,2]
di "MARGINS_P2_UL=" %22.15g tab_mp[6,2]

di "CI_ORACLE_CAPTURE_DONE_MARKER"
log close
