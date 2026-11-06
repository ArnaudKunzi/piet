// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
#pragma once

// Prototypes for every cobra subcommand handler (99 cmd_* functions), formerly
// static/anonymous-namespace bodies inside the single apps/cecon_cli.cpp monolith,
// now split across the themed apps/cmds/cmds_*.cpp translation units below. Pure
// declarations — external linkage lives in namespace cecon::cli so main()'s
// dispatch if-chain in cecon_cli.cpp (via `using namespace cecon::cli;`) can still
// call them unqualified, byte-identical to before.
//
//   cmds_linear.cpp   : ols, iv, feols, fepois, liml, wildboot, reg3
//   cmds_panel.cpp    : xtreg, xtpois, xtlogit, nbreg, csdid
//   cmds_mle.cpp      : logit, probit, tobit, heckman, qreg
//   cmds_postest.cpp  : test, chow, lincom, nlcom, mediate, margins, predict, hausman
//   cmds_diag.cpp     : hettest, ovtest, dwatson, durbinalt, bgodfrey, archlm, ivdiag, vif
//   cmds_ts.cpp       : newey, prais, tsops, dfuller, pperron, tsfill, wntestq
//   cmds_summary.cpp  : corr, sdtest, ttest, tabstat, tabulate, codebook, collapse
//   cmds_survey.cpp   : mean, proportion (P3 answerbook wave — pweight-only survey estimators)
//   cmds_data.cpp     : eval, gsort, order, rename, clonevar, append, joinby, merge,
//                       duplicates, recode, encode, setcol, reshape, convert, import_dta,
//                       run, grid, info, version
//   cmds_rd.cpp           : rdrobust, rddensity, rdplot (P-QE wave)
//   cmds_eventstudy.cpp   : eventstudyinteract, did_imputation (P-QE wave)
//   cmds_sdid.cpp         : sdid (P-QE wave)
//   cmds_binscatter.cpp   : binscatter (P-QE wave)

#include <string>
#include <vector>

