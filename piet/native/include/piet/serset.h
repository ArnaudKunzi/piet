// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// serset.h — Stata's sersets: the data a graph was drawn from, kept beside the
// graph and read back with `serset use` (Stata documents that making a graph
// extracts its data into a serset).
//
// THE STORE. One per session (SessionState::sersets). A serset has an id
// 0..1999; a new one takes the lowest free id and becomes the current serset
// (`serset` answers r(id) of it, -1 when there is none). Every serset is owned by
// the graph that made it (its name; an unnamed graph is "Graph"): replacing that
// graph drops its sersets, `graph drop <name>` drops them, `graph drop _all`,
// `serset clear` and `clear all` drop every one (w2al_twoway_family.log:177,
// :5456 tf_ss_two_sc `3|2|0 1 2`, :5525 tf_ss_afterdrop_sc `0|-1|`).
//
// THE PRODUCERS. A graph command hands registerGraph() the sersets it made, in
// plot order. The helpers below build them:
//   - buildPlotSersets(): the twoway data plots (scatter, line, connected, area,
//     the range plots, tsline): one column per role variable in role order, then
//     the weight variable, then the mlabel() variable; rows = the plot's sample
//     (if/in) in data order, missing values kept. Plots with the same sample key
//     and no `sort` share one serset, created at the first such plot; a later
//     plot's variable takes a column stored under its name that this plot has
//     not used yet, else it is appended under its own name, which `serset use`
//     makes <name>1, <name>2, ... (first free) when the serset holds it already
//     (StataBE 19.5, BUG-1543: `(scatter y x) (line y y) (line x x)` stores
//     `y x y x` and loads `y x y1 x1`; `(line y y) (line y y)` stores `y y`); a
//     plot drawn with `sort` gets its own serset sorted by its x variable (w2al
//     tf_tw_007 :630, tf_rcap_082 :2207, tf_rcapsym_077 :2380, tf_lfit_107 :1676,
//     tf_ss_dir :5380).
//   - computed plots (fits, functions, immediates, histogram/kdensity layers, bar
//     and box statistics) build their own Serset and pass it as `computed`.
// A column whose name begins with `__` is a Stata tempvar; tempName(i) makes one.
//
// Sersets ride in the SESSION, never in the emitted graph block: freeze fixtures
// pin graph blocks byte for byte.
#ifndef PIET_NATIVE_SERSET_H
#define PIET_NATIVE_SERSET_H

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "vendor/nlohmann/json.hpp"

namespace piet {
class SessionState;
class Datastore;
}  // namespace piet

namespace piet::serset {

// One serset variable. Numeric values are doubles (NaN = missing) already
// rounded to the storage type; a string column holds its text.
struct Column {
  std::string name;
  // The name the column is stored under when `name` is its renamed duplicate
  // (`serset dir` lists it; "" = `name`).
  std::string stored;
  std::string type;  // Stata storage type: byte int long float double str#
  bool isStr = false;
  std::vector<double> num;
  std::vector<std::string> str;

  size_t size() const { return isStr ? str.size() : num.size(); }
};

struct Serset {
  std::vector<Column> cols;
  int64_t n = 0;       // rows
  std::string owner;   // the graph name ("Graph" for an unnamed graph)
  // False when the serset was too large to keep (kMaxRetainedCells): its shape
  // (n, names, types) is kept for `serset`/`serset dir`, its values are not, and
  // `serset use` refuses it by name instead of loading anything else.
  bool retained = true;
};

// A serset larger than this many cells keeps only its shape (see `retained`).
inline constexpr int64_t kMaxRetainedCells = 8'000'000;

class Store {
 public:
  // Adds `s` under the lowest free id (0..1999) and makes it current. Returns
  // the id, or -1 when all 2000 slots are taken.
  int add(Serset s);
  bool select(int id);
  int current() const { return current_; }
  const Serset* get(int id) const;
  const Serset* currentSet() const { return get(current_); }
  void drop(int id);
  void dropOwner(const std::string& owner);
  // graph-family AI-6: `graph rename` moves a graph's sersets with it.
  void renameOwner(const std::string& from, const std::string& to);
  void clear();
  const std::map<int, Serset>& all() const { return sets_; }
  bool empty() const { return sets_.empty(); }
  int64_t bytes() const;
  // The name the last registered graph took (`graph save` without a name saves
  // that graph's sersets with it).
  const std::string& lastOwner() const { return lastOwner_; }
  void setLastOwner(const std::string& o) { lastOwner_ = o; }

