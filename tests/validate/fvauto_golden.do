* Golden for collinear-column omission in factor-variable interactions.
* `regress price i.foreign##i.rep78` on sysuse auto: foreign cars only have
* rep78 in {3,4,5}, so 1.foreign == sum of 1.foreign#{3,4,5}.rep78 — an EXACT
* collinearity on top of the structurally-empty cell 1.foreign#2.rep78. Stata
* omits the LATER collinear term (1.foreign#5.rep78). This golden records the
* surviving coefficients + the omitted set so CoBrA can match coef-for-coef.
clear all
set more off
capture log close
log using "fvauto_golden.log", replace text
* Run from tests/validate: every path below is relative to it.

sysuse auto, clear

* show the (un)balanced foreign x rep78 cross-tab (which cells are empty)
tab foreign rep78, missing

regress price i.foreign##i.rep78

* full coefficient vector — omitted terms appear with value 0 and an "o." prefix
matrix b = e(b)
matrix list b, format(%21.15e)

* which columns are flagged omitted? e(rmcoll) / colnames carry the o. prefix.
di "rank = " e(rank) "   df_m = " e(df_m)

* dump a key,value CSV for the validator (surviving coefs by canonical name).
file open fh using "fvauto_golden.csv", write replace text
file write fh "key,value" _n
program define dumpb
    file write fh "`1'," %21.15e (`2') _n
end

* surviving terms (Stata keeps the main effect + the lower interactions)
dumpb "1.foreign"            _b[1.foreign]
dumpb "2.rep78"             _b[2.rep78]
dumpb "3.rep78"             _b[3.rep78]
dumpb "4.rep78"             _b[4.rep78]
dumpb "5.rep78"             _b[5.rep78]
dumpb "1.foreign#3.rep78"  _b[1.foreign#3.rep78]
dumpb "1.foreign#4.rep78"  _b[1.foreign#4.rep78]
dumpb "1.foreign#5.rep78"  _b[1.foreign#5.rep78]
dumpb "_cons"               _b[_cons]
file close fh

type "fvauto_golden.csv"
log close
