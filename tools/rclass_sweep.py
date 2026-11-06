#!/usr/bin/env python3
"""rclass_sweep.py — does each class command publish what Stata documents?

WHY (INSPECT-2608 W1 lens L6): capture-based gates compare what a command
PRINTS. BUG-353/354 were about what a command FAILS TO PUBLISH, alive while
freeze/eval/ctest were all GREEN — a captured-output corpus can't see an
absence it never asked for.

ORIGINAL SHAPE, KEPT: a hardcoded 10-row TABLE of curated commands, swept
PRESENCE-only (`capture noisily display r(NAME)`, grep rc=0), rows read off a
live StataBE 19.5 session by hand. Preserved byte-identical below as
FALLBACK_TABLE, run by the legacy code path — the tool never loses today's
capability when no --table is supplied.

THREE MODES NOW:
  --gen-capture OUTDIR      parse registry.cpp's kRegistry (ONE read, lines
                             121-929) and emit a Stata capture package: a
                             master do-file for REAL StataBE 19.5 + a manifest.
  --from-capture LOG -o OUT parse the resulting Stata log into a table JSON
                             {scenario: {command, setup, cmd, r_*, e_*}} —
                             this REPLACES the hardcoded TABLE.
  (default) [--table JSON]  sweep PIET: PRESENCE, and wherever a value was
                             captured, VALUE within --rtol. No --table ==
                             legacy presence-only sweep against FALLBACK_TABLE.

Report-only by design: exits 0 and prints what is missing/mismatched unless
--strict.
"""
import argparse, json, os, re, subprocess, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import engine_arm  # noqa: E402  (same directory)
from collections import namedtuple
from string import Template

# --------------------------------------------------------------------------
# FALLBACK_TABLE — the original TABLE, unchanged. name -> (setup lines,
# command, kind, core names, full Stata list as measured 2026-08-18).
# --------------------------------------------------------------------------
FALLBACK_TABLE = [
    ("summarize", [], "summarize beertax", "r",
     ["N", "mean", "sd", "min", "max", "sum"],
     "N sum_w mean Var sd min max sum"),
    ("summarize_detail", [], "summarize beertax, detail", "r",
     ["N", "mean", "sd", "p25", "p50", "p75", "skewness", "kurtosis"],
     "N sum_w mean Var sd skewness kurtosis sum min max p1 p5 p10 p25 p50 p75 p90 p95 p99"),
    ("count", [], "count", "r", ["N"], "N"),
    ("correlate", [], "correlate beertax unemp", "r", ["N", "rho"], "N rho"),
    ("pwcorr", [], "pwcorr beertax unemp", "r", ["N", "rho"], "N rho"),
    ("ttest", [], "ttest beertax == .5", "r",
     ["N_1", "mu_1", "t", "p", "se", "df_t"],
     "ub_1 lb_1 level sd_1 se p_u p_l p t df_t mu_1 N_1"),
    ("tabulate", [], "tabulate year", "r", ["N", "r"], "N r"),
    ("regress", [], "regress fatal beertax", "e",
     ["N", "df_m", "df_r", "F", "r2", "rmse", "mss", "rss"],
     "N sum_w df_m df_r F r2 rmse mss rss r2_a ll ll_0 rank"),
    ("logit", ["gen byte hi = (fatal > 1000)"], "logit hi beertax", "e",
     ["N", "ll", "ll_0", "df_m", "chi2", "p", "r2_p"],
     "rank N ic k k_eq noconstant consonly k_dv converged rc k_autoCns ll k_eq_model "
     "ll_0 df_m chi2 p N_cdf N_cds r2_p"),
    ("probit", ["gen byte hi2 = (fatal > 1000)"], "probit hi2 beertax", "e",
     ["N", "ll", "ll_0", "df_m", "chi2", "p", "r2_p"],
     "(same shape as logit — measured identical)"),
]

# BUG-372: this used to emit `sysuse fatalities`, and the first scenario of the
# first real capture run died on it — `sysuse` resolves only Stata's OWN bundled
# datasets, and `fatalities` is ours (we ship it as a CSV in the demo package).
# The capture package now loads the .dta we generate ourselves
# (internal script fatalities_fixtures.do), passed in as FATDTA so the
# do-file is not tied to one checkout path.
# BUG-372: this used to emit `sysuse fatalities`, and the FIRST scenario of the
# first real capture run died on it — Stata's `sysuse` resolves only StataCorp's
# OWN bundled datasets, and `fatalities` is ours (shipped as a CSV in the demo
# package). The generator now writes an explicit `use "<abs path>"` of the .dta
# we produce ourselves (internal script fatalities_fixtures.do), resolved
# at GENERATION time rather than through a Stata local — these two strings are
# substituted into ~155 emitted lines and a macro would put a quoting hazard in
# every one of them.
FIXTURE_DTA = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
                           "tests", "validate", "dta_fixtures", "fatalities_118.dta")
def _fixture_load(quiet):
    return ('quietly ' if quiet else '') + 'use "%s", clear' % FIXTURE_DTA

# --------------------------------------------------------------------------
# SKIP_NO_RCLASS — commands genuinely believed to publish no r()/e() at all:
# pure display/graph/settings/control-flow, per the task's stated categories.
# EXPLICIT dict, never a family-derived silent filter. `rdplot` stays OUT
# despite Family::Graph — registry.cpp's comment on that row says it
# "additionally prints a table", a live hint of real r() output, so it's left
# for the operator to determine empirically rather than guessed away.
#
# NOT included, on purpose: the much larger body of DataMgmt/IO mutation/file
# verbs (generate, replace, egen, drop, sort, rename, merge, export, cd, ...)
# that are ALSO plausibly zero-r()/e(). Adding those would mean guessing past
# the task's stated categories; instead they stay in the capture target list
# and the live Stata capture decides empirically (a zero-yield capture there
# costs nothing downstream).
# --------------------------------------------------------------------------
SKIP_NO_RCLASS = {
    "display":  "display",
    "set":      "settings",
    "version":  "settings",
    "compress": "settings",
    "exit":     "control-flow",
    "preserve": "control-flow",
    "restore":  "control-flow",
    "tempvar":  "macro",       # dispatcher's U4 pre-tokenizer intercept, not a 809-2203 arm
    "scatter":     "graph",
    "histogram":   "graph",
    "kdensity":    "graph",
    "line":        "graph",
    "xtline":      "graph",
    "tsline":      "graph",
    "twoway":      "graph",
    "graph":       "graph",
    "binscatter":  "graph",
    "coefplot":    "graph",
    "marginsplot": "graph",
}

