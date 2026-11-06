// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// serset.cpp — the serset store, the `serset` command family, the graph
// registration lifecycle and the generic data-plot serset builder. See
// include/piet/serset.h for the contract and the oracle lines it rests on.
#include "piet/serset.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <limits>
#include <regex>

#include "piet/cobra_runner.h"
#include "piet/datastore.h"
#include "piet/eval/errors.h"
#include "piet/eval/session.h"
#include "piet/mutation_result.h"
#include "piet/scratch_seq.h"
#include "piet/session_state.h"
#include "piet/strutil.h"
#include "io/win_compat.h"

namespace piet::serset {

using ojson = nlohmann::ordered_json;

namespace {

constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();
constexpr int kMaxSersetId = 1999;

ojson refusal(const std::string& message, int rc) {
  return ojson(eval::errorBlock(rc == 198 ? "syntax_error" : "engine_error", message, std::nullopt,
                                std::nullopt, std::nullopt, "r(" + std::to_string(rc) + ")"));
}

ojson displayBlock(const std::string& text) {
  ojson b;
  b["type"] = "display";
  b["text"] = text;
  return b;
}

std::string trimText(const std::string& s) {
  size_t a = 0, b = s.size();
  while (a < b && std::isspace(static_cast<unsigned char>(s[a]))) ++a;
  while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) --b;
  return s.substr(a, b - a);
}

bool parseInt(const std::string& s, int* out) {
  const std::string t = trimText(s);
  if (t.empty()) return false;
  size_t i = 0;
  if (t[0] == '+' || t[0] == '-') i = 1;
  if (i >= t.size()) return false;
  for (size_t j = i; j < t.size(); ++j)
    if (!std::isdigit(static_cast<unsigned char>(t[j]))) return false;
  try {
    *out = std::stoi(t);
  } catch (...) {
    return false;
  }
  return true;
}

// Rows of `s` reordered by `order` (indices into the current rows).
void permuteRows(Serset& s, const std::vector<size_t>& order) {
  for (Column& c : s.cols) {
    if (c.isStr) {
      std::vector<std::string> v;
      v.reserve(order.size());
      for (size_t i : order) v.push_back(i < c.str.size() ? c.str[i] : std::string());
      c.str = std::move(v);
    } else {
      std::vector<double> v;
      v.reserve(order.size());
      for (size_t i : order) v.push_back(i < c.num.size() ? c.num[i] : kNaN);
      c.num = std::move(v);
    }
  }
}

// Stable ascending sort by column `k`, missing last (Stata's sort order).
void sortByColumn(Serset& s, int k) {
  if (k < 0 || static_cast<size_t>(k) >= s.cols.size()) return;
  const Column& key = s.cols[static_cast<size_t>(k)];
  std::vector<size_t> order(static_cast<size_t>(s.n));
  for (size_t i = 0; i < order.size(); ++i) order[i] = i;
  if (key.isStr) {
    std::stable_sort(order.begin(), order.end(),
                     [&key](size_t a, size_t b) { return key.str[a] < key.str[b]; });
  } else {
    std::stable_sort(order.begin(), order.end(), [&key](size_t a, size_t b) {
      const double x = key.num[a], y = key.num[b];
      const bool mx = std::isnan(x), my = std::isnan(y);
      if (mx || my) return !mx && my;
      return x < y;
    });
  }
  permuteRows(s, order);
}

bool hasColumn(const Serset& s, const std::string& name) {
  for (const Column& c : s.cols)
    if (c.name == name) return true;
  return false;
}

// The name a duplicate claim appends under: <name>1, <name>2, ... first free.
std::string freshName(const Serset& s, const std::string& base) {
  for (int k = 1;; ++k) {
    std::string cand = base + std::to_string(k);
    if (!hasColumn(s, cand)) return cand;
  }
}

const std::string& storedName(const Column& c) { return c.stored.empty() ? c.name : c.stored; }

// Adds a data plot's columns to `s` (rule (c) of serset.h): each variable takes
// the first column stored under its name that this plot has not used yet, else
// a new one (StataBE 19.5, BUG-1543: `(line y y) (line y1 x)` stores `y y y1 x`
// and loads `y y1 y11 x`, the real y1 in y11).
void mergePlotColumns(Serset& s, const std::vector<Column>& cols) {
  std::vector<bool> claimed(s.cols.size(), false);
  for (const Column& c : cols) {
    size_t k = 0;
    while (k < claimed.size() && (claimed[k] || storedName(s.cols[k]) != c.name)) ++k;
    if (k < claimed.size()) {
      claimed[k] = true;
      continue;
    }
    Column copy = c;
    if (hasColumn(s, c.name)) {
      copy.stored = c.name;
      copy.name = freshName(s, c.name);
    }
    s.cols.push_back(std::move(copy));
    claimed.push_back(true);
  }
}

int64_t cellsOf(const Serset& s) {
  return s.n * static_cast<int64_t>(std::max<size_t>(1, s.cols.size()));
}

// Drops the values of an oversize serset (its shape stays).
void enforceRetention(Serset& s) {
  if (cellsOf(s) <= kMaxRetainedCells) return;
  s.retained = false;
  for (Column& c : s.cols) {
    std::vector<double>().swap(c.num);
    std::vector<std::string>().swap(c.str);
  }
}

std::string csvNumber(double v) {
  if (std::isnan(v)) return ".";
  char buf[40];
  std::snprintf(buf, sizeof(buf), "%.17g", v);
  return buf;
}

std::string csvString(const std::string& v) {
  std::string out = "\"";
  for (char c : v) {
    if (c == '"') out += '"';
    out += c;
  }
  out += '"';
  return out;
}

// `serset use`: replaces the data in memory with the serset's variables, their
// names and storage types (the w2al helper reads `: type` of each).
ojson useSerset(SessionState& session, const Serset& s) {
  ojson out = ojson::array();
  if (!s.retained) {
    out.push_back(refusal("serset use: this serset holds " + std::to_string(s.n) + " observations on " +
                              std::to_string(s.cols.size()) +
                              " variables, more than PIET keeps for read-back; its data were not "
                              "retained",
                          910));
    return out;
  }
  const std::string hbin = mintScratchPath(session.workdir(), "serset");
  const std::string csv = hbin + ".csv";
  {
    std::ofstream f(cecon::to_native_path(csv), std::ios::binary);
    if (!f) {
      out.push_back(refusal("serset use: could not stage the serset's data", 603));
      return out;
    }
    for (size_t c = 0; c < s.cols.size(); ++c) {
      if (c) f << ",";
      f << s.cols[c].name;
    }
    f << "\n";
    for (int64_t i = 0; i < s.n; ++i) {
      for (size_t c = 0; c < s.cols.size(); ++c) {
        if (c) f << ",";
        const Column& col = s.cols[c];
        const size_t r = static_cast<size_t>(i);
        if (col.isStr) {
          f << csvString(r < col.str.size() ? col.str[r] : std::string());
        } else {
          f << csvNumber(r < col.num.size() ? col.num[r] : kNaN);
        }
      }
      f << "\n";
    }
    f.flush();
    if (!f) {
      cecon::native_unlink(csv);
      out.push_back(refusal("serset use: could not stage the serset's data (disk full or I/O error)",
                            693));
      return out;
    }
  }
  std::string factorList;
  for (const Column& c : s.cols) {
    if (!c.isStr) continue;
    if (!factorList.empty()) factorList += ",";
    factorList += c.name;
  }
  std::vector<std::string> args = {"convert", "--data", csv, "--out", hbin, "--delim", ","};
  if (!factorList.empty()) {
    args.push_back("--factor");
    args.push_back(factorList);
  }
  const CobraRunResult cr = runCobra(args);
  cecon::native_unlink(csv);
  if (!cr.ok || !cecon::native_exists(hbin)) {
    cecon::native_unlink(hbin);
    out.push_back(refusal("serset use: the serset's data could not be loaded", 693));
    return out;
  }
  session.load(hbin, SessionState::LoadIntent::FreshRead);
  session.clearMetadata();
  session.invalidateDatastore();
  for (const Column& c : s.cols) session.varMeta[c.name].stataType = c.type;
  out.push_back(handlers::mutationResult(session, "use"));
  return out;
}

}  // namespace

