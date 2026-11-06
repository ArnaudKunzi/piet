clear all
set more off
capture log close
log using "/tmp/cobra_t4_tabstat.log", replace text

sysuse auto, clear

* open CSV output
file open out using "/tmp/cobra_t4_tabstat.csv", write replace text
file write out "key,value" _n

* ---------------------------------------------------------------
* helper program: dump a saved tabstat matrix to CSV cell by cell
*   matrix M : rows = statistics (rownames = statname),
*              cols = variables  (colnames = varname)
*   prefix   : label for the block (e.g. "total", "g1", "g2")
* ---------------------------------------------------------------
program define dumpmat
    args mname prefix
    tempname M
    matrix `M' = `mname'
    local nr = rowsof(`M')
    local nc = colsof(`M')
    local rn : rownames `M'
    local cn : colnames `M'
    forvalues i = 1/`nr' {
        local sname : word `i' of `rn'
        forvalues j = 1/`nc' {
            local vname : word `j' of `cn'
            local val = `M'[`i',`j']
            file write out "`prefix'.`sname'.`vname'," %21.15e (`val') _n
        }
    }
end

* ===============================================================
* (1) tabstat price mpg weight, stat(mean sd min max n p50) save
* ===============================================================
tabstat price mpg weight, stat(mean sd min max n p50) save
matrix list r(StatTotal)
dumpmat r(StatTotal) "T1"

* ===============================================================
* (2) tabstat price mpg, by(foreign) stat(mean sd n) save
*   r(StatTotal) = overall (Total); r(Stat1), r(Stat2) = groups
*   r(name1), r(name2) = the by-group labels/values
* ===============================================================
tabstat price mpg, by(foreign) stat(mean sd n) save
matrix list r(StatTotal)
matrix list r(Stat1)
matrix list r(Stat2)
local nm1 "`r(name1)'"
local nm2 "`r(name2)'"
file write out "T2.name1,`nm1'" _n
file write out "T2.name2,`nm2'" _n

* must dump matrices BEFORE any other r-class call clobbers r()
dumpmat r(StatTotal) "T2.total"
dumpmat r(Stat1) "T2.g1"
dumpmat r(Stat2) "T2.g2"

* ===============================================================
* (3) Independent cross-check via summarize/detail so we pin the
*     EXACT scalars tabstat should reproduce. Use %21.15e.
* ===============================================================
* ---- full sample (74 obs) ----
foreach v in price mpg weight {
    quietly summarize `v', detail
    file write out "S.`v'.mean,"   %21.15e (r(mean))   _n
    file write out "S.`v'.sd,"     %21.15e (r(sd))     _n
    file write out "S.`v'.var,"    %21.15e (r(Var))    _n
    file write out "S.`v'.min,"    %21.15e (r(min))    _n
    file write out "S.`v'.max,"    %21.15e (r(max))    _n
    file write out "S.`v'.n,"      %21.15e (r(N))      _n
    file write out "S.`v'.sum,"    %21.15e (r(sum))    _n
    file write out "S.`v'.p25,"    %21.15e (r(p25))    _n
    file write out "S.`v'.p50,"    %21.15e (r(p50))    _n
    file write out "S.`v'.p75,"    %21.15e (r(p75))    _n
    file write out "S.`v'.range,"  %21.15e (r(max)-r(min)) _n
    file write out "S.`v'.cv,"     %21.15e (r(sd)/r(mean)) _n
    file write out "S.`v'.semean," %21.15e (r(sd)/sqrt(r(N))) _n
}

* ---- by foreign==0 (Domestic) ----
foreach v in price mpg {
    quietly summarize `v' if foreign==0, detail
    file write out "S0.`v'.mean," %21.15e (r(mean)) _n
    file write out "S0.`v'.sd,"   %21.15e (r(sd))   _n
    file write out "S0.`v'.n,"    %21.15e (r(N))    _n
    file write out "S0.`v'.p50,"  %21.15e (r(p50))  _n
}
* ---- by foreign==1 (Foreign) ----
foreach v in price mpg {
    quietly summarize `v' if foreign==1, detail
    file write out "S1.`v'.mean," %21.15e (r(mean)) _n
    file write out "S1.`v'.sd,"   %21.15e (r(sd))   _n
    file write out "S1.`v'.n,"    %21.15e (r(N))    _n
    file write out "S1.`v'.p50,"  %21.15e (r(p50))  _n
}

* ---- rep78 has 5 missing: verify n = count of nonmissing ----
quietly summarize rep78, detail
file write out "S.rep78.n,"    %21.15e (r(N))   _n
file write out "S.rep78.mean," %21.15e (r(mean)) _n
file write out "S.rep78.p50,"  %21.15e (r(p50))  _n

* ---- tabstat rep78 mpg, stat(n mean cv p25 p50 p75) save (parity of cv & q) ----
tabstat rep78 mpg, stat(n mean cv p25 p50 p75) save
matrix list r(StatTotal)
dumpmat r(StatTotal) "T3"

file close out
log close