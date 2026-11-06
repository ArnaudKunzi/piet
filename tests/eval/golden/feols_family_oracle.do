* feols_iid + feols_scalars_iid + feols_scalars_hc1 oracle re-derivation
* (BUG-227 tranche 1, WS-X 2026-08-07). feols.csv / feols_scalars_iid.csv /
* feols_scalars_hc1.csv were self-captured from cobra.
* Specs (tests/eval/manifest.json):
*   feols_iid:         cobra feols --data {auto} --y price --x mpg,weight,foreign --vcov iid
*   feols_scalars_iid: cobra feols --data {auto} --y price --x mpg,weight --vcov iid
*   feols_scalars_hc1: cobra feols --data {auto} --y price --x mpg,weight --vcov hc1
* Stata equivalents: regress price ... ; vce(robust) IS HC1 for regress.
* Mapping notes:
*   - feols.csv key r2_within: cobra feols with no absorbed FE reports
*     r2_within == r2; both map to e(r2) (centered R^2 — see the earlier
*     feols_iid_r2.do correction, which established e(r2) as the truth here).
*   - p_F is not returned by regress; computed as Ftail(e(df_m),e(df_r),e(F)).
* Precision: closed-form OLS quantities can legitimately agree bit-for-bit
* across implementations, so 15-digit %21.15e capture cannot distinguish
* "bit-identical" from "self-captured". This family therefore ALSO emits
* ORACLEX| lines in Stata %21x hexfloat (the full IEEE-754 double); the
* rewrite tool prefers those.
* Data transport: tests/validate/auto.hbin -> auto_export.dta via
*   `cobra export-dta` (round-trip-gated transport).
* Run:  StataBE -b do feols_family_oracle.do -> feols_family_oracle.log
clear all
set more off
set type double
capture log close
log using "feols_family_oracle.log", replace text
display "stata_version=" c(stata_version) " born=" c(born_date)
use "../../validate/auto_export.dta", clear

regress price mpg weight foreign
display "FEOLS_IID_ORACLE_BEGIN"
display "ORACLE|feols.csv|coeftable[mpg].b|" %21.15e _b[mpg]
display "ORACLEX|feols.csv|coeftable[mpg].b|" %21x _b[mpg]
display "ORACLE|feols.csv|coeftable[mpg].se|" %21.15e _se[mpg]
display "ORACLEX|feols.csv|coeftable[mpg].se|" %21x _se[mpg]
display "ORACLE|feols.csv|coeftable[weight].b|" %21.15e _b[weight]
display "ORACLEX|feols.csv|coeftable[weight].b|" %21x _b[weight]
display "ORACLE|feols.csv|coeftable[weight].se|" %21.15e _se[weight]
display "ORACLEX|feols.csv|coeftable[weight].se|" %21x _se[weight]
display "ORACLE|feols.csv|coeftable[foreign].b|" %21.15e _b[foreign]
display "ORACLEX|feols.csv|coeftable[foreign].b|" %21x _b[foreign]
display "ORACLE|feols.csv|coeftable[foreign].se|" %21.15e _se[foreign]
display "ORACLEX|feols.csv|coeftable[foreign].se|" %21x _se[foreign]
display "ORACLE|feols.csv|coeftable[(intercept)].b|" %21.15e _b[_cons]
display "ORACLEX|feols.csv|coeftable[(intercept)].b|" %21x _b[_cons]
display "ORACLE|feols.csv|coeftable[(intercept)].se|" %21.15e _se[_cons]
display "ORACLEX|feols.csv|coeftable[(intercept)].se|" %21x _se[_cons]
display "ORACLE|feols.csv|n_obs|" %21.15e e(N)
display "ORACLEX|feols.csv|n_obs|" %21x e(N)
display "ORACLE|feols.csv|r2_within|" %21.15e e(r2)
display "ORACLEX|feols.csv|r2_within|" %21x e(r2)
display "ORACLE|feols.csv|r2|" %21.15e e(r2)
display "ORACLEX|feols.csv|r2|" %21x e(r2)
display "ORACLE|feols.csv|rmse|" %21.15e e(rmse)
display "ORACLEX|feols.csv|rmse|" %21x e(rmse)
display "ORACLE|feols.csv|dof_resid|" %21.15e e(df_r)
display "ORACLEX|feols.csv|dof_resid|" %21x e(df_r)
display "FEOLS_IID_ORACLE_END"

