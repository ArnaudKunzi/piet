* ============================================================================
* iv_scalars_oracle.do — Stata truth for ivregress MODEL-LEVEL scalars (MI-049)
* Eval Harness §10 golden source.  StataNow BE 19.5.
*
* Purpose: capture e(r2) / e(rmse) / model F (+dof, p) for the ivdiag 2SLS/LIML
* models the harness already asserts (liml_basic case + iv command route).
* cobra's `iv` command emits n_obs/dof_resid/t_dof only and `liml` emits
* sigma2/n_obs/df_r/kappa — NEITHER emits r2/rmse/F, so these cells are stored
* with the *_NOT_EMITTED convention (pending engine emission; assertion is one
* flip away once cobra surfaces them). The maintainer routes the emission request to
* CoBrA Op.
*
* Model (== the manifest liml_basic case, literal flag reading:
*        --endog treated --instruments female,edu_hi --x age_z):
*   A: ivregress 2sls visits_w age_z (treated = female edu_hi), small
*   B: ivregress liml visits_w age_z (treated = female edu_hi), small
* Data: ivdiag_input.dta == tests/validate ivdiag.hbin fixture (N=73079).
*
* NO-GAMING CROSS-CHECK: block B re-displays _b/_se for treated/age_z/_cons —
* they must reproduce the ALREADY-COMMITTED liml.csv golden
* (b_treated=6.866261946381447e+01, se_treated=2.217625000163225e+00, ...),
* proving this .dta and model are the same behind the committed goldens.
*
* Run:  cd tests/eval/golden && StataBE -b do iv_scalars_oracle.do
* Values transcribed (%21.15e) into liml.csv as *_NOT_EMITTED keys by the Eval
* curator, validated against this log, then committed (Constitution §10).
* ============================================================================
clear all
set more off
set type double
capture log close
log using "iv_scalars_oracle.log", replace text

use "../../validate/ivdiag_input.dta", clear

* ---- Model A: 2SLS with small-sample stats (F / t) ----
ivregress 2sls visits_w age_z (treated = female edu_hi), small
display "TSLS_SCALARS_BEGIN"
display %21.15e e(r2)    " = e(r2)    2sls R-squared (. = negative, suppressed)"
display %21.15e e(rmse)  " = e(rmse)  2sls Root MSE"
display %21.15e e(F)     " = e(F)     2sls model F(df_m, df_r)"
display %21.15e e(df_m)  " = e(df_m)"
display %21.15e e(df_r)  " = e(df_r)"
display %21.15e Ftail(e(df_m),e(df_r),e(F)) " = Prob > F"
display %21.15e e(N)     " = e(N)"
display "TSLS_SCALARS_END"

* ---- Model B: LIML, NO small — the committed liml_basic convention ----
* (liml.csv has small=0 and sigma2=rss/N: the case is the asymptotic-z variant.
*  The small-run SEs differ by exactly sqrt(N/(N-k)); coefs are identical.)
ivregress liml visits_w age_z (treated = female edu_hi)
display "LIML_CROSSCHECK_BEGIN"
display %21.15e _b[treated]  " = _b[treated]   want committed 6.866261946381447e+01"
display %21.15e _se[treated] " = _se[treated]  want committed 2.217625000163225e+00"
display %21.15e _b[age_z]    " = _b[age_z]     want committed -4.960176115143728e+00"
display %21.15e _se[age_z]   " = _se[age_z]    want committed 2.230620086555116e-01"
display %21.15e _b[_cons]    " = _b[_cons]     want committed 1.790706482488271e+01"
display %21.15e _se[_cons]   " = _se[_cons]    want committed 3.192134937244138e-01"
display "LIML_CROSSCHECK_END"
display "LIML_SCALARS_BEGIN"
display %21.15e e(r2)    " = e(r2)    liml R-squared (. = negative, suppressed)"
display %21.15e e(rmse)  " = e(rmse)  liml Root MSE (no-small: sqrt(rss/N) domain)"
display %21.15e e(rss)   " = e(rss)"
display %21.15e e(chi2)  " = e(chi2)  Wald chi2 (asymptotic header test)"
display %21.15e e(df_m)  " = e(df_m)"
display %21.15e chi2tail(e(df_m),e(chi2)) " = Prob > chi2"
display %21.15e e(N)     " = e(N)"
display "LIML_SCALARS_END"

* ---- Model B': LIML with small (F/t variant, for the record) ----
ivregress liml visits_w age_z (treated = female edu_hi), small
display "LIML_SMALL_SCALARS_BEGIN"
display %21.15e e(rmse)  " = e(rmse)  liml Root MSE (small: sqrt(rss/(N-k)))"
display %21.15e e(F)     " = e(F)     liml model F(df_m, df_r)"
display %21.15e e(df_m)  " = e(df_m)"
display %21.15e e(df_r)  " = e(df_r)"
display %21.15e Ftail(e(df_m),e(df_r),e(F)) " = Prob > F"
display "LIML_SMALL_SCALARS_END"

log close
