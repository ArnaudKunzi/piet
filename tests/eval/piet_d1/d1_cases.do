* Phase-D1 golden set — human-readable REFERENCE script (Eval Harness Gen,
* BETA campaign Phase D). Mirrors the exact commands in d1_expected.json's
* per-case "do_body", concatenated in oracle order (D1a..D1i, then the two
* NO-ORACLE/engine-parity cases), `cap noi`-wrapped in the same style the
* real oracle probe used for its own D1i/D1j availability checks.
*
* NOT what the automated driver (run_d1_eval.py) executes: piet_native's
* --do runner halts the WHOLE file at the first "type":"error" block, and
* `cap noi` does not prevent that halt (empirically verified — see
* run_d1_eval.py's module docstring and README.md). Running this file
* wholesale against a build that only has SOME D1 verbs wired will therefore
* stop at the first still-unsupported command and never reach the rest; use
* it only for manual spot-checking once several verbs have landed, one
* command at a time. The gating source of truth is d1_expected.json; this
* file is documentation, kept in sync with it by hand.
*
* Oracle: internal capture d1-oracle-stata.log
* (real StataBE 19.5, `import delimited piet/packaging/sample/fatalities.csv`,
* 336 obs). piet's own `sysuse fatalities` loads the identical bundled CSV.

sysuse fatalities, clear

di "=== D1a ivregress 2sls just-identified ==="
cap noi ivregress 2sls income (beertax = spirits)
di "rc=" _rc

di "=== D1b ivregress 2sls over-identified + exog control ==="
cap noi ivregress 2sls income emppop (beertax = spirits unemp)
di "rc=" _rc

di "=== D1c 2sls robust ==="
cap noi ivregress 2sls income emppop (beertax = spirits unemp), vce(robust)
di "rc=" _rc

di "=== D1d 2sls cluster ==="
cap noi ivregress 2sls income emppop (beertax = spirits unemp), vce(cluster state)
di "rc=" _rc

di "=== D1e ivregress gmm ==="
cap noi ivregress gmm income emppop (beertax = spirits unemp)
di "rc=" _rc

di "=== D1f ivregress liml ==="
cap noi ivregress liml income emppop (beertax = spirits unemp)
di "rc=" _rc

di "=== D1g reg3 (3sls default) ==="
cap noi reg3 (income beertax spirits) (emppop unemp income)
di "rc=" _rc

di "=== D1h reg3 sure ==="
cap noi reg3 (income beertax spirits) (emppop unemp income), sure
di "rc=" _rc

di "=== D1i reghdfe 2-way cluster + absorb ==="
cap noi reghdfe income beertax, absorb(state) vce(cluster state year)
di "rc=" _rc

di "=== D1x ivreghdfe(absorb) — NO Stata oracle, structural check only ==="
cap noi ivreghdfe income emppop (beertax = spirits unemp), absorb(state) cluster(state)
di "rc=" _rc

di "=== D1-boottest engine-parity (no Stata oracle; run twice, compare) ==="
* NOTE: the prerequisite fit MUST be vce(cluster ...) — boottest is a WILD
* CLUSTER bootstrap; the wiring honest-rejects after a plain-iid fit
* ("boottest requires a preceding regression with vce(cluster ...)").
cap noi regress income beertax spirits, vce(cluster state)
cap noi boottest beertax, reps(499) seed(20260715)
di "rc=" _rc

di "=== D1 REFERENCE END ==="
