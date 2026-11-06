# PIET 2.0 native core — M0 walking skeleton.
#
# Standalone CMake subproject (its own project(), NOT add_subdirectory'd from
# the repo-root CMakeLists.txt, which is cobra's own `cecon` build). Per the
# Master Plan (docs/piet/ROADMAP.md, "2.0.0 — C++ core"), piet/native/ lives
# beside piet/backend and piet/frontend and is built/tested independently:
#
#   cmake -S piet/native -B piet/native/build -DCMAKE_BUILD_TYPE=Release
#   cmake --build piet/native/build -j
#   ./piet/native/build/piet_native --selftest
#   ./piet/native/build/piet_native            # interactive demo window
#
# cobra itself is NOT built here — piet_native calls the pinned cobra binary
# as a subprocess (Art 7 provenance; see src/cobra_runner.cpp).
cmake_minimum_required(VERSION 3.20)
project(piet_native LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

add_compile_options(-Wall -Wextra -Wno-deprecated-declarations)

# M4a (ROADMAP §2.0.0 "M4"): numpy-parity floating-point — three
# translation units compute quantities that must byte-match numpy's
# IEEE-754 DOUBLE-rounded arithmetic (multiply, THEN add — two separate
# roundings). On arm64, clang's default -ffp-contract=fast fuses `a*b + c`
# into a single FMA instruction — ONE rounding — which diverges in the
# last ULP. That divergence is invisible to the cross_impl numeric-
# tolerance profile but breaks Eval's ZERO-TOLERANCE CoefficientRow anchor
# harness (ci_lo/ci_hi ARE anchor fields, Doctrine ①):
#   - graph_m4.cpp   : bin/grid-edge `xmin + step*i` (np.linspace internals)
#   - estimation_m4.cpp: CI bounds `b ± kZ975*se` in the margins BUG-006
#                        AME remap (cobraToEstimationM4) and csdid's
#                        _coef_row (coefRowM4, feeding estat simple/event)
#   - eststo_esttab.cpp: coefplot's non-default-level() CI recompute
#                        `b ± zAlpha*se` (runCoefplot)
# (Eval: ~29% of random (b,se) pairs diverge; missed pre-merge only by
# thin corpus/debug_all coverage of these exact rows. Post-fix: 24/24
# anchor GREEN + coefplot level90 adversarial GREEN, zero regression.)
# Scoped to just these numpy-parity files — NOT applied globally, since no
# other TU in this build has been shown to need it.
set_source_files_properties(
  src/graph_m4.cpp
  src/estimation_m4.cpp
  src/estimation_m4b.cpp
  src/estimation_m4c.cpp
  src/eststo_esttab.cpp
  src/twoway_opts.cpp
  # Mata invsym(): StataBE's invsym() rounds every product and difference
  # separately; its %21x bits match unfused arithmetic and not FMA.
  src/mata_subset.cpp
  # ml (moptimize lf): StataBE's gradient, Hessian and step arithmetic match
  # separately rounded products and sums, never FMA (scratch-r150/ml_tail).
  src/handlers_ml.cpp
  # `matrix` expressions (handlers_m3.cpp): StataBE's products and inv() round
  # every product and sum separately; fused, `(0.1,0.7)*(0.2\0.6)` differs in
  # the last bit and inv() of a singular X'X misses its zero pivot.
  src/handlers_m3.cpp
  # LAPACK DSYEVR translation (Mata symeigensystem() in StataBE): the fused
  # multiply-adds of StataBE's gfortran build are written as std::fma at the
  # same sites; nothing else may be contracted.
  src/lapack_dsyevr.cpp
  PROPERTIES
  COMPILE_FLAGS "-ffp-contract=off"
)

# Monorepo root (piet/native/../..) — M2 LINKS cobra's src/io/columnar.{h,cpp}
# for hbin IO instead of reimplementing it (Master Plan: read-only compile
# reuse; piet/native never edits anything under src/).
get_filename_component(MONOREPO_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)

# R1 (selftest split, 2026-09-01): one OBJECT library holds every shared TU;
# two thin executables assemble from it. piet_native no longer links the
# 24k-line selftest.cpp — the suite ships as the sibling `piet_selftest`, over
# the SAME objects, so what is tested is byte-for-byte what is shipped.
add_library(piet_core OBJECT
  src/app_support.cpp
  src/shell.cpp
  src/bridge_internal.cpp
  src/routes.cpp
  src/cobra_runner.cpp
  # MAXPERF Stage 1b: persistent `cobra serve` worker client (§25 stateless +
  # §26 resident-frame data plane) the cobra_runner intercept routes to.
  src/cobra_session.cpp
  # ONESHOT-GETCOLS: the shared "fetch only the columns an expression names" pair
  # (contract_A §38). Shared deliberately — see piet/column_fetch.h.
  src/column_fetch.cpp
  src/bundle_paths.cpp
  src/progress.cpp
  # 2.2.0 item D: persisted app-shell settings + hardware detection (SPEC
  # II-1/II-2) — settings.get/settings.set routes + the on-disk store.
  src/settings.cpp
  # M1 evaluator base (C++ port of piet/backend/piet/router/* — see
  # include/piet/eval/*.h; differential-parity harness in tests/eval_diff/)
  src/eval/errors.cpp
  src/eval/tokenizer.cpp
  src/eval/gettoken.cpp
  src/eval/pyvalue.cpp
  src/eval/pyeval.cpp
  src/eval/exprfns.cpp
  src/eval/macros.cpp
  src/eval/evalcase.cpp
  src/eval/evalcase_ext.cpp
  # M2 glue + vertical slice (see ROADMAP.md §2.0.0 "M2")
  src/datastore.cpp
  src/session_state.cpp
  # BETA campaign Phase A4: the c()-class system-value registry (BS-091/
  # BS-101) — single source for every `c(name)` read (display, gen/replace's
  # cobra-forwarded RHS, if/while conditions, scalar/local `= expr`).
  src/cclass.cpp
  src/dispatcher.cpp
  # Shared mutationResult builder (see include/piet/mutation_result.h) —
  # consolidated out of four near-identical per-TU copies (handlers.cpp,
  # handlers_m3.cpp, handlers_data_verbs.cpp, egen.cpp).
  src/mutation_result.cpp
  src/handlers.cpp
  src/handlers_ml.cpp
  # DSYEVR(V,A,L) from Netlib LAPACK 3.12.0 for ml's `difficult` step.
  src/lapack_dsyevr.cpp
  src/run_block.cpp
  # BETA campaign Phase A0: the command registry (registry.h/.cpp) — a
  # hand-transcribed description of dispatcher.cpp's routing table, feeding
  # `--dump-registry`, tools/freeze_matrix.py, and selftest.cpp's
  # testRegistryRoutes() drift guard (see registry.h's doc comment).
  src/registry.cpp
  # M3a: control flow + expanded surface (see ROADMAP.md §2.0.0 "M3")
  src/translate.cpp
  src/loops.cpp
  src/parallel.cpp
  src/parallel_auto.cpp
  # SPEC II-4 (2026-09-02): memory pre-flight gate before data-size-scaling verbs.
  src/preflight.cpp
  src/parallel_effects.cpp
  src/parallel_paths.cpp
  src/doexec.cpp
  src/handlers_m3.cpp
  src/handlers_aer_env.cpp
  src/handlers_aer_abbrev.cpp
  src/handlers_aer_results.cpp
  src/mata_subset.cpp
  src/handlers_aer_gtools.cpp
  src/handlers_aer_tables.cpp
  # W2D-T01: `ereturn clear|post|local` (+ named refusals for the rest).
  src/handlers_ereturn.cpp
  src/handlers_aer_reshape.cpp
  src/program.cpp
  src/display_hardening.cpp
  # M3b: MI-066 data verbs via cobra 0.18.0 (see ROADMAP.md §2.0.0 "M3b")
  src/handlers_data_verbs.cpp
  src/handlers_data_verbs2.cpp
  src/handlers_data_verbs3.cpp
  # The one delimited-text reader behind import delimited / insheet
  # (include/piet/delimited_io.h; data-io-family design-step3.md section 2).
  src/delimited_io.cpp
  # P6 wave: Stata frames (frame put/drop/prefix, frlink, frget)
  src/handlers_frames.cpp
  # P6 wave: bootstrap/jackknife/permute prefixes (PIET-side replicate loop)
  src/handlers_resample.cpp
  # Arc F (P3 wave): the seven QE/RD commands' PIET half — rdrobust, rddensity,
  # rdplot, binscatter, eventstudyinteract, did_imputation, sdid. Parse + call
  # the engine + render Stata's tables; no statistics live there.
  src/handlers_qe.cpp
  src/egen.cpp
  # BUG-471 T2: the one storage-type resolver (gen/egen/replace -> --store-type).
  src/storage_type.cpp
  src/file_io.cpp
  src/postfile.cpp
  # serset-twoway SS: the graph-owned serset store + `serset` command family
  src/serset.cpp
  # BETA campaign Phase G (BS-090): shared `in a/b` + `if` row-filter
  # composition for every non-gen/replace command (materialize-idx-then-
  # keepif — see inrange_filter.h's doc comment for why the naive `_n`-fold
  # gen/replace uses doesn't generalize to a plain --keepif call).
  src/inrange_filter.cpp
  # M4a: output-completion surface (see ROADMAP.md §2.0.0 "M4")
  src/tabulate.cpp
  src/winsor2.cpp
  src/estimation_m4.cpp
  src/estimation_m4b.cpp
  src/estimation_m4c.cpp
  src/eststo_esttab.cpp
  src/graph_m4.cpp
  # graph-family AI-1/AI-2: graph bar/hbar/box/hbox (bargraph_g.class) + its serset
  src/graph_bar.cpp
  # graph-family AI-4/6/7: graph describe/copy/rename/display/draw
  src/graph_store.cpp
  # coefplot-family AN-1..AN-4: coefplot 1.8.8's data pipeline (generate(), r())
  src/handlers_coefplot.cpp
  # graph-family AI-13: graph export to png/pdf/eps (no external library)
  src/graph_export_formats.cpp
  # analysis.do acceptance campaign: parenthesised multi-layer twoway
  # (M4a-remaining) — C++ ports of twoway_parse.py (layer-aware raw-text
  # parser) + twoway_opts.py (Stata option -> SVG style-dict translation).
  src/twoway_parse.cpp
  src/twoway_opts.cpp
  # analysis.do acceptance campaign (BUG-032): row-wise string gen/replace/
  # count/keep-if/drop-if — C++ port of piet/backend/piet/strmutate.py, reusing
  # the M1 pyeval/exprfns evaluator; writes results back as factor columns via
  # a CSV round-trip through `cobra convert` (piet/native has no hbin writer).
  src/strmutate.cpp
  # linked, not copied — src/** stays read-only (Master Plan Doctrine ③)
  src/xlsx.cpp
  src/handlers_aer_iotext.cpp
  src/handlers_aer_util.cpp
  src/handlers_t7_lookfor.cpp
  src/handlers_t7_ihstrans.cpp
  src/handlers_t7_sctorezone.cpp
  src/handlers_lb_leebounds.cpp
  src/handlers_aer_graphx.cpp
  src/handlers_aer_estim.cpp
  src/handlers_aer_k1.cpp
  # AER tier-1 (2026-10-28): one TU per lane; zip_io.h exposes xlsx.cpp's ZIP code.
  src/handlers_t1_zip.cpp
  src/handlers_t1_env.cpp
  src/handlers_t1_meta.cpp
  src/handlers_t1_gen.cpp
  src/handlers_t1_matrix.cpp
  # AER scope-2 B2-3: Stata business calendars (bcal create + bofd/dofb).
  # The reader/writer/index arithmetic live apart from the handler so the
  # selftest can pin the .stbcal BYTES against StataBE's own files.
  src/bcal.cpp
  # X2-GLM: `glm` and `margins` after it run in-process. estimation_glm.cpp is the
  # PIET half (parse, design, e() posting); the estimator is the engine TU itself,
  # linked here the way columnar.cpp is, so both binaries fit with the same code.
  src/estimation_glm.cpp
  # E3 scaffold: one refusal-stub handler TU per E3 command (native tasks replace the bodies).
  src/handlers_e3_swindex.cpp
  src/handlers_e3_qreg2.cpp
  src/handlers_e3_lasso.cpp
  src/handlers_e3_suest.cpp
  src/handlers_e3_ritest.cpp
  src/handlers_e3_acreg.cpp
  ${MONOREPO_ROOT}/src/estimators/glm.cpp
  ${MONOREPO_ROOT}/src/io/columnar.cpp
)

# Usage requirements shared by the core objects and both executables. Every
# former `target_*(piet_native PRIVATE ...)` settings line below now targets
# this INTERFACE library instead, so the three targets cannot drift.
add_library(piet_settings INTERFACE)
target_link_libraries(piet_core PRIVATE piet_settings)

add_executable(piet_native src/main.cpp $<TARGET_OBJECTS:piet_core>)
add_executable(piet_selftest src/selftest.cpp src/selftest_main.cpp
  src/selftest_w2_macron.cpp
  src/selftest_w2_predictframe.cpp
  src/selftest_w2_eststoprefix.cpp
  src/selftest_w2_labelglued.cpp
  src/selftest_w2_doinclude.cpp
  src/selftest_w2_bykeep.cpp
  src/selftest_w2_estimopts.cpp
  src/selftest_w2_matrix.cpp
  src/selftest_w2_getmata.cpp
  src/selftest_r150_ivreg2.cpp
  src/selftest_r150_xtivreg2.cpp
  src/selftest_w2_loops.cpp
  src/selftest_w2_progctl.cpp
  src/selftest_w2_mergeappend.cpp
  src/selftest_w2_esttabnames.cpp
  src/selftest_w2_bs_bootstrap.cpp
  src/selftest_w2_egenrename.cpp
  src/selftest_w2c_macrofns.cpp
  src/selftest_w2c_datafns.cpp
  src/selftest_w2_xtsetpersist.cpp
  src/selftest_w2_tsset.cpp
  src/selftest_w2_esample.cpp
  src/selftest_w2b_linearfe.cpp
  src/selftest_w2b_joinreshape.cpp
  src/selftest_w2b_strlwriter.cpp
  src/selftest_w2b_delimited.cpp
  src/selftest_w2b_dataopts.cpp
  src/selftest_w2b_ivfamily.cpp
  src/selftest_w2b_descxtset.cpp
  src/selftest_w2b_predict.cpp
  src/selftest_w2b_esttabscreen.cpp
  src/selftest_w2b_egenopts.cpp
  src/selftest_w2b_glm.cpp
  src/selftest_w2b_cluster3.cpp
  src/selftest_w2b_margins.cpp
  src/selftest_w2b_misc.cpp
  src/selftest_w2b_destring.cpp
  src/selftest_w2d_inrange.cpp
  src/selftest_w2c_postest.cpp
  src/selftest_w2_ml.cpp
  src/selftest_w2e_float.cpp
  src/selftest_w2i_varsub.cpp
  src/selftest_w2x_fnstring.cpp
  src/selftest_w2z_tostring.cpp
  src/selftest_w2y_datetime.cpp
  src/selftest_w2b_iv.cpp
  src/selftest_w2d_ereturn.cpp
  src/selftest_w2_e3swindex.cpp
  src/selftest_w2_e3qreg2.cpp
  src/selftest_w2_e3lasso.cpp
  src/selftest_w2_e3suest.cpp
  src/selftest_w2_e3ritest.cpp
  src/selftest_w2_e3acreg.cpp
  src/selftest_w2_t1_zip.cpp
  src/selftest_w2_t1_env.cpp
  src/selftest_w2_t1_meta.cpp
  src/selftest_w2_t1_gen.cpp
  src/selftest_w2_t1_matrix.cpp
  src/selftest_w2_lpoly.cpp
  src/selftest_w2_ebmat.cpp
  src/selftest_w2_gfxbinscatter.cpp
  src/selftest_w2_gfxbar.cpp
  src/selftest_w2ap_egen.cpp
  src/selftest_w2aq_histogram.cpp
  src/selftest_w2_t7_lookfor.cpp
  src/selftest_w2_t7_ihstrans.cpp
  src/selftest_w2_t7_sctorezone.cpp
  src/selftest_w2_lb_leebounds.cpp
  src/selftest_w2_t7_clearall.cpp
  src/selftest_w2_uni_core.cpp
  src/selftest_w2_uni_v.cpp
  src/selftest_w2_uni_m.cpp
  src/selftest_w2_uni_d.cpp
  src/selftest_w2_uni_w.cpp
  src/selftest_w2_c1067.cpp
  src/selftest_w2_display_format.cpp
  src/selftest_w2af_format.cpp
  src/selftest_w2ah_dataverbs.cpp
  $<TARGET_OBJECTS:piet_core>)
target_link_libraries(piet_native PRIVATE piet_settings)
target_link_libraries(piet_selftest PRIVATE piet_settings)

target_include_directories(piet_settings INTERFACE
  include
  src
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${MONOREPO_ROOT}/src            # for columnar.cpp's "io/columnar.h" / "core/error.h"
  ${MONOREPO_ROOT}/vendor         # for columnar.cpp's "nlohmann/json.hpp" (repo-root vendor copy)
)

# WS-D1: the dispatch zone profiler (src/dispatch_profile.h). OFF by default —
# when off, every zone scope compiles to nothing and the binary is identical to
# one built without the header, which is what keeps the freeze fixtures and the
# answerbook pins out of its blast radius. Turn it on to reproduce the per-zone
# attribution table:
#   cmake -DPIET_ENABLE_DISPATCH_PROFILE=ON <src> && make piet_native
#   PIET_DISPATCH_PROFILE=1 ./piet_native --do <file>   # dumps to stderr at exit
option(PIET_ENABLE_DISPATCH_PROFILE "Compile the per-zone dispatch profiler" OFF)
if(PIET_ENABLE_DISPATCH_PROFILE)
  target_compile_definitions(piet_settings INTERFACE PIET_ENABLE_DISPATCH_PROFILE=1)
endif()

find_package(Threads REQUIRED)
target_link_libraries(piet_settings INTERFACE Threads::Threads)

# AER scope-2 W3a: `import delimited ..., encoding("latin1")` converts the file
# to UTF-8 before parsing, through iconv(3). glibc carries iconv in libc; macOS
# and the BSDs need the separate libiconv, which is what Iconv_LIBRARIES names
# when it is not builtin. REQUIRED rather than optional on purpose: an encoding()
# that silently did nothing would decode extended ASCII as UTF-8 garbage at rc 0.
find_package(Iconv REQUIRED)
target_link_libraries(piet_settings INTERFACE Iconv::Iconv)

# ── SINGLE VERSION SOURCE (2.0.2 / MI-138 WP-2, postmortem RC6) ──────────
# These three lines are THE one and only definition site for the piet-axis
# version tuple. Everything else DERIVES from the values compiled in here:
#   - routes.cpp's routes::version (the /version bridge route) emits the
#     macros directly;
#   - `piet_native --version` (main.cpp) prints the compiled tuple as JSON;
#   - piet/packaging/package_native_app.sh stamps Info.plist by reading
#     `--version` back OUT of the actual binary being bundled (never a
#     hand-synced copy — the 2.0.1 contract_a Info.plist drift bug class);
#   - `piet_native --selftest`'s version-single-source leg asserts
#     compiled tuple == /version route == packaged Info.plist ==
#     the private coordination tree's releases/CURRENT (per-artifact SKIP
#     where not applicable).
# Do NOT introduce another hardcoded piet version string anywhere in
# piet/native/ or piet/packaging/ — derive from the binary instead.
# (piet/pyproject.toml is EXCLUDED: frozen at 1.16.0, the deprecated Python
# backend line, deletion queued separately — see ROADMAP.)
# contract_a/contract_b mirror internal note CONTRACT-engine.md / CONTRACT.md's
# frozen version headers; bump each together with its .md when a contract
# changes (never let the compiled string drift from the doc).
#
# Held in CMake variables (rather than inline literals) so the Windows
# resource layer below can DERIVE the PE's VERSIONINFO / manifest identity
# from the very same values instead of introducing a second hand-synced copy
# — the RC strings, the manifest assemblyIdentity, and the Inno installer's
# AppVersion all trace back to PIET_VERSION_VALUE here. Editing these three
# lines is still the one and only version bump.
set(PIET_VERSION_VALUE    "A.0.1")
set(PIET_CONTRACT_A_VALUE "1.26.0")
set(PIET_CONTRACT_B_VALUE "2.10.0")
target_compile_definitions(piet_settings INTERFACE
  PIET_VERSION_STRING="${PIET_VERSION_VALUE}"
  PIET_CONTRACT_A_VERSION="${PIET_CONTRACT_A_VALUE}"
  PIET_CONTRACT_B_VERSION="${PIET_CONTRACT_B_VALUE}"
)

if(APPLE)
  # WEBVIEW_COCOA path (webview.h auto-detects __APPLE__) needs WebKit +
  # Cocoa. webview.h itself only strictly requires WebKit; Cocoa/ObjC runtime
  # come along transitively via the system frameworks on macOS toolchains.
  find_library(WEBKIT_FRAMEWORK WebKit REQUIRED)
  find_library(COCOA_FRAMEWORK Cocoa REQUIRED)
  target_link_libraries(piet_settings INTERFACE ${WEBKIT_FRAMEWORK} ${COCOA_FRAMEWORK})
  # webview.h's Cocoa backend is Objective-C++ under the hood (uses the id/SEL
  # objc runtime directly via objc_msgSend, manual retain/release — NOT
  # written for ARC). Since we #include it from a .cpp, force Objective-C++
  # compilation for the translation unit that reaches into WEBVIEW_COCOA
  # (shell.cpp is the only one that includes webview.h) WITHOUT -fobjc-arc:
  # ARC would reject the header's raw (id) bridging casts (see MIGRATION.md
  # upstream — the library manages Cocoa object lifetimes itself).
  set_source_files_properties(src/shell.cpp PROPERTIES
    COMPILE_FLAGS "-x objective-c++"
  )
elseif(UNIX)
  # WEBVIEW_GTK path — needs webkit2gtk + gtk3 via pkg-config. This is the
  # Linux target Phase D ports to; M0's docker compile-check exercises this
  # branch (see docker/Dockerfile.linux-webkitgtk-check, added for this
  # milestone).
  find_package(PkgConfig REQUIRED)
  pkg_check_modules(WEBKIT2GTK REQUIRED webkit2gtk-4.1)
  target_include_directories(piet_settings INTERFACE ${WEBKIT2GTK_INCLUDE_DIRS})
  target_link_libraries(piet_settings INTERFACE ${WEBKIT2GTK_LIBRARIES})
  target_compile_options(piet_settings INTERFACE ${WEBKIT2GTK_CFLAGS_OTHER})
elseif(WIN32)
  # MUST MATCH the engine's own define (the root CMakeLists.txt if(WIN32) block)
  # AND the mman shim's compile line (docker/Dockerfile.windows). MinGW-w64's
  # off_t is 4 bytes without this and 8 with it; piet_native compiles the engine's
  # columnar.cpp, which calls the shim's mmap(..., off_t). A mismatch is not a
  # compile error — it silently passes a half-width offset, so every .hbin mapping
  # fails at runtime ("mmap failed") on the first `use`. Found exactly that way.
  target_compile_definitions(piet_settings INTERFACE _FILE_OFFSET_BITS=64)

  # WEBVIEW_EDGE path (webview.h auto-selects it from _WIN32). Phase D / WP-W1.
  #
  # The COM interface *types* come from the vendored Microsoft WebView2 SDK
  # header (vendor/webview2/WebView2.h, BSD-3-Clause — see vendor/VENDOR.md).
  # WEBVIEW_MSWEBVIEW2_BUILTIN_IMPL=1 (webview.h's default) means the WebView2
  # loader is resolved at runtime via LoadLibrary/GetProcAddress — no import
  # library, no WebView2Loader.dll shipped.
  target_include_directories(piet_settings INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}/vendor/webview2)
  # webview.h lists its Win32 import libs via `#pragma comment(lib, ...)`, but
  # those pragmas are guarded by `#ifdef _MSC_VER` — a no-op under MinGW-w64.
  # Name them explicitly for the ld link line. uuid supplies the CLSID_/IID_
  # symbols for IFileOpenDialog (WP-W3); ole32/oleaut32 for COM; shlwapi/
  # shell32/shobjidl surfaces; user32/gdi32 window; version/advapi32/dwmapi
  # per webview.h's own pragma list (dwmapi is dlopen'd at runtime, harmless
  # to also import). Matches docker/Dockerfile.piet-windows's link flags so a
  # plain cross-cmake links without external -l flags.
  target_link_libraries(piet_settings INTERFACE
    ole32 oleaut32 shlwapi shell32 user32 gdi32 version advapi32 uuid dwmapi)
  # Ship as a GUI (WINDOWS) subsystem binary so a double-click does NOT pop a
  # console window (the macOS .app has no terminal either). main() still runs
  # (MinGW's -mwindows bridges WinMain->main); the CLI modes reattach to the
  # parent console at startup (see main.cpp) so --do/--selftest still print.
  target_link_options(piet_native PRIVATE -mwindows)

  # ── Windows resource layer: icon + version info + process manifest ──────
  # Explorer/taskbar/shortcut branding, truthful version metadata, and the
  # UTF-8 / long-path / per-monitor-DPI process manifest are embedded IN the
  # PE (no sidecar .manifest file beside the exe — a sidecar is ignored once
  # a manifest resource exists, and gets lost by the installer anyway).
  #
  # Sources: resources/windows/{piet.rc.in,piet.manifest.in,resource.h}.
  # Everything version-shaped is derived from PIET_VERSION_VALUE above.
  enable_language(RC)

  # VS_FIXEDFILEINFO's FILEVERSION/PRODUCTVERSION are four 16-bit integers;
  # PIET's release axis (for example, "B.7.4") is not, so map it onto a numeric
  # tuple here and keep the human-readable string for the RC string table.
  # A -> 1.x.y.0, B -> 0.x.y.0 (B is the pre-1.0 axis, so it sorts BELOW A);
  # a plain X.Y.Z is taken literally. Anything else is a hard error rather
  # than a silently wrong "0.0.0.0" in the shipped binary.
  if(PIET_VERSION_VALUE MATCHES "^B\\.([0-9]+)\\.([0-9]+)$")
    set(PIET_WIN_VERSION_MAJOR 0)
    set(PIET_WIN_VERSION_MINOR "${CMAKE_MATCH_1}")
    set(PIET_WIN_VERSION_PATCH "${CMAKE_MATCH_2}")
    set(PIET_WIN_VERSION_BUILD 0)
  elseif(PIET_VERSION_VALUE MATCHES "^A\\.([0-9]+)\\.([0-9]+)$")
    set(PIET_WIN_VERSION_MAJOR 1)
    set(PIET_WIN_VERSION_MINOR "${CMAKE_MATCH_1}")
    set(PIET_WIN_VERSION_PATCH "${CMAKE_MATCH_2}")
    set(PIET_WIN_VERSION_BUILD 0)
  elseif(PIET_VERSION_VALUE MATCHES "^([0-9]+)\\.([0-9]+)\\.([0-9]+)$")
    set(PIET_WIN_VERSION_MAJOR "${CMAKE_MATCH_1}")
    set(PIET_WIN_VERSION_MINOR "${CMAKE_MATCH_2}")
    set(PIET_WIN_VERSION_PATCH "${CMAKE_MATCH_3}")
    set(PIET_WIN_VERSION_BUILD 0)
  else()
    message(FATAL_ERROR
      "PIET_VERSION_VALUE ('${PIET_VERSION_VALUE}') is not a supported release "
      "version: expected B.X.Y, A.X.Y, or X.Y.Z. Extend the mapping in "
      "piet/native/CMakeLists.txt's WIN32 branch when the version axis changes.")
  endif()

  set(PIET_WINDOWS_RESOURCE_DIR "${CMAKE_CURRENT_BINARY_DIR}/windows_resources")
  file(MAKE_DIRECTORY "${PIET_WINDOWS_RESOURCE_DIR}")
  # Forward slashes throughout: an .rc string literal treats backslash as an
  # escape character, so a native Windows path baked into the generated
  # piet.rc would mangle (\b, \t, ...). CMake paths are already '/'-form;
  # TO_CMAKE_PATH makes that explicit and survives a -D override.
  file(TO_CMAKE_PATH "${PIET_WINDOWS_RESOURCE_DIR}/piet.manifest"
       PIET_WINDOWS_MANIFEST_PATH)
  file(TO_CMAKE_PATH "${MONOREPO_ROOT}/branding/PIET.ico"
       PIET_WINDOWS_ICON_PATH)

  # branding/PIET.ico is a GENERATED, checked-in artifact
  # (branding/make_windows_ico.py packs the canonical cobra-icon-<N>.png set
  # verbatim into a PNG-compressed ICO). If a checkout lacks it, try to
  # regenerate into the build tree — never into the source tree — and fall
  # back to an iconless build with a loud diagnostic rather than failing
  # windres on a missing file. Configure with -DPIET_REQUIRE_WINDOWS_ICON=ON
  # to make the fallback fatal instead (release builds should).
  option(PIET_REQUIRE_WINDOWS_ICON
         "Fail configuration if the Windows application icon is unavailable" OFF)
  set(PIET_WINDOWS_ICON_GENERATOR "${MONOREPO_ROOT}/branding/make_windows_ico.py")
  if(NOT EXISTS "${PIET_WINDOWS_ICON_PATH}" AND EXISTS "${PIET_WINDOWS_ICON_GENERATOR}")
    find_package(Python3 QUIET COMPONENTS Interpreter)
    if(Python3_Interpreter_FOUND)
      set(PIET_WINDOWS_ICON_REGEN "${PIET_WINDOWS_RESOURCE_DIR}/PIET.ico")
      execute_process(
        COMMAND "${Python3_EXECUTABLE}" "${PIET_WINDOWS_ICON_GENERATOR}"
                --output "${PIET_WINDOWS_ICON_REGEN}"
        RESULT_VARIABLE PIET_WINDOWS_ICON_REGEN_RC
        OUTPUT_VARIABLE PIET_WINDOWS_ICON_REGEN_OUT
        ERROR_VARIABLE  PIET_WINDOWS_ICON_REGEN_ERR)
      # STREQUAL "0", not EQUAL 0: a failure to even launch the interpreter
      # puts a message string (not a number) in RESULT_VARIABLE.
      if(PIET_WINDOWS_ICON_REGEN_RC STREQUAL "0" AND EXISTS "${PIET_WINDOWS_ICON_REGEN}")
        message(STATUS
          "piet: branding/PIET.ico absent; regenerated into the build tree "
          "(${PIET_WINDOWS_ICON_REGEN})")
        file(TO_CMAKE_PATH "${PIET_WINDOWS_ICON_REGEN}" PIET_WINDOWS_ICON_PATH)
      else()
        message(STATUS
          "piet: branding/PIET.ico regeneration failed "
          "(exit ${PIET_WINDOWS_ICON_REGEN_RC}): "
          "${PIET_WINDOWS_ICON_REGEN_OUT}${PIET_WINDOWS_ICON_REGEN_ERR}")
      endif()
    endif()
  endif()

  if(EXISTS "${PIET_WINDOWS_ICON_PATH}")
    set(PIET_WINDOWS_ICON_RESOURCE "IDI_PIET ICON \"${PIET_WINDOWS_ICON_PATH}\"")
  elseif(PIET_REQUIRE_WINDOWS_ICON)
    message(FATAL_ERROR
      "Windows application icon not available: ${PIET_WINDOWS_ICON_PATH} does "
      "not exist and could not be regenerated. Run "
      "`python3 branding/make_windows_ico.py` (needs branding/cobra-icon-{16,32,"
      "64,128,256}.png and no third-party modules), then re-configure.")
  else()
    message(WARNING
      "piet: building piet_native.exe WITHOUT an application icon — "
      "${PIET_WINDOWS_ICON_PATH} does not exist and could not be regenerated "
      "(needs python3 plus branding/cobra-icon-{16,32,64,128,256}.png). "
      "Explorer, the taskbar and the installer shortcuts will show the generic "
      "executable icon. Fix: run `python3 branding/make_windows_ico.py` and "
      "re-configure; use -DPIET_REQUIRE_WINDOWS_ICON=ON to make this fatal.")
    set(PIET_WINDOWS_ICON_RESOURCE
        "/* no application icon: branding/PIET.ico was unavailable at configure time */")
  endif()

  # resource.h is copied (not substituted) so the generated piet.rc's
  # `#include "resource.h"` resolves next to itself even if windres' include
  # search differs from the compiler's.
  configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/resources/windows/resource.h"
    "${PIET_WINDOWS_RESOURCE_DIR}/resource.h"
    COPYONLY)
  configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/resources/windows/piet.manifest.in"
    "${PIET_WINDOWS_MANIFEST_PATH}"
    @ONLY)
  configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/resources/windows/piet.rc.in"
    "${PIET_WINDOWS_RESOURCE_DIR}/piet.rc"
    @ONLY)
  target_sources(piet_native PRIVATE "${PIET_WINDOWS_RESOURCE_DIR}/piet.rc")
  target_include_directories(piet_settings INTERFACE "${PIET_WINDOWS_RESOURCE_DIR}")

  # columnar.cpp's <sys/mman.h> mmap has no MinGW header/impl; the cross-build
  # (docker/Dockerfile.piet-windows) supplies docker/windows-compat's shim via
  # -I + a static libmman_compat.a on the link line — a build-environment
  # concern, not wired here (piet/native does not own docker/).
endif()