# --------------------------------------------------------------------------
# SKIP_VALUE — commands whose VALUES are legitimately seed/replicate-dependent
# (bootstrap/permute/jackknife class). Presence is still checked in sweep
# mode; value comparison is skipped and the reason is recorded in the report.
# Only `boottest` is a registry row of this class — `bootstrap`/`permute`/
# `jackknife`/`rolling` are PREFIXES (registry.cpp's own comment: "prefixes
# carry no registry row"), so they never appear as target commands at all.
# --------------------------------------------------------------------------
SKIP_VALUE = {
    "boottest": "bootstrap",
}

# --------------------------------------------------------------------------
# Default-invocation guesses for commands NOT already covered by
# FALLBACK_TABLE. All of these are tagged invocation_confidence="guess" in
# the manifest --gen-capture writes; the operator repairs refused ones after
# the first live Stata pass, per the task brief. Only the fixture's own known
# variables are used (beertax, unemp, fatal, year) — this script does not
# independently inspect the .dta schema.
# --------------------------------------------------------------------------
HI_SETUP = ["gen byte hi = (fatal > 1000)"]
ORD_SETUP = ["egen __ord = cut(fatal), group(3)"]
# BUG-372, second half. The comment above says this script "does not
# independently inspect the .dta schema", and the first live capture run is what
# that cost: fatalities is a PANEL (48 states x 7 years), so
#   `xtset year`  declared YEAR as the panel id — syntactically legal, so it ran,
#                 but every xt* scenario underneath it was estimating nonsense;
#   `tsset year`  is an outright error (repeated time values within the panel),
#                 which in batch mode aborts the whole do-file.
# Measured on the real fixture: 336 rows, year takes 7 distinct values.
XT_SETUP = ["egen long __id = group(state)", "xtset __id year"]
# A time series needs ONE series, so narrow to a single state first.
TS_SETUP = ['keep if state == "al"', "tsset year"]
ST_SETUP = HI_SETUP + ["stset year, failure(hi)"]
REGRESS_SETUP = ["regress fatal beertax"]

BINARY_ML = {"logit", "probit", "logistic"}
ORDINAL_ML = {"ologit", "oprobit", "mlogit"}

