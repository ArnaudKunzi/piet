// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// AER tier-1 (2026-10-28), lane GEN: verbs that write a column -- mvdecode integ sencode
// fillmissing astile. Registry rows, dispatcher arms, handlers.h declarations, effects/floors
// rows, NOTICE lines and pangram lines landed with the shared-surface unit; this file is the
// lane's alone. Answers: tests/eval/golden/aer_w2/w2ax_t1_official.log and
// w2ax_t1_community.log (StataBE 19.5), plus the lane's own StataBE probes cited by arm.
//
// WHAT EACH COMMAND REUSES. Nothing here computes a statistic the engine already computes:
//
//   mvdecode     reads each column, writes it back IN PLACE through `cobra setcol` (the
//                write shape runCarryforward/runMvencode use). The only arithmetic is the
//                equality test, which follows mvdecode's documented float rule: a float
//                variable is compared with float(#) (StataBE probe p2 mvfloat).
//   integ        implements the cubic-spline rule described in [R] dydx "Methods and
//                formulas" (clamped spline, endpoint slopes from a quadratic through the
//                three end points; trapezoid on request). This is the one computation here,
//                because no other command in PIET fits a spline; identified against StataBE
//                (w2ax_t1_official.log integ_*; probe p2 i3/i2/i1/ities).
//   sencode      implements sencode per its help file (Newson): codes in order of first
//                appearance (or of a gsort() key), "" missing, an existing value label reused
//                and extended at its largest code + 1 (encode's own extension rule, which
//                runEncode documents). The label-store writes are runEncode's shape.
//   fillmissing  implements fillmissing per its help file (Shah). The copy fills
//                (any/previous/next/first/last) are row copies; the statistic fills
//                (mean/min/max/median) are a DISPATCHED `egen` (so the statistic and its
//                float storage are egen's) followed by a dispatched `replace` (so type
//                promotion, `byte` -> `float`, is replace's). `any` is the group's SMALLEST
//                non-missing value -- the package sorts the group descending and carries
//                the last value forward (StataBE probe p1 any: ., 5, ., 3 -> 3, not 5).
//   astile       implements astile per its help file (Shah): xtile's cut points per by()
//                group. The cut points are the engine's own `egen pctile` (the same
//                stata_pctile rule the engine's xtile uses, at the same p = 100*k/nq), and
//                the group number is 1 + #(x > cut), xtile's assignment rule. One engine
//                call for any number of groups.
//
// TEMPORARY COLUMNS. Every scratch variable this file makes is named `__piet_t1g_*` and is
// dropped before the reply is built, so the card in the reply and the exit census never see
// one (probe ORPHAN 0).
//
// DEFECT CLASSES (handlers_aer_util.cpp's list): every return is a block object or an array of
// block objects; every rc Stata gives that coarseRc cannot infer rides `reason` as "r(NNN)";
// every option read is registered below at namespace scope, real-but-unread options sit in
// `unimplemented`.
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <fstream>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "io/win_compat.h"  // cecon::to_native_path / native_unlink
#include "piet/cobra_runner.h"
#include "piet/datastore.h"
#include "piet/dispatcher.h"
#include "piet/eval/errors.h"
#include "piet/eval/pyvalue.h"
#include "piet/eval/session.h"
#include "piet/eval/tokenizer.h"
#include "piet/handlers.h"
#include "piet/loops.h"
#include "piet/mutation_result.h"
#include "piet/scratch_seq.h"
#include "piet/session_state.h"
#include "piet/stata_format.h"
#include "piet/storage_type.h"
#include "piet/strutil.h"

// integ's spline must round every product and sum separately, as StataBE's expression
// evaluator does; a fused multiply-add changes the last bit (integ_basic is pinned to it).
#if defined(__clang__)
#pragma clang fp contract(off)
#endif

// ---------------------------------------------------------------------------
// OPTION VOCABULARIES -- namespace scope, before any dispatch. The aliases are the minimum
// abbreviations the commands' own syntax lines declare (Generate -> g, Trapezoid -> t,
// Initial -> i, Label -> l, Nquantiles -> n), which the 2-character prefix floor cannot reach.
namespace {

// mvdecode's syntax line has exactly one option, mv().
const piet::eval::OptVocab& kVocabMvdecode =
    piet::eval::registerOptVocab({"mvdecode"}, {{"mv"}, {}});

// integ: every option of the syntax line is read (double/float pick the stored type).
const piet::eval::OptVocab& kVocabInteg = piet::eval::registerOptVocab(
    {"integ"}, {{"generate", "replace", "by", "trapezoid", "initial", "double", "float"},
                {},
                {{"g", "generate"}, {"t", "trapezoid"}, {"i", "initial"}}});

// sencode: `fast` only skips the package's preserve/restore on Break, so it changes nothing
// in the answer and is read as a no-op. `manyto1` (one code per observation group rather than
// per string) and `noextend` (refuse strings the label lacks) change the codes and are not
// served.
const piet::eval::OptVocab& kVocabSencode = piet::eval::registerOptVocab(
    {"sencode"}, {{"generate", "replace", "label", "gsort", "fast"},
                  {"manyto1", "noextend"},
                  {{"g", "generate"}, {"l", "label"}}});

// fillmissing: with() and by(); `by` is also how the dispatcher hands over the by-prefix.
const piet::eval::OptVocab& kVocabFillmissing =
    piet::eval::registerOptVocab({"fillmissing"}, {{"with", "by"}, {}});

// astile: every option of the syntax line is read; qc() (the criterion the breakpoints are
// taken over) included.
const piet::eval::OptVocab& kVocabAstile = piet::eval::registerOptVocab(
    {"astile"}, {{"nquantiles", "by", "qc"}, {}, {{"n", "nquantiles"}}});

}  // namespace

namespace piet::handlers {

using ojson = nlohmann::ordered_json;

namespace {

// ===========================================================================
// Blocks and errors
// ===========================================================================

// An error with the exact Stata rc in `reason` (the one channel coarseRc reads first).
ojson rcError(const std::string& message, int rc, const std::string& code = "engine_error") {
  return ojson(eval::errorBlock(code, message, std::nullopt, std::nullopt, std::nullopt,
                                "r(" + std::to_string(rc) + ")"));
}

ojson syntaxError(const std::string& message) { return rcError(message, 198, "syntax_error"); }

ojson displayBlock(const std::string& text) {
  ojson b;
  b["type"] = "display";
  b["text"] = text;
  return b;
}

bool isErrorBlock(const ojson& b) {
  return b.is_object() && b.value("type", std::string()) == "error";
}

std::optional<ojson> firstError(const ojson& result) {
  if (isErrorBlock(result)) return result;
  if (result.is_array())
    for (const auto& b : result)
      if (isErrorBlock(b)) return b;
  return std::nullopt;
}

// ===========================================================================
// Options
// ===========================================================================

using OptList = std::vector<std::pair<std::string, eval::OptValue>>;

const eval::OptValue* findOpt(const OptList& opts, const std::string& key) {
  for (const auto& [k, v] : opts)
    if (k == key) return &v;
  return nullptr;
}

bool hasFlag(const OptList& opts, const std::string& key) {
  const eval::OptValue* v = findOpt(opts, key);
  if (v == nullptr) return false;
  return v->isBool ? v->boolValue : true;
}

// A valued option's text, trimmed; nullopt when absent or given bare.
std::optional<std::string> optStr(const OptList& opts, const std::string& key) {
  const eval::OptValue* v = findOpt(opts, key);
  if (v == nullptr || v->isBool) return std::nullopt;
  return strutil::trim(v->strValue);
}

std::vector<std::string> splitWs(const std::string& s) {
  std::vector<std::string> out;
  std::istringstream iss(s);
  std::string t;
  while (iss >> t) out.push_back(t);
  return out;
}

std::string joinSpace(const std::vector<std::string>& v) {
  std::string out;
  for (size_t i = 0; i < v.size(); ++i) {
    if (i) out += ' ';
    out += v[i];
  }
  return out;
}

bool parseStrictDouble(const std::string& sIn, double* out) {
  const std::string s = strutil::trim(sIn);
  if (s.empty() || s.find_first_of("xXnN") != std::string::npos) return false;
  try {
    size_t pos = 0;
    const double v = std::stod(s, &pos);
    if (pos != s.size()) return false;
    *out = v;
    return true;
  } catch (const std::exception&) {
    return false;
  }
}

// A value as a setcol cell: blank = missing, else the shortest round-trip decimal.
std::string numCell(double v) {
  if (std::isnan(v)) return std::string();
  return eval::pythonFloatRepr(v);
}

double toFloat(double v) { return static_cast<double>(static_cast<float>(v)); }

// ===========================================================================
// Variables
// ===========================================================================

// Varlist tokens to dataset names: wildcards, ranges and _all through loops::expandVarlist
// (the one expander), then each token exact or a unique abbreviation, as in any Stata varlist.
std::optional<ojson> resolveVars(SessionState& session, const std::vector<std::string>& toks,
                                 std::vector<std::string>* out) {
  out->clear();
  if (toks.empty()) return std::nullopt;
  loops::VarlistExpandResult ex = loops::expandVarlist(joinSpace(toks), session);
  Datastore* ds = session.datastore();
  if (ds == nullptr) return ojson(eval::noDataLoaded());
  if (ex.isError) {
    // The expander's range error (`x-zz`) carries no rc and names both ends; Stata names the
    // end that is missing, at r(111) (review gk K2).
    const std::string reason = ex.error.value("reason", std::string());
    if (reason.rfind("r(", 0) == 0) return ex.error;
    for (const auto& t : toks) {
      const size_t dash = t.find('-');
      if (dash == std::string::npos || dash == 0) continue;
      for (const std::string& end : {t.substr(0, dash), t.substr(dash + 1)})
        if (!end.empty() && !ds->has(end)) return rcError("variable " + end + " not found", 111);
    }
    return rcError(ex.error.value("message", std::string("variable not found")), 111);
  }
  const std::vector<std::string>& names = ds->names();
  for (const auto& t : ex.tokens) {
    if (ds->has(t)) {
      out->push_back(t);
      continue;
    }
    std::vector<std::string> hits;
    for (const auto& nm : names)
      if (nm.size() > t.size() && nm.compare(0, t.size(), t) == 0) hits.push_back(nm);
    if (hits.size() == 1) {
      out->push_back(hits.front());
      continue;
    }
    if (hits.size() > 1) return rcError(t + " ambiguous abbreviation", 111);
    return rcError("variable " + t + " not found", 111);
  }
  return std::nullopt;
}

bool columnExists(SessionState& session, const std::string& name) {
  Datastore* ds = session.datastore();
  return ds != nullptr && ds->has(name);
}

// Drop scratch columns that exist, through the dispatched `drop` (which also forgets their
// metadata planes). Never fails the caller: a scratch drop that fails leaves a column the
// census will name, which is the louder outcome.
void dropColumns(SessionState& session, const std::vector<std::string>& names) {
  std::vector<std::string> present;
  for (const auto& n : names)
    if (columnExists(session, n)) present.push_back(n);
  if (present.empty()) return;
  try {
    (void)dispatch("quietly drop " + joinSpace(present), session);
  } catch (const std::exception&) {
  }
}

// The scratch columns a handler made, dropped on every return path. `clear()` runs before
// the reply's card is built; the destructor is the net for an exception.
class ScratchCols {
 public:
  explicit ScratchCols(SessionState& s) : s_(s) {}
  ~ScratchCols() {
    try {
      clear();
    } catch (...) {
    }
  }
  void add(const std::string& n) { names_.push_back(n); }
  void clear() {
    if (!names_.empty()) dropColumns(s_, names_);
    names_.clear();
  }

