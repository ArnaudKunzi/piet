// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// AER tier-1 (2026-10-28), lane META: verbs that read or rename, never compute -- findname
// xtdescribe cf fvunab numlabel labvars renpfix.
//
// Registry rows, dispatcher arms (dispatcher.cpp kTierOne), handlers.h declarations,
// effects/floors rows, NOTICE lines and pangram lines landed with the shared-surface unit;
// this file is the lane's alone. Answers: the StataBE 19.5 captures w2ax_t1_official.log
// (xtdes_* cf_* fvunab_* numlabel_* renpfix_*) and w2ax_t1_community.log (findname_*
// labvars_*), plus the lane's own extra probes named at each rule below (mx1..mx5, run
// on the same StataBE build while this lane was written).
//
// NOTHING HERE COMPUTES A SECOND TIME WHAT PIET ALREADY COMPUTES.
//   findname  variable labels through the `: variable label` projection
//             (SessionState::syncEvalVarLabels); the glob is strutil::globMatch.
//   xtdescribe  the T_i distribution's r() is `summarize T_i, detail` -- the SAME
//             summarizeCoreStats every `summarize, detail` runs, over a one-column scratch
//             frame, and captured through eval::captureRFromBlock; the qualifier goes
//             through applyIfAndInFilter; every number printed goes through
//             eval::applyStataFmt.
//   cf        the using file is read the way merge reads one (cobra import-dta into a
//             scratch hbin), never by preserve/use/restore of the session frame; Stata's
//             `!=` (extended missings included) is cecon::stata_cmp.
//   fvunab    the varlist half is loops::expandVarlist (the `unab` arm's machinery) plus
//             display_hardening's resolveVarName for abbreviations; only the operator
//             prefixes are parsed here, because no other surface normalizes them.
//   numlabel  every write is a `label define ..., modify` through runLabelDefine, so
//             both label stores (labelDefs and the per-variable valueLabels) and the
//             display-format widening move together, exactly as `label define` moves them.
//   labvars   runLabelVariable.
//   renpfix   a loop over runRename, ONE mutationResult for the renames done.
//
// THE DEFECT CLASSES (handlers_aer_util.cpp's header names them): every block returned is a
// typed object or an array of typed objects (SHAPE); every error with a known Stata rc
// carries "r(NNN)" in `reason` (RC); every option read is registered at namespace scope
// below and every handler calls the reject net first (VOCABULARY); r() is filled here, not
// by a dispatcher capture (R-CLASS).
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <fstream>
#include <iterator>
#include <map>
#include <memory>
#include <optional>
#include <regex>
#include <set>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "data/missing.h"  // cecon::stata_cmp / missing_kind -- Stata's `!=`, extended missings
#include "io/win_compat.h"  // cecon::to_native_path / native_unlink
#include "piet/cobra_runner.h"
#include "piet/column_fetch.h"
#include "piet/datastore.h"
#include "piet/display_hardening.h"  // handleDisplay (display's default number text), resolveVarName
#include "piet/eval/errors.h"
#include "piet/eval/exprfns.h"  // applyStataFmt, fnAbbrev
#include "piet/eval/macros.h"   // captureRFromBlock
#include "piet/eval/pyvalue.h"  // pythonFloatRepr / PyValue
#include "piet/eval/session.h"  // replaceRClass / declareRPosted
#include "piet/eval/tokenizer.h"  // scanOneToken / quotedSpanMask
#include "piet/handlers.h"
#include "piet/inrange_filter.h"
#include "piet/loops.h"  // expandVarlist -- the ONE varlist wildcard/range expander
#include "piet/mutation_result.h"
#include "piet/scratch_seq.h"
#include "piet/session_state.h"
#include "piet/strutil.h"
#include "vendor/nlohmann/json.hpp"

// ---------------------------------------------------------------------------
// OPTION VOCABULARIES -- namespace scope, before any dispatch (defect class 3).
// `known` is what the handler READS; `unimplemented` is a real option of the same
// command this build does not do, refused by name.
namespace {

// findname.ado 1.1.2's syntax line. The selection options this build reads are the
// variable-label pair plus the presentation/result ones; every other selector would
// pick a DIFFERENT set of variables, so each is refused rather than ignored.
const piet::eval::OptVocab& kVocabFindname = piet::eval::registerOptVocab(
    {"findname"},
    {{"varlabeltext", "varlabel", "insensitive", "not", "alpha", "local", "cslocal", "indent",
      "skip", "varwidth"},
     {"type", "all", "any", "format", "columns", "vallabel", "vallabelname", "vallabeltext",
      "vallabeltextdef", "vallabeltextuse", "vallabelcountdef", "vallabelcountuse", "char",
      "charname", "chartext", "placeholder", "detail"}});

// xtdescribe: `i()`/`t()` override the xtset variables (the pre-xtset
// spelling); not read here.
const piet::eval::OptVocab& kVocabXtdescribe = piet::eval::registerOptVocab(
    {"xtdescribe"}, {{"patterns", "width"}, {"i", "t"}});

// cf: `tolerance()` (undocumented) compares by reldif instead of `!=`.
const piet::eval::OptVocab& kVocabCf =
    piet::eval::registerOptVocab({"cf"}, {{"verbose", "all"}, {"tolerance"}});

// fvunab's own options.
const piet::eval::OptVocab& kVocabFvunab =
    piet::eval::registerOptVocab({"fvunab"}, {{"min", "max", "name"}, {}});

// numlabel's syntax line, all read.
const piet::eval::OptVocab& kVocabNumlabel = piet::eval::registerOptVocab(
    {"numlabel"}, {{"add", "remove", "mask", "force", "detail"}, {}});

// labvars.ado 1.0.5's syntax line, all read.
const piet::eval::OptVocab& kVocabLabvars = piet::eval::registerOptVocab(
    {"labvars"}, {{"remove", "carryon", "alternate", "names"}, {}});

// renpfix.ado takes no option at all: anything after the comma is r(198).
const piet::eval::OptVocab& kVocabRenpfix =
    piet::eval::registerOptVocab({"renpfix"}, {{}, {}});

}  // namespace

