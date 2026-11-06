* Run from tests/validate: every path below is relative to it.
* Mediation product-of-coefficients golden (visits_w <- age_z <- edu_hi), complete cases.
clear all
set more off
set type double
use "csdid_input.dta", clear
drop if missing(visits_w) | missing(age_z) | missing(edu_hi)
regress age_z edu_hi
local a = _b[edu_hi]
local sa = _se[edu_hi]
regress visits_w edu_hi age_z
local b = _b[age_z]
local sb = _se[age_z]
local cp = _b[edu_hi]
local ind = (`a')*(`b')
local sob = sqrt((`a')^2*(`sb')^2 + (`b')^2*(`sa')^2)
file open o using "mediate_golden.csv", write replace
file write o "a,se_a,b,se_b,cp,indirect,sobel" _n
file write o "`a',`sa',`b',`sb',`cp',`ind',`sob'" _n
file close o
