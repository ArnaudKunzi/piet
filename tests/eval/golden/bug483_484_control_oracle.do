* Oracle for BUG-483 (`continue` outside a loop) and BUG-484 (`tabulate` on a
* missing column). Both defects end the run silently at rc 0 in PIET, so what
* has to be captured is (a) the rc Stata sets and (b) that the NEXT statement
* runs. Every arm is `capture` so one failure cannot abort the file and hide
* the arms after it.
clear all
set more off
set obs 20
generate double a = mod(_n,4)
generate double b = _n

display "ARM01-BEGIN"

* --- BUG-483: continue / break outside any loop -----------------------------
capture noisily continue
display "A1 continue-toplevel rc=" _rc
display "A1-REACHED"

capture noisily break
display "A2 break-toplevel rc=" _rc
display "A2-REACHED"

capture noisily continue, break
display "A3 continue-break-toplevel rc=" _rc
display "A3-REACHED"

* Inside a loop it is legal — the control arm, so a wrong reading of the arms
* above cannot be blamed on `continue` being unimplemented.
local hits 0
forvalues i = 1/4 {
    if `i' == 2 continue
    local hits = `hits' + 1
}
display "A4 inloop-continue hits=" `hits' " (expect 3)"

* --- BUG-484: tabulate on a column that does not exist ----------------------
capture noisily tabulate nosuchcol
display "B1 tab-missing rc=" _rc
display "B1-REACHED"

capture noisily tab a nosuchcol
display "B2 tab2way-missing rc=" _rc
display "B2-REACHED"

capture noisily tab nosuchcol, missing
display "B3 tab-missing-opt rc=" _rc
display "B3-REACHED"

* Controls: the same missing column through verbs PIET handles correctly, so the
* rc above can be compared against its own family rather than asserted alone.
capture noisily summarize nosuchcol
display "C1 summarize-missing rc=" _rc
capture noisily list nosuchcol
display "C2 list-missing rc=" _rc
capture noisily tabulate a
display "C3 tab-good rc=" _rc

display "ORACLE COMPLETE"
