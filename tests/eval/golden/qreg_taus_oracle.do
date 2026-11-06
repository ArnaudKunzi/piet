* qreg_tau0p25 / qreg_tau0p5 / qreg_tau0p75 oracle re-derivation
* (BUG-227 tranche 1, WS-X 2026-08-07). qreg.csv was self-captured from cobra.
* Spec (tests/eval/manifest.json):
*   cobra qreg --data {csdid_input} --y visits_w --x age_z,edu_hi --tau {0.25,0.5,0.75}
* Stata equivalent: qreg visits_w age_z edu_hi, quantile(tau)  — default
* vce(iid) sparsity-function SE (the manifest's T4 tolnote names Stata's
* default kernel as the definition being matched).
* sparsity key: written only when Stata returns e(sparsity)/e(bwidth)-derived
* value; the eval checks read only b/se, sparsity is provenance.
* Data transport: tests/validate/csdid_input.hbin -> csdid_input_export.dta via
*   `cobra export-dta` (round-trip-gated transport).
* Run:  StataBE -b do qreg_taus_oracle.do -> qreg_taus_oracle.log
clear all
set more off
set type double
capture log close
log using "qreg_taus_oracle.log", replace text
display "stata_version=" c(stata_version) " born=" c(born_date)
use "../../validate/csdid_input_export.dta", clear

foreach tau in 25 5 75 {
    local q = 0.`tau'
    qreg visits_w age_z edu_hi, quantile(`q')
    local tag = "tau0p`tau'"
    display "QREG_`tag'_ORACLE_BEGIN"
    display "ORACLE|qreg.csv|`tag':coeftable[age_z].b|" %21.15e _b[age_z]
    display "ORACLE|qreg.csv|`tag':coeftable[age_z].se|" %21.15e _se[age_z]
    display "ORACLE|qreg.csv|`tag':coeftable[edu_hi].b|" %21.15e _b[edu_hi]
    display "ORACLE|qreg.csv|`tag':coeftable[edu_hi].se|" %21.15e _se[edu_hi]
    display "ORACLE|qreg.csv|`tag':coeftable[(intercept)].b|" %21.15e _b[_cons]
    display "ORACLE|qreg.csv|`tag':coeftable[(intercept)].se|" %21.15e _se[_cons]
    capture display "ORACLE|qreg.csv|`tag':sparsity|" %21.15e e(sparsity)
    display "QREG_`tag'_ORACLE_END"
    ereturn list
}
log close
