* Run from tests/validate: every path below is relative to it.
* IV diagnostics golden for cecon 2SLS/IV: Hansen J e(j), Kleibergen-Paap rk LM
* underid e(idstat), and Montiel-Olea-Pflueger effective F (weakivtest r(F_eff)).
* Spec: visits_w treated (age_z = female edu_hi) on the KOWEPS csdid_input data,
* under three vcov settings. cecon must reproduce these to ~1e-12.
* Generates iv_hansenj_golden.csv, consumed by tests/unit/test_iv_hansenj.cpp.
* Requires ssc packages: ivreg2, ivreghdfe, ranktest, weakivtest, avar, distinct.
clear all
set more off
set type double
use "csdid_input.dta", clear
gen byte treated = (gvar>0 & wv>=gvar) if !missing(gvar)
recast double treated

file open o using "iv_hansenj_golden.csv", write replace
file write o "case,j,jdf,jp,N,N_clust,idstat,iddf,idp,Feff" _n

* weakivtest runs internal regressions that clear e(); capture e() into locals first.
* (1) no FE, robust (HC) -> Vcov::HC1
ivreg2 visits_w treated (age_z = female edu_hi), robust
local j=e(j)
local jdf=e(jdf)
local jp=chi2tail(e(jdf),e(j))
local N=e(N)
local idstat=e(idstat)
local iddf=e(iddf)
local idp=e(idp)
weakivtest
file write o "nofe_robust,`j',`jdf',`jp',`N',.,`idstat',`iddf',`idp',`=r(F_eff)'" _n

* (2) no FE, cluster(gvar) -> Vcov::Cluster
ivreg2 visits_w treated (age_z = female edu_hi), cluster(gvar)
local j=e(j)
local jdf=e(jdf)
local jp=chi2tail(e(jdf),e(j))
local N=e(N)
local Nc=e(N_clust)
local idstat=e(idstat)
local iddf=e(iddf)
local idp=e(idp)
weakivtest
file write o "nofe_cluster,`j',`jdf',`jp',`N',`Nc',`idstat',`iddf',`idp',`=r(F_eff)'" _n

* (3) absorb(gvar) + cluster(gvar) -> FE + Vcov::Cluster
ivreghdfe visits_w treated (age_z = female edu_hi), absorb(gvar) cluster(gvar)
local j=e(j)
local jdf=e(jdf)
local jp=chi2tail(e(jdf),e(j))
local N=e(N)
local Nc=e(N_clust)
local idstat=e(idstat)
local iddf=e(iddf)
local idp=e(idp)
weakivtest
file write o "fe_cluster,`j',`jdf',`jp',`N',`Nc',`idstat',`iddf',`idp',`=r(F_eff)'" _n

file close o