namespace piet::handlers {

using ojson = nlohmann::ordered_json;

namespace {

// ===========================================================================
// Blocks and errors
// ===========================================================================

// An error with a KNOWN Stata rc: `reason` is the only channel carrying an exact rc.
ojson rcError(const std::string& message, int rc) {
  return ojson(eval::errorBlock("engine_error", message, std::nullopt, std::nullopt, std::nullopt,
                                "r(" + std::to_string(rc) + ")"));
}

// A real Stata form this build does not implement: refused by name. r(199) is the
// codebase's honest "not implemented" rc (the brief's rule); Stata would have run it.
ojson notImplemented(const std::string& message) { return rcError(message, 199); }

ojson displayBlock(const std::string& text) {
  ojson b;
  b["type"] = "display";
  b["text"] = text;
  return b;
}

ojson silent() { return ojson::array(); }

bool isErrorBlock(const ojson& b) {
  return b.is_object() && b.value("type", std::string()) == "error";
}

const ojson* firstError(const ojson& result) {
  if (isErrorBlock(result)) return &result;
  if (result.is_array())
    for (const auto& b : result)
      if (isErrorBlock(b)) return &b;
  return nullptr;
}

// ===========================================================================
// Options
// ===========================================================================

bool hasFlag(const eval::ParsedCommand& p, const std::string& name) {
  for (const auto& [k, v] : p.opts)
    if (k == name) return !v.isBool || v.boolValue;
  return false;
}

std::optional<std::string> optStr(const eval::ParsedCommand& p, const std::string& name) {
  for (const auto& [k, v] : p.opts)
    if (k == name) return v.isBool ? std::string() : v.strValue;
  return std::nullopt;
}

// STRICT integer: the whole token must parse.
std::optional<long long> strictInt(const std::string& s) {
  const std::string t = strutil::trim(s);
  if (t.empty()) return std::nullopt;
  size_t i = 0;
  if (t[0] == '-' || t[0] == '+') i = 1;
  if (i >= t.size()) return std::nullopt;
  for (size_t j = i; j < t.size(); ++j)
    if (!std::isdigit(static_cast<unsigned char>(t[j]))) return std::nullopt;
  try {
    return std::stoll(t);
  } catch (const std::exception&) {
    return std::nullopt;
  }
}

// An integer option `name(#)`, or `fallback` when absent; nullopt + error when malformed.
std::optional<long long> intOption(const eval::ParsedCommand& p, const std::string& name,
                                   long long fallback, ojson* err) {
  auto v = optStr(p, name);
  if (!v.has_value()) return fallback;
  auto n = strictInt(*v);
  if (!n.has_value()) {
    *err = rcError("option " + name + "() incorrectly specified", 198);
    return std::nullopt;
  }
  return n;
}

// Stata's word tokenization (tokenize / Mata tokens()): blanks separate, a "..." or a
// compound `"..."' is one word with its quotes removed. eval::scanOneToken is the
// codebase's one scanner for exactly that; `"a""b"` is two words, as Stata counts them
// (w2ax_t1_community.log labvars_dq: `"with ""inner"" quote"` is 3 labels).
std::vector<std::string> stataWords(const std::string& text) {
  std::vector<std::string> out;
  std::string rest = text;
  while (!strutil::trim(rest).empty()) {
    auto [tok, rem] = eval::scanOneToken(rest, /*bind=*/true);
    out.push_back(tok);
    if (rem.size() >= rest.size()) break;  // no progress -- never loop forever
    rest = rem;
  }
  return out;
}

std::string joinWords(const std::vector<std::string>& v, const std::string& sep = " ") {
  std::string out;
  for (size_t i = 0; i < v.size(); ++i) {
    if (i) out += sep;
    out += v[i];
  }
  return out;
}

std::string padTo(std::string s, size_t col) {  // _column(col+1): pad to `col` characters
  if (s.size() < col) s.append(col - s.size(), ' ');
  return s;
}

std::string rightAlign(const std::string& s, size_t w) {
  return s.size() >= w ? s : std::string(w - s.size(), ' ') + s;
}

// `display %w.df v`: the format engine's text (eval::applyStataFmt), right-aligned in the
// format's width as display prints it.
std::string fmtNum(double v, const char* fmt) {
  std::string s = eval::applyStataFmt(v, fmt);
  const int w = std::atoi(fmt + 1);
  if (w > 0 && s.size() < static_cast<size_t>(w)) s.insert(0, static_cast<size_t>(w) - s.size(), ' ');
  return s;
}

// Stata's abbrev(s, n) -- the expression function itself (exprfns fnAbbrev), not a copy.
std::string stataAbbrev(const std::string& s, int n) {
  eval::PyValue r = eval::fnAbbrev({eval::PyValue::str(s), eval::PyValue::integer(n)});
  return r.isStr() ? r.strVal() : s;
}

// The text `display <number>` prints for a value (no format): display's own renderer,
// fed the full-precision literal -- nine characters, so a ten-digit id is 1.235e+09
// (review p2/p3/p8: xtdescribe's header and cf's verbose lines both print it that way).
// A missing value prints as its own token.
std::string displayNumber(double v, SessionState& session) {
  const int kind = cecon::missing_kind(v);
  if (kind >= 0) return kind == 0 ? std::string(".") : std::string(".") + char('a' + kind - 1);
  ojson d = handleDisplay(eval::pythonFloatRepr(v), session);
  if (d.is_object() && d.value("type", std::string()) == "display")
    return d.value("text", std::string());
  return eval::pythonFloatRepr(v);
}

// ===========================================================================
// Variables
// ===========================================================================

// Every variable name, dataset order; empty when nothing is loaded.
std::vector<std::string> allVarNames(SessionState& session) {
  if (!session.hasData()) return {};
  if (const std::vector<VarInfo>* card = session.residentReadCard()) {
    std::vector<std::string> out;
    out.reserve(card->size());
    for (const auto& vi : *card) out.push_back(vi.name);
    return out;
  }
  Datastore* ds = nullptr;
  try {
    ds = session.datastore();
  } catch (const std::exception&) {
    ds = nullptr;
  }
  return ds == nullptr ? std::vector<std::string>() : ds->names();
}

// `unab`'s answer for a varlist: wildcards and ranges by loops::expandVarlist (the one
// expander), every literal confirmed to exist or resolved as a unique abbreviation by
// resolveVarName (display's binding rule). Stata's r(111) texts: "variable x not
// found", "x ambiguous abbreviation", "variables out of order" (mx2/mx5 fvunab arms).
std::optional<ojson> unabVarlist(const std::string& spec, SessionState& session,
                                 std::vector<std::string>* out) {
  out->clear();
  const std::vector<std::string> names = allVarNames(session);
  if (names.empty()) return rcError("no variables defined", 111);
  if (spec.find('~') != std::string::npos)
    return notImplemented("the `~` varlist wildcard is not implemented in this version of PIET (" +
                          strutil::trim(spec) + ")");
  // A reversed `b-a` range is r(111) "variables out of order"; expandVarlist answers it
  // with an empty run, so the order is checked on the same collapsed text it reads.
  {
    std::string joined;
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
    static const std::regex reRange(R"(^(\w+)-(\w+)$)");
    std::istringstream iss(joined);
    for (std::string tok; iss >> tok;) {
      std::smatch m;
      if (!std::regex_match(tok, m, reRange)) continue;
      auto a = std::find(names.begin(), names.end(), m.str(1));
      auto b = std::find(names.begin(), names.end(), m.str(2));
      if (a == names.end()) return rcError("variable " + m.str(1) + " not found", 111);
      if (b == names.end()) return rcError("variable " + m.str(2) + " not found", 111);
      if (a > b) return rcError("variables out of order", 111);
    }
  }
  loops::VarlistExpandResult ex = loops::expandVarlist(spec, session);
  if (ex.isError) return ex.error;
  for (const auto& t : ex.tokens) {
    if (std::find(names.begin(), names.end(), t) != names.end()) {
      out->push_back(t);
      continue;
    }
    bool ambiguous = false;
    std::optional<std::string> hit;
    if (strutil::isValidStataIdentifier(t)) hit = resolveVarName(t, session, &ambiguous);
    if (hit.has_value() && std::find(names.begin(), names.end(), *hit) != names.end()) {
      out->push_back(*hit);
      continue;
    }
    if (ambiguous) return rcError(t + " ambiguous abbreviation", 111);
    return rcError("variable " + t + " not found", 111);
  }
  return std::nullopt;
}

// A name Stata reserves: `rename a long` is r(198) "long invalid varname" (mx6.log RV|*:
// _b byte _coef _cons double float if in int long _n _N _pi _pred _rc str# strL using
// with; `_skip`, `strl`, `str0` and `str` are legal). Case-sensitive, as measured.
bool isReservedVarName(const std::string& n) {
  static const std::set<std::string> kReserved = {
      "_b",   "byte",   "_coef", "_cons", "double", "float", "if", "in",
      "int",  "long",   "_n",    "_N",    "_pi",    "_pred", "_rc", "strL",
      "using", "with"};
  if (kReserved.count(n) != 0) return true;
  if (n.size() > 3 && n.compare(0, 3, "str") == 0 && n[3] != '0') {
    for (size_t i = 3; i < n.size(); ++i)
      if (!std::isdigit(static_cast<unsigned char>(n[i]))) return false;
    return true;
  }
  return false;
}

// The variable label `: variable label v` answers -- the one projection describe shares.
std::string varLabelOf(SessionState& session, const std::string& v) {
  const std::string* l = session.eval.varLabels.find(v);
  return l == nullptr ? std::string() : *l;
}

// Read scope over a few named columns: the qualifier (when present) through
// applyIfAndInFilter's filtered scratch copy; otherwise a section-38 projection of just
// those columns, with the materialized head as the fallback (column_fetch.h's law).
class ColumnScope {
 public:
  ColumnScope(SessionState& session, const std::vector<std::string>& cols,
              const std::optional<std::string>& ifExpr, const std::optional<std::string>& inRange,
              const std::string& verb) {
    if (!session.hasData()) {
      err_ = ojson(eval::noDataLoaded());
      return;
    }
    const bool qualified = (ifExpr.has_value() && !strutil::trim(*ifExpr).empty()) ||
                           (inRange.has_value() && !strutil::trim(*inRange).empty());
    try {
      if (qualified) {
        InRangeFilterResult f = applyIfAndInFilter(session, *session.currentHbin(), ifExpr, inRange);
        if (!f.ok) {
          err_ = inRangeRejectError(f, inRange.value_or(std::string()), verb);
          return;
        }
        if (f.dataPath != *session.currentHbin()) {
          session.noteProjectionScratch(f.dataPath);  // BUG-381: read once, abandoned
          owned_ = std::make_unique<Datastore>(f.dataPath);
          ds_ = owned_.get();
        } else {
          ds_ = session.datastore();
        }
        return;
      }
      std::vector<const std::string*> exprs;
      for (const auto& c : cols) exprs.push_back(&c);
      if (auto want = referencedColumns(session, exprs, "")) {
        std::string projPath;
        std::unique_ptr<Datastore> proj = fetchColumns(session, *want, session.workdir(), &projPath);
        if (proj) {
          bool complete = true;
          for (const auto& c : cols)
            if (!proj->has(c)) complete = false;
          if (complete) owned_ = std::move(proj);
        }
      }
      ds_ = owned_ ? owned_.get() : session.datastore();
    } catch (const std::exception& e) {
      err_ = ojson(eval::engineError(verb + ": could not read the data -- " + e.what(), "data_io"));
    }
  }
  Datastore* get() const { return ds_; }
  bool failed() const { return !err_.is_null() || ds_ == nullptr; }
  ojson error() const { return err_.is_null() ? ojson(eval::noDataLoaded()) : err_; }

