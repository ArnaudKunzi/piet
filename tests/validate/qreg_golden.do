* Run from tests/validate: every path below is relative to it.
* Quantile regression golden for cecon qreg (Frisch-Newton + iid fitted/hsheather SE).
clear all
set more off
set type double
use "csdid_input.dta", clear
file open o using "qreg_golden.csv", write replace
file write o "tau,name,b,se,sparsity" _n
foreach q in 0.25 0.5 0.75 {
  qreg visits_w age_z edu_hi, quantile(`q')
  local sp = e(sparsity)
  foreach v in age_z edu_hi _cons {
    file write o "`q',`v',`=_b[`v']',`=_se[`v']',`sp'" _n
  }
}
file close o
