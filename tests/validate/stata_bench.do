* Run from tests/validate: every path below is relative to it.
* Time each Stata estimator that cecon replicates. One invocation = one startup.
clear all
set more off
set type double
capture log close
log using "stata_bench.log", replace text

* --- Load + prep (matches what cecon's tests use) ---
timer clear
timer on 1
use "csdid_input.dta", clear
xtset h_pid wv
gen byte treated = (gvar>0 & wv>=gvar) if !missing(gvar)
recast double treated
gen long y_int = round(visits_w)
gen byte y01 = (visits_w > 0) if !missing(visits_w)
timer off 1

* --- xtreg, re ---
timer on 2
quietly xtreg visits_w treated female age_z edu_hi, re
timer off 2

* --- xtpoisson, fe (iid) ---
preserve
drop if missing(y_int)
timer on 3
quietly xtpoisson y_int treated female age_z edu_hi, fe
timer off 3

* --- xtpoisson, fe vce(robust) ---
timer on 4
quietly xtpoisson y_int treated female age_z edu_hi, fe vce(robust)
timer off 4

* --- nbreg (iid) ---
timer on 5
quietly nbreg y_int treated female age_z edu_hi
timer off 5

* --- nbreg vce(robust) ---
timer on 6
quietly nbreg y_int treated female age_z edu_hi, vce(robust)
timer off 6

* --- poisson (pooled) + margins ---
timer on 7
quietly poisson y_int treated age_z edu_hi
quietly margins, dydx(*)
timer off 7
restore

* --- xtlogit, fe ---
preserve
drop if missing(y01)
timer on 8
quietly xtlogit y01 treated age_z edu_hi, fe
timer off 8

* --- logit (pooled) + margins ---
timer on 9
quietly logit y01 treated age_z edu_hi
quietly margins, dydx(*)
timer off 9
restore

* --- csdid (the headline) ---
quietly summarize visits, detail
gen visits_w_top = visits_w
quietly summarize age
gen age_z2 = (age - r(mean))/r(sd)
local COV female age_z edu_hi
timer on 10
quietly csdid visits_w `COV', ivar(h_pid) time(wv) gvar(gvar) method(dripw)
quietly estat simple
quietly estat event, window(-5 5)
timer off 10

timer list
log close
