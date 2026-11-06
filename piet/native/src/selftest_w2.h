// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// W2 selftest scaffold (2026-09-19). One TU per wave-2 lane, so that parallel
// lanes never edit the same selftest file: each lane owns exactly one
// selftest_w2_<lane>.cpp and reports through w2check(), which forwards to the
// suite's own counter in selftest.cpp. Registration (this header, the calls in
// runSelftest(), the CMake source list) landed once, up front, for all lanes.
#pragma once
#include <string>

void w2check(bool cond, const std::string& label);  // selftest.cpp
// A SKIP line (counted, not a PASS): for an arm that is knowingly not met and
// is listed as such, e.g. a display-gate mismatch the lane cannot close.
void w2skip(const std::string& label, const std::string& why);  // selftest.cpp

void testW2MacroN();  // selftest_w2_macron.cpp
void testW2PredictFrame();  // selftest_w2_predictframe.cpp
void testW2EststoPrefix();  // selftest_w2_eststoprefix.cpp
void testW2LabelGlued();  // selftest_w2_labelglued.cpp
void testW2DoInclude();  // selftest_w2_doinclude.cpp
void testW2ByKeep();  // selftest_w2_bykeep.cpp
void testW2EstimOpts();  // selftest_w2_estimopts.cpp
void testW2Matrix();  // selftest_w2_matrix.cpp
void testW2Getmata();  // selftest_w2_getmata.cpp (ml-getmata T4)
void testW2Loops();  // selftest_w2_loops.cpp
void testW2ProgCtl();  // selftest_w2_progctl.cpp (ge2-prog-controlflow-temp)
void testW2MergeAppend();  // selftest_w2_mergeappend.cpp
void testW2EsttabNames();  // selftest_w2_esttabnames.cpp
void testW2BsBootstrap();  // selftest_w2_bs_bootstrap.cpp (lane BS, uni-bs)
void testW2EgenRename();  // selftest_w2_egenrename.cpp
void testW2Esample();  // selftest_w2_esample.cpp
void testR150Ivreg2Surface();  // selftest_r150_ivreg2.cpp (remaining-150 WS-F)
void testR150Xtivreg2Fe();  // selftest_r150_xtivreg2.cpp (remaining-150 WS-F2)

// Wave 2B lanes (scaffold landed 2026-09-19, same rule: one TU per lane).
void testW2bLinearFe();  // selftest_w2b_linearfe.cpp
void testW2bJoinReshape();  // selftest_w2b_joinreshape.cpp
void testW2bStrlWriter();  // selftest_w2b_strlwriter.cpp
void testW2bDelimited();  // selftest_w2b_delimited.cpp
void testW2bDataOpts();  // selftest_w2b_dataopts.cpp
void testW2bIvFamily();  // selftest_w2b_ivfamily.cpp
void testW2bIv();  // selftest_w2b_iv.cpp
void testW2bDescXtset();  // selftest_w2b_descxtset.cpp
void testW2bPredictStats();  // selftest_w2b_predictstats.cpp
void testW2bEsttabScreen();  // selftest_w2b_esttabscreen.cpp
void testW2bEgenOpts();  // selftest_w2b_egenopts.cpp
void testW2bGlm();  // selftest_w2b_glm.cpp
void testW2bCluster3();  // selftest_w2b_cluster3.cpp
void testW2bMargins();  // selftest_w2b_margins.cpp
void testW2bMisc();  // selftest_w2b_misc.cpp
void testW2cMacroFns();  // selftest_w2c_macrofns.cpp
void testW2cDataFns();  // selftest_w2c_datafns.cpp (piet-row-evaluator-string-data-arms)
void testW2bDestring();  // selftest_w2b_destring.cpp
void testW2XtsetPersist();  // selftest_w2_xtsetpersist.cpp
void testW2Tsset();  // selftest_w2_tsset.cpp (tsset-panel-tsfill-full)
void testW2dInRange();  // selftest_w2d_inrange.cpp
void testW2dEreturn();  // selftest_w2d_ereturn.cpp