 private:
  SessionState& s_;
  std::vector<std::string> names_;
};

bool hasQualifier(const eval::ParsedCommand& parsed) {
  return (parsed.ifExpr.has_value() && !strutil::trim(*parsed.ifExpr).empty()) ||
         (parsed.inRange.has_value() && !strutil::trim(*parsed.inRange).empty());
}

std::string qualifierText(const eval::ParsedCommand& parsed) {
  std::string q;
  if (parsed.ifExpr.has_value() && !strutil::trim(*parsed.ifExpr).empty())
    q += " if " + *parsed.ifExpr;
  if (parsed.inRange.has_value() && !strutil::trim(*parsed.inRange).empty())
    q += " in " + *parsed.inRange;
  return q;
}

// The if/in sample as a 0/1 row mask, evaluated by the dispatched `generate` (the one
// qualifier evaluator). No qualifier = every row. With `keepAs` the marker column (1 in the
// sample, missing outside) stays on the frame under that name for the caller to use and drop.
std::optional<ojson> sampleMask(SessionState& session, const eval::ParsedCommand& parsed,
                                std::vector<char>* mask, const std::string& keepAs = "") {
  Datastore* ds = session.datastore();
  if (ds == nullptr) return ojson(eval::noDataLoaded());
  const int64_t n = ds->nRows();
  mask->assign(static_cast<size_t>(n), 1);
  if (!hasQualifier(parsed)) return std::nullopt;
  const std::string tmp = keepAs.empty() ? "__piet_t1g_touse" : keepAs;
  dropColumns(session, {tmp});
  ojson r = dispatch("quietly generate byte " + tmp + " = 1" + qualifierText(parsed), session);
  if (auto e = firstError(r)) {
    dropColumns(session, {tmp});
    return *e;
  }
  ds = session.datastore();
  if (ds == nullptr || !ds->has(tmp)) return rcError("could not evaluate the if/in qualifier", 198);
  const double* d = ds->col(tmp).f64();
  for (int64_t i = 0; i < n; ++i) (*mask)[static_cast<size_t>(i)] = std::isnan(d[i]) ? 0 : 1;
  if (keepAs.empty()) dropColumns(session, {tmp});
  return std::nullopt;
}

// One column's cells as text ("" = missing): the level text of a string, the round-trip
// decimal of a number. This is the shape a setcol payload takes.
std::vector<std::string> columnCells(const Datastore& ds, const std::string& name) {
  const int64_t n = ds.nRows();
  std::vector<std::string> out;
  out.reserve(static_cast<size_t>(n));
  const cecon::Column& c = ds.col(name);
  if (c.dtype == cecon::DType::Factor) {
    const int32_t* codes = c.codes();
    for (int64_t i = 0; i < n; ++i) {
      const int32_t k = codes[i];
      if (k < 0 || static_cast<size_t>(k) >= c.levels.size())
        out.emplace_back();
      else
        out.push_back(c.levels[static_cast<size_t>(k)]);
    }
  } else {
    const double* d = c.f64();
    for (int64_t i = 0; i < n; ++i) out.push_back(numCell(d[i]));
  }
  return out;
}

std::vector<double> columnDoubles(const Datastore& ds, const std::string& name) {
  const double* d = ds.col(name).f64();
  return std::vector<double>(d, d + ds.nRows());
}

// ===========================================================================
// Writing: one column through `cobra setcol` (in place, or appended with isNew)
// ===========================================================================

std::optional<ojson> runVerb(const std::vector<std::string>& args) {
  CobraRunResult cr = runCobra(args);
  if (cr.ok) return std::nullopt;
  std::string t = cr.stderr_text;
  while (!t.empty() && (t.back() == '\n' || t.back() == '\r' || t.back() == ' ')) t.pop_back();
  try {
    nlohmann::json payload = nlohmann::json::parse(t);
    nlohmann::json err = payload.contains("error") ? payload.at("error") : payload;
    if (err.is_object() && err.contains("message")) {
      CobraError ce(err.value("code", std::string("engine_error")),
                    err.at("message").get<std::string>(), err.value("detail", std::string()));
      return ojson(eval::engineError(cobraErrorMessage(ce), ce.code));
    }
  } catch (const nlohmann::json::exception&) {
  }
  return ojson(eval::engineError(t.empty() ? "cobra failed" : t));
}

// `cells` are rendered values ("" = missing). A string column's cells are quoted so a value's
// own padding survives; an empty line is the payload's missing cell. `storeType` "" = double.
std::optional<ojson> setColumn(SessionState& session, const std::string& var,
                               const std::vector<std::string>& cells, bool isString, bool isNew,
                               const std::string& storeType) {
  const std::string hbinName = mintScratchPath(session.workdir(), "t1gen_vals");
  forgetScratchMint(hbinName);
  const std::string csvPath = hbinName.substr(0, hbinName.size() - 5) + ".csv";
  registerScratchMint(csvPath);
  {
    std::ofstream f(cecon::to_native_path(csvPath));
    if (!f) return rcError("could not stage the values of `" + var + "'", 693, "data_io");
    for (const auto& c : cells) {
      if (c.empty()) {
        f << "\n";
        continue;
      }
      if (isString) {
        f << '"';
        for (char ch : c) {
          if (ch == '"') f << '"';
          f << ch;
        }
        f << '"';
      } else {
        f << c;
      }
      f << "\n";
    }
    f.flush();
    if (!f) {
      cecon::native_unlink(csvPath);
      forgetScratchMint(csvPath);
      return ojson(eval::engineError("could not write the values of `" + var +
                                         "' (disk full or I/O error)",
                                     "data_io"));
    }
  }
  const std::string out = mintScratchPath(session.workdir(), "t1gen_setcol");
  std::vector<std::string> args = {"setcol", "--data", *session.currentHbin(), "--var", var,
                                   "--values", csvPath, "--dtype", isString ? "factor" : "f64"};
  if (!isString) storage::appendStoreTypeArg(args, storeType);
  if (isNew) args.push_back("--new");
  args.push_back("--out");
  args.push_back(out);
  std::optional<ojson> err = runVerb(args);
  cecon::native_unlink(csvPath);
  forgetScratchMint(csvPath);
  if (err) {
    cecon::native_unlink(out);
    return err;
  }
  session.load(out);
  return std::nullopt;
}

// Stata's abbrev(name, 12): longer names keep their first ten characters, a `~`, and their
// last character.
std::string abbrev12(const std::string& name) {
  if (name.size() <= 12) return name;
  return name.substr(0, 10) + "~" + name.substr(name.size() - 1);
}

std::string rightAlign(const std::string& s, size_t w) {
  return s.size() >= w ? s : std::string(w - s.size(), ' ') + s;
}

// ===========================================================================
// Group keys (by-variables, plus the sample marker where the package groups on it)
// ===========================================================================

struct KeyCol {
  bool isString = false;
  std::vector<double> num;
  std::vector<std::string> str;  // "" = missing
};

KeyCol readKeyCol(const Datastore& ds, const std::string& name) {
  KeyCol k;
  if (ds.col(name).dtype == cecon::DType::Factor) {
    k.isString = true;
    k.str = columnCells(ds, name);
  } else {
    k.num = columnDoubles(ds, name);
    for (double& v : k.num)
      if (v == 0.0) v = 0.0;  // -0 and +0 are one Stata value
  }
  return k;
}

bool keyMissing(const KeyCol& k, size_t i) {
  return k.isString ? k.str[i].empty() : std::isnan(k.num[i]);
}

// Stata's sort order on one key: numbers ascending with missing last, strings by bytes.
int keyCompare(const KeyCol& k, size_t a, size_t b) {
  if (k.isString) {
    const int c = k.str[a].compare(k.str[b]);
    return c < 0 ? -1 : (c > 0 ? 1 : 0);
  }
  const double x = k.num[a], y = k.num[b];
  const bool mx = std::isnan(x), my = std::isnan(y);
  if (mx || my) return mx == my ? 0 : (mx ? 1 : -1);
  return x < y ? -1 : (x > y ? 1 : 0);
}

int keysCompare(const std::vector<KeyCol>& keys, size_t a, size_t b) {
  for (const auto& k : keys) {
    const int c = keyCompare(k, a, b);
    if (c != 0) return c;
  }
  return 0;
}

// A dense group id per row over the key columns (first-seen numbering).
std::vector<int64_t> groupIds(const std::vector<KeyCol>& keys, size_t n, int64_t* nGroups) {
  std::vector<int64_t> gid(n, 0);
  std::unordered_map<std::string, int64_t> seen;
  for (size_t i = 0; i < n; ++i) {
    std::string key;
    for (const auto& k : keys) {
      if (k.isString) {
        key += 'S';
        key += k.str[i];
        key += '\x1f';
      } else {
        double v = k.num[i];
        uint64_t bits = 0;
        if (std::isnan(v)) {
          bits = 0xFFFFFFFFFFFFFFFFull;
        } else {
          std::memcpy(&bits, &v, sizeof(bits));
        }
        key += 'N';
        key.append(reinterpret_cast<const char*>(&bits), sizeof(bits));
      }
    }
    auto it = seen.find(key);
    if (it == seen.end()) it = seen.emplace(key, static_cast<int64_t>(seen.size())).first;
    gid[i] = it->second;
  }
  *nGroups = static_cast<int64_t>(seen.size());
  return gid;
}

// ===========================================================================
// mvdecode
// ===========================================================================

// A generated numlist element as Stata's numlist hands it back: as text with 15 significant
// digits, so 0(0.1)1 yields .3 itself and not 0.30000000000000004 (review gi I1/I3, gg G1).
double numlistElement(double v) {
  char buf[64];
  std::snprintf(buf, sizeof(buf), "%.15g", v);
  return std::strtod(buf, nullptr);
}

// The elements from `a` to `b` in steps of `d`, the first one excluded when `skipFirst`.
bool numlistRun(double a, double d, double b, bool skipFirst, std::vector<double>* out) {
  if (d == 0.0 || (b - a) / d < 0) return false;
  for (int64_t k = skipFirst ? 1 : 0;; ++k) {
    const double v = numlistElement(a + static_cast<double>(k) * d);
    if (d > 0 ? v > b : v < b) break;
    out->push_back(v);
    if (out->size() > 1000000) return false;
  }
  return true;
}

// A numlist, Stata's spellings ([U] 11.1.8): #, #/#, #(#)#, #[#]#, and `#1 #2 to #3` /
// `#1 #2 : #3` (the step is #2 - #1). Anything else is r(121) with the whole numlist named,
// as mvdecode reports it (probe p2 mvbad).
std::optional<ojson> parseNumlist(const std::string& text, std::vector<double>* out) {
  const std::string bad = text + " is not a valid numlist";
  std::string spec;
  for (char c : text) {
    if (c == ',')
      spec += ' ';
    else if (c == ':')
      spec += " : ";
    else
      spec += c;
  }
  const std::vector<std::string> toks = splitWs(spec);
  if (toks.empty()) return rcError(bad, 121);
  int plainRun = 0;  // how many of the last elements were plain numbers
  for (size_t i = 0; i < toks.size(); ++i) {
    const std::string& tok = toks[i];
    double a = 0, b = 0, d = 0;
    if (tok == "to" || tok == ":") {
      if (plainRun < 2 || i + 1 >= toks.size() || !parseStrictDouble(toks[i + 1], &b))
        return rcError(bad, 121);
      ++i;
      const double a1 = (*out)[out->size() - 2], a2 = out->back();
      if (!numlistRun(a2, a2 - a1, b, true, out)) return rcError(bad, 121);
      plainRun = 0;
      continue;
    }
    const size_t slash = tok.find('/', 1);
    const size_t lp = tok.find_first_of("([", 1);
    if (slash != std::string::npos) {
      if (!parseStrictDouble(tok.substr(0, slash), &a) ||
          !parseStrictDouble(tok.substr(slash + 1), &b) ||
          !numlistRun(a, a <= b ? 1.0 : -1.0, b, false, out))
        return rcError(bad, 121);
      plainRun = 0;
      continue;
    }
    if (lp != std::string::npos) {
      const size_t rp = tok.find(tok[lp] == '(' ? ')' : ']', lp);
      if (rp == std::string::npos || !parseStrictDouble(tok.substr(0, lp), &a) ||
          !parseStrictDouble(tok.substr(lp + 1, rp - lp - 1), &d) ||
          !parseStrictDouble(tok.substr(rp + 1), &b) || !numlistRun(a, d, b, false, out))
        return rcError(bad, 121);
      plainRun = 0;
      continue;
    }
    if (!parseStrictDouble(tok, &a)) return rcError(bad, 121);
    out->push_back(a);
    ++plainRun;
  }
  return std::nullopt;
}

// mv(): a bare numlist (every value -> .), or rules `numlist = code [\ numlist = code ...]`.
// The grammar and its three messages are mvdecode's own (official log mvdecode_*; probe p2
// mvbad r(121), mvbadcode r(198)). An extended-missing code (.a-.z) cannot be stored by this
// build, which has one missing value, so that rule is refused by name.
std::optional<ojson> parseMv(const std::string& spec, std::vector<double>* values) {
  if (spec.find_first_of("=\\") == std::string::npos) return parseNumlist(spec, values);
  std::vector<std::string> toks;
  std::string cur;
  auto flush = [&]() {
    const std::string t = strutil::trim(cur);
    if (!t.empty()) toks.push_back(t);
    cur.clear();
  };
  for (char c : spec) {
    if (c == '=' || c == '\\') {
      flush();
      toks.emplace_back(1, c);
    } else {
      cur += c;
    }
  }
  flush();
  size_t i = 0;
  auto next = [&]() -> std::string { return i < toks.size() ? toks[i++] : std::string(); };
  std::string tok = next();
  while (!tok.empty()) {
    std::vector<double> lhs;
    if (auto e = parseNumlist(tok, &lhs)) return e;
    if (next() != "=") return syntaxError("= expected");
    const std::string code = next();
    const bool sysmiss = code == ".";
    const bool extended = code.size() == 2 && code[0] == '.' && code[1] >= 'a' && code[1] <= 'z';
    if (!sysmiss && !extended) return syntaxError(code + " is not a missing value code");
    if (extended)
      return ojson(eval::unsupportedOption(
          "mv(" + spec + ")",
          "mvdecode: the rule `= " + code + "' stores an extended missing value; this version "
          "of PIET stores one missing value (.), so .a-.z cannot be kept apart. Use `= .`."));
    values->insert(values->end(), lhs.begin(), lhs.end());
    const std::string sep = next();
    if (!sep.empty() && sep != "\\") return syntaxError(sep + " found, where \\ expected");
    tok = next();
  }
  return std::nullopt;
}

}  // namespace

// `mvdecode varlist [if] [in], mv(numlist | numlist=. [\ numlist=. ...])`. Implements
// mvdecode per [D] mvencode: each listed value becomes `.`; a float variable is compared with
// float(#); string variables are reported and skipped; each variable that changed prints
// `<name>: N missing value(s) generated` (official log mvdecode_single/_rules/_withstr/_all).
ojson runMvdecode(const eval::ParsedCommand& parsed, SessionState& session) {
  if (auto err = eval::rejectUnknownOptions(parsed.opts, kVocabMvdecode.known,
                                            kVocabMvdecode.unimplemented))
    return ojson(*err);
  if (!session.hasData()) return ojson(eval::noDataLoaded());
  if (parsed.varlist.empty()) return rcError("varlist required", 100);
  std::vector<std::string> vars;
  if (auto e = resolveVars(session, parsed.varlist, &vars)) return *e;
  const std::optional<std::string> mv = optStr(parsed.opts, "mv");
  if (!mv.has_value()) return syntaxError("option mv() required");  // mvdecode_nomv r(198)
  // An empty sample exits before mv() is read, as mvdecode does (review gg G4: `if 0, mv(bad)`
  // is rc 0).
  std::vector<char> mask;
  if (auto e = sampleMask(session, parsed, &mask)) return *e;
  if (std::find(mask.begin(), mask.end(), 1) == mask.end()) return ojson::array();
  std::vector<double> values;
  if (auto e = parseMv(*mv, &values)) return *e;

  std::string text;
  std::vector<std::string> rewritten;
  for (const auto& v : vars) {
    Datastore* ds = session.datastore();
    if (ds == nullptr) return ojson(eval::noDataLoaded());
    if (ds->col(v).dtype == cecon::DType::Factor) {
      text += rightAlign(abbrev12(v), 12) + ": string variable ignored\n";
      continue;
    }
    const bool isFloat = storage::resolveReplaceType(session, v) == "float";
    std::vector<double> targets;
    for (double t : values) targets.push_back(isFloat ? toFloat(t) : t);
    std::vector<double> col = columnDoubles(*ds, v);
    int64_t changed = 0;
    for (size_t i = 0; i < col.size(); ++i) {
      if (!mask[i] || std::isnan(col[i])) continue;
      if (std::find(targets.begin(), targets.end(), col[i]) != targets.end()) {
        col[i] = std::numeric_limits<double>::quiet_NaN();
        ++changed;
      }
    }
    if (changed == 0) continue;
    std::vector<std::string> cells;
    cells.reserve(col.size());
    for (double x : col) cells.push_back(numCell(x));
    if (auto e = setColumn(session, v, cells, false, false, "")) return *e;
    if (std::find(rewritten.begin(), rewritten.end(), v) == rewritten.end()) rewritten.push_back(v);
    text += rightAlign(abbrev12(v), 12) + ": " + std::to_string(changed) + " missing value" +
            (changed > 1 ? "s" : "") + " generated\n";
  }
  ojson out = ojson::array();
  if (!text.empty()) out.push_back(displayBlock(text));
  if (!rewritten.empty()) out.push_back(mutationResult(session, "gen", {}, {}, rewritten));
  return out;
}

namespace {

// ===========================================================================
// integ -- the cubic spline of [R] dydx, Methods and formulas
// ===========================================================================

// Second derivatives y'' of the spline through n >= 3 points with distinct, ascending x. The
// end slopes are those of the quadratic through the three points at each end; the interior
// rows are the tridiagonal sweep, then the back substitution. Each operation is written in
// the order StataBE evaluates it, unfused; that reproduces integ_basic (34.83333333333333)
// and probe p2 i3 (+1.1555555555555X+003) to the last bit. On irregularly spaced x a 1-ulp
// difference can remain (review gd D1: +1.0500c0dd332deX+004 in StataBE, ...dd here).
std::vector<double> splineSecondDerivs(const std::vector<double>& x, const std::vector<double>& y) {
  const size_t n = x.size();
  std::vector<double> s(n, 0.0), z(n, 0.0), y2(n, 0.0);
  s[0] = -0.5;
  z[0] = (3.0 / (x[1] - x[0])) * ((y[1] - y[2]) / (x[1] - x[2]) - (y[0] - y[2]) / (x[0] - x[2]));
  for (size_t i = 1; i + 1 < n; ++i) {
    const double w = (x[i] - x[i - 1]) / (x[i + 1] - x[i - 1]);
    const double p = w * s[i - 1] + 2.0;
    s[i] = (w - 1.0) / p;
    z[i] = (6.0 * ((y[i + 1] - y[i]) / (x[i + 1] - x[i]) - (y[i] - y[i - 1]) / (x[i] - x[i - 1])) /
                (x[i + 1] - x[i - 1]) -
            w * z[i - 1]) /
           p;
  }
  const size_t m = n - 1;
  y2[m] = ((6.0 / (x[m] - x[m - 1])) *
               ((y[m] - y[m - 2]) / (x[m] - x[m - 2]) - (y[m - 1] - y[m - 2]) / (x[m - 1] - x[m - 2])) -
           z[m - 1]) /
          (s[m - 1] + 2.0);
  for (size_t k = m; k-- > 0;) y2[k] = s[k] * y2[k + 1] + z[k];
  return y2;
}

// The running integral at each point: I0 + sum over the intervals so far of the spline's exact
// integral, h(y_k + y_k+1)/2 - h^3(y''_k + y''_k+1)/24 (the trapezoid alone with `trapezoid`, or
// when there are fewer than three points).
std::vector<double> runningIntegral(const std::vector<double>& x, const std::vector<double>& y,
                                    bool trapezoid, double initial) {
  const size_t n = x.size();
  const bool spline = !trapezoid && n > 2;
  std::vector<double> y2;
  if (spline) y2 = splineSecondDerivs(x, y);
  std::vector<double> out(n, 0.0);
  double run = 0.0;
  for (size_t i = 0; i < n; ++i) {
    if (i > 0) {
      const double h = x[i] - x[i - 1];
      const double c = spline ? (h * h * (y2[i] + y2[i - 1])) / 12.0 : 0.0;
      run += (h * ((y[i] + y[i - 1]) - c)) / 2.0;
    }
    out[i] = initial + run;
  }
  return out;
}

}  // namespace

// `integ yvar xvar [if] [in] [, generate(newvar) replace by(varlist) trapezoid initial(#)
// double float]`. Implements integ per [R] dydx: the points are the observations with both
// variables non-missing (and every by() variable non-missing), taken in ascending x; tied x
// values are replaced by their mean y. Without by() it prints the count and the integral and
// sets r(N_points) and r(integral); with by() it prints nothing and r() is cleared
// (probe p2 iby). generate() is float unless `double` (or set type double), labelled
// "Integral of <y>" (probe p2 itiesg).
ojson runInteg(const eval::ParsedCommand& parsed, SessionState& session) {
  if (auto err = eval::rejectUnknownOptions(parsed.opts, kVocabInteg.known,
                                            kVocabInteg.unimplemented))
    return ojson(*err);
  if (!session.hasData()) return ojson(eval::noDataLoaded());
  std::vector<std::string> vars;
  if (auto e = resolveVars(session, parsed.varlist, &vars)) return *e;
  if (vars.size() < 2) return rcError("too few variables specified", 102);  // probe p2 ione
  if (vars.size() > 2) return rcError("too many variables specified", 103);
  const std::string yvar = vars[0], xvar = vars[1];

  const std::optional<std::string> gen = optStr(parsed.opts, "generate");
  const bool replaceOpt = hasFlag(parsed.opts, "replace");
  const bool trapezoid = hasFlag(parsed.opts, "trapezoid");
  const bool dbl = hasFlag(parsed.opts, "double");
  const bool flt = hasFlag(parsed.opts, "float");
  std::vector<std::string> byToks;
  if (auto b = optStr(parsed.opts, "by")) byToks = splitWs(*b);
  if (!gen.has_value() || gen->empty()) {
    if (!byToks.empty())
      return syntaxError("you must specify option generate() with option by()");  // ibynogen
    if (replaceOpt) return syntaxError("you must specify option generate() with option replace");
    if (dbl) return syntaxError("you must specify option generate() with option double");
  }
  if (dbl && flt) return syntaxError("only one of double or float is allowed");
  double initial = 0.0;
  if (auto iv = optStr(parsed.opts, "initial")) {
    if (!parseStrictDouble(*iv, &initial))
      return syntaxError("option initial() incorrectly specified");
  }
  std::string genVar;
  if (gen.has_value() && !gen->empty()) {
    const std::vector<std::string> names = splitWs(*gen);
    for (const auto& nm : names) {
      if (!strutil::isValidStataIdentifier(nm)) return syntaxError(nm + " invalid name");
      if (columnExists(session, nm) && !replaceOpt)
        return rcError("option generate():\nvariable " + nm + " already defined", 110);  // iexist
    }
    if (names.size() > 1) return rcError("option generate():  too many variables specified", 103);
    genVar = names.front();
  }
  std::vector<std::string> byVars;
  if (auto e = resolveVars(session, byToks, &byVars)) return *e;

  std::vector<char> mask;
  if (auto e = sampleMask(session, parsed, &mask)) return *e;
  Datastore* ds = session.datastore();
  if (ds == nullptr) return ojson(eval::noDataLoaded());
  if (ds->col(yvar).dtype == cecon::DType::Factor || ds->col(xvar).dtype == cecon::DType::Factor)
    return rcError("type mismatch", 109);
  const std::vector<double> y = columnDoubles(*ds, yvar);
  const std::vector<double> x = columnDoubles(*ds, xvar);
  std::vector<KeyCol> keys;
  for (const auto& b : byVars) keys.push_back(readKeyCol(*ds, b));
  const size_t n = y.size();

  // The sample: if/in, both variables non-missing, every by() variable non-missing.
  std::vector<size_t> rows;
  for (size_t i = 0; i < n; ++i) {
    if (!mask[i] || std::isnan(x[i]) || std::isnan(y[i])) continue;
    bool byMiss = false;
    for (const auto& k : keys) byMiss = byMiss || keyMissing(k, i);
    if (!byMiss) rows.push_back(i);
  }
  // Ascending x within each by() group; a tie keeps the current row order.
  std::stable_sort(rows.begin(), rows.end(), [&](size_t a, size_t b) {
    const int c = keysCompare(keys, a, b);
    if (c != 0) return c < 0;
    return x[a] < x[b];
  });

  std::vector<double> integral(n, std::numeric_limits<double>::quiet_NaN());
  int64_t nPoints = 0;
  double lastIntegral = std::numeric_limits<double>::quiet_NaN();
  bool anyTie = false;
  size_t g0 = 0;
  while (g0 < rows.size()) {
    size_t g1 = g0 + 1;
    while (g1 < rows.size() && keysCompare(keys, rows[g1], rows[g0]) == 0) ++g1;
    // One point per distinct x: the mean y over a tie (the running sum over the tie divided
    // by its size, in sort order), placed on the tie's last row.
    std::vector<double> px, py;
    std::vector<size_t> at;
    size_t t0 = g0;
    while (t0 < g1) {
      size_t t1 = t0 + 1;
      while (t1 < g1 && x[rows[t1]] == x[rows[t0]]) ++t1;
      double sum = 0.0;
      for (size_t q = t0; q < t1; ++q) sum += y[rows[q]];
      if (t1 - t0 > 1) anyTie = true;
      px.push_back(x[rows[t0]]);
      py.push_back(t1 - t0 > 1 ? sum / static_cast<double>(t1 - t0) : y[rows[t0]]);
      at.push_back(rows[t1 - 1]);
      t0 = t1;
    }
    std::vector<double> I = runningIntegral(px, py, trapezoid, initial);
    // `float` makes the running integral a float variable: every value, r(integral) among
    // them, is stored as a float (review gd D5/D10).
    if (flt)
      for (double& v : I) v = toFloat(v);
    for (size_t k = 0; k < I.size(); ++k) integral[at[k]] = I[k];
    nPoints += static_cast<int64_t>(px.size());
    if (!I.empty()) lastIntegral = I.back();
    g0 = g1;
  }
  if (nPoints == 0) return rcError("no observations", 2000);  // probe p2 inone
  // StataBE keeps a tie's value on ONE of its rows, the one its own (unstable) sort puts last
  // -- probe p2 irepl stored it on the FIRST row of the tie {1,4} and the LAST of {3,7}. Which
  // row that is cannot be reproduced, so a generate() over tied x is refused rather than
  // written to a row Stata might not have chosen. The integral itself does not depend on it.
  if (!genVar.empty() && anyTie)
    return rcError(
        "integ: " + xvar + " has tied values in the sample. Stata stores generate() on one row "
        "of each tie, chosen by its own sort, which this version of PIET cannot reproduce; the "
        "integral itself is served without generate(). Collapse the ties first (e.g. `collapse "
        "(mean) " + yvar + ", by(" + xvar + ")`).",
        199);

  ojson out = ojson::array();
  if (byVars.empty()) {
    eval::replaceRClass(session.eval);
    session.eval.rValues.set("N_points", static_cast<double>(nPoints));
    session.eval.rValues.set("integral", lastIntegral);
    eval::declareRPosted(session.eval, "integ", {"N_points", "integral"});
    out.push_back(displayBlock("\nnumber of points = " + std::to_string(nPoints) +
                               "\n\nintegral         = " +
                               strutil::trim(fmt::general(lastIntegral, 10, 0)) + "\n"));
  } else {
    // integ is r-class and posts nothing with by(): r() is emptied, and a read of
    // r(integral) is `.` (probe p2 iby).
    eval::replaceRClass(session.eval);
    eval::declareRPosted(session.eval, "integ", {});
  }
  if (!genVar.empty()) {
    const bool asDouble = dbl || (!flt && storage::resolveCreateType(session, "") == "double");
    const std::string type = asDouble ? "double" : "float";
    if (replaceOpt && columnExists(session, genVar)) dropColumns(session, {genVar});
    std::vector<std::string> cells;
    cells.reserve(n);
    for (double v : integral) cells.push_back(numCell(v));
    if (auto e = setColumn(session, genVar, cells, false, true, type)) return *e;
    storage::recordCreateType(session, genVar, type, type);
    session.setVarLabel(genVar, "Integral of " + yvar);
    out.push_back(mutationResult(session, "gen", {genVar}));
  }
  return out;
}

namespace {

// ===========================================================================
// sencode
// ===========================================================================

struct GsortKey {
  std::string var;
  bool descending = false;
};

// The value-label set `lbl` with `var` attached, and every variable carrying the set given
// its new {code: text} map -- the label-store writes runEncode makes.
void attachLabelSet(SessionState& session, const std::string& var, const std::string& lbl,
                    const std::map<std::string, std::string>& codeMap) {
  session.labelDefs[lbl] = codeMap;
  session.stampLabelSet(lbl);
  session.varLblname[var] = lbl;
  for (const auto& [v, lname] : session.varLblname) {
    if (lname != lbl) continue;
    std::map<int, std::string> numeric;
    for (const auto& [codeStr, text] : codeMap) {
      try {
        numeric[std::stoi(codeStr)] = text;
      } catch (const std::exception&) {
      }
    }
    if (numeric.empty())
      session.valueLabels.erase(v);
    else
      session.valueLabels[v] = numeric;
  }
}

// What `compress` leaves a positive integer code in.
std::string compressedCodeType(long long maxCode) {
  if (maxCode <= 100) return "byte";
  if (maxCode <= 32740) return "int";
  return "long";
}

}  // namespace

// `sencode strvar [if] [in], generate(newvar) | replace [label(name[, replace]) gsort(keys)
// fast]`. Implements sencode per its help file (Newson): one code per distinct non-empty
// string, numbered in order of first appearance (community log sencode_basic: b a c -> 1 2 3)
// or in the order of a gsort() key list (sencode_gsort / _gsortdesc); "" and rows outside
// if/in are missing and are not labelled (probe p4 sin). The value label is label() or the
// new variable's name; an existing set keeps its codes and new strings take its largest code
// + 1 in that order (probe p3 slab: pre {7 c} -> b 8, a 9, c 7). The variable is stored as
// `compress` leaves it (byte for <= 100 codes), %8.0g, with the string's variable label.
// `replace` puts the codes in the string variable's place, label named after it.
ojson runSencode(const eval::ParsedCommand& parsed, SessionState& session) {
  if (auto err = eval::rejectUnknownOptions(parsed.opts, kVocabSencode.known,
                                            kVocabSencode.unimplemented))
    return ojson(*err);
  if (!session.hasData()) return ojson(eval::noDataLoaded());
  if (parsed.varlist.empty()) return rcError("varlist required", 100);
  std::vector<std::string> vars;
  if (auto e = resolveVars(session, parsed.varlist, &vars)) return *e;
  if (vars.size() > 1) return rcError("too many variables specified", 103);
  const std::string src = vars.front();
  {
    Datastore* ds = session.datastore();
    if (ds == nullptr) return ojson(eval::noDataLoaded());
    if (ds->col(src).dtype != cecon::DType::Factor)  // probe p3 snum
      return rcError("numeric variables not allowed in varlist;\n" + src + " is a numeric variable",
                     109);
  }
  const bool replaceOpt = hasFlag(parsed.opts, "replace");
  const std::optional<std::string> genOpt = optStr(parsed.opts, "generate");
  const bool haveGen = genOpt.has_value() && !genOpt->empty();
  if (replaceOpt && haveGen)  // probe p3 sboth
    return syntaxError("options generate() and replace are mutually exclusive");
  if (!replaceOpt && !haveGen)  // sencode_noopt
    return syntaxError("must specify either generate() or replace option");

  // label(name [, replace]).
  std::string lbl;
  bool lreplace = false;
  if (auto lo = optStr(parsed.opts, "label")) {
    std::string spec = *lo;
    const size_t comma = spec.find(',');
    if (comma != std::string::npos) {
      const std::string sub = strutil::lower(strutil::trim(spec.substr(comma + 1)));
      if (sub == "replace") {
        lreplace = true;
      } else if (!sub.empty()) {
        return syntaxError("option " + sub + " not allowed");
      }
      spec = spec.substr(0, comma);
    }
    lbl = strutil::trim(spec);
    if (!lbl.empty() && !strutil::isValidStataIdentifier(lbl))
      return syntaxError(lbl + " invalid name");
  }

  std::string target = src;
  if (haveGen) {
    const std::vector<std::string> names = splitWs(*genOpt);
    // A generate() that is not one name reaches the package's label lookup as the label name
    // and fails there (probe p4 sbadname / stwonames: st_vlexists() 3300).
    if (names.size() != 1 || !strutil::isValidStataIdentifier(names.front())) {
      if (lbl.empty()) return rcError("st_vlexists():  3300  argument out of range", 3300);
      return syntaxError(*genOpt + " invalid name");
    }
    target = names.front();
    if (columnExists(session, target))  // sencode_exists r(110)
      return rcError("variable " + target + " already defined", 110);
  }
  if (lbl.empty()) lbl = target;

  // gsort(keylist [, mfirst]).
  std::vector<GsortKey> gkeys;
  bool mfirst = false;
  if (auto go = optStr(parsed.opts, "gsort")) {
    std::string spec = *go;
    const size_t comma = spec.find(',');
    if (comma != std::string::npos) {
      for (const auto& w : splitWs(spec.substr(comma + 1))) {
        if (strutil::lower(w) == "mfirst" || strutil::lower(w) == "mf")
          mfirst = true;
        else
          return ojson(eval::unsupportedOption(
              "gsort(" + *go + ")", "sencode: gsort()'s option `" + w + "' is not served"));
      }
      spec = spec.substr(0, comma);
    }
    for (std::string tok : splitWs(spec)) {
      GsortKey k;
      while (!tok.empty() && (tok[0] == '-' || tok[0] == '+')) {
        k.descending = tok[0] == '-';
        tok.erase(0, 1);
      }
      if (tok.empty()) continue;  // a bare sign belongs to the next name
      std::vector<std::string> one;
      if (auto e = resolveVars(session, {tok}, &one)) return *e;
      for (const auto& v : one) gkeys.push_back(GsortKey{v, k.descending});
    }
  }

  std::vector<char> mask;
  if (auto e = sampleMask(session, parsed, &mask)) return *e;
  Datastore* ds = session.datastore();
  if (ds == nullptr) return ojson(eval::noDataLoaded());
  const std::vector<std::string> text = columnCells(*ds, src);
  const size_t n = text.size();
  std::vector<KeyCol> keyCols;
  for (const auto& k : gkeys) keyCols.push_back(readKeyCol(*ds, k.var));

  // The order rows are visited in: the gsort() keys, then the string itself; with no gsort()
  // the row order. Each string's place is its first row in that order.
  std::vector<size_t> order;
  for (size_t i = 0; i < n; ++i)
    if (mask[i] && !text[i].empty()) order.push_back(i);
  if (!gkeys.empty()) {
    std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b) {
      for (size_t j = 0; j < gkeys.size(); ++j) {
        const KeyCol& k = keyCols[j];
        const bool ma = keyMissing(k, a), mb = keyMissing(k, b);
        if (!k.isString && ma != mb) {
          // gsort: missing last ascending; descending keeps it last unless mfirst.
          const bool aFirst = gkeys[j].descending && mfirst ? ma : mb;
          return aFirst;
        }
        int c = keyCompare(k, a, b);
        if (gkeys[j].descending) c = -c;
        if (c != 0) return c < 0;
      }
      return text[a] < text[b];
    });
  }
  std::vector<std::string> sequence;
  {
    std::set<std::string> seen;
    for (size_t i : order)
      if (seen.insert(text[i]).second) sequence.push_back(text[i]);
  }

  // The label: an existing set (unless label(, replace)) keeps every code it has; a string it
  // lacks takes the set's largest code + 1.
  std::map<std::string, std::string> codeMap;
  if (!lreplace) {
    auto it = session.labelDefs.find(lbl);
    if (it != session.labelDefs.end()) codeMap = it->second;
  }
  std::map<std::string, long long> codeOfText;
  long long maxCode = 0;
  for (const auto& [codeStr, t] : codeMap) {
    long long c = 0;
    try {
      c = std::stoll(codeStr);
    } catch (const std::exception&) {
      continue;
    }
    maxCode = std::max(maxCode, c);
    auto ins = codeOfText.emplace(t, c);
    if (!ins.second && c < ins.first->second) ins.first->second = c;
  }
  std::map<std::string, long long> codeFor;
  long long maxUsed = 0;
  for (const auto& s : sequence) {
    long long code = 0;
    auto known = codeOfText.find(s);
    if (known != codeOfText.end()) {
      code = known->second;
    } else {
      code = ++maxCode;
      codeMap[std::to_string(code)] = s;
      codeOfText.emplace(s, code);
    }
    codeFor[s] = code;
    maxUsed = std::max(maxUsed, code);
  }

  std::vector<std::string> cells;
  cells.reserve(n);
  for (size_t i = 0; i < n; ++i) {
    if (!mask[i] || text[i].empty()) {
      cells.emplace_back();
      continue;
    }
    cells.push_back(std::to_string(codeFor[text[i]]));
  }
  std::optional<std::string> srcLabel;
  {
    auto it = session.varMeta.find(src);
    if (it != session.varMeta.end() && it->second.label.has_value()) srcLabel = it->second.label;
  }
  if (auto e = setColumn(session, target, cells, false, haveGen, "")) return *e;

  if (lreplace) {
    for (const auto& [v, lname] : session.varLblname)
      if (lname == lbl) session.valueLabels.erase(v);
  }
  attachLabelSet(session, target, lbl, codeMap);
  VarMeta& vm = session.varMeta[target];
  vm.stataType = compressedCodeType(maxUsed);
  vm.format = std::string("%8.0g");
  if (haveGen && srcLabel.has_value() && !srcLabel->empty()) vm.label = srcLabel;
  session.implicitFloatVars.erase(target);
  session.userFormattedVars.erase(target);
  if (haveGen) return mutationResult(session, "gen", {target});
  return mutationResult(session, "gen", {}, {}, {target});
}

