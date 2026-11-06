* ============================================================================
* g6_degenerate_scalars.do — Stata truth for degenerate summarize/tabstat scalars
* Eval Harness §10 golden source.  StataNow BE 19.5.
* COUNCIL-20260710 golden batch #1 — G6 (PLAYBOOK.md §7.2 item 6, gates F9).
*
* Purpose: pin Stata's exact degenerate outputs for (a) an all-missing variable
* (N=0 selection): mean/sd must be missing, NOT 0 -- cobra's Frame::summarize
* early-return leaves mean/sd at their struct default (0.0) for N=0 (CONFIRMED
* live: `cobra eval --summarize allmiss` -> N:0 mean:0.0 sd:0.0 today);
* (b) a string variable passed to summarize -- Stata's behavior here decides
* whether the golden is a value-pin or a reject shape (cobra's cmd_eval
* --summarize currently throws an "internal" runtime_error via require_f64,
* CONFIRMED live: rc=1 "column 'strvar' is a factor, not f64");
* (c) a singleton by-group (N=1): sample sd/semean are undefined (n-1=0) --
* Stata must show "." but cobra's tabstat `stat_of()` (apps/cecon_cli.cpp
* ~4284-4305) gives sd=0.0/semean=0.0 for n==1 (CONFIRMED live:
* `cobra tabstat --vars y --by g --stats n,mean,sd,semean` group g=2 (n=1)
* -> sd:0.0 semean:0.0 today) -- and a bonus: cobra's stat_of ALSO reports
* n=null (not 0) for a fully-missing by-group (g=3), since stat_of's n==0
* early-return short-circuits BEFORE the n/count branch -- Stata's count is a
* well-defined 0, not missing, so that is pinned too.
*
* Fixture: tests/eval/golden/g6_degenerate_full.csv (6 rows, synthetic, tiny) ==
*   tests/validate/g6_degenerate.hbin (--factor strvar; committed CSV, gitignored
*   hbin per tests/validate convention). Columns: g (group 1/2/3), y (numeric,
*   group1 N=3 normal / group2 N=1 singleton / group3 N=0 all-missing), strvar
*   (string A..F), allmiss (numeric, missing on every row).
*
* Run:  cd tests/eval/golden && StataBE -b do g6_degenerate_scalars.do
* Truth values transcribed (%21.15e, or NOTSET/error text for non-numeric
* outcomes) into g6_degenerate_scalars.csv by the Eval curator, validated
* against this log, then committed (Constitution Art 10).
* ============================================================================
clear all
set more off
set type double
capture log close
log using "g6_degenerate_scalars.log", replace text

import delimited "g6_degenerate_full.csv", clear case(preserve)
* Force allmiss numeric regardless of how import delimited inferred an
* entirely-blank column (some versions infer str1 for all-empty columns) --
* the semantic fixture is "numeric variable, always missing", not "string".
capture drop allmiss
gen double allmiss = .

* ---- (a) all-missing numeric variable: N=0 selection ----
summarize allmiss
display "ALLMISS_BEGIN"
display %21.15e r(N) " = r(N)"
capture display %21.15e r(mean)
if _rc != 0 display "MEAN_NOTSET rc=`=_rc'"
capture display %21.15e r(sd)
if _rc != 0 display "SD_NOTSET rc=`=_rc'"
display "ALLMISS_END"

* ---- (b) string variable passed to summarize ----
capture summarize strvar
display "STRVAR_BEGIN"
display "RC=`=_rc'"
if _rc != 0 display "ERRTEXT=" _rc
if _rc == 0 {
    display %21.15e r(N) " = r(N)"
}
display "STRVAR_END"

* ---- (c) by-group degenerate cases: g=1 normal(N=3), g=2 singleton(N=1), g=3 all-missing(N=0) ----
summarize y if g==1
display "GROUP1_BEGIN"
display %21.15e r(N)    " = r(N)"
display %21.15e r(mean) " = r(mean)"
display %21.15e r(sd)   " = r(sd)"
display "GROUP1_END"

summarize y if g==2
display "GROUP2_SINGLETON_BEGIN"
display %21.15e r(N)    " = r(N)"
display %21.15e r(mean) " = r(mean)"
capture display %21.15e r(sd)
if _rc != 0 display "SD_NOTSET rc=`=_rc'"
display "GROUP2_SINGLETON_END"

summarize y if g==3
display "GROUP3_ALLMISS_BEGIN"
display %21.15e r(N) " = r(N)"
capture display %21.15e r(mean)
if _rc != 0 display "MEAN_NOTSET rc=`=_rc'"
capture display %21.15e r(sd)
if _rc != 0 display "SD_NOTSET rc=`=_rc'"
display "GROUP3_ALLMISS_END"

* ---- cross-check via tabstat, by(g) (the actual buggy cobra code path: stat_of) ----
display "TABSTAT_BEGIN"
tabstat y, by(g) stat(n mean sd semean) format(%21.15e) longstub
display "TABSTAT_END"

log close
