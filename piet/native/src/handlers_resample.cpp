// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// resampling prefixes — `bootstrap:`, `jackknife:`, `permute:` (P6 wave).
//
// DESIGN — the loop lives HERE, on the PIET side, and the inner command runs
// through the ORDINARY dispatcher. That is what makes these prefixes general:
// whatever PIET can run, they can wrap, and whatever the expression layer can
// evaluate against r()/e()/_b[] is a legal exp_list element. There is no
// engine-side "resample" verb and no list of blessed inner commands.
//
// The shape, once, for all three methods:
//
//     run the inner command ONCE           -> the observed statistics
//     for each replicate:
//         transform the data               (the ONLY per-method difference)
//         run the inner command again      (normal dispatch, output discarded)
//         evaluate the exp_list            (r()/e()/_b[] as of that run)
//     restore the session's dataset; aggregate; render Stata's table
//
// WHY THE EXP_LIST MUST NOT BE MACRO-EXPANDED AT PARSE TIME. dispatch() expands
// macro and r() references on the raw line BEFORE routing (step 0b). For an
// ordinary command that is right; for `bootstrap mean=r(mean), reps(30):
// summarize y` it is fatal — `r(mean)` would be substituted with whatever the
// PREVIOUS command left (or error as unset) instead of being evaluated after
// each replicate. So the resampling prefixes are intercepted BEFORE that step
// and hand the exp_list here verbatim; only the options and the inner command
// are expanded, because those may legitimately carry locals
// (`reps(`R')`, `: regress `depvar' x`).
//
// RNG. `bootstrap` draws Stata's own replicates (User ruling 2026-10-27: the
// session RNG is Stata-exact MT19937-64 and resampling draws follow the Stata
// stream). runBootstrap below implements bootstrap's behaviour, identified
// against StataBE probes (scratch-r150/lee/bs/o1.do/.log): `seed()` is a
// `set seed` issued after the full-sample run, each replicate is one `bsample`
// call (bsample_draws.h, shared with the `bsample` command) on the session
// stream, so with the same seed PIET reproduces Stata's bootstrap standard
// errors, and without one the draws continue the session stream as Stata's do.
// `permute` still shuffles on Philox4x32-10 (the engine's counter-based rng.h,
// reproducible for a fixed seed() but NOT Stata's permutation stream): it is
// checked on its own counting identities, not on Stata's p-values.
//
// MATERIALIZATION — PIET has no hbin writer, so a computed column reaches the
// data the way every other PIET-side computation does: write a CSV, run `cobra
// convert`, join it back (the strmutate.cpp precedent). Each replicate is a
// handful of engine calls; on the resident `serve` worker those are in-process,
// not process spawns. This is not free, and the wave records the cost rather
// than hiding it — the engine-side fast path is a named P2-PERF candidate, not
// something to smuggle in here.
#include "piet/handlers.h"

#include <algorithm>
#include <cctype>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <numeric>
#include <optional>
#include <regex>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "bsample_draws.h"       // bsample's Stata draws (shared with the bsample command)
#include "core/ident.h"          // the shared Unicode name rule
#include "core/distributions.h"  // t_critical / t_twosided_pvalue / z_critical (shared with cobra)
#include "core/rng.h"            // Philox4x32-10, the engine's own counter-based RNG (permute)
#include "handlers_e3_common.h"  // postBV / installEsampleMask / invsym / setE*OnFit
#include "io/win_compat.h"
#include "piet/cobra_runner.h"
#include "piet/datastore.h"
#include "piet/dispatcher.h"
#include "piet/eval/errors.h"
#include "piet/display_hardening.h"  // fmtStataDefaultG — Stata's %9.0g default rendering
#include "piet/eval/macros.h"
#include "piet/session_state.h"
#include "piet/scratch_seq.h"
#include "piet/strutil.h"

namespace piet {
namespace handlers {

using ojson = nlohmann::ordered_json;

namespace {

ojson rsError(const std::string& message, const std::string& code = "engine_error") {
  return ojson(eval::engineError(message, code));
}

// BUG-389: the tree's one minting body, so every replicate frame is in the mint
// registry and the exit sweep reclaims what no ledger owns (the probe's census
// counted 151 ORPHAN replicate files per bootstrap with the old local copy).
std::string nextPath(const std::string& workdir, const std::string& prefix) {
  return mintScratchPath(workdir, prefix);
}

CobraError errorFromRaw(const CobraRunResult& r) {
  std::string t = r.stderr_text;
  while (!t.empty() && (t.back() == '\n' || t.back() == '\r' || t.back() == ' ')) t.pop_back();
  try {
    nlohmann::json payload = nlohmann::json::parse(t);
    nlohmann::json err = payload.contains("error") ? payload.at("error") : payload;
    if (err.is_object() && err.contains("message")) {
      return CobraError(err.value("code", std::string("engine_error")),
                        err.at("message").get<std::string>(), err.value("detail", std::string()));
    }
  } catch (const nlohmann::json::exception&) {
  }
  return CobraError("engine_error", t.empty() ? "cobra failed" : t);
}

std::string cobraErrorStr(const CobraError& ce) { return cobraErrorMessage(ce); }

// The row-index column every replicate transform joins on. Underscore-prefixed
// and long enough that it cannot collide with a user variable in practice; it
// never survives into the session's own dataset.
constexpr const char* kRowIdx = "__piet_rs_row";

struct ExpItem {
  std::string name;  // the label Stata prints in the leftmost column
  std::string expr;  // RAW, evaluated fresh after every replicate
};

// One replicate's evaluated statistics, parallel to the exp_list.
using RepValues = std::vector<double>;

// ---------------------------------------------------------------------------
// exp_list parsing. Stata's grammar is `NAME=EXPR [NAME=EXPR ...]`, and an EXPR
// may itself contain spaces and brackets (`_b[x1]`, `r(mean)`), so the split is
// on the `NAME=` boundaries, not on whitespace.
std::vector<ExpItem> parseExpList(const std::string& raw, std::string* err) {
  std::vector<ExpItem> out;
  std::string s = raw;
  size_t i = 0;
  auto skipWs = [&]() { while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i]))) ++i; };
  while (true) {
    skipWs();
    if (i >= s.size()) break;
    const size_t nameStart = i;
    while (i < s.size() && cecon::ident::isIdentByte(s[i])) ++i;  // bytes >= 0x80: Unicode names
    const std::string name = s.substr(nameStart, i - nameStart);
    skipWs();
    if (name.empty() || i >= s.size() || s[i] != '=') {
      *err = "expression list must be `name=expression [name2=expression2 ...]` (got `" + raw + "`)";
      return {};
    }
    ++i;  // '='
    // The expression runs to the next `NAME=` at bracket depth zero, or to the end.
    const size_t exprStart = i;
    int depth = 0;
    size_t exprEnd = s.size();
    size_t j = i;
    while (j < s.size()) {
      const char c = s[j];
      if (c == '(' || c == '[') ++depth;
      else if (c == ')' || c == ']') --depth;
      else if (depth == 0 && std::isspace(static_cast<unsigned char>(c))) {
        // Look ahead: is the next token a `NAME=`? If so, this space ends the
        // expression; otherwise it is a space INSIDE the expression.
        size_t k = j;
        while (k < s.size() && std::isspace(static_cast<unsigned char>(s[k]))) ++k;
        size_t m = k;
        while (m < s.size() && cecon::ident::isIdentByte(s[m])) ++m;
        size_t n = m;
        while (n < s.size() && std::isspace(static_cast<unsigned char>(s[n]))) ++n;
        if (m > k && n < s.size() && s[n] == '=' && (n + 1 >= s.size() || s[n + 1] != '=')) {
          exprEnd = j;
          break;
        }
      }
      ++j;
    }
    if (j >= s.size()) exprEnd = s.size();
    std::string expr = s.substr(exprStart, exprEnd - exprStart);
    while (!expr.empty() && std::isspace(static_cast<unsigned char>(expr.back()))) expr.pop_back();
    while (!expr.empty() && std::isspace(static_cast<unsigned char>(expr.front()))) expr.erase(0, 1);
    if (expr.empty()) {
      *err = "empty expression for `" + name + "`";
      return {};
    }
    out.push_back(ExpItem{name, expr});
    i = exprEnd;
  }
  if (out.empty()) *err = "expression list required";
  return out;
}

// Evaluate one exp_list element AGAINST THE CURRENT SESSION STATE — i.e. after
// the inner command has just run and posted its r()/e()/_b[]. This is the same
// resolver `display r(mean)` uses, which is exactly the point: the exp_list can
// say anything a display can.
std::optional<double> evalStat(const std::string& expr, SessionState& session) {
  // TWO resolvers, in this order, because they cover different names and
  // neither is a superset of the other:
  //   expandMacros  — locals/globals, r(), e(), s()
  //   evalArith     — the estimation scalars `_b[...]`/`_se[...]`
  //                   (resolveEstScalarRefs), matrix subscripts, arithmetic
  // Using only the first leaves `_b[x1]` unresolved; using only the second
  // leaves `r(mean)` unresolved. An exp_list may hold either, so it gets both.
  std::string text = expr;
  if (eval::hasMacroRefs(text)) {
    eval::ExpandResult r = eval::expandMacros(text, session.eval);
    if (r.isError) return std::nullopt;
    text = r.line;
  }
  std::optional<std::string> folded = eval::evalArith(text, &session.eval);
  const std::string& out = folded.has_value() ? *folded : text;
  try {
    size_t pos = 0;
    const double v = std::stod(out, &pos);
    while (pos < out.size() && std::isspace(static_cast<unsigned char>(out[pos]))) ++pos;
    if (pos != out.size()) return std::nullopt;
    return v;
  } catch (const std::exception&) {
    return std::nullopt;
  }
}

// A one-column CSV of int64 values, for `cobra convert` to turn into an hbin.
bool writeIndexCsv(const std::string& path, const std::string& colName,
                   const std::vector<int64_t>& vals) {
  std::ofstream f(cecon::to_native_path(path));
  if (!f) return false;
  f << colName << "\n";
  for (int64_t v : vals) f << v << "\n";
  return static_cast<bool>(f);
}

// P2-PERF ARC-C C2-②: the HEADERLESS single-column payload `cobra setcol`
// reads (`Frame::read_csv_column` — one raw value per row, row count == n_rows).
// It replaces the old two-column (index, value) CSV, whose whole purpose was to
// give a 1:1 merge something to join on. permute does not need a join: the
// permuted values are already in the base frame's own row order, so the column
// can simply be written over the one it replaces.
bool writePermuteValuesCsv(const std::string& path, const std::vector<double>& vals) {
  std::ofstream f(cecon::to_native_path(path));
  if (!f) return false;
  f.setf(std::ios::fmtflags(0), std::ios::floatfield);
  f.precision(17);
  for (double v : vals) {
    if (!std::isnan(v)) f << v;
    f << "\n";
  }
  return static_cast<bool>(f);
}

std::string commaGroup(int64_t v) {
  std::string s = std::to_string(v < 0 ? -v : v);
  std::string out;
  int c = 0;
  for (auto it = s.rbegin(); it != s.rend(); ++it) {
    if (c && c % 3 == 0) out.push_back(',');
    out.push_back(*it);
    ++c;
  }
  if (v < 0) out.push_back('-');
  std::reverse(out.begin(), out.end());
  return out;
}


// --- Stata table rendering ---------------------------------------------------
// The oracle log IS the layout spec: these reproduce the column widths and the
// rules of `bootstrap`/`jackknife`/`permute` output.
std::string g9(double v) { return fmtStataDefaultG(v); }

// BUG-183: a COEFFICIENT-TABLE cell is one character narrower than the shared
// %9.0g `display` renderer — the coefficient table reserves the sign column, so the
// digits get 8 characters where `display` gets 9. `fmtStataDefaultG` is
// calibrated for `display` (its own doc comment records
// `2.157789219986937 -> "2.1577892"`, nine characters) and is correct THERE, so
// the fix is a second, narrower entry point at the table call sites rather than
// a change to the shared renderer — moving the shared one would break `display`.
//
// Exposure is narrow and was measured, not assumed: the two rules coincide below
// |1| (".2071479" is eight characters either way) and, above it, whenever the 9th
// character would be a trailing zero, which `%g` strips. So this only prints
// differently when |value| >= 1 AND its 8th significant digit is nonzero — which
// is why the P6 answerbook stayed byte-identical (its jackknife CI upper is
// `1.099479`, exactly the trailing-zero case) and why this was filed LATENT.
//
// Kept verbatim in sync with `g9cell` in handlers_qe.cpp, which is where the P-QE
// table arc first hit this wall (rendering did_imputation, whose tau values are
// all > 1). That file owns the original; this is the documented
// duplicate-and-keep-in-sync pattern this codebase already uses for TU-local
// helpers that cannot be shared without widening a header (see strutil.h's note).
// Oracle anchors for the rule itself: did_imputation tau0 1.2081958740803342
// prints `1.208196` not `1.2081959` (p3_stata.log:326); P6's jackknife CI upper
// prints `1.099479` (p6_stata.log:64).
std::string g9cell(double v) {
  std::string s = fmtStataDefaultG(v);
  const size_t sign = (!s.empty() && s[0] == '-') ? 1u : 0u;
  if (s.size() - sign <= 8) return s;
  for (int prec = 7; prec >= 1; --prec) {
    char buf[64];
    std::snprintf(buf, sizeof buf, "%.*g", prec, v);
    std::string t(buf);
    const size_t sg = (!t.empty() && t[0] == '-') ? 1u : 0u;
    if (t.size() > sg + 1 && t.compare(sg, 2, "0.") == 0) t.erase(sg, 1);
    if (t.size() - sg <= 8) return t;
  }
  return s;
}

