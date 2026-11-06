* Run from tests/validate: every path below is relative to it.
* xtreg, re golden (Swamy-Arora variance components). Reuses csdid_input.dta.
clear all
set more off
set type double
capture log close
log using "xtreg_re_golden.log", replace text

use "csdid_input.dta", clear
xtset h_pid wv
gen byte treated = (gvar>0 & wv>=gvar) if !missing(gvar)
recast double treated

* Stata default: Swamy-Arora type 1 ("sigma_u^2_T"), not the SA-unbalanced one.
xtreg visits_w treated female age_z edu_hi, re
matrix b   = e(b)
matrix V   = e(V)
scalar sig_e = e(sigma_e)
scalar sig_u = e(sigma_u)
scalar rho   = e(rho)
scalar N     = e(N)
scalar Ng    = e(N_g)
scalar k     = colsof(b)
scalar Tbar_h= e(Tbar)

file open out using "xtreg_re_golden.csv", write replace
file write out "name,coef,se" _n
local nms : colnames b
forvalues j = 1/`=k' {
    local nm : word `j' of `nms'
    file write out "`nm',`=b[1,`j']',`=sqrt(V[`j',`j'])'" _n
}
file close out
di "SIG_E="     %22.15g sig_e
di "SIG_U="     %22.15g sig_u
di "RHO="       %22.15g rho
di "TBAR_H="    %22.15g Tbar_h
di "N=" N " Ng=" Ng " k=" k

log close
