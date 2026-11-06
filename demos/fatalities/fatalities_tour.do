*==============================================================================*
* fatalities_tour.do  —  PIET / cobra capability "pangram"                       *
*                                                                                *
* One end-to-end tour of the core econometrics PIET 2.0.4 ships, run on the      *
* bundled US traffic-fatalities panel (48 states x 1982-1988; Stock & Watson's   *
* teaching extract of Ruhm 1996). Every block below is verified GREEN through    *
* the shipped app binary via demos/run_demos.sh (zero error blocks).             *
*                                                                                *
* HOW TO RUN                                                                      *
*   Anywhere. §1 loads the bundled sample via sysuse (bundle-first), so this     *
*   tour runs from ANY working directory on ANY install — File > Open do-file,   *
*   Run .do, or headless:                                                         *
*       PIET.app/Contents/MacOS/piet_native --do fatalities_tour.do              *
*                                                                                *
* The companion HANDBOOK.md explains every model, the standard-error choice,     *
* the identifying assumptions, and interprets the actual numbers this script     *
* produces. Comments here say only WHAT each command does.                        *
*                                                                                *
* HONEST SCOPE NOTES (roadmap gaps — substitutions are flagged in-line and in    *
* the handbook): the shipped do-executor does not yet wire ivregress/ivreghdfe   *
* (§5 builds the just-identified IV by hand from OLS), boottest wild bootstrap   *
* (§4 shows the analytic SE hierarchy instead), poisson/nbreg (§6 uses the       *
* log-linear analogue), or newey HAC (§9 uses plain ts operators).               *
*==============================================================================*

clear all
set more off
set type double

*------------------------------------------------------------------------------*
* §1  DATA: load, inspect, construct                                            *
*------------------------------------------------------------------------------*
sysuse fatalities, clear                     // bundled sample — loads on ANY install, ANY working dir
*   (sysuse resolves the app bundle first, so this tour runs immediately after
*    installation with no cd needed. The kit folder also ships fatalities.csv
*    for Stata users / transparency: in Stata, import delimited + see HANDBOOK.)
describe                                      // variable list, types, formats
gen frate   = fatal/pop*10000                // outcome: traffic deaths per 10,000 people
gen lincome = log(income)                     // log real per-capita income
gen lfatal  = log(fatal)                      // log death count (for §6 exposure model)
gen lpop    = log(pop)                         // log population (exposure proxy)
gen drinkd  = (breath == "yes")               // 1 if state has a breathalyzer (preliminary-breath-test) law
egen statenum = group(state)                  // integer state id (48 states)
xtset statenum year                           // declare the panel (id=state, time=year)
label variable frate   "Fatalities per 10,000"
label variable beertax "Real beer tax ($/case)"
summarize frate beertax unemp income          // headline summary statistics
tabulate breath                               // how many state-years carry the breath law

*------------------------------------------------------------------------------*
* §2  POOLED OLS: the naive cross-sectional regression and why its SEs lie      *
*------------------------------------------------------------------------------*
regress frate beertax                         // pooled OLS, classical (iid) SEs
estat hettest                                 // Breusch-Pagan/Cook-Weisberg heteroskedasticity test
predict ols_resid, residuals                  // store OLS residuals for inspection
regress frate beertax, vce(robust)            // identical slope, HC1 heteroskedasticity-robust SEs
regress frate beertax unemp lincome           // add unemployment and log-income controls
estat vif                                     // variance inflation factors (collinearity check)
estat ovtest                                  // Ramsey RESET test for functional-form misspecification

*------------------------------------------------------------------------------*
* §3  PANEL FIXED EFFECTS: within estimator, one-way then two-way, clustered    *
*------------------------------------------------------------------------------*
xtreg frate beertax unemp lincome, fe vce(cluster statenum)          // state FE, SE clustered by state
reghdfe frate beertax unemp lincome, absorb(statenum year) vce(cluster statenum)  // two-way (state+year) FE
test beertax unemp lincome                    // joint Wald test that all three slopes are zero
xtreg frate beertax unemp lincome, re         // random-effects GLS, for contrast with FE

*------------------------------------------------------------------------------*
* §4  INFERENCE HIERARCHY: same two-way FE point estimate, three SE regimes     *
*     (boottest wild cluster bootstrap is a roadmap gap — see handbook §4)      *
*------------------------------------------------------------------------------*
reghdfe frate beertax unemp lincome, absorb(statenum year)                        // default (iid) SEs
reghdfe frate beertax unemp lincome, absorb(statenum year) vce(robust)            // HC1 robust SEs
reghdfe frate beertax unemp lincome, absorb(statenum year) vce(cluster statenum)  // cluster-robust, 48 clusters