regress price mpg weight
local pF = Ftail(e(df_m), e(df_r), e(F))
display "FEOLS_SCALARS_IID_ORACLE_BEGIN"
display "ORACLE|feols_scalars_iid.csv|r2|" %21.15e e(r2)
display "ORACLEX|feols_scalars_iid.csv|r2|" %21x e(r2)
display "ORACLE|feols_scalars_iid.csv|adj_r2|" %21.15e e(r2_a)
display "ORACLEX|feols_scalars_iid.csv|adj_r2|" %21x e(r2_a)
display "ORACLE|feols_scalars_iid.csv|F|" %21.15e e(F)
display "ORACLEX|feols_scalars_iid.csv|F|" %21x e(F)
display "ORACLE|feols_scalars_iid.csv|p_F|" %21.15e `pF'
display "ORACLEX|feols_scalars_iid.csv|p_F|" %21x `pF'
display "ORACLE|feols_scalars_iid.csv|rmse|" %21.15e e(rmse)
display "ORACLEX|feols_scalars_iid.csv|rmse|" %21x e(rmse)
display "ORACLE|feols_scalars_iid.csv|mss|" %21.15e e(mss)
display "ORACLEX|feols_scalars_iid.csv|mss|" %21x e(mss)
display "ORACLE|feols_scalars_iid.csv|rss|" %21.15e e(rss)
display "ORACLEX|feols_scalars_iid.csv|rss|" %21x e(rss)
display "ORACLE|feols_scalars_iid.csv|df_m|" %21.15e e(df_m)
display "ORACLEX|feols_scalars_iid.csv|df_m|" %21x e(df_m)
display "ORACLE|feols_scalars_iid.csv|df_r|" %21.15e e(df_r)
display "ORACLEX|feols_scalars_iid.csv|df_r|" %21x e(df_r)
display "ORACLE|feols_scalars_iid.csv|n_obs|" %21.15e e(N)
display "ORACLEX|feols_scalars_iid.csv|n_obs|" %21x e(N)
display "FEOLS_SCALARS_IID_ORACLE_END"

regress price mpg weight, vce(robust)
local pF = Ftail(e(df_m), e(df_r), e(F))
display "FEOLS_SCALARS_HC1_ORACLE_BEGIN"
display "ORACLE|feols_scalars_hc1.csv|r2|" %21.15e e(r2)
display "ORACLEX|feols_scalars_hc1.csv|r2|" %21x e(r2)
display "ORACLE|feols_scalars_hc1.csv|adj_r2|" %21.15e e(r2_a)
display "ORACLEX|feols_scalars_hc1.csv|adj_r2|" %21x e(r2_a)
display "ORACLE|feols_scalars_hc1.csv|F|" %21.15e e(F)
display "ORACLEX|feols_scalars_hc1.csv|F|" %21x e(F)
display "ORACLE|feols_scalars_hc1.csv|p_F|" %21.15e `pF'
display "ORACLEX|feols_scalars_hc1.csv|p_F|" %21x `pF'
display "ORACLE|feols_scalars_hc1.csv|rmse|" %21.15e e(rmse)
display "ORACLEX|feols_scalars_hc1.csv|rmse|" %21x e(rmse)
display "ORACLE|feols_scalars_hc1.csv|mss|" %21.15e e(mss)
display "ORACLEX|feols_scalars_hc1.csv|mss|" %21x e(mss)
display "ORACLE|feols_scalars_hc1.csv|rss|" %21.15e e(rss)
display "ORACLEX|feols_scalars_hc1.csv|rss|" %21x e(rss)
display "ORACLE|feols_scalars_hc1.csv|df_m|" %21.15e e(df_m)
display "ORACLEX|feols_scalars_hc1.csv|df_m|" %21x e(df_m)
display "ORACLE|feols_scalars_hc1.csv|df_r|" %21.15e e(df_r)
display "ORACLEX|feols_scalars_hc1.csv|df_r|" %21x e(df_r)
display "ORACLE|feols_scalars_hc1.csv|n_obs|" %21.15e e(N)
display "ORACLEX|feols_scalars_hc1.csv|n_obs|" %21x e(N)
display "FEOLS_SCALARS_HC1_ORACLE_END"
log close
