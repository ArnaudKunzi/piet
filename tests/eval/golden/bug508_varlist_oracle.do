* Oracle for BUG-507 and its sibling: what does Stata do when a VARLIST names a
* variable that does not exist? Three consumers, one question.
clear all
set obs 5
generate double alpha = _n
generate double beta = _n*2

capture noisily list alpha nosuchcol
display "L1 list-mixed rc=" _rc
capture noisily list nosuchcol
display "L2 list-only rc=" _rc
capture noisily list alpha*
display "L3 list-wildcard rc=" _rc
capture noisily list alpha-beta
display "L4 list-range rc=" _rc
capture noisily list zz*
display "L5 list-unmatched-wildcard rc=" _rc

capture noisily foreach v of varlist nosuchcol {
    display "saw `v'"
}
display "F1 foreach-varlist-unknown rc=" _rc
capture noisily foreach v of varlist zz* {
    display "saw `v'"
}
display "F2 foreach-varlist-unmatched-wildcard rc=" _rc

capture noisily unab z : nosuchcol
display "U1 unab-unknown rc=" _rc
capture noisily ds nosuchcol
display "D1 ds-unknown rc=" _rc

display "ORACLE COMPLETE"
