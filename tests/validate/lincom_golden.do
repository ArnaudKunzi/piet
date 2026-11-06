* Run from tests/validate: every path below is relative to it.
* lincom golden: linear combinations after regress visits_w age_z age2 edu_hi.
clear all
set more off
set type double
use "csdid_input.dta", clear
gen double age2 = age_z^2
keep visits_w age_z age2 edu_hi
keep if !missing(visits_w, age_z, age2, edu_hi)
regress visits_w age_z age2 edu_hi

file open o using "lincom_golden.csv", write replace
file write o "exp,est,se,lb,ub" _n

lincom age_z + edu_hi
file write o "c1," (string(r(estimate),"%21.15e")) "," (string(r(se),"%21.15e")) "," (string(r(lb),"%21.15e")) "," (string(r(ub),"%21.15e")) _n

lincom 2*age_z - age2
file write o "c2," (string(r(estimate),"%21.15e")) "," (string(r(se),"%21.15e")) "," (string(r(lb),"%21.15e")) "," (string(r(ub),"%21.15e")) _n

lincom edu_hi + 1.5
file write o "c3," (string(r(estimate),"%21.15e")) "," (string(r(se),"%21.15e")) "," (string(r(lb),"%21.15e")) "," (string(r(ub),"%21.15e")) _n

lincom _cons - age_z
file write o "c4," (string(r(estimate),"%21.15e")) "," (string(r(se),"%21.15e")) "," (string(r(lb),"%21.15e")) "," (string(r(ub),"%21.15e")) _n
file close o