// Widths count CHARACTERS (StataBE pads a Unicode name by characters:
// scratch-r150/uni-v/p4.do), so a multi-byte name stays aligned.
std::string padLeft(const std::string& s, size_t w) {
  const size_t n = cecon::ident::codePointCount(s);
  return n >= w ? s : std::string(w - n, ' ') + s;
}
std::string padRight(const std::string& s, size_t w) {
  const size_t n = cecon::ident::codePointCount(s);
  return n >= w ? s : s + std::string(w - n, ' ');
}
std::string fixed2(double v) {
  char buf[64];
  std::snprintf(buf, sizeof buf, "%.2f", v);
  return std::string(buf);
}
std::string fixed3(double v) {
  char buf[64];
  std::snprintf(buf, sizeof buf, "%.3f", v);
  return std::string(buf);
}
std::string fixed4(double v) {
  char buf[64];
  std::snprintf(buf, sizeof buf, "%.4f", v);
  return std::string(buf);
}
// Stata prints a probability in (0,1) without the leading zero (".0000").
std::string prob4(double v) {
  std::string s = fixed4(v);
  if (s.size() > 1 && s[0] == '0' && s[1] == '.') s.erase(0, 1);
  return s;
}

// Clopper-Pearson exact binomial interval — what Stata's permute reports as
// [95% CI(p)]. Verified against the oracle's own row: c=30/n=30 gives a lower
// limit of .8843 (= (alpha/2)^(1/n)) and c=0/n=30 an upper limit of .1157,
// which is the TWO-sided form, not the one-sided one. Inverted by bisection on
// the regularized incomplete beta (cecon::betai) rather than a second beta
// implementation of our own.
double betaInv(double target, double a, double b) {
  if (a <= 0) return 0.0;
  if (b <= 0) return 1.0;
  double lo = 0.0, hi = 1.0;
  for (int i = 0; i < 200; ++i) {
    const double mid = 0.5 * (lo + hi);
    if (cecon::betai(a, b, mid) < target) lo = mid; else hi = mid;
  }
  return 0.5 * (lo + hi);
}
std::pair<double, double> clopperPearson(int64_t c, int64_t n, double alpha) {
  const double lo = (c <= 0) ? 0.0
                             : betaInv(alpha / 2.0, static_cast<double>(c),
                                       static_cast<double>(n - c + 1));
  const double hi = (c >= n) ? 1.0
                             : betaInv(1.0 - alpha / 2.0, static_cast<double>(c + 1),
                                       static_cast<double>(n - c));
  return {lo, hi};
}

// Two-sided normal p-value, P(|Z| > |z|). erfc is the direct spelling; the
// engine has no normal p-value helper to share, so there is nothing to diverge
// from here.
double normalTwoSidedP(double z) { return std::erfc(std::fabs(z) * 0.7071067811865476); }

// bootstrap's exp_list: `name=exp` elements (exp one token or parenthesized) and
// UNNAMED ones, a parenthesized `(exp)` or a bare token such as
// `_b[treat:lower]`; an unnamed element at position i is named `_bs_i` and its
// e(exp#) loses the outer parentheses (StataBE scratch-r150/lee/bs/o5.log U1:
// `_b[treat:lower] (_b[treat:upper]*2)` -> _bs_1 _bs_2, e(exp2) `_b[treat:upper]*2`).
std::vector<ExpItem> parseBootstrapExpList(const std::string& s, std::string* err) {
  std::vector<ExpItem> out;
  size_t i = 0;
  auto ws = [&](size_t k) { return k < s.size() && std::isspace(static_cast<unsigned char>(s[k])); };
  // End of a bracket-balanced run starting at k that stops at depth-0 space.
  auto tokenEnd = [&](size_t k) {
    int depth = 0;
    while (k < s.size()) {
      const char c = s[k];
      if (c == '(' || c == '[') ++depth;
      else if (c == ')' || c == ']') --depth;
      else if (depth <= 0 && std::isspace(static_cast<unsigned char>(c))) break;
      ++k;
    }
    return k;
  };
  while (true) {
    while (ws(i)) ++i;
    if (i >= s.size()) break;
    size_t m = i;
    while (m < s.size() && cecon::ident::isIdentByte(s[m])) ++m;
    size_t n = m;
    while (ws(n)) ++n;
    const bool named = m > i && n < s.size() && s[n] == '=' && (n + 1 >= s.size() || s[n + 1] != '=');
    if (named) {
      // name=exp: the expression is ONE token -- a spaced expression must be
      // parenthesized, as in Stata (`d = _b[x] - (_b[_cons])` is "expressions
      // must be bound in parentheses", o6.log I2; the bare `-` token below).
      size_t j = n + 1;
      while (ws(j)) ++j;
      const size_t e = tokenEnd(j);
      std::string expr = s.substr(j, e - j);
      if (expr.size() >= 2 && expr.front() == '(' && expr.back() == ')') expr = expr.substr(1, expr.size() - 2);
      if (expr.empty()) {
        *err = "empty expression for `" + s.substr(i, m - i) + "`";
        return {};
      }
      out.push_back(ExpItem{s.substr(i, m - i), expr});
      i = e;
      continue;
    }
    const size_t e = tokenEnd(i);
    std::string expr = s.substr(i, e - i);
    if (std::strchr("+-*/^&|!<>=,", expr[0]) != nullptr) {
      *err = "expressions must be bound in parentheses";
      return {};
    }
    if (expr.size() >= 2 && expr.front() == '(' && expr.back() == ')') expr = expr.substr(1, expr.size() - 2);
    if (expr.empty() || expr == "_b" || expr == "_se") {
      *err = "`" + s.substr(i, e - i) +
             "` mixed with other elements is not implemented in this version of PIET (the "
             "default _b alone is)";
      return {};
    }
    out.push_back(ExpItem{"_bs_" + std::to_string(out.size() + 1), expr});
    i = e;
  }
  if (out.empty()) *err = "expression list required";
  return out;
}

// `cmd ... [if exp] [in range] [weight] [, options]` with the if and in removed
// (outside quotes and brackets; the if-expression runs to ` in `, `[` or the
// option comma). Spacing of what remains is normalized to single blanks.
std::string stripIfIn(const std::string& cmd) {
  std::string head = cmd, tail;
  const long long comma = eval::findUnquotedComma(cmd);
  if (comma >= 0) {
    head = cmd.substr(0, static_cast<size_t>(comma));
    tail = cmd.substr(static_cast<size_t>(comma) + 1);
  }
  const std::vector<bool> quoted = eval::quotedSpanMask(head);
  // Depth-0, unquoted word starts.
  std::vector<std::pair<size_t, size_t>> words;  // [begin, end)
  {
    int depth = 0;
    size_t i = 0;
    while (i < head.size()) {
      while (i < head.size() && std::isspace(static_cast<unsigned char>(head[i])) && depth == 0) ++i;
      if (i >= head.size()) break;
      const size_t b = i;
      while (i < head.size()) {
        const char c = head[i];
        const bool q = i < quoted.size() && quoted[i];
        if (!q && (c == '(' || c == '[')) ++depth;
        else if (!q && (c == ')' || c == ']') && depth > 0) --depth;
        else if (!q && depth == 0 && std::isspace(static_cast<unsigned char>(c))) break;
        ++i;
      }
      words.emplace_back(b, i);
    }
  }
  std::string out;
  for (size_t w = 0; w < words.size(); ++w) {
    const std::string word = head.substr(words[w].first, words[w].second - words[w].first);
    if (w > 0 && word == "if") {
      // skip the expression: up to the next `in` word or a `[weight]` word
      size_t k = w + 1;
      while (k < words.size()) {
        const std::string nw = head.substr(words[k].first, words[k].second - words[k].first);
        if (nw == "in" || nw[0] == '[') break;
        ++k;
      }
      w = k - 1;
      continue;
    }
    if (w > 0 && word == "in" && w + 1 < words.size()) {
      ++w;  // the range
      continue;
    }
    if (!out.empty()) out += ' ';
    out += word;
  }
  while (!tail.empty() && std::isspace(static_cast<unsigned char>(tail.front()))) tail.erase(0, 1);
  return tail.empty() ? out : out + ", " + tail;
}

// An error block carrying Stata's rc (coarseRc reads the `r(NNN)` reason).
ojson bsRc(const std::string& message, int rc, const std::string& code = "engine_error") {
  return ojson(eval::errorBlock(code, message, std::nullopt, std::nullopt, std::nullopt,
                                "r(" + std::to_string(rc) + ")"));
}

const ojson* firstErrorBlock(const ojson& blocks) {
  if (blocks.is_object()) return blocks.value("type", std::string()) == "error" ? &blocks : nullptr;
  if (!blocks.is_array()) return nullptr;
  for (const auto& b : blocks)
    if (b.is_object() && b.value("type", std::string()) == "error") return &b;
  return nullptr;
}

// Stata's `_pctile x, p(#)` (the default definition) on ascending `x`:
// P = n*p/100, i = floor(P); x_(i+1) when P has a fractional part, else the
// mean of x_(i) and x_(i+1) (clamped at the ends).
double pctileStata(const std::vector<double>& x, double p) {
  const size_t n = x.size();
  if (n == 0) return std::nan("");
  const double P = static_cast<double>(n) * p / 100.0;
  const double fi = std::floor(P);
  const long long i = static_cast<long long>(fi);
  auto at = [&](long long k) {  // 1-based, clamped
    if (k < 1) k = 1;
    if (k > static_cast<long long>(n)) k = static_cast<long long>(n);
    return x[static_cast<size_t>(k - 1)];
  };
  if (P - fi > 0) return at(i + 1);
  return (at(i) + at(i + 1)) / 2.0;
}

// chi2tail(df, x) for the bootstrap Wald model test. PIET's expression layer
// has no chi2tail(), so: df == 1 is 2*normal(-sqrt(x)) on Stata's normal()
// (cecon::stata_normal); otherwise the regularized upper incomplete gamma
// Q(df/2, x/2) by series / continued fraction -- the form handlers_e3_qreg2.cpp
// chi2tailQ uses (file-local there, so kept in step here).
double chi2tailBs(double df, double x) {
  if (!(x > 0)) return 1.0;
  if (df == 1.0) return 2.0 * cecon::stata_normal(-std::sqrt(x));
  const double a = df / 2.0, z = x / 2.0;
  const double gln = std::lgamma(a);
  if (z < a + 1.0) {
    double ap = a, sum = 1.0 / a, del = sum;
    for (int n = 0; n < 1000; ++n) {
      ap += 1.0;
      del *= z / ap;
      sum += del;
      if (std::fabs(del) < std::fabs(sum) * 1e-16) break;
    }
    return 1.0 - sum * std::exp(-z + a * std::log(z) - gln);
  }
  double b = z + 1.0 - a, c = 1.0 / 1e-300, d = 1.0 / b, h = d;
  for (int i = 1; i < 1000; ++i) {
    const double an = -i * (i - a);
    b += 2.0;
    d = an * d + b;
    if (std::fabs(d) < 1e-300) d = 1e-300;
    c = b + an / c;
    if (std::fabs(c) < 1e-300) c = 1e-300;
    d = 1.0 / d;
    const double del = d * c;
    h *= del;
    if (std::fabs(del - 1.0) < 1e-16) break;
  }
  return std::exp(-z + a * std::log(z) - gln) * h;
}

// A posted e() matrix (replaces one of the same name).
void setEMatrix(SessionState& session, const std::string& name, eval::EvalMatrix m) {
  for (auto& kv : session.eval.eMatrices) {
    if (kv.first == name) {
      kv.second = std::move(m);
      return;
    }
  }
  session.eval.eMatrices.emplace_back(name, std::move(m));
}

// Stata's option-abbreviation rule for one option: `key` names `full` when it
// is a prefix of it at least `minLen` long.
bool optIs(const std::string& key, const char* full, size_t minLen) {
  const std::string f(full);
  return key.size() >= minLen && key.size() <= f.size() && f.compare(0, key.size(), key) == 0;
}

// _prefix_title: the inner e(title) when it has one, else bootstrap.ado's
// table of titles for a few commands, else "Bootstrap results".
std::string bootstrapTitle(const std::string& innerTitle, const std::string& cmdname) {
  if (!innerTitle.empty()) return innerTitle;
  static const std::vector<std::pair<std::string, std::string>> k = {
      {"cnreg", "Censored normal regression"},   {"logit", "Logistic regression"},
      {"mlogit", "Multinomial logistic regression"}, {"ologit", "Ordered logistic regression"},
      {"oprobit", "Ordered probit regression"},  {"probit", "Probit regression"},
      {"regress", "Linear regression"},          {"tobit", "Tobit regression"}};
  for (const auto& [c, t] : k)
    if (c == cmdname) return t;
  return "Bootstrap results";
}

}  // namespace

