* summ_fweight_auto + C1F20a_round2arg + C1F19_pmzero_fe oracle re-derivation /
* strengthening (BUG-227 tranche 1, WS-X 2026-08-07).
* - fweight_summ_auto.csv was self-captured: recaptured here from
*   `summarize price [fw=mpg]` (spec: cobra eval --data {auto} --summarize price
*   --fweight mpg).
* - f20a_round2.csv (spec: --gen z=round(2.567, 0.01) --summarize z) had a
*   committed 3-digit warrant in f19_f20_oracle.log ("F20a_round=2.57");
*   recaptured at full precision so the mean/min pins carry a real double.
* - f19_pmzero.csv coef.x already had external authority (f19_f20_oracle.log
*   _b[x]=2.7272727 = 30/11 analytic); recaptured at %21x hexfloat to upgrade
*   the 7-displayed-digit warrant to the full double. Fixture is the COMMITTED
*   golden/f19_pmzero_input.csv (import delimited; Stata reads -0.0 as 0 ->
*   one zero group, the very semantics the golden pins).
* All numeric keys captured at %21.15e AND %21x hexfloat.
* Run:  StataBE -b do misc_fw_round_pmzero_oracle.do -> misc_fw_round_pmzero_oracle.log
clear all
set more off
set type double
capture log close
log using "misc_fw_round_pmzero_oracle.log", replace text
display "stata_version=" c(stata_version) " born=" c(born_date)

use "../../validate/auto_export.dta", clear
summarize price [fw=mpg]
display "SUMM_FWEIGHT_ORACLE_BEGIN"
display "ORACLE|fweight_summ_auto.csv|N|" %21.15e r(N)
display "ORACLE|fweight_summ_auto.csv|mean|" %21.15e r(mean)
display "ORACLEX|fweight_summ_auto.csv|mean|" %21x r(mean)
display "ORACLE|fweight_summ_auto.csv|sd|" %21.15e r(sd)
display "ORACLEX|fweight_summ_auto.csv|sd|" %21x r(sd)
display "ORACLE|fweight_summ_auto.csv|min|" %21.15e r(min)
display "ORACLE|fweight_summ_auto.csv|max|" %21.15e r(max)
display "SUMM_FWEIGHT_ORACLE_END"

clear
set obs 1
gen double z = round(2.567, 0.01)
summarize z
display "C1F20A_ROUND_ORACLE_BEGIN"
display "ORACLE|f20a_round2.csv|summarize.z.mean|" %21.15e r(mean)
display "ORACLEX|f20a_round2.csv|summarize.z.mean|" %21x r(mean)
display "ORACLE|f20a_round2.csv|summarize.z.min|" %21.15e r(min)
display "ORACLEX|f20a_round2.csv|summarize.z.min|" %21x r(min)
display "C1F20A_ROUND_ORACLE_END"

import delimited "f19_pmzero_input.csv", clear case(preserve)
tab g
areg y x, absorb(g)
display "C1F19_PMZERO_ORACLE_BEGIN"
display "ORACLE|f19_pmzero.csv|coef.x|" %21.15e _b[x]
display "ORACLEX|f19_pmzero.csv|coef.x|" %21x _b[x]
display "absorbed group count (expect 2): " e(df_a)+1
display "C1F19_PMZERO_ORACLE_END"
log close
