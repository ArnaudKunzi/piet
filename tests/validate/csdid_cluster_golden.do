* ============================================================================
* csdid_cluster_golden.do
* Deterministic synthetic clustered staggered panel for cobra csdid --cluster
* parity validation. Generates data + StataBE truth (e(b)/e(V) from estat event).
* ============================================================================
clear all
set more off
set seed 20260628
set type double

capture log close
log using "csdid_cluster_golden.log", replace text

* ----------------------------------------------------------------------------
* Build balanced panel:
*   60 clusters, 5 units/cluster = 300 units, 8 time periods t=1..8 => 2400 rows
* ----------------------------------------------------------------------------
local NCLU   = 60
local UPC    = 5          /* units per cluster */
local NUNIT  = `NCLU'*`UPC'    /* 300 */
local NT     = 8

set obs `NUNIT'
gen long unit = _n
* cluster id: constant within unit, 5 consecutive units per cluster
gen long clu  = ceil(unit/`UPC')          /* 1..60 */

* staggered cohort by cluster: g in {0,4,5,6,7}, spread evenly over clusters
*  clusters 1..12 -> 0 (never), 13..24 -> 4, 25..36 -> 5, 37..48 -> 6, 49..60 -> 7
gen long g = .
replace g = 0 if clu>=1  & clu<=12
replace g = 4 if clu>=13 & clu<=24
replace g = 5 if clu>=25 & clu<=36
replace g = 6 if clu>=37 & clu<=48
replace g = 7 if clu>=49 & clu<=60

* unit fixed effect (constant within unit) and cluster shock (constant within cluster)
gen double unit_fe = rnormal(0,1)
* cluster-level shock: draw once per cluster -> assign by collapse-merge approach.
* Simpler: draw per-unit then overwrite to the value of the first unit in cluster.
gen double clu_shock = rnormal(0,sqrt(0.5))
bysort clu (unit): replace clu_shock = clu_shock[1]

* expand to 8 periods
expand `NT'
bysort unit: gen long t = _n          /* 1..8 within unit */

* covariate, idiosyncratic error
gen double x = rnormal(0,1)
gen double e = rnormal(0,1)

* dynamic treatment effect: tau = 0.4*(t-g+1) for t>=g & g>0, else 0
gen double tau = 0
replace tau = 0.4*(t - g + 1) if (t>=g & g>0)

* outcome
gen double y = unit_fe + 0.3*t + 0.5*x + tau + clu_shock + e

* keep required columns, recast double
keep unit t g clu x y
order unit t g clu x y
recast double unit t g clu x y
sort unit t

* ----------------------------------------------------------------------------
* Export data two ways
* ----------------------------------------------------------------------------
save "csdid_cluster_synth.dta", replace
export delimited unit t g clu x y using "csdid_cluster_synth.csv", replace

* ----------------------------------------------------------------------------
* RUN csdid + estat event, dump truth
* ----------------------------------------------------------------------------
xtset unit t
csdid y x, ivar(unit) time(t) gvar(g) method(dripw) cluster(clu)

di "CSDID_N=" e(N)
di "CSDID_NCLUST=" e(N_clust)

estat event, post
matrix b = e(b)
matrix V = e(V)
di "EVENT_N=" e(N)

local names : colnames b
local k = colsof(b)
di "EVENT_K=" `k'

* --- diagonal truth csv: name,b,se ---
file open tr using "csdid_cluster_synth_TRUTH.csv", write replace
file write tr "name,b,se" _n
forvalues j = 1/`k' {
    local nm : word `j' of `names'
    file write tr "`nm',`=b[1,`j']',`=sqrt(V[`j',`j'])'" _n
}
file close tr

* --- full V matrix (k x k) ---
file open vv using "csdid_cluster_synth_TRUTH_V.csv", write replace
forvalues i = 1/`k' {
    local line ""
    forvalues j = 1/`k' {
        if `j'==1 local line "`=V[`i',`j']'"
        else      local line "`line',`=V[`i',`j']'"
    }
    file write vv "`line'" _n
}
file close vv

* ----------------------------------------------------------------------------
* simple ATT (re-estimate because estat event,post replaced e())
* ----------------------------------------------------------------------------
csdid y x, ivar(unit) time(t) gvar(g) method(dripw) cluster(clu)
estat simple
matrix Rs = r(table)
di "SIMPLE_ATT=" %20.15g Rs[1,1] " SE=" %20.15g Rs[2,1]

file open sm using "csdid_cluster_synth_SIMPLE.csv", write replace
file write sm "name,b,se" _n
file write sm "simple_att,`=Rs[1,1]',`=Rs[2,1]'" _n
file close sm

di "DONE_OK"
log close