// ---------------------------------------------------------------------------
// `bootstrap [exp_list] [, options] : command`
//
// Implements bootstrap's behaviour (with bsample), identified against StataBE probes (scratch-r150/lee/bs/o1.do,
// o1.log; T-numbers below are its sections):
//   * e() and r() are cleared, then the command runs once on the data (the
//     observed statistics). An e-class command's e(sample) is the resampling
//     base: rows outside it are dropped before the replicates (T6: 54 of 60
//     rows after a missing regressor; T10: `if x > 2`). A command that leaves
//     no e(sample) resamples the whole data.
//   * No exp_list (or `_b`): every column of the command's e(b), named as e(b)
//     names them, equation stripes included (T1, T4, T11 `y:x ... c:_cons`).
//     A command without e(b) is r(100) "expression list required".
//   * seed(#) is `set seed #` after the full-sample run (T9: `set seed 123` +
//     bootstrap equals seed(123); without seed() the draws continue the
//     session stream). Nothing else draws between replicates.
//   * cluster(var): the base is sorted by the cluster (stable; a string cluster
//     sorts by its bytes, T7) and each replicate is `bsample, cluster()`
//     (bsdraw::clustSrswr); a missing cluster value in the base is r(459)
//     (T8), one cluster r(460). Without cluster() each replicate is the
//     no-option `bsample` (bsdraw::srswrFast).
//   * Replicates are posted with `postfile` in FLOAT (bootstrap's default
//     storage), so e(V) is the (n-1)-divisor covariance of the float-rounded
//     replicate values over the replicates with no missing statistic; a
//     replicate whose command fails or whose e(b) changes shape posts missing.
//   * e(): e(b) the observed values, e(V) the replicate covariance, the
//     inner e() copied (default _b only, less F/chi2/df_r/df_m), e(cmd) the
//     inner e(cmd) for the default _b and "bootstrap" otherwise, plus
//     bootstrap's own: N, N_reps, N_misreps, N_clust, level, k_eq, k_exp,
//     k_eexp, k_extra, rank, bs_version, prefix, cmdname, command, exp#,
//     vce, vcetype, clustvar/cluster, title, rngstate, the Wald model test
//     (chi2/df_m/p/chi2type, default _b only) and the matrices b_bs, reps,
//     bias, z0, se, ci_normal, ci_percentile, ci_bc, V_modelbased.
//   * r(table): normal-based (z, df missing, crit invnormal(.975)).
// Refused by name (not implemented): strata(), size(), idcluster(), saving(),
// bca, mse, nodrop, reject(), and an r-class command with if/in.
static ojson runBootstrap(const std::string& expListRaw, const std::string& optsRaw,
                          const std::string& innerCmd, SessionState& session) {
  const std::string method = "bootstrap";
  if (!session.currentHbin().has_value()) return eval::noDataLoaded();
  if (innerCmd.empty()) return rsError("bootstrap: no command found after the colon");

  // -- the exp_list ----------------------------------------------------------
  std::string expTrim = expListRaw;
  while (!expTrim.empty() && std::isspace(static_cast<unsigned char>(expTrim.back()))) expTrim.pop_back();
  while (!expTrim.empty() && std::isspace(static_cast<unsigned char>(expTrim.front()))) expTrim.erase(0, 1);
  const bool bMode = expTrim.empty() || expTrim == "_b" || expTrim == "(_b)";
  std::vector<ExpItem> expList;
  if (!bMode) {
    std::string perr;
    expList = parseBootstrapExpList(expTrim, &perr);
    if (expList.empty()) {
      if (perr == "expressions must be bound in parentheses") return bsRc(perr, 198, "syntax_error");
      return rsError("bootstrap: " + perr);
    }
  }

  // -- options ---------------------------------------------------------------
  int reps = 50;
  std::string seedText;
  std::string clusterVar;
  double level = static_cast<double>(session.level);
  {
    size_t i = 0;
    while (i < optsRaw.size()) {
      while (i < optsRaw.size() &&
             (std::isspace(static_cast<unsigned char>(optsRaw[i])) || optsRaw[i] == ',')) ++i;
      if (i >= optsRaw.size()) break;
      const size_t ns = i;
      while (i < optsRaw.size() &&
             (std::isalnum(static_cast<unsigned char>(optsRaw[i])) || optsRaw[i] == '_')) ++i;
      const std::string key = strutil::lower(optsRaw.substr(ns, i - ns));
      std::string val;
      bool hasVal = false;
      if (i < optsRaw.size() && optsRaw[i] == '(') {
        hasVal = true;
        int depth = 1;
        ++i;
        const size_t vs = i;
        while (i < optsRaw.size() && depth > 0) {
          if (optsRaw[i] == '(') ++depth;
          else if (optsRaw[i] == ')') --depth;
          if (depth > 0) ++i;
        }
        val = optsRaw.substr(vs, i - vs);
        if (i < optsRaw.size()) ++i;
      }
      if (key.empty()) break;
      auto trimmed = [](std::string v) {
        while (!v.empty() && std::isspace(static_cast<unsigned char>(v.back()))) v.pop_back();
        while (!v.empty() && std::isspace(static_cast<unsigned char>(v.front()))) v.erase(0, 1);
        return v;
      };
      if (optIs(key, "reps", 1)) {
        try {
          size_t pos = 0;
          const std::string v = trimmed(val);
          reps = std::stoi(v, &pos);
          if (pos != v.size()) throw std::invalid_argument(v);
        } catch (const std::exception&) {
          return bsRc("option reps() incorrectly specified", 198, "syntax_error");
        }
      } else if (key == "seed" || key == "rseed") {
        seedText = trimmed(val);
        if (seedText.empty()) return bsRc("option " + key + "() incorrectly specified", 198, "syntax_error");
      } else if (optIs(key, "cluster", 2)) {
        std::istringstream is(val);
        std::vector<std::string> names;
        for (std::string w; is >> w;) names.push_back(w);
        if (names.size() != 1)
          return rsError("bootstrap: cluster() with " + std::to_string(names.size()) +
                             " variables is not implemented in this version of PIET (one cluster "
                             "variable is).",
                         "unsupported_option");
        clusterVar = names[0];
      } else if (optIs(key, "level", 1)) {
        try {
          level = std::stod(trimmed(val));
        } catch (const std::exception&) {
          return bsRc("option level() incorrectly specified", 198, "syntax_error");
        }
        if (!(level >= 10.0 && level <= 99.99))
          return bsRc("level() must be between 10 and 99.99 inclusive", 198, "syntax_error");
      } else if (!hasVal && (key == "nodots" || key == "noisily" || key == "nowarn" ||
                             key == "notable" || key == "noheader" || key == "nolegend" ||
                             key == "verbose" || optIs(key, "noisily", 3) || optIs(key, "verbose", 1))) {
        // Presentation-only switches: nothing about the numbers depends on them.
      } else if (key == "dots") {
        // dots(#): progress output, which PIET does not print.
      } else {
        return rsError("bootstrap: option `" + key +
                           "` is not implemented in this version of PIET. Implemented: reps(), "
                           "seed(), cluster(), level(), and the presentation switches "
                           "(nodots/notable/...).",
                       "unsupported_option");
      }
    }
  }
  if (reps < 2) return bsRc("option reps() requires an integer greater than 1", 198, "syntax_error");

  try {
    (void)session.datastore();
  } catch (const std::exception& e) {
    return rsError(std::string("bootstrap: could not materialize the working dataset: ") + e.what());
  }
  const std::string startHbin = *session.currentHbin();
  std::istringstream cmdWords(innerCmd);
  std::string cmdname;
  cmdWords >> cmdname;
  {
    const size_t cut = cmdname.find(',');
    if (cut != std::string::npos) cmdname = cmdname.substr(0, cut);
  }
  // The command each replicate runs (and e(command)): the inner command without
  // its if/in -- the resampling base is already restricted to its sample, and an
  // `in` range or `_n` condition must not be re-applied to the resampled rows --
  // options and weights kept (bootstrap.ado's `command`; o1.log T10 e(command)
  // `regress y x` for `regress y x if x > 2`).
  const std::string repCmd = stripIfIn(innerCmd);

  // -- 1. the full-sample run ------------------------------------------------
  // `_prefix_clear, e r` first: an r-class command then leaves no e() behind,
  // which is how "no e(sample)" is told apart from the previous model's.
  (void)dispatch("ereturn clear", session);
  eval::replaceRClass(session.eval);
  {
    const ojson obs = dispatch(innerCmd, session);
    if (const ojson* err = firstErrorBlock(obs)) {
      session.load(startHbin);
      return *err;  // _prefix_run_error: the command's own error and rc
    }
  }
  std::vector<std::string> names;  // e(b)'s column names (eq:name when striped)
  std::vector<std::string> exprs;  // e(exp#)
  std::vector<double> observed;
  std::optional<eval::EvalMatrix> obsV;
  if (bMode) {
    int ebRc = 0;
    std::string ebWhy;
    const std::optional<eval::EvalMatrix> eb = session.eval.resolveEMatrix("b", &ebRc, &ebWhy);
    if (!eb.has_value() || eb->data.size() != 1 || eb->data[0].empty() ||
        eb->data[0].size() != eb->colnames.size()) {
      // A command that posts no e(b) is Stata's r(100); one whose e(b) this
      // build refuses to build keeps that named refusal.
      if (!eb.has_value() && ebRc == 198 && !ebWhy.empty())
        return bsRc("bootstrap: the default exp_list is _b, and " + ebWhy, 198, "unsupported_option");
      return bsRc("expression list required", 100);
    }
    names = eb->colnames;
    observed = eb->data[0];
    // e(exp#): `_b[x]`, or `[eq]_b[x]` on a striped e(b) (o2.log A3).
    for (const auto& n : names) {
      const size_t colon = n.find(':');
      exprs.push_back(colon == std::string::npos
                          ? "_b[" + n + "]"
                          : "[" + n.substr(0, colon) + "]_b[" + n.substr(colon + 1) + "]");
    }
    obsV = session.eval.resolveEMatrix("V");
  } else {
    for (const auto& e : expList) {
      std::optional<double> v = evalStat(e.expr, session);
      if (!v.has_value()) {
        return rsError("bootstrap: could not evaluate `" + e.expr + "` after running `" + innerCmd +
                       "` — it does not resolve to a number.");
      }
      names.push_back(e.name);
      exprs.push_back(e.expr);
      observed.push_back(*v);
    }
  }
  for (size_t k = 0; k < observed.size(); ++k) {
    if (std::isnan(observed[k])) {
      return bsRc("'" + exprs[k] + "' evaluated to missing in full sample", 322);
    }
  }
  const size_t K = names.size();

  // What the copied e() carries (default _b only): the inner fit's block, its
  // posted scalars/macros, its e(cmd)/e(depvar)/e(title).
  std::optional<ojson> innerBlock;
  std::vector<std::pair<std::string, double>> innerScalars;
  std::vector<std::pair<std::string, std::string>> innerStrings;
  const std::string innerECmd = session.eval.eStrings.get("cmd", std::string());
  const std::string innerDepvar = session.eval.eStrings.get("depvar", std::string());
  const std::string innerTitle = session.eval.eStrings.get("title", std::string());
  double innerDfM = std::nan("");
  if (bMode) {
    if (session.lastEstBlock.has_value()) innerBlock = *session.lastEstBlock;
    for (const auto& [k, v] : session.eval.eScalars.items()) innerScalars.emplace_back(k, v);
    for (const auto& [k, v] : session.eval.eStrings.items()) innerStrings.emplace_back(k, v);
    if (const std::optional<double> dfm = evalStat("e(df_m)", session)) innerDfM = *dfm;
  }

  // -- 2. the resampling base ------------------------------------------------
  // The working frame now (the full-sample run may have installed its
  // e(sample) column on it) is the frame the session returns to.
  try {
    (void)session.datastore();
  } catch (const std::exception& e) {
    return rsError(std::string("bootstrap: could not materialize the working dataset: ") + e.what());
  }
  const std::string obsHbin = *session.currentHbin();
  int64_t nHead = 0;
  std::vector<char> inBase;  // per head row: in the resampling base (and e(sample) after)
  bool haveEsample = false;
  {
    Datastore d(obsHbin);
    nHead = d.nRows();
    inBase.assign(static_cast<size_t>(nHead), 1);
    const SessionState::EsampleReading er = session.esampleReading();
    if (er.kind == SessionState::EsampleReading::Kind::kRefused) {
      return rsError("bootstrap: the estimation sample of `" + innerCmd +
                         "` cannot be determined in this version of PIET, so the rows to resample "
                         "are unknown: " + er.refusal,
                     "unsupported_option");
    }
    if (er.kind == SessionState::EsampleReading::Kind::kColumn && d.has(kEsampleColumn)) {
      const double* m = d.col(kEsampleColumn).f64();
      int64_t ones = 0;
      for (int64_t i = 0; i < nHead; ++i) {
        const bool in = m != nullptr && m[i] == static_cast<double>(er.serial);
        inBase[static_cast<size_t>(i)] = in ? 1 : 0;
        ones += in ? 1 : 0;
      }
      // _prefix_check4esample: all-0 e(sample) is "no e(sample)" (whole data).
      if (ones == 0) std::fill(inBase.begin(), inBase.end(), 1);
      else haveEsample = true;
    }
    if (!haveEsample) {
      // An r-class command: Stata marks its if/in and resamples those rows;
      // this build does not evaluate the inner qualifier itself.
      static const std::regex reQual(R"(\s(if|in)\s)");
      std::smatch qm;
      if (eval::searchUnquoted(" " + innerCmd + " ", reQual, qm)) {
        return rsError("bootstrap: `" + innerCmd +
                           "` sets no e(sample), and resampling the rows of its if/in qualifier "
                           "is not implemented in this version of PIET; restrict the data first "
                           "(preserve + keep if ...).",
                       "unsupported_option");
      }
    }
  }
  std::vector<int64_t> baseRows;  // 0-based head rows, in resampling order
  for (int64_t i = 0; i < nHead; ++i)
    if (inBase[static_cast<size_t>(i)]) baseRows.push_back(i);
  const int64_t nBase = static_cast<int64_t>(baseRows.size());
  if (nBase <= 1) {
    return bsRc("insufficient observations to compute bootstrap standard errors", 2000);
  }

  // cluster(): sort the base by the cluster, stable; number the clusters.
  std::vector<int64_t> clid;
  int64_t nClust = 0;
  if (!clusterVar.empty()) {
    Datastore d(obsHbin);
    if (!d.has(clusterVar) || isHiddenColumn(clusterVar)) {
      return bsRc("variable " + clusterVar + " not found", 111);
    }
    const cecon::Column& c = d.col(clusterVar);
    const bool isStr = c.dtype == cecon::DType::Factor;
    std::vector<std::string> skey(isStr ? static_cast<size_t>(nBase) : 0);
    std::vector<double> nkey(isStr ? 0 : static_cast<size_t>(nBase));
    for (int64_t k = 0; k < nBase; ++k) {
      const int64_t row = baseRows[static_cast<size_t>(k)];
      bool missing = false;
      if (isStr) {
        const int32_t code = c.codes()[row];
        missing = code < 0 || static_cast<size_t>(code) >= c.levels.size() ||
                  c.levels[static_cast<size_t>(code)].empty();
        if (!missing) skey[static_cast<size_t>(k)] = c.levels[static_cast<size_t>(code)];
      } else {
        const double v = c.f64()[row];
        missing = std::isnan(v);
        nkey[static_cast<size_t>(k)] = v;
      }
      if (missing) {
        return bsRc("missing values in cluster variable " + clusterVar + " not allowed", 459);
      }
    }
    std::vector<int64_t> ord(static_cast<size_t>(nBase));
    std::iota(ord.begin(), ord.end(), int64_t{0});
    std::stable_sort(ord.begin(), ord.end(), [&](int64_t a, int64_t b) {
      return isStr ? skey[static_cast<size_t>(a)] < skey[static_cast<size_t>(b)]
                   : nkey[static_cast<size_t>(a)] < nkey[static_cast<size_t>(b)];
    });
    std::vector<int64_t> sorted(static_cast<size_t>(nBase));
    clid.assign(static_cast<size_t>(nBase), 0);
    for (size_t q = 0; q < ord.size(); ++q) {
      const size_t k = static_cast<size_t>(ord[q]);
      sorted[q] = baseRows[k];
      const bool fresh =
          q == 0 || (isStr ? skey[k] != skey[static_cast<size_t>(ord[q - 1])]
                           : nkey[k] != nkey[static_cast<size_t>(ord[q - 1])]);
      if (fresh) ++nClust;
      clid[q] = nClust;
    }
    baseRows = std::move(sorted);
    if (nClust == 1) {
      return bsRc("singleton cluster detected", 460);
    }
  }

  // -- 3. the seed, then the replicates --------------------------------------
  if (!seedText.empty()) {
    const ojson s = dispatch("set seed " + seedText, session);
    if (const ojson* err = firstErrorBlock(s)) {
      return *err;
    }
  }
  const std::string rngstate = session.seed.stream().state_string();

  const std::string basePathIdx = nextPath(session.workdir(), "bs_baseidx");
  {
    CobraRunResult cr = runCobra({"eval", "--data", obsHbin, "--egen",
                                  std::string(kRowIdx) + " = seq()", "--out", basePathIdx});
    if (!cr.ok) {
      CobraError ce = errorFromRaw(cr);
      return rsError(cobraErrorStr(ce), ce.code);
    }
  }
  SessionState::ScratchPin pin(session, {basePathIdx, startHbin, obsHbin});

  std::vector<std::vector<double>> repVals;  // one row per replicate, NaN = missing
  repVals.reserve(static_cast<size_t>(reps));
  std::string failure;
  for (int b = 0; b < reps && failure.empty(); ++b) {
    const std::vector<int64_t> pos =
        clusterVar.empty() ? bsdraw::srswrFast(session.seed.stream(), nBase)
                           : bsdraw::clustSrswr(session.seed.stream(), clid, nClust);
    std::vector<int64_t> idx;
    idx.reserve(pos.size());
    for (int64_t p : pos) idx.push_back(baseRows[static_cast<size_t>(p)] + 1);
    const std::string csv = session.workdir() + "/bs_idx.csv";
    registerScratchMint(csv);  // BUG-389
    if (!writeIndexCsv(csv, kRowIdx, idx)) { failure = "could not write the resample index"; break; }
    const std::string idxHbin = nextPath(session.workdir(), "bs_idx");
    CobraRunResult c1 = runCobra({"convert", "--data", csv, "--out", idxHbin, "--delim", ","});
    if (!c1.ok) { failure = cobraErrorStr(errorFromRaw(c1)); break; }
    const std::string merged = nextPath(session.workdir(), "bs_merged");
    // master = the drawn rows (with repeats, in the resample's row order),
    // using = the base keyed by the same index; the using-only (undrawn) rows
    // are not part of the sample.
    CobraRunResult c2 = runCobra({"merge", "--data", idxHbin, "--using", basePathIdx, "--type",
                                  "m:1", "--key", kRowIdx, "--out", merged});
    if (!c2.ok) { failure = cobraErrorStr(errorFromRaw(c2)); break; }
    const std::string repPath = nextPath(session.workdir(), "bs_rep");
    CobraRunResult c3 = runCobra({"eval", "--data", merged, "--keepif", "_merge!=2", "--drop",
                                  "_merge", "--out", repPath});
    if (!c3.ok) { failure = cobraErrorStr(errorFromRaw(c3)); break; }
    // The two intermediates are never loaded; drop them now rather than at exit.
    for (const std::string& p : {idxHbin, merged}) {
      cecon::native_unlink(p);
      forgetScratchMint(p);
    }

    session.load(repPath);
    const ojson inner = dispatch(repCmd, session);
    std::vector<double> vals(K, std::nan(""));
    if (firstErrorBlock(inner) == nullptr) {
      if (bMode) {
        const std::optional<eval::EvalMatrix> rb = session.eval.resolveEMatrix("b");
        // A replicate whose e(b) is not the full sample's columns posts missing
        // (_loop_bs's _check_omit rule).
        if (rb.has_value() && rb->data.size() == 1 && rb->colnames == names &&
            rb->data[0].size() == K)
          vals = rb->data[0];
      } else {
        for (size_t k = 0; k < K; ++k) {
          std::optional<double> v = evalStat(expList[k].expr, session);
          if (v.has_value()) vals[k] = *v;
        }
      }
    }
    // postfile stores the replicates as float (bootstrap's default storage).
    for (double& v : vals) v = e3::toFloat(v);
    repVals.push_back(std::move(vals));
  }
  session.load(obsHbin);
  // A failed run leaves no e(): the live one would be the last replicate's fit
  // on resampled data, which no later `test`/`predict` may read.
  auto failNoE = [&](const ojson& err) {
    (void)dispatch("ereturn clear", session);
    eval::replaceRClass(session.eval);
    return err;
  };
  if (!failure.empty()) return failNoE(rsError("bootstrap: " + failure));

  // -- 4. _bs_sum ------------------------------------------------------------
  std::vector<const std::vector<double>*> good;  // listwise: every statistic present
  for (const auto& rv : repVals) {
    bool ok = true;
    for (double v : rv) ok = ok && !std::isnan(v);
    if (ok) good.push_back(&rv);
  }
  const int64_t nReps = static_cast<int64_t>(good.size());
  const int64_t nMis = static_cast<int64_t>(repVals.size()) - nReps;
  if (nReps <= 1)
    return failNoE(bsRc("insufficient observations to compute bootstrap standard errors; no "
                        "results will be saved",
                        2000));
  std::vector<double> mean(K, 0.0);
  for (size_t k = 0; k < K; ++k) {
    long double s = 0;
    for (const auto* rv : good) s += (*rv)[k];
    mean[k] = static_cast<double>(s / static_cast<long double>(nReps));
  }
  std::vector<std::vector<double>> V(K, std::vector<double>(K, 0.0));
  for (size_t a = 0; a < K; ++a) {
    for (size_t c = 0; c <= a; ++c) {
      long double s = 0;
      for (const auto* rv : good)
        s += (static_cast<long double>((*rv)[a]) - mean[a]) * (static_cast<long double>((*rv)[c]) - mean[c]);
      V[a][c] = V[c][a] = static_cast<double>(s / static_cast<long double>(nReps - 1));
    }
  }
  const double zalpha = cecon::stata_invnormal((100.0 + level) / 200.0);
  std::vector<double> se(K), bias(K), z0(K), pLo(K), pHi(K), bcLo(K), bcHi(K);
  for (size_t k = 0; k < K; ++k) {
    se[k] = V[k][k] >= 0 ? std::sqrt(V[k][k]) : std::nan("");
    bias[k] = mean[k] - observed[k];
    std::vector<double> xs;
    xs.reserve(good.size());
    for (const auto* rv : good) xs.push_back((*rv)[k]);
    std::sort(xs.begin(), xs.end());
    int64_t ct = 0;
    for (double x : xs) ct += x <= observed[k] ? 1 : 0;
    z0[k] = bcLo[k] = bcHi[k] = std::nan("");
    if (ct > 0 && ct < nReps) {
      z0[k] = cecon::stata_invnormal(static_cast<double>(ct) / static_cast<double>(nReps));
      bcLo[k] = pctileStata(xs, 100.0 * cecon::stata_normal(2 * z0[k] - zalpha));
      bcHi[k] = pctileStata(xs, 100.0 * cecon::stata_normal(2 * z0[k] + zalpha));
    }
    pLo[k] = pctileStata(xs, (100.0 - level) / 2.0);
    pHi[k] = pctileStata(xs, (100.0 + level) / 2.0);
  }

  // rank of e(V) and the Wald model test
  // (`test [#1]`, the first equation's coefficients but its _cons).
  const std::vector<std::vector<double>> Vinv = e3::invsym(V);
  int rank = 0;
  for (size_t k = 0; k < K; ++k) rank += Vinv[k][k] != 0.0 ? 1 : 0;
  double chi2 = std::nan(""), chi2p = std::nan(""), dfM = std::nan("");
  const bool modelTest = bMode && std::isfinite(innerDfM);
  if (modelTest) {
    const std::string eq1 = names[0].find(':') == std::string::npos
                                ? std::string()
                                : names[0].substr(0, names[0].find(':'));
    std::vector<size_t> sel;
    for (size_t k = 0; k < K; ++k) {
      const size_t colon = names[k].find(':');
      const std::string eq = colon == std::string::npos ? std::string() : names[k].substr(0, colon);
      const std::string nm = colon == std::string::npos ? names[k] : names[k].substr(colon + 1);
      if (eq != eq1) continue;
      if (nm == "_cons") continue;
      if (V[k][k] == 0.0 && observed[k] == 0.0) continue;  // a base / omitted column
      sel.push_back(k);
    }
    std::vector<std::vector<double>> Vs(sel.size(), std::vector<double>(sel.size()));
    for (size_t a = 0; a < sel.size(); ++a)
      for (size_t c = 0; c < sel.size(); ++c) Vs[a][c] = V[sel[a]][sel[c]];
    const std::vector<std::vector<double>> Vsi = e3::invsym(Vs);
    int df = 0;
    long double q = 0;
    for (size_t a = 0; a < sel.size(); ++a) {
      if (Vsi[a][a] != 0.0) ++df;
      for (size_t c = 0; c < sel.size(); ++c)
        q += static_cast<long double>(observed[sel[a]]) * Vsi[a][c] * observed[sel[c]];
    }
    dfM = df;
    if (df > 0) {
      chi2 = static_cast<double>(q);
      chi2p = chi2tailBs(static_cast<double>(df), chi2);
    }
  }

  // -- 5. post e() -----------------------------------------------------------
  const double crit = zalpha;  // invnormal((100+level)/200), as e(ci_normal)
  ojson rows = ojson::array();
  for (size_t k = 0; k < K; ++k) {
    ojson r;
    r["name"] = names[k];
    r["b"] = observed[k];
    if (std::isfinite(se[k]) && se[k] > 0) {
      const double z = observed[k] / se[k];
      r["se"] = se[k];
      r["z"] = z;
      r["p"] = normalTwoSidedP(z);
      r["ci_lo"] = observed[k] - crit * se[k];
      r["ci_hi"] = observed[k] + crit * se[k];
    } else {
      r["se"] = std::isfinite(se[k]) ? ojson(se[k]) : ojson(nullptr);
      r["z"] = nullptr;
      r["p"] = nullptr;
      r["ci_lo"] = nullptr;
      r["ci_hi"] = nullptr;
    }
    rows.push_back(r);
  }
  ojson block = innerBlock.has_value() && innerBlock->is_object() ? *innerBlock : ojson::object();
  // bootstrap's exclusions from the copied e() (F chi2 df_r df_m, the fit's
  // own V and its p), plus the fit's stripe/rank/cluster keys, which describe
  // the engine's coefficient table rather than this posting.
  // An eform fit's `coef_metric` would make r(table) exponentiate these
  // coefficient-metric rows.
  for (const char* key : {"df_r", "df_m", "F", "p_F", "chi2", "p", "e_V", "eb_stripe", "k_vars",
                          "n_clusters", "vce", "vcetype", "cluster", "clustvar", "coef_metric"})
    block.erase(key);
  block["type"] = "estimation";
  block["estimator"] = "bootstrap";
  const double nObs = static_cast<double>(std::count(inBase.begin(), inBase.end(), char{1}));
  block["n_obs"] = static_cast<long long>(nObs);
  block["coeftable"] = rows;
  if (bMode && !innerDepvar.empty()) block["depvar"] = innerDepvar;
  else block.erase("depvar");

  LastModel lm;
  lm.cobraCmd = "bootstrap";
  lm.stataCmd = bMode && !innerECmd.empty() ? innerECmd : "bootstrap";
  lm.depvar = bMode ? innerDepvar : std::string();
  lm.vcov = "bootstrap";
  lm.eVce = "bootstrap";
  lm.result = block;
  if (!e3::installEsampleMask(session, lm, inBase))
    return rsError("bootstrap: the results were computed, but e(sample) could not be installed");
  session.setLastModel(lm);
  session.lastEstBlock = block;
  {
    std::vector<double> bvec = observed;
    e3::postBV(session, names, bvec, V);
  }
  // The copied inner e() (default _b only).
  if (bMode) {
    static const std::set<std::string> kXScalars = {"F", "chi2", "df_r", "df_m", "N", "N_clust",
                                                    "rank", "p", "level"};
    static const std::set<std::string> kXStrings = {
        "cmd", "_estimates_name", "chi2type", "novariance", "clustvar", "vce", "vcetype",
        "cmdline", "title", "properties", "depvar", "cluster", "prefix"};
    for (const auto& [k, v] : innerScalars)
      if (!kXScalars.count(k)) e3::setEScalarOnFit(session, k, v);
    for (const auto& [k, v] : innerStrings)
      if (!kXStrings.count(k)) e3::setEStringOnFit(session, k, v);
    if (obsV.has_value() && obsV->data.size() == K) setEMatrix(session, "V_modelbased", *obsV);
  }
  e3::setEScalarOnFit(session, "N", nObs);
  e3::setEScalarOnFit(session, "level", level);
  e3::setEScalarOnFit(session, "N_reps", static_cast<double>(nReps));
  e3::setEScalarOnFit(session, "N_misreps", static_cast<double>(nMis));
  e3::setEScalarOnFit(session, "bs_version", 3);
  e3::setEScalarOnFit(session, "rank", rank);
  std::set<std::string> eqs;
  for (const auto& n : names) {
    const size_t colon = n.find(':');
    eqs.insert(colon == std::string::npos ? std::string() : n.substr(0, colon));
  }
  e3::setEScalarOnFit(session, "k_eq", static_cast<double>(eqs.size()));
  e3::setEScalarOnFit(session, "k_exp", bMode ? 0.0 : static_cast<double>(K));
  e3::setEScalarOnFit(session, "k_eexp", bMode ? static_cast<double>(K) : 0.0);
  e3::setEScalarOnFit(session, "k_extra", 0);
  if (!clusterVar.empty()) {
    e3::setEScalarOnFit(session, "N_clust", static_cast<double>(nClust));
    e3::setEStringOnFit(session, "clustvar", clusterVar);
    e3::setEStringOnFit(session, "cluster", clusterVar);
  }
  if (modelTest) {
    e3::setEScalarOnFit(session, "df_m", dfM);
    if (std::isfinite(chi2)) {
      e3::setEScalarOnFit(session, "chi2", chi2);
      e3::setEScalarOnFit(session, "p", chi2p);
    }
    e3::setEStringOnFit(session, "chi2type", "Wald");
  }
  {
    // Stata's `bootstrap `0'' (o1.log T1 `bootstrap , rep(50) seed(123): regress y x`).
    std::string cmdline = "bootstrap " + expTrim + (optsRaw.empty() ? "" : ", " + optsRaw) + ": " +
                          innerCmd;
    e3::setEStringOnFit(session, "cmdline", cmdline);
  }
  e3::setEStringOnFit(session, "cmdname", cmdname);
  e3::setEStringOnFit(session, "prefix", "bootstrap");
  e3::setEStringOnFit(session, "title", bootstrapTitle(innerTitle, cmdname));
  e3::setEStringOnFit(session, "rngstate", rngstate);
  for (size_t k = 0; k < K; ++k) e3::setEStringOnFit(session, "exp" + std::to_string(k + 1), exprs[k]);
  e3::setEStringOnFit(session, "command", repCmd);
  e3::setEStringOnFit(session, "vcetype", "Bootstrap");
  e3::setEStringOnFit(session, "vce", "bootstrap");
  e3::setEStringOnFit(session, "properties", "b V");
  {
    auto rowVec = [&](const std::vector<double>& v) {
      eval::EvalMatrix m;
      m.data.push_back(v);
      m.rownames = {"y1"};
      m.colnames = names;
      return m;
    };
    auto ci = [&](const std::vector<double>& lo, const std::vector<double>& hi) {
      eval::EvalMatrix m;
      m.data = {lo, hi};
      m.rownames = {"ll", "ul"};
      m.colnames = names;
      return m;
    };
    std::vector<double> repsV(K, static_cast<double>(nReps)), nlo(K), nhi(K);
    for (size_t k = 0; k < K; ++k) {
      nlo[k] = observed[k] - zalpha * se[k];
      nhi[k] = observed[k] + zalpha * se[k];
    }
    setEMatrix(session, "b_bs", rowVec(mean));
    setEMatrix(session, "reps", rowVec(repsV));
    setEMatrix(session, "bias", rowVec(bias));
    setEMatrix(session, "z0", rowVec(z0));
    setEMatrix(session, "se", rowVec(se));
    setEMatrix(session, "ci_normal", ci(nlo, nhi));
    setEMatrix(session, "ci_percentile", ci(pLo, pHi));
    setEMatrix(session, "ci_bc", ci(bcLo, bcHi));
  }

  // r(): the coefficient table's r(table) (normal-based) and r(N).
  eval::replaceRClass(session.eval);
  (void)postRTableFromBlock(session, block, level);
  session.eval.rValues.set("N", nObs);

  // -- 6. render -------------------------------------------------------------
  ojson lines = ojson::array();
  auto say = [&lines](const std::string& t) { lines.push_back(ojson{{"type", "display"}, {"text", t}}); };
  if (haveEsample) say("(running " + cmdname + " on estimation sample)");
  say("");
  say(padRight(bootstrapTitle(innerTitle, cmdname), 56) + "Number of obs = " +
      padLeft(commaGroup(static_cast<int64_t>(nObs)), 6));
  say(padRight("", 56) + "Replications  = " + padLeft(std::to_string(nReps), 6));
  if (modelTest && std::isfinite(chi2)) {
    char buf[64];
    std::snprintf(buf, sizeof buf, "%.2f", chi2);
    say(padRight("", 56) + padRight("Wald chi2(" + std::to_string(static_cast<int>(dfM)) + ")", 14) +
        "= " + padLeft(buf, 6));
    say(padRight("", 56) + "Prob > chi2   = " + padLeft(fixed4(chi2p), 6));
  }
  if (!bMode) {
    say("");
    say("      Command: " + innerCmd);
    for (const auto& e : expList) say(padLeft(e.name, 13) + ": " + e.expr);
  }
  say("");
  if (!clusterVar.empty())
    say(padLeft("(Replications based on " + std::to_string(nClust) + " clusters in " + clusterVar + ")",
                78));
  const std::string rule(78, '-');
  say(rule);
  say("             |   Observed   Bootstrap                         Normal-based");
  {
    std::string corner = bMode && eqs.size() == 1 && eqs.count(std::string()) ? innerDepvar : "";
    say(padLeft(corner, 12) + " | coefficient  std. err.      z    P>|z|     [" +
        std::to_string(static_cast<int>(level)) + "% conf. interval]");
  }
  say("-------------+" + std::string(64, '-'));
  std::string lastEq = "\x01";
  for (size_t k = 0; k < K; ++k) {
    const size_t colon = names[k].find(':');
    const std::string eq = colon == std::string::npos ? std::string() : names[k].substr(0, colon);
    const std::string nm = colon == std::string::npos ? names[k] : names[k].substr(colon + 1);
    if (!eq.empty() && eq != lastEq) {
      if (lastEq != "\x01") say("-------------+" + std::string(64, '-'));
      say(padRight(eq, 12) + " |");
    }
    lastEq = eq;
    const ojson& r = rows[k];
    std::string line = padLeft(nm, 12) + " |" + padLeft(g9cell(observed[k]), 11);
    if (r["z"].is_number()) {
      line += padLeft(g9cell(se[k]), 11) + padLeft(fixed2(r["z"].get<double>()), 9) +
              padLeft(fixed3(r["p"].get<double>()), 8) + padLeft(g9cell(r["ci_lo"].get<double>()), 13) +
              padLeft(g9cell(r["ci_hi"].get<double>()), 12);
    } else {
      line += padLeft("(omitted)", 13);
    }
    say(line);
  }
  say(rule);
  if (nMis > 0)
    say("Note: One or more parameters could not be estimated in " + std::to_string(nMis) +
        " bootstrap replicate" + (nMis == 1 ? "" : "s") + ";\n      standard-error estimates "
        "include only complete replications.");

  ojson out;
  out["type"] = "resample";
  out["method"] = method;
  out["command"] = innerCmd;
  out["n_obs"] = static_cast<int64_t>(nObs);
  out["n_reps"] = nReps;
  out["n_reps_requested"] = reps;
  out["level"] = level;
  ojson srows = ojson::array();
  for (size_t k = 0; k < K; ++k) {
    ojson r = rows[k];
    r["bias"] = bias[k];
    srows.push_back(r);
  }
  out["rows"] = srows;
  out["est_sample"] = haveEsample;
  if (!seedText.empty()) out["seed"] = seedText;
  if (!clusterVar.empty()) {
    out["cluster"] = clusterVar;
    out["n_clust"] = nClust;
  }
  lines.push_back(out);
  return lines;
}

