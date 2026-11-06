* Run from tests/validate: every path below is relative to it.
* heckman two-step (Heckit) golden. Synthetic sample-selection DGP with rho=0.5.
clear all
set more off
set type double
set seed 20260616
set obs 3000
gen double z1 = rnormal()
gen double z2 = rnormal()
gen double x1 = rnormal()
gen double e1 = rnormal()
gen double e2 = rnormal()
local rho = 0.5
local sig = 5
gen double u2 = e2
gen double u1 = `sig'*(`rho'*e2 + sqrt(1-`rho'^2)*e1)
gen double Sstar = 0.3 + 0.7*z1 - 0.4*z2 + u2
gen byte S = Sstar > 0
gen double y = 10 + 2*x1 + u1 if S==1
keep y x1 z1 z2 S
save "heckman_input.dta", replace

heckman y x1, select(S = z1 z2) twostep
matrix b = e(b)
matrix V = e(V)
matrix list b

* columns of e(b): 1=y:x1 2=y:_cons 3=S:z1 4=S:z2 5=S:_cons 6=mills:lambda
local oy_x1   = b[1,1]
local oy_x1se = sqrt(V[1,1])
local oy_c    = b[1,2]
local oy_cse  = sqrt(V[2,2])
local s_z1    = b[1,3]
local s_z1se  = sqrt(V[3,3])
local s_z2    = b[1,4]
local s_c     = b[1,5]
local lam     = b[1,6]
local lamse   = sqrt(V[6,6])
local rho_e   = e(rho)
local sig_e   = e(sigma)
local N       = e(N)
local Nsel    = e(N_selected)

file open o using "heckman_golden.csv", write replace
file write o "stat,value" _n
foreach s in oy_x1 oy_x1se oy_c oy_cse s_z1 s_z1se s_z2 s_c lam lamse rho_e sig_e N Nsel {
    file write o "`s'," (string(``s'',"%21.15e")) _n
}
file close o
