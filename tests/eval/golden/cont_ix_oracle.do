* cont_ix_{cc_full,cxif,fc_full} oracle re-derivation (BUG-227 tranche 1, WS-X 2026-08-07).
* The three cont_ix_*.csv goldens were self-captured from cobra.
* Specs (tests/eval/manifest.json), all `cobra feols --data {auto} --vcov iid`:
*   cont_ix_cc_full: --y mpg --x c.weight##c.length
*   cont_ix_cxif:    --y mpg --x c.weight#i.foreign
*   cont_ix_fc_full: --y mpg --x i.foreign##c.weight
* Stata equivalents are the same factor/continuous interaction expressions under
* regress; golden key 'weight#length' maps to Stata _b[c.weight#c.length],
* 'weight#0.foreign' to _b[0.foreign#c.weight] (Stata orders the factor first),
* '1.foreign#weight' to _b[1.foreign#c.weight].
* Captured at %21.15e AND %21x hexfloat (closed-form OLS — honest bit-level
* adjudication; see feols_family_oracle.do).
* Data transport: tests/validate/auto.hbin -> auto_export.dta via
*   `cobra export-dta` (round-trip-gated transport).
* Run:  StataBE -b do cont_ix_oracle.do -> cont_ix_oracle.log
clear all
set more off
set type double
capture log close
log using "cont_ix_oracle.log", replace text
display "stata_version=" c(stata_version) " born=" c(born_date)
use "../../validate/auto_export.dta", clear

regress mpg c.weight##c.length
display "CONT_IX_CC_FULL_ORACLE_BEGIN"
display "ORACLE|cont_ix_cc_full.csv|coeftable[(intercept)].b|" %21.15e _b[_cons]
display "ORACLEX|cont_ix_cc_full.csv|coeftable[(intercept)].b|" %21x _b[_cons]
display "ORACLE|cont_ix_cc_full.csv|coeftable[(intercept)].se|" %21.15e _se[_cons]
display "ORACLEX|cont_ix_cc_full.csv|coeftable[(intercept)].se|" %21x _se[_cons]
display "ORACLE|cont_ix_cc_full.csv|coeftable[weight].b|" %21.15e _b[weight]
display "ORACLEX|cont_ix_cc_full.csv|coeftable[weight].b|" %21x _b[weight]
display "ORACLE|cont_ix_cc_full.csv|coeftable[weight].se|" %21.15e _se[weight]
display "ORACLEX|cont_ix_cc_full.csv|coeftable[weight].se|" %21x _se[weight]
display "ORACLE|cont_ix_cc_full.csv|coeftable[length].b|" %21.15e _b[length]
display "ORACLEX|cont_ix_cc_full.csv|coeftable[length].b|" %21x _b[length]
display "ORACLE|cont_ix_cc_full.csv|coeftable[length].se|" %21.15e _se[length]
display "ORACLEX|cont_ix_cc_full.csv|coeftable[length].se|" %21x _se[length]
display "ORACLE|cont_ix_cc_full.csv|coeftable[weight#length].b|" %21.15e _b[c.weight#c.length]
display "ORACLEX|cont_ix_cc_full.csv|coeftable[weight#length].b|" %21x _b[c.weight#c.length]
display "ORACLE|cont_ix_cc_full.csv|coeftable[weight#length].se|" %21.15e _se[c.weight#c.length]
display "ORACLEX|cont_ix_cc_full.csv|coeftable[weight#length].se|" %21x _se[c.weight#c.length]
display "ORACLE|cont_ix_cc_full.csv|n_obs|" %21.15e e(N)
display "ORACLE|cont_ix_cc_full.csv|dof_resid|" %21.15e e(df_r)
display "CONT_IX_CC_FULL_ORACLE_END"

regress mpg c.weight#i.foreign
display "CONT_IX_CXIF_ORACLE_BEGIN"
display "ORACLE|cont_ix_cxif.csv|coeftable[(intercept)].b|" %21.15e _b[_cons]
display "ORACLEX|cont_ix_cxif.csv|coeftable[(intercept)].b|" %21x _b[_cons]
display "ORACLE|cont_ix_cxif.csv|coeftable[(intercept)].se|" %21.15e _se[_cons]
display "ORACLEX|cont_ix_cxif.csv|coeftable[(intercept)].se|" %21x _se[_cons]
display "ORACLE|cont_ix_cxif.csv|coeftable[weight#0.foreign].b|" %21.15e _b[0.foreign#c.weight]
display "ORACLEX|cont_ix_cxif.csv|coeftable[weight#0.foreign].b|" %21x _b[0.foreign#c.weight]
display "ORACLE|cont_ix_cxif.csv|coeftable[weight#0.foreign].se|" %21.15e _se[0.foreign#c.weight]
display "ORACLEX|cont_ix_cxif.csv|coeftable[weight#0.foreign].se|" %21x _se[0.foreign#c.weight]
display "ORACLE|cont_ix_cxif.csv|coeftable[weight#1.foreign].b|" %21.15e _b[1.foreign#c.weight]
display "ORACLEX|cont_ix_cxif.csv|coeftable[weight#1.foreign].b|" %21x _b[1.foreign#c.weight]
display "ORACLE|cont_ix_cxif.csv|coeftable[weight#1.foreign].se|" %21.15e _se[1.foreign#c.weight]
display "ORACLEX|cont_ix_cxif.csv|coeftable[weight#1.foreign].se|" %21x _se[1.foreign#c.weight]
display "ORACLE|cont_ix_cxif.csv|n_obs|" %21.15e e(N)
display "ORACLE|cont_ix_cxif.csv|dof_resid|" %21.15e e(df_r)
display "CONT_IX_CXIF_ORACLE_END"

regress mpg i.foreign##c.weight
display "CONT_IX_FC_FULL_ORACLE_BEGIN"
display "ORACLE|cont_ix_fc_full.csv|coeftable[(intercept)].b|" %21.15e _b[_cons]
display "ORACLEX|cont_ix_fc_full.csv|coeftable[(intercept)].b|" %21x _b[_cons]
display "ORACLE|cont_ix_fc_full.csv|coeftable[(intercept)].se|" %21.15e _se[_cons]
display "ORACLEX|cont_ix_fc_full.csv|coeftable[(intercept)].se|" %21x _se[_cons]
display "ORACLE|cont_ix_fc_full.csv|coeftable[1.foreign].b|" %21.15e _b[1.foreign]
display "ORACLEX|cont_ix_fc_full.csv|coeftable[1.foreign].b|" %21x _b[1.foreign]
display "ORACLE|cont_ix_fc_full.csv|coeftable[1.foreign].se|" %21.15e _se[1.foreign]
display "ORACLEX|cont_ix_fc_full.csv|coeftable[1.foreign].se|" %21x _se[1.foreign]
display "ORACLE|cont_ix_fc_full.csv|coeftable[weight].b|" %21.15e _b[weight]
display "ORACLEX|cont_ix_fc_full.csv|coeftable[weight].b|" %21x _b[weight]
display "ORACLE|cont_ix_fc_full.csv|coeftable[weight].se|" %21.15e _se[weight]
display "ORACLEX|cont_ix_fc_full.csv|coeftable[weight].se|" %21x _se[weight]
display "ORACLE|cont_ix_fc_full.csv|coeftable[1.foreign#weight].b|" %21.15e _b[1.foreign#c.weight]
display "ORACLEX|cont_ix_fc_full.csv|coeftable[1.foreign#weight].b|" %21x _b[1.foreign#c.weight]
display "ORACLE|cont_ix_fc_full.csv|coeftable[1.foreign#weight].se|" %21.15e _se[1.foreign#c.weight]
display "ORACLEX|cont_ix_fc_full.csv|coeftable[1.foreign#weight].se|" %21x _se[1.foreign#c.weight]
display "ORACLE|cont_ix_fc_full.csv|n_obs|" %21.15e e(N)
display "ORACLE|cont_ix_fc_full.csv|dof_resid|" %21.15e e(df_r)
display "CONT_IX_FC_FULL_ORACLE_END"
log close
