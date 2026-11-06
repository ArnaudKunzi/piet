clear
set obs 4
gen wk = tw(2020w1) + _n - 1
format wk %tw
gen y = _n
list
save "tw_fixture.dta", replace
* what raw numeric does %tw hold? tw(2020w1) = weeks since 1960w1
display "tw2020w1_raw=" tw(2020w1)
exit, clear
