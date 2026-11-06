// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// cecon — Stata time-series operators (L. F. D. S.) as a data-prep primitive.
// Operators are linear polynomials in the lag operator B and all commute, so a
// composed operator (e.g. LDS12.x) is applied as a sequence of single steps with
// NaN propagation. Each step indexes by the TIME variable value (not row order):
// the value at t-k*delta is found via a (panel,time)->row map, so gaps and panel
// boundaries yield missing (never "skip to the previous present row"). Semantics
// from [TS] tsset:  Lk.x=x_{t-k*d}  Fk.x=x_{t+k*d}  Dk.x=(1-B)^k x  Sk.x=x_t-x_{t-k*d}.
#pragma once
#include <cctype>
#include <cmath>
#include <cstdint>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace cecon {

struct TsOpSpec {
    int lag_net = 0;            // net lag (>0) / lead (<0) shift in delta units
    int d_count = 0;           // number of first-difference applications
    std::vector<int> s_lags;   // one seasonal difference per entry, at that lag
    std::string var;           // the variable the operator applies to
    bool ok = false;
    std::string err;
};

// Does `pre` (the dot-separated operator chain before a variable) consist ONLY of
// time-series operator letters (L/F/D/S, case-insensitive, optionally numbered)?
// Tells `L.x` / `L2.x` / `LD.x` (TS) apart from `i.`/`c.`/`b#.` (factor). Single-
// sourced here because THREE callers need the same answer: model.cpp's split_ts
// (varlist pre-pass), and frame.cpp's expression parser (BUG-369), which must make
// the identical L.-vs-not call so `regress y L.y` and `gen ly = L.y` cannot drift.
inline bool is_ts_prefix(const std::string& pre) {
    if (pre.empty()) return false;
    for (size_t i = 0; i < pre.size(); ++i) {
        char c = pre[i];
        if (c == '.') continue;
        char u = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        if (u == 'L' || u == 'F' || u == 'D' || u == 'S') continue;
        if (std::isdigit(static_cast<unsigned char>(c))) continue;
        return false;  // any other letter (i/c/b/o/n/...) -> not a pure TS chain
    }
    return true;
}

// Parse "L2.x", "D2S12.z", "F.y", or a bare "x" (identity). Operator letters are
// case-insensitive; a letter with no number means multiplicity 1; a 0 means
// identity (L0.x=F0.x=D0.x=S0.x=x, needed for numlist forms).
inline TsOpSpec parse_tsop(const std::string& expr) {
    TsOpSpec s;
    auto dot = expr.rfind('.');  // variable is after the LAST dot; the prefix may
                                 // itself contain dots (Stata writes "L.D.x")
    if (dot == std::string::npos) { s.var = expr; s.ok = true; return s; }  // no operator
    std::string pre = expr.substr(0, dot);
    s.var = expr.substr(dot + 1);
    if (s.var.empty()) { s.err = "missing variable after operator"; return s; }
    size_t i = 0;
    while (i < pre.size()) {
        char c = pre[i++];
        if (c == '.') continue;  // separator between composed operators
        char u = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        if (u != 'L' && u != 'F' && u != 'D' && u != 'S') {
            s.err = "unrecognized ts-operator '" + std::string(1, c) + "'"; return s;
        }
        int mult = 1;
        if (i < pre.size() && std::isdigit(static_cast<unsigned char>(pre[i]))) {
            mult = 0;
            while (i < pre.size() && std::isdigit(static_cast<unsigned char>(pre[i]))) {
                if (mult < 100000000) mult = mult * 10 + (pre[i] - '0');  // cap to avoid signed-int overflow
                ++i;
            }
        }
        if (mult == 0) continue;  // identity (L0/F0/D0/S0)
        if (u == 'L') s.lag_net += mult;
        else if (u == 'F') s.lag_net -= mult;
        else if (u == 'D') s.d_count += mult;
        else s.s_lags.push_back(mult);  // S: single seasonal difference at this lag
    }
    s.ok = true;
    return s;
}

// Apply a parsed operator to column x (length n). time is the (f64) time variable;
// panel may be nullptr for a single series. delta is the time step. Result -> out;
// out[r] is NaN wherever a required lagged/lead value is missing (gap, boundary, or
// an upstream NaN). Returns false (with err) if the data is not uniquely keyed.
inline bool ts_apply(const double* x, int64_t n, const double* time, const double* panel,
                     double delta, const TsOpSpec& spec, std::vector<double>& out,
                     std::string& err) {
    std::map<std::pair<double, double>, int64_t> idx;
    for (int64_t r = 0; r < n; ++r) {
        double pk = panel ? panel[r] : 0.0;
        auto key = std::make_pair(pk, time[r]);
        if (!idx.emplace(key, r).second) {
            err = "repeated time values within panel (data not uniquely tsset)";
            return false;
        }
    }
    auto lookup = [&](int64_t r, int k) -> int64_t {  // row at time[r] - k*delta, same panel
        double pk = panel ? panel[r] : 0.0;
        auto it = idx.find(std::make_pair(pk, time[r] - k * delta));
        return (it == idx.end()) ? -1 : it->second;
    };

    std::vector<double> col(x, x + n), next(n);
    auto diff_at = [&](int k) {  // col[t] - col[t-k*delta], NaN-propagating
        for (int64_t r = 0; r < n; ++r) {
            int64_t r2 = lookup(r, k);
            next[r] = (r2 >= 0 && !std::isnan(col[r]) && !std::isnan(col[r2]))
                          ? col[r] - col[r2] : std::nan("");
        }
        col.swap(next);
    };
    for (int sl : spec.s_lags) diff_at(sl);       // seasonal differences
    for (int d = 0; d < spec.d_count; ++d) diff_at(1);  // iterated first differences
    if (spec.lag_net != 0) {                       // net lag / lead shift
        for (int64_t r = 0; r < n; ++r) {
            int64_t r2 = lookup(r, spec.lag_net);
            next[r] = (r2 >= 0) ? col[r2] : std::nan("");
        }
        col.swap(next);
    }
    out = std::move(col);
    return true;
}

}  // namespace cecon