 private:
  std::unique_ptr<Datastore> owned_;
  Datastore* ds_ = nullptr;
  ojson err_;
};

// One cell as Stata compares it: a string, or a double carrying cecon's missing kinds.
struct Cell {
  bool isStr = false;
  double d = 0;
  std::string s;
};

std::vector<Cell> columnCells(const Datastore& ds, const std::string& name) {
  std::vector<Cell> out;
  const int64_t n = ds.nRows();
  out.reserve(static_cast<size_t>(n));
  const cecon::Column& c = ds.col(name);
  if (c.dtype == cecon::DType::Factor) {
    const int32_t* codes = c.codes();
    for (int64_t i = 0; i < n; ++i) {
      Cell cell;
      cell.isStr = true;
      const int32_t k = codes[i];
      if (k >= 0 && static_cast<size_t>(k) < c.levels.size()) cell.s = c.levels[static_cast<size_t>(k)];
      out.push_back(std::move(cell));
    }
  } else {
    const double* d = c.f64();
    for (int64_t i = 0; i < n; ++i) {
      Cell cell;
      cell.d = d[i];
      out.push_back(std::move(cell));
    }
  }
  return out;
}

// Delete a scratch file this lane minted, now, rather than at the dispatch boundary.
void dropScratch(const std::string& path) {
  if (path.empty()) return;
  cecon::native_unlink(path);
  forgetScratchMint(path);
}

// ===========================================================================
// findname
// ===========================================================================
//
// Implements findname's variable-label selection per its help file (Nicholas J. Cox,
// findname 1.1.2); identified against StataBE probes (w2ax_t1_community.log findname_*,
// mx2.log FN|*):
//   * each varlabeltext() word is a pattern matched against the WHOLE label, `*` any run
//     and `?` one character, case-sensitive unless `insensitive` (findname_case: `P*`
//     finds only "Price 1"); the words are Stata's tokens, so `varlabeltext(price two)`
//     is two patterns and `varlabeltext("")` matches the empty label (FN|unq, FN|emptypat);
//   * a leading varlist restricts the search (findname_varlist), bare findname lists every
//     variable (findname_bare), `not` takes the complement over every variable, `alpha`
//     sorts;
//   * nothing found is rc 0, nothing printed and r() cleared (findname_none); otherwise the
//     names print in columns -- each abbreviated to varwidth() (default 12), column width
//     = longest + skip() (default 2), as many columns as the line holds, filled down first
//     (findname_glob2 `p1  q`, FN|forty, FN|insen `alpha_long~e  b             ...`) --
//     and r(varlist) is the full names. local()/cslocal() copy it to a local;
//   * if/in pick observations, which only the value-based selectors (not implemented here)
//     read -- an empty sample is r(2000) "no observations" all the same; indent() below 0
//     is 0 (review p1 fn_if0, fn_in, fn_indneg).
//
// Line width: PIET has no `set linesize`; the column count uses 134, the c(linesize) of
// the StataBE batch session the oracle and the probes ran under (mx1.log LINESIZE|134|).
constexpr int kLineSize = 134;

std::string displayInColumns(const std::vector<std::string>& items, int indent, int pad) {
  if (items.empty()) return std::string();
  size_t wid = 0;
  for (const auto& x : items) wid = std::max(wid, x.size());
  wid += static_cast<size_t>(pad);
  const long long cols = (kLineSize + 1 - indent) / static_cast<long long>(wid);
  std::vector<std::string> lines;
  if (cols < 2) {
    for (const auto& x : items) lines.push_back(std::string(static_cast<size_t>(indent), ' ') + x);
    return joinWords(lines, "\n");
  }
  const long long n = static_cast<long long>(items.size());
  const long long nLines = (n + cols - 1) / cols;
  for (long long i = 1; i <= nLines; ++i) {
    const long long top = std::min(cols * nLines + i, n);
    std::string line;
    size_t col = static_cast<size_t>(indent);
    for (long long j = i; j <= top; j += nLines) {
      line = padTo(line, col);
      line += items[static_cast<size_t>(j - 1)];
      col += wid;
    }
    lines.push_back(line);
  }
  return joinWords(lines, "\n");
}

}  // namespace

ojson runFindname(const eval::ParsedCommand& parsed, SessionState& session) {
  if (auto err = eval::rejectUnknownOptions(parsed.opts, kVocabFindname.known,
                                            kVocabFindname.unimplemented))
    return ojson(*err);
  // findname is r-class: whatever it finds, r() is replaced (findname_none leaves an
  // empty r(varlist) behind a previous non-empty one). Nothing found posts nothing.
  eval::replaceRClass(session.eval);
  eval::declareRPosted(session.eval, "findname", {});

  ojson perr;
  auto varwidth = intOption(parsed, "varwidth", 12, &perr);
  if (!varwidth) return perr;
  auto skip = intOption(parsed, "skip", 2, &perr);
  if (!skip) return perr;
  auto indent = intOption(parsed, "indent", 0, &perr);
  if (!indent) return perr;
  if (*varwidth < 5 || *varwidth > 32) return rcError("varwidth() should be in the range 5..32", 198);
  if (*skip < 1 || *skip > 10) return rcError("skip() should be in the range 1..10", 198);
  if (*indent < 0) *indent = 0;  // DisplayInCols' own rule (review p1 fn_indneg)
  if (*indent > kLineSize) *indent = kLineSize;

  // if/in select observations, which only the value-based selectors read; with none of
  // those an empty sample is still r(2000) (review p1 fn_if0, fn_in).
  const bool qualified = (parsed.ifExpr.has_value() && !strutil::trim(*parsed.ifExpr).empty()) ||
                         (parsed.inRange.has_value() && !strutil::trim(*parsed.inRange).empty());
  if (qualified) {
    ColumnScope scope(session, {}, parsed.ifExpr, parsed.inRange, "findname");
    if (scope.failed()) return scope.error();
    if (scope.get()->nRows() == 0) return rcError("no observations", 2000);
  }

  const std::vector<std::string> all = allVarNames(session);
  std::vector<std::string> vars;
  if (!parsed.varlist.empty()) {
    if (auto e = unabVarlist(strutil::trim(parsed.argText), session, &vars)) return *e;
  } else {
    vars = all;  // `syntax [varlist]`: no data means no variables, and findname exits
  }

  const bool insensitive = hasFlag(parsed, "insensitive");
  if (hasFlag(parsed, "varlabel") || optStr(parsed, "varlabeltext").has_value())
    session.syncEvalVarLabels();
  if (hasFlag(parsed, "varlabel")) {
    std::vector<std::string> keep;
    for (const auto& v : vars)
      if (!varLabelOf(session, v).empty()) keep.push_back(v);
    vars = keep;
  }
  if (auto text = optStr(parsed, "varlabeltext")) {
    std::vector<std::string> patterns = stataWords(*text);
    // `varlabeltext("")`: one empty pattern, which matches an empty label (FN|emptypat).
    if (patterns.empty() && !strutil::trim(*text).empty()) patterns.emplace_back();
    if (insensitive)
      for (auto& p : patterns) p = strutil::lower(p);
    std::vector<std::string> keep;
    for (const auto& v : vars) {
      std::string lbl = varLabelOf(session, v);
      if (insensitive) lbl = strutil::lower(lbl);
      for (const auto& p : patterns) {
        if (strutil::globMatch(p, lbl)) {
          keep.push_back(v);
          break;
        }
      }
    }
    vars = keep;
  }
  if (hasFlag(parsed, "not")) {
    std::vector<std::string> comp;
    for (const auto& v : all)
      if (std::find(vars.begin(), vars.end(), v) == vars.end()) comp.push_back(v);
    vars = comp;
  }
  if (vars.empty()) return silent();
  if (hasFlag(parsed, "alpha")) std::sort(vars.begin(), vars.end());

  std::vector<std::string> shown;
  shown.reserve(vars.size());
  for (const auto& v : vars) shown.push_back(stataAbbrev(v, static_cast<int>(*varwidth)));
  ojson out = ojson::array();
  out.push_back(displayBlock(displayInColumns(shown, static_cast<int>(*indent),
                                              static_cast<int>(*skip))));

  const std::string joined = joinWords(vars);
  session.eval.rStrings.set("varlist", joined);
  eval::declareRPosted(session.eval, "findname", {"varlist"});
  const std::string loc = strutil::trim(optStr(parsed, "local").value_or(std::string()));
  const std::string csloc = strutil::trim(optStr(parsed, "cslocal").value_or(std::string()));
  if (!loc.empty()) session.eval.locals.set(loc, joined);
  if (!csloc.empty()) {
    if (csloc == loc) out.push_back(displayBlock("\nnote: cslocal() result overwrites local() result"));
    session.eval.locals.set(csloc, joinWords(vars, ","));
  }
  return out;
}

// ===========================================================================
// xtdescribe
// ===========================================================================
//
// Implements xtdescribe per its help file; identified against StataBE probes
// (w2ax_t1_official.log xtdes_*, mx4.log XD|*):
//   * not xtset -> r(459) "must specify panelvar and timevar; use xtset"; a panel but no
//     time variable -> r(459) "must specify timevar; use xtset" (xtdes_notset/notime);
//   * the sample is if/in and a non-missing panel id; fewer than two distinct non-missing
//     times in it -> r(459) "<t> must have multiple distinct nonmissing values" (XD|ifnone,
//     XD|onet);
//   * T_i is each panel's observation count in the sample (a missing time counts, XD|nonuniq),
//     and r() is `summarize T_i, detail` over the panels (xtdes_basic r(N)=10 r(sum)=51
//     r(p50)=5.5 -- printed %6.0f as 6);
//   * the header: id range, n, the time range in the time variable's display format
//     (XD|tq `2000q1, 2000q2, ..., 2002q2`), T = distinct times (a missing time is one
//     more, XD|nonuniq T 6), Delta, Span = periods from first to last;
//   * width(2) is noted ("width must be a positive integer greater than 2") and used, binning
//     two periods a column; below 2 is refused (review p2 xd_w2);
//   * patterns(#) (default 9, 0 = no table, >= 1000 = all, < 0 r(198)): the times are put
//     on the delta grid, one column per period from the first to the last (several periods
//     per column only when that exceeds width(#), with a `*` note); a panel's pattern is its
//     count per column ('.', the digit, X above 9); patterns are listed by frequency, ties in
//     ascending order of the pattern itself ('.' before '1': xtdes_basic `..1111` before
//     `11.111`), the rest pooled as "(other patterns)", then the total line where a column
//     any panel reaches is X.
namespace {

// `summarize name, detail` over `values`, through the summarize computation itself: the
// values become a one-column scratch frame (cobra convert) and summarizeCoreStats reads
// it. Returns the summarize block (or an error block).
ojson summarizeDetailOf(const std::vector<double>& values, const std::string& name,
                        SessionState& session) {
  const std::string base = mintScratchPath(session.workdir(), "t1xtdes");
  const std::string csv = base + ".csv";
  registerScratchMint(csv);
  {
    std::ofstream f(cecon::to_native_path(csv), std::ios::binary);
    if (!f) return ojson(eval::engineError("xtdescribe: cannot stage the T_i column", "data_io"));
    f << name << "\n";
    for (double v : values) f << (std::isnan(v) ? std::string() : eval::pythonFloatRepr(v)) << "\n";
    f.flush();
    if (!f) {
      dropScratch(csv);
      return ojson(eval::engineError("xtdescribe: cannot stage the T_i column", "data_io"));
    }
  }
  CobraRunResult cr = runCobra({"convert", "--data", csv, "--out", base, "--delim", ","});
  dropScratch(csv);
  if (!cr.ok) {
    dropScratch(base);
    return ojson(eval::engineError("xtdescribe: could not build the T_i frame -- " +
                                       strutil::trim(cr.stderr_text.empty() ? cr.error : cr.stderr_text),
                                   "data_io"));
  }
  ojson block;
  try {
    Datastore tmp(base);
    SessionState::StatelessRead stateless(session);
    block = summarizeCoreStats(tmp, std::vector<std::string>{name}, /*detail=*/true);
  } catch (const std::exception& e) {
    block = ojson(eval::engineError(std::string("xtdescribe: ") + e.what(), "data_io"));
  }
  dropScratch(base);
  return block;
}

// Stata orders patterns by (-freq, the pattern's columns ascending).
struct Pattern {
  std::vector<long long> cols;
  long long freq = 0;
};

}  // namespace

ojson runXtdescribe(const eval::ParsedCommand& parsed, SessionState& session) {
  if (auto err = eval::rejectUnknownOptions(parsed.opts, kVocabXtdescribe.known,
                                            kVocabXtdescribe.unimplemented))
    return ojson(*err);
  if (!session.hasData()) return ojson(eval::noDataLoaded());
  ojson perr;
  auto patterns = intOption(parsed, "patterns", 9, &perr);
  if (!patterns) return perr;
  auto width = intOption(parsed, "width", 100, &perr);
  if (!width) return perr;
  if (*patterns < 0) return rcError("invalid syntax", 198);  // XD|patneg
  const bool allPatterns = *patterns >= 1000;
  // width(2) is reported and then used (review p2 xd_w2: the note, then a table binned two
  // periods a column). Below 2 the column width divides by zero in Stata itself; refused.
  if (*patterns != 0 && *width < 2)
    return rcError("width must be a positive integer greater than 2", 198);
  const bool widthNote = *patterns != 0 && *width == 2;

  // From the panel check on, r() is replaced (review p2 xd_rnotset, p9 xd_r_notime: a
  // 459 leaves r(N) empty behind an earlier summarize).
  eval::replaceRClass(session.eval);
  eval::declareRPosted(session.eval, "xtdescribe", {});
  const bool havePanel = session.panelvar.has_value() && !session.panelvar->empty();
  const bool haveTime = session.timevar.has_value() && !session.timevar->empty();
  if (!havePanel && !haveTime)
    return rcError("must specify panelvar and timevar; use xtset", 459);
  if (!havePanel) return rcError("must specify panelvar; use xtset", 459);
  if (!haveTime) return rcError("must specify timevar; use xtset", 459);
  const std::string ivar = *session.panelvar;
  const std::string tvar = *session.timevar;
  const double delta = session.delta.value_or(1.0);

  // tmin over the WHOLE data (xtset's r(tmin)): the delta grid's origin.
  double tminAll = NAN;
  std::string tfmt = "%9.0g";
  {
    Datastore* full = session.datastore();
    if (full == nullptr) return ojson(eval::noDataLoaded());
    if (!full->has(ivar)) return rcError("variable " + ivar + " not found", 111);
    if (!full->has(tvar)) return rcError("variable " + tvar + " not found", 111);
    if (full->dtype(ivar) != "f64" || full->dtype(tvar) != "f64")
      return rcError("xtdescribe: the panel and time variables must be numeric", 109);
    const double* t = full->col(tvar).f64();
    for (int64_t i = 0; i < full->nRows(); ++i)
      if (!std::isnan(t[i]) && (std::isnan(tminAll) || t[i] < tminAll)) tminAll = t[i];
    const std::vector<VarInfo> card = full->formatCard(std::vector<std::string>{tvar});
    if (!card.empty()) tfmt = session.displayFormat(card.front());
  }

  ColumnScope scope(session, {ivar, tvar}, parsed.ifExpr, parsed.inRange, "xtdescribe");
  if (scope.failed()) return scope.error();
  Datastore& ds = *scope.get();
  const int64_t n = ds.nRows();
  const double* idc = ds.col(ivar).f64();
  const double* tc = ds.col(tvar).f64();

  // The sample: a non-missing panel id. tg = the time on the delta grid.
  struct Obs {
    double id, t, tg;
  };
  std::vector<Obs> obs;
  obs.reserve(static_cast<size_t>(n));
  for (int64_t i = 0; i < n; ++i) {
    if (std::isnan(idc[i])) continue;
    const double tg = std::isnan(tc[i]) ? tc[i] : std::trunc((tc[i] - tminAll) / delta);
    obs.push_back({idc[i], tc[i], tg});
  }

  // `tab t`: fewer than two distinct non-missing times is r(459), with tab's r(N)/r(r)
  // left behind as Stata leaves them (XD|ifnone r(N) 0, XD|onet r(N) 5).
  std::set<double> distinctT;
  long long nT = 0;
  for (const auto& o : obs)
    if (!std::isnan(o.tg)) {
      distinctT.insert(o.tg);
      ++nT;
    }
  if (distinctT.size() < 2) {
    eval::replaceRClass(session.eval);
    session.eval.rValues.set("N", static_cast<double>(nT));
    session.eval.rValues.set("r", static_cast<double>(distinctT.size()));
    eval::declareRPosted(session.eval, "xtdescribe", {"N", "r"});
    return rcError(tvar + " must have multiple distinct nonmissing values", 459);
  }

  // Times: T counts the distinct values the sorted time column carries, a missing value
  // (of each kind) one more (XD|nonuniq).
  std::set<int> missKinds;
  for (const auto& o : obs)
    if (std::isnan(o.t)) missKinds.insert(cecon::missing_kind(o.t));
  const long long T = static_cast<long long>(distinctT.size() + missKinds.size());
  const double tv0 = *distinctT.begin(), tv1 = *distinctT.rbegin();
  const double tval2 = *std::next(distinctT.begin());
  const long long span = static_cast<long long>(tv1 - tv0) + 1;
  // The real-time edges for the header.
  double realFirst = NAN, realSecond = NAN, realLast = NAN;
  for (const auto& o : obs) {
    if (std::isnan(o.tg)) continue;
    if (o.tg == tv0 && (std::isnan(realFirst) || o.t < realFirst)) realFirst = o.t;
    if (o.tg == tval2 && (std::isnan(realSecond) || o.t < realSecond)) realSecond = o.t;
    if (std::isnan(realLast) || o.t > realLast) realLast = o.t;
  }
  double dtv = INFINITY;
  for (auto it = std::next(distinctT.begin()); it != distinctT.end(); ++it)
    dtv = std::min(dtv, *it - *std::prev(it));
  if (*patterns != 0 && static_cast<long long>(std::trunc((tv1 - tv0) / dtv)) + 1 > *width)
    dtv = std::trunc((tv1 - tv0) / static_cast<double>(*width - 1));

  // Panels, ascending id; T_i and uniqueness of (id, t).
  std::map<double, std::vector<double>> byPanel;  // id -> grid times (NaN kept)
  for (const auto& o : obs) byPanel[o.id].push_back(o.tg);
  bool unique = true;
  for (const auto& [id, ts] : byPanel) {
    std::vector<double> s = ts;
    std::sort(s.begin(), s.end(), [](double a, double b) { return cecon::stata_cmp(a, b) < 0; });
    for (size_t k = 1; k < s.size(); ++k)
      if (cecon::stata_cmp(s[k], s[k - 1]) == 0) unique = false;
  }
  const long long nPanels = static_cast<long long>(byPanel.size());
  const double val1 = byPanel.begin()->first, valn = byPanel.rbegin()->first;
  const double val2 = nPanels > 1 ? std::next(byPanel.begin())->first : NAN;
  std::vector<double> ti;
  ti.reserve(byPanel.size());
  for (const auto& kv : byPanel) ti.push_back(static_cast<double>(kv.second.size()));

  // r(): summarize T_i, detail -- the summarize computation itself.
  ojson summ = summarizeDetailOf(ti, "T_i", session);
  if (isErrorBlock(summ)) return summ;
  eval::replaceRClass(session.eval);  // summarize replaces ALL of r(), macros included
  eval::captureRFromBlock(summ, session.eval);
  {
    std::vector<std::string> names = {"N", "mean", "min", "max", "sum_w", "sum", "Var", "sd",
                                      "skewness", "kurtosis", "p1", "p5", "p10", "p25",
                                      "p50", "p75", "p90", "p95", "p99"};
    // A name summarize posts whose value is undefined here (one panel: sd, Var,
    // skewness, kurtosis) is posted missing, as summarize posts it (XD|onepanel `.`).
    for (const auto& nm : names)
      if (session.eval.rValues.find(nm) == nullptr)
        session.eval.rValues.set(nm, eval::kStataMissingSentinel);
    eval::declareRPosted(session.eval, "xtdescribe", names);
  }
  auto rv = [&](const char* k) {
    const double* v = session.eval.rValues.find(k);
    return v == nullptr ? NAN : *v;
  };

  // ---- the header ----------------------------------------------------------
  std::vector<std::string> lines;
  if (widthNote) lines.push_back("width must be a positive integer greater than 2");
  lines.emplace_back();  // the leading `di`
  auto label8 = [](const std::string& name) {
    const long long skipN = 8 - static_cast<long long>(name.size());
    return std::string(static_cast<size_t>(std::max(0LL, skipN)), ' ') + name + ":  ";
  };
  {
    std::string l = label8(ivar) + displayNumber(val1, session) + ", " +
                    displayNumber(val2, session) + ", ..., " + displayNumber(valn, session);
    l = padTo(l, 61) + "n =" + fmtNum(static_cast<double>(nPanels), "%11.0f");
    lines.push_back(l);
  }
  {
    // xtdescribe's PrettyPrintT: up to three lines of the time range.
    auto fmtT = [&](double v) { return strutil::trim(eval::applyStataFmt(v, tfmt)); };
    std::string line1 = fmtT(realFirst) + ",", line2, line3;
    const std::string per2 = fmtT(realSecond);
    int lincnt = 1;
    if (line1.size() + per2.size() < 46) {
      line1 += " " + per2 + ", ...,";
    } else {
      line2 = per2 + ", ...,";
      lincnt = 2;
    }
    const std::string pern = fmtT(realLast);
    std::string& cur = lincnt == 1 ? line1 : line2;
    if (cur.size() + pern.size() < 50) {
      cur += " " + pern;
    } else if (lincnt == 1) {
      line2 = pern;
    } else {
      line3 = pern;
    }
    std::string l = padTo(label8(tvar) + line1, 61) + "T =" + fmtNum(static_cast<double>(T), "%11.0f");
    lines.push_back(l);
    if (!line2.empty()) lines.push_back(std::string(11, ' ') + line2);
    if (!line3.empty()) lines.push_back(std::string(11, ' ') + line3);
  }
  lines.push_back(std::string(11, ' ') + "Delta(" + tvar + ") = " + tsDeltaText(delta, tfmt));
  lines.push_back(std::string(11, ' ') + "Span(" + tvar + ")  = " + std::to_string(span) + " periods");
  lines.push_back(std::string(11, ' ') + "(" + ivar + "*" + tvar +
                  (unique ? " uniquely identifies each observation)"
                          : " does not uniquely identify observations)"));
  lines.emplace_back();
  lines.push_back("Distribution of T_i:   min      5%     25%       50%       75%     95%     max");
  lines.push_back(std::string(20, ' ') + fmtNum(rv("min"), "%6.0f") + "  " + fmtNum(rv("p5"), "%6.0f") +
                  "  " + fmtNum(rv("p25"), "%6.0f") + "    " + fmtNum(rv("p50"), "%6.0f") + "    " +
                  fmtNum(rv("p75"), "%6.0f") + "  " + fmtNum(rv("p95"), "%6.0f") + "  " +
                  fmtNum(rv("max"), "%6.0f"));
  if (*patterns == 0) return displayBlock(joinWords(lines, "\n"));

  // ---- the pattern table ---------------------------------------------------
  std::vector<std::pair<double, double>> bins;  // (l, u]: l < t <= u
  for (double l = tv0 - 0.5; l < tv1; l += dtv) bins.emplace_back(l, l + dtv);
  std::map<std::vector<long long>, long long> freq;
  for (const auto& [id, ts] : byPanel) {
    std::vector<long long> c(bins.size(), 0);
    for (double t : ts) {
      if (std::isnan(t)) continue;
      for (size_t b = 0; b < bins.size(); ++b)
        if (bins[b].first < t && t <= bins[b].second) ++c[b];
    }
    ++freq[c];
  }
  std::vector<Pattern> pats;
  for (const auto& [c, f] : freq) pats.push_back({c, f});
  std::stable_sort(pats.begin(), pats.end(), [](const Pattern& a, const Pattern& b) {
    if (a.freq != b.freq) return a.freq > b.freq;
    return a.cols < b.cols;
  });
  const bool binned = dtv != 1.0;
  const std::string patsMark = binned ? "*" : "";
  const long long dup = std::max<long long>(
      static_cast<long long>(std::trunc((tv1 - tv0) / dtv + 1.5)),
      static_cast<long long>(std::string("Pattern" + patsMark).size()));
  const std::string rule = " " + std::string(27, '-') + "+--" + std::string(static_cast<size_t>(dup), '-');
  lines.emplace_back();
  lines.push_back("     Freq.  Percent    Cum. |  Pattern" + patsMark);
  lines.push_back(rule);
  const double total = static_cast<double>(nPanels);
  const size_t shownN = allPatterns ? pats.size()
                                    : std::min(pats.size(), static_cast<size_t>(*patterns));
  long long cum = 0;
  for (size_t i = 0; i < shownN; ++i) {
    cum += pats[i].freq;
    std::string l = fmtNum(static_cast<double>(pats[i].freq), "%9.0f") +
                    fmtNum(100.0 * static_cast<double>(pats[i].freq) / total, "%10.2f") +
                    fmtNum(100.0 * static_cast<double>(cum) / total, "%8.2f") + " |  ";
    for (long long c : pats[i].cols) l += c == 0 ? "." : (c <= 9 ? std::to_string(c) : "X");
    lines.push_back(l);
  }
  if (shownN < pats.size()) {
    const double rest = total - static_cast<double>(cum);
    lines.push_back(fmtNum(rest, "%9.0f") + fmtNum(100.0 * rest / total, "%10.2f") +
                    fmtNum(100.0, "%8.2f") + " | (other patterns)");
  }
  lines.push_back(rule);
  {
    std::string l = fmtNum(total, "%9.0f") + fmtNum(100.0, "%10.2f") + std::string(8, ' ') + " |  ";
    for (size_t b = 0; b < bins.size(); ++b) {
      bool any = false;
      for (const auto& p : pats) any = any || p.cols[b] != 0;
      l += any ? "X" : ".";
    }
    lines.push_back(l);
  }
  if (binned) {
    // {c BT} prints as `-` in text output (review p2 xd_binned), and the final `di` is then a
    // blank line of its own; unbinned, that `di` only ends the totals line.
    lines.push_back(" " + std::string(30 + static_cast<size_t>(dup), '-'));
    lines.push_back(" *Each column represents " + std::to_string(static_cast<long long>(dtv)) +
                    " periods.");
    lines.emplace_back();
  }
  return displayBlock(joinWords(lines, "\n"));
}

// ===========================================================================
// cf
// ===========================================================================
//
// Implements cf per its help file; identified against StataBE probes
// (w2ax_t1_official.log cf_*, mx3.log CF|*):
//   * `cf varlist using file [, verbose all]`; no `using` is r(100) "using required", no
//     varlist r(100) "varlist required"; the using file takes .dta when it has no extension
//     and a missing one is r(601) "file <name>.dta not found" (cf_nofile);
//   * a different _N is r(9) "master has # obs, using #" before any variable (cf_nobs);
//   * per variable, in varlist order, the label is abbrev(name,16) right-aligned in 18:
//     absent from using -> "does not exist in using" (cf_notinusing); string against
//     numeric -> "<type> in master but <type> in using" (CF|typemis); otherwise the
//     count of observations with master != using under Stata's `!=` (float against
//     double differs, .b against .a differs: CF|dblvsfloat, CF|verbose) -> "# mismatch(es)",
//     with `verbose` one "obs #. <master> in master; <using> in using" line each; `all`
//     also prints "match" for an identical variable (CF|all);
//   * any difference -> r(9); r(Nsum) (a macro, as cf posts it) is the total mismatch count
//     whenever the variable loop ran (cf_diff 9|2, CF|typemis 9|0) and absent when cf
//     stopped before it.
// The current data are never touched: the using file is converted into a scratch frame of
// its own (merge's using reader), read, and dropped.
namespace {

// The storage type `describe`/`: type` report for a master variable.
std::string masterType(SessionState& session, const Datastore& ds, const std::string& v) {
  auto it = session.varMeta.find(v);
  if (it != session.varMeta.end() && it->second.stataType.has_value() &&
      !it->second.stataType->empty())
    return *it->second.stataType;
  if (ds.dtype(v) != "factor") return "float";
  size_t w = 1;
  for (const auto& c : columnCells(ds, v)) w = std::max(w, c.s.size());
  return "str" + std::to_string(w);
}

bool fileExistsAt(const std::string& path) {
  std::ifstream f(cecon::to_native_path(path), std::ios::binary);
  return static_cast<bool>(f);
}

bool looksLikeDta(const std::string& path) {
  std::ifstream input(cecon::to_native_path(path), std::ios::binary);
  char header[11] = {};
  input.read(header, sizeof header);
  const auto size = input.gcount();
  const bool xml = size == 11 && std::string(header, 11) == "<stata_dta>";
  const auto version = static_cast<unsigned char>(header[0]);
  const bool legacy = size >= 3 && (version == 113 || version == 114 || version == 115) &&
                      (header[1] == 1 || header[1] == 2) && header[2] == 1;
  return xml || legacy;
}

}  // namespace

ojson runCf(const eval::ParsedCommand& parsed, SessionState& session) {
  if (auto err = eval::rejectUnknownOptions(parsed.opts, kVocabCf.known, kVocabCf.unimplemented))
    return ojson(*err);
  const bool verbose = hasFlag(parsed, "verbose");
  const bool all = hasFlag(parsed, "all");
  // cf is r-class: a stop before its `return` leaves r() empty (mx3.log CF|novar `.`).
  eval::replaceRClass(session.eval);
  eval::declareRPosted(session.eval, "cf", {});

  // `varlist using filename`: split at the first unquoted `using` word.
  const std::string text = strutil::trim(parsed.argText);
  std::string vlText, fileText;
  bool haveUsing = false;
  {
    const std::vector<bool> quoted = eval::quotedSpanMask(text);
    for (size_t i = 0; i + 5 <= text.size(); ++i) {
      if (quoted[i] || text.compare(i, 5, "using") != 0) continue;
      const bool leftOk = i == 0 || std::isspace(static_cast<unsigned char>(text[i - 1]));
      const bool rightOk = i + 5 == text.size() || std::isspace(static_cast<unsigned char>(text[i + 5]));
      if (!leftOk || !rightOk) continue;
      vlText = strutil::trim(text.substr(0, i));
      fileText = strutil::trim(text.substr(i + 5));
      haveUsing = true;
      break;
    }
    if (!haveUsing) vlText = text;
  }
  if (vlText.empty()) return rcError("varlist required", 100);
  std::vector<std::string> vars;
  if (auto e = unabVarlist(vlText, session, &vars)) return *e;
  {
    // A variable named twice is r(198) (review p3 cf_dup `cf x x using f`).
    std::set<std::string> seen;
    for (const auto& v : vars)
      if (!seen.insert(v).second) return rcError("nothing found where name expected", 198);
  }
  if (!haveUsing || fileText.empty()) return rcError("using required", 100);

  // The using file, as `describe using` finds it.
  std::string typed = fileText;
  {
    std::vector<std::string> w = stataWords(typed);
    if (w.size() != 1) return rcError("invalid file specification", 198);
    typed = w.front();
  }
  std::string path = resolveUserPath(session, typed);
  session.materializeVirtualSave(path);
  std::string shown = typed;
  {
    const size_t slash = path.find_last_of('/');
    const size_t dot = path.find_last_of('.');
    const bool noExt = dot == std::string::npos || (slash != std::string::npos && dot < slash);
    if (noExt && !fileExistsAt(path)) {
      path += ".dta";
      shown += ".dta";
      session.materializeVirtualSave(path);
    }
  }
  if (!fileExistsAt(path)) return rcError("file " + shown + " not found", 601);
  if (!looksLikeDta(path)) return rcError("file " + shown + " not Stata format", 610);

  const std::string conv = mintScratchPath(session.workdir(), "t1cf_using");
  const std::string metaPath = conv + ".meta.json";
  registerScratchMint(metaPath);
  CobraRunResult cr = runCobra({"import-dta", path, conv, "--out-meta", metaPath});
  if (!cr.ok) {
    dropScratch(conv);
    dropScratch(metaPath);
    return rcError("file " + shown + " could not be read: " +
                       strutil::trim(cr.stderr_text.empty() ? cr.error : cr.stderr_text),
                   610);
  }
  std::map<std::string, std::string> usingTypes;
  try {
    std::ifstream mf(cecon::to_native_path(metaPath), std::ios::binary);
    const nlohmann::json meta = nlohmann::json::parse(mf);
    if (meta.contains("variables") && meta["variables"].is_array())
      for (const auto& v : meta["variables"])
        usingTypes[v.value("name", std::string())] = v.value("stata_type", std::string());
  } catch (const std::exception&) {
    // The type text is only printed on a string/numeric clash; absent, it falls back below.
  }
  dropScratch(metaPath);

  std::unique_ptr<Datastore> usingDs;
  try {
    usingDs = std::make_unique<Datastore>(conv);
  } catch (const std::exception& e) {
    dropScratch(conv);
    return rcError("file " + shown + " could not be read: " + e.what(), 610);
  }
  session.noteProjectionScratch(conv);

  ColumnScope scope(session, vars, std::nullopt, std::nullopt, "cf");
  if (scope.failed()) return scope.error();
  Datastore& master = *scope.get();
  const int64_t nM = master.nRows(), nU = usingDs->nRows();
  if (nM != nU)
    return rcError("master has " + std::to_string(nM) + " obs, using " + std::to_string(nU), 9);

  std::vector<std::string> lines;
  long long nsum = 0;
  bool dif = false;
  const size_t maxObsLen = std::to_string(nM).size();
  for (const auto& v : vars) {
    const std::string stub = rightAlign(stataAbbrev(v, 16) + ": ", 18);
    if (!usingDs->has(v)) {
      lines.push_back(stub + "does not exist in using");
      dif = true;
      continue;
    }
    const std::vector<Cell> m = columnCells(master, v);
    const std::vector<Cell> u = columnCells(*usingDs, v);
    const bool mStr = master.dtype(v) == "factor", uStr = usingDs->dtype(v) == "factor";
    if (mStr != uStr) {
      std::string ut = usingTypes.count(v) ? usingTypes[v] : std::string();
      if (ut.empty()) ut = uStr ? "str" : "float";
      lines.push_back(stub + masterType(session, master, v) + " in master but " + ut + " in using");
      dif = true;
      continue;
    }
    std::vector<int64_t> diffs;
    for (int64_t i = 0; i < nM; ++i) {
      const Cell& a = m[static_cast<size_t>(i)];
      const Cell& b = u[static_cast<size_t>(i)];
      const bool differ = mStr ? a.s != b.s : cecon::stata_cmp(a.d, b.d) != 0;
      if (differ) diffs.push_back(i);
    }
    if (diffs.empty()) {
      if (all) lines.push_back(stub + "match");
      continue;
    }
    lines.push_back(stub + std::to_string(diffs.size()) + " mismatch" + (diffs.size() > 1 ? "es" : ""));
    nsum += static_cast<long long>(diffs.size());
    dif = true;
    if (verbose) {
      for (int64_t i : diffs) {
        const Cell& a = m[static_cast<size_t>(i)];
        const Cell& b = u[static_cast<size_t>(i)];
        const std::string mv = mStr ? a.s : displayNumber(a.d, session);
        const std::string uv = mStr ? b.s : displayNumber(b.d, session);
        lines.push_back(std::string(18, ' ') + "obs " + rightAlign(std::to_string(i + 1), maxObsLen) +
                        ". " + mv + " in master; " + uv + " in using");
      }
    }
  }
  session.eval.rStrings.set("Nsum", std::to_string(nsum));
  eval::declareRPosted(session.eval, "cf", {"Nsum"});
  ojson out = ojson::array();
  if (!lines.empty()) out.push_back(displayBlock(joinWords(lines, "\n")));
  if (dif) out.push_back(rcError("cf: master and using data differ", 9));
  return out;
}

// ===========================================================================
// fvunab
// ===========================================================================
//
// Implements fvunab per its help file; identified against StataBE probes
// (w2ax_t1_official.log fvunab_*, mx1/mx2/mx5.log FV|*):
//   * `fvunab lmacname : varlist [, min(#) max(#) name(str)]`; no colon r(198) "'' found
//     where ':' expected"; two names r(103); an invalid name r(198);
//   * the varlist is unab'd (wildcards, ranges, _all, unique abbreviations; r(111) for no
//     match / an ambiguous abbreviation / a reversed range);
//   * time-series operators need tsset/xtset (r(111) "time variable not set") and a numeric
//     variable (r(109)); they come back in canonical form: L and F net out, then D, then S,
//     a count of 1 unwritten and of 0 dropped (`l.` -> L., `l7.` -> L7., `l2f.` -> L.,
//     `LF.` -> nothing, `dl.`/`L.D.` -> LD., `sd.` -> DS., `l3l2.` -> L5.); a numlist
//     operator expands, variable by variable (`L(1/2).tau*` -> L.tau1 L2.tau1 L.tau2 ...);
//   * `c.` alone is dropped (`c.tau2` -> tau2); `i.`, `ib#.`/`b#.`, `ibn.`/`bn.`, `#.` (-> i#.)
//     are kept in lower case, and a variable's second factor term is dropped (`i.tau1
//     i.tau1 tau1` -> `i.tau1 tau1`, `ib2.tau1 i.tau1` -> `ib2.tau1`); a string variable as
//     a factor is r(109);
//   * an operator group that does not parse is r(198) "<term> invalid name";
//   * the local is set before the count check; fewer than min() r(102), more than max() r(103).
//   * an operator number past 2147483646 is r(198) "<op>:  operator invalid", a numlist of
//     more than 100 values (or two numlists in one term) r(198) "numlist in operator
//     invalid" (mx6/mx7.log); a factor level past it, which Stata wraps negative, is refused
//     the same way.
// Interactions (`#`, `##`), `o.`/`co.`, `i(numlist).`, `ib(first|last|freq).` and a factor
// operator combined with a time-series one are refused by name (r(199)): Stata accepts
// them and this build does not normalize them.
namespace {

struct TsOps {
  long long l = 0, d = 0, s = 0;
  std::string text() const {
    std::string out;
    auto part = [&](const char* c, long long k) {
      if (k <= 0) return;
      out += c;
      if (k > 1) out += std::to_string(k);
    };
    if (l > 0) part("L", l);
    else if (l < 0) part("F", -l);
    part("D", d);
    part("S", s);
    return out;
  }
};

// An operator's integer, or nullopt past 2147483646 -- the edge Stata measures
// (mx6/mx7.log: `L2147483648.` and `L(2147483647).` are r(198)); never a throwing parse.
std::optional<long long> opInt(const std::string& text) {
  size_t i = 0;
  bool neg = false;
  if (i < text.size() && (text[i] == '+' || text[i] == '-')) neg = text[i++] == '-';
  if (i >= text.size()) return std::nullopt;
  long long v = 0;
  for (; i < text.size(); ++i) {
    if (!std::isdigit(static_cast<unsigned char>(text[i]))) return std::nullopt;
    v = v * 10 + (text[i] - '0');
    if (v >= 2147483647LL) return std::nullopt;
  }
  return neg ? -v : v;
}

// A numlist inside an operator's parentheses: `1/3`, `3/1`, `1 3`, `1(1)3`, `-1`, `0`. At
// most 100 values (mx7.log: L(1/100). runs, L(1/101). is r(198)), counted BEFORE a range
// is filled, so `L(1/2000000000).` costs nothing.
std::optional<std::vector<long long>> opNumlist(const std::string& text) {
  constexpr long long kMax = 100;
  std::vector<long long> out;
  std::string t = text;
  std::replace(t.begin(), t.end(), ',', ' ');
  std::istringstream iss(t);
  static const std::regex reRange(R"(^([+-]?\d+)/([+-]?\d+)$)");
  static const std::regex reStep(R"(^([+-]?\d+)\(([+-]?\d+)\)([+-]?\d+)$)");
  for (std::string w; iss >> w;) {
    std::smatch m;
    if (auto one = opInt(w)) {
      out.push_back(*one);
    } else if (std::regex_match(w, m, reRange)) {
      const auto a = opInt(m.str(1)), b = opInt(m.str(2));
      if (!a || !b) return std::nullopt;
      const long long n = (*a <= *b ? *b - *a : *a - *b) + 1;
      if (static_cast<long long>(out.size()) + n > kMax) return std::nullopt;
      if (*a <= *b)
        for (long long k = *a; k <= *b; ++k) out.push_back(k);
      else
        for (long long k = *a; k >= *b; --k) out.push_back(k);
    } else if (std::regex_match(w, m, reStep)) {
      const auto a = opInt(m.str(1)), st = opInt(m.str(2)), b = opInt(m.str(3));
      if (!a || !st || !b || *st == 0) return std::nullopt;
      if ((*st > 0 && *a > *b) || (*st < 0 && *a < *b)) return std::nullopt;
      const long long n = (*st > 0 ? (*b - *a) / *st : (*a - *b) / -*st) + 1;
      if (static_cast<long long>(out.size()) + n > kMax) return std::nullopt;
      if (*st > 0)
        for (long long k = *a; k <= *b; k += *st) out.push_back(k);
      else
        for (long long k = *a; k >= *b; k += *st) out.push_back(k);
    } else {
      return std::nullopt;
    }
    if (static_cast<long long>(out.size()) > kMax) return std::nullopt;
  }
  if (out.empty()) return std::nullopt;
  return out;
}

// One parsed term: its operator prefix (already canonical) and the variables it names.
struct FvTerm {
  std::vector<std::string> tsPrefixes;  // one per numlist value ("" = no ts operator)
  std::string fvPrefix;                 // "i", "ib2", "ibn", "i2" or ""
  std::vector<std::string> vars;
};

// The term grammar is `[ops.]varspec`, where ops is a run of operator groups each ended by
// a `.` at paren depth 0 and varspec is a name/wildcard/range or a parenthesised varlist.
std::optional<ojson> parseFvTerm(const std::string& term, SessionState& session, FvTerm* out) {
  if (term.find('#') != std::string::npos)
    return notImplemented("fvunab: factor-variable interactions (`#`, `##`) are not implemented "
                          "in this version of PIET (" + term + ")");
  size_t lastDot = std::string::npos;
  int depth = 0;
  for (size_t i = 0; i < term.size(); ++i) {
    if (term[i] == '(') ++depth;
    else if (term[i] == ')') --depth;
    else if (term[i] == '.' && depth == 0) lastDot = i;
  }
  const std::string ops = lastDot == std::string::npos ? std::string() : term.substr(0, lastDot);
  std::string varspec = lastDot == std::string::npos ? term : term.substr(lastDot + 1);
  if (varspec.size() >= 2 && varspec.front() == '(' && varspec.back() == ')')
    varspec = varspec.substr(1, varspec.size() - 2);
  if (varspec.find('(') != std::string::npos || varspec.find(')') != std::string::npos)
    return rcError("nothing found where name expected", 198);  // `L(1/2)` with no variable
  if (varspec.find('.') != std::string::npos)
    return notImplemented("fvunab: an operator inside a parenthesised varlist is not implemented "
                          "in this version of PIET (" + term + ")");
  if (strutil::trim(varspec).empty()) return rcError("nothing found where name expected", 198);

  // Operator groups: split ops on depth-0 dots.
  std::vector<std::string> groups;
  {
    std::string cur;
    int d = 0;
    for (char c : ops) {
      if (c == '(') ++d;
      if (c == ')') --d;
      if (c == '.' && d == 0) {
        groups.push_back(cur);
        cur.clear();
        continue;
      }
      cur.push_back(c);
    }
    if (!ops.empty()) groups.push_back(cur);
  }
  static const std::regex reI(R"(^i$)", std::regex::icase);
  static const std::regex reC(R"(^c$)", std::regex::icase);
  static const std::regex reIb(R"(^i?b(\d+)$)", std::regex::icase);
  static const std::regex reIbParen(R"(^i?b\((\d+)\)$)", std::regex::icase);
  static const std::regex reIbn(R"(^i?bn$)", std::regex::icase);
  static const std::regex reIn(R"(^i?(\d+)$)", std::regex::icase);
  static const std::regex reFvRefused(R"(^(c?o|i\(.*\)|i?b\((first|last|freq)\))$)",
                                      std::regex::icase);
  std::string fv;
  std::vector<std::pair<char, std::vector<long long>>> tsAtoms;
  for (const auto& g : groups) {
    std::smatch m;
    if (g.empty()) return rcError(term + " invalid name", 198);
    std::string fvHere;
    if (std::regex_match(g, reC)) continue;  // `c.` is the default; dropped (fvunab_fv)
    const std::string invalidOp = g + ":  operator invalid";
    if (std::regex_match(g, reI)) {
      fvHere = "i";
    } else if (std::regex_match(g, m, reIb) || std::regex_match(g, m, reIbParen)) {
      // Past 2^31-1 Stata wraps the level to a negative one (mx7.log `ib2147483648.` ->
      // ib-2147483648); that is refused here rather than reproduced.
      auto lv = opInt(m.str(1));
      if (!lv) return rcError(invalidOp, 198);
      fvHere = "ib" + std::to_string(*lv);
    } else if (std::regex_match(g, reIbn)) {
      fvHere = "ibn";
    } else if (std::regex_match(g, m, reIn)) {
      auto lv = opInt(m.str(1));
      if (!lv) return rcError(invalidOp, 198);
      fvHere = "i" + std::to_string(*lv);
    }
    if (!fvHere.empty()) {
      if (!fv.empty())
        return notImplemented("fvunab: two factor-variable operators on one term are not "
                              "implemented in this version of PIET (" + term + ")");
      fv = fvHere;
      continue;
    }
    // A run of time-series atoms: a letter L F D S, then digits or a (numlist).
    size_t i = 0;
    bool ok = true;
    std::vector<std::pair<char, std::vector<long long>>> atoms;
    while (i < g.size()) {
      const char c = static_cast<char>(std::toupper(static_cast<unsigned char>(g[i])));
      if (c != 'L' && c != 'F' && c != 'D' && c != 'S') {
        ok = false;
        break;
      }
      ++i;
      std::vector<long long> vals{1};
      if (i < g.size() && g[i] == '(') {
        size_t close = std::string::npos;
        int pd = 0;
        for (size_t j = i; j < g.size(); ++j) {
          if (g[j] == '(') ++pd;
          if (g[j] == ')' && --pd == 0) {
            close = j;
            break;
          }
        }
        if (close == std::string::npos) {
          ok = false;
          break;
        }
        auto nl = opNumlist(g.substr(i + 1, close - i - 1));
        if (!nl) return rcError(g + "\nnumlist in operator invalid", 198);  // mx7.log L(1/101).
        vals = *nl;
        i = close + 1;
      } else if (i < g.size() && std::isdigit(static_cast<unsigned char>(g[i]))) {
        size_t j = i;
        while (j < g.size() && std::isdigit(static_cast<unsigned char>(g[j]))) ++j;
        auto lag = opInt(g.substr(i, j - i));  // L2147483648. is r(198) (mx6.log)
        if (!lag) return rcError(g + ":  operator invalid", 198);
        vals = {*lag};
        i = j;
      }
      atoms.emplace_back(c, vals);
    }
    if (!ok) {
      if (std::regex_match(g, reFvRefused))
        return notImplemented("fvunab: the factor-variable operator `" + g + ".` is not "
                              "implemented in this version of PIET (" + term + ")");
      // A group mixing the factor letters with time-series ones (`iL.`, `Li2.`, `2b.`) is a
      // real operator this build does not normalize; any other letter is not an operator.
      bool fvLetters = false, other = false;
      for (char ch : g) {
        const char lc = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        if (lc == 'i' || lc == 'c' || lc == 'b' || lc == 'n' || lc == 'o') fvLetters = true;
        else if (!(lc == 'l' || lc == 'f' || lc == 'd' || lc == 's' || std::isdigit(static_cast<unsigned char>(ch))))
          other = true;
      }
      if (fvLetters && !other)
        return notImplemented("fvunab: the operator `" + g + ".` (a factor-variable operator "
                              "combined with another) is not implemented in this version of "
                              "PIET (" + term + ")");
      return rcError(term + " invalid name", 198);
    }
    for (auto& a : atoms) tsAtoms.push_back(std::move(a));
  }
  if (!fv.empty() && !tsAtoms.empty())
    return notImplemented("fvunab: a factor-variable operator combined with a time-series "
                          "operator is not implemented in this version of PIET (" + term + ")");
  int multi = 0;
  for (const auto& a : tsAtoms)
    if (a.second.size() > 1) ++multi;
  if (multi > 1)  // mx7.log `L(1/2)D(1/2).a` r(198)
    return rcError(term + "\nnumlist in operator invalid", 198);

  std::vector<std::string> vars;
  if (auto e = unabVarlist(varspec, session, &vars)) return *e;

  out->fvPrefix = fv;
  out->tsPrefixes.clear();
  if (tsAtoms.empty()) {
    out->tsPrefixes.push_back(std::string());
  } else {
    if (!session.timevar.has_value() || session.timevar->empty())
      return rcError("time variable not set", 111);
    size_t multiAt = tsAtoms.size();
    for (size_t k = 0; k < tsAtoms.size(); ++k)
      if (tsAtoms[k].second.size() > 1) multiAt = k;
    const size_t nCombos = multiAt == tsAtoms.size() ? 1 : tsAtoms[multiAt].second.size();
    for (size_t c = 0; c < nCombos; ++c) {
      TsOps ts;
      for (size_t k = 0; k < tsAtoms.size(); ++k) {
        const long long v = k == multiAt ? tsAtoms[k].second[c] : tsAtoms[k].second.front();
        switch (tsAtoms[k].first) {
          case 'L': ts.l += v; break;
          case 'F': ts.l -= v; break;
          case 'D': ts.d += v; break;
          default: ts.s += v; break;
        }
      }
      out->tsPrefixes.push_back(ts.text());
    }
  }
  // Operators on a string variable (mx2.log FV|l.s, FV|i.s).
  if (!fv.empty() || !tsAtoms.empty()) {
    Datastore* ds = session.datastore();
    for (const auto& v : vars) {
      if (ds != nullptr && ds->has(v) && ds->dtype(v) == "factor") {
        if (!fv.empty())
          return rcError(v + ":  string variables may not be used as factor variables", 109);
        bool anyTs = false;
        for (const auto& p : out->tsPrefixes) anyTs = anyTs || !p.empty();
        if (anyTs) return rcError(v + ":  may not use time-series operators on string variables", 109);
      }
    }
  }
  out->vars = vars;
  return std::nullopt;
}

// Split the varlist text into terms at depth-0 blanks. Blanks around a depth-0 `-` go
// first (expandVarlist's own range rule: `tau1 - tau3` is one range), and an operator
// group ending in `.` followed by a blank joins the next word (`L. tau1` is L.tau1).
std::vector<std::string> fvTerms(const std::string& textIn) {
  std::string text;
  int depth = 0;
  for (size_t i = 0; i < textIn.size(); ++i) {
    const char c = textIn[i];
    if (c == '(') ++depth;
    if (c == ')') --depth;
    if (c == '-' && depth == 0) {
      while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back()))) text.pop_back();
      text.push_back('-');
      while (i + 1 < textIn.size() && std::isspace(static_cast<unsigned char>(textIn[i + 1]))) ++i;
      continue;
    }
    text.push_back(c);
  }
  std::vector<std::string> raw;
  std::string cur;
  depth = 0;
  for (char c : text) {
    if (c == '(') ++depth;
    if (c == ')') --depth;
    if (std::isspace(static_cast<unsigned char>(c)) && depth == 0) {
      if (!cur.empty()) raw.push_back(cur);
      cur.clear();
      continue;
    }
    cur.push_back(c);
  }
  if (!cur.empty()) raw.push_back(cur);
  std::vector<std::string> out;
  for (size_t i = 0; i < raw.size(); ++i) {
    std::string t = raw[i];
    while (!t.empty() && t.back() == '.' && i + 1 < raw.size()) t += raw[++i];
    out.push_back(t);
  }
  return out;
}

}  // namespace

