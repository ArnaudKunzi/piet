// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// cecon — nlcom: nonlinear combinations of fitted coefficients with delta-method
// SE. Var(g) = G' V G, G = a numerical Jacobian at beta-hat (see nlcom_apply).
// Coefficients are referenced as _b[name] (and _b[_cons]).
//
// P7: the recursive-descent parser that used to live here was PROMOTED to
// src/core/expr_eval.h so `nl` and `gmm` share it instead of growing a second
// copy of the grammar. nlcom's semantics are unchanged — it compiles with
// allow_params/allow_vars OFF, which is exactly how the old grammar behaved:
//   * `{` was not a token, so it fell through to the number scan and raised
//     "nlcom: parse error"; with allow_params off it still does.
//   * a bare identifier raised "nlcom: bare identifier 'x' (reference
//     coefficients as _b[name])"; that is expr_eval's default hint text.
//   * every other message keeps its wording via Options::tag = "nlcom".
// The tree expr_eval builds has the same shape/associativity as the old direct
// evaluation, so the arithmetic is performed in the same order and the numbers
// are bit-identical (tests/unit/test_nlcom.cpp is the standing check).
//
// THE ONE MEASURED DIFFERENCE, recorded rather than glossed. The old evaluator
// resolved `_b[...]` DURING the parse, so an expression malformed in two ways at
// once — an unknown coefficient AND a syntax error — reported the coefficient
// first. Compiling to a tree finishes parsing before any lookup happens, so the
// syntax error now wins:
//     "_b[nope]*(1"      was "unknown coefficient 'nope'", now "missing ')'"
//     "_b[nope] junk"    was "unknown coefficient 'nope'", now "trailing input near 'junk'"
//     "_b[nope]+sin(1)"  was "unknown coefficient 'nope'", now "unknown function 'sin'"
// Every WELL-FORMED expression and every singly-malformed one is unchanged, text
// included (verified by replaying the CLI error surface against a pre-promotion
// binary). Reporting the syntax error first is also the more useful order.
#pragma once
#include <cmath>
#include <functional>
#include <limits>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

#include "core/expr_eval.h"
#include "estimators/postest_names.h"

namespace cecon {

struct NlcomTerm {
    std::string name;
    double est = 0, se = 0;
    std::vector<double> grad;  // the Jacobian row G (length k) the se was taken from
};

// A `_b[<level>.<var>]` that is not a design column, on a call with no estimation
// sample to classify it (the §31 handle path). The CLI answers exit 3, which sends a
// client back to the full-specification path, where `levels` is known.
struct NlcomNeedsSample : std::runtime_error {
    using std::runtime_error::runtime_error;
};

// Evaluate each "label=expr" (or bare "expr") combination of the coefficients and
// its delta-method SE. `V` is the k*k coefficient covariance (symmetric).
// `_b[name]` resolves through postest::resolve_coef — the one definition test and
// lincom share — so `_b[2.c]` is the level column and `_b[0.c]` (the omitted base
// level of `i.c`, when `levels` says the sample has it) is exactly 0 with a zero
// Jacobian entry. `levels` null keeps the pre-existing behaviour for every name
// that is a column.
inline std::vector<NlcomTerm> nlcom_apply(const std::vector<std::string>& names,
                                          const std::vector<double>& beta,
                                          const std::vector<double>& V,
                                          const std::vector<std::string>& exprs,
                                          const postest::FactorLevels* levels = nullptr);

// The delta-method covariance of the combinations, G V G' (row-major m*m, m =
// terms.size()), from the Jacobian rows nlcom_apply kept. nlcom_apply takes each
// term's variance from this diagonal, so se^2 is the diagonal to the bit, and (q,p)
// is copied from (p,q): symmetric to the bit.
inline std::vector<double> nlcom_covariance(const std::vector<NlcomTerm>& terms,
                                            const std::vector<double>& V) {
    const size_t m = terms.size();
    std::vector<double> out(m * m, 0.0);
    for (size_t p = 0; p < m; ++p) {
        for (size_t q = p; q < m; ++q) {
            const std::vector<double>& Gp = terms[p].grad;
            const std::vector<double>& Gq = terms[q].grad;
            const size_t k = Gp.size();
            double v = 0;
            for (size_t aa = 0; aa < k; ++aa) {
                double gv = 0;
                for (size_t bb = 0; bb < k; ++bb) gv += Gq[bb] * V[aa + bb * k];
                v += Gp[aa] * gv;
            }
            out[p * m + q] = v;
            out[q * m + p] = v;
        }
    }
    return out;
}

namespace nlcom_internal {

// Stata has no infinities: an overflow is a missing value like any other.
inline double as_stata_value(double v) {
    return std::isfinite(v) ? v : std::numeric_limits<double>::quiet_NaN();
}

// The label is what precedes the first '=' (trimmed); without one the term is named
// by its 1-based position. The body keeps its blanks: the expression engine skips them.
inline void split_label(const std::string& raw, size_t position, std::string& label,
                        std::string& body) {
    const size_t eq = raw.find('=');
    if (eq == std::string::npos) {
        label = "_nl_" + std::to_string(position + 1);
        body = raw;
    } else {
        label = expr::trim(raw.substr(0, eq));
        body = raw.substr(eq + 1);
    }
}

// Reads `_b[...]` tokens against a movable coefficient point. Each token is classified
// once; a token that is not a coefficient is reported to the evaluator, which words the
// "unknown coefficient" refusal itself.
class CoefficientPoint {
public:
    CoefficientPoint(const std::vector<std::string>& names, const std::vector<double>& at,
                     const postest::FactorLevels* levels)
        : names_(names), levels_(levels), current_(at) {
        // Assignment lets a repeated name, and a repeated intercept, end on the later column.
        for (size_t c = 0; c < names.size(); ++c) {
            exact_[names[c]] = static_cast<int>(c);
            if (names[c] == "(intercept)") intercept_ = static_cast<int>(c);
        }
    }