SUGGESTED_INVOCATIONS = {
    # ---- panel / time-series (need a prior xtset/tsset) ----
    "xtset":      (["egen long __id = group(state)"], "xtset __id year"),
    "tsset":      (['keep if state == "al"'], "tsset year"),
    "xtreg":      (XT_SETUP, "xtreg fatal beertax, fe"),
    "xtabond":    (XT_SETUP, "xtabond fatal beertax"),
    "xtpoisson":  (XT_SETUP, "xtpoisson fatal beertax, fe"),
    "xtlogit":    (XT_SETUP + HI_SETUP, "xtlogit hi beertax, fe"),
    "var":        (TS_SETUP, "var fatal beertax"),
    "vargranger": (TS_SETUP + ["var fatal beertax"], "vargranger"),
    "irf":        (TS_SETUP + ["var fatal beertax"], "irf create __irf1, step(2)"),
    "newey":      (TS_SETUP, "newey fatal beertax, lag(1)"),
    "prais":      (TS_SETUP, "prais fatal beertax"),
    "dfuller":    (TS_SETUP, "dfuller fatal"),
    "pperron":    (TS_SETUP, "pperron fatal"),
    "wntestq":    (TS_SETUP, "wntestq fatal"),
    "tsfill":     (TS_SETUP, "tsfill"),
    "mixed":      ([], "mixed fatal beertax || year:"),
    "svyset":     ([], "svyset year"),

    # ---- survival ----
    "stset": (HI_SETUP, "stset year, failure(hi)"),
    "stcox": (ST_SETUP, "stcox beertax"),
    "streg": (ST_SETUP, "streg beertax, distribution(exponential)"),

    # ---- models needing option syntax a generic "cmd fatal beertax" template
    # gets structurally wrong (not just imprecise): a required paren group,
    # link word, select()/inflate()/by(), or a single- not double-varlist. ----
    "zip":       (HI_SETUP, "zip fatal beertax, inflate(hi)"),
    "zinb":      (HI_SETUP, "zinb fatal beertax, inflate(hi)"),
    "fracreg":   (HI_SETUP, "fracreg probit hi beertax"),
    "heckman":   (HI_SETUP, "heckman fatal beertax, select(hi = beertax unemp)"),
    "mediate":   (HI_SETUP, "mediate (hi beertax) (fatal hi beertax)"),
    "ivregress": ([], "ivregress 2sls fatal (beertax = unemp)"),
    "ivreghdfe": ([], "ivreghdfe fatal (beertax = unemp)"),
    "reg3":      ([], "reg3 (fatal beertax) (unemp beertax)"),
    "nl":        ([], "nl (fatal = {b0} + {b1}*beertax)"),
    "gmm":       ([], "gmm (fatal - {b0} - {b1}*beertax), instruments(beertax)"),
    "rddensity": ([], "rddensity beertax, c(1)"),  # single running var, not a two-var template
    "csdid":     (HI_SETUP, "csdid fatal, ivar(year) time(year) gvar(year)"),
    "drdid":     (HI_SETUP, "drdid fatal beertax, time(year) treatment(hi)"),
    # Remaining Arc F / count-model rows (tobit, poisson, ppmlhdfe, fepois,
    # nbreg, rdrobust, rdplot, eventstudyinteract, did_imputation, sdid) fall
    # through to the generic Estimation "cmd fatal beertax" template — the
    # registry's own comments confirm the bare/no-option form is valid for
    # tobit/poisson/ppmlhdfe/fepois; the DID/RD ones will likely need repair,
    # same as any other guess-tier row.

    # ---- post-estimation (need a prior estimated model) ----
    "test":        (REGRESS_SETUP, "test beertax"),
    "lincom":      (REGRESS_SETUP, "lincom beertax"),
    "nlcom":       (REGRESS_SETUP, "nlcom _b[beertax]"),
    "margins":     (REGRESS_SETUP, "margins"),
    "predict":     (REGRESS_SETUP, "predict __yhat"),
    "estat":       (REGRESS_SETUP, "estat vif"),
    "estimates":   (REGRESS_SETUP, "estimates store __m1"),
    "eststo":      (REGRESS_SETUP, "eststo __m1"),
    "esttab":      (REGRESS_SETUP, "esttab"),
    "coefplot":    (REGRESS_SETUP, "coefplot"),
    "marginsplot": (REGRESS_SETUP + ["margins beertax"], "marginsplot"),
    "hausman":     (REGRESS_SETUP + ["estimates store __fe", "regress fatal beertax"], "hausman __fe ."),
    "boottest":    (REGRESS_SETUP, "boottest beertax"),
    "sdtest":      ([], "sdtest beertax, by(year)"),
    "chow":        (REGRESS_SETUP, "chow beertax"),
    # AMBIGUOUS (flagged, not silently resolved — see final report): registry.cpp
    # tags `ereturn`'s row "bare `ereturn list` only", so cmd is literally that.
    "ereturn":     (REGRESS_SETUP, "ereturn list"),

    # ---- data management / misc rows whose real syntax a two-var template
    # can't approximate at all (a two-var/two-varlist shape, or a required
    # sub-form) — the rest (clonevar, rename, save, isid, assert, expand,
    # fillin, contract, duplicates, levelsof, winsor2, table, tabstat, mean,
    # label, labelbook, datasignature, codebook, describe) fall through to
    # the generic DataMgmt/Meta template, each syntactically plausible or an
    # acceptable guess-tier failure. ----
    "proportion":    (HI_SETUP, "proportion hi"),
    "mkspline":      ([], "mkspline __s1 100 __s2 = beertax"),
    "format":        ([], "format beertax %9.2f"),
    # AMBIGUOUS (flagged, not silently resolved — see final report): registry.cpp
    # tags `matrix`'s row runMatrixDef (definition), but a bare definition
    # plausibly sets no r()/e() at all; this guess uses the `list` inspection
    # sub-form instead so the probe has something to check. Verify/repair.
    "matrix":        (["matrix __C = (1,2\\3,4)"], "matrix list __C"),
    "scalar":        ([], "scalar list"),

    # ---- REPAIRS from the first live capture (2026-08-31) -------------------
    # 66 of 155 scenarios came back with a non-zero rc from Stata ITSELF, i.e.
    # the guessed invocation was not valid Stata for this fixture. Those
    # commands' returns are UNMEASURED, not proven absent, so each repair below
    # buys back a row of the declared-returns table. The rc from that run is
    # quoted on each line so the next repair pass can tell a fix from a guess.
    #
    # A note on the shape of the mistake: the generic template appends the
    # fixture's two numeric variables to the command name, which for a verb whose
    # syntax is `cmd newvar = expr` (or that takes `using`, or a sub-command)
    # produces something that is not merely imprecise but ungrammatical.
    "generate":  ([], "generate __g1 = beertax * 2"),            # rc 110: name taken
    "replace":   (["gen __r1 = beertax"], "replace __r1 = __r1 * 2"),  # rc 100
    "clonevar":  ([], "clonevar __c1 = beertax"),                # rc 198
    "rename":    (["gen __rn = beertax"], "rename __rn __rn2"),  # rc 198
    "recode":    (["gen __rc1 = beertax"], "recode __rc1 (0/1 = 1)"),  # rc 198
    "egen":      ([], "egen __e1 = mean(beertax)"),              # rc 198
    "encode":    ([], "encode state, gen(__enc)"),               # rc 107: needs a STRING
    "decode":    ([], "decode breathcode, gen(__dec)"),          # rc 182: needs a labeled NUMERIC
    "destring":  (["tostring beertax, gen(__bs) force"], "destring __bs, gen(__bn)"),  # rc 198
    "tostring":  ([], "tostring beertax, gen(__ts) force"),      # rc 198
    "xtile":     ([], "xtile __q = beertax, nq(4)"),             # rc 110
    "ipolate":   ([], "ipolate beertax year, gen(__ip)"),        # rc 102: needs y AND x
    "fillin":    ([], "fillin state year"),                      # rc 102: needs 2+ vars
    "tab2":      ([], "tab2 beertax year"),                       # rc 102: needs 2+ vars
    "duplicates": ([], "duplicates report state"),                # rc 198: needs a sub-verb
    "isid":      ([], "isid state year"),                         # rc 459: beertax is not an id
    "unab":      ([], "unab __u : beer*"),                        # rc 198: colon form
    "label":     ([], "label list"),                              # rc 198: needs a sub-verb
    "reshape":   (["keep state year beertax"], "reshape wide beertax, i(state) j(year)"),  # rc 198
    "frame":     ([], "frame create __f1"),                       # rc 198: needs a sub-verb
    "frget":     (["frame create __f2", "frame __f2: set obs 1"], "frame change __f2"),  # rc 198
    "frlink":    ([], "frame create __f3"),                       # rc 197
    "tssmooth":  (['keep if state == "al"', "tsset year"], "tssmooth ma __ma = fatal, window(1 1 1)"),  # rc 198
    "xtsum":     (["egen long __id = group(state)", "xtset __id year"], "xtsum beertax"),  # rc 459
    "xttab":     (["egen long __id = group(state)", "xtset __id year", "gen byte __b = (fatal>1000)"],
                  "xttab __b"),                                   # rc 459
    "testparm":  (["regress fatal beertax unemp"], "testparm beertax unemp"),  # rc 301: no prior fit
    "sdtest":    (["gen byte __two = (year > 1984)"], "sdtest beertax, by(__two)"),  # rc 420: >2 groups
    # cross needs a real `using` file, so make one from the fixture itself.
    "cross":     (["preserve", "keep state", "duplicates drop state, force",
                   "save __cross_u.dta, replace", "restore"],
                  "cross using __cross_u.dta"),                 # rc 101
    "confirm":   ([], "confirm variable beertax"),                # rc 198
    "which":     ([], "which summarize"),                         # rc 198
    "timer":     ([], "timer list"),                              # rc 198
    "mkdir":     ([], "mkdir __probe_dir"),                       # rc 198
    "rmdir":     (["mkdir __probe_dir2"], "rmdir __probe_dir2"),  # rc 198
    "erase":     (["file open __fh using __probe.txt, write replace", "file close __fh"],
                  "erase __probe.txt"),                           # rc 198
    "rm":        (["file open __fh2 using __probe2.txt, write replace", "file close __fh2"],
                  "erase __probe2.txt"),                          # rc 198 (rm is not Stata; erase is)
    "file":      ([], "file open __fh3 using __probe3.txt, write replace"),  # rc 198
    "postfile":  ([], "postfile __pf a b using __post.dta, replace"),        # rc 198
    "post":      (["postfile __pf2 a b using __post2.dta, replace"], "post __pf2 (1) (2)"),
    "postclose": (["postfile __pf3 a b using __post3.dta, replace"], "postclose __pf3"),
    "export":    ([], "export delimited using __out.csv, replace"),  # rc 198
    "import":    (["export delimited using __in.csv, replace"], "import delimited using __in.csv, clear"),
    "save":      ([], "save __probe_save.dta, replace"),          # rc 198
    "use":       (["save __probe_use.dta, replace"], "use __probe_use.dta, clear"),  # rc 198
    "sysuse":    ([], "sysuse auto, clear"),                      # rc 198 — Stata's own, fine as a PROBE
    "clear":     ([], "clear"),                                   # rc 198: takes no varlist
    "ssc":       ([], "ssc describe estout"),                     # rc 198 (network; may still fail)
    "irf":       (['keep if state == "al"', "tsset year", "var fatal beertax",
                   "irf create __irf1, step(2) set(__irfset, replace)"], "irf describe __irf1"),
    "rdrobust":  ([], "rdrobust fatal beertax, c(1)"),            # rc 125
    "rdplot":    ([], "rdplot fatal beertax, c(1)"),              # rc 100
    "heckman":   (["gen byte __sel = (fatal > 500)"],
                  "heckman beertax unemp, select(__sel = unemp income)"),   # rc 430
    "csdid":     (["egen long __id = group(state)", "gen __g = 1985"],
                  "csdid fatal, ivar(__id) time(year) gvar(__g)"),          # rc 451
    "drdid":     (["egen long __id = group(state)", "gen byte __tr = (year >= 1985)"],
                  "drdid fatal beertax, ivar(__id) time(year) treatment(__tr)"),  # rc 555
    "ivprobit":  (["gen byte __y = (fatal > 1000)"], "ivprobit __y unemp (beertax = income)"),  # rc 498
    "ivtobit":   ([], "ivtobit fatal unemp (beertax = income)"),  # rc 498
    "mediate":   (["gen byte __m = (unemp > 7)"],
                  "mediate (fatal) (__m beertax) (beertax)"),     # rc 198
    "eventstudyinteract": (["egen long __id = group(state)", "gen __ff = 1985",
                            "gen byte __rel = year - 1985"],
                           "eventstudyinteract fatal __rel, absorb(__id) cohort(__ff) control_cohort(__ff)"),
    "did_imputation": (["egen long __id = group(state)", "gen __ff2 = 1985"],
                       "did_imputation fatal __id year __ff2"),   # rc 102
    "sdid":      (["egen long __id = group(state)", "gen byte __tr2 = (year >= 1985)"],
                  "sdid fatal __id year __tr2, vce(noinference)"),  # rc 102
    # NOT REPAIRABLE ON THIS MACHINE (rc 199 = command does not exist in the
    # User's Stata install): feols, fepois, chow, mi. Their returns are
    # unmeasurable here and must be reported as such rather than as absent.
}


