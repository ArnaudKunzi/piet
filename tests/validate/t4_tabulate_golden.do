clear all
set more off
capture log close
log using "/tmp/cobra_t4_tabulate.log", replace text
cd /tmp

sysuse auto, clear

* open csv output
file open f using "/tmp/cobra_t4_tabulate.csv", write replace text
file write f "key,value" _n

*-------------------------------------------------------------------
* PART A: one-way tabulate of rep78 (has 5 missing -> excluded)
*-------------------------------------------------------------------
tabulate rep78
file write f "ow_N" "," %21.15e (r(N)) _n
file write f "ow_r" "," %21.15e (r(r)) _n

* one-way with missing treated as a category
tabulate rep78, missing
file write f "ow_miss_N" "," %21.15e (r(N)) _n
file write f "ow_miss_r" "," %21.15e (r(r)) _n

* Recompute one-way freq/percent/cum by hand via counts so we can dump
* the exact per-level numbers tabulate prints (freq, percent, cum%).
* rep78 levels are 1..5 (nonmissing).
quietly count if rep78 < .
local owN = r(N)
file write f "ow_total_freq" "," %21.15e (`owN') _n
local cum = 0
forvalues k = 1/5 {
    quietly count if rep78 == `k'
    local fk = r(N)
    local pk = 100*`fk'/`owN'
    local cum = `cum' + `pk'
    file write f "ow_freq_`k'"    "," %21.15e (`fk')  _n
    file write f "ow_pct_`k'"     "," %21.15e (`pk')  _n
    file write f "ow_cum_`k'"     "," %21.15e (`cum') _n
}

*-------------------------------------------------------------------
* PART B: two-way tabulate foreign rep78 with all measures + exact
*   foreign: 0=Domestic,1=Foreign (2 rows)
*   rep78  : 1..5 (5 cols), missing excluded by default
*-------------------------------------------------------------------
tabulate foreign rep78, chi2 lrchi2 V gamma taub exact

file write f "tw_N"        "," %21.15e (r(N))        _n
file write f "tw_r"        "," %21.15e (r(r))        _n
file write f "tw_c"        "," %21.15e (r(c))        _n
file write f "tw_chi2"     "," %21.15e (r(chi2))     _n
file write f "tw_p"        "," %21.15e (r(p))        _n
file write f "tw_chi2_lr"  "," %21.15e (r(chi2_lr))  _n
file write f "tw_p_lr"     "," %21.15e (r(p_lr))     _n
file write f "tw_CramersV" "," %21.15e (r(CramersV)) _n
file write f "tw_gamma"    "," %21.15e (r(gamma))    _n
file write f "tw_ase_gam"  "," %21.15e (r(ase_gam))  _n
file write f "tw_taub"     "," %21.15e (r(taub))     _n
file write f "tw_ase_taub" "," %21.15e (r(ase_taub)) _n
file write f "tw_p_exact"  "," %21.15e (r(p_exact))  _n

* Cell frequencies + marginals computed directly so cobra can match each cell.
* rows: foreign in {0,1}; cols: rep78 in {1,2,3,4,5}
forvalues fr = 0/1 {
    forvalues rc = 1/5 {
        quietly count if foreign == `fr' & rep78 == `rc'
        file write f "cell_f`fr'_r`rc'" "," %21.15e (r(N)) _n
    }
}
* row totals
forvalues fr = 0/1 {
    quietly count if foreign == `fr' & rep78 < .
    file write f "rowtot_f`fr'" "," %21.15e (r(N)) _n
}
* col totals
forvalues rc = 1/5 {
    quietly count if rep78 == `rc' & foreign < .
    file write f "coltot_r`rc'" "," %21.15e (r(N)) _n
}
* grand total
quietly count if foreign < . & rep78 < .
file write f "grandtot" "," %21.15e (r(N)) _n

file close f
log close