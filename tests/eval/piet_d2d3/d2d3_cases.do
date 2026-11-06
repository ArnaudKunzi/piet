* Phase-D2+D3 golden set — human-readable REFERENCE script (Eval Harness Gen,
* BETA campaign Phase D). Mirrors the exact commands in d2d3_expected.json's
* per-case "do_body", concatenated in oracle order, `cap noi`-wrapped.
*
* NOT what the automated driver (run_d2d3_eval.py) executes — same rationale
* as tests/eval/piet_d1/d1_cases.do (piet_native --do halts the whole file at
* the first error block; see that leg's README). The gating source of truth
* is d2d3_expected.json; this file is documentation, kept in sync by hand.
*
* Oracles:
*   D2  : internal capture d2-oracle-stata.log
*   D2g : internal capture d2b-mediate.log
*   D3  : internal capture d3-oracle-stata.log
*   D3h : <repo>/tests/validate/drdid2x2.log (+ .do/.dta)

* ===================== D2 (wirings LANDED, GREEN) ======================
sysuse fatalities, clear

di "=== D2a tobit ul(15000) ==="
gen inc_cens = min(income, 15000)
cap noi tobit inc_cens beertax spirits, ul(15000)
di "rc=" _rc

di "=== D2b tobit ll(11000) ==="
gen inc_lo = max(income, 11000)
cap noi tobit inc_lo beertax spirits, ll(11000)
di "rc=" _rc

di "=== D2c heckman twostep ==="
gen high = (beertax > .5)
gen inc_obs = income if high==1
cap noi heckman inc_obs beertax, select(high = spirits unemp) twostep
di "rc=" _rc

di "=== D2d heckman MLE (honest-reject expected: engine is two-step only) ==="
cap noi heckman inc_obs beertax, select(high = spirits unemp)
di "rc=" _rc

di "=== D2e qreg median ==="
cap noi qreg income beertax spirits
di "rc=" _rc

di "=== D2f qreg quantile(.25) ==="
cap noi qreg income beertax, quantile(.25)
di "rc=" _rc

di "=== D2g mediate (ENGINE-PARITY ONLY — no Stata point-estimate pins) ==="
cap noi mediate (income) (spirits) (beertax, continuous(0.5 1)), reps(199) seed(20260715)
di "rc=" _rc

* ===================== D3 (wirings IN FLIGHT, RED-first) ======================
sysuse fatalities, clear
gen cnt = round(spirits * 3)
encode state, gen(snum)
xtset snum year

di "=== D3a poisson ==="
cap noi poisson cnt beertax unemp
di "rc=" _rc

di "=== D3b poisson offset(lpop) ==="
gen lpop = ln(income)
cap noi poisson cnt beertax, offset(lpop)
di "rc=" _rc

di "=== D3c poisson vce(robust) ==="
cap noi poisson cnt beertax unemp, vce(robust)
di "rc=" _rc

di "=== D3d ppmlhdfe absorb(snum) ==="
cap noi ppmlhdfe cnt beertax, absorb(snum)
di "rc=" _rc

di "=== D3e nbreg (boundary: alpha->0, collapses to Poisson) ==="
cap noi nbreg cnt beertax unemp
di "rc=" _rc

di "=== D3f xtpoisson fe ==="
cap noi xtpoisson cnt beertax unemp, fe
di "rc=" _rc

di "=== D3g xtlogit fe (n_obs=77: 37 degenerate groups dropped) ==="
gen hi2 = (spirits > 1.5)
cap noi xtlogit hi2 beertax unemp, fe
di "rc=" _rc

di "=== D3h drdid dripw (own fixture; driver uses in-leg v118 conversion) ==="
cap noi use "drdid2x2_v118.dta", clear
cap noi drdid Y x1 x2, ivar(id) time(t) treatment(treat) dripw
di "rc=" _rc

di "=== D2+D3 REFERENCE END ==="