    double coordinate(size_t j) const { return current_[j]; }
    void move(size_t j, double to) { current_[j] = to; }

    bool read(const std::string& token, double& value) {
        auto seen = bindings_.find(token);
        if (seen == bindings_.end()) seen = bindings_.emplace(token, classify(token)).first;
        const Binding& b = seen->second;
        if (b.unknown) return false;
        value = b.column >= 0 ? current_[static_cast<size_t>(b.column)] : 0.0;
        return true;
    }

private:
    struct Binding {
        int column = -1;  // -1 with unknown false: an omitted level, fixed at zero
        bool unknown = false;
    };

    Binding classify(const std::string& token) const {
        Binding b;
        const auto hit = exact_.find(token);
        if (hit != exact_.end()) {
            b.column = hit->second;
        } else if ((token == "_cons" || token == "cons") && intercept_ >= 0) {
            b.column = intercept_;
        } else {
            const postest::CoefRef ref = postest::resolve_coef(names_, token, levels_);
            switch (ref.kind) {
                case postest::CoefKind::Column: b.column = ref.idx; break;
                case postest::CoefKind::Omitted: break;
                case postest::CoefKind::NotFound: b.unknown = true; break;
                case postest::CoefKind::NeedsSample:
                    throw NlcomNeedsSample("nlcom: " + ref.name +
                                           " cannot be classified without the estimation sample");
            }
        }
        return b;
    }

    const std::vector<std::string>& names_;
    const postest::FactorLevels* levels_;
    std::vector<double> current_;
    std::unordered_map<std::string, int> exact_;
    int intercept_ = -1;
    std::unordered_map<std::string, Binding> bindings_;
};

// The Jacobian is a central difference whose step is tuned until a one-sided move changes
// f by between 1e-6 and 1e-5 of |f|+1, which balances truncation against cancellation.
// [R] nlcom documents only the iteration limit; the scheme itself (the band, the start
// h0 = .01|x|+.01, the retreat to the left, the band-centre rescale and how it treats a
// left start) was established by black-box measurement on StataBE. The results agree with
// Stata's r(G) to the bit only when every operation is rounded on its own, so each block
// below that does arithmetic opens with this switch: the pragma is scoped to its block.
#if defined(__clang__)
#define NLCOM_ROUND_EACH_OP _Pragma("clang fp contract(off)")
#else
#define NLCOM_ROUND_EACH_OP
#endif

// The first usable reading of f for a coefficient. `side` is -1 when the right-hand
// point was missing and the reading came from the left; `step` stays positive either way.
struct FirstReading {
    double step;
    double value;
    double side;
};

template <class ValueAt>
FirstReading take_first_reading(const ValueAt& value_at, double x) {
    NLCOM_ROUND_EACH_OP
    const double step = std::fabs(x) * 0.01 + 0.01;
    const double right = value_at(x + step);
    if (!std::isnan(right)) return {step, right, 1.0};
    const double left = value_at(x - step);
    if (std::isnan(left)) throw std::runtime_error("nlcom: could not calculate numerical derivative");
    return {step, left, -1.0};
}

// Owns the step and judges readings of f; it never evaluates f itself. Each reading
// is either accepted, or moves the step toward the centre of the band for the next one.
class StepTuner {
public:
    enum class Verdict { Accepted, Rescaled, Unmeasurable };

