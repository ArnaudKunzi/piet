// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// AER tier-1 (2026-10-28), lane MATRIX: the matrix-store utilities -- matsort matselrc.
// Answers: tests/eval/golden/aer_w2/w2ax_t1_community.log (arms matsort_* and matselrc_*,
// StataBE 19.5) plus the lane's own StataBE probe mp1 (recorded in
// selftest_w2_t1_matrix.cpp's comments where a check rests on it).
//
// BOTH VERBS ARE WIRING OVER THE USER MATRIX STORE. They read and write
// `session.eval.matrices` (EvalMatrix{data,rownames,colnames}; a stripe name is kept
// as typed, `eq:name`, and the `: roweq`/`: rownames` readers split it at the first
// colon -- eval/macros.cpp). Nothing here evaluates a matrix expression: the selection
// is index arithmetic on the stored rows, the sort is a row permutation. A sort column
// or a subscript that is not a plain number goes through evalScalarExpr, the reader
// `display` uses.
//
//   matsort   implements matsort's row sort per its help file: `matsort M [col] [dir]
//             [rankonly]`, a straight selection sort that, for each position i, takes
//             the FIRST row j >= i holding the strict minimum (up) / maximum (down) of
//             the sort column and SWAPS it into place. The swap is what makes the order
//             of tied rows what it is (w2ax_t1_community.log matsort_ties: values
//             w 2, x 1, y 2, z 1 sorted up give `x z y w`, not the stable `x z w y`).
//             The scan starts from fixed sentinels (2147483620 up, -2147483647 down),
//             so a position with no candidate -- the missing values in an ascending
//             sort, a value beyond the sentinel -- is Stata's r(503) (mp1 miss_up,
//             huge), with the matrix left as it was. The column is an expression (`1+1`, a
//             scalar) whose matrix subscript truncates; a stored-results source (`e(b)`,
//             `r(table)`) sorts under rankonly. Row names move with the rows,
//             equation names stay in position (mp1 eq). r(rank) and r(order) are
//             posted, and -- as in Stata -- they are built under the user names
//             `rank`/`order`, which the post then moves away (mp1 rankgone).
//   matselrc  implements matselrc's row/column selection per its help file: columns
//             first, then rows; each list is a positive-integer numlist bounded by the
//             matrix (r(125)/r(126) as numlist reports them), or, when it is not a
//             numlist at all, a list of row/column names (`eq:name`, `eq:` for the
//             whole equation, a bare name only where there is no equation; the first
//             match). `names` makes the ROW list a name list and -- as the package
//             itself does -- leaves the COLUMN list read as subscript expressions, which
//             round (mp1 cnames_n, cnum_n; `c(2.7) names` is column 3). Stripe names travel with the selection; the destination may be
//             the source.
//
// The defect classes of handlers_aer_util.cpp's header apply verbatim: every return is
// a block object with a "type" key or an empty array (SHAPE); every rc other than
// 198/199 travels in the `reason` channel as "r(NNN)" (RC); both command tokens register
// their vocabularies at namespace scope (VOCABULARY); matsort replaces r() itself
// (R-CLASS) -- it is an rclass program, so a failed matsort leaves r() empty.
#include <cctype>
#include <cmath>
#include <cstddef>
#include <limits>
#include <optional>
#include <regex>
#include <string>
#include <utility>
#include <vector>

#include "piet/display_hardening.h"  // evalScalarExpr: the expression a column or subscript is
#include "piet/eval/errors.h"
#include "piet/eval/session.h"  // EvalMatrix, replaceRClass, declareRPosted
#include "piet/eval/tokenizer.h"
#include "piet/handlers.h"
#include "piet/session_state.h"

// ---------------------------------------------------------------------------
// OPTION VOCABULARIES -- namespace scope, before any dispatch (defect class 3).
namespace {

// matsort reads positional words only (`args matname sortcol dir rankonly`); it has no
// options, so any option is refused (see runMatsort).
const piet::eval::OptVocab& kVocabMatsort =
    piet::eval::registerOptVocab({"matsort"}, {{}, {}});

// matselrc's `syntax` line: Row(str) Col(str) Names. The capitalised stems make `r()`,
// `c()` and `n` the minimum spellings, which the 2-character prefix floor cannot reach,
// so they are declared aliases (errors.h OptVocab::aliases).
const piet::eval::OptVocab& kVocabMatselrc = piet::eval::registerOptVocab(
    {"matselrc"}, {{"row", "col", "names"}, {}, {{"r", "row"}, {"c", "col"}, {"n", "names"}}});

}  // namespace

