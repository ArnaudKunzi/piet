// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// logit / probit / margins validated vs Stata (glm_golden.do). Data built in C++
// dropping NaN visits_w (matching Stata's `if !missing`), so N=73079.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include "estimators/glm.h"
#include "estimators/logit.h"
#include "estimators/probit.h"
#include "estimators/posthest.h"
#include "io/columnar.h"
#include "validate_dir.h"

using namespace cecon;

static int chk(double a, double b, double rtol, const char* what) {
    double r = std::fabs(a - b) / (std::fabs(b) + 1e-30);
    std::printf("  %-18s cecon=%.10g stata=%.10g rel=%.2e\n", what, a, b, r);
    return r <= rtol ? 0 : 1;
}


// ---------------------------------------------------------------------------
// glm_fit against StataBE's default glm lifecycle. Oracle:
// tests/eval/golden/aer_w2/w2_glm_lifecycle.{do,log}; every constant below is the
// log's own %21x record, rewritten mechanically as a C++ hex literal (B|, V|,
// SCALAR| ll/phi/deviance/deviance_p/ic/converged). The data are the do-file's
// 96-row synthetic stanza, rebuilt here in the same evaluation order.
namespace {

struct LcArm {
    const char* name;
    std::vector<double> b;      // e(b), every column Stata posts (base levels included)
    std::vector<double> Vlow;   // e(V), lower triangle row by row
    double ll, phi, dev, devp, ic, conv;
};

const LcArm kLcArms[] = {
    {"gam_def", {0x1.287270399c3b8p-6, 0x1.df962e2a0f875p-14, 0x1.95b36ffd48cbep+1},
     {0x1.01b308ddac1bdp-16, 0x1.570c9b7271090p-27, 0x1.3317ae9a2b143p-29, -0x1.c4d0e700a7675p-12, -0x1.d53df800d1de9p-18, 0x1.11d78e4512636p-5},
     -0x1.dec6dbd62c5e2p+8, 0x1.a391d262efe5ep-4, 0x1.4c86e9211f399p+3, 0x1.30d7f2dbe24cfp+3, 0x1.0000000000000p+1, 0x1.0000000000000p+0},
    {"gam_tight", {0x1.2872832d7fb79p-6, 0x1.df967d7f37bb2p-14, 0x1.95b3673512893p+1},
     {0x1.0155b6505f58ep-16, 0x1.56905ee0efe90p-27, 0x1.32a878901a753p-29, -0x1.c42cea535fccfp-12, -0x1.d49407f04dac6p-18, 0x1.11746162881d8p-5},
     -0x1.dec6dbd62c56cp+8, 0x1.a391c0f0cd8e7p-4, 0x1.4c86e9211d632p+3, 0x1.30d7e62ef559bp+3, 0x1.0000000000000p+2, 0x1.0000000000000p+0},
    {"gam_it0", {0x1.1b65205a9abcep-6, 0x1.a738ebe6b0adep-14, 0x1.991d70eef5027p+1},
     {0x1.0aa2bb1c9aca7p-16, 0x1.6408dd2adce10p-27, 0x1.3e093b2f44c73p-29, -0x1.d58ad20a3cae0p-12, -0x1.e7001b6d85e96p-18, 0x1.1ca6b5fb084f7p-5},
     -0x1.ded75c962a517p+8, 0x1.c8f7646870294p-4, 0x1.50a719209c0f6p+3, 0x1.4c03bef3e17dfp+3, 0.0, 0.0},
    {"gam_it1", {0x1.2800ca26fd7bep-6, 0x1.ddb0dec4cf8d6p-14, 0x1.95e1e0b697d44p+1},
     {0x1.18166ede1474fp-16, 0x1.74e4285fdea98p-27, 0x1.4dc83981bb705p-29, -0x1.ec312ddf24805p-12, -0x1.fe0f9312b036ap-18, 0x1.29adda8a82165p-5},
     -0x1.dec6dda03cf36p+8, 0x1.a429f0f7be02bp-4, 0x1.4c875ba54489dp+3, 0x1.31467914000dep+3, 0x1.0000000000000p+0, 0.0},
    {"bin_logit_it0", {0x1.49549fea7b899p-5, -0x1.43e28352d3982p-14, 0x1.014bee1323562p+0},
     {0x1.5072f7e3fc98ap-10, 0x1.5916289b55a00p-22, 0x1.805c946ba0078p-23, -0x1.0636f64f1675dp-5, -0x1.218f70805119dp-11, 0x1.451a5459c8c90p+1},
     -0x1.14e94b3085e35p+5, 0x1.0000000000000p+0, 0x1.14e94b3085e35p+6, 0x1.2f876f3dbf115p+6, 0.0, 0.0},
    {"bin_logit_it1", {0x1.64ed13d8f2538p-4, -0x1.58bcf274332c7p-13, 0x1.667cf83d1a49ep-2},
     {0x1.d030ed0b8f5ccp-10, 0x1.60d5d1fd2c8c0p-23, 0x1.c2cacc6f05985p-23, -0x1.424aa7da9aee9p-5, -0x1.54348a0897e03p-11, 0x1.7a911e2eba651p+1},
     -0x1.0f52205f8885cp+5, 0x1.0000000000000p+0, 0x1.0f52205f8885ep+6, 0x1.47a01ce122728p+6, 0x1.0000000000000p+0, 0.0},
    {"bin_logit_def", {0x1.504e0d5ef6b23p-4, -0x1.3e9abc371607bp-13, 0x1.ba4dff6b645f6p-2},
     {0x1.c667e0c46f69bp-10, 0x1.b7b5f293c4dc0p-23, 0x1.c2a0b36da7252p-23, -0x1.3f6e36e261957p-5, -0x1.542ab1c6cef47p-11, 0x1.7a4183fcfd908p+1},
     -0x1.0f419834f2e9ap+5, 0x1.0000000000000p+0, 0x1.0f419834f2e9ap+6, 0x1.48eaf82796eebp+6, 0x1.8000000000000p+1, 0x1.0000000000000p+0},
    {"bin_probit_def", {0x1.962ecdb4bb3c3p-5, -0x1.3d5c17fca42d1p-14, 0x1.794012c095a51p-3},
     {0x1.2447d1935799ep-11, 0x1.0b768074fa630p-23, 0x1.0aac5c5907c68p-24, -0x1.acebc24c58e42p-7, -0x1.92e21441f796dp-13, 0x1.d39c1bbd3dd90p-1},
     -0x1.0cf675671e820p+5, 0x1.0000000000000p+0, 0x1.0cf675671e81fp+6, 0x1.4364d2347a8eep+6, 0x1.8000000000000p+1, 0x1.0000000000000p+0},
    {"nb_default", {0x1.33aace78a7ef2p-6, 0x1.f8452d654c17ep-14, 0x1.00ce486e580d5p+1},
     {0x1.1d50422c7a2d4p-15, 0x1.6fae22fa85997p-23, 0x1.4dd6447f1b7cap-26, -0x1.6a2ae426bc5b7p-10, -0x1.08a709680ff42p-14, 0x1.cf8d8c4300ca6p-3},
     -0x1.7543a5648be93p+8, 0x1.0000000000000p+0, 0x1.50fa379d55f6ap+3, 0x1.348d2070cbdc2p+3, 0x1.0000000000000p+1, 0x1.0000000000000p+0},
    {"nb_off_poisson_control", {0x1.2c57c926dd17fp-6, 0x1.d63326952d64cp-14, 0x1.057f90ca5efcap+1},
     {0x1.029c9d8398667p-15, 0x1.7c1d6adfd14acp-24, 0x1.4b9324abca11ap-26, -0x1.15b52567da687p-10, -0x1.fb551ea8553ccp-15, 0x1.ae5bf4be653abp-3},
     -0x1.3d389175b4ba8p+8, 0x1.0000000000000p+0, 0x1.781019c8a30a8p+7, 0x1.6dcbc10ec6475p+7, 0x1.0000000000000p+1, 0x1.0000000000000p+0},
    {"pois_aw_cl_def", {0x1.1eec749b40809p-6, 0x1.d6b65e1ac8bd7p-14, 0.0, -0x1.2ff67a655a812p-4, 0x1.33041a85a9811p-6, -0x1.f0b666b760820p-8, 0x1.0a59543c429b9p+1},
     {0x1.a8bb6ce7b6958p-16, 0x1.a9d03fd49b072p-24, 0x1.4d2381a0ac03bp-26, 0.0, 0.0, 0.0, -0x1.9da9bd1619fa5p-15, -0x1.0802489c93247p-19, 0.0, 0x1.8102005c816bfp-8, -0x1.30c2f0e41bea6p-13, 0x1.2a78954fc28d8p-20, 0.0, 0x1.25899276c2e93p-9, 0x1.75d1f7682e3e0p-8, -0x1.c8c7bf2034ce0p-16, 0x1.a289f866df9bap-20, 0.0, 0x1.6a2aa46cc12ffp-9, 0x1.9cf77ba948ee1p-9, 0x1.701b84c812697p-8, -0x1.bfedcbc72b04ep-11, -0x1.063065ee741cfp-14, 0.0, 0x1.3c75823b96d75p-8, -0x1.99b6cd31f8d90p-10, -0x1.d2ddc873278dcp-8, 0x1.b7ef2c47982dap-3},
     -0x1.3bff4b91c5678p+8, 0x1.0000000000000p+0, 0x1.726b98008f4bfp+7, 0x1.65eb47e2f7ea8p+7, 0x1.0000000000000p+1, 0x1.0000000000000p+0},
    {"pois_noweight_control", {0x1.2dc63120e39f4p-6, 0x1.ddab448babd81p-14, 0.0, -0x1.55cba84b22c8fp-5, 0x1.26ea58739c4d4p-6, 0x1.661e0b7f5a7bep-10, 0x1.05325ae3a4e96p+1},
     {0x1.dc6b4639a291bp-16, 0x1.ce27eb48fd13ep-24, 0x1.4cfa3b8716cb1p-26, 0.0, 0.0, 0.0, -0x1.82ae9168a74cep-15, -0x1.16a30d3aa8b26p-20, 0.0, 0x1.0d0845ab1cd68p-8, -0x1.60f7958c1ebedp-13, 0x1.3a947230218e6p-20, 0.0, 0x1.63c93ce799ed6p-10, 0x1.03f98865ff35ep-8, -0x1.19becd8c2cc9ap-16, 0x1.268da42628c02p-20, 0.0, 0x1.e80e3dcfbc2a0p-10, 0x1.1a267250fe969p-9, 0x1.036beeb05c34fp-8, -0x1.04503a39a900ep-10, -0x1.056c09aee685dp-14, 0.0, 0x1.6bb43633933fcp-9, -0x1.0fdc7f6172260p-12, -0x1.417b66eac1450p-8, 0x1.bbc0dcb6974f5p-3},
     -0x1.3cd22fce7960fp+8, 0x1.0000000000000p+0, 0x1.7676932bb5a43p+7, 0x1.6a35c46621907p+7, 0x1.0000000000000p+1, 0x1.0000000000000p+0},
    {"gam_cl_factor", {0.0, -0x1.41ae958b8f0b4p-10, 0x1.0d182cdb7cd64p-6, 0x1.1603fdeec6fc8p-5, 0x1.259a2da4d4c61p-6, 0x1.e7a2e1da8b1f2p-14, 0x1.93f4fe13fa817p+1},
     {0.0, 0.0, 0x1.2237d920eba66p-8, 0.0, 0x1.c703e201b4f30p-11, 0x1.17c13db765518p-8, 0.0, 0x1.a79f2ce8248fbp-9, 0x1.83a9c15cf7002p-10, 0x1.1272698cd1dcap-8, 0.0, -0x1.958983f867100p-21, -0x1.b605bd0ecafb8p-14, -0x1.74288495d07dep-15, 0x1.f20d1074a8138p-16, 0.0, 0x1.bd2cdb0398eb6p-18, 0x1.c47363fe2a795p-20, 0x1.eef6819f38424p-18, 0x1.0595ca4f9bc86p-23, 0x1.536b248a87c77p-26, 0.0, -0x1.64c371ec03baep-6, -0x1.8c58346afc4f2p-9, -0x1.7e8e776c7e445p-6, -0x1.1ae71c72aeb75p-10, -0x1.16e531cf6b2aap-14, 0x1.eaba958b43b12p-3},
     -0x1.dec462b0ae358p+8, 0x1.b22f505c20ce7p-4, 0x1.4be89fc195112p+3, 0x1.31494480c7111p+3, 0x1.0000000000000p+1, 0x1.0000000000000p+0},
};

const LcArm* lcArm(const char* name) {
    for (const auto& a : kLcArms)
        if (std::string(a.name) == name) return &a;
    return nullptr;
}

// rtol 1e-9 (TOLERANCE.md T2) with a floor of 1e-9 x the largest |value| in the
// vector, for entries that sit near zero.
int lcCmp(const char* arm, const char* what, const std::vector<double>& got,
          const std::vector<double>& want) {
    double scale = 0.0, worst = 0.0;
    for (double v : want) scale = std::max(scale, std::fabs(v));
    int bad = 0;
    for (size_t i = 0; i < want.size(); ++i) {
        const double d = std::fabs(got[i] - want[i]);
        worst = std::max(worst, d / (std::fabs(want[i]) + 1e-300));
        if (!(d <= 1e-9 * std::fabs(want[i]) + 1e-9 * scale)) bad = 1;
    }
    std::printf("  %-22s %-9s max rel %.2e %s\n", arm, what, worst, bad ? "FAIL" : "ok");
    return bad;
}

int lcScalar(const char* arm, const char* what, double got, double want, double rtol) {
    const double rel = std::fabs(got - want) / (std::fabs(want) + 1e-300);
    const int bad = (rel <= rtol) ? 0 : 1;
    std::printf("  %-22s %-9s got %.17g want %.17g rel %.2e %s\n", arm, what, got, want, rel,
                bad ? "FAIL" : "ok");
    return bad;
}

struct LcSpec {
    const char* arm;
    GlmFamily fam;
    GlmLinkFn link;
    int yvar;        // 0 = y, 1 = cnt, 2 = bin
    int design;      // 0 = x1 x2 _cons; 1 = x1 x2 i.g _cons; 2 = i.(g) x1 x2 _cons
    bool aw, clus;
    int maxiter;     // -1 = default
    bool tight;
};

int glm_lifecycle_arms() {
    std::printf("test_glm (w2_glm_lifecycle):\n");
    const int64_t n = 96;
    std::vector<double> x1(n), x2(n), w(n), y(n), cnt(n), bin(n), g(n);
    std::vector<int64_t> cl(n);
    for (int64_t i = 0; i < n; ++i) {
        const double k = static_cast<double>(i + 1);
        cl[i] = static_cast<int64_t>(std::fmod(k - 1, 8.0));  // dense id: cl - 1
        g[i] = std::fmod(k - 1, 4.0) + 1;
        x1[i] = 12 + std::fmod(k * 7, 29.0);
        x2[i] = 1800 + 25 * std::fmod(k * 13, 97.0);
        w[i] = 1 + std::fmod(k, 5.0) / 3;
        y[i] = (40 + std::fmod(k * 37, 101.0)) * (1 + x1[i] / 25) * (1 + x2[i] / 1500) / 10;
        cnt[i] = std::floor(y[i] / 3);
        bin[i] = (x1[i] + std::fmod(k * 19, 37.0)) > 28 ? 1.0 : 0.0;
    }
    const LcSpec specs[] = {
        {"gam_def", GlmFamily::Gamma, GlmLinkFn::Log, 0, 0, false, false, -1, false},
        {"gam_tight", GlmFamily::Gamma, GlmLinkFn::Log, 0, 0, false, false, -1, true},
        {"gam_it0", GlmFamily::Gamma, GlmLinkFn::Log, 0, 0, false, false, 0, false},
        {"gam_it1", GlmFamily::Gamma, GlmLinkFn::Log, 0, 0, false, false, 1, false},
        {"bin_logit_it0", GlmFamily::Bernoulli, GlmLinkFn::Logit, 2, 0, false, false, 0, false},
        {"bin_logit_it1", GlmFamily::Bernoulli, GlmLinkFn::Logit, 2, 0, false, false, 1, false},
        {"bin_logit_def", GlmFamily::Bernoulli, GlmLinkFn::Logit, 2, 0, false, false, -1, false},
        {"bin_probit_def", GlmFamily::Bernoulli, GlmLinkFn::Probit, 2, 0, false, false, -1, false},
        {"nb_default", GlmFamily::NegBinomial, GlmLinkFn::Log, 1, 0, false, true, -1, false},
        {"nb_off_poisson_control", GlmFamily::Poisson, GlmLinkFn::Log, 1, 0, false, true, -1, false},
        {"pois_aw_cl_def", GlmFamily::Poisson, GlmLinkFn::Log, 1, 1, true, true, -1, false},
        {"pois_noweight_control", GlmFamily::Poisson, GlmLinkFn::Log, 1, 1, false, true, -1, false},
        {"gam_cl_factor", GlmFamily::Gamma, GlmLinkFn::Log, 0, 2, false, true, -1, false},
    };
    int fail = 0;
    for (const auto& sp : specs) {
        const LcArm* a = lcArm(sp.arm);
        if (a == nullptr) {
            std::printf("  %s: no oracle row\n", sp.arm);
            fail = 1;
            continue;
        }
        // Design columns and, per column, its position in Stata's e(b) (the base
        // level 1b.g is posted as a zero column and is not a column here).
        std::vector<std::vector<double>> cols;
        std::vector<size_t> pos;
        auto lvl = [&](int L) {
            std::vector<double> c(n);
            for (int64_t i = 0; i < n; ++i) c[i] = (g[i] == L) ? 1.0 : 0.0;
            return c;
        };
        const std::vector<double> one(n, 1.0);
        if (sp.design == 0) { cols = {x1, x2, one}; pos = {0, 1, 2}; }
        if (sp.design == 1) { cols = {x1, x2, lvl(2), lvl(3), lvl(4), one}; pos = {0, 1, 3, 4, 5, 6}; }
        if (sp.design == 2) { cols = {lvl(2), lvl(3), lvl(4), x1, x2, one}; pos = {1, 2, 3, 4, 5, 6}; }
        const int k = static_cast<int>(cols.size());
        std::vector<double> X;
        for (const auto& c : cols) X.insert(X.end(), c.begin(), c.end());
        GlmOptions o;
        o.family = sp.fam;
        o.link = sp.link;
        if (sp.maxiter >= 0) o.maxiter = sp.maxiter;
        if (sp.tight) { o.tol = 1e-14; o.ltol = 1e-14; o.nrtol = 1e-12; }
        const std::vector<double>& yy = sp.yvar == 0 ? y : (sp.yvar == 1 ? cnt : bin);
        GlmResult r = glm_fit(X.data(), n, k, yy.data(), sp.aw ? w.data() : nullptr, nullptr,
                              sp.clus ? cl.data() : nullptr, o);
        if (!r.ok) {
            std::printf("  %s: glm_fit failed: %s\n", sp.arm, r.error.c_str());
            fail = 1;
            continue;
        }
        std::vector<double> wb, gb, wv, gv;
        for (int j = 0; j < k; ++j) {
            wb.push_back(a->b[pos[j]]);
            gb.push_back(r.beta[j]);
        }
        for (int i = 0; i < k; ++i)
            for (int j = 0; j <= i; ++j) {
                const size_t pi = pos[i], pj = pos[j];
                wv.push_back(a->Vlow[pi * (pi + 1) / 2 + pj]);
                gv.push_back(r.V[static_cast<size_t>(i) * k + j]);
            }
        fail |= lcCmp(sp.arm, "e(b)", gb, wb);
        fail |= lcCmp(sp.arm, "e(V)", gv, wv);
        fail |= lcScalar(sp.arm, "e(ll)", r.ll, a->ll, 1e-12);
        fail |= lcScalar(sp.arm, "e(phi)", r.phi, a->phi, 1e-9);
        fail |= lcScalar(sp.arm, "deviance", r.deviance, a->dev, 1e-9);
        fail |= lcScalar(sp.arm, "dev_p", r.deviance_p, a->devp, 1e-9);
        fail |= lcScalar(sp.arm, "e(ic)", r.ic, a->ic, 0.0);
        fail |= lcScalar(sp.arm, "converged", r.converged ? 1.0 : 0.0, a->conv, 0.0);
    }
    // An exactly collinear regressor (x3 = 2*x1) leaves a roundoff-level pivot,
    // not a zero one; glm_fit must say so rather than return a finite fit.
    {
        std::vector<double> X;
        for (int64_t i = 0; i < n; ++i) X.push_back(x1[i]);
        for (int64_t i = 0; i < n; ++i) X.push_back(2 * x1[i]);
        for (int64_t i = 0; i < n; ++i) X.push_back(1.0);
        GlmOptions o;
        o.family = GlmFamily::Gamma;
        o.link = GlmLinkFn::Log;
        GlmResult r = glm_fit(X.data(), n, 3, y.data(), nullptr, nullptr, nullptr, o);
        const bool ok = !r.ok && r.error.find("collinear") != std::string::npos;
        std::printf("  %-22s collinear x3 = 2*x1 refused: %s\n", "collinear", ok ? "ok" : "FAIL");
        fail |= ok ? 0 : 1;
    }
    return fail;
}

}  // namespace

