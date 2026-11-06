// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// Portions derived from coefplot 1.8.8 (MIT).
// See THIRD_PARTY_NOTICES.
//
// handlers_coefplot.cpp -- coefplot 1.8.8 (Ben Jann), the data pipeline
// (coefplot-family AN-1..AN-4). Derived from
// ~/Library/Application Support/Stata/ado/plus/c/coefplot.ado for everything a
// batch run observes: the subgraph/plot/model grammar (parse_subgraphs,
// parse_plots, parse_models, parse_model, the option merges),
// collect_coefs/coefplot_keepdrop, the arrangement
// (coefplot_arrange/niceorder/order, coeflbls, catvals, ticks_and_labels), the
// offsets, generate() and the r() results, with the
// refusals the oracle pins. The drawn twoway graph itself is not built: the
// graph block stays the visual summary the old handler made.
// Oracle: tests/eval/golden/aer_w2/w2an_coefplot_family.log.
#include <algorithm>
#include <cctype>
#include <cmath>
#include <fstream>
#include <functional>
#include <tuple>
#include <limits>
#include <map>
#include <optional>
#include <regex>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "core/distributions.h"
#include "piet/cobra_runner.h"
#include "piet/dispatcher.h"
#include "piet/eval/errors.h"
#include "piet/eval/session.h"
#include "piet/eval/tokenizer.h"
#include "piet/handlers.h"
#include "piet/mutation_result.h"
#include "piet/scratch_seq.h"
#include "piet/serset.h"
#include "piet/strutil.h"
#include "io/win_compat.h"

namespace piet::eval {
double stataTtail(double df, double t);
double stataInvttail(double df, double p);
}  // namespace piet::eval

namespace piet::handlers {

using ojson = nlohmann::ordered_json;

namespace {

constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();

ojson refuseCp(const std::string& message, int rc) {
  return ojson(eval::errorBlock(rc == 198 ? "syntax_error" : "engine_error", message, std::nullopt,
                                std::nullopt, std::nullopt, "r(" + std::to_string(rc) + ")"));
}

ojson noteCp(const std::string& text) {
  ojson b;
  b["type"] = "display";
  b["text"] = text;
  return b;
}

// A refusal carried out of the pipeline.
struct CpError {
  ojson block;
};

std::string lowerC(std::string s) {
  for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return s;
}
std::string trimC(const std::string& s) { return strutil::trim(s); }

// Stata's %18.0g-like text of a position (strofreal: %9.0g).
std::string strofreal(double v) {
  if (std::isnan(v)) return ".";
  if (v == std::floor(v) && std::fabs(v) < 1e9) {
    char b[32];
    std::snprintf(b, sizeof b, "%.0f", v);
    return b;
  }
  char b[64];
  std::snprintf(b, sizeof b, "%.7g", v);
  std::string s = b;
  // %9.0g drops the leading zero of a fraction
  if (s.compare(0, 2, "0.") == 0) s.erase(0, 1);
  else if (s.compare(0, 3, "-0.") == 0) s.erase(1, 1);
  return s;
}

// One level of outer quotes ("..." or `"..."') removed.
std::string unquote(const std::string& s0) {
  const std::string s = trimC(s0);
  if (s.size() >= 4 && s.compare(0, 2, "`\"") == 0 && s.compare(s.size() - 2, 2, "\"'") == 0)
    return s.substr(2, s.size() - 4);
  if (s.size() >= 2 && s.front() == '"' && s.back() == '"') return s.substr(1, s.size() - 2);
  return s;
}

// Stata's strmatch(): * and ? wildcards.
bool globMatch(const std::string& s, const std::string& p) { return strutil::globMatch(p, s); }

// ---------------------------------------------------------------------------
// text scanning (paren / quote aware)

// Index just past the closing quote of a quote starting at i ("..." or
// `"..."', nested compound quotes counted).
size_t skipQuote(const std::string& s, size_t i) {
  if (s[i] == '"') {
    const size_t c = s.find('"', i + 1);
    return c == std::string::npos ? s.size() : c + 1;
  }
  // compound
  int depth = 0;
  size_t j = i;
  while (j < s.size()) {
    if (s.compare(j, 2, "`\"") == 0) {
      ++depth;
      j += 2;
      continue;
    }
    if (s.compare(j, 2, "\"'") == 0) {
      --depth;
      j += 2;
      if (depth == 0) return j;
      continue;
    }
    ++j;
  }
  return s.size();
}

bool quoteStart(const std::string& s, size_t i) {
  return s[i] == '"' || s.compare(i, 2, "`\"") == 0;
}

// Splits on a top-level separator string (outside parens/brackets/quotes).
std::vector<std::string> splitTop(const std::string& s, const std::string& sep) {
  std::vector<std::string> out;
  int depth = 0;
  size_t start = 0, i = 0;
  while (i < s.size()) {
    if (quoteStart(s, i)) {
      i = skipQuote(s, i);
      continue;
    }
    const char c = s[i];
    if (c == '(' || c == '[') ++depth;
    else if ((c == ')' || c == ']') && depth > 0) --depth;
    if (depth == 0 && s.compare(i, sep.size(), sep) == 0) {
      out.push_back(s.substr(start, i - start));
      i += sep.size();
      start = i;
      continue;
    }
    ++i;
  }
  out.push_back(s.substr(start));
  return out;
}

// `_parse comma lhs rhs`: the text before the first top-level comma, and the
// rest without the comma.
std::pair<std::string, std::string> parseComma(const std::string& s) {
  int depth = 0;
  size_t i = 0;
  while (i < s.size()) {
    if (quoteStart(s, i)) {
      i = skipQuote(s, i);
      continue;
    }
    const char c = s[i];
    if (c == '(' || c == '[') ++depth;
    else if ((c == ')' || c == ']') && depth > 0) --depth;
    else if (c == ',' && depth == 0) return {s.substr(0, i), s.substr(i + 1)};
    ++i;
  }
  return {s, std::string()};
}

// Tokens: a parenthesised group is one token (with `paren` set, the parens
// removed); otherwise blank-separated words (a word may hold parens).
struct Tok {
  std::string text;
  bool paren = false;
};
std::vector<Tok> tokensParen(const std::string& s) {
  std::vector<Tok> out;
  size_t i = 0;
  while (i < s.size()) {
    while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i]))) ++i;
    if (i >= s.size()) break;
    if (s[i] == '(') {
      int depth = 0;
      size_t j = i;
      while (j < s.size()) {
        if (quoteStart(s, j)) {
          j = skipQuote(s, j);
          continue;
        }
        if (s[j] == '(') ++depth;
        else if (s[j] == ')' && --depth == 0) break;
        ++j;
      }
      out.push_back({s.substr(i + 1, (j < s.size() ? j : s.size()) - i - 1), true});
      i = j < s.size() ? j + 1 : s.size();
      continue;
    }
    size_t j = i;
    int depth = 0;
    while (j < s.size()) {
      if (quoteStart(s, j)) {
        j = skipQuote(s, j);
        continue;
      }
      if (s[j] == '(' || s[j] == '[') ++depth;
      else if ((s[j] == ')' || s[j] == ']') && depth > 0) --depth;
      else if (depth == 0 && std::isspace(static_cast<unsigned char>(s[j]))) break;
      ++j;
    }
    out.push_back({s.substr(i, j - i), false});
    i = j;
  }
  return out;
}

// ---------------------------------------------------------------------------
// options

struct Opt {
  std::string name;  // as typed, lower case
  bool hasArg = false;
  std::string arg;   // the text inside the parentheses
};

// `name` or `name(arg)`, a blank allowed before the parenthesis (cf_keep_sp
// `keep (x1 x2 )`).
std::vector<Opt> parseOptList(const std::string& s) {
  std::vector<Opt> out;
  size_t i = 0;
  while (i < s.size()) {
    while (i < s.size() && (std::isspace(static_cast<unsigned char>(s[i])) || s[i] == ',')) ++i;
    if (i >= s.size()) break;
    size_t j = i;
    while (j < s.size() && (std::isalnum(static_cast<unsigned char>(s[j])) || s[j] == '_')) ++j;
    if (j == i) {  // stray character
      if (quoteStart(s, i)) i = skipQuote(s, i);
      else ++i;
      continue;
    }
    Opt o;
    o.name = lowerC(s.substr(i, j - i));
    size_t k = j;
    while (k < s.size() && std::isspace(static_cast<unsigned char>(s[k]))) ++k;
    if (k < s.size() && s[k] == '(') {
      int depth = 0;
      size_t m = k;
      while (m < s.size()) {
        if (quoteStart(s, m)) {
          m = skipQuote(s, m);
          continue;
        }
        if (s[m] == '(') ++depth;
        else if (s[m] == ')' && --depth == 0) break;
        ++m;
      }
      o.hasArg = true;
      o.arg = s.substr(k + 1, (m < s.size() ? m : s.size()) - k - 1);
      i = m < s.size() ? m + 1 : s.size();
    } else {
      i = j;
    }
    out.push_back(o);
  }
  return out;
}

enum class OptKind { kGlobal, kModel, kPlot, kSubgr };
struct OptDef {
  const char* full;
  size_t minLen;
  OptKind kind;
  int arg;  // 0 either, 1 bare only, 2 argument only
};

const std::vector<OptDef>& optDefs() {
  static const std::vector<OptDef> d = {
      // global (parse_globalopts :725-765)
      {"horizontal", 3, OptKind::kGlobal, 1},   {"vertical", 4, OptKind::kGlobal, 1},
      {"sort", 4, OptKind::kGlobal, 0},         {"orderby", 7, OptKind::kGlobal, 2},
      {"order", 5, OptKind::kGlobal, 2},        {"bycoefs", 3, OptKind::kGlobal, 1},
      {"norecycle", 5, OptKind::kGlobal, 1},    {"grid", 4, OptKind::kGlobal, 2},
      {"nooffsets", 5, OptKind::kGlobal, 1},    {"format", 6, OptKind::kGlobal, 2},
      {"nolabels", 5, OptKind::kGlobal, 1},     {"coeflabels", 5, OptKind::kGlobal, 2},
      {"noeqlabels", 7, OptKind::kGlobal, 1},   {"eqlabels", 3, OptKind::kGlobal, 2},
      {"eqstrict", 8, OptKind::kGlobal, 1},     {"headings", 4, OptKind::kGlobal, 2},
      {"groups", 5, OptKind::kGlobal, 2},       {"plotlabels", 5, OptKind::kGlobal, 2},
      {"bylabels", 8, OptKind::kGlobal, 2},     {"generate", 3, OptKind::kGlobal, 0},
      {"relocate", 5, OptKind::kGlobal, 2},     {"replace", 7, OptKind::kGlobal, 1},
      {"addplot", 7, OptKind::kGlobal, 2},      {"nodrop", 6, OptKind::kGlobal, 1},
      {"legend", 3, OptKind::kGlobal, 2},       {"byopts", 4, OptKind::kGlobal, 2},
      {"pcycle", 4, OptKind::kGlobal, 2},
      // model (_merge_modelopts :1507-1531)
      {"omitted", 4, OptKind::kModel, 1},       {"baselevels", 4, OptKind::kModel, 1},
      {"bname", 1, OptKind::kModel, 2},         {"atname", 2, OptKind::kModel, 0},
      {"swapnames", 4, OptKind::kModel, 1},     {"keep", 4, OptKind::kModel, 2},
      {"drop", 4, OptKind::kModel, 2},          {"rename", 6, OptKind::kModel, 2},
      {"eqrename", 5, OptKind::kModel, 2},      {"asequation", 4, OptKind::kModel, 0},
      {"eform", 5, OptKind::kModel, 0},         {"rescale", 7, OptKind::kModel, 2},
      {"transform", 5, OptKind::kModel, 2},     {"noci", 4, OptKind::kModel, 1},
      {"levels", 1, OptKind::kModel, 2},        {"ciname", 2, OptKind::kModel, 2},
      {"vname", 1, OptKind::kModel, 2},         {"sename", 2, OptKind::kModel, 2},
      {"dfname", 2, OptKind::kModel, 2},        {"citype", 6, OptKind::kModel, 2},
      {"aux", 3, OptKind::kModel, 2},
      // plot (_merge_plotopts :1269-1286)
      {"label", 3, OptKind::kPlot, 2},          {"offset", 6, OptKind::kPlot, 2},
      {"pstyle", 4, OptKind::kPlot, 2},         {"axis", 2, OptKind::kPlot, 2},
      {"recast", 6, OptKind::kPlot, 2},         {"mlabels", 2, OptKind::kPlot, 1},
      {"mlabel", 2, OptKind::kPlot, 2},         {"mlabvposition", 6, OptKind::kPlot, 2},
      {"cionly", 6, OptKind::kPlot, 1},         {"citop", 5, OptKind::kPlot, 1},
      {"cismooths", 3, OptKind::kPlot, 1},      {"cismooth", 3, OptKind::kPlot, 2},
      {"ciopts", 4, OptKind::kPlot, 2},         {"cirecast", 4, OptKind::kPlot, 2},
      {"ifopt", 2, OptKind::kPlot, 2},          {"weightopt", 1, OptKind::kPlot, 2},
      {"nokey", 5, OptKind::kPlot, 1},          {"key", 3, OptKind::kPlot, 0},
      // subgraph
      {"bylabel", 5, OptKind::kSubgr, 2},
  };
  return d;
}

