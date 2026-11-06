* dfuller_drift + pperron_default oracle re-derivation (BUG-227 tranche 1,
* WS-X 2026-08-07). unitroot.csv was self-captured from cobra.
* NOTE on the exact-zero cells: the cv_1/cv_5/cv_10 cells are INTERPOLATED
* TABULATED critical values — legitimately exact between implementations that
* share the table. Zt/Zrho/p are computed statistics and are re-derived here.
* Spec (tests/eval/manifest.json):
*   dfuller_drift:   cobra dfuller --data {unitroot} --var y --timevar t   (adf:* keys)
*   pperron_default: cobra pperron --data {unitroot} --var y --timevar t   (pp:* keys)
* Stata equivalents: tsset t; dfuller y   /  pperron y   (defaults, 0 lags ADF,
* pperron default Newey-West lags).
* Data transport: tests/validate/unitroot.hbin -> unitroot_export.dta via
*   `cobra export-dta` (round-trip-gated transport).
* Run:  StataBE -b do unitroot_oracle.do -> unitroot_oracle.log
clear all
set more off
set type double
capture log close
log using "unitroot_oracle.log", replace text
display "stata_version=" c(stata_version) " born=" c(born_date)
use "../../validate/unitroot_export.dta", clear
tsset t

dfuller y
display "DFULLER_ORACLE_BEGIN"
display "ORACLE|unitroot.csv|adf:Zt|" %21.15e r(Zt)
display "ORACLEX|unitroot.csv|adf:Zt|" %21x r(Zt)
display "ORACLE|unitroot.csv|adf:cv_1|" %21.15e r(cv_1)
display "ORACLEX|unitroot.csv|adf:cv_1|" %21x r(cv_1)
display "ORACLE|unitroot.csv|adf:cv_5|" %21.15e r(cv_5)
display "ORACLEX|unitroot.csv|adf:cv_5|" %21x r(cv_5)
display "ORACLE|unitroot.csv|adf:cv_10|" %21.15e r(cv_10)
display "ORACLEX|unitroot.csv|adf:cv_10|" %21x r(cv_10)
display "ORACLE|unitroot.csv|adf:n_obs|" %21.15e r(N)
display "ORACLEX|unitroot.csv|adf:n_obs|" %21x r(N)
capture display "ORACLE|unitroot.csv|adf:lags|" %21.15e r(lags)
display "ORACLE|unitroot.csv|adf:p|" %21.15e r(p)
display "ORACLEX|unitroot.csv|adf:p|" %21x r(p)
display "DFULLER_ORACLE_END"
return list

pperron y
* pperron stores the interpolated critical values as matrix r(cvalues) (rows
* Z(rho)/Z(t), col 1 = statistic, cols 2-4 = 1%/5%/10%), and the MacKinnon
* p-value as r(pval) — not the r(cv_*)/r(p) scalars dfuller uses.
matrix C = r(cvalues)
matrix list C
display "PPERRON_ORACLE_BEGIN"
display "ORACLE|unitroot.csv|pp:Zrho|" %21.15e r(Zrho)
display "ORACLEX|unitroot.csv|pp:Zrho|" %21x r(Zrho)
display "ORACLE|unitroot.csv|pp:Zt|" %21.15e r(Zt)
display "ORACLEX|unitroot.csv|pp:Zt|" %21x r(Zt)
display "ORACLE|unitroot.csv|pp:cv_1|" %21.15e C[2,2]
display "ORACLEX|unitroot.csv|pp:cv_1|" %21x C[2,2]
display "ORACLE|unitroot.csv|pp:cv_5|" %21.15e C[2,3]
display "ORACLEX|unitroot.csv|pp:cv_5|" %21x C[2,3]
display "ORACLE|unitroot.csv|pp:cv_10|" %21.15e C[2,4]
display "ORACLEX|unitroot.csv|pp:cv_10|" %21x C[2,4]
display "ORACLE|unitroot.csv|pp:n_obs|" %21.15e r(N)
display "ORACLEX|unitroot.csv|pp:n_obs|" %21x r(N)
display "ORACLE|unitroot.csv|pp:newey_west_lags|" %21.15e r(lags)
display "ORACLEX|unitroot.csv|pp:newey_west_lags|" %21x r(lags)
display "ORACLE|unitroot.csv|pp:p|" %21.15e r(pval)
display "ORACLEX|unitroot.csv|pp:p|" %21x r(pval)
display "PPERRON_ORACLE_END"
return list
log close
