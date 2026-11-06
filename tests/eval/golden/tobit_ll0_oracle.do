* tobit_ll0 oracle re-derivation (BUG-227 tranche 1, WS-X 2026-08-07).
* The tobit.csv golden was self-captured from cobra (BUG-227); this captures the
* same spec from real Stata so the golden carries external authority.
* Spec (tests/eval/manifest.json case tobit_ll0):
*   cobra tobit --data {tobit} --y visits_w --x age_z,edu_hi,treated --ll 0
* Data transport: tests/validate/tobit.hbin -> tobit_export.dta via
*   `cobra export-dta tests/validate/tobit.hbin tests/validate/tobit_export.dta`
* (data transport is pinned by the .dta round-trip gates; the EXPECTED VALUES
*  are what lacked authority, not the fixture).
* Mapping note: modern Stata tobit reports the ancillary as var(e.visits_w);
* cobra reports sigma. sigma = sqrt(var) is the same MLE parameter (delta-map).
* Run:  StataBE -b do tobit_ll0_oracle.do   -> tobit_ll0_oracle.log
clear all
set more off
set type double
capture log close
log using "tobit_ll0_oracle.log", replace text
display "stata_version=" c(stata_version) " born=" c(born_date)
use "../../validate/tobit_export.dta", clear
tobit visits_w age_z edu_hi treated, ll(0)
matrix b = e(b)
matrix V = e(V)
local k = colsof(b)
local cn : colfullnames b
local lastnm : word `k' of `cn'
display "last ancillary column = `lastnm'"
if strpos("`lastnm'", "var(") {
    local sigma = sqrt(b[1,`k'])
}
else {
    local sigma = b[1,`k']
}
display "TOBIT_LL0_ORACLE_BEGIN"
display "ORACLE|tobit.csv|coeftable[age_z].b|" %21.15e _b[age_z]
display "ORACLE|tobit.csv|coeftable[age_z].se|" %21.15e _se[age_z]
display "ORACLE|tobit.csv|coeftable[edu_hi].b|" %21.15e _b[edu_hi]
display "ORACLE|tobit.csv|coeftable[edu_hi].se|" %21.15e _se[edu_hi]
display "ORACLE|tobit.csv|coeftable[treated].b|" %21.15e _b[treated]
display "ORACLE|tobit.csv|coeftable[treated].se|" %21.15e _se[treated]
display "ORACLE|tobit.csv|coeftable[(intercept)].b|" %21.15e _b[_cons]
display "ORACLE|tobit.csv|coeftable[(intercept)].se|" %21.15e _se[_cons]
display "ORACLE|tobit.csv|sigma|" %21.15e `sigma'
display "ORACLE|tobit.csv|n_left_censored|" %21.15e e(N_lc)
display "ORACLE|tobit.csv|ll|" %21.15e e(ll)
display "TOBIT_LL0_ORACLE_END"
log close