ojson runFvunab(const eval::ParsedCommand& parsed, SessionState& session) {
  if (auto err = eval::rejectUnknownOptions(parsed.opts, kVocabFvunab.known,
                                            kVocabFvunab.unimplemented))
    return ojson(*err);
  ojson perr;
  auto minN = intOption(parsed, "min", 1, &perr);
  if (!minN) return perr;
  auto maxN = intOption(parsed, "max", 32000, &perr);
  if (!maxN) return perr;
  const std::string optName = strutil::trim(optStr(parsed, "name").value_or(std::string()));

  const std::string text = parsed.argText;
  size_t colon = std::string::npos;
  {
    const std::vector<bool> quoted = eval::quotedSpanMask(text);
    for (size_t i = 0; i < text.size(); ++i)
      if (text[i] == ':' && !quoted[i]) {
        colon = i;
        break;
      }
  }
  if (colon == std::string::npos) return rcError("'' found where ':' expected", 198);
  std::vector<std::string> names;
  {
    std::istringstream iss(text.substr(0, colon));
    for (std::string w; iss >> w;) names.push_back(w);
  }
  if (names.empty()) return rcError("nothing found where name expected", 198);
  if (names.size() > 1) return rcError("macro name:  too many specified", 103);
  const std::string lname = names.front();
  if (!strutil::isValidStataIdentifier(lname) || lname.size() > 31)
    return rcError(lname + " invalid name", 198);

  std::vector<std::string> result;
  std::set<std::string> fvSeen;  // variables already carrying a factor term
  for (const auto& term : fvTerms(text.substr(colon + 1))) {
    FvTerm ft;
    if (auto e = parseFvTerm(term, session, &ft)) return *e;
    for (const auto& v : ft.vars) {
      if (!ft.fvPrefix.empty()) {
        if (!fvSeen.insert(v).second) continue;
        result.push_back(ft.fvPrefix + "." + v);
        continue;
      }
      for (const auto& p : ft.tsPrefixes) result.push_back(p.empty() ? v : p + "." + v);
    }
  }
  const std::string joined = joinWords(result);
  session.eval.locals.set(lname, joined);  // c_local before the count check (FV|min2)

  const long long n = static_cast<long long>(result.size());
  if (*minN <= n && n <= *maxN) return silent();
  const int rc = n < *minN ? 102 : 103;
  const std::string what = n < *minN ? "too few variables specified" : "too many variables specified";
  if (optName.empty()) return rcError(joined + ":\n" + what, rc);
  std::string need;
  if (*minN == *maxN) need = std::to_string(*minN) + " variable" + (*maxN > 1 ? "s" : "") + " required";
  else if (*minN <= 0) need = *maxN == 1 ? "0 or 1 variables required" : std::to_string(*maxN) + " or fewer variables required";
  else if (*maxN >= 32000) need = std::to_string(*minN) + " or more variables required";
  else need = std::to_string(*minN) + " - " + std::to_string(*maxN) + " variables required";
  return rcError(optName + ": " + what + "\n" + std::string(optName.size() + 4, ' ') + need, rc);
}

