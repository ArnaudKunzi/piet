* ============================================================================
* xtreg_fe_scalars.do — Stata truth for SINGLE-FE panel scalars (MI-049 fill)
* Eval Harness §10 golden source.  StataNow BE 19.5.
*
* Purpose: pin r2_within / r2_between / r2_overall / F(df1,df2) / sigma_e (rmse)
* for the single-FE within estimator on the committed panel_synth fixture, so
* cobra `feols --fe id` (the reghdfe/xtreg-fe route; emits these since 0.15.1)
* is asserted against Stata — closing the MI-049 R2-family blind spot for FE.
*
* Model: xtreg y x1 x2, fe   (panel_synth, N=800, 100 units x 8 periods)
*   == cobra feols --data panel_synth.hbin --y y --x x1,x2 --fe id --vcov iid
*   (xtreg,fe sigma_e = sqrt(rss/(N-n-k)) = cobra rmse; F = F(k, N-n-k))
*
* Data: tests/validate/panel_synth.hbin (committed) converted losslessly to CSV:
*   /opt/homebrew/bin/python3 -c "import sys; sys.path.insert(0,'tools'); \
*     from hbin_read import read_hbin; n,c=read_hbin('tests/validate/panel_synth.hbin'); \
*     ks=[k for k in c if not k.endswith('__levels')]; \
*     f=open('panel_synth_full.csv','w'); f.write(','.join(ks)+chr(10)); \
*     [f.write(','.join('%.17g'%c[k][i] for k in ks)+chr(10)) for i in range(n)]"
*   (%.17g round-trips float64 exactly.)
*
* NO-GAMING CROSS-CHECK: the RE block below must reproduce the ALREADY-COMMITTED
* xtreg_re_scalars.csv golden (r2_w=.23734349 r2_b=.00083108 r2_o=.07978813
* chi2=215.41662) — proving the CSV conversion is bit-faithful to the hbin the
* engine reads. Only then are the FE numbers transcribed into the golden.
*
* Run:  cd tests/eval/golden && StataBE -b do xtreg_fe_scalars.do
* Truth values transcribed (%21.15e) into xtreg_fe_scalars.csv by the Eval
* curator, validated against this log, then committed (Constitution §10).
* ============================================================================
clear all
set more off
set type double
capture log close
log using "xtreg_fe_scalars.log", replace text

import delimited using "`c(pwd)'/panel_synth_full.csv", clear case(preserve) asdouble
xtset id t

* ---- CROSS-CHECK: RE must reproduce the committed xtreg_re_scalars.csv ----
xtreg y x1 x2, re
display "RE_CROSSCHECK_BEGIN"
display %21.15e e(r2_w)   " = e(r2_w)   want 2.373434896083940e-01"
display %21.15e e(r2_b)   " = e(r2_b)   want 8.310832933300000e-04"
display %21.15e e(r2_o)   " = e(r2_o)   want 7.978812728629730e-02"
display %21.15e e(chi2)   " = e(chi2)   want 2.154166192850350e+02"
display %21.15e e(N)      " = e(N)      want 8.000000000000000e+02"
display "RE_CROSSCHECK_END"

* ---- THE capture: single-FE within estimator scalars ----
xtreg y x1 x2, fe
display "GOLDEN_BEGIN"
display %21.15e e(r2_w)    " = e(r2_w)    within R-squared  (cobra r2_within)"
display %21.15e e(r2_b)    " = e(r2_b)    between R-squared (cobra r2_between)"
display %21.15e e(r2_o)    " = e(r2_o)    overall R-squared (cobra r2_overall)"
display %21.15e e(F)       " = e(F)       header model F(k, N-n-k)  (cobra F)"
display %21.15e e(df_m)    " = e(df_m)    NB: xtreg counts FE dof: k + N_g - 1 = 101 (cobra df_m = k = 2)"
display %21.15e e(df_r)    " = e(df_r)    residual dof N-n-k (cobra dof_resid)"
display %21.15e e(N)       " = e(N)       observations (cobra n_obs)"
display %21.15e e(N_g)     " = e(N_g)     groups (cobra k_absorbed)"
display %21.15e e(sigma_e) " = e(sigma_e) sqrt(rss/(N-n-k)) (cobra rmse)"
display %21.15e e(df_m)-(e(N_g)-1) " = k = header F numerator dof (cobra df_m)"
display %21.15e Ftail(e(df_m)-(e(N_g)-1), e(df_r), e(F)) " = Prob>F at header dof (k, N-n-k) (cobra p_F)"
display %21.15e _b[x1]     " = _b[x1]"
display %21.15e _se[x1]    " = _se[x1]"
display %21.15e _b[x2]     " = _b[x2]"
display %21.15e _se[x2]    " = _se[x2]"
display "GOLDEN_END"

log close