*------------------------------------------------------------------------------*
* §5  IV / 2SLS BY HAND: indirect least squares from OLS building blocks.       *
*     (ivregress/ivreghdfe are a roadmap gap; the just-identified IV estimate   *
*      is reduced-form slope / first-stage slope — computed in handbook §5.)    *
*     DIDACTIC ONLY: neither instrument's exclusion restriction is credible.    *
*------------------------------------------------------------------------------*
regress beertax baptist unemp lincome         // first stage: % Baptist as instrument for beer tax
test baptist                                  // first-stage F on the instrument (weak-IV diagnostic)
regress beertax dry unemp lincome             // contrast first stage: % dry-county population (a WEAK instrument)
test dry                                       // its first-stage F — expect it far below 10
regress frate baptist unemp lincome           // reduced form: outcome on the same instrument

*------------------------------------------------------------------------------*
* §6  COUNT DATA / EXPOSURE: log-linear analogue of a rate model                *
*     (poisson ... , exposure(pop) is a roadmap gap; here log(pop) enters as a  *
*      free exposure term whose slope Poisson-exposure would restrict to 1.)    *
*------------------------------------------------------------------------------*
reghdfe lfatal beertax unemp lincome lpop, absorb(statenum year) vce(cluster statenum)  // log-count model with free exposure

*------------------------------------------------------------------------------*
* §7  STAGGERED DIFFERENCE-IN-DIFFERENCES: Callaway & Sant'Anna (2021)          *
*------------------------------------------------------------------------------*
gen treatyr = year if drinkd == 1             // year-of-treatment marker (missing when untreated)
egen gvar = min(treatyr), by(statenum)        // cohort = first year the state has the breath law
replace gvar = 0 if missing(gvar)             // never-treated states get cohort 0
gen byte evertreat = (gvar > 0)               // ever-treated indicator (for the §8 raw-means plot)
tabulate gvar                                 // cohort sizes; the 1982 cohort is always-treated -> dropped by CS
csdid frate unemp lincome, ivar(statenum) time(year) gvar(gvar) method(dripw)  // doubly-robust CS estimator
*   NOTE (BUG-040, 2026-07-09): no cluster() here — Stata's csdid rejects
*   cluster(statenum)==ivar(statenum) with r(198) ("may not be both target and
*   by()"), and cobra >= 0.20.1 mirrors that rejection. CS default-clusters on
*   the panel unit (ivar) anyway, so the SEs are the clustered ones either way.
estat simple                                  // overall (weighted-average) ATT
estat event                                   // event-study: dynamic ATT by time-to-treatment (pre-trends live here)
estat calendar                                // ATT by calendar year
estat group                                   // ATT by treatment cohort — all 4 cohorts incl. the 1-state 1987
*                                             // cohort (complete separation kept at the likelihood plateau,
*                                             // cobra >= 0.20.1 / BUG-036; re-enabled 2026-07-09)

*------------------------------------------------------------------------------*
* §8  GRAPHICS: exported as SVG (PNG/PDF export is a roadmap gap)               *
*------------------------------------------------------------------------------*
twoway (scatter frate beertax) (lfit frate beertax)   // scatter of the raw cross-section + OLS fit line
graph export "fig_scatter.svg", replace               // write the scatter to SVG
eststo clear                                          // reset the stored-estimates store
quietly reghdfe frate beertax unemp lincome, absorb(statenum year) vce(cluster statenum)
eststo m_fe                                           // stash the two-way FE model
coefplot m_fe, keep(beertax)                          // coefficient plot of the beer-tax slope (renders inline)
preserve                                              // work on a temporary collapsed copy
collapse (mean) frate, by(year evertreat)             // mean fatality rate per year x ever-treated group
twoway (line frate year if evertreat==1) (line frate year if evertreat==0)  // raw parallel-trends picture
graph export "fig_did.svg", replace                   // write the DiD raw-means plot to SVG
restore                                               // bring back the full panel

*------------------------------------------------------------------------------*
* §9  DYNAMICS via time-series operators (newey/HAC is a roadmap gap)           *
*------------------------------------------------------------------------------*
regress frate L.beertax unemp lincome         // one-year lag of beer tax as regressor
regress frate beertax L.beertax unemp lincome // contemporaneous tax plus its lag
regress frate D.beertax unemp lincome         // first-difference of beer tax on the right-hand side
regress frate L.frate beertax                 // lagged dependent variable (illustrates Nickell-bias concern)

*==============================================================================*
* End of tour. Expected: every block above renders with no error block.         *
*==============================================================================*