// The canonical option an option word names, or nullptr (a twoway option).
const OptDef* resolveOpt(const Opt& o) {
  for (const OptDef& d : optDefs()) {
    if (o.name == d.full && (d.arg == 0 || (d.arg == 1) != o.hasArg)) return &d;
  }
  for (const OptDef& d : optDefs()) {
    if (d.arg == 1 && o.hasArg) continue;
    if (d.arg == 2 && !o.hasArg) continue;
    const std::string f(d.full);
    if (o.name.size() >= d.minLen && o.name.size() <= f.size() && f.compare(0, o.name.size(), o.name) == 0)
      return &d;
  }
  return nullptr;
}

// One level's options by canonical name (the last spelling wins).
struct OptMap {
  std::map<std::string, Opt> m;
  std::vector<Opt> twoway;
  bool has(const std::string& k) const { return m.count(k) != 0; }
  const Opt* get(const std::string& k) const {
    auto it = m.find(k);
    return it == m.end() ? nullptr : &it->second;
  }
};

OptMap optMapOf(const std::string& text) {
  OptMap om;
  for (const Opt& o : parseOptList(text)) {
    const OptDef* d = resolveOpt(o);
    if (d == nullptr) {
      om.twoway.push_back(o);
      continue;
    }
    om.m[d->full] = o;
  }
  return om;
}

// Innermost-first lookup across levels, with coefplot's exclusive pairs.
struct OptChain {
  std::vector<const OptMap*> levels;  // innermost first
  const Opt* get(const std::string& k) const {
    for (const OptMap* l : levels) {
      if (l == nullptr) continue;
      // exclusive pairs: an inner level that names one of a pair hides the outer other
      auto pairOf = [](const std::string& x) -> std::vector<std::string> {
        if (x == "mlabel" || x == "mlabels") return {"mlabel", "mlabels"};
        if (x == "nokey" || x == "key") return {"nokey", "key"};
        if (x == "vname" || x == "sename") return {"vname", "sename"};
        if (x == "cismooth" || x == "cismooths") return {"cismooth", "cismooths"};
        return {x};
      };
      bool any = false;
      for (const auto& p : pairOf(k))
        if (l->has(p)) any = true;
      if (any) return l->get(k);
    }
    return nullptr;
  }
  bool has(const std::string& k) const { return get(k) != nullptr; }
  std::string arg(const std::string& k) const {
    const Opt* o = get(k);
    return o == nullptr ? std::string() : o->arg;
  }
};

// ---------------------------------------------------------------------------
// the plot/model grammar

struct ModelSpec {
  std::string name;    // estimates name, "." or the matrix() model name
  std::string matrix;  // matrix(...) argument, "" = e() mode
  OptMap opts;         // the model's own options (not the last model's)
};
struct PlotSpec2 {
  std::vector<ModelSpec> models;
  OptMap opts;  // plot-level options (the last model's comma part)
  bool skip = false;
};
struct SubgrSpec {
  std::vector<PlotSpec2> plots;
  OptMap opts;
};

// `estimates dir pattern`: the stored names matching, in store order; 111
// when none.
std::optional<CpError> expandEstNames(SessionState& session, const std::string& pat,
                                      std::vector<std::string>* out) {
  bool any = false;
  for (const auto& kv : session.estimates) {
    if (strutil::globMatch(pat, kv.first)) {
      out->push_back(kv.first);
      any = true;
    }
  }
  if (!any) return CpError{refuseCp("estimation result " + pat + " not found", 111)};
  return std::nullopt;
}

// parse_models (:1404-1451) on the text inside one plot's parentheses (or a
// bare model word).
std::optional<CpError> parseModels(SessionState& session, const std::string& text, PlotSpec2* plot) {
  const std::vector<std::string> parts = splitTop(text, "\\");
  for (size_t p = 0; p < parts.size(); ++p) {
    auto [model, opts] = parseComma(parts[p]);
    model = trimC(model);
    const bool last = p + 1 == parts.size();
    if (last) plot->opts = optMapOf(opts);
    if (model.empty() && (last ? true : trimC(opts).empty())) {
      if (!last || plot->models.empty()) {
        ModelSpec ms;
        ms.name = ".";
        if (!last) ms.opts = optMapOf(opts);
        plot->models.push_back(ms);
      }
      continue;
    }
    if (model.find('(') != std::string::npos) {
      // matrix(name[...])
      ModelSpec ms;
      const size_t open = model.find('(');
      const std::string head = lowerC(trimC(model.substr(0, open)));
      const size_t close = model.rfind(')');
      const std::string inner =
          close == std::string::npos ? model.substr(open + 1) : model.substr(open + 1, close - open - 1);
      if (head.empty() || std::string("matrix").compare(0, head.size(), head) != 0) {
        return CpError{refuseCp(model + " not allowed", 198)};
      }
      ms.matrix = trimC(inner);
      size_t e = 0;
      while (e < ms.matrix.size() && ms.matrix[e] != '[' && !std::isspace(static_cast<unsigned char>(ms.matrix[e])))
        ++e;
      ms.name = ms.matrix.substr(0, e);
      if (!last) ms.opts = optMapOf(opts);
      plot->models.push_back(ms);
      continue;
    }
    std::istringstream is(model.empty() ? std::string(".") : model);
    std::string w;
    while (is >> w) {
      std::vector<std::string> names;
      if (w.find('*') != std::string::npos || w.find('?') != std::string::npos) {
        if (auto e = expandEstNames(session, w, &names)) return e;
      } else {
        names.push_back(w);
      }
      for (const auto& n : names) {
        ModelSpec ms;
        ms.name = n;
        if (!last) ms.opts = optMapOf(opts);
        plot->models.push_back(ms);
      }
    }
  }
  if (plot->models.empty()) {
    ModelSpec ms;
    ms.name = ".";
    plot->models.push_back(ms);
  }
  if (plot->models.size() == 1 && plot->models[0].name == "_skip" && plot->models[0].matrix.empty())
    plot->skip = true;
  return std::nullopt;
}

// parse_plots (:1172-1205).
std::optional<CpError> parsePlots(SessionState& session, const std::string& text, SubgrSpec* sg) {
  auto [plotsText, opts] = parseComma(text);
  sg->opts = optMapOf(opts);
  for (const Tok& t : tokensParen(plotsText)) {
    if (t.paren) {
      PlotSpec2 p;
      if (auto e = parseModels(session, t.text, &p)) return e;
      sg->plots.push_back(std::move(p));
      continue;
    }
    std::vector<std::string> names;
    if (t.text.find('(') == std::string::npos &&
        (t.text.find('*') != std::string::npos || t.text.find('?') != std::string::npos)) {
      if (auto e = expandEstNames(session, t.text, &names)) return e;
    } else {
      names.push_back(t.text);
    }
    for (const auto& n : names) {
      PlotSpec2 p;
      if (auto e = parseModels(session, n, &p)) return e;
      sg->plots.push_back(std::move(p));
    }
  }
  if (sg->plots.empty()) {
    PlotSpec2 p;
    ModelSpec ms;
    ms.name = ".";
    p.models.push_back(ms);
    sg->plots.push_back(p);
  }
  return std::nullopt;
}

// parse_subgraphs (:693-723): subgraphs split at `||`; the global options are
// the comma part of the LAST piece.
void parseSubgraphs(const std::string& args, std::vector<std::string>* subgr, std::string* globalOpts) {
  std::vector<std::string> pieces = splitTop(args, "||");
  bool empty = true;
  for (size_t i = 0; i < pieces.size(); ++i) {
    std::string piece = pieces[i];
    const bool last = i + 1 == pieces.size();
    if (i > 0) {
      // a `||` precedes this piece
      if (empty) subgr->push_back(".");
      empty = true;
    }
    if (last) {
      auto [lhs, rhs] = parseComma(piece);
      *globalOpts = rhs;
      piece = lhs;
    }
    if (!trimC(piece).empty()) {
      empty = false;
      subgr->push_back(piece);
    }
  }
  // the `||` handling above adds "." for an empty piece BETWEEN separators; the
  // first piece's emptiness is decided at the first `||` (parse_subgraphs adds
  // "." there too) -- undo the extra one for a leading `||`-less empty start.
  if (subgr->empty()) subgr->push_back(".");
}

// ---------------------------------------------------------------------------
// coefficient sources

struct CoefSrc {
  bool empty = true;
  std::vector<std::string> eq, coef;
  std::vector<double> b, V;
  double dfr = kNaN;
  bool hasDf = false;
};

// Splits "eq:name" (no eq -> "").
void splitEqName(const std::string& full, std::string* eq, std::string* name) {
  const size_t c = full.find(':');
  if (c == std::string::npos) {
    *eq = "";
    *name = full;
  } else {
    *eq = full.substr(0, c);
    *name = full.substr(c + 1);
  }
}

