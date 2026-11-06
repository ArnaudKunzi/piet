* External research data, not shipped: set global HEALTH_PANEL_DIR to its directory first.
* Run from tests/validate: every path below is relative to it.
* csdid golden: run Stata's Callaway-Sant'Anna (method dripw, never-treated) on the
* real 건강심리 panel and dump ATT(g,t) + estat simple + estat event. Also export the
* exact constructed inputs so cecon estimates on identical data.
clear all
set more off
set type double
capture log close
log using "csdid_golden.log", replace text

use "$HEALTH_PANEL_DIR/STATA/analysis_panel.dta", clear
xtset h_pid wv
recast double h_pid wv gvar visits age female h_g6

* inputs (match _prep_common); double precision
quietly summarize visits, detail
gen visits_w = visits
replace visits_w = r(p99) if visits>r(p99) & !missing(visits)
quietly summarize age
gen age_z = (age - r(mean))/r(sd)
gen edu_hi = (h_g6>=4) if !missing(h_g6)
recast double visits_w age_z edu_hi

local COV female age_z edu_hi
csdid visits_w `COV', ivar(h_pid) time(wv) gvar(gvar) method(dripw)

* dump ATT(g,t)
matrix b = e(b)
matrix V = e(V)
local names : colnames b
local k = colsof(b)
file open at using "csdid_attgt.csv", write replace
file write at "name,att,se" _n
forvalues j = 1/`k' {
    local nm : word `j' of `names'
    file write at "`nm',`=b[1,`j']',`=sqrt(V[`j',`j'])'" _n
}
file close at
di "ATTGT_K=" `k'

estat simple
matrix Rs = r(table)
di "SIMPLE_ATT=" %20.15g Rs[1,1] " SE=" %20.15g Rs[2,1]

estat event, window(-5 5)
matrix Re = r(table)
matrix be = r(b)
local enames : colnames be
local ke = colsof(be)
file open ev using "csdid_event.csv", write replace
file write ev "event,att,se" _n
forvalues j = 1/`ke' {
    local nm : word `j' of `enames'
    file write ev "`nm',`=Re[1,`j']',`=Re[2,`j']'" _n
}
file close ev
di "EVENT_K=" `ke'

keep h_pid wv gvar visits_w female age_z edu_hi
save "csdid_input.dta", replace
log close
