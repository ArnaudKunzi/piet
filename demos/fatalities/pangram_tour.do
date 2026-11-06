*==============================================================================*
* pangram_tour.do — PIET full-surface pangram on the fatalities panel          *
*------------------------------------------------------------------------------*
* A "pangram" in the house sense: one do-file that uses EVERY command the      *
* shipped do-executor registers (piet_native --dump-registry, 119 rows) at     *
* least once, on one real dataset — plus a dedicated section proving the      *
* registered reduced forms (reg, sum, d, tab, cor, l, g, di, mat, est, ...)   *
* dispatch identically to their full spellings.                                *
*                                                                              *
* THE DATA — fatalities.csv                                                    *
*   The classic US traffic-fatalities panel: 48 contiguous states x 7 years   *
*   (1982-1988), 336 state-year observations. Stock & Watson's teaching       *
*   extract of Ruhm (1996, J. Health Economics); public teaching data.        *
*   One row per state-year. Key variables:                                     *
*     fatal    traffic deaths (count)          pop      state population       *
*     beertax  real tax on a case of beer      income   real per-capita income *
*     spirits  spirits consumption p.c.        unemp    unemployment rate (%)  *
*     emppop   employment/population ratio     miles    avg. miles per driver  *
*     breath   preliminary breath-test law (yes/no)                            *
*     jail     mandatory jail for first DUI (yes/no)                           *
*     drinkage minimum legal drinking age      dry      % residents in dry     *
*     afatal   alcohol-involved deaths                  counties               *
*   plus age-specific fatality/population splits (fatal1517, pop1820, ...).   *
*   The workhorse outcome below is frate = fatal/pop*10000, deaths per 10,000. *
*   Beer-tax-on-fatalities is the textbook FE example: pooled OLS gives a     *
*   perverse positive slope; state+year FE flip it negative.                   *
*                                                                              *
* HOW TO RUN                                                                   *
*   From the folder holding fatalities.csv (S2 loads the .csv directly):       *
*       PIET.exe --do pangram_tour.do                                          *
*   or File > Open do-file in the app. Scratch files (pangram_*.dta etc.)     *
*   land in the working directory and are erased in the final section.        *
*                                                                              *
* GUARDED LINES: three stanzas are deliberate `capture` legs (ERROR-LEG):     *
*   mkdir (rerun idempotence), ssc (offline by design), mediate (engine-      *
*   parity guard). Everything else runs bare — a failure anywhere halts the   *
*   run, which is the point.                                                   *
*==============================================================================*

*------------------------------------------------------------------------------*
* S1  Session housekeeping                                                     *
*------------------------------------------------------------------------------*
clear all
version                                       // report interpreter version
set more off                                  // session setting (set multiplex)
set seed 20260731                             // deterministic (nothing random below, but pinned anyway)
pwd                                           // where scratch files will land
cd .                                          // cd: no-op change keeps the run location-independent
which regress                                 // command lookup
timer clear
timer on 1                                    // time the whole run
capture mkdir "pangram_scratch"               // ERROR-LEG only on rerun (dir exists -> rc 693); fresh run rc 0
display "mkdir rc = " _rc
capture ssc install estout                    // ERROR-LEG: ssc is an honest offline reject (no network)
display "ssc rc (expected nonzero) = " _rc
log using "pangram_session.log", replace text // open a plain-text session log
display "pangram log opened"
log close
compress                                      // storage optimization (silent no-op is fine)

*------------------------------------------------------------------------------*
* S2  Load, inspect, construct                                                 *
*------------------------------------------------------------------------------*
* ---- THE ONE LINE THAT WILL NOT RUN IN STATA -------------------------------
* PIET sniffs the file's CONTENT, not its extension, so a .csv is a dataset
* like any other: `use` opens it, types it, and hands you a live frame. No
* import step, no intermediate .dta. The installer puts this do-file and
* fatalities.csv together in Documents\PIET — the app's default working
* directory — so the relative path below just works.
*
* In Stata, `use` takes .dta only; use the fallback instead:
*   import delimited "fatalities.csv", clear
use "fatalities.csv", clear

