// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
#include "common.h"
#include "commands.h"
#include "core/distributions.h"
#include "estimators/miimpute.h"

// P7 wave — mi impute regress.
//
// Stata's three-step ritual (`mi set mlong` / `mi register imputed x2` /
// `mi impute regress x2 = x1 y, add(2) rseed(...)`) collapses to ONE engine call
// here, because the first two steps are pure bookkeeping over the same dataset:
// `mi register imputed x2` only computes and prints "(129 m=0 obs now marked as
// incomplete)", which is the count of missing x2 — and that count is emitted below
// as `n_incomplete`, from the same scan that builds the estimation sample. Keeping
// the classification in ONE pass is what makes the accounting box and the
// regression sample provably consistent with each other.
//
// This verb does NOT write the mlong rows itself. It emits the imputed values
// keyed by (imputation, original row index) and lets PIET materialize `_mi_m` /
// `_mi_id` however its frame model wants them — the engine's job is the draw, not
// the reshape.
//
// Oracle: StataBE 19.5, internal capture p7_stata.log.
// Assertions: tests/unit/test_miimpute.cpp.

namespace cecon {
namespace cli {

namespace {

// The design here is built by hand from raw columns rather than through
// load_glm, because load_glm's listwise deletion is exactly what we must NOT do
// to the rows being imputed: it would delete them. Its factor expansion is the
// casualty, so factor / interaction / time-series tokens are REJECTED rather than
// silently mis-coded — an honest reject is parity; a silent wrong answer is not.
void reject_expansions(const std::string& name, const char* what) {
    if (name.find_first_of("#()") != std::string::npos || name.find('.') != std::string::npos)
        throw EngineError("unsupported",
                          std::string("mi-impute-regress: ") + what + " '" + name +
                              "' uses a factor / interaction / time-series expression; this verb "
                              "takes plain numeric variable names only (the rows being imputed are "
                              "precisely the rows a factor expansion would drop)");
}

}  // namespace

// cobra mi-impute-regress --data f.hbin --y X2 --x X1,Y --add 2 --seed 20260802 [--out f.json]
int cmd_mi_impute_regress(const std::vector<std::string>& a) {
    std::string data = opt(a, "--data"), y = opt(a, "--y"), xcsv = opt(a, "--x"), out = opt(a, "--out");
    if (data_missing(data) || y.empty() || xcsv.empty()) {
        std::fprintf(stderr,
                     "usage: cobra mi-impute-regress --data f.hbin --y Y --x X1,X2 [--add 5] "
                     "[--seed 42] [--keepif EXPR] [--out f.json]\n"
                     "  Stata `mi impute regress Y = X1 X2, add(#) rseed(#)`: multiple imputation\n"
                     "  of the continuous variable Y by linear regression on X1 X2, using Rubin's\n"
                     "  PROPER scheme — per imputation m,\n"
                     "      sigma*^2 = RSS / chi2(n-k),  beta* = b + sigma* L u  (L L' = (X'X)^-1),\n"
                     "      Y*_i     = x_i' beta* + sigma* eps_i.\n"
                     "  The inner regression runs over the complete cases (Y and every X observed),\n"
                     "  matching Stata's listwise-deletion note; it is reported under `coefs` so a\n"
                     "  driver can print the `noisily` table.\n"
                     "  The imputed VALUES are cobra's own draws on its counter-based Philox stream,\n"
                     "  not Stata's: --seed makes THIS engine reproducible, and every draw is\n"
                     "  addressed (seed, imputation m, counter) so it does not depend on draw order.\n");
        return 2;
    }
    const std::vector<std::string> xs = split_csv(xcsv);
    reject_expansions(y, "--y");
    for (const auto& x : xs) reject_expansions(x, "--x");

    const std::string adds = opt(a, "--add", "5");
    int add = 0;
    try {
        add = std::stoi(adds);
    } catch (const std::exception&) {
        throw EngineError("bad_input", "mi-impute-regress: --add must be a positive integer (got " + adds + ")");
    }
    if (add < 1 || add > 10000)
        throw EngineError("bad_input",
                          "mi-impute-regress: --add must be in [1, 10000] (got " + adds + ")");
    const std::string seeds = opt(a, "--seed", "42");
    uint64_t seed = 0;
    try {
        seed = std::stoull(seeds);
    } catch (const std::exception&) {
        throw EngineError("bad_input",
                          "mi-impute-regress: --seed must be a non-negative integer (got " + seeds + ")");
    }

    data = maybe_keepif(data, opt(a, "--keepif"));
    const Dataset& ds = acquire_data(data);
    const double* yp = require_num(ds, "mi-impute-regress", y);
    std::vector<const double*> xp;
    xp.reserve(xs.size());
    for (const auto& x : xs) xp.push_back(require_num(ds, "mi-impute-regress", x));
    const int64_t n_rows = ds.n_rows();
    const int k = static_cast<int>(xs.size()) + 1;  // + intercept

    MiSample s = mi_classify(yp, xp, n_rows);
    if (s.fit_rows.empty())
        throw EngineError("bad_input",
                          "mi-impute-regress: no complete cases — the imputation model cannot be fit");

    // Designs, col-major with the intercept LAST (load_glm's contract, so the
    // coefficient order every other cobra table uses is preserved).
    const int64_t nf = static_cast<int64_t>(s.fit_rows.size());
    const int64_t ni = static_cast<int64_t>(s.impute_rows.size());
    std::vector<double> Xf(static_cast<size_t>(nf) * static_cast<size_t>(k), 1.0), yf(static_cast<size_t>(nf));
    std::vector<double> Xi(static_cast<size_t>(ni) * static_cast<size_t>(k), 1.0);
    for (int64_t i = 0; i < nf; ++i) {
        const int64_t row = s.fit_rows[static_cast<size_t>(i)];
        yf[static_cast<size_t>(i)] = yp[row];
        for (size_t c = 0; c < xp.size(); ++c) Xf[static_cast<int64_t>(c) * nf + i] = xp[c][row];
    }
    for (int64_t i = 0; i < ni; ++i) {
        const int64_t row = s.impute_rows[static_cast<size_t>(i)];
        for (size_t c = 0; c < xp.size(); ++c) Xi[static_cast<int64_t>(c) * ni + i] = xp[c][row];
    }

    MiImputeResult r = mi_impute_regress(Xf.data(), yf.data(), nf, k, Xi.data(), ni, add, seed);
    if (!r.ok) throw EngineError("bad_input", "mi-impute-regress: " + (r.err.empty() ? std::string("fit failed") : r.err));
    r.sample = s;

    std::vector<std::string> names = xs;
    names.push_back("_cons");
    json coefs = json::array();
    const double tc = t_critical(0.05, static_cast<double>(r.df_resid));
    for (int c = 0; c < k; ++c) {
        const double b = r.beta[static_cast<size_t>(c)], se = r.se[static_cast<size_t>(c)];
        const double t = tstat(b, se);
        coefs.push_back({{"name", names[static_cast<size_t>(c)]},
                         {"b", b},
                         {"se", se},
                         {"t", t},
                         {"p", (se > 0) ? t_twosided_pvalue(t, static_cast<double>(r.df_resid)) : 1.0},
                         {"ci_lo", b - tc * se},
                         {"ci_hi", b + tc * se}});
    }

    // The imputed values, keyed by (imputation, ORIGINAL row index). `rows` is
    // shared by every imputation — the draw never changes WHICH rows are filled,
    // only what goes in them — so it is emitted once instead of per m.
    json rows = json::array();
    for (int64_t j = 0; j < ni; ++j) rows.push_back(s.impute_rows[static_cast<size_t>(j)]);
    json imps = json::array();
    for (int m = 1; m <= add; ++m) {
        json vals = json::array();
        for (int64_t j = 0; j < ni; ++j)
            vals.push_back(r.values[static_cast<size_t>(m - 1) * static_cast<size_t>(ni) + static_cast<size_t>(j)]);
        json bstar = json::array();
        for (int c = 0; c < k; ++c)
            bstar.push_back(r.beta_star[static_cast<size_t>(m - 1) * static_cast<size_t>(k) + static_cast<size_t>(c)]);
        imps.push_back({{"m", m},
                        {"sigma_star", r.sigma_star[static_cast<size_t>(m - 1)]},
                        {"beta_star", bstar},
                        {"values", vals}});
    }

    json o = {{"estimator", "mi-impute-regress"},
              {"method", "regress"},
              {"imputed_var", y},
              // The inner regression Stata prints under `noisily`.
              {"n_obs", r.n_fit},
              {"df_model", r.df_model},
              {"df_resid", r.df_resid},
              {"mss", r.mss},
              {"rss", r.rss},
              {"tss", r.tss},
              {"r2", r.r2},
              {"r2_adj", r.r2_adj},
              {"rmse", r.rmse},
              {"F", r.f_stat},
              {"p_F", r.p_f},
              // The accounting box.
              {"n_total", s.n_total},
              {"n_complete", s.n_complete},
              {"n_incomplete", s.n_incomplete},
              {"n_imputed", ni},
              {"imputations", add},
              {"added", add},
              {"updated", 0},
              {"seed", seeds},
              // Stata's own footnote whenever a predictor is missing anywhere in
              // the complete cases: the model sample is SMALLER than the complete
              // count, and the difference is not an error.
              {"listwise_deletion", s.n_complete != r.n_fit},
              // Rows whose Y is missing but which could NOT be imputed because a
              // predictor is missing too. Stata leaves these missing in every m,
              // which is why "Imputed" can be less than "Incomplete"; reported so
              // the gap is never silent.
              {"n_unimputable", s.n_incomplete - ni},
              {"draws_note",
               "values are cobra's own Philox draws addressed (seed, m, counter): counter 0 = the "
               "chi-square for sigma*, 1..k = the normals behind beta*, k+1+j = the residual normal "
               "for imputed row j — not a reproduction of Stata's stream"},
              {"imputed_rows", rows},
              {"coefs", coefs},
              {"imputations_detail", imps}};

    // --out-data: materialize Stata's `mlong` layout — the original rows carrying
    // `_mi_m == 0`, followed by one copy of every INCOMPLETE row per imputation
    // with `_mi_m == m` and the imputed value in place. Written here rather than
    // reassembled by the caller: the engine already holds both the source frame
    // and the draws, and doing it caller-side would mean round-tripping the
    // values back through a CSV merge — more moving parts for the same result,
    // and a second place for the row order to drift.
    const std::string out_data = opt(a, "--out-data");
    if (!out_data.empty()) {
        Frame base = Frame::from_dataset(ds);
        const int64_t n_rows = base.n_rows();
        std::vector<double> mi_m(static_cast<size_t>(n_rows), 0.0);
        std::vector<double> mi_id(static_cast<size_t>(n_rows));
        std::vector<double> mi_miss(static_cast<size_t>(n_rows), 0.0);
        std::vector<int64_t> incomplete;  // ascending, ALL rows whose y is missing
        for (int64_t i = 0; i < n_rows; ++i) {
            mi_id[static_cast<size_t>(i)] = static_cast<double>(i + 1);
            if (std::isnan(yp[i])) {
                mi_miss[static_cast<size_t>(i)] = 1.0;
                incomplete.push_back(i);
            }
        }
        base.set_f64_column("_mi_m", mi_m);
        base.set_f64_column("_mi_id", mi_id);
        base.set_f64_column("_mi_miss", mi_miss);

        // Map each incomplete row to its slot among the IMPUTABLE ones. A row that
        // is incomplete but not imputable (a predictor is missing too) still gets
        // its mlong copy — Stata keeps it and leaves the value missing in every m —
        // so it maps to no slot and stays NaN rather than silently borrowing the
        // next row's draw.
        std::vector<int64_t> slot(incomplete.size(), -1);
        {
            size_t j = 0;
            for (size_t i = 0; i < incomplete.size() && j < s.impute_rows.size(); ++i)
                if (incomplete[i] == s.impute_rows[j]) slot[i] = static_cast<int64_t>(j++);
        }

        Frame result = base;
        for (int m = 0; m < add; ++m) {
            Frame sub = base;
            // keep_if preserves source row order (it gathers an ascending index
            // list), which is what lets these rows line up positionally with
            // `incomplete` and therefore with the draws.
            sub.keep_if("_mi_miss==1");
            if (sub.n_rows() != static_cast<int64_t>(incomplete.size()))
                throw EngineError("internal",
                                  "mi-impute-regress: the mlong copy selected " +
                                      std::to_string(sub.n_rows()) + " rows, expected " +
                                      std::to_string(incomplete.size()));
            std::vector<double> m_col(incomplete.size(), static_cast<double>(m + 1));
            std::vector<double> y_col(incomplete.size(), std::numeric_limits<double>::quiet_NaN());
            for (size_t i = 0; i < incomplete.size(); ++i) {
                if (slot[i] < 0) continue;
                const size_t k_off = static_cast<size_t>(m) * static_cast<size_t>(r.n_imputed) +
                                     static_cast<size_t>(slot[i]);
                if (k_off < r.values.size()) y_col[i] = r.values[k_off];
            }
            sub.set_f64_column("_mi_m", m_col);
            sub.set_f64_column(y, y_col);
            result = result.append(sub);
        }
        o["out_data"] = out_data;
        o["n_rows_mlong"] = result.n_rows();
        commit_frame(std::move(result), out_data);
    }

    emit(o, out);
    return 0;
}

}  // namespace cli
}  // namespace cecon