def guess_invocation(name, family):
    if name in SUGGESTED_INVOCATIONS:
        return SUGGESTED_INVOCATIONS[name]
    if name in BINARY_ML:
        return (HI_SETUP, f"{name} hi beertax")
    if name in ORDINAL_ML:
        return (ORD_SETUP, f"{name} __ord beertax")
    if family == "Estimation":
        return ([], f"{name} fatal beertax")
    if family == "DataMgmt":
        return ([], f"{name} beertax")
    return ([], name)


# --------------------------------------------------------------------------
# Registry parsing — the ONE C++ read this tool depends on: kRegistry in
# piet/native/src/registry.cpp. Each row has the shape
#   {"name", {"abbrev1", "abbrev2", ...}, Family::X, ...more fields...},
# across possibly several source lines, with `//` line comments interleaved.
# We strip `//...` per line (this table has none embedded in a string
# literal) and regex-match the row prefix; trailing fields (weight policy,
# knownOptions, handler tag, byIllegalInStata, communityNames) are not needed
# here and are left unparsed.
#
# ANCHOR DISCREPANCY (report this, don't silently trust it): the task brief's
# anchor says "kRegistry ... lines 121-928 (closing `};` at 929)". Reading
# that exact range showed the opening `{` at line 121 as stated, but the
# closing `};` of the vector initializer at line 926, not 929 (927 is
# `return kRegistry;`, 928 is the enclosing function's `}`). Harmless to
# parsing here (the regex only matches well-formed rows and ignores the few
# extra trailing non-row lines), but the line numbers in the brief and in the
# file disagree by a few lines — flagged rather than quietly reconciled.
# --------------------------------------------------------------------------
ROW_RE = re.compile(r'\{\s*"([A-Za-z_][A-Za-z0-9_]*)"\s*,\s*\{([^{}]*)\}\s*,\s*Family::(\w+)')
ABBREV_RE = re.compile(r'"([A-Za-z_][A-Za-z0-9_]*)"')


