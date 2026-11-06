// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// expr_blanks.h -- the blanks Stata takes out of an expression before reading it,
// and its refusal of brackets that do not pair.
//
// One definition for both evaluators that read Stata expression text: cobra's
// row Parser (frame.cpp: generate/replace and every `if`) and PIET's
// display/scalar/local layer (eval/macros.cpp evalArith), so `abs(1 2)` cannot be
// 12 in one and an error in the other.
//
// Stata drops every blank inside parentheses and brackets, string literals (plain
// and compound) kept as typed (StataBE 19.5, FU-875): `abs(1 2)` is abs(12),
// `max(1 2, 3 4)` is 34, `(a b)` reads the variable ab, `max(a,b c)` is "bc not
// found" r(111), `a[1 2]` is a[12], `strlen("a b")` stays 3. Between top-level
// operands a blank is not dropped: `generate g = 1 2` is "invalid '2'" r(198).
#pragma once
#include <cctype>
#include <string>

namespace cecon {

// One past the end of the compound-quoted string `"..."' that opens at `i`,
// nested pairs counted; src.size() when it never closes. Its body is a string
// literal whatever quotes it holds (`"a "b" c"'), so neither rule below looks in.
inline size_t compound_quote_end(const std::string& src, size_t i) {
    int depth = 0;
    for (size_t k = i; k + 1 < src.size(); ++k) {
        if (src[k] == '`' && src[k + 1] == '"') {
            ++depth;
            ++k;
        } else if (src[k] == '"' && src[k + 1] == '\'') {
            if (--depth == 0) return k + 2;
            ++k;
        }
    }
    return src.size();
}

inline bool opens_compound_quote(const std::string& src, size_t i) {
    return src[i] == '`' && i + 1 < src.size() && src[i + 1] == '"';
}

inline std::string squeeze_bracketed_blanks(const std::string& src) {
    std::string out;
    out.reserve(src.size());
    int depth = 0;
    bool quoted = false;
    for (size_t i = 0; i < src.size(); ++i) {
        const char c = src[i];
        if (!quoted && opens_compound_quote(src, i)) {
            const size_t end = compound_quote_end(src, i);
            out.append(src, i, end - i);
            i = end - 1;
            continue;
        }
        if (c == '"') quoted = !quoted;
        if (!quoted) {
            if (c == '(' || c == '[') ++depth;
            else if ((c == ')' || c == ']') && depth > 0) --depth;
            else if (depth > 0 && std::isspace(static_cast<unsigned char>(c))) continue;
        }
        out.push_back(c);
    }
    return out;
}

// Stata's refusal of an expression whose parentheses or brackets do not pair up,
// r(132) in every command (StataBE 19.5): a closer with no opener of its own kind
// before it is "too many ')' or ']'" (`1+2)`, `(1]`, `)1(`), an opener left at
// the end "too few ')' or ']'" (`tq(2010q1`, `max(1 2`, `a[1`). The two kinds
// are counted apart -- `a[(1])` passes this test and fails later, r(303), in
// Stata too. String literals, compound ones too, are skipped. "" when the text
// pairs up.
inline std::string bracket_imbalance(const std::string& src) {
    int paren = 0, bracket = 0;
    bool quoted = false;
    for (size_t i = 0; i < src.size(); ++i) {
        const char c = src[i];
        if (!quoted && opens_compound_quote(src, i)) {
            i = compound_quote_end(src, i) - 1;
            continue;
        }
        if (c == '"') quoted = !quoted;
        if (quoted) continue;
        if (c == '(') ++paren;
        else if (c == '[') ++bracket;
        else if ((c == ')' && --paren < 0) || (c == ']' && --bracket < 0))
            return "too many ')' or ']'";
    }
    return paren > 0 || bracket > 0 ? "too few ')' or ']'" : "";
}

}  // namespace cecon
