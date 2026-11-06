* F17: tsset with non-integer / colliding time
import delimited "f17_tscollide.csv", clear
capture tsset t
display "F17_tsset_rc=" _rc
capture tsfill
display "F17_tsfill_rc=" _rc
* F18: wntestq on a series with an interior MISSING VALUE (contiguous time)
clear
import delimited "f18_gap.csv", clear
gen t = _n
tsset t
capture wntestq x, lags(3)
display "F18_wntestq_rc=" _rc
if _rc==0 {
  display "F18_Q=" %21.15e r(stat)
  display "F18_df=" r(df)
}
* F18b: also try a genuine time GAP (t skips 3)
clear
import delimited "f18_gap.csv", clear
gen t = _n
replace t = t+1 if _n>=3
drop if x==.
tsset t
capture wntestq x, lags(3)
display "F18b_gap_wntestq_rc=" _rc
exit, clear
