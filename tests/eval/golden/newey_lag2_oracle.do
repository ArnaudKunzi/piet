* newey_lag2 oracle re-derivation (BUG-227 tranche 1, WS-X 2026-08-07).
* neweyprais.csv's nw2:* keys were self-captured from cobra (prais:* keys belong
* to case prais_iter, not in the self-captured class — left untouched).
* Spec (tests/eval/manifest.json case newey_lag2):
*   cobra newey --data {neweyprais} --y price --x weight,length --timevar t --lag 2
* Stata equivalent: tsset t; newey price weight length, lag(2)
* Data transport: tests/validate/neweyprais.hbin -> neweyprais_export.dta via
*   `cobra export-dta` (round-trip-gated transport).
* Run:  StataBE -b do newey_lag2_oracle.do -> newey_lag2_oracle.log
clear all
set more off
set type double
capture log close
log using "newey_lag2_oracle.log", replace text
display "stata_version=" c(stata_version) " born=" c(born_date)
use "../../validate/neweyprais_export.dta", clear
tsset t
newey price weight length, lag(2)
display "NEWEY_LAG2_ORACLE_BEGIN"
display "ORACLE|neweyprais.csv|nw2:coeftable[weight].b|" %21.15e _b[weight]
display "ORACLE|neweyprais.csv|nw2:coeftable[weight].se|" %21.15e _se[weight]
display "ORACLE|neweyprais.csv|nw2:coeftable[length].b|" %21.15e _b[length]
display "ORACLE|neweyprais.csv|nw2:coeftable[length].se|" %21.15e _se[length]
display "ORACLE|neweyprais.csv|nw2:coeftable[(intercept)].b|" %21.15e _b[_cons]
display "ORACLE|neweyprais.csv|nw2:coeftable[(intercept)].se|" %21.15e _se[_cons]
display "NEWEY_LAG2_ORACLE_END"
log close