def parse_registry(cpp_path, start_line=121, end_line=929):
    with open(cpp_path) as fh:
        all_lines = fh.readlines()
    chunk = all_lines[start_line - 1:end_line]
    stripped = []
    for ln in chunk:
        idx = ln.find("//")
        stripped.append(ln[:idx] if idx != -1 else ln)
    blob = "".join(stripped)
    entries = []
    for m in ROW_RE.finditer(blob):
        name, abbrev_blob, family = m.group(1), m.group(2), m.group(3)
        abbrevs = ABBREV_RE.findall(abbrev_blob)
        entries.append({"name": name, "abbrevs": abbrevs, "family": family})
    return entries


Scenario = namedtuple("Scenario", "scenario canonical family setup cmd confidence")


def build_target_scenarios(entries):
    """Registry names, minus SKIP_NO_RCLASS, minus canonicals already covered
    by a FALLBACK_TABLE curated row (those are kept verbatim under their
    original scenario label instead of being re-guessed)."""
    abbrev_to_canonical = {}
    family_by_canonical = {}
    for e in entries:
        family_by_canonical[e["name"]] = e["family"]
        abbrev_to_canonical[e["name"]] = e["name"]
        for ab in e["abbrevs"]:
            abbrev_to_canonical.setdefault(ab, e["name"])

    def canonical_of(word):
        return abbrev_to_canonical.get(word, word)

    curated_by_canonical = {}
    for label, setup, cmd, kind, core, full in FALLBACK_TABLE:
        canon = canonical_of(cmd.split()[0])
        curated_by_canonical.setdefault(canon, []).append((label, setup, cmd))

    scenarios = []
    for canon, rows in curated_by_canonical.items():
        fam = family_by_canonical.get(canon, "?")
        for (scen_label, setup, cmd) in rows:
            scenarios.append(Scenario(scen_label, canon, fam, setup, cmd, "curated"))

    for e in entries:
        name = e["name"]
        if name in curated_by_canonical or name in SKIP_NO_RCLASS:
            continue
        setup, cmd = guess_invocation(name, e["family"])
        scenarios.append(Scenario(name, name, e["family"], setup, cmd, "guess"))
    return scenarios


# --------------------------------------------------------------------------
# --gen-capture: write a Stata do-file that, per scenario, runs the fixture
# reload + setup + invocation, then enumerates EVERY currently-set r()/e()
# scalar and macro name generically via Stata's `local x : r(scalars)` /
# `: r(macros)` / `: e(scalars)` / `: e(macros)` extended macro functions
# (the exact mechanism the ORIGINAL FALLBACK_TABLE's docstring says was used
# by hand at a live console — carried forward here, not independently
# re-verified since this tool cannot run Stata). Scalars are dumped via
# hex-float round-tripping (%21x) so values survive exactly; macros are
# presence-only. A plain `return list` / `ereturn list` block is ALSO emitted
# (RAWSTART/RAWEND markers) purely for human eyeballing if the extended
# macro functions above turn out not to behave as expected — --from-capture
# does not parse that block.
# --------------------------------------------------------------------------
SCENARIO_BLOCK = Template('''
display "BEGIN|$scenario|$canonical"
$fixture_load
return clear
capture ereturn clear
$setup_block
display "INVOKE|$scenario|$cmd_escaped"
capture noisily $cmd
display "CMDRC|$scenario|" _rc
foreach cls in r e {
    capture local __s : `cls'(scalars)
    if _rc == 0 {
        foreach __n of local __s {
            display "S|`cls'|$scenario|`__n'|" %21x `cls'(`__n')
        }
    }
    capture local __m : `cls'(macros)
    if _rc == 0 {
        foreach __n of local __m {
            display "M|`cls'|$scenario|`__n'"
        }
    }
}
display "RAWSTART|$scenario"
capture noisily return list
capture noisily ereturn list
display "RAWEND|$scenario"
display "END|$scenario|$canonical"
''')


def make_scenario_block(sc):
    setup_lines = []
    for s in sc.setup:
        # Quotes inside the setup text would terminate the display string early
        # (`display "SETUP|var|keep if state == "al""`), so echo the same
        # single-quoted form the INVOKE line already uses below.
        setup_lines.append('display "SETUP|%s|%s"' % (sc.scenario, s.replace('"', "'")))
        # BUG-372: guarded, and the rc is REPORTED rather than swallowed. In batch
        # mode an unguarded setup failure aborts the entire do-file — that is how
        # the first capture run died at scenario 1 of 155. A capture package is a
        # User round trip, so one bad line must cost that scenario, not the run.
        setup_lines.append(f'capture noisily {s}')
        setup_lines.append(f'display "SETUPRC|{sc.scenario}|" _rc')
    return SCENARIO_BLOCK.substitute(
        scenario=sc.scenario, canonical=sc.canonical, fixture_load=_fixture_load(True),
        setup_block="\n".join(setup_lines), cmd=sc.cmd, cmd_escaped=sc.cmd.replace('"', "'"),
    )


