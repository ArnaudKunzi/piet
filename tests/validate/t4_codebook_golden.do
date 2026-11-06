*! Golden facts for `cobra codebook` parity (Stata `codebook`)
*! Recompute every numeric fact codebook prints, per variable, into a key,value CSV.
clear all
set more off
sysuse auto, clear

tempname fh
file open `fh' using "/tmp/cobra_t4_codebook.csv", write replace text
file write `fh' "key,value" _n

* ---- dataset-level facts ----
file write `fh' "N_obs," %21.15e (_N) _n
quietly ds
local nvars : word count `r(varlist)'
file write `fh' "N_vars," %21.15e (`nvars') _n

* ---- helper macro lists ----
local task price rep78 foreign
local allnum price mpg rep78 headroom trunk weight length turn displacement gear_ratio foreign

********************************************************************************
* Per-variable numeric facts: n (nonmissing), n_unique (nonmissing),
* n_missing (sysmiss), N (total), min, max, mean, sd, Var, and pctiles 10/25/50/75/90.
********************************************************************************
foreach v of local allnum {
    quietly count if !missing(`v')
    local n = r(N)
    file write `fh' "`v'_n," %21.15e (`n') _n
    quietly count if missing(`v')
    file write `fh' "`v'_n_missing," %21.15e (r(N)) _n
    file write `fh' "`v'_N," %21.15e (_N) _n
    quietly levelsof `v', local(lv)
    local nu : word count `lv'
    file write `fh' "`v'_n_unique," %21.15e (`nu') _n
    quietly summarize `v'
    file write `fh' "`v'_mean," %21.15e (r(mean)) _n
    file write `fh' "`v'_sd," %21.15e (r(sd)) _n
    file write `fh' "`v'_min," %21.15e (r(min)) _n
    file write `fh' "`v'_max," %21.15e (r(max)) _n
    file write `fh' "`v'_Var," %21.15e (r(Var)) _n
    quietly summarize `v', detail
    file write `fh' "`v'_p10," %21.15e (r(p10)) _n
    file write `fh' "`v'_p25," %21.15e (r(p25)) _n
    file write `fh' "`v'_p50," %21.15e (r(p50)) _n
    file write `fh' "`v'_p75," %21.15e (r(p75)) _n
    file write `fh' "`v'_p90," %21.15e (r(p90)) _n
}

********************************************************************************
* String variable: make -> Unique values + Missing "" / N
********************************************************************************
quietly count if make != ""
file write `fh' "make_n," %21.15e (r(N)) _n
quietly count if make == ""
file write `fh' "make_n_missing," %21.15e (r(N)) _n
file write `fh' "make_N," %21.15e (_N) _n
quietly levelsof make, local(mk)
local mku : word count `mk'
file write `fh' "make_n_unique," %21.15e (`mku') _n

********************************************************************************
* Categorical tabulations (Freq/Numeric/Label rows).
********************************************************************************
quietly levelsof foreign, local(fl)
local k = 0
foreach val of local fl {
    local ++k
    quietly count if foreign == `val'
    file write `fh' "foreign_tab`k'_value," %21.15e (`val') _n
    file write `fh' "foreign_tab`k'_freq," %21.15e (r(N)) _n
}
file write `fh' "foreign_tab_nlevels," %21.15e (`k') _n

quietly levelsof rep78, local(rl)
local k = 0
foreach val of local rl {
    local ++k
    quietly count if rep78 == `val'
    file write `fh' "rep78_tab`k'_value," %21.15e (`val') _n
    file write `fh' "rep78_tab`k'_freq," %21.15e (r(N)) _n
}
file write `fh' "rep78_tab_nlevels," %21.15e (`k') _n
quietly count if missing(rep78)
file write `fh' "rep78_tab_missing_freq," %21.15e (r(N)) _n

file close `fh'
display "DONE writing /tmp/cobra_t4_codebook.csv"