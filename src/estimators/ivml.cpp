// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
#include "ivml.h"

#include <algorithm>
#include <cmath>
#include <functional>
#include <utility>
#include <vector>

#include "logit.h"
#include "probit.h"
#include "tobit.h"

namespace cecon {
namespace {

// The joint log likelihood (ivml.h) on any pair of designs, closed over its outcomes.
using JointDensity = std::function<double(const double* Xs, const double* Xw, int ks, int kw,
                                          const std::vector<double>& theta, std::vector<double>* grad,
                                          std::vector<double>* scores)>;

// ----------------------------------------------------------------------------------
// The likelihood as ml_newton_path sees it: the analytic gradient from ivml.h and a
// numerical Hessian, the pair [M-5] moptimize calls an lf1 evaluator.
//
// Stata differentiates the scores with respect to each equation's linear index, not
// each parameter: one step per equation, sized from the mean index, and each block of
// H is X_j' diag(d s_j / d index_i) X_i. The step and the "later equation responds to
// the earlier one's shift" fill were identified from Stata's optimizer; they decide
// where the iteration stops, so they reach the last digits of every reported number.
//
// To get the scores with respect to an index out of the one set of formulas in ivml.h,
// the density is evaluated on a stand-in model: each design is the two columns
// (index, 1) with coefficients (1, shift). Its score column for the unit regressor is
// then the score with respect to that index, and `shift` is the displacement.
// ----------------------------------------------------------------------------------
class IvJointLikelihood final : public MlObjective {
public:
    // ancillary_order: the theta offsets (past delta and Pi) of the free parameters, in
    // Stata's equation order; that order picks which triangle of H is computed.
    IvJointLikelihood(JointDensity density, const double* Xs, const double* Xw, int64_t n, int ks, int kw,
                      const std::vector<int>& ancillary_order)
        : density_(std::move(density)), Xs_(Xs), Xw_(Xw), n_(n), ks_(ks), kw_(kw),
          np_(ks + kw + static_cast<int>(ancillary_order.size())),
          rows_(static_cast<size_t>(n)),
          unit_(rows_, 1.0),
          stand_in_s_(2 * rows_, 1.0),
          stand_in_w_(2 * rows_, 1.0),
          stand_in_theta_(kFirstFreeSlot + ancillary_order.size(), 0.0),
          slope_(rows_),
          weighted_(rows_) {
        stand_in_theta_[kIndexCoefS] = 1.0;
        stand_in_theta_[kIndexCoefW] = 1.0;
        equations_.push_back({Xs_, ks_, 0, kShiftS});
        equations_.push_back({Xw_, kw_, ks_, kShiftW});
        for (int offset : ancillary_order)
            equations_.push_back({unit_.data(), 1, ks_ + kw_ + offset, kFirstFreeSlot + static_cast<size_t>(offset)});
    }

    double loglik(const std::vector<double>& theta) override {
        return density_(Xs_, Xw_, ks_, kw_, theta, nullptr, nullptr);
    }

    double evaluate(const std::vector<double>& theta, std::vector<double>& g, std::vector<double>& A) override {
        g.resize(static_cast<size_t>(np_));
        const double ll = density_(Xs_, Xw_, ks_, kw_, theta, &g, nullptr);
        neg_hessian(theta, A);
        return ll;
    }

private:
    // Stand-in parameter vector: (1, shift_s, 1, shift_w, free parameters...).
    static constexpr size_t kIndexCoefS = 0, kShiftS = 1, kIndexCoefW = 2, kShiftW = 3, kFirstFreeSlot = 4;

    struct Equation {
        const double* design;  // n x cols; the unit column for a free parameter
        int cols;
        int theta_at;          // first element of theta
        size_t slot;           // what a shift of this equation's index moves in the stand-in
    };

    double index_step(const Equation& eq) const {
        double level;
        if (eq.slot == kShiftS)
            level = la::sum(stand_in_s_.data(), n_) / static_cast<double>(n_);
        else if (eq.slot == kShiftW)
            level = la::sum(stand_in_w_.data(), n_) / static_cast<double>(n_);
        else
            level = stand_in_theta_[eq.slot];
        return (std::fabs(level) + 1e-4) * 1e-4;
    }

    void stand_in_scores(std::vector<double>& out) {
        density_(stand_in_s_.data(), stand_in_w_.data(), 2, 2, stand_in_theta_, nullptr, &out);
    }