// ---------------------------------------------------------------------------
// Store

int Store::add(Serset s) {
  int id = 0;
  while (id <= kMaxSersetId && sets_.count(id) != 0) ++id;
  if (id > kMaxSersetId) return -1;
  enforceRetention(s);
  sets_[id] = std::move(s);
  current_ = id;
  return id;
}

bool Store::select(int id) {
  if (sets_.count(id) == 0) return false;
  current_ = id;
  return true;
}

const Serset* Store::get(int id) const {
  auto it = sets_.find(id);
  return it == sets_.end() ? nullptr : &it->second;
}

void Store::drop(int id) {
  sets_.erase(id);
  if (id == current_) current_ = -1;
}

void Store::dropOwner(const std::string& owner) {
  for (auto it = sets_.begin(); it != sets_.end();) {
    if (it->second.owner == owner) {
      if (it->first == current_) current_ = -1;
      it = sets_.erase(it);
    } else {
      ++it;
    }
  }
}

void Store::renameOwner(const std::string& from, const std::string& to) {
  for (auto& kv : sets_)
    if (kv.second.owner == from) kv.second.owner = to;
  if (lastOwner_ == from) lastOwner_ = to;
}

void Store::clear() {
  sets_.clear();
  current_ = -1;
  lastOwner_.clear();
}

