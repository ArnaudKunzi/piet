* Run from tests/validate: every path below is relative to it.
* nlcom delta-method golden for cecon nlcom. Model: regress visits_w on age_z,
* age_z^2, edu_hi; nlcom turning point / linear combo / ratio / _cons*2.
clear all
set more off
set type double
use "csdid_input.dta", clear
gen double age2 = age_z^2
regress visits_w age_z age2 edu_hi
nlcom (turn: -_b[age_z]/(2*_b[age2])) (sumc: _b[age_z]+_b[edu_hi]) (ratio: _b[age_z]/_b[edu_hi]) (cc: _b[_cons]*2), post
matrix b = e(b)
matrix V = e(V)
file open o using "nlcom_golden.csv", write replace
file write o "name,b,se" _n
file write o "turn,"  (string(b[1,1],"%21.15e")) "," (string(sqrt(V[1,1]),"%21.15e")) _n
file write o "sumc,"  (string(b[1,2],"%21.15e")) "," (string(sqrt(V[2,2]),"%21.15e")) _n
file write o "ratio," (string(b[1,3],"%21.15e")) "," (string(sqrt(V[3,3]),"%21.15e")) _n
file write o "cc,"    (string(b[1,4],"%21.15e")) "," (string(sqrt(V[4,4]),"%21.15e")) _n
file close o