def cmd_gen_capture(a):
    global FIXTURE_DTA
    FIXTURE_DTA = getattr(a, "fixture_dta", FIXTURE_DTA)
    if not os.path.exists(FIXTURE_DTA):
        print("ERROR: fixture .dta not found: %s\n"
              "       Generate it first in Stata from piet/packaging/sample/fatalities.csv,\n"
              "       or pass --fixture-dta PATH." % FIXTURE_DTA, file=sys.stderr)
        return 1
    entries = parse_registry(a.registry, a.registry_start, a.registry_end)
    if not entries:
        print(f"ERROR: parsed zero registry rows from {a.registry} "
              f"(lines {a.registry_start}-{a.registry_end}) — check the anchor range.",
              file=sys.stderr)
        return 1
    scenarios = build_target_scenarios(entries)
    os.makedirs(a.outdir, exist_ok=True)

    do_path = os.path.join(a.outdir, "capture.do")
    with open(do_path, "w") as fh:
        fh.write("* Auto-generated by tools/rclass_sweep.py --gen-capture. Regenerate, don't hand-edit.\n")
        fh.write("* Run under REAL StataBE 19.5. The dataset is OUR fatalities .dta (Stata's\n")
        fh.write("* `sysuse` only resolves StataCorp's own bundled datasets, which is what the\n")
        fh.write("* first capture attempt died on):\n")
        fh.write("*   /path/to/StataBE -b do capture.do\n")
        fh.write("* Hand the resulting capture.log to: rclass_sweep.py --from-capture capture.log -o table.json\n")
        fh.write("set more off\n")
        for sc in scenarios:
            fh.write(make_scenario_block(sc))

    manifest = [
        {"scenario": sc.scenario, "command": sc.canonical, "family": sc.family,
         "setup": sc.setup, "cmd": sc.cmd, "invocation_confidence": sc.confidence}
        for sc in scenarios
    ]
    manifest_path = os.path.join(a.outdir, "capture_manifest.json")
    with open(manifest_path, "w") as fh:
        json.dump(manifest, fh, indent=2)

    n_curated = sum(1 for sc in scenarios if sc.confidence == "curated")
    n_guess = sum(1 for sc in scenarios if sc.confidence == "guess")
    print(f"Registry rows parsed: {len(entries)} (from {a.registry}, "
          f"lines {a.registry_start}-{a.registry_end})")
    print(f"SKIP_NO_RCLASS: {len(SKIP_NO_RCLASS)} rows excluded (see source for reasons)")
    print(f"Scenarios generated: {len(scenarios)} ({n_curated} curated verbatim, {n_guess} guessed)")
    print(f"Wrote {do_path}")
    print(f"Wrote {manifest_path}")
    return 0


# --------------------------------------------------------------------------
# --from-capture: parse the Stata batch log produced by capture.do into a
# table JSON keyed by scenario. Tolerant of Stata's own command-echo lines
# (". display ...") by matching only true line-start prefixes — the echoed
# source line is prefixed by ". ", the actual printed output is not.
# --------------------------------------------------------------------------
RE_BEGIN = re.compile(r'^BEGIN\|([^|]+)\|([^|]+)$')
RE_SETUPRC = re.compile(r"^SETUPRC\|([^|]+)\|\s*(-?\d+)\s*$")
RE_SETUP = re.compile(r'^SETUP\|([^|]+)\|(.*)$')
RE_CMDRC = re.compile(r'^CMDRC\|([^|]+)\|(-?\d+)$')
RE_S = re.compile(r'^S\|(r|e)\|([^|]+)\|([^|]+)\|(.+)$')   # scalar: cls, scenario, name, hexvalue
RE_M = re.compile(r'^M\|(r|e)\|([^|]+)\|(.+)$')            # macro: cls, scenario, name
RE_END = re.compile(r'^END\|([^|]+)\|([^|]+)$')


def parse_capture_log(path):
    table = {}
    cur_name = None
    with open(path) as fh:
        for raw in fh:
            line = raw.strip()
            m = RE_BEGIN.match(line)
            if m:
                cur_name, canonical = m.group(1), m.group(2)
                table[cur_name] = {
                    "command": canonical, "setup": [], "cmd": None, "cmd_rc": None,
                    "r_scalars": {}, "r_macros": [], "e_scalars": {}, "e_macros": [],
                }
                continue
            if cur_name is None or cur_name not in table:
                continue
            row = table[cur_name]
            # BUG-372: a setup that FAILED makes every r()/e() value below it
            # meaningless, so the rc is carried rather than dropped — a scenario
            # with setup_rc != 0 is "not asked", not "answered nothing".
            m = RE_SETUPRC.match(line)
            if m and m.group(1) == cur_name:
                rc = int(m.group(2))
                row.setdefault("setup_rc", []).append(rc)
                if rc != 0:
                    row["setup_failed"] = True
                continue
            m = RE_SETUP.match(line)
            if m and m.group(1) == cur_name:
                row["setup"].append(m.group(2))
                continue
            if line.startswith(f"INVOKE|{cur_name}|"):
                row["cmd"] = line[len(f"INVOKE|{cur_name}|"):]
                continue
            m = RE_CMDRC.match(line)
            if m and m.group(1) == cur_name:
                row["cmd_rc"] = int(m.group(2))
                continue
            m = RE_S.match(line)
            if m and m.group(2) == cur_name:
                row[f"{m.group(1)}_scalars"][m.group(3)] = m.group(4)
                continue
            m = RE_M.match(line)
            if m and m.group(2) == cur_name:
                row[f"{m.group(1)}_macros"].append(m.group(3))
                continue
            m = RE_END.match(line)
            if m and m.group(1) == cur_name:
                cur_name = None
                continue
    return table


