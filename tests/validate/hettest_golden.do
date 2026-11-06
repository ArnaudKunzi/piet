* Run from tests/validate: every path below is relative to it.
* estat hettest / estat ovtest goldens for cecon hettest & ovtest.
* Canonical docs example: regress price mpg weight foreign on sysuse auto.
clear all
set more off
set type double
sysuse auto, clear
keep price mpg weight foreign
label values foreign        // drop value label so to_hbin keeps foreign numeric (0/1)
save "hettest_input.dta", replace

regress price mpg weight foreign

* --- Breusch-Pagan / Cook-Weisberg, default (normal), z = fitted values ---
estat hettest
local ht_chi2 = r(chi2)
local ht_df   = r(df)
local ht_p    = r(p)

* --- Koenker N*R^2 (iid), z = fitted values ---
estat hettest, iid
local hi_chi2 = r(chi2)
local hi_df   = r(df)

* --- Wooldridge F form, z = fitted values ---
estat hettest, fstat
local hf_F   = r(F)
local hf_dfm = r(df_m)
local hf_dfr = r(df_r)

* --- normal, z = rhs (all regressors) ---
estat hettest, rhs
local hr_chi2 = r(chi2)
local hr_df   = r(df)

* --- iid (N*R^2), z = rhs ---
estat hettest, rhs iid
local hri_chi2 = r(chi2)
local hri_df   = r(df)

* --- White's general test (n*R^2 of e^2 on X + cross products) ---
* (this Stata version stores White's stat in r(chi2)/r(df), not r(chi2_w)/r(df_w))
estat imtest, white
local w_chi2 = r(chi2)
local w_df   = r(df)

* --- Ramsey RESET (powers of fitted values) ---
estat ovtest
local ov_F   = r(F)
local ov_dfm = r(df)
local ov_dfr = r(df_r)
local ov_p   = r(p)

file open o using "hettest_golden.csv", write replace
file write o "stat,value" _n
file write o "ht_chi2,"  (string(`ht_chi2',"%21.15e"))  _n
file write o "ht_df,"    (string(`ht_df',"%21.15e"))    _n
file write o "ht_p,"     (string(`ht_p',"%21.15e"))     _n
file write o "hi_chi2,"  (string(`hi_chi2',"%21.15e"))  _n
file write o "hi_df,"    (string(`hi_df',"%21.15e"))    _n
file write o "hf_F,"     (string(`hf_F',"%21.15e"))     _n
file write o "hf_dfm,"   (string(`hf_dfm',"%21.15e"))   _n
file write o "hf_dfr,"   (string(`hf_dfr',"%21.15e"))   _n
file write o "hr_chi2,"  (string(`hr_chi2',"%21.15e"))  _n
file write o "hr_df,"    (string(`hr_df',"%21.15e"))    _n
file write o "hri_chi2," (string(`hri_chi2',"%21.15e")) _n
file write o "hri_df,"   (string(`hri_df',"%21.15e"))   _n
file write o "w_chi2,"   (string(`w_chi2',"%21.15e"))   _n
file write o "w_df,"     (string(`w_df',"%21.15e"))     _n
file write o "ov_F,"     (string(`ov_F',"%21.15e"))     _n
file write o "ov_dfm,"   (string(`ov_dfm',"%21.15e"))   _n
file write o "ov_dfr,"   (string(`ov_dfr',"%21.15e"))   _n
file write o "ov_p,"     (string(`ov_p',"%21.15e"))     _n
file close o
