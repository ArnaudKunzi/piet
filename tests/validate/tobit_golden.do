* Run from tests/validate: every path below is relative to it.
* tobit golden: left-censored (at 0) normal MLE. visits_w on age_z edu_hi treated.
clear all
set more off
set type double
use "csdid_input.dta", clear
gen byte treated = (gvar>0 & wv>=gvar) if !missing(gvar)
recast double treated
keep visits_w age_z edu_hi treated
keep if !missing(visits_w, age_z, edu_hi, treated)
save "tobit_input.dta", replace

tobit visits_w age_z edu_hi treated, ll(0)
matrix b = e(b)
matrix V = e(V)
local N      = e(N)
local N_lc   = e(N_lc)
local ll     = e(ll)
local k = colsof(b)            // classic tobit: last column is /sigma (direct metric)
local sigma    = b[1,`k']
local se_sigma = sqrt(V[`k',`k'])

file open o using "tobit_golden.csv", write replace
file write o "name,b,se" _n
file write o "age_z,"   (string(b[1,1],"%21.15e")) "," (string(sqrt(V[1,1]),"%21.15e")) _n
file write o "edu_hi,"  (string(b[1,2],"%21.15e")) "," (string(sqrt(V[2,2]),"%21.15e")) _n
file write o "treated," (string(b[1,3],"%21.15e")) "," (string(sqrt(V[3,3]),"%21.15e")) _n
file write o "_cons,"   (string(b[1,4],"%21.15e")) "," (string(sqrt(V[4,4]),"%21.15e")) _n
file write o "sigma,"   (string(`sigma',"%21.15e")) "," (string(`se_sigma',"%21.15e")) _n
file write o "ll,"      (string(`ll',"%21.15e"))    ",0" _n
file write o "N,"       (string(`N',"%21.15e"))     ",0" _n
file write o "N_lc,"    (string(`N_lc',"%21.15e"))  ",0" _n
file close o
