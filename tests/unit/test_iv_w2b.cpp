// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// w2b_estim IV arms — the engine half of `ivregress ..., first`, the exogenous
// regressors typed after the paren block, ivreghdfe, and ivreg2's partial().
//
// Oracle: tests/eval/golden/aer_w2/w2b_estim.log (StataBE 19.5, 2026-09-19):
//   :919  ivregress_first            _b[x1] 1.940390224456596  _se[x1] .1611032742759025
//   :945  ivregress_exog_after_paren _b[x1] 1.940390224456596  _se[x1] .1770926044974198 (robust)
//   :1013 ivreg2_partial             _b[x1] 1.940390224456596  _se[x1] .1611032742759026
//   :1060 ivreg2_ctl                 _b[x1] 1.940390224456606  _se[x1] .1611032742759016
//   :1185 ivreghdfe_first / :1243 _exog_after_paren / :1301 _ctl
//                                    _b[x1] 2.00795312049193   _se[x1] .1308860665203314
//   first-stage tables (display precision, 7 significant digits):
//   :894-896 ivregress first  x1 on x2 z1 _cons: z1 .2724199 (.0983224), x2 .1668943 (.2727122)
//   :1078-1079 ivreghdfe first (absorbed id):    z1 .2657743 (.1072108), x2 .159917  (.2957897)
//
// Fixture = the do-file's: set obs 60, every `generate` default-typed, i.e. a FLOAT
// column, and Stata fits on the stored floats (test_estim_w2b.cpp's recipe: each
// column evaluated in double, then rounded to float; z1 and y read the float x1/x2).
//
// What the PIET side (estimation_m4b.cpp runIvRegress) relies on, pinned here:
//   * `first` prints the engine's --savefirst stage, which IS the oracle's first
//     stage with and without absorbed effects;
//   * partial() is served by dropping the partialled-out rows from the FULL fit.
//     The partialled fit (y, x1, z1 residualised on [1, x2], no exogenous block)
//     has the same b and the same asymptotic iid and robust se (Frisch-Waugh-
//     Lovell), and the oracle's :1013 equals its :1060 to 1e-14. Under --small the
//     partialled fit's own K would be 1 instead of 3, which is why the full fit's
//     dof -- ivreg2 counts the partialled-out variables in K -- are the ones used.
//
// Tier (tests/eval/TOLERANCE.md): b at T1/T2 (rtol 1e-9; closed-form 2SLS), se at
// T2 (1e-9). The first-stage cells are compared at the log's own display precision
// (half a unit in the last printed decimal) -- the oracle's precision limit.
//
// No engine code changed in this task: these are controls for the identities the
// PIET delta stands on, not discriminators of an engine defect.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "core/model.h"
#include "estimators/iv2sls.h"

using namespace cecon;

static int g_fail = 0;

static void close_to(double got, double want, double rtol, const std::string& what) {
    const double rel = std::fabs(got - want) / std::fabs(want);
    const bool ok = rel <= rtol;
    std::printf("  %-52s got %.16g want %.16g rel %.2e %s\n", what.c_str(), got, want, rel,
                ok ? "ok" : "<<FAIL");
    if (!ok) g_fail = 1;
}

// A displayed table cell, passed as the log prints it: |got - cell| within half a unit
// of the cell's last printed decimal (".0983224" -> 5e-8, ".159917" -> 5e-7).
static void displayed(double got, const char* cell, const std::string& what) {
    const double want = std::strtod(cell, nullptr);
    const char* dot = std::strchr(cell, '.');
    const int decimals = dot ? static_cast<int>(std::strlen(dot + 1)) : 0;
    const double half = 0.5 * std::pow(10.0, -decimals);
    const bool ok = std::fabs(got - want) <= half;
    std::printf("  %-52s got %.10g want %s %s\n", what.c_str(), got, cell, ok ? "ok" : "<<FAIL");
    if (!ok) g_fail = 1;
}

static int idx(const std::vector<std::string>& names, const std::string& n) {
    for (size_t j = 0; j < names.size(); ++j)
        if (names[j] == n) return static_cast<int>(j);
    std::printf("  <<FAIL no coefficient named %s\n", n.c_str());
    g_fail = 1;
    return -1;
}