namespace {

// ===========================================================================
// fillmissing
// ===========================================================================

enum class FillMode { Any, Previous, Next, First, Last, Mean, Min, Max, Median, Inert };

bool fillModeOf(const std::string& w, FillMode* m) {
  static const std::map<std::string, FillMode> kModes = {
      {"any", FillMode::Any},       {"previous", FillMode::Previous}, {"next", FillMode::Next},
      {"first", FillMode::First},   {"last", FillMode::Last},         {"mean", FillMode::Mean},
      {"min", FillMode::Min},       {"max", FillMode::Max},           {"median", FillMode::Median},
      {"rand", FillMode::Inert},    {"pattern", FillMode::Inert}};
  auto it = kModes.find(w);
  if (it == kModes.end()) return false;
  *m = it->second;
  return true;
}

bool isStatMode(FillMode m) {
  return m == FillMode::Mean || m == FillMode::Min || m == FillMode::Max || m == FillMode::Median;
}

bool isOrderMode(FillMode m) {
  return m == FillMode::Previous || m == FillMode::Next || m == FillMode::First ||
         m == FillMode::Last;
}

// The copy fills, over one column's cells ("" = missing), within groups of rows that share a
// group id, in the current row order. Only rows in the sample are filled; a group is either
// wholly in or wholly out of the sample (the sample marker is part of the group key).
// `firstPass` receives how many cells the package's first `replace` of with(next) fills (a
// missing row followed by a non-missing one); the rest are its second, reversed pass.
std::vector<std::string> copyFill(const std::vector<std::string>& cells,
                                  const std::vector<double>* num, FillMode mode,
                                  const std::vector<int64_t>& gid, int64_t nGroups,
                                  const std::vector<char>& mask, int64_t* firstPass) {
  *firstPass = 0;
  const size_t n = cells.size();
  std::vector<std::vector<size_t>> members(static_cast<size_t>(nGroups));
  for (size_t i = 0; i < n; ++i) members[static_cast<size_t>(gid[i])].push_back(i);
  std::vector<std::string> out = cells;
  for (const auto& rows : members) {
    if (rows.empty() || !mask[rows.front()]) continue;
    switch (mode) {
      case FillMode::Any: {
        // The smallest non-missing value: the package sorts the group descending and carries
        // the last non-missing value into the missing rows below it.
        const std::string* best = nullptr;
        size_t bestRow = 0;
        for (size_t r : rows) {
          if (cells[r].empty()) continue;
          const bool better =
              best == nullptr || (num != nullptr ? (*num)[r] < (*num)[bestRow] : cells[r] < *best);
          if (better) {
            best = &cells[r];
            bestRow = r;
          }
        }
        if (best != nullptr)
          for (size_t r : rows)
            if (out[r].empty()) out[r] = *best;
        break;
      }
      case FillMode::Previous:
        for (size_t k = 1; k < rows.size(); ++k)
          if (out[rows[k]].empty()) out[rows[k]] = out[rows[k - 1]];
        break;
      case FillMode::Next:
        for (size_t k = 0; k + 1 < rows.size(); ++k)
          if (cells[rows[k]].empty() && !cells[rows[k + 1]].empty()) ++*firstPass;
        for (size_t k = rows.size(); k-- > 1;)
          if (out[rows[k - 1]].empty()) out[rows[k - 1]] = out[rows[k]];
        break;
      case FillMode::First: {
        const std::string v = cells[rows.front()];
        for (size_t r : rows)
          if (out[r].empty()) out[r] = v;
        break;
      }
      case FillMode::Last: {
        const std::string v = cells[rows.back()];
        for (size_t r : rows)
          if (out[r].empty()) out[r] = v;
        break;
      }
      default:
        break;
    }
  }
  return out;
}

// replace's and egen's notes: "(N real change(s) made)", "(N missing value(s) generated)".
std::string changesNote(int64_t n) {
  return "(" + std::to_string(n) + " real change" + (n == 1 ? "" : "s") + " made)\n";
}
std::string missingNote(int64_t n) {
  return "(" + std::to_string(n) + " missing value" + (n == 1 ? "" : "s") + " generated)\n";
}

}  // namespace

