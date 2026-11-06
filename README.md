# PIET + CoBrA — a free, offline, Stata-syntax-compatible econometrics tool

**PIET** is a desktop application for learning and doing econometrics with the
command syntax many researchers already know. It reads `.do` scripts and `.dta`
datasets, runs them through its own independent computation engine, and renders
tables, regression output, and graphs — fully offline, no license server, no
telemetry.

**CoBrA** (C-Oriented Bootstrap & Regression Accelerator) is the engine: a
dependency-light C++17 library and CLI for high-dimensional fixed-effects
econometrics — OLS/IV/2SLS/LIML, FE-Poisson (PPML), panel models, tobit,
Heckman, quantile regression, Callaway–Sant'Anna staggered DiD, HAC/Newey–West,
robust/cluster/wild-bootstrap inference, and a Stata-style data-preparation
layer (`generate`/`egen`/`merge`/`joinby`/`reshape`/`collapse` …). Heavy linear
algebra goes through the platform BLAS (Apple **Accelerate** on macOS — the AMX
path on Apple Silicon); the engine never reimplements BLAS.

PIET is aimed at students, instructors, and researchers who want familiar
syntax without a paid license. It is **not** a copy, port, or clone of Stata,
and contains no Stata source code, manual text, or other StataCorp-authored
material (see *Relationship to Stata* below).

## Download for macOS