// ===========================================================================
// numlabel
// ===========================================================================
//
// Implements numlabel per its help file; identified against StataBE probes
// (w2ax_t1_official.log numlabel_*, mx1.log NL|*):
//   * exactly one of add/remove (both, or neither: r(198)); mask() must hold exactly one `#`
//     (r(198) "mask <m> should contain 1 substitution char #"), default "#. ";
//   * the label sets named, or every set (none named, or `_all`: review p6 nl_all); a name
//     that is not one r(198) (p4 nl_wild `* invalid name`), an unknown one r(111) "value
//     label <n> not found"; from there on r() is replaced (p9 nl_r); no set at all is a silent rc 0;
//   * per code, the prefix is the mask with `#` replaced by the code (-1 -> "(-1) ", .a ->
//     "(.a) "); a text "carries" the prefix when it begins with it;
//   * without `force`, a set in which some texts carry the prefix and some do not stops the
//     whole command with r(180) (the "partially numlabel-ed" note, the per-set listing under
//     `detail`); with `force` it is modified anyway (NL|partialforce);
//   * add prefixes every text that does not carry it (add twice changes nothing:
//     numlabel_add_again), remove strips it (a text that was only the prefix becomes empty,
//     i.e. the code loses its label: NL|empty); nothing to change -> "(no value label to be
//     modified)", rc 0 (numlabel_force);
//   * every set processed is rewritten with `label define ..., modify` -- here runLabelDefine,
//     so labelDefs, the attached variables' valueLabels and their display-format widening
//     (NL|fmt %22.0g) all follow the one writer `label define` uses.
ojson runNumlabel(const eval::ParsedCommand& parsed, SessionState& session) {
  if (auto err = eval::rejectUnknownOptions(parsed.opts, kVocabNumlabel.known,
                                            kVocabNumlabel.unimplemented))
    return ojson(*err);
  const bool add = hasFlag(parsed, "add"), remove = hasFlag(parsed, "remove");
  const bool force = hasFlag(parsed, "force"), detail = hasFlag(parsed, "detail");
  if (add && remove) return rcError("options add and remove must not be both specified", 198);
  if (!add && !remove) return rcError("option add or remove should be specified", 198);
  std::string mask = "#. ";
  if (auto m = optStr(parsed, "mask")) {
    // A quoted mask is taken as written; an unquoted one loses its outer blanks, as a
    // `str` option does (review p4 nl_mask_sp2: remove with mask(# - ) leaves " one").
    const std::string t = strutil::trim(*m);
    const std::vector<std::string> w = stataWords(t);
    const bool quoted = !t.empty() && (t[0] == '"' || (t.size() >= 2 && t[0] == '`' && t[1] == '"'));
    const std::string given = quoted && w.size() == 1 ? w.front() : t;
    if (!given.empty()) {
      if (std::count(given.begin(), given.end(), '#') != 1)
        return rcError("mask " + given + " should contain 1 substitution char #", 198);
      mask = given;
    }
  }

  // From here numlabel's own work replaces r() (review p9 nl_r: r(N) of an earlier
  // summarize is gone; nl_r_err: the option errors above leave it).
  eval::replaceRClass(session.eval);
  eval::declareRPosted(session.eval, "numlabel", {});

  // Label names, `_all` among them meaning every set (review p6 nl_all, mx7.log NL|mixall);
  // a name that is not a name is r(198) at its first bad character (p4 nl_wild `* invalid
  // name`), an unknown one r(111).
  std::vector<std::string> sets;
  bool everySet = parsed.varlist.empty();
  for (const auto& nm : parsed.varlist) {
    if (nm == "_all") {
      everySet = true;
      continue;
    }
    if (!strutil::isValidStataIdentifier(nm)) {
      size_t bad = 0;
      while (bad < nm.size() && (std::isalnum(static_cast<unsigned char>(nm[bad])) || nm[bad] == '_')) ++bad;
      if (bad == 0 || bad >= nm.size()) bad = 0;
      return rcError(nm.substr(bad) + " invalid name", 198);
    }
    if (session.labelDefs.find(nm) == session.labelDefs.end())
      return rcError("value label " + nm + " not found", 111);
    if (std::find(sets.begin(), sets.end(), nm) == sets.end()) sets.push_back(nm);
  }
  if (everySet) {
    sets.clear();
    for (const auto& kv : session.labelDefs) sets.push_back(kv.first);
  }
  if (sets.empty()) return displayBlock("(dataset has no value labels)");  // mx1.log NL|nolabels

  struct Entry {
    std::string code, text, prefix;
    bool carries = false;
  };
  std::map<std::string, std::vector<Entry>> plan;
  for (const auto& s : sets) {
    auto& entries = plan[s];
    for (const auto& [code, text] : session.labelDefs.at(s)) {
      Entry e;
      e.code = code;
      e.text = text;
      e.prefix = mask;
      e.prefix.replace(e.prefix.find('#'), 1, code);
      e.carries = text.compare(0, e.prefix.size(), e.prefix) == 0;
      entries.push_back(std::move(e));
    }
  }

  if (!force) {
    std::vector<std::string> partial;
    for (const auto& s : sets) {
      bool anyYes = false, anyNo = false;
      for (const auto& e : plan[s]) (e.carries ? anyYes : anyNo) = true;
      if (anyYes && anyNo) partial.push_back(s);
    }
    if (!partial.empty()) {
      ojson out = ojson::array();
      if (detail) {
        std::string t = "\nThe following value labels are only partially numlabel-ed";
        for (const auto& s : partial) {
          t += "\n" + std::string(kLineSize, '-') + "\n-> lname = " + s + "\n\n  value         label";
          std::vector<std::pair<std::string, std::string>> rows;
          for (const auto& e : plan[s]) rows.emplace_back(e.code, e.text);
          std::stable_sort(rows.begin(), rows.end(), [](const auto& a, const auto& b) {
            const bool am = !a.first.empty() && a.first[0] == '.';
            const bool bm = !b.first.empty() && b.first[0] == '.';
            if (am != bm) return bm;
            if (am) return a.first < b.first;
            return std::stoll(a.first) < std::stoll(b.first);
          });
          for (const auto& [code, text] : rows) t += "\n" + rightAlign(code, 11) + "     " + text;
          t += "\n";
        }
        out.push_back(displayBlock(t));
      } else {
        out.push_back(displayBlock(
            "there are value labels that are only partially numlabel-ed\n"
            "specify option detail for a list of these value labels"));
      }
      out.push_back(rcError("specify option force to modify value labels anyway", 180));
      return out;
    }
  }

  bool anyChange = false;
  for (auto& [s, entries] : plan) {
    for (auto& e : entries) {
      std::string next = e.text;
      if (add && !e.carries) next = e.prefix + e.text;
      if (remove && e.carries) next = e.text.substr(e.prefix.size());
      if (add && next.size() > 32000)
        return rcError("impossible to prefix a value to value labels > 32000 chars", 180);
      if (next != e.text) anyChange = true;
      e.text = next;
    }
  }
  if (!anyChange) return displayBlock("(no value label to be modified)");

  for (const auto& [s, entries] : plan) {
    std::string line = "label define " + s;
    for (const auto& e : entries) {
      // Compound quotes carry any text but one that itself closes or opens them.
      if (e.text.find("\"'") != std::string::npos || e.text.find("`\"") != std::string::npos)
        return notImplemented("numlabel: the text of code " + e.code + " in value label " + s +
                              " holds a compound-quote delimiter, which this version of PIET "
                              "cannot write back");
      line += " " + e.code + " `\"" + e.text + "\"'";
    }
    line += ", modify";
    ojson r = runLabelDefine(line, session);
    if (const ojson* e = firstError(r)) return *e;
  }
  return silent();
}

