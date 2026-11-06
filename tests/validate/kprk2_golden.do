* Run from tests/validate: every path below is relative to it.
* Multi-endogenous (p=2) weak-IV F golden: ivreg2 e(widstat) = Cragg-Donald F
* (homoskedastic) and Kleibergen-Paap rk Wald F (robust/cluster). 2 endogenous
* (age_z edu_hi), 3 excluded instruments (female gvar treated). One exog (wv).
clear all
set more off
set type double
use "csdid_input.dta", clear
gen byte treated = (gvar>0 & wv>=gvar) if !missing(gvar)
recast double treated
keep visits_w age_z edu_hi female gvar treated wv
keep if !missing(visits_w, age_z, edu_hi, female, gvar, treated, wv)
save "kprk2_input.dta", replace

file open o using "kprk2_golden.csv", write replace
file write o "stat,value" _n

* homoskedastic -> Cragg-Donald
ivreg2 visits_w wv (age_z edu_hi = female gvar treated)
file write o "cd_widstat," (string(e(widstat),"%21.15e")) _n
file write o "N," (string(e(N),"%21.15e")) _n

* robust -> Kleibergen-Paap rk Wald F
ivreg2 visits_w wv (age_z edu_hi = female gvar treated), robust
file write o "kp_widstat," (string(e(widstat),"%21.15e")) _n

* cluster(gvar) -> KP rk Wald F clustered
ivreg2 visits_w wv (age_z edu_hi = female gvar treated), cluster(gvar)
file write o "kpc_widstat," (string(e(widstat),"%21.15e")) _n
file write o "kpc_Nclust," (string(e(N_clust),"%21.15e")) _n
file close o
