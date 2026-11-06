* Run from tests/validate: every path below is relative to it.
* nbreg golden (NB2 MLE).
clear all
set more off
set type double
capture log close
log using "nbreg_golden.log", replace text

use "csdid_input.dta", clear
gen byte treated = (gvar>0 & wv>=gvar) if !missing(gvar)
recast double treated
gen long y_int = round(visits_w)
drop if missing(y_int)

* nbreg iid SE
nbreg y_int treated female age_z edu_hi
matrix b   = e(b)
matrix V   = e(V)
scalar ll_iid = e(ll)
scalar alpha_iid = e(alpha)
scalar N = e(N)
scalar k = colsof(b)

* nbreg robust SE
nbreg y_int treated female age_z edu_hi, vce(robust)
matrix b_r = e(b)
matrix V_r = e(V)

file open out using "nbreg_golden.csv", write replace
file write out "name,coef_iid,se_iid,coef_rob,se_rob" _n
local nms : colnames b
forvalues j = 1/`=k' {
    local nm : word `j' of `nms'
    file write out "`nm',`=b[1,`j']',`=sqrt(V[`j',`j'])',`=b_r[1,`j']',`=sqrt(V_r[`j',`j'])'" _n
}
file close out
di "LL=" %22.15g ll_iid
di "ALPHA=" %22.15g alpha_iid
di "LNALPHA=" %22.15g ln(alpha_iid)
di "N=" N " k=" k
log close