int64_t Store::bytes() const {
  int64_t total = 0;
  for (const auto& kv : sets_) {
    for (const Column& c : kv.second.cols) {
      total += static_cast<int64_t>(c.name.size() + c.type.size());
      total += static_cast<int64_t>(c.num.size()) * 8;
      for (const auto& s : c.str) total += static_cast<int64_t>(s.size());
    }
  }
  return total;
}

// ---------------------------------------------------------------------------
// Column helpers

std::string tempName(int i) {
  char buf[16];
  std::snprintf(buf, sizeof(buf), "__%06d", i);
  return buf;
}

double toFloat(double v) {
  if (std::isnan(v)) return v;
  return static_cast<double>(static_cast<float>(v));
}

Column numericColumn(std::string name, std::string type, std::vector<double> values) {
  Column c;
  c.name = std::move(name);
  c.type = std::move(type);
  if (c.type == "float") {
    for (double& v : values) v = toFloat(v);
  }
  c.num = std::move(values);
  return c;
}

Column stringColumn(std::string name, std::vector<std::string> values, std::string type) {
  Column c;
  c.name = std::move(name);
  c.isStr = true;
  if (type.empty()) {
    size_t w = 1;
    for (const auto& s : values) w = std::max(w, s.size());
    type = "str" + std::to_string(w);
  }
  c.type = std::move(type);
  c.str = std::move(values);
  return c;
}

std::string storageType(SessionState& session, Datastore& ds, const std::string& var) {
  auto it = session.varMeta.find(var);
  if (it != session.varMeta.end() && it->second.stataType.has_value() &&
      !it->second.stataType->empty()) {
    return *it->second.stataType;
  }
  if (ds.has(var) && ds.dtype(var) == "f64") return "float";
  size_t w = 1;
  if (ds.has(var)) {
    for (const auto& lvl : ds.col(var).levels) w = std::max(w, lvl.size());
  }
  return "str" + std::to_string(w);
}

