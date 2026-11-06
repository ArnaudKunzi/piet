// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// cecon — coefficient-name resolution for the post-estimation commands
// (test / testparm / lincom / nlcom). ONE definition, so the four surfaces cannot
// disagree about what `2.c`, `_b[2.c]`, `0b.c` or `_cons` refer to.
//
// Why this exists (w2c_postest oracle, tests/eval/golden/aer_w2/w2c_postest.log):
//   * a factor level is named `<level>.<var>` in the fitted design (Stata's own
//     spelling), and user input may carry the base/omitted marker (`0b.c`, `3o.c`);
//   * the BASE level of `i.c` (and any other omitted level) is not a design column
//     at all, but Stata treats `0.c` as a coefficient that is exactly 0 with zero
//     variance — `test 2.c = 0.5*(1.c+0.c)` is rc 0 with F 3.7448 (arm
//     test_2c_eq_avg_base) — while a level the estimation sample never had is
//     `9.c not found`, r(111) (arm test_level_not_in_model). Telling the two apart
//     needs the sample's levels, which only the full-specification path can see;
//   * the linear parsers these commands used read the leading `1.` of `1.c` as the
//     number 1 and then looked up a coefficient named `c` ("not among regressors"),
//     and had no parentheses at all, so `0.5*(1.c+0.c)` could not be written.
#pragma once
#include "core/ident.h"  // non-ASCII bytes are identifier bytes (Unicode names)

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <string>
#include <unordered_map>
#include <vector>

namespace cecon {
namespace postest {

// Distinct values each factor variable takes in the ESTIMATION SAMPLE, sorted.
// Only the full-specification path (which has the dataset and the kept rows) can
// fill this; a handle-addressed call passes nullptr.
using FactorLevels = std::unordered_map<std::string, std::vector<double>>;

enum class CoefKind {
    Column,       // a design column: `idx` is its position in names
    Omitted,      // a factor level present in the sample but not a column (base or
                  // omitted): its coefficient is 0 with zero variance
    NotFound,     // no such coefficient — Stata's "<name> not found", r(111)
    NeedsSample,  // a factor-level spelling of a model factor that is not a column,
                  // and no sample levels were supplied to classify it
};

struct CoefRef {
    CoefKind kind = CoefKind::NotFound;
    int idx = -1;
    std::string name;  // the name as the user wrote it, `_b[]` unwrapped
};

inline std::string trim_ws(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && std::isspace(static_cast<unsigned char>(s[a]))) ++a;
    while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) --b;
    return s.substr(a, b - a);
}

// `<digits>[b|o|bn].<var>` — a factor-level reference to ONE variable (no `#`).
// Fills the level and the variable; false for anything else.
inline bool parse_level_ref(const std::string& t, double& level, std::string& var) {
    size_t i = 0;
    while (i < t.size() && std::isdigit(static_cast<unsigned char>(t[i]))) ++i;
    if (i == 0) return false;
    const std::string digits = t.substr(0, i);
    if (t.compare(i, 3, "bn.") == 0) i += 2;
    else if (i < t.size() && (t[i] == 'b' || t[i] == 'o')) ++i;
    if (i >= t.size() || t[i] != '.') return false;
    var = t.substr(i + 1);
    if (var.empty() || !cecon::ident::isIdentStartByte(var[0]))
        return false;
    for (char c : var)
        if (!cecon::ident::isIdentByte(c)) return false;
    level = std::strtod(digits.c_str(), nullptr);
    return true;
}

// Stata's spelling of a factor level in a design name: integer levels print bare.
inline std::string level_name(double level, const std::string& var) {
    char buf[64];
    if (level == std::floor(level) && std::fabs(level) < 1e15)
        std::snprintf(buf, sizeof buf, "%.0f", level);
    else
        std::snprintf(buf, sizeof buf, "%.17g", level);
    return std::string(buf) + "." + var;
}

inline int name_index(const std::vector<std::string>& names, const std::string& n) {
    for (size_t j = 0; j < names.size(); ++j)
        if (names[j] == n) return static_cast<int>(j);
    return -1;
}

