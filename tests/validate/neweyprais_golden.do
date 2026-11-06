* Run from tests/validate: every path below is relative to it.
* newey (HAC) + prais (AR(1) FGLS) goldens. sysuse auto, t=_n (contiguous tsset).
clear all
set more off
set type double
sysuse auto, clear
gen double t = _n
tsset t
keep price weight length t
save "neweyprais_input.dta", replace

* ===== NEWEY lag(2) =====
newey price weight length, lag(2)
local nw2_b_w = _b[weight]
local nw2_b_l = _b[length]
local nw2_b_c = _b[_cons]
local nw2_se_w = _se[weight]
local nw2_se_l = _se[length]
local nw2_se_c = _se[_cons]
local nw2_N = e(N)

* ===== NEWEY lag(0) == regress robust =====
newey price weight length, lag(0)
local nw0_se_w = _se[weight]
local nw0_se_l = _se[length]
regress price weight length, vce(robust)
local rob_se_w = _se[weight]
local rob_se_l = _se[length]

* ===== PRAIS iterated (default rhotype regress, PW) =====
prais price weight length
local pr_b_w = _b[weight]
local pr_b_l = _b[length]
local pr_b_c = _b[_cons]
local pr_se_w = _se[weight]
local pr_se_l = _se[length]
local pr_se_c = _se[_cons]
local pr_rho = e(rho)
local pr_N = e(N)
local pr_dw = e(dw)
local pr_dw0 = e(dw_0)

* ===== PRAIS twostep =====
prais price weight length, twostep
local pr2_b_w = _b[weight]
local pr2_rho = e(rho)
local pr2_se_w = _se[weight]

* ===== PRAIS corc (Cochrane-Orcutt) =====
prais price weight length, corc
local prc_b_w = _b[weight]
local prc_rho = e(rho)
local prc_N = e(N)
local prc_se_w = _se[weight]

* ===== PRAIS rhotype(tscorr) =====
prais price weight length, rhotype(tscorr)
local prt_rho = e(rho)
local prt_b_w = _b[weight]

file open o using "neweyprais_golden.csv", write replace
file write o "stat,value" _n
foreach s in nw2_b_w nw2_b_l nw2_b_c nw2_se_w nw2_se_l nw2_se_c nw2_N ///
             nw0_se_w nw0_se_l rob_se_w rob_se_l ///
             pr_b_w pr_b_l pr_b_c pr_se_w pr_se_l pr_se_c pr_rho pr_N pr_dw pr_dw0 ///
             pr2_b_w pr2_rho pr2_se_w prc_b_w prc_rho prc_N prc_se_w prt_rho prt_b_w {
    file write o "`s'," (string(``s'',"%21.15e")) _n
}
file close o
