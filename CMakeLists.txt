cmake_minimum_required(VERSION 3.20)
project(cecon LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(WIN32)
  # MinGW's unsuffixed stat/off_t ABI is otherwise 32-bit. Keep .hbin sizes
  # valid above 2 GiB; the separately-built mmap shim receives the same define
  # in docker/Dockerfile.windows.
  add_compile_definitions(_FILE_OFFSET_BITS=64)
endif()
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

# Default optimization. On Apple Silicon we can safely use -march=native (we
# only run on this Mac). On Linux we use a portable baseline so binaries built
# in a container run on any modern cloud CPU. The non-Apple x86 -march is a cache
# variable so a downstream build (e.g. Porting's docker/) can retarget the
# baseline without editing this src-governed file — e.g. -DCECON_X86_ARCH=x86-64
# for a no-AVX2 box. (The Apple/aarch64 paths are unaffected.)
set(CECON_X86_ARCH "x86-64-v3" CACHE STRING
    "x86 -march baseline for non-Apple builds (override e.g. -DCECON_X86_ARCH=x86-64)")
if(APPLE)
  add_compile_options(-O3 -march=native -Wall -Wextra -Wno-deprecated-declarations)
else()
  # AMD64 (upper case) is what CMake reports on native Windows; without it the
  # baseline above is silently not applied there.
  if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|amd64|AMD64)$")
    add_compile_options(-O3 -march=${CECON_X86_ARCH} -mtune=generic
                        -Wall -Wextra -Wno-deprecated-declarations)
  else()  # aarch64 / arm64 / other
    add_compile_options(-O3 -mtune=generic
                        -Wall -Wextra -Wno-deprecated-declarations)
  endif()
endif()

# BLAS/LAPACK backend: Apple Accelerate on macOS, OpenBLAS + LAPACK on Linux.
set(THREADS_PREFER_PTHREAD_FLAG ON)
find_package(Threads REQUIRED)

if(APPLE)
  find_library(ACCELERATE_LIB Accelerate REQUIRED)
  set(CECON_LA_LIBS ${ACCELERATE_LIB})
  set(CECON_LA_DEFS ACCELERATE_NEW_LAPACK=1)
elseif(CECON_STATIC_LIBS)
  # Caller (typically the Docker static build) passed full .a archive paths
  # already including OpenBLAS / LAPACKE / gfortran / quadmath. Add system
  # dynamic deps that always live on a glibc box.
  set(CECON_LA_LIBS ${CECON_STATIC_LIBS})
  if(UNIX)
    list(APPEND CECON_LA_LIBS -ldl)  # libdl is glibc-only; MinGW-w64 (WIN32 cross) has none
  endif()
  set(CECON_LA_DEFS "")
  set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -static-libgcc -static-libstdc++")
else()
  # Linux dynamic-link path (system OpenBLAS via .so).
  find_package(PkgConfig QUIET)
  if(PkgConfig_FOUND)
    pkg_check_modules(OPENBLAS QUIET openblas)
  endif()
  if(OPENBLAS_FOUND)
    set(CECON_LA_LIBS ${OPENBLAS_LIBRARIES})
    include_directories(${OPENBLAS_INCLUDE_DIRS})
  else()
    find_library(OPENBLAS_LIB NAMES openblas REQUIRED)
    set(CECON_LA_LIBS ${OPENBLAS_LIB})
  endif()
  find_library(LAPACKE_LIB NAMES lapacke)
  if(LAPACKE_LIB)
    list(APPEND CECON_LA_LIBS ${LAPACKE_LIB})
  endif()
  set(CECON_LA_DEFS "")
endif()

add_library(cecon_core STATIC
  src/io/columnar.cpp
  src/io/dta_reader.cpp
  src/io/dta_writer.cpp
  src/data/frame.cpp
  src/data/frame2.cpp
  src/data/datasignature.cpp
  src/core/linalg.cpp
  src/estimators/aer_smooth.cpp
  src/estimators/oaxaca.cpp
  src/estimators/nnmatch.cpp
  src/estimators/pca.cpp
  src/core/matrix.cpp
  src/core/model.cpp
  src/core/threads.cpp
  src/hdfe/demean.cpp
  src/hdfe/dof.cpp
  src/vcov/vcov.cpp
  src/estimators/ols.cpp
  src/estimators/feols.cpp
  src/estimators/fepois.cpp
  src/estimators/logit.cpp
  src/estimators/probit.cpp
  src/estimators/csdid.cpp
  src/estimators/xtpois.cpp
  src/estimators/xtreg.cpp
  src/estimators/nbreg.cpp
  src/estimators/xtlogit.cpp
  src/estimators/posthest.cpp
  src/estimators/iv2sls.cpp
  src/estimators/qreg.cpp
  src/inference/wildboot.cpp
  src/estimators/oglm.cpp
  src/estimators/mlogit.cpp
  src/estimators/zicount.cpp
  src/estimators/ivml.cpp
  src/estimators/mixedlm.cpp
  src/estimators/survival.cpp
  src/estimators/varmodel.cpp
  src/estimators/xtabond.cpp
  src/estimators/nlgmm.cpp
  src/estimators/miimpute.cpp
  src/vcov/svy_vcov.cpp
  src/estimators/rdlocal.cpp
  src/estimators/sunab.cpp
  src/estimators/didimpute.cpp
  src/estimators/sdid.cpp
  # X2-GLM: Stata-default glm lifecycle; also compiled into piet_core (PIET fits glm
  # in-process), so it depends on nothing but the standard library and core/rng_normal.h.
  src/estimators/glm.cpp
)
target_include_directories(cecon_core PUBLIC src vendor)
if(CECON_LA_DEFS)
  target_compile_definitions(cecon_core PUBLIC ${CECON_LA_DEFS})