def cmd_from_capture(a):
    table = parse_capture_log(a.from_capture)
    n_err = sum(1 for row in table.values() if row.get("cmd_rc") not in (0, None))
    n_novalues = sum(1 for row in table.values()
                      if not row["r_scalars"] and not row["r_macros"]
                      and not row["e_scalars"] and not row["e_macros"])
    n_setupfail = sum(1 for row in table.values() if row.get("setup_failed"))
    out = {"_meta": {"source_log": a.from_capture, "scenarios": len(table)}, "table": table}
    with open(a.out, "w") as fh:
        json.dump(out, fh, indent=2)
    print(f"Parsed {len(table)} scenarios from {a.from_capture}")
    print(f"  {n_err} scenario(s) had a non-zero cmd_rc in Stata itself (invocation needs repair)")
    print(f"  {n_novalues} scenario(s) captured zero scalar/macro names (genuinely none, or cmd_rc != 0)")
    print(f"  {n_setupfail} scenario(s) had a FAILED setup — their returns are not evidence")
    print(f"Wrote {a.out}")
    return 0


# --------------------------------------------------------------------------
# Shared: run one do-file against PIET, return the last JSON block emitted.
# --------------------------------------------------------------------------
def run_do(binary, lines, workdir, fname="sweep.do"):
    path = os.path.join(workdir, fname)
    with open(path, "w") as fh:
        fh.write("\n".join(lines) + "\n")
    env = dict(os.environ)
    engine_arm.pin_settings(env, workdir)  # never the user's live settings.json
    p = subprocess.run([binary, "--do", path], capture_output=True, text=True, env=env)
    tail = [l for l in p.stdout.strip().splitlines() if l.startswith("{")]
    return json.loads(tail[-1]) if tail else {}


# --------------------------------------------------------------------------
# Legacy sweep (no --table): the ORIGINAL script's main() body, unchanged, so
# the tool never loses today's capability when no table JSON is supplied.
# --------------------------------------------------------------------------
def legacy_sweep(a):
    os.makedirs(a.workdir, exist_ok=True)
    print(f"{'command':20s} {'kind':5s} {'core':>5s} {'set':>4s}  missing")
    print("-" * 78)
    missing_total = 0
    for name, setup, cmd, kind, core, full in FALLBACK_TABLE:
        lines = [_fixture_load(False)] + setup + [cmd]
        for nm in core:
            lines.append(f'capture noisily display {kind}({nm})')
            lines.append(f'display "PROBE {nm} rc=" _rc')
        d = run_do(a.piet, lines, a.workdir)
        texts = [b.get("text", "") for b in d.get("blocks", []) if b.get("type") == "display"]
        missing = []
        for nm in core:
            hit = [t for t in texts if t.startswith(f"PROBE {nm} rc=")]
            if not hit or not hit[0].endswith("rc=0"):
                missing.append(nm)
        missing_total += len(missing)
        print(f"{name:20s} {kind:5s} {len(core):5d} {len(core)-len(missing):4d}  "
              f"{', '.join(missing) if missing else '-'}")
    print("-" * 78)
    print(f"CORE names unset: {missing_total}")
    print("\nTable provenance: StataBE 19.5, 2026-08-18, `: r(scalars)` / `: e(scalars)`.")
    print("Rows are the CORE subset; Stata's full list is recorded in the source beside each row.")
    if a.strict and missing_total:
        return 1
    return 0


# --------------------------------------------------------------------------
# Value-aware sweep (--table TABLEJSON): presence AND value checks.
# --------------------------------------------------------------------------
STATA_MISSING_MIN = 8.988465674311579e+307  # smallest-magnitude Stata system missing (`.`)


def decode_stata_hex(s):
    """Stata %21x hex-float text -> Python float, via Python's own
    float.fromhex (Stata's `X`+exponent notation converts 1:1 to C99 hex-
    float `p`+exponent notation once the leading sign/`0x` are patched)."""
    s = s.strip()
    up = s.upper()
    if "X" not in up:
        return None
    idx = up.index("X")
    mantissa, exp = s[:idx], s[idx + 1:]
    sign = "-" if mantissa[:1] == "-" else ""
    if mantissa[:1] in "+-":
        mantissa = mantissa[1:]
    try:
        return float.fromhex(f"{sign}0x{mantissa}p{exp}")
    except ValueError:
        return None


def is_stata_missing(x):
    return isinstance(x, (int, float)) and abs(x) >= STATA_MISSING_MIN


def within_rtol(a, b, rtol):
    if a == b:
        return True
    denom = max(abs(a), abs(b), 1e-300)
    return abs(a - b) <= rtol * denom


def load_table_json(path):
    with open(path) as fh:
        data = json.load(fh)
    return data.get("table", data)


def build_probe_lines(scenario, row):
    lines = [_fixture_load(False)] + row.get("setup", [])
    cmd = row.get("cmd")
    lines.append(f"capture noisily {cmd}")
    lines.append('display "CMDRC|" _rc')
    for kind, scalars, macros in (("r", row.get("r_scalars", {}), row.get("r_macros", [])),
                                   ("e", row.get("e_scalars", {}), row.get("e_macros", []))):
        for nm in scalars:
            lines.append(f'capture noisily display "VAL|{kind}|{nm}|" {kind}({nm})')
            lines.append(f'display "RC|{kind}|{nm}|" _rc')
        for nm in macros:
            lines.append(f'capture noisily display {kind}({nm})')
            lines.append(f'display "RC|{kind}|{nm}|" _rc')
    return lines