// An interaction's identity for lookup: its `#` atoms with any `c.` prefix
// dropped, sorted. Stata names `c.age_BL##treatment`'s cell `1.treatment#c.age_BL`
// and accepts `c.age_BL#1.treatment` for it in `test` (StataBE, AER package 007
// Joint_test_BL_char... : constraint `(1) 1.treatment#c.age_BL = 0`); the engine
// stores the cell as `age_BL#1.treatment`. All three spellings are one column.
inline std::string interaction_key(const std::string& name) {
    if (name.find('#') == std::string::npos) return std::string();
    std::vector<std::string> atoms;
    size_t start = 0;
    while (start <= name.size()) {
        size_t end = name.find('#', start);
        if (end == std::string::npos) end = name.size();
        std::string a = name.substr(start, end - start);
        if (a.size() > 2 && a.compare(0, 2, "c.") == 0) a.erase(0, 2);
        atoms.push_back(a);
        start = end + 1;
    }
    std::sort(atoms.begin(), atoms.end());
    std::string key;
    for (size_t i = 0; i < atoms.size(); ++i) {
        if (i) key += '#';
        key += atoms[i];
    }
    return key;
}

// The variable one design atom names: `1.treatment`, `c.age`, `L2.x`, `eq:x`.
inline std::string atom_var(const std::string& atom) {
    std::string a = atom;
    const size_t colon = a.find(':');
    if (colon != std::string::npos) a.erase(0, colon + 1);
    const size_t dot = a.rfind('.');
    if (dot != std::string::npos) a.erase(0, dot + 1);
    return a;
}

// Variable-name abbreviation inside a coefficient reference ([U] 11.2): an
// atom's variable that is no variable of the MODEL stands for the unique model
// variable it begins -- the model's, not the dataset's (StataBE
// uni-oracle/hz.do Z1: `lincom 1.treatment + 1.treatment#1.husb` with husb_x in
// the data but only husb_influence_kids_original_BL in the fit is rc 0; Z2/Z9
// `_b[weight]` is _b[weight_a]; AER 007's HTE tables). Two candidates is
// r(111) (Z5/Z6/Z8). Returns the rewritten reference, or "" when nothing
// changes; `*ambiguous` (when given) is set to an atom with two candidates.
inline std::string expand_coef_abbrev(const std::vector<std::string>& names, const std::string& t,
                                      bool* ambiguous = nullptr) {
    std::vector<std::string> vars;
    for (const auto& n : names) {
        size_t start = 0;
        while (start <= n.size()) {
            size_t end = n.find('#', start);
            if (end == std::string::npos) end = n.size();
            const std::string v = atom_var(n.substr(start, end - start));
            if (!v.empty() && std::find(vars.begin(), vars.end(), v) == vars.end()) vars.push_back(v);
            start = end + 1;
        }
    }
    std::string out;
    bool changed = false;
    size_t start = 0;
    while (start <= t.size()) {
        size_t end = t.find('#', start);
        if (end == std::string::npos) end = t.size();
        const std::string atom = t.substr(start, end - start);
        size_t cut = atom.rfind('.');
        if (cut == std::string::npos) cut = atom.find(':');
        const std::string head = cut == std::string::npos ? std::string() : atom.substr(0, cut + 1);
        const std::string v = cut == std::string::npos ? atom : atom.substr(cut + 1);
        std::string use = v;
        if (!v.empty() && cecon::ident::isIdentStartByte(v[0]) &&
            std::find(vars.begin(), vars.end(), v) == vars.end()) {
            std::vector<std::string> hits;
            for (const auto& m : vars)
                if (m.size() > v.size() && m.compare(0, v.size(), v) == 0) hits.push_back(m);
            if (hits.size() > 1) {
                if (ambiguous) *ambiguous = true;
                return std::string();
            }
            if (hits.size() == 1) { use = hits.front(); changed = true; }
        }
        if (start) out += '#';
        out += head + use;
        start = end + 1;
    }
    return changed ? out : std::string();
}

