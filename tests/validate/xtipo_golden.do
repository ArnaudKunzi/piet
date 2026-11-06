* Run from tests/validate: every path below is relative to it.
* xtile + ipolate golden. Continuous x for decile bins; y with interior gaps and a
* missing tail to exercise interpolation + (epolate) extrapolation edges.
clear all
set more off
set type double
set seed 7
set obs 60
gen double x = rnormal()*10 + 50
gen double t = _n
gen double y = 2*t + rnormal()
replace y = . if inlist(_n, 5, 12, 13, 30, 31, 32, 60)
xtile xt10 = x, nq(10)
xtile xt4  = x, nq(4)
ipolate y t, gen(yi)
ipolate y t, gen(ye) epolate

preserve
keep t x y
save "xtipo_input.dta", replace
restore

file open o using "xtipo_golden.csv", write replace
file write o "t,xt10,xt4,yi,ye" _n
forvalues i = 1/`=_N' {
    file write o (string(t[`i'])) "," (string(xt10[`i'])) "," (string(xt4[`i'])) ","
    file write o (string(yi[`i'],"%21.15e")) "," (string(ye[`i'],"%21.15e")) _n
}
file close o
