* Run from tests/validate: every path below is relative to it.
* ppmlhdfe golden: FE-Poisson with all-zero-FE-cell separation drop (default).
* Use h_pid FE so persons with all y_int==0 get dropped (the "fe" technique).
clear all
set more off
set type double
capture log close
log using "fepois_sep_golden.log", replace text
use "csdid_input.dta", clear
gen byte treated = (gvar>0 & wv>=gvar) if !missing(gvar)
recast double treated
gen long y_int = round(visits_w)
drop if missing(y_int)
cap which ppmlhdfe
if _rc {
    di "PPMLHDFE_NOT_INSTALLED"
}
else {
    ppmlhdfe y_int treated age_z, absorb(h_pid)
    di "PP_N=" e(N) " B_treated=" %20.15g _b[treated] " B_age_z=" %20.15g _b[age_z]
}
log close