// `fillmissing varlist [if] [in] [, with(any|previous|next|first|last|mean|min|max|median)
// by(varlist)]`; `by g:` arrives as by(). Implements fillmissing per its help file (Shah):
// within each by() group of the sample, a missing value takes the group's smallest value
// (any, the default; probe p1 any), the previous / next non-missing value, the value at the
// group's first / last row even when that is missing (fill_first_by: all four stay .), or the
// group's mean / min / max / median as egen stores it (float: probe p2 meand 5/3 ->
// +1.aaaaaa0000000X+000; a byte variable widens to float, probe p3 meanb). A string variable
// takes any/previous/next/first/last; with a statistic it is reported and skipped at rc 0
// (fill_str_mean). The data keep their row order.
ojson runFillmissing(const eval::ParsedCommand& parsed, SessionState& session) {
  if (auto err = eval::rejectUnknownOptions(parsed.opts, kVocabFillmissing.known,
                                            kVocabFillmissing.unimplemented))
    return ojson(*err);
  if (!session.hasData()) return ojson(eval::noDataLoaded());
  if (parsed.varlist.empty()) return rcError("varlist required", 100);
  std::vector<std::string> vars;
  if (auto e = resolveVars(session, parsed.varlist, &vars)) return *e;
  {
    Datastore* ds = session.datastore();
    if (ds == nullptr) return ojson(eval::noDataLoaded());
    if (ds->nRows() == 0) return displayBlock("No observation in the dataset\n");
  }
  std::string with = "any";
  if (auto w = optStr(parsed.opts, "with")) {
    if (!w->empty()) with = *w;
  }
  FillMode mode = FillMode::Any;
  if (with == "linear")
    return ojson(eval::unsupportedOption(
        "with(linear)",
        "fillmissing: with(linear) recasts the variable to float and averages the neighbouring "
        "values through the package's own Mata routine; this version of PIET does not serve it. "
        "ipolate does linear interpolation."));
  if (!fillModeOf(with, &mode))  // the package prints its usage and exits at rc 0
    return displayBlock(
        "Option with() incorrectly specified\nwith accepts only these : previous, next, first, "
        "last, any\nmean, min, max, median, rand, pattern\n");
  std::vector<std::string> byVars;
  if (auto b = optStr(parsed.opts, "by")) {
    if (auto e = resolveVars(session, splitWs(*b), &byVars)) return *e;
  }

  // The package decides "string" once and never resets it, so every variable after a string
  // one is handled as a string (probe p2 strnum). With a statistic that only adds its notice;
  // with a copy fill a numeric variable meets `== ""` and StataBE stops at r(109) part way
  // through, leaving the data re-sorted. That is refused before anything is written.
  std::vector<char> asString(vars.size(), 0);
  {
    Datastore* ds = session.datastore();
    bool seenString = false;
    for (size_t j = 0; j < vars.size(); ++j) {
      const bool isStr = ds->col(vars[j]).dtype == cecon::DType::Factor;
      if (!isStr && seenString && mode != FillMode::Inert && !isStatMode(mode))
        return rcError("type mismatch -- fillmissing handles every variable after the string "
                       "variable in the varlist as a string; run it once per variable",
                       109);
      seenString = seenString || isStr;
      asString[j] = seenString ? 1 : 0;
    }
  }

  ScratchCols scratch(session);
  const std::string touseCol = "__piet_t1g_touse";
  std::vector<char> mask;
  if (auto e = sampleMask(session, parsed, &mask, isStatMode(mode) ? touseCol : "")) return *e;
  if (isStatMode(mode) && hasQualifier(parsed)) scratch.add(touseCol);

  // Groups: the by() variables and the sample marker, as the package forms them.
  std::vector<KeyCol> keys;
  {
    Datastore* ds = session.datastore();
    for (const auto& b : byVars) keys.push_back(readKeyCol(*ds, b));
    KeyCol t;
    t.num.reserve(mask.size());
    for (char m : mask) t.num.push_back(m ? 1.0 : 0.0);
    keys.push_back(std::move(t));
  }
  const size_t n = mask.size();
  // The order fills read the group in its sorted order. When the rows are not already in
  // by()/sample order StataBE sorts them first, and its sort leaves tied rows in an order of
  // its own (probe p1 in: `in 1/2, with(last)` read row 1 as the group's last); that order
  // cannot be reproduced, so those fills are refused instead of answered from another order.
  if (isOrderMode(mode)) {
    for (size_t i = 1; i < n; ++i) {
      if (keysCompare(keys, i - 1, i) > 0)
        return rcError("fillmissing, with(" + with + "): the data are not in by()/sample order, "
                       "and Stata would read each group in the order its own sort leaves tied "
                       "rows in, which this version of PIET cannot reproduce. Sort by the by() "
                       "variables first (`sort g t`) and fill without if/in, or restrict with "
                       "`keep if` first.",
                       199);
    }
  }
  int64_t nGroups = 0;
  const std::vector<int64_t> gid = groupIds(keys, n, &nGroups);

  std::string text;
  std::vector<std::string> rewritten;
  for (size_t j = 0; j < vars.size(); ++j) {
    const std::string& v = vars[j];
    if (asString[j] && isStatMode(mode)) {
      text += "Variable " + v + " is a string variable, option with(" + with +
              ") cannot be used with string variables.\n";
      continue;
    }
    if (mode == FillMode::Inert) continue;  // rand/pattern: accepted by the package, no branch
    if (isStatMode(mode)) {
      const char* fn = mode == FillMode::Mean ? "mean"
                       : mode == FillMode::Min ? "min"
                       : mode == FillMode::Max ? "max"
                                               : "median";
      const std::string stat = "__piet_t1g_stat";
      dropColumns(session, {stat});
      scratch.add(stat);
      std::string line = "quietly egen " + stat + " = " + fn + "(" + v + ")";
      if (hasQualifier(parsed)) line += " if " + touseCol + " == 1";
      if (!byVars.empty()) line += ", by(" + joinSpace(byVars) + ")";
      if (auto e = firstError(dispatch(line, session))) return *e;
      int64_t fills = 0, statMissing = 0;
      {
        Datastore* ds = session.datastore();
        const std::vector<double> cur = columnDoubles(*ds, v);
        const std::vector<double> st = columnDoubles(*ds, stat);
        for (size_t i = 0; i < cur.size(); ++i) {
          if (std::isnan(st[i])) ++statMissing;
          if (mask[i] && std::isnan(cur[i]) && !std::isnan(st[i])) ++fills;
        }
      }
      // The package's egen and replace are not quiet (community log fill_mean_by:
      // "(2 missing values generated)" then "(4 real changes made)").
      if (statMissing > 0) text += missingNote(statMissing);
      if (fills > 0) {
        const std::string before = storage::resolveReplaceType(session, v);
        std::string rl = "quietly replace " + v + " = " + stat + " if missing(" + v +
                         ") & !missing(" + stat + ")";
        if (auto e = firstError(dispatch(rl, session))) return *e;
        const std::string after = storage::resolveReplaceType(session, v);
        if (after != before)  // probe p3 meanb: "variable b was byte now float"
          text += "variable " + v + " was " + before + " now " + after + "\n";
        rewritten.push_back(v);
      }
      text += changesNote(fills);
      dropColumns(session, {stat});
      continue;
    }
    Datastore* ds = session.datastore();
    const bool isStr = ds->col(v).dtype == cecon::DType::Factor;
    const std::vector<std::string> cells = columnCells(*ds, v);
    std::vector<double> num;
    if (!isStr) num = columnDoubles(*ds, v);
    int64_t firstPass = 0;
    const std::vector<std::string> filled =
        copyFill(cells, isStr ? nullptr : &num, mode, gid, nGroups, mask, &firstPass);
    int64_t changed = 0;
    for (size_t i = 0; i < cells.size(); ++i) changed += filled[i] != cells[i] ? 1 : 0;
    // The package's one noisy `replace` per variable; with(next) runs two (probe p1 next:
    // "(3 real changes made)" then "(0 real changes made)").
    if (mode == FillMode::Next) {
      text += changesNote(firstPass);
      text += changesNote(changed - firstPass);
    } else {
      text += changesNote(changed);
    }
    if (changed == 0) continue;
    if (auto e = setColumn(session, v, filled, isStr, false, "")) return *e;
    rewritten.push_back(v);
  }
  scratch.clear();
  ojson out = ojson::array();
  if (!text.empty()) out.push_back(displayBlock(text));
  if (!rewritten.empty()) out.push_back(mutationResult(session, "gen", {}, {}, rewritten));
  return out;
}