endif()
target_link_libraries(cecon_core PUBLIC ${CECON_LA_LIBS} Threads::Threads)

add_executable(cobra apps/cecon_cli.cpp apps/cmds/common.cpp apps/cmds/options_table.cpp apps/cmds/cmds_linear.cpp apps/cmds/cmds_panel.cpp apps/cmds/cmds_mle.cpp apps/cmds/cmds_postest.cpp apps/cmds/cmds_diag.cpp apps/cmds/cmds_ts.cpp apps/cmds/cmds_summary.cpp apps/cmds/cmds_data.cpp apps/cmds/cmds_survey.cpp apps/cmds/cmds_oglm.cpp apps/cmds/cmds_mlogit.cpp apps/cmds/cmds_zicount.cpp apps/cmds/cmds_ivml.cpp apps/cmds/cmds_mixedlm.cpp apps/cmds/cmds_survival.cpp apps/cmds/cmds_varmodel.cpp apps/cmds/cmds_xtabond.cpp apps/cmds/cmds_nlgmm.cpp apps/cmds/cmds_miimpute.cpp apps/cmds/cmds_rd.cpp apps/cmds/cmds_eventstudy.cpp apps/cmds/cmds_sdid.cpp apps/cmds/cmds_binscatter.cpp apps/cmds/cmds_aer_estim.cpp apps/cmds/cmds_oaxaca.cpp apps/cmds/cmds_nnmatch.cpp)
target_link_libraries(cobra PRIVATE cecon_core)
target_compile_definitions(cobra PRIVATE COBRA_VERSION="1.0.2")

enable_testing()
find_package(Python3 COMPONENTS Interpreter QUIET)

# Fixture root for the tests/validate goldens (tests/unit/validate_dir.h). Every
# target created BELOW this point is a test target, so a directory-scoped define
# is enough — cecon_core and cobra are already defined above and stay untouched.
# Runtime override: set the COBRA_VALIDATE_DIR environment variable.
add_compile_definitions(COBRA_VALIDATE_DIR="${CMAKE_SOURCE_DIR}/tests/validate")





# BUG-021 regression: iv drops singleton FE groups by default (drop_singletons_iv).



# C2-F8: radix gsort permutation-identity fuzz (header-only radix_sort.h vs the
# former std::stable_sort comparator over pathological + 4000 random cases).









































# MI-066: native .dta reader (formats 115/117/118) parity vs pandas oracle.

# §35: format 114 (Stata 10/11 `saveold`) shares the legacy container with 115 —
# the parser body always handled it, only the version gate refused it. Pinned on
# the two real 114 files in tests/validate/, cross-checked cell-for-cell against
# pandas.read_stata (an independent 114 decoder).

# §35: fepois e(ll)/e(ll_0)/e(r2_p)/e(chi2)/e(df_m) mechanical invariants — the
# deviance identity ll == ll_sat - deviance/2 (an independent cross-check, since
# ll is computed from mu directly), the iid-LR vs robust-Wald selection, and the
# intercept-only null under absorb(). Stata-oracle values: validate_wave_oracle.py.

# P6 rider: all 27 missing values (`.`, `.a`..`.z`) survive a .dta write->read.


# MI-066: data verbs (merge/reshape/append/duplicates/gsort/order/recode) —
# Frame-level unit coverage.

# MAXPERF Stage 3: morsel-parallel collapse/merge determinism — asserts the kernel
# outputs are BYTE-identical across CECON_NUM_THREADS=1 vs =4 (the ① gate for the
# Stage-3 thread dispatch). Self-contained: forks on POSIX, self-spawns on Windows.
# stats-summary-tabulate-family (BUG-1013/1015): summarize's exact accumulation, its
# per-group row mask (tabstat) and xtile's weighted cutpoints, pinned to the StataBE 19.5
# capture w2as_summary_stats_family (sx_sum_acc*, sx_xtile_n_aw).
add_executable(test_summary_kernels tests/unit/test_summary_kernels.cpp)
target_link_libraries(test_summary_kernels PRIVATE cecon_core)
add_test(NAME summary_kernels COMMAND test_summary_kernels)

