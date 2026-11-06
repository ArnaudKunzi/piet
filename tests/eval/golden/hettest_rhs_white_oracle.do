* hettest_rhs + hettest_white oracle re-derivation (BUG-227 tranche 1, WS-X 2026-08-07).
* hettest.csv's rhs_* and white_* keys were self-captured from cobra. The bp_*
* keys belong to case hettest_bp (not in the self-captured class) and are left
* untouched; the default hettest run below is provenance evidence only.
* Spec (tests/eval/manifest.json):
*   hettest_rhs:   cobra hettest --data {hettest} --y price --x mpg,weight,foreign --rhs
*   hettest_white: cobra hettest --data {hettest} --y price --x mpg,weight,foreign --white
* Stata equivalents after `regress price mpg weight foreign`:
*   --rhs   -> estat hettest, rhs        (Breusch-Pagan/Cook-Weisberg on all RHS)
*   --white -> estat imtest, white       (White's general test; r(chi2_h)/r(df_h)/r(p_h))
* Data transport: tests/validate/hettest.hbin -> hettest_export.dta via
*   `cobra export-dta` (round-trip-gated transport).
* Run:  StataBE -b do hettest_rhs_white_oracle.do -> hettest_rhs_white_oracle.log
clear all
set more off
set type double
capture log close
log using "hettest_rhs_white_oracle.log", replace text
display "stata_version=" c(stata_version) " born=" c(born_date)
use "../../validate/hettest_export.dta", clear
regress price mpg weight foreign

estat hettest
display "evidence only (case hettest_bp untouched): bp_chi2=" %21.15e r(chi2) " bp_p=" %21.15e r(p)
return list

estat hettest, rhs
display "HETTEST_RHS_ORACLE_BEGIN"
display "ORACLE|hettest.csv|rhs_chi2|" %21.15e r(chi2)
display "ORACLEX|hettest.csv|rhs_chi2|" %21x r(chi2)
display "ORACLE|hettest.csv|rhs_df|" %21.15e r(df)
display "ORACLEX|hettest.csv|rhs_df|" %21x r(df)
display "ORACLE|hettest.csv|rhs_p|" %21.15e r(p)
display "ORACLEX|hettest.csv|rhs_p|" %21x r(p)
display "HETTEST_RHS_ORACLE_END"
return list

estat imtest, white
* imtest returns the White heteroskedasticity leg as r(chi2)/r(df)/r(p) (its
* summary-table decomposition legs are r(chi2_h)/r(df_h) etc.; there is no
* r(p_h)). r(chi2) and r(chi2_h) are the same statistic through two arithmetic
* paths and differ only in float noise — capture both for the record, pin the
* canonical r(chi2)/r(p) pair.
display "white legs: r(chi2)=" %21.15e r(chi2) "  r(chi2_h)=" %21.15e r(chi2_h)
display "HETTEST_WHITE_ORACLE_BEGIN"
display "ORACLE|hettest.csv|white_chi2|" %21.15e r(chi2)
display "ORACLEX|hettest.csv|white_chi2|" %21x r(chi2)
display "ORACLE|hettest.csv|white_df|" %21.15e r(df)
display "ORACLEX|hettest.csv|white_df|" %21x r(df)
display "ORACLE|hettest.csv|white_p|" %21.15e r(p)
display "ORACLEX|hettest.csv|white_p|" %21x r(p)
display "HETTEST_WHITE_ORACLE_END"
return list
log close