// ===========================================================================
// labvars
// ===========================================================================
//
// Implements labvars per its help file (Daniel Klein, labvars 1.0.5); identified against
// StataBE probes (w2ax_t1_community.log labvars_*, mx3.log LV|*):
//   * `labvars [varlist] "l1" "l2" ... [, remove carryon alternate names]`; the varlist is the
//     text before a `\` or else before the first quote, or every variable when the first word
//     is quoted (LV|allq); the labels are Stata's words (labvars_dq counts 3);
//   * fewer labels than variables is r(198) "# variables but # labels specified" unless
//     remove/carryon (then the rest are cleared / take the last label: LV|remove1,
//     LV|carry); more labels than variables is always r(198) (labvars_more); NOTHING is
//     labelled on an error (labvars_fewer keeps v1's earlier label);
//   * no labels and no option r(100) "<k> variables but 0 labels specified"; `labvars,
//     remove` clears every variable; remove with carryon r(198);
//   * `alternate` reads var/label pairs (LV|alt), `names` labels each variable with its name;
//     either beside another option is r(198) "option <x> not allowed" before any label is
//     written (review p5 lv_altnam);
//   * the arguments are read as typed: an if/in qualifier is not one, its words count as
//     labels (p5 lv_if, 5 labels), and a `\` inside quotes is text (p7 lv_bs);
//   * each label keeps its first 80 characters, with Stata's note (LV|long).
namespace {

// `label variable v "text"`'s 80-character rule (the dispatcher's label-variable arm), with
// the note Stata prints for it.
ojson applyVarLabel(const std::string& v, std::string text, SessionState& session,
                    ojson& notes) {
  size_t chars = 0, cut = text.size();
  for (size_t i = 0; i < text.size(); ++i) {
    if ((static_cast<unsigned char>(text[i]) & 0xC0) == 0x80) continue;
    if (chars == 80) {
      cut = i;
      break;
    }
    ++chars;
  }
  if (cut < text.size()) {
    text = text.substr(0, cut);
    notes.push_back(displayBlock("note: label truncated to 80 characters"));
  }
  return runLabelVariable(v, text, session);
}

std::string countText(size_t n, const char* noun) {
  return std::to_string(n) + " " + noun + (n == 1 ? "" : "s");
}

}  // namespace

