* Run from tests/validate: every path below is relative to it.
* xtlogit, fe golden (Chamberlain conditional logit).
clear all
set more off
set type double
capture log close
log using "xtlogit_fe_golden.log", replace text

use "csdid_input.dta", clear
xtset h_pid wv
gen byte treated = (gvar>0 & wv>=gvar) if !missing(gvar)
recast double treated

* Build a binary outcome from visits_w (1 if any visits, 0 otherwise)
gen byte visits_any = (visits_w>0) if !missing(visits_w)

xtlogit visits_any treated age_z edu_hi, fe
matrix b   = e(b)
matrix V   = e(V)
scalar ll  = e(ll)
scalar Ng  = e(N_g)
scalar N   = e(N)
scalar k   = colsof(b)

file open out using "xtlogit_fe_golden.csv", write replace
file write out "name,coef_iid,se_iid" _n
local nms : colnames b
forvalues j = 1/`=k' {
    local nm : word `j' of `nms'
    file write out "`nm',`=b[1,`j']',`=sqrt(V[`j',`j'])'" _n
}
file close out
di "LL=" %22.15g ll
di "N=" N " Ng=" Ng " k=" k
log close
