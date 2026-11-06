* Run from tests/validate: every path below is relative to it.
* ts-diagnostics goldens: estat dwatson / durbinalt / bgodfrey / archlm.
* Synthetic AR(1)-error single time series (tsset) so the postestimation
* time-series tests are well defined and detect serial correlation.
clear all
set more off
set type double
set seed 20260616
set obs 200
gen double t = _n
tsset t
gen double x1 = rnormal()
gen double x2 = rnormal()
gen double v  = rnormal()
gen double e  = v
replace     e  = 0.6*e[_n-1] + v in 2/L
gen double y  = 1 + 0.5*x1 - 0.3*x2 + e
keep y x1 x2 t
order t y x1 x2
save "ts_diag_input.dta", replace

regress y x1 x2

* ---- Durbin-Watson ----
estat dwatson
local dw   = r(dw)
local dw_N = r(N)
local dw_k = r(k)

* ---- Durbin's alternative (default chi2), lags 1 & 2 ----
estat durbinalt, lags(1 2)
matrix DA_c = r(chi2)
matrix DA_p = r(p)
local da_l1_chi2 = DA_c[1,1]
local da_l1_p    = DA_p[1,1]
local da_l2_chi2 = DA_c[1,2]
local da_l2_p    = DA_p[1,2]

* ---- Durbin's alternative, small (F form), lag 1 ----
estat durbinalt, small lags(1)
matrix DA_F  = r(F)
matrix DA_dr = r(df_r)
matrix DA_sp = r(p)
local da_s_F   = DA_F[1,1]
local da_s_dfr = DA_dr[1,1]
local da_s_p   = DA_sp[1,1]

* ---- Durbin's alternative, robust (HC1; forces F form), lag 1 ----
estat durbinalt, robust lags(1)
matrix DA_rF  = r(F)
matrix DA_rdr = r(df_r)
matrix DA_rp  = r(p)
local da_r_F   = DA_rF[1,1]
local da_r_dfr = DA_rdr[1,1]
local da_r_p   = DA_rp[1,1]

* ---- Breusch-Godfrey (default chi2), lags 1 & 2 ----
estat bgodfrey, lags(1 2)
matrix BG_c = r(chi2)
matrix BG_p = r(p)
local bg_l1_chi2 = BG_c[1,1]
local bg_l1_p    = BG_p[1,1]
local bg_l2_chi2 = BG_c[1,2]
local bg_l2_p    = BG_p[1,2]

* ---- Breusch-Godfrey, small (F form), lag 1 ----
estat bgodfrey, small lags(1)
matrix BG_F  = r(F)
matrix BG_dr = r(df_r)
matrix BG_sp = r(p)
local bg_s_F   = BG_F[1,1]
local bg_s_dfr = BG_dr[1,1]
local bg_s_p   = BG_sp[1,1]

* ---- ARCH LM, lags 1 2 3 ----
estat archlm, lags(1 2 3)
matrix AR_c = r(arch)
matrix AR_p = r(p)
local ar_l1_chi2 = AR_c[1,1]
local ar_l1_p    = AR_p[1,1]
local ar_l2_chi2 = AR_c[1,2]
local ar_l2_p    = AR_p[1,2]
local ar_l3_chi2 = AR_c[1,3]
local ar_l3_p    = AR_p[1,3]

* ---- Ljung-Box portmanteau (wntestq) on y (autocorrelated) and x1 (white) ----
wntestq y, lags(5)
local wq_y5_stat = r(stat)
local wq_y5_df   = r(df)
local wq_y5_p    = r(p)
wntestq y, lags(10)
local wq_y10_stat = r(stat)
local wq_y10_df   = r(df)
local wq_y10_p    = r(p)
wntestq x1, lags(5)
local wq_x5_stat = r(stat)
local wq_x5_df   = r(df)
local wq_x5_p    = r(p)

file open o using "ts_diag_golden.csv", write replace
file write o "stat,value" _n
file write o "dw,"          (string(`dw',"%21.15e"))          _n
file write o "dw_N,"        (string(`dw_N',"%21.15e"))        _n
file write o "dw_k,"        (string(`dw_k',"%21.15e"))        _n
file write o "da_l1_chi2,"  (string(`da_l1_chi2',"%21.15e"))  _n
file write o "da_l1_p,"     (string(`da_l1_p',"%21.15e"))     _n
file write o "da_l2_chi2,"  (string(`da_l2_chi2',"%21.15e"))  _n
file write o "da_l2_p,"     (string(`da_l2_p',"%21.15e"))     _n
file write o "da_s_F,"      (string(`da_s_F',"%21.15e"))      _n
file write o "da_s_dfr,"    (string(`da_s_dfr',"%21.15e"))    _n
file write o "da_s_p,"      (string(`da_s_p',"%21.15e"))      _n
file write o "da_r_F,"      (string(`da_r_F',"%21.15e"))      _n
file write o "da_r_dfr,"    (string(`da_r_dfr',"%21.15e"))    _n
file write o "da_r_p,"      (string(`da_r_p',"%21.15e"))      _n
file write o "bg_l1_chi2,"  (string(`bg_l1_chi2',"%21.15e"))  _n
file write o "bg_l1_p,"     (string(`bg_l1_p',"%21.15e"))     _n
file write o "bg_l2_chi2,"  (string(`bg_l2_chi2',"%21.15e"))  _n
file write o "bg_l2_p,"     (string(`bg_l2_p',"%21.15e"))     _n
file write o "bg_s_F,"      (string(`bg_s_F',"%21.15e"))      _n
file write o "bg_s_dfr,"    (string(`bg_s_dfr',"%21.15e"))    _n
file write o "bg_s_p,"      (string(`bg_s_p',"%21.15e"))      _n
file write o "ar_l1_chi2,"  (string(`ar_l1_chi2',"%21.15e"))  _n
file write o "ar_l1_p,"     (string(`ar_l1_p',"%21.15e"))     _n
file write o "ar_l2_chi2,"  (string(`ar_l2_chi2',"%21.15e"))  _n
file write o "ar_l2_p,"     (string(`ar_l2_p',"%21.15e"))     _n
file write o "ar_l3_chi2,"  (string(`ar_l3_chi2',"%21.15e"))  _n
file write o "ar_l3_p,"     (string(`ar_l3_p',"%21.15e"))     _n
file write o "wq_y5_stat,"  (string(`wq_y5_stat',"%21.15e"))  _n
file write o "wq_y5_df,"    (string(`wq_y5_df',"%21.15e"))    _n
file write o "wq_y5_p,"     (string(`wq_y5_p',"%21.15e"))     _n
file write o "wq_y10_stat," (string(`wq_y10_stat',"%21.15e")) _n
file write o "wq_y10_p,"    (string(`wq_y10_p',"%21.15e"))    _n
file write o "wq_x5_stat,"  (string(`wq_x5_stat',"%21.15e"))  _n
file write o "wq_x5_p,"     (string(`wq_x5_p',"%21.15e"))     _n
file close o
