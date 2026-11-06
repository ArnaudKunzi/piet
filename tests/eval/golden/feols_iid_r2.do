* Stata truth for the existing feols_iid golden model's CENTERED R^2.
* Model: regress price mpg weight foreign  (auto.dta, no FE).
* The feols.csv golden stored an UNCENTERED r2_within (0.90782257) captured from
* the buggy pre-fix engine; the centered truth is e(r2) below. Eval §10 correction.
* Run:  StataBE -b do feols_iid_r2.do  -> feols_iid_r2.log
clear all
set more off
capture log close
log using "feols_iid_r2.log", replace text
sysuse auto, clear
regress price mpg weight foreign
display "FEOLS_IID_R2_BEGIN"
display %21.15e e(r2)   " = e(r2)   centered R-squared"
display %21.15e e(rmse) " = e(rmse) Root MSE"
display %21.15e e(N)    " = e(N)"
display %21.15e e(df_r) " = e(df_r)"
display "FEOLS_IID_R2_END"
log close
