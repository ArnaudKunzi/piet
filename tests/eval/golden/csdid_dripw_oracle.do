* csdid_dripw oracle re-derivation (BUG-227 tranche 1, WS-X 2026-08-07).
* csdid.csv was self-captured from cobra; the generation-time Stata cross-check
* (tests/validate/csdid_attgt.csv, written by csdid_golden.do) was read into
* gen_csdid() and NEVER referenced again (BUG-201 — the tolnotes on this case
* already say so). This capture makes the golden itself Stata-sourced.
* Spec (tests/eval/manifest.json case csdid_dripw):
*   cobra csdid --data {csdid_input} --y visits_w --ivar h_pid --time wv
*               --gvar gvar --x female,age_z,edu_hi        (method default dripw)
* Stata equivalent (csdid package, Callaway-Sant'Anna, never-treated control):
*   csdid visits_w female age_z edu_hi, ivar(h_pid) time(wv) gvar(gvar) method(dripw)
* Key mapping: cobra cell[g<G>,t<T>] = ATT(g=G, t=T); Stata equation g<G>,
* coefficient name's LAST time index = T (post rows t_<g-1>_<T>, pre rows
* t_<T-1>_<T>) — parsed from colfullnames below.
* Data transport: tests/validate/csdid_input.hbin -> csdid_input_export.dta via
*   `cobra export-dta` (round-trip-gated transport).
* Run:  StataBE -b do csdid_dripw_oracle.do -> csdid_dripw_oracle.log
clear all
set more off
set type double
capture log close
log using "csdid_dripw_oracle.log", replace text
display "stata_version=" c(stata_version) " born=" c(born_date)
use "../../validate/csdid_input_export.dta", clear
xtset h_pid wv

csdid visits_w female age_z edu_hi, ivar(h_pid) time(wv) gvar(gvar) method(dripw)
matrix b = e(b)
matrix V = e(V)
local k = colsof(b)
local fullnames : colfullnames b
display "ATTGT_K=" `k'
display "CSDID_DRIPW_ORACLE_BEGIN"
forvalues j = 1/`k' {
    local fn : word `j' of `fullnames'
    if regexm("`fn'", "^g([0-9]+):.*_([0-9]+)$") {
        local g = regexs(1)
        local t = regexs(2)
        display "ORACLE|csdid.csv|cell[g`g',t`t'].att|" %21.15e b[1,`j']
        display "ORACLE|csdid.csv|cell[g`g',t`t'].se|" %21.15e sqrt(V[`j',`j'])
    }
    else {
        display "UNPARSED colname: `fn'"
    }
}
display "CSDID_DRIPW_ORACLE_END"
log close
