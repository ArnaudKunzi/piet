// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
#include "piet/loops.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cmath>
#include <optional>
#include <regex>
#include <set>
#include <sstream>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>

#include "piet/cclass.h"
#include "piet/counters.h"  // BUG-350 coverage: the loop-capture drop site counts too
#include "piet/doexec.h"
#include "piet/eval/errors.h"
#include "piet/eval/exprfns.h"
#include "piet/eval/macros.h"
#include "piet/eval/pyeval.h"
#include "piet/eval/tokenizer.h"
#include "piet/parallel.h"
#include "piet/stata_format.h"  // BUG-1395: numlist element text is %15.0g
#include "piet/strutil.h"
#include "core/expr_blanks.h"  // FU-875: Stata's blank squeeze and r(132) pairing rule
#include "core/ident.h"  // Unicode names: the shared identifier byte tests
#include "data/missing.h"  // BUG-921: the extended-missing kind of a cell read by the if condition
#include "dispatch_profile.h"  // EXPRLAYER: expression-zone attribution (opt-in, stderr)

// BUG-920: Stata's macro real-to-text rule (%18.0g text, no leading 0), defined
// in eval/macros.cpp beside the local/global store that applies it.
namespace piet::eval {
std::string stataMacroRealText(double d);
}  // namespace piet::eval

