* ============================================================================
* csdid_long2_gap_oracle.do — BUG-227 tranche 2, case csdid_long2_gap.
*
* WHY: csdid_long2_gap.csv is a pure cobra self-capture (BUG-201: gen_csdid()'s
* own tests/validate/csdid_attgt.csv read is assigned and never referenced
* again). This capture makes it Stata-sourced.
*
* WHY *X* (hexfloat): Stata's %21.15e prints ~15 significant digits, and the
* pinned quantities here land with zeros in the low digits — e.g. this fixture's
* g2:t_1_4 ATT prints as 1.60292652700000e+00. At that width the golden and
* cobra can agree at relative error EXACTLY 0 for a reason that is only about
* PRINT WIDTH, which is precisely what the self-captured-golden ratchet flags
* and cannot distinguish from circular capture. So every quantity is emitted
* TWICE: ORACLE| at %21.15e (human-readable, and what a reader can check against
* the log by eye) and ORACLEX| at %21x (the exact IEEE double). rederive_golden.py
* prefers the ORACLEX line and writes it at 17-digit round-trip precision.
* If cobra and Stata then still agree bit-for-bit, that is a real finding about
* the engine, not a laundering artifact — and it is recorded as such.
*
* Spec (tests/eval/manifest.json case csdid_long2_gap):
*   cobra csdid --data {csdid_long2_gap} --y y --ivar id --time t --gvar g
*               --method dripw --long2
* Stata equivalent:
*   csdid y, ivar(id) time(t) gvar(g) method(dripw) long2
*
* DATA — deliberate deviation from tests/validate/csdid_long2_gap.do:
* that recipe REGENERATES the panel from `set seed 20260706` + rnormal(), which
* makes the oracle depend on Stata's RNG stream reproducing across versions. We
* instead export the COMMITTED fixture the engine actually reads
* (tests/validate/csdid_long2_gap.hbin) to .dta via `cobra export-dta` (format
* 118, lossless float64) and run Stata on that. Same guarantee the original
* recipe was after ("truth computed on the exact values that go into the hbin"),
* obtained by transport instead of by re-drawing. Export is a throwaway.
*
* INDEX MAPPING — measured, not assumed (probe log /tmp/long2_probe.log,
* 2026-08-08; e(b) colfullnames order printed and matched against the committed
* golden by value):
*   cells[1] = g2:t_1_2   cells[2] = g2:t_1_3 (gap, exactly 0)
*   cells[3] = g2:t_1_4   cells[4] = g2:t_1_5 (gap, exactly 0)
*   cells[5] = g2:t_1_6
* Emission below matches BY NAME, never by position, so a future reordering of
* Stata's e(b) cannot silently re-key the golden.
*
* SCOPE HONESTY: cells[2].att / cells[4].att are the filled-grid GAP cells that
* csdid reports as exactly 0 and cobra's drop-exact-zero aggregation gate keys
* on. They are integer-valued (0), carry atol-only pins, and are not in the
* ratchet's flagged set; they are emitted here anyway so the log is complete.
*
* CONTROL: 360 rows, and simple_att must land within ~1e-9 of the committed
* 1.316523393e+00. Otherwise this is not the pinned model.
*
* Run:  cd tests/eval/golden && StataBE -b do csdid_long2_gap_oracle.do
* Consume: python3 tests/eval/rederive_golden.py <that log> [--dry-run]
* ============================================================================
clear all
set more off
set type double
capture log close
log using "csdid_long2_gap_oracle.log", replace text
display "stata_version=" c(stata_version) " born=" c(born_date)

use "/tmp/csdid_long2_gap_export.dta", clear
display "DIAG_BEGIN"
quietly count
display "DIAG_N_ROWS=" r(N)
quietly levelsof t, local(tv)
display "DIAG_TIMES=`tv'"
quietly levelsof g, local(gv)
display "DIAG_GVALS=`gv'"
display "DIAG_END"

csdid y, ivar(id) time(t) gvar(g) method(dripw) long2

matrix b = e(b)
matrix V = e(V)
local names : colfullnames e(b)
local k = colsof(b)
display "ATTGT_K=" `k'
display "CSDID_LONG2_GAP_ORACLE_BEGIN"
forvalues j = 1/`k' {
    local nm : word `j' of `names'
    local idx = ""
    if "`nm'" == "g2:t_1_2" local idx = "1"
    if "`nm'" == "g2:t_1_3" local idx = "2"
    if "`nm'" == "g2:t_1_4" local idx = "3"
    if "`nm'" == "g2:t_1_5" local idx = "4"
    if "`nm'" == "g2:t_1_6" local idx = "5"
    if "`idx'" != "" {
        display "DIAG_MAP|`nm'|cells[`idx']"
        display "ORACLE|csdid_long2_gap.csv|cells[`idx'].att|" %21.15e b[1,`j']
        display "ORACLEX|csdid_long2_gap.csv|cells[`idx'].att|" %21x b[1,`j']
        display "ORACLE|csdid_long2_gap.csv|cells[`idx'].se|" %21.15e sqrt(V[`j',`j'])
        display "ORACLEX|csdid_long2_gap.csv|cells[`idx'].se|" %21x sqrt(V[`j',`j'])
    }
}

* ---- estat simple FIRST (before `estat event, post` replaces e()) ----
*   ORDERING IS LOAD-BEARING, and was established by measurement: the first
*   version of this .do followed tests/validate/csdid_long2_gap.do and re-fit
*   csdid then called `estat simple, post` LAST. That run produced all 13 other
*   keys and then HUNG — StataBE sat in state S having burned 3s of CPU over 14
*   minutes of wall, so `log close` never ran and the log was left unflushed
*   without its END marker ([[stata-batch-noexit]] class: the verdict is the log
*   marker, and here the marker's absence is honest — the run really was
*   incomplete). Both sibling captures that succeeded this session
*   (csdid_kitchensink_oracle.do, csdid_cluster_event_oracle.do) call `estat
*   simple` BEFORE `estat event`, and need no second fit. Same shape adopted.
estat simple
matrix Rs = r(table)
display "ORACLE|csdid_long2_gap.csv|simple_att|" %21.15e Rs[1,1]
display "ORACLEX|csdid_long2_gap.csv|simple_att|" %21x Rs[1,1]
display "ORACLE|csdid_long2_gap.csv|simple_se|" %21.15e Rs[2,1]
display "ORACLEX|csdid_long2_gap.csv|simple_se|" %21x Rs[2,1]

* ---- estat event, post: Pre_avg / Post_avg matched BY NAME ----
estat event, post
matrix eb = e(b)
matrix eV = e(V)
local en : colnames eb
local ek = colsof(eb)
forvalues j = 1/`ek' {
    local nm : word `j' of `en'
    display "DIAG_EVENT|`nm'"
    if "`nm'" == "Pre_avg" {
        display "ORACLE|csdid_long2_gap.csv|pre_avg|" %21.15e eb[1,`j']
        display "ORACLEX|csdid_long2_gap.csv|pre_avg|" %21x eb[1,`j']
    }
    if "`nm'" == "Post_avg" {
        display "ORACLE|csdid_long2_gap.csv|post_avg|" %21.15e eb[1,`j']
        display "ORACLEX|csdid_long2_gap.csv|post_avg|" %21x eb[1,`j']
        display "ORACLE|csdid_long2_gap.csv|post_avg_se|" %21.15e sqrt(eV[`j',`j'])
        display "ORACLEX|csdid_long2_gap.csv|post_avg_se|" %21x sqrt(eV[`j',`j'])
    }
}

display "CSDID_LONG2_GAP_ORACLE_END"
log close
exit, clear STATA