add_executable(test_morsel_determinism tests/unit/test_morsel_determinism.cpp)
target_link_libraries(test_morsel_determinism PRIVATE cecon_core)
add_test(NAME morsel_determinism COMMAND test_morsel_determinism)

# e(sample) T1a: `--esample-out` on feols/iv/xtreg/newey/prais writes the fit's
# admitted rows (one 1/0 per --data row, or scattered through --esample-index).
# Arms pinned to tests/eval/golden/aer_w2/w2_esample.log counts on the capture's own
# deterministic data; drives the cobra CLI (the flag lives in apps/cmds).
add_executable(test_esample_out tests/unit/test_esample_out.cpp)
target_link_libraries(test_esample_out PRIVATE cecon_core)
add_test(NAME esample_out COMMAND test_esample_out $<TARGET_FILE:cobra>)

# BUG-471 T1 (float default): `cobra eval --store-type byte|int|long|float|double` rounds
# every --gen/--egen/--rowegen/--bygen output to that Stata storage type, and float() in
# the row evaluator. Arms pinned to tests/eval/golden/aer_w2/w2e_float_default.log %21x
# values; drives the cobra CLI (the flag lives in apps/cmds).
add_executable(test_eval_store_type tests/unit/test_eval_store_type.cpp)
target_link_libraries(test_eval_store_type PRIVATE cecon_core)
add_test(NAME eval_store_type COMMAND test_eval_store_type $<TARGET_FILE:cobra>)

# MAXPERF Stage 2 (contract_A §27): column-granularity CoW store + O(1) session
# snapshots — derived-frame column sharing, column-CoW isolation, stash/unstash +
# virtual-file round-trips, and borrowed-root survival across a resident swap.


# COUNCIL-20260710 pre-wave audit: E2 finite-or-missing evaluator (BUG-057),
# E3 keep_if bare-truthy missing=TRUE (BUG-058), E4 recode copy-through
# (BUG-059), E8 save,replace via temp+rename — all in src/data/frame.cpp.

# COUNCIL-20260710 pre-wave audit: E1 TS operators materialize before `if`
# (BUG-054, build_model/build_iv_model row_mask + apps/cecon_cli.cpp's
# plan_keepif/eval_keepif_mask/split_ts routing), E6 CLI unknown-flag
# rejection net (BUG-060, apps/cecon_cli.cpp opt() consumed-argv tracking).

# Python-backed CLI tests use the discovered interpreter (`python.exe` is a
# normal Windows spelling, and there is no `python3` there). A production-only
# configure remains dependency-free; release gates verify the expected CTest
# count and therefore require Python.
if(Python3_Interpreter_FOUND)
# Tier-4 stats/tabulation parity (corr/pwcorr/ttest/sdtest/vif/tabstat/tabulate/codebook)
# Python validator vs Stata goldens; LOCAL-only (auto.hbin + t4_*_golden.csv are gitignored).
add_test(NAME t4_stats
  COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tests/validate/validate_t4.py
          $<TARGET_FILE:cobra> ${CMAKE_SOURCE_DIR}/tests/validate)

# Factor-variable interactions (i./c./#/##) parity vs Stata golden; LOCAL-only
# (fvint.hbin + fvint_golden.csv gitignored).
add_test(NAME fvint
  COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tests/validate/validate_fvint.py
          $<TARGET_FILE:cobra> ${CMAKE_SOURCE_DIR}/tests/validate)

# Collinear-column omission in rank-deficient factor-variable designs (empty cells
# + exact collinearity): cobra must omit the SAME term as Stata `regress` and
# match surviving coefs. LOCAL-only (auto.hbin + fvauto_golden.csv gitignored).
add_test(NAME fvcoll
  COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tests/validate/validate_fvcoll.py
          $<TARGET_FILE:cobra> ${CMAKE_SOURCE_DIR}/tests/validate)

# GA sweep 2026-08-29 (leg 5). These three read DATA fixtures the repo
# deliberately does not ship (the comments above already said "LOCAL-only"), and
# until now ctest reported their absence as a FAILURE — so a fresh clone saw 80
# of 92 red and could not tell "this gate found a defect" from "this gate never
# ran". Exit 77 is the gates' honest-skip signal (tests/validate/_fixtures.py);
# telling ctest about it is what turns a lie into a declared skip. A gate that
# cannot run must say so, not report a result it never measured (BUG-362's rule,
# applied to the test harness itself).
set_tests_properties(t4_stats fvint fvcoll PROPERTIES SKIP_RETURN_CODE 77)


