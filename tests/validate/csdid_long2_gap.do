* ============================================================================
* csdid_long2_gap.do — synthesize a GAPPED staggered panel + Stata csdid truth,
* exercising the long2 filled-grid path (COBRA 689b53c) and the drop-exact-zero
* aggregation gate (346b6bd).  StataNow BE 19.5 + csdid/drdid.
*
* Design: units observed only at times {1,2,4,6} (GAPS at t=3,t=5). gdelta=1
* (min consecutive step). long2 must build the FILLED regular grid
* range(1,6,1)={1,2,3,4,5,6} and pin each g's fixed base at g-gdelta. Staggered
* cohorts g in {2,4} + never-treated g=0. The filled grid introduces (g,t) cells
* whose ATT is EXACTLY 0 (e.g. g2:t_1_3, g2:t_1_5, all g4 cells) — Stata's
* csdid_estat gates aggregation on b[k]!=0, so those never contribute.
*
* SELF-CONSISTENT ORACLE: the data is exported at FULL precision (%.17g), then
* RE-IMPORTED, and csdid is run on the RE-IMPORTED data — so the Stata truth is
* computed on the exact same float64 values that go into the cobra hbin. This
* guarantees cobra (reading the hbin built from this CSV) and Stata are comparing
* the identical dataset (no export-precision drift).
*
* Emits (into tests/validate/):
*   csdid_long2_gap.csv       — headered data (id,t,g,y) at %.17g for hbin build
*   csdid_long2_gap_TRUTH.csv — name,att,se for the long2 ATT(g,t) cells
* Run:  cd tests/validate && StataBE -b do csdid_long2_gap.do
* ============================================================================
clear all
set more off
set seed 20260706
set type double
capture log close
log using "csdid_long2_gap.log", replace text

* ---- build a time-GAPPED staggered panel (times {1,2,4,6}) ----
local NU = 90
set obs `NU'
gen long id = _n
gen g = cond(id<=30, 2, cond(id<=60, 4, 0))
gen double ui = rnormal()
expand 4
bysort id: gen t = cond(_n==1,1, cond(_n==2,2, cond(_n==3,4,6)))
gen byte post = (g>0 & t>=g)
gen double y = ui + 0.4*t + 1.5*post + rnormal()*0.5
keep id t g y
order id t g y
sort id t

* ---- export losslessly, then RE-IMPORT so truth == hbin data ----
export delimited using "csdid_long2_gap.csv", replace nolabel datafmt
* re-import the exact %.17g values
import delimited using "csdid_long2_gap.csv", clear case(preserve) asdouble
* (export delimited datafmt on set type double writes full precision)

* ---- Stata csdid TRUTH with long2 on the RE-IMPORTED data ----
csdid y, ivar(id) time(t) gvar(g) method(dripw) long2
display "CSDID_LONG2_ATTGT_BEGIN"
matrix b = e(b)
matrix V = e(V)
local names : colfullnames e(b)
local k = colsof(b)
forvalues j = 1/`k' {
    local nm : word `j' of `names'
    display "ATTGT," "`nm'," %21.15e b[1,`j'] "," %21.15e sqrt(V[`j',`j'])
}
display "CSDID_LONG2_ATTGT_END"

estat event, post
display "CSDID_LONG2_EVENT_BEGIN"
matrix eb = e(b)
matrix eV = e(V)
local enames : colfullnames e(b)
local ek = colsof(eb)
forvalues j = 1/`ek' {
    local nm : word `j' of `enames'
    display "EVENT," "`nm'," %21.15e eb[1,`j'] "," %21.15e sqrt(eV[`j',`j'])
}
display "CSDID_LONG2_EVENT_END"

csdid y, ivar(id) time(t) gvar(g) method(dripw) long2
estat simple, post
display "CSDID_LONG2_SIMPLE_BEGIN"
display "SIMPLE,simple_att," %21.15e _b[Post_avg] "," %21.15e _se[Post_avg]
display "CSDID_LONG2_SIMPLE_END"

log close
