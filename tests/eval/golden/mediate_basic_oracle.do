* mediate_basic oracle re-derivation (BUG-227 tranche 1, WS-X 2026-08-07).
* mediate.csv was self-captured from cobra.
* Spec (tests/eval/manifest.json case mediate_basic):
*   cobra mediate --data {mediate} --y visits_w --mediator age_z --treat edu_hi
*                 --reps 1000 --seed 12345
* Checked quantities are the CLOSED-FORM product-of-coefficients pieces
* (a, b, direct, indirect, Sobel SE) — the reps/seed drive only the bootstrap
* CI, which the golden does not pin. Stata equivalent (Baron-Kenny / Sobel):
*   complete cases on (visits_w, age_z, edu_hi);
*   mediator model:  regress age_z edu_hi          -> a, se_a
*   outcome model:   regress visits_w edu_hi age_z -> b=_b[age_z], se_b,
*                    direct=_b[edu_hi]
*   indirect = a*b;  se_sobel = sqrt(a^2*se_b^2 + b^2*se_a^2)
* Data transport: tests/validate/mediate.hbin -> mediate_export.dta via
*   `cobra export-dta` (round-trip-gated transport).
* Run:  StataBE -b do mediate_basic_oracle.do -> mediate_basic_oracle.log
clear all
set more off
set type double
capture log close
log using "mediate_basic_oracle.log", replace text
display "stata_version=" c(stata_version) " born=" c(born_date)
use "../../validate/mediate_export.dta", clear
keep if !missing(visits_w, age_z, edu_hi)

regress age_z edu_hi
local a  = _b[edu_hi]
local sa = _se[edu_hi]
regress visits_w edu_hi age_z
local b   = _b[age_z]
local sb  = _se[age_z]
local dir = _b[edu_hi]
local ind = (`a')*(`b')
local sob = sqrt((`a')^2*(`sb')^2 + (`b')^2*(`sa')^2)

display "MEDIATE_BASIC_ORACLE_BEGIN"
display "ORACLE|mediate.csv|a|" %21.15e `a'
display "ORACLE|mediate.csv|se_a|" %21.15e `sa'
display "ORACLE|mediate.csv|b|" %21.15e `b'
display "ORACLE|mediate.csv|se_b|" %21.15e `sb'
display "ORACLE|mediate.csv|direct|" %21.15e `dir'
display "ORACLE|mediate.csv|indirect|" %21.15e `ind'
display "ORACLE|mediate.csv|se_sobel|" %21.15e `sob'
display "MEDIATE_BASIC_ORACLE_END"
log close
