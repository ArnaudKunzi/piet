* ============================================================================
* g7_egen_tabulate_order.do — Stata truth for egen group()/tabulate label order
* Eval Harness §10 golden source.  StataNow BE 19.5.
* COUNCIL-20260710 golden batch #1 — G7 (PLAYBOOK.md §7.2 item 7, gates F8).
*
* Purpose: pin Stata's SORTED (alphabetical) level order for egen group() ids
* and tabulate rows. cobra's egen_group (src/data/frame.cpp ~918) and
* cmd_tabulate's categorize() (apps/cecon_cli.cpp ~4351) both take a factor
* column's STORED (first-seen, from to_hbin.py's pandas.factorize) dictionary
* order verbatim instead of sorting by label -- CONFIRMED live on this exact
* fixture: `cobra eval --egen "g=group(grp)"` gives B=1,C=2,D=3,E=4,A=5 (not
* Stata's A=1,B=2,C=3,D=4,E=5); `cobra tabulate --vars grp` lists levels
* [B,C,D,E,A] (not Stata's [A,B,C,D,E]).
*
* Fixture: tests/eval/golden/g7_egen_order_full.csv (8 rows, synthetic) ==
*   tests/validate/g7_egen_order.hbin. Single string var `grp`, first-seen
*   order B,C,D,E,A,B,C,B (level B x3, C x2, D/E/A x1 each) -- deliberately
*   NOT alphabetical in first-seen order, matching the audit's own repro shape
*   ("fixture levels stored [B,C,D,E,A]", RAW-correctness.md F8).
*
* Run:  cd tests/eval/golden && StataBE -b do g7_egen_tabulate_order.do
* Truth transcribed into g7_egen_tabulate_order.csv (group-id mapping is a
* plain documented Stata convention -- alphabetical rank -- verified live
* here rather than assumed). Constitution Art 10.
* ============================================================================
clear all
set more off
capture log close
log using "g7_egen_tabulate_order.log", replace text

* NOTE: a single-column all-string CSV needs varnames(1) forced explicitly --
* import delimited's header auto-detection mis-fires on it (no comma to help
* disambiguate header-vs-data), silently naming the var "v1" and treating the
* header text "grp" as a 9th data row (diagnosed live via g7b_diag_probe.do).
import delimited "g7_egen_order_full.csv", clear case(preserve) varnames(1)

egen g = group(grp)
sort grp
by grp: gen tag = (_n==1)

display "EGEN_GROUP_ORDER_BEGIN"
list grp g if tag==1, clean noobs
display "EGEN_GROUP_ORDER_END"

display "TABULATE_BEGIN"
tabulate grp
display "TABULATE_END"

* machine-parseable exact frequencies + order (no reliance on printed-table
* column widths): loop the 5 distinct levels in Stata's sorted order.
display "TABULATE_EXACT_BEGIN"
levelsof grp, local(lv)
foreach l of local lv {
    quietly count if grp == "`l'"
    display "`l' freq=" r(N)
}
display "TABULATE_EXACT_END"

log close
