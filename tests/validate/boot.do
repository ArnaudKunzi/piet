clear all
set more off
* Run from tests/validate: every path below is relative to it.
use deryugina.dta, clear
quietly reghdfe y_all pm25 tmaxb tminb precd wsd, absorb(sg year) cluster(sg)
timer clear
timer on 1
boottest pm25, reps(9999) seed(42) nograph weighttype(rademacher)
timer off 1
timer list