# BUG-021 (iv default singleton drop + --keep-singletons opt-out) and BUG-020
# (--keepif scratch honors $TMPDIR + is cleaned up), end-to-end via the CLI.
# Self-contained (builds its own CSV fixture), so it needs no gitignored data.
add_test(NAME iv_singleton_cli
  COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tests/validate/validate_iv_singleton.py
          $<TARGET_FILE:cobra>)

# feols --fe panel R² (Stata xtreg,fe / areg within/between/overall), end-to-end via
# the CLI. Regression guard for the 3217393 _cons-row side-effect that silently dropped
# r2_between/r2_overall (add_panel_r2 guard rejected the k+1 names). Uses the committed
# panel_synth.hbin fixture (tracked, not gitignored).
add_test(NAME panel_r2_cli
  COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tests/validate/validate_panel_r2.py
          $<TARGET_FILE:cobra>)

# §35: an excluded instrument annihilated by the FE absorption must be OMITTED
# (reported in the JSON `omitted` array, since the engine has no success-path
# warning channel) instead of raising the old hard "collinear" error — ivreghdfe
# parity. Also pins that the surviving fit is IDENTICAL to the hand-pruned spec and
# that genuine under-identification still errors. Self-contained (builds its own
# synthetic panel), so it cannot false-fail on a fresh worktree.
add_test(NAME iv_omit_cli
  COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tests/validate/validate_iv_omit.py
          $<TARGET_FILE:cobra>)

# §35: Stata 19.5 answer-book pins for the new fepois ll/chi2/r2_p and the
# xtreg,fe sigma_u/sigma_e/rho/corr_u_xb, read out of the P2 pangram oracle log.
# SKIPs loudly (exit 0) when the gitignored pangram wave artifact is absent.
add_test(NAME wave_oracle_cli
  COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tests/validate/validate_wave_oracle.py
          $<TARGET_FILE:cobra>)

# BUG-040: csdid input-validation parity — Stata refuses ivar(X) ... cluster(X)
# (same variable named as both the panel id and the cluster var) with r(198)
# "X may not be both target and by()"; cobra must reject it too (Doctrine-1,
# bidirectional gate 1). Also pins that a DIFFERENT cluster var, and a
# value-identical copy of ivar under a DIFFERENT name, both stay ACCEPTED
# (the Stata check is name/token-based, not data-based). End-to-end via the
# CLI. Self-contained (builds its own CSV fixture).
add_test(NAME csdid_input_parity_cli
  COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tests/validate/validate_csdid_input_parity.py
          $<TARGET_FILE:cobra>)

# COUNCIL-20260710 pre-wave audit: E6 CLI unknown-flag rejection net (BUG-060)
# — any argv "--xxx" token no opt()/has()/opt_all() call in the dispatched
# command ever looks up is rejected (bad_input, naming the flag) instead of
# silently ignored, while every real flag (incl. ones only read inside a
# conditional branch) stays accepted. End-to-end via the CLI (the tracking
# mechanism lives entirely in apps/cecon_cli.cpp, an executable with no
# externally-linkable test surface). Self-contained (builds its own CSV
# fixture).
add_test(NAME e6_unknown_flag_cli
  COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tests/validate/validate_e6_unknown_flag.py
          $<TARGET_FILE:cobra>)

# B.0.3 option-forwarding contract: buffered dispatch (no stdout leak on reject),
# `cobra options --json` shape, and options_table() drift (COBRA_OPTIONS_STRICT).
add_test(NAME options_contract_cli
  COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tests/validate/validate_options_contract.py
          $<TARGET_FILE:cobra>)

# WS-D2 / S06 ARC-MRG: a two-file verb (merge) opens each input EXACTLY ONCE.
# The P2-PERF close named merge as opening its files three times; the third was
# Frame::from_dataset re-opening a path handed to it as a bare `const Dataset&`.
# Counter gate (deterministic), not a timing one. Self-contained fixtures.
add_test(NAME merge_file_opens_cli
  COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tests/validate/validate_merge_file_opens.py
          $<TARGET_FILE:cobra>)

# BUG-112(6): omitted (collinear) coef-map terms land at their Stata-natural
# design-matrix position (add_omitted's new full_names param) instead of
# tail-appended after the survivors. Covers all 3 add_omitted call sites:
# cmd_ols, run_feols, run_feols_multi.
add_test(NAME bug112_addomitted_order_cli
  COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tests/validate/validate_bug112_addomitted_order.py
          $<TARGET_FILE:cobra>)

# BUG-112(8): `ib#.` with a base level absent from the data is no longer silent —
# the response carries a top-level "warnings" array (emitted ONLY when non-empty,
# so clean responses stay byte-identical and no golden re-captures). The StataBE
# 19.5 oracle (2026-07-31) ruled out a hard reject: _rc==0 on all 5 arms, with a
# `note:` line — warn-and-proceed. Estimates are unchanged (ib99. == ibn.).
add_test(NAME bug112_ibwarn_cli
  COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tests/validate/validate_bug112_ibwarn.py
          $<TARGET_FILE:cobra>)