 private:
  std::map<int, Serset> sets_;
  int current_ = -1;
  std::string lastOwner_;
};

// `__000000`, `__000001`, ... (unique within one serset).
std::string tempName(int i);

// A numeric column `name` of `type`; the values are rounded to float when the
// type is float.
Column numericColumn(std::string name, std::string type, std::vector<double> values);
// A string column; its type is str<max byte length, at least 1> unless given.
Column stringColumn(std::string name, std::vector<std::string> values, std::string type = "");

// Rounds to IEEE single precision (Stata's float storage), NaN kept.
double toFloat(double v);

// The storage type `: type` answers for a dataset variable: the recorded
// declaration, else float for a numeric column; a string column with no record
// is str<longest value>.
std::string storageType(SessionState& session, Datastore& ds, const std::string& var);

// One dataset variable over `rows` (indices into `ds`, in the order given), as a
// serset column named `name` (default: the variable's own name).
Column variableColumn(SessionState& session, Datastore& ds, const std::string& var,
                      const std::vector<int64_t>& rows, const std::string& name = "");

// One plot's contribution to the generic builder.
struct PlotSpec {
  // A computed plot: its own serset, never shared.
  std::optional<Serset> computed;
  // A data plot: its columns over its sample, in serset order (role variables,
  // then weight, then mlabel), all of the same length.
  std::vector<Column> cols;
  std::string key;      // the sample key: layer if / in / weight text
  bool sorted = false;  // drawn with `sort`: own serset, sorted by sortCol
  int sortCol = -1;     // index into cols of the x variable
};

// The sersets of a graph in creation order (see the file comment).
std::vector<Serset> buildPlotSersets(std::vector<PlotSpec> plots);

// Registers a finished graph: name() parsed from `nameOpt` (default "Graph"),
// the previous graph of that name and its sersets are dropped, `sets` are added
// in order (the last one is current), lastGraph and namedGraphs[name] take the
// block. An explicit name is written into the block's "name" field (as before);
// the default name is not.
// graph-family AI-4: `meta` (makeGraphMeta) is what `graph describe` reports for
// the graph (session.graphMeta[name]; a twoway record with no command when
// null), and the graph becomes the current graph (session.currentGraph).
void registerGraph(SessionState& session, nlohmann::ordered_json& block, const std::string& nameOpt,
                   std::vector<Serset> sets, const nlohmann::ordered_json* meta = nullptr);

// graph-family AI-4: a graph's describe record (graph describe's r()):
// {family, command, scheme, ysize, xsize, date, time}. The size is the
// scheme's graphsize, never the xsize()/ysize() options (w2ai gf_tw_titles_d
// `s1mono|4|5.5` under xsize(5) ysize(4), log:1406; gf_cmb_018_d, :1593).
nlohmann::ordered_json makeGraphMeta(const std::string& family, const std::string& command,
                                     const std::string& scheme);
// The `graphsize y` and `graphsize x` of a scheme, following its #include
// chain as the base schemes ship it (/Applications/StataNow/ado/base/s/).
std::pair<std::string, std::string> schemeGraphSize(const std::string& scheme);

// The graph name name() selects, and whether it said `replace`.
struct GraphName {
  std::string name;  // "" when no name() was given
  bool replace = false;
};
GraphName parseNameOption(const std::string& nameOpt);

// r(110) `graph <name> already exists` when name() names a graph in memory
// without `replace` (w2al tf_st_dup2, log:5024-5029); nullopt otherwise.
std::optional<nlohmann::ordered_json> nameCollision(SessionState& session, const std::string& nameOpt);

// `graph drop` hook: drop the sersets of the named graphs, or all of them.
void dropGraphSersets(SessionState& session, const std::vector<std::string>& names, bool all);

// The `serset` command family: `serset`, `serset [set] #`, `serset use [, clear]`,
// `serset clear`, `serset drop`, `serset dir`. `args` = the words after
// `serset` (before the comma); `clearOpt` = `, clear` given.
nlohmann::ordered_json runSerset(SessionState& session, const std::vector<std::string>& args,
                                 bool clearOpt);

// `graph dir [pattern] [, memory gph detail]` (graph dir): r(list) is the
// in-memory graph names (byte-sorted, `: list sort`) and the *.gph files of the
// working directory, joined as `"`mlist' `glist'"'` -- with `memory` alone a
// trailing blank (w2al tf_ss_two_gd `tf_g1 tf_g2 `, log:5510; w2ai
// gf_dropall_0_gd ` `). `cwd` = the directory *.gph files are listed from.
nlohmann::ordered_json runGraphDir(SessionState& session, const std::string& pattern, bool memory,
                                   bool gph, const std::string& cwd);

// .gph persistence: the graph's sersets as JSON (names, types, values) and back.
nlohmann::ordered_json toJson(const std::vector<Serset>& sets);
std::vector<Serset> fromJson(const nlohmann::ordered_json& j);
// The sersets a registered graph owns, in id order.
std::vector<Serset> ownedBy(SessionState& session, const std::string& owner);

}  // namespace piet::serset

#endif  // PIET_NATIVE_SERSET_H
