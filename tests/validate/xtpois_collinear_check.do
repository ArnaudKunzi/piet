* Run from tests/validate: every path below is relative to it.
clear all
set more off
set type double
capture log close
log using "xtpois_collinear_check.log", replace text
use "csdid_input.dta", clear
xtset h_pid wv
* female is time-invariant within person -> Stata should omit it.
xtpoisson visits_w female age_z, fe
di "AGE_Z_B=" %20.15g _b[age_z]
di "AGE_Z_SE=" %20.15g _se[age_z]
log close
