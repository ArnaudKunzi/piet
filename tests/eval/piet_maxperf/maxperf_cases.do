* MAXPERF golden set — human-readable REFERENCE script (Eval Harness Gen,
* MSG-MI-237 items 1-3, 2026-07-17). Mirrors the exact commands in
* maxperf_expected.json's per-case "do_body", concatenated in item order.
*
* NOT what the automated driver (run_maxperf_eval.py) executes: each case
* runs as its OWN isolated `piet_native --do` invocation (see README.md
* "Why each case is an isolated invocation"), and case mp1's `save`/`using`
* path below is a REAL placeholder path for illustration only — the driver
* substitutes a fresh tempdir path at run time. Use this file only for
* manual spot-checking once a fix has landed, one case at a time (run each
* `di "=== ... ==="` block separately, replacing __USING_HBIN__ with a real
* path of your choosing for mp1). The gating source of truth is
* maxperf_expected.json; this file is documentation, kept in sync by hand.

di "=== mp1 merge m:1 high-precision-key (MI-237 item 1) ==="
clear
set obs 4
gen id = .
replace id = 1005938 in 1
replace id = 1005942 in 2
replace id = 3141.59261 in 3
replace id = 3141.59264 in 4
gen payout = .
replace payout = 11 in 1
replace payout = 22 in 2
replace payout = 33 in 3
replace payout = 44 in 4
save "__USING_HBIN__", replace
clear
set obs 8
gen id = .
replace id = 1005938 in 1
replace id = 1005938 in 2
replace id = 1005942 in 3
replace id = 1005942 in 4
replace id = 3141.59261 in 5
replace id = 3141.59261 in 6
replace id = 3141.59264 in 7
replace id = 3141.59264 in 8
merge m:1 id using "__USING_HBIN__"
display "MERGE_LIST_BEGIN"
list id payout, clean noobs
display "MERGE_LIST_END"

di "=== mp2 collapse factor-by precision (MI-237 item 2) ==="
clear
set obs 4
gen str10 grp = "alpha" in 1/2
replace grp = "beta" in 3/4
gen val = .
replace val = 1000 + 1/1024 in 1
replace val = 1000 + 3/1024 in 2
replace val = 2000 + 5/1024 in 3
replace val = 2000 + 9/1024 in 4
collapse (mean) val, by(grp)
display "COLLAPSE_BEGIN"
list grp val, clean noobs
display "COLLAPSE_END"

di "=== mp3 foreach-continue after captured error (MI-237 item 3) ==="
program define failprog
    display "inside-failprog"
    error 111
end

foreach v in a b c {
    display "ITER-`v'-START"
    capture noisily failprog
    display "ITER-`v'-RC=" _rc
}

display "POSTLOOP-MARKER"

di "=== MAXPERF REFERENCE END ==="
