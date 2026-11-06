* ============================================================================
* g9_collapse_firstlast.do — Stata truth for collapse (first)/(last)/(firstnm)/(lastnm)
* Eval Harness §10 golden source.  StataNow BE 19.5.
* COUNCIL-20260710 golden batch #1 — G9 (PLAYBOOK.md §7.2 item 9, gates F13).
*
* Purpose: pin Stata's RAW (missing-included) semantics for (first)/(last) and
* the non-missing variants (firstnm)/(lastnm). cobra's collapse (src/data/frame.cpp
* ~1279, CollapseSpec "first"/"last") currently gathers only the NON-MISSING
* values of the group before taking front()/back() -- i.e. it computes what
* Stata calls (firstnm)/(lastnm) when asked for plain (first)/(last).
* (firstnm)/(lastnm) themselves are not implemented at all yet (CONFIRMED live:
* `cobra collapse --stat "(firstnm) x=x"` -> error "unknown stat 'firstnm'").
*
* CONFIRMED live today (WRONG vs the Stata truth this .do pins):
*   g=1 (x: ., 5, 7)    cobra (first)=5 (last)=7   -- Stata (first) must be .
*   g=2 (x: 3, 4, .)    cobra (first)=3 (last)=4   -- Stata (last)  must be .
*   g=3 (x: ., ., 8)    cobra (first)=8 (last)=8   -- Stata (first) must be .
*   g=4 (x: 9, ., .)    cobra (first)=9 (last)=9   -- Stata (last)  must be .
*
* Fixture: tests/eval/golden/g9_collapse_firstlast_full.csv (12 rows, synthetic)
*   == tests/validate/g9_collapse_firstlast.hbin. Group g in {1,2,3,4}, var x
*   with leading-missing (g=1,g=3) and trailing-missing (g=2,g=4) patterns,
*   3 rows/group, row order == group order (collapse's stable sort preserves
*   within-group input order, so "first"/"last" map directly to file row order).
*
* Run:  cd tests/eval/golden && StataBE -b do g9_collapse_firstlast.do
* Truth transcribed into g9_collapse_firstlast.csv by the Eval curator,
* validated against this log, then committed (Constitution Art 10).
* ============================================================================
clear all
set more off
set type double
capture log close
log using "g9_collapse_firstlast.log", replace text

import delimited "g9_collapse_firstlast_full.csv", clear case(preserve) asdouble

collapse (first) x_first=x (last) x_last=x (firstnm) x_firstnm=x (lastnm) x_lastnm=x, by(g)

* Plain `list` (not capture+display -- capture suppresses ALL output, even on
* success, diagnosed live) is unambiguous here: every value is an exact small
* integer or missing, no floating-point precision concerns.
display "COLLAPSE_BEGIN"
list g x_first x_last x_firstnm x_lastnm, clean noobs
display "COLLAPSE_END"

log close