namespace {

struct Fixture {
    int64_t n = 60;
    std::vector<double> x1, x2, z1, y;
    std::vector<int32_t> id;
};

Fixture make_fixture() {
    Fixture f;
    for (int64_t i = 0; i < f.n; ++i) {
        const double obs = static_cast<double>(i + 1);
        const double idv = std::ceil(obs / 6.0);
        const double x1 = static_cast<float>(std::fmod(obs * 7, 11) + std::fmod(obs, 5) / 10);
        const double x2 = static_cast<float>(std::fmod(obs * 3, 5) + std::fmod(obs * 13, 7) / 10);
        const double z1 = static_cast<float>(std::fmod(obs * 5, 13) + x1 / 2);
        const double y = static_cast<float>(1 + 2 * x1 - x2 + idv / 3 + std::fmod(obs * 11, 17) / 5);
        f.x1.push_back(x1);
        f.x2.push_back(x2);
        f.z1.push_back(z1);
        f.y.push_back(y);
        f.id.push_back(static_cast<int32_t>(idv) - 1);
    }
    return f;
}

Factor mkfac(const std::string& name, const std::vector<int32_t>& codes) {
    Factor fac;
    fac.name = name;
    fac.codes = codes;
    int mx = 0;
    for (int32_t c : codes) mx = std::max(mx, static_cast<int>(c));
    fac.n_levels = mx + 1;
    fac.count.assign(fac.n_levels, 0.0);
    for (int32_t c : codes) fac.count[c] += 1.0;
    return fac;
}

// `ivregress 2sls y x2 (x1 = z1)` / `ivregress 2sls y (x1 = z1) x2`: the same design.
IvModelData full_model(const Fixture& f) {
    IvModelData md;
    md.n = f.n;
    md.has_intercept = true;
    md.kexog = 2;
    md.kendog = 1;
    md.kinst = 1;
    md.Xexog.assign(static_cast<size_t>(f.n), 1.0);
    md.Xexog.insert(md.Xexog.end(), f.x2.begin(), f.x2.end());
    md.Xendog = f.x1;
    md.Zinst = f.z1;
    md.y = f.y;
    md.exog_names = {"(intercept)", "x2"};
    md.endog_names = {"x1"};
    md.inst_names = {"z1"};
    md.keep.resize(static_cast<size_t>(f.n));
    for (int64_t i = 0; i < f.n; ++i) md.keep[i] = i;
    return md;
}

// OLS residual of v on [1, x2] -- ivreg2's partial(x2) applied to one variable.
std::vector<double> partial_out(const std::vector<double>& v, const std::vector<double>& x2) {
    const size_t n = v.size();
    double sx = 0, sv = 0;
    for (size_t i = 0; i < n; ++i) { sx += x2[i]; sv += v[i]; }
    const double mx = sx / n, mv = sv / n;
    double sxx = 0, sxv = 0;
    for (size_t i = 0; i < n; ++i) {
        sxx += (x2[i] - mx) * (x2[i] - mx);
        sxv += (x2[i] - mx) * (v[i] - mv);
    }
    const double b = sxv / sxx;
    std::vector<double> r(n);
    for (size_t i = 0; i < n; ++i) r[i] = (v[i] - mv) - b * (x2[i] - mx);
    return r;
}

}  // namespace

