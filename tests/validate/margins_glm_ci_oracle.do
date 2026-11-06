* Run from tests/validate: every path below is relative to it.
* WS-J margins-CI job: fresh Stata oracle for a logit margins, at() GLM case
* on the small `auto` fixture (N=74) already used by logit_scalars /
* MI231_logit_level90 — small N makes the BUG-194 t-vs-z gap clearly visible
* (t(~70) vs z differ by ~1.7%, bigger than the 400-row repro's ~0.3%).
clear all
set more off
set type double
capture log close
log using "margins_glm_ci_oracle.log", replace text

sysuse auto, clear
logit foreign mpg weight
margins, at(mpg=20 weight=3000) at(mpg=30 weight=2000) post
matrix tab = r(table)
di "MGLM_P1_B="  %22.15g tab[1,1]
di "MGLM_P1_SE=" %22.15g tab[2,1]
di "MGLM_P1_LL=" %22.15g tab[5,1]
di "MGLM_P1_UL=" %22.15g tab[6,1]
di "MGLM_P2_B="  %22.15g tab[1,2]
di "MGLM_P2_SE=" %22.15g tab[2,2]
di "MGLM_P2_LL=" %22.15g tab[5,2]
di "MGLM_P2_UL=" %22.15g tab[6,2]
di "MARGINS_GLM_CI_ORACLE_DONE_MARKER"
log close