# BUG-152: the shared GLM loader must route Stata factor-variable tokens through
# the canonical model expander instead of looking for a literal `i.region`
# column.  Pins logit + probit and the historical intercept-last coefficient
# order using an equivalent explicitly dummy-coded design.
add_test(NAME bug152_glm_fv_cli
  COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tests/validate/validate_bug152_glm_fv.py
          $<TARGET_FILE:cobra>)

# contract_A §26 resident-frame serve session (MAXPERF Stage 1b): a scripted
# `cobra serve` session — use / run (mutator + read-only) / snapshot / drop —
# pinning the frame deltas, the atomic commit-on-success (a bad run leaves state
# intact), stateless↔frame interleaving, structural (keepif) snapshots, and the
# parity law (resident feols numbers == one-shot feols on the snapshot). Drives the
# stdin/stdout line-JSON protocol end-to-end. Self-contained (builds its own CSV).
add_test(NAME serve_frame_cli
  COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tests/validate/validate_serve_frame.py
          $<TARGET_FILE:cobra>)
else()
  message(WARNING "Python3 interpreter not found: Python-backed CLI tests are disabled")
endif()

# Benchmark harness (manual; not a ctest)
add_executable(bench_demean tests/bench/bench_demean.cpp)
target_link_libraries(bench_demean PRIVATE cecon_core)

# --- P7 wave unit tests -------------------------------------------------












# P-QE wave — scaffold-only placeholder tests (see tests/unit/test_*.cpp for the
# oracle targets the implementing arc must hit).




# BUG-369 — time-series operators inside an EXPRESSION (gen/replace/keep if).
# Pins the gap, the panel boundary and the "a column named L is still a column"
# fallback; a row-based lag or a hijacked identifier fails here.

# COBRA-DD-ACCUM (TOLERANCE §5) — compensated-accumulation kernel test. Every
# case is DESIGNED to go RED against the pre-fix plain-BLAS kernels; the measured
# pre-fix relative error is recorded per case inside the file.

# P9 entry contract (TOLERANCE §4.2) — thread-count determinism for the ESTIMATOR-side
# parallel kernels: HDFE demean (lane reduction), xtlogit,fe (per-panel parallel_for),
# wild cluster bootstrap (per-replicate parallel_for). test_morsel_determinism covers
# the Frame-level dispatch sites; these three were unguarded until the P9 entry audit.
# Same two-process shape (fork on POSIX, self-spawn on Windows), raw f64 bit compare.
# AER bundle H1 — the four smoothers, pinned against StataBE oracles and against
# the specific defects the pre-implementation spec review found.
add_executable(test_aer_estim tests/unit/test_aer_estim.cpp)
target_link_libraries(test_aer_estim PRIVATE cecon_core)
add_test(NAME aer_estim COMMAND test_aer_estim)

add_executable(test_oaxaca tests/unit/test_oaxaca.cpp)
target_link_libraries(test_oaxaca PRIVATE cecon_core)
add_test(NAME oaxaca COMMAND test_oaxaca)
add_executable(test_nnmatch tests/unit/test_nnmatch.cpp)
target_link_libraries(test_nnmatch PRIVATE cecon_core)
add_test(NAME nnmatch COMMAND test_nnmatch)

add_executable(test_thread_determinism tests/unit/test_thread_determinism.cpp)
target_link_libraries(test_thread_determinism PRIVATE cecon_core)
add_test(NAME thread_determinism COMMAND test_thread_determinism)

# Settings-derived compute envelope (2026-09-02): the resolver every dispatch site
# sizes from must honour the CORE budget ($CECON_NUM_THREADS) and the MEMORY
# budget ($CECON_MEMORY_BUDGET_MB) PIET exports from the Settings panel — the
# memory term bounds per-thread scratch to a quarter of the budget and is
# unbounded when undeclared. Both are first-call-cached singletons, so each
# environment is a separate child process (fork on POSIX, self-spawn on Windows).
add_executable(test_threads_budget tests/unit/test_threads_budget.cpp)
target_link_libraries(test_threads_budget PRIVATE cecon_core)
add_test(NAME threads_budget COMMAND test_threads_budget)

# engine-expression-functions (2026-09-29): the w2c_functions data arms. Standalone
# rather than in a grouped runner because a runner's dispatch table lives in
# tests/unit/runners/, which this task does not own; fold it into one when a
# runner is next regenerated.
add_executable(test_fn_w2c tests/unit/test_fn_w2c.cpp)
target_link_libraries(test_fn_w2c PRIVATE cecon_core)
add_test(NAME fn_w2c COMMAND test_fn_w2c)