Column variableColumn(SessionState& session, Datastore& ds, const std::string& var,
                      const std::vector<int64_t>& rows, const std::string& name) {
  Column c;
  c.name = name.empty() ? var : name;
  c.type = storageType(session, ds, var);
  if (!ds.has(var)) {
    c.num.assign(rows.size(), kNaN);
    return c;
  }
  const cecon::Column& col = ds.col(var);
  const int64_t n = ds.nRows();
  if (ds.dtype(var) == "f64") {
    const double* d = col.f64();
    c.num.reserve(rows.size());
    for (int64_t r : rows) c.num.push_back(r >= 0 && r < n ? d[r] : kNaN);
  } else {
    c.isStr = true;
    const int32_t* codes = col.codes();
    c.str.reserve(rows.size());
    for (int64_t r : rows) {
      int32_t code = (r >= 0 && r < n) ? codes[r] : -1;
      if (code >= 0 && static_cast<size_t>(code) < col.levels.size()) {
        c.str.push_back(col.levels[static_cast<size_t>(code)]);
      } else {
        c.str.emplace_back();
      }
    }
  }
  return c;
}

// ---------------------------------------------------------------------------
// The generic builder

std::vector<Serset> buildPlotSersets(std::vector<PlotSpec> plots) {
  struct Group {
    Serset s;
    std::string key;
    bool shareable = false;
  };
  std::vector<Group> groups;
  for (PlotSpec& p : plots) {
    if (p.computed.has_value()) {
      Group g;
      g.s = std::move(*p.computed);
      groups.push_back(std::move(g));
      continue;
    }
    const int64_t n = p.cols.empty() ? 0 : static_cast<int64_t>(p.cols.front().size());
    if (p.sorted) {
      Group g;
      g.s.n = n;
      mergePlotColumns(g.s, p.cols);
      sortByColumn(g.s, p.sortCol);
      groups.push_back(std::move(g));
      continue;
    }
    Group* target = nullptr;
    for (Group& g : groups) {
      if (g.shareable && g.key == p.key && g.s.n == n) {
        target = &g;
        break;
      }
    }
    if (target == nullptr) {
      Group g;
      g.key = p.key;
      g.shareable = true;
      g.s.n = n;
      groups.push_back(std::move(g));
      target = &groups.back();
    }
    mergePlotColumns(target->s, p.cols);
  }
  std::vector<Serset> out;
  out.reserve(groups.size());
  for (Group& g : groups) out.push_back(std::move(g.s));
  return out;
}

// ---------------------------------------------------------------------------
// Graph lifecycle

GraphName parseNameOption(const std::string& nameOpt) {
  GraphName g;
  const std::string t = trimText(nameOpt);
  if (t.empty()) return g;
  static const std::regex reSplit(R"([,\s]+)");
  std::sregex_token_iterator it(t.begin(), t.end(), reSplit, -1), endIt;
  bool first = true;
  for (; it != endIt; ++it) {
    const std::string tok = it->str();
    if (tok.empty()) continue;
    if (first) {
      g.name = tok;
      first = false;
    } else if (strutil::lower(tok) == "replace") {
      g.replace = true;
    }
  }
  return g;
}

std::optional<ojson> nameCollision(SessionState& session, const std::string& nameOpt) {
  const GraphName g = parseNameOption(nameOpt);
  if (g.name.empty() || g.replace) return std::nullopt;
  if (session.namedGraphs.count(g.name) == 0) return std::nullopt;
  return refusal("graph " + g.name + " already exists", 110);
}

void registerGraph(SessionState& session, ojson& block, const std::string& nameOpt,
                   std::vector<Serset> sets, const ojson* meta) {
  const GraphName g = parseNameOption(nameOpt);
  const std::string nm = g.name.empty() ? std::string("Graph") : g.name;
  session.sersets.dropOwner(nm);
  for (Serset& s : sets) {
    s.owner = nm;
    session.sersets.add(std::move(s));
  }
  session.sersets.setLastOwner(nm);
  session.lastGraph = block;
  session.namedGraphs[nm] = block;
  // graph-family AI-4: the describe record and the current graph.
  session.graphMeta[nm] = meta != nullptr && meta->is_object() ? *meta : makeGraphMeta("twoway", "", "");
  session.currentGraph = nm;
  if (!g.name.empty()) block["name"] = nm;
}