namespace piet::loops {

namespace {

constexpr int kWhileCap = 1000;
constexpr int kNumlistMaxElements = 10000;

// P7 (console cap, COUNCIL-20260710 D5/F6): kWhileCap/kNumlistMaxElements
// above already bound how many times a loop can ITERATE, but not how many
// blocks its body emits per iteration — a 3-command body over 10,000
// numlist-bounded iterations is already 30,000+ blocks, well past what a
// single bridge payload should ever carry. This bounds allBlocks itself,
// independent of (and in addition to) the iteration-count caps. Mirrors
// doexec.cpp's kMaxRunBlocks in magnitude — generous / classroom-day-safe,
// and (same reasoning as there) it only stops STORING blocks: the loop still
// runs to its normal completion/break/error so dataset/scalar side effects
// stay correct, only the excess console text is elided.
constexpr size_t kMaxLoopBlocks = 20000;

ojson errBlock(const std::string& msg) { return eval::engineError(msg); }

ojson loopTruncatedBlock() {
  ojson b;
  b["type"] = "display";
  b["text"] = "(output truncated: this loop exceeded " + std::to_string(kMaxLoopBlocks) +
              " blocks -- remaining iterations run, but their output is suppressed)";
  return b;
}

// Appends `b` to allBlocks unless the per-loop cap already tripped; the
// first block that would cross kMaxLoopBlocks becomes a single elision
// marker instead, and every push after that is a no-op. `truncated` is
// owned by the caller (one flag per runForeach/runWhile invocation).
void pushLoopBlock(ojson& allBlocks, bool& truncated, const ojson& b) {
  if (truncated) return;
  if (allBlocks.size() >= kMaxLoopBlocks) {
    allBlocks.push_back(loopTruncatedBlock());
    truncated = true;
    return;
  }
  allBlocks.push_back(b);
}

// A5-04: forward to the shared strutil copy (piet/strutil.h) — kept as thin
// wrappers (not removed) so this file's many lower(...)/trim(...) call
// sites didn't need touching. Pure dedup, no behavior change.
std::string lower(std::string s) { return strutil::lower(std::move(s)); }

std::string trim(const std::string& s) { return strutil::trim(s); }

std::string rstripBrace(std::string s) {
  s = trim(s);
  if (!s.empty() && s.back() == '{') {
    s.pop_back();
    s = trim(s);
  }
  return s;
}

// ---------------------------------------------------------------------------
// Header keyword detection (loops.py's _LOOP_IF_KEYWORDS)

// BS-015: also covers `capture` and its abbreviations (cap/capt/captu/
// captur/capture — same spelling family as BS-016's fix to dispatcher.cpp's
// single-line capture stripping). `capture` is not a loop/branch construct,
// but like foreach/forvalues/while/if it CAN head a brace-aggregated `{ }`
// block (`capture { ... }`), and doexec.cpp's aggregateBraces()/
// collectBody() key EVERY brace-block recognition off this one set. Without
// this, a multi-line `capture { ... }` block's body lines were aggregated
// as independent top-level statements — never wrapped by capture at all:
// each line ran uncaptured (so `_rc` reflected whatever unrelated command
// ran last, not the block's real outcome), and an error inside the block
// would incorrectly halt the entire do-file instead of being swallowed the
// way `capture` promises. See executeLoop's dedicated "capture" branch
// below (and runCaptureBlock) for the actual block semantics.
const std::unordered_set<std::string>& loopIfKeywords() {
  static const std::unordered_set<std::string> kw = {
      // AER-SCOPE1 bundle A: forval/forv/forvalue are Stata's own accepted
      // truncations of forvalues, and the corpus writes them 946 times across
      // 40 packages -- more often than the full spelling in some files. They
      // must be recognised HERE, not just in the registry: a block opener is
      // detected before any registry lookup happens, so an abbreviation the
      // registry knows and this set does not would still fail to open a block.
      "forval", "forv", "forvalue",
      "foreach", "forvalues", "while", "if",
      "capture", "captur", "captu", "capt", "cap",
      // BUG-132: quietly/noisily ALSO head brace-aggregated blocks
      // (`quietly { ... }` / `noisily { ... }`) — the same omission BS-015
      // fixed for capture. Spellings mirror the dispatcher's single-line
      // prefix stripper (kQuietlyPrefixes, dispatcher.cpp:668-670) exactly.
      // See executeLoop's dedicated quietly/noisily branch (runQuietlyBlock)
      // for the block semantics. These only ever gate block recognition when a
      // `{` is actually present — every isLoopHeader call site requires a brace
      // on the line or a look-ahead to a lone `{` line — so a plain single-line
      // `quietly cmd` / `noisily cmd` still routes to the flat dispatch path
      // (dispatcher.cpp's Pre-0 prefix stripper) unchanged.
      "quietly", "qui", "quie", "quiet",
      "noisily", "noisil", "noisi", "nois", "noi", "no", "n",
      // PLEVEL-IMPL-WAVE arc ①: `parallel[, workers(N)] { … }` — the design's
      // §2.3 option B, a real block command rather than a comment directive.
      // Registered here for the same reason capture/quietly are: this set is
      // the ONE source of brace-block recognition (doexec.cpp's
      // aggregateBraces/collectBody key off it), so without the entry the body
      // lines would run as independent top-level statements and the block head
      // would error as an unknown command. NO abbreviations: unlike
      // capture/quietly (whose spellings mirror Stata's own prefix family),
      // `parallel` is a NEW PIET-only keyword with no Stata abbreviation
      // heritage to honour — and a short form like `par` would shadow user
      // programs. Recognition still requires an actual `{` on the line (or a
      // lone `{` look-ahead), so a bare `parallel` line is untouched by this
      // and keeps whatever the dispatcher does with it today.
      "parallel"};
  return kw;
}

std::string firstWord(const std::string& line) {
  std::string s = trim(line);
  size_t sp = s.find_first_of(" \t");
  return sp == std::string::npos ? s : s.substr(0, sp);
}

// The lowered head keyword of a block line, for the loopIfKeywords() lookup.
//
// `parallel, workers(2) {` is the FIRST block head in this codebase whose
// keyword is immediately followed by an option comma, so firstWord() returns
// "parallel," and the keyword lookup misses — the body lines then run as
// independent top-level statements and the head errors as an unknown command
// (measured, not theorised: that is exactly what the first build did). Strip
// that one trailing comma.
//
// Scoped to `parallel` ON PURPOSE. Stripping it for every keyword would
// silently start treating `capture, {` / `quietly, {` — today unknown-command
// errors — as brace blocks, which is a real behaviour change to lines this arc
// was not asked to touch. If that widening is ever wanted it should arrive as
// its own change with its own fixture, not as a side effect of adding a
// keyword.
std::string headKeyword(const std::string& line) {
  std::string fw = lower(firstWord(line));
  if (fw.size() > 1 && fw.back() == ',') {
    std::string stripped = fw.substr(0, fw.size() - 1);
    if (stripped == "parallel") return stripped;
  }
  return fw;
}

// ---------------------------------------------------------------------------
// Numlist expansion (loops.py's _parse_numlist)

struct NumlistResult {
  bool isError = false;
  ojson error;
  std::vector<std::string> tokens;
};

// BUG-1395: whether the element past the end still belongs follows StataBE 19.5's
// own measurements, not one formula: no rule tried reproduces all 2,392 measured
// ranges (lane expr-sem x1b nl*.do, ho.do and r3/g1.do; the reviews' vnl*.do,
// v95b-d.do and nlx.do). `v` is start + k*step and `run` the element's value
// (numlistValue below). An element that is a whole number gets no hair:
// 0(1)2.9999999999999996, 0.5(0.5)2.99999999999999 and 1(1.5)3.99999999999999 stop
// short of it. An end the value lands within one unit in the last place of is
// kept (21.10(0.07)21.31). Otherwise `v` may pass the end by a hair relative to the
// distance covered, k*|step|, compared in doubles, so the hair rounds to the end's
// grid: 0(0.1)0.3 ends at .3, 0(2.5)7.4999999999999 keeps 7.5, 0(0.1)0.299999999999996
// drops .3, and 156.33(0.05)158.38 and -200.10(0.03)-198.30 keep an end `v` passes by
// one unit in the last place. That keeps every end Stata keeps, here and on a fresh
// hold-out of 360 ranges (r3/g2.do). 226 of the 2,392 and 3 of the 360 keep an end
// Stata drops, as base PIET did: 60.9(0.02)61, 10(.005)10.02, 1000.1(0.1)1000.4
// and the step 0.02 from 32.5 to 63.3 and from 128.4 up.
bool numlistPastEnd(double v, double run, double k, double step, double end) {
  const double over = step > 0 ? v - end : end - v;
  if (over <= 0) return false;
  if (v == std::floor(v)) return true;
  const double runOver = step > 0 ? run - end : end - run;
  const double endUlp = std::nextafter(std::fabs(end), INFINITY) - std::fabs(end);
  if (runOver <= endUlp) return false;
  const double hair = 1.335e-14 * std::fabs(k * step);
  return step > 0 ? v > end + hair : v < end - hair;
}

// BUG-1395: an element's value is the running sum, start, start + step, ..., as
// StataBE 19.5 holds it: 100(-0.1)0 prints 9.900000000001 and .1000000000014 and
// -1(0.10000000000001).5 9.97812943e-14, the sum's digits, where start + k*step
// prints 9.9, .1 and 9.99200722e-14. A sum that lands within 1.01e-14 of the
// distance covered, |sum - start|, of a whole number becomes that number, and the
// sum goes on from there: -1(.2)1 and .3(-.1)-.3 print 0, 27.88(-0.17)-192.78
// steps on from 17 and prints .85 .68 .51, 29.8(-0.1)-26.7 prints 1 .9 ... 0 ... -1,
// while 11.36(-0.01)-0.58 never snaps and prints -.0099999999998. The element the
// end is reached at prints as the end: 100(-0.1)0 ends at 0, -10.65(0.01)0.11 at
// .11. Element texts then differ from Stata's in 12 of 106,814 measured and 5 of
// 146,546 on the hold-out, all in four ranges whose snaps sit within 0.5% of the
// cut: Stata snapped 27.88(-0.17)-192.78 at 17 but not -55.76(0.17)87.55 at -34 at
// the same ratio, so the cut Stata applies is not exactly this one.
double numlistValue(double run, double start) {
  const double whole = std::round(run);
  if (whole != run && std::fabs(run - whole) < 1.01e-14 * std::fabs(run - start)) return whole;
  return run;
}

NumlistResult parseNumlist(const std::string& spec) {
  NumlistResult r;
  std::vector<double> result;

  // A number as Stata writes one: `.5`, `-.25`, `1e3` and `1.5e-3` too
  // (StataBE: -.5(.25)0 is -.5 -.25 0; 1e15(1e15)3e15 is three elements).
  static const std::string kNum = R"([+-]?(?:\d+\.?\d*|\.\d+)(?:[eE][+-]?\d+)?)";
  static const std::regex reRangeStep("^(" + kNum + ")\\((" + kNum + ")\\)(" + kNum + ")$");
  static const std::regex reSlash("^(" + kNum + ")/(" + kNum + ")$");

  std::istringstream iss(spec);
  std::string tok;
  while (iss >> tok) {
    std::smatch m;
    double start = 0, step = 0, end = 0;
    bool range = false;
    if (std::regex_match(tok, m, reRangeStep)) {
      start = std::stod(m[1]), step = std::stod(m[2]), end = std::stod(m[3]);
      if (step == 0.0) {
        r.isError = true;
        r.error = errBlock("numlist: step cannot be zero");
        return r;
      }
      // The step runs toward the end whatever its sign: StataBE 19.5 expands
      // 3(1)1 to 3 2 1 and 1(-1)3 to 1 2 3.
      if ((end - start) * step < 0) step = -step;
      range = true;
    } else if (std::regex_match(tok, m, reSlash)) {
      start = std::stod(m[1]), end = std::stod(m[2]);
      step = start <= end ? 1.0 : -1.0;
      range = true;
    }
    if (range) {
      double run = start;
      for (int k = 0;; ++k) {
        // start + k*step, unfused, is what the end test reads (numlistPastEnd).
#if defined(__clang__)
#pragma clang fp contract(off)
#endif
        const double v = start + k * step;
        if (k > 0) run = numlistValue(run + step, start);
        if (numlistPastEnd(v, run, k, step, end)) break;
        const double hair = 1.335e-14 * std::fabs(k * step);
        result.push_back(k > 0 && v >= end - hair && v <= end + hair ? end : run);
        if (k + 1 > kNumlistMaxElements) {
          r.isError = true;
          r.error = errBlock("numlist: more than " + std::to_string(kNumlistMaxElements) + " elements");
          return r;
        }
      }
      continue;
    }
    try {
      size_t pos = 0;
      double v = std::stod(tok, &pos);
      if (pos != tok.size()) throw std::invalid_argument("trailing");
      result.push_back(v);
    } catch (...) {
      r.isError = true;
      r.error = errBlock("numlist: invalid token '" + tok + "'");
      return r;
    }
  }
  if (result.size() > static_cast<size_t>(kNumlistMaxElements)) {
    r.isError = true;
    r.error = errBlock("numlist: more than " + std::to_string(kNumlistMaxElements) + " elements");
    return r;
  }
  // BUG-1395: each element's text is its %15.0g rendering with the padding
  // dropped -- StataBE 19.5 (x1b/a6-a8.do, 38 values): .0625 not 0.0625,
  // .1234567890123, 1.00000000e-20, 1.23456789e+13 but 1234567890123, -.3333333333333.
  for (double v : result) r.tokens.push_back(piet::fmt::general(v, 15, 0));
  return r;
}


// ---------------------------------------------------------------------------
// foreach-in token split (quote-aware) — loops.py's _split_foreach_tokens

std::vector<std::string> splitForeachTokens(const std::string& itemsStr) {
  std::vector<std::string> tokens;
  std::string s = trim(itemsStr);
  size_t i = 0;
  while (i < s.size()) {
    if (s[i] == '"') {
      size_t j = s.find('"', i + 1);
      if (j == std::string::npos) {
        tokens.push_back(s.substr(i + 1));
        break;
      }
      tokens.push_back(s.substr(i + 1, j - i - 1));
      i = j + 1;
    } else if (std::isspace(static_cast<unsigned char>(s[i]))) {
      ++i;
    } else {
      size_t j = i;
      while (j < s.size() && !std::isspace(static_cast<unsigned char>(s[j]))) ++j;
      tokens.push_back(s.substr(i, j - i));
      i = j;
    }
  }
  return tokens;
}

// ---------------------------------------------------------------------------
// _eval_bool_expr — C++ port using the M1 pyeval/exprfns machinery instead of
// a fresh eval engine. Rewrite order mirrors loops.py exactly.

// Regex-based substring replace with a callback (mirrors Python re.sub).
std::string regexReplace(const std::regex& re, const std::string& s,
                          const std::function<std::string(const std::smatch&)>& fn) {
  std::string out;
  auto begin = std::sregex_iterator(s.begin(), s.end(), re);
  auto end = std::sregex_iterator();
  size_t last = 0;
  for (auto it = begin; it != end; ++it) {
    const std::smatch& m = *it;
    out.append(s, last, static_cast<size_t>(m.position()) - last);
    out += fn(m);
    last = static_cast<size_t>(m.position() + m.length());
  }
  out.append(s, last, s.size() - last);
  return out;
}

// libc++'s std::regex (ECMAScript grammar) does NOT support lookbehind
// assertions ((?<!...)/(?<=...) — a PCRE/C++23 feature, not in the
// ECMAScript std::regex grammar) — throws std::regex_error at construction.
// loops.py's Python `re` module DOES support lookbehind, so its
// `(?<!X)C(?!Y)` patterns (single-char C not preceded by X, not followed by
// Y) are ported here as manual character scans instead of regex.

// Single `=` (assignment-in-condition) -> `==`, skipping when adjacent to
// =/!/</> (already part of a multi-char comparison operator).
std::string replaceLoneEquals(const std::string& s) {
  std::string out;
  out.reserve(s.size());
  auto isCmpChar = [](char c) { return c == '=' || c == '!' || c == '<' || c == '>'; };
  for (size_t i = 0; i < s.size(); ++i) {
    if (s[i] == '=') {
      bool precededByCmp = (i > 0 && isCmpChar(s[i - 1]));
      bool followedByEq = (i + 1 < s.size() && s[i + 1] == '=');
      if (!precededByCmp && !followedByEq) {
        out += "==";
        continue;
      }
    }
    out += s[i];
  }
  return out;
}

struct BoolEvalResult {
  bool isError = false;
  ojson error;
  bool value = false;
};

// BUG-921: a data-plane cell as the condition evaluator's number. The data
// plane codes a missing as a NaN carrying its kind; the evaluator's missing is
// the 2^1023 band (exprfns.h), where `x == .` is true and `. > 1e300` holds.
// Binding the raw NaN made every comparison with a missing cell false.
double cellAsEvalNumber(double v) {
  if (!std::isnan(v)) return v;
  const int k = cecon::missing_kind(v);
  return eval::stataMissingWithKind(k >= cecon::kMissA && k <= cecon::kMissZ ? k : 0);
}

// BUG-921: the row-1 value of a dataset variable as an evaluator value -- the
// number (missing mapped as above) or, for a string variable, its text.
// nullopt when the column has no row 1.
std::optional<eval::PyValue> obsValueAsPy(Datastore& ds, const std::string& name, int64_t row) {
  if (row < 0 || row >= ds.nRows()) return std::nullopt;
  const cecon::Column& col = ds.col(name);
  if (ds.dtype(name) == "factor") {
    const int32_t code = col.codes()[static_cast<size_t>(row)];
    const auto& levels = col.levels;
    return eval::PyValue::str(code >= 0 && static_cast<size_t>(code) < levels.size()
                                  ? levels[static_cast<size_t>(code)]
                                  : std::string());
  }
  const double* d = col.f64();
  if (d == nullptr) return std::nullopt;
  return eval::PyValue::floating(cellAsEvalNumber(d[row]));
}

// BUG-921: explicit subscripts `var[exp]` in an if/while condition read that
// observation (oracle w2_prog_controlflow pc_if_varref: `if i[3] > 250` reads
// i[3] = 300; pc_if_missing: `if d[99] == .` and `if missing(f[7])` past _N are
// missing, so true). Each subscript of a DATASET variable is replaced by its
// value as literal text: a number in parentheses, `.`/`.a`... for a missing
// (the missing-literal pass below turns those into the evaluator's doubles), a
// quoted string for a string variable ("" past _N, as Stata's string cells
// have no missing). Matrices were already substituted (substituteMatrixRefs)
// and _b[]/_se[] are not dataset variables, so neither is touched. Quoted text
// is skipped. A subscript whose index does not evaluate, or a string value
// holding a double quote, is left as it was (the evaluator then refuses it as
// before). nullopt = nothing substituted.
std::optional<std::string> substituteVarSubscripts(const std::string& in, SessionState& session) {
  if (in.find('[') == std::string::npos) return std::nullopt;
  Datastore* ds = nullptr;
  bool dsAsked = false;
  std::string out;
  bool any = false;
  // Unicode names (lane M): the shared identifier byte tests, so `قيمة[2]` and
  // the mixed-script `دخل_2020[1]` are each one subscripted name.
  const auto isId0 = [](char c) { return cecon::ident::isIdentStartByte(c); };
  const auto isId = [](char c) { return cecon::ident::isIdentByte(c); };
  size_t i = 0;
  while (i < in.size()) {
    const char c = in[i];
    if (c == '"') {  // a string literal: copy through its closing quote
      const size_t close = in.find('"', i + 1);
      const size_t stop = close == std::string::npos ? in.size() : close + 1;
      out.append(in, i, stop - i);
      i = stop;
      continue;
    }
    if (!isId0(c) || (i > 0 && (isId(in[i - 1]) || in[i - 1] == '.'))) {
      out += c;
      ++i;
      continue;
    }
    size_t j = i + 1;
    while (j < in.size() && isId(in[j])) ++j;
    size_t k = j;
    while (k < in.size() && (in[k] == ' ' || in[k] == '\t')) ++k;
    const std::string name = in.substr(i, j - i);
    if (k >= in.size() || in[k] != '[') {
      out.append(name);
      i = j;
      continue;
    }
    // Find the matching close bracket.
    int depth = 0;
    size_t close = std::string::npos;
    for (size_t q = k; q < in.size(); ++q) {
      if (in[q] == '[') ++depth;
      if (in[q] == ']' && --depth == 0) {
        close = q;
        break;
      }
    }
    if (close == std::string::npos) {
      out.append(name);
      i = j;
      continue;
    }
    if (name == "_b" || name == "_se" || session.eval.findMatrix(name) != nullptr) {
      out.append(in, i, close + 1 - i);
      i = close + 1;
      continue;
    }
    if (!dsAsked) {
      ds = session.datastore();
      dsAsked = true;
    }
    std::optional<std::string> lit;
    if (ds != nullptr && ds->has(name)) {
      const std::string idxText =
          eval::substituteObsSpecials(trim(in.substr(k + 1, close - k - 1)), session.eval);
      std::optional<std::string> idxVal = eval::evalArith(idxText, &session.eval);
      double idx = 0.0;
      bool idxOk = false;
      if (idxVal.has_value()) {
        char* e = nullptr;
        idx = std::strtod(idxVal->c_str(), &e);
        idxOk = e != idxVal->c_str() && *e == '\0' && std::isfinite(idx);
      }
      if (idxOk) {
        const long long obs = static_cast<long long>(std::trunc(idx));
        const bool isStr = ds->dtype(name) == "factor";
        std::optional<eval::PyValue> v = obsValueAsPy(*ds, name, obs - 1);
        if (isStr) {
          const std::string text = v.has_value() ? v->strVal() : std::string();
          if (text.find('"') == std::string::npos) lit = "\"" + text + "\"";
        } else if (!v.has_value()) {
          lit = " . ";
        } else {
          const double d = v->floatVal();
          if (auto kind = eval::stataExtendedMissingKind(d)) {
            lit = std::string(" ") + cecon::missing_text(*kind) + " ";
          } else {
            char buf[40];
            std::snprintf(buf, sizeof(buf), "(%.17g)", d);
            lit = buf;
          }
        }
      }
    }
    if (lit.has_value()) {
      out += *lit;
      any = true;
    } else {
      out.append(in, i, close + 1 - i);
    }
    i = close + 1;
  }
  if (!any) return std::nullopt;
  return out;
}

BoolEvalResult evalBoolExpr(const std::string& expr, SessionState& session) {
  dprof::EScope _z(dprof::kEBoolExpr);
  BoolEvalResult r;
  std::string cleaned = trim(expr);
  if (cleaned.empty()) {
    r.isError = true;
    r.error = errBlock("if/while: empty condition");
    return r;
  }

  // EXPRLAYER-1a. Every regex below is guarded by a find() for its own
  // mandatory literal. This is the A3 idiom this file and macros.cpp already
  // use, applied to the LAST unguarded regexes on the condition path. It is a
  // strict pre-filter in every case — a pattern cannot match text that does not
  // contain the literal it requires — so no match can be missed and no
  // substitution can be skipped.
  //
  // MEASURED, not assumed: the EXPRLAYER zone profile of S01 put 30.6 ms in
  // string-literal extraction and 33.1 ms in the operator rewrite across 15,374
  // condition evaluations, on a corpus (`while `w' < 250`, `if mod(`i',3)==0`)
  // where not one of these six patterns can match anything. The whole cost was
  // the scan that proves the absence.

  // Compound double-quotes `"..."' -> "..." when the body holds no '"'; EM5: one
  // that does stays compound (eval::normalizeCompoundQuotes, nesting-aware) and
  // is extracted whole below. The old non-greedy regex cut `"say "hi" now"' at
  // its first inner "' (w2af mt_if_cq_eq, mt_while_cq; w2d_scan's
  // `strpos(`"`macval(line)'"', ...)` over csv lines holding quotes).
  dprof::ezoneBegin(dprof::kEBoolStrlit);
  if (cleaned.find("`\"") != std::string::npos) {
    cleaned = eval::normalizeCompoundQuotes(cleaned);
  }
  // FU-875: the blanks inside ( ) and [ ] go, as Stata reads the condition, and
  // brackets that do not pair are r(132) before any name is read -- the rule of
  // the other evaluators (core/expr_blanks.h): `if abs(1 2) == 12` is true and
  // `while (`k' < 1 2)` runs to k = 12 (StataBE 19.5). Guarded like the passes
  // above: a condition without a bracket has nothing to squeeze or pair.
  if (cleaned.find_first_of("()[]") != std::string::npos) {
    cleaned = cecon::squeeze_bracketed_blanks(cleaned);
    if (const std::string u = cecon::bracket_imbalance(cleaned); !u.empty()) {
      dprof::ezoneEnd(dprof::kEBoolStrlit);
      r.isError = true;
      r.error =
          eval::errorBlock("engine_error", u, std::nullopt, std::nullopt, std::nullopt, "r(132)");
      return r;
    }
  }

  // Quote-aware operator rewrite: extract string literals first.
  // colnumb(M, "name") / colsof(M) / rowsof(M) in a condition: the same text
  // substitution the display/local layer runs (eval::substituteMatrixRefs),
  // before the literals are swapped out -- colnumb reads its name from one.
  // `if !missing(colnumb(CB, "`col'")) {` stopped the do-file (w2x_fn_string_math
  // fx_colnumb_if). colnumb() of a matrix that does not exist is r(111).
  // A matrix element NAME[i,j] / el(NAME,i,j) is the same substitution: the
  // condition `if MN[1,1]^2 > 3 {` was rc 1 (w2ac_expr_valueref ifcmd_vr_matrix,
  // StataBE takes the branch); valueRefText keeps a negative cell one operand.
  {
    const std::string lower = strutil::lower(cleaned);
    if (lower.find("colnumb") != std::string::npos || lower.find("colsof") != std::string::npos ||
        lower.find("rowsof") != std::string::npos || lower.find('[') != std::string::npos ||
        lower.find("el(") != std::string::npos) {
      // Unicode names: identifier-class boundary, not ASCII `\b` (an Arabic
      // byte is a non-word character to `\b`, so `مcolnumb(` would match).
      static const std::regex reColnumbMat(
          R"((?:^|[^\w\x80-\xff])colnumb\s*\(\s*([A-Za-z_\x80-\xff][\w\x80-\xff]*)\s*,)",
          std::regex::icase);
      for (std::sregex_iterator it(cleaned.begin(), cleaned.end(), reColnumbMat), end; it != end;
           ++it) {
        if (session.eval.findMatrix((*it)[1].str()) == nullptr) {
          r.isError = true;
          r.error = eval::errorBlock("engine_error", "matrix " + (*it)[1].str() + " not found",
                                     std::nullopt, std::nullopt, std::nullopt, "r(111)");
          return r;
        }
      }
      cleaned = eval::substituteMatrixRefs(cleaned, session.eval);
    }
  }
  if (auto subbed = substituteVarSubscripts(cleaned, session)) cleaned = *subbed;

  std::vector<std::string> placeholders;
  static const std::regex reStrLit(R"("[^"]*")");
  std::string pyExpr;
  if (cleaned.find("`\"") != std::string::npos) {
    // EM5: one left-to-right scan; a compound span (nesting) and a plain literal
    // each become one placeholder, restored verbatim for pyeval's lexer.
    pyExpr.reserve(cleaned.size());
    size_t i = 0;
    while (i < cleaned.size()) {
      size_t end = std::string::npos;
      if (eval::startsCompound(cleaned, i)) {
        end = eval::compoundSpanEnd(cleaned, i);
      } else if (cleaned[i] == '"') {
        const size_t close = cleaned.find('"', i + 1);
        if (close != std::string::npos) end = close + 1;
      }
      if (end == std::string::npos) {
        pyExpr += cleaned[i++];
        continue;
      }
      placeholders.push_back(cleaned.substr(i, end - i));
      pyExpr += "__STR" + std::to_string(placeholders.size() - 1) + "__";
      i = end;
    }
  } else {
    pyExpr = cleaned.find('"') != std::string::npos
                 ? regexReplace(reStrLit, cleaned,
                                [&](const std::smatch& m) {
                                  placeholders.push_back(m.str(0));
                                  return "__STR" + std::to_string(placeholders.size() - 1) + "__";
                                })
                 : cleaned;
  }

  dprof::ezoneEnd(dprof::kEBoolStrlit);
  dprof::ezoneBegin(dprof::kEBoolRewrite);

  // Stata -> Python boolean/comparison operators (replaceLoneEquals/
  // replaceStataMissingLiterals below replicate
  // loops.py's lookaround-regex ordering via manual character scans — see
  // their doc comments for why (libc++'s std::regex has no lookbehind)).
  // A3-①: these four were the only NON-static std::regex constructions on any
  // hot path in the tree — every other pattern here and in macros.cpp is already
  // a function-local static. Constructing a std::regex COMPILES it, and the
  // compile dominates: `if` lines measured 136 us against 53 us for a `local`
  // line whose only structural difference is that it does not come through here
  // (interpreter micro-probe, 2026-08-06) — ~21 us per compile, paid on every
  // single condition evaluation of every loop. Same patterns, same order, same
  // results; they are simply compiled once now.
  static const std::regex reOrOr(R"(\|\|)");
  static const std::regex reAndAnd(R"(&&)");
  static const std::regex reNotEqTilde(R"(~=)");
  //
  // EXPRLAYER-1a: each pass is now guarded by the literal it needs, and every
  // guard is RECOMPUTED against the output of the pass before it — never
  // hoisted. That is not caution, it is required: the `|`/`&` passes run twice
  // (the doubled forms first), and a hoisted guard set computed on the ORIGINAL
  // text would not see what an earlier pass introduced or removed. find() is a
  // memchr-class scan, so paying it per pass costs
  // orders of magnitude less than the regex it guards. replaceLoneChar and
  // replaceLoneEquals are full character copies, so skipping one removes an
  // allocation as well as a pass.
  if (pyExpr.find('|') != std::string::npos) {
    pyExpr = std::regex_replace(pyExpr, reOrOr, " or ");
  }
  if (pyExpr.find('&') != std::string::npos) {
    pyExpr = std::regex_replace(pyExpr, reAndAnd, " and ");
  }
  // A lone `|` / `&` stays as written: pyeval reads it as Stata's operator
  // (Tok::Pipe/Tok::Amp), which evaluates every operand -- `if 1 | runiform() > 2`
  // consumes a draw -- and refuses a string (`if 1 | "a"` is r(109)); Python's
  // `or`/`and` skipped the right operand and let the string through (StataBE
  // 19.5).
  if (pyExpr.find('~') != std::string::npos) {
    pyExpr = std::regex_replace(pyExpr, reNotEqTilde, "!=");
  }
  // FU-811: a bare `!` stays as written. pyeval lexes it as Stata's `!`
  // (Tok::Bang) at negation level; the old rewrite to Python's loose `not` made
  // `if !3==2 {` run its branch (oracle w2ac_expr_precedence ifcmd_not_eq: it
  // does not). `^` becomes `**` the way the display/local layer does it
  // (macros.cpp), after the string literals are placeholders; before this, every
  // `if` condition with a `^` was rc 1 (ifcmd_neg_pow, ifcmd_chain_const, ...).
  if (pyExpr.find('^') != std::string::npos) {
    std::string tmp;
    tmp.reserve(pyExpr.size() + 8);
    for (char c : pyExpr) {
      if (c == '^') {
        tmp += "**";
      } else {
        tmp.push_back(c);
      }
    }
    pyExpr = tmp;
  }
  if (pyExpr.find('=') != std::string::npos) {
    pyExpr = replaceLoneEquals(pyExpr);
  }

  dprof::ezoneEnd(dprof::kEBoolRewrite);
  dprof::ezoneBegin(dprof::kEBoolTail);

  // Est-scalar refs (_b[]/_se[]/scalar()/e()) — best-effort, mirrors the
  // oracle's `try: ... except Exception: pass`.
  try {
    std::string bErr;
    pyExpr = eval::resolveEstScalarRefs(pyExpr, session.eval, &bErr);
    if (!bErr.empty()) {
      // BUG-456: `if _b[nosuch] == 0 {` is r(111) in Stata and aborts the
      // do-file even under `capture noisily {` (refute3 V9) -- an honest error
      // here, never a `.` that quietly takes the else branch.
      r.isError = true;
      // BUG-456 follow-up: r(109) when the resolver's number reached a
      // string-typed slot (`if strlen(e(nosuch))==7 {`).
      const std::string code = eval::splitErrCode(&bErr);
      r.error = eval::errorBlock("engine_error", bErr, std::nullopt, std::nullopt, std::nullopt,
                                 code);
      dprof::ezoneEnd(dprof::kEBoolTail);
      return r;
    }
  } catch (...) {
  }
  // BUG-456: an e-STRING arrives from the resolver as a double-quoted literal
  // (`"regress"`), AFTER the placeholder extraction above and BEFORE
  // replaceStataMissingLiterals below -- so an e(cmdline)/e(depvar) text with a `.` in it
  // (`d.x`, `a.b`) would have been sentinelized (the BUG-443 class). Extract
  // the new literals into the same placeholder list; the restore loop below
  // puts them back untouched, and the identifier scan never sees their text.
  if (pyExpr.find('"') != std::string::npos) {
    pyExpr = regexReplace(reStrLit, pyExpr, [&](const std::smatch& m) {
      placeholders.push_back(m.str(0));
      return "__STR" + std::to_string(placeholders.size() - 1) + "__";
    });
  }

  // BS-091/BS-101 (BETA A4): c(name) refs resolve to a literal — the same
  // shared registry gen/replace/scalar/local/display use (substituteCRefs,
  // cclass.h), so `if c(N) > 500 { ... }` / `while ... c(level) ...` work.
  // Safe here (after the string-literal placeholder extraction above): any
  // literal "c(...)" text INSIDE a quoted string is already a __STRn__
  // placeholder token by this point, so only a real unquoted c(name)
  // reference can match.
  pyExpr = substituteCRefs(pyExpr, session);
  // BUG-1330: a c() nothing answers is StataBE's r(133) `c(name) undefined`, or
  // the named refusal of a real name this build does not serve, as display and
  // assert word it; `if c(bogus) == 1 {` failed at rc 1 with no message.
  std::string cRc;
  if (auto cErr = unservedCRefError(pyExpr, &cRc)) {
    r.isError = true;
    r.error = eval::errorBlock("engine_error", *cErr, std::nullopt, std::nullopt, std::nullopt,
                               "r(" + cRc + ")");
    dprof::ezoneEnd(dprof::kEBoolTail);
    return r;
  }

  // Stata missing: scalar(NAME) unresolved, a bare `.` and `.a`..`.z` -> their
  // exact doubles (exprfns.h; `.` is 2^1023) so `missing <= 0` is False, `.a > .`
  // is True and eval never NameErrors (Stata's missing = +infinity ordering).
  // EXPRLAYER-1a: same strict pre-filter argument. The pattern needs the
  // literal "scalar(" and replaceStataMissingLiterals needs a '.'; both were
  // unconditional, and the rewrite allocates and copies the whole string even
  // when it changes nothing.
  // Unicode names: the boundary is the identifier class (group 1 keeps the
  // character before), as in macros.cpp's substituteScalarCalls scan -- under
  // ASCII `\b` the tail of `مscalar(y)` was rewritten.
  static const std::regex reScalarUnresolved(
      R"((^|[^\w\x80-\xff])scalar\(\s*[A-Za-z_\x80-\xff][\w\x80-\xff]*\s*\))");
  if (pyExpr.find("scalar(") != std::string::npos) {
    // A callback, not a "$1..." format: the literal starts with a digit, and
    // "$18.98..." would read as group 18.
    static const std::string kMissing = eval::stataMissingLiteral(0);
    pyExpr = regexReplace(reScalarUnresolved, pyExpr,
                          [](const std::smatch& m) { return m.str(1) + kMissing; });
  }
  if (pyExpr.find('.') != std::string::npos) {
    pyExpr = eval::replaceStataMissingLiterals(pyExpr);
  }

  // C4-L3 (D2a value plane): the referenced-identifier set of the FINAL
  // rewritten expression, collected while string literals are still __STRn__
  // placeholder tokens so quoted text can never contribute a name. The lazy
  // binder below binds ONLY these — outcome-identical to the historical
  // bind-everything (evaluation consults referenced names only), and what
  // lets a pure-scalar condition (`if _rc`, `if r(N)>0`, `while c<3` — the
  // corpus-dominant shapes, c4_window_attribution §3) skip
  // session.datastore() entirely: on a dirty virtual head that call WAS the
  // EMH window-closer, snapshotting the whole frame for a condition that
  // reads no data. A token immediately preceded by a digit or '.' is a
  // number's exponent tail (1e+307), not a name — skipping it can only
  // under-collect inside already-invalid expressions, where evaluation fails
  // identically in both arms.
  dprof::ezoneEnd(dprof::kEBoolTail);
  std::unordered_set<std::string> exprRefs;
  {
    dprof::EScope _zi(dprof::kEBoolIdentScan);
    // EXPRLAYER-1b: `[A-Za-z_]\w*` walked left to right, in place of a
    // sregex_iterator over the same pattern. `\w` in ECMAScript is exactly
    // [A-Za-z0-9_] (it is NOT locale-dependent, unlike Python's), and
    // sregex_iterator finds leftmost, non-overlapping matches scanning forward
    // — and the greedy `\w*` makes each of them maximal, since nothing after it
    // can force a backtrack. That is exactly this loop, character for
    // character: skip to the first identifier start, run to the end of the
    // identifier characters, resume from there. The
    // "preceded by a digit or '.'" test and the __STR prefix test are
    // transcribed unchanged. selftest cross-checks this scan against the regex
    // over a corpus of condition shapes (selftestBoolIdentScanMatchesRegex).
    const size_t n = pyExpr.size();
    for (size_t i = 0; i < n;) {
      // Unicode names (lane M): the shared byte tests; the regex twin carries
      // the same `\x80-\xff` widening.
      if (!cecon::ident::isIdentStartByte(pyExpr[i])) {
        ++i;
        continue;
      }
      size_t j = i + 1;
      while (j < n && cecon::ident::isIdentByte(pyExpr[j])) ++j;
      const bool expTail =
          i > 0 && (std::isdigit(static_cast<unsigned char>(pyExpr[i - 1])) || pyExpr[i - 1] == '.');
      if (!expTail && pyExpr.compare(i, 5, "__STR") != 0) {
        exprRefs.insert(pyExpr.substr(i, j - i));
      }
      i = j;
    }
  }

  // Restore string placeholders.
  for (size_t i = 0; i < placeholders.size(); ++i) {
    std::string key = "__STR" + std::to_string(i) + "__";
    size_t pos = pyExpr.find(key);
    if (pos != std::string::npos) pyExpr.replace(pos, key.size(), placeholders[i]);
  }

  // Build the evaluation namespace: math + Stata fns + session scalars.
  // EXPRLAYER-1c: the 39-entry math+Stata registry is a process constant and is
  // referenced, not rebuilt (39 map nodes + 39 std::function constructions per
  // condition, measured at 25.9 ms across one S01 run). Only `scalar` — whose
  // closure captures this session — and the value bindings below are per-call,
  // and they go into the OWN maps, which outrank the base exactly as a later
  // insertion into the single merged map used to.
  eval::PyNamespace ns;
  {
    dprof::EScope _zn(dprof::kEBoolNs);
    ns.base = &eval::stataMathNamespace();
    eval::addScalarFn(ns, [&session](const std::string& name) -> std::optional<std::string> {
      const std::string* v = session.eval.scalars.find(name);
      return v ? std::optional<std::string>(*v) : std::nullopt;
    });
  }
  dprof::ezoneBegin(dprof::kEBoolBind);

  // _rc: session._locals["_rc"], default "0" if unset/blank/".".
  {
    dprof::EScope _zr(dprof::kEBoolBindRc);
    const std::string* rcRaw = session.eval.locals.find("_rc");
    double rcVal = 0.0;
    if (rcRaw && !trim(*rcRaw).empty() && trim(*rcRaw) != ".") {
      try {
        rcVal = std::stod(*rcRaw);
      } catch (...) {
        rcVal = 0.0;
      }
    }
    ns.consts["_rc"] = eval::PyValue::floating(rcVal);
  }

  // _N / _n + dataset column first-obs values (Stata's `while c<3` reads
  // row 1 at program-level scope).
  //
  // C4-L3 (D2a, cobra unchanged): LAZY arm first. When the resident card is
  // provably current (the same honest-currency rule every D1 consumer uses),
  // the card answers _N and column name/dtype membership without opening the
  // datastore; session.datastore() — on a dirty virtual head, a full EMH
  // frame snapshot — is paid ONLY when the condition really reads a column's
  // row-1 value (any referenced column; since BUG-921 a column outranks a
  // same-named scalar and a string column binds its text). Anything less than a current card — stale/absent,
  // no dataset, or the PIET_NO_RESIDENT_READ escape arm via
  // residentReadCard()'s gate — falls to the pre-D2 eager path VERBATIM
  // below, including its bind-every-f64 behavior and its
  // materialize-on-dirty side effect (the floor RED arm's reproduction).
  dprof::ezoneBegin(dprof::kEBoolBindCard);
  // BUG-921: column values are collected here and bound AFTER the scalars, so a
  // variable outranks a same-named scalar (Stata's rule, and dispatcher.cpp's
  // for row predicates). Oracle w2_prog_controlflow pc_if_varref: with
  // `scalar i = 999` and variable i (i[1] = 100), `if i > 250` is false and
  // `if i == 999` is false. A string variable binds its row-1 text
  // (`if s == "s 1"`), which used to be unbound and stopped the do-file (rc 1).
  std::unordered_map<std::string, eval::PyValue> colBound;
  bool boundLazily = false;
  if (const std::vector<VarInfo>* card = session.residentReadCard()) {
    std::optional<int64_t> cardRows = session.residentRowsIfCurrent();
    if (cardRows.has_value()) {
      if (exprRefs.count("_N") > 0) {
        ns.consts["_N"] = eval::PyValue::floating(static_cast<double>(*cardRows));
      }
      if (exprRefs.count("_n") > 0) ns.consts["_n"] = eval::PyValue::floating(1.0);
      std::vector<std::string> needValue;
      if (*cardRows > 0) {
        for (const auto& tok : exprRefs) {
          for (const auto& vi : *card) {
            if (vi.name == tok) {
              needValue.push_back(tok);  // BUG-921: string variables too
              break;  // a current card is authoritative for absence too
            }
          }
        }
      }
      if (!needValue.empty()) {
        // Honest flush: the condition genuinely reads data. Same cost and
        // semantics as the eager arm for exactly these columns.
        Datastore* dsv = session.datastore();
        if (dsv != nullptr) {
          for (const auto& tok : needValue) {
            if (!dsv->has(tok)) continue;
            if (auto v = obsValueAsPy(*dsv, tok, 0)) colBound[tok] = *v;
          }
        }
      }
      boundLazily = true;
    }
  }
  if (!boundLazily) {
    Datastore* ds = session.datastore();
    if (ds != nullptr) {
      // EXPRLAYER-4: bind only the names the expression REFERENCES, exactly as
      // the lazy arm above already does. The eager arm bound _N, _n and every
      // f64 column of the dataset — 25 dtype() calls, 25 col() lookups and ~25
      // std::map inserts on the P2 fixture — for conditions like `while `w' <
      // 250` that reference none of them. Measured at 33.4 ms of S01's 50.7 ms
      // of condition time, the largest remaining item in the zone table.
      //
      // OUTCOME-IDENTICAL, on the argument this file already ratified for the
      // lazy arm (C4-L3): evaluation consults REFERENCED names only, and
      // exprRefs is collected from the final rewritten expression, so a name
      // that is not in it cannot be looked up. The DATASTORE CALL IS NOT MOVED
      // — session.datastore() still runs first and unconditionally, so the
      // materialize-on-dirty side effect this arm is deliberately preserved to
      // reproduce (the floor RED arm) happens exactly as before. Only the
      // binding loop narrowed. (BUG-921: the columns now bind after the
      // scalars, so a same-named variable wins.)
      if (exprRefs.count("_N") > 0) {
        ns.consts["_N"] = eval::PyValue::floating(static_cast<double>(ds->nRows()));
      }
      if (exprRefs.count("_n") > 0) ns.consts["_n"] = eval::PyValue::floating(1.0);
      if (ds->nRows() > 0) {
        for (const auto& tok : exprRefs) {
          if (!ds->has(tok)) continue;  // col()/dtype() throw on an absent name
          if (auto v = obsValueAsPy(*ds, tok, 0)) colBound[tok] = *v;  // BUG-921: strings too
        }
      }
    } else {
      ns.consts.emplace("_N", eval::PyValue::floating(0.0));
      ns.consts.emplace("_n", eval::PyValue::floating(1.0));
    }
  }

  dprof::ezoneEnd(dprof::kEBoolBindCard);

  // Named scalars (session._scalars), then the dataset columns collected above
  // on top of them: a variable wins over a same-named scalar (BUG-921; the old
  // order, inherited from loops.py, let the scalar win).
  dprof::ezoneBegin(dprof::kEBoolBindScalars);
  for (const auto& [sname, sval] : session.eval.scalars.items()) {
    auto f = eval::pythonFloat(sval);
    if (f) ns.consts[sname] = eval::PyValue::floating(*f);
  }
  for (auto& [cname, cval] : colBound) ns.consts[cname] = std::move(cval);
  dprof::ezoneEnd(dprof::kEBoolBindScalars);

  dprof::ezoneEnd(dprof::kEBoolBind);

  // `if 1/0 == . {`, `if invttail(5,2) == . {` and `while sqrt(-1) == .` are
  // Stata conditions like any other: no error, and a NaN is `.` (see
  // StataDivisionScope; BUG-1358, BUG-1065).
  eval::StataDivisionScope stataSemantics;
  auto outcome = eval::evalPyExprDetailed(pyExpr, ns);
  if (outcome.status != eval::PyEvalOutcome::Status::Value) {
    r.isError = true;
    // BUG-456: a function announcing Stata's EXACT code (`r(109): type
    // mismatch` from pyEquals on `if e(cmd) == 1 {`) is carried as that code,
    // the way `local =` and display already carry it (BUG-246/249).
    if (outcome.status == eval::PyEvalOutcome::Status::Raised &&
        outcome.error.rfind("r(", 0) == 0 && outcome.error.find(')') != std::string::npos) {
      const std::string rc = outcome.error.substr(0, outcome.error.find(')') + 1);
      const size_t colon = outcome.error.find(": ");
      r.error = eval::errorBlock(
          "engine_error",
          (colon != std::string::npos ? outcome.error.substr(colon + 2) : outcome.error) +
              " (expr=" + cleaned + ")",
          std::nullopt, std::nullopt, std::nullopt, rc);
      return r;
    }
    r.error = errBlock("if/while condition evaluation failed (expr=" + cleaned + ")");
    return r;
  }
  r.value = outcome.value.truthy();
  return r;
}

// ---------------------------------------------------------------------------
// Varlist wildcard expansion (loops.py's _expand_varlist)

// The implementation stays here in the anonymous namespace; the PUBLIC entry point
// (declared in piet/loops.h since BUG-371, so `ds` and `unab` can ask the same
// question `foreach ... of varlist` asks) is the one-line forwarder below, after
// this namespace closes.
// Stata's varlist wildcards: `*` matches any run (including empty), `?` matches
// exactly one character. Iterative backtracking rather than a regex, because a
// name is short and building a std::regex per token per call is what the zone
// profile has already caught twice in this file.
// Unicode names (lane M): forwards to the shared matcher, whose `?` is ONE
// CHARACTER (StataBE: `ds ق?` lists the two-letter Arabic names) -- this copy's
// `?` was one byte, so it matched half an Arabic letter.
bool globMatch(const std::string& pat, const std::string& name) {
  return strutil::globMatch(pat, name);
}

VarlistExpandResult expandVarlistImpl(const std::string& spec, SessionState& session) {
  VarlistExpandResult r;
  // C4-L2B (D1 card plane, brief §4): wildcard/range expansion needs only the
  // ORDERED NAME LIST, which the CURRENT resident card carries (seeded in
  // column order by every writer, the same order ds->names() reports). Reading
  // it through session.datastore() here forced, on a dirty virtual head, the
  // full EMH frame snapshot — for a `foreach ... of varlist` header that never
  // looks at a single value. Honest fallback: a stale/absent card (or the
  // PIET_NO_RESIDENT_READ escape arm, via residentReadCard's gate) drops to
  // the pre-D1 datastore path verbatim, including its no-dataset token
  // passthrough branch (a current card implies a loaded dataset, so the card
  // plane can never shadow that branch).
  std::vector<std::string> allVars;
  if (const std::vector<VarInfo>* card = session.residentReadCard()) {
    allVars.reserve(card->size());
    for (const auto& v : *card) allVars.push_back(v.name);
  } else {
    Datastore* ds = session.datastore();
    if (ds == nullptr) {
      std::istringstream iss(spec);
      std::string tok;
      while (iss >> tok) r.tokens.push_back(tok);
      return r;
    }
    allVars = ds->names();
  }
  // AER scope 2, K1b: the range operator may carry whitespace on either side.
  // Measured on StataNow 19.5 BE (`unab a : ...` over auto.dta): `price- trunk`,
  // `price -trunk` and `price - trunk` all give the same five names as
  // `price-trunk`. The corpus spells it with the trailing space -- paper 082's
  // `tolower AL1- KONEV_L` -- and without this the two halves arrive as two
  // separate tokens and neither resolves. Collapsing is safe HERE because a
  // variable name cannot contain a hyphen (strutil::isValidStataIdentifier), so
  // in a varlist a `-` is always the range operator and never part of a name.
  std::string joined;
  joined.reserve(spec.size());
  for (size_t i = 0; i < spec.size(); ++i) {
    if (spec[i] == '-') {
      while (!joined.empty() && std::isspace(static_cast<unsigned char>(joined.back())))
        joined.pop_back();
      joined.push_back('-');
      while (i + 1 < spec.size() && std::isspace(static_cast<unsigned char>(spec[i + 1]))) ++i;
      continue;
    }
    joined.push_back(spec[i]);
  }

  std::istringstream iss(joined);
  std::string tok;
  while (iss >> tok) {
    // `_all` is the varlist keyword for every variable, and it was reaching the
    // literal-name branch below, where it answered `variable _all not found`.
    // Measured: `unab a : _all` on auto.dta is rc 0 and returns all twelve
    // names in column order. Case-insensitive because Stata's own varlist
    // keywords are.
    if (tok.size() == 4 && (tok[0] == '_') &&
        std::tolower(static_cast<unsigned char>(tok[1])) == 'a' &&
        std::tolower(static_cast<unsigned char>(tok[2])) == 'l' &&
        std::tolower(static_cast<unsigned char>(tok[3])) == 'l') {
      for (const auto& v : allVars) r.tokens.push_back(v);
      continue;
    }
    // AER scope 2, K1b: `*` matches anywhere and any number of times, and `?`
    // matches exactly one character. This used to be a TRAILING `*` only, i.e.
    // a prefix match, which silently refused three of the four shapes Stata
    // accepts. Measured on StataNow 19.5 BE against auto.dta, `unab`:
    //   t*     -> trunk turn        (this is the one the old code did)
    //   *e     -> make price
    //   tr*k   -> trunk
    //   ?rice  -> price
    //   t*n    -> turn
    //   zz*    -> r(111), no match
    //   ~e     -> r(111): `~` is NOT a wildcard here, so it stays unmatched
    // The corpus needs the others: `renvars *2010, subst(...)` is a real line
    // in it, and 948 more `drop`/`keep` lines carry a wildcard (those reach a
    // different, unwired path -- BUG-450).
    if (tok.find('*') != std::string::npos || tok.find('?') != std::string::npos) {
      bool any = false;
      for (const auto& v : allVars) {
        if (globMatch(tok, v)) {
          r.tokens.push_back(v);
          any = true;
        }
      }
      if (!any) {
        r.isError = true;
        // r(111) is what Stata answers for a pattern that matches nothing, and
        // `reason` is the only channel that carries an exact rc.
        r.error = eval::errorBlock("engine_error", "variable " + tok + " not found", std::nullopt,
                                   std::nullopt, std::nullopt, "r(111)");
        return r;
      }
      continue;
    }
    static const std::regex reRange(R"(^[\w\x80-\xff]+-[\w\x80-\xff]+$)");
    if (std::regex_match(tok, reRange)) {
      size_t dash = tok.find('-');
      std::string v1 = tok.substr(0, dash), v2 = tok.substr(dash + 1);
      auto it1 = std::find(allVars.begin(), allVars.end(), v1);
      auto it2 = std::find(allVars.begin(), allVars.end(), v2);
      if (it1 == allVars.end() || it2 == allVars.end()) {
        r.isError = true;
        r.error = errBlock("varlist expansion: '" + v1 + "' or '" + v2 + "' not found");
        return r;
      }
      auto i1 = std::distance(allVars.begin(), it1);
      auto i2 = std::distance(allVars.begin(), it2);
      for (auto k = i1; k <= i2; ++k) r.tokens.push_back(allVars[static_cast<size_t>(k)]);
      continue;
    }
    r.tokens.push_back(tok);
  }
  return r;
}

// BUG-509 (corrected)/BUG-544: cheap existence guard for `foreach ... of
// varlist ...`, mirroring dispatcher.cpp's `unab` arm's known-names check
// (dispatcher.cpp, incl. its no-card fallback). expandVarlistImpl above only
// validates WILDCARD/RANGE spellings (see its own comment): a bare unknown
// literal token passes straight through by design (9 other callers rely on
// that passthrough), so a caller that needs Stata's r(111) "variable not
// found" ahead of any iteration checks it here instead. Oracle:
// tests/eval/golden/aer_w2/w2_openbugs.log arms fe_unknown/fe_mixed/
// fe_prog_unknown — the check runs over the WHOLE expanded token list before
// runForeach ever sets the loop variable once.
std::optional<ojson> checkKnownVarTokens(std::vector<std::string>& tokens,
                                         SessionState& session) {
  std::vector<std::string> known;
  if (const std::vector<VarInfo>* card = session.residentReadCard()) {
    for (const auto& vi : *card) known.push_back(vi.name);
  } else if (Datastore* ds = session.datastore()) {
    known = ds->names();
  } else {
    return ojson(eval::noDataLoaded());
  }
  // A unique prefix stands for its variable, as in every Stata varlist
  // (w2_openbugs.log:190-194 fe_abbrev: `foreach v of varlist alph` iterates
  // `alpha`, rc 0); several matches are r(111) "ambiguous abbreviation".
  for (auto& t : tokens) {
    if (std::find(known.begin(), known.end(), t) != known.end()) continue;
    std::vector<std::string> hits;
    for (const auto& k : known)
      if (k.size() > t.size() && k.compare(0, t.size(), t) == 0) hits.push_back(k);
    if (hits.size() == 1) {
      t = hits.front();
      continue;
    }
    if (hits.size() > 1)
      return ojson(eval::engineError(t + " ambiguous abbreviation", "unknown_variable"));
    return ojson(eval::engineError("variable " + t + " not found", "unknown_variable"));
  }
  return std::nullopt;
}

// ---------------------------------------------------------------------------
// Body runner — pipes body_lines through doexec::executeDoSource (handles
// nested blocks via the SAME brace-aggregation entry point).

struct BodyRunResult {
  ojson blocks = ojson::array();
  bool hasError = false;
  // BUG-154: set iff the halt was a CLEAN `exit [N]` (mirrors
  // doexec::ExecuteResult::stoppedAtExitCode) — runCaptureBlock uses it to
  // set the exact Stata _rc (capture { exit 5 } → _rc 5, verified against
  // real Stata 19.5) instead of the coarse "1".
  std::optional<int> exitCode;
  std::optional<std::string> control;  // "continue" | "break"
};

// BUG-483: how many LOOP bodies deep the current dispatch is. The three loop
// runners below open a scope; `if`/`capture`/`quietly`/`parallel` deliberately
// do not (see loops.h). The accessor and the scope's members are defined after
// this anonymous namespace closes, because they are declared in piet::loops.
thread_local int g_loopBodyDepth = 0;

BodyRunResult runBody(const std::vector<std::string>& bodyLines, SessionState& session,
                       const DispatchFn& dispatchFn) {
  std::string source;
  for (size_t i = 0; i < bodyLines.size(); ++i) {
    if (i) source += "\n";
    source += bodyLines[i];
  }
  doexec::ExecuteResult result = doexec::executeDoSource(source, session, dispatchFn);
  BodyRunResult out;
  out.blocks = result.blocks;
  // F3: a body aborts its enclosing loop ONLY when execution actually HALTED on
  // an uncaught error (or a cancel) — precisely the condition executeDoSource
  // records in stoppedAtLine. An error consumed by `capture` (BS-127: the
  // dispatcher tags it captured:true; it is display-only, sets _rc, and does
  // NOT halt) leaves stoppedAtLine unset, so the loop must keep iterating. The
  // old "any type==error block present" scan wrongly treated a captured error
  // as fatal, aborting foreach/forvalues/while (and nested loops) after the
  // first captured body error. Keying off stoppedAtLine mirrors doexec.cpp's
  // own top-level halt rule (type==error && !captured) exactly, so runBody can
  // never disagree with it; an UNcaptured error still sets stoppedAtLine and
  // still aborts the loop (Stata semantics for uncaught errors preserved).
  out.hasError = result.stoppedAtLine.has_value();
  // BUG-154: a CLEAN `exit [N]` halt inside this body is invisible to the
  // caller's caller. The halt rides the error channel as a sentinel block
  // ("exit"/"exit N"), and the INNER executeDoSource above both consumed and
  // ERASED it (doexec.cpp handleUncapturedError removes the sentinel so a
  // clean exit never paints a red card). An ordinary uncaptured error stays
  // in out.blocks and re-halts the outer executeDoSource when this body's
  // blocks bubble up through runIf/runForeach/runWhile — but the erased exit
  // sentinel left NOTHING to re-halt on, so `if (cond) { exit 7 }` fell
  // through and the do-file kept running (guards were dead). Re-emit the
  // sentinel here: the outer handleUncapturedError recognizes the message
  // shape, halts with the SAME code, and erases it again — cascading through
  // any nesting depth. `capture { ... exit N }` is unaffected: its body runs
  // under the capture arm, which traps the error channel and sets _rc=N by
  // design (the sentinel we append here is exactly what capture expects to
  // trap).
  if (result.stoppedAtLine.has_value() && result.stoppedAtExitCode.has_value()) {
    int code = *result.stoppedAtExitCode;
    out.exitCode = code;
    ojson halt;
    halt["type"] = "error";
    halt["message"] = "exit" + (code != 0 ? (" " + std::to_string(code)) : std::string());
    out.blocks.push_back(halt);
  }
  if (result.control.has_value()) out.control = result.control;
  return out;
}

// ---------------------------------------------------------------------------
// Runners

ojson runForeach(const std::string& varname, const std::vector<std::string>& tokens,
                  const std::vector<std::string>& bodyLines, SessionState& session,
                  const DispatchFn& dispatchFn) {
  LoopBodyScope __loopBody;  // BUG-483: `continue` asks how deep we are
  ojson allBlocks = ojson::array();
  bool truncated = false;   // P7: allBlocks storage cap (see kMaxLoopBlocks)
  for (const auto& tok : tokens) {
    session.eval.locals.set(varname, tok);
    BodyRunResult result = runBody(bodyLines, session, dispatchFn);
    for (auto& b : result.blocks) pushLoopBlock(allBlocks, truncated, b);
    if (result.hasError) break;
    if (result.control == "break") break;
    // control == "continue": fall through to next token.
  }
  // BUG-920: StataBE 19.5 leaves the loop macro EMPTY once the loop is over --
  // not the last element, not the value it held before (oracle
  // w2_prog_controlflow pc_fe_after: `local m "before"` then
  // `foreach m in p1 p2 p3` leaves `m' as []). On EVERY exit: after
  // `continue, break`, an error, zero iterations and inside an outer loop alike
  // (w2ad_prog_preserve_nest pn_lm_fe_break, pn_lm_fe_err, pn_lm_fe_zero,
  // pn_lm_nested_done, pn_lm_nested_break: all []).
  session.eval.locals.erase(varname);
  return allBlocks;
}

// FULL FUNCTIONAL CHECK (2026-08-05): `forvalues` iterates its range LAZILY.
//
// It used to materialise every value into a vector<string> first
// (genForvaluesTokens) and hand that to runForeach, which is why a 10,000-
// iteration cap existed at all: the cap was a MEMORY guard on that vector, not
// a language limit. Stata has no such limit, so a legal do-file with a longer
// loop was refused — and simply raising the number would have moved the failure
// from an honest reject to an out-of-memory one. Generating each value in place
// removes the reason for the cap instead of arguing with it.
//
// The values accumulate `v += step` (drift and all), as Stata's do. BUG-920:
// StataBE 19.5 tests the accumulated value against the end with NO tolerance
// and writes it as macro text by the local/global rule (%18.0g, no leading
// 0). Oracle w2_prog_controlflow: pc_fv_frac (0(0.1)1 assigns
// 0 .1 .2 .3 ... .7999999999999999 .8999999999999999 .9999999999999999, 11
// values) and pc_fv_frac_small (0(0.01)0.31 runs 31 times and ends at
// .3000000000000001: the 32nd accumulated value 0.3100000000000001 is past
// 0.31, which the old 1e-9 tolerance let through). The old rendering was
// Python's repr (0.30000000000000004). A `foreach`-style enumerated list
// still goes through runForeach; only the numeric range is lazy.
ojson runForvaluesRange(const std::string& varname, double start, double step, double end,
                        const std::vector<std::string>& bodyLines, SessionState& session,
                        const DispatchFn& dispatchFn) {
  LoopBodyScope __loopBody;  // BUG-483: `continue` asks how deep we are
  ojson allBlocks = ojson::array();
  bool truncated = false;   // P7: allBlocks storage cap (see kMaxLoopBlocks)
  for (double v = start; (step > 0 && v <= end) || (step < 0 && v >= end); v += step) {
    session.eval.locals.set(varname, eval::stataMacroRealText(v));
    BodyRunResult result = runBody(bodyLines, session, dispatchFn);
    for (auto& b : result.blocks) pushLoopBlock(allBlocks, truncated, b);
    if (result.hasError) break;
    if (result.control == "break") break;
    // control == "continue": fall through to the next value.
  }
  // BUG-920: the loop macro is empty after the loop (oracle pc_fv_ab: `i' is
  // [] after `forvalues i = 1/5`, though it held "before" on entry), on every
  // exit (w2ad pn_lm_fv_break, pn_lm_fv_err, pn_lm_fv_zero: []).
  session.eval.locals.erase(varname);
  return allBlocks;
}

ojson runWhile(const std::string& condition, const std::vector<std::string>& bodyLines,
               SessionState& session, const DispatchFn& dispatchFn) {
  LoopBodyScope __loopBody;  // BUG-483: `continue` asks how deep we are
  ojson allBlocks = ojson::array();
  bool truncated = false;   // P7: allBlocks storage cap (see kMaxLoopBlocks)
  int nIters = 0;
  while (true) {
    std::string condExpanded = condition;
    if (eval::hasMacroRefs(condition)) {
      // unsetRAsMissing: a bare/guarded r() that isn't set evaluates to Stata
      // missing (`.`) in a while condition, not an error (FIX-RESTIMATE).
      eval::ExpandResult exp = eval::expandMacros(condition, session.eval,
                                                  /*unsetRAsMissing=*/true);
      if (exp.isError) {
        allBlocks.push_back(exp.error);
        return allBlocks;
      }
      condExpanded = exp.line;
    }
    BoolEvalResult truth = evalBoolExpr(condExpanded, session);
    if (truth.isError) {
      allBlocks.push_back(truth.error);
      return allBlocks;
    }
    if (!truth.value) break;

    if (++nIters > kWhileCap) {
      allBlocks.push_back(errBlock(
          "while: exceeded " + std::to_string(kWhileCap) +
          " iterations — possible runaway loop. Check your condition to ensure it "
          "eventually becomes false."));
      return allBlocks;
    }

    BodyRunResult result = runBody(bodyLines, session, dispatchFn);
    for (auto& b : result.blocks) pushLoopBlock(allBlocks, truncated, b);
    if (result.hasError) break;
    if (result.control == "break") break;
    // control == "continue": loop back to condition re-eval.
  }
  return allBlocks;
}

ojson runIf(const std::string& condition, const std::vector<std::string>& bodyLines,
            const std::vector<std::string>& elseBodyLines, SessionState& session,
            const DispatchFn& dispatchFn) {
  std::string condExpanded = condition;
  if (eval::hasMacroRefs(condition)) {
    // unsetRAsMissing: `if !_rc & !missing(r(estimate))` after a failed
    // capture must NOT error on the unset r() — Stata evaluates it as missing
    // (`.`). Only the control-flow condition path opts in (FIX-RESTIMATE).
    eval::ExpandResult exp = eval::expandMacros(condition, session.eval,
                                                /*unsetRAsMissing=*/true);
    if (exp.isError) return ojson::array({exp.error});
    condExpanded = exp.line;
  }
  BoolEvalResult truth = evalBoolExpr(condExpanded, session);
  if (truth.isError) return ojson::array({truth.error});

  BodyRunResult result = truth.value ? runBody(bodyLines, session, dispatchFn)
                                      : runBody(elseBodyLines, session, dispatchFn);
  ojson out = result.blocks;
  if (result.control.has_value()) {
    ojson ctrl;
    ctrl["type"] = "_control";
    ctrl["action"] = *result.control;
    out.push_back(ctrl);
  }
  return out;
}

// BS-015: `capture { line1 \n line2 \n ... }` multi-line block. Real Stata
// semantics: run the body sequentially; if a line errors, ITS error message
// is suppressed and the rest of the block does NOT run (mirrors how an
// uncaptured error normally halts execution — capture just catches this
// halt locally instead of letting it propagate out of the block), but
// lines BEFORE the failure keep whatever output they already produced
// (capture does not retroactively hide output that already happened — it
// is not `quietly`). `_rc` mirrors the existing single-line `capture cmd`
// convention (dispatcher.cpp's Pre-0 block): "1" on any error inside the
// block, "0" otherwise — the same coarse boolean this codebase uses
// everywhere today (an exact numeric _rc taxonomy is BS-100/A4, a later
// phase, not this fix). Reuses runBody (the SAME helper runForeach/runWhile/
// runIf already use) so nested blocks/control-flow inside the capture body
// work exactly like any other brace body, for free.
//
// BUG-1434: `showError` is a noisily after the capture (`capture noisily { …
// }`, `capture quietly noisily { … }`) -- the halting error is kept and shown
// (tagged captured, so it does not halt the caller), which is what StataBE
// prints ("variable nosuchvar not found" inside the block). `quiet` is the
// block's output visibility, which the last of the chain's capture/quietly/
// noisily decides: a bare `capture { … }` and `capture noisily quietly { … }`
// drop ordinary output (keptWhenQuiet), `quietly capture noisily { … }` shows
// it.
ojson runCaptureBlock(const std::vector<std::string>& bodyLines, SessionState& session,
                      const DispatchFn& dispatchFn, bool showError, bool quiet) {
  BodyRunResult result;
  {
    std::optional<SessionState::LogVerbosityScope> lv;
    if (quiet) lv.emplace(session, true);
    result = runBody(bodyLines, session, dispatchFn);
  }
  ojson out = ojson::array();
  // BUG-116/F3: the old scan below treated ANY type=="error" block as fatal
  // to this capture block, including one already caught by a NESTED
  // `capture` (tagged captured:true by the dispatcher's capture arm, BS-127
  // — display-only, does not halt). That wrongly truncated the block's
  // output at the nested capture's line and set the OUTER _rc from an error
  // that never actually escaped to this scope. Key off result.hasError
  // instead — runBody already computes it from doexec's own halt authority
  // (stoppedAtLine), the SAME signal runForeach/runWhile/runIf rely on — so
  // a captured nested error passes through untouched and the block keeps
  // running exactly like the enclosing do-file already does around it.
  //
  // BUG-546: the halting block's own coarse rc (eval::coarseRc) is captured
  // HERE, from the same block the loop is about to suppress, rather than
  // hardcoding "1" below — a `capture noisily { summarize nosuchcol }` gave
  // _rc 1 where the single-line `capture noisily summarize nosuchcol` (which
  // already used coarseRc, dispatcher.cpp) gave the real 111. Oracle:
  // tests/eval/golden/aer_w2/w2_openbugs.log fe_unknown/fe_mixed (the SAME
  // capture-BLOCK shape wraps the foreach arms there).
  std::string haltedRc = "1";
  for (const auto& b : result.blocks) {
    const std::string t = b.value("type", "");
    if (result.hasError && t == "error" && !b.value("captured", false)) {
      haltedRc = eval::coarseRc(b);
      // `exit` with code 0 is no error, and capture does not hold it: StataBE
      // `capture noisily { … exit 0 }` ends the do-file ("end of do-file").
      if (result.exitCode == 0) {
        out.push_back(b);
        break;
      }
      if (showError) {
        ojson tagged = b;
        tagged["captured"] = true;
        out.push_back(std::move(tagged));
      }
      break;  // suppress the error itself (and anything after — the block stops here)
    }
    // BUG-1338: a failed inline `: fn'/`=exp' in the body prints nothing under
    // the capture (StataBE 19.5: `capture { display "`: type nosuch'" }`), and
    // its message under `capture noisily { }`, as the halting error's.
    if (b.value("inline_note", false) && !showError) continue;
    if (quiet && !keptWhenQuiet(b)) continue;
    out.push_back(b);
  }
  settleNoisily(out, session);
  if (result.exitCode == 0) return out;
  // BUG-154: a clean `exit N` inside the block carries its EXACT code to _rc
  // (Stata 19.5: `capture { exit 5 }` → _rc 5, block stops, file continues).
  // Every other error carries the exact coarse code (BUG-546, above).
  session.eval.locals.set("_rc", result.exitCode.has_value()
                                     ? std::to_string(*result.exitCode)
                                     : (result.hasError ? haltedRc : "0"));
  if (result.control.has_value()) {
    ojson ctrl;
    ctrl["type"] = "_control";
    ctrl["action"] = *result.control;
    out.push_back(ctrl);
  }
  return out;
}

// BUG-132: `quietly { … }` / `noisily { … }` multi-line block. Like
// `capture { … }` (runCaptureBlock above), quietly/noisily are prefix-class
// keywords that can head a brace-aggregated block — but their semantics differ
// from capture on TWO axes, so they get their own handler rather than reusing
// the capture path:
//   (1) OUTPUT visibility. `quietly` SUPPRESSES the body's normal output
//       (display/result/data_mutation cards) while still showing command
//       echoes and any error message — exactly the filter dispatcher.cpp's
//       single-line quietly branch applies (keep type=="echo"||"error";
//       dispatcher.cpp:769-773). `noisily` is the transparent case: it shows
//       everything the body produced (its purpose is to un-quiet a region
//       nested inside an enclosing quietly block; at top level it behaves like
//       a plain block).
//   (2) ERROR propagation. UNLIKE capture, neither quietly nor noisily
//       SWALLOWS an uncaught error — an error inside the block still HALTS the
//       do-file. runBody leaves the error block untagged (captured stays
//       false) and reports hasError, so passing that error block through
//       unmodified lets doexec.cpp's top-level halt rule (type=="error" &&
//       !captured) fire exactly as it does for a bare uncaught error. This
//       matches the single-line `quietly cmd` behaviour BUG-132's repro
//       confirmed correct (quietly is output-only; it never catches errors the
//       way capture does).
// No `_rc` is set here, on purpose: only `capture` owns `_rc` in this codebase,
// and real Stata's quietly/noisily are transparent to `_rc` — they neither set
// nor clear it — so leaving it untouched is the correct semantics, not an
// omission. Reuses runBody (the SAME helper runForeach/runWhile/runIf/
// runCaptureBlock use) so nested blocks/control-flow inside the body work for
// free.
ojson runQuietlyBlock(const std::vector<std::string>& bodyLines, SessionState& session,
                      const DispatchFn& dispatchFn, bool quiet) {
  // BUG-189: the filter below cannot reach `log using` — dispatch() tees each
  // body line to the log before this function ever sees the block. Declaring the
  // scope makes the tee itself aware of it.
  BodyRunResult result;
  {
    SessionState::LogVerbosityScope __lv(session, quiet);
    result = runBody(bodyLines, session, dispatchFn);
  }
  ojson out = ojson::array();
  for (const auto& b : result.blocks) {
    if (quiet) {
      // quietly: keep only command echoes + error messages (and a nested
      // noisily's output, BUG-1434); suppress the body's normal output.
      // Mirrors the single-line quietly filter.
      std::string t = b.value("type", "");
      if (t != "_control" && keptWhenQuiet(b)) out.push_back(b);
    } else {
      // noisily: fully transparent — show every block the body produced.
      out.push_back(b);
    }
  }
  if (quiet) {
    settleNoisily(out, session);
  } else {
    markNoisily(out, session);
  }
  if (result.control.has_value()) {
    ojson ctrl;
    ctrl["type"] = "_control";
    ctrl["action"] = *result.control;
    out.push_back(ctrl);
  }
  return out;
}

// PLEVEL-IMPL-WAVE arc ① — `parallel[, workers(N)] { … }`, the SKELETON leg.
// Design authority + arc boundary: include/piet/parallel.h. What lands here is
// header parsing (honest errors), the §3.4 task decomposition, the resolved
// worker budget, the §2.4 task spec, the stderr banner — and SERIAL execution
// of the decomposed tasks in this process. Arc ② replaces the execution loop
// below with spawn/splice over `--parallel-worker` children; everything around
// it (what a task is, what a child inherits, what the parent learns) is already
// what that arc will hand to them.
//
// Two deliberate properties of this leg:
//  · **Per-TASK dispatch, not one whole-body run.** Running the body as a
//    single doexec pass would be one line shorter, but it would make a failing
//    task halt the rest of the block — the opposite of §2.6's failure
//    isolation, which is a documented semantic of the construct and not merely
//    an artefact of running in parallel. Dispatching per task means the
//    isolation rule holds identically at N=1 and at N>1, which is exactly what
//    makes the N=1 leg a legitimate reference arm for G-1.
//  · **The banner goes to stderr.** stdout is the byte-pinned surface (§2.5;
//    freeze/run_eval/floor_gate are all byte-exact), so a stdout banner would
//    diff every fixture at once. Precedent: main.cpp's `--do scratch workdir`
//    line is stderr for the same reason.
ojson runParallelBlock(const std::string& header, const std::vector<std::string>& bodyLines,
                       SessionState& session, const DispatchFn& dispatchFn) {
  parallel::HeaderOptions opt = parallel::parseHeader(header);
  if (opt.isError) return ojson::array({opt.error});

  std::vector<std::string> tasks = parallel::decomposeTasks(bodyLines);
  // 2026-09-02: the derivation gets the WORK it is sizing for — this block's
  // task count, the dataset the workers will inherit, and the scratch volume.
  parallel::ResolveContext rctx;
  rctx.taskCount = static_cast<int64_t>(tasks.size());
  rctx.residentBytes = parallel::residentBytesEstimate(session);
  rctx.scratchDir = session.workdir();
  parallel::WorkerBudget budget = parallel::resolveWorkers(opt.workers, rctx);

  std::string banner = "[piet_native] parallel: " + std::to_string(tasks.size()) + " tasks / " +
                       std::to_string(budget.n) + " workers";
  if (!budget.reason.empty()) banner += " (" + budget.reason + ")";
  // arc ② decides here whether this block actually fans out. Three reasons not
  // to, each NAMED in the banner rather than silently degrading (§10.9: a block
  // that quietly runs serially is how "why isn't it faster" becomes unanswerable):
  //   - budget resolved to one worker (nothing to fan out to);
  //   - we ARE a worker (PIET_PARALLEL_WORKER) — one generation only;
  //   - fan-out could not be started at all, decided below.
  const bool inWorker = [] {
    const char* e = std::getenv("PIET_PARALLEL_WORKER");
    return e != nullptr && *e != '\0';
  }();
  bool fanOut = budget.n > 1 && !inWorker && tasks.size() > 1;
  if (inWorker && budget.n > 1) banner += " (nested: serial)";

  // §2.4 — the dataset travels BY PATH. datastore() materializes a virtual or
  // dirty resident head to a real file first, so what the children open is the
  // parent's actual current data rather than whatever the last save left on
  // disk. The parent executes nothing while the block runs, so the scratch head
  // cannot be reclaimed out from under them.
  std::string parentHead;
  if (fanOut && session.hasData()) {
    try {
      session.datastore();
      if (session.currentHbin().has_value()) parentHead = *session.currentHbin();
    } catch (const std::exception&) {
      parentHead.clear();
    }
    if (parentHead.empty()) {
      fanOut = false;
      banner += " (resident head could not be snapshotted: serial)";
    }
  }

  std::vector<parallel::TaskOutcome> outcomes;
  if (fanOut) {
    static std::atomic<unsigned long long> blockSeq{0};
    const std::string scratchDir = session.workdir() + "/parallel_" +
                                   std::to_string(blockSeq.fetch_add(1));
    outcomes = parallel::runTasksInWorkers(tasks, session, budget, parentHead, scratchDir);
    if (outcomes.empty()) {
      fanOut = false;
      banner += " (fan-out unavailable: serial)";
    }
  }
  if (!fanOut) banner += " [serial]";
  std::fprintf(stderr, "%s\n", banner.c_str());

  // Opt-in spec dump — kept as the schema-pinning hook selftest uses.
  parallel::dumpTaskSpecIfRequested(
      parallel::buildTaskSpec(tasks, session, budget, parentHead));

  ojson out = ojson::array();
  bool truncated = false;  // P7 storage cap — same discipline as every loop body
  int nFailed = 0;
  std::optional<std::string> control;
  // ---- arc ② replay: SOURCE order, whatever order the workers finished in ----
  if (fanOut) {
    std::optional<int> exitCode;
    size_t replayEnd = tasks.size();
    for (size_t i = 0; i < outcomes.size(); ++i) {
      if (outcomes[i].hasExitCode) {
        // §2.6 exit promotion: the earliest task (in SOURCE order) that exited
        // decides. Later tasks may already have finished; their OUTPUT is
        // discarded, which is what "the do-file stopped there" has to mean.
        exitCode = outcomes[i].exitCode;
        replayEnd = i + 1;
        break;
      }
    }
    for (size_t i = 0; i < replayEnd; ++i) {
      parallel::TaskOutcome& o = outcomes[i];
      if (!o.delivered) {
        // A worker that died owes an honest block, never silence (§2.6).
        ojson eb = errBlock("parallel: task " + std::to_string(i) + " did not complete (" +
                            (o.driverError.empty() ? std::string("worker died") : o.driverError) +
                            ")");
        eb["captured"] = true;
        pushLoopBlock(out, truncated, eb);
        ++nFailed;
        continue;
      }
      // Same per-task varlist thinning the serial leg does — a task's output is
      // what it would have been as a top-level statement.
      doexec::coalesceMutationVarlists(o.blocks);
      // G-4: fold this task's additive counters into the parent's own
      // accumulators BEFORE its blocks are replayed, so the env-gated emit at
      // the end of the run reports the whole run's work rather than only the
      // part that happened in this address space. Only the additive set folds
      // (counters.h::foldableAccumulators explains what is excluded and why);
      // error/graph counts are left to the parent's own replay so they are
      // counted exactly once.
      for (auto it = o.countersDelta.begin(); it != o.countersDelta.end(); ++it) {
        if (!it.value().is_number_integer()) continue;
        for (const auto& [name, slot] : piet::counters::foldableAccumulators()) {
          if (it.key() == name) { *slot += it.value().get<int64_t>(); break; }
        }
      }
      // G-2: the log. In every other path a statement's blocks reach an open log
      // because the OUTERMOST dispatch tees them on the way back. Replayed
      // blocks never pass through dispatch — they were produced in another
      // process, and a child is never given the parent's log (log state is
      // control, and two writers on one file is not a thing to attempt). So the
      // replay has to tee them itself, once per task, in source order. Without
      // this line the parallel run's log is missing every task's output while
      // its stdout is byte-perfect — which is precisely the split G-2 exists to
      // catch.
      session.logTee(o.blocks, 1);
      for (auto& b : o.blocks) {
        if (o.failed && b.value("type", "") == "error" && !b.value("captured", false)) {
          ojson tagged = b;
          tagged["captured"] = true;
          pushLoopBlock(out, truncated, tagged);
          continue;
        }
        pushLoopBlock(out, truncated, b);
      }
      if (o.failed) ++nFailed;
    }
    session.eval.locals.set("_rc", nFailed > 0 ? "1" : "0");
    session.eval.rValues.set("n_failed", static_cast<double>(nFailed));
    if (exitCode.has_value()) {
      // Re-raise the halt through the SAME sentinel the serial paths use, so
      // doexec's uncaptured-error rule promotes it to a clean parent halt with
      // this code (BUG-139's message shape).
      out.push_back(errBlock(*exitCode == 0 ? "exit" : "exit " + std::to_string(*exitCode)));
      if (replayEnd < tasks.size())
        std::fprintf(stderr,
                     "[piet_native] parallel: exit at task %zu — output of tasks %zu..%zu "
                     "discarded; completed tasks' file side effects were not rolled back\n",
                     replayEnd - 1, replayEnd, tasks.size() - 1);
    }
    return out;
  }

  for (const auto& task : tasks) {
    std::vector<std::string> taskLines;
    {
      std::istringstream iss(task);
      std::string l;
      while (std::getline(iss, l)) taskLines.push_back(l);
    }
    BodyRunResult r = runBody(taskLines, session, dispatchFn);
    // §2.5 byte law — one `varlist_after` card per TASK, not per block.
    // executeDoSource applies this thinning per top-level item, and a task is
    // exactly what a top-level item would have been had the wrapper not been
    // written; without this call the whole block shares one card and tasks
    // 1..K-1 silently lose theirs. cmp(A1,AN) cannot see that (both arms lose
    // them identically) — the probe's wrapper-less A0 arm is what caught it.
    doexec::coalesceMutationVarlists(r.blocks);
    for (auto& b : r.blocks) {
      // §2.6 failure isolation, the half that per-task dispatch alone does NOT
      // buy: a task's uncaught error block would otherwise escape the block
      // untagged and trip doexec's top-level halt rule (type=="error" &&
      // !captured), so the do-file would stop at the parallel block even though
      // the remaining tasks already ran. Measured on the first build: task 2
      // ran, and `stopped_at_line` was still set.
      //
      // The error stays VISIBLE (honest-error doctrine — it is pushed, not
      // swallowed); `captured` is this codebase's existing "display-only, does
      // not halt" tag, the same one the dispatcher's capture arm sets (BS-127),
      // and it is the right one here because a parallel block reports failure
      // the way capture does: through `_rc` (+ r(n_failed) below), not by
      // halting. A caller that wants the old halt behaviour writes
      // `if _rc { exit _rc }` after the block.
      //
      // ⚠ ARC ② OWED — `exit` inside a task. §2.6 requires a task's clean
      // `exit`/`exit N` to be PROMOTED to a parent halt (stop dispatching, let
      // running tasks finish, replay in source order, then halt). runBody drops
      // ExecuteResult::stoppedAtExitCode, so this arc cannot distinguish a
      // clean exit from a failure and treats both as a non-halting task
      // failure. Arc ② must widen BodyRunResult with that field — the child
      // protocol already carries it (`exit_code` in parallel.cpp's worker
      // result), so the parent half is what is missing, not the wire.
      if (r.hasError && b.value("type", "") == "error" && !b.value("captured", false)) {
        ojson tagged = b;
        tagged["captured"] = true;
        pushLoopBlock(out, truncated, tagged);
        continue;
      }
      pushLoopBlock(out, truncated, b);
    }
    if (r.hasError) ++nFailed;  // §2.6: counted, but the block keeps going
    if (r.control.has_value()) {
      // A break/continue that bubbled out of a task belongs to an ENCLOSING
      // loop, so it must not be swallowed — and no further task may run once
      // it fired (that is what break/continue mean).
      control = r.control;
      break;
    }
  }

  // §8 #4 ruling (c): the parent learns the failure count and a coarse _rc.
  // "1 on any failure, 0 otherwise" is the same convention runCaptureBlock uses
  // — an exact numeric rc taxonomy is BS-100/A4 everywhere in this codebase,
  // not something this block invents for itself.
  session.eval.locals.set("_rc", nFailed > 0 ? "1" : "0");
  session.eval.rValues.set("n_failed", static_cast<double>(nFailed));

  if (control.has_value()) {
    ojson ctrl;
    ctrl["type"] = "_control";
    ctrl["action"] = *control;
    out.push_back(ctrl);
  }
  return out;
}

}  // namespace

// BUG-483: declared in loops.h, defined here — outside the anonymous namespace
// above, which is where the counter lives (still visible for the rest of this
// translation unit).
int loopBodyDepth() { return g_loopBodyDepth; }
LoopBodyScope::LoopBodyScope() { ++g_loopBodyDepth; }
LoopBodyScope::~LoopBodyScope() { --g_loopBodyDepth; }

void markNoisily(ojson& blocks, const SessionState& session) {
  if (!session.logEchoSuppressed() || !blocks.is_array()) return;
  for (auto& b : blocks) {
    if (b.is_object() && b.value("type", std::string()) != "echo") b["noisily"] = true;
  }
}

// BUG-1338: the message of a failed inline `: fn'/`=exp' (inline_note) shows
// under quietly as an error's does (StataBE 19.5: `quietly { display "`: type
// nosuch'" }` prints it).
bool keptWhenQuiet(const ojson& b) {
  const std::string t = b.value("type", std::string());
  return t == "echo" || t == "error" || t == "_control" || b.value("noisily", false) ||
         b.value("inline_note", false);
}

void settleNoisily(ojson& blocks, const SessionState& session, bool force) {
  if ((!force && session.logEchoSuppressed()) || !blocks.is_array()) return;
  for (auto& b : blocks) {
    if (b.is_object()) b.erase("noisily");
  }
}

VarlistExpandResult expandVarlist(const std::string& spec, SessionState& session) {
  return expandVarlistImpl(spec, session);
}

std::optional<nlohmann::ordered_json> checkKnownVarlist(std::vector<std::string>& tokens,
                                                        SessionState& session) {
  return checkKnownVarTokens(tokens, session);
}

bool expandNumlist(const std::string& spec, std::vector<std::string>* elements) {
  NumlistResult nr = parseNumlist(spec);
  if (nr.isError) return false;
  *elements = std::move(nr.tokens);
  return true;
}

bool isLoopHeader(const std::string& line) {
  return loopIfKeywords().count(headKeyword(line)) != 0;
}

bool isParallelHeader(const std::string& line) { return headKeyword(line) == "parallel"; }

// BUG-167 — see the contract in loops.h. The prefix-class spellings mirror
// loopIfKeywords() exactly (capture/quietly/noisily + their abbreviations);
// foreach/forvalues/while/if are the CONTROL class, whose header legitimately
// carries a list or condition ahead of the brace.
bool opensBraceBlock(const std::string& line) {
  if (!isLoopHeader(line)) return false;
  size_t brace = line.find('{');
  if (brace == std::string::npos) return false;

  static const std::unordered_set<std::string> kControl = {"foreach", "forvalues", "forval",
                                                          "forv",    "forvalue",  "while",
                                                          "if"};
  if (kControl.count(lower(firstWord(line))) != 0) return true;

  // Prefix class: everything before the brace must itself be prefix keywords.
  std::istringstream iss(line.substr(0, brace));
  std::string word;
  while (iss >> word) {
    std::string lw = lower(word);
    if (kControl.count(lw) != 0) return true;  // `capture if ... {` — control wins.
    if (loopIfKeywords().count(lw) == 0) return false;
  }
  return true;
}

// P4 wave — see loops.h. Splits `COND command ...` by walking whitespace-
// separated tokens at paren/quote depth 0 with a tiny operand/operator state
// machine: the command begins at the first token that arrives where an
// OPERATOR was expected. Returning false (rather than a best guess) whenever
// the walk cannot reach that state is the whole safety property — the caller
// then reports its ordinary "unsupported" error, and a malformed line can
// never silently execute a command the user did not write.
bool splitOneLineIf(const std::string& rest, std::string& cond, std::string& body) {
  static const std::vector<std::string> kBinOps = {
      "==", "!=", "~=", ">=", "<=", "&&", "||", "&", "|", "<", ">", "+", "-", "*", "/", "^"};
  auto isBinOpToken = [&](const std::string& t) {
    return std::find(kBinOps.begin(), kBinOps.end(), t) != kBinOps.end();
  };
  // A token ENDING in a binary operator (e.g. `x'!=`) still expects an operand;
  // one that merely contains a complete comparison (`x'!=0`) does not.
  auto endsWithBinOp = [&](const std::string& t) {
    for (const auto& op : kBinOps) {
      if (t.size() >= op.size() && t.compare(t.size() - op.size(), op.size(), op) == 0) return true;
    }
    return false;
  };

  size_t i = 0, n = rest.size();
  int depth = 0;
  bool inQuote = false;
  bool expectOperand = true;
  size_t splitAt = std::string::npos;
  while (i < n) {
    while (i < n && std::isspace(static_cast<unsigned char>(rest[i]))) ++i;
    if (i >= n) break;
    const size_t tokStart = i;
    while (i < n) {
      const char c = rest[i];
      if (c == '"') inQuote = !inQuote;
      else if (!inQuote && (c == '(' || c == '[')) ++depth;
      else if (!inQuote && (c == ')' || c == ']')) --depth;
      else if (!inQuote && depth == 0 && std::isspace(static_cast<unsigned char>(c))) break;
      ++i;
    }
    const std::string tok = rest.substr(tokStart, i - tokStart);
    if (inQuote || depth != 0) return false;  // unbalanced — refuse to guess
    if (expectOperand) {
      // A lone `!`/`~` is a unary prefix; anything else completes an operand
      // unless it trails a binary operator awaiting its right-hand side.
      if (tok != "!" && tok != "~") expectOperand = endsWithBinOp(tok);
    } else if (isBinOpToken(tok)) {
      expectOperand = true;
    } else {
      splitAt = tokStart;  // an operand where an operator belongs = the command
      break;
    }
  }
  if (splitAt == std::string::npos || splitAt == 0) return false;
  cond = rest.substr(0, splitAt);
  body = rest.substr(splitAt);
  // trim
  auto trimBoth = [](std::string& s2) {
    size_t a = s2.find_first_not_of(" \t");
    size_t b = s2.find_last_not_of(" \t");
    s2 = (a == std::string::npos) ? std::string() : s2.substr(a, b - a + 1);
  };
  trimBoth(cond);
  trimBoth(body);
  return !cond.empty() && !body.empty();
}

ojson runOneLineIf(const std::string& condition, const std::string& bodyLine,
                   SessionState& session, const DispatchFn& dispatchFn) {
  return runIf(condition, {bodyLine}, {}, session, dispatchFn);
}

// Legacy `for` command (out of date since Stata 8, still accepted by Stata).
// Behaviour: every rule below is
// measured StataBE 19.5 behaviour (the manuals no longer document `for`).
namespace {

ojson forSyntaxError(const std::string& msg) {
  return ojson(eval::errorBlock("syntax_error", msg, std::nullopt, std::nullopt, std::nullopt,
                                std::string("r(198)")));
}

ojson forRcError(const std::string& msg, int rc) {
  return ojson(eval::errorBlock("engine_error", msg, std::nullopt, std::nullopt, std::nullopt,
                                "r(" + std::to_string(rc) + ")"));
}

bool forIsBlank(char c) { return c == ' ' || c == '\t'; }

bool forIsWordByte(unsigned char c) { return std::isalnum(c) != 0 || c == '_' || c >= 0x80; }

// Stand-in for the '(' of a bare r(name)/e(name) while the line's macros are
// expanded: those are expressions each generated command evaluates when it
// runs, not macros of the `for` line.
constexpr char kForParenHold = '\x1d';

// Expands the line's macros once, as Stata does for any command line, leaving
// bare r()/e() expressions alone. Compound quotes come back as typed.
std::optional<std::string> forExpandLine(const std::string& line, SessionState& session,
                                         ojson& err) {
  std::string held = line;
  int macroDepth = 0;
  int compound = 0;
  for (size_t i = 0; i < held.size(); ++i) {
    const char c = held[i];
    const char nx = i + 1 < held.size() ? held[i + 1] : '\0';
    if (c == '`' && nx == '"') { ++compound; ++i; continue; }
    if (c == '"' && nx == '\'' && compound > 0) { --compound; ++i; continue; }
    if (c == '`') { ++macroDepth; continue; }
    if (c == '\'' && macroDepth > 0) { --macroDepth; continue; }
    if (macroDepth == 0 && (c == 'r' || c == 'e') && nx == '(' &&
        (i == 0 || !forIsWordByte(static_cast<unsigned char>(held[i - 1])))) {
      held[i + 1] = kForParenHold;
      ++i;
    }
  }
  std::string out = held;
  if (eval::hasMacroRefs(held)) {
    eval::ExpandResult er = eval::expandMacros(held, session.eval);
    if (er.isError) {
      err = er.error;
      return std::nullopt;
    }
    out = er.typed.empty() ? er.line : er.typed;
  }
  std::replace(out.begin(), out.end(), kForParenHold, '(');
  return out;
}

// End (exclusive) of the compound-quoted span opening at `i` (s[i..i+1] is
// `" ); nesting counts. Runs to the end of `s` when unterminated.
size_t forCompoundEnd(const std::string& s, size_t i) {
  int depth = 0;
  size_t j = i;
  while (j < s.size()) {
    if (s[j] == '`' && j + 1 < s.size() && s[j + 1] == '"') {
      ++depth;
      j += 2;
    } else if (s[j] == '"' && j + 1 < s.size() && s[j + 1] == '\'') {
      --depth;
      j += 2;
      if (depth == 0) return j;
    } else {
      ++j;
    }
  }
  return s.size();
}

// A blank-separated word of the header: `text` is the word as written,
// `value` the element it stands for (outer quotes removed), `quoted` whether
// it was a quoted string.
struct ForWord {
  std::string text;
  std::string value;
  bool quoted = false;
};

std::vector<ForWord> forHeaderWords(const std::string& s) {
  std::vector<ForWord> words;
  size_t i = 0;
  while (i < s.size()) {
    if (forIsBlank(s[i])) { ++i; continue; }
    ForWord w;
    size_t end;
    if (s[i] == '`' && i + 1 < s.size() && s[i + 1] == '"') {
      end = forCompoundEnd(s, i);
      w.quoted = true;
      const size_t inner = end - i >= 4 ? end - i - 4 : 0;
      w.value = s.substr(i + 2, inner);
    } else if (s[i] == '"') {
      const size_t close = s.find('"', i + 1);
      end = close == std::string::npos ? s.size() : close + 1;
      w.quoted = true;
      w.value = s.substr(i + 1, close == std::string::npos ? std::string::npos : close - i - 1);
    } else {
      end = i;
      while (end < s.size() && !forIsBlank(s[end])) ++end;
      w.value = s.substr(i, end - i);
    }
    w.text = s.substr(i, end - i);
    words.push_back(std::move(w));
    i = end;
  }
  return words;
}

// One command of the body rebuilt the way `for` stores it: blank runs become
// one blank, a word opening with `"` runs to the next `"` and is rewritten as a
// compound-quoted string, a compound-quoted word is kept, and anything else
// runs to the next blank.
std::string forRebuildCommand(const std::string& s) {
  std::string out;
  size_t i = 0;
  auto add = [&out](const std::string& w) {
    if (!out.empty()) out += ' ';
    out += w;
  };
  while (i < s.size()) {
    if (forIsBlank(s[i])) { ++i; continue; }
    if (s[i] == '`' && i + 1 < s.size() && s[i + 1] == '"') {
      const size_t end = forCompoundEnd(s, i);
      add(s.substr(i, end - i));
      i = end;
    } else if (s[i] == '"') {
      const size_t close = s.find('"', i + 1);
      const std::string inner =
          s.substr(i + 1, close == std::string::npos ? std::string::npos : close - i - 1);
      add("`\"" + inner + "\"'");
      i = close == std::string::npos ? s.size() : close + 1;
    } else {
      size_t end = i;
      while (end < s.size() && !forIsBlank(s[end])) ++end;
      add(s.substr(i, end - i));
      i = end;
    }
  }
  return out;
}

// Every non-overlapping occurrence of `from`, left to right, becomes `to`; the
// inserted text is not rescanned.
std::string forReplaceAll(const std::string& s, const std::string& from, const std::string& to) {
  if (from.empty()) return s;
  std::string out;
  size_t pos = 0;
  for (size_t hit = s.find(from); hit != std::string::npos; hit = s.find(from, pos)) {
    out.append(s, pos, hit - pos);
    out += to;
    pos = hit + from.size();
  }
  out.append(s, pos, std::string::npos);
  return out;
}

// `word` names `full` when it is a prefix of it at least `minLen` long
// (case-sensitive).
bool forAbbrev(const std::string& word, const std::string& full, size_t minLen) {
  return word.size() >= minLen && word.size() <= full.size() &&
         full.compare(0, word.size(), word) == 0;
}

struct ForOptions {
  bool dryrun = false;
  bool noheader = false;
  bool nostop = false;
};

std::optional<ojson> forParseOptions(const std::string& text, ForOptions& opts) {
  std::string own = text;
  std::string afterSecondComma;
  bool secondComma = false;
  const long long c2 = eval::findUnquotedComma(text);
  if (c2 >= 0) {
    own = text.substr(0, static_cast<size_t>(c2));
    afterSecondComma = text.substr(static_cast<size_t>(c2) + 1);
    secondComma = true;
  }
  for (const ForWord& w : forHeaderWords(own)) {
    const std::string& word = w.text;
    const size_t paren = word.find('(');
    if (paren != std::string::npos) {
      return forSyntaxError("option " + word.substr(0, paren) + "() not allowed");
    }
    if (forAbbrev(word, "dryrun", 1)) opts.dryrun = true;
    else if (forAbbrev(word, "noheader", 3)) opts.noheader = true;
    else if (forAbbrev(word, "nostop", 3)) opts.nostop = true;
    else if (forAbbrev(word, "pause", 1)) {
      // Pauses only under `pause on`; nothing to do here.
    } else {
      return forSyntaxError("option " + word + " not allowed");
    }
  }
  if (secondComma) {
    const std::vector<ForWord> rest = forHeaderWords(afterSecondComma);
    if (rest.empty()) return forSyntaxError("invalid syntax");
    return forSyntaxError("invalid '" + rest.front().text + "' ");
  }
  return std::nullopt;
}

// Elements of one `newlist` list: a numbered range stubA-stubB expands to the
// names in between; every name must be valid and not yet a variable.
std::optional<ojson> forNewlistElements(const std::vector<ForWord>& words, SessionState& session,
                                        std::vector<std::string>& out) {
  std::vector<std::string> existing;
  if (const std::vector<VarInfo>* card = session.residentReadCard()) {
    for (const auto& v : *card) existing.push_back(v.name);
  } else if (Datastore* ds = session.datastore()) {
    existing = ds->names();
  }
  auto splitNumbered = [](const std::string& name, std::string& stub, std::string& digits) {
    size_t k = name.size();
    while (k > 0 && std::isdigit(static_cast<unsigned char>(name[k - 1])) != 0) --k;
    stub = name.substr(0, k);
    digits = name.substr(k);
    return !stub.empty() && !digits.empty();
  };
  std::vector<std::string> names;
  for (const ForWord& w : words) {
    const std::string& tok = w.text;
    const size_t dash = tok.find('-');
    if (dash != std::string::npos && dash > 0 && dash + 1 < tok.size() &&
        tok.find('-', dash + 1) == std::string::npos) {
      const std::string lo = tok.substr(0, dash), hi = tok.substr(dash + 1);
      std::string s1, d1, s2, d2;
      if (strutil::isValidStataIdentifier(lo) && strutil::isValidStataIdentifier(hi) &&
          splitNumbered(lo, s1, d1) && splitNumbered(hi, s2, d2) && s1 == s2 &&
          d1.size() < 10 && d2.size() < 10) {
        const long n1 = std::stol(d1), n2 = std::stol(d2);
        const long step = n1 <= n2 ? 1 : -1;
        for (long n = n1;; n += step) {
          names.push_back(s1 + std::to_string(n));
          if (n == n2) break;
        }
        continue;
      }
    }
    names.push_back(tok);
  }
  for (const std::string& n : names) {
    if (!strutil::isValidStataIdentifier(n) || n.size() > 32) {
      return forSyntaxError(n + " invalid name");
    }
    if (std::find(existing.begin(), existing.end(), n) != existing.end()) {
      return forRcError("variable " + n + " already defined", 110);
    }
  }
  out = std::move(names);
  return std::nullopt;
}

// Elements of one `varlist` list: existing variables, wildcards and ranges
// expanded as in any varlist.
std::optional<ojson> forVarlistElements(const std::vector<ForWord>& words, SessionState& session,
                                        std::vector<std::string>& out) {
  if (words.empty()) {
    out.clear();
    return std::nullopt;
  }
  std::string spec;
  for (const ForWord& w : words) {
    if (!spec.empty()) spec += ' ';
    spec += w.text;
  }
  VarlistExpandResult ex = expandVarlistImpl(spec, session);
  if (ex.isError) {
    // A range whose end is not a variable: name the missing end.
    std::vector<std::string> known;
    if (Datastore* ds = session.datastore()) known = ds->names();
    for (const ForWord& w : words) {
      const size_t dash = w.text.find('-');
      if (dash == std::string::npos) continue;
      for (const std::string& end : {w.text.substr(0, dash), w.text.substr(dash + 1)}) {
        if (!end.empty() && std::find(known.begin(), known.end(), end) == known.end())
          return forRcError("variable " + end + " not found", 111);
      }
    }
    if (eval::coarseRc(ex.error) == "111") return ex.error;
    return forRcError(ex.error.value("message", std::string("invalid varlist")), 111);
  }
  if (auto bad = checkKnownVarTokens(ex.tokens, session)) return bad;
  out = std::move(ex.tokens);
  return std::nullopt;
}

struct ForList {
  std::string name;
  std::vector<std::string> elements;
};

}  // namespace

bool isLegacyForLine(const std::string& line) {
  const std::string t = trim(line);
  if (t.compare(0, 3, "for") != 0) return false;
  return t.size() == 3 || forIsBlank(t[3]) || t[3] == ':';
}

ojson runLegacyFor(const std::string& rawLine, SessionState& session,
                   const DispatchFn& dispatchFn) {
  ojson out = ojson::array();
  try {
    // 1. The line's macros, once, before `for` reads it.
    ojson expandErr;
    const std::optional<std::string> expanded = forExpandLine(trim(rawLine), session, expandErr);
    if (!expanded) {
      out.push_back(expandErr);
      return out;
    }
    const std::string rest = expanded->size() > 3 ? expanded->substr(3) : std::string();

    // 2. Header : body, at the first colon outside quotes.
    const std::vector<bool> qmask = eval::quotedSpanMask(rest);
    size_t colon = std::string::npos;
    for (size_t i = 0; i < rest.size(); ++i) {
      if (rest[i] == ':' && !qmask[i]) {
        colon = i;
        break;
      }
    }
    if (colon == std::string::npos || trim(rest.substr(0, colon)).empty()) {
      out.push_back(forSyntaxError("invalid syntax"));
      return out;
    }
    const std::string header = rest.substr(0, colon);
    const std::string body = rest.substr(colon + 1);

    // 3. Options.
    std::string listText = header;
    ForOptions opts;
    const long long comma = eval::findUnquotedComma(header);
    if (comma >= 0) {
      listText = header.substr(0, static_cast<size_t>(comma));
      if (auto bad = forParseOptions(header.substr(static_cast<size_t>(comma) + 1), opts)) {
        out.push_back(*bad);
        return out;
      }
    }

    // 4. Lists: segments separated by a `\` that stands as its own word.
    std::vector<std::vector<ForWord>> segments(1);
    for (ForWord& w : forHeaderWords(listText)) {
      if (!w.quoted && w.text == "\\") {
        segments.emplace_back();
        continue;
      }
      segments.back().push_back(std::move(w));
    }
    segments.erase(std::remove_if(segments.begin(), segments.end(),
                                  [](const std::vector<ForWord>& s) { return s.empty(); }),
                   segments.end());
    static const char* const kDefaultNames[] = {"X", "Y", "Z", "A", "B", "C", "D", "E", "F"};
    if (segments.size() > 9) {
      out.push_back(forSyntaxError("too many lists"));
      return out;
    }
    std::vector<ForList> lists;
    for (size_t k = 0; k < segments.size(); ++k) {
      const std::vector<ForWord>& seg = segments[k];
      ForList list;
      size_t typeAt = 0;
      if (seg.size() >= 2 && !seg[1].quoted && seg[1].text == "in") {
        if (seg.size() < 3) {
          out.push_back(forSyntaxError("invalid syntax"));
          return out;
        }
        list.name = seg[0].text;
        typeAt = 2;
      } else {
        list.name = kDefaultNames[k];
      }
      const std::string& type = seg[typeAt].text;
      const std::vector<ForWord> elems(seg.begin() + static_cast<long>(typeAt) + 1, seg.end());
      if (forAbbrev(type, "varlist", 3)) {
        if (auto bad = forVarlistElements(elems, session, list.elements)) {
          out.push_back(*bad);
          return out;
        }
      } else if (forAbbrev(type, "numlist", 3)) {
        if (!elems.empty()) {
          std::string spec;
          for (const ForWord& w : elems) spec += w.text + " ";
          NumlistResult nl = parseNumlist(spec);
          if (nl.isError) {
            out.push_back(forRcError("invalid numlist", 121));
            return out;
          }
          list.elements = std::move(nl.tokens);
        }
      } else if (forAbbrev(type, "newlist", 3)) {
        if (auto bad = forNewlistElements(elems, session, list.elements)) {
          out.push_back(*bad);
          return out;
        }
      } else if (type == "any") {
        for (const ForWord& w : elems) list.elements.push_back(w.value);
      } else {
        out.push_back(forSyntaxError("\"" + type + "\" invalid list type"));
        return out;
      }
      lists.push_back(std::move(list));
    }
    for (const ForList& l : lists) {
      if (l.elements.size() != lists.front().elements.size()) {
        out.push_back(forSyntaxError("lists have unequal number of elements"));
        return out;
      }
    }

    // 5. Every placeholder must occur in the commands; each list's first
    // element stands in for its placeholder before the next list is looked for.
    {
      std::string probe = body;
      for (const ForList& l : lists) {
        if (probe.find(l.name) == std::string::npos) {
          out.push_back(forSyntaxError(l.name + " not in command"));
          return out;
        }
        if (!l.elements.empty()) probe = forReplaceAll(probe, l.name, l.elements.front());
      }
    }

    // 6. Commands: split at every `\` outside quotes, then rebuilt.
    std::vector<std::string> commands;
    {
      const std::vector<bool> bmask = eval::quotedSpanMask(body);
      size_t from = 0;
      for (size_t i = 0; i <= body.size(); ++i) {
        if (i == body.size() || (body[i] == '\\' && !bmask[i])) {
          commands.push_back(forRebuildCommand(body.substr(from, i - from)));
          from = i + 1;
        }
      }
    }

    // 7. Iterate. The generated commands run in a local scope of their own,
    // as the body of any program does: a `local` they set is gone afterwards
    // (StataBE: `local i 0` then `for num 1 2 3: local i = `i' + X` leaves i
    // at 0, while the same with a global leaves it at 3).
    const size_t n = lists.empty() ? 0 : lists.front().elements.size();
    doexec::PrivateLocalScope bodyScope(session);
    for (size_t it = 0; it < n; ++it) {
      for (const std::string& cmdTemplate : commands) {
        std::string cmd = cmdTemplate;
        for (const ForList& l : lists) cmd = forReplaceAll(cmd, l.name, l.elements[it]);
        if (!opts.noheader) {
          ojson hdr;
          hdr["type"] = "display";
          hdr["text"] = cmd.empty() ? std::string("-> ") : "->  " + cmd;
          out.push_back(std::move(hdr));
        }
        if (opts.dryrun || cmd.empty()) continue;
        ojson res;
        try {
          res = dispatchFn(cmd, session);
        } catch (const std::exception& e) {
          res = ojson(eval::engineError(e.what()));
        }
        if (!res.is_array()) res = ojson::array({res});
        // A failure is the command's first uncaptured error block. Without
        // nostop it goes out as is and the loop ends there; with nostop it is
        // shown as captured (an `exit N` halt has no message to show), then
        // `->  r(N); `, and the loop goes on.
        static const std::regex reExitHalt(R"(^exit(\s+-?\d+)?$)");
        std::optional<std::string> failedRc;
        for (const auto& b : res) {
          const std::string t = b.value("type", std::string());
          if (t == "echo") continue;
          if (!failedRc && t == "error" && !b.value("captured", false)) {
            failedRc = eval::coarseRc(b);
            if (!opts.nostop) {
              out.push_back(b);
              return out;
            }
            if (std::regex_match(b.value("message", std::string()), reExitHalt)) continue;
            ojson shown = b;
            shown["captured"] = true;
            out.push_back(std::move(shown));
            continue;
          }
          out.push_back(b);
        }
        if (!failedRc) continue;
        ojson rcLine;
        rcLine["type"] = "display";
        rcLine["text"] = "->  r(" + *failedRc + "); ";
        out.push_back(std::move(rcLine));
      }
    }
  } catch (const std::exception& e) {
    out.push_back(ojson(eval::engineError(std::string("for: ") + e.what())));
  } catch (...) {
    out.push_back(ojson(eval::engineError("for: internal error")));
  }
  return out;
}

namespace {

// FULL FUNCTIONAL CHECK (2026-08-05). Peel leading PREFIX keywords off a block
// header while what remains still starts with a CONTROL keyword, e.g.
// `quietly forvalues i = 1/10 {` -> ("quietly", "forvalues i = 1/10").
//
// Why this exists: executeLoop dispatches on the header's FIRST WORD, so a
// prefixed loop header matched the prefix branch and ran the body as a bare
// `quietly { … }` block — the `forvalues i = 1/10` part was discarded outright.
// The body therefore executed ONCE with the loop macro never defined, which
// surfaces as "Local macro `i' is not defined". Stata accepts a prefix on any
// command, loop headers included. Returns false when the header is not a
// prefixed control header, leaving every existing dispatch untouched.
bool splitPrefixedControlHeader(const std::string& hdr, std::vector<std::string>& prefixes,
                                std::string& inner) {
  static const std::unordered_set<std::string> kControl = {"foreach", "forvalues", "forval",
                                                          "forv",    "forvalue",  "while",
                                                          "if"};
  std::string rest = trim(hdr);
  prefixes.clear();
  while (true) {
    std::string fw = lower(firstWord(rest));
    if (fw.empty()) return false;
    if (kControl.count(fw) != 0) {
      // `if` is BOTH a control keyword and a legal prefix-free header; only
      // treat what we peeled as prefixes if we actually peeled something.
      if (prefixes.empty()) return false;
      inner = rest;
      return true;
    }
    if (loopIfKeywords().count(fw) == 0) return false;  // not a prefix either
    prefixes.push_back(fw);
    size_t sp = rest.find_first_of(" \t");
    if (sp == std::string::npos) return false;          // prefix with no inner command
    rest = trim(rest.substr(sp));
  }
}

bool isCapturePrefix(const std::string& w) {
  return w == "capture" || w == "captur" || w == "captu" || w == "capt" || w == "cap";
}
bool isQuietlyPrefix(const std::string& w) {
  return w == "quietly" || w == "qui" || w == "quie" || w == "quiet";
}
// The noisily-class spellings splitPrefixedControlHeader accepts. Everything
// that is neither capture- nor quietly-class in a prefix chain is this, but the
// BUG-189 log scope has to say WHICH it is (noisily pushes a visible scope,
// capture pushes none), so the set is named rather than inferred by elimination.
bool isNoisilyPrefix(const std::string& w) {
  return w == "noisily" || w == "noisil" || w == "noisi" || w == "nois" ||
         w == "noi" || w == "no" || w == "n";
}

}  // namespace

ojson executeLoop(const std::string& header, const std::vector<std::string>& bodyLinesIn,
                   const std::vector<std::string>& elseBodyLines, SessionState& session,
                   const DispatchFn& dispatchFn) {
  std::string hdr = rstripBrace(header);
  // AER-SCOPE1 bundle A: fold the accepted truncations onto the canonical head
  // ONCE, here, rather than widening the five `fw == "forvalues"` compares and
  // three `^forvalues\s+` regexes downstream. Five spellings times eight sites
  // is forty places for one of them to be forgotten; this is one place, and the
  // prefixed path (`quietly forval i=1/3 {`) reaches it too because that path
  // recurses back into executeLoop with the inner header.
  {
    size_t sp = hdr.find_first_of(" \t");
    std::string head = lower(sp == std::string::npos ? hdr : hdr.substr(0, sp));
    if (head == "forval" || head == "forv" || head == "forvalue") {
      hdr = "forvalues" + (sp == std::string::npos ? std::string() : hdr.substr(sp));
    }
  }

  // A prefixed control header runs the CONTROL construct and then applies the
  // prefix's own semantics to what it produced — innermost prefix first, so
  // `capture quietly forvalues …` quiets the loop and then captures it, the
  // same order the single-line prefix stripper composes in.
  {
    std::vector<std::string> prefixes;
    std::string inner;
    if (splitPrefixedControlHeader(hdr, prefixes, inner)) {
      // BUG-189: `quietly forvalues … {` must hide its body from the log too.
      // The stack is innermost-wins, so pushing the prefixes outermost-first
      // leaves the innermost keyword on top — the same rule the result filters
      // below implement by folding in reverse.
      // BUG-1434: a capture hides output as quietly does, and a noisily after
      // a capture or quietly turns output back on (the last word decides,
      // StataBE 19.5) -- so either one filters only when no noisily follows.
      auto noisilyAfter = [&](size_t at) {
        for (size_t i = at + 1; i < prefixes.size(); ++i)
          if (isNoisilyPrefix(prefixes[i])) return true;
        return false;
      };
      std::vector<std::unique_ptr<SessionState::LogVerbosityScope>> lvScopes;
      for (size_t i = 0; i < prefixes.size(); ++i) {
        const std::string& p = prefixes[i];
        if (isQuietlyPrefix(p) || (isCapturePrefix(p) && !noisilyAfter(i))) {
          lvScopes.push_back(std::make_unique<SessionState::LogVerbosityScope>(session, true));
        } else if (isNoisilyPrefix(p)) {
          lvScopes.push_back(std::make_unique<SessionState::LogVerbosityScope>(session, false));
        }
      }
      ojson result = executeLoop(inner, bodyLinesIn, elseBodyLines, session, dispatchFn);
      lvScopes.clear();  // close the scopes before the result filters run
      for (auto it = prefixes.rbegin(); it != prefixes.rend(); ++it) {
        const size_t at = static_cast<size_t>(prefixes.rend() - it) - 1;
        if ((isQuietlyPrefix(*it) || isCapturePrefix(*it)) && !noisilyAfter(at)) {
          // quietly: keep command echoes + errors, drop ordinary output —
          // the same filter runQuietlyBlock applies to a braced body.
          ojson kept = ojson::array();
          for (const auto& b : result)
            if (keptWhenQuiet(b)) kept.push_back(b);
          result = kept;
        }
        if (isCapturePrefix(*it)) {
          // capture: swallow an uncaught error and publish _rc, matching
          // runCaptureBlock. A loop that errored has already stopped itself.
          // BUG-1434: under `capture noisily` the error is SHOWN (tagged
          // captured, so it does not halt) -- StataBE prints the inner
          // command's message, as the single-line prefix does.
          const bool showError = noisilyAfter(at);
          ojson kept = ojson::array();
          bool errored = false;
          // BUG-546: the exact coarse rc of the FIRST uncaptured error block,
          // not a hardcoded "1" — mirrors runCaptureBlock's own fix above, and
          // the pre-existing single-line `capture` prefix in dispatcher.cpp,
          // which already reads eval::coarseRc. Oracle: w2_openbugs.log
          // fe_unknown/fe_mixed via `capture noisily foreach ... { }`.
          std::string errRc;
          for (const auto& b : result) {
            if (b.value("type", std::string()) == "error" && !b.value("captured", false)) {
              errored = true;
              if (errRc.empty()) errRc = eval::coarseRc(b);
              // BUG-350 coverage gap, found 2026-09-01 by the leg-1D probe-2
              // audit: this is the SECOND place an error block is dropped where
              // the user cannot see it, and it was not counted. The counter's
              // whole claim is "a run that hid something says so", and a
              // `capture` on a LOOP prefix hid it silently. Counted here for the
              // same reason and by the same call as dispatcher.cpp's site.
              //
              // Under `capture noisily` the block is kept and shown instead.
              if (showError) {
                ojson tagged = b;
                tagged["captured"] = true;
                kept.push_back(std::move(tagged));
                continue;
              }
              piet::counters::noteSwallowedError(b.value("message", std::string()));
              continue;
            }
            // BUG-1338: the messages of failed inline `: fn'/`=exp' in the body
            // are silent under a capture (StataBE: `capture foreach v in a {
            // display "`: type nosuch'" }` prints nothing), shown under capture
            // noisily.
            if (b.value("inline_note", false) && !showError) continue;
            kept.push_back(b);
          }
          session.eval.locals.set("_rc", errored ? errRc : "0");
          result = kept;
        }
        // noisily: transparent; inside an enclosing quiet scope its output is
        // the nested noisily's, which that scope keeps.
        if (isNoisilyPrefix(*it)) markNoisily(result, session);
      }
      settleNoisily(result, session);
      return result;
    }
  }

  // Macro expansion of forvalues/foreach headers ONLY (while/if re-expand
  // per-iteration inside their own runners) — loops.py's has_macro_refs guard.
  std::string fw = headKeyword(hdr);
  // wt_x01_L4/L5 (w2at_table_writers.do:834, log): Stata stores a loop body
  // with each unquoted blank run collapsed to one blank, before macro
  // expansion (eval::collapseUnquotedBlanks; `tex `lab' &  ``lab'_m' \\`
  // writes `x1 & .125 \\`). The body lines are the flattened block, so an
  // `if {}` nested in the loop is collapsed with it; a top-level `if {}` body
  // reaches here as `if` and keeps its blanks, as Stata's does.
  std::vector<std::string> collapsedBody;
  const bool storesCollapsed = fw == "foreach" || fw == "forvalues" || fw == "while";
  if (storesCollapsed) {
    collapsedBody.reserve(bodyLinesIn.size());
    for (const auto& l : bodyLinesIn) collapsedBody.push_back(eval::collapseUnquotedBlanks(l));
  }
  const std::vector<std::string>& bodyLines = storesCollapsed ? collapsedBody : bodyLinesIn;
  if ((fw == "forvalues" || fw == "foreach") && eval::hasMacroRefs(hdr)) {
    eval::ExpandResult exp = eval::expandMacros(hdr, session.eval);
    if (exp.isError) return ojson::array({exp.error});
    hdr = exp.line;
  }

  // foreach VAR of varlist VARLIST
  //
  // BUG-545: the keyword accepts any >=3-char prefix of `varlist` (`var`,
  // `varl`, `vari`, `varli`, `varlis`, `varlist`), not the literal word only.
  // Corpus-measured: 19 lines spell `of var`, 1 spells `of varl`, 20 lines in
  // 8 papers total (open-bugs.json use_count_measured); oracle
  // fe_of_var (w2_openbugs.log) is rc 0 for `of var`.
  {
    static const std::regex re(R"(^foreach\s+([\w\x80-\xff]+)\s+of\s+var(?:l(?:i(?:s(?:t)?)?)?)?\s+(.*))",
                                std::regex::icase);
    std::smatch m;
    if (std::regex_search(hdr, m, re)) {
      VarlistExpandResult vr = expandVarlistImpl(trim(m.str(2)), session);
      if (vr.isError) return ojson::array({vr.error});
      // BUG-509 (corrected)/BUG-544: `foreach v of varlist nosuchcol` is
      // r(111) "variable nosuchcol not found", caught BEFORE any iteration --
      // the entry's original r(199) pin was an artifact of the oracle's
      // `capture noisily foreach ... {` spelling (the 199 was the orphaned
      // `}`, not the foreach's own rc). Also fires for a MIXED list (oracle
      // fe_mixed: `alpha nosuchcol` never iterates `alpha`). KNOWN DISCLOSED
      // GAP: unique abbreviations (`alph` for `alpha`) are refused r(111)
      // here, while Stata resolves them (oracle fe_abbrev: rc 0, iterates
      // `alpha`) -- resolved now by checkKnownVarTokens (selftest arm
      // W2 loops/BUG-544 fe_abbrev).
      if (auto err = checkKnownVarTokens(vr.tokens, session)) {
        return ojson::array({*err});
      }
      return runForeach(m.str(1), vr.tokens, bodyLines, session, dispatchFn);
    }
  }
  // foreach VAR of numlist NUMLIST
  {
    static const std::regex re(R"(^foreach\s+([\w\x80-\xff]+)\s+of\s+numlist\s+(.*))",
                                std::regex::icase);
    std::smatch m;
    if (std::regex_search(hdr, m, re)) {
      NumlistResult nr = parseNumlist(trim(m.str(2)));
      if (nr.isError) return ojson::array({nr.error});
      return runForeach(m.str(1), nr.tokens, bodyLines, session, dispatchFn);
    }
  }
  // foreach VAR of local NAME
  {
    static const std::regex re(R"(^foreach\s+([\w\x80-\xff]+)\s+of\s+local\s+([\w\x80-\xff]+)\s*$)",
                                std::regex::icase);
    std::smatch m;
    if (std::regex_search(hdr, m, re)) {
      std::vector<std::string> tokens = splitForeachTokens(session.eval.locals.get(m.str(2), ""));
      return runForeach(m.str(1), tokens, bodyLines, session, dispatchFn);
    }
  }
  // foreach VAR of global NAME
  {
    static const std::regex re(R"(^foreach\s+([\w\x80-\xff]+)\s+of\s+global\s+([\w\x80-\xff]+)\s*$)",
                                std::regex::icase);
    std::smatch m;
    if (std::regex_search(hdr, m, re)) {
      std::vector<std::string> tokens = splitForeachTokens(session.eval.globals.get(m.str(2), ""));
      return runForeach(m.str(1), tokens, bodyLines, session, dispatchFn);
    }
  }
  // foreach VAR of newlist TOKENS (BS-034a, BETA campaign C3) — newlist is
  // a list of NEW variable NAMES (e.g. names not yet created as columns,
  // often built from `tempvar`); unlike `of varlist` above, there is NO
  // existence check against the current dataset — iteration is simply over
  // the tokens as given, same split rule as the plain "in" form (oracle:
  // `foreach v of newlist na nb nc` -> "newvar na" / "newvar nb" /
  // "newvar nc"). Must come before the plain "in" form for the SAME reason
  // the other `of ...` forms do (though there's no actual overlap risk
  // here — "newlist" the literal keyword vs "in" — ordering just mirrors
  // the existing convention).
  {
    static const std::regex re(R"(^foreach\s+([\w\x80-\xff]+)\s+of\s+newlist\s+(.*))",
                                std::regex::icase);
    std::smatch m;
    if (std::regex_search(hdr, m, re)) {
      std::vector<std::string> tokens = splitForeachTokens(trim(m.str(2)));
      return runForeach(m.str(1), tokens, bodyLines, session, dispatchFn);
    }
  }
  // foreach VAR in TOKENS  (must come AFTER the `of` forms)
  {
    // BUG-920: an empty list (`foreach v in {`, the header left as
    // `foreach v in` once the brace is stripped) runs zero times at rc 0
    // (oracle pc_fe_empty_in); `\s+in\s+` needed a token after `in` and sent
    // the bare header to the unrecognised-header error (rc 1).
    static const std::regex re(R"(^foreach\s+([\w\x80-\xff]+)\s+in(?:\s+(.*))?$)", std::regex::icase);
    std::smatch m;
    if (std::regex_search(hdr, m, re)) {
      std::vector<std::string> tokens = splitForeachTokens(trim(m.str(2)));
      return runForeach(m.str(1), tokens, bodyLines, session, dispatchFn);
    }
  }
  // forvalues i = #1 #2 to #3  /  forvalues i = #1 #2 : #3 (BS-034b/c, BETA
  // campaign C3) — Stata's "explicit first two values" numlist form: start
  // = #1, the step is IMPLIED by the gap between the first two values (#2 -
  // #1), end = #3; `to`/`:` are interchangeable separators (oracle:
  // `forvalues i = 1 3 to 9` == `forvalues i = 1 3 : 9` -> 1 3 5 7 9). Must
  // be checked AFTER the `(STEP)` and `/` forms below it structurally
  // cannot ambiguously match this (neither uses bare whitespace as its
  // start/second-number separator) — placed here regardless, matching
  // this file's existing top-to-bottom convention. The already-working
  // `1(3)9` form is untouched — this is a syntactically distinct header.
  {
    static const std::regex re(
        R"(^forvalues\s+([\w\x80-\xff]+)\s*=\s*([+-]?\d+(?:\.\d+)?)\s+([+-]?\d+(?:\.\d+)?)\s*(?:to|:)\s*([+-]?\d+(?:\.\d+)?))",
        std::regex::icase);
    std::smatch m;
    if (std::regex_search(hdr, m, re)) {
      double first = std::stod(m.str(2)), second = std::stod(m.str(3)), end = std::stod(m.str(4));
      double step = second - first;
      if (step == 0.0) {
        return ojson::array({errBlock("forvalues: step cannot be zero")});
      }
      return runForvaluesRange(m.str(1), first, step, end, bodyLines, session,
                               dispatchFn);
    }
  }
  // forvalues i=START(STEP)END
  {
    static const std::regex re(
        R"(^forvalues\s+([\w\x80-\xff]+)\s*=\s*([+-]?\d+(?:\.\d+)?)\s*\(([+-]?\d+(?:\.\d+)?)\)\s*([+-]?\d+(?:\.\d+)?))",
        std::regex::icase);
    std::smatch m;
    if (std::regex_search(hdr, m, re)) {
      double start = std::stod(m.str(2)), step = std::stod(m.str(3)), end = std::stod(m.str(4));
      if (step == 0.0) {
        return ojson::array({errBlock("forvalues: step cannot be zero")});
      }
      return runForvaluesRange(m.str(1), start, step, end, bodyLines, session,
                               dispatchFn);
    }
  }
  // forvalues i=START/END  (step=1)
  {
    static const std::regex re(
        R"(^forvalues\s+([\w\x80-\xff]+)\s*=\s*([+-]?\d+(?:\.\d+)?)\s*/\s*([+-]?\d+(?:\.\d+)?))",
        std::regex::icase);
    std::smatch m;
    if (std::regex_search(hdr, m, re)) {
      // BUG-920: #1/#2 always steps by +1, so `forvalues i = 3/1` runs zero
      // times (oracle pc_fv_empty n1 0; [P] forvalues: "#1/#2 ... in steps
      // of 1"). It used to count down 3 2 1. A numlist's 3/1 does count down
      // (parseNumlist), which is a different rule.
      double start = std::stod(m.str(2)), end = std::stod(m.str(3));
      const double step = 1.0;
      return runForvaluesRange(m.str(1), start, step, end, bodyLines, session,
                               dispatchFn);
    }
  }
  // while EXPR
  {
    static const std::regex re(R"(^while\s+(.*))", std::regex::icase);
    std::smatch m;
    if (std::regex_search(hdr, m, re)) {
      return runWhile(trim(m.str(1)), bodyLines, session, dispatchFn);
    }
  }
  // if EXPR [else { ... }]
  {
    static const std::regex re(R"(^if\s+(.*))", std::regex::icase);
    std::smatch m;
    if (std::regex_search(hdr, m, re)) {
      return runIf(trim(m.str(1)), bodyLines, elseBodyLines, session, dispatchFn);
    }
  }
  // capture { ... } (BS-015) — not a loop/branch; `fw` (computed above) is
  // already the lowered first word of the header, so no re-parsing needed.
  // A prefix chain that holds `capture` (`capture {`, `capture noisily
  // [quietly] {`, `noisily capture {`) is a capture block. BUG-1434, StataBE
  // 19.5 2026-11-03: a noisily anywhere after the capture shows the error
  // (`capture quietly noisily {` and `capture noisily quietly {` both print
  // it), and the LAST of the chain's words decides whether ordinary output
  // shows -- capture and quietly hide it, noisily shows it (`quietly capture
  // noisily {` prints, `noisily capture {` and `capture {` print nothing). A
  // chain without capture is a quietly/noisily block by the same last word.
  {
    std::vector<std::string> words;
    std::string rest = trim(hdr);
    while (!rest.empty()) {
      const size_t sp = rest.find_first_of(" \t");
      words.push_back(lower(sp == std::string::npos ? rest : rest.substr(0, sp)));
      rest = sp == std::string::npos ? std::string() : trim(rest.substr(sp));
    }
    size_t capAt = words.size();
    bool allPrefixes = !words.empty();
    for (size_t i = 0; i < words.size(); ++i) {
      if (isCapturePrefix(words[i]) && capAt == words.size()) capAt = i;
      allPrefixes = allPrefixes && (isCapturePrefix(words[i]) || isQuietlyPrefix(words[i]) ||
                                    isNoisilyPrefix(words[i]));
    }
    if (allPrefixes && capAt < words.size()) {
      bool showError = false;
      for (size_t i = capAt + 1; i < words.size(); ++i) showError = showError || isNoisilyPrefix(words[i]);
      const bool quiet = !isNoisilyPrefix(words.back());
      return runCaptureBlock(bodyLines, session, dispatchFn, showError, quiet);
    }
    if (allPrefixes && words.size() > 1) {
      return runQuietlyBlock(bodyLines, session, dispatchFn, /*quiet=*/!isNoisilyPrefix(words.back()));
    }
  }
  // BUG-132: quietly/noisily { ... } block — not a loop/branch; suppress
  // (quietly) or pass through (noisily) the body's output, but (unlike
  // capture) never swallow an uncaught error. `fw` is the lowered first word;
  // spellings mirror the dispatcher's kQuietlyPrefixes split.
  if (fw == "quietly" || fw == "qui" || fw == "quie" || fw == "quiet") {
    return runQuietlyBlock(bodyLines, session, dispatchFn, /*quiet=*/true);
  }
  if (fw == "noisily" || fw == "noisil" || fw == "noisi" || fw == "nois" || fw == "noi" ||
      fw == "no" || fw == "n") {
    return runQuietlyBlock(bodyLines, session, dispatchFn, /*quiet=*/false);
  }
  // PLEVEL-IMPL-WAVE arc ①: `parallel[, workers(N)] { … }`.
  if (fw == "parallel") {
    return runParallelBlock(hdr, bodyLines, session, dispatchFn);
  }

  return ojson::array({errBlock("loops: unrecognised loop header: '" + hdr + "'")});
}

// ---------------------------------------------------------------------------
// EXPRLAYER drift guard (see loops.h).

bool selftestBoolIdentScanMatchesRegex(std::string* firstDisagreement) {
  static const std::vector<std::string> kCases = {
      "`w' < 250", "w < 250", "mod(i,3)==0", "i==1000", "_rc", "_N > 500", "_n == 1",
      "x1 > 0 & x2 < 1", "__STR0__ == \"a\"", "1e+307 > x", "1.5e-9 < y", "a.b",
      "_x9 + x_9", "abc", "", "   ", "3", "3x", "x3", "e(N) > 0", "scalar(q) != .",
      "__STR10__", "__STRx", "not x", "x or y", "8.988465674311579e+307",
      "f(1)+g2(3)", "_", "__", "___STR0__", "1_x", "x1e5", "9e", "a1b2c3",
      // Unicode names (lane M): a UTF-8 name is one token, the mixed-script
      // `دخل_2020` included (its ASCII tail is NOT a token of its own), and the
      // digit / `.` exponent-tail rule applies to a high byte the same way.
      "قيمة > 0", "دخل_2020 > 1", "x قيمة", "1ق", "ق.ب", "ق1 + _ق", "_2020 + دخل_2020",
      "xملاحظات", "ملاحظات1x", "cond(قيمة>0,1,0)", "__STR0__ == ق",
  };
  static const std::regex reIdentTok(R"([A-Za-z_\x80-\xff][\w\x80-\xff]*)");
  for (const auto& c : kCases) {
    // The regex arm — verbatim the loop this replaced.
    std::set<std::string> viaRegex;
    for (auto it = std::sregex_iterator(c.begin(), c.end(), reIdentTok);
         it != std::sregex_iterator(); ++it) {
      const size_t pos = static_cast<size_t>(it->position());
      if (pos > 0 &&
          (std::isdigit(static_cast<unsigned char>(c[pos - 1])) || c[pos - 1] == '.')) {
        continue;
      }
      std::string tok = it->str();
      if (tok.rfind("__STR", 0) == 0) continue;
      viaRegex.insert(std::move(tok));
    }
    // The scan arm — verbatim the loop in evalBoolExpr.
    std::set<std::string> viaScan;
    const size_t n = c.size();
    for (size_t i = 0; i < n;) {
      if (!cecon::ident::isIdentStartByte(c[i])) {
        ++i;
        continue;
      }
      size_t j = i + 1;
      while (j < n && cecon::ident::isIdentByte(c[j])) ++j;
      const bool expTail =
          i > 0 && (std::isdigit(static_cast<unsigned char>(c[i - 1])) || c[i - 1] == '.');
      if (!expTail && c.compare(i, 5, "__STR") != 0) viaScan.insert(c.substr(i, j - i));
      i = j;
    }
    if (viaRegex != viaScan) {
      if (firstDisagreement) {
        std::string a, b;
        for (const auto& t : viaRegex) a += t + " ";
        for (const auto& t : viaScan) b += t + " ";
        *firstDisagreement = "line=[" + c + "] regex={" + a + "} scan={" + b + "}";
      }
      return false;
    }
  }
  return true;
}

}  // namespace piet::loops