    void neg_hessian(const std::vector<double>& theta, std::vector<double>& A) {
        const size_t np = static_cast<size_t>(np_);
        A.assign(np * np, 0.0);
        la::mat_vec(Xs_, n_, ks_, theta.data(), stand_in_s_.data());
        la::mat_vec(Xw_, n_, kw_, theta.data() + ks_, stand_in_w_.data());
        for (size_t e = 2; e < equations_.size(); ++e)
            stand_in_theta_[equations_[e].slot] = theta[static_cast<size_t>(equations_[e].theta_at)];

        for (size_t shifted = 0; shifted < equations_.size(); ++shifted) {
            const Equation& source = equations_[shifted];
            const double h = index_step(source);
            const double centre = stand_in_theta_[source.slot];
            stand_in_theta_[source.slot] = centre + h;
            stand_in_scores(scores_up_);
            stand_in_theta_[source.slot] = centre - h;
            stand_in_scores(scores_down_);
            stand_in_theta_[source.slot] = centre;
            for (size_t responding = shifted; responding < equations_.size(); ++responding)
                fill_block(source, equations_[responding], h, responding == shifted, A);
        }
    }

    // H block (responding, source), both triangles of A from the one computed triangle.
    void fill_block(const Equation& source, const Equation& responding, double h, bool same_equation,
                    std::vector<double>& A) {
        const size_t np = static_cast<size_t>(np_);
        const double* up = scores_up_.data() + responding.slot * rows_;
        const double* down = scores_down_.data() + responding.slot * rows_;
        for (size_t r = 0; r < rows_; ++r) slope_[r] = (up[r] - down[r]) / (2.0 * h);
        block_column_.resize(static_cast<size_t>(responding.cols));
        for (int s = 0; s < source.cols; ++s) {
            const double* xs = source.design + static_cast<size_t>(s) * rows_;
            for (size_t r = 0; r < rows_; ++r) weighted_[r] = slope_[r] * xs[r];
            la::xtv(responding.design, n_, responding.cols, weighted_.data(), block_column_.data());
            const size_t col = static_cast<size_t>(source.theta_at + s);
            for (int t = same_equation ? s : 0; t < responding.cols; ++t) {
                const size_t row = static_cast<size_t>(responding.theta_at + t);
                A[col * np + row] = -block_column_[static_cast<size_t>(t)];
                A[row * np + col] = A[col * np + row];
            }
        }
    }

