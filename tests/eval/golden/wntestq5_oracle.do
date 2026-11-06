* wntestq_5 oracle re-derivation (BUG-227 tranche 1, WS-X 2026-08-07).
* ts_diag.csv's wq:* keys were self-captured from cobra (dw:/bg:/arch: keys
* belong to cases dwatson/bgodfrey_12/archlm_123, not in the self-captured
* class — left untouched).
* Spec (tests/eval/manifest.json case wntestq_5):
*   cobra wntestq --data {ts_diag} --var y --lags 5
* Stata equivalent: tsset t; wntestq y, lags(5)  -> r(stat) Portmanteau Q, r(p).
* Data transport: tests/validate/ts_diag.hbin -> ts_diag_export.dta via
*   `cobra export-dta` (round-trip-gated transport).
* Run:  StataBE -b do wntestq5_oracle.do -> wntestq5_oracle.log
clear all
set more off
set type double
capture log close
log using "wntestq5_oracle.log", replace text
display "stata_version=" c(stata_version) " born=" c(born_date)
use "../../validate/ts_diag_export.dta", clear
tsset t
wntestq y, lags(5)
display "WNTESTQ5_ORACLE_BEGIN"
display "ORACLE|ts_diag.csv|wq:Q|" %21.15e r(stat)
display "ORACLEX|ts_diag.csv|wq:Q|" %21x r(stat)
display "ORACLE|ts_diag.csv|wq:df|" %21.15e r(df)
display "ORACLEX|ts_diag.csv|wq:df|" %21x r(df)
display "ORACLE|ts_diag.csv|wq:p|" %21.15e r(p)
display "ORACLEX|ts_diag.csv|wq:p|" %21x r(p)
display "WNTESTQ5_ORACLE_END"
return list
log close
