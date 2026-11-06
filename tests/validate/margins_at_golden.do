* Run from tests/validate: every path below is relative to it.
* Predictive-margins-at() golden for cecon margins --at (linear model).
* regress visits_w age_z age2 edu_hi ; margins at 3 covariate points.
clear all
set more off
set type double
use "csdid_input.dta", clear
gen double age2 = age_z^2
regress visits_w age_z age2 edu_hi
margins, at(age_z=0 age2=0) at(age_z=1 age2=1) at(age_z=-0.5 age2=0.25 edu_hi=1)
matrix b = r(b)
matrix V = r(V)
file open o using "margins_at_golden.csv", write replace
file write o "pt,margin,se" _n
file write o "p1," (string(b[1,1],"%21.15e")) "," (string(sqrt(V[1,1]),"%21.15e")) _n
file write o "p2," (string(b[1,2],"%21.15e")) "," (string(sqrt(V[2,2]),"%21.15e")) _n
file write o "p3," (string(b[1,3],"%21.15e")) "," (string(sqrt(V[3,3]),"%21.15e")) _n
file close o