std::pair<std::string, std::string> schemeGraphSize(const std::string& schemeIn) {
  // {scheme, its #include parent, graphsize y, graphsize x} as shipped in
  // /Applications/StataNow/ado/base/s/scheme-*.scheme ("" = inherited).
  struct Row {
    const char* name;
    const char* parent;
    const char* y;
    const char* x;
  };
  static const Row kRows[] = {
      {"s2color", "", "4", "5.5"},          {"s2color8", "s2color", "", ""},
      {"s2mono", "s2color", "", ""},        {"s1color", "s2color", "", ""},
      {"s1mono", "s1color", "", ""},        {"s1rcolor", "s1color", "", ""},
      {"meta", "s2color", "", ""},          {"economist", "s2color", "2.2", "2.2"},
      {"s1manual", "s1mono", "2.392", "3.12"}, {"s2gcolor", "s2color", "2.392", "3.12"},
      {"s2gmanual", "s2mono", "2.392", "3.12"}, {"s2manual", "s2mono", "2.392", "3.12"},
      {"sj", "s2mono", "2.6", "3.575"},     {"stsj", "s2mono", "2.6", "3.575"},
      {"stcolor", "s2color", "4.5", "7.5"}, {"stcolor_alt", "stcolor", "4", "6"},
      {"stcolor_meta", "stcolor", "", ""},  {"stcolor_mv", "stcolor_alt", "", ""},
      {"stcoloralias", "stcolor", "", ""},  {"stgcolor", "stcolor", "2.392", "3.987"},
      {"stgcolor_alt", "stcolor_alt", "2.392", "3.588"},
      {"stgcolor_meta", "stcolor_meta", "2.392", "3.987"},
      {"stgcolor_mv", "stgcolor_alt", "", ""}, {"stmono1", "s1mono", "4.5", "7.5"},
      {"stmono2", "s2mono", "4.5", "7.5"},  {"sttrans", "s2color", "4.5", "7.5"},
  };
  std::string cur = strutil::lower(schemeIn);
  std::string y, x;
  for (int guard = 0; guard < 16 && !cur.empty(); ++guard) {
    const Row* row = nullptr;
    for (const Row& r : kRows)
      if (cur == r.name) row = &r;
    if (row == nullptr) break;
    if (y.empty() && row->y[0] != '\0') y = row->y;
    if (x.empty() && row->x[0] != '\0') x = row->x;
    if (!y.empty() && !x.empty()) break;
    cur = row->parent;
  }
  if (y.empty()) y = "4.5";
  if (x.empty()) x = "7.5";
  return {y, x};
}

ojson makeGraphMeta(const std::string& family, const std::string& command, const std::string& scheme) {
  ojson m;
  m["family"] = family;
  m["command"] = command;
  const std::string sch = scheme.empty() ? std::string("stcolor") : scheme;
  m["scheme"] = sch;
  const auto sz = schemeGraphSize(sch);
  m["ysize"] = sz.first;
  m["xsize"] = sz.second;
  std::time_t now = std::time(nullptr);
  std::tm tmv{};
#if defined(_WIN32)
  localtime_s(&tmv, &now);
#else
  localtime_r(&now, &tmv);
#endif
  char d[32], t[32];
  std::strftime(d, sizeof d, "%b %Y", &tmv);
  std::strftime(t, sizeof t, "%H:%M:%S", &tmv);
  // The day unpadded: r(command_date) is "3 Nov 2026" (StataBE 19.5, measured
  // 2026-11-03, scratch-laneA2/agents/gfx/g2.do; c(current_date) " 3 Nov 2026").
  m["date"] = std::to_string(tmv.tm_mday) + " " + d;
  m["time"] = std::string(t);
  return m;
}

void dropGraphSersets(SessionState& session, const std::vector<std::string>& names, bool all) {
  if (all) {
    session.sersets.clear();
    return;
  }
  for (const auto& nm : names) session.sersets.dropOwner(nm);
}

std::vector<Serset> ownedBy(SessionState& session, const std::string& owner) {
  std::vector<Serset> out;
  for (const auto& kv : session.sersets.all())
    if (kv.second.owner == owner) out.push_back(kv.second);
  return out;
}

// ---------------------------------------------------------------------------
// graph dir