int main() {
    std::printf("test_iv_w2b:\n");
    const Fixture f = make_fixture();
    const double kT2 = 1e-9;

    // ivregress_first / ivreg2_ctl: asymptotic iid (ivregress's and ivreg2's default),
    // with the first stage the `first` option prints.
    {
        IvModelData md = full_model(f);
        IvResult r = iv2sls_fit(md, Vcov::IID, 0, false, /*small=*/false, /*savefirst=*/true);
        const int j = idx(r.names, "x1");
        if (j >= 0) {
            close_to(r.beta[j], 1.940390224456596, kT2, "ivregress_first _b[x1]");
            close_to(r.se[j], .1611032742759025, kT2, "ivregress_first _se[x1]");
            close_to(r.se[j], .1611032742759016, kT2, "ivreg2_ctl _se[x1]");
        }
        if (r.first_stage.size() != 1) {
            std::printf("  <<FAIL first_stage size %zu\n", r.first_stage.size());
            g_fail = 1;
        } else {
            const IvFirstStage& fs = r.first_stage[0];
            const int jz = idx(fs.names, "z1"), jx = idx(fs.names, "x2");
            if (jz >= 0 && jx >= 0) {
                displayed(fs.beta[jz], ".2724199", "ivregress_first stage b[z1]");
                displayed(fs.se[jz], ".0983224", "ivregress_first stage se[z1]");
                displayed(fs.beta[jx], ".1668943", "ivregress_first stage b[x2]");
                displayed(fs.se[jx], ".2727122", "ivregress_first stage se[x2]");
            }
            if (fs.n != 60 || fs.df_r != 57) {
                std::printf("  <<FAIL first stage N %lld df_r %lld (want 60, 57: F(2, 57) :885)\n",
                            static_cast<long long>(fs.n), static_cast<long long>(fs.df_r));
                g_fail = 1;
            }
        }
    }

    // ivregress_exog_after_paren: vce(robust) -> HC0, no small-sample factor.
    {
        IvModelData md = full_model(f);
        IvResult r = iv2sls_fit(md, Vcov::HC1, 0, false, /*small=*/false);
        const int j = idx(r.names, "x1");
        if (j >= 0) {
            close_to(r.beta[j], 1.940390224456596, kT2, "ivregress_exog_after_paren _b[x1]");
            close_to(r.se[j], .1770926044974198, kT2, "ivregress_exog_after_paren _se[x1]");
        }
    }

    // ivreg2_partial: the partialled fit equals the full fit's x1 row (FWL), iid and
    // robust; the oracle's own partial cell is the third leg.
    {
        IvModelData full = full_model(f);
        IvResult rf = iv2sls_fit(full, Vcov::IID, 0, false, false);
        IvModelData fullR = full_model(f);
        IvResult rfR = iv2sls_fit(fullR, Vcov::HC1, 0, false, false);
        auto partialled = [&]() {
            IvModelData md;
            md.n = f.n;
            md.has_intercept = false;
            md.kexog = 0;
            md.kendog = 1;
            md.kinst = 1;
            md.Xendog = partial_out(f.x1, f.x2);
            md.Zinst = partial_out(f.z1, f.x2);
            md.y = partial_out(f.y, f.x2);
            md.endog_names = {"x1"};
            md.inst_names = {"z1"};
            md.keep.resize(static_cast<size_t>(f.n));
            for (int64_t i = 0; i < f.n; ++i) md.keep[i] = i;
            return md;
        };
        IvModelData mp = partialled();
        IvResult rp = iv2sls_fit(mp, Vcov::IID, 0, false, false);
        IvModelData mpR = partialled();
        IvResult rpR = iv2sls_fit(mpR, Vcov::HC1, 0, false, false);
        const int jf = idx(rf.names, "x1"), jp = idx(rp.names, "x1");
        if (jf >= 0 && jp >= 0) {
            close_to(rp.beta[jp], rf.beta[jf], 1e-12, "partial(x2) b[x1] == full b[x1]");
            close_to(rp.se[jp], rf.se[jf], 1e-12, "partial(x2) iid se[x1] == full");
            close_to(rpR.se[jp], rfR.se[jf], 1e-12, "partial(x2) robust se[x1] == full");
            close_to(rf.se[jf], .1611032742759026, kT2, "ivreg2_partial _se[x1] (full-fit row)");
        }
    }

    // ivreghdfe_first / _exog_after_paren / _ctl: absorb(id), small-sample (ivreghdfe's
    // default), and the FE-absorbed first stage `first` prints (no _cons row).
    {
        IvModelData md;
        md.n = f.n;
        md.has_intercept = false;
        md.kexog = 1;
        md.kendog = 1;
        md.kinst = 1;
        md.Xexog = f.x2;
        md.Xendog = f.x1;
        md.Zinst = f.z1;
        md.y = f.y;
        md.exog_names = {"x2"};
        md.endog_names = {"x1"};
        md.inst_names = {"z1"};
        md.fe = {mkfac("id", f.id)};
        md.keep.resize(static_cast<size_t>(f.n));
        for (int64_t i = 0; i < f.n; ++i) md.keep[i] = i;
        IvResult r = iv2sls_fit(md, Vcov::IID, 0, false, /*small=*/true, /*savefirst=*/true);
        const int j = idx(r.names, "x1");
        if (j >= 0) {
            close_to(r.beta[j], 2.00795312049193, kT2, "ivreghdfe_ctl _b[x1]");
            close_to(r.se[j], .1308860665203314, kT2, "ivreghdfe_ctl _se[x1]");
        }
        if (r.dof.k_fe != 10) {
            std::printf("  <<FAIL k_fe %d (want 10: :1179 10 categories, 0 redundant)\n",
                        static_cast<int>(r.dof.k_fe));
            g_fail = 1;
        }
        if (r.first_stage.size() == 1) {
            const IvFirstStage& fs = r.first_stage[0];
            const int jz = idx(fs.names, "z1"), jx = idx(fs.names, "x2");
            if (jz >= 0 && jx >= 0) {
                displayed(fs.beta[jz], ".2657743", "ivreghdfe_first stage b[z1]");
                displayed(fs.se[jz], ".1072108", "ivreghdfe_first stage se[z1]");
                displayed(fs.beta[jx], ".159917", "ivreghdfe_first stage b[x2]");
                displayed(fs.se[jx], ".2957897", "ivreghdfe_first stage se[x2]");
            }
            for (const auto& nm : fs.names) {
                if (nm == "(intercept)") {
                    std::printf("  <<FAIL absorbed first stage carries a constant row\n");
                    g_fail = 1;
                }
            }
            if (fs.df_r != 48) {
                std::printf("  <<FAIL absorbed first stage df_r %lld (want 48: F(1, 48) :1082)\n",
                            static_cast<long long>(fs.df_r));
                g_fail = 1;
            }
        } else {
            std::printf("  <<FAIL absorbed first_stage size %zu\n", r.first_stage.size());
            g_fail = 1;
        }
    }

    if (g_fail) { std::printf("FAILED\n"); return 1; }
    std::printf("PASSED\n");
    return 0;
}
