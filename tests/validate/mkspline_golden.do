* Run from tests/validate: every path below is relative to it.
* Restricted cubic spline (classic mkspline ,cubic, default 5 Harrell knots) golden.
clear all
set more off
set type double
use "csdid_input.dta", clear
mkspline ag = age_z, cubic
matrix K = r(knots)
matrix list K
gen long _row = _n
keep if _row <= 200
export delimited age_z ag1 ag2 ag3 ag4 using "mkspline_golden.csv", replace