namespace {

// The text after `=` as typed: up to the comma, a `[` or the `if`/`in` keyword, with every
// blank before it kept, as Stata's `=/exp` hands it on (review gb B6 `3 quantiles of xf `;
// probe p7: `x , nq(3)` -> "x ", `x  if g==1` -> "x  "). `fallback` when the line has no `=`.
std::string typedExpression(const std::string& line, const std::string& fallback) {
  const size_t e = line.find('=');
  if (e == std::string::npos) return fallback;
  size_t b = e + 1;
  while (b < line.size() && std::isspace(static_cast<unsigned char>(line[b]))) ++b;
  size_t stop = line.size();
  int depth = 0;
  bool inStr = false;
  for (size_t i = b; i < line.size(); ++i) {
    const char ch = line[i];
    if (ch == '"') inStr = !inStr;
    if (inStr) continue;
    if (ch == '(') ++depth;
    if (ch == ')') --depth;
    if (depth > 0) continue;
    if (ch == ',' || ch == '[') {
      stop = i;
      break;
    }
    if (i > b && std::isspace(static_cast<unsigned char>(line[i - 1])) &&
        (line.compare(i, 2, "if") == 0 || line.compare(i, 2, "in") == 0) &&
        (i + 2 == line.size() || std::isspace(static_cast<unsigned char>(line[i + 2])))) {
      stop = i;
      break;
    }
  }
  std::string out = line.substr(b, stop - b);
  if (stop == line.size())  // the end of the line is not a separator
    while (!out.empty() && std::isspace(static_cast<unsigned char>(out.back()))) out.pop_back();
  return out.empty() ? fallback : out;
}

}  // namespace

