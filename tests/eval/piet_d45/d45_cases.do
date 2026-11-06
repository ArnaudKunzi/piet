* Phase-D4+D5 golden set — human-readable REFERENCE script (Eval Harness Gen,
* BETA campaign Phase D). Mirrors the exact commands in d45_expected.json's
* per-case "do_body", concatenated in oracle order, `cap noi`-wrapped.
*
* NOT what the automated driver (run_d45_eval.py) executes — same rationale
* as piet_d1/piet_d2d3 (--do halts at the first error block; the 5 designed
* reject cases below WOULD halt a monolithic run without cap). The gating
* source of truth is d45_expected.json; this file is documentation.
*
* Oracle: internal capture d45-oracle-stata.log

sysuse fatalities, clear
gen high = (beertax > .5)
encode state, gen(snum)
gen t = _n
tsset t

di "=== D4a newey lag(2) ==="
cap noi newey income beertax spirits, lag(2)
di "rc=" _rc

di "=== D4b prais twostep / corc ==="
cap noi prais income beertax, twostep
di "rc=" _rc
cap noi prais income beertax, corc
di "rc=" _rc

di "=== D4c dfuller bare / lags(2) trend ==="
cap noi dfuller income
di "rc=" _rc
cap noi dfuller income, lags(2) trend
di "rc=" _rc

di "=== D4d pperron ==="
cap noi pperron income
di "rc=" _rc

di "=== D4e wntestq bare / lags(10) ==="
cap noi wntestq income
di "rc=" _rc
cap noi wntestq income, lags(10)
di "rc=" _rc

di "=== D4f tsfill (contiguous-gap variant; NOT the oracle's mod(t,7) — mod() unimplemented until Phase G) ==="
preserve
drop if t >= 100 & t <= 106
count
cap noi tsfill
di "rc=" _rc
count
restore

di "=== D5a sdtest by(high) ==="
cap noi sdtest income, by(high)
di "rc=" _rc

di "=== D5b estat durbinalt bare / small lags(2) ==="
cap noi regress income beertax spirits
cap noi estat durbinalt
di "rc=" _rc
cap noi estat durbinalt, small lags(2)
di "rc=" _rc

di "=== D5c estat firststage + endogenous/overid (rejects expected) ==="
cap noi ivregress 2sls income emppop (beertax = spirits unemp)
cap noi estat firststage
di "rc=" _rc
cap noi estat endogenous
di "endog rc=" _rc " (honest reject expected)"
cap noi estat overid
di "overid rc=" _rc " (honest reject expected)"

di "=== D5d margins after logit: dydx GREEN / at() reject ==="
cap noi logit high income unemp
cap noi margins, dydx(income)
di "rc=" _rc
cap noi margins, at(income=13000)
di "rc=" _rc " (honest non-OLS reject expected)"

di "=== D5e hausman estimates-store idiom ==="
cap noi xtset snum year
cap noi xtreg income beertax unemp, fe
cap noi estimates store feM
cap noi xtreg income beertax unemp, re
cap noi estimates store reM
cap noi hausman feM reM
di "rc=" _rc

di "=== D5f chow by(high) (oracle = manual pooled/split RSS formula) ==="
cap noi chow income beertax, by(high)
di "rc=" _rc

di "=== D5g mkspline: knots() mechanical / nknots(4) reject (engine gap, Phase F) ==="
cap noi mkspline inc_sp = income, cubic knots(11000 13000 15000 17000)
di "rc=" _rc
cap noi summarize inc_sp1 inc_sp2 inc_sp3
cap noi mkspline inc_sp2_ = income, cubic nknots(4)
di "rc=" _rc " (engine_error expected — nknots broken, tracked Phase F)"

di "=== D4+D5 REFERENCE END ==="
