# piet/packaging/licenses/ — third-party notices manifest (WI-5, legal remediation 2026-07-08)

Single source for the shipped `Resources/THIRD_PARTY_NOTICES.txt`.
`package_native_app.sh` generates that file at package time by concatenating,
in lexical order, every `NN-*.txt` file in THIS directory (top level only),
and then PREFLIGHT-ASSERTS the generated file exists non-empty in the bundle
(the build fails if it is absent). The in-app "About / Legal" panel reads the
same file back through the `legal.notices` bridge route (dev builds fall back
to concatenating this directory directly).

## Active (ships in every macOS bundle today)

| File | Component | License | Obligation |
|---|---|---|---|
| `00-ABOUT-AND-DISCLAIMER.txt` | — | — | StataCorp non-affiliation / trademark disclaimer (WI-6) |
| `10-webview-MIT.txt` | webview/webview (vendored `piet/native/vendor/webview/`) | MIT | notice must travel with the binary |
| `20-nlohmann-json-MIT.txt` | nlohmann/json (vendored `piet/native/vendor/nlohmann/`) | MIT | notice must travel with the binary |
| `30-DATA-SOURCES.txt` | "Fatalities" sample dataset (`piet/packaging/sample/fatalities.csv` → `Resources/sample/`) | public-domain facts (US gov statistics) | provenance note + Ruhm (1996) citation (WI-4) |
| `50-LAPACK-BSD-3-Clause.txt` | Netlib LAPACK 3.12.0 (DSYEVR call tree translated to `piet/native/src/lapack_dsyevr.*`; register row in `40-ADOPTED-CODE.txt`) | BSD-3-Clause | copyright notice + conditions must travel with the binary |
| `60-R-nmath-pnorm-GPL-2.0-or-later.txt` | R Mathlib `pnorm.c` (Cody normal CDF; `stata_normal()` in `src/core/distributions.h`; register row in `40-ADOPTED-CODE.txt`) | GPL-2.0-or-later (distributed here under GPL-3.0-or-later) | upstream copyright + licence notice must travel with the binary |
| `61-MT19937-64-BSD-3-Clause.txt` | Nishimura & Matsumoto `mt19937-64.c` 2004/9/29 (`src/core/rng_mt64.h`; register row) | BSD-3-Clause | copyright notice + conditions must travel with the binary |
| `62-lassopack-GPL-3.0.txt` | lassopack 1.3 (`rlasso.ado` / `lassoutils.ado`; `piet/native/src/handlers_e3_lasso.cpp`; register row) | GPL-3.0 (stated in the SSC listing; no LICENSE file upstream) | attribution + licence statement must travel with the binary |
| `63-swindex-GPL-3.0.txt` | swindex (SSC s458912, SJ 20-4; `piet/native/src/handlers_e3_swindex.cpp`; register row) | GPL-3.0 (stated in the SSC listing; no LICENSE file upstream) | attribution + licence statement must travel with the binary |
| `64-estout-MIT.txt` | estout / esttab / estadd / eststo 3.33, Ben Jann (`handlers_aer_tables.cpp`, `eststo_esttab.cpp`; register row) | MIT (+ GPL v3 on SSC) | verbatim MIT text must travel with the binary |
| `65-coefplot-MIT.txt` | coefplot 1.8.8, Ben Jann (`handlers_coefplot.cpp`; register row) | MIT (+ GPL v3 on SSC) | verbatim MIT text must travel with the binary |
| `66-texsave-MIT.txt` | texsave 1.6.1, Julian Reif (`handlers_aer_tables.cpp`; register row) | MIT (+ GPL v3 on SSC) | verbatim MIT text must travel with the binary |
| `67-outreg2-GPL-3.0.txt` | outreg2 2.3.2, Roy Wada (`handlers_aer_tables.cpp`; register row) | GPL-3.0 (stated in the SSC listing) | attribution + licence statement must travel with the binary |
| `68-mat2txt-GPL-3.0.txt` | mat2txt 1.1.2, Ben Jann & Michael Blasnik (`handlers_aer_tables.cpp`; register row) | GPL-3.0 (stated in the SSC listing) | attribution + licence statement must travel with the binary |
| `69-acreg-GPL-3.0.txt` | acreg 1.1.0, Colella/Lalive/Sakalli/Thoenig (`handlers_e3_acreg.cpp`; register row) | GPL-3.0 (stated in the SSC listing; ado marked "Beta Version, please do not circulate") | attribution + licence statement must travel with the binary |
| `70-qreg2-GPL-3.0.txt` | qreg2 4.00, Machado/Parente/Santos Silva (`handlers_e3_qreg2.cpp`; register row) | GPL-3.0 (stated in the SSC listing) | attribution + licence statement must travel with the binary |
| `71-ritest-GPL-3.0.txt` | ritest 1.1.7, Simon Hess (`handlers_e3_ritest.cpp`; register row) | GPL-3.0 (stated in the SSC listing) | attribution + licence statement must travel with the binary |

License texts are verbatim copies of the vendored upstream LICENSE files —
if a vendor bump changes the upstream text, re-copy it here in the same commit.

## Staged (NOT concatenated — activates with static Linux/Windows builds)

`staged/` holds placeholders for components the future static Linux/Windows
ports will link (per the porting recipes / WI-12): OpenBLAS (BSD-3-Clause),
GCC runtime libraries (GPL-3.0 WITH GCC-exception-3.1), MinGW-w64
winpthreads. **At the first static Linux/Windows build**: vendor the verbatim
license text from the actual toolchain/component used into a numbered
top-level `NN-*.txt` file so it enters the generated notices; see each
placeholder for the activation condition and canonical source. The
GCC-exception invariant (unmodified GCC/MinGW toolchain, no proprietary
codegen plugins) is recorded in WI-12a and must hold for those builds.

Renaming/adding a top-level `NN-*.txt` file changes the shipped notices —
treat it like any other release-gated packaging change.