// Resolve one coefficient reference against the fitted design names.
inline CoefRef resolve_coef(const std::vector<std::string>& names, const std::string& token,
                            const FactorLevels* levels) {
    CoefRef r;
    std::string t = trim_ws(token);
    if (t.size() > 4 && t.compare(0, 3, "_b[") == 0 && t.back() == ']')
        t = trim_ws(t.substr(3, t.size() - 4));
    // `[eq]name` (a joint `test [A1_mean]x1`, suest) is the column `eq:name`
    if (t.size() > 2 && t[0] == '[') {
        const size_t close = t.find(']');
        if (close != std::string::npos && close + 1 < t.size())
            t = trim_ws(t.substr(1, close - 1)) + ":" + trim_ws(t.substr(close + 1));
    }
    r.name = t;
    if (t == "_cons" || t == "cons" || t == "(intercept)") {
        int j = name_index(names, "(intercept)");
        if (j < 0) j = name_index(names, "_cons");
        if (j >= 0) { r.kind = CoefKind::Column; r.idx = j; }
        return r;
    }
    int j = name_index(names, t);
    if (j >= 0) { r.kind = CoefKind::Column; r.idx = j; return r; }
    // An abbreviated variable (expand_coef_abbrev): resolve the full spelling.
    // The rewrite names only model variables, so the second pass never rewrites.
    if (const std::string full = expand_coef_abbrev(names, t); !full.empty())
        return resolve_coef(names, full, levels);
    // An interaction spelled in another atom order or with/without `c.` (see
    // interaction_key): the unique design column with the same atoms.
    if (const std::string key = interaction_key(t); !key.empty()) {
        int hit = -1;
        for (size_t q = 0; q < names.size(); ++q) {
            if (interaction_key(names[q]) != key) continue;
            if (hit >= 0) { hit = -2; break; }  // two columns: not this rule's to pick
            hit = static_cast<int>(q);
        }
        if (hit >= 0) { r.kind = CoefKind::Column; r.idx = hit; return r; }
    }
    double level = 0;
    std::string var;
    if (!parse_level_ref(t, level, var)) return r;  // NotFound
    j = name_index(names, level_name(level, var));  // `2b.c` / `2o.c` spelled plainly
    if (j >= 0) { r.kind = CoefKind::Column; r.idx = j; return r; }
    // A posted e(b) that carries the base/omitted column itself (`0b.c`, `18o.c`:
    // qreg2 and the margins it posts, PIET's own posting): the plain `0.c` is that
    // level, a coefficient that is 0 with zero variance (w2j_e3_qreg2.log M04
    // `test 2.clinic = 0.5*(1.clinic+0.clinic)` after `margins, dydx(clinic) post`).
    {
        const std::string plain = level_name(level, var);
        const std::string head = plain.substr(0, plain.size() - var.size() - 1);
        for (const char* mark : {"b.", "o.", "bo."})
            if (name_index(names, head + mark + var) >= 0) {
                r.kind = CoefKind::Omitted;
                return r;
            }
    }
    if (levels != nullptr) {
        auto it = levels->find(var);
        if (it != levels->end() &&
            std::binary_search(it->second.begin(), it->second.end(), level))
            r.kind = CoefKind::Omitted;
        return r;  // Omitted, or NotFound (a level the sample never had)
    }
    // No sample to consult: if `var` is a factor of this model at all, the call
    // cannot be classified here (base level vs. absent level) — say so rather than
    // guess either way.
    for (const auto& n : names) {
        double l2 = 0;
        std::string v2;
        if (parse_level_ref(n, l2, v2) && v2 == var) { r.kind = CoefKind::NeedsSample; break; }
    }
    return r;
}

// The factor variables a varlist names: `i.c`, `ib2.c`, `ibn.c`, `b2.c`,
// `i(2/3).c`, `ib(first).c` and every part of a `#`/`##` interaction that is not
// `c.`-prefixed (inside an interaction Stata implies `i.`).
inline std::vector<std::string> factor_vars_of(const std::vector<std::string>& xterms) {
    std::vector<std::string> out;
    auto add = [&out](const std::string& v) {
        if (!v.empty() && std::find(out.begin(), out.end(), v) == out.end()) out.push_back(v);
    };
    auto strip_prefix = [](const std::string& part, bool& is_factor) -> std::string {
        is_factor = false;
        if (part.compare(0, 2, "c.") == 0) return part.substr(2);
        size_t close = part.find(").");
        if (part.compare(0, 2, "i(") == 0 && close != std::string::npos) {
            is_factor = true;
            return part.substr(close + 2);
        }
        if (part.compare(0, 3, "ib(") == 0 && close != std::string::npos) {
            is_factor = true;
            return part.substr(close + 2);
        }
        size_t dot = part.find('.');
        if (dot == std::string::npos) return part;
        const std::string pre = part.substr(0, dot);
        bool fv = pre == "i" || pre == "ibn" || pre == "o";
        if (!fv && pre.size() > 1 && (pre[0] == 'b' || (pre[0] == 'i' && pre[1] == 'b'))) {
            size_t k = (pre[0] == 'b') ? 1 : 2;
            fv = k < pre.size();
            for (size_t q = k; q < pre.size(); ++q)
                if (!std::isdigit(static_cast<unsigned char>(pre[q]))) fv = false;
        }
        if (!fv) return std::string();  // a time-series operator or unknown prefix
        is_factor = true;
        return part.substr(dot + 1);
    };
    for (const auto& term : xterms) {
        const bool interaction = term.find('#') != std::string::npos;
        size_t pos = 0;
        while (pos <= term.size()) {
            size_t h = term.find('#', pos);
            std::string part = term.substr(pos, h == std::string::npos ? std::string::npos : h - pos);
            if (!part.empty()) {
                bool is_factor = false;
                std::string v = strip_prefix(part, is_factor);
                const bool explicit_c = part.compare(0, 2, "c.") == 0;
                if (is_factor || (interaction && !explicit_c && part.find('.') == std::string::npos))
                    add(v);
            }
            if (h == std::string::npos) break;
            pos = h + (term.compare(h, 2, "##") == 0 ? 2 : 1);
        }
    }
    return out;
}

