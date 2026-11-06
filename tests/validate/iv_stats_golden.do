* Run from tests/validate: every path below is relative to it.
clear all
set more off
set type double
capture log close
use "csdid_input.dta", clear
gen byte treated = (gvar>0 & wv>=gvar) if !missing(gvar)
recast double treated
ivreghdfe visits_w treated (age_z = female edu_hi), absorb(gvar) cluster(gvar)
file open o using "iv_stats_golden.csv", write replace
file write o "key,value" _n
file write o "b_age_z,`=_b[age_z]'" _n
file write o "se_age_z,`=_se[age_z]'" _n
file write o "widstat,`=e(widstat)'" _n
file write o "rkf,`=e(rkf)'" _n
file write o "j,`=e(j)'" _n
file write o "jdf,`=e(jdf)'" _n
file write o "jp,`=e(jp)'" _n
file write o "idstat,`=e(idstat)'" _n
file write o "iddf,`=e(iddf)'" _n
file write o "idp,`=e(idp)'" _n
file write o "N,`=e(N)'" _n
file write o "N_clust,`=e(N_clust)'" _n
file write o "sdofminus,`=e(sdofminus)'" _n
file write o "dofminus,`=e(dofminus)'" _n
file close o