# unicode-names lane E (2026-10-28): UTF-8 (Arabic, mixed-script) names through the
# engine's identifier lexers -- Frame's gen/if parser, L. operators, nl/gmm/nlcom
# expr_eval, the lincom/test grammar and the datetime-literal word boundary.
add_executable(test_unicode_ident tests/unit/test_unicode_ident.cpp)
target_link_libraries(test_unicode_ident PRIVATE cecon_core)
add_test(NAME unicode_ident COMMAND test_unicode_ident)

# w2b-estim-vce-options (2026-10-12): the w2b_estim qreg arms (vce(robust), [pw=]
# vce(robust), and the iid / q(.25) controls) on the do-file's float-stored data.
# Standalone for the same reason as test_fn_w2c: the runner dispatch tables live in
# tests/unit/runners/, which this task does not own.
add_executable(test_estim_w2b tests/unit/test_estim_w2b.cpp)
target_link_libraries(test_estim_w2b PRIVATE cecon_core)
add_test(NAME estim_w2b COMMAND test_estim_w2b)

# engine-multiway-cluster3 (2026-10-12): the w2b_estim reghdfe_cluster3 arm and its
# reghdfe_cluster2_ctl control (k-way multiway clustering in compute_vcov).
# Standalone for the same reason as test_estim_w2b.
add_executable(test_cluster3 tests/unit/test_cluster3.cpp)
target_link_libraries(test_cluster3 PRIVATE cecon_core)
add_test(NAME cluster3 COMMAND test_cluster3)

# FU-138 / FU-210 / FU-205: logit/probit reproduce Stata's ml iterate (the trace,
# e(ic), b, se and e(ll) of w2b_estim / w2e_probes / w2c_ml_getmata on their float
# replicas), the maximize-option flags, and probit's --vce validation. Kernel arms
# plus CLI arms (argv[1] = cobra, run under COBRA_OPTIONS_STRICT=1).
add_executable(test_mle_parity tests/unit/test_mle_parity.cpp)
target_link_libraries(test_mle_parity PRIVATE cecon_core)
add_test(NAME mle_parity COMMAND test_mle_parity $<TARGET_FILE:cobra>)

# w2b-iv-options (2026-10-12): the w2b_estim IV arms' engine half -- the
# --savefirst stages `first` prints (with and without absorbed effects) and the
# Frisch-Waugh identity ivreg2's partial() is served by. Standalone for the same
# reason as test_estim_w2b.
add_executable(test_iv_w2b tests/unit/test_iv_w2b.cpp)
target_link_libraries(test_iv_w2b PRIVATE cecon_core)
add_test(NAME iv_w2b COMMAND test_iv_w2b)