ojson runGraphDir(SessionState& session, const std::string& pattern, bool memory, bool gph,
                  const std::string& cwd) {
  const bool doMem = memory || !gph;
  const bool doGph = gph || !memory;
  std::vector<std::string> mlist, glist;
  if (doMem) {
    for (const auto& kv : session.namedGraphs) {
      if (pattern.empty() || strutil::globMatch(pattern, kv.first)) mlist.push_back(kv.first);
    }
    std::sort(mlist.begin(), mlist.end());
  }
  if (doGph && !cwd.empty()) {
    std::error_code ec;
    for (const auto& e : std::filesystem::directory_iterator(cwd, ec)) {
      if (ec) break;
      const std::string nm = e.path().filename().u8string();
      if (nm.size() < 4 || nm.substr(nm.size() - 4) != ".gph") continue;
      if (pattern.empty() || strutil::globMatch(pattern, nm)) glist.push_back(nm);
    }
    std::sort(glist.begin(), glist.end());
  }
  auto join = [](const std::vector<std::string>& v) {
    std::string out;
    for (size_t i = 0; i < v.size(); ++i) {
      if (i) out += " ";
      out += v[i];
    }
    return out;
  };
  eval::replaceRClass(session.eval);
  session.eval.rStrings.set("list", join(mlist) + " " + join(glist));
  eval::declareRPosted(session.eval, "graph dir", {"list"});
  ojson out = ojson::array();
  std::vector<std::string> all = mlist;
  all.insert(all.end(), glist.begin(), glist.end());
  if (!all.empty()) {
    std::string text;
    for (size_t i = 0; i < all.size(); ++i) {
      text += (i % 4 == 0) ? (i ? "\n    " : "    ") : "  ";
      text += all[i];
    }
    out.push_back(displayBlock(text));
  }
  return out;
}

// ---------------------------------------------------------------------------
// .gph persistence

ojson toJson(const std::vector<Serset>& sets) {
  ojson arr = ojson::array();
  for (const Serset& s : sets) {
    ojson js;
    js["n"] = s.n;
    js["retained"] = s.retained;
    ojson cols = ojson::array();
    for (const Column& c : s.cols) {
      ojson jc;
      jc["name"] = c.name;
      if (!c.stored.empty()) jc["stored"] = c.stored;  // BUG-1543
      jc["type"] = c.type;
      if (c.isStr) {
        jc["str"] = c.str;
      } else {
        ojson vals = ojson::array();
        for (double v : c.num) {
          if (std::isnan(v)) vals.push_back(nullptr);
          else vals.push_back(v);
        }
        jc["num"] = vals;
      }
      cols.push_back(jc);
    }
    js["cols"] = cols;
    arr.push_back(js);
  }
  return arr;
}

std::vector<Serset> fromJson(const ojson& j) {
  std::vector<Serset> out;
  if (!j.is_array()) return out;
  for (const auto& js : j) {
    if (!js.is_object()) continue;
    Serset s;
    s.n = js.value("n", static_cast<int64_t>(0));
    s.retained = js.value("retained", true);
    for (const auto& jc : js.value("cols", ojson::array())) {
      Column c;
      c.name = jc.value("name", std::string());
      c.stored = jc.value("stored", std::string());
      c.type = jc.value("type", std::string("double"));
      if (jc.contains("str")) {
        c.isStr = true;
        for (const auto& v : jc["str"]) c.str.push_back(v.is_string() ? v.get<std::string>() : "");
      } else {
        for (const auto& v : jc.value("num", ojson::array()))
          c.num.push_back(v.is_number() ? v.get<double>() : kNaN);
      }
      s.cols.push_back(std::move(c));
    }
    out.push_back(std::move(s));
  }
  return out;
}

// ---------------------------------------------------------------------------
// The `serset` command family

