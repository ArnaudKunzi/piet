* Run from tests/validate: every path below is relative to it.
* ts-operators golden: L/F/D/S operators over a 2-panel series with a time gap
* (panel 1 is missing t=4), to validate gap- and boundary-aware missing rules.
clear all
set more off
set type double
input double id double t double x
1 1 10
1 2 20
1 3 30
1 5 50
2 1 100
2 2 200
2 3 300
end
xtset id t
gen double L1x = L.x
gen double L2x = L2.x
gen double F1x = F.x
gen double D1x = D.x
gen double D2x = D2.x
gen double S2x = S2.x
gen double LDx = L.D.x

preserve
keep id t x
save "ts_operators_input.dta", replace
restore

file open o using "ts_operators_golden.csv", write replace
file write o "id,t,x,L1x,L2x,F1x,D1x,D2x,S2x,LDx" _n
forvalues i = 1/`=_N' {
    file write o (string(id[`i'])) "," (string(t[`i'])) "," (string(x[`i'],"%21.15e")) ","
    file write o (string(L1x[`i'],"%21.15e")) "," (string(L2x[`i'],"%21.15e")) ","
    file write o (string(F1x[`i'],"%21.15e")) "," (string(D1x[`i'],"%21.15e")) ","
    file write o (string(D2x[`i'],"%21.15e")) "," (string(S2x[`i'],"%21.15e")) ","
    file write o (string(LDx[`i'],"%21.15e")) _n
}
file close o