// coefplot-family AN-EB, the part that needs no estimation-side change: the
// engine's e(b) stripe spells a factor fit the short way, Stata's full way is
// what coefplot.ado reads. (1) A continuous variable inside an interaction is
// `c.name` (`0.d#x1` -> `0.d#c.x1`, w2an cf_off_048 `0.`/`1.`, log:1067).
// (2) A main-effect factor `i.var` carries its base level as a `#b.var`
// column with b = 0 and V = 0 (`1b.g` before `2.g`; cf_bls_act g=1 b=0 V=0,
// log:1793), the base being the smallest value of var not among the
// estimated levels. Interactions of two factors and collinear fits are left
// to the estimation side (T-E2).
void stataFactorStripe(SessionState& session, CoefSrc* s) {
  static const std::regex reLevel(R"(^(\d+)([a-z]*)\.([A-Za-z_][A-Za-z0-9_]*)$)");
  for (auto& c : s->coef) {
    if (c.find('#') == std::string::npos) continue;
    std::string outName;
    size_t i = 0;
    while (i <= c.size()) {
      const size_t h = c.find('#', i);
      std::string part = c.substr(i, h == std::string::npos ? std::string::npos : h - i);
      if (part.find('.') == std::string::npos && !part.empty()) part = "c." + part;
      outName += part;
      if (h == std::string::npos) break;
      outName += "#";
      i = h + 1;
    }
    c = outName;
  }
  // main-effect factor columns, by variable
  std::map<std::string, std::vector<size_t>> byVar;
  std::map<std::string, bool> hasBase;
  for (size_t j = 0; j < s->coef.size(); ++j) {
    std::smatch m;
    if (!std::regex_match(s->coef[j], m, reLevel)) continue;
    const std::string var = m[3].str();
    byVar[var].push_back(j);
    if (m[2].str().find('b') != std::string::npos) hasBase[var] = true;
  }
  Datastore* ds = session.datastore();
  if (ds == nullptr) return;
  // insert from the last column backwards so earlier indices stay valid
  std::vector<std::pair<size_t, std::pair<std::string, std::string>>> inserts;
  for (const auto& [var, cols] : byVar) {
    if (hasBase.count(var) || !ds->has(var) || ds->dtype(var) != "f64") continue;
    std::set<double> present;
    for (size_t j : cols) {
      std::smatch m;
      std::regex_match(s->coef[j], m, reLevel);
      present.insert(std::atof(m[1].str().c_str()));
    }
    const double* d = ds->col(var).f64();
    std::optional<double> base;
    for (int64_t r = 0; r < ds->nRows(); ++r) {
      const double v = d[r];
      if (std::isnan(v) || v != std::floor(v) || v < 0 || present.count(v)) continue;
      if (!base.has_value() || v < *base) base = v;
    }
    if (!base.has_value() || *base > *present.begin()) continue;  // the base is the first level
    char lb[32];
    std::snprintf(lb, sizeof lb, "%.0f", *base);
    inserts.push_back({cols.front(), {s->eq[cols.front()], std::string(lb) + "b." + var}});
  }
  std::sort(inserts.begin(), inserts.end(),
            [](const auto& a, const auto& b) { return a.first > b.first; });
  for (const auto& in : inserts) {
    const size_t at = in.first;
    s->eq.insert(s->eq.begin() + static_cast<long>(at), in.second.first);
    s->coef.insert(s->coef.begin() + static_cast<long>(at), in.second.second);
    s->b.insert(s->b.begin() + static_cast<long>(at), 0.0);
    s->V.insert(s->V.begin() + static_cast<long>(at), 0.0);
  }
}

// The live e(): e(b) (or, where this version refuses to build e(b) -- a
// collinear fit -- the engine's coefficient table with an omitted column read
// as o.name), diag(e(V)), e(df_r).
CoefSrc readLiveE(SessionState& session) {
  CoefSrc s;
  eval::EvalSession& ev = session.eval;
  if (!ev.eLive()) return s;
  auto ab = ev.resolveEMatrixFor("b", eval::EvalSession::EReader::kCopy);
  if (ab.matrix.has_value()) {
    const eval::EvalMatrix& m = *ab.matrix;
    if (m.data.empty()) return s;
    const auto& row = m.data[0];
    for (size_t j = 0; j < row.size(); ++j) {
      std::string eq, nm;
      splitEqName(j < m.colnames.size() ? m.colnames[j] : std::string("c") + std::to_string(j + 1), &eq, &nm);
      s.eq.push_back(eq);
      s.coef.push_back(nm);
      s.b.push_back(row[j]);
      s.V.push_back(kNaN);
    }
    auto av = ev.resolveEMatrixFor("V", eval::EvalSession::EReader::kCopy);
    if (av.matrix.has_value() && av.matrix->data.size() == s.b.size()) {
      for (size_t j = 0; j < s.b.size(); ++j) {
        const auto& r = av.matrix->data[j];
        s.V[j] = j < r.size() ? r[j] : kNaN;
      }
    }
  } else if (!ab.absent) {
    // fallback: the engine's coefficient table
    const ojson* src = ev.coefSource();
    if (src == nullptr || !src->is_object()) return s;
    const ojson* ct = nullptr;
    if (src->contains("coeftable") && (*src)["coeftable"].is_array()) ct = &(*src)["coeftable"];
    if (ct == nullptr) return s;
    std::vector<size_t> order;
    std::optional<size_t> cons;
    for (size_t i = 0; i < ct->size(); ++i) {
      const std::string nm = (*ct)[i].value("name", std::string());
      if (nm == "(intercept)" || nm == "_cons") cons = i;
      else order.push_back(i);
    }
    if (cons.has_value()) order.push_back(*cons);
    for (size_t i : order) {
      const ojson& r = (*ct)[i];
      std::string nm = r.value("name", std::string());
      if (nm == "(intercept)") nm = "_cons";
      const double b = r.contains("b") && r["b"].is_number() ? r["b"].get<double>() : kNaN;
      const double se = r.contains("se") && r["se"].is_number() ? r["se"].get<double>() : kNaN;
      if (b == 0 && (std::isnan(se) || se == 0)) nm = "o." + nm;
      std::string eq, name;
      splitEqName(nm, &eq, &name);
      s.eq.push_back(eq);
      s.coef.push_back(name);
      s.b.push_back(b);
      s.V.push_back(std::isnan(se) ? kNaN : se * se);
    }
  } else {
    return s;
  }
  if (const double* d = ev.eScalars.find("df_r")) {
    s.dfr = *d;
    s.hasDf = true;
  } else if (const ojson* blk = ev.coefSource(); blk != nullptr && blk->is_object() &&
             blk->contains("df_r") && (*blk)["df_r"].is_number()) {
    // e(df_r) as `display e(df_r)` reads it (eval::eScalarKey: df_r -> df_r)
    s.dfr = (*blk)["df_r"].get<double>();
    s.hasDf = true;
  }
  s.empty = s.b.empty();
  stataFactorStripe(session, &s);
  return s;
}

// A stored model's e(), read the way `estimates restore` would make it live,
// without disturbing the active results (coefplot holds and restores them,
// :54-55).
struct ScopedStored {
  eval::EvalSession& s;
  std::optional<nlohmann::ordered_json> savedLMR;
  const nlohmann::ordered_json* savedOverride;
  eval::OrderedFloatDict savedScalars;
  eval::OrderedDict savedStrings;
  std::vector<std::pair<std::string, eval::EvalMatrix>> savedMatrices;
  std::function<bool()> savedLive;
  ScopedStored(eval::EvalSession& s_, const SessionState::StoredModel& sm)
      : s(s_), savedLMR(s_.lastModelResult), savedOverride(s_.coefOverride),
        savedScalars(s_.eScalars), savedStrings(s_.eStrings), savedMatrices(s_.eMatrices),
        savedLive(s_.modelLive) {
    s.lastModelResult = sm.estBlock.has_value() ? sm.estBlock : sm.lastModelResult;
    s.coefOverride = s.lastModelResult.has_value() ? &*s.lastModelResult : nullptr;
    s.eScalars = sm.eScalars;
    s.eStrings = sm.eStrings;
    s.eMatrices = sm.eMatrices;
    s.modelLive = []() { return true; };
  }
  ~ScopedStored() {
    s.lastModelResult = savedLMR;
    s.coefOverride = savedOverride;
    s.eScalars = savedScalars;
    s.eStrings = savedStrings;
    s.eMatrices = savedMatrices;
    s.modelLive = savedLive;
  }
  ScopedStored(const ScopedStored&) = delete;
  ScopedStored& operator=(const ScopedStored&) = delete;
};

// A user matrix row/column spec "name", "name[#]", "name[#,]", "name[,#]".
struct MatSpec {
  std::string name;
  long row = 1;   // -1 = all (".")
  long col = -1;  // -1 = all
  bool ok = true;
};
MatSpec parseMatSpec(const std::string& s0) {
  MatSpec ms;
  std::string s = trimC(s0);
  const size_t lb = s.find('[');
  if (lb == std::string::npos) {
    ms.name = s;
    return ms;
  }
  ms.name = trimC(s.substr(0, lb));
  const size_t rb = s.find(']', lb);
  std::string in = s.substr(lb + 1, rb == std::string::npos ? std::string::npos : rb - lb - 1);
  const size_t comma = in.find(',');
  std::string r = trimC(comma == std::string::npos ? in : in.substr(0, comma));
  std::string c = comma == std::string::npos ? std::string(".") : trimC(in.substr(comma + 1));
  if (r.empty()) r = ".";
  if (c.empty()) c = ".";
  if ((r == ".") + (c == ".") != 1) {
    ms.ok = false;
    return ms;
  }
  ms.row = r == "." ? -1 : std::atol(r.c_str());
  ms.col = c == "." ? -1 : std::atol(c.c_str());
  return ms;
}

// The vector a MatSpec selects (a row -> its columns; a column -> its rows),
// and the names along it.
bool matVector(const eval::EvalMatrix& m, const MatSpec& ms, std::vector<double>* v,
               std::vector<std::string>* names) {
  v->clear();
  if (names) names->clear();
  const long nr = static_cast<long>(m.data.size());
  const long nc = nr > 0 ? static_cast<long>(m.data[0].size()) : 0;
  if (ms.row >= 1) {
    if (ms.row > nr) return false;
    for (long j = 0; j < nc; ++j) {
      v->push_back(m.data[static_cast<size_t>(ms.row - 1)][static_cast<size_t>(j)]);
      if (names) names->push_back(static_cast<size_t>(j) < m.colnames.size() ? m.colnames[static_cast<size_t>(j)] : "c" + std::to_string(j + 1));
    }
  } else {
    if (ms.col > nc || ms.col < 1) return false;
    for (long i = 0; i < nr; ++i) {
      v->push_back(m.data[static_cast<size_t>(i)][static_cast<size_t>(ms.col - 1)]);
      if (names) names->push_back(static_cast<size_t>(i) < m.rownames.size() ? m.rownames[static_cast<size_t>(i)] : "r" + std::to_string(i + 1));
    }
  }
  return true;
}

// ---------------------------------------------------------------------------
// the coefficient table (COEFPLOT_STRUCT)

struct Row {
  double b = kNaN, V = kNaN, se = kNaN, t = kNaN, df = kNaN, pval = kNaN, at = 1;
  std::vector<double> ci;
  std::string mlbl;
  double mlpos = kNaN;
  double plot = kNaN, by = kNaN;
  std::string coefnm, eqnm, coeflbl;
  double eq = 1, grp = 0;
};

// coefplot_parse_namelist: (eq, name) pairs.
std::vector<std::pair<std::string, std::string>> parseNamelist(const std::string& s, std::string defeq,
                                                               bool* bad) {
  std::vector<std::string> tok;
  {
    size_t i = 0;
    while (i < s.size()) {
      if (std::isspace(static_cast<unsigned char>(s[i]))) {
        ++i;
        continue;
      }
      if (s[i] == ':') {
        tok.push_back(":");
        ++i;
        continue;
      }
      if (quoteStart(s, i)) {
        const size_t e = skipQuote(s, i);
        tok.push_back(unquote(s.substr(i, e - i)));
        i = e;
        continue;
      }
      size_t j = i;
      while (j < s.size() && !std::isspace(static_cast<unsigned char>(s[j])) && s[j] != ':') ++j;
      tok.push_back(s.substr(i, j - i));
      i = j;
    }
  }
  std::vector<std::pair<std::string, std::string>> res;
  const size_t c = tok.size();
  for (size_t i = 0; i < c; ++i) {
    if (i + 1 < c && tok[i + 1] == ":") {
      if (tok[i] == ":") {
        if (bad) *bad = true;
        return res;
      }
      if (i + 2 < c) {
        if (tok[i + 2] == ":") {
          if (bad) *bad = true;
          return res;
        }
        if (i + 3 < c && tok[i + 3] == ":") {
          res.emplace_back(tok[i], "*");
          i += 1;
          continue;
        }
        res.emplace_back(tok[i], tok[i + 2]);
        defeq = tok[i];
        i += 2;
        continue;
      }
      res.emplace_back(tok[i], "*");
      i += 1;
      continue;
    }
    if (tok[i] == ":") {
      if (bad) *bad = true;
      return res;
    }
    res.emplace_back(defeq, tok[i]);
  }
  return res;
}

