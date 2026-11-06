* Run from tests/validate: every path below is relative to it.
* csdid estat pretrend + estat calendar + estat group golden, on the saved
* csdid_input subset with the same covariates the cecon test uses
* (female age_z edu_hi).
clear all
set more off
set type double
use "csdid_input.dta", clear
xtset h_pid wv
csdid visits_w female age_z edu_hi, ivar(h_pid) time(wv) gvar(gvar) method(dripw)

file open o using "csdid_estat_golden.csv", write replace
file write o "stat,value" _n

estat pretrend
file write o "pretrend_chi2," (string(r(chi2),"%21.15e")) _n
file write o "pretrend_df," (string(r(df),"%21.15e")) _n
file write o "pretrend_p," (string(r(p),"%21.15e")) _n

estat calendar
matrix cb = r(b)
matrix cV = r(V)
local cn : colnames cb
local kc = colsof(cb)
di "CAL_K=" `kc'
forvalues j = 1/`kc' {
    local nm : word `j' of `cn'
    file write o "cal_`nm'_att," (string(cb[1,`j'],"%21.15e")) _n
    file write o "cal_`nm'_se," (string(sqrt(cV[`j',`j']),"%21.15e")) _n
}

estat group
matrix gb = r(b)
matrix gV = r(V)
local gn : colnames gb
local kg = colsof(gb)
di "GRP_K=" `kg'
forvalues j = 1/`kg' {
    local nm : word `j' of `gn'
    file write o "grp_`nm'_att," (string(gb[1,`j'],"%21.15e")) _n
    file write o "grp_`nm'_se," (string(sqrt(gV[`j',`j']),"%21.15e")) _n
}
file close o
