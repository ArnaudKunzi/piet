// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
#include "core/model.h"
#include "core/error.h"
#include "data/ts_operators.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <unordered_map>

namespace cecon {

namespace {

struct KeyCol {
    const Column* c;
    bool is_factor;
};

inline bool valid(const KeyCol& k, int64_t i) {
    return k.is_factor ? (k.c->codes()[i] >= 0) : !std::isnan(k.c->f64()[i]);
}

inline int64_t key_at(const KeyCol& k, int64_t i) {
    if (k.is_factor) return static_cast<int64_t>(k.c->codes()[i]);
    int64_t bits;
    double v = k.c->f64()[i];
    std::memcpy(&bits, &v, sizeof(bits));
    // normalize -0.0 -> +0.0 at the bit level: Stata stores signed zeros as one
    // group key, and a float-level `if (v==0.0) v=0.0;` store is legal for an
    // optimizer to elide (BUG-093 hardening — keying must not depend on flags).
    if (bits == INT64_MIN) bits = 0;
    return bits;
}

// A side-table of materialized columns (e.g. time-series operator outputs) that the
// factor expander can resolve by name in addition to the Dataset's own columns.
using ExtraCols = std::unordered_map<std::string, const double*>;

const double* require_f64(const Dataset& ds, const std::string& name,
                          const ExtraCols* extra = nullptr) {
    if (extra) {
        auto it = extra->find(name);
        if (it != extra->end()) return it->second;  // TS-materialized temp column
    }
    const Column& c = ds.col(name);
    if (c.dtype != DType::F64)
        throw EngineError("factor_misuse", "column '" + name + "' must be numeric (f64)", name);
    return c.f64();
}

Factor make_factor(const std::string& name, const KeyCol& kc,
                   const std::vector<int64_t>& keep) {
    Factor f;
    f.name = name;
    f.codes.resize(keep.size());
    int32_t next = 0;
    if (kc.is_factor) {
        // Source already has dense int codes 0..K-1; remap via a flat array
        // (listwise deletion may drop some levels, so compress to present ones).
        const int32_t* src = kc.c->codes();
        std::vector<int32_t> remap(kc.c->levels.size(), -1);
        for (size_t r = 0; r < keep.size(); ++r) {
            int32_t o = src[keep[r]];
            int32_t d = remap[o];
            if (d < 0) { d = next++; remap[o] = d; }
            f.codes[r] = d;
        }
    } else {
        // f64 key column: hash distinct bit patterns (rare for FE/cluster keys).
        std::unordered_map<int64_t, int32_t> remap;
        remap.reserve(keep.size() / 4 + 16);
        for (size_t r = 0; r < keep.size(); ++r) {
            int64_t key = key_at(kc, keep[r]);
            auto it = remap.find(key);
            int32_t code;
            if (it == remap.end()) { code = next++; remap.emplace(key, code); }
            else code = it->second;
            f.codes[r] = code;
        }
    }
    f.n_levels = next;
    f.count.assign(f.n_levels, 0.0);
    for (int32_t c : f.codes) f.count[c] += 1.0;
    return f;
}

// An absorbed interaction `a#b` (reghdfe/ppmlhdfe absorb(a#b), `i.a#i.b` too):
// the parts, each a plain key column. A `c.` part (heterogeneous slopes) is not a
// grouping and is refused by name.
// BUG-1180: `a##b` of grouping variables expands to a, b and a#b, whose span is a#b's
// alone, and reghdfe absorbs exactly that one group (StataBE 19.5: `reghdfe y x,
// absorb(a##b)` posts e(extended_absvars) "a#b", N_hdfe 1, e(df_a) 12 and the fit of
// absorb(a#b) to the digit), so `##` is read as `#` here; mixed forms likewise span
// the full interaction.
std::vector<std::string> fe_interaction_parts(const std::string& name_in) {
    if (name_in.find('#') == std::string::npos) return {name_in};
    std::string name = name_in;
    for (size_t h = name.find("##"); h != std::string::npos; h = name.find("##", h + 1))
        name.erase(h, 1);
    std::vector<std::string> parts;
    size_t start = 0;
    while (true) {
        const size_t h = name.find('#', start);
        std::string p = name.substr(start, h == std::string::npos ? std::string::npos : h - start);
        if (p.size() > 2 && (p[0] == 'i' || p[0] == 'I') && p[1] == '.') p = p.substr(2);
        if (p.empty() || p.find('.') != std::string::npos)
            throw EngineError("unsupported",
                              "absorb(" + name_in + "): only interactions of grouping variables "
                              "are supported", name_in);
        parts.push_back(p);
        if (h == std::string::npos) break;
        start = h + 1;
    }
    return parts;
}

// The grouping of an absorbed interaction: one level per observed combination of
// the parts' levels (each part keyed exactly as make_factor keys it).
Factor make_interaction_factor(const std::string& name, const std::vector<KeyCol>& kcs,
                               const std::vector<int64_t>& keep) {
    Factor f = make_factor(name, kcs.front(), keep);
    // Compress after each pair so the key stays within int64 even for many
    // high-cardinality parts (each factor's codes are int32).
    for (size_t j = 1; j < kcs.size(); ++j)
        f = intersect_factors(f, make_factor(name, kcs[j], keep));
    f.name = name;
    return f;
}

// ── Factor-variable terms (Stata i. / c. / # / ## / ib#. / ibn. / i(numlist). / o.) ─
// A part = one operand of an interaction. factor=true → i(col) indicators
// (base = lowest level dropped); factor=false → c(col)/bare continuous.
//
// Base selection (Stata `ib#.` / `ibn.`):
//   base_mode=Lowest  → default: drop the smallest level (Stata `i.`).
//   base_mode=Value   → drop the level equal to `base_value` (Stata `ib#.` / `b#.`).
//                       If that value is absent from the data Stata does NOT error:
//                       it warns and falls back to last-level collinearity drop.
//   base_mode=None    → no base requested (Stata `ibn.`). With an intercept the
//                       system is rank-deficient, so Stata auto-drops the LAST
//                       (highest) present level for collinearity. Numerically this
//                       equals base_mode=Value at the highest level.
// Level selection (Stata `i(numlist).`): if has_sel, only levels in `sel` are
//   materialized as indicators; the lowest selected level is the base. Rows whose
//   value is outside `sel` are NOT dropped — they fold into the reference group.
// Omit (Stata `o.`): omit=true contributes ZERO design columns (the variable is
//   "controlled for" only as a label; it adds no regressors and no dof).
enum class FvBase { Lowest, Value, None };
struct FvPart {
    bool factor;
    std::string name;
    const double* src;
    FvBase base_mode = FvBase::Lowest;
    double base_value = 0.0;          // valid when base_mode==Value
    bool has_sel = false;             // i(numlist). active
    std::vector<double> sel;          // requested levels (sorted, deduped)
    bool omit = false;                // o. operator → emits no columns
};
// A term = product of its parts (joined by '#'); a plain main effect has one part.
// pure_interaction marks a standalone `#` (no accompanying main effects): Stata
// then crosses the FULL level sets and omits only the single all-base cell, vs a
// `##`-derived interaction part which crosses the non-base levels.
struct FvTerm { std::vector<FvPart> parts; bool pure_interaction = false; };

std::string fmt_level(double v) {
    char b[32];
    if (v == std::floor(v) && std::fabs(v) < 1e15)
        std::snprintf(b, sizeof(b), "%lld", static_cast<long long>(v));
    else
        std::snprintf(b, sizeof(b), "%g", v);
    return b;
}

std::vector<std::string> split_on(const std::string& s, const std::string& sep) {
    std::vector<std::string> out;
    size_t p = 0;
    while (true) {
        size_t q = s.find(sep, p);
        if (q == std::string::npos) { out.push_back(s.substr(p)); break; }
        out.push_back(s.substr(p, q - p));
        p = q + sep.size();
    }
    return out;
}

// Parse a Stata numlist into an explicit sorted/deduped value set. Supports
//   a/b      inclusive integer range (a..b, step ±1)
//   a(s)b    arithmetic progression from a to b with step s
//   v1 v2 v3 individual values (space-separated, Stata's canonical separator)
//   v1,v2    comma-separated values (tolerated alongside spaces)
// Used by the `i(numlist).var` factor-level selector. A malformed token throws.
std::vector<double> parse_numlist(const std::string& spec, const std::string& ctx) {
    std::vector<double> out;
    auto to_d = [&](const std::string& tok) -> double {
        try {
            size_t pos = 0;
            double v = std::stod(tok, &pos);
            while (pos < tok.size() && std::isspace((unsigned char)tok[pos])) ++pos;
            if (pos != tok.size()) throw std::invalid_argument("trailing");
            return v;
        } catch (...) {
            throw EngineError("bad_input", "malformed factor numlist token '" + tok + "'", ctx);
        }
    };
    // Normalize commas to spaces, then split on whitespace into atoms.
    std::string norm = spec;
    for (char& c : norm) if (c == ',') c = ' ';
    std::vector<std::string> atoms;
    size_t i = 0;
    while (i < norm.size()) {
        while (i < norm.size() && std::isspace((unsigned char)norm[i])) ++i;
        size_t j = i;
        while (j < norm.size() && !std::isspace((unsigned char)norm[j])) ++j;
        if (j > i) atoms.push_back(norm.substr(i, j - i));
        i = j;
    }
    if (atoms.empty())
        throw EngineError("bad_input", "empty factor numlist in i(...)", ctx);
    for (const std::string& a : atoms) {
        size_t slash = a.find('/');
        size_t lpar = a.find('(');
        if (slash != std::string::npos && lpar == std::string::npos) {
            double lo = to_d(a.substr(0, slash));
            double hi = to_d(a.substr(slash + 1));
            double step = (hi >= lo) ? 1.0 : -1.0;
            for (double v = lo; (step > 0 ? v <= hi + 1e-9 : v >= hi - 1e-9); v += step)
                out.push_back(v);
        } else if (lpar != std::string::npos && a.back() == ')') {
            // Stata step syntax is a(s)b, but our atoms are space-split; the common
            // case a(s)b arrives as one atom only when unspaced. Parse "a(s)b".
            size_t rpar = a.find(')', lpar);
            if (rpar == std::string::npos || rpar + 1 >= a.size())
                throw EngineError("bad_input", "malformed numlist step '" + a + "'", ctx);
            double lo = to_d(a.substr(0, lpar));
            double step = to_d(a.substr(lpar + 1, rpar - lpar - 1));
            double hi = to_d(a.substr(rpar + 1));
            if (step == 0.0)
                throw EngineError("bad_input", "zero step in numlist '" + a + "'", ctx);
            if (step > 0)
                for (double v = lo; v <= hi + 1e-9; v += step) out.push_back(v);
            else
                for (double v = lo; v >= hi - 1e-9; v += step) out.push_back(v);
        } else {
            out.push_back(to_d(a));
        }
    }
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}

// Strip a leading factor operator prefix off a bare token and set the FvPart's
// base/selection/omit metadata. Handles (in Stata grammar, `i` optional):
//   ibn.col           → base_mode=None  (no base; last level dropped for collinearity)
//   ib#.col / b#.col  → base_mode=Value (base = level #)
//   i(numlist).col    → has_sel + sel   (only those levels; lowest selected = base)
//   o.col / io#.col   → omit            (zero design columns; io# = omit even as base)
//   c.col             → force continuous (is_continuous = true)
//   i.col / col       → base_mode=Lowest (default)
// Returns the residual column name (everything after the operator + '.').
// `is_factor` is set true when an `i`/`ib`/`ibn`/`o.` prefix forces factor treatment;
// `is_continuous` is set true when a `c.` prefix forces continuous treatment. The two
// are mutually exclusive — at most one fires per token.
static std::string parse_fv_prefix(const std::string& s, FvPart& part, bool& is_factor,
                                   bool& is_continuous) {
    // c.col → explicit continuous operator (Stata `c.`). Forces continuous even
    // inside a '#'/'##' interaction where a bare operand would default to factor.
    if (s.size() > 2 && s.compare(0, 2, "c.") == 0) {
        is_continuous = true;
        return s.substr(2);
    }
    // o.col or io#.col → omit operator.
    if (s.size() > 2 && s.compare(0, 2, "o.") == 0) {
        part.omit = true; is_factor = true;
        return s.substr(2);
    }
    // i(numlist).col → level selection.
    if (s.size() > 2 && s[0] == 'i' && s[1] == '(') {
        size_t rp = s.find(").");
        if (rp != std::string::npos) {
            std::string nl = s.substr(2, rp - 2);
            std::string col = s.substr(rp + 2);
            part.has_sel = true;
            part.sel = parse_numlist(nl, col);
            is_factor = true;
            return col;
        }
        // fall through if no ")." — treat literally below
    }
    // ibn.col → no base.
    if (s.size() > 4 && s.compare(0, 4, "ibn.") == 0) {
        part.base_mode = FvBase::None; is_factor = true;
        return s.substr(4);
    }
    // ib#.col / b#.col → explicit base value (# may be negative or multi-digit).
    auto try_base = [&](size_t off) -> bool {
        // off points just past 'b'. Expect <number>'.'<col>.
        size_t dot = s.find('.', off);
        if (dot == std::string::npos || dot == off) return false;
        std::string num = s.substr(off, dot - off);
        try {
            size_t pos = 0;
            double v = std::stod(num, &pos);
            if (pos != num.size()) return false;
            part.base_mode = FvBase::Value; part.base_value = v; is_factor = true;
            return true;
        } catch (...) { return false; }
    };
    if (s.size() > 3 && s.compare(0, 2, "ib") == 0) {
        // ib#.col
        size_t dot = s.find('.', 2);
        if (dot != std::string::npos && try_base(2)) return s.substr(dot + 1);
    }
    if (s.size() > 2 && s[0] == 'b') {
        // b#.col  (i omitted)
        size_t dot = s.find('.', 1);
        if (dot != std::string::npos && try_base(1)) return s.substr(dot + 1);
    }
    // i.col → explicit default factor.
    if (s.size() > 2 && s.compare(0, 2, "i.") == 0) {
        is_factor = true;
        return s.substr(2);
    }
    return s;  // no recognized prefix
}

// Parse one operand into a part. `bare_factor` controls a bare `col` (no i()/c()
// wrapper): inside a '#'/'##' interaction Stata implies i. (factor); as a plain
// main effect a bare column is continuous (== Stata c.). Stata factor operators
// `ib#.` / `ibn.` / `i(numlist).` / `o.` are recognized here and recorded on the part.
FvPart parse_fvpart(const Dataset& ds, const std::string& s, bool bare_factor,
                    const ExtraCols* extra = nullptr) {
    if (s.size() > 3 && s.compare(0, 2, "i(") == 0 && s.back() == ')' &&
        s.find(").") == std::string::npos) {
        // i(col) — legacy column wrapper (no level numlist, no trailing ".col").
        std::string in = s.substr(2, s.size() - 3);
        FvPart part; part.factor = true; part.name = in; part.src = require_f64(ds, in, extra);
        return part;
    }
    if (s.size() > 3 && s.compare(0, 2, "c(") == 0 && s.back() == ')') {
        std::string in = s.substr(2, s.size() - 3);
        FvPart part; part.factor = false; part.name = in; part.src = require_f64(ds, in, extra);
        return part;
    }
    FvPart part;
    bool forced_factor = false;
    bool forced_continuous = false;
    std::string col = parse_fv_prefix(s, part, forced_factor, forced_continuous);
    // Precedence: an explicit c. forces continuous, an explicit i./ib./o. forces
    // factor; otherwise fall back to the caller's context default (`bare_factor`:
    // true inside a '#'/'##' interaction, false as a plain main effect).
    part.factor = forced_continuous ? false : (forced_factor ? true : bare_factor);
    part.name = col;
    part.src = require_f64(ds, col, extra);
    return part;
}

// Expand a varlist (already comma-split) into factor-variable terms. `i(a)##i(b)`
// is full-factorial (Stata ##): all non-empty subsets of operands, ordered by
// increasing size (→ i.a, i.b, a#b). `#` alone is a single interaction term. A
// token with neither operator is a single main-effect part.
std::vector<FvTerm> parse_fvterms(const Dataset& ds, const std::vector<std::string>& names,
                                  const ExtraCols* extra = nullptr, bool bare_factor = false) {
    std::vector<FvTerm> terms;
    for (const auto& nm : names) {
        if (nm.find("##") != std::string::npos) {
            std::vector<std::string> ops = split_on(nm, "##");
            std::vector<FvPart> parts;
            for (const auto& o : ops) {
                if (o.find('#') != std::string::npos)
                    throw EngineError("bad_input",
                        "mixing '#' and '##' in one term is unsupported", nm);
                parts.push_back(parse_fvpart(ds, o, true, extra));
            }
            int m = static_cast<int>(parts.size());
            std::vector<std::string> seen;  // dedup identical subsets (Stata a##a = a + a#a)
            for (int sz = 1; sz <= m; ++sz)
                for (int mask = 1; mask < (1 << m); ++mask) {
                    if (__builtin_popcount(static_cast<unsigned>(mask)) != sz) continue;
                    FvTerm t;
                    std::string sig;
                    for (int b = 0; b < m; ++b)
                        if (mask & (1 << b)) {
                            t.parts.push_back(parts[static_cast<size_t>(b)]);
                            sig += (parts[b].factor ? "i:" : "c:") + parts[b].name + "#";
                        }
                    if (std::find(seen.begin(), seen.end(), sig) != seen.end()) continue;
                    seen.push_back(sig);
                    terms.push_back(std::move(t));
                }
        } else if (nm.find('#') != std::string::npos) {
            FvTerm t;
            t.pure_interaction = true;  // standalone # → full cross minus all-base cell
            for (const auto& o : split_on(nm, "#")) t.parts.push_back(parse_fvpart(ds, o, true, extra));
            terms.push_back(std::move(t));
        } else {
            FvTerm t;
            t.parts.push_back(parse_fvpart(ds, nm, bare_factor, extra));
            terms.push_back(std::move(t));
        }
    }
    // A factor-variable term repeated across the varlist is ONE term at its first
    // position, which implements Stata's varlist expansion per StataBE probes
    // (scratch-r150/c1067/m2.do, m3.do; regress, reghdfe, areg and xtreg,fe alike):
    // `c.x##i.t s##i.t` carries i.t once (`x 0b.t 1.t 2.t ... 0b.s 1.s ...`),
    // `i.t x i.t` is `0b.t 1.t 2.t x`, and `i.t##i.s i.s##i.t` collapses s#t into
    // t#s. A term of continuous parts only is NOT collapsed: `c.x c.x` and the c.x
    // of `c.x##i.t c.x##i.s` repeat as an omitted `o.x`. Without this the repeated
    // column was a second, collinear copy under the SAME name, and the reply's
    // omitted entry overwrote the kept coefficient with 0 (package 007's
    // `a##treatment b##treatment ...`: _b[1.treatment] reported 0). Parts are
    // compared as a set (operand order does not matter) together with their base
    // and level selection, so `i.t ib1.t` stays two terms.
    std::vector<FvTerm> unique;
    std::vector<std::string> seen;
    unique.reserve(terms.size());
    for (auto& t : terms) {
        bool any_factor = false;
        std::vector<std::string> keys;
        for (const auto& p : t.parts) {
            any_factor = any_factor || p.factor;
            std::string k = (p.factor ? "i:" : "c:") + p.name + "|" +
                            std::to_string(static_cast<int>(p.base_mode)) + ":" +
                            fmt_level(p.base_value) + "|" + (p.omit ? "o" : "") + "|";
            if (p.has_sel)
                for (double v : p.sel) k += fmt_level(v) + ",";
            keys.push_back(std::move(k));
        }
        if (any_factor) {
            std::sort(keys.begin(), keys.end());
            std::string sig = t.pure_interaction ? "#|" : "##|";
            for (const auto& k : keys) sig += k + "&";
            if (std::find(seen.begin(), seen.end(), sig) != seen.end()) continue;
            seen.push_back(std::move(sig));
        }
        unique.push_back(std::move(t));
    }
    return unique;
}

// All source columns referenced by the terms (for listwise NaN deletion).
std::vector<const double*> fvterm_srcs(const std::vector<FvTerm>& terms) {
    std::vector<const double*> s;
    for (const auto& t : terms) for (const auto& p : t.parts) s.push_back(p.src);
    return s;
}

// Resolve which indices of a factor part's sorted distinct data `levels` are
// materialized as indicator columns (i.e. the non-base, in-selection levels),
// and which index is the base. Encodes Stata's `ib#.` / `ibn.` / `i(numlist).`:
//
//   default (Lowest)    : base = index 0; emit indices 1..K-1.
//   ib#. (Value present) : base = the index whose value == base_value; emit the rest.
//   ib#. (Value absent)  : Stata warns + falls back to last-level collinearity drop
//                          → base = last index; emit 0..K-2. (numerically == ibn.)
//                          StataBE 19.5 oracle 2026-07-31 (BUG-112(8), 5 arms:
//                          regress/logit × ib99./ib0./fvset base): rc=0 on every
//                          arm — Stata does NOT reject — and emits
//                          `note: 99b.rep78 identifies no observations in the
//                          sample.`. The COEFFICIENTS match this fallback exactly
//                          (Stata keeps 99 as an empty base column and omits the
//                          last level for collinearity instead; same estimates,
//                          different term inventory — the inventory gap is filed
//                          separately). `base_absent` carries the note out.
//   ibn. (None)          : with a model intercept the system is rank-deficient, so
//                          Stata drops the LAST level for collinearity → same as the
//                          Value-absent fallback: base = last index, emit 0..K-2.
//   i(numlist).          : restrict to levels in `sel` that are present in the data;
//                          base = the lowest such present level; emit the remaining
//                          selected present levels. Levels outside `sel` are neither
//                          base nor emitted — rows holding them fold into the
//                          reference (intercept) group, matching Stata (NOT a sample
//                          restriction).
//
// `base_idx` is set to -1 when there is no in-data base (only happens if `sel`
// selects exactly one present level → that single level is the base and nothing is
// emitted, i.e. the term is dead). Returns the emit indices (possibly empty).
struct LevelPlan {
    std::vector<int> emit;
    int base_idx;
    // BUG-112(8): `ib#.` named a base level that no kept row holds. The plan still
    // falls back to the last level (Stata-equivalent estimates); this flag lets the
    // caller emit Stata's note instead of staying silent.
    bool base_absent = false;
    double requested_base = 0.0;      // valid when base_absent
    double used_base = 0.0;           // the level actually used as base
};
static LevelPlan resolve_levels(const FvPart& part, const std::vector<double>& levels) {
    const int K = static_cast<int>(levels.size());
    LevelPlan plan; plan.base_idx = -1;
    if (part.omit) return plan;  // o. → no base, no emit (handled as dead upstream)

    // Restrict the candidate index set to the selected levels (if any).
    std::vector<int> cand;
    if (part.has_sel) {
        for (int i = 0; i < K; ++i)
            if (std::binary_search(part.sel.begin(), part.sel.end(), levels[i]))
                cand.push_back(i);
        if (cand.empty()) return plan;  // no selected level present → dead term
    } else {
        for (int i = 0; i < K; ++i) cand.push_back(i);
    }

    // Pick the base index within `cand`.
    int base = -1;
    switch (part.base_mode) {
        case FvBase::Value: {
            for (int i : cand) if (levels[i] == part.base_value) { base = i; break; }
            if (base < 0) {
                base = cand.back();  // absent base → last-level fallback
                plan.base_absent = true;
                plan.requested_base = part.base_value;
            }
            break;
        }
        case FvBase::None:
            base = cand.back();               // ibn. → drop last for collinearity
            break;
        case FvBase::Lowest:
        default:
            base = cand.front();              // lowest (selected) level
            break;
    }
    plan.base_idx = base;
    plan.used_base = levels[base];
    for (int i : cand) if (i != base) plan.emit.push_back(i);
    return plan;
}

// Count design columns and collect each factor part's sorted distinct levels over
// the kept rows; also resolve each factor part's emit-index plan (`plans`). A
// continuous part contributes ×1; an omitted (`o.`) factor part kills its term.
int fvterm_ncols(const std::vector<FvTerm>& terms, const std::vector<int64_t>& keep,
                 std::vector<std::vector<std::vector<double>>>& levels,
                 std::vector<std::vector<LevelPlan>>& plans) {
    levels.assign(terms.size(), {});
    plans.assign(terms.size(), {});
    int nc = 0;
    for (size_t t = 0; t < terms.size(); ++t) {
        levels[t].assign(terms[t].parts.size(), {});
        plans[t].assign(terms[t].parts.size(), {});
        int cols = 1;
        bool term_dead = false;
        for (size_t p = 0; p < terms[t].parts.size(); ++p) {
            if (!terms[t].parts[p].factor) continue;  // continuous part: ×1
            std::vector<double> vals;
            vals.reserve(keep.size());
            for (int64_t r : keep) vals.push_back(terms[t].parts[p].src[r]);
            std::sort(vals.begin(), vals.end());
            vals.erase(std::unique(vals.begin(), vals.end()), vals.end());
            // Stata r(452) on the estimation sample, the first offending variable in
            // varlist order, noninteger checked before negative (StataBE: x = k/4 in i.x,
            // x#z or c.(x#z): "x:  factor variables may not contain noninteger values";
            // -2..2: "... negative values"; a variable with both: noninteger; fine under
            // `if x == floor(x)`).
            const std::string& pn = terms[t].parts[p].name;
            for (double v : vals)
                if (v != std::floor(v))
                    throw EngineError("factor_values",
                                      pn + ":  factor variables may not contain noninteger values", pn);
            if (!vals.empty() && vals.front() < 0)
                throw EngineError("factor_values",
                                  pn + ":  factor variables may not contain negative values", pn);
            LevelPlan lp = resolve_levels(terms[t].parts[p], vals);
            // o. omit, or a selection/level set that yields no emittable level,
            // collapses the whole term to zero columns (Stata "(omitted)").
            if (terms[t].parts[p].omit || lp.emit.empty()) term_dead = true;
            cols *= static_cast<int>(lp.emit.size());
            levels[t][p] = std::move(vals);
            plans[t][p] = std::move(lp);
        }
        if (!term_dead) nc += cols;
    }
    return nc;
}

// Expand terms into concrete (name, column-over-kept-rows) design columns. For an
// interaction, columns are the Cartesian product of each factor part's non-base
// levels (first part outermost), each value = product of the level indicators
// times any continuous parts. Structurally empty interaction cells (all-zero
// columns) are dropped — matching Stata's automatic empty-cell omission.
// `warnings` (optional, BUG-112(8)): when non-null, model-spec notes raised while
// resolving the design are APPENDED (never cleared) — an IV build calls this twice
// and both passes accumulate into the same sink.
std::vector<std::pair<std::string, std::vector<double>>>
fvterm_columns(const std::vector<FvTerm>& terms, const std::vector<int64_t>& keep,
               std::vector<std::string>* warnings = nullptr,
               std::vector<size_t>* base_cells = nullptr) {
    std::vector<std::vector<std::vector<double>>> levels;
    std::vector<std::vector<LevelPlan>> plans;
    fvterm_ncols(terms, keep, levels, plans);  // levels + per-part emit/base plan
    // BUG-112(8): Stata warns before falling back; mirror the note. Built here (not
    // in resolve_levels) because only this layer holds the part's name.
    if (warnings) {
        for (size_t t = 0; t < terms.size(); ++t)
            for (size_t p = 0; p < terms[t].parts.size(); ++p) {
                if (!terms[t].parts[p].factor) continue;
                const LevelPlan& lp = plans[t][p];
                if (!lp.base_absent) continue;
                warnings->push_back(
                    "ib" + fmt_level(lp.requested_base) + "." + terms[t].parts[p].name +
                    ": base level " + fmt_level(lp.requested_base) +
                    " identifies no observations in the sample; level " +
                    fmt_level(lp.used_base) + " used as the base instead");
            }
    }
    const int64_t n = static_cast<int64_t>(keep.size());
    // BUG-1476: every term of the model as its set of atoms (`i:a`, `c:x`), for the
    // base-cell rule of a standalone '#' below.
    auto atom = [](const FvPart& p) { return std::string(p.factor ? "i:" : "c:") + p.name; };
    auto atoms_sig = [](std::vector<std::string> a) {
        std::sort(a.begin(), a.end());
        std::string s;
        for (const auto& x : a) s += x + "#";
        return s;
    };
    std::vector<std::string> model_terms;
    for (const auto& term : terms) {
        bool o = false;
        std::vector<std::string> a;
        for (const auto& p : term.parts) {
            o = o || (p.factor && p.omit);
            a.push_back(atom(p));
        }
        if (!o) model_terms.push_back(atoms_sig(std::move(a)));
    }
    std::vector<std::pair<std::string, std::vector<double>>> out;
    for (size_t t = 0; t < terms.size(); ++t) {
        const auto& parts = terms[t].parts;
        const bool pure = terms[t].pure_interaction;
        bool has_cont = false;
        for (const auto& p : parts) if (!p.factor) has_cont = true;
        // An o. omit on any factor part suppresses the whole term (zero columns).
        bool omitted = false;
        for (const auto& p : parts) if (p.factor && p.omit) omitted = true;
        if (omitted) continue;
        // Per factor part, the level indices to materialize. A standalone '#'
        // (pure) crosses ALL data levels and later omits the base cells the rule
        // below names (at least the all-base cell of a pure-factor '#');
        // otherwise we emit exactly the resolved non-base / in-selection levels.
        std::vector<size_t> fp;                 // positions of the factor parts
        std::vector<std::vector<int>> lidx;     // chosen level indices per factor part
        std::vector<int> base_of;               // base index per factor part (for pure all-base cell)
        long long combos = 1;
        bool dead = false;
        for (size_t p = 0; p < parts.size(); ++p) {
            if (!parts[p].factor) continue;
            const LevelPlan& lp = plans[t][p];
            std::vector<int> ids;
            if (pure) {
                int K = static_cast<int>(levels[t][p].size());
                for (int l = 0; l < K; ++l) ids.push_back(l);  // full cross
            } else {
                ids = lp.emit;                                 // non-base / selected
            }
            if (ids.empty()) { dead = true; break; }  // nothing to emit → drop term
            fp.push_back(p); lidx.push_back(std::move(ids)); base_of.push_back(lp.base_idx);
            combos *= static_cast<long long>(lidx.back().size());
        }
        if (dead) continue;
        std::vector<int> sel(fp.size(), 0);
        for (long long cmb = 0; cmb < combos; ++cmb) {
            // standalone '#': a cell with some factor parts at their own resolved base
            // level is omitted when the term left after dropping some of those parts is
            // in the model anywhere in the varlist -- or is empty (the constant), which
            // is the single all-base cell of a pure-factor '#'. BUG-1476, StataBE
            // fvexpand: `c.x#i.g c.x` -> `0b.g#co.x 1.g#c.x x`; `i.a i.a#i.g` ->
            // `... 0b.a#0b.g 0b.a#1.g 1o.a#0b.g 1.a#1.g 2o.a#0b.g 2.a#1.g`; `c.x#i.g`
            // alone keeps both cells.
            bool omit_cell = false;
            if (pure && !fp.empty()) {
                std::vector<size_t> at_base;  // indices into fp
                for (size_t k = 0; k < fp.size(); ++k)
                    if (lidx[k][sel[k]] == base_of[k]) at_base.push_back(k);
                for (unsigned m = 1; m < (1u << at_base.size()) && !omit_cell; ++m) {
                    std::vector<std::string> rest;
                    for (size_t p = 0; p < parts.size(); ++p) {
                        bool dropped = false;
                        for (size_t b = 0; b < at_base.size(); ++b)
                            if (((m >> b) & 1u) && fp[at_base[b]] == p) dropped = true;
                        if (!dropped) rest.push_back(atom(parts[p]));
                    }
                    omit_cell = rest.empty() ||
                                std::find(model_terms.begin(), model_terms.end(),
                                          atoms_sig(rest)) != model_terms.end();
                }
            }
            if (!omit_cell) {
                std::string nm;
                for (size_t p = 0; p < parts.size(); ++p) {
                    if (!nm.empty()) nm += "#";
                    if (parts[p].factor) {
                        size_t k = 0; while (fp[k] != p) ++k;
                        // BUG-101: Stata factor-term naming is `level.varname`
                        // (e.g. 10.state_num), and `level.var#level.var` for
                        // factor interactions — NOT `varname==level`.
                        nm += fmt_level(levels[t][p][lidx[k][sel[k]]]) + "." + parts[p].name;
                    } else {
                        nm += parts[p].name;
                    }
                }
                std::vector<double> colv(static_cast<size_t>(n));
                bool allzero = true;
                for (int64_t r = 0; r < n; ++r) {
                    int64_t row = keep[static_cast<size_t>(r)];
                    double v = 1.0;
                    for (size_t p = 0; p < parts.size() && v != 0.0; ++p) {
                        if (parts[p].factor) {
                            size_t k = 0; while (fp[k] != p) ++k;
                            v *= (parts[p].src[row] == levels[t][p][lidx[k][sel[k]]]) ? 1.0 : 0.0;
                        } else {
                            v *= parts[p].src[row];
                        }
                    }
                    colv[static_cast<size_t>(r)] = v;
                    if (v != 0.0) allzero = false;
                }
                if (!allzero) {
                    // A pure cell holding some factor's base level (ModelData::late_cells).
                    if (base_cells != nullptr && pure && !has_cont)
                        for (size_t k = 0; k < fp.size(); ++k)
                            if (lidx[k][sel[k]] == base_of[k]) { base_cells->push_back(out.size()); break; }
                    out.emplace_back(std::move(nm), std::move(colv));
                }
            }
            for (int k = static_cast<int>(fp.size()) - 1; k >= 0; --k) {
                if (++sel[k] < static_cast<int>(lidx[k].size())) break;
                sel[k] = 0;
            }
        }
    }
    return out;
}

// ---- Time-series operator pre-pass (shared by build_model / build_iv_model /
//      expand_varlist) ------------------------------------------------------------
// split_ts lives OUTSIDE this anonymous namespace (just below it, see
// "Time-series token detector" below) — it is declared in model.h and used by
// cecon_cli.cpp's keepif-aware pre-materialization (E1), so it needs external
// linkage. Its helper is_ts_prefix is inline in data/ts_operators.h (shared with
// frame.cpp's expression parser). split_ts is still defined ahead of any OTHER
// translation unit's use point via the model.h declaration seen at the top of this
// file, and ts_prepass below calls split_ts exactly as before.

// Shared time-series pre-pass. For each token carrying an L./F./D./S. operator,
// materialize the operated column once (panel/time-aware) into `storage`, register
// a clean synthetic name in `extra`, and rewrite the token (keeping any factor
// prefix) so the factor pass sees a plain column name — so `i.L.x` still factorizes
// and a bare `L.x` becomes a plain column. Tokens without a TS operator pass through
// unchanged. `synth_to_real` maps each synthetic id back to its source token for
// human-readable output names.
std::vector<std::string> ts_prepass(const Dataset& ds, const std::vector<std::string>& names,
                                    const TsMeta* ts, std::vector<std::vector<double>>& storage,
                                    ExtraCols& extra,
                                    std::vector<std::pair<std::string, std::string>>& synth_to_real) {
    const int64_t n = ds.n_rows();
    std::vector<std::string> rewritten;
    rewritten.reserve(names.size());
    const double* tvar = nullptr;
    const double* pvar = nullptr;
    bool ts_ready = false;
    auto ensure_ts_meta = [&]() {
        if (ts_ready) return;
        if (!ts || ts->timevar.empty())
            throw EngineError("ts_no_tsset",
                "time-series operator (L./F./D./S.) used but no time variable set; "
                "pass the time var (and panel id for panel data) via tsset/xtset metadata", "");
        tvar = require_f64(ds, ts->timevar);
        pvar = ts->panelvar.empty() ? nullptr : require_f64(ds, ts->panelvar);
        ts_ready = true;
    };
    int ts_counter = 0;
    for (const auto& tok : names) {
        TsSplit sp = split_ts(tok);
        if (!sp.has_ts) { rewritten.push_back(tok); continue; }
        ensure_ts_meta();
        TsOpSpec spec = parse_tsop(sp.ts_token);
        if (!spec.ok)
            throw EngineError("ts_bad_operator",
                "bad time-series operator in '" + tok + "': " + spec.err, tok);
        const double* base = require_f64(ds, spec.var);
        std::vector<double> out;
        std::string err;
        if (!ts_apply(base, n, tvar, pvar, ts ? ts->delta : 1.0, spec, out, err))
            throw EngineError("ts_apply", "time-series operator '" + tok + "': " + err, tok);
        // BUG-1277: Stata holds an operated variable in a FLOAT temporary when its
        // source is byte, int or float (StataBE tsrevar: D./S./D2./F. of a float and
        // D. of a byte or int are float, of a long or double double), the operator
        // evaluated in double and rounded once (D2.yf == float(yf - 2*L.yf + L2.yf),
        // DS2.yf alike). The .hbin carries no storage type, so a source whose every
        // value is a float is taken for one. Only a difference can leave float; a lag
        // or lead of float values stays float as is.
        if (spec.d_count > 0 || !spec.s_lags.empty()) {
            auto is_float = [](double v) {
                return std::fabs(v) <= 3.4028234663852886e38 &&
                       static_cast<double>(static_cast<float>(v)) == v;
            };
            bool float_src = true;
            for (int64_t i = 0; i < n && float_src; ++i)
                if (!std::isnan(base[i]) && !is_float(base[i])) float_src = false;
            if (float_src)
                for (double& v : out)
                    if (!std::isnan(v) && std::fabs(v) <= 3.4028234663852886e38)
                        v = static_cast<double>(static_cast<float>(v));
        }
        std::string synth = "__ts" + std::to_string(ts_counter++) + "_" + sp.ts_token;
        storage.push_back(std::move(out));
        extra[synth] = storage.back().data();
        synth_to_real.emplace_back(synth, sp.ts_token);
        rewritten.push_back(sp.fac_prefix + synth);
    }
    return rewritten;
}

// Rewrite synthetic TS names back to readable source tokens in a column name.
void restore_ts_names(std::string& nm,
                      const std::vector<std::pair<std::string, std::string>>& synth_to_real) {
    for (const auto& kv : synth_to_real) {
        size_t pos = nm.find(kv.first);
        if (pos != std::string::npos) nm.replace(pos, kv.first.size(), kv.second);
    }
}

}  // namespace

// ---- Time-series token detector (external linkage — see model.h) ----------------
// is_ts_prefix now lives in data/ts_operators.h (inline) so the varlist pre-pass and
// frame.cpp's expression parser (BUG-369) make the SAME L.-vs-factor call from one
// definition; `L.x` accepted in `regress y L.x` but rejected in `gen z = L.x` was
// exactly the drift a second copy invites.

// Split a token into an optional leading factor prefix and a trailing operand. If
// the operand carries a time-series operator chain (e.g. the `L.` in `i.L.x` or a
// bare `L.x`), returns {factor_prefix, ts_token, base_var, has_ts}. factor_prefix
// is "" for a pure-TS token. For `i.L.gdp`: prefix="i.", ts="L.gdp", base="gdp".
// Declared in model.h (TsSplit + split_ts) with EXTERNAL linkage: ts_prepass (above,
// inside the anonymous namespace) uses it via that header declaration, and
// cecon_cli.cpp's keepif-aware pre-materialization (E1) calls it directly to detect
// TS tokens in a varlist before any data is opened.
TsSplit split_ts(const std::string& tok) {
    TsSplit r;
    // Strip a leading factor prefix (i. / c. / o. / ibN. / ibn. / bN.) if present;
    // the remainder is the operand the TS detector inspects.
    std::string operand = tok;
    auto starts = [&](const char* p) { return tok.compare(0, std::strlen(p), p) == 0; };
    size_t cut = 0;
    if (starts("i.")) cut = 2;
    else if (starts("c.")) cut = 2;
    else if (starts("o.")) cut = 2;
    else if (starts("ibn.")) cut = 4;
    else if (starts("ib")) { size_t d = tok.find('.', 2); if (d != std::string::npos) cut = d + 1; }
    else if (tok.size() > 1 && tok[0] == 'b' && std::isdigit(static_cast<unsigned char>(tok[1]))) {
        size_t d = tok.find('.', 1); if (d != std::string::npos) cut = d + 1;
    }
    if (cut > 0) { r.fac_prefix = tok.substr(0, cut); operand = tok.substr(cut); }
    // Does `operand` carry a TS operator? operand = pre + '.' + var, pre all-TS.
    size_t dot = operand.rfind('.');
    if (dot != std::string::npos) {
        std::string pre = operand.substr(0, dot);
        if (is_ts_prefix(pre)) {
            r.has_ts = true;
            r.ts_token = operand;
            r.base_var = operand.substr(dot + 1);
            return r;
        }
    }
    return r;  // no TS operator
}

// C3-F3: compact a column-major n×k matrix to its first n_alive rows IN PLACE,
// selecting rows via `idx` (idx[r] = original row index of new row r, r in
// [0,n_alive)). Requires idx to be a strictly increasing subsequence of [0,n) —
// true of every idx built by scanning i=0..n-1 and keeping a subset — so
// idx[r] >= r for all r. Combined with n_alive <= n, the write cursor
// (c*n_alive+r) never overtakes the read cursor (c*n+idx[r]) as (c,r) advance in
// this loop's order: every source value is read before this or any later
// iteration could overwrite it. So overwriting X's own storage in place is safe
// and produces bit-identical output to gathering into a fresh n_alive*k buffer
// and moving it over (same values, same order) — it just never holds both
// alive at once. Deliberately leaves capacity at the pre-compaction size (no
// shrink_to_fit): reducing capacity would itself require a temporary
// reallocation holding the old (n*k) and new (n_alive*k) storage
// simultaneously, reintroducing on this path the exact transient this function
// exists to remove. The retained capacity was already part of X's pre-existing
// footprint, so keeping it does not add a new peak.
static void compact_cols_inplace(std::vector<double>& X, int64_t n, int k,
                                 const std::vector<int64_t>& idx, int64_t n_alive) {
    double* base = X.data();
    for (int c = 0; c < k; ++c) {
        const double* src = base + static_cast<size_t>(c) * n;
        double* dst = base + static_cast<size_t>(c) * n_alive;
        for (int64_t r = 0; r < n_alive; ++r) dst[r] = src[idx[static_cast<size_t>(r)]];
    }
    X.resize(static_cast<size_t>(n_alive) * k);
}

int64_t drop_zero_outcome_fe_cells(ModelData& md) {
    if (md.fe.empty()) return 0;
    const int64_t n = md.dm.n;
    const int k = md.dm.k;
    std::vector<char> alive(static_cast<size_t>(n), 1);
    int64_t n_alive = n;
    bool changed = true;
    while (changed) {
        changed = false;
        for (const Factor& f : md.fe) {
            std::vector<double> ysum(static_cast<size_t>(f.n_levels), 0.0);
            std::vector<int64_t> cnt(static_cast<size_t>(f.n_levels), 0);
            for (int64_t i = 0; i < n; ++i)
                if (alive[static_cast<size_t>(i)]) {
                    int32_t c = f.codes[static_cast<size_t>(i)];
                    ysum[static_cast<size_t>(c)] += md.dm.y[static_cast<size_t>(i)];
                    cnt[static_cast<size_t>(c)]++;
                }
            for (int64_t i = 0; i < n; ++i)
                if (alive[static_cast<size_t>(i)]) {
                    int32_t c = f.codes[static_cast<size_t>(i)];
                    if (cnt[static_cast<size_t>(c)] > 0 && ysum[static_cast<size_t>(c)] == 0.0) {
                        alive[static_cast<size_t>(i)] = 0; --n_alive; changed = true;
                    }
                }
        }
    }
    if (n_alive == n) return 0;

    std::vector<int64_t> idx;
    idx.reserve(static_cast<size_t>(n_alive));
    for (int64_t i = 0; i < n; ++i) if (alive[static_cast<size_t>(i)]) idx.push_back(i);

    // Rebuild design (column-major), y, offset, keep. X compacts in place (see
    // compact_cols_inplace above) instead of gathering into a second n*k-sized
    // buffer that would be alive alongside the original for the whole gather.
    compact_cols_inplace(md.dm.X, n, k, idx, n_alive);
    std::vector<double> y2(static_cast<size_t>(n_alive));
    for (int64_t r = 0; r < n_alive; ++r) y2[static_cast<size_t>(r)] = md.dm.y[static_cast<size_t>(idx[static_cast<size_t>(r)])];
    std::vector<double> off2;
    if (!md.offset.empty()) {
        off2.resize(static_cast<size_t>(n_alive));
        for (int64_t r = 0; r < n_alive; ++r) off2[static_cast<size_t>(r)] = md.offset[static_cast<size_t>(idx[static_cast<size_t>(r)])];
    }
    // Weights ride the row gather exactly like the offset. No current caller
    // reaches here with weights set (both row-dropping passes are FE-only and
    // feols_fit rejects weights under FE), but a weights vector left un-gathered
    // would misalign against y/X silently — the one failure mode this file must
    // never have.
    std::vector<double> w2;
    if (!md.weights.empty()) {
        w2.resize(static_cast<size_t>(n_alive));
        for (int64_t r = 0; r < n_alive; ++r) w2[static_cast<size_t>(r)] = md.weights[static_cast<size_t>(idx[static_cast<size_t>(r)])];
    }
    std::vector<int64_t> keep2(static_cast<size_t>(n_alive));
    for (int64_t r = 0; r < n_alive; ++r) keep2[static_cast<size_t>(r)] = md.keep[static_cast<size_t>(idx[static_cast<size_t>(r)])];

    // Rebuild each factor: compress codes to the levels that survive.
    auto recompress = [&](Factor& f) {
        std::vector<int32_t> remap(static_cast<size_t>(f.n_levels), -1);
        int32_t next = 0;
        std::vector<int32_t> codes2(static_cast<size_t>(n_alive));
        for (int64_t r = 0; r < n_alive; ++r) {
            int32_t o = f.codes[static_cast<size_t>(idx[static_cast<size_t>(r)])];
            int32_t d = remap[static_cast<size_t>(o)];
            if (d < 0) { d = next++; remap[static_cast<size_t>(o)] = d; }
            codes2[static_cast<size_t>(r)] = d;
        }
        f.codes = std::move(codes2);
        f.n_levels = next;
        f.count.assign(static_cast<size_t>(next), 0.0);
        for (int64_t r = 0; r < n_alive; ++r) f.count[static_cast<size_t>(f.codes[static_cast<size_t>(r)])] += 1.0;
    };
    for (Factor& f : md.fe) recompress(f);
    for (Factor& f : md.cluster) recompress(f);

    md.dm.y = std::move(y2);
    md.dm.n = n_alive;
    md.dm.n_dropped += (n - n_alive);
    md.offset = std::move(off2);
    md.weights = std::move(w2);
    md.keep = std::move(keep2);
    return n - n_alive;
}

// Rebuild md's design/factors/keep to only the `alive` rows (n_alive of them).
static void compact_to_alive(ModelData& md, const std::vector<char>& alive, int64_t n_alive) {
    const int64_t n = md.dm.n;
    const int k = md.dm.k;
    std::vector<int64_t> idx;
    idx.reserve(static_cast<size_t>(n_alive));
    for (int64_t i = 0; i < n; ++i) if (alive[static_cast<size_t>(i)]) idx.push_back(i);
    // X compacts in place (see compact_cols_inplace above) instead of gathering
    // into a second n*k-sized buffer held alongside the original.
    compact_cols_inplace(md.dm.X, n, k, idx, n_alive);
    std::vector<double> y2(static_cast<size_t>(n_alive));
    for (int64_t r = 0; r < n_alive; ++r) y2[static_cast<size_t>(r)] = md.dm.y[static_cast<size_t>(idx[static_cast<size_t>(r)])];
    std::vector<double> off2;
    if (!md.offset.empty()) {
        off2.resize(static_cast<size_t>(n_alive));
        for (int64_t r = 0; r < n_alive; ++r) off2[static_cast<size_t>(r)] = md.offset[static_cast<size_t>(idx[static_cast<size_t>(r)])];
    }
    // Weights ride the row gather exactly like the offset. No current caller
    // reaches here with weights set (both row-dropping passes are FE-only and
    // feols_fit rejects weights under FE), but a weights vector left un-gathered
    // would misalign against y/X silently — the one failure mode this file must
    // never have.
    std::vector<double> w2;
    if (!md.weights.empty()) {
        w2.resize(static_cast<size_t>(n_alive));
        for (int64_t r = 0; r < n_alive; ++r) w2[static_cast<size_t>(r)] = md.weights[static_cast<size_t>(idx[static_cast<size_t>(r)])];
    }
    std::vector<int64_t> keep2(static_cast<size_t>(n_alive));
    for (int64_t r = 0; r < n_alive; ++r) keep2[static_cast<size_t>(r)] = md.keep[static_cast<size_t>(idx[static_cast<size_t>(r)])];
    auto recompress = [&](Factor& f) {
        std::vector<int32_t> remap(static_cast<size_t>(f.n_levels), -1);
        int32_t next = 0;
        std::vector<int32_t> codes2(static_cast<size_t>(n_alive));
        for (int64_t r = 0; r < n_alive; ++r) {
            int32_t o = f.codes[static_cast<size_t>(idx[static_cast<size_t>(r)])];
            int32_t d = remap[static_cast<size_t>(o)];
            if (d < 0) { d = next++; remap[static_cast<size_t>(o)] = d; }
            codes2[static_cast<size_t>(r)] = d;
        }
        f.codes = std::move(codes2);
        f.n_levels = next;
        f.count.assign(static_cast<size_t>(next), 0.0);
        for (int64_t r = 0; r < n_alive; ++r) f.count[static_cast<size_t>(f.codes[static_cast<size_t>(r)])] += 1.0;
    };
    for (Factor& f : md.fe) recompress(f);
    for (Factor& f : md.cluster) recompress(f);
    md.dm.y = std::move(y2); md.dm.n = n_alive;
    md.dm.n_dropped += (n - n_alive);
    md.offset = std::move(off2); md.weights = std::move(w2); md.keep = std::move(keep2);
}

int64_t drop_singletons(ModelData& md) {
    if (md.fe.empty()) return 0;
    int64_t total = 0;
    bool changed = true;
    while (changed) {
        changed = false;
        const int64_t n = md.dm.n;
        std::vector<char> alive(static_cast<size_t>(n), 1);
        int64_t n_alive = n;
        for (const Factor& f : md.fe) {
            for (int64_t i = 0; i < n; ++i)
                if (alive[static_cast<size_t>(i)] &&
                    f.count[static_cast<size_t>(f.codes[static_cast<size_t>(i)])] == 1.0) {
                    alive[static_cast<size_t>(i)] = 0; --n_alive;
                }
        }
        if (n_alive < n) { compact_to_alive(md, alive, n_alive); total += (n - n_alive); changed = true; }
    }
    return total;
}

// IV analogue of compact_to_alive: subset every parallel array of an IvModelData
// (Xexog/Xendog/Zinst/y/keep) to the alive rows and recompress FE/cluster codes.
static void compact_to_alive_iv(IvModelData& md, const std::vector<char>& alive, int64_t n_alive) {
    const int64_t n = md.n;
    std::vector<int64_t> idx;
    idx.reserve(static_cast<size_t>(n_alive));
    for (int64_t i = 0; i < n; ++i) if (alive[static_cast<size_t>(i)]) idx.push_back(i);
    // Each of Xexog/Xendog/Zinst compacts in place (see compact_cols_inplace
    // above) instead of gathering into a fresh n_alive*k buffer held alongside
    // the original — the same monotonic-cursor proof applies independently to
    // each (own n, own k), so this is bit-identical to the old subset_cols
    // gather-then-assign, minus the transient second copy of each matrix.
    compact_cols_inplace(md.Xexog, n, md.kexog, idx, n_alive);
    compact_cols_inplace(md.Xendog, n, md.kendog, idx, n_alive);
    compact_cols_inplace(md.Zinst, n, md.kinst, idx, n_alive);
    std::vector<double> y2(static_cast<size_t>(n_alive));
    for (int64_t r = 0; r < n_alive; ++r) y2[static_cast<size_t>(r)] = md.y[static_cast<size_t>(idx[static_cast<size_t>(r)])];
    md.y = std::move(y2);
    std::vector<int64_t> keep2(static_cast<size_t>(n_alive));
    for (int64_t r = 0; r < n_alive; ++r) keep2[static_cast<size_t>(r)] = md.keep[static_cast<size_t>(idx[static_cast<size_t>(r)])];
    md.keep = std::move(keep2);
    auto recompress = [&](Factor& f) {
        std::vector<int32_t> remap(static_cast<size_t>(f.n_levels), -1);
        int32_t next = 0;
        std::vector<int32_t> codes2(static_cast<size_t>(n_alive));
        for (int64_t r = 0; r < n_alive; ++r) {
            int32_t o = f.codes[static_cast<size_t>(idx[static_cast<size_t>(r)])];
            int32_t d = remap[static_cast<size_t>(o)];
            if (d < 0) { d = next++; remap[static_cast<size_t>(o)] = d; }
            codes2[static_cast<size_t>(r)] = d;
        }
        f.codes = std::move(codes2);
        f.n_levels = next;
        f.count.assign(static_cast<size_t>(next), 0.0);
        for (int64_t r = 0; r < n_alive; ++r) f.count[static_cast<size_t>(f.codes[static_cast<size_t>(r)])] += 1.0;
    };
    for (Factor& f : md.fe) recompress(f);
    for (Factor& f : md.cluster) recompress(f);
    md.n = n_alive;
    md.n_dropped += (n - n_alive);
}

int64_t drop_singletons_iv(IvModelData& md) {
    if (md.fe.empty()) return 0;
    int64_t total = 0;
    bool changed = true;
    while (changed) {
        changed = false;
        const int64_t n = md.n;
        std::vector<char> alive(static_cast<size_t>(n), 1);
        int64_t n_alive = n;
        for (const Factor& f : md.fe) {
            for (int64_t i = 0; i < n; ++i)
                if (alive[static_cast<size_t>(i)] &&
                    f.count[static_cast<size_t>(f.codes[static_cast<size_t>(i)])] == 1.0) {
                    alive[static_cast<size_t>(i)] = 0; --n_alive;
                }
        }
        if (n_alive < n) { compact_to_alive_iv(md, alive, n_alive); total += (n - n_alive); changed = true; }
    }
    return total;
}

Factor intersect_factors(const Factor& a, const Factor& b) {
    Factor f;
    f.name = a.name + "^" + b.name;
    const int64_t n = static_cast<int64_t>(a.codes.size());
    f.codes.resize(n);
    std::unordered_map<int64_t, int32_t> remap;
    remap.reserve(n / 4 + 16);
    int32_t next = 0;
    for (int64_t i = 0; i < n; ++i) {
        int64_t key = static_cast<int64_t>(a.codes[i]) * b.n_levels + b.codes[i];
        auto it = remap.find(key);
        int32_t code;
        if (it == remap.end()) { code = next++; remap.emplace(key, code); }
        else code = it->second;
        f.codes[i] = code;
    }
    f.n_levels = next;
    f.count.assign(f.n_levels, 0.0);
    for (int32_t c : f.codes) f.count[c] += 1.0;
    return f;
}

ModelData build_model(const Dataset& ds, const std::string& yname,
                      const std::vector<std::string>& xnames,
                      const std::vector<std::string>& fenames,
                      const std::vector<std::string>& clusternames,
                      const std::string& offset_name, bool drop_singleton,
                      const TsMeta* ts, const std::vector<char>* row_mask,
                      const std::string& weight_name, WeightKind weight_kind) {
    const int64_t N = ds.n_rows();
    // Time-series pre-pass: materialize any L./F./D./S. tokens (panel/time-aware)
    // into a side-table, rewriting them to plain names the factor expander resolves.
    std::vector<std::vector<double>> ts_storage;
    ExtraCols extra;
    std::vector<std::pair<std::string, std::string>> synth_to_real;
    // BUG-1277: the depvar takes the same pre-pass (StataBE `regress D.y x1 x2 if t>=3`
    // after tsset: e(N) 48, e(depvar) D.y), then resolves from the side-table.
    const std::vector<std::string> yn = ts_prepass(ds, {yname}, ts, ts_storage, extra, synth_to_real);
    const double* yp = require_f64(ds, yn[0], &extra);
    const double* op = offset_name.empty() ? nullptr : require_f64(ds, offset_name);
    const double* wp = weight_name.empty() ? nullptr : require_f64(ds, weight_name);
    std::vector<std::string> xn = ts_prepass(ds, xnames, ts, ts_storage, extra, synth_to_real);
    // Regressors are factor-variable terms: plain numeric, `i(col)` indicators,
    // or `#`/`##` interactions (Stata i./c./#/## — see parse_fvterms).
    std::vector<FvTerm> xterms = parse_fvterms(ds, xn, &extra);
    std::vector<const double*> xsrc = fvterm_srcs(xterms);

    std::vector<KeyCol> keys;
    auto add_key = [&](const std::string& nm) {
        const Column& c = ds.col(nm);
        keys.push_back({&c, c.dtype == DType::Factor});
    };
    // Per absorbed term, how many key columns it reads (2+ for `a#b`).
    std::vector<size_t> fe_nparts;
    for (const auto& nm : fenames) {
        const std::vector<std::string> parts = fe_interaction_parts(nm);
        for (const auto& p : parts) add_key(p);
        fe_nparts.push_back(parts.size());
    }
    for (const auto& nm : clusternames) add_key(nm);

    // Single listwise deletion across y, X, fe, cluster. row_mask (E1), when given,
    // applies a Stata `if` sample restriction on top — checked first (cheapest) so a
    // masked-out row costs one branch. `ts` was already materialized against the
    // FULL (unmasked) `ds` above, exactly as when row_mask is null: the mask only
    // narrows which of those already-correct rows enter the design.
    std::vector<int64_t> keep;
    keep.reserve(N);
    for (int64_t i = 0; i < N; ++i) {
        if (row_mask && !(*row_mask)[static_cast<size_t>(i)]) continue;
        if (std::isnan(yp[i])) continue;
        if (op && std::isnan(op[i])) continue;
        if (wp) {
            // A missing weight drops the row like any other missing value; a zero
            // weight leaves the row contributing nothing, which is Stata's exclusion
            // (it matters: under aw/pw the reported N is the ROW count, so a kept
            // zero-weight row would inflate N and every dof derived from it).
            if (std::isnan(wp[i]) || wp[i] == 0.0) continue;
            if (wp[i] < 0.0)
                throw EngineError("bad_input",
                    "weight '" + weight_name + "' has a negative value (" +
                    std::to_string(wp[i]) + "); weights must be non-negative");
            if (weight_kind == WeightKind::Frequency && wp[i] != std::floor(wp[i]))
                throw EngineError("bad_input",
                    "fweight '" + weight_name + "' has a non-integer value (" +
                    std::to_string(wp[i]) + "); Stata requires integer frequency weights");
        }
        bool ok = true;
        for (const double* p : xsrc)
            if (std::isnan(p[i])) { ok = false; break; }
        if (ok)
            for (const auto& k : keys)
                if (!valid(k, i)) { ok = false; break; }
        if (ok) keep.push_back(i);
    }

    ModelData md;
    md.has_intercept = fenames.empty();
    DesignMatrix& dm = md.dm;
    dm.n = static_cast<int64_t>(keep.size());
    dm.n_dropped = N - dm.n;

    // Expand factor-variable terms into concrete design columns (levels enumerated
    // over kept rows, base = smallest dropped; empty interaction cells omitted).
    std::vector<size_t> base_cells;
    auto xcols = fvterm_columns(xterms, keep, &md.warnings, &base_cells);
    int n_cols = static_cast<int>(xcols.size());
    dm.k = n_cols + (md.has_intercept ? 1 : 0);
    dm.X.resize(static_cast<size_t>(dm.n) * dm.k);
    dm.y.resize(dm.n);
    dm.xnames.reserve(dm.k);

    int col = 0;
    if (md.has_intercept) {
        double* c0 = dm.col(col++);
        for (int64_t r = 0; r < dm.n; ++r) c0[r] = 1.0;
        dm.xnames.push_back("(intercept)");
    }
    for (size_t xi = 0; xi < xcols.size(); ++xi) {
        auto& nc = xcols[xi];
        double* cj = dm.col(col++);
        std::copy(nc.second.begin(), nc.second.end(), cj);
        restore_ts_names(nc.first, synth_to_real);
        if (std::find(base_cells.begin(), base_cells.end(), xi) != base_cells.end())
            md.late_cells.push_back(nc.first);
        dm.xnames.push_back(std::move(nc.first));
        // C3-F3: release this column's own buffer now that its values are
        // copied into dm.X. xcols is never read again before it goes out of
        // scope at function end (FE/cluster construction and drop_singletons
        // below only touch md.dm/md.fe/md.cluster/md.keep), so leaving it alive
        // would just double the design matrix's footprint for the rest of
        // build_model for no reason. Shrinking to empty is a pure
        // deallocation (there is no size-0 buffer to allocate), so this cannot
        // itself introduce a new peak.
        nc.second.clear();
        nc.second.shrink_to_fit();
    }
    for (int64_t r = 0; r < dm.n; ++r) dm.y[r] = yp[keep[r]];
    if (op) {
        md.offset.resize(dm.n);
        for (int64_t r = 0; r < dm.n; ++r) md.offset[r] = op[keep[r]];
    }
    if (wp) {
        md.weights.resize(dm.n);
        for (int64_t r = 0; r < dm.n; ++r) md.weights[r] = wp[keep[r]];
        md.weight_kind = weight_kind;
    }

    size_t ki = 0;
    for (size_t f = 0; f < fenames.size(); ++f) {
        if (fe_nparts[f] == 1) {
            md.fe.push_back(make_factor(fenames[f], keys[ki++], keep));
        } else {
            const std::vector<KeyCol> kcs(keys.begin() + static_cast<std::ptrdiff_t>(ki),
                                          keys.begin() + static_cast<std::ptrdiff_t>(ki + fe_nparts[f]));
            md.fe.push_back(make_interaction_factor(fenames[f], kcs, keep));
            ki += fe_nparts[f];
        }
    }
    for (const auto& nm : clusternames)
        md.cluster.push_back(make_factor(nm, keys[ki++], keep));

    md.keep = std::move(keep);
    if (drop_singleton && !md.fe.empty()) md.n_singletons_dropped = drop_singletons(md);
    return md;
}

IvModelData build_iv_model(const Dataset& ds, const std::string& yname,
                           const std::vector<std::string>& exog,
                           const std::vector<std::string>& endog,
                           const std::vector<std::string>& instruments,
                           const std::vector<std::string>& fenames,
                           const std::vector<std::string>& clusternames,
                           const TsMeta* ts, bool drop_singleton,
                           const std::vector<char>* row_mask, bool no_intercept) {
    const int64_t N = ds.n_rows();
    // TS pre-pass over exog, endog, instruments (shared side-table so a var lagged
    // in more than one list materializes once per token).
    std::vector<std::vector<double>> ts_storage;
    ExtraCols extra;
    std::vector<std::pair<std::string, std::string>> synth_to_real;
    // BUG-1277: and over the depvar, as build_model does.
    const std::vector<std::string> yn = ts_prepass(ds, {yname}, ts, ts_storage, extra, synth_to_real);
    const double* yp = require_f64(ds, yn[0], &extra);
    std::vector<std::string> exn = ts_prepass(ds, exog, ts, ts_storage, extra, synth_to_real);
    std::vector<std::string> zin = ts_prepass(ds, instruments, ts, ts_storage, extra, synth_to_real);
    std::vector<std::string> enn = ts_prepass(ds, endog, ts, ts_storage, extra, synth_to_real);
    // exog and instruments may carry i(col) factor-expansion tokens; endog is
    // taken as plain numeric columns (factor-valued endogenous regressors are not
    // a standard 2SLS construct — each level would need its own instrument).
    std::vector<FvTerm> ex = parse_fvterms(ds, exn, &extra);
    std::vector<FvTerm> zi = parse_fvterms(ds, zin, &extra);
    std::vector<const double*> ex_src = fvterm_srcs(ex);
    std::vector<const double*> zi_src = fvterm_srcs(zi);
    std::vector<const double*> en;
    for (const auto& nm : enn) en.push_back(require_f64(ds, nm, &extra));

    std::vector<KeyCol> keys;
    for (const auto& nm : fenames) {
        const Column& c = ds.col(nm);
        keys.push_back({&c, c.dtype == DType::Factor});
    }
    for (const auto& nm : clusternames) {
        const Column& c = ds.col(nm);
        keys.push_back({&c, c.dtype == DType::Factor});
    }

    std::vector<int64_t> keep;
    keep.reserve(N);
    for (int64_t i = 0; i < N; ++i) {
        if (row_mask && !(*row_mask)[static_cast<size_t>(i)]) continue;  // E1: `if` restriction, applied after ts_prepass above saw the FULL series
        if (std::isnan(yp[i])) continue;
        bool ok = true;
        for (const double* p : ex_src) if (std::isnan(p[i])) { ok = false; break; }
        if (ok) for (const double* p : en) if (std::isnan(p[i])) { ok = false; break; }
        if (ok) for (const double* p : zi_src) if (std::isnan(p[i])) { ok = false; break; }
        if (ok) for (const auto& k : keys) if (!valid(k, i)) { ok = false; break; }
        if (ok) keep.push_back(i);
    }

    IvModelData md;
    md.n = static_cast<int64_t>(keep.size());
    md.n_dropped = N - md.n;
    md.has_intercept = fenames.empty() && !no_intercept;
    md.no_constant = fenames.empty() && no_intercept;
    const int64_t n = md.n;

    // Expand exog & instrument terms into concrete columns (empty cells omitted).
    auto excols = fvterm_columns(ex, keep, &md.warnings);
    auto zicols = fvterm_columns(zi, keep, &md.warnings);
    int kexog_x = static_cast<int>(excols.size());
    int kinst_x = static_cast<int>(zicols.size());
    md.kexog = kexog_x + (md.has_intercept ? 1 : 0);
    md.kendog = static_cast<int>(endog.size());
    md.kinst = kinst_x;

    md.Xexog.resize(static_cast<size_t>(n) * md.kexog);
    md.Xendog.resize(static_cast<size_t>(n) * md.kendog);
    md.Zinst.resize(static_cast<size_t>(n) * md.kinst);
    md.y.resize(n);

    int col0 = 0;
    if (md.has_intercept) {
        double* c0 = md.Xexog.data();
        for (int64_t r = 0; r < n; ++r) c0[r] = 1.0;
        md.exog_names.push_back("(intercept)");
        col0 = 1;
    }
    for (auto& nc : excols) {
        double* cj = md.Xexog.data() + static_cast<int64_t>(col0++) * n;
        std::copy(nc.second.begin(), nc.second.end(), cj);
        restore_ts_names(nc.first, synth_to_real);
        md.exog_names.push_back(std::move(nc.first));
        // C3-F3: same release-early as build_model's xcols loop above — nc.second
        // is fully copied into md.Xexog and never read again.
        nc.second.clear();
        nc.second.shrink_to_fit();
    }
    for (size_t j = 0; j < endog.size(); ++j) {
        double* cj = md.Xendog.data() + static_cast<int64_t>(j) * n;
        for (int64_t r = 0; r < n; ++r) cj[r] = en[j][keep[static_cast<size_t>(r)]];
        md.endog_names.push_back(endog[j]);  // original token = readable (e.g. "L.x")
    }
    for (size_t j = 0; j < zicols.size(); ++j) {
        double* cj = md.Zinst.data() + static_cast<int64_t>(j) * n;
        std::copy(zicols[j].second.begin(), zicols[j].second.end(), cj);
        restore_ts_names(zicols[j].first, synth_to_real);
        md.inst_names.push_back(std::move(zicols[j].first));
        zicols[j].second.clear();
        zicols[j].second.shrink_to_fit();
    }
    for (int64_t r = 0; r < n; ++r) md.y[r] = yp[keep[r]];

    size_t ki = 0;
    for (const auto& nm : fenames) md.fe.push_back(make_factor(nm, keys[ki++], keep));
    for (const auto& nm : clusternames)
        md.cluster.push_back(make_factor(nm, keys[ki++], keep));
    md.keep = std::move(keep);
    // Drop singleton FE groups (default on), matching reghdfe/ivreghdfe (Correia
    // 2015). Done after all parallel arrays are built so compaction subsets them.
    if (drop_singleton && !md.fe.empty()) md.n_singletons_dropped = drop_singletons_iv(md);
    return md;
}

std::vector<std::pair<std::string, std::vector<double>>>
expand_varlist(const Dataset& ds, const std::vector<std::string>& names,
               const TsMeta& ts, bool bare_factor) {
    const int64_t n = ds.n_rows();
    std::vector<std::vector<double>> storage;
    ExtraCols extra;
    std::vector<std::pair<std::string, std::string>> synth_to_real;
    std::vector<std::string> rewritten = ts_prepass(ds, names, &ts, storage, extra, synth_to_real);

    std::vector<FvTerm> terms = parse_fvterms(ds, rewritten, &extra, bare_factor);
    std::vector<int64_t> keep(static_cast<size_t>(n));
    for (int64_t i = 0; i < n; ++i) keep[static_cast<size_t>(i)] = i;  // all rows
    auto cols = fvterm_columns(terms, keep);
    for (auto& nc : cols) restore_ts_names(nc.first, synth_to_real);
    return cols;
}

}  // namespace cecon