namespace piet::handlers {

using ojson = nlohmann::ordered_json;

namespace {

// An error whose Stata rc is known: "r(NNN)" in `reason` is the exact-code channel
// eval::coarseRc honours; 198 also carries the syntax_error code.
ojson matError(const std::string& message, int rc) {
  return ojson(eval::errorBlock(rc == 198 ? "syntax_error" : "engine_error", message, std::nullopt,
                                std::nullopt, std::nullopt, "r(" + std::to_string(rc) + ")"));
}

std::string trimCopy(const std::string& s) {
  const size_t b = s.find_first_not_of(" \t\r\n");
  if (b == std::string::npos) return std::string();
  const size_t e = s.find_last_not_of(" \t\r\n");
  return s.substr(b, e - b + 1);
}

// Blank-separated words; a double-quoted run is one word with its quotes removed
// (`args`/`tokenize` semantics).
std::vector<std::string> words(const std::string& text) {
  std::vector<std::string> out;
  size_t i = 0;
  while (i < text.size()) {
    if (text[i] == ' ' || text[i] == '\t') {
      ++i;
      continue;
    }
    if (text[i] == '"') {
      const size_t j = text.find('"', i + 1);
      if (j == std::string::npos) {
        out.push_back(text.substr(i + 1));
        break;
      }
      out.push_back(text.substr(i + 1, j - i - 1));
      i = j + 1;
      continue;
    }
    size_t j = i;
    while (j < text.size() && text[j] != ' ' && text[j] != '\t') ++j;
    out.push_back(text.substr(i, j - i));
    i = j;
  }
  return out;
}

bool isNameWord(const std::string& s) {
  if (s.empty() || s.size() > 32) return false;
  const unsigned char c0 = static_cast<unsigned char>(s[0]);
  if (!(std::isalpha(c0) || c0 == '_')) return false;
  for (char ch : s) {
    const unsigned char c = static_cast<unsigned char>(ch);
    if (!(std::isalnum(c) || c == '_')) return false;
  }
  return true;
}

// A whole-string number as Stata's numlist reads one: optional sign, digits with an
// optional point, optional exponent; `.` is the missing value. Nothing else -- std::stod
// alone would also take `0x2`, `inf` and `nan` (review st_mat hex: `r(0x2)` is a NAME,
// "0x2 not found", r(111)).
bool parseNumber(const std::string& s, double* out) {
  if (s == ".") {
    *out = std::numeric_limits<double>::quiet_NaN();
    return true;
  }
  static const std::regex re(R"(^[+-]?(\d+\.?\d*|\.\d+)([eE][+-]?\d+)?$)");
  if (!std::regex_match(s, re)) return false;
  try {
    *out = std::stod(s);
    return true;
  } catch (...) {
    return false;
  }
}

// Stata's ordering of a matrix cell: a missing value sorts above every number.
double sortKey(double v) { return std::isnan(v) ? std::numeric_limits<double>::infinity() : v; }

eval::EvalMatrix* userMatrix(SessionState& session, const std::string& name) {
  for (auto& p : session.eval.matrices)
    if (p.first == name) return &p.second;
  return nullptr;
}

void putUserMatrix(SessionState& session, const std::string& name, eval::EvalMatrix mat) {
  if (eval::EvalMatrix* m = userMatrix(session, name)) {
    *m = std::move(mat);
    return;
  }
  session.eval.matrices.emplace_back(name, std::move(mat));
}

// Removes a user matrix and hands it back (Stata's `return matrix x = x` moves it).
std::optional<eval::EvalMatrix> takeUserMatrix(SessionState& session, const std::string& name) {
  auto& store = session.eval.matrices;
  for (auto it = store.begin(); it != store.end(); ++it) {
    if (it->first == name) {
      eval::EvalMatrix m = std::move(it->second);
      store.erase(it);
      return m;
    }
  }
  return std::nullopt;
}

// Stripe names that are never empty: a matrix stored without them reads as Stata's
// default r1 r2 ... / c1 c2 ...
std::vector<std::string> stripeOrDefault(const std::vector<std::string>& names, size_t n,
                                         char prefix) {
  if (names.size() == n) return names;
  std::vector<std::string> out;
  out.reserve(n);
  for (size_t i = 0; i < n; ++i) out.push_back(std::string(1, prefix) + std::to_string(i + 1));
  return out;
}

// `eq:name` -> {eq, name}; no colon -> {"", name}.
std::pair<std::string, std::string> splitStripe(const std::string& s) {
  const size_t colon = s.find(':');
  if (colon == std::string::npos) return {std::string(), s};
  return {s.substr(0, colon), s.substr(colon + 1)};
}

// An n x 1 column vector with the default stripe (`matrix x = J(n,1,0)`).
eval::EvalMatrix columnVector(const std::vector<double>& v) {
  eval::EvalMatrix m;
  for (double x : v) m.data.push_back({x});
  m.rownames = stripeOrDefault({}, v.size(), 'r');
  m.colnames = {"c1"};
  return m;
}

// ---------------------------------------------------------------------------
// matselrc's numlist: `numlist "...", int range(>0 <=max)`. Elements are separated by
// blanks or commas; each is #, #/#, #(#)# or #[#]#, and `# # to #` / `# # : #` take
// their step from the first two. 121 = not a numlist (matselrc then reads the list as
// names), 127 = a missing element, 126 = a non-integer element, 125 = an element outside
// 1..max -- numlist's own codes and messages (w2ax_t1_community.log matselrc_toobig, mp1
// nonint/czero, review st_mat dot/st_mat2 nl_dot).
struct NumlistOutcome {
  int rc = 0;
  std::vector<long long> values;
};

bool numlistRun(double a, double step, double b, std::vector<double>* out) {
  if (step == 0.0 || std::isnan(a) || std::isnan(b) || std::isnan(step)) return false;
  if ((step > 0 && a > b) || (step < 0 && a < b)) return true;  // an empty run
  const double span = (b - a) / step;
  if (span > 1e6) return false;
  const long long n = static_cast<long long>(std::floor(span + 1e-9));
  for (long long k = 0; k <= n; ++k) out->push_back(a + static_cast<double>(k) * step);
  return true;
}

NumlistOutcome positiveIntNumlist(const std::string& text, long long maxValue) {
  NumlistOutcome res;
  std::string flat = text;
  for (char& ch : flat)
    if (ch == ',') ch = ' ';
  std::vector<std::string> toks = words(flat);
  std::vector<double> vals;
  for (size_t i = 0; i < toks.size(); ++i) {
    const std::string& tok = toks[i];
    double a = 0.0, b = 0.0, d = 0.0;
    // `a b to c` / `a b : c`
    if (i + 3 < toks.size() && (toks[i + 2] == "to" || toks[i + 2] == ":")) {
      double a2 = 0.0;
      if (parseNumber(tok, &a) && parseNumber(toks[i + 1], &a2) && parseNumber(toks[i + 3], &b)) {
        // The run must reach past its second element: `2 3 to 5` is 2 3 4 5, but `2 3 to 3`
        // is no numlist (review st_mat2 nl_to1/nl_to2).
        std::vector<double> run;
        if (!numlistRun(a2 + (a2 - a), a2 - a, b, &run) || run.empty()) {
          res.rc = 121;
          return res;
        }
        vals.push_back(a);
        vals.push_back(a2);
        vals.insert(vals.end(), run.begin(), run.end());
        i += 3;
        continue;
      }
    }
    if (parseNumber(tok, &a)) {
      vals.push_back(a);
      continue;
    }
    const size_t slash = tok.find('/');
    if (slash != std::string::npos) {
      if (!parseNumber(tok.substr(0, slash), &a) || !parseNumber(tok.substr(slash + 1), &b) ||
          !numlistRun(a, a <= b ? 1.0 : -1.0, b, &vals)) {
        res.rc = 121;
        return res;
      }
      continue;
    }
    const size_t lp = tok.find_first_of("([");
    if (lp != std::string::npos) {
      const char close = tok[lp] == '(' ? ')' : ']';
      const size_t rp = tok.find(close, lp + 1);
      if (rp == std::string::npos || !parseNumber(tok.substr(0, lp), &a) ||
          !parseNumber(tok.substr(lp + 1, rp - lp - 1), &d) ||
          !parseNumber(tok.substr(rp + 1), &b) || !numlistRun(a, d, b, &vals)) {
        res.rc = 121;
        return res;
      }
      continue;
    }
    res.rc = 121;
    return res;
  }
  for (double v : vals) {
    if (std::isnan(v)) {
      res.rc = 127;
      return res;
    }
  }
  for (double v : vals) {
    if (v != std::floor(v)) {
      res.rc = 126;
      return res;
    }
  }
  for (double v : vals) {
    if (v <= 0.0 || v > static_cast<double>(maxValue)) {
      res.rc = 125;
      return res;
    }
  }
  for (double v : vals) res.values.push_back(static_cast<long long>(v));
  return res;
}

// The 0-based stripe positions a name selects: `eq:` = every entry of that equation,
// `eq:name` = the first entry with both, a bare name = the first entry with that name
// and no equation (mp1 eqname: `r(y)` against `b:y` is "y not found"; dupname: the
// first of two `x` rows).
std::vector<size_t> positionsForName(const std::vector<std::string>& stripe,
                                     const std::string& want) {
  std::vector<size_t> out;
  const auto [weq, wname] = splitStripe(want);
  const bool hasColon = want.find(':') != std::string::npos;
  for (size_t i = 0; i < stripe.size(); ++i) {
    auto [eq, nm] = splitStripe(stripe[i]);
    if (eq == "_") eq.clear();
    if (hasColon && wname.empty()) {
      if (eq == weq) out.push_back(i);
      continue;
    }
    const std::string wantEq = (weq == "_") ? std::string() : weq;
    if (eq == wantEq && nm == wname) {
      out.push_back(i);
      break;
    }
  }
  return out;
}

// One axis of the selection. `asNumbers` forces the index reading of each word (the
// `names` option's effect on columns); otherwise the list is a numlist when it is one
// and names when numlist says r(121). Returns an error block or nullopt; `picked` gets
// 0-based positions in the order given.
std::optional<ojson> selectAxis(const std::string& spec, const std::vector<std::string>& stripe,
                                bool forceNumbers, bool forceNames, SessionState& session,
                                std::vector<size_t>* picked) {
  const long long extent = static_cast<long long>(stripe.size());
  if (forceNumbers) {
    // The package hands each word to a submatrix subscript as an EXPRESSION, and a
    // submatrix subscript ROUNDS (review st_mat2 names_frac: `c(2.7) names` is column 3;
    // names_slash: `c(1/2) names` is 0.5, column 1). An index outside the matrix is the
    // subscript's r(503); a name that is no scalar or variable is r(111) (mp1 cnames_n).
    for (const std::string& w : words(spec)) {
      double v = 0.0;
      if (!parseNumber(w, &v)) {
        std::optional<double> ev = evalScalarExpr(w, session);
        if (!ev.has_value()) {
          if (isNameWord(w)) return matError(w + " not found", 111);
          return matError("invalid syntax", 198);
        }
        v = *ev;
      }
      const double t = std::floor(v + 0.5);
      if (std::isnan(v) || t < 1.0 || t > static_cast<double>(extent))
        return matError("conformability error", 503);
      picked->push_back(static_cast<size_t>(t) - 1);
    }
    return std::nullopt;
  }
  bool asNames = forceNames;
  if (!asNames) {
    NumlistOutcome nl = positiveIntNumlist(spec, extent);
    if (nl.rc == 0) {
      for (long long v : nl.values) picked->push_back(static_cast<size_t>(v - 1));
      return std::nullopt;
    }
    if (nl.rc == 127) return matError("invalid numlist has missing values", 127);
    if (nl.rc == 126) return matError("invalid numlist has noninteger elements", 126);
    if (nl.rc == 125) return matError("invalid numlist has elements outside of allowed range", 125);
    asNames = true;  // r(121): not a numlist, so a list of names
  }
  for (const std::string& w : words(spec)) {
    std::vector<size_t> pos = positionsForName(stripe, w);
    if (pos.empty()) return matError(w + " not found", 111);
    picked->insert(picked->end(), pos.begin(), pos.end());
  }
  return std::nullopt;
}

// A matrix as `matrix A = name` reads it: a user matrix, or a stored-results matrix spelled
// e(name)/r(name). nullopt in *out when there is none; an error block only for an e() matrix
// the resolver refuses by name (an e(V) the fit did not send).
std::optional<ojson> sourceMatrix(const std::string& name, SessionState& session,
                                  std::optional<eval::EvalMatrix>* out) {
  if (isNameWord(name)) {
    if (const eval::EvalMatrix* m = session.eval.findMatrix(name)) *out = *m;
    return std::nullopt;
  }
  if (name.size() > 3 && (name[0] == 'e' || name[0] == 'r') && name[1] == '(' &&
      name.back() == ')') {
    const std::string inner = trimCopy(name.substr(2, name.size() - 3));
    if (name[0] == 'e') {
      int rc = 111;
      std::string why;
      *out = session.eval.resolveEMatrix(inner, &rc, &why);
      if (!out->has_value() && rc != 111 && rc != 301 && !why.empty()) return matError(why, rc);
    } else if (const eval::EvalMatrix* m = session.eval.findClassMatrix('r', inner)) {
      *out = *m;
    }
  }
  return std::nullopt;
}

}  // namespace

namespace {

// The body of matsort. It reads its source BEFORE r() is replaced (inside an rclass
// program the caller's r() is still visible -- review st_mat rtable_rankonly sorts
// r(table)), writes the user matrices `rank`/`order` and, unless rankonly, the sorted
// matrix; the caller moves `rank`/`order` into r() on success.
ojson matsortCore(const eval::ParsedCommand& parsed, SessionState& session) {
  // `args` takes no options: `matsort X 1, d` is r(101) "options not allowed" (review st_mat2
  // comma).
  if (!parsed.opts.empty()) return matError("options not allowed", 101);
  const std::vector<std::string> w = words(trimCopy(parsed.argText));
  const std::string matname = w.size() > 0 ? w[0] : std::string();
  const std::string sortcolText = w.size() > 1 ? w[1] : std::string("1");
  std::string dir = w.size() > 2 ? w[2] : std::string("u");
  const std::string rankonly = w.size() > 3 ? w[3] : std::string("replace");
  // Words past the fourth are never read (`args` names four).

  std::optional<eval::EvalMatrix> src;
  if (auto e = sourceMatrix(matname, session, &src)) return *e;
  if (!src.has_value()) return matError(matname + " is not a matrix", 198);
  const bool classSource = !isNameWord(matname);
  const char d0 = dir.empty() ? 'u' : dir[0];
  if (d0 != 'd' && d0 != 'u')
    return matError("Sort direction must be either up (for ascending) or down (for descending)",
                    198);
  const bool down = d0 == 'd';
  bool replace = true;
  if (rankonly == "rankonly") {
    replace = false;
  } else if (rankonly != "replace") {
    return matError("Replace option must be either replace or rankonly: " + rankonly, 198);
  }

  eval::EvalMatrix A = std::move(*src);
  const size_t nrows = A.data.size();
  const size_t ncols = nrows > 0 ? A.data[0].size() : 0;
  A.rownames = stripeOrDefault(A.rownames, nrows, 'r');
  A.colnames = stripeOrDefault(A.colnames, ncols, 'c');

  // The column is an expression: a number, `1+1`, a scalar `sc` (review st_mat sortcol_expr,
  // sortcol_scalar, both rc 0). A name that is no scalar or variable is r(111) (mp1 nonnum).
  double sortcol = 0.0;
  if (!parseNumber(sortcolText, &sortcol)) {
    std::optional<double> ev = evalScalarExpr(sortcolText, session);
    if (!ev.has_value()) {
      if (isNameWord(sortcolText)) return matError(sortcolText + " not found", 111);
      return matError("invalid syntax", 198);
    }
    sortcol = *ev;
  }
  if (std::isnan(sortcol) || sortcol > static_cast<double>(ncols) || sortcol < 1.0)
    return matError("Invalid column number specified: " + sortcolText, 198);
  // A matrix element in an expression truncates its subscript (review st_mat2 col17: column
  // 1.7 sorts by column 1), unlike matselrc's submatrix subscript, which rounds.
  const size_t col = static_cast<size_t>(std::trunc(sortcol)) - 1;

  // `rank` and `order` are user matrices from here on, exactly as in Stata.
  std::vector<double> order(nrows), rank(nrows, 0.0);
  for (size_t i = 0; i < nrows; ++i) order[i] = static_cast<double>(i + 1);
  putUserMatrix(session, "rank", columnVector(rank));
  putUserMatrix(session, "order", columnVector(order));

  const double kMaxStart = -2147483647.0;
  const double kMinStart = 2147483620.0;
  for (size_t i = 0; i < nrows; ++i) {
    double best = down ? kMaxStart : kMinStart;
    bool found = false;
    size_t highest = 0;
    for (size_t j = i; j < nrows; ++j) {
      const double cur = sortKey(A.data[j][col]);
      if ((down && best < cur) || (!down && best > cur)) {
        highest = j;
        best = cur;
        found = true;
      }
    }
    if (!found) {
      // The package then reads row 0 of its order vector: Stata's r(503), with the
      // matrix untouched and `order` as far as the sort had got (mp1 miss_up, huge).
      putUserMatrix(session, "order", columnVector(order));
      return matError("conformability error", 503);
    }
    std::swap(order[i], order[highest]);
    std::swap(A.data[i], A.data[highest]);
  }
  for (size_t i = 0; i < nrows; ++i)
    rank[static_cast<size_t>(order[i]) - 1] = static_cast<double>(i + 1);

  // Row names follow the rows; the equation stripe stays where it was (mp1 eq:
  // `e1 e1 e2 e2` before and after).
  {
    const std::vector<std::string> before = A.rownames;
    for (size_t i = 0; i < nrows; ++i) {
      const std::string eq = splitStripe(before[i]).first;
      const std::string nm = splitStripe(before[static_cast<size_t>(order[i]) - 1]).second;
      A.rownames[i] = eq.empty() ? nm : eq + ":" + nm;
    }
  }
  putUserMatrix(session, "rank", columnVector(rank));
  putUserMatrix(session, "order", columnVector(order));
  if (replace) {
    // `mat r(table) = A` is no matrix operation (review st_mat2 rtable_replace: r(501));
    // rankonly on a stored-results matrix is fine (eb_rankonly, rtable_rankonly).
    if (classSource) return matError("matrix operation not found", 501);
    putUserMatrix(session, matname, std::move(A));
  }
  return ojson::array();
}

}  // namespace

// `matsort matname [sortcol [dir [rankonly]]]`.
nlohmann::ordered_json runMatsort(const eval::ParsedCommand& parsed, SessionState& session) {
  (void)kVocabMatsort;  // no options at all: matsortCore refuses any, r(101)
  ojson result = matsortCore(parsed, session);
  // An rclass program: r() is replaced whatever the outcome (a failed run leaves it
  // empty), and only a completed sort posts r(rank)/r(order).
  eval::replaceRClass(session.eval);
  eval::declareRPosted(session.eval, "matsort", {});
  if (result.is_object() && result.value("type", std::string()) == "error") return result;
  // `return matrix rank = rank` / `order = order` MOVE the two user matrices into r().
  for (const char* nm : {"rank", "order"}) {
    std::optional<eval::EvalMatrix> m = takeUserMatrix(session, nm);
    if (!m.has_value()) continue;
    m->gen = eval::nextStoreGeneration();
    session.eval.rMatrices.emplace_back(nm, std::move(*m));
  }
  eval::declareRPosted(session.eval, "matsort", {"rank", "order"});
  return ojson::array();
}

// `matselrc m1 m2 [, row() col() names]`.
nlohmann::ordered_json runMatselrc(const eval::ParsedCommand& parsed, SessionState& session) {
  // The two names come before the comma (`gettoken` on blank and comma).
  std::string pre = parsed.argText;
  if (const size_t comma = pre.find(','); comma != std::string::npos) pre = pre.substr(0, comma);
  const std::vector<std::string> names = words(trimCopy(pre));
  if (names.size() < 2) return matError("must name two matrices", 198);
  if (names.size() > 2) return matError("invalid syntax", 198);
  if (auto err = eval::rejectUnknownOptions(parsed.opts, kVocabMatselrc.known,
                                            kVocabMatselrc.unimplemented))
    return ojson(*err);

  std::string rowSpec, colSpec;
  bool byNames = false;
  for (const auto& [k, v] : parsed.opts) {
    std::string val = v.isBool ? std::string() : trimCopy(v.strValue);
    if (val.size() >= 2 && val.front() == '"' && val.back() == '"') val = val.substr(1, val.size() - 2);
    if (k == "row") rowSpec = val;
    else if (k == "col") colSpec = val;
    else if (k == "names") byNames = true;
  }
  if (trimCopy(rowSpec).empty() && trimCopy(colSpec).empty())
    return matError("nothing to do", 198);

  const std::string& m1 = names[0];
  const std::string& m2 = names[1];
  if (!isNameWord(m2)) return matError(m2 + " invalid name", 198);

  // The source: `mat A = m1` -- a user matrix, a stored-results matrix spelled
  // e(name)/r(name), or either transposed with a trailing `'` (review st_mat transpose_src).
  std::string srcName = m1;
  bool transpose = false;
  if (srcName.size() > 1 && srcName.back() == '\'') {
    transpose = true;
    srcName.pop_back();
  }
  std::optional<eval::EvalMatrix> srcCopy;
  if (auto e = sourceMatrix(srcName, session, &srcCopy)) return *e;
  if (!srcCopy.has_value()) return matError(srcName + " not found", 111);
  if (transpose) {
    eval::EvalMatrix t;
    const size_t r0 = srcCopy->data.size();
    const size_t c0 = r0 > 0 ? srcCopy->data[0].size() : 0;
    t.data.assign(c0, std::vector<double>(r0));
    for (size_t r = 0; r < r0; ++r)
      for (size_t c = 0; c < c0; ++c) t.data[c][r] = srcCopy->data[r][c];
    t.rownames = stripeOrDefault(srcCopy->colnames, c0, 'r');
    t.colnames = stripeOrDefault(srcCopy->rownames, r0, 'c');
    srcCopy = std::move(t);
  }

  eval::EvalMatrix A = std::move(*srcCopy);
  const size_t nrows = A.data.size();
  const size_t ncols = nrows > 0 ? A.data[0].size() : 0;
  A.rownames = stripeOrDefault(A.rownames, nrows, 'r');
  A.colnames = stripeOrDefault(A.colnames, ncols, 'c');

  if (!trimCopy(colSpec).empty()) {
    std::vector<size_t> pick;
    if (auto e = selectAxis(colSpec, A.colnames, byNames, false, session, &pick)) return *e;
    eval::EvalMatrix B;
    B.rownames = A.rownames;
    for (size_t r = 0; r < nrows; ++r) {
      std::vector<double> row;
      row.reserve(pick.size());
      for (size_t c : pick) row.push_back(A.data[r][c]);
      B.data.push_back(std::move(row));
    }
    for (size_t c : pick) B.colnames.push_back(A.colnames[c]);
    A = std::move(B);
  }
  if (!trimCopy(rowSpec).empty()) {
    std::vector<size_t> pick;
    if (auto e = selectAxis(rowSpec, A.rownames, false, byNames, session, &pick)) return *e;
    eval::EvalMatrix B;
    B.colnames = A.colnames;
    for (size_t r : pick) {
      B.data.push_back(A.data[r]);
      B.rownames.push_back(A.rownames[r]);
    }
    A = std::move(B);
  }
  A.gen = eval::nextStoreGeneration();
  putUserMatrix(session, m2, std::move(A));
  return ojson::array();
}

}  // namespace piet::handlers
