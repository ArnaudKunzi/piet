* Run from tests/validate: every path below is relative to it.
* Post-estimation goldens: Hausman, margins (poisson, logit). Same input data.
clear all
set more off
set type double
capture log close
log using "posthest_golden.log", replace text

use "csdid_input.dta", clear
xtset h_pid wv
gen byte treated = (gvar>0 & wv>=gvar) if !missing(gvar)
recast double treated
gen long y_int = round(visits_w)
drop if missing(y_int)

* Hausman: FE vs RE on visits_w.
quietly xtreg visits_w treated age_z edu_hi, fe
estimates store fe
quietly xtreg visits_w treated age_z edu_hi, re
estimates store re
hausman fe re, sigmamore
scalar haus_chi2 = r(chi2)
scalar haus_df   = r(df)
scalar haus_p    = r(p)
di "HAUSMAN_CHI2=" %22.15g haus_chi2 " DF=" haus_df " P=" %22.15g haus_p

* Margins for Poisson (AME): use plain poisson (not xtpois fe — margins works
* simpler with pooled poisson).
poisson y_int treated age_z edu_hi
matrix b_p = e(b)
matrix V_p = e(V)
margins, dydx(*) post
matrix m_p = e(b)
matrix V_pmar = e(V)
file open op using "posthest_pois_margins.csv", write replace
file write op "name,dydx,se" _n
local nms : colnames m_p
local k = colsof(m_p)
forvalues j = 1/`k' {
    local nm : word `j' of `nms'
    file write op "`nm',`=m_p[1,`j']',`=sqrt(V_pmar[`j',`j'])'" _n
}
file close op

* Also dump the underlying Poisson coefs for cecon's input.
file open op2 using "posthest_pois_coefs.csv", write replace
file write op2 "name,beta,se" _n
local nms2 : colnames b_p
local k2 = colsof(b_p)
forvalues j = 1/`k2' {
    local nm : word `j' of `nms2'
    file write op2 "`nm',`=b_p[1,`j']',`=sqrt(V_p[`j',`j'])'" _n
}
file close op2

* Logit margins.
gen byte y01 = (visits_w > 0) if !missing(visits_w)
logit y01 treated age_z edu_hi
matrix b_l = e(b)
matrix V_l = e(V)
margins, dydx(*) post
matrix m_l = e(b)
matrix V_lmar = e(V)
file open ol using "posthest_logit_margins.csv", write replace
file write ol "name,dydx,se" _n
local nms3 : colnames m_l
local k3 = colsof(m_l)
forvalues j = 1/`k3' {
    local nm : word `j' of `nms3'
    file write ol "`nm',`=m_l[1,`j']',`=sqrt(V_lmar[`j',`j'])'" _n
}
file close ol

file open ol2 using "posthest_logit_coefs.csv", write replace
file write ol2 "name,beta,se" _n
local nms4 : colnames b_l
local k4 = colsof(b_l)
forvalues j = 1/`k4' {
    local nm : word `j' of `nms4'
    file write ol2 "`nm',`=b_l[1,`j']',`=sqrt(V_l[`j',`j'])'" _n
}
file close ol2

log close
