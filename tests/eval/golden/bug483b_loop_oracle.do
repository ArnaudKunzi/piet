* BUG-483 part 2: `break` and `continue` INSIDE a loop, and `continue` inside a
* non-loop block at top level. The top-level arms are in b483_484_control_oracle;
* these are the arms that decide whether one rule can cover both keywords.
clear all
set obs 5

* B1: bare `break` inside forvalues — does it exit the loop?
local n1 = 0
forvalues i = 1/5 {
    local n1 = `n1' + 1
    if `i' == 3 break
}
display "B1 forvalues-bare-break iters=" `n1' " (5 = break did nothing, 3 = it exited)"

* B2: `continue, break` inside forvalues — the documented exit
local n2 = 0
forvalues i = 1/5 {
    local n2 = `n2' + 1
    if `i' == 3 continue, break
}
display "B2 forvalues-continue-break iters=" `n2'

* B3: bare `break` inside while
local n3 = 0
local k = 0
while `k' < 5 {
    local k = `k' + 1
    local n3 = `n3' + 1
    if `k' == 2 break
}
display "B3 while-bare-break iters=" `n3'

* B4: bare `break` inside foreach
local n4 = 0
foreach v in a b c d {
    local n4 = `n4' + 1
    if "`v'" == "b" break
}
display "B4 foreach-bare-break iters=" `n4'

* B5: `continue` inside an `if` block at TOP LEVEL (no enclosing loop) — is the
* rc the same 119 as a bare top-level continue, i.e. is `if` transparent?
capture noisily {
    if 1 == 1 {
        continue
    }
}
display "B5 if-block-toplevel-continue rc=" _rc
display "B5-REACHED"

* B6: `continue` inside a loop that is inside `quietly` — quietly must be
* transparent to the loop
local n6 = 0
quietly {
    forvalues i = 1/4 {
        if `i' == 2 continue
        local n6 = `n6' + 1
    }
}
display "B6 quietly-loop-continue hits=" `n6' " (expect 3)"

display "ORACLE COMPLETE"