// coefplot_parse_matchlist: "names = value" pairs (nc values).
std::vector<std::vector<std::string>> parseMatchlist(const std::string& s, size_t nc, bool* bad) {
  std::vector<std::vector<std::string>> res;
  if (trimC(s).empty()) return res;
  // tokens split at blanks and `=`, binding quotes and parens
  std::vector<std::string> stok;
  size_t i = 0;
  while (i < s.size()) {
    if (std::isspace(static_cast<unsigned char>(s[i]))) {
      ++i;
      continue;
    }
    if (s[i] == '=') {
      stok.push_back("=");
      ++i;
      continue;
    }
    size_t j = i;
    int depth = 0;
    while (j < s.size()) {
      if (quoteStart(s, j)) {
        j = skipQuote(s, j);
        continue;
      }
      if (s[j] == '(') ++depth;
      else if (s[j] == ')' && depth > 0) --depth;
      else if (depth == 0 && (std::isspace(static_cast<unsigned char>(s[j])) || s[j] == '=')) break;
      ++j;
    }
    stok.push_back(s.substr(i, j - i));
    i = j;
  }
  const size_t c = stok.size();
  size_t a = 0;
  for (size_t b = 0; b < c; ++b) {
    if (stok[b] != "=") continue;
    // (b==a | b==c | b+nc>cols) in the ado's 1-based indexing
    if (b == a || b + nc > c - 1) {
      if (bad) *bad = true;
      return res;
    }
    std::vector<std::string> row;
    std::string names;
    for (size_t k = a; k < b; ++k) names += (k > a ? " " : "") + stok[k];
    row.push_back(names);
    for (size_t k = 1; k <= nc; ++k) row.push_back(stok[b + k]);
    a = b + 1 + nc;
    b = a - 1;
    res.push_back(row);
  }
  if (a < c) {
    if (bad) *bad = true;
    return res;
  }
  for (auto& r : res) r.back() = unquote(r.back());
  return res;
}

// compile_xlabel (:1943-1993): the default label of a coefficient name.
std::string compileXlabel(SessionState& session, const std::string& v, const std::string& interact) {
  Datastore* ds = session.datastore();
  session.syncEvalVarLabels();
  auto varExists = [&](const std::string& n) { return ds != nullptr && ds->has(n); };
  auto varLab = [&](const std::string& n) { return session.eval.varLabels.get(n, std::string()); };
  std::string xlab;
  size_t i = 0;
  while (i <= v.size()) {
    const size_t h = v.find('#', i);
    const std::string vi = v.substr(i, h == std::string::npos ? std::string::npos : h - i);
    std::string xlabi;
    if (vi.find('.') == std::string::npos) {
      if (varExists(vi)) xlabi = varLab(vi);
      if (xlabi.empty()) xlabi = vi;
    } else {
      const size_t dot = vi.find('.');
      std::string li = vi.substr(0, dot);
      const std::string vii = vi.substr(dot + 1);
      if (varExists(vii) && !vii.empty()) {
        char* end = nullptr;
        std::strtod(li.c_str(), &end);
        const bool isNum = !li.empty() && end != nullptr && *end == '\0';
        if (!isNum) {
          xlabi = varLab(vii);
          if (xlabi.empty()) xlabi = vii;
          if (!li.empty() && li[0] == 'c') li = li.substr(1);
          if (!li.empty()) xlabi = li + "." + xlabi;
        } else {
          std::string vlab;
          auto it = session.varLblname.find(vii);
          if (it != session.varLblname.end() && !it->second.empty()) {
            auto ld = session.labelDefs.find(it->second);
            if (ld != session.labelDefs.end()) {
              auto lv = ld->second.find(li);
              xlabi = lv != ld->second.end() ? lv->second : li;
            } else {
              xlabi = li;
            }
          } else {
            vlab = varLab(vii);
            if (vlab.empty()) vlab = vii;
            xlabi = vlab + "=" + li;
          }
        }
      }
      if (xlabi.empty()) xlabi = vi;
    }
    xlab += xlabi;
    if (h == std::string::npos) break;
    xlab += interact;
    i = h + 1;
  }
  return xlab;
}

// Stata's `piece #i #len of string`: words packed into pieces of at most len
// characters; a longer word is cut.
std::vector<std::string> pieces(const std::string& s, size_t len) {
  std::vector<std::string> out;
  std::istringstream is(s);
  std::string w, cur;
  while (is >> w) {
    while (w.size() > len) {
      if (!cur.empty()) {
        out.push_back(cur);
        cur.clear();
      }
      out.push_back(w.substr(0, len));
      w = w.substr(len);
    }
    if (cur.empty()) cur = w;
    else if (cur.size() + 1 + w.size() <= len) cur += " " + w;
    else {
      out.push_back(cur);
      cur = w;
    }
  }
  if (!cur.empty()) out.push_back(cur);
  return out;
}

double normalCdf(double z) { return 0.5 * std::erfc(-z / std::sqrt(2.0)); }

// The storage type `compress` gives a numeric variable.
std::string compressType(const std::vector<double>& v) {
  bool integral = true;
  double lo = 0, hi = 0;
  bool any = false;
  for (double x : v) {
    if (std::isnan(x)) continue;
    if (x != std::floor(x)) integral = false;
    if (!any) {
      lo = hi = x;
      any = true;
    } else {
      lo = std::min(lo, x);
      hi = std::max(hi, x);
    }
  }
  if (!any) return "byte";
  if (!integral) return "float";
  if (lo >= -127 && hi <= 100) return "byte";
  if (lo >= -32767 && hi <= 32740) return "int";
  if (lo >= -2147483647.0 && hi <= 2147483620.0) return "long";
  return "float";
}

double toFloatC(double v) { return std::isnan(v) ? v : static_cast<double>(static_cast<float>(v)); }

std::optional<ojson> writeColumn(SessionState& session, const std::string& name, const std::vector<double>* num,
                                 const std::vector<std::string>* str) {
  const std::string hbin = mintScratchPath(session.workdir(), "cpgen");
  forgetScratchMint(hbin);
  const std::string csv = hbin.substr(0, hbin.size() - 5) + ".csv";
  registerScratchMint(csv);
  {
    std::ofstream f(cecon::to_native_path(csv), std::ios::binary);
    if (!f) return refuseCp("could not write the values payload", 693);
    f.precision(17);
    if (num != nullptr) {
      for (double d : *num) {
        if (!std::isnan(d)) f << d;
        f << "\n";
      }
    } else {
      for (const auto& s : *str) {
        std::string q = "\"";
        for (char c : s) {
          if (c == '"') q += "\"\"";
          else q += c;
        }
        q += "\"";
        f << q << "\n";
      }
    }
    if (!f) return refuseCp("could not write the values payload", 693);
  }
  const std::string out = mintScratchPath(session.workdir(), "cpgen");
  const CobraRunResult r = runCobra({"setcol", "--data", *session.currentHbin(), "--var", name, "--values", csv,
                                     "--dtype", num != nullptr ? "f64" : "factor", "--new", "--out", out});
  cecon::native_unlink(csv);
  if (!r.ok || !cecon::native_exists(out)) {
    return ojson(eval::engineError("could not create " + name + " (disk full or I/O error)", "data_io"));
  }
  session.load(out);
  session.implicitFloatVars.erase(name);
  return std::nullopt;
}

std::string compoundQuote(const std::string& s) { return "`\"" + s + "\"'"; }

// The value label of plot i: plotlabels() wins over label()/the model name.
std::string plotLabels(const std::vector<std::string>& plotlabels, const std::vector<std::string>& plotLbl,
                       int i) {
  if (i >= 1 && static_cast<size_t>(i) <= plotlabels.size()) return plotlabels[static_cast<size_t>(i - 1)];
  if (i >= 1 && static_cast<size_t>(i) < plotLbl.size()) return plotLbl[static_cast<size_t>(i)];
  return std::string();
}

}  // namespace

// ===========================================================================