// ---------------------------------------------------------------------------
// The driver.
//
// `method` is bootstrap | jackknife | permute. `expListRaw` is verbatim (never
// macro-expanded — see the file header). `optsRaw` and `innerCmd` HAVE been
// expanded by the caller, so their locals are already resolved.
ojson runResample(const std::string& method, const std::string& permuteVar,
                  const std::string& expListRaw, const std::string& optsRaw,
                  const std::string& innerCmd, SessionState& session) {
  if (method == "bootstrap") return runBootstrap(expListRaw, optsRaw, innerCmd, session);
  if (!session.currentHbin().has_value()) return eval::noDataLoaded();
  if (innerCmd.empty()) return rsError(method + ": no command found after the colon");

  std::string perr;
  std::vector<ExpItem> expList = parseExpList(expListRaw, &perr);
  if (expList.empty()) return rsError(method + ": " + perr);

  // -- options ---------------------------------------------------------------
  int reps = 50;                 // Stata's own default for bootstrap/permute
  uint64_t seed = 0;
  bool haveSeed = false;
  {
    // The option list is a flat `name(value)` / `name` sequence; parse it here
    // rather than through the tokenizer because this line was deliberately kept
    // out of the normal parse path.
    size_t i = 0;
    while (i < optsRaw.size()) {
      while (i < optsRaw.size() &&
             (std::isspace(static_cast<unsigned char>(optsRaw[i])) || optsRaw[i] == ',')) ++i;
      if (i >= optsRaw.size()) break;
      const size_t ns = i;
      while (i < optsRaw.size() && (std::isalnum(static_cast<unsigned char>(optsRaw[i])) ||
                                    optsRaw[i] == '_')) ++i;
      std::string key = optsRaw.substr(ns, i - ns);
      for (auto& c : key) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
      std::string val;
      if (i < optsRaw.size() && optsRaw[i] == '(') {
        int depth = 1;
        ++i;
        const size_t vs = i;
        while (i < optsRaw.size() && depth > 0) {
          if (optsRaw[i] == '(') ++depth;
          else if (optsRaw[i] == ')') --depth;
          if (depth > 0) ++i;
        }
        val = optsRaw.substr(vs, i - vs);
        if (i < optsRaw.size()) ++i;  // ')'
      }
      if (key.empty()) break;
      if (key == "reps" || key == "rep") {
        try { reps = std::stoi(val); } catch (const std::exception&) {
          return rsError(method + ": reps() needs a positive integer (got `" + val + "`)");
        }
      } else if (key == "seed") {
        try { seed = static_cast<uint64_t>(std::stoll(val)); haveSeed = true; }
        catch (const std::exception&) {
          return rsError(method + ": seed() needs an integer (got `" + val + "`)");
        }
      } else if (key == "nodots" || key == "dots" || key == "noisily" || key == "nowarn" ||
                 key == "notable" || key == "noheader" || key == "nolegend" || key == "verbose") {
        // Presentation-only switches: accepted and inert. Nothing about the
        // numbers depends on them, so silently honouring them is not a
        // narrowing — dots are progress output PIET does not emit at all.
      } else if (key == "level") {
        // handled below via session.level
        try { session.level = std::stoi(val); } catch (const std::exception&) {
          return rsError(method + ": level() needs an integer");
        }
      } else {
        return rsError(method + ": option `" + key +
                       "` is not implemented in this build. Implemented: reps(), seed(), level(), "
                       "and the presentation switches (nodots/notable/...).");
      }
    }
  }
  if (reps <= 0) return rsError(method + ": reps() must be positive");

  // The head may be a DIRTY VIRTUAL resident frame whose path names a file that
  // was never written (`keep if` deliberately leaves it that way — see
  // inrange_filter.cpp). This driver both re-opens the base as a file and
  // returns the session to it at the end, so anchor it on disk first.
  // datastore() is the established materialization point.
  try {
    (void)session.datastore();
  } catch (const std::exception& e) {
    return rsError(method + ": could not materialize the working dataset: " + e.what());
  }
  const std::string startHbin = *session.currentHbin();

  // -- 1. the observed run ---------------------------------------------------
  // Also the run that tells us what the estimation sample IS: an e-class
  // command leaves a LastModel behind, an r-class one does not.
  {
    ojson obs = dispatch(innerCmd, session);
    for (const auto& b : obs) {
      if (b.is_object() && b.value("type", "") == "error") {
        session.load(startHbin);
        return rsError(method + ": the command `" + innerCmd + "` failed, so there is nothing to " +
                       (method == "permute" ? "permute" : "resample") + ": " +
                       b.value("message", std::string("(no message)")));
      }
    }
  }
  RepValues observed;
  for (const auto& e : expList) {
    std::optional<double> v = evalStat(e.expr, session);
    if (!v.has_value()) {
      session.load(startHbin);
      return rsError(method + ": could not evaluate `" + e.expr + "` after running `" + innerCmd +
                     "` — it does not resolve to a number.");
    }
    observed.push_back(*v);
  }
  // jackknife posts its own e() over the observed run's estimation sample
  // (BUG-1456): its mask and e(title) are read here, while that fit is live.
  std::vector<char> jkSample;
  const std::string innerTitle = session.eval.eStrings.get("title", std::string());
  if (method == "jackknife") {
    const SessionState::EsampleReading er = session.esampleReading();
    Datastore* d = session.datastore();
    if (d != nullptr) {
      const int64_t nHead = d->nRows();
      if (er.kind == SessionState::EsampleReading::Kind::kColumn && d->has(kEsampleColumn)) {
        const double* m = d->col(kEsampleColumn).f64();
        jkSample.assign(static_cast<size_t>(nHead), 0);
        for (int64_t i = 0; i < nHead; ++i)
          jkSample[static_cast<size_t>(i)] = m != nullptr && m[i] == static_cast<double>(er.serial);
      } else if (!session.lastModel().has_value()) {
        // An r-class command: the whole data unless it carries an if/in, whose
        // rows this build does not mark (e(sample) then stays unavailable).
        static const std::regex reQual(R"(\s(if|in)\s)");
        std::smatch qm;
        if (!eval::searchUnquoted(" " + innerCmd + " ", reQual, qm))
          jkSample.assign(static_cast<size_t>(nHead), 1);
      }
    }
  }

  // -- 2. the resampling base ------------------------------------------------
  // Stata draws from the inner command's ESTIMATION SAMPLE when it sets one,
  // and from the whole dataset (with a warning) when it does not. Our carriage
  // for "the sample the fit actually ran on" is LastModel (the P5 BUG-163
  // mechanism): its fitDataPath/fitKeepif carry an if/in restriction, and the
  // model's own y/x lists let us drop the rows a missing value excluded.
  std::string basePath = startHbin;
  bool haveEstSample = false;
  std::vector<std::string> missingFilterVars;
  if (session.lastModel().has_value()) {
    const auto& lm = *session.lastModel();
    haveEstSample = true;
    if (!lm.fitDataPath.empty()) basePath = lm.fitDataPath;
    Datastore probe(basePath);
    auto consider = [&](const std::string& v) {
      // Only plain existing columns: a factor-expanded term like `i.region`
      // is not a column and carries no missingness of its own.
      if (!v.empty() && probe.has(v)) missingFilterVars.push_back(v);
    };
    consider(lm.y);
    for (const auto& x : lm.x) consider(x);
    std::string keepif = lm.fitKeepif;
    for (const auto& v : missingFilterVars) {
      const std::string term = "!missing(" + v + ")";
      keepif = keepif.empty() ? term : ("(" + keepif + ") & " + term);
    }
    if (!keepif.empty()) {
      const std::string filtered = nextPath(session.workdir(), method + "_base");
      CobraRunResult cr =
          runCobra({"eval", "--data", basePath, "--keepif", keepif, "--out", filtered});
      if (!cr.ok) {
        session.load(startHbin);
        CobraError ce = errorFromRaw(cr);
        return rsError(cobraErrorStr(ce), ce.code);
      }
      basePath = filtered;
    }
  }

  // Every transform joins on a row index, so materialize it once.
  const std::string basePathIdx = nextPath(session.workdir(), method + "_baseidx");
  {
    CobraRunResult cr = runCobra(
        {"eval", "--data", basePath, "--egen", std::string(kRowIdx) + " = seq()", "--out", basePathIdx});
    if (!cr.ok) {
      session.load(startHbin);
      CobraError ce = errorFromRaw(cr);
      return rsError(cobraErrorStr(ce), ce.code);
    }
  }
  // The loop below writes several scratch files per replicate, which pushes the
  // reclaim budget over and makes these two — the oldest at that moment — the
  // first candidates for eviction, while they are still the thing being drawn
  // from. Pin them for the length of this command (RAII: released on every
  // exit path, including the throw one, since the loop dispatches arbitrary
  // user commands).
  SessionState::ScratchPin pin(session, {basePath, basePathIdx, startHbin});

  int64_t nBase = 0;
  {
    Datastore d(basePathIdx);
    nBase = d.nRows();
  }
  if (nBase <= 1) {
    session.load(startHbin);
    return rsError(method + ": need more than one observation to resample (have " +
                   std::to_string(nBase) + ")");
  }

  // permute's variable must be numeric and present.
  std::vector<double> permOriginal;
  if (method == "permute") {
    if (permuteVar.empty()) {
      session.load(startHbin);
      return rsError("permute: the permutation variable is required (permute VAR name=exp: cmd)");
    }
    Datastore d(basePathIdx);
    if (!d.has(permuteVar)) {
      session.load(startHbin);
      return rsError("variable " + permuteVar + " not found");
    }
    const cecon::Column& c = d.col(permuteVar);
    if (c.dtype == cecon::DType::Factor) {
      session.load(startHbin);
      return rsError("permute: `" + permuteVar +
                     "` is a string/factor variable; this build permutes numeric variables only.");
    }
    const double* p = c.f64();
    permOriginal.assign(p, p + static_cast<size_t>(nBase));
  }

  // jackknife's replicate count is not a user choice — it is n.
  const int64_t nReps = (method == "jackknife") ? nBase : static_cast<int64_t>(reps);

  // -- 3. the replicate loop -------------------------------------------------
  std::vector<RepValues> reps_values;
  reps_values.reserve(static_cast<size_t>(nReps));
  std::string failure;
  for (int64_t b = 0; b < nReps && failure.empty(); ++b) {
    std::string repPath;
    if (method == "jackknife") {
      // Leave observation b+1 out.
      repPath = nextPath(session.workdir(), "jk_rep");
      CobraRunResult cr = runCobra({"eval", "--data", basePathIdx, "--keepif",
                                    std::string(kRowIdx) + "!=" + std::to_string(b + 1), "--out",
                                    repPath});
      if (!cr.ok) { failure = cobraErrorStr(errorFromRaw(cr)); break; }
    } else {  // permute
      std::vector<double> shuffled = permOriginal;
      // Fisher-Yates over the Philox stream.
      for (int64_t j = nBase - 1; j > 0; --j) {
        const uint32_t d = cecon::Philox::draw32(seed, static_cast<uint64_t>(b),
                                                 static_cast<uint64_t>(j));
        const int64_t k = static_cast<int64_t>(d % static_cast<uint32_t>(j + 1));
        std::swap(shuffled[static_cast<size_t>(j)], shuffled[static_cast<size_t>(k)]);
      }
      // P2-PERF ARC-C C2-②: ONE call, not three. The old shape wrote a
      // two-column CSV, converted it to an hbin, dropped the original column
      // and 1:1-joined the new one back on the row index — a whole join
      // apparatus to move n doubles into a column that was already there, in
      // the order it was already in. `cobra setcol` patches exactly one column
      // from a headerless payload and preserves every other byte, which is the
      // operation this always was. It also stops MOVING the column: the join
      // re-appended it at the end of the varlist, where an in-place replace
      // leaves it where the user put it — closer to Stata, not further.
      const std::string csv = session.workdir() + "/pm_val.csv";
      registerScratchMint(csv);  // BUG-389
      if (!writePermuteValuesCsv(csv, shuffled)) {
        failure = "could not write the permuted values";
        break;
      }
      repPath = nextPath(session.workdir(), "pm_rep");
      CobraRunResult c1 = runCobra({"setcol", "--data", basePathIdx, "--var", permuteVar, "--values",
                                    csv, "--dtype", "f64", "--out", repPath});
      if (!c1.ok) { failure = cobraErrorStr(errorFromRaw(c1)); break; }
    }

    session.load(repPath);
    ojson inner = dispatch(innerCmd, session);
    bool innerFailed = false;
    for (const auto& blk : inner) {
      if (blk.is_object() && blk.value("type", "") == "error") {
        innerFailed = true;
        break;
      }
    }
    if (innerFailed) {
      // Stata drops a replicate whose command fails and reports the loss rather
      // than averaging over a hole it never mentions. Same here: a failed
      // replicate contributes nothing and is counted.
      continue;
    }
    RepValues vals;
    bool ok = true;
    for (const auto& e : expList) {
      std::optional<double> v = evalStat(e.expr, session);
      if (!v.has_value()) { ok = false; break; }
      vals.push_back(*v);
    }
    if (ok) reps_values.push_back(std::move(vals));
  }

  // The session's own dataset is never left on a replicate.
  session.load(startHbin);
  if (!failure.empty()) return rsError(method + ": " + failure);

  const int64_t nCompleted = static_cast<int64_t>(reps_values.size());
  if (nCompleted < 2) {
    return rsError(method + ": only " + std::to_string(nCompleted) +
                   " replicate(s) completed successfully — too few to report a standard error.");
  }
  // jackknife keeps its replicates as float (postfile's default storage; the
  // `double` option is not implemented here) and computes e(V) from those
  // (StataBE 19.5, BUG-1456: _se[b1] .15455305643343692, where the double
  // replicates give .15455301963687726).
  if (method == "jackknife")
    for (auto& rv : reps_values)
      for (double& v : rv) v = static_cast<double>(static_cast<float>(v));

  // -- 4. aggregate ----------------------------------------------------------
  const double alpha = 1.0 - static_cast<double>(session.level) / 100.0;
  ojson rows = ojson::array();
  for (size_t k = 0; k < expList.size(); ++k) {
    double sum = 0.0;
    for (const auto& rv : reps_values) sum += rv[k];
    const double mean = sum / static_cast<double>(nCompleted);
    double ss = 0.0;
    for (const auto& rv : reps_values) {
      const double d = rv[k] - mean;
      ss += d * d;
    }

    ojson row;
    row["name"] = expList[k].name;
    row["b"] = observed[k];

    if (method == "permute") {
      // Counting, not a standard error: how often a permuted statistic reaches
      // the observed one. Stata reports both one-sided counts and the two-sided
      // p; SE(p) is the binomial standard error of the Monte-Carlo proportion.
      int64_t cLower = 0, cUpper = 0;
      for (const auto& rv : reps_values) {
        if (rv[k] <= observed[k]) ++cLower;
        if (rv[k] >= observed[k]) ++cUpper;
      }
      const double n = static_cast<double>(nCompleted);
      const double pLower = static_cast<double>(cLower) / n;
      const double pUpper = static_cast<double>(cUpper) / n;
      const double pTwo = std::min(1.0, 2.0 * std::min(pLower, pUpper));
      row["c_lower"] = cLower;
      row["c_upper"] = cUpper;
      row["n"] = nCompleted;
      row["p_lower"] = pLower;
      row["p_upper"] = pUpper;
      row["p_twosided"] = pTwo;
      row["se_lower"] = std::sqrt(pLower * (1.0 - pLower) / n);
      row["se_upper"] = std::sqrt(pUpper * (1.0 - pUpper) / n);
    } else if (method == "jackknife") {
      // Deterministic, and the one place the oracle judges our arithmetic digit
      // for digit. Stata's jackknife variance is the leave-one-out scaling
      // (n-1)/n * sum((theta_i - theta_bar)^2), with t(n-1) inference — fitted
      // against the P6 oracle row (SE .1256869 on `jackknife bx1=_b[x1]:
      // regress y x1 x2`), not assumed from the first plausible formula.
      const double n = static_cast<double>(nCompleted);
      const double se = std::sqrt((n - 1.0) / n * ss);
      const double dof = n - 1.0;
      const double t = se > 0 ? observed[k] / se : std::nan("");
      row["se"] = se;
      row["t"] = t;
      row["p"] = cecon::t_twosided_pvalue(t, dof);
      const double crit = cecon::t_critical(alpha, dof);
      row["ci_lo"] = observed[k] - crit * se;
      row["ci_hi"] = observed[k] + crit * se;
      row["df"] = dof;
    }
    rows.push_back(row);
  }

  // -- 4b. e() ----------------------------------------------------------------
  // Neither prefix leaves the last replicate's fit live (BUG-1456; StataBE 19.5):
  // permute empties e(); jackknife posts its own -- e(b) the observed statistics
  // under the exp_list names, e(V) the jackknife covariance (n-1)/n * sum of the
  // replicates' cross-deviations, e(b_jk) the mean of the pseudovalues
  // n*b - (n-1)*mean(replicates) (2.0658634 against b 2.0621382), e(sample) the
  // observed run's sample, and the scalars and macros `ereturn list` shows there.
  clearEResults(session);
  if (method == "jackknife") {
    const size_t K = expList.size();
    const double n = static_cast<double>(nCompleted);
    std::vector<std::string> names;
    std::vector<double> means(K, 0.0);
    for (size_t k = 0; k < K; ++k) {
      names.push_back(expList[k].name);
      for (const auto& rv : reps_values) means[k] += rv[k];
      means[k] /= n;
    }
    std::vector<std::vector<double>> V(K, std::vector<double>(K, 0.0));
    for (size_t a = 0; a < K; ++a)
      for (size_t c = 0; c < K; ++c) {
        double s = 0.0;
        for (const auto& rv : reps_values) s += (rv[a] - means[a]) * (rv[c] - means[c]);
        V[a][c] = (n - 1.0) / n * s;
      }
    ojson block = ojson::object();
    block["type"] = "estimation";
    block["estimator"] = "jackknife";
    block["n_obs"] = nBase;
    block["coeftable"] = rows;
    LastModel lm;
    lm.cobraCmd = "jackknife";
    lm.stataCmd = "jackknife";
    lm.vcov = "jackknife";
    lm.eVce = "jackknife";
    lm.result = block;
    if (!jkSample.empty() && !e3::installEsampleMask(session, lm, jkSample))
      return rsError("jackknife: the results were computed, but e(sample) could not be installed");
    session.setLastModel(lm);
    session.lastEstBlock = block;
    e3::postBV(session, names, observed, V);
    {
      eval::EvalMatrix m;
      std::vector<double> bjk(K);
      for (size_t k = 0; k < K; ++k) bjk[k] = n * observed[k] - (n - 1.0) * means[k];
      m.data.push_back(bjk);
      m.rownames = {"y1"};
      m.colnames = names;
      setEMatrix(session, "b_jk", std::move(m));
    }
    const std::vector<std::vector<double>> Vinv = e3::invsym(V);
    int rank = 0;
    for (size_t k = 0; k < K; ++k) rank += Vinv[k][k] != 0.0 ? 1 : 0;
    e3::setEScalarOnFit(session, "df_r", n - 1.0);
    e3::setEScalarOnFit(session, "N_reps", static_cast<double>(nReps));
    e3::setEScalarOnFit(session, "N_misreps", static_cast<double>(nReps - nCompleted));
    e3::setEScalarOnFit(session, "rank", rank);
    e3::setEScalarOnFit(session, "k_eq", 1);
    e3::setEScalarOnFit(session, "k_exp", static_cast<double>(K));
    e3::setEScalarOnFit(session, "k_eexp", 0);
    e3::setEScalarOnFit(session, "k_extra", 0);
    e3::setEScalarOnFit(session, "singleton", std::nan(""));
    e3::setEScalarOnFit(session, "N", static_cast<double>(nBase));
    std::string cmdname;
    {
      std::istringstream words(innerCmd);
      words >> cmdname;
      const size_t cut = cmdname.find(',');
      if (cut != std::string::npos) cmdname = cmdname.substr(0, cut);
    }
    std::string title = bootstrapTitle(innerTitle, cmdname);
    if (title == "Bootstrap results") title = "Jackknife results";
    e3::setEStringOnFit(session, "cmdline", "jackknife " + strutil::trim(expListRaw) +
                                                (optsRaw.empty() ? "" : ", " + optsRaw) + ": " +
                                                innerCmd);
    e3::setEStringOnFit(session, "cmd", "jackknife");
    e3::setEStringOnFit(session, "prefix", "jackknife");
    e3::setEStringOnFit(session, "cmdname", cmdname);
    if (haveEstSample) e3::setEStringOnFit(session, "nfunction", "e(N)");
    e3::setEStringOnFit(session, "command", innerCmd);
    for (size_t k = K; k-- > 0;)
      e3::setEStringOnFit(session, "exp" + std::to_string(k + 1), expList[k].expr);
    e3::setEStringOnFit(session, "title", title);
    e3::setEStringOnFit(session, "vcetype", "Jackknife");
    e3::setEStringOnFit(session, "vce", "jackknife");
    e3::setEStringOnFit(session, "properties", "b V");
  }

  // -- 5. render, in Stata's own layout -------------------------------------
  // Emitted as `display` text blocks: the numbers reach the console, the log
  // tee and the webview through a path that already exists, so no ResultBlock
  // variant is added and contract B does not move. The layout is copied from
  // the oracle log, which is the spec for it.
  ojson lines = ojson::array();
  auto say = [&lines](const std::string& t) {
    lines.push_back(ojson{{"type", "display"}, {"text", t}});
  };
  {
    const std::string obsStr = commaGroup(nBase);
    if (method == "permute") {
      say(padRight("Monte Carlo permutation results", 51) +
          "Number of observations = " + padLeft(obsStr, 3));
      say(padRight("Permutation variable: " + permuteVar, 51) +
          "Number of permutations = " + padLeft(std::to_string(nCompleted), 3));
    } else {
      const std::string title = "Linear regression";  // jackknife
      say(padRight(title, 59) + "Number of obs = " + padLeft(obsStr, 3));
      say(padRight("", 59) + "Replications  = " + padLeft(std::to_string(nCompleted), 3));
    }
    say("");
    say("      Command: " + innerCmd);
    for (const auto& e : expList) {
      say(padLeft(e.name, 13) + ": " + e.expr);
    }
    say("");
  }
  if (method == "permute") {
    const std::string rule(79, '-');
    say(rule);
    say("             |                                               Monte Carlo error");
    say("             |                                              -------------------");
    say("           T |    T(obs)       Test       c       n      p  SE(p)   [95% CI(p)]");
    say("-------------+" + std::string(65, '-'));
    for (const auto& row : rows) {
      const int64_t cl = row["c_lower"].get<int64_t>();
      const int64_t cu = row["c_upper"].get<int64_t>();
      const int64_t n = row["n"].get<int64_t>();
      const double pl = row["p_lower"].get<double>();
      const double pu = row["p_upper"].get<double>();
      const double p2 = row["p_twosided"].get<double>();
      const auto ciL = clopperPearson(cl, n, alpha);
      const auto ciU = clopperPearson(cu, n, alpha);
      say(padLeft(row["name"].get<std::string>(), 12) + " |" +
          padLeft(g9cell(row["b"].get<double>()), 10) + padLeft("lower", 11) +
          padLeft(std::to_string(cl), 8) + padLeft(std::to_string(n), 8) +
          padLeft(prob4(pl), 7) + padLeft(prob4(row["se_lower"].get<double>()), 7) +
          padLeft(prob4(ciL.first), 7) + padLeft(prob4(ciL.second), 7));
      say(std::string(12, ' ') + " |" + std::string(10, ' ') + padLeft("upper", 11) +
          padLeft(std::to_string(cu), 8) + padLeft(std::to_string(n), 8) +
          padLeft(prob4(pu), 7) + padLeft(prob4(row["se_upper"].get<double>()), 7) +
          padLeft(prob4(ciU.first), 7) + padLeft(prob4(ciU.second), 7));
      say(std::string(12, ' ') + " |" + std::string(10, ' ') + padLeft("two-sided", 15) +
          std::string(12, ' ') + padLeft(prob4(p2), 7) + padLeft(prob4(0.0), 7) +
          padLeft(".", 7) + padLeft(".", 7));
    }
    say(rule);
    say("Notes: For lower one-sided test, c = #{T <= T(obs)} and p = p_lower = c/n.");
    say("       For upper one-sided test, c = #{T >= T(obs)} and p = p_upper = c/n.");
    say("       For two-sided test, p = 2*min(p_lower, p_upper); SE and CI approximate.");
  } else {
    const std::string rule(78, '-');
    const std::string stat = "t";  // jackknife (bootstrap renders in runBootstrap)
    say(rule);
    say("             |              Jackknife");
    say("             | Coefficient  std. err.      t    P>|t|     [95% conf. interval]");
    say("-------------+" + std::string(64, '-'));
    for (const auto& row : rows) {
      const double statv = row[stat].get<double>();
      say(padLeft(row["name"].get<std::string>(), 12) + " |" +
          padLeft(g9cell(row["b"].get<double>()), 11) + padLeft(g9cell(row["se"].get<double>()), 11) +
          padLeft(fixed2(statv), 9) + padLeft(fixed3(row["p"].get<double>()), 8) +
          padLeft(g9cell(row["ci_lo"].get<double>()), 13) + padLeft(g9cell(row["ci_hi"].get<double>()), 12));
    }
    say(rule);
  }

  ojson out;
  out["type"] = "resample";
  out["method"] = method;
  out["command"] = innerCmd;
  out["n_obs"] = nBase;
  out["n_reps"] = nCompleted;
  out["n_reps_requested"] = nReps;
  out["level"] = session.level;
  out["rows"] = rows;
  out["est_sample"] = haveEstSample;
  if (haveSeed) out["seed"] = static_cast<long long>(seed);
  if (method == "permute") out["permute_var"] = permuteVar;
  // Stata warns when the inner command sets no estimation sample, because the
  // resampling then cannot exclude the rows the command itself would have
  // dropped. Carry the same warning rather than resampling silently.
  if (!haveEstSample) {
    out["warning"] = std::string(innerCmd.substr(0, innerCmd.find(' '))) +
                     " does not set e(sample), so no observations will be excluded from the "
                     "resampling because of missing values or other reasons.";
  }
  // The structured block rides LAST so a consumer that wants the numbers has
  // them, while the human-facing text is what the console and log show. Its
  // type is new but it is never the sole carrier of a result, and the
  // ResultBlock union the frontend switches on is untouched.
  lines.push_back(out);
  return lines;
}