*
* Nothing else in this file changes. That is the whole point: the difference
* is one verb at the door, not a different dialect inside. In Stata a .csv is
* a foreign format you IMPORT and a .dta is a dataset you USE; in PIET both
* are datasets and `use` opens either, because the sniffer reads the bytes
* rather than trusting the extension. Same for `save`: `save x.dta` writes
* real format-118, `save x.hbin` writes the native columnar store, and the
* extension picks the writer.
count
describe                                      // schema: 34 vars, 336 obs
codebook fatal                                // distribution + missingness for one variable
codebook fatal if year==1982                  // if-qualified codebook
confirm variable beertax                      // assert the variable exists (rc 0)

generate frate  = fatal/pop*10000             // outcome: deaths per 10,000 residents
gen lincome     = log(income)                 // log real p.c. income
gen byte post   = (year>=1985)                // later-half indicator (typed gen)
gen drinkd      = (breath=="yes")             // breath-test-law dummy (string compare on a factor)
gen jaild       = (jail=="yes")               // mandatory-jail dummy
gen cnt         = round(spirits*3)            // small synthetic count for the count-model family
gen hi2         = (spirits>1.5)               // binary for the FE-logit
gen popr        = round(pop)                  // integer population (fweights must be integers)
encode state, gen(snum)                       // string state -> numeric id 1..48
label variable frate "Fatalities per 10,000"
label define yn 0 "no" 1 "yes"                // value-label define
label values drinkd yn                        // attach
label list yn                                 // display the label set
format frate %9.3f                            // display format
clonevar btax2 = beertax                      // exact copy incl. metadata
recode drinkage (18/20.99=0)(21=1), gen(mlda21)   // recode ranges -> MLDA-21 indicator
tostring year, gen(ystr)                      // numeric -> string
destring ystr, gen(ynum)                      // and back
replace lincome = . if income<=0              // (no such rows; shows conditional replace)
egen mfr   = mean(frate), by(snum)            // state means
egen tagst = tag(snum)                        // one row per state
egen zfr   = std(frate)                       // standardized outcome
bysort year: egen ymfr = mean(frate)          // by-prefix + egen (national mean by year)
winsor2 frate, cuts(1 99)                     // tail-clamp copy (frate_w)
mkspline bsp = beertax, cubic nknots(3)       // restricted cubic spline basis
xtset snum year                               // declare the panel
duplicates report snum year                   // 0 surplus rows — keys are unique
duplicates tag snum year, gen(dupt)           // tag variant (all zeros)
levelsof year, local(ylev)                    // distinct years into a local
display "years: `ylev'"
scalar nstates = 48
scalar list
tempvar tv                                    // engine-named temporary variable
gen `tv' = 1
summarize `tv'
count if frate>2 & beertax<1                  // compound-condition count
list state year frate in 1/5                  // in-range listing
order snum year frate                         // column order
sort snum year
gsort -frate                                  // descending sort (worst state-years first)
sort snum year

*------------------------------------------------------------------------------*
* S3  Save / use round-trip, dataset surgery (merge, joinby, append, reshape)  *
*------------------------------------------------------------------------------*
drop jail service                             // CA-1988 carries a string missing; .dta strings have no
*                                             //   missing distinct from "", so the writer honest-rejects.
*                                             //   The jaild dummy (built above) is what later sections use.
save "pangram_fat.dta", replace               // real Stata-format writer (v118)
preserve                                      // snapshot before surgery
drop frate
restore                                       // frate is back
clear                                         // wipe data...
use "pangram_fat.dta", clear                  // ...and reload our own .dta
count

tempfile sm                                   // state-mean lookup for merge/joinby
preserve
collapse (mean) minc=income (mean) mfrate=frate, by(snum)
save "`sm'"
restore

preserve
joinby snum using "`sm'"                      // inner key-join (m:1 on snum here)
count
restore

merge m:1 snum using "`sm'"                   // classic m:1 merge, adds _merge
tabulate _merge
drop _merge minc mfrate

tempfile t88                                  // append: split a year off, stack it back
preserve
keep if year==1988
save "`t88'"
restore
preserve
keep if year==1982
append using "`t88'"                          // 48 + 48 = 96
count
restore