int test_glm_main() {
    const int lifecycleFail = glm_lifecycle_arms();
    std::printf("test_glm:\n");
    Dataset ds = Dataset::open(cobra_test::validate_path("csdid_input.hbin"));
    int64_t N = ds.n_rows();
    const double* wv = ds.col("wv").f64();
    const double* g = ds.col("gvar").f64();
    const double* vw = ds.col("visits_w").f64();
    const double* az = ds.col("age_z").f64();
    const double* eh = ds.col("edu_hi").f64();
    std::vector<int64_t> keep;
    for (int64_t i = 0; i < N; ++i) if (!std::isnan(vw[i])) keep.push_back(i);
    int64_t n = static_cast<int64_t>(keep.size());
    int k = 4;
    std::vector<double> X(static_cast<size_t>(n) * k), y01(static_cast<size_t>(n)), yint(static_cast<size_t>(n));
    for (int64_t i = 0; i < n; ++i) {
        int64_t r = keep[static_cast<size_t>(i)];
        double tr = (g[r] > 0 && wv[r] >= g[r]) ? 1.0 : 0.0;
        X[0 * n + i] = tr; X[1 * n + i] = az[r]; X[2 * n + i] = eh[r]; X[3 * n + i] = 1.0;
        y01[static_cast<size_t>(i)] = (vw[r] > 0) ? 1.0 : 0.0;
        yint[static_cast<size_t>(i)] = std::round(vw[r]);
    }
    int fail = 0;

    LogitResult lg = logit_fit(X.data(), n, k, y01.data());
    fail |= chk(lg.beta[0], 0.989193094473059, 1e-6, "logit treated");
    fail |= chk(lg.beta[1], -0.0493091010378093, 1e-5, "logit age_z");
    fail |= chk(lg.beta[2], 0.0402888529810946, 1e-5, "logit edu_hi");
    {
        MarginsResult m = margins_logit(X.data(), n, k, lg.beta.data(), lg.bread.data());
        fail |= chk(m.dydx[0], 0.0305064686462157, 1e-6, "logit AME treated");
        fail |= chk(m.se[0], 0.0028995758164692, 1e-5, "logit AME se");
    }

    ProbitResult pb = probit_fit(X.data(), n, k, y01.data());
    fail |= chk(pb.beta[0], 0.402960385563978, 5e-6, "probit treated");
    fail |= chk(pb.beta[1], -0.0182706296306294, 5e-5, "probit age_z");
    {
        MarginsResult m = margins_probit(X.data(), n, k, pb.beta.data(), pb.V.data());
        fail |= chk(m.dydx[0], 0.0286837499723377, 5e-5, "probit AME treated");
    }

    // poisson AME via pooled poisson would need fepois_fit + ModelData; covered by
    // the CLI golden (margins --model poisson) — exact match recorded there.

    fail |= lifecycleFail;
    std::printf("%s\n", fail ? "FAILED" : "PASSED");
    return fail;
}