// A linear form over the design columns: sum_j w[j]*b[j] + konst.
struct LinearForm {
    std::vector<double> w;
    double konst = 0.0;
    bool is_constant() const {
        for (double v : w) if (v != 0.0) return false;
        return true;
    }
};

// Parse a LINEAR expression over coefficient references — the single grammar
// behind `test exp=exp` and `lincom exp`, with Stata's operator order ([U] 13.2.5:
// `/` before `*`, binary `-` before `+`), which decides how a weight's roundings
// combine (BUG-1231: `lincom 0.1*x1 + 0.2*x1 - 0.3*x1` is 0.1 + (0.2 - 0.3) =
// 2^-55 in StataBE, not (0.1 + 0.2) - 0.3 = 2^-54; `0.3*x1/0.7*0.9` is
// 0.3 * (x1/0.7) * 0.9):
//   expr    := diff ('+' diff)*      diff := prod ('-' prod)*
//   prod    := quot ('*' quot)*      quot := unary ('/' unary)*
//   unary   := ('+'|'-') unary | primary
//   primary := number | name | _b[name] | '(' expr ')'
// A name is a design name (`x1`, `2.c`, `0b.c`, `1.c#1.g`, `L.x`, `_cons`). A product
// of two non-constant forms, or a division by one, is not linear and is refused.
// `needs_sample` is set (and false returned) when a factor level could not be
// classified without the estimation sample (see CoefKind::NeedsSample).
class LinearParser {
 public:
    LinearParser(const std::string& s, const std::vector<std::string>& names,
                 const FactorLevels* levels)
        : s_(s), names_(names), levels_(levels) {}

    bool parse(LinearForm& out, std::string& err, bool& needs_sample) {
        needs_sample = false;
        try {
            ws();
            if (i_ >= s_.size()) throw Fail("empty expression");
            out = expr();
            ws();
            if (i_ < s_.size()) throw Fail("invalid syntax near '" + s_.substr(i_) + "'");
        } catch (const Fail& f) {
            err = f.msg;
            needs_sample = needs_sample_;
            return false;
        }
        return true;
    }

 private:
    struct Fail {
        std::string msg;
        explicit Fail(std::string m) : msg(std::move(m)) {}
    };
    const std::string& s_;
    const std::vector<std::string>& names_;
    const FactorLevels* levels_;
    size_t i_ = 0;
    bool needs_sample_ = false;

    void ws() { while (i_ < s_.size() && std::isspace(static_cast<unsigned char>(s_[i_]))) ++i_; }
    LinearForm zero() const { LinearForm f; f.w.assign(names_.size(), 0.0); return f; }