// ---------------------------------------------------------------------------
// rolling  — `rolling explist, window(N) saving("FILE", replace): command`
//
// Same skeleton as the resampling prefixes: the only difference is that the
// per-"replicate" transform is a WINDOW SLICE of the time variable rather than
// a resample, so the inner command stays completely general.
//
// Requires a prior `tsset`/`xtset` (Stata's own precondition). Windows run over
// the DISTINCT observed time values, so a gap in the series shortens the span
// in calendar terms exactly as it does in Stata.
ojson runRolling(const std::string& expListRaw, const std::string& optsRaw,
                 const std::string& innerCmd, SessionState& session) {
  if (!session.currentHbin().has_value()) return eval::noDataLoaded();
  if (innerCmd.empty()) return rsError("rolling: no command found after the colon");
  if (!session.timevar.has_value() || session.timevar->empty()) {
    return rsError("rolling: the data must be tsset (or xtset) first — rolling needs to know which "
                   "variable is time.");
  }
  const std::string tvar = *session.timevar;

  std::string perr;
  std::vector<ExpItem> expList = parseExpList(expListRaw, &perr);
  if (expList.empty()) return rsError("rolling: " + perr);

  int window = 0;
  std::string savingPath;
  bool savingReplace = false;
  {
    size_t i = 0;
    while (i < optsRaw.size()) {
      while (i < optsRaw.size() &&
             (std::isspace(static_cast<unsigned char>(optsRaw[i])) || optsRaw[i] == ',')) ++i;
      if (i >= optsRaw.size()) break;
      const size_t ns = i;
      while (i < optsRaw.size() && (std::isalnum(static_cast<unsigned char>(optsRaw[i])) ||
                                    optsRaw[i] == '_')) ++i;
      std::string key = optsRaw.substr(ns, i - ns);
      for (auto& c : key) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
      std::string val;
      if (i < optsRaw.size() && optsRaw[i] == '(') {
        int depth = 1; ++i;
        const size_t vs = i;
        while (i < optsRaw.size() && depth > 0) {
          if (optsRaw[i] == '(') ++depth;
          else if (optsRaw[i] == ')') --depth;
          if (depth > 0) ++i;
        }
        val = optsRaw.substr(vs, i - vs);
        if (i < optsRaw.size()) ++i;
      }
      if (key.empty()) break;
      if (key == "window") {
        try { window = std::stoi(val); } catch (const std::exception&) {
          return rsError("rolling: window() needs a positive integer");
        }
      } else if (key == "saving") {
        // `saving("FILE", replace)`
        std::string v = val;
        size_t comma = v.find(',');
        if (comma != std::string::npos) {
          std::string tail = v.substr(comma + 1);
          for (auto& c : tail) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
          savingReplace = tail.find("replace") != std::string::npos;
          v = v.substr(0, comma);
        }
        while (!v.empty() && std::isspace(static_cast<unsigned char>(v.back()))) v.pop_back();
        while (!v.empty() && std::isspace(static_cast<unsigned char>(v.front()))) v.erase(0, 1);
        if (v.size() >= 2 && ((v.front() == '"' && v.back() == '"') ||
                              (v.front() == '\'' && v.back() == '\''))) {
          v = v.substr(1, v.size() - 2);
        }
        savingPath = v;
      } else if (key == "nodots" || key == "dots" || key == "noisily") {
        // presentation-only
      } else {
        return rsError("rolling: option `" + key +
                       "` is not implemented in this build. Implemented: window(), saving().");
      }
    }
  }
  if (window <= 0) return rsError("rolling: window(#) is required and must be positive");
  if (savingPath.empty()) {
    // Without saving() Stata REPLACES the dataset in memory with the results.
    // That branch is not implemented here, and saying so is better than
    // quietly computing results the user never sees.
    return rsError("rolling: this build requires saving(\"FILE\") — the in-memory-replacement form "
                   "is not implemented.");
  }

  try {
    (void)session.datastore();
  } catch (const std::exception& e) {
    return rsError(std::string("rolling: could not materialize the working dataset: ") + e.what());
  }
  const std::string startHbin = *session.currentHbin();
  SessionState::ScratchPin pin(session, {startHbin});

  // Distinct, sorted time values.
  std::vector<double> tvals;
  {
    Datastore d(startHbin);
    if (!d.has(tvar)) return rsError("variable " + tvar + " not found");
    const cecon::Column& c = d.col(tvar);
    if (c.dtype == cecon::DType::Factor) {
      return rsError("rolling: the time variable `" + tvar + "` must be numeric");
    }
    const double* p = c.f64();
    tvals.assign(p, p + static_cast<size_t>(d.nRows()));
    std::sort(tvals.begin(), tvals.end());
    tvals.erase(std::unique(tvals.begin(), tvals.end()), tvals.end());
  }
  if (static_cast<int>(tvals.size()) < window) {
    return rsError("rolling: the series has only " + std::to_string(tvals.size()) +
                   " periods, fewer than window(" + std::to_string(window) + ")");
  }

  std::vector<double> startCol, endCol;
  std::vector<std::vector<double>> statCols(expList.size());
  std::string failure;
  for (size_t i = 0; i + static_cast<size_t>(window) <= tvals.size() && failure.empty(); ++i) {
    const double t0 = tvals[i];
    const double t1 = tvals[i + static_cast<size_t>(window) - 1];
    const std::string slice = nextPath(session.workdir(), "roll_win");
    std::string cond = tvar + ">=" + fmtStataDefaultG(t0) + " & " + tvar +
                       "<=" + fmtStataDefaultG(t1);
    CobraRunResult cr =
        runCobra({"eval", "--data", startHbin, "--keepif", cond, "--out", slice});
    if (!cr.ok) { failure = cobraErrorStr(errorFromRaw(cr)); break; }

    session.load(slice);
    ojson inner = dispatch(innerCmd, session);
    bool innerFailed = false;
    for (const auto& blk : inner) {
      if (blk.is_object() && blk.value("type", "") == "error") { innerFailed = true; break; }
    }
    std::vector<double> vals(expList.size(), std::nan(""));
    if (!innerFailed) {
      for (size_t k = 0; k < expList.size(); ++k) {
        std::optional<double> v = evalStat(expList[k].expr, session);
        if (v.has_value()) vals[k] = *v;
      }
    }
    startCol.push_back(t0);
    endCol.push_back(t1);
    for (size_t k = 0; k < expList.size(); ++k) statCols[k].push_back(vals[k]);
  }
  session.load(startHbin);
  if (!failure.empty()) return rsError("rolling: " + failure);
  if (startCol.empty()) return rsError("rolling: no windows were produced");

  // Results -> CSV -> hbin -> .dta, reproducing the shape Stata's own saved file
  // has (decoded from the oracle artifact, not re-derived from prose): start and
  // end as int %8.0g, each statistic as float %9.0g labelled with its own
  // expression, and the dataset labelled "rolling: <command word>".
  const std::string csv = session.workdir() + "/rolling_out.csv";
  registerScratchMint(csv);  // BUG-389
  {
    std::ofstream f(cecon::to_native_path(csv));
    if (!f) return rsError("rolling: could not write the results file");
    f << "start,end";
    for (const auto& e : expList) f << "," << e.name;
    f << "\n";
    f.precision(17);
    for (size_t r = 0; r < startCol.size(); ++r) {
      f << startCol[r] << "," << endCol[r];
      for (size_t k = 0; k < expList.size(); ++k) {
        f << ",";
        // Stata stores rolling results as `float` (its default type), so the
        // saved values ARE float-rounded. We declare the same storage type in
        // the sidecar, so round to it here too — otherwise the writer sees a
        // value that a float cannot hold losslessly, promotes the column to
        // double to avoid changing it, and our file would differ from Stata's
        // in the eighth digit while claiming the same declared type.
        if (!std::isnan(statCols[k][r])) {
          f << static_cast<double>(static_cast<float>(statCols[k][r]));
        }
      }
      f << "\n";
    }
  }
  const std::string outHbin = nextPath(session.workdir(), "rolling_res");
  {
    CobraRunResult cr = runCobra({"convert", "--data", csv, "--out", outHbin, "--delim", ","});
    if (!cr.ok) return rsError("rolling: " + cobraErrorStr(errorFromRaw(cr)));
  }
  const std::string metaPath = session.workdir() + "/rolling_meta.json";
  registerScratchMint(metaPath);  // BUG-389
  {
    std::string cmdWord = innerCmd.substr(0, innerCmd.find(' '));
    ojson meta;
    meta["data_label"] = "rolling: " + cmdWord;
    ojson vars = ojson::array();
    auto var = [&](const std::string& n, const std::string& t, const std::string& fmt,
                   const std::string& lbl) {
      vars.push_back(ojson{{"name", n}, {"stata_type", t}, {"format", fmt}, {"label", lbl}});
    };
    var("start", "int", "%8.0g", "");
    var("end", "int", "%8.0g", "");
    for (const auto& e : expList) var(e.name, "float", "%9.0g", e.expr);
    meta["variables"] = vars;
    std::ofstream mf(cecon::to_native_path(metaPath));
    if (!mf) return rsError("rolling: could not write the result metadata");
    mf << meta.dump(1);
  }
  if (!savingReplace && cecon::native_exists(savingPath)) {
    return rsError("file " + savingPath + " already exists");
  }
  {
    CobraRunResult cr = runCobra({"export-dta", "--data", outHbin, "--out", savingPath,
                                  "--in-meta", metaPath});
    if (!cr.ok) return rsError("rolling: " + cobraErrorStr(errorFromRaw(cr)));
  }

  ojson lines = ojson::array();
  lines.push_back(ojson{{"type", "display"},
                        {"text", "(running " + innerCmd.substr(0, innerCmd.find(' ')) +
                                     " on estimation sample)"}});
  lines.push_back(ojson{{"type", "display"},
                        {"text", "file " + savingPath + " saved"}});
  return lines;
}