A ready-built app for Apple Silicon Macs is attached to each release on the
[releases page](https://github.com/w4s7jksf68-ai/piet/releases): a `.dmg`
containing `PIET.app` (the interface, the CoBrA engine, the sample dataset,
the licence and the third-party notices), with its SHA-256 beside it. Drag
`PIET.app` to Applications. Requirements: macOS 13.3 or newer, Apple Silicon
(arm64); Intel Macs and Windows/Linux are not built yet (the Windows port is
in progress from the `docker/` recipes).

**About the security warning on first launch.** The app is signed only with
an ad-hoc signature and is **not notarized**: notarization needs an Apple
Developer ID, which costs money the project does not have (see the
developer's note below). macOS will therefore say the app "cannot be opened
because Apple cannot check it for malicious software" or that it is from an
unidentified developer. This is Gatekeeper reporting the missing
notarization, not a finding about the app. To open it anyway, once:

1. In Finder, right-click (or Control-click) `PIET.app` and choose **Open**,
   then click **Open** in the dialog; or
2. try to open it normally, then go to **System Settings → Privacy &
   Security**, scroll to the message about PIET and click **Open Anyway**; or
3. from a terminal, clear the quarantine flag:
   `xattr -d com.apple.quarantine /Applications/PIET.app`.

If you would rather not trust a download, the whole app can be built from
this source tree in about ten minutes (`sh scripts/build-macos.sh`, then
`sh scripts/package-macos-dmg.sh` makes the same `.dmg`), and every release
page states the exact source commit it was built from. The corresponding
source for every binary is this repository at the release's tag.

## Developer's note

Hi, I'm Mun Kyung! I started this because I wanted anyone, regardless of
their financial circumstances, to be able to study econometrics with the same
tools and methods that are standard in academia, and I spent the past four or
five months building it. I expect there are still quite a few bugs, but I have
put a great deal of effort into making it functional enough for everyday use;
it is free software, so I hope some of you will help improve it. I would have
liked to polish it more, but I have run out of money to continue, so this is
where it stands for now. It is not yet something I would call ready for
serious academic research, but it should handle coursework and reports at the
undergraduate and graduate level, and there are no artificial limits on the
number of variables or CPU cores. If it makes studying econometrics even a
little easier for someone, that makes the whole endeavour worthwhile.

The full note, in English and Korean, with a way to donate if you would like
to, is in [DEVELOPER_NOTE.md](DEVELOPER_NOTE.md).

## What works today

- **160+ commands** across the data-management, estimation, post-estimation,
  graphics, and programming surfaces (loops, locals/globals, programs,
  `eclass`/`sclass` returns, matrices, frames, `preserve`/`restore`).
- **Resampling inference**: `bootstrap`, `jackknife`, `permute`, wild cluster
  bootstrap; deterministic seeded streams.
- **Parallel execution**: `parallel, workers(N)` fan-out with byte-identical
  output across worker counts.
- **File formats**: native `.dta` reader (legacy formats through 118, strL
  included) and 118 writer, CSV/XLSX import-export, and `.hbin`, a
  memory-mapped columnar format for fast repeated loads.
- **Graphs**: twoway families, event-study/coefficient plots, histograms,
  binscatter, `graph export` to PNG/SVG.

## Accuracy

Numbers are the product. Every estimator is validated in three rings before it
ships:

1. **Synthetic exact-answer unit tests** (ctest).
2. **Cross-engine golden fixtures** captured from real Stata runs and pinned
   byte-for-byte; the release gate re-runs the full fixture corpus on every
   stamp.
3. **Cell-level parity** against a live Stata oracle on realistic workloads,
   held to a documented tolerance ladder (baseline relative tolerance 1e-9,
   i.e. the displayed digits plus two guard digits; see
   `tests/eval/TOLERANCE.md`).

Where PIET cannot honestly reproduce a quantity, it refuses with an error
rather than returning a plausible number — "honest reject over silent wrong
answer" is the project's first law, and known numerical limitations are named
in the release notes rather than hidden.

## A published replication package, end to end

The strongest single test of a Stata-syntax tool is somebody else's real
project. PIET runs the complete AEA replication package of

> Abou Daher, Chaza, Erica Field, Kendal Swanson, and Kate Vyborny. "Drivers of
> Change: Employment Responses to the Lifting of the Saudi Female Driving Ban."
> *American Economic Review* (replication package openICPSR-226844, Stata 18 code).

from its `Master.do` to the last appendix table, unmodified apart from a
two-line path patch (`mainfolder`). The package cleans four data sources
(a three-wave RCT survey merged with administrative records, plus Global
Findex, the Saudi Education and Training Survey and the Saudi Labor Force
Survey) and produces 30 tables and 5 figures; along the way it exercises
`import excel`, string cleaning in Arabic and English, `merge`/`reshape`,
`reghdfe`, `leebounds` under `bootstrap`, `swindex`, `esttab`/`estout` and
`graph export`.

| | |
|---|---|
| Command blocks executed | about 40,000 (`Master.do` plus every nested do-file) |
| Result files written | 114 of 114 (tables, figures, cleaned datasets) |
| Error blocks | 0 (the three errors the package itself swallows with `capture` are its own `which <package>` probes) |
| Wall time, PIET A.0.1 / CoBrA 1.0.1 | 7.2 minutes on an Apple M1 (8 GB RAM, macOS 26.6), single process |

**Against the reference run.** The same package was run in Stata/BE 19.5 on
the same machine and compared cell by cell (`.tex` tables) and value by value
(cleaned `.dta` files, including variable and value labels):

- 101 of 102 tables are cell-identical once the package's two `sort` +
  `collapse (lastnm)` lines are given `, stable`; against the package as
  shipped it is 99 of 102. The remainder is Stata's own randomized tie-break in
  `sort` on duplicate keys, which also changes between two Stata runs with a
  different `sortseed`, so no deterministic program can match it.
- 14 of 14 cleaned datasets are identical in values and labels; one variable
  differs by one unit in the last place inside `swindex`'s inverse-covariance
  weighting, and no table is affected.
- The reference run of the same package in Stata/BE took about 1.5 minutes on
  this machine, so on this workload PIET is several times slower, not faster.
  The package is dominated by data cleaning over survey exports rather than by
  estimation; it has not been profiled yet, and no speed claim is made from it.

The replication data belong to the authors and the AEA Data Editor and are
not redistributed here; the package is public on openICPSR for anyone who
wants to repeat the run. The five other packages in the same test batch are
work in progress and are not claimed.

## Speed

What is known today, measured rather than promised (PIET A.0.1 / CoBrA 1.0.1,
Apple M1, 8 GB RAM, macOS 26.6, release build):

| Workload | Wall time |
|---|---|
| `demos/fatalities/fatalities_tour.do` (120 blocks, 28 estimations incl. two-way FE, Callaway–Sant'Anna, SVG graphs) | under 1 s |
| `demos/fatalities/pangram_tour.do` (627 blocks across 10 sections: 73 estimations, 10 graphs, merge/joinby/append/reshape round-trips, limited-dependent-variable models) | about 1 s |
| The replication package above (about 40,000 blocks, 114 output files) | 432 s |

Where the time goes by design: the engine is C++17 with the platform BLAS
(Apple Accelerate), datasets are memory-mapped in a columnar format (`.hbin`)
so repeated loads cost nothing, one resident engine process serves a session,
and `parallel, workers(N)` fans a do-file out with byte-identical output
across worker counts. What is not here: a published head-to-head benchmark
against Stata (the project does not publish comparative-performance claims
about Stata), and we have not benchmarked against the R or Python
fixed-effects packages ourselves. If you measure PIET against something,
please share the do-file and the machine in an issue.

## Demo data and demo do-files

Everything needed to try PIET ships in the tree, and it is also what the
project's own release gate runs ("the demo law": every block of every demo
must run through the shipped app with zero error blocks, `demos/README.md`).

**The data: `demos/fatalities/fatalities.csv`** (also bundled in the app as
the `sysuse fatalities` sample). The US traffic-fatalities panel: the 48
contiguous states observed annually 1982–1988, 336 rows. It was assembled for
Ruhm (1996), "Alcohol policies and highway vehicle fatalities" (*Journal of
Health Economics* 15, 435–454), is the teaching panel of Stock & Watson's
*Introduction to Econometrics*, and this copy comes from the R package `AER`
via the Rdatasets collection; the contents are facts compiled from US
government statistics (NHTSA FARS, Census, BEA). Key variables: `fatal`
(traffic deaths), `pop`, `beertax` (real tax per case of beer), `unemp`,
`income`, `spirits`, `breath` (breath-test law), `jail`, `miles`,
`youngdrivers`, `drinkage`.

**`fatalities_tour.do`** (141 lines, nine sections) is a commented tour that
uses one dataset to show the core estimators the way a panel-econometrics
course would introduce them:

1. load, inspect and construct the fatality rate;
2. pooled OLS and why its classical standard errors mislead on a panel;
3. one-way and two-way fixed effects (`reghdfe`), clustered by state;
4. the inference hierarchy on one point estimate: iid, HC1, cluster (and the
   48-cluster caveat);
5. just-identified IV assembled by hand from the reduced form and the first
   stage (indirect least squares), including a weak-instrument illustration;
6. the log-linear rate model as the count-data analogue;
7. Callaway–Sant'Anna staggered difference-in-differences (`csdid`) on the
   staggered adoption of breath-test laws;
8. graphs exported as SVG;
9. dynamics with the `L.`/`D.` time-series operators.

`사용법.md` in the same folder is a short Korean guide to the application's
screen (install, open, run, settings); `HANDBOOK.md` beside it is the teaching text (currently in Korean): for each
section the model, the estimator, the standard-error choice and why, how
credible the identifying assumptions are on this data, and how to read the
actual numbers the run prints. The tour was written against an early build
and deliberately uses only what shipped then (its header lists the
substitutions it made, e.g. IV by hand instead of `ivregress`); the current
build also runs `ivregress`, `ivreghdfe`, `poisson`, `newey` and `boottest`
directly.

**`pangram_tour.do`** (438 lines, sections S1–S10) is the "everything once"
script: session housekeeping; `use` straight on a `.csv` (PIET sniffs file
content, so no `import` step — the one line that is not Stata syntax, flagged
in the file); save/use round trips and dataset surgery (`merge`, `joinby`,
`append`, `reshape`); descriptive statistics; registered command
abbreviations proven equivalent to the full spellings; the estimation surface
(linear, limited-dependent, IV, systems, panel, counts); `csdid` on the real
policy variation (and `hausman` for FE vs RE); time-series declaration;
post-estimation (`predict`, `test`, `nlcom`, `margins`, `marginsplot`,
`esttab`); and graphics. It exists so that a
change anywhere in the engine or the do-executor shows up as a red block
before a release.

Run them from the app (File → Open do-file, then Run) or headless:

```sh
cd demos/fatalities
COBRA_BIN=$PWD/../../build/cobra ../../piet/native/build/piet_native --do fatalities_tour.do
```

The headless run prints one JSON document with every block the app would
render; the SVG graphs land in the working directory.

## Build

Requires CMake ≥ 3.20 and a C++17 compiler. Currently macOS-first (Accelerate);
Windows and Linux ports are in progress (`docker/` holds the cross-build recipes).

The one-command path, which is also what the CI workflow runs:

```sh
sh scripts/build-macos.sh      # builds both executables, runs ctest and the public smoke test
```

Step by step:

```sh
# the engine (repo root)
cmake --preset release
cmake --build --preset release -j4
ctest --preset release

# the PIET app — a standalone CMake project with its own presets
cd piet/native && cmake --preset release && cmake --build --preset release -j4
```

Without presets the equivalent is `cmake -S . -B build -DCMAKE_BUILD_TYPE=Release`
and `cmake -S piet/native -B piet/native/build -DCMAKE_BUILD_TYPE=Release`.

**Tests from a fresh clone.** `ctest` (the engine's unit tests) and
`tests/public_smoke.py` (import → regress → save → reload through the app, checked
against an independent OLS) run self-contained; `scripts/build-macos.sh` runs both.
`piet_native --selftest` is the maintainers' suite: most of its arms read private
oracle fixtures (Stata captures and datasets the repo deliberately does not ship),
so in a clone it reports those arms as failed or skipped rather than passing.

The app finds its engine on its own when both are built in place — it looks
beside itself, then at the repo-root `build/cobra`. Set `COBRA_BIN` to override
that (test harnesses do, to pin an exact engine); packaged builds bundle their
own. See `piet/README.md` for app-side details and the UI test harness.

## Layout

```
src/ apps/        CoBrA engine (io, core, estimators, vcov, inference, CLI)
piet/native/      PIET application (C++ core, WKWebView UI, dispatcher)
piet/frontend/    UI (HTML/CSS/JS)
tests/            unit tests, golden fixtures, parity harnesses
tools/            converters and audit tooling
vendor/           vendored single-header dependencies
```

## Relationship to Stata

PIET is not affiliated with, endorsed by, or sponsored by StataCorp LLC.
"Stata" is a registered trademark of StataCorp LLC. PIET is an independently
developed tool that implements a compatible command syntax and reads/writes
compatible file formats; command languages and file formats are not subject to
copyright. See `NOTICE.txt` for the full non-affiliation statement, trademark
rationale, and per-author credits for community-package command names.

## License

**GPL-3.0-or-later.** See `LICENSE.txt`. Third-party components are listed
with their licenses under `piet/packaging/licenses/`, including the
adopted-code register (`40-ADOPTED-CODE.txt`), which records the provenance of
any code adopted from GPL-compatible sources.

## Citing

If you use PIET's Callaway–Sant'Anna staggered difference-in-differences
(`csdid`) estimator in published or for-credit academic work, please cite:

> Callaway, Brantly, and Pedro H. C. Sant'Anna. 2021. "Difference-in-Differences
> with Multiple Time Periods." *Journal of Econometrics* 225(2): 200-230.
>
> Sant'Anna, Pedro H. C., and Jun Zhao. 2020. "Doubly Robust
> Difference-in-Differences Estimators." *Journal of Econometrics* 219(1):
> 101-122.

If you use PIET more generally in published work, please also cite the tool
itself:

> Kim, M.K. 2026. "PIET: a Stata-syntax-compatible econometrics tool."
> [software]. https://github.com/w4s7jksf68-ai/piet

<!-- hook live-fire 2026-09-02: docs-only commit on main -->
