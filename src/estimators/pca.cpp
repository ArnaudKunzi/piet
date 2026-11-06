// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
#include "estimators/pca.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <utility>

#include "core/error.h"
#include "core/linalg.h"

namespace cecon {

namespace {

// Stata's pca, measured on StataBE; [MV] pca does not
// state these three.
constexpr double kNegligibleEigenRatio = 1e-8;  // of the leading eigenvalue: becomes exactly 0
constexpr double kTraceSnapRelative = 1e-6;     // of |trace|
constexpr double kPsiSnapAbsolute = 1e-6;

// Our own guard, of |leading|: the moment matrix is PSD, so data cannot reach it.
constexpr double kImpossibleNegativeRatio = 1e-5;

// x with its nearest integer substituted when closer to it than `reach`.
double snap_to_integer(double x, double reach) {
    const double nearest = std::round(x);
    return std::fabs(x - nearest) < reach ? nearest : x;
}

// Every allowed row with no missing value in any column, in row order.
std::vector<size_t> listwise_rows(const std::vector<const double*>& columns,
                                  const std::vector<char>& allowed) {
    std::vector<size_t> rows;
    for (size_t i = 0; i < allowed.size(); ++i) {
        if (!allowed[i]) continue;
        const bool complete = std::none_of(columns.begin(), columns.end(),
                                           [i](const double* col) { return std::isnan(col[i]); });
        if (complete) rows.push_back(i);
    }
    return rows;
}

bool constant_over(const double* col, const std::vector<size_t>& rows) {
    return std::all_of(rows.begin(), rows.end(),
                       [&](size_t i) { return col[i] == col[rows.front()]; });
}

// The columns' sample means and the p x p covariance matrix with divisor N - 1.
void sample_moments(const std::vector<const double*>& columns, const std::vector<size_t>& rows,
                    std::vector<double>& means, std::vector<double>& covariance) {
    const size_t p = columns.size();
    const double count = static_cast<double>(rows.size());
    std::vector<std::vector<double>> deviations(p, std::vector<double>(rows.size()));
    means.assign(p, 0.0);
    for (size_t a = 0; a < p; ++a) {
        double total = 0.0;
        for (size_t i : rows) total += columns[a][i];
        means[a] = total / count;
        std::transform(rows.begin(), rows.end(), deviations[a].begin(),
                       [&](size_t i) { return columns[a][i] - means[a]; });
    }
    covariance.assign(p * p, 0.0);
    for (size_t a = 0; a < p; ++a) {
        for (size_t b = a; b < p; ++b) {
            double cross = 0.0;
            for (size_t t = 0; t < rows.size(); ++t) cross += deviations[a][t] * deviations[b][t];
            covariance[a * p + b] = covariance[b * p + a] = cross / (count - 1.0);
        }
    }
}

// Eigenvalues (descending, negligible ones exactly 0) and their eigenvectors of the
// symmetric `matrix`, each vector signed so its elements sum to >= 0 ([MV] pca). A sum
// of exactly 0 has no rule: the solver's orientation stands.
struct Spectrum {
    std::vector<double> values;
    std::vector<std::vector<double>> vectors;
};

Spectrum ordered_spectrum(std::vector<double> matrix, int p) {
    const size_t order = static_cast<size_t>(p);
    std::vector<double> ascending(order);
    if (la::sym_eig(matrix.data(), p, ascending.data()) != 0)
        throw EngineError("numerical", "pca: eigendecomposition failed");

    Spectrum out;
    out.values.assign(ascending.rbegin(), ascending.rend());
    const double leading = out.values.front();
    if (out.values.back() < -kImpossibleNegativeRatio * std::fabs(leading))
        throw EngineError("numerical", "pca: negative eigenvalue found where none expected",
                          "r(198)");
    for (double& value : out.values)
        if (value / leading < kNegligibleEigenRatio) value = 0.0;

    out.vectors.reserve(order);
    for (size_t rank = 0; rank < order; ++rank) {
        const double* stored = matrix.data() + (order - 1 - rank) * order;
        std::vector<double> v(stored, stored + order);
        double element_sum = 0.0;
        for (double element : v) element_sum += element;
        if (element_sum < 0.0)
            for (double& element : v) element = -element;
        out.vectors.push_back(std::move(v));
    }
    return out;
}

}  // namespace

PcaResult pca_fit(const std::vector<std::string>& names, const std::vector<const double*>& cols,
                  const std::vector<char>& keep, const PcaOpts& o) {
    PcaResult fit;

    // Screening, in Stata's measured order: the sample size comes before zero variance.
    const std::vector<size_t> sample = listwise_rows(cols, keep);
    if (sample.empty()) throw EngineError("bad_input", "pca: no observations", "r(2000)");
    if (sample.size() == 1)
        throw EngineError("bad_input", "pca: insufficient observations", "r(2001)");

    std::vector<const double*> analysed;
    for (size_t j = 0; j < cols.size(); ++j) {
        if (constant_over(cols[j], sample)) fit.dropped.push_back(names[j]);
        else analysed.push_back(cols[j]);
    }
    if (analysed.empty())
        throw EngineError("bad_input", "pca: all variables dropped because of zero variance",
                          "r(498)");
    if (analysed.size() == 1)
        throw EngineError("bad_input", "pca: too few variables specified", "r(102)");

    const size_t p = analysed.size();
    fit.n_obs = static_cast<int>(sample.size());
    fit.p = static_cast<int>(p);
    fit.ctype = o.covariance ? "covariance" : "correlation";

    // The analysed matrix.
    sample_moments(analysed, sample, fit.means, fit.c);
    fit.vars.resize(p);
    for (size_t j = 0; j < p; ++j) fit.vars[j] = fit.c[j * p + j];
    if (!o.covariance) {
        fit.sds.resize(p);
        std::transform(fit.vars.begin(), fit.vars.end(), fit.sds.begin(),
                       [](double v) { return std::sqrt(v); });
        for (size_t a = 0; a < p; ++a)
            for (size_t b = 0; b < p; ++b)
                fit.c[a * p + b] = a == b ? 1.0 : fit.c[a * p + b] / (fit.sds[a] * fit.sds[b]);
    }

    const Spectrum spectrum = ordered_spectrum(fit.c, fit.p);
    fit.ev = spectrum.values;

    // Retained components: leading eigenvalues at or above mineigen, within the cap.
    const size_t cap = o.components > 0 ? std::min(static_cast<size_t>(o.components), p) : p;
    size_t retained = 0;
    while (retained < cap && fit.ev[retained] >= o.mineigen) ++retained;
    fit.f = static_cast<int>(retained);

    // Scalars. The trace is the snapped eigenvalue sum, so rho may pass 1.
    double raw_trace = 0.0;
    for (double value : fit.ev) raw_trace += value;
    fit.trace = snap_to_integer(raw_trace, kTraceSnapRelative * std::fabs(raw_trace));

    double explained = 0.0;
    for (size_t j = 0; j < retained; ++j) explained += fit.ev[j];
    fit.rho = retained > 0 ? explained / fit.trace : 0.0;

    if (fit.ev.back() > 0.0) {
        for (double value : fit.ev) fit.lndet += std::log(value);
        fit.cond = std::sqrt(fit.ev.front() / fit.ev.back());
    } else {
        fit.lndet_missing = true;
        fit.cond_missing = true;
    }

    // Loadings, unit length as [MV] pca states; uniqueness is what they leave unexplained.
    fit.loadings.reserve(p * retained);
    fit.psi.resize(p);
    for (size_t row = 0; row < p; ++row) {
        double unexplained = fit.c[row * p + row];
        for (size_t comp = 0; comp < retained; ++comp) {
            const double loading = spectrum.vectors[comp][row];
            fit.loadings.push_back(loading);
            unexplained -= fit.ev[comp] * loading * loading;
        }
        fit.psi[row] = snap_to_integer(unexplained, kPsiSnapAbsolute);
    }

    // e(components): eigenvalue, gap to the next one, share of the trace, running share.
    fit.components_tbl.reserve(p * 4);
    double running_share = 0.0;
    for (size_t j = 0; j < p; ++j) {
        const double share = fit.ev[j] / fit.trace;
        running_share += share;
        const double gap =
            j + 1 < p ? fit.ev[j] - fit.ev[j + 1] : std::numeric_limits<double>::quiet_NaN();
        fit.components_tbl.insert(fit.components_tbl.end(), {fit.ev[j], gap, share, running_share});
    }
    return fit;
}

}  // namespace cecon