ojson runSerset(SessionState& session, const std::vector<std::string>& args, bool clearOpt) {
  (void)clearOpt;
  ojson out = ojson::array();
  Store& st = session.sersets;
  const std::string sub = args.empty() ? std::string() : strutil::lower(args[0]);

  // `serset`: r(id) of the current serset (-1 when none), r(k), r(N).
  if (sub.empty()) {
    eval::replaceRClass(session.eval);
    session.eval.rValues.set("id", static_cast<double>(st.current()));
    std::vector<std::string> posted = {"id"};
    if (const Serset* s = st.currentSet()) {
      session.eval.rValues.set("k", static_cast<double>(s->cols.size()));
      session.eval.rValues.set("N", static_cast<double>(s->n));
      posted = {"id", "k", "N"};
    }
    eval::declareRPosted(session.eval, "serset", posted);
    return out;
  }

  // `serset [set] #`
  int id = 0;
  if (sub == "set" || parseInt(args[0], &id)) {
    const std::string num = sub == "set" ? (args.size() > 1 ? args[1] : std::string()) : args[0];
    if (!parseInt(num, &id)) {
      out.push_back(refusal("serset set: a serset number is required", 198));
      return out;
    }
    if (!st.select(id)) {
      out.push_back(refusal("series " + std::to_string(id) + " not found", 111));
      return out;
    }
    return out;
  }

  // `serset use [, clear]`: the current serset becomes the data (w2al log:183-188
  // `no serset in use`, r(198)).
  if (sub == "use") {
    const Serset* s = st.currentSet();
    if (s == nullptr) {
      out.push_back(refusal("no serset in use", 198));
      return out;
    }
    const Serset copy = *s;
    ojson blocks = useSerset(session, copy);
    for (auto& b : blocks) {
      if (b.is_object() && b.value("type", std::string()) == "error") {
        out.push_back(b);
        return out;
      }
    }
    eval::replaceRClass(session.eval);
    std::string names;
    for (size_t i = 0; i < copy.cols.size(); ++i) {
      if (i) names += " ";
      names += copy.cols[i].name;
    }
    session.eval.rStrings.set("varnames", names);
    eval::declareRPosted(session.eval, "serset use", {"varnames"});
    for (auto& b : blocks) out.push_back(b);
    return out;
  }

  if (sub == "clear") {
    st.clear();
    return out;
  }

  if (sub == "drop") {
    if (args.size() < 2) {
      st.clear();
      return out;
    }
    std::vector<int> ids;
    for (size_t i = 1; i < args.size(); ++i) {
      const std::string a = args[i];
      if (strutil::lower(a) == "_all") {
        st.clear();
        return out;
      }
      const size_t slash = a.find('/');
      int lo = 0, hi = 0;
      if (slash != std::string::npos && parseInt(a.substr(0, slash), &lo) &&
          parseInt(a.substr(slash + 1), &hi)) {
        for (int k = lo; k <= hi; ++k) ids.push_back(k);
      } else if (parseInt(a, &lo)) {
        ids.push_back(lo);
      } else {
        out.push_back(refusal("serset drop: `" + a + "' is not a serset number", 198));
        return out;
      }
    }
    for (int k : ids) st.drop(k);
    return out;
  }

  // `serset dir` (w2al log:5418-5429): the names as stored, a renamed duplicate
  // under its own (StataBE 19.5, BUG-1543: `y x y x` where `serset use` loads
  // `y x y1 x1`).
  if (sub == "dir") {
    std::string text;
    for (const auto& kv : st.all()) {
      const Serset& s = kv.second;
      char head[96];
      std::snprintf(head, sizeof(head), "%3d.  %lld observation%s on %zu variable%s", kv.first,
                    static_cast<long long>(s.n), s.n == 1 ? "" : "s", s.cols.size(),
                    s.cols.size() == 1 ? "" : "s");
      std::string names;
      for (size_t i = 0; i < s.cols.size(); ++i) {
        if (i) names += " ";
        names += storedName(s.cols[i]);
      }
      text += "\n" + std::string(head) + "\n      " + names + "\n";
    }
    if (!text.empty()) out.push_back(displayBlock(text));
    return out;
  }

  out.push_back(refusal("serset " + sub + " is not supported in this version of PIET", 199));
  return out;
}

}  // namespace piet::serset