ojson runLabvars(const eval::ParsedCommand& parsed, SessionState& session) {
  if (auto err = eval::rejectUnknownOptions(parsed.opts, kVocabLabvars.known,
                                            kVocabLabvars.unimplemented))
    return ojson(*err);
  const std::vector<std::string> allVars = allVarNames(session);
  if (allVars.empty()) return rcError("no variables defined", 111);
  const bool remove = hasFlag(parsed, "remove"), carryon = hasFlag(parsed, "carryon");
  const bool alternate = hasFlag(parsed, "alternate"), names = hasFlag(parsed, "names");
  // The arguments as typed, up to the options comma. labvars takes `anything`, so an
  // `if`/`in` qualifier is not one: its words are labels (review p5 lv_if: `labvars b
  // "lab b" if b > 0` is "1 variable but 5 labels specified").
  std::string text;
  {
    std::string raw = strutil::trim(parsed.raw);
    size_t sp = 0;  // the command word: `labvars, remove` has no blank before its comma
    while (sp < raw.size() &&
           (std::isalnum(static_cast<unsigned char>(raw[sp])) || raw[sp] == '_'))
      ++sp;
    raw = raw.substr(sp);
    const long long comma = eval::findUnquotedComma(raw);
    if (comma >= 0) raw = raw.substr(0, static_cast<size_t>(comma));
    text = strutil::trim(raw);
  }
  ojson notes = ojson::array();

  // The two subroutines take only their own option: anything else beside them is refused
  // before a label is written (review p5 lv_altnam `option names not allowed`).
  if (alternate || names) {
    for (const auto& [k, v] : parsed.opts) {
      (void)v;
      const bool allowed = alternate ? k == "alternate" : k == "names";
      if (!allowed) return rcError("option " + k + " not allowed", 198);
    }
  }
  if (alternate || names) {
    if (alternate) {
      const std::vector<std::string> w = stataWords(text);
      if (w.empty()) return rcError("too few variables specified", 102);
      if (w.size() % 2 != 0)
        return rcError(countText((w.size() + 1) / 2, "variable") + " but " +
                           countText(w.size() / 2, "label") + " specified",
                       198);
      std::vector<std::string> vars;
      for (size_t i = 0; i < w.size(); i += 2) {
        std::vector<std::string> one;
        if (auto e = unabVarlist(w[i], session, &one)) return *e;
        if (one.size() != 1) return rcError(w[i] + " invalid name", 198);
        vars.push_back(one.front());
      }
      for (size_t i = 0; i < vars.size(); ++i)
        if (const ojson* e = firstError(applyVarLabel(vars[i], w[2 * i + 1], session, notes)))
          return *e;
    }
    if (names) {
      std::vector<std::string> vars = allVars;
      if (!text.empty())
        if (auto e = unabVarlist(text, session, &vars)) return *e;
      for (const auto& v : vars)
        if (const ojson* e = firstError(applyVarLabel(v, v, session, notes))) return *e;
    }
    return notes;
  }
  if (remove && carryon) return rcError("options remove and carryon not both allowed", 198);

  std::string vlText, labText;
  if (text.empty()) {
    if (!remove && !carryon)
      return rcError(countText(allVars.size(), "variable") + " but 0 labels specified", 100);
    vlText = "*";
  } else if (text[0] == '"' || (text.size() >= 2 && text[0] == '`' && text[1] == '"')) {
    vlText = "*";
    labText = text;
  } else {
    // The backslash separator counts outside quotes only (review p7 lv_bs: `labvars b
    // "a\b"` labels b with the backslash kept).
    size_t bs = std::string::npos;
    {
      const std::vector<bool> quoted = eval::quotedSpanMask(text);
      for (size_t i = 0; i < text.size(); ++i)
        if (text[i] == '\\' && !quoted[i]) {
          bs = i;
          break;
        }
    }
    if (bs != std::string::npos) {
      vlText = strutil::trim(text.substr(0, bs));
      labText = text.substr(bs + 1);
      if (vlText.empty()) vlText = "*";
    } else {
      const size_t q = text.find('"');
      vlText = strutil::trim(q == std::string::npos ? text : text.substr(0, q));
      labText = q == std::string::npos ? std::string() : text.substr(q);
      if (q != std::string::npos && q > 0 && text[q - 1] == '`') {
        vlText = strutil::trim(text.substr(0, q - 1));
        labText = text.substr(q - 1);
      }
    }
  }
  std::vector<std::string> vars;
  if (auto e = unabVarlist(vlText, session, &vars)) return *e;
  const std::vector<std::string> labels = stataWords(labText);
  if (vars.size() != labels.size() && (vars.size() < labels.size() || (!remove && !carryon)))
    return rcError(countText(vars.size(), "variable") + " but " + countText(labels.size(), "label") +
                       " specified",
                   198);
  std::string lbl;
  for (size_t i = 0; i < vars.size(); ++i) {
    const std::string tok = i < labels.size() ? labels[i] : std::string();
    if (!tok.empty() || !carryon) lbl = tok;
    if (const ojson* e = firstError(applyVarLabel(vars[i], lbl, session, notes))) return *e;
  }
  return notes;
}

