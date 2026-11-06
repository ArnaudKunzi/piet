* Oracle for cobra iv singleton-FE drop (contract 1.7.0, §21a)
* Data: singleton_iv.csv — firm FE with 5 singleton firms (100..104, 1 obs each)
* Truth for cobra `iv --y y --endog x --instruments z --x w --fe firm`
clear all
set more off
import delimited "singleton_iv.csv", clear

* ---- (A) DEFAULT: cobra drops singleton FE groups, then 2SLS with firm FE ----
* Reproduce by dropping singleton firms, then LSDV ivregress (== ivreghdfe default).
bysort firm: gen _nfirm = _N
preserve
drop if _nfirm==1
ivregress 2sls y w i.firm (x = z), small
di "=== DEFAULT (singletons dropped) ==="
di "N = " e(N)
mat b = e(b)
di "b[x] = " _b[x] "   se[x] = " _se[x]
di "b[w] = " _b[w] "   se[w] = " _se[w]
restore

* ---- (B) KEEP-SINGLETONS: cobra --keep-singletons keeps all rows ----
ivregress 2sls y w i.firm (x = z), small
di "=== KEEP-SINGLETONS (all rows) ==="
di "N = " e(N)
di "b[x] = " _b[x] "   se[x] = " _se[x]
di "b[w] = " _b[w] "   se[w] = " _se[w]
