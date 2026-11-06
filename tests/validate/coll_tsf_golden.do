* Run from tests/validate: every path below is relative to it.
* collapse + tsfill goldens. Small 3-panel series with a time gap.
clear all
set more off
set type double
input double id double t double y double w
1 1 10 2
1 2 20 4
1 4 40 8
2 1 5 1
2 2 15 3
2 3 25 5
3 2 100 9
3 3 200 .
end
save "coll_tsf_input.dta", replace

* ---- collapse ----
preserve
collapse (mean) my=y (sum) sy=y (sd) sdy=y (count) ny=y (max) mxw=w (min) mnw=w (median) mdy=y, by(id)
file open o using "collapse_golden.csv", write replace
file write o "id,my,sy,sdy,ny,mxw,mnw,mdy" _n
forvalues i = 1/`=_N' {
    file write o (string(id[`i'])) "," (string(my[`i'],"%21.15e")) "," (string(sy[`i'],"%21.15e")) ","
    file write o (string(sdy[`i'],"%21.15e")) "," (string(ny[`i'])) "," (string(mxw[`i'],"%21.15e")) ","
    file write o (string(mnw[`i'],"%21.15e")) "," (string(mdy[`i'],"%21.15e")) _n
}
file close o
restore

* ---- tsfill ----
tsset id t
tsfill
file open o2 using "tsfill_golden.csv", write replace
file write o2 "id,t,y,w" _n
forvalues i = 1/`=_N' {
    file write o2 (string(id[`i'])) "," (string(t[`i'])) "," (string(y[`i'],"%21.15e")) "," (string(w[`i'],"%21.15e")) _n
}
file close o2