// Wave 2C.
void testW2cPostest();  // selftest_w2c_postest.cpp
void testW2Ml();  // selftest_w2_ml.cpp

// E3 scaffold (one TU per E3 command; each native task owns its own arm).
void testW2E3Swindex();  // selftest_w2_e3swindex.cpp
void testW2E3Qreg2();  // selftest_w2_e3qreg2.cpp
void testW2E3Lasso();  // selftest_w2_e3lasso.cpp
void testW2E3Suest();  // selftest_w2_e3suest.cpp
void testW2E3Ritest();  // selftest_w2_e3ritest.cpp
void testW2E3Acreg();  // selftest_w2_e3acreg.cpp

// COMMON_GE2 string/math/matrix functions (goal ge2-fn-uncovered-string-math).
void testW2xFnString();  // selftest_w2x_fnstring.cpp
void testW2zTostring();  // selftest_w2z_tostring.cpp

// COMMON_GE2 graphics: lpoly/lowess (goal gfx-lpoly-lowess).
void testW2Lpoly();  // selftest_w2_lpoly.cpp
void testW2DisplayFormat();  // selftest_w2_display_format.cpp (W2D-T03)
void testW2afFormat();  // selftest_w2af_format.cpp (format-display-default-lifecycle)
void testW2AhDataVerbs();  // selftest_w2ah_dataverbs.cpp (data-verbs-effects-rename-recode-label)

// eb-order T-P3 + T-P1: one coefficient source, one canonical e(b)/e(V)
// (goal eb-order-canonical-ematrix).
void testW2Ebmat();  // selftest_w2_ebmat.cpp

// AER tier-1 (2026-10-28): one TU per lane, against w2ax_t1_{official,community}.log.
void testW2T1Zip();     // selftest_w2_t1_zip.cpp
void testW2T1Env();     // selftest_w2_t1_env.cpp
void testW2T1Meta();    // selftest_w2_t1_meta.cpp
void testW2T1Gen();     // selftest_w2_t1_gen.cpp
void testW2T1Matrix();  // selftest_w2_t1_matrix.cpp

// binscatter (Stepner 7.02) against w2k_binscatter.log (goal gfx-binscatter).
void testW2GfxBinscatter();  // selftest_w2_gfxbinscatter.cpp
void testW2GfxBar();  // selftest_w2_gfxbar.cpp (graph-family AI-1/AI-4, coefplot-family AN-1)

// egen's statistics family against w2ap_egen_family.log (goal data-egen-stat-family).
void testW2apEgen();  // selftest_w2ap_egen.cpp

// local/global/scalar = var[i] and bare variables against w2i_varsub.log
// (goal varsub-assign-from-variable-bug710-v2; BUG-710).
void testW2iVarsub();  // selftest_w2i_varsub.cpp
// histogram/hist, twoway__histogram_gen and kdensity against the
// w2aq_histogram_family capture (goal gfx-histogram-kdensity-family).
void testW2aqHistogram();  // selftest_w2aq_histogram.cpp

// Package-007 unblock (T7): one TU per new command.
void testW2T7Lookfor();  // selftest_w2_t7_lookfor.cpp
void testW2T7Ihstrans();  // selftest_w2_t7_ihstrans.cpp
void testW2T7Sctorezone();  // selftest_w2_t7_sctorezone.cpp
void testW2LbLeebounds();  // selftest_w2_lb_leebounds.cpp (package-007 lane LB)
void testW2T7ClearAll();  // selftest_w2_t7_clearall.cpp

// Unicode names (2026-10-28): unit A core + one TU per lane.
void testW2UniCore();  // selftest_w2_uni_core.cpp
void testW2UniV();  // selftest_w2_uni_v.cpp (validators / I/O)
void testW2UniM();  // selftest_w2_uni_m.cpp (macro / expression lexers)
void testW2UniD();  // selftest_w2_uni_d.cpp (dispatcher / misc scanners)
void testW2UniW();  // selftest_w2_uni_w.cpp (display widths)

// BUG-1067: reghdfe/areg/xtreg,fe keep the earlier member of a collinear set.
void testW2C1067();  // selftest_w2_c1067.cpp
