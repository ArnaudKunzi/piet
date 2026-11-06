* External research data, not shipped: set global ILL_WINDS_DTA_DIR to its directory first.
clear all
set more off
set type double
* Run from tests/validate: every path below is relative to it.
capture log close
log using "tsma_golden.log", replace text

use geo_id date PM10_mean using ///
    "$ILL_WINDS_DTA_DIR/wind_iv_full_panel.dta", clear
keep if !missing(PM10_mean)
recast double PM10_mean
xtset geo_id date
tssmooth ma pm10_ma15 = PM10_mean, window(7 1 7)
recast double pm10_ma15
di "TSMA_N=" _N
keep geo_id date PM10_mean pm10_ma15
save "tsma_golden.dta", replace
log close