ojson runCoefplotJann(SessionState& session, const eval::ParsedCommand& parsed, ojson* drawRows) {
  ojson out = ojson::array();
  // the text after the command word
  std::string args;
  {
    const std::string& raw = parsed.raw;
    size_t b = raw.find_first_not_of(" \t");
    size_t e = b;
    while (e < raw.size() && (std::isalnum(static_cast<unsigned char>(raw[e])) || raw[e] == '_')) ++e;
    args = e < raw.size() ? raw.substr(e) : std::string();
  }

  std::vector<std::string> subgrTexts;
  std::string globalText;
  parseSubgraphs(args, &subgrTexts, &globalText);
  const OptMap G = optMapOf(globalText);

  // global-only options
  const bool horizontalOpt = G.has("horizontal"), verticalOpt = G.has("vertical");
  if (horizontalOpt && verticalOpt) {
    out.push_back(refuseCp("horizontal and vertical not both allowed", 198));
    return out;
  }
  for (const char* unsup : {"sort", "orderby", "bycoefs", "headings", "groups", "relocate", "addplot", "eqstrict"}) {
    if (G.has(unsup)) {
      out.push_back(ojson(eval::unsupportedOption(
          std::string(unsup) + "()",
          std::string("coefplot ") + unsup + "() rearranges the plotted coefficients in a way this version of "
                                             "PIET does not reproduce")));
      return out;
    }
  }
  const bool recycle = !G.has("norecycle");
  const bool nooffsets = G.has("nooffsets");
  const bool nolabels = G.has("nolabels");
  const bool noeqlabels = G.has("noeqlabels");
  const bool replaceOpt = G.has("replace");
  std::string generate;
  if (const Opt* g = G.get("generate")) {
    generate = g->hasArg ? trimC(g->arg) : std::string("__");
    if (generate.empty()) generate = "__";
  }
  std::string format;
  if (const Opt* f = G.get("format")) format = trimC(f->arg);
  const bool legendGiven = G.has("legend");
  std::string coeflabelsText, wrapLen, truncLen;
  std::string interact = " # ";
  if (const Opt* c = G.get("coeflabels")) {
    auto [lhs, rhs] = parseComma(c->arg);
    coeflabelsText = lhs;
    for (const Opt& so : parseOptList(rhs)) {
      if (so.name.size() >= 1 && std::string("wrap").compare(0, so.name.size(), so.name) == 0) wrapLen = so.arg;
      else if (so.name.size() >= 1 && std::string("truncate").compare(0, so.name.size(), so.name) == 0)
        truncLen = so.arg;
      else if (so.name.size() >= 1 && std::string("interaction").compare(0, so.name.size(), so.name) == 0)
        interact = unquote(so.arg);
    }
  }
  std::vector<std::string> eqlabelsUser;
  if (const Opt* e = G.get("eqlabels")) {
    auto [lhs, rhs] = parseComma(e->arg);
    (void)rhs;
    for (const Tok& t : tokensParen(lhs)) eqlabelsUser.push_back(unquote(t.text));
  }
  std::vector<std::string> plotlabels, bylabels;
  if (const Opt* p = G.get("plotlabels")) {
    auto [lhs, rhs] = parseComma(p->arg);
    (void)rhs;
    for (const Tok& t : tokensParen(lhs)) plotlabels.push_back(unquote(t.text));
  }
  if (const Opt* p = G.get("bylabels")) {
    auto [lhs, rhs] = parseComma(p->arg);
    (void)rhs;
    for (const Tok& t : tokensParen(lhs)) bylabels.push_back(unquote(t.text));
  }
  std::string orderText = G.get("order") ? G.get("order")->arg : std::string();

  // ---- dry run: subgraphs, plots, models ----
  std::vector<SubgrSpec> subgrs;
  for (const auto& t : subgrTexts) {
    SubgrSpec sg;
    if (auto e = parsePlots(session, t, &sg)) {
      out.push_back(e->block);
      return out;
    }
    subgrs.push_back(std::move(sg));
  }
  const int nSubgr = static_cast<int>(subgrs.size());

  // plot numbering (recycle: restart per subgraph)
  std::vector<std::vector<int>> plotIdx(subgrs.size());
  int Nplots = 0;
  {
    int i = 0;
    for (size_t j = 0; j < subgrs.size(); ++j) {
      if (recycle) i = 0;
      for (size_t k = 0; k < subgrs[j].plots.size(); ++k) {
        ++i;
        plotIdx[j].push_back(i);
      }
      Nplots = std::max(Nplots, i);
    }
  }
  std::vector<std::string> plotLbl(static_cast<size_t>(Nplots) + 1), byLbl(static_cast<size_t>(nSubgr) + 1);
  std::vector<bool> plotLblForced(static_cast<size_t>(Nplots) + 1, false);
  std::vector<int> nCiPlot(static_cast<size_t>(Nplots) + 1, -1);
  std::vector<std::string> offsetPlot(static_cast<size_t>(Nplots) + 1);
  std::vector<int> keyPlot(static_cast<size_t>(Nplots) + 1, 0);
  std::vector<bool> citopPlot(static_cast<size_t>(Nplots) + 1, false);
  std::vector<std::string> mlabelPlot(static_cast<size_t>(Nplots) + 1);
  bool customOffset = false;

  // subgraph labels (parse_subgropts: the bylabel, set before the models)
  for (size_t j = 0; j < subgrs.size(); ++j) {
    const OptChain ch{{&subgrs[j].opts, &G}};
    if (const Opt* bl = ch.get("bylabel")) byLbl[j + 1] = unquote(bl->arg);
  }
  // plot options (parse_plotopts)
  for (size_t j = 0; j < subgrs.size(); ++j) {
    for (size_t k = 0; k < subgrs[j].plots.size(); ++k) {
      const int i = plotIdx[j][k];
      const OptChain ch{{&subgrs[j].plots[k].opts, &subgrs[j].opts, &G}};
      if (const Opt* l = ch.get("label")) {
        plotLbl[static_cast<size_t>(i)] = unquote(l->arg);
        plotLblForced[static_cast<size_t>(i)] = true;
      }
      if (const Opt* o = ch.get("offset")) {
        offsetPlot[static_cast<size_t>(i)] = trimC(o->arg);
        customOffset = true;
      }
      if (ch.has("nokey")) keyPlot[static_cast<size_t>(i)] = -1;
      else if (const Opt* kk = ch.get("key")) {
        if (kk->hasArg) {
          std::istringstream is(kk->arg);
          std::string ci, n;
          is >> ci >> n;
          keyPlot[static_cast<size_t>(i)] = n.empty() ? 1 : std::atoi(n.c_str());
        }
      }
      if (ch.has("citop")) citopPlot[static_cast<size_t>(i)] = true;
      if (ch.has("mlabels")) mlabelPlot[static_cast<size_t>(i)] = "@b";
      else if (const Opt* ml = ch.get("mlabel")) mlabelPlot[static_cast<size_t>(i)] = ml->arg;
    }
  }

  // ---- collect ----
  std::vector<Row> C;
  size_t ciCols = 0;  // pairs
  bool atmode = false;
  bool seenModel = false;
  std::vector<ojson> msgs;
  for (size_t j = 0; j < subgrs.size(); ++j) {
    for (size_t k = 0; k < subgrs[j].plots.size(); ++k) {
      const PlotSpec2& plot = subgrs[j].plots[k];
      const int i = plotIdx[j][k];
      if (plot.skip) continue;
      for (const ModelSpec& ms : plot.models) {
        const OptChain ch{{&ms.opts, &plot.opts, &subgrs[j].opts, &G}};
        if (ch.has("transform") || ch.has("aux") || ch.has("citype") ||
            ch.has("bname") || ch.has("vname") || ch.has("sename") || ch.has("dfname") || ch.has("eform") ||
            ch.has("rescale") || ch.has("eqrename")) {
          const char* which = ch.has("transform") ? "transform" : "option";
          out.push_back(ojson(eval::unsupportedOption(
              std::string(which) + "()",
              "coefplot: this option of the model specification is not implemented in this version of "
              "PIET (at(), b(), v(), se(), df(), transform(), aux(), citype(), eform, rescale(), "
              "eqrename())")));
          return out;
        }
        // ---- at() (collect_coefs) ----
        std::string atname;
        if (const Opt* ao = ch.get("atname")) {
          std::string a = ao->hasArg ? trimC(parseComma(ao->arg).first) : std::string();
          atname = a.empty() ? std::string("at") : a;
          if (!parseComma(ao->hasArg ? ao->arg : std::string()).second.empty() ||
              atname.find('(') != std::string::npos) {
            out.push_back(ojson(eval::unsupportedOption(
                "at()", "coefplot at(..., transform()) and at(matrix()) are not implemented in this version of PIET")));
            return out;
          }
        }
        if (!atname.empty()) {
          if (seenModel && !atmode) {
            out.push_back(refuseCp("must specify at for all or none", 198));
            return out;
          }
          atmode = true;
        } else if (atmode) {
          out.push_back(refuseCp("must specify at for all or none", 198));
          return out;
        }
        seenModel = true;
        // ---- source ----
        CoefSrc src;
        std::vector<std::vector<double>> matCi;  // matrix-mode CI limits per pair
        const bool emode = ms.matrix.empty();
        std::string modelLabel = ms.name;
        std::vector<double> mb;
        std::vector<std::string> mnames;
        // The model's own margins at() record (e(cmd), e(k_at), e(at),
        // e(atstats#)), read while ITS e() is in scope: a stored model's e()
        // is swapped in only for the source read below (ScopedStored), and the
        // at() positions are computed after it is swapped out again.
        struct AtRecord {
          bool margins = false;
          double kAt = 0;
          std::optional<eval::EvalMatrix> at;
          std::vector<std::string> stats;  // e(atstats1) ...
        } atRec;
        const auto readAtRecord = [&]() {
          const std::string* ecmd = session.eval.eStrings.find("cmd");
          atRec.margins = ecmd != nullptr && *ecmd == "margins";
          const double* kat = session.eval.eScalars.find("k_at");
          atRec.kAt = kat != nullptr ? *kat : 0.0;
          if (!atRec.margins || !(atRec.kAt > 0)) return;
          auto am = session.eval.resolveEMatrixFor("at", eval::EvalSession::EReader::kCopy);
          if (am.matrix.has_value()) atRec.at = *am.matrix;
          for (size_t ri = 0; atRec.at.has_value() && ri < atRec.at->data.size(); ++ri) {
            const std::string* st = session.eval.eStrings.find("atstats" + std::to_string(ri + 1));
            atRec.stats.push_back(st != nullptr ? *st : std::string());
          }
        };
        if (emode) {
          if (ms.name == ".") {
            src = readLiveE(session);
            readAtRecord();
          } else {
            auto it = session.estimatesModels.find(ms.name);
            if (session.estimatesGet(ms.name) == nullptr && it == session.estimatesModels.end()) {
              out.push_back(refuseCp("estimation result " + ms.name + " not found", 111));
              return out;
            }
            if (it == session.estimatesModels.end() || !it->second) {
              out.push_back(refuseCp("estimation result " + ms.name + " not found", 111));
              return out;
            }
            ScopedStored guard(session.eval, *it->second);
            src = readLiveE(session);
            readAtRecord();
          }
        } else {
          const MatSpec mspec = parseMatSpec(ms.matrix);
          const eval::EvalMatrix* m = session.eval.findMatrix(mspec.name);
          if (m == nullptr || !mspec.ok || !matVector(*m, mspec, &mb, &mnames)) {
            if (m == nullptr) {
              out.push_back(refuseCp("matrix " + mspec.name + " not found", 111));
            } else {
              out.push_back(refuseCp(ms.name + ": invalid subscript for matrix()", 503));
            }
            return out;
          }
          src.empty = mb.empty();
          for (size_t q = 0; q < mb.size(); ++q) {
            std::string eq, nm;
            splitEqName(mnames[q], &eq, &nm);
            src.eq.push_back(eq);
            src.coef.push_back(nm);
            src.b.push_back(mb[q]);
            src.V.push_back(kNaN);
          }
        }
        // ---- levels / ci names (parse_cilevels :1712-1760) ----
        std::vector<double> levels;
        std::vector<std::string> cinames;
        {
          std::istringstream ls(ch.arg("levels"));
          std::string l;
          while (ls >> l) {
            levels.push_back(std::atof(l.c_str()));
            cinames.push_back("");
          }
          const std::string cis = ch.arg("ciname");
          for (const Tok& t : tokensParen(cis)) {
            char* end = nullptr;
            const double d = std::strtod(t.text.c_str(), &end);
            if (!t.paren && end != nullptr && *end == '\0' && !t.text.empty()) {
              levels.push_back(d);
              cinames.push_back("");
            } else {
              levels.push_back(kNaN);
              cinames.push_back(t.text);
            }
          }
          if (levels.empty()) {
            levels.push_back(95);  // c(level)
            cinames.push_back("");
          }
        }
        const bool noci = ch.has("noci");
        if (noci) {
          levels.clear();
          cinames.clear();
        }
        const size_t nci = levels.size();
        // ---- keepdrop ----
        if (src.empty) {
          msgs.push_back(noteCp("(" + ms.name + ": no coefficients found, all dropped, or none kept)"));
          if (nCiPlot[static_cast<size_t>(i)] < 0) nCiPlot[static_cast<size_t>(i)] = 0;
          // labels still attach (coefplot_add_label after collect)
          if (byLbl[j + 1].empty()) byLbl[j + 1] = ms.name;
          if (!plotLblForced[static_cast<size_t>(i)] && plotLbl[static_cast<size_t>(i)].empty())
            plotLbl[static_cast<size_t>(i)] = ms.name;
          continue;
        }
        const size_t r = src.b.size();
        std::vector<std::string> eqnm = src.eq, coefnm = src.coef;
        for (auto& e : eqnm)
          if (e.empty()) e = "_";
        bool meqs = false;
        for (const auto& e : eqnm)
          if (e != eqnm[0]) meqs = true;
        std::vector<double> se(r, kNaN), V = src.V, df(r, kNaN);
        for (size_t q = 0; q < r; ++q) se[q] = std::isnan(V[q]) ? kNaN : std::sqrt(V[q]);
        if (emode && src.hasDf)
          for (auto& d : df) d = src.dfr;
        // CIs
        std::vector<std::vector<double>> ci(r, std::vector<double>(2 * nci, kNaN));
        for (size_t lj = 0; lj < nci; ++lj) {
          if (!cinames[lj].empty()) {
            const std::string cname = trimC(cinames[lj]);
            std::vector<std::string> llul;
            std::istringstream ts(cname);
            std::vector<std::string> words;
            std::string w;
            while (ts >> w) words.push_back(w);
            if (words.size() == 1) {
              llul = {cname + "[1]", cname + "[2]"};
            } else if (cname.find('[') == std::string::npos) {
              if (words.size() != 2) {
                out.push_back(refuseCp("(" + ms.name + ": invalid syntax in ci())", 198));
                return out;
              }
              llul = words;
            } else {
              const size_t rb = cname.find(']');
              if (rb == cname.size() - 1) {
                const size_t sp = cname.find(' ');
                llul = {cname.substr(0, sp), cname.substr(sp + 1)};
              } else {
                llul = {cname.substr(0, rb + 1), cname.substr(rb + 1)};
              }
            }
            for (int h = 0; h < 2; ++h) {
              const std::string part = trimC(llul[static_cast<size_t>(h)]);
              const MatSpec mspec = parseMatSpec(part);
              std::vector<double> v;
              const eval::EvalMatrix* m = nullptr;
              std::optional<eval::EvalMatrix> em;
              std::string shown = mspec.name;
              if (emode) {
                shown = "e(" + mspec.name + ")";
                auto a = session.eval.resolveEMatrixFor(mspec.name, eval::EvalSession::EReader::kCopy);
                if (a.matrix.has_value()) {
                  em = *a.matrix;
                  m = &*em;
                }
              } else {
                m = session.eval.findMatrix(mspec.name);
              }
              if (m == nullptr) {
                msgs.push_back(noteCp("(" + ms.name + ": " + shown + " not found)"));
                continue;
              }
              if (!matVector(*m, mspec, &v, nullptr)) {
                out.push_back(refuseCp(ms.name + ": invalid subscript for ci", 503));
                return out;
              }
              if (v.size() != r) {
                msgs.push_back(noteCp("(" + ms.name + ": " + shown + " not conformable)"));
                continue;
              }
              for (size_t q = 0; q < r; ++q) ci[q][2 * lj + static_cast<size_t>(h)] = v[q];
            }
          } else {
            const double level = 1 - (1 - levels[lj] / 100) / 2;
            for (size_t q = 0; q < r; ++q) {
              const double crit = (std::isnan(df[q]) || df[q] > 2e17) ? cecon::stata_invnormal(level)
                                                                       : eval::stataInvttail(df[q], 1 - level);
              ci[q][2 * lj] = src.b[q] - crit * se[q];
              ci[q][2 * lj + 1] = src.b[q] + crit * se[q];
            }
          }
        }
        // omitted / base levels (:2417-2431)
        std::vector<char> p(r, 1);
        auto repl = [](std::string s, const std::string& from, const std::string& to) {
          size_t pos = 0;
          while ((pos = s.find(from, pos)) != std::string::npos) {
            s.replace(pos, from.size(), to);
            pos += to.size();
          }
          return s;
        };
        for (auto& c : coefnm) {
          c = repl(c, "bn.", ".");
          c = repl(c, "bno.", "o.");
        }
        if (!ch.has("omitted")) {
          for (size_t q = 0; q < r; ++q)
            if (globMatch(coefnm[q], "*o.*") && !(src.b[q] != 0)) p[q] = 0;
        }
        for (auto& c : coefnm) {
          if (c.compare(0, 2, "o.") == 0) c = c.substr(2);
          c = repl(c, "o.", ".");
        }
        if (!ch.has("baselevels")) {
          for (size_t q = 0; q < r; ++q)
            if (globMatch(coefnm[q], "*b.*") && !(src.b[q] != 0)) p[q] = 0;
        }
        for (auto& c : coefnm) c = repl(c, "b.", ".");
        bool firsteqonly = true;
        std::string equationOut;
        if (const Opt* kp = ch.get("keep")) {
          bool bad = false;
          auto kl = parseNamelist(kp->arg, "", &bad);
          if (bad) {
            out.push_back(refuseCp("keep(): invalid namelist", 198));
            return out;
          }
          for (const auto& pr : kl)
            if (!pr.first.empty()) firsteqonly = false;
          for (size_t q = 0; q < r; ++q) {
            bool any = false;
            for (const auto& pr : kl)
              if (globMatch(eqnm[q], pr.first.empty() ? "*" : pr.first) && globMatch(coefnm[q], pr.second))
                any = true;
            if (!any) p[q] = 0;
          }
        }
        if (const Opt* dp = ch.get("drop")) {
          bool bad = false;
          auto dl = parseNamelist(dp->arg, "", &bad);
          if (bad) {
            out.push_back(refuseCp("drop(): invalid namelist", 198));
            return out;
          }
          for (const auto& pr : dl)
            if (!pr.first.empty()) firsteqonly = false;
          for (size_t q = 0; q < r; ++q) {
            for (const auto& pr : dl)
              if (globMatch(eqnm[q], pr.first.empty() ? "*" : pr.first) && globMatch(coefnm[q], pr.second))
                p[q] = 0;
          }
        }
        if (firsteqonly) {
          for (size_t q = 0; q < r; ++q) {
            if (p[q]) {
              const std::string e0 = eqnm[q];
              for (size_t z = 0; z < r; ++z)
                if (eqnm[z] != e0) p[z] = 0;
              break;
            }
          }
        }
        bool anySel = false;
        for (char c : p) anySel = anySel || c;
        if (!anySel) {
          msgs.push_back(noteCp("(" + ms.name + ": no coefficients found, all dropped, or none kept)"));
          if (nCiPlot[static_cast<size_t>(i)] < 0) nCiPlot[static_cast<size_t>(i)] = 0;
          if (byLbl[j + 1].empty()) byLbl[j + 1] = ms.name;
          if (!plotLblForced[static_cast<size_t>(i)] && plotLbl[static_cast<size_t>(i)].empty())
            plotLbl[static_cast<size_t>(i)] = ms.name;
          continue;
        }
        std::vector<Row> rows;
        for (size_t q = 0; q < r; ++q) {
          if (!p[q]) continue;
          Row rw;
          rw.b = src.b[q];
          rw.V = V[q];
          rw.se = se[q];
          rw.df = df[q];
          rw.ci = ci[q];
          rw.eqnm = eqnm[q];
          rw.coefnm = coefnm[q];
          rw.at = 1;
          rw.plot = i;
          rw.by = static_cast<double>(j + 1);
          rw.t = (std::isnan(rw.se) || rw.se == 0) ? kNaN : rw.b / rw.se;
          if (std::isnan(rw.t)) rw.pval = kNaN;
          else if (std::isnan(rw.df)) rw.pval = (1 - normalCdf(std::fabs(rw.t))) * 2;
          else rw.pval = eval::stataTtail(rw.df, std::fabs(rw.t)) * 2;
          rows.push_back(rw);
        }
        // rename (:2496-2501; the non-regex form)
        if (const Opt* rn = ch.get("rename")) {
          auto [lhs, rhs] = parseComma(rn->arg);
          (void)rhs;
          bool bad = false;
          auto ml = parseMatchlist(lhs, 1, &bad);
          if (bad) {
            out.push_back(refuseCp("rename(): invalid matchlist", 198));
            return out;
          }
          std::vector<size_t> p0;
          for (size_t q = 0; q < rows.size(); ++q) p0.push_back(q);
          for (const auto& m : ml) {
            bool bad2 = false;
            auto names = parseNamelist(m[0], "*", &bad2);
            for (const auto& nm : names) {
              if (p0.empty()) break;
              std::vector<size_t> keepP0;
              for (size_t q : p0) {
                Row& rw = rows[q];
                bool match = false;
                const std::string& pat = nm.second;
                if (!pat.empty() && pat[0] == '*') {
                  const size_t rl = pat.size() - 1;
                  match = globMatch(rw.eqnm, nm.first) && rw.coefnm.size() >= rl &&
                          rw.coefnm.compare(rw.coefnm.size() - rl, rl, pat.substr(1)) == 0;
                  if (match) rw.coefnm = rw.coefnm.substr(0, rw.coefnm.size() - rl) + m[1];
                } else if (!pat.empty() && pat.back() == '*') {
                  const size_t rl = pat.size() - 1;
                  match = globMatch(rw.eqnm, nm.first) && rw.coefnm.compare(0, rl, pat.substr(0, rl)) == 0;
                  if (match) rw.coefnm = m[1] + rw.coefnm.substr(rl);
                } else {
                  match = globMatch(rw.eqnm, nm.first) && rw.coefnm == pat;
                  if (match) rw.coefnm = m[1];
                }
                if (!match) keepP0.push_back(q);
              }
              p0 = keepP0;
            }
          }
        }
        // asequation (:2504-2506)
        if (const Opt* ae = ch.get("asequation")) {
          const std::string eqv = ae->hasArg ? unquote(ae->arg) : (ms.name == "." ? std::string("_") : ms.name);
          for (auto& rw : rows) rw.eqnm = eqv;
        }
        // at (:2369-2398, :2513-2519): the margins at() values, or the numeric
        // equation/coefficient names
        if (atname == "_eq" || atname == "_coef") {
          for (auto& rw : rows) {
            const std::string& t = atname == "_eq" ? rw.eqnm : rw.coefnm;
            char* end = nullptr;
            const double v = std::strtod(t.c_str(), &end);
            rw.at = (!t.empty() && end != nullptr && *end == '\0') ? v : kNaN;
          }
        } else if (!atname.empty()) {
          char* end = nullptr;
          const double posv = atname == "at" ? 1.0 : std::strtod(atname.c_str(), &end);
          if (!emode || !atRec.margins || !(atRec.kAt > 0) || !atRec.at.has_value() ||
              (atname != "at" && (end == nullptr || *end != '\0'))) {
            out.push_back(ojson(eval::unsupportedOption(
                "at()", "coefplot at(): the plot positions come from margins' e(at)/e(atstats#), which this "
                        "model does not carry in this version of PIET")));
            return out;
          }
          const eval::EvalMatrix& at0 = *atRec.at;
          for (auto& rw : rows) rw.at = kNaN;
          for (size_t ri = 0; ri < at0.data.size(); ++ri) {
            if (ri >= atRec.stats.size() || atRec.stats[ri].empty()) continue;
            std::istringstream is(atRec.stats[ri]);
            std::string w;
            std::vector<size_t> valCols;
            size_t cidx = 0;
            while (is >> w) {
              if (w == "values") valCols.push_back(cidx);
              ++cidx;
            }
            if (static_cast<size_t>(posv) > valCols.size() || posv < 1) continue;
            const size_t jcol = valCols[static_cast<size_t>(posv) - 1];
            const std::string pat = std::to_string(ri + 1) + "._at*";
            for (auto& rw : rows) {
              std::string cn = rw.coefnm;
              if (cn.compare(0, 7, "1bn._at") == 0) cn = "1._at" + cn.substr(7);
              if (globMatch(cn, pat) && jcol < at0.data[ri].size()) rw.at = at0.data[ri][jcol];
            }
          }
        }
        if (ch.has("swapnames"))
          for (auto& rw : rows) std::swap(rw.coefnm, rw.eqnm);
        // notes on missings (:2532-2537)
        for (size_t lj = 0; lj < nci; ++lj) {
          bool anyMiss = false, anyOk = false;
          for (const auto& rw : rows) {
            if (std::isnan(rw.ci[2 * lj]) || std::isnan(rw.ci[2 * lj + 1])) anyMiss = true;
            if (!std::isnan(rw.ci[2 * lj]) || !std::isnan(rw.ci[2 * lj + 1])) anyOk = true;
          }
          if (anyMiss) {
            msgs.push_back(noteCp("(" + ms.name + ": " +
                                  (anyOk ? "CI" + std::to_string(lj + 1) + " missing for some coefficients"
                                         : "could not determine CI" + std::to_string(lj + 1)) +
                                  ")"));
          }
        }
        // equation for the plot label (:2570-2572)
        if (!firsteqonly && meqs) {
          bool same = true;
          for (const auto& rw : rows)
            if (rw.eqnm != rows[0].eqnm) same = false;
          if (same) equationOut = rows[0].eqnm;
        }
        ciCols = std::max(ciCols, nci);
        for (auto& rw : rows) C.push_back(rw);
        nCiPlot[static_cast<size_t>(i)] = std::max(nCiPlot[static_cast<size_t>(i)], static_cast<int>(nci));
        // labels (coefplot_add_label, force 0)
        std::string lab = ms.name;
        if (!equationOut.empty()) lab = ms.name == "." ? equationOut : ms.name + "=" + equationOut;
        if (byLbl[j + 1].empty()) byLbl[j + 1] = ms.name;
        if (!plotLblForced[static_cast<size_t>(i)] && plotLbl[static_cast<size_t>(i)].empty())
          plotLbl[static_cast<size_t>(i)] = lab;
      }
    }
  }
  for (auto& rw : C) rw.ci.resize(2 * ciCols, kNaN);
  for (auto& m : msgs) out.push_back(m);

  // ---- nothing to plot: r() cleared, rc 0 (:184-189) ----
  eval::replaceRClass(session.eval);
  if (C.empty()) {
    // an r-class program that returns nothing: every r() is unset, and reads
    // as missing (w2an cf_rf_keepnone `0|.|.|.|.|`, log:2758)
    eval::declareRPosted(session.eval, "coefplot", {});
    out.push_back(noteCp("(nothing to plot)"));
    return out;
  }
  const int Nci = static_cast<int>(ciCols);
  const bool vertical = verticalOpt || (!horizontalOpt && atmode);
  const bool horizontal = !vertical;

  int meqs = 0;
  std::string xlabels, eqlabelsR;
  // at() mode: the positions are the at() values, nothing is arranged or
  // labelled (coefplot_add_eq_and_grp)
  if (!atmode) {
  // ---- arrange (coefplot_multiple_eqs, niceorder, order) ----
  {
    std::vector<std::tuple<std::string, std::string, std::string>> eqv;
    for (const auto& rw : C) eqv.emplace_back(strofreal(rw.by), strofreal(rw.plot), rw.eqnm);
    std::sort(eqv.begin(), eqv.end());
    for (size_t i = 1; i < eqv.size(); ++i) {
      if (std::get<2>(eqv[i]) != std::get<2>(eqv[i - 1]) && std::get<1>(eqv[i]) == std::get<1>(eqv[i - 1]) &&
          std::get<0>(eqv[i]) == std::get<0>(eqv[i - 1])) {
        meqs = 1;
        break;
      }
    }
    if (meqs == 0)
      for (auto& rw : C) rw.eqnm = "_";
  }
  {
    // niceorder: equation by first appearance, coefficient by first appearance
    // (_cons last), then the original order
    const size_t r = C.size();
    std::map<std::string, size_t> eqFirst;
    std::map<std::pair<std::string, std::string>, size_t> coefFirst;
    for (size_t q = 0; q < r; ++q) {
      eqFirst.emplace(C[q].eqnm, q);
      coefFirst.emplace(std::make_pair(C[q].eqnm, C[q].coefnm), q);
    }
    std::vector<size_t> idx(r);
    for (size_t q = 0; q < r; ++q) idx[q] = q;
    std::stable_sort(idx.begin(), idx.end(), [&](size_t a, size_t b) {
      const size_t ea = eqFirst[C[a].eqnm], eb = eqFirst[C[b].eqnm];
      if (ea != eb) return ea < eb;
      const double ca = C[a].coefnm == "_cons" ? std::numeric_limits<double>::infinity()
                                               : static_cast<double>(coefFirst[{C[a].eqnm, C[a].coefnm}]);
      const double cb = C[b].coefnm == "_cons" ? std::numeric_limits<double>::infinity()
                                               : static_cast<double>(coefFirst[{C[b].eqnm, C[b].coefnm}]);
      if (ca != cb) return ca < cb;
      return a < b;
    });
    std::vector<Row> sorted;
    for (size_t q : idx) sorted.push_back(C[q]);
    C = std::move(sorted);
  }
  if (!trimC(orderText).empty()) {
    bool bad = false;
    auto ol = parseNamelist(orderText, "", &bad);
    if (bad) {
      out.push_back(refuseCp("order(): invalid namelist", 198));
      return out;
    }
    bool anyEq = false, allEq = true;
    for (const auto& pr : ol) {
      if (!pr.first.empty()) anyEq = true;
      else allEq = false;
    }
    if (ol.size() > 0 && ol[0].first.empty() && anyEq) {
      out.push_back(refuseCp("inconsistent order(): specify equations for all or for none", 198));
      return out;
    }
    std::vector<Row> res;
    std::vector<char> tag(C.size(), 0);
    auto emptyRow = [&](const std::string& eq) {
      Row e;
      e.at = 1;
      e.eqnm = eq;
      e.ci.assign(2 * ciCols, kNaN);
      return e;
    };
    if (allEq && anyEq) {
      for (const auto& pr : ol) {
        if (pr.second == ".") {
          res.push_back(emptyRow(pr.first));
          continue;
        }
        for (size_t q = 0; q < C.size(); ++q)
          if (!tag[q] && C[q].eqnm == pr.first && globMatch(C[q].coefnm, pr.second)) {
            tag[q] = 1;
            res.push_back(C[q]);
          }
      }
      for (size_t q = 0; q < C.size(); ++q)
        if (!tag[q]) res.push_back(C[q]);
    } else {
      std::vector<std::string> eqs;
      for (const auto& rw : C)
        if (eqs.empty() || eqs.back() != rw.eqnm) eqs.push_back(rw.eqnm);
      for (const auto& eq : eqs) {
        for (const auto& pr : ol) {
          if (pr.second == ".") {
            res.push_back(emptyRow(eq));
            continue;
          }
          for (size_t q = 0; q < C.size(); ++q)
            if (!tag[q] && C[q].eqnm == eq && globMatch(C[q].coefnm, pr.second)) {
              tag[q] = 1;
              res.push_back(C[q]);
            }
        }
        for (size_t q = 0; q < C.size(); ++q)
          if (!tag[q] && C[q].eqnm == eq) {
            tag[q] = 1;
            res.push_back(C[q]);
          }
      }
    }
    C = std::move(res);
  }

  // ---- coefficient labels (coefplot_coeflbls + coeflbls) ----
  {
    bool bad = false;
    auto cl = parseMatchlist(coeflabelsText, 1, &bad);
    if (bad) {
      out.push_back(refuseCp("coeflabels(): invalid matchlist", 198));
      return out;
    }
    std::vector<size_t> tag(C.size(), 0);
    for (size_t jj = 0; jj < cl.size(); ++jj) {
      bool bad2 = false;
      auto names = parseNamelist(cl[jj][0], "*", &bad2);
      for (const auto& nm : names)
        for (size_t q = 0; q < C.size(); ++q)
          if (tag[q] == 0 && globMatch(C[q].eqnm, nm.first) && globMatch(C[q].coefnm, nm.second)) tag[q] = jj + 1;
    }
    std::string last;
    bool haveLast = false;
    for (size_t q = 0; q < C.size(); ++q) {
      if (tag[q] && !std::isnan(C[q].plot)) C[q].coeflbl = cl[tag[q] - 1][1];
    }
    for (size_t q = 0; q < C.size(); ++q) {
      const std::string v = C[q].coefnm;
      if (v.empty()) continue;
      if (haveLast && v == last) {
        // coefplot_add_label(..., "coef", i, "coeflbl", 1): the label of the
        // previous coefficient
        C[q].coeflbl = C[q - 1].coeflbl;
        continue;
      }
      if (C[q].coeflbl.empty() && !(tag[q] && !std::isnan(C[q].plot))) {
        C[q].coeflbl = nolabels ? v : compileXlabel(session, v, interact);
      }
      last = v;
      haveLast = true;
    }
  }
  // ---- category values (coefplot_catvals) ----
  for (size_t q = 1; q < C.size(); ++q) {
    C[q].at = C[q - 1].at + ((C[q].coefnm != C[q - 1].coefnm) || (C[q].eqnm != C[q - 1].eqnm) ||
                             std::isnan(C[q].plot)
                                 ? 1
                                 : 0);
  }
  double xmin = C.empty() ? 1 : C[0].at;
  for (const auto& rw : C) xmin = std::min(xmin, rw.at);
  std::vector<std::string> eqlbl;
  {
    int jj = 1;
    auto eqLabelOf = [&](int idx, const std::string& eqn) {
      return idx <= static_cast<int>(eqlabelsUser.size()) ? eqlabelsUser[static_cast<size_t>(idx - 1)] : eqn;
    };
    if (!C.empty()) eqlbl.push_back(eqLabelOf(1, C[0].eqnm));
    C[0].eq = 1;
    for (size_t q = 1; q < C.size(); ++q) {
      C[q].eq = C[q - 1].eq;
      if (C[q].eqnm != C[q - 1].eqnm) {
        ++jj;
        eqlbl.push_back(eqLabelOf(jj, C[q].eqnm));
        C[q].eq = C[q].eq + 1;
      }
    }
    const double eqgap = noeqlabels ? 0 : 1;
    std::vector<double> at0;
    for (const auto& rw : C) at0.push_back(rw.at);
    for (size_t q = 1; q < C.size(); ++q) {
      C[q].at = C[q - 1].at + (at0[q] - at0[q - 1]) + (C[q].eqnm != C[q - 1].eqnm ? eqgap : 0);
    }
  }
  // ---- axis labels (get_axis_labels / coefplot_ticks_and_labels) ----
  {
    std::string space;
    for (size_t q = 0; q < C.size(); ++q) {
      if (q > 0 && C[q].eqnm == C[q - 1].eqnm && C[q].coefnm == C[q - 1].coefnm) continue;
      if (!std::isnan(C[q].plot)) {
        std::string lbl = C[q].coeflbl;
        std::string piece = compoundQuote(lbl);
        if (!wrapLen.empty() || !truncLen.empty()) {
          std::string l2 = lbl;
          if (!truncLen.empty()) {
            auto pc = pieces(l2, static_cast<size_t>(std::atoi(truncLen.c_str())));
            l2 = pc.empty() ? std::string() : pc[0];
          }
          if (!wrapLen.empty()) {
            auto pc = pieces(l2, static_cast<size_t>(std::atoi(wrapLen.c_str())));
            if (pc.empty()) {
              piece = compoundQuote(l2);
            } else {
              std::string inner;
              for (size_t z = 0; z < pc.size(); ++z) inner += (z ? " " : "") + compoundQuote(pc[z]);
              if (pc.size() == 1) inner += " " + compoundQuote("");
              piece = compoundQuote(inner);
            }
          } else {
            piece = compoundQuote(l2);
          }
        }
        xlabels += space + strofreal(C[q].at) + " " + piece;
        space = " ";
      }
    }
    if (meqs && !noeqlabels) {
      std::string sp;
      for (size_t e = 0; e < eqlbl.size(); ++e) {
        double mn = std::numeric_limits<double>::infinity(), mx = -mn;
        for (const auto& rw : C)
          if (static_cast<size_t>(rw.eq) == e + 1) {
            mn = std::min(mn, rw.at);
            mx = std::max(mx, rw.at);
          }
        eqlabelsR += sp + strofreal(mn + (mx - mn) / 2) + " " + compoundQuote(eqlbl[e]);
        sp = " ";
      }
    }
  }
  }  // !atmode
  // ---- offsets (:433-456) ----
  if (customOffset) {
    for (auto& rw : C) {
      const size_t pi = std::isnan(rw.plot) ? 0 : static_cast<size_t>(rw.plot);
      if (pi > 0 && !offsetPlot[pi].empty()) {
        const double off = std::atof(offsetPlot[pi].c_str());
        rw.at = horizontal ? rw.at - off : rw.at + off;
      }
    }
  } else if (!atmode && !nooffsets && Nplots > 1) {
    std::set<std::pair<double, double>> u;
    bool unique = true;
    for (const auto& rw : C) {
      if (!u.insert({rw.by, rw.at}).second) unique = false;
    }
    if (!unique) {
      if (recycle == false || nSubgr == 1 || true) {
        if (!recycle || nSubgr == 1) {
          for (auto& rw : C)
            if (!std::isnan(rw.plot)) rw.at = rw.at - 0.5 + rw.plot / (Nplots + 1);
        } else {
          for (int jj = 1; jj <= nSubgr; ++jj) {
            const int first = plotIdx[static_cast<size_t>(jj - 1)].empty() ? 1 : plotIdx[static_cast<size_t>(jj - 1)].front();
            const int lastp = plotIdx[static_cast<size_t>(jj - 1)].empty() ? 0 : plotIdx[static_cast<size_t>(jj - 1)].back();
            for (auto& rw : C)
              if (rw.by == jj && !std::isnan(rw.plot))
                rw.at = rw.at - 0.5 + (rw.plot - first + 1) / (lastp - first + 2);
          }
        }
      }
    }
  }

  // ---- mlabel expressions (:467-481) and mlbl width ----
  const size_t R = C.size();
  // ---- legend (:526-650) ----
  std::string legendlbls, legendorder;
  {
    int jkey = 0;
    for (int i = 1; i <= Nplots; ++i) {
      if (nCiPlot[static_cast<size_t>(i)] < 0) continue;
      const int nci = nCiPlot[static_cast<size_t>(i)];
      int key = 0;
      bool haveKey = false;
      const int k = keyPlot[static_cast<size_t>(i)];
      if (!citopPlot[static_cast<size_t>(i)] && nci > 0) {
        if (k >= 1 && k <= nci) {
          key = jkey + k;
          haveKey = true;
        }
        jkey += nci;
      }
      ++jkey;  // the marker plot
      if (k == 0) {
        key = jkey;
        haveKey = true;
      }
      if (citopPlot[static_cast<size_t>(i)] && nci > 0) {
        if (k >= 1 && k <= nci) {
          key = jkey + k;
          haveKey = true;
        }
        jkey += nci;
      }
      if (haveKey) {
        std::string lab = plotLabels(plotlabels, plotLbl, i);
        // gettoken trash : plotlab, qed(hasquotes): a label already in quotes
        // stays as it is, any other is put in compound quotes (:624-628)
        const std::string tl = trimC(lab);
        const bool hasQuotes = !tl.empty() && (tl.front() == '"' || tl.compare(0, 2, "`\"") == 0);
        legendlbls += (legendlbls.empty() ? "" : " ") + std::string("label(") + std::to_string(key) + " " +
                      (hasQuotes ? lab : compoundQuote(lab)) + ")";
        legendorder += (legendorder.empty() ? "" : " ") + std::to_string(key);
      }
    }
    if (!legendorder.empty()) {
      legendorder = "all order(" + legendorder + ")";
      if (Nplots == 1 && nSubgr == 1 && !legendGiven) legendorder += " off";
    } else {
      legendorder = "off";
    }
  }

  // ---- generate (:245-354) ----
  if (!generate.empty()) {
    std::vector<std::string> names = {"by", "plot", "at", "mlbl", "mlpos", "b", "V", "se", "t", "df", "pval"};
    for (int c = 1; c <= Nci; ++c) {
      names.push_back("ll" + std::to_string(c));
      names.push_back("ul" + std::to_string(c));
    }
    for (const auto& n : names) {
      const std::string full = generate + n;
      if (!strutil::isValidStataIdentifier(full)) {
        out.push_back(refuseCp(full + " invalid name", 198));
        return out;
      }
      // re-read every time: a drop reloads the frame
      Datastore* ds0 = session.datastore();
      if (ds0 != nullptr && ds0->has(full)) {
        if (replaceOpt) {
          SessionState::TempDropScope td(session);
          (void)dispatch("quietly drop " + full, session);
        } else {
          out.push_back(refuseCp("variable " + full + " already defined", 110));
          return out;
        }
      }
    }
    int64_t N = session.datastore() != nullptr ? session.datastore()->nRows() : 0;
    if (N < static_cast<int64_t>(R)) {
      (void)dispatch("quietly set obs " + std::to_string(R), session);
      N = static_cast<int64_t>(R);
    }
    auto col = [&](auto getter, bool asFloat) {
      std::vector<double> v(static_cast<size_t>(N), kNaN);
      for (size_t q = 0; q < R; ++q) {
        const double x = getter(C[q]);
        v[q] = asFloat ? toFloatC(x) : x;
      }
      return v;
    };
    // mlbl
    std::vector<std::string> mlbl(static_cast<size_t>(N));
    size_t mlLen = 1;
    for (size_t q = 0; q < R; ++q) {
      mlbl[q] = C[q].mlbl;
      mlLen = std::max(mlLen, C[q].mlbl.size());
    }
    struct Out {
      std::string name;
      std::vector<double> v;
      std::string type;
      bool isStr = false;
      std::string fmt;
      std::string label;
    };
    std::vector<Out> cols;
    const std::vector<double> byV = col([](const Row& r) { return r.by; }, true);
    const std::vector<double> plotV = col([](const Row& r) { return r.plot; }, true);
    const std::vector<double> atV = col([](const Row& r) { return r.at; }, true);
    const std::vector<double> mlposV = col([](const Row& r) { return r.mlpos; }, true);
    const std::vector<double> dfV = col([](const Row& r) { return r.df; }, true);
    const std::string nf = format.empty() ? std::string("%9.0g") : format;
    cols.push_back({generate + "by", byV, compressType(byV), false, "%9.0g", "subgraph ID"});
    cols.push_back({generate + "plot", plotV, compressType(plotV), false, "%9.0g", "plot ID"});
    cols.push_back({generate + "at", atV, compressType(atV), false, "%9.0g", "plot position (category axis)"});
    cols.push_back({generate + "mlbl", {}, "str" + std::to_string(mlLen), true,
                    "%" + std::to_string(std::max<size_t>(mlLen, 9)) + "s", "marker label"});
    cols.push_back({generate + "mlpos", mlposV, compressType(mlposV), false, "%9.0g", "marker label position"});
    cols.push_back({generate + "b", col([](const Row& r) { return r.b; }, true), "float", false, nf, "coefficient"});
    cols.push_back({generate + "V", col([](const Row& r) { return r.V; }, true), "float", false, nf, "variance"});
    cols.push_back(
        {generate + "se", col([](const Row& r) { return r.se; }, true), "float", false, nf, "standard error"});
    cols.push_back(
        {generate + "t", col([](const Row& r) { return r.t; }, true), "float", false, nf, "t or z statistic"});
    cols.push_back({generate + "df", dfV, compressType(dfV), false, "%9.0g", "degrees of freedom"});
    cols.push_back({generate + "pval", col([](const Row& r) { return r.pval; }, true), "float", false, nf, "p-value"});
    for (int c = 1; c <= Nci; ++c) {
      const size_t lj = static_cast<size_t>(c - 1);
      cols.push_back({generate + "ll" + std::to_string(c),
                      col([lj](const Row& r) { return r.ci[2 * lj]; }, true), "float", false, nf,
                      "CI" + std::to_string(c) + ": lower limit"});
      cols.push_back({generate + "ul" + std::to_string(c),
                      col([lj](const Row& r) { return r.ci[2 * lj + 1]; }, true), "float", false, nf,
                      "CI" + std::to_string(c) + ": upper limit"});
    }
    std::vector<std::string> newVars;
    for (auto& oc : cols) {
      std::optional<ojson> err;
      if (oc.isStr) err = writeColumn(session, oc.name, nullptr, &mlbl);
      else err = writeColumn(session, oc.name, &oc.v, nullptr);
      if (err.has_value()) {
        out.push_back(*err);
        return out;
      }
      session.varMeta[oc.name].stataType = oc.type;
      session.varMeta[oc.name].format = oc.fmt;
      session.setVarLabel(oc.name, oc.label);
      newVars.push_back(oc.name);
    }
    // value labels (set_by_and_plot_labels :1995-2013, plotlabels()/bylabels())
    auto defineLabels = [&](const std::string& var, const std::vector<double>& vals,
                            const std::function<std::string(int)>& textOf) {
      std::set<int> lv;
      for (double v : vals)
        if (!std::isnan(v)) lv.insert(static_cast<int>(v));
      (void)dispatch("capture label drop " + var, session);
      for (int l : lv) {
        // a plain "..." string (PIET's label define does not take compound
        // quotes) unless the text itself holds a double quote
        const std::string txt = textOf(l);
        const std::string q = txt.find('"') == std::string::npos ? "\"" + txt + "\"" : compoundQuote(txt);
        (void)dispatch("quietly label define " + var + " " + std::to_string(l) + " " + q + ", add", session);
      }
      if (!lv.empty()) (void)dispatch("quietly label values " + var + " " + var, session);
    };
    defineLabels(generate + "plot", plotV, [&](int l) { return plotLabels(plotlabels, plotLbl, l); });
    defineLabels(generate + "by", byV, [&](int l) {
      if (l >= 1 && static_cast<size_t>(l) <= bylabels.size()) return bylabels[static_cast<size_t>(l - 1)];
      return (l >= 1 && l <= nSubgr) ? byLbl[static_cast<size_t>(l)] : std::string();
    });
    // mlabel() string expressions (:467-481): `replace mlbl = exp if plot==i`
    // with the @names injected (_coefplot_inject_temvars).
    for (int i = 1; i <= Nplots; ++i) {
      const std::string& ml = mlabelPlot[static_cast<size_t>(i)];
      if (ml.empty()) continue;
      std::string e = ml;
      std::string res;
      size_t pos = 0;
      bool bad = false;
      while (pos < e.size()) {
        if (e[pos] == '@') {
          size_t q = pos + 1;
          while (q < e.size() && (std::islower(static_cast<unsigned char>(e[q])) || e[q] == 'V')) ++q;
          while (q < e.size() && std::isdigit(static_cast<unsigned char>(e[q]))) ++q;
          std::string vn = e.substr(pos + 1, q - pos - 1);
          if (vn.empty()) {
            res += e[pos++];
            continue;
          }
          if (vn == "ll" || vn == "ul" || vn == "aux") vn += "1";
          static const std::set<std::string> kNs = {"b", "V", "se", "t", "df", "pval", "at", "mlbl", "mlpos", "plot", "by"};
          bool known = kNs.count(vn) != 0;
          for (int c = 1; c <= Nci; ++c)
            if (vn == "ll" + std::to_string(c) || vn == "ul" + std::to_string(c)) known = true;
          if (!known) {
            out.push_back(refuseCp("@" + vn + " not found", 111));
            bad = true;
            break;
          }
          res += generate + vn;
          pos = q;
          continue;
        }
        res += e[pos++];
      }
      if (bad) return out;
      const std::string tr = trimC(res);
      Datastore* dsN = session.datastore();
      if (dsN != nullptr && dsN->has(tr)) continue;  // a variable: drawn as is
      ojson rb = dispatch("quietly replace " + generate + "mlbl = " + res + " if " + generate + "plot==" +
                              std::to_string(i),
                          session);
      for (const auto& b : rb) {
        if (b.is_object() && b.value("type", std::string()) == "error") {
          out.push_back(refuseCp("invalid expression in mlabel()", 198));
          return out;
        }
      }
    }
    out.push_back(noteCp("Generated variables:"));
    out.push_back(mutationResult(session, "gen", newVars));
  }

  // ---- r() (:682-690): an empty macro is an unset one ----
  {
    std::vector<std::string> keys;
    auto putS = [&](const std::string& k, const std::string& v) {
      if (trimC(v).empty()) return;
      session.eval.rStrings.set(k, v);
      keys.push_back(k);
    };
    putS("labels", xlabels);
    putS("eqlabels", eqlabelsR);
    putS("legend", (legendlbls.empty() ? std::string() : legendlbls) + " " + legendorder);
    session.eval.rValues.set("n_plots", Nplots);
    session.eval.rValues.set("n_subgr", nSubgr);
    session.eval.rValues.set("n_ci", Nci);
    keys.push_back("n_plots");
    keys.push_back("n_subgr");
    keys.push_back("n_ci");
    eval::declareRPosted(session.eval, "coefplot", keys);
  }

  // rows for the drawn block
  if (drawRows != nullptr) {
    *drawRows = ojson::array();
    for (const auto& rw : C) {
      if (std::isnan(rw.plot)) continue;
      ojson e;
      e["plot"] = rw.plot;
      e["plot_label"] = plotLabels(plotlabels, plotLbl, static_cast<int>(rw.plot));
      e["name"] = rw.coefnm;
      e["b"] = std::isnan(rw.b) ? ojson(nullptr) : ojson(rw.b);
      e["ci_lo"] = rw.ci.empty() || std::isnan(rw.ci[0]) ? ojson(nullptr) : ojson(rw.ci[0]);
      e["ci_hi"] = rw.ci.size() < 2 || std::isnan(rw.ci[1]) ? ojson(nullptr) : ojson(rw.ci[1]);
      drawRows->push_back(e);
    }
  }
  return out;
}

}  // namespace piet::handlers