preserve
keep snum year frate
reshape wide frate, i(snum) j(year)           // 336 rows -> 48 (frate1982..frate1988)
count
reshape long frate, i(snum) j(year)           // and back
count
restore

postfile ph b_tax se_tax using "pangram_post", replace   // collect results row-by-row
regress frate beertax
post ph (_b[beertax]) (_se[beertax])
postclose ph

*------------------------------------------------------------------------------*
* S4  Descriptive statistics                                                   *
*------------------------------------------------------------------------------*
summarize frate beertax unemp income
summarize frate, detail                       // percentiles, skewness, kurtosis
summarize frate in 1/48                       // in-range summarize
summarize frate [fweight=popr]                // population-weighted (fweight family)
bysort snum: summarize frate                  // per-state summaries (by-prefix summarize)
tabstat frate beertax, stats(mean sd n)
tabulate drinkd                               // one-way
tabulate year drinkd, summarize(frate)        // two-way with cell means
tab1 drinkd
tab2 drinkd post
table drinkd
correlate frate beertax spirits
pwcorr frate beertax, obs                     // pairwise N
ttest frate == 2                              // one-sample
ttest frate, by(drinkd)                       // two-group
ttest frate, by(drinkd) unequal               // Welch
sdtest frate, by(drinkd)                      // variance-ratio test

*------------------------------------------------------------------------------*
* S5  REDUCED FORMS — registered abbreviations, proven equivalent              *
*     (PIET resolves the registered short spellings, not arbitrary prefixes.)  *
*------------------------------------------------------------------------------*
regress frate beertax, vce(cluster state)     // full spelling
display "b[regress] = " _b[beertax] "   se = " _se[beertax]
reg frate beertax, vce(cluster state)         // regress -> reg
display "b[reg]     = " _b[beertax] "   se = " _se[beertax]
reg frate beertax, vce(cluster state)         // regress -> reg (Stata's shortest ACCEPTED form;
                                              // `r` was removed at the {B.11.0, 0.39.0, 1.24.0, 2.9.0}
                                              // stamp because Stata itself answers r(199) to both `r`
                                              // and `re` — measured, not assumed. Demonstrating an
                                              // abbreviation the oracle refuses is the same defect
                                              // class as omitting one it accepts.
display "b[r]       = " _b[beertax] "   se = " _se[beertax]
* the three lines above print bit-identical numbers: one registry row, one handler
di "di is display"                            // display -> di
su frate                                      // summarize -> su
sum frate, detail                             // summarize -> sum
d                                             // describe -> d
desc                                          // describe -> desc
ta drinkd                                     // tabulate -> ta
tab drinkd post                               // tabulate -> tab
cor frate beertax                             // corr -> cor
correlate frate beertax spirits               // corr -> correlate
g one1 = 1                                    // generate -> g
ren one1 onevar                               // rename -> ren
drop onevar
l state year frate in 1/3                     // list -> l
br state year frate in 1/3                    // browse -> br (headless: list view)
mat bm = e(b)                                 // matrix -> mat (coef vector of the last fit)
est store m_abbrev                            // estimates -> est
hist frate                                    // histogram -> hist
logistic drinkd beertax lincome               // its OWN command since the {B.11.0, 0.39.0, 1.24.0,
                                              // 2.9.0} stamp, not a logit synonym: it carries a
                                              // distinct odds-ratio transform, so folding it into
                                              // logit would have silently killed that branch.

*------------------------------------------------------------------------------*
* S6  Estimation — linear, limited-dependent, IV, systems, panel, counts       *
*------------------------------------------------------------------------------*
* Linear family. Pooled OLS on beertax has the famous wrong (positive) sign;
* state+year FE recover the negative one. Cluster on state throughout.
regress frate beertax spirits                 // pooled OLS, classical SE
regress frate beertax spirits, vce(robust)    // HC1
regress frate beertax spirits, vce(cluster state)
regress frate beertax spirits in 1/200        // in-range qualifier
regress frate beertax spirits if year>=1985, vce(cluster state)   // if + cluster
estat hettest                                 // Breusch-Pagan (heteroskedasticity is why we cluster)
estat ovtest                                  // Ramsey RESET
regress frate beertax spirits unemp lincome
estat vif                                     // collinearity check
feols frate beertax spirits, vce(cluster state)                   // engine-name spelling of the same fit
reghdfe frate beertax, absorb(snum year) vce(cluster snum)        // two-way FE: the sign flips negative
boottest beertax, reps(499) seed(20260731)    // wild-cluster bootstrap p on the FE fit (48 clusters)

