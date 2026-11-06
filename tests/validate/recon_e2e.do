* External research data, not shipped: set global ILL_WINDS_DTA_DIR to its directory first.
clear all
set more off
* Run from tests/validate: every path below is relative to it.
capture log close
log using "recon_e2e.log", replace text

use geo_id date year doy PM10_mean deaths_cardio lnrate_cardio pop t2m_mean_C tp_sum_mm rh_mean wind_speed ///
    using "$ILL_WINDS_DTA_DIR/wind_iv_full_panel.dta", clear
keep if !missing(PM10_mean)

* --- data prep (the part cecon can't do yet: tssmooth/gen) ---
xtset geo_id date
tssmooth ma pm10_ma15 = PM10_mean, window(7 1 7)
forvalues k = 1/3 {
    gen sin`k' = sin(2*_pi*`k'*doy/365.25)
    gen cos`k' = cos(2*_pi*`k'*doy/365.25)
}
gen lpop = ln(pop)
gen t2m_sq = t2m_mean_C*t2m_mean_C

* common sample for a fair cecon-vs-Stata comparison
egen _miss = rowmiss(lnrate_cardio deaths_cardio pm10_ma15 sin1 cos1 sin2 cos2 sin3 cos3 ///
                     t2m_mean_C t2m_sq tp_sum_mm rh_mean wind_speed lpop)
keep if _miss==0
drop _miss
di "ANALYSIS_N=" _N

di "=== REGHDFE (continuous outcome) ==="
reghdfe lnrate_cardio pm10_ma15 sin1 cos1 sin2 cos2 sin3 cos3 t2m_mean_C tp_sum_mm rh_mean wind_speed, ///
    absorb(geo_id year) vce(cluster geo_id)
di "REGHDFE_N=" e(N)
di "REGHDFE_b_pm10_ma15=" %20.15g _b[pm10_ma15]
di "REGHDFE_se_pm10_ma15=" %20.15g _se[pm10_ma15]

di "=== PPMLHDFE (count outcome, offset) ==="
capture which ppmlhdfe
if _rc==0 {
    ppmlhdfe deaths_cardio pm10_ma15 t2m_mean_C t2m_sq tp_sum_mm rh_mean wind_speed ///
        sin1 cos1 sin2 cos2 sin3 cos3, absorb(geo_id year) offset(lpop) vce(cluster geo_id)
    di "PPML_N=" e(N)
    di "PPML_b_pm10_ma15=" %20.15g _b[pm10_ma15]
    di "PPML_se_pm10_ma15=" %20.15g _se[pm10_ma15]
}
else di "PPMLHDFE not installed"

* export the exact prepped analysis vars for cecon
keep geo_id year lnrate_cardio deaths_cardio pm10_ma15 sin1 cos1 sin2 cos2 sin3 cos3 ///
     t2m_mean_C t2m_sq tp_sum_mm rh_mean wind_speed lpop
save "recon_prepped.dta", replace
log close
