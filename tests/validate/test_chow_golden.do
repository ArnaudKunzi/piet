* Run from tests/validate: every path below is relative to it.
* Goldens for `cecon test` (Wald F: joint + linear restrictions) and `cecon chow`.
clear all
set more off
set type double
capture log close
log using "test_chow_golden.log", replace text
use "csdid_input.dta", clear
gen byte treated = (gvar>0 & wv>=gvar) if !missing(gvar)
recast double treated

* ---- cecon test: pooled OLS, then Wald F (default = homoskedastic) ----
regress visits_w treated age_z edu_hi

test treated age_z                       // joint H0: treated=age_z=0
di "JOINT_F=" %20.15g r(F) " DF1=" r(df) " DF2=" r(df_r) " P=" %20.15g r(p)

test treated = age_z                     // linear restriction
di "LINR_F=" %20.15g r(F) " DF1=" r(df) " DF2=" r(df_r) " P=" %20.15g r(p)

test treated + age_z = 1                 // affine restriction
di "AFFINE_F=" %20.15g r(F) " DF1=" r(df) " DF2=" r(df_r) " P=" %20.15g r(p)

* robust (HC1) joint test for cecon --vcov hc1
regress visits_w treated age_z edu_hi, robust
test treated age_z
di "JOINT_F_HC1=" %20.15g r(F) " DF1=" r(df) " DF2=" r(df_r) " P=" %20.15g r(p)

* ---- cecon chow: structural break by female (2 groups) ----
* classic homoskedastic Chow via SSR.
regress visits_w treated age_z edu_hi
scalar ssr_p = e(rss)
scalar n_all = e(N)
regress visits_w treated age_z edu_hi if female==0
scalar ssr0 = e(rss)
regress visits_w treated age_z edu_hi if female==1
scalar ssr1 = e(rss)
scalar kc = 4
scalar chow_F = ((ssr_p - (ssr0+ssr1))/kc) / ((ssr0+ssr1)/(n_all - 2*kc))
scalar chow_df2 = n_all - 2*kc
di "CHOW_F=" %20.15g chow_F " DF1=" kc " DF2=" chow_df2 " P=" %20.15g Ftail(kc, chow_df2, chow_F)

* robust Chow = HC1 Wald on interactions.
regress visits_w i.female##(c.treated c.age_z c.edu_hi), robust
testparm i.female 1.female#c.treated 1.female#c.age_z 1.female#c.edu_hi
di "CHOW_ROBUST_F=" %20.15g r(F) " DF1=" r(df) " DF2=" r(df_r) " P=" %20.15g r(p)

log close