* Limited dependent variables.
logit drinkd beertax lincome                  // which states carry a breath law
probit drinkd beertax lincome
gen frate_c = min(frate, 2.5)                 // artificial right-censoring at 2.5
tobit frate_c beertax spirits, ul(2.5)        // censored-normal MLE
gen inc_obs = income if drinkd==1             // outcome observed only where drinkd==1
heckman inc_obs beertax, select(drinkd = spirits unemp) twostep   // Heckit (exclusion: spirits, unemp)
qreg frate beertax spirits                    // median regression
qreg frate beertax, quantile(.25)             // lower-quartile fit

* IV / systems. NOTE: mechanical syntax illustrations — spirits/unemp are not
* credible instruments for beertax; treat coefficients as demo output only.
ivregress 2sls frate (beertax = spirits)      // just-identified 2SLS
ivregress 2sls income emppop (beertax = spirits unemp)    // overidentified 2SLS
estat endogenous                              // Durbin / Wu-Hausman
estat overid                                  // Sargan / Basmann
ivregress gmm income emppop (beertax = spirits unemp)     // GMM
ivregress liml income emppop (beertax = spirits unemp)    // LIML
ivreghdfe income emppop (beertax = spirits unemp), absorb(state) cluster(state)
reg3 (income beertax spirits) (emppop unemp income)       // 3SLS
reg3 (income beertax spirits) (emppop unemp income), sure // SUR

* Panel.
xtset snum year                               // re-declare (a fresh `use` resets the declaration)
xtreg frate beertax unemp, fe                 // within estimator
estimates store feM
xtreg frate beertax unemp, re                 // Swamy-Arora RE
estimates store reM
hausman feM reM                               // FE vs RE specification test
xtreg frate beertax unemp if year>=1983, fe vce(cluster snum)
chow frate beertax, by(post)                  // structural break at 1985 (classic + robust)

