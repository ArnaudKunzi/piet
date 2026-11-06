* ============================================================================
* csdid_kitchensink_oracle.do — BUG-227 tranche 2, case csdid_kitchensink_940.
*
* WHY: csdid_kitchensink.csv was captured from cobra itself. Its manifest note
* says every cell was "cross-verified against the StataBE 19.5 PRINTED grid
* (internal capture debug_all.log L1070-1098) at generation" — i.e. eyeballed
* against a rounded print, never transplanted at full precision. So the golden
* cannot adjudicate a numerics change in either direction (BUG-227 class).
* This capture makes the golden Stata-sourced.
*
* Spec (tests/eval/manifest.json case csdid_kitchensink_940):
*   cobra csdid --data {kitchensink_panel} --y y --x x1,x2 --ivar id
*               --time year --gvar cohort --method dripw
* Stata equivalent (csdid package, Callaway-Sant'Anna, never-treated control):
*   csdid y x1 x2, ivar(id) time(year) gvar(cohort) method(dripw)
*
* Data: internal fixture kitchensink_panel.dta (committed; source of the
* engine fixture tests/validate/kitchensink_panel.hbin via tools/to_hbin.py).
* NO export-dta transport is needed for this case — the .dta IS the source.
*
* Key mapping: cobra cell[gG,tT] = ATT(g=G, t=T) with CALENDAR labels
* (g2016..g2020, t2015..t2021). Stata equation name g<G>, coefficient name's
* LAST time index = T (post rows t_<g-1>_<T>, pre rows t_<T-1>_<T>) — parsed
* from colfullnames below, exactly as csdid_dripw_oracle.do:35 does.
*
* CONTROL: the DIAG_ block below prints the data shape and e(N). e(N) must be
* 940 (the golden's n_obs, rtol 0). If it is not, this .do is not running the
* model the golden pins and NOTHING from this log may be transplanted.
*
* Run:  cd tests/eval/golden && StataBE -b do csdid_kitchensink_oracle.do
*       -> csdid_kitchensink_oracle.log
* Consume: python3 tests/eval/rederive_golden.py <that log> [--dry-run]
* ============================================================================
clear all
set more off
set type double
capture log close
log using "csdid_kitchensink_oracle.log", replace text
display "stata_version=" c(stata_version) " born=" c(born_date)

use "../../kitchensink/kitchensink_panel.dta", clear

display "DIAG_BEGIN"
quietly count
display "DIAG_N_ROWS=" r(N)
quietly levelsof cohort, local(gvals)
display "DIAG_COHORTS=`gvals'"
quietly levelsof year, local(tvals)
display "DIAG_YEARS=`tvals'"
quietly distinct id
display "DIAG_N_UNITS=" r(ndistinct)
display "DIAG_END"

xtset id year

csdid y x1 x2, ivar(id) time(year) gvar(cohort) method(dripw)
display "CSDID_E_N=" e(N)

* ---- ATT(g,t) cells ----
matrix b = e(b)
matrix V = e(V)
local k = colsof(b)
local fullnames : colfullnames b
display "ATTGT_K=" `k'
display "CSDID_KITCHENSINK_ORACLE_BEGIN"
forvalues j = 1/`k' {
    local fn : word `j' of `fullnames'
    if regexm("`fn'", "^g([0-9]+):.*_([0-9]+)$") {
        local g = regexs(1)
        local t = regexs(2)
        display "ORACLE|csdid_kitchensink.csv|cell[g`g',t`t'].att|" %21.15e b[1,`j']
        display "ORACLE|csdid_kitchensink.csv|cell[g`g',t`t'].se|" %21.15e sqrt(V[`j',`j'])
    }
    else {
        display "UNPARSED colname: `fn'"
    }
}

* ---- estat simple: simple_att / simple_se ----
estat simple
matrix Rs = r(table)
display "ORACLE|csdid_kitchensink.csv|simple_att|" %21.15e Rs[1,1]
display "ORACLE|csdid_kitchensink.csv|simple_se|" %21.15e Rs[2,1]

* ---- estat event: Pre_avg / Post_avg (+ SEs), matched BY NAME, never by
*      position — the event table's row order is not a contract.  Same shape
*      as the committed csdid_agg_scalars.do capture for csdid_cluster_event. ----
estat event
matrix Re = r(table)
local enames : colnames Re
local ek = colsof(Re)
display "EVENT_AGG_BEGIN k=" `ek'
forvalues j = 1/`ek' {
    local nm : word `j' of `enames'
    display "DIAG_EVENT|`nm'|" %21.15e Re[1,`j'] "|" %21.15e Re[2,`j']
    if "`nm'" == "Pre_avg" {
        display "ORACLE|csdid_kitchensink.csv|pre_avg|" %21.15e Re[1,`j']
        display "ORACLE|csdid_kitchensink.csv|pre_avg_se|" %21.15e Re[2,`j']
    }
    if "`nm'" == "Post_avg" {
        display "ORACLE|csdid_kitchensink.csv|post_avg|" %21.15e Re[1,`j']
        display "ORACLE|csdid_kitchensink.csv|post_avg_se|" %21.15e Re[2,`j']
    }
}
display "EVENT_AGG_END"
display "CSDID_KITCHENSINK_ORACLE_END"
log close
exit, clear STATA
