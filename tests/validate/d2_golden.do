* External research data, not shipped: set global HEALTH_PANEL_DIR to its directory first.
* Run from tests/validate: every path below is relative to it.
* D2 golden: exercise every cecon D2 op on the real 건강심리 panel and export
* both the inputs and Stata's computed outputs (double precision) for diffing.
clear all
set more off
set type double
capture log close
log using "d2_golden.log", replace text

use h_pid wv la visits age h_g6 using "$HEALTH_PANEL_DIR/STATA/analysis_panel.dta", clear
* ensure inputs are double for an exact cross-engine comparison
recast double h_pid wv la visits age h_g6
xtset h_pid wv

* --- bysort + gen subscripting ---
bysort h_pid (wv): gen c_first_in = wv[1]
bysort h_pid (wv): gen c_lag_la   = la[_n-1]
bysort h_pid (wv): gen c_n        = _n
bysort h_pid (wv): gen c_N        = _N

* --- egen by-group (with cond() expression args) ---
bysort h_pid (wv): egen c_base_la = max(cond(wv==c_first_in, la, .))
bysort h_pid (wv): egen c_onset   = min(cond(la==1, wv, .))
egen c_ever_la = max(la), by(h_pid)
egen c_sum_la  = sum(la), by(h_pid)
egen c_cnt_v   = count(visits), by(h_pid)
egen c_mean_v  = mean(visits), by(h_pid)
egen c_sd_v    = sd(visits), by(h_pid)
bysort wv: egen c_med_la = median(la)

* --- conditional replace (gvar-style) ---
gen c_gvar = 0
replace c_gvar = c_onset if (c_ever_la==1 & c_base_la==0 & !missing(c_onset))

* --- summarize, detail -> winsorize p99 (and capture the scalars) ---
quietly summarize visits, detail
scalar P99 = r(p99)
scalar P1  = r(p1)
scalar P25 = r(p25)
scalar P50 = r(p50)
scalar P75 = r(p75)
scalar VMEAN = r(mean)
scalar VSD   = r(sd)
di "SUMM_DETAIL visits: p1=" %20.15g P1 " p25=" %20.15g P25 " p50=" %20.15g P50 ///
   " p75=" %20.15g P75 " p99=" %20.15g P99 " mean=" %20.15g VMEAN " sd=" %20.15g VSD
gen c_visits_w = visits
replace c_visits_w = P99 if visits>P99 & !missing(visits)

* --- standardize ---
quietly summarize age
gen c_age_z = (age - r(mean))/r(sd)

* --- expression funcs ---
gen c_ln_visits = ln(1+visits)
gen c_edu_hi    = (h_g6>=4) if !missing(h_g6)
gen c_inrange_v = inrange(visits, 1, 10)
gen c_inlist_e  = inlist(h_g6, 4, 5, 6)

di "D2_N=" _N
keep h_pid wv la visits age h_g6 c_*
save "d2_golden.dta", replace
log close