// ===========================================================================
// renpfix
// ===========================================================================
//
// Implements renpfix per its help file; identified against StataBE probes
// (w2ax_t1_official.log renpfix_*, mx2.log RP|*):
//   * `renpfix old [new]` -- two words at most (a third: r(198) "invalid syntax"), quotes
//     allowed (RP|quoted); every variable matching `old*`, in dataset order, is renamed to
//     new + the rest of its name (renpfix_basic), and `new` omitted strips the prefix
//     (renpfix_drop); the prefix match is case-sensitive (RP|case);
//   * no match r(111) "variable old* not found" (renpfix_nomatch);
//   * the renames run IN ORDER and stop at the first that fails, the earlier ones staying
//     done (renpfix_collide `u1 u2 a b other ttx ux`, r(110) "variable ux already
//     defined"); a name that becomes empty is r(100) "varname required" (RP|exact), an
//     illegal one r(198) "<name> invalid name" (RP|badnew, RP|toempty), a reserved one
//     r(198) "<name> invalid varname" (review p7 rp_resv `long`, `_n`);
//   * r(varlist) holds the new names when every rename succeeded.
ojson runRenpfix(const eval::ParsedCommand& parsed, SessionState& session) {
  if (auto err = eval::rejectUnknownOptions(parsed.opts, kVocabRenpfix.known,
                                            kVocabRenpfix.unimplemented))
    return ojson(*err);
  eval::replaceRClass(session.eval);  // renpfix is r-class; a failure leaves r() empty
  eval::declareRPosted(session.eval, "renpfix", {});
  const std::vector<std::string> words = stataWords(parsed.argText);
  if (words.size() > 2) return rcError("invalid syntax", 198);
  const std::string prefix = words.empty() ? std::string() : words[0];
  const std::string repl = words.size() > 1 ? words[1] : std::string();

  std::vector<std::string> vars;
  if (auto e = unabVarlist(prefix + "*", session, &vars)) return *e;

  std::vector<std::string> fromNames, toNames;
  std::optional<ojson> failure;
  std::vector<std::string> live = allVarNames(session);
  for (const auto& v : vars) {
    const std::string next = repl + v.substr(std::min(prefix.size(), v.size()));
    if (next.empty()) {
      failure = rcError("varname required", 100);
      break;
    }
    if (!strutil::isValidStataIdentifier(next)) {
      failure = rcError(next + " invalid name", 198);
      break;
    }
    // A reserved name stops the loop like any other failed rename (review p7 rp_resv:
    // `renpfix a long` renames abc to longbc, then `long invalid varname`).
    if (isReservedVarName(next)) {
      failure = rcError(next + " invalid varname", 198);
      break;
    }
    if (std::find(live.begin(), live.end(), next) != live.end()) {
      failure = rcError("variable " + next + " already defined", 110);
      break;
    }
    ojson r = runRename(v, next, session);
    if (const ojson* e = firstError(r)) {
      failure = *e;
      break;
    }
    std::replace(live.begin(), live.end(), v, next);
    fromNames.push_back(v);
    toNames.push_back(next);
  }
  ojson out = ojson::array();
  // ONE block for the renames done (renvarlab's BUG-452 shape), then the stop, if any.
  if (!toNames.empty()) out.push_back(mutationResult(session, "rename", toNames, fromNames));
  if (failure.has_value()) {
    out.push_back(*failure);
    return out;
  }
  session.eval.rStrings.set("varlist", joinWords(toNames));
  eval::declareRPosted(session.eval, "renpfix", {"varlist"});
  return out;
}

}  // namespace piet::handlers
