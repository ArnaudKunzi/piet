* ============================================================================
* g3_logit_probit_vce.do — Stata truth for logit/probit vce(robust) + vce(cluster)
* Eval Harness §10 golden source.  StataNow BE 19.5, sysuse auto.
* COUNCIL-20260710 golden batch #1 — G3 (PLAYBOOK.md §7.2 item 3).
*
* Purpose: pin the CORRECT robust/cluster SEs for logit/probit so the engine's
* current omission of Stata's small-sample factor (F15: vce(robust) sandwich
* uses factor 1.0 instead of N/(N-1)) cannot ship silently. Also pins vce(cluster)
* SEs -- the engine currently has NO cluster support in cmd_logit/cmd_probit at
* all (--vce cluster silently falls through to the IID bread, see F4/P3), so
* this golden additionally gates the eventual real cluster-for-MLE estimator work.
*
* Model: logit/probit foreign mpg weight   (auto.dta, n=74; matches the existing
*   committed logit_scalars.csv / probit_scalars.csv IID goldens' X vars exactly,
*   so this golden is a same-model companion, not a new model).
*   == cobra logit  --data auto.hbin --y foreign --x mpg,weight --vce robust
*   == cobra logit  --data auto.hbin --y foreign --x mpg,weight --vce cluster --cluster rep78
*   == cobra probit --data auto.hbin --y foreign --x mpg,weight --vce robust
*   == cobra probit --data auto.hbin --y foreign --x mpg,weight --vce cluster --cluster rep78
*
* NO-GAMING CROSS-CHECK: the IID coefficients (b, not se) below must match the
* ALREADY-COMMITTED logit_scalars.csv/probit_scalars.csv ll/ll_0 model (same y/x),
* proving this is the same model the IID golden already pins.
*
* Run:  cd tests/eval/golden && StataBE -b do g3_logit_probit_vce.do
* Truth values transcribed (%21.15e) into g3_logit_probit_vce.csv by the Eval
* curator, validated against this log, then committed (Constitution Art 10).
* ============================================================================
clear all
set more off
set type double
capture log close
log using "g3_logit_probit_vce.log", replace text

sysuse auto, clear

* ---- NO-GAMING CROSS-CHECK: reproduce the committed logit_scalars.csv model ----
quietly logit foreign mpg weight
display "IID_CROSSCHECK_BEGIN"
display %21.15e e(ll)   " = e(ll)   want -2.717515619912890e+01 (logit_scalars.csv)"
display %21.15e e(ll_0) " = e(ll_0) want -4.503320955699140e+01 (logit_scalars.csv)"
display "IID_CROSSCHECK_END"

* ============================================================================
* LOGIT — vce(robust)
* ============================================================================
logit foreign mpg weight, vce(robust)
display "LOGIT_ROBUST_BEGIN"
display %21.15e e(N)       " = e(N)"
display %21.15e _b[mpg]    " = b_mpg"
display %21.15e _se[mpg]   " = se_mpg  (F15 discriminator: engine omits N/(N-1))"
display %21.15e _b[weight] " = b_weight"
display %21.15e _se[weight] " = se_weight"
display %21.15e _b[_cons]  " = b_cons"
display %21.15e _se[_cons] " = se_cons"
display "LOGIT_ROBUST_END"

* ============================================================================
* LOGIT — vce(cluster rep78)
* ============================================================================
logit foreign mpg weight, vce(cluster rep78)
display "LOGIT_CLUSTER_BEGIN"
display %21.15e e(N)        " = e(N)"
display %21.15e e(N_clust)  " = e(N_clust)"
display %21.15e _b[mpg]     " = b_mpg"
display %21.15e _se[mpg]    " = se_mpg  (engine currently has NO cluster support -- silently IID)"
display %21.15e _b[weight]  " = b_weight"
display %21.15e _se[weight] " = se_weight"
display %21.15e _b[_cons]   " = b_cons"
display %21.15e _se[_cons]  " = se_cons"
display "LOGIT_CLUSTER_END"

* ============================================================================
* PROBIT — vce(robust)
* ============================================================================
probit foreign mpg weight, vce(robust)
display "PROBIT_ROBUST_BEGIN"
display %21.15e e(N)       " = e(N)"
display %21.15e _b[mpg]    " = b_mpg"
display %21.15e _se[mpg]   " = se_mpg  (F15 discriminator)"
display %21.15e _b[weight] " = b_weight"
display %21.15e _se[weight] " = se_weight"
display %21.15e _b[_cons]  " = b_cons"
display %21.15e _se[_cons] " = se_cons"
display "PROBIT_ROBUST_END"

* ============================================================================
* PROBIT — vce(cluster rep78)
* ============================================================================
probit foreign mpg weight, vce(cluster rep78)
display "PROBIT_CLUSTER_BEGIN"
display %21.15e e(N)        " = e(N)"
display %21.15e e(N_clust)  " = e(N_clust)"
display %21.15e _b[mpg]     " = b_mpg"
display %21.15e _se[mpg]    " = se_mpg  (engine currently has NO cluster support)"
display %21.15e _b[weight]  " = b_weight"
display %21.15e _se[weight] " = se_weight"
display %21.15e _b[_cons]   " = b_cons"
display %21.15e _se[_cons]  " = se_cons"
display "PROBIT_CLUSTER_END"

log close
