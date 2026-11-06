* fvops_{ib2,ibn,inumlist,omit} oracle re-derivation (BUG-227 tranche 1, WS-X 2026-08-07).
* The four fvops_*.csv goldens were self-captured from cobra.
* Specs (tests/eval/manifest.json), all `cobra feols --data {auto} --vcov iid`:
*   fvops_ib2:      --y price --x ib2.rep78
*   fvops_ibn:      --y price --x ibn.rep78     (constant kept; Stata drops one level)
*   fvops_inumlist: --y price --x i(2/4).rep78  (level-selected indicators, base 2)
*   fvops_omit:     --y price --x o.rep78,foreign (rep78 omitted, still marks out its missings)
* Stata equivalents are the same factor-operator expressions under regress.
* Goldens pin b's only (+ integer n_obs/dof_resid); captured at %21.15e AND %21x
* hexfloat (closed-form OLS legitimately bit-matches; hexfloat makes the
* adjudication honest — see feols_family_oracle.do).
* Data transport: tests/validate/auto.hbin -> auto_export.dta via
*   `cobra export-dta` (round-trip-gated transport).
* Run:  StataBE -b do fvops_oracle.do -> fvops_oracle.log
clear all
set more off
set type double
capture log close
log using "fvops_oracle.log", replace text
display "stata_version=" c(stata_version) " born=" c(born_date)
use "../../validate/auto_export.dta", clear

regress price ib2.rep78
display "FVOPS_IB2_ORACLE_BEGIN"
display "ORACLE|fvops_ib2.csv|coeftable[(intercept)].b|" %21.15e _b[_cons]
display "ORACLEX|fvops_ib2.csv|coeftable[(intercept)].b|" %21x _b[_cons]
display "ORACLE|fvops_ib2.csv|coeftable[1.rep78].b|" %21.15e _b[1.rep78]
display "ORACLEX|fvops_ib2.csv|coeftable[1.rep78].b|" %21x _b[1.rep78]
display "ORACLE|fvops_ib2.csv|coeftable[3.rep78].b|" %21.15e _b[3.rep78]
display "ORACLEX|fvops_ib2.csv|coeftable[3.rep78].b|" %21x _b[3.rep78]
display "ORACLE|fvops_ib2.csv|coeftable[4.rep78].b|" %21.15e _b[4.rep78]
display "ORACLEX|fvops_ib2.csv|coeftable[4.rep78].b|" %21x _b[4.rep78]
display "ORACLE|fvops_ib2.csv|coeftable[5.rep78].b|" %21.15e _b[5.rep78]
display "ORACLEX|fvops_ib2.csv|coeftable[5.rep78].b|" %21x _b[5.rep78]
display "ORACLE|fvops_ib2.csv|n_obs|" %21.15e e(N)
display "ORACLE|fvops_ib2.csv|dof_resid|" %21.15e e(df_r)
display "FVOPS_IB2_ORACLE_END"

regress price ibn.rep78
display "FVOPS_IBN_ORACLE_BEGIN"
display "ORACLE|fvops_ibn.csv|coeftable[(intercept)].b|" %21.15e _b[_cons]
display "ORACLEX|fvops_ibn.csv|coeftable[(intercept)].b|" %21x _b[_cons]
display "ORACLE|fvops_ibn.csv|coeftable[1.rep78].b|" %21.15e _b[1.rep78]
display "ORACLEX|fvops_ibn.csv|coeftable[1.rep78].b|" %21x _b[1.rep78]
display "ORACLE|fvops_ibn.csv|coeftable[2.rep78].b|" %21.15e _b[2.rep78]
display "ORACLEX|fvops_ibn.csv|coeftable[2.rep78].b|" %21x _b[2.rep78]
display "ORACLE|fvops_ibn.csv|coeftable[3.rep78].b|" %21.15e _b[3.rep78]
display "ORACLEX|fvops_ibn.csv|coeftable[3.rep78].b|" %21x _b[3.rep78]
display "ORACLE|fvops_ibn.csv|coeftable[4.rep78].b|" %21.15e _b[4.rep78]
display "ORACLEX|fvops_ibn.csv|coeftable[4.rep78].b|" %21x _b[4.rep78]
display "ORACLE|fvops_ibn.csv|n_obs|" %21.15e e(N)
display "ORACLE|fvops_ibn.csv|dof_resid|" %21.15e e(df_r)
display "FVOPS_IBN_ORACLE_END"

regress price i(2/4).rep78
display "FVOPS_INUMLIST_ORACLE_BEGIN"
display "ORACLE|fvops_inumlist.csv|coeftable[(intercept)].b|" %21.15e _b[_cons]
display "ORACLEX|fvops_inumlist.csv|coeftable[(intercept)].b|" %21x _b[_cons]
display "ORACLE|fvops_inumlist.csv|coeftable[3.rep78].b|" %21.15e _b[3.rep78]
display "ORACLEX|fvops_inumlist.csv|coeftable[3.rep78].b|" %21x _b[3.rep78]
display "ORACLE|fvops_inumlist.csv|coeftable[4.rep78].b|" %21.15e _b[4.rep78]
display "ORACLEX|fvops_inumlist.csv|coeftable[4.rep78].b|" %21x _b[4.rep78]
display "ORACLE|fvops_inumlist.csv|n_obs|" %21.15e e(N)
display "ORACLE|fvops_inumlist.csv|dof_resid|" %21.15e e(df_r)
display "FVOPS_INUMLIST_ORACLE_END"

regress price o.rep78 foreign
display "FVOPS_OMIT_ORACLE_BEGIN"
display "ORACLE|fvops_omit.csv|coeftable[(intercept)].b|" %21.15e _b[_cons]
display "ORACLEX|fvops_omit.csv|coeftable[(intercept)].b|" %21x _b[_cons]
display "ORACLE|fvops_omit.csv|coeftable[foreign].b|" %21.15e _b[foreign]
display "ORACLEX|fvops_omit.csv|coeftable[foreign].b|" %21x _b[foreign]
display "ORACLE|fvops_omit.csv|n_obs|" %21.15e e(N)
display "ORACLE|fvops_omit.csv|dof_resid|" %21.15e e(df_r)
display "FVOPS_OMIT_ORACLE_END"
log close