// `statsby _b _se, clear: command` — the UNGROUPED form (no by()): the command runs
// once on the data in memory and the dataset is replaced by one observation holding
// the collected statistics (w2d_uncovered.log:3422 xtreg_statsby_fe_028
// `|0|1|4|1.981762409210205|.3451376259326935|`).
//  - Names follow the prefix name expansion (statsby calls it with
//    stub(_stat_)): id = <stat><name>, the name prefixed with `_` unless it already
//    starts with one, so x1 -> _b_x1 and _cons -> _b_cons (never _b__cons). An id that
//    is not a valid name (a factor-level column such as 1b.g) falls back to
//    _stat_<i>, and a repeated id gets the index appended, as there.
//  - Storage: statsby posts with `double` only when asked, so every
//    statistic is a float by default — the oracle values are float(_b[x1]) and
//    float(_se[x1]) of the unprefixed fit (log:3460 1.981762433275536 /
//    .345137615018114).
// by(), saving(), total, subsets and arbitrary expressions are not implemented and
// are refused by name before the command runs; a multi-equation e(b) likewise.
ojson runStatsby(const std::string& expListRaw, const std::string& optsRaw,
                 const std::string& innerCmd, SessionState& session) {
  if (!session.hasData()) return ojson(eval::noDataLoaded());
  auto splitWs = [](const std::string& s) {
    std::vector<std::string> out;
    std::istringstream in(s);
    for (std::string w; in >> w;) out.push_back(w);
    return out;
  };
  if (splitWs(innerCmd).empty()) return rsError("statsby: no command found after the colon");
  bool clear = false, asDouble = false;
  for (const auto& word : splitWs(optsRaw)) {
    std::string w = word;
    for (auto& c : w) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (w == "clear") clear = true;
    else if (w == "double") asDouble = true;
    else if (w == "nodots" || w == "noisily" || w == "nolegend" || w == "verbose") continue;
    else
      return rsError("statsby: option `" + word +
                         "` is a valid Stata option that this version of PIET does not implement "
                         "(only the ungrouped `statsby _b _se, clear [double]: command` form is)",
                     "unsupported_option");
  }
  std::vector<std::string> statistics;
  for (const auto& word : splitWs(expListRaw)) {
    if (word != "_b" && word != "_se")
      return rsError("statsby: `" + word +
                         "` — this version of PIET collects the _b and _se vectors only",
                     "unsupported_option");
    if (std::find(statistics.begin(), statistics.end(), word) == statistics.end())
      statistics.push_back(word);
  }
  if (statistics.empty()) statistics.push_back("_b");  // statsby's default exp_list
  // statsby.ado replaces the data in memory, which Stata refuses without `clear`
  // when the data have changed since they were last saved (r(4)).
  if (!clear)
    return ojson(eval::errorBlock("engine_error", "no; dataset in memory has changed since last saved",
                                  std::nullopt, std::nullopt, std::nullopt, "r(4)"));

  const ojson result = dispatch(innerCmd, session);
  for (const auto& block : result)
    if (block.is_object() && block.value("type", std::string()) == "error") return block;
  const auto b = session.eval.resolveEMatrix("b");
  if (!b || b->data.size() != 1 || b->data[0].size() != b->colnames.size() ||
      b->colnames.empty())
    return ojson(eval::errorBlock("engine_error", "statsby: the command did not post e(b)",
                                  std::nullopt, std::nullopt, std::nullopt, "r(301)"));
  for (const auto& cn : b->colnames)
    if (cn.find(':') != std::string::npos)
      return rsError("statsby: a multi-equation e(b) is not implemented in this version of PIET",
                     "unsupported_option");
  std::optional<eval::EvalMatrix> v;
  if (std::find(statistics.begin(), statistics.end(), "_se") != statistics.end()) {
    v = session.eval.resolveEMatrix("V");
    if (!v || v->data.size() != b->colnames.size())
      return ojson(eval::errorBlock("engine_error", "statsby: the command did not post e(V)",
                                    std::nullopt, std::nullopt, std::nullopt, "r(301)"));
  }
  auto validName = [](const std::string& s) { return cecon::ident::isValidName(s); };
  std::vector<std::string> names, legend;
  std::vector<double> values;
  size_t idx = 0;
  for (const auto& statistic : statistics) {
    for (size_t j = 0; j < b->colnames.size(); ++j) {
      ++idx;
      const std::string& na = b->colnames[j];
      std::string id = statistic + (na.rfind('_', 0) == 0 ? na : "_" + na);
      if (std::find(names.begin(), names.end(), id) != names.end()) id += std::to_string(idx);
      if (!validName(id)) id = "_stat_" + std::to_string(idx);
      double value = b->data[0][j];
      if (statistic == "_se") {
        const double vjj = j < v->data[j].size() ? v->data[j][j] : std::nan("");
        value = (std::isfinite(vjj) && vjj >= 0) ? std::sqrt(vjj) : std::nan("");
      }
      names.push_back(id);
      legend.push_back(statistic + "[" + na + "]");
      values.push_back(asDouble || !std::isfinite(value)
                           ? value
                           : static_cast<double>(static_cast<float>(value)));
    }
  }

  const std::string out = nextPath(session.workdir(), "statsby");
  const std::string csv = out + ".csv";
  registerScratchMint(csv);  // BUG-389
  {
    std::ofstream f(cecon::to_native_path(csv));
    if (!f) return rsError("statsby: could not write the results file");
    f.precision(17);
    for (size_t j = 0; j < names.size(); ++j) f << (j ? "," : "") << names[j];
    f << '\n';
    for (size_t j = 0; j < values.size(); ++j) {
      if (j) f << ',';
      if (std::isfinite(values[j])) f << values[j];
    }
    f << '\n';
    if (!f) return rsError("statsby: could not write the results file");
  }
  const CobraRunResult converted = runCobra({"convert", "--data", csv, "--out", out, "--delim", ","});
  if (!converted.ok) return rsError("statsby: " + cobraErrorStr(errorFromRaw(converted)));
  session.clearMetadata();
  session.load(out);
  for (const auto& name : names) session.varMeta[name].stataType = asDouble ? "double" : "float";

  ojson lines = ojson::array();
  lines.push_back(ojson{{"type", "display"}, {"text", "      Command: " + innerCmd}});
  for (size_t j = 0; j < names.size(); ++j) {
    std::string pad = names[j].size() < 13 ? std::string(13 - names[j].size(), ' ') : std::string();
    lines.push_back(ojson{{"type", "display"}, {"text", pad + names[j] + ": " + legend[j]}});
  }
  return lines;
}

}  // namespace handlers
}  // namespace piet
