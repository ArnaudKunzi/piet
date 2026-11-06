* xtreg_re_keepif oracle re-derivation (BUG-227 tranche 1, WS-X 2026-08-07).
* The xtreg_keepif.csv golden was self-captured from cobra (BUG-227, tolnote
* "gen_xtreg() has no independent Stata cross-check wired" = BUG-201).
* Spec (tests/eval/manifest.json case xtreg_re_keepif):
*   cobra xtreg --data {xtpois_in} --y y_int --x treated,age_z,edu_hi
*               --ivar h_pid --keepif age_z>0
* Data transport: tests/validate/xtpois_in.hbin -> xtpois_in_export.dta via
*   `cobra export-dta` (round-trip-gated transport).
* Mapping: cobra xtreg default = random effects; Stata equivalent is
*   `xtreg y x..., re` (Swamy-Arora GLS RE) with xtset h_pid.
*   --keepif age_z>0 maps to `if age_z>0` (Stata missing>0 is true, but rows
*   with missing age_z are dropped by estimation either way).
* Run:  StataBE -b do xtreg_re_keepif_oracle.do -> xtreg_re_keepif_oracle.log
clear all
set more off
set type double
capture log close
log using "xtreg_re_keepif_oracle.log", replace text
display "stata_version=" c(stata_version) " born=" c(born_date)
use "../../validate/xtpois_in_export.dta", clear
xtset h_pid
xtreg y_int treated age_z edu_hi if age_z>0, re
display "XTREG_RE_KEEPIF_ORACLE_BEGIN"
display "ORACLE|xtreg_keepif.csv|coeftable[treated].b|" %21.15e _b[treated]
display "ORACLE|xtreg_keepif.csv|coeftable[treated].se|" %21.15e _se[treated]
display "ORACLE|xtreg_keepif.csv|coeftable[age_z].b|" %21.15e _b[age_z]
display "ORACLE|xtreg_keepif.csv|coeftable[age_z].se|" %21.15e _se[age_z]
display "ORACLE|xtreg_keepif.csv|coeftable[edu_hi].b|" %21.15e _b[edu_hi]
display "ORACLE|xtreg_keepif.csv|coeftable[edu_hi].se|" %21.15e _se[edu_hi]
display "ORACLE|xtreg_keepif.csv|coeftable[_cons].b|" %21.15e _b[_cons]
display "ORACLE|xtreg_keepif.csv|coeftable[_cons].se|" %21.15e _se[_cons]
display "XTREG_RE_KEEPIF_ORACLE_END"
log close