def classify_scenario(scenario, row, texts, rtol):
    def find(prefix):
        hits = [t[len(prefix):] for t in texts if t.startswith(prefix)]
        return hits[-1] if hits else None

    cmdrc_text = find("CMDRC|")
    cmdrc = int(cmdrc_text) if cmdrc_text is not None else None
    command = row.get("command", scenario)
    skip_reason = SKIP_VALUE.get(command)

    results = []
    for kind, scalars, macros in (("r", row.get("r_scalars", {}), row.get("r_macros", [])),
                                   ("e", row.get("e_scalars", {}), row.get("e_macros", []))):
        for nm, stata_hex in scalars.items():
            rc_text = find(f"RC|{kind}|{nm}|")
            piet_rc = int(rc_text) if rc_text is not None else None
            entry = {"accessor": kind, "name": nm, "piet_rc": piet_rc,
                      "stata_value_hex": stata_hex}
            if cmdrc not in (0, None) and cmdrc != 0:
                entry["status"] = "piet_errored"
            elif piet_rc is None or piet_rc != 0:
                entry["status"] = "missing"
            else:
                stata_val = decode_stata_hex(stata_hex)
                val_text = find(f"VAL|{kind}|{nm}|")
                if skip_reason:
                    entry["status"] = "ok"
                    entry["note"] = f"value skipped (SKIP_VALUE: {skip_reason})"
                elif stata_val is not None and is_stata_missing(stata_val):
                    entry["status"] = "ok"
                    entry["note"] = "value skipped (stata value is a system missing sentinel)"
                else:
                    try:
                        piet_val = float(val_text) if val_text is not None else None
                    except ValueError:
                        piet_val = None
                    if piet_val is None or stata_val is None:
                        entry["status"] = "value_mismatch"
                        entry["note"] = "unparseable value"
                        entry["piet_value"] = val_text
                    elif within_rtol(piet_val, stata_val, rtol):
                        entry["status"] = "ok"
                        entry["piet_value"] = piet_val
                        entry["stata_value"] = stata_val
                    else:
                        entry["status"] = "value_mismatch"
                        entry["piet_value"] = piet_val
                        entry["stata_value"] = stata_val
            results.append(entry)
        for nm in macros:
            rc_text = find(f"RC|{kind}|{nm}|")
            piet_rc = int(rc_text) if rc_text is not None else None
            status = ("piet_errored" if (cmdrc not in (0, None) and cmdrc != 0)
                      else "ok" if piet_rc == 0 else "missing")
            results.append({"accessor": kind, "name": nm, "piet_rc": piet_rc,
                             "status": status, "note": "macro (presence-only)"})
    return cmdrc, results


def table_sweep(a):
    table = load_table_json(a.table)
    os.makedirs(a.workdir, exist_ok=True)
    print(f"{'scenario':22s} {'cmdrc':>5s} {'ok':>4s} {'missing':>7s} {'mismatch':>8s} {'err':>4s}")
    print("-" * 78)
    all_rows = []
    any_bad = False
    for scenario, row in sorted(table.items()):
        if not row.get("cmd"):
            print(f"{scenario:22s}  (no captured invocation — skipped)")
            continue
        lines = build_probe_lines(scenario, row)
        d = run_do(a.piet, lines, a.workdir, fname=f"sweep_{scenario}.do")
        texts = [b.get("text", "") for b in d.get("blocks", []) if b.get("type") == "display"]
        cmdrc, results = classify_scenario(scenario, row, texts, a.rtol)
        n_ok = sum(1 for r in results if r["status"] == "ok")
        n_missing = sum(1 for r in results if r["status"] == "missing")
        n_mismatch = sum(1 for r in results if r["status"] == "value_mismatch")
        n_err = sum(1 for r in results if r["status"] == "piet_errored")
        if n_missing or n_mismatch or n_err:
            any_bad = True
        print(f"{scenario:22s} {str(cmdrc):>5s} {n_ok:4d} {n_missing:7d} {n_mismatch:8d} {n_err:4d}")
        for r in results:
            if r["status"] != "ok":
                detail = f"    {r['accessor']}({r['name']}): {r['status']}"
                if r["status"] == "value_mismatch":
                    detail += f"  piet={r.get('piet_value')} stata={r.get('stata_value')}"
                print(detail)
        all_rows.append({"scenario": scenario, "command": row.get("command"), "cmdrc": cmdrc,
                          "results": results})
    print("-" * 78)
    print(f"SKIP_VALUE commands (presence-only): {', '.join(sorted(SKIP_VALUE)) or '-'}")
    if a.json:
        with open(a.json, "w") as fh:
            json.dump({"rtol": a.rtol, "scenarios": all_rows}, fh, indent=2)
        print(f"Wrote {a.json}")
    if a.strict and any_bad:
        return 1
    return 0


def default_registry_path():
    here = os.path.dirname(os.path.abspath(__file__))
    return os.path.normpath(os.path.join(here, "..", "piet", "native", "src", "registry.cpp"))


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                  formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--fixture-dta", default=FIXTURE_DTA, dest="fixture_dta",
                    help="the .dta the capture do-file loads (default: our own "
                         "tests/validate/fatalities_118.dta)")
    ap.add_argument("--gen-capture", metavar="OUTDIR", dest="gen_capture",
                     help="write a Stata capture package (do-file + manifest) to OUTDIR")
    ap.add_argument("--registry", default=default_registry_path(),
                     help="path to registry.cpp (--gen-capture only)")
    ap.add_argument("--registry-start", type=int, default=121, dest="registry_start")
    ap.add_argument("--registry-end", type=int, default=929, dest="registry_end")

    ap.add_argument("--from-capture", metavar="LOGPATH", dest="from_capture",
                     help="parse a Stata capture.log into a table JSON")
    ap.add_argument("-o", "--out", metavar="TABLEJSON", help="output path for --from-capture")

    ap.add_argument("--table", metavar="TABLEJSON",
                     help="table JSON from --from-capture (sweep mode); omit for FALLBACK_TABLE")
    ap.add_argument("--piet", default="piet/native/build/piet_native")
    ap.add_argument("--workdir", default="/tmp/rclass_sweep")
    ap.add_argument("--rtol", type=float, default=1e-9, help="relative tolerance for value checks")
    ap.add_argument("--strict", action="store_true",
                     help="exit 1 on any missing/value_mismatch/piet_errored (default: report only)")
    ap.add_argument("--json", metavar="OUT", help="machine-readable sweep report")
    a = ap.parse_args()

    if a.gen_capture:
        a.outdir = a.gen_capture
        return cmd_gen_capture(a)
    if a.from_capture:
        if not a.out:
            ap.error("--from-capture requires -o/--out")
        return cmd_from_capture(a)
    if a.table:
        return table_sweep(a)
    return legacy_sweep(a)


if __name__ == "__main__":
    sys.exit(main())
