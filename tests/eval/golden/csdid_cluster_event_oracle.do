* ============================================================================
* csdid_cluster_event_oracle.do — BUG-227 tranche 2, case csdid_cluster_event.
*
* WHY: this case's _comment already claims Stata provenance (TRUTH csv from
* csdid_cluster_golden.do + csdid_agg_scalars.{do,log}), and it is TRUE — but
* those captures were transcribed from Stata's DEFAULT %g print (~16 digits),
* e.g. tests/validate/csdid_cluster_synth_TRUTH.csv row
*   Tm5,-.2965505326849328,.2388191041326803
* At that precision cobra matches the golden at relative error EXACTLY 0 on
* estat_event.e_b.values[2], which is what the self-captured-golden ratchet
* flags: a truncated-print golden cannot adjudicate a numerics change even
* when its provenance is honest. This capture re-takes the SAME quantities at
* %21.15e so the pin carries the oracle's full precision.
*
* Spec (tests/eval/manifest.json case csdid_cluster_event):
*   cobra csdid --data {csdid_cluster_synth} --y y --ivar unit --time t
*               --gvar g --x x --method dripw --cluster clu
* Stata equivalent:
*   csdid y x, ivar(unit) time(t) gvar(g) method(dripw) cluster(clu)
*   -> estat event, post
*
* Data transport: tests/validate/csdid_cluster_synth.hbin -> .dta via
*   `cobra export-dta` (round-trip-gated transport, tranche-1 precedent).
*   The export is a throwaway and is never committed.
*
* INDEX MAPPING (identification by approximation, adoption verbatim): the
* golden's estat_event.e_b.values[i] are POSITIONAL in e(b) after
* `estat event, post`. The committed golden and TRUTH.csv agree that the order
* is Pre_avg, Post_avg, Tm5, Tm4, ... — this .do prints DIAG_EVENTB lines with
* the Stata name at each position so the mapping is re-proved from THIS log,
* not inherited. If the printed names ever stop matching that order, the
* transplant must STOP: a wrong index mapping is a silent wrong golden.
*
* CONTROL: e(N) and the distinct-unit count are printed. n_units must be 300
* and N 2400 (60 clusters x 5 units x 8 periods) per the fixture recipe
* tests/validate/csdid_cluster_golden.do; simple_att must land within ~1e-9 of
* the committed 1.003548392075270e+00. Otherwise this is not the pinned model.
*
* Run:  cd tests/eval/golden && StataBE -b do csdid_cluster_event_oracle.do
* Consume: python3 tests/eval/rederive_golden.py <that log> [--dry-run]
* ============================================================================
clear all
set more off
set type double
capture log close
log using "csdid_cluster_event_oracle.log", replace text
display "stata_version=" c(stata_version) " born=" c(born_date)

use "/tmp/csdid_cluster_synth_export.dta", clear

display "DIAG_BEGIN"
quietly count
display "DIAG_N_ROWS=" r(N)
quietly distinct unit
display "DIAG_N_UNITS=" r(ndistinct)
quietly distinct clu
display "DIAG_N_CLUST=" r(ndistinct)
display "DIAG_END"

xtset unit t
csdid y x, ivar(unit) time(t) gvar(g) method(dripw) cluster(clu)
display "CSDID_E_N=" e(N)

* ---- estat simple: simple_att / simple_se ----
estat simple
matrix Rs = r(table)
display "ORACLE|csdid_cluster_event.csv|simple_att|" %21.15e Rs[1,1]
display "ORACLE|csdid_cluster_event.csv|simple_se|" %21.15e Rs[2,1]

* ---- estat event, post: e(b) / e(V) positional pins + named aggregates ----
estat event, post
matrix eb = e(b)
matrix eV = e(V)
local ek = colsof(eb)
local enames : colnames eb
display "EVENTB_K=" `ek'
display "CSDID_CLUSTER_EVENT_ORACLE_BEGIN"
forvalues j = 1/`ek' {
    local nm : word `j' of `enames'
    local i = `j' - 1
    display "DIAG_EVENTB|pos=`i'|name=`nm'"
    display "ORACLE|csdid_cluster_event.csv|estat_event.e_b.values[`i']|" %21.15e eb[1,`j']
    display "ORACLE|csdid_cluster_event.csv|estat_event.e_V.matrix[`i'][`i']|" %21.15e eV[`j',`j']
    if "`nm'" == "Pre_avg" {
        display "ORACLE|csdid_cluster_event.csv|pre_avg|" %21.15e eb[1,`j']
        display "ORACLE|csdid_cluster_event.csv|pre_avg_se|" %21.15e sqrt(eV[`j',`j'])
    }
    if "`nm'" == "Post_avg" {
        display "ORACLE|csdid_cluster_event.csv|post_avg|" %21.15e eb[1,`j']
        display "ORACLE|csdid_cluster_event.csv|post_avg_se|" %21.15e sqrt(eV[`j',`j'])
    }
}
display "CSDID_CLUSTER_EVENT_ORACLE_END"
log close
exit, clear STATA