// `astile newvar = varname [if] [in] [, nquantiles(#) by(varlist) qc(exp)]`; `bysort g:`
// arrives as by(). Implements astile per its help file (Shah): xtile's quantile groups,
// computed within each by() group of the if/in sample (community log astile_vs_xtile10: 0
// disagreements with xtile; astile_nq2_by / _nq4_by / _byprefix / _if). Unlike xtile,
// nquantiles() may exceed the group's size (probe p1 asmall/anq20) and must be a whole number
// in 2..4097 (anq1, review gk K10: r(3598)). The variable is c(type), double with by(), and is
// labelled "# quantiles of <exp as typed>". An existing name and an expression in place of a
// variable are r(3598), as the package's Mata routine answers them (astile_exists,
// astile_expr); a string variable is r(109) (probe p1 astr).
ojson runAstile(const eval::ParsedCommand& parsed, SessionState& session) {
  if (auto err = eval::rejectUnknownOptions(parsed.opts, kVocabAstile.known,
                                            kVocabAstile.unimplemented))
    return ojson(*err);
  if (!session.hasData()) return ojson(eval::noDataLoaded());
  const std::string arg = strutil::trim(parsed.argText);
  const size_t eq = arg.find('=');
  if (eq == std::string::npos)
    return syntaxError("astile: syntax is `astile newvar = varname, nquantiles(#)'");
  const std::string newvar = strutil::trim(arg.substr(0, eq));
  const std::string expText = strutil::trim(arg.substr(eq + 1));
  if (newvar.empty() || expText.empty()) return syntaxError("invalid syntax");
  const std::string expTyped = typedExpression(parsed.raw, expText);
  if (splitWs(newvar).size() != 1 || !strutil::isValidStataIdentifier(newvar))
    return syntaxError(newvar + " invalid name");

  std::string nqText = "2";
  if (auto q = optStr(parsed.opts, "nquantiles")) {
    if (!q->empty()) nqText = *q;
  }
  if (columnExists(session, newvar))
    return rcError("variable " + newvar + " already defined", 3598);
  // The package hands the text after `=` to Mata as a variable name.
  std::string srcvar;
  {
    const std::vector<std::string> words = splitWs(expText);
    const bool plainName = words.size() == 1 && strutil::isValidStataIdentifier(words.front());
    std::vector<std::string> one;
    if (!plainName || resolveVars(session, {words.front()}, &one).has_value() || one.size() != 1)
      return rcError("variable " + words.front() + " not found", 3598);
    srcvar = one.front();
  }
  double nqD = 0.0;
  if (!parseStrictDouble(nqText, &nqD) || nqD != std::floor(nqD))  // review gk K10
    return rcError("option nq() incorrectly specified", 3598);
  if (nqD < 2 || nqD > 4097) return rcError("nquantiles must be between 2 and 4097", 3598);
  const int nq = static_cast<int>(nqD);
  std::vector<std::string> byVars;
  if (auto b = optStr(parsed.opts, "by")) {
    if (auto e = resolveVars(session, splitWs(*b), &byVars)) return *e;
  }
  {
    Datastore* ds = session.datastore();
    if (ds == nullptr) return ojson(eval::noDataLoaded());
    if (ds->col(srcvar).dtype == cecon::DType::Factor) return rcError("type mismatch", 109);
  }

  // The values the cut points come from: the variable itself, or a copy that is missing
  // outside if/in.
  ScratchCols scratch(session);
  std::string xcol = srcvar;
  if (hasQualifier(parsed)) {
    xcol = "__piet_t1g_ax";
    dropColumns(session, {xcol});
    scratch.add(xcol);
    if (auto e = firstError(dispatch("quietly generate double " + xcol + " = " + srcvar +
                                         qualifierText(parsed),
                                     session)))
      return *e;
  }
  // qc(): the breakpoints come from the sample's rows that meet the criterion, and every row
  // of the sample is then placed against them (astile's help, option 3; probe p5 aqc*).
  std::string cutSource = xcol;
  if (auto qc = optStr(parsed.opts, "qc")) {
    if (qc->empty()) return syntaxError("qc() requires an expression");
    cutSource = "__piet_t1g_aq";
    dropColumns(session, {cutSource});
    scratch.add(cutSource);
    if (auto e = firstError(dispatch("quietly generate double " + cutSource + " = " + xcol +
                                         " if (" + *qc + ")",
                                     session)))
      return *e;
  }
  // One engine call: the nq-1 cut points per group (egen pctile at p = 100*k/nq, the p the
  // engine's xtile uses), then 1 + #(x > cut), then the cut columns go.
  std::vector<std::string> args = {"eval", "--data", *session.currentHbin()};
  std::string assign = newvar + "=cond(missing(" + xcol + "),.,1";
  std::vector<std::string> cuts;
  for (int k = 1; k < nq; ++k) {
    const std::string c = "__piet_t1g_c" + std::to_string(k);
    cuts.push_back(c);
    const double p = 100.0 * k / nq;
    std::string spec = c + "=pctile(" + cutSource + "), p(" + eval::pythonFloatRepr(p) + ")";
    if (!byVars.empty()) spec += " by(" + joinSpace(byVars) + ")";
    args.push_back("--egen");
    args.push_back(spec);
    assign += "+(" + xcol + ">" + c + ")";
  }
  assign += ")";
  args.push_back("--gen");
  args.push_back(assign);
  for (const auto& c : cuts) {
    args.push_back("--drop");
    args.push_back(c);
  }
  const std::string out = mintScratchPath(session.workdir(), "t1gen_astile");
  args.push_back("--out");
  args.push_back(out);
  {
    SessionState::ResidentEligible resident(session);
    if (auto e = runVerb(args)) {
      cecon::native_unlink(out);
      return *e;
    }
    session.load(out);
  }
  scratch.clear();
  // The type: double with by() (or the by-prefix), else c(type) like any new variable --
  // float by default, double under `set type double` (review gc C1/C2).
  const std::string type =
      byVars.empty() ? storage::resolveCreateType(session, "") : std::string("double");
  storage::recordCreateType(session, newvar, byVars.empty() ? "" : "double", type);
  session.setVarLabel(newvar, nqText + " quantiles of " + expTyped);
  return mutationResult(session, "gen", {newvar});
}

}  // namespace piet::handlers