namespace cecon {
namespace cli {

// cmds_linear.cpp
int cmd_ols(const std::vector<std::string>& a);
int cmd_iv(const std::vector<std::string>& a);
int cmd_feols(const std::vector<std::string>& a);
int cmd_fepois(const std::vector<std::string>& a);
int cmd_liml(const std::vector<std::string>& a);
int cmd_wildboot(const std::vector<std::string>& a);
int cmd_reg3(const std::vector<std::string>& a);

// cmds_panel.cpp
int cmd_xtreg(const std::vector<std::string>& a);
int cmd_xtpois(const std::vector<std::string>& a);
int cmd_xtlogit(const std::vector<std::string>& a);
int cmd_nbreg(const std::vector<std::string>& a);
int cmd_csdid(const std::vector<std::string>& a);

// cmds_mle.cpp
int cmd_logit(const std::vector<std::string>& a);
int cmd_probit(const std::vector<std::string>& a);
int cmd_cloglog(const std::vector<std::string>& a);
int cmd_tobit(const std::vector<std::string>& a);

// --- P7 wave -------------------------------------------------------------
int cmd_ologit(const std::vector<std::string>& a);
int cmd_oprobit(const std::vector<std::string>& a);
int cmd_mlogit(const std::vector<std::string>& a);
int cmd_zip(const std::vector<std::string>& a);
int cmd_zinb(const std::vector<std::string>& a);
int cmd_fracreg(const std::vector<std::string>& a);
int cmd_ivprobit(const std::vector<std::string>& a);
int cmd_ivtobit(const std::vector<std::string>& a);
int cmd_mixed(const std::vector<std::string>& a);
int cmd_stset(const std::vector<std::string>& a);
int cmd_stcox(const std::vector<std::string>& a);
int cmd_streg(const std::vector<std::string>& a);
int cmd_var(const std::vector<std::string>& a);
int cmd_xtabond(const std::vector<std::string>& a);
int cmd_nl(const std::vector<std::string>& a);
int cmd_gmm(const std::vector<std::string>& a);
int cmd_mi_impute_regress(const std::vector<std::string>& a);
int cmd_heckman(const std::vector<std::string>& a);
int cmd_qreg(const std::vector<std::string>& a);

// cmds_postest.cpp
int cmd_test(const std::vector<std::string>& a);
int cmd_chow(const std::vector<std::string>& a);
int cmd_lincom(const std::vector<std::string>& a);
int cmd_nlcom(const std::vector<std::string>& a);
int cmd_mediate(const std::vector<std::string>& a);
int cmd_margins(const std::vector<std::string>& a);
int cmd_predict(const std::vector<std::string>& a);
int cmd_hausman(const std::vector<std::string>& a);

// cmds_diag.cpp
int cmd_hettest(const std::vector<std::string>& a);
int cmd_ovtest(const std::vector<std::string>& a);
int cmd_dwatson(const std::vector<std::string>& a);
int cmd_durbinalt(const std::vector<std::string>& a);
int cmd_bgodfrey(const std::vector<std::string>& a);
int cmd_archlm(const std::vector<std::string>& a);
int cmd_ivdiag(const std::vector<std::string>& a);
int cmd_vif(const std::vector<std::string>& a);

// cmds_ts.cpp
int cmd_newey(const std::vector<std::string>& a);
int cmd_prais(const std::vector<std::string>& a);
int cmd_tsops(const std::vector<std::string>& a);
int cmd_dfuller(const std::vector<std::string>& a);
int cmd_pperron(const std::vector<std::string>& a);
int cmd_tsfill(const std::vector<std::string>& a);
int cmd_wntestq(const std::vector<std::string>& a);

// cmds_summary.cpp
int cmd_corr(const std::vector<std::string>& a);
int cmd_sdtest(const std::vector<std::string>& a);
int cmd_ttest(const std::vector<std::string>& a);
int cmd_tabstat(const std::vector<std::string>& a);
int cmd_tabulate(const std::vector<std::string>& a);
int cmd_codebook(const std::vector<std::string>& a);
int cmd_collapse(const std::vector<std::string>& a);

// cmds_survey.cpp
int cmd_mean(const std::vector<std::string>& a);
int cmd_proportion(const std::vector<std::string>& a);

// cmds_data.cpp
int cmd_eval(const std::vector<std::string>& a);
int cmd_gsort(const std::vector<std::string>& a);
int cmd_order(const std::vector<std::string>& a);
int cmd_rename(const std::vector<std::string>& a);
int cmd_clonevar(const std::vector<std::string>& a);
int cmd_append(const std::vector<std::string>& a);
int cmd_joinby(const std::vector<std::string>& a);
// P1 wave data verbs — Stata expand / fillin / cross / contract.
int cmd_expand(const std::vector<std::string>& a);
int cmd_fillin(const std::vector<std::string>& a);
int cmd_cross(const std::vector<std::string>& a);
int cmd_contract(const std::vector<std::string>& a);
int cmd_merge(const std::vector<std::string>& a);
int cmd_duplicates(const std::vector<std::string>& a);
int cmd_recode(const std::vector<std::string>& a);
int cmd_encode(const std::vector<std::string>& a);
int cmd_setcol(const std::vector<std::string>& a);
int cmd_reshape(const std::vector<std::string>& a);
int cmd_convert(const std::vector<std::string>& a);
int cmd_import_dta(const std::vector<std::string>& a);
int cmd_export_dta(const std::vector<std::string>& a);
int cmd_datasignature(const std::vector<std::string>& a);
int cmd_run(const std::vector<std::string>& a);
int cmd_grid(const std::vector<std::string>& a);
int cmd_info(const std::string& path);
int cmd_version(const std::vector<std::string>& a);

// options_table.cpp — B.0.3 `cobra options [--json]` (accepted-flag contract dump)
int cmd_options(const std::vector<std::string>& a);
// options_table.cpp — `cobra functions [--egen] [--json]` (accepted-function dump).
// The read-only companion to `options`: what the expression and egen parsers accept,
// answered from the tables those parsers resolve against.
int cmd_functions(const std::vector<std::string>& a);

// The egen vocabulary, defined beside cmd_eval's parser in cmds_data.cpp (see the
// table comment there). One row per accepted spelling, ordered expr, then varlist,
// then row. `kind` is the shape of the function's INPUT: "expr" | "varlist" | "row".
struct EgenFnInfo { const char* name; const char* kind; };
const std::vector<EgenFnInfo>& egen_function_table();

// --- P-QE wave -------------------------------------------------------------

// cmds_rd.cpp
int cmd_rdrobust(const std::vector<std::string>& a);
int cmd_rddensity(const std::vector<std::string>& a);
int cmd_rdplot(const std::vector<std::string>& a);

// cmds_eventstudy.cpp
int cmd_eventstudyinteract(const std::vector<std::string>& a);
int cmd_did_imputation(const std::vector<std::string>& a);

// cmds_sdid.cpp
int cmd_sdid(const std::vector<std::string>& a);

// cmds_binscatter.cpp
int cmd_binscatter(const std::vector<std::string>& a);

// cmds_aer_estim.cpp — AER bundle H1
int cmd_hpfilter(const std::vector<std::string>& a);
int cmd_lpoly(const std::vector<std::string>& a);
int cmd_lowess(const std::vector<std::string>& a);
int cmd_oaxaca(const std::vector<std::string>& a);
int cmd_nnmatch(const std::vector<std::string>& a);
int cmd_pca(const std::vector<std::string>& a);

}  // namespace cli
}  // namespace cecon