    JointDensity density_;
    const double* Xs_;
    const double* Xw_;
    int64_t n_;
    int ks_, kw_, np_;
    size_t rows_;
    std::vector<double> unit_;
    std::vector<Equation> equations_;
    std::vector<double> stand_in_s_, stand_in_w_, stand_in_theta_;
    std::vector<double> scores_up_, scores_down_, slope_, weighted_, block_column_;
};

// Stata's documented ml defaults; only the non-concave handling is selected here.
MlMaxOptions joint_fit_options() {
    MlMaxOptions opt;
    opt.spd_concavity = true;
    return opt;
}

// OLS of y2 on Xw: the Pi of both commands' starts and of the exogenous point.
struct ReducedForm {
    std::vector<double> pi, resid;
    double rss = 0.0;
};

bool fit_reduced_form(const double* Xw, int64_t n, int kw, const double* y2, ReducedForm& rf) {
    const size_t cols = static_cast<size_t>(kw);
    std::vector<double> inv_xx(cols * cols), xy(cols);
    la::xtx(Xw, n, kw, inv_xx.data());
    if (la::spd_inverse(inv_xx.data(), kw) != 0) return false;
    la::xtv(Xw, n, kw, y2, xy.data());
    rf.pi.resize(cols);
    la::matvec(inv_xx.data(), kw, xy.data(), rf.pi.data());
    rf.resid.resize(static_cast<size_t>(n));
    la::residual(Xw, n, kw, rf.pi.data(), y2, rf.resid.data());
    rf.rss = la::sumsq(rf.resid.data(), n);
    return rf.rss > 0.0;
}

// theta = (delta, Pi, atanh rho, ln sigma_v [, ln sigma_u]).
std::vector<double> joint_point(const std::vector<double>& delta, const std::vector<double>& pi, double athrho,
                                double ln_sigma_v) {
    std::vector<double> theta = delta;
    theta.insert(theta.end(), pi.begin(), pi.end());
    theta.push_back(athrho);
    theta.push_back(ln_sigma_v);
    return theta;
}

// The ivprobit start from [R] ivprobit's two-step relations: a probit of y1 on
// (Xs's regressors, the reduced-form residual, constant) estimates delta_rho = delta /
// sqrt(1 - rho^2) and lambda_rho = rho / (sigma_v sqrt(1 - rho^2)); solving those with
// sigma_v^2 = RSS / (N - kw) gives the point below. Stata keeps it even when that probit
// runs into perfect prediction, so any finite coefficients count. False sends the
// caller to the exogenous point.
bool control_function_start(const double* Xs, int64_t n, int ks, int kw, const double* y1, const ReducedForm& rf,
                            std::vector<double>& theta) {
    const double var_v = rf.rss / static_cast<double>(n - kw);
    if (!(var_v > 0.0) || !std::isfinite(var_v)) return false;

    const size_t rows = static_cast<size_t>(n);
    const size_t slopes = static_cast<size_t>(ks - 1);
    std::vector<double> design(rows * (slopes + 2));
    std::copy_n(Xs, rows * slopes, design.begin());
    std::copy(rf.resid.begin(), rf.resid.end(), design.begin() + static_cast<std::ptrdiff_t>(rows * slopes));
    std::copy_n(Xs + rows * slopes, rows, design.begin() + static_cast<std::ptrdiff_t>(rows * (slopes + 1)));
    const ProbitResult cf = probit_fit_ml(design.data(), n, ks + 1, y1, MlMaxOptions{}, nullptr,
                                          last_column_is_constant(design.data(), n, ks + 1));
    if (cf.beta.size() != slopes + 2 ||
        !std::all_of(cf.beta.begin(), cf.beta.end(), [](double v) { return std::isfinite(v); }))
        return false;

    const double lambda = cf.beta[slopes];
    const double unscale = 1.0 / std::sqrt(1.0 + lambda * lambda * var_v);
    std::vector<double> delta(static_cast<size_t>(ks));
    for (size_t c = 0; c < slopes; ++c) delta[c] = cf.beta[c] * unscale;
    delta[slopes] = cf.beta[slopes + 1] * unscale;

    // cov(u, v). When rounding puts rho on 1 the implied covariance matrix is not
    // positive definite, and the residual's coefficient is halved until it is.
    double cov = var_v * lambda * unscale;
    for (int halvings = 0; halvings < 10 && !(var_v - cov * cov > 0.0); ++halvings) cov *= 0.5;
    if (!(var_v - cov * cov > 0.0)) return false;

    theta = joint_point(delta, rf.pi, std::atanh(cov / std::sqrt(var_v)), 0.5 * std::log(var_v));
    return true;
}

double diagonal_se(const std::vector<double>& V, int np, int p) {
    return std::sqrt(std::max(0.0, V[static_cast<size_t>(p) * static_cast<size_t>(np) + static_cast<size_t>(p)]));
}

// Everything IvmlResult reports at the iterate where the fit ended. Leaves ok false when
// -H there has no inverse; rho and the sigmas take first-order delta-method SEs.
void report_fit(MlPath path, int ks, int kw, bool has_sigma_u, IvmlResult& r) {
    r.iters = path.ic;
    r.converged = path.converged;
    r.ll = path.ll;
    if (!path.concave) return;

    const int np = ks + kw + (has_sigma_u ? 3 : 2);
    r.ks = ks;
    r.kw = kw;
    r.np = np;
    r.i_athrho = ks + kw;
    r.i_lnsv = ks + kw + 1;
    r.i_lnse = has_sigma_u ? ks + kw + 2 : -1;
    r.theta = std::move(path.b);
    r.vcov = std::move(path.inv_curvature);

    const auto se_of = [&](int p) { return diagonal_se(r.vcov, np, p); };
    const auto first = r.theta.begin();
    r.beta.assign(first, first + ks);
    r.pi.assign(first + ks, first + ks + kw);
    r.se.clear();
    r.se_pi.clear();
    for (int p = 0; p < ks; ++p) r.se.push_back(se_of(p));
    for (int p = 0; p < kw; ++p) r.se_pi.push_back(se_of(ks + p));

    r.athrho = r.theta[static_cast<size_t>(r.i_athrho)];
    r.se_athrho = se_of(r.i_athrho);
    r.rho = std::tanh(r.athrho);
    r.se_rho = (1.0 - r.rho * r.rho) * r.se_athrho;

    r.ln_sigma_v = r.theta[static_cast<size_t>(r.i_lnsv)];
    r.se_ln_sigma_v = se_of(r.i_lnsv);
    r.sigma_v = std::exp(r.ln_sigma_v);
    r.se_sigma_v = r.sigma_v * r.se_ln_sigma_v;
    if (has_sigma_u) {
        r.ln_sigma_e = r.theta[static_cast<size_t>(r.i_lnse)];
        r.se_ln_sigma_e = se_of(r.i_lnse);
        r.sigma_e = std::exp(r.ln_sigma_e);
        r.se_sigma_e = r.sigma_e * r.se_ln_sigma_e;
    }
    r.ok = true;
}

}  // namespace

// ivprobit by full-information ML, [R] ivprobit Methods and formulas.
IvmlResult ivprobit_fit(const double* Xs, const double* Xw, int64_t n, int ks, int kw,
                        const double* y1, const double* y2) {
    IvmlResult r;
    r.n = n;
    if (n <= 0 || ks <= 0 || kw <= 0) return r;
    ReducedForm rf;
    if (!fit_reduced_form(Xw, n, kw, y2, rf)) return r;

    IvJointLikelihood like(
        [=](const double* S, const double* W, int kS, int kW, const std::vector<double>& th,
            std::vector<double>* grad, std::vector<double>* scores) {
            return ivm::ivprobit_ll_grad(S, W, n, kS, kW, y1, y2, th, grad, scores);
        },
        Xs, Xw, n, ks, kw, {0, 1});

    // The exogenous point: plain probit, rho = 0, the reduced form's own sigma_v.
    const ProbitResult plain = probit_fit_ml(Xs, n, ks, y1, MlMaxOptions{}, nullptr,
                                             last_column_is_constant(Xs, n, ks));
    std::vector<double> delta = plain.beta;
    delta.resize(static_cast<size_t>(ks), 0.0);
    std::vector<double> theta =
        joint_point(delta, rf.pi, 0.0, 0.5 * std::log(rf.rss / static_cast<double>(n)));
    r.ll_exog = like.loglik(theta);

    control_function_start(Xs, n, ks, kw, y1, rf, theta);  // else stays at the exogenous point
    report_fit(ml_newton_path(like, std::move(theta), joint_fit_options()), ks, kw, false, r);
    return r;
}

// ivtobit by full-information ML, [R] ivtobit Methods and formulas. Starts from the
// exogenous tobit, the reduced form and rho = 0.
IvmlResult ivtobit_fit(const double* Xs, const double* Xw, int64_t n, int ks, int kw,
                       const double* y1, const double* y2, bool has_ll, double ll_lim,
                       bool has_ul, double ul_lim) {
    IvmlResult r;
    r.n = n;
    if (n <= 0 || ks <= 0 || kw <= 0) return r;

    std::vector<int8_t> censoring(static_cast<size_t>(n), 0);
    for (int64_t i = 0; i < n; ++i) {
        const bool at_lower = has_ll && y1[i] <= ll_lim;
        const bool at_upper = !at_lower && has_ul && y1[i] >= ul_lim;
        censoring[static_cast<size_t>(i)] = at_lower ? 1 : at_upper ? 2 : 0;
        ++(at_lower ? r.n_lc : at_upper ? r.n_rc : r.n_unc);
    }

    ReducedForm rf;
    if (!fit_reduced_form(Xw, n, kw, y2, rf)) return r;
    const TobitResult prefit = tobit_fit(Xs, n, ks, y1, has_ll, ll_lim, has_ul, ul_lim);
    if (!prefit.ok || !(prefit.sigma > 0.0)) return r;

    const int8_t* classes = censoring.data();
    // theta's free parameters run (athrho, ln sigma_v, ln sigma_u); Stata's equations run
    // (athrho, /lnsigma1 = ln sigma_u, /lnsigma2 = ln sigma_v).
    IvJointLikelihood like(
        [=](const double* S, const double* W, int kS, int kW, const std::vector<double>& th,
            std::vector<double>* grad, std::vector<double>* scores) {
            return ivm::ivtobit_ll_grad(S, W, n, kS, kW, y1, y2, classes, ll_lim, ul_lim, th, grad, scores);
        },
        Xs, Xw, n, ks, kw, {0, 2, 1});

    std::vector<double> theta =
        joint_point(prefit.beta, rf.pi, 0.0, 0.5 * std::log(rf.rss / static_cast<double>(n)));
    theta.push_back(std::log(prefit.sigma));
    r.ll_exog = like.loglik(theta);

    report_fit(ml_newton_path(like, std::move(theta), joint_fit_options()), ks, kw, true, r);
    return r;
}

std::vector<double> ivprobit_scores(const double* Xs, const double* Xw, int64_t n, int ks, int kw,
                                    const double* y1, const double* y2, const IvmlResult& r) {
    std::vector<double> S;
    if (!r.ok) return S;
    ivm::ivprobit_ll_grad(Xs, Xw, n, ks, kw, y1, y2, r.theta, nullptr, &S);
    return S;
}

std::vector<double> ivtobit_scores(const double* Xs, const double* Xw, int64_t n, int ks, int kw,
                                   const double* y1, const double* y2, bool has_ll, double ll_lim,
                                   bool has_ul, double ul_lim, const IvmlResult& r) {
    std::vector<double> S;
    if (!r.ok) return S;
    std::vector<int8_t> cls(static_cast<size_t>(n), 0);
    for (int64_t i = 0; i < n; ++i) {
        if (has_ll && y1[i] <= ll_lim) cls[static_cast<size_t>(i)] = 1;
        else if (has_ul && y1[i] >= ul_lim) cls[static_cast<size_t>(i)] = 2;
    }
    ivm::ivtobit_ll_grad(Xs, Xw, n, ks, kw, y1, y2, cls.data(), ll_lim, ul_lim, r.theta, nullptr, &S);
    return S;
}

}  // namespace cecon
