* ============================================================================
* G12 exotica golden — factor-variable coefficient rows under value labels
* Oracle: StataBE 19.5 (real).  Fixture: piet/packaging/sample/fatalities.csv
* (336 obs, byte-identical to what piet_native's `sysuse fatalities` loads).
*
* Question the oracle settles (BUG-102):
*   when a numeric factor variable carries a value label, WHAT does Stata put
*   in (a) the coefficient-table row stub and (b) the e(b) column names?
* Three arms: fully labeled / unlabeled / PARTIALLY labeled.
* ============================================================================
clear all
set more off
set linesize 120

import delimited "<REPO>/piet/packaging/sample/fatalities.csv", clear

* ---------------------------------------------------------------- arm A ----
* fully labeled 7-level factor
label define yrlbl 1982 "Y82" 1983 "Y83" 1984 "Y84" 1985 "Y85" ///
                   1986 "Y86" 1987 "Y87" 1988 "Y88"
label values year yrlbl

di "MARK_A_BEGIN"
regress fatal i.year
di "MARK_A_END"
local cnA : colnames e(b)
di "COLNAMES_A: `cnA'"
foreach nm of local cnA {
    di "COEF_A|`nm'|" %20.15e _b[`nm'] "|" %20.15e _se[`nm']
}

* ---------------------------------------------------------------- arm B ----
* same factor, NO value label attached (fallback shape)
capture drop yearnl
gen yearnl = year
di "MARK_B_BEGIN"
regress fatal i.yearnl
di "MARK_B_END"
local cnB : colnames e(b)
di "COLNAMES_B: `cnB'"

* ---------------------------------------------------------------- arm C ----
* PARTIALLY labeled: only 1983/1985/1987 carry a label; 1982/1984/1986/1988
* are in the factor but absent from the value-label set.
capture drop yearpl
gen yearpl = year
label define yrpart 1983 "Odd83" 1985 "Odd85" 1987 "Odd87"
label values yearpl yrpart
di "MARK_C_BEGIN"
regress fatal i.yearpl
di "MARK_C_END"
local cnC : colnames e(b)
di "COLNAMES_C: `cnC'"

label list yrlbl yrpart

di "ORACLE_DONE"
