* egen row-function golden for cecon Frame::row_egen (eval --rowegen).
clear all
set more off
set type double
set obs 6
gen double s1 = _n
gen double s2 = _n*2 - 1
gen double s3 = _n + 3
replace s3 = . in 2
replace s1 = . in 5
replace s2 = . in 5
replace s3 = . in 5
egen double rm = rowmean(s1 s2 s3)
egen double rsd = rowsd(s1 s2 s3)
egen double rt = rowtotal(s1 s2 s3)
egen double rmax = rowmax(s1 s2 s3)
egen double rmin = rowmin(s1 s2 s3)
egen double rmiss = rowmiss(s1 s2 s3)
egen double rnm = rownonmiss(s1 s2 s3)
export delimited using "rowegen_golden.csv", replace
