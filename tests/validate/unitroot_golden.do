* Run from tests/validate: every path below is relative to it.
* unit-root goldens: dfuller (cases) + pperron. Persistent AR(1) series, tsset.
clear all
set more off
set type double
set seed 20260616
set obs 200
gen double t = _n
tsset t
gen double e = rnormal()
gen double y = e
replace     y = 0.9*y[_n-1] + e in 2/L
keep y t
save "unitroot_input.dta", replace

* dfuller default (case 2, constant)
dfuller y
local d2_Zt=r(Zt)
local d2_p=r(p)
local d2_cv1=r(cv_1)
local d2_cv5=r(cv_5)
local d2_cv10=r(cv_10)
local d2_N=r(N)

* dfuller noconstant (case 1)
dfuller y, noconstant
local d1_Zt=r(Zt)
local d1_cv5=r(cv_5)
local d1_N=r(N)

* dfuller trend (case 4)
dfuller y, trend
local d4_Zt=r(Zt)
local d4_p=r(p)
local d4_cv5=r(cv_5)
local d4_N=r(N)

* dfuller drift (case 3, standard t)
dfuller y, drift
local d3_Zt=r(Zt)
local d3_p=r(p)
local d3_cv5=r(cv_5)
local d3_N=r(N)

* dfuller lags(2), default
dfuller y, lags(2)
local dl_Zt=r(Zt)
local dl_p=r(p)
local dl_N=r(N)

* pperron default (case 2)
pperron y
local pp_Zt=r(Zt)
local pp_Zrho=r(Zrho)
local pp_p=r(pval)
local pp_N=r(N)
local pp_lags=r(lags)

* pperron trend (case 4)
pperron y, trend
local ppt_Zt=r(Zt)
local ppt_Zrho=r(Zrho)
local ppt_p=r(pval)
local ppt_N=r(N)

file open o using "unitroot_golden.csv", write replace
file write o "stat,value" _n
foreach s in d2_Zt d2_p d2_cv1 d2_cv5 d2_cv10 d2_N d1_Zt d1_cv5 d1_N ///
             d4_Zt d4_p d4_cv5 d4_N d3_Zt d3_p d3_cv5 d3_N dl_Zt dl_p dl_N ///
             pp_Zt pp_Zrho pp_p pp_N pp_lags ppt_Zt ppt_Zrho ppt_p ppt_N {
    file write o "`s'," (string(``s'',"%21.15e")) _n
}
file close o
