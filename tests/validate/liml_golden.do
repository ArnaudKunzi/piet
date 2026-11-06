* Run from tests/validate: every path below is relative to it.
* LIML / Fuller k-class golden vs Stata ivregress liml. Overidentified IV:
* visits_w treated (age_z = female edu_hi). 2SLS kappa must == 1; LIML kappa < 1.
clear all
set more off
set type double
use "ivdiag_input.dta", clear

file open o using "liml_golden.csv", write replace
file write o "stat,value" _n

ivregress 2sls visits_w treated (age_z = female edu_hi), small
file write o "tsls_kappa," (string(e(kappa),"%21.15e")) _n
file write o "tsls_b_age," (string(_b[age_z],"%21.15e")) _n

ivregress liml visits_w treated (age_z = female edu_hi), small
file write o "liml_kappa," (string(e(kappa),"%21.15e")) _n
file write o "liml_b_age," (string(_b[age_z],"%21.15e")) _n
file write o "liml_se_age," (string(_se[age_z],"%21.15e")) _n
file write o "liml_b_treated," (string(_b[treated],"%21.15e")) _n
file write o "liml_se_treated," (string(_se[treated],"%21.15e")) _n
file write o "liml_b_cons," (string(_b[_cons],"%21.15e")) _n
file write o "liml_se_cons," (string(_se[_cons],"%21.15e")) _n
file close o
