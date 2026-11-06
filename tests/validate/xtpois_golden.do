* Run from tests/validate: every path below is relative to it.
* xtpoisson, fe golden: run Stata's conditional FE Poisson on a synthetic-ish
* panel and dump coefficients + robust SEs. We reuse the 건강심리 csdid input
* (which is already a real panel with a count-like dependent variable).
clear all
set more off
set type double
capture log close
log using "xtpois_golden.log", replace text

use "csdid_input.dta", clear
xtset h_pid wv

* Build a 0/1 covariate from gvar so we have a treatment-like variable.
gen byte treated = (gvar>0 & wv>=gvar) if !missing(gvar)
recast double treated

* Round the outcome to integer counts (xtpoisson requires non-negative integers).
gen long y_int = round(visits_w)
qui replace y_int = . if missing(visits_w)

* Default: maximum-likelihood iid SE.
xtpoisson y_int treated female age_z edu_hi, fe
matrix b_iid  = e(b)
matrix V_iid  = e(V)
scalar ll_iid = e(ll)
scalar Ng_iid = e(N_g)
scalar N_iid  = e(N)
scalar k_iid  = e(k)

* Robust (Wooldridge 1999) — cluster-robust by panel.
xtpoisson y_int treated female age_z edu_hi, fe vce(robust)
matrix b_rob  = e(b)
matrix V_rob  = e(V)
scalar ll_rob = e(ll)

file open out using "xtpois_golden.csv", write replace
file write out "name,coef_iid,se_iid,coef_rob,se_rob" _n
local nms : colnames b_iid
local k = colsof(b_iid)
forvalues j = 1/`k' {
    local nm : word `j' of `nms'
    file write out "`nm',`=b_iid[1,`j']',`=sqrt(V_iid[`j',`j'])',`=b_rob[1,`j']',`=sqrt(V_rob[`j',`j'])'" _n
}
file close out

di "LL_IID=" %20.15g ll_iid
di "LL_ROB=" %20.15g ll_rob
di "NGROUPS=" Ng_iid " N=" N_iid " K=" k_iid

log close
