* ============================================================================
* reghdfe_absorb_vce_oracle.do — BUG-227 tranche 3 (ONESHOT-ZERO WS-Q).
*
* WHY: reghdfe_absorb_vce_B.csv was the ONLY golden among the 13 the
* self-captured ratchet flags whose exact-zero cells had no machine-checkable
* provenance. It was never self-captured — it is a %21.15e transcription of
* reghdfe_absorb_vce.log's GOLDEN_B block, verified verbatim — but a
* transcription is a human step, and a human step is exactly what the
* ORACLE| corpus exists to remove. This re-emits the same B block in the
* ORACLE|/ORACLEX| convention rederive_golden.py and gen_goldens.py both read,
* so the case's authority is checkable by a program instead of by a reader.
*
* Spec (manifest case reghdfe_absorb_vce_twoway):
*   cobra feols --data {panel_synth} --y y --x x1,x2 --fe id,t
*               --vcov cluster --cluster id
* Stata equivalent: reghdfe y x1 x2, absorb(id t) vce(cluster id)
*
* Data + run instructions: identical to reghdfe_absorb_vce.do (same directory,
* same committed panel_synth_full.csv). Requires StataNow BE 19.5 + reghdfe.
* ============================================================================
clear all
set more off
set type double
capture log close
log using "reghdfe_absorb_vce_oracle.log", replace text
display "stata_version=" c(stata_version) " flavor=" c(flavor)
display "REGHDFE_ORACLE_BEGIN"

import delimited using "`c(pwd)'/panel_synth_full.csv", clear case(preserve) asdouble
xtset id t

reghdfe y x1 x2, absorb(id t) vce(cluster id)
foreach v in x1 x2 {
    display "ORACLE|reghdfe_absorb_vce_B.csv|coeftable[`v'].b|"  %21.15e _b[`v']
    display "ORACLEX|reghdfe_absorb_vce_B.csv|coeftable[`v'].b|" %21x   _b[`v']
    display "ORACLE|reghdfe_absorb_vce_B.csv|coeftable[`v'].se|"  %21.15e _se[`v']
    display "ORACLEX|reghdfe_absorb_vce_B.csv|coeftable[`v'].se|" %21x   _se[`v']
}
display "ORACLE|reghdfe_absorb_vce_B.csv|coeftable[_cons].b|"  %21.15e _b[_cons]
display "ORACLEX|reghdfe_absorb_vce_B.csv|coeftable[_cons].b|" %21x   _b[_cons]
display "ORACLE|reghdfe_absorb_vce_B.csv|coeftable[_cons].se|"  %21.15e _se[_cons]
display "ORACLEX|reghdfe_absorb_vce_B.csv|coeftable[_cons].se|" %21x   _se[_cons]
display "ORACLE|reghdfe_absorb_vce_B.csv|r2_within|"  %21.15e e(r2_within)
display "ORACLEX|reghdfe_absorb_vce_B.csv|r2_within|" %21x   e(r2_within)
display "ORACLE|reghdfe_absorb_vce_B.csv|rmse|"  %21.15e e(rmse)
display "ORACLEX|reghdfe_absorb_vce_B.csv|rmse|" %21x   e(rmse)
display "ORACLE|reghdfe_absorb_vce_B.csv|F|"  %21.15e e(F)
display "ORACLEX|reghdfe_absorb_vce_B.csv|F|" %21x   e(F)

* --- BUG-296 rider: the %ty family, whose first capture was not self-consistent.
display "TY|BEGIN"
foreach v in 0 1 5 43 99 100 245 1959 1960 2020 12345 -3 42.5 43.5 {
    display "TY|`v'|A|[" %ty (`v') "]"
    display "TY|`v'|B|[" string((`v'),"%ty") "]|len=" length(string((`v'),"%ty"))
}
display "TY|END"
display "REGHDFE_ORACLE_END"
log close
