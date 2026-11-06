* Run from tests/validate: every path below is relative to it.
* iv-diagnostics golden: estat endogenous (Durbin/Wu-Hausman) + estat overid
* (Sargan/Basmann) + estat firststage after ivregress 2sls (iid). Model:
* visits_w treated (age_z = female edu_hi) on KOWEPS csdid_input (overid df = 1).
clear all
set more off
set type double
use "csdid_input.dta", clear
gen byte treated = (gvar>0 & wv>=gvar) if !missing(gvar)
recast double treated
keep visits_w treated age_z female edu_hi
keep if !missing(visits_w, treated, age_z, female, edu_hi)
save "ivdiag_input.dta", replace

ivregress 2sls visits_w treated (age_z = female edu_hi)
local N = e(N)

estat endogenous
local durbin   = r(durbin)
local durbin_p = r(p_durbin)
local wu       = r(wu)
local wu_p     = r(p_wu)
local wu_dfr   = r(wudf_r)
local endog_df = r(df)

estat overid
local sargan   = r(sargan)
local sargan_p = r(p_sargan)
local basmann  = r(basmann)
local basmann_p= r(p_basmann)
local overid_df= r(df)

estat firststage
matrix FS = r(singleresults)
* singleresults columns: 1=fs F? capture the whole row for inspection in log.
matrix list FS

file open o using "ivdiag_golden.csv", write replace
file write o "stat,value" _n
file write o "N,"         (string(`N',"%21.15e"))         _n
file write o "durbin,"    (string(`durbin',"%21.15e"))    _n
file write o "durbin_p,"  (string(`durbin_p',"%21.15e"))  _n
file write o "wu,"        (string(`wu',"%21.15e"))        _n
file write o "wu_p,"      (string(`wu_p',"%21.15e"))      _n
file write o "wu_dfr,"    (string(`wu_dfr',"%21.15e"))    _n
file write o "endog_df,"  (string(`endog_df',"%21.15e"))  _n
file write o "sargan,"    (string(`sargan',"%21.15e"))    _n
file write o "sargan_p,"  (string(`sargan_p',"%21.15e"))  _n
file write o "basmann,"   (string(`basmann',"%21.15e"))   _n
file write o "basmann_p," (string(`basmann_p',"%21.15e")) _n
file write o "overid_df," (string(`overid_df',"%21.15e")) _n
file write o "fs_F,"      (string(FS[1,4],"%21.15e"))     _n
file write o "fs_r2,"     (string(FS[1,1],"%21.15e"))     _n
file write o "fs_par_r2," (string(FS[1,3],"%21.15e"))     _n
file close o