# R4 (test consolidation, 2026-09-01): 76 single-main test TUs link into 8
# grouped runners (one exec per ctest entry keeps per-test process isolation;
# names are unchanged). The two SELF-EXEC tests (morsel/thread determinism
# re-exec argv[0] with their own child-mode arguments) stay standalone --
# a dispatcher argv would collide with theirs.
add_executable(runner01 tests/unit/runners/runner01.cpp tests/unit/test_binscatter.cpp tests/unit/test_csdid_separation.cpp tests/unit/test_dta114.cpp tests/unit/test_glmqp.cpp tests/unit/test_iv_singleton.cpp tests/unit/test_miimpute.cpp tests/unit/test_nlgmm.cpp tests/unit/test_reg3.cpp tests/unit/test_ts_operators.cpp tests/unit/test_xtlogit.cpp)
target_link_libraries(runner01 PRIVATE cecon_core)
add_test(NAME binscatter COMMAND runner01 binscatter)
add_test(NAME csdid_separation COMMAND runner01 csdid_separation)
add_test(NAME dta114 COMMAND runner01 dta114)
add_test(NAME glmqp COMMAND runner01 glmqp)
add_test(NAME iv_singleton COMMAND runner01 iv_singleton)
add_test(NAME miimpute COMMAND runner01 miimpute)
add_test(NAME nlgmm COMMAND runner01 nlgmm)
add_test(NAME reg3 COMMAND runner01 reg3)
add_test(NAME ts_operators COMMAND runner01 ts_operators)
add_test(NAME xtlogit COMMAND runner01 xtlogit)
add_executable(runner02 tests/unit/runners/runner02.cpp tests/unit/test_cli_prewave.cpp tests/unit/test_csv.cpp tests/unit/test_dta_missing_roundtrip.cpp tests/unit/test_hdfe.cpp tests/unit/test_ivml.cpp tests/unit/test_missing.cpp tests/unit/test_oglm.cpp tests/unit/test_rowegen.cpp tests/unit/test_tsma.cpp tests/unit/test_xtpois.cpp)
target_link_libraries(runner02 PRIVATE cecon_core)
add_test(NAME cli_prewave COMMAND runner02 cli_prewave)
add_test(NAME csv COMMAND runner02 csv)
add_test(NAME dta_missing_roundtrip COMMAND runner02 dta_missing_roundtrip)
add_test(NAME hdfe COMMAND runner02 hdfe)
add_test(NAME ivml COMMAND runner02 ivml)
add_test(NAME missing COMMAND runner02 missing)
add_test(NAME oglm COMMAND runner02 oglm)
add_test(NAME rowegen COMMAND runner02 rowegen)
add_test(NAME tsma COMMAND runner02 tsma)
add_test(NAME xtpois COMMAND runner02 xtpois)
add_executable(runner03 tests/unit/runners/runner03.cpp tests/unit/test_coll_tsf.cpp tests/unit/test_d2.cpp tests/unit/test_dta_reader.cpp tests/unit/test_heckman.cpp tests/unit/test_kprk2.cpp tests/unit/test_mixedlm.cpp tests/unit/test_ols_synth.cpp tests/unit/test_sdid.cpp tests/unit/test_unitroot.cpp tests/unit/test_xtreg.cpp)
target_link_libraries(runner03 PRIVATE cecon_core)
add_test(NAME coll_tsf COMMAND runner03 coll_tsf)
add_test(NAME d2 COMMAND runner03 d2)
add_test(NAME dta_reader COMMAND runner03 dta_reader)
add_test(NAME heckman COMMAND runner03 heckman)
add_test(NAME kprk2 COMMAND runner03 kprk2)
add_test(NAME mixedlm COMMAND runner03 mixedlm)
add_test(NAME ols_synth COMMAND runner03 ols_synth)
add_test(NAME sdid COMMAND runner03 sdid)
add_test(NAME unitroot COMMAND runner03 unitroot)
add_test(NAME xtreg COMMAND runner03 xtreg)
add_executable(runner04 tests/unit/runners/runner04.cpp tests/unit/test_cow_frame.cpp tests/unit/test_data_verbs.cpp tests/unit/test_eventstudy.cpp tests/unit/test_hettest.cpp tests/unit/test_liml.cpp tests/unit/test_mkspline.cpp tests/unit/test_patch_view.cpp tests/unit/test_survival.cpp tests/unit/test_varmodel.cpp tests/unit/test_zicount.cpp)
target_link_libraries(runner04 PRIVATE cecon_core)
add_test(NAME cow_frame COMMAND runner04 cow_frame)
add_test(NAME data_verbs COMMAND runner04 data_verbs)
add_test(NAME eventstudy COMMAND runner04 eventstudy)
add_test(NAME hettest COMMAND runner04 hettest)
add_test(NAME liml COMMAND runner04 liml)
add_test(NAME mkspline COMMAND runner04 mkspline)
add_test(NAME patch_view COMMAND runner04 patch_view)
add_test(NAME survival COMMAND runner04 survival)
add_test(NAME varmodel COMMAND runner04 varmodel)
add_test(NAME zicount COMMAND runner04 zicount)
add_executable(runner05 tests/unit/runners/runner05.cpp tests/unit/test_csdid.cpp tests/unit/test_dd_accum.cpp tests/unit/test_fepois_ll.cpp tests/unit/test_iv.cpp tests/unit/test_lincom.cpp tests/unit/test_mlogit.cpp tests/unit/test_posthest.cpp tests/unit/test_svy.cpp tests/unit/test_wald.cpp)
target_link_libraries(runner05 PRIVATE cecon_core)
add_test(NAME csdid COMMAND runner05 csdid)
add_test(NAME dd_accum COMMAND runner05 dd_accum)
add_test(NAME fepois_ll COMMAND runner05 fepois_ll)
add_test(NAME iv COMMAND runner05 iv)
add_test(NAME lincom COMMAND runner05 lincom)
add_test(NAME mlogit COMMAND runner05 mlogit)
add_test(NAME posthest COMMAND runner05 posthest)
add_test(NAME svy COMMAND runner05 svy)
add_test(NAME wald COMMAND runner05 wald)
# w2c_postest (factor-level post-estimation names, 2026-09-26): its own executable,
# because the grouped runners' sources live in tests/unit/runners/, outside that lane.
add_executable(test_postest_w2c tests/unit/test_postest_w2c.cpp)
target_link_libraries(test_postest_w2c PRIVATE cecon_core)
add_test(NAME postest_w2c COMMAND test_postest_w2c)
# BUG-1067: feols --collinear-forward (reghdfe/areg/xtreg,fe keep the earlier member
# of a collinear set) and the refits that replay it, against StataBE m1.do values.
add_executable(test_collinear_forward tests/unit/test_collinear_forward.cpp)
target_link_libraries(test_collinear_forward PRIVATE cecon_core)
add_test(NAME collinear_forward COMMAND test_collinear_forward)
add_executable(runner06 tests/unit/runners/runner06.cpp tests/unit/test_csdid_estat.cpp tests/unit/test_demean_aa.cpp tests/unit/test_frame_prewave.cpp tests/unit/test_iv_diagnostics.cpp tests/unit/test_logit.cpp tests/unit/test_nbreg.cpp tests/unit/test_qreg.cpp tests/unit/test_tobit.cpp tests/unit/test_wildboot.cpp)
target_link_libraries(runner06 PRIVATE cecon_core)
add_test(NAME csdid_estat COMMAND runner06 csdid_estat)
add_test(NAME demean_aa COMMAND runner06 demean_aa)
add_test(NAME frame_prewave COMMAND runner06 frame_prewave)
add_test(NAME iv_diagnostics COMMAND runner06 iv_diagnostics)
add_test(NAME logit COMMAND runner06 logit)
add_test(NAME nbreg COMMAND runner06 nbreg)
add_test(NAME qreg COMMAND runner06 qreg)
add_test(NAME tobit COMMAND runner06 tobit)
add_test(NAME wildboot COMMAND runner06 wildboot)
add_executable(runner07 tests/unit/runners/runner07.cpp tests/unit/test_csdid_long2_grid.cpp tests/unit/test_determinism.cpp tests/unit/test_fvops.cpp tests/unit/test_iv_hansenj.cpp tests/unit/test_margins_at.cpp tests/unit/test_neweyprais.cpp tests/unit/test_radix_sort.cpp tests/unit/test_ts_diag.cpp tests/unit/test_xtabond.cpp)
target_link_libraries(runner07 PRIVATE cecon_core)
add_test(NAME csdid_long2_grid COMMAND runner07 csdid_long2_grid)
add_test(NAME determinism COMMAND runner07 determinism)
add_test(NAME fvops COMMAND runner07 fvops)
add_test(NAME iv_hansenj COMMAND runner07 iv_hansenj)
add_test(NAME margins_at COMMAND runner07 margins_at)
add_test(NAME neweyprais COMMAND runner07 neweyprais)
add_test(NAME radix_sort COMMAND runner07 radix_sort)
add_test(NAME ts_diag COMMAND runner07 ts_diag)
add_test(NAME xtabond COMMAND runner07 xtabond)
add_executable(runner08 tests/unit/runners/runner08.cpp tests/unit/test_csdid_missing_x.cpp tests/unit/test_drdid.cpp tests/unit/test_glm.cpp tests/unit/test_iv_multi.cpp tests/unit/test_mediate.cpp tests/unit/test_nlcom.cpp tests/unit/test_rdlocal.cpp tests/unit/test_ts_expr.cpp tests/unit/test_xtipo.cpp)
target_link_libraries(runner08 PRIVATE cecon_core)
add_test(NAME csdid_missing_x COMMAND runner08 csdid_missing_x)
add_test(NAME drdid COMMAND runner08 drdid)
add_test(NAME glm COMMAND runner08 glm)
add_test(NAME iv_multi COMMAND runner08 iv_multi)
add_test(NAME mediate COMMAND runner08 mediate)
add_test(NAME nlcom COMMAND runner08 nlcom)
add_test(NAME rdlocal COMMAND runner08 rdlocal)
add_test(NAME ts_expr COMMAND runner08 ts_expr)
add_test(NAME xtipo COMMAND runner08 xtipo)


