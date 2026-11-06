* Run from tests/validate: every path below is relative to it.
* glm quasi-Poisson scale(x2) golden vs cecon fepois --scale x2 (pooled, no FE).
clear all
set more off
set type double
use "csdid_input.dta", clear
gen byte treated = (gvar>0 & wv>=gvar) if !missing(gvar)
recast double treated
keep visits_w age_z edu_hi treated
keep if !missing(visits_w, age_z, edu_hi, treated)
save "glmqp_input.dta", replace

* baseline Poisson (scale 1) — coefs cecon must match
glm visits_w age_z edu_hi treated, family(poisson) link(log)
matrix b0 = e(b)

* quasi-Poisson scale(x2) — dispersion-scaled SEs
glm visits_w age_z edu_hi treated, family(poisson) link(log) scale(x2)
matrix b = e(b)
matrix V = e(V)
local disp = e(dispersp)        // Pearson dispersion (phi)
if "`disp'"=="" local disp = e(phi)

file open o using "glmqp_golden.csv", write replace
file write o "name,b,se" _n
file write o "age_z,"   (string(b[1,1],"%21.15e")) "," (string(sqrt(V[1,1]),"%21.15e")) _n
file write o "edu_hi,"  (string(b[1,2],"%21.15e")) "," (string(sqrt(V[2,2]),"%21.15e")) _n
file write o "treated," (string(b[1,3],"%21.15e")) "," (string(sqrt(V[3,3]),"%21.15e")) _n
file write o "_cons,"   (string(b[1,4],"%21.15e")) "," (string(sqrt(V[4,4]),"%21.15e")) _n
file write o "phi,"     (string(`disp',"%21.15e")) ",0" _n
file close o
