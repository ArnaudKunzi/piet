// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
#include "common.h"
#include "commands.h"

#include "estimators/ols.h"
#include "estimators/liml.h"
#include "estimators/reg3.h"
#include "inference/wildboot.h"
// feols.h/iv2sls.h/fepois.h: cmd_feols/cmd_iv/cmd_fepois below only call the
// shared run_feols/run_iv/run_fepois(_multi) wrappers (apps/cmds/common.cpp),
// which return plain json — these three headers are not strictly required for
// THIS file to compile, but are included defensively since they're the natural
// estimator family for ols/iv/feols/fepois/liml/wildboot/reg3.
#include "estimators/feols.h"
#include "estimators/iv2sls.h"
#include "estimators/fepois.h"

namespace cecon {
namespace cli {

int cmd_ols(const std::vector<std::string>& a) {
    std::string data = opt(a, "--data"), yname = opt(a, "--y"), xcsv = opt(a, "--x");
    if (data_missing(data) || yname.empty() || xcsv.empty()) {
        std::fprintf(stderr, "usage: cobra ols --data f.hbin --y Y --x X1,X2 [--no-intercept] [--out f.json]\n");
        return 2;
    }
    bool intercept = !has(a, "--no-intercept");
    // e(sample) T1a: build_design keeps its listwise row set private (core/matrix.cpp),
    // so there is no admitted-row list to emit. Refuse by name rather than write a
    // mask recomputed outside the fit; feols fits the same regression and emits it.
    if (esample_request(a, "ols").active())
        throw EngineError("unsupported",
            "ols: --esample-out is not supported (the design builder does not expose its "
            "kept rows); use feols, which fits the same model and emits them",
            "--esample-out");
    const Dataset& ds = acquire_data(data);
    DesignMatrix dm = build_design(ds, yname, split_csv(xcsv), intercept);
    OlsResult r = ols_fit(dm);
    json coef, sec, seh, th;
    for (int j = 0; j < r.k; ++j) {
        coef[r.names[j]] = r.beta[j];
        sec[r.names[j]] = r.se_classic[j];
        seh[r.names[j]] = r.se_hc1[j];
        th[r.names[j]] = tstat(r.beta[j], r.se_hc1[j]);
    }
    json out = {{"estimator", "ols"},  {"depvar", yname}, {"n_obs", r.n},
                {"n_params", r.k},     {"n_dropped", r.n_dropped},
                {"r2", r.r2},          {"sigma2", r.sigma2},
                {"coef", coef},        {"se_classic", sec},
                {"se_hc1", seh},       {"tstat_hc1", th}};
    // p-values + 95% CI use HC1 SE and residual dof (Stata regress, robust).
    add_inference(out, r.names, r.beta, r.se_hc1, static_cast<double>(r.n - r.k));
    add_omitted(out, r.omitted, r.full_names);
    emit(out, opt(a, "--out"));
    return 0;
}

int cmd_iv(const std::vector<std::string>& a) {
    std::string data = opt(a, "--data"), y = opt(a, "--y");
    std::string endog = opt(a, "--endog"), z = opt(a, "--instruments", opt(a, "--z"));
    if (data_missing(data) || y.empty() || endog.empty() || z.empty()) {
        std::fprintf(stderr,
            "usage: cobra iv --data f.hbin --y Y[,Y2,...] --endog D --instruments Z1,Z2\n"
            "                [--x EXOG] [--fe F1,F2] [--vcov iid|hc1|cluster|twoway] [--cluster C]\n"
            "                [--gmm] [--small] [--keep-singletons] [--keepif EXPR] [--out f.json]\n"
            "                [--savefirst [--savefprefix P]] [--weight W --wtype aw|pw]\n"
            "                [--esample-out PATH [--esample-index COL --esample-n N]]\n"
            "  --gmm        efficient two-step GMM (Stata `ivregress gmm`): robust weight matrix +\n"
            "               robust SEs by default; over-identified GMM differs from 2SLS. With\n"
            "               --vcov cluster, uses the cluster-robust GMM weight matrix + SEs.\n"
            "  --small      Stata `ivregress ..., small`: restores the N-K small-sample sigma2 /\n"
            "               HC1 / cluster factors and t/F inference. Default (no --small) matches\n"
            "               `ivregress`'s own default: asymptotic sigma2=RSS/N, HC0 robust SEs, no\n"
            "               cluster small-sample factor, and z/chi2 inference. Ignored with --gmm\n"
            "               (GMM's SEs are always the asymptotic sandwich).\n"
            "  --y          one outcome, or a comma list to batch several outcomes that share the\n"
            "               SAME endog/instruments/FE design (FE absorption + first-stage done once).\n"
            "  --endog      comma list of endogenous regressors.\n"
            "  --instruments (alias --z) excluded instruments; need >= #endog (over-id => Hansen J).\n"
            "  --x          exogenous controls; supports i(col)/i.col factor expansion and the Stata\n"
            "               factor operators ib#. / ibn. / i(numlist). / o. (e.g. --x \"ib2.tb,rh\").\n"
            "  --fe         high-dimensional fixed effects to absorb (FWL within transform).\n"
            "  --vcov       iid (default) | hc1 | cluster (needs --cluster) | twoway.\n"
            "  --savefirst  emit each endogenous regressor's FIRST-STAGE OLS (ivreg2 `savefirst`),\n"
            "               so a consumer can register it as a stored estimate: JSON key `first_stage`\n"
            "               (keyed by endogenous regressor) plus `firsteqs` (the ivreg2 equation names).\n"
            "               The stage is `regress ENDOG EXCLUDED_IVS EXOG` under the PARENT's vce with\n"
            "               the small-sample correction ALWAYS on, so --small does not move it.\n"
            "  --savefprefix P  prefix for those names (ivreg2 savefprefix()); default `_ivreg2_`.\n"
            "  --weight W --wtype aw|pw  ivreg2's observation weights: wf*W with wf = N/sum(W),\n"
            "               so N stays the row count; pw additionally needs --vcov hc1, cluster or\n"
            "               twoway (ivreg2: pweight = aweight + robust). A missing or zero weight\n"
            "               drops the row. With --fe the within transform is weighted (group means\n"
            "               sum(W*v)/sum(W), xtivreg2's). A multi-outcome --y weights every\n"
            "               outcome alike.\n"
            "               With --savefirst each first stage also carries `ffirst`, ivreg2's\n"
            "               e(first) column (rmse sheapr2 pr2 F df df_r pvalue SW... AP...).\n"
            "  --dofminus-fe  xtivreg2's ivreg2 call (dofminus = the absorbed groups, needs --fe):\n"
            "               sigma2 = RSS/(N-dofminus), robust meat * N/(N-dofminus), cluster\n"
            "               meat untouched, rmse on that scale; --small's cluster K is the\n"
            "               regressors alone; ffirst carries the same dofminus. Every panel must sit\n"
            "               inside one cluster (else code cluster_spans_panel). Not with --gmm,\n"
            "               twoway/multiway or a multi-outcome --y.\n"
            "  --multiway-no-psd-fix  ivreg2's two-way variance S1+S2-S12 as summed: a negative\n"
            "               eigenvalue is not repaired, so a negative variance is a missing SE.\n"
            "  --esample-out PATH  write e(sample): one 1/0 line per --data row (before --keepif),\n"
            "               1 = the row entered the fit; the JSON gains esample_n (# of 1s).\n"
            "               --esample-index COL --esample-n N: COL is each row's 1-based position\n"
            "               in an N-row frame; the file then has N lines, scattered through COL.\n"
            "  Diagnostics: first_stage_F[_iid] (homoskedastic), kp_wald_F (robust/cluster, e(widstat)),\n"
            "               eff_F (Montiel-Olea-Pflueger), kp_lm (under-id), hansen_j (over-id).\n"
            "  NOTE: iv DROPS singleton FE groups by default (matching feols/fepois/reghdfe/ivreghdfe;\n"
            "        Correia 2015). Pass --keep-singletons to retain them (changes N / residual DoF /\n"
            "        clustered SEs).\n"
            "  Example: cobra iv --data p.hbin --y lncardio,lnresp --endog lnpm --instruments q1,q2,q3 \\\n"
            "                    --x \"i(tbin),rh\" --fe sgg,ym --vcov cluster --cluster sgg\n");
        return 2;
    }
    std::vector<std::string> clusters = split_csv(opt(a, "--cluster"));
    std::vector<std::string> xs = split_csv(opt(a, "--x"));
    std::vector<std::string> endogs = split_csv(endog);
    std::vector<std::string> insts = split_csv(z);
    std::vector<std::string> ys = split_csv(y);
    bool gmm = has(a, "--gmm");
    bool small = has(a, "--small");  // Stata `ivregress ..., small`; default = asymptotic
    // --no-intercept: Stata `ivregress ..., noconstant` (w2d_uncovered.log
    // ivregress_nocons_ctl). Absorbed effects already exclude the constant, and a
    // batch has no slot for it: both refused.
    const bool no_intercept = has(a, "--no-intercept");
    if (no_intercept && (!split_csv(opt(a, "--fe")).empty() || ys.size() > 1))
        throw EngineError("bad_input",
            "iv: --no-intercept is not valid with --fe or a multi-outcome --y");
    // ivreg2 `savefirst [savefprefix(P)]`. The prefix is data, not a flag: an empty
    // one is the DOCUMENTED default `_ivreg2_`, so it is passed through as "" and
    // resolved where the name is minted (ivreg2_first_eqname in common.cpp).
    bool savefirst = has(a, "--savefirst");
    std::string savefprefix = opt(a, "--savefprefix");
    if (!savefprefix.empty() && !savefirst)
        throw EngineError("bad_input", "iv: --savefprefix requires --savefirst");
    bool drop_singleton = !has(a, "--keep-singletons");  // default drop, matches feols/ivreghdfe
    // Stata `ivregress gmm` defaults to a robust weight matrix + robust SEs. When the
    // user asks for --gmm without an explicit --vcov, default to hc1 so the SEs match.
    std::string vcov = opt(a, "--vcov", gmm ? "hc1" : "iid");
    // FU-387: Stata ivregress's `small` multiway variance (the built-in convention's
    // per-combination factors). Without --small ivregress
    // applies no factor at all, so the switch would be silently ignored there, and it
    // has no measured meaning for gmm, absorbed effects or a batch: all refused.
    const bool multiway_subset_factors = has(a, "--multiway-subset-factors");
    if (multiway_subset_factors &&
        (!small || gmm || (vcov != "twoway" && vcov != "multiway") ||
         !split_csv(opt(a, "--fe")).empty() || ys.size() > 1))
        throw EngineError("bad_input",
            "iv: --multiway-subset-factors requires --small and --vcov twoway or multiway, "
            "without --gmm, --fe or a multi-outcome --y (Stata ivregress's small multiway "
            "variance)");
    cecon::TsMeta tsm = parse_tsmeta(a);
    const EsampleRequest esreq = esample_request(a, "iv");
    EsampleRows esrows;
    EsampleRows* es_p = esreq.active() ? &esrows : nullptr;
    // E1: --x / --endog / --instruments may all carry L./F./D./S. tokens (see
    // build_iv_model's ts_prepass calls) — route `if` through plan_keepif so they
    // materialize against the FULL series before the sample filter applies. So may
    // --y (BUG-1277).
    const std::string keepif = opt(a, "--keepif");
    KeepifPlan kp = plan_keepif(data, keepif, &tsm, {&xs, &endogs, &insts, &ys});
    const std::vector<char>* mask_p = kp.active ? &kp.mask : nullptr;
    // --emit-v: opt-in full e(V) of the fitted model (see add_e_v). A batch has one V per
    // outcome and no per-outcome slot for it, so it is refused rather than ignored.
    const bool emit_v = has(a, "--emit-v");
    if (emit_v && ys.size() > 1)
        throw EngineError("unsupported",
            "iv: --emit-v is not supported with a multi-outcome --y batch; run one outcome per invocation");
    // ivreg2's two-way variance is posted unrepaired (compute_vcov no_psd_fix); only
    // meaningful under a multiway VCE without the subset-factor convention.
    const bool multiway_no_psd_fix = has(a, "--multiway-no-psd-fix");
    if (multiway_no_psd_fix && ((vcov != "twoway" && vcov != "multiway") || multiway_subset_factors ||
                                ys.size() > 1))
        throw EngineError("bad_input",
            "iv: --multiway-no-psd-fix requires --vcov twoway or multiway, without "
            "--multiway-subset-factors or a multi-outcome --y");
    const std::string weight_var = opt(a, "--weight"), wtype = opt(a, "--wtype");
    cecon::WeightKind wkind = cecon::WeightKind::None;
    if (!weight_var.empty() || !wtype.empty()) {
        if (weight_var.empty()) throw EngineError("bad_input", "iv: --wtype requires --weight");
        if (wtype.empty() || wtype == "aw" || wtype == "aweight") wkind = cecon::WeightKind::Analytic;
        else if (wtype == "pw" || wtype == "pweight") wkind = cecon::WeightKind::Probability;
        else
            throw EngineError("bad_input",
                "iv: --wtype must be aw or pw (got '" + wtype + "'); fweight and iweight are not "
                "implemented for iv");
        if (wkind == cecon::WeightKind::Probability && vcov != "hc1" && vcov != "HC1" &&
            vcov != "cluster" && vcov != "twoway" && vcov != "multiway")
            throw EngineError("bad_input",
                "iv: [pweight=] requires --vcov hc1, cluster or twoway (ivreg2 makes pweight robust)");
    }
    const bool dofminus_fe = has(a, "--dofminus-fe");
    if (dofminus_fe && (split_csv(opt(a, "--fe")).empty() || gmm || ys.size() > 1 ||
                        (vcov != "iid" && vcov != "hc1" && vcov != "HC1" && vcov != "cluster")))
        throw EngineError("bad_input",
            "iv: --dofminus-fe requires --fe and --vcov iid, hc1 or cluster, without --gmm or a "
            "multi-outcome --y");
    json out;
    if (ys.size() > 1)
        out = run_iv_multi(kp.data, ys, xs, endogs, insts, split_csv(opt(a, "--fe")), vcov,
                           clusters, gmm, &tsm, drop_singleton, mask_p, small, savefirst,
                           savefprefix, es_p, weight_var, wkind);
    else
        out = run_iv(kp.data, y, xs, endogs, insts, split_csv(opt(a, "--fe")), vcov, clusters,
                     gmm, &tsm, drop_singleton, mask_p, small, savefirst, savefprefix, es_p,
                     emit_v, multiway_subset_factors, no_intercept, weight_var, wkind,
                     multiway_no_psd_fix, dofminus_fe);
    emit_esample(esreq, "iv", data, keepif, kp.active, esrows, out);
    emit(out, opt(a, "--out"));
    return 0;
}

int cmd_feols(const std::vector<std::string>& a) {
    std::string data = opt(a, "--data"), y = opt(a, "--y"), xcsv = opt(a, "--x");
    if (data_missing(data) || y.empty() || xcsv.empty()) {
        std::fprintf(stderr, "usage: cobra feols --data f.hbin --y Y[,Y2,...] --x X1,X2 "
                             "[--fe F1,F2] [--vcov iid|hc1|cluster] [--cluster C] "
                             "[--no-intercept] [--level P] [--weight W --wtype aw|fw|iw|pw] "
                             "[--keepif EXPR] [--esample-out PATH [--esample-index COL --esample-n N]] "
                             "[--out f.json]\n"
                             "  --x terms: plain numeric, i(col)/i.col factor indicators (base=lowest dropped),\n"
                             "    c(col) continuous, and interactions joined by # / ## (Stata factor vars):\n"
                             "      i(treat)##i(post)  DID main effects + cross   c(x)##c(x)  x and x^2\n"
                             "      i(g)##c(x)  group slopes      i(a)#i(b)  interaction only (full cross - base cell)\n"
                             "  Stata factor operators (parity):\n"
                             "      ib#.col   custom base level # (e.g. ib2.rep78); i may be omitted (b2.rep78)\n"
                             "      ibn.col   no base requested (last level auto-dropped for collinearity)\n"
                             "      i(2/4).col / i(2 4).col  only those levels (lowest selected = base);\n"
                             "                obs OUTSIDE the set fold into the reference group (NOT dropped)\n"
                             "      o.col     omit operator: contributes no columns/dof (\"controlled for\" label)\n"
                             "    (cells must be populated; empty-cell unbalanced factor x factor may differ from Stata.)\n"
                             "  --no-intercept  Stata `regress ..., noconstant`; rejected together with --fe\n"
                             "                  (fixed effects already absorb the constant).\n"
                             "  --level P       confidence level for ci_lo/ci_hi, 10 < P <= 99.99 (default 95).\n"
                             "  --weight W --wtype T  Stata estimation weights [aw=]/[fw=]/[iw=]/[pw=].\n"
                             "                  aw/pw report N = rows; fw/iw report N = sum(W). pw additionally\n"
                             "                  makes --vcov hc1/cluster the linearized (survey) sandwich.\n"
                             "                  With --fe (aw/fw/pw only) the cells are demeaned with the weights.\n"
                             "                  pw requires --vcov hc1, cluster or twoway (Stata has no iid VCE\n"
                             "                  under pweight).\n"
                             "  --esample-out PATH  write e(sample): one 1/0 line per --data row (before\n"
                             "                  --keepif), 1 = the row entered the fit (after listwise, weight,\n"
                             "                  cluster, TS-operator and singleton losses); the JSON gains\n"
                             "                  esample_n = # of 1s (== n_obs; == n_rows under fw/iw).\n"
                             "  --esample-index COL --esample-n N  COL is each loaded row's 1-based position\n"
                             "                  in an N-row frame; the file then has N lines, scattered via COL.\n"
                             "  --keep-nested-fe-dof  Stata `areg` clustered dof: the cluster small-sample\n"
                             "                  factor charges every absorbed FE, even one nested in the\n"
                             "                  cluster (default: reghdfe/xtreg,fe exempt it). Needs --fe and\n"
                             "                  --vcov cluster|twoway.\n"
                             "  --multiway-subset-factors  Stata `regress`/`areg` multiway cluster: every\n"
                             "                  cluster combination's sandwich carries its own G/(G-1)*(N-1)/(N-K)\n"
                             "                  (K charges every absorbed FE), and negative eigenvalues of the sum\n"
                             "                  are set to 0 (default: reghdfe's single Gmin factor). Needs\n"
                             "                  --vcov twoway|multiway. K follows --keep-nested-fe-dof (areg)\n"
                             "                  or exempts an FE nested in a cluster (xtreg,fe).\n"
                             "  --fe-nested-in-clusters  Stata `xtreg, fe` cluster rule: every --cluster\n"
                             "                  variable must be constant within each level of the first\n"
                             "                  --fe on the estimation sample; else bad_input with detail\n"
                             "                  panels_not_nested (Stata r(498)). Needs --fe and a cluster VCE.\n"
                             "  --regress-exact-fit  Stata `regress`/`areg` exact-fit rule: e(r2) > 1 - 1e-14\n"
                             "                  is an exact fit (rss, V and se 0; t, p and the CI null).\n"
                             "                  --vcov iid only; aw/fw/iw and --fe allowed, pw refused\n"
                             "                  (reghdfe keeps the computed RSS).\n"
                             "  --collinear-forward  Stata reghdfe/areg/xtreg,fe collinearity rule: of a\n"
                             "                  collinear set the EARLIER term is kept and the later one\n"
                             "                  omitted (default: the norm-pivoted basis of `regress`).\n");
        return 2;
    }
    std::string vcov_type = opt(a, "--vcov", "iid");
    std::vector<std::string> clusters = split_csv(opt(a, "--cluster"));
    std::vector<std::string> xs = split_csv(xcsv);
    // regress-constant-only: `regress y [if]` (w2j_e3_builtin.log:288/:310/:332 F_C1-F_C3,
    // rc 0) arrives as `--x (intercept)`: the constant is the design's own intercept
    // column, so the token names no regressor and the x list is empty.
    xs.erase(std::remove(xs.begin(), xs.end(), std::string("(intercept)")), xs.end());
    std::vector<std::string> fes = split_csv(opt(a, "--fe"));
    bool drop_singleton = !has(a, "--keep-singletons");
    bool no_intercept = has(a, "--no-intercept");
    if (xs.empty() && no_intercept) {
        std::fprintf(stderr, "feols: --x (intercept) with --no-intercept leaves no regressor.\n");
        return 2;
    }
    if (no_intercept && !fes.empty()) {
        std::fprintf(stderr, "feols: --no-intercept is not valid with --fe "
                             "(fixed effects already absorb the constant).\n");
        return 2;
    }
    bool level_set = has(a, "--level");
    double level = std::stod(opt(a, "--level", "95"));
    if (level_set && (level < 10.0 || level > 99.99)) {
        std::fprintf(stderr, "feols: --level must be in [10, 99.99] (got %s).\n",
                     opt(a, "--level").c_str());
        return 2;
    }
    // Stata weights. --wtype names the KIND, which is the whole difference between
    // them (see WeightKind in core/model.h); an unrecognized spelling is rejected
    // rather than silently treated as one of the four, because picking the wrong
    // kind changes N, the dof and every SE without changing β — the exact shape of
    // a silently wrong answer.
    std::string weight_var = opt(a, "--weight"), wtype = opt(a, "--wtype");
    cecon::WeightKind wkind = cecon::WeightKind::None;
    if (!weight_var.empty() || !wtype.empty()) {
        if (weight_var.empty())
            throw EngineError("bad_input", "feols: --wtype requires --weight");
        if (wtype.empty() || wtype == "aw" || wtype == "aweight") wkind = cecon::WeightKind::Analytic;
        else if (wtype == "fw" || wtype == "fweight") wkind = cecon::WeightKind::Frequency;
        else if (wtype == "iw" || wtype == "iweight") wkind = cecon::WeightKind::Importance;
        else if (wtype == "pw" || wtype == "pweight") wkind = cecon::WeightKind::Probability;
        else
            throw EngineError("bad_input",
                "feols: --wtype must be one of aw|fw|iw|pw (got '" + wtype + "')");
        // Weighted absorption is implemented (feols_fit demeans with the weights).
        // pweight has no iid VCE in Stata: anything run_feols would fit as iid (the
        // default, or a spelling it does not map to a sandwich) is refused rather
        // than returning a conventional SE Stata never reports -- with or without
        // --fe. The no-FE half closes the silent-IID class at the engine boundary:
        // PIET's regress (runRegress) and the shared M4 builder push `--vcov hc1`
        // for a bare pweight (BUG-536, w2_pweight.log R2_pw == R3), and no PIET
        // post-estimation replay forwards `--wtype pw` to feols without the fit's
        // vcov, so the only caller left fitting pw as iid was a silent wrong answer.
        // `--svy` is exempt: it replaces the VCE entirely (and refuses --vcov).
        // iweight + --fe stays refused: Stata's absorbing estimators do not take
        // iweights (w2_pweight.log X11_iw_fe: xtreg [iweight=], fe is r(101)), so
        // fitting one would answer a model Stata refuses.
        if (wkind == cecon::WeightKind::Importance && !fes.empty())
            throw EngineError("bad_input",
                "feols: [iweight=] is not allowed with --fe (Stata's areg/reghdfe/xtreg,fe "
                "take aweight, fweight and pweight only)");
        const bool sandwich = vcov_type == "hc1" || vcov_type == "HC1" ||
                              vcov_type == "cluster" || vcov_type == "twoway" ||
                              vcov_type == "multiway";
        if (wkind == cecon::WeightKind::Probability && !sandwich && !has(a, "--svy"))
            throw EngineError("bad_input",
                "feols: [pweight=] requires --vcov hc1, cluster or twoway; Stata has no "
                "iid VCE under pweight");
    }
    // BUG-568: Stata areg's clustered dof. areg charges every absorbed parameter in
    // the cluster (N-1)/(N-K) factor, including an FE nested in the cluster, where
    // reghdfe and xtreg,fe (this command's default) exempt it: w2b_estim.log
    // areg_a_cl se[x1] .0722686736 vs reghdfe_cluster_ctl .0663182832 on the same
    // fit. Only meaningful with a cluster VCE and absorbed FE; anywhere else the
    // flag would be silently ignored, so it is refused instead.
    const bool keep_nested_fe_dof = has(a, "--keep-nested-fe-dof");
    if (keep_nested_fe_dof) {
        const bool cluster_vce = vcov_type == "cluster" || vcov_type == "twoway" ||
                                 vcov_type == "multiway";
        if (!cluster_vce || fes.empty())
            throw EngineError("bad_input",
                "feols: --keep-nested-fe-dof requires --fe and --vcov cluster or twoway (it "
                "selects which absorbed parameters the cluster small-sample factor charges)");
    }
    // FU-365: Stata's built-in multiway cluster convention (regress/areg), against
    // reghdfe's Gmin factor (this command's default): w2l_vce_shape_det.log
    // regress_vce_c2_se .0643525900770342 and areg_vce_c2_se .0778721308909151 vs
    // reghdfe_vce_c2_se .0711126484294995. Only a multiway VCE reads it; anywhere
    // else it would be silently ignored, so it is refused instead.
    const bool multiway_subset_factors = has(a, "--multiway-subset-factors");
    if (multiway_subset_factors && vcov_type != "twoway" && vcov_type != "multiway")
        throw EngineError("bad_input",
            "feols: --multiway-subset-factors requires --vcov twoway or multiway (it selects "
            "the small-sample convention of a multiway cluster variance)");
    // FU-401/FU-407: xtreg,fe's panels-nested-in-clusters rule (run_feols). Only
    // meaningful with absorbed effects and a cluster VCE; refused elsewhere.
    const bool fe_nested_in_clusters = has(a, "--fe-nested-in-clusters");
    if (fe_nested_in_clusters &&
        (fes.empty() || (vcov_type != "cluster" && vcov_type != "twoway" && vcov_type != "multiway")))
        throw EngineError("bad_input",
            "feols: --fe-nested-in-clusters requires --fe and --vcov cluster, twoway or multiway");
    // Stata `regress`'s exact-fit rule (feols_fit): only the builtin fit has it
    // (reghdfe keeps the computed RSS) -- regress, with or without [aw]/[fw]/[iw], and
    // areg's absorbed fit (BUG-1055) -- and it was probed under the classic VCE only.
    // Anywhere else (a pweight's robust VCE, any sandwich, svy) it would be silently
    // ignored, so it is refused instead.
    const bool regress_exact_fit = has(a, "--regress-exact-fit");
    if (regress_exact_fit &&
        (wkind == cecon::WeightKind::Probability || has(a, "--svy") ||
         !(vcov_type == "iid" || vcov_type.empty())))
        throw EngineError("bad_input",
            "feols: --regress-exact-fit requires --vcov iid without --svy or a pweight (it "
            "selects Stata regress/areg's exact-fit rule for the classic variance)");
    // BUG-1067: which member of a collinear set is omitted. reghdfe, areg and
    // xtreg,fe keep the earlier term (StataBE, z = 2*x: `x o.z`), where regress
    // keeps the norm-pivoted basis this command defaults to (`o.x z`). areg carries
    // both this and the exact-fit rule (BUG-1055), so only a fit without --fe refuses
    // the pair.
    const bool collinear_forward = has(a, "--collinear-forward");
    if (collinear_forward && ((regress_exact_fit && fes.empty()) || has(a, "--svy")))
        throw EngineError("bad_input",
            "feols: --collinear-forward is not valid with --svy, or with --regress-exact-fit "
            "without --fe (Stata regress keeps the norm-pivoted basis)");
    cecon::TsMeta tsm = parse_tsmeta(a);
    std::vector<std::string> ys = split_csv(y);
    // --emit-v: opt-in full e(V) of the fitted model (see add_e_v). The svy and the
    // multi-outcome paths have no e_V slot, so they refuse it rather than ignore it.
    const bool emit_v = has(a, "--emit-v");
    if (emit_v && (has(a, "--svy") || ys.size() > 1))
        throw EngineError("unsupported",
            "feols: --emit-v is not supported with --svy or a multi-outcome --y batch");
    const EsampleRequest esreq = esample_request(a, "feols");
    EsampleRows esrows;
    EsampleRows* es_p = esreq.active() ? &esrows : nullptr;
    // E1: --x may carry L./F./D./S. tokens (build_model's ts_prepass) — route `if`
    // through plan_keepif so they materialize against the FULL series first. So may
    // --y (BUG-1277: `regress D.y x1 x2 if t>=3` reads y at t = 2).
    const std::string keepif = opt(a, "--keepif");
    KeepifPlan kp = plan_keepif(data, keepif, &tsm, {&xs, &ys});
    const std::vector<char>* mask_p = kp.active ? &kp.mask : nullptr;

    // P7 `svy: regress`. A survey design is not a variant of --vcov: it replaces
    // both the meat and the reference distribution, so it takes its own path and
    // rejects the combinations that would otherwise be silently ignored.
    if (has(a, "--svy")) {
        // --strata is optional: a design without strata() is one stratum
        // (w2t_boottest_family.log:2455 H1 `svyset id [pweight=w]`, run_feols_svy).
        std::string psu = opt(a, "--psu"), strata = opt(a, "--strata");
        if (psu.empty())
            throw EngineError("bad_input", "feols: --svy requires --psu");
        if (wkind != cecon::WeightKind::Probability)
            throw EngineError("bad_input",
                "feols: --svy requires --weight W --wtype pw (a survey design is a "
                "probability-weighted sample)");
        if (!fes.empty())
            throw EngineError("unsupported", "feols: --svy is not supported together with --fe");
        if (ys.size() > 1)
            throw EngineError("unsupported",
                "feols: --svy is not supported with a multi-outcome --y batch");
        if (has(a, "--vcov") || !clusters.empty())
            throw EngineError("bad_input",
                "feols: --svy supplies its own linearized variance; --vcov/--cluster are "
                "not accepted with it");
        if (no_intercept)
            throw EngineError("unsupported", "feols: --svy is not supported with --no-intercept");
        json svy_out = run_feols_svy(kp.data, y, xs, weight_var, psu, strata, &tsm, mask_p, level,
                                     es_p);
        emit_esample(esreq, "feols", data, keepif, kp.active, esrows, svy_out);
        emit(svy_out, opt(a, "--out"));
        return 0;
    }

    json out;
    if (ys.size() > 1) {
        // The multi-outcome batch shares one absorbed design across outcomes and has
        // no weight plumbing. Rejecting is mandatory, not tidiness: falling through
        // would run the batch UNweighted and return numbers that look right.
        if (wkind != cecon::WeightKind::None)
            throw EngineError("unsupported",
                "feols: weights are not supported with a multi-outcome --y batch; "
                "run one outcome per invocation");
        // Same reason: the batch has no keep_nested_fe_dof plumbing, so it would
        // answer with the reghdfe dof under an areg request.
        if (keep_nested_fe_dof)
            throw EngineError("unsupported",
                "feols: --keep-nested-fe-dof is not supported with a multi-outcome --y "
                "batch; run one outcome per invocation");
        if (multiway_subset_factors || fe_nested_in_clusters || regress_exact_fit)
            throw EngineError("unsupported",
                "feols: --multiway-subset-factors, --fe-nested-in-clusters and "
                "--regress-exact-fit are not supported with a multi-outcome --y batch; run one "
                "outcome per invocation");
        out = run_feols_multi(kp.data, ys, xs, fes, vcov_type, clusters, drop_singleton,
                              &tsm, mask_p, no_intercept, level, level_set, es_p,
                              collinear_forward);
    } else {
        out = run_feols(kp.data, y, xs, fes, vcov_type, clusters, drop_singleton, &tsm,
                        mask_p, no_intercept, level, level_set, weight_var, wkind, es_p,
                        keep_nested_fe_dof, emit_v, multiway_subset_factors,
                        fe_nested_in_clusters, regress_exact_fit, collinear_forward);
    }
    emit_esample(esreq, "feols", data, keepif, kp.active, esrows, out);
    emit(out, opt(a, "--out"));
    return 0;
}

// apps/cmds/common.cpp: run_fepois with the opt-in --emit-v (FU-859). Declared
// here rather than in common.h, which is outside this change set; the linker
// checks the signature against the definition.
json run_fepois_emit_v(const std::string& data, const std::string& y,
                       const std::vector<std::string>& xs, const std::vector<std::string>& fes,
                       const std::string& offset, const std::string& vcov_type,
                       const std::vector<std::string>& clusters, bool drop_zero_fe,
                       bool scale_x2, const std::string& weight_var, cecon::WeightKind weight_kind,
                       bool emit_v, const std::string& d_out = std::string(),
                       const std::string& d_name = std::string(),
                       const cecon::TsMeta* ts = nullptr,
                       const std::vector<char>* if_mask = nullptr);

int cmd_fepois(const std::vector<std::string>& a) {
    std::string data = opt(a, "--data"), y = opt(a, "--y"), xcsv = opt(a, "--x");
    // --no-x: a model with no regressors (`ppmlhdfe y` / `ppmlhdfe y, absorb(f)`):
    // the constant alone, or the absorbed effects alone with _cons recovered.
    const bool no_x = has(a, "--no-x");
    if (no_x && !xcsv.empty())
        throw EngineError("bad_input", "fepois: --no-x and --x are mutually exclusive");
    if (data_missing(data) || y.empty() || (xcsv.empty() && !no_x)) {
        std::fprintf(stderr, "usage: cobra fepois --data f.hbin --y COUNT[,C2,...] --x X1,X2|--no-x "
                             "[--fe F] [--offset O] [--vcov iid|hc1|cluster|twoway] [--cluster C] "
                             "[--weight W --wtype pw|fw|iw] [--keepif EXPR]\n"
                             "  --weight W --wtype pw  Stata pweight (P5): forces --vcov hc1/robust or\n"
                             "                  cluster (Stata has no iid VCE under pweight); with --fe\n"
                             "                  the absorption is weighted by pw*mu (ppmlhdfe).\n"
                             "  --wtype fw|iw   Stata fweight / iweight: the same weighted fit under\n"
                             "                  any VCE; an fweight row counts w times (e(N) = Sum w).\n");
        return 2;
    }
    // --d-out PATH --d-name NAME: the --data frame plus ppmlhdfe's `d` (the sum of the
    // absorbed effects net of _cons; 0 without --fe), missing off the fit's rows.
    const std::string d_out = opt(a, "--d-out"), d_name = opt(a, "--d-name");
    if (d_out.empty() != d_name.empty())
        throw EngineError("bad_input", "fepois: --d-out and --d-name must be given together");
    if (!d_out.empty() && !opt(a, "--keepif").empty())
        throw EngineError("unsupported", "fepois: --d-out is not supported together with --keepif");
    // BUG-1484: under --xtset/--tsset, --x/--y may carry L./F./D./S. tokens (`poisson
    // cnt L.x1 x2 if t > 2`); route `if` through plan_keepif, as cmd_feols does, so
    // they read the FULL series before the restriction applies.
    const cecon::TsMeta tsm = parse_tsmeta(a);
    const std::vector<std::string> ts_xs = split_csv(xcsv), ts_ys = split_csv(y);
    KeepifPlan kp = plan_keepif(data, opt(a, "--keepif"), &tsm, {&ts_xs, &ts_ys});
    data = kp.data;
    const std::vector<char>* mask_p = kp.active ? &kp.mask : nullptr;
    bool drop_zero_fe = !has(a, "--keep-zero-fe");  // default: drop all-zero FE cells (ppmlhdfe)
    bool scale_x2 = (opt(a, "--scale") == "x2");    // glm quasi-Poisson dispersion-scaled SEs
    std::string vcov_type = opt(a, "--vcov", "iid");
    // Stata pweight always forces a sandwich VCE — Stata has no iid pweight
    // estimator. fweight and iweight (BUG-1538/1539) fit the same weighted
    // likelihood under any VCE; an fweight's e(N) is Sum w (fepois.cpp, and
    // run_fepois_emit_v's n_obs). Stata's `poisson` takes no aweight and
    // `ppmlhdfe` no aweight or iweight (r(101), PIET's tokenizer answers those).
    std::string weight_var = opt(a, "--weight"), wtype = opt(a, "--wtype");
    cecon::WeightKind wkind = cecon::WeightKind::None;
    if (!weight_var.empty() || !wtype.empty()) {
        if (weight_var.empty())
            throw EngineError("bad_input", "fepois: --wtype requires --weight");
        if (wtype == "fw" || wtype == "fweight") wkind = cecon::WeightKind::Frequency;
        else if (wtype == "iw" || wtype == "iweight") wkind = cecon::WeightKind::Importance;
        else if (wtype == "pw" || wtype == "pweight") wkind = cecon::WeightKind::Probability;
        else throw EngineError("bad_input", "fepois: --wtype must be pw, fw or iw");
        if (wkind == cecon::WeightKind::Probability && vcov_type == "iid")
            throw EngineError("bad_input",
                "fepois: [pweight=] requires --vcov hc1/robust or cluster; Stata has no "
                "iid VCE under pweight");
    }
    std::vector<std::string> ys = split_csv(y);
    // --emit-v: opt-in full e(V) of the fitted model (FU-859, see add_e_v). A batch has
    // one V per outcome. Absorbed fits retain their recovered _cons covariance;
    // multi-outcome batches still require a separate matrix per outcome.
    const bool emit_v = has(a, "--emit-v");
    if (emit_v && ys.size() > 1)
        throw EngineError("unsupported",
            "fepois: --emit-v is not supported with a multi-outcome --y batch");
    if (!d_out.empty() && ys.size() > 1)
        throw EngineError("unsupported", "fepois: --d-out is not supported with a multi-outcome --y batch");
    json out;
    if (ys.size() > 1) {
        // Mirrors feols_multi's weight rejection: the shared design across
        // outcomes has no weight plumbing, so silently running unweighted would
        // return numbers that look right but aren't.
        if (wkind != cecon::WeightKind::None)
            throw EngineError("unsupported",
                "fepois: weights are not supported with a multi-outcome --y batch; "
                "run one outcome per invocation");
        // The batch has no time-series plumbing: plan_keepif's full-frame mask would be
        // silently ignored there.
        if (kp.active)
            throw EngineError("unsupported",
                "fepois: time-series operators with --keepif are not supported with a "
                "multi-outcome --y batch; run one outcome per invocation");
        out = run_fepois_multi(data, ys, split_csv(xcsv), split_csv(opt(a, "--fe")),
                               opt(a, "--offset"), vcov_type,
                               split_csv(opt(a, "--cluster")), drop_zero_fe, scale_x2);
    } else {
        out = run_fepois_emit_v(data, y, split_csv(xcsv), split_csv(opt(a, "--fe")),
                                opt(a, "--offset"), vcov_type,
                                split_csv(opt(a, "--cluster")), drop_zero_fe, scale_x2,
                                weight_var, wkind, emit_v, d_out, d_name, &tsm, mask_p);
    }
    emit(out, opt(a, "--out"));
    return 0;
}

// boottest's cluster rows run in ascending cluster-value order (the ado sorts
// on the cluster variable before panelsetup); a Factor's codes run in first-
// appearance order. Rank each code by its value: numbers numerically, strings
// byte-wise. Used only for the Stata draw plan (StataWildDraws).
static std::vector<int32_t> stata_cluster_rank(const Dataset& ds, const std::string& name,
                                               const std::vector<int64_t>& keep, const Factor& f) {
    const Column& c = ds.col(name);
    std::vector<int64_t> rep(static_cast<size_t>(f.n_levels), -1);
    for (size_t r = 0; r < f.codes.size() && r < keep.size(); ++r)
        if (f.codes[r] >= 0 && rep[static_cast<size_t>(f.codes[r])] < 0)
            rep[static_cast<size_t>(f.codes[r])] = keep[r];
    std::vector<int32_t> order(static_cast<size_t>(f.n_levels));
    for (int32_t g = 0; g < f.n_levels; ++g) order[static_cast<size_t>(g)] = g;
    auto rowOf = [&](int32_t g) { return rep[static_cast<size_t>(g)]; };
    if (c.dtype == DType::Factor) {
        const int32_t* codes = c.codes();
        std::stable_sort(order.begin(), order.end(), [&](int32_t x, int32_t y) {
            const int32_t cx = codes[rowOf(x)], cy = codes[rowOf(y)];
            const std::string& sx = cx < 0 ? std::string() : c.levels[static_cast<size_t>(cx)];
            const std::string& sy = cy < 0 ? std::string() : c.levels[static_cast<size_t>(cy)];
            return sx < sy;
        });
    } else {
        const double* v = c.f64();
        std::stable_sort(order.begin(), order.end(),
                         [&](int32_t x, int32_t y) { return v[rowOf(x)] < v[rowOf(y)]; });
    }
    std::vector<int32_t> rank(static_cast<size_t>(f.n_levels));
    for (int32_t q = 0; q < f.n_levels; ++q) rank[static_cast<size_t>(order[static_cast<size_t>(q)])] = q;
    return rank;
}

int cmd_wildboot(const std::vector<std::string>& a) {
    std::string data = opt(a, "--data"), y = opt(a, "--y");
    std::string clcsv = opt(a, "--cluster"), endog = opt(a, "--endog");
    if (data_missing(data) || y.empty() || clcsv.empty()) {
        std::fprintf(stderr, "usage:\n"
            "  OLS: cobra wildboot --data f.hbin --y Y --x X1,X2 --param X1 --cluster C "
            "[--fe F] [--keep-singletons] [--null 0] [--reps 9999] [--seed 42] "
            "[--weights rademacher|webb|mammen|normal|gamma]\n"
            "  IV:  cobra wildboot --data f.hbin --y Y --x EXOG --endog D --instruments Z "
            "--cluster C [--fe F] [--keep-singletons] [--null 0] [--reps 9999] [--seed 42]\n");
        return 2;
    }
    double r = std::stod(opt(a, "--null", "0"));
    int reps = std::stoi(opt(a, "--reps", "9999"));
    if (reps < 1) { std::fprintf(stderr, "wildboot: --reps must be >= 1.\n"); return 2; }
    uint64_t seed = std::stoull(opt(a, "--seed", "42"));
    // --weights: boottest's weighttype() by its full name (BUG-1519); Rademacher when absent.
    BootWeights wt = BootWeights::Rademacher;
    if (const std::string ws = opt(a, "--weights"); !ws.empty() && !boot_weights_from_name(ws, wt))
        throw EngineError("bad_input",
                          "wildboot: --weights must be rademacher, webb, mammen, normal or gamma",
                          "--weights");
    // --keep-singletons: resample the fit's sample when that fit kept its
    // singleton FE groups (areg, xtreg fe); default drop, as feols/iv.
    const bool drop_singleton = !has(a, "--keep-singletons");
    const Dataset& ds = acquire_data(data);
    // --rng-mt64 STATE: draw the Rademacher weights exactly as boottest does, from
    // Stata's session stream at that state (StataWildDraws; PIET sends the state
    // boottest's `set seed` leaves). The response reports the raw draws taken.
    std::optional<StataWildDraws> stata_draws;
    if (const std::string ms = opt(a, "--rng-mt64"); !ms.empty()) {
        stata_draws.emplace();
        if (!stata_draws->stream.restore(ms))
            throw EngineError("bad_input", "wildboot: --rng-mt64 is not an mt64 state string");
    }
    const std::string first_cluster = split_csv(clcsv).empty() ? std::string() : split_csv(clcsv)[0];

    auto t0 = std::chrono::steady_clock::now();
    WildBootResult R;
    double n_obs = 0;
    std::vector<std::string> fv_warnings;  // BUG-112(8): md is branch-scoped, hoist the notes
    bool is_iv = !endog.empty();
    const std::string score_model = opt(a, "--model");
    if (!score_model.empty()) {
        if (is_iv || !opt(a, "--fe").empty() || has(a, "--no-intercept") ||
            !opt(a, "--weight").empty() || !opt(a, "--wtype").empty() || split_csv(clcsv).size() != 1)
            throw EngineError("unsupported", "ML score bootstrap requires one unweighted, unabsorbed, with-constant model");
        GlmInputs g = load_glm(ds, "score bootstrap", y, split_csv(opt(a, "--x")), split_csv(clcsv));
        const std::string param = opt(a, "--param");
        auto it = std::find(g.names.begin(), g.names.end(), param);
        if (it == g.names.end()) throw EngineError("bad_input", "score bootstrap: parameter is not in the fitted design");
        if (stata_draws)
            stata_draws->cluster_rank = stata_cluster_rank(ds, first_cluster, g.keep, g.cluster.at(0));
        R = wild_score_bootstrap(g.X, g.n, g.k, g.y, g.cluster.at(0),
                                static_cast<int>(it - g.names.begin()), param, score_model, r, reps, seed, wt,
                                stata_draws ? &*stata_draws : nullptr);
        n_obs = g.n;
        fv_warnings = g.warnings;
    } else if (has(a, "--wre") || (!is_iv && split_csv(clcsv).size() > 1)) {
        // boottest's WRE after IV, and its multiway wild bootstrap-t after OLS:
        // one direct-refit kernel (wild_restricted_efficient_bootstrap). No FE,
        // no weights: the caller passes the model boottest builds (after
        // ivreghdfe that is the unabsorbed, constant-free one).
        if (!opt(a, "--fe").empty() || !opt(a, "--weight").empty() || !opt(a, "--wtype").empty())
            throw EngineError("unsupported", "wildboot: the WRE / multiway bootstrap takes no fixed effects or weights");
        const std::string param = opt(a, "--param");
        if (param.empty()) throw EngineError("bad_input", "wildboot: --param is required");
        WreSpec spec;
        spec.null_beta = r;
        spec.reps = reps;
        spec.seed = seed;
        spec.weights = wt;
        spec.liml = has(a, "--liml");
        spec.small = has(a, "--small");
        spec.fe_dof_adj = std::stod(opt(a, "--fe-dof-adj", "0"));
        if (is_iv) {
            IvModelData md = build_iv_model(ds, y, split_csv(opt(a, "--x")), split_csv(endog),
                                            split_csv(opt(a, "--instruments", opt(a, "--z"))), {},
                                            split_csv(clcsv), nullptr, false, nullptr,
                                            has(a, "--no-intercept"));
            int j = -1;
            for (int i = 0; i < md.kexog; ++i)
                if (md.exog_names[static_cast<size_t>(i)] == param) j = i;
            for (int m = 0; m < md.kendog; ++m)
                if (md.endog_names[static_cast<size_t>(m)] == param) j = md.kexog + m;
            if (j < 0) throw EngineError("bad_input", "wildboot: --param '" + param + "' is not a regressor");
            spec.param_index = j;
            n_obs = static_cast<double>(md.n);
            fv_warnings = md.warnings;
            R = wild_restricted_efficient_bootstrap(md.y, md.n, md.Xexog, md.kexog, md.exog_names,
                                                    md.Xendog, md.kendog, md.endog_names, md.Zinst,
                                                    md.kinst, md.cluster, spec);
        } else {
            ModelData md = build_model(ds, y, split_csv(opt(a, "--x")), {}, split_csv(clcsv), "",
                                       false);
            if (has(a, "--no-intercept") && !md.dm.xnames.empty() &&
                md.dm.xnames.front() == "(intercept)") {
                md.dm.X.erase(md.dm.X.begin(), md.dm.X.begin() + md.dm.n);
                md.dm.xnames.erase(md.dm.xnames.begin());
                --md.dm.k;
            }
            int j = -1;
            for (int i = 0; i < md.dm.k; ++i)
                if (md.dm.xnames[static_cast<size_t>(i)] == param) j = i;
            if (j < 0) throw EngineError("bad_input", "wildboot: --param '" + param + "' is not a regressor");
            spec.param_index = j;
            n_obs = static_cast<double>(md.dm.n);
            fv_warnings = md.warnings;
            R = wild_restricted_efficient_bootstrap(md.dm.y, md.dm.n, md.dm.X, md.dm.k, md.dm.xnames,
                                                    {}, 0, {}, {}, 0, md.cluster, spec);
        }
    } else if (is_iv) {
        if (has(a, "--no-intercept") || !opt(a, "--weight").empty() || !opt(a, "--wtype").empty())
            throw EngineError("unsupported", "wildboot: weighted/no-intercept IV bootstrap is not supported");
        IvModelData md = build_iv_model(ds, y, split_csv(opt(a, "--x")), split_csv(endog),
                                        split_csv(opt(a, "--instruments", opt(a, "--z"))),
                                        split_csv(opt(a, "--fe")), split_csv(clcsv), nullptr,
                                        drop_singleton);
        n_obs = md.n;
        fv_warnings = md.warnings;
        R = wild_cluster_bootstrap_iv(md, r, reps, seed, 0, wt);
    } else {
        std::string param = opt(a, "--param"), xcsv = opt(a, "--x");
        if (param.empty() || xcsv.empty()) {
            std::fprintf(stderr, "OLS wildboot needs --x and --param\n");
            return 2;
        }
        const std::string weight = opt(a, "--weight"), wtype = opt(a, "--wtype");
        WeightKind kind = WeightKind::None;
        if (!weight.empty() || !wtype.empty()) {
            if (weight.empty()) throw EngineError("bad_input", "wildboot: --wtype requires --weight");
            if (wtype.empty() || wtype == "aw" || wtype == "aweight") kind = WeightKind::Analytic;
            else if (wtype == "pw" || wtype == "pweight") kind = WeightKind::Probability;
            else if (wtype == "fw" || wtype == "fweight") kind = WeightKind::Frequency;
            else if (wtype == "iw" || wtype == "iweight") kind = WeightKind::Importance;
            else throw EngineError("bad_input", "wildboot: unrecognized --wtype");
        }
        ModelData md = build_model(ds, y, split_csv(xcsv), split_csv(opt(a, "--fe")),
                                   split_csv(clcsv), "", drop_singleton, nullptr, nullptr, weight, kind);
        if (has(a, "--no-intercept")) {
            if (!md.fe.empty()) throw EngineError("unsupported", "wildboot: --no-intercept with fixed effects is not supported");
            if (!md.dm.xnames.empty() && md.dm.xnames.front() == "(intercept)") {
                md.dm.X.erase(md.dm.X.begin(), md.dm.X.begin() + md.dm.n);
                md.dm.xnames.erase(md.dm.xnames.begin());
                --md.dm.k;
                md.has_intercept = false;
            }
        }
        int j = -1;
        for (int i = 0; i < static_cast<int>(md.dm.xnames.size()); ++i)
            if (md.dm.xnames[i] == param) j = i;
        if (j < 0) {
            std::fprintf(stderr, "param '%s' not among regressors\n", param.c_str());
            return 2;
        }
        n_obs = md.dm.n;
        if (kind == WeightKind::Frequency) {
            n_obs = 0.0;
            for (double w : md.weights) n_obs += w;
        }
        fv_warnings = md.warnings;
        if (stata_draws && !md.cluster.empty())
            stata_draws->cluster_rank = stata_cluster_rank(ds, first_cluster, md.keep, md.cluster.at(0));
        R = wild_cluster_bootstrap(md, j, r, reps, seed, 0, wt,
                                   stata_draws && !md.cluster.empty() ? &*stata_draws : nullptr);
    }
    auto t1 = std::chrono::steady_clock::now();

    json out = {
        {"estimator", "wild_cluster_bootstrap"},
        {"param", R.param},
        {"null_beta", R.null_beta},
        {"beta", R.beta},
        {"se", R.se},
        {"t0", R.t0},
        {"p_symmetric", R.p_symmetric},
        {"p_equaltail", R.p_equaltail},
        {"reps", R.reps},
        {"weights", R.weights},
        {"webb_fallback", R.webb_fallback},
        {"n_clusters", R.n_clusters},
        {"n_obs", n_obs},
        {"seed", R.seed},
        {"time_ms", std::chrono::duration<double, std::milli>(t1 - t0).count()},
    };
    if (!score_model.empty()) {
        out["estimator"] = "wild_score_bootstrap";
        out["z"] = R.t0;
        out.erase("beta"); out.erase("se"); out.erase("t0");
    }
    if (stata_draws) out["rng_draws"] = stata_draws->taken;
    if (std::isfinite(R.df_r)) {  // the direct-refit kernel (WRE / multiway)
        out["estimator"] = is_iv ? "wild_restricted_efficient_bootstrap" : "wild_cluster_bootstrap_multiway";
        out["df_r"] = R.df_r;
        out["small"] = has(a, "--small");
        if (is_iv) out["kappa"] = R.kappa;
        if (!std::isfinite(R.t0)) out["t0"] = nullptr;
        if (!std::isfinite(R.se)) out["se"] = nullptr;
        if (!std::isfinite(R.p_symmetric)) out["p_symmetric"] = nullptr;
        if (!std::isfinite(R.p_equaltail)) out["p_equaltail"] = nullptr;
    }
    add_warnings(out, fv_warnings);  // BUG-112(8): absent-key when empty
    emit(out, opt(a, "--out"));
    return 0;
}

// liml — LIML / Fuller k-class IV estimator (Stata ivregress liml).
int cmd_liml(const std::vector<std::string>& a) {
    std::string data = opt(a, "--data"), y = opt(a, "--y"), endog = opt(a, "--endog");
    std::string inst = opt(a, "--instruments"), xcsv = opt(a, "--x");
    if (data_missing(data) || y.empty() || endog.empty() || inst.empty()) {
        std::fprintf(stderr, "usage: cobra liml --data f.hbin --y Y --endog D1[,D2] --instruments Z1,Z2 "
                             "[--x EXOG] [--fuller A] [--small] [--keepif EXPR] [--out f.json]\n");
        return 2;
    }
    data = maybe_keepif(data, opt(a, "--keepif"));
    bool small = has(a, "--small");
    double fuller = std::stod(opt(a, "--fuller", "0"));
    const Dataset& ds = acquire_data(data);
    auto endogs = split_csv(endog), insts = split_csv(inst), exogs = split_csv(xcsv);
    int p = static_cast<int>(endogs.size()), m = static_cast<int>(insts.size());
    int kx1 = static_cast<int>(exogs.size()) + 1;  // +constant
    int64_t n_raw = ds.n_rows();
    const double* yraw = require_num(ds, "liml", y);
    std::vector<const double*> Yr, Xr, Zr, all = {yraw};
    for (auto& s : endogs) { Yr.push_back(require_num(ds, "liml", s)); all.push_back(Yr.back()); }
    for (auto& s : exogs)  { Xr.push_back(require_num(ds, "liml", s)); all.push_back(Xr.back()); }
    for (auto& s : insts)  { Zr.push_back(require_num(ds, "liml", s)); all.push_back(Zr.back()); }
    // --vcov iid (default) | robust/hc1 | cluster --cluster C.
    const std::string vcov = opt(a, "--vcov", "iid"), clname = opt(a, "--cluster");
    int vce = 0;
    if (vcov == "robust" || vcov == "hc1") vce = 1;
    else if (vcov == "cluster") vce = 2;
    else if (vcov != "iid")
        throw EngineError("unsupported", "liml: --vcov must be iid, robust/hc1 or cluster");
    if (vce == 2 && (clname.empty() || split_csv(clname).size() != 1))
        throw EngineError("unsupported", "liml: --vcov cluster takes exactly one --cluster variable");
    const double* clraw = vce == 2 ? require_num(ds, "liml", clname) : nullptr;
    if (clraw) all.push_back(clraw);
    auto keep = listwise_keep(all, n_raw);
    int64_t n = static_cast<int64_t>(keep.size());
    require_sample(n);  // BUG-1269: r(2000) "no observations" / r(2001) for one row
    auto gv = [&](const double* p2) { return gather_rows(p2, keep); };
    std::vector<double> yv = gv(yraw);
    std::vector<double> Y(static_cast<size_t>(n) * p), X1(static_cast<size_t>(n) * kx1), Z2(static_cast<size_t>(n) * m);
    for (int c = 0; c < p; ++c) { auto col = gv(Yr[c]); std::copy(col.begin(), col.end(), Y.data() + static_cast<int64_t>(c) * n); }
    for (int c = 0; c + 1 < kx1; ++c) { auto col = gv(Xr[c]); std::copy(col.begin(), col.end(), X1.data() + static_cast<int64_t>(c) * n); }
    for (int64_t i = 0; i < n; ++i) X1[static_cast<int64_t>(kx1 - 1) * n + i] = 1.0;
    for (int c = 0; c < m; ++c) { auto col = gv(Zr[c]); std::copy(col.begin(), col.end(), Z2.data() + static_cast<int64_t>(c) * n); }

    std::vector<int32_t> clcodes;
    int n_clusters = 0;
    if (clraw) {
        std::vector<double> cv = gv(clraw), u = cv;
        std::sort(u.begin(), u.end());
        u.erase(std::unique(u.begin(), u.end()), u.end());
        clcodes.resize(cv.size());
        for (size_t i = 0; i < cv.size(); ++i)
            clcodes[i] = static_cast<int32_t>(std::lower_bound(u.begin(), u.end(), cv[i]) - u.begin());
        n_clusters = static_cast<int>(u.size());
        if (n_clusters < 2) throw EngineError("unsupported", "liml: --vcov cluster needs at least 2 clusters");
    }
    LimlResult r = liml_fit(yv, n, Y, p, X1, kx1, Z2, m, small, fuller, -1.0, vce,
                            clraw ? &clcodes : nullptr, n_clusters);
    if (!r.ok) { std::fprintf(stderr, "liml: estimation failed (underidentified / singular?).\n"); return 1; }
    std::vector<std::string> names = endogs;
    for (auto& s : exogs) names.push_back(s);
    names.push_back("(intercept)");
    double dof = r.dof_r, tc = (small && dof > 0) ? t_critical(0.05, dof) : 1.959963984540054;
    json coefs = json::array();
    for (size_t j = 0; j < names.size(); ++j) {
        double se = r.se[j], t = (se > 0) ? r.beta[j] / se : 0.0;
        double pval = small ? ((se > 0 && dof > 0) ? t_twosided_pvalue(t, dof) : 1.0)
                            : z_twosided_pvalue(t);
        coefs.push_back({{"name", names[j]}, {"b", r.beta[j]}, {"se", se}, {"t", t}, {"p", pval},
                         {"ci_lo", r.beta[j] - tc * se}, {"ci_hi", r.beta[j] + tc * se}});
    }
    json out = {{"estimator", fuller > 0 ? "fuller" : "liml"}, {"kappa", r.kappa},
                {"fuller_alpha", fuller}, {"n_obs", r.n}, {"df_r", dof}, {"sigma2", r.sigma2},
                {"small", small}, {"coefs", coefs}};
    if (vce == 1) out["vcov_type"] = "hc1";
    if (vce == 2) {
        out["vcov_type"] = "cluster(" + clname + ")";
        out["n_clusters"] = n_clusters;
    }
    // --emit-v (BUG-1196): the fit's full covariance, in the coefficients' order.
    if (has(a, "--emit-v")) add_e_v(out, names, r.vcov);
    emit(out, opt(a, "--out"));
    return 0;
}

// reg3 — systems estimation (SUR / 3SLS). Repeatable --eq "dep=rhs1,rhs2".
int cmd_reg3(const std::vector<std::string>& a) {
    std::string data = opt(a, "--data"), method = opt(a, "--method", "sure");
    std::vector<std::string> eqspecs = opt_all(a, "--eq");
    if (data_missing(data) || eqspecs.size() < 2) {
        std::fprintf(stderr, "usage: cobra reg3 --data f.hbin --eq \"y1=x1,x2\" --eq \"y2=x3\" "
                             "[--method sure|3sls] [--exog Z1,Z2] [--keepif EXPR] [--out f.json]\n"
                             "  SUR (default) or 3SLS (RHS vars that are also a depvar are endogenous).\n");
        return 2;
    }
    bool threesls = (method == "3sls");
    // parse equations
    std::vector<std::string> deps;
    std::vector<std::vector<std::string>> rhs;
    for (auto& s : eqspecs) {
        auto eq = s.find('=');
        if (eq == std::string::npos) { std::fprintf(stderr, "reg3: bad --eq '%s'\n", s.c_str()); return 2; }
        deps.push_back(s.substr(0, eq));
        rhs.push_back(split_csv(s.substr(eq + 1)));
    }
    int M = static_cast<int>(deps.size());
    auto is_dep = [&](const std::string& v) { for (auto& d : deps) if (d == v) return true; return false; };
    // instrument set = exogenous RHS (not a depvar) + --exog extras + constant
    std::vector<std::string> instv;
    auto add_inst = [&](const std::string& v) { for (auto& x : instv) if (x == v) return; instv.push_back(v); };
    for (auto& rl : rhs) for (auto& v : rl) if (!is_dep(v)) add_inst(v);
    for (auto& v : split_csv(opt(a, "--exog"))) if (!v.empty()) add_inst(v);

    data = maybe_keepif(data, opt(a, "--keepif"));
    const Dataset& ds = acquire_data(data);
    int64_t n_raw = ds.n_rows();
    // gather all variables for a joint listwise sample
    std::vector<const double*> must;
    for (auto& d : deps) must.push_back(require_num(ds, "reg3", d));
    for (auto& rl : rhs) for (auto& v : rl) must.push_back(require_num(ds, "reg3", v));
    for (auto& v : instv) must.push_back(require_num(ds, "reg3", v));
    auto keep = listwise_keep(must, n_raw);
    int64_t n = static_cast<int64_t>(keep.size());
    require_sample(n);  // BUG-1269: r(2000) "no observations" / r(2001) for one row (reg3 and sureg)
    auto getcol = [&](const std::string& v) { return gather_rows(require_num(ds, "reg3", v), keep); };

    std::vector<Reg3Eq> eqs(M);
    for (int i = 0; i < M; ++i) {
        int k = static_cast<int>(rhs[i].size()) + 1;
        eqs[i].k = k;
        eqs[i].depname = deps[i];
        eqs[i].y = getcol(deps[i]);
        eqs[i].Z.assign(static_cast<size_t>(n) * k, 0.0);
        eqs[i].endog.assign(k, 0);
        for (int c = 0; c + 1 < k; ++c) {
            auto col = getcol(rhs[i][c]);
            std::copy(col.begin(), col.end(), eqs[i].Z.data() + static_cast<int64_t>(c) * n);
            eqs[i].names.push_back(rhs[i][c]);
            if (threesls && is_dep(rhs[i][c])) eqs[i].endog[c] = 1;
        }
        for (int64_t t = 0; t < n; ++t) eqs[i].Z[static_cast<int64_t>(k - 1) * n + t] = 1.0;
        eqs[i].names.push_back("(intercept)");
    }
    int kw = static_cast<int>(instv.size()) + 1;
    std::vector<double> W(static_cast<size_t>(n) * kw, 0.0);
    for (int c = 0; c + 1 < kw; ++c) { auto col = getcol(instv[c]); std::copy(col.begin(), col.end(), W.data() + static_cast<int64_t>(c) * n); }
    for (int64_t t = 0; t < n; ++t) W[static_cast<int64_t>(kw - 1) * n + t] = 1.0;

    Reg3Result r = reg3_fit(eqs, n, W, kw);
    if (!r.ok) { std::fprintf(stderr, "reg3: estimation failed (collinear / underidentified?).\n"); return 1; }
    json eqsj = json::array();
    for (int i = 0; i < M; ++i) {
        json cj = json::array();
        for (int c = 0; c < eqs[i].k; ++c) {
            int idx = r.koff[i] + c;
            double se = r.se[idx];
            const double z = se > 0 ? r.beta[idx] / se : 0.0;
            // Stata's reg3/sureg table is large-sample: z, P>|z| and the normal
            // interval (no small option here).
            cj.push_back({{"name", eqs[i].names[c]}, {"b", r.beta[idx]}, {"se", se},
                          {"t", z}, {"p", se > 0 ? z_twosided_pvalue(z) : 1.0},
                          {"ci_lo", r.beta[idx] - 1.959963984540054 * se},
                          {"ci_hi", r.beta[idx] + 1.959963984540054 * se}});
        }
        eqsj.push_back({{"depvar", deps[i]}, {"coefs", cj}});
    }
    json sig = json::array();
    for (int i = 0; i < M; ++i) { json row = json::array(); for (int j = 0; j < M; ++j) row.push_back(r.Sigma[static_cast<size_t>(i) * M + j]); sig.push_back(row); }
    json out = {{"estimator", "reg3"}, {"method", threesls ? "3sls" : "sure"}, {"n_obs", n},
                {"n_equations", M}, {"equations", eqsj}, {"sigma", sig}};
    // --emit-v: the stacked system covariance (K x K, cross-equation blocks
    // included), named `depvar:coef` in e(b)'s order with the intercept spelled
    // `_cons` -- Stata's reg3/sureg e(V) stripe (`y:x1 y:x2 y:_cons x3:x2 x3:_cons`).
    if (has(a, "--emit-v")) {
        std::vector<std::string> vn;
        for (int i = 0; i < M; ++i)
            for (int c = 0; c < eqs[i].k; ++c)
                vn.push_back(deps[i] + ":" +
                             (eqs[i].names[c] == "(intercept)" ? std::string("_cons") : eqs[i].names[c]));
        add_e_v(out, vn, r.vcov, nullptr);
    }
    emit(out, opt(a, "--out"));
    return 0;
}

}  // namespace cli
}  // namespace cecon
