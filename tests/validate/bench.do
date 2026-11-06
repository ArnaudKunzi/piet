clear all
set more off
* Run from tests/validate: every path below is relative to it.
timer clear
timer on 1
use deryugina.dta, clear
timer off 1
* warmup (mata/ftools one-time compile, cache)
quietly reghdfe y_all pm25, absorb(sg year) cluster(sg)
* timed HDFE OLS
timer on 2
reghdfe y_all pm25 tmaxb tminb precd wsd, absorb(sg year) cluster(sg)
timer off 2
* timed 2SLS/IV
timer on 3
ivreghdfe y_all tmaxb tminb precd wsd (pm25 = wq), absorb(sg year) cluster(sg)
timer off 3
timer list
