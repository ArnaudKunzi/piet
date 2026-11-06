clear all
set more off
capture log close
log using "demean3fe_golden.log", replace text
* Run from tests/validate: every path below is relative to it.

* Synthetic panel with THREE genuinely crossed, connected fixed effects so the
* demean must iterate (exercises cecon's Anderson-accelerated path, fe>=3).
set seed 20260616
set obs 20000
gen long id1 = mod(_n, 500)
gen long id2 = mod(_n*7 + 3, 311)
gen long id3 = mod(_n*13 + 5, 127)
gen double x1 = rnormal()
gen double x2 = rnormal()

* FE effects + structural coefficients + noise
bys id1: gen double fe1 = rnormal() if _n==1
bys id1 (fe1): replace fe1 = fe1[1]
bys id2: gen double fe2 = rnormal() if _n==1
bys id2 (fe2): replace fe2 = fe2[1]
bys id3: gen double fe3 = rnormal() if _n==1
bys id3 (fe3): replace fe3 = fe3[1]
gen double y = 1.5*x1 - 0.8*x2 + fe1 + fe2 + fe3 + rnormal()

* reghdfe with 3 absorbed FE, cluster on id1
reghdfe y x1 x2, absorb(id1 id2 id3) vce(cluster id1)

* dump coefficients/SEs for the comparison
file open f using "demean3fe_golden.csv", write replace
file write f "name,b,se" _n
file write f "x1," (string(_b[x1],"%21.15e")) "," (string(_se[x1],"%21.15e")) _n
file write f "x2," (string(_b[x2],"%21.15e")) "," (string(_se[x2],"%21.15e")) _n
file close f

* export the SAME data for cecon (only the columns it needs)
keep y x1 x2 id1 id2 id3
export delimited using "demean3fe_data.csv", replace

log close