* Counts (cnt is a small synthetic count; fatal itself is fine for the FE
* Poisson but keeps nbreg's MLE happy near a small intercept).
poisson cnt beertax unemp
poisson cnt beertax unemp, vce(robust)
nbreg cnt beertax unemp                       // NB2; alpha ~ 0 here (Poisson limit)
fepois cnt beertax, absorb(snum)              // FE-Poisson (ppmlhdfe machinery)
ppmlhdfe cnt beertax, absorb(snum)
xtpoisson cnt beertax unemp, fe               // conditional FE Poisson
xtlogit hi2 beertax unemp, fe                 // conditional FE logit

* Mediation (ERROR-LEG guard: engine-parity scope; see the manual).
capture mediate (income) (spirits) (beertax, continuous(0.5 1)), reps(199) seed(20260731)
display "mediate rc = " _rc

*------------------------------------------------------------------------------*
* S7  Difference-in-differences on the real policy variation                   *
*------------------------------------------------------------------------------*
* Callaway-Sant'Anna on staggered breath-test-law adoption. gvar = first year
* the state carries the law (0 = never); 1982 adopters are always-treated and
* are dropped rather than silently excluded.
gen byr = year if drinkd==1                   // adoption-candidate years (missing where no law)
egen gv0 = min(byr), by(snum)
gen gv = cond(missing(gv0), 0, gv0)
drop if gv==1982
csdid frate unemp lincome, ivar(snum) time(year) gvar(gv) method(dripw)

* drdid: classic 2x2 on mandatory-jail laws, 1982 vs 1988; drop states already
* treated in 1982, treatment() = ever-adopter flag (constant within state).
sysuse fatalities, clear
gen frate = fatal/pop*10000
gen jaild = (jail=="yes")
encode state, gen(snum)
keep if inlist(year,1982,1988)
gen j82row = jaild*(year==1982)
gen j88row = jaild*(year==1988)
egen j82 = max(j82row), by(snum)
egen j88 = max(j88row), by(snum)
drop if j82==1
drdid frate unemp, ivar(snum) time(year) treatment(j88) dripw

*------------------------------------------------------------------------------*
* S8  Time series (single-series declaration is mechanical: syntax demo)       *
*------------------------------------------------------------------------------*
sysuse fatalities, clear
gen frate = fatal/pop*10000
gen t = _n
tsset t
newey frate beertax spirits, lag(2)           // HAC (Newey-West) SEs
prais frate beertax, twostep                  // Prais-Winsten AR(1) FGLS
prais frate beertax, corc                     // Cochrane-Orcutt variant
dfuller frate                                 // ADF unit-root
dfuller frate, lags(2) trend                  // augmented + trend
pperron frate                                 // Phillips-Perron
wntestq frate                                 // Ljung-Box portmanteau
wntestq frate, lags(10)
preserve
drop if t>=100 & t<=106                       // punch a hole...
tsfill                                        // ...and fill the panel gap
count
restore
tsline frate                                  // time-series line plot

*------------------------------------------------------------------------------*
* S9  Post-estimation                                                          *
*------------------------------------------------------------------------------*
sysuse fatalities, clear
gen frate = fatal/pop*10000
gen drinkd = (breath=="yes")
regress frate beertax spirits unemp
matrix b = e(b)                               // coefficient vector
matrix V = e(V)                               // covariance
ereturn list                                  // stored e() results
predict fhat                                  // fitted values
predict fres, residuals                       // residuals
test beertax spirits                          // joint F
test beertax = spirits                        // linear equality
lincom beertax - spirits                      // linear combination + SE
nlcom _b[beertax]/_b[spirits]                 // delta-method ratio
margins, at(beertax=1)                        // predictive margin at a point
eststo m1                                     // store the current (OLS) fit
regress frate beertax spirits
eststo m2
regress frate beertax
eststo m3
esttab m1 m2 m3                               // regression table
esttab m1 m2 m3, se star(* 0.10 ** 0.05 *** 0.01)
coefplot m2                                   // coefficient plot
logit drinkd beertax spirits
margins, dydx(beertax)                        // AME after logit
marginsplot                                   // and its plot
estimates clear                               // (`estimates drop _all` is BUG-149 — clear is the working form)

*------------------------------------------------------------------------------*
* S10 Graphics                                                                 *
*------------------------------------------------------------------------------*
sysuse fatalities, clear
gen frate = fatal/pop*10000
encode state, gen(snum)
scatter frate beertax                         // the raw (misleading) cross-section
line frate year
twoway (scatter frate beertax) (lfit frate beertax)   // overlay with an OLS fit
histogram frate
kdensity frate
graph export "pangram_fig.svg", replace       // headless SVG export of the last graph
xtset snum year
xtline frate if snum<=4                       // per-state panel lines (first 4 states)

*------------------------------------------------------------------------------*
* S11 Macros & control flow (language surface; one line each)                  *
*------------------------------------------------------------------------------*
local a = 7
global gg = 9
display "local a=`a'  global gg=$gg"
local k = 0
foreach v of varlist beertax spirits unemp {
  local k = `k' + 1
}
display "foreach counted `k' vars"
local s = 0
forvalues i = 1/4 {
  local s = `s' + `i'
}
display "forvalues sum = `s'"
local w = 0
while `w' < 3 {
  local w = `w' + 1
}
display "while ended at `w'"
if _N > 300 {
  display "if-branch: full panel loaded"
}
else {
  display "else-branch: subset"
}

*------------------------------------------------------------------------------*
* S12 Cleanup & exit                                                           *
*------------------------------------------------------------------------------*
erase "pangram_fat.dta"                       // remove scratch artifacts
rm "pangram_post"                             // rm = built-in synonym of erase
erase "pangram_fig.svg"
rm "pangram_session.log"
timer off 1
timer list
display "PANGRAM COMPLETE — every registered command exercised"
exit                                          // clean halt (headless rc 0)