# B4/W6 riders (merge update/replace/generate; the new egen functions). Its own
# runner, appended, so the wave adds lines here instead of editing an existing
# runner's source list while a second lane is in the same tree.
add_executable(runner_b4 tests/unit/runners/runner_b4.cpp tests/unit/test_b4_riders.cpp tests/unit/test_b4_egen.cpp tests/unit/test_b4_concat.cpp tests/unit/test_b4_savefirst.cpp)
target_link_libraries(runner_b4 PRIVATE cecon_core)
add_test(NAME b4_merge_update COMMAND runner_b4 b4_merge_update)
add_test(NAME b4_egen COMMAND runner_b4 b4_egen)
add_test(NAME b4_concat COMMAND runner_b4 b4_concat)
add_test(NAME b4_savefirst COMMAND runner_b4 b4_savefirst)

# GA sweep 2026-08-29 (leg 5). This must stay the LAST test-related statement in
# this file: get_property(... TESTS) only sees tests already add_test()'d above
# it, so anything registered after this point (a future runner, a future rider)
# would silently escape the sweep and its exit-77 skips would count as ctest
# FAILUREs again (BUG-648's rule).
#
# 77 = "this gate could not run", declared for EVERY test rather than a
# hand-picked list. Naming them individually is what made the first attempt
# wrong: the helpers exit 77 from a shared door, so any gate reading an
# uncommitted DATA fixture can reach it, and a list is a promise to remember
# every future one. The exit code carries the meaning; the list only decides
# whether ctest is told about it.
#
# This costs nothing where fixtures exist — a passing gate returns 0 — and it is
# what lets a fresh clone show honest skips instead of failures it never measured
# (BUG-362's rule, applied to the harness).
get_property(_all_tests DIRECTORY PROPERTY TESTS)
set_tests_properties(${_all_tests} PROPERTIES SKIP_RETURN_CODE 77)
