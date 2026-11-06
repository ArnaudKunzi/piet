* Run from tests/validate: every path below is relative to it.
* reg3 goldens: SUR (sure) and 3SLS (default) on a 2-equation system with
* cross-equation correlated errors. 3SLS eq1 has endogenous RHS y2.
clear all
set more off
set type double
set seed 20260616
set obs 500
gen double x1 = rnormal()
gen double x2 = rnormal()
gen double x3 = rnormal()
gen double e1 = rnormal()
gen double e2 = rnormal()
gen double u1 = e1
gen double u2 = 0.6*e1 + 0.8*e2
* SUR system (all exogenous)
gen double y1 = 1 + 0.5*x1 - 0.3*x2 + u1
* simultaneous-ish: y2 from exog, y1s with endogenous y2
gen double y2 = 2 + 0.7*x2 - 0.4*x3 + u2
gen double y1s = 1 + 0.5*y2 + 0.3*x1 + u1
keep x1 x2 x3 y1 y2 y1s
save "reg3_input.dta", replace

file open o using "reg3_golden.csv", write replace
file write o "stat,value" _n

* ===== SUR: (y1 x1 x2) (y2 x2 x3) =====
reg3 (y1 x1 x2) (y2 x2 x3), sure
matrix b = e(b)
matrix V = e(V)
matrix Sg = e(Sigma)
* e(b) columns: y1:x1 y1:x2 y1:_cons  y2:x2 y2:x3 y2:_cons
file write o "sur_b1," (string(b[1,1],"%21.15e")) _n
file write o "sur_se1," (string(sqrt(V[1,1]),"%21.15e")) _n
file write o "sur_b2," (string(b[1,2],"%21.15e")) _n
file write o "sur_bc," (string(b[1,3],"%21.15e")) _n
file write o "sur_bcse," (string(sqrt(V[3,3]),"%21.15e")) _n
file write o "sur_b4," (string(b[1,4],"%21.15e")) _n
file write o "sur_se4," (string(sqrt(V[4,4]),"%21.15e")) _n
file write o "sur_b5," (string(b[1,5],"%21.15e")) _n
file write o "sur_sig11," (string(Sg[1,1],"%21.15e")) _n
file write o "sur_sig12," (string(Sg[1,2],"%21.15e")) _n
file write o "sur_sig22," (string(Sg[2,2],"%21.15e")) _n
file write o "sur_N," (string(e(N),"%21.15e")) _n

* ===== 3SLS: (y1s y2 x1) (y2 x2 x3) =====  (y2 endogenous in eq1)
reg3 (y1s y2 x1) (y2 x2 x3)
matrix b = e(b)
matrix V = e(V)
* e(b): y1s:y2 y1s:x1 y1s:_cons  y2:x2 y2:x3 y2:_cons
file write o "tsls_by2," (string(b[1,1],"%21.15e")) _n
file write o "tsls_by2se," (string(sqrt(V[1,1]),"%21.15e")) _n
file write o "tsls_bx1," (string(b[1,2],"%21.15e")) _n
file write o "tsls_bc," (string(b[1,3],"%21.15e")) _n
file write o "tsls_bcse," (string(sqrt(V[3,3]),"%21.15e")) _n
file write o "tsls_e2x2," (string(b[1,4],"%21.15e")) _n
file write o "tsls_e2x2se," (string(sqrt(V[4,4]),"%21.15e")) _n
file write o "tsls_N," (string(e(N),"%21.15e")) _n
file close o