    // One left-associative level of the grammar: operand (op operand)*.
    bool at(char op) {
        ws();
        return i_ < s_.size() && s_[i_] == op;
    }
    LinearForm expr() {
        LinearForm acc = diff();
        while (at('+')) {
            ++i_;
            LinearForm rhs = diff();
            for (size_t j = 0; j < acc.w.size(); ++j) acc.w[j] += rhs.w[j];
            acc.konst += rhs.konst;
        }
        return acc;
    }
    LinearForm diff() {
        LinearForm acc = prod();
        while (at('-')) {
            ++i_;
            LinearForm rhs = prod();
            for (size_t j = 0; j < acc.w.size(); ++j) acc.w[j] -= rhs.w[j];
            acc.konst -= rhs.konst;
        }
        return acc;
    }
    LinearForm prod() {
        LinearForm acc = quot();
        while (at('*')) {
            ++i_;
            LinearForm rhs = quot();
            if (acc.is_constant()) std::swap(acc, rhs);  // acc: form, rhs: scalar
            if (!rhs.is_constant()) throw Fail("expression is not linear in the coefficients");
            for (double& v : acc.w) v *= rhs.konst;
            acc.konst *= rhs.konst;
        }
        return acc;
    }
    LinearForm quot() {
        LinearForm acc = unary();
        while (at('/')) {
            ++i_;
            LinearForm rhs = unary();
            if (!rhs.is_constant()) throw Fail("expression is not linear in the coefficients");
            if (rhs.konst == 0.0) throw Fail("division by zero");
            for (double& v : acc.w) v /= rhs.konst;
            acc.konst /= rhs.konst;
        }
        return acc;
    }
    LinearForm unary() {
        ws();
        if (i_ < s_.size() && (s_[i_] == '+' || s_[i_] == '-')) {
            const double sg = (s_[i_] == '-') ? -1.0 : 1.0;
            ++i_;
            LinearForm f = unary();
            for (double& v : f.w) v *= sg;
            f.konst *= sg;
            return f;
        }
        return primary();
    }
    static bool name_char(char c) {
        return cecon::ident::isIdentByte(c) || c == '.' || c == '#';
    }
    LinearForm primary() {
        ws();
        if (i_ >= s_.size()) throw Fail("unexpected end of expression");
        const char c = s_[i_];
        if (c == '(') {
            ++i_;
            LinearForm f = expr();
            ws();
            if (i_ >= s_.size() || s_[i_] != ')') throw Fail("unmatched '('");
            ++i_;
            return f;
        }
        if (s_.compare(i_, 3, "_b[") == 0) {
            size_t close = s_.find(']', i_);
            if (close == std::string::npos) throw Fail("missing ']'");
            std::string nm = s_.substr(i_ + 3, close - i_ - 3);
            i_ = close + 1;
            return coef(nm);
        }
        if (std::isdigit(static_cast<unsigned char>(c)) || c == '.') {
            // A factor-level name starts with digits too: `2.c`, `0b.c`, `1.c#1.g`.
            size_t j = i_;
            while (j < s_.size() && name_char(s_[j])) ++j;
            double lv = 0;
            std::string var;
            std::string word = s_.substr(i_, j - i_);
            std::string head = word.substr(0, word.find('#'));
            if (parse_level_ref(head, lv, var)) {
                i_ = j;
                return coef(word);
            }
            char* end = nullptr;
            const double v = std::strtod(s_.c_str() + i_, &end);
            if (end == s_.c_str() + i_) throw Fail("invalid syntax near '" + s_.substr(i_) + "'");
            i_ = static_cast<size_t>(end - s_.c_str());
            LinearForm f = zero();
            f.konst = v;
            return f;
        }
        // `[eq]name` -- an equation-qualified coefficient (Stata's test/lincom
        // syntax after a multi-equation posting: suest's `[A1_mean]x1`,
        // w2j_e3_builtin.do:184/:199) -- names the column `eq:name`.
        if (c == '[') {
            const size_t close = s_.find(']', i_);
            if (close == std::string::npos) throw Fail("missing ']'");
            std::string eq = trim_ws(s_.substr(i_ + 1, close - i_ - 1));
            size_t j = close + 1;
            while (j < s_.size() && std::isspace(static_cast<unsigned char>(s_[j]))) ++j;
            const size_t k0 = j;
            while (j < s_.size() && name_char(s_[j])) ++j;
            if (j == k0) throw Fail("invalid syntax near '" + s_.substr(i_) + "'");
            const std::string nm = s_.substr(k0, j - k0);
            i_ = j;
            return coef(eq + ":" + nm);
        }
        if (cecon::ident::isIdentStartByte(c)) {
            size_t j = i_;
            while (j < s_.size() && name_char(s_[j])) ++j;
            std::string word = s_.substr(i_, j - i_);
            // `eq:name`, the same column spelled with a colon
            if (j < s_.size() && s_[j] == ':' && j + 1 < s_.size() &&
                cecon::ident::isIdentByte(s_[j + 1])) {
                size_t k = j + 1;
                while (k < s_.size() && name_char(s_[k])) ++k;
                word = s_.substr(i_, k - i_);
                j = k;
            }
            i_ = j;
            return coef(word);
        }
        throw Fail("invalid syntax near '" + s_.substr(i_) + "'");
    }
    LinearForm coef(const std::string& nm) {
        const CoefRef r = resolve_coef(names_, nm, levels_);
        LinearForm f = zero();
        switch (r.kind) {
            case CoefKind::Column: f.w[static_cast<size_t>(r.idx)] = 1.0; return f;
            case CoefKind::Omitted: return f;  // coefficient 0, variance 0
            case CoefKind::NeedsSample:
                needs_sample_ = true;
                throw Fail(r.name + " cannot be classified without the estimation sample");
            case CoefKind::NotFound: break;
        }
        throw Fail(r.name + " not found");
    }
};

}  // namespace postest
}  // namespace cecon