    StepTuner(double f0, const FirstReading& first) : f0_(f0), step_(first.step) {
        NLCOM_ROUND_EACH_OP
        const double magnitude = std::fabs(f0);
        band_low_ = magnitude * 1e-6 + 1e-6;
        band_high_ = magnitude * 1e-5 + 1e-5;
        // A left start carries its sign into every rescale, so the side alternates (measured).
        signed_centre_ = first.side * ((band_low_ + band_high_) / 2);
    }

    double step() const { return step_; }

    Verdict judge(double reading) {
        NLCOM_ROUND_EACH_OP
        const double moved = as_stata_value(std::fabs(reading - f0_));
        if (moved >= band_low_ && moved <= band_high_) return Verdict::Accepted;
        // The quotient is formed first; the other grouping rounds differently, and Stata's
        // Jacobian bits select this one.
        step_ = signed_centre_ * (step_ / moved);
        // The cap outranks a non-finite step, which means moved was 0 or missing.
        if (rescales_++ > kRescaleCap)
            throw std::runtime_error("nlcom: Maximum number of iterations exceeded.");
        return std::isfinite(step_) ? Verdict::Rescaled : Verdict::Unmeasurable;
    }

private:
    static constexpr int kRescaleCap = 100;  // the documented default of iterate()
    double f0_;
    double step_;
    double band_low_ = 0, band_high_ = 0, signed_centre_ = 0;
    int rescales_ = 0;
};

// d f / d x at x, where f(x) = f0. An f that does not move, or moves unmeasurably, has slope 0.
template <class ValueAt>
double tuned_central_slope(const ValueAt& value_at, double x, double f0) {
    NLCOM_ROUND_EACH_OP
    const FirstReading first = take_first_reading(value_at, x);
    if (first.value == f0) return 0.0;

    StepTuner tuner(f0, first);
    double reading = first.value;
    StepTuner::Verdict verdict = tuner.judge(reading);
    while (verdict == StepTuner::Verdict::Rescaled) {
        reading = value_at(x + tuner.step());
        verdict = tuner.judge(reading);
    }
    if (verdict == StepTuner::Verdict::Unmeasurable) return 0.0;
    const double mirrored = value_at(x - tuner.step());
    return (reading - mirrored) / (2 * tuner.step());
}

#undef NLCOM_ROUND_EACH_OP

}  // namespace nlcom_internal

inline std::vector<NlcomTerm> nlcom_apply(const std::vector<std::string>& names,
                                          const std::vector<double>& beta,
                                          const std::vector<double>& V,
                                          const std::vector<std::string>& exprs,
                                          const postest::FactorLevels* levels) {
    using nlcom_internal::as_stata_value;

    nlcom_internal::CoefficientPoint point(names, beta, levels);
    const expr::Resolver reader = [&point](const std::string& token, double& value) {
        return point.read(token, value);
    };
    expr::Options grammar;
    grammar.tag = "nlcom";
    grammar.allow_params = false;
    grammar.allow_vars = false;
    grammar.allow_coefs = true;

    // Every term is valued at e(b) before any is differentiated: with one term missing
    // there and another whose derivative fails, Stata reports the missing one.
    std::vector<NlcomTerm> terms(exprs.size());
    std::vector<expr::Program> programs;
    programs.reserve(exprs.size());
    for (size_t t = 0; t < exprs.size(); ++t) {
        std::string body;
        nlcom_internal::split_label(exprs[t], t, terms[t].name, body);
        programs.push_back(expr::Program::compile(body, grammar));
        terms[t].est = as_stata_value(programs[t].eval(nullptr, reader));
        // PIET's r(498) mapping keys on this exact wording (no `nlcom: ` prefix).
        if (std::isnan(terms[t].est))
            throw std::runtime_error("expression (" + expr::trim(body) + ") evaluates to missing");
    }

    for (size_t t = 0; t < exprs.size(); ++t) {
        terms[t].grad.resize(beta.size());
        for (size_t j = 0; j < beta.size(); ++j) {
            const auto value_at = [&](double xj) {
                point.move(j, xj);
                const double value = as_stata_value(programs[t].eval(nullptr, reader));
                point.move(j, beta[j]);
                return value;
            };
            terms[t].grad[j] = nlcom_internal::tuned_central_slope(value_at, beta[j], terms[t].est);
        }
    }

    const std::vector<double> delta_cov = nlcom_covariance(terms, V);
    for (size_t t = 0; t < terms.size(); ++t) {
        const double variance = delta_cov[t * terms.size() + t];
        terms[t].se = variance > 0 ? std::sqrt(variance) : 0.0;
    }
    return terms;
}

}  // namespace cecon
