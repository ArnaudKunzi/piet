// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// Arc F — the seven QE / RD commands' PIET half: parse the Stata line, call the
// already-merged cobra verb, and print the table exactly as Stata prints it.
//
// NO STATISTICS LIVE HERE. Every number in every table below is read straight
// out of the engine's JSON; this file's whole job is the three things a human
// actually sees — the Stata syntax it accepts, the column geometry it prints,
// and the e()/rc it leaves behind.
//
//   rdrobust y running, c(0)          -> cobra rdrobust
//   rddensity running, c(0)           -> cobra rddensity
//   rdplot y running, c(0)            -> cobra rdplot          (table only; the
//                                        plot coordinates in the JSON are a
//                                        sibling arc's GRAPH half)
//   binscatter y x1, nquantiles(20)   -> cobra binscatter      (prints nothing but
//                                        its savedata notes, warnings and
//                                        reportreg tables; posts e())
//   eventstudyinteract y F2 .. L4, absorb() cohort() control_cohort() vce()
//   did_imputation y id t g, horizons() pretrends() autosample
//   sdid y id t treated, vce() reps() seed()
//
// RENDERING CONTRACT. Handlers here return an ARRAY of blocks: one `display`
// block per printed line, then ONE structured block last. That is the
// handlers_resample.cpp precedent (P6 wave) and it is what makes the log tee
// and the console show the Stata table rather than a JSON key dump — the tee
// (session_state.cpp::logTee) renders `display` blocks as their text and
// everything else as `[type]` + an indented dump. It also composes with
// `quietly` for free: the quietly filter keeps only error/echo blocks, so
// `quietly rdrobust ...` prints nothing without this file knowing about it.
//
// LAYOUT AUTHORITY. internal capture p3_stata.log lines
// 152-378 (live Stata 19.5). Every column width below was measured off that
// file, not guessed. The two RD header blocks and rddensity's binomial ladder
// are genuinely unique shapes, so they are laid out per-command here rather
// than by bending the shared coefficient-table renderer; the ordinary
// coefficient tables (eventstudyinteract / did_imputation) reuse the EXACT
// widths handlers_resample.cpp already established for Stata's 78-column
// regression table, and every number goes through fmtStataDefaultG — the one
// %9.0g renderer the whole product shares.
//
// P-VALUES. Not computed here. The engine returns a p for every row of every
// one of these commands, so there is no normal-CDF in this file at all.
#include "piet/handlers.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <unordered_set>
#include <vector>

#include "io/win_compat.h"  // to_native_path / native_exists: binscatter's savedata files
#include "piet/cobra_runner.h"
#include "piet/dispatcher.h"  // dispatch -- binscatter reruns the ado's marksample/regressions
#include "piet/display_hardening.h"  // fmtStataDefaultG — Stata's %9.0g default rendering
#include "piet/eval/errors.h"
#include "piet/eval/tokenizer.h"  // findUnquotedComma — binscatter reads its options as typed
#include "piet/inrange_filter.h"
#include "piet/mutation_result.h"
#include "piet/scratch_seq.h"
#include "piet/session_state.h"
#include "piet/settings.h"
#include "piet/stata_format.h"  // fmt::general — %w.0g text of file write / regress
#include "piet/strutil.h"
#include "piet/twoway_opts.h"  // twoway::layerStyle — Arc G's graph half reuses it verbatim

namespace piet::eval {
// eval/macros.cpp: the text a `local v = <real>` stores (%18.0g). Declared
// here as loops.cpp declares it (macros.h does not export it).
std::string stataMacroRealText(double d);
}  // namespace piet::eval

// ---------------------------------------------------------------------------
// BUG-233 / FIX:OPTNET-ABBREV-SWEEP — this file's option vocabularies.
//
// Every set below used to be a function-local `static const` sitting next to its
// reject net, which meant nothing upstream of the handler could see it — and the
// handler's own `k == "fullname"` comparisons therefore ran against RAW keys
// while eval::rejectUnknownOptions had already ACCEPTED an abbreviation of them.
// That is BUG-231's mechanism, and it was live in nets across six files.
//
// Registering here (static init, before any dispatch) lets dispatcher.cpp's single
// resolution point canonicalize the keys BEFORE routing. The sets did not get
// copied: each net now binds a reference back to its registered entry, so there is
// still exactly one place each spelling is written.
namespace {
const piet::eval::OptVocab& kVocabRdrobust = piet::eval::registerOptVocab(
    {"rdrobust"}, {{"c", "p", "q", "h", "b", "kernel", "nnmatch", "level"},
     {"covs", "fuzzy", "bwselect", "vce", "scalepar", "scaleregul", "masspoints",
      "bwcheck", "bwrestrict", "stdvars", "all", "weights", "deriv", "rho", "sharpbw"}});
const piet::eval::OptVocab& kVocabRddensity = piet::eval::registerOptVocab(
    {"rddensity"}, {{"c", "p", "q", "h", "kernel"},
     {"vce", "fitselect", "bwselect", "all", "plot", "masspoints", "bino", "nobino",
      "binomw", "binomn", "binomv", "level"}});
const piet::eval::OptVocab& kVocabRdplot = piet::eval::registerOptVocab(
    {"rdplot"}, {{"c", "p", "h", "nbins", "kernel"},
     {"covs", "binselect", "scale", "weights", "ci", "shade", "hide", "genvars",
      "graph_options", "support", "masspoints"}});
// binscatter (7.02). `known` = the options runBinscatter serves;
// `unimplemented` = the rest of the ado's syntax line, refused by name. The
// aliases are the syntax line's minimum abbreviations (the capitalised stems:
// Nquantiles, MEDians, CONTROLs, noAddmean, LINEtype, COLors, MColors,
// LColors, Msymbols, GENxq) plus the longer prefixes the graph keys unioned in
// by dispatcher.cpp's kVocabBinscatterGraph would otherwise make ambiguous.
// runBinscatter itself reads the options from the typed line by the same
// stems, so these only keep the reject net from refusing a spelling Stata
// takes.
const piet::eval::OptVocab& kVocabBinscatter = piet::eval::registerOptVocab(
    {"binscatter"},
    {{"by", "nquantiles", "genxq", "discrete", "medians", "controls", "absorb", "noaddmean",
      "linetype", "reportreg", "colors", "mcolors", "lcolors", "msymbols", "savedata", "replace"},
     {"xq", "rd", "savegraph", "nofastxtile", "randvar", "randcut", "randn", "nbins",
      "create_xq", "x_q", "symbols", "method", "unique"},
     {{"n", "nquantiles"}, {"nq", "nquantiles"}, {"med", "medians"}, {"median", "medians"},
      {"control", "controls"}, {"noa", "noaddmean"}, {"line", "linetype"}, {"col", "colors"},
      {"color", "colors"}, {"mc", "mcolors"}, {"mcolor", "mcolors"}, {"lc", "lcolors"},
      {"lcolor", "lcolors"}, {"m", "msymbols"}, {"ms", "msymbols"}, {"msymbol", "msymbols"},
      {"gen", "genxq"}}});
const piet::eval::OptVocab& kVocabEventstudyinteract = piet::eval::registerOptVocab(
    {"eventstudyinteract"}, {{"absorb", "cohort", "control_cohort",
                                                          "vce", "level", "cluster"},
     {"covariates", "weights",
                                                                  "residualize"}});
const piet::eval::OptVocab& kVocabDidImputation = piet::eval::registerOptVocab(
    {"did_imputation"}, {{"horizons", "pretrends", "autosample",
                                                          "level"},
     {"fe", "controls", "unitcontrols", "timecontrols", "cluster", "wtr", "delta",
      "allhorizons", "avgeffectsby", "saveweights", "minn", "shift", "maxit", "tol",
      "loadweights", "autosample_off", "leaveout", "nose"}});
const piet::eval::OptVocab& kVocabSdid = piet::eval::registerOptVocab(
    {"sdid"}, {{"vce", "reps", "seed", "level"},
     {"covariates", "method", "graph", "g1on", "g1_opt", "g2_opt", "graph_export",
      "msize", "unstandardized", "mattitles"}});
}  // namespace

namespace piet {
namespace handlers {

using ojson = nlohmann::ordered_json;

namespace {

// --- error / engine plumbing -------------------------------------------------

ojson qeError(const std::string& message, const std::string& code = "engine_error") {
  return ojson(eval::engineError(message, code));
}

// Handlers here return an ARRAY, so a rejection has to be an array too.
ojson qeErrorBlocks(const std::string& message, const std::string& code = "engine_error") {
  return ojson::array({qeError(message, code)});
}

// --- formatting helpers ------------------------------------------------------
// Deliberately local and tiny: the SHARED piece is fmtStataDefaultG (the %9.0g
// numeric renderer), which is what "do not write a second formatter" is about.
// padLeft/padRight/fixedN are three-line column primitives, identical in shape
// to handlers_resample.cpp's own file-local set.

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
// The regress family's name abbreviation: a name past `w` CHARACTERS keeps its
// first w-2, `~`, and its last character (never splits a UTF-8 sequence).
std::string abbrevName(const std::string& n, size_t w) {
  if (cecon::ident::codePointCount(n) <= w) return n;
  size_t i = n.size();
  do {
    --i;
  } while (i > 0 && (static_cast<unsigned char>(n[i]) & 0xC0) == 0x80);
  return cecon::ident::truncateCodePoints(n, w - 2) + "~" + n.substr(i);
}
std::string g9(double v) { return fmtStataDefaultG(v); }

// Stata's coefficient-TABLE cell is one character narrower than the shared
// %9.0g `display` renderer: the coefficient table reserves the sign column, so the
// digits get 8 characters where `display` gets 9. fmtStataDefaultG is
// calibrated for the display context — its own doc comment records
// `2.157789219986937 -> "2.1577892"`, a NINE-character result — and that is
// correct there; it is only the table cell that is narrower. Measured on the
// oracle twice over: did_imputation's tau0 (1.2081958740803342) prints
// `1.208196`, not `1.2081959` (p3_stata.log:326), and P6's jackknife CI upper
// prints `1.099479` (p6_stata.log:64). Below |1| the two rules COINCIDE —
// ".2071479" is eight characters either way — which is exactly why the shared
// renderer already reproduces every eventstudyinteract row byte for byte and
// only the >=1 magnitudes need this pass. The shared renderer still decides
// the digits whenever its answer fits; this only re-runs the same %g ladder
// one notch tighter when it does not.
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

std::string fixedN(double v, int n) {
  char buf[64];
  std::snprintf(buf, sizeof buf, "%.*f", n, v);
  return std::string(buf);
}
std::string f2(double v) { return fixedN(v, 2); }
std::string f3(double v) { return fixedN(v, 3); }
std::string f4(double v) { return fixedN(v, 4); }
std::string f5(double v) { return fixedN(v, 5); }

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

// JSON readers that never throw: a field the engine did not send prints as
// Stata's own missing marker rather than crashing or inventing a zero.
double num(const ojson& j, const char* key, double fallback = 0.0) {
  if (!j.contains(key) || j.at(key).is_null() || !j.at(key).is_number()) return fallback;
  return j.at(key).get<double>();
}
int64_t inum(const ojson& j, const char* key, int64_t fallback = 0) {
  if (!j.contains(key) || j.at(key).is_null() || !j.at(key).is_number()) return fallback;
  return static_cast<int64_t>(j.at(key).get<double>());
}
std::string str(const ojson& j, const char* key, const std::string& fallback = "") {
  if (!j.contains(key) || !j.at(key).is_string()) return fallback;
  return j.at(key).get<std::string>();
}
// Stata's standard 78-column coefficient-table column header, e.g.
//   " Coefficient  std. err.      t    P>|t|     [95% conf. interval]"
// `errWord` is "std. err." (eventstudyinteract, under a Robust banner) or
// "Std. err." (did_imputation); `stat` is "t" or "z".
std::string coefHeader(const std::string& errWord, const std::string& stat,
                       const std::string& lvl) {
  return " Coefficient  " + errWord + "      " + stat + "    P>|" + stat + "|     [" + lvl +
         "% conf. interval]";
}

// A nested engine map (`coef`, `se`, ...) keyed by coefficient name.
std::optional<double> mapNum(const ojson& j, const char* obj, const std::string& key) {
  if (!j.contains(obj) || !j.at(obj).is_object()) return std::nullopt;
  const auto& m = j.at(obj);
  if (!m.contains(key) || !m.at(key).is_number()) return std::nullopt;
  return m.at(key).get<double>();
}

// The confidence-level word Stata stamps into a CI header: 95 -> "95".
std::string levelWord(double level) { return g9(level); }

// --- the two RD header-block geometries --------------------------------------
// rdrobust / rddensity share ONE geometry (label right-aligned in 18, " |",
// then a 10- and a 12-wide numeric column = 42 columns); rdplot's label field
// is three wider (21 + " |", still 10/12 after it = 45). The right-hand
// "Number of obs = ..." panel differs per command in both its start column and
// its value width, so it is a parameter rather than a constant.

struct TwoColGeom {
  size_t label;  // width of the right-aligned row label
  size_t c1;     // width of the "Left of c" column
  size_t c2;     // width of the "Right of c" column
};
constexpr TwoColGeom kRdGeom{18, 10, 12};      // rdrobust, rddensity
constexpr TwoColGeom kRdplotGeom{21, 10, 12};  // rdplot

std::string twoColRow(const TwoColGeom& g, const std::string& label, const std::string& a,
                      const std::string& b) {
  return padLeft(label, g.label) + " |" + padLeft(a, g.c1) + padLeft(b, g.c2);
}
std::string twoColRule(const TwoColGeom& g) {
  return std::string(g.label + 1, '-') + "+" + std::string(g.c1 + g.c2, '-');
}

// The right-hand info panel: `pad` blanks, then key padded to keyW, "=", then
// the value right-aligned in valW.
std::string infoCell(size_t pad, const std::string& key, size_t keyW, const std::string& value,
                     size_t valW) {
  return std::string(pad, ' ') + padRight(key, keyW) + "=" + padLeft(value, valW);
}

// --- block emission ----------------------------------------------------------

struct Printer {
  ojson lines = ojson::array();
  void operator()(const std::string& t) {
    lines.push_back(ojson{{"type", "display"}, {"text", t}});
  }
};

// --- e-class posting ---------------------------------------------------------
//
// BUG-163's rule, applied: a command that posts e() in Stata must post OUR
// numbers, never leave the previous model's e() reachable under its name. The
// three estimation commands (eventstudyinteract / did_imputation / sdid) build
// a real coeftable so `_b[F2]` / `_se[tau0]` / `e(N)` answer from the fit that
// just ran. rdrobust posts its 1x3 e(b)/e(V) (Conventional, Bias-corrected,
// Robust; rdrobust). rddensity is e-class too but never runs
// `ereturn post`: it posts scalars with NO e(cmd) and no coeftable, so
// `_b[...]` is "not found" rather than answering from whatever regression ran
// three lines earlier, and estimates store refuses it (runRddensity).
//
// rdplot is NOT e-class in Stata (it is r-class), so it deliberately leaves
// lastModel untouched — clearing it would be its own wrong answer. binscatter
// IS e-class (binscatter `eclass`, `ereturn post`): runBinscatter
// replaces the whole of e() itself (FU-9's binscatter half).
//
// `result` carries the engine's key names where they already match what
// macros.cpp's resolver wants (n_obs -> e(N), n_clusters -> e(N_clust), F, r2,
// adj_r2, rmse, p_F -> e(p)); anything we do not compute is simply absent, so
// e(ll) and friends answer "." instead of a fabricated number.
void postEClass(SessionState& session, const std::string& cobraCmd, const std::string& depvar,
                const std::string& dataPath, ojson result, bool ePosted = true,
                const std::string& weightVar = std::string(), const std::string& wtype = std::string()) {
  LastModel lm;
  lm.ePosted = ePosted;
  lm.fitWeight = weightVar;  // e(wtype)/e(wexp)
  lm.fitWtype = wtype;
  lm.cobraCmd = cobraCmd;
  lm.y = depvar;
  lm.depvar = depvar;
  lm.dataPath = dataPath;
  lm.result = std::move(result);
  session.setLastModel(lm);
}

ojson coefRow(const std::string& name, std::optional<double> b, std::optional<double> se,
              std::optional<double> t, std::optional<double> p, std::optional<double> lo,
              std::optional<double> hi) {
  ojson r;
  r["name"] = name;
  auto put = [&](const char* k, const std::optional<double>& v) {
    if (v.has_value()) r[k] = *v; else r[k] = nullptr;
  };
  put("b", b);
  put("se", se);
  put("t", t);
  put("p", p);
  put("ci_lo", lo);
  put("ci_hi", hi);
  return r;
}

// --- shared option plumbing --------------------------------------------------

std::optional<std::string> optStr(const eval::ParsedCommand& parsed, const std::string& name) {
  for (const auto& [k, v] : parsed.opts) {
    if (k == name && !v.isBool) return v.strValue;
  }
  return std::nullopt;
}
bool optFlag(const eval::ParsedCommand& parsed, const std::string& name) {
  for (const auto& [k, v] : parsed.opts) {
    if (k == name) return true;
  }
  return false;
}
std::vector<std::string> splitWsOrComma(const std::string& s) {
  std::vector<std::string> out;
  std::string cur;
  for (char c : s) {
    if (c == ' ' || c == '\t' || c == ',') {
      if (!cur.empty()) out.push_back(cur);
      cur.clear();
    } else {
      cur += c;
    }
  }
  if (!cur.empty()) out.push_back(cur);
  return out;
}
// ROW ORDER IS NOT THE ENGINE'S KEY ORDER. runCobraJson returns
// nlohmann::json (NOT ordered_json), whose object is a std::map — so the
// engine's `coef` object arrives SORTED BY NAME, not in the order the model
// was specified. did_imputation showed this immediately: `pre1 pre2 tau0..`
// instead of Stata's `tau0..tau3 pre1 pre2`. Every table below therefore
// builds its own row order from the REQUEST (the varlist / horizons /
// pretrends the user actually asked for), then appends any leftover key the
// order missed so a coefficient can never be silently dropped.
std::vector<std::string> orderedCoefNames(const ojson& r, const std::vector<std::string>& wanted) {
  std::vector<std::string> out;
  auto has = [&](const std::string& n) {
    return r.contains("coef") && r.at("coef").is_object() && r.at("coef").contains(n);
  };
  for (const auto& n : wanted) {
    if (has(n)) out.push_back(n);
  }
  if (r.contains("coef") && r.at("coef").is_object()) {
    for (auto it = r.at("coef").begin(); it != r.at("coef").end(); ++it) {
      bool seen = false;
      for (const auto& n : out) {
        if (n == it.key()) { seen = true; break; }
      }
      if (!seen) out.push_back(it.key());
    }
  }
  return out;
}

std::string joinComma(const std::vector<std::string>& v) {
  std::string s;
  for (size_t i = 0; i < v.size(); ++i) {
    if (i) s += ",";
    s += v[i];
  }
  return s;
}

// Forward a scalar-valued option straight through under an engine flag.
void pushOpt(std::vector<std::string>& argv, const eval::ParsedCommand& parsed,
             const std::string& stataOpt, const std::string& flag) {
  auto v = optStr(parsed, stataOpt);
  if (v.has_value() && !v->empty()) {
    argv.push_back(flag);
    argv.push_back(*v);
  }
}

// `level(#)` -> --level, honouring `set level` when the command carries none
// (the same fallback every other estimator in this codebase uses).
void pushLevel(std::vector<std::string>& argv, const eval::ParsedCommand& parsed,
               const SessionState& session) {
  auto v = optStr(parsed, "level");
  if (v.has_value() && !v->empty()) {
    argv.push_back("--level");
    argv.push_back(*v);
  } else if (session.level != 95) {
    argv.push_back("--level");
    argv.push_back(std::to_string(session.level));
  }
}

void pushKeepif(std::vector<std::string>& argv, const eval::ParsedCommand& parsed) {
  if (parsed.ifExpr.has_value()) {
    argv.push_back("--keepif");
    argv.push_back(*parsed.ifExpr);
  }
}

// One call site for "run the engine, or turn its refusal into an honest PIET
// error". The engine already refuses unsupported options BY NAME
// ("rdrobust: --kernel epanechnikov is not supported ..."); that message is
// surfaced verbatim rather than being replaced with a generic one.
struct EngineCall {
  bool ok = false;
  ojson raw;
  ojson error;
};
EngineCall callEngine(std::vector<std::string> argv) {
  EngineCall r;
  if (!piet::settings::load().provenanceHash) argv.push_back("--no-provenance");
  try {
    r.raw = runCobraJson(argv);
    r.ok = true;
  } catch (const CobraError& ce) {
    r.error = qeError(cobraErrorMessage(ce), ce.code);
  }
  return r;
}

// --- Arc G: the graph half of rdplot / binscatter ---------------------------
//
// COMPOSE, DO NOT INVENT. rdplot and binscatter are both, structurally,
// "some binned/pre-averaged points plus one overlay curve" — exactly the
// shape `twoway (scatter y x) (line y x)` already draws. So this section
// builds the SAME block shape runTwowayLayers (graph_m4.cpp) emits —
// `{"type":"graph","kind":"twoway","layers":[{"kind":"scatter",...},
// {"kind":"line",...}]}` — and hands it to buildSvgTwoway UNCHANGED; no new
// SVG-drawing code exists anywhere in this file or in graph_m4.cpp. The one
// shared piece pulled in is twoway::layerStyle (piet/twoway_opts.h), the
// exact per-layer color/marker resolver every twoway layer already uses.
//
// SHAPE MISMATCH. The engine's own plot arrays are `[{"x":.., "y":..}, ...]`
// (cmds_rd.cpp's plot_pairs / cmds_binscatter.cpp's per-bin objects) — the
// graph layer's own `points` field is positional `[[x,y], ...]`
// (buildSvgTwoway indexes `pts[i][0]`/`pts[i][1]`). The two converters below
// are the entire translation.

// `[{"x":.., "y":..}, ...]` -> `[[x,y], ...]`. A non-numeric or malformed
// entry is dropped rather than crashing or inventing a zero — the same
// never-throw discipline num()/str() above use for the table half.
ojson xyObjArrayToPoints(const ojson& arr) {
  ojson out = ojson::array();
  if (!arr.is_array()) return out;
  for (const auto& p : arr) {
    if (p.is_object() && p.contains("x") && p.contains("y") && p.at("x").is_number() &&
        p.at("y").is_number()) {
      out.push_back(ojson::array({p.at("x").get<double>(), p.at("y").get<double>()}));
    }
  }
  return out;
}

// rdplot's `bins`/`fit` keys are each `{"left":[...], "right":[...]}` —
// reach one side of one of those two groups and convert it in one call.
ojson qeSidePoints(const ojson& r, const char* group, const char* side) {
  if (!r.contains(group) || !r.at(group).is_object()) return ojson::array();
  const ojson& g = r.at(group);
  if (!g.contains(side) || !g.at(side).is_array()) return ojson::array();
  return xyObjArrayToPoints(g.at(side));
}

// One rendered twoway layer, built exactly the way layerScatter/
// layerLineOrConnected (graph_m4.cpp) build theirs — `style` comes from
// twoway::layerStyle with an EMPTY opts list (these two commands have no
// per-layer color/symbol options of their own), so a scatter layer gets a
// filled circle and a line layer gets a plain stroked path, same as any
// other twoway plot's default styling.
ojson qeGraphLayer(const std::string& kind, ojson points, int layerIdx) {
  ojson lyr;
  lyr["kind"] = kind;
  const int64_t n = static_cast<int64_t>(points.size());
  lyr["points"] = std::move(points);
  lyr["style"] = twoway::layerStyle({}, layerIdx, kind);
  lyr["n"] = n;
  return lyr;
}

// Assembles the full `{"type":"graph","kind":"twoway",...}` block, field for
// field the same as runTwowayLayers' own tail (graph_m4.cpp) — x_min/x_max/
// y_min/y_max computed from the layers actually drawn, an empty-data
// fallback to a unit range rather than an error (mirroring runTwowayLayers'
// own "Stata still draws the framed plot ... Never error" rule), and the
// same axes/legend/name/n_obs/suppressed fields every other graph command's
// block carries so `graph export`/`graph save`/`graph combine` (which read
// those fields generically) see nothing unusual about a QE graph.
ojson qeGraphBlock(const std::vector<ojson>& layers, const std::string& title,
                   const std::string& xlabel, const std::string& ylabel,
                   SessionState& session, std::optional<double> xlineAt = std::nullopt) {
  double xMin = 0, xMax = 0, yMin = 0, yMax = 0;
  bool first = true;
  int64_t nObs = 0;
  ojson layersJ = ojson::array();
  for (const auto& lyr : layers) {
    for (const auto& p : lyr.value("points", ojson::array())) {
      double x = p[0].get<double>(), y = p[1].get<double>();
      if (first) {
        xMin = xMax = x;
        yMin = yMax = y;
        first = false;
      } else {
        xMin = std::min(xMin, x);
        xMax = std::max(xMax, x);
        yMin = std::min(yMin, y);
        yMax = std::max(yMax, y);
      }
    }
    nObs += lyr.value("n", 0);
    layersJ.push_back(lyr);
  }
  if (first) {  // no plottable point in any layer
    xMin = 0.0; xMax = 1.0; yMin = 0.0; yMax = 1.0;
  }

  ojson out;
  out["type"] = "graph";
  out["kind"] = "twoway";
  out["title"] = title;
  out["subtitle"] = std::string();
  out["x_label"] = xlabel;
  out["y_label"] = ylabel;
  out["note"] = std::string();
  out["x_min"] = xMin;
  out["x_max"] = xMax;
  out["y_min"] = yMin;
  out["y_max"] = yMax;
  out["layers"] = layersJ;

  ojson axes;
  axes["xline"] = ojson::array();
  if (xlineAt.has_value()) axes["xline"].push_back(*xlineAt);
  axes["yline"] = ojson::array();
  axes["xscale_noline"] = false;
  axes["yscale_noline"] = false;
  axes["xlabel_none"] = false;
  axes["ylabel_none"] = false;
  out["axes"] = axes;

  ojson legend;
  legend["order"] = ojson::array();
  legend["cols"] = nullptr;
  legend["size"] = nullptr;
  legend["off"] = true;  // neither command has a legend() option to honour
  out["legend"] = legend;

  out["name"] = nullptr;  // rdplot/binscatter accept no name() option (yet)
  out["n_obs"] = nObs;
  out["suppressed"] = session.graphicsOff;
  return out;
}

// The dual-flow cache seam (graph_m4.cpp's L2-FIX-B / BUG-066): the FULL
// block goes into session.lastGraph verbatim (so `graph export`/`graph save`
// see every point, byte-identical to what was computed); a SEPARATE copy is
// thinned by decimateGraphBlockForView for the live console/bridge view. The
// two must never alias the same ojson, or the mutation done for the view
// would corrupt the one graph export reads back later.
ojson registerQeGraph(ojson block, SessionState& session) {
  session.lastGraph = block;      // full copy, this is what export/save read
  ojson view = block;             // independent deep copy for the live view
  decimateGraphBlockForView(view);
  return view;
}

}  // namespace

// ===========================================================================
// rdrobust y running [if], c(#) p(#) q(#) h(#) b(#) kernel() nnmatch(#) level(#)
//
// Oracle: p3_stata.log:153-175.
ojson runRdrobust(const eval::ParsedCommand& parsed, SessionState& session) {
  if (!session.currentHbin().has_value()) return ojson::array({ojson(eval::noDataLoaded())});
  if (parsed.varlist.size() != 2) {
    return qeErrorBlocks(
        "syntax_error: rdrobust requires exactly two variables — `rdrobust depvar runvar, c(#)`.",
        "syntax_error");
  }
  // Stata's rdrobust has a much wider option surface than cobra's CLI. The
  // ones cobra has no flag for are named explicitly as "not yet implemented"
  // rather than being silently dropped (a dropped bwselect() would answer a
  // DIFFERENT specification with rc 0).
  // BUG-233: the one copy of this vocabulary now lives in the file-scope
  // registration block (see top of file); this binding keeps every existing
  // use — including the reject net below — reading the same set.
  const std::unordered_set<std::string>& kKnown = kVocabRdrobust.known;
  const std::unordered_set<std::string>& kUnimplemented = kVocabRdrobust.unimplemented;
  if (auto err = eval::rejectUnknownOptions(parsed.opts, kKnown, kUnimplemented)) {
    return ojson::array({ojson(*err)});
  }

  std::vector<std::string> argv = {"rdrobust", "--y", parsed.varlist[0], "--x", parsed.varlist[1]};
  for (const auto& [opt, flag] : {std::pair<const char*, const char*>{"c", "--c"},
                                   {"p", "--p"},
                                   {"q", "--q"},
                                   {"h", "--h"},
                                   {"b", "--b"},
                                   {"kernel", "--kernel"},
                                   {"nnmatch", "--nnmatch"}}) {
    pushOpt(argv, parsed, opt, flag);
  }
  pushLevel(argv, parsed, session);
  pushKeepif(argv, parsed);
  argv.push_back("--data");
  argv.push_back(*session.currentHbin());

  EngineCall call = callEngine(argv);
  if (!call.ok) return ojson::array({call.error});
  const ojson& r = call.raw;

  Printer say;
  const bool mass = (str(r, "masspoints") == "adjust");
  if (mass) {
    say("Mass points detected in the running variable.");
    say("");
  }
  const std::string design = str(r, "design", "sharp");
  say((design == "fuzzy" ? std::string("Fuzzy") : std::string("Sharp")) +
      " RD estimates using local polynomial regression.");
  say("");

  const auto& G = kRdGeom;
  // Header row + the right-hand info panel (10 blanks are folded into the
  // 12-space gap the oracle shows between the two halves: 12 blanks, key
  // padded to 14, "=", value right-aligned in 11 => column 80).
  say(twoColRow(G, "Cutoff c = " + g9(num(r, "cutoff")), "Left of c", "Right of c") +
      infoCell(12, "Number of obs", 14, std::to_string(inum(r, "n_obs")), 11));
  say(twoColRule(G) + infoCell(12, "BW type", 14, str(r, "bw_type"), 11));
  say(twoColRow(G, "Number of obs", std::to_string(inum(r, "n_left")),
                std::to_string(inum(r, "n_right"))) +
      infoCell(12, "Kernel", 14, str(r, "kernel"), 11));
  say(twoColRow(G, "Eff. Number of obs", std::to_string(inum(r, "n_eff_left")),
                std::to_string(inum(r, "n_eff_right"))) +
      infoCell(12, "VCE method", 14, str(r, "vce"), 11));
  say(twoColRow(G, "Order est. (p)", std::to_string(inum(r, "p")), std::to_string(inum(r, "p"))));
  say(twoColRow(G, "Order bias (q)", std::to_string(inum(r, "q")), std::to_string(inum(r, "q"))));
  say(twoColRow(G, "BW est. (h)", f3(num(r, "h_left")), f3(num(r, "h_right"))));
  say(twoColRow(G, "BW bias (b)", f3(num(r, "b_left")), f3(num(r, "b_right"))));
  say(twoColRow(G, "rho (h/b)", f3(num(r, "rho")), f3(num(r, "rho"))));
  say(twoColRow(G, "Unique obs", std::to_string(inum(r, "n_unique_left")),
                std::to_string(inum(r, "n_unique_right"))));
  say("");
  say("Outcome: " + str(r, "y") + ". Running variable: " + str(r, "x") + ".");

  // The estimate table. 80 columns: label 18 + " |", an 8-wide estimate cell
  // padded out to 15, "|", then z (7) / p (13) / CI (12 + 12).
  const std::string rule80(80, '-');
  const std::string sep = std::string(19, '-') + "+" + std::string(60, '-');
  const std::string lvl = levelWord(num(r, "level", 95.0));
  say(rule80);
  say(padLeft("", 18) + " |" + padRight(" Point", 15) + "| Robust Inference");
  say(padLeft("", 18) + " |" + padRight(" Estimate", 15) + "|" + padLeft("z-stat", 7) +
      padLeft("P>|z|", 13) + padLeft("[" + lvl + "% Conf. Interval]", 24));
  say(sep);
  say(padLeft("RD Effect", 18) + " |" + padRight(padLeft(g9(num(r, "effect")), 8), 15) + "|" +
      padLeft(f4(num(r, "z")), 7) + padLeft(f3(num(r, "pvalue")), 13) +
      padLeft(g9(num(r, "ci_lo")), 12) + padLeft(g9(num(r, "ci_hi")), 12));
  say(rule80);
  if (mass) say("Estimates adjusted for mass points in the running variable.");

  // e15d_list_eb (w2g_ebmat.log:2302-2312): rdrobust.ado posts
  // `ereturn post b V, esample(touse)` with b = (tau_cl, tau_bc,
  // tau_bc) striped `y1:Conventional Bias-corrected Robust` and a
  // diagonal V whose first two cells are V_tau_cl ("Bias-corrected: SE =
  // sqrt(V_tau_cl)", :1015) and the last V_tau_rb. The engine's three rows are
  // exactly those (cmds_rd.cpp: se_bc = se_conventional), so they are posted
  // as e(b)/e(V) and as the coefficient table that answers _b[]/_se[].
  ojson e;
  eval::EvalMatrix eb, eV;
  eb.rownames = {"y1"};
  eb.data.emplace_back();
  e["coeftable"] = ojson::array();
  std::vector<double> ses;
  if (r.contains("rows") && r.at("rows").is_array()) {
    for (const auto& row : r.at("rows")) {
      const std::string name = str(row, "method");
      eb.colnames.push_back(name);
      eb.data[0].push_back(num(row, "coef"));
      ses.push_back(num(row, "se"));
      e["coeftable"].push_back(coefRow(name, num(row, "coef"), num(row, "se"), num(row, "z"),
                                       num(row, "p"), num(row, "ci_lo"), num(row, "ci_hi")));
    }
  }
  eV.rownames = eV.colnames = eb.colnames;
  eV.data.assign(ses.size(), std::vector<double>(ses.size(), 0.0));
  for (size_t i = 0; i < ses.size(); ++i) eV.data[i][i] = ses[i] * ses[i];
  e["estimator"] = "rdrobust";
  e["depvar"] = str(r, "y");
  e["n_obs"] = inum(r, "n_obs");
  e["tau_cl"] = num(r, "effect");
  e["se_tau_rb"] = num(r, "se");
  e["pv_rb"] = num(r, "pvalue");
  e["ci_l_rb"] = num(r, "ci_lo");
  e["ci_r_rb"] = num(r, "ci_hi");
  e["h_l"] = num(r, "h_left");
  e["h_r"] = num(r, "h_right");
  e["b_l"] = num(r, "b_left");
  e["b_r"] = num(r, "b_right");
  e["N_h_l"] = inum(r, "n_eff_left");
  e["N_h_r"] = inum(r, "n_eff_right");
  postEClass(session, "rdrobust", str(r, "y"), *session.currentHbin(), e);
  // After the fresh setLastModel (which clears the previous fit's block and
  // e(b)/e(V)), as every fit that has an e(b) does.
  if (!eb.colnames.empty()) {
    session.lastEstBlock = e;
    session.eval.eMatrices.emplace_back("b", std::move(eb));
    session.eval.eMatrices.emplace_back("V", std::move(eV));
  }

  ojson out = r;
  out["type"] = "qe_result";
  say.lines.push_back(out);
  return say.lines;
}

// ===========================================================================
// rddensity runvar [if], c(#) p(#) q(#) h(#) kernel()
//
// Oracle: p3_stata.log:186-222. Note the manipulation test takes ONLY the
// running variable — there is no depvar.
ojson runRddensity(const eval::ParsedCommand& parsed, SessionState& session) {
  if (!session.currentHbin().has_value()) return ojson::array({ojson(eval::noDataLoaded())});
  if (parsed.varlist.size() != 1) {
    return qeErrorBlocks(
        "syntax_error: rddensity takes exactly one variable (the running variable) — "
        "`rddensity runvar, c(#)`.",
        "syntax_error");
  }
  // BUG-233: the one copy of this vocabulary now lives in the file-scope
  // registration block (see top of file); this binding keeps every existing
  // use — including the reject net below — reading the same set.
  const std::unordered_set<std::string>& kKnown = kVocabRddensity.known;
  const std::unordered_set<std::string>& kUnimplemented = kVocabRddensity.unimplemented;
  if (auto err = eval::rejectUnknownOptions(parsed.opts, kKnown, kUnimplemented)) {
    return ojson::array({ojson(*err)});
  }

  // rddensity.ado's own guards, run before any fit and leaving e() untouched
  // (:94-105): over the if/in sample with the running variable non-missing,
  // a cutoff not strictly inside (min, max) is r(125), and fewer than 10 rows
  // on either side (x < c, x >= c) is r(2001) (w2v_estpost_ematrix.log
  // ep15d_rddensity `2001` on the 20-row fixture with c(.5); the earlier
  // regress e(b) then survives, ep15d_b_shape).
  {
    double c = 0.0;
    bool cOk = true;
    for (const auto& [k, v] : parsed.opts) {
      if (k != "c" || v.isBool) continue;
      const std::string t = strutil::trim(v.strValue);
      char* end = nullptr;
      c = std::strtod(t.c_str(), &end);
      cOk = !t.empty() && end != nullptr && *end == '\0';
    }
    if (cOk) {
      try {
        const InRangeFilterResult fr = applyIfAndInFilter(session, *session.currentHbin(),
                                                          parsed.ifExpr, parsed.inRange);
        // No if/in: the live head (possibly a resident frame not on disk);
        // otherwise the filtered scratch copy applyIfAndInFilter wrote.
        std::unique_ptr<Datastore> owned;
        const Datastore* dsp = nullptr;
        if (fr.ok && fr.dataPath == *session.currentHbin()) {
          dsp = session.datastore();
        } else if (fr.ok) {
          owned = std::make_unique<Datastore>(fr.dataPath);
          dsp = owned.get();
        }
        if (dsp != nullptr) {
          const Datastore& ds = *dsp;
          const std::string& xn = parsed.varlist[0];
          if (ds.has(xn) && ds.col(xn).dtype == cecon::DType::F64 && ds.col(xn).f64() != nullptr) {
            const double* xv = ds.col(xn).f64();
            double lo = std::numeric_limits<double>::infinity(), hi = -lo;
            long long nl = 0, nr = 0;
            for (int64_t i = 0; i < ds.nRows(); ++i) {
              if (std::isnan(xv[i])) continue;
              lo = std::min(lo, xv[i]);
              hi = std::max(hi, xv[i]);
              if (xv[i] < c) ++nl;
              else ++nr;
            }
            if (nl + nr > 0) {
              if (c <= lo || c >= hi)
                return ojson::array({eval::errorBlock(
                    "syntax_error", "c() should be set within the range of " + xn + ".",
                    std::nullopt, std::nullopt, std::nullopt, "r(125)")});
              if (nl < 10 || nr < 10)
                return ojson::array({eval::errorBlock(
                    "syntax_error", "Not enough observations to perform calculations.",
                    std::nullopt, std::nullopt, std::nullopt, "r(2001)")});
            }
          }
        }
      } catch (const std::exception&) {
        // An unreadable sample leaves the engine's own checks to answer.
      }
    }
  }

  std::vector<std::string> argv = {"rddensity", "--x", parsed.varlist[0]};
  for (const auto& [opt, flag] : {std::pair<const char*, const char*>{"c", "--c"},
                                   {"p", "--p"},
                                   {"q", "--q"},
                                   {"h", "--h"},
                                   {"kernel", "--kernel"}}) {
    pushOpt(argv, parsed, opt, flag);
  }
  pushKeepif(argv, parsed);
  argv.push_back("--data");
  argv.push_back(*session.currentHbin());

  EngineCall call = callEngine(argv);
  if (!call.ok) return ojson::array({call.error});
  const ojson& r = call.raw;

  Printer say;
  say("Computing data-driven bandwidth selectors.");
  say("");
  if (str(r, "masspoints") == "adjust") {
    say("Point estimates and standard errors have been adjusted for repeated observations.");
    say("(Use option nomasspoints to suppress this adjustment.)");
    say("");
  }
  say("RD Manipulation test using local polynomial density estimation.");
  say("");

  const auto& G = kRdGeom;
  // rddensity's cutoff label is `c = ` + a %.3f value in its own 10-wide cell,
  // not the trimmed spelling rdrobust uses.
  const std::string cutoffLabel = padLeft("c =", 8) + padLeft(f3(num(r, "cutoff")), 10);
  say(cutoffLabel + " |" + padLeft("Left of c", G.c1) + padLeft("Right of c", G.c2) +
      infoCell(10, "Number of obs", 14, std::to_string(inum(r, "n_obs")), 13));
  say(twoColRule(G) + infoCell(10, "Model", 14, str(r, "model"), 13));
  say(twoColRow(G, "Number of obs", std::to_string(inum(r, "n_left")),
                std::to_string(inum(r, "n_right"))) +
      infoCell(10, "BW method", 14, str(r, "bw_method"), 13));
  say(twoColRow(G, "Eff. Number of obs", std::to_string(inum(r, "n_eff_left")),
                std::to_string(inum(r, "n_eff_right"))) +
      infoCell(10, "Kernel", 14, str(r, "kernel"), 13));
  say(twoColRow(G, "Order est. (p)", std::to_string(inum(r, "p")), std::to_string(inum(r, "p"))) +
      infoCell(10, "VCE method", 14, str(r, "vce"), 13));
  say(twoColRow(G, "Order bias (q)", std::to_string(inum(r, "q")), std::to_string(inum(r, "q"))));
  say(twoColRow(G, "BW est. (h)", f3(num(r, "h_left")), f3(num(r, "h_right"))));
  say("");
  say("Running variable: " + str(r, "x") + ".");

  const std::string rule42(42, '-');
  say(rule42);
  // Stata's own header spacing here is not column-aligned with the values
  // below it (the "T" sits three columns left of where the value ends); the
  // oracle is the spec, so the header is reproduced literally.
  say(padLeft("Method", 18) + " |" + "      T          P>|T|");
  say(twoColRule(G));
  say(twoColRow(G, "Robust", f4(num(r, "T")), f4(num(r, "pvalue"))));
  say(rule42);
  say("");

  say("P-values of binomial tests. (H0: prob = .5)");
  const std::string rule53(53, '-');
  say(rule53);
  say(padLeft("Window Length / 2", 18) + " |" + padLeft("<c", 9) + padLeft(">=c", 12) + " |" +
      padLeft("P>|T|", 10));
  say(std::string(19, '-') + "+" + std::string(22, '-') + "+" + std::string(10, '-'));
  if (r.contains("binomial") && r.at("binomial").is_array()) {
    for (const auto& row : r.at("binomial")) {
      say(padLeft(f3(num(row, "window")), 18) + " |" +
          padLeft(std::to_string(inum(row, "n_left")), 9) +
          padLeft(std::to_string(inum(row, "n_right")), 12) + " |" +
          padLeft(f4(num(row, "p")), 10));
    }
  }
  say(rule53);

  // e15d_eststo2/e15d_esttab2 (w2g_ebmat.log:2497-2513): rddensity is
  // eclass but never runs `ereturn post`: `ereturn clear`, then scalars and
  // the macros runningvar/kernel/bwmethod/vce (rddensity) -- no
  // e(cmd), e(depvar) or e(N). The e() stays live (`_b[x1]` and `matrix list
  // e(b)` are r(111), log:2486-2495), but it is not an estimation result:
  // `estimates store` exits 301 on the empty e(cmd),
  // and eststo's "." fill still reaches the hold step,
  // which refuses an e() that was never posted. So
  // the model is marked not storable (runEststo) and e(cmd)/e(depvar) are
  // erased (an empty depvar erases; setEString("") erases).
  ojson e;
  e["N_l"] = inum(r, "n_left");
  e["N_r"] = inum(r, "n_right");
  e["c"] = num(r, "cutoff");
  e["h_l"] = num(r, "h_left");
  e["h_r"] = num(r, "h_right");
  postEClass(session, "rddensity", "", *session.currentHbin(), e, /*ePosted=*/false);
  session.eval.setEString("cmd", "");

  ojson out = r;
  out["type"] = "qe_result";
  say.lines.push_back(out);
  return say.lines;
}

// ===========================================================================
// rdplot y running [if], c(#) p(#) h(#) nbins(# #) kernel()
//
// Oracle: p3_stata.log:233-261 (note the ONE trailing blank line rdplot prints
// after its rule — every other command here prints none).
//
// SCOPE: table only. The JSON's `bins`/`fit`/`fit_coef` arrays are the plot,
// and drawing them is a sibling arc's GRAPH half; they ride out on the
// structured block untouched so that arc has them.
ojson runRdplot(const eval::ParsedCommand& parsed, SessionState& session) {
  if (!session.currentHbin().has_value()) return ojson::array({ojson(eval::noDataLoaded())});
  if (parsed.varlist.size() != 2) {
    return qeErrorBlocks(
        "syntax_error: rdplot requires exactly two variables — `rdplot depvar runvar, c(#)`.",
        "syntax_error");
  }
  // BUG-233: the one copy of this vocabulary now lives in the file-scope
  // registration block (see top of file); this binding keeps every existing
  // use — including the reject net below — reading the same set.
  const std::unordered_set<std::string>& kKnown = kVocabRdplot.known;
  const std::unordered_set<std::string>& kUnimplemented = kVocabRdplot.unimplemented;
  if (auto err = eval::rejectUnknownOptions(parsed.opts, kKnown, kUnimplemented)) {
    return ojson::array({ojson(*err)});
  }

  std::vector<std::string> argv = {"rdplot", "--y", parsed.varlist[0], "--x", parsed.varlist[1]};
  for (const auto& [opt, flag] : {std::pair<const char*, const char*>{"c", "--c"},
                                   {"p", "--p"},
                                   {"h", "--h"},
                                   {"kernel", "--kernel"}}) {
    pushOpt(argv, parsed, opt, flag);
  }
  // Stata writes `nbins(# #)` space-separated; the engine wants `L,R`.
  if (auto nb = optStr(parsed, "nbins"); nb.has_value() && !nb->empty()) {
    argv.push_back("--nbins");
    argv.push_back(joinComma(splitWsOrComma(*nb)));
  }
  pushKeepif(argv, parsed);
  argv.push_back("--data");
  argv.push_back(*session.currentHbin());

  EngineCall call = callEngine(argv);
  if (!call.ok) return ojson::array({call.error});
  const ojson& r = call.raw;

  Printer say;
  if (str(r, "masspoints") == "adjust") {
    say("Mass points detected in the running variable.");
    say("");
  }
  // Only the `esmv` phrasing is oracle-verified (p3_stata.log:235); the manual
  // form is the plain evenly-spaced wording rdplot uses when the bin count is
  // given rather than selected.
  const std::string binselect = str(r, "binselect", "esmv");
  std::string what = "evenly spaced mimicking variance number of bins";
  if (binselect == "manual" || binselect == "es") what = "evenly spaced number of bins";
  say("RD Plot with " + what + " using polynomial regression.");
  say("");

  const auto& G = kRdplotGeom;
  say(twoColRow(G, "Cutoff c = " + g9(num(r, "cutoff")), "Left of c", "Right of c") +
      infoCell(8, "Number of obs", 15, std::to_string(inum(r, "n_obs")), 11));
  say(twoColRule(G) + infoCell(8, "Kernel", 15, str(r, "kernel"), 11));
  say(twoColRow(G, "Number of obs", std::to_string(inum(r, "n_left")),
                std::to_string(inum(r, "n_right"))));
  say(twoColRow(G, "Eff. Number of obs", std::to_string(inum(r, "n_eff_left")),
                std::to_string(inum(r, "n_eff_right"))));
  say(twoColRow(G, "Order poly. fit (p)", std::to_string(inum(r, "p_left")),
                std::to_string(inum(r, "p_right"))));
  say(twoColRow(G, "BW poly. fit (h)", f3(num(r, "h_left")), f3(num(r, "h_right"))));
  say(twoColRow(G, "Number of bins scale", f3(num(r, "scale_left")), f3(num(r, "scale_right"))));
  say("");
  say("Outcome: " + str(r, "y") + ". Running variable: " + str(r, "x") + ".");

  const std::string rule45(45, '-');
  say(rule45);
  say(twoColRow(G, "", "Left of c", "Right of c"));
  say(twoColRule(G));
  say(twoColRow(G, "Bins selected", std::to_string(inum(r, "bins_left")),
                std::to_string(inum(r, "bins_right"))));
  say(twoColRow(G, "Average bin length", f3(num(r, "avg_bin_length_left")),
                f3(num(r, "avg_bin_length_right"))));
  say(twoColRow(G, "Median bin length", f3(num(r, "med_bin_length_left")),
                f3(num(r, "med_bin_length_right"))));
  say(twoColRule(G));
  say(twoColRow(G, "IMSE-optimal bins", std::to_string(inum(r, "bins_imse_left")),
                std::to_string(inum(r, "bins_imse_right"))));
  say(twoColRow(G, "Mimicking Var. bins", std::to_string(inum(r, "bins_mv_left")),
                std::to_string(inum(r, "bins_mv_right"))));
  say(twoColRule(G));
  // Stata prints this section label with a trailing space after the pipe.
  say(padLeft("Rel. to IMSE-optimal:", G.label) + " | ");
  say(twoColRow(G, "Implied scale", f3(num(r, "implied_scale_left")),
                f3(num(r, "implied_scale_right"))));
  say(twoColRow(G, "WIMSE var. weight", f3(num(r, "wimse_var_weight")),
                f3(num(r, "wimse_var_weight"))));
  say(twoColRow(G, "WIMSE bias weight", f3(num(r, "wimse_bias_weight")),
                f3(num(r, "wimse_bias_weight"))));
  say(rule45);
  say("");  // rdplot's own trailing blank line (oracle line 261)

  // rdplot is r-class in Stata, not e-class: lastModel is deliberately left
  // alone so a preceding regression's e() keeps answering, exactly as it would
  // in Stata.

  // Arc G: the picture. Four layers — one binned-scatter + one fitted-curve
  // per side of the cutoff — built from the SAME bins/fit arrays the table
  // above already read (no second engine call, no recomputation). The two
  // sides never share a layer, so there is no line connecting the last left
  // bin to the first right bin — that IS the "visually separated at the
  // cutoff" requirement, for free, just from drawing two independent
  // layers rather than one continuous one. layerIdx 0/1 also gives each
  // side its own palette color (scatter and its own fit curve share a
  // color), so the two sides read as two series even before the cutoff
  // line is looked at.
  std::vector<ojson> rdplotLayers = {
      qeGraphLayer("scatter", qeSidePoints(r, "bins", "left"), 0),
      qeGraphLayer("line", qeSidePoints(r, "fit", "left"), 0),
      qeGraphLayer("scatter", qeSidePoints(r, "bins", "right"), 1),
      qeGraphLayer("line", qeSidePoints(r, "fit", "right"), 1),
  };
  ojson rdplotGraph = qeGraphBlock(rdplotLayers, std::string(), str(r, "x"), str(r, "y"), session,
                                   num(r, "cutoff"));
  say.lines.push_back(registerQeGraph(std::move(rdplotGraph), session));

  ojson out = r;
  out["type"] = "qe_result";
  say.lines.push_back(out);
  return say.lines;
}

// ===========================================================================
// binscatter yvars xvar [if] [in] [aw|fw], by() nquantiles() discrete medians
//            controls() absorb() noaddmean linetype() reportreg colors()
//            mcolors() lcolors() msymbols() savedata() replace [twoway options]
//
// Michael Stepner's binscatter 7.02 as StataBE 19.5 runs it. The installed ado
// (~/Library/Application Support/Stata/ado/plus/b/binscatter.ado, "ado:N"
// below) is the text; the oracle is tests/eval/golden/aer_w2/w2k_binscatter.log
// (the BS_* arms). The engine (cobra binscatter, apps/cmds/cmds_binscatter.cpp)
// computes every number: the sample, the residuals, the bins, the bin points
// and the fit lines. This handler owns what Stata builds as TEXT from them:
//   * the option checks the ado makes before it computes anything (ado),
//     three of which print an error and `exit` with rc 0, posting nothing
//     (BS_lt_bad, BS_reportreg_none, BS_disc_nq);
//   * reportreg's regress tables (ado), named the way Stata names the
//     residualized tempvars;
//   * the graph command e(graphcmd) (ado): scatteri points and function
//     lines whose numbers are macro text (`local v = M[i,j]`, %18.0g), colors,
//     legend, and the options binscatter does not know appended as typed;
//   * savedata's f.csv and f.do (ado) and r(602) without replace, which
//     is checked before anything is computed (ado);
//   * e(): `ereturn post` replaces the previous estimation results wholesale,
//     then e(N), e(graphcmd), e(y#_coefs) and e(byvalues) (ado).

namespace {

// binscatter's syntax line: each option's full name and the length
// of its capitalised stem (the minimum abbreviation). Lower-case declarations
// must be typed in full.
struct BsOpt {
  const char* name;
  size_t minLen;
};
constexpr BsOpt kBsSyntax[] = {
    {"by", 2},          {"nquantiles", 1}, {"genxq", 3},     {"discrete", 8},
    {"xq", 2},          {"medians", 3},    {"controls", 7},  {"absorb", 6},
    {"noaddmean", 3},   {"linetype", 4},   {"rd", 2},        {"reportreg", 9},
    {"colors", 3},      {"mcolors", 2},    {"lcolors", 2},   {"msymbols", 1},
    {"savegraph", 9},   {"savedata", 8},   {"replace", 7},   {"nofastxtile", 11},
    {"randvar", 7},     {"randcut", 7},    {"randn", 5},     {"nbins", 5},
    {"create_xq", 9},   {"x_q", 3},        {"symbols", 7},   {"method", 6},
    {"unique", 6},
};

// The options this build computes. Every other binscatter option is refused by
// name (rc 198) before anything runs: each changes which bins or which fit
// Stata draws (xq, rd, the fastxtile sampling options) or writes a file
// (savegraph), so accepting one and ignoring it would be a wrong answer.
const std::unordered_set<std::string> kBsServed = {
    "by",        "nquantiles", "genxq",  "discrete", "medians", "controls", "absorb",
    "noaddmean", "linetype",   "reportreg", "colors", "mcolors", "lcolors", "msymbols",
    "savedata",  "replace"};

// One option as typed after the comma: its name, its argument (the text inside
// the outer parentheses, or nullopt for a bare word) and the whole token.
struct RawOpt {
  std::string name;
  std::optional<std::string> arg;
  std::string text;
};

// The option tail without a trailing `, if ...` / `, in ...` qualifier. The
// tokenizer has already moved that qualifier into parsed.ifExpr/inRange
// (B4/W1, tokenizer.cpp findTrailingQualifierComma) but parsed.raw still holds
// it, and left in the tail its words would reach the catch-all and be appended
// to e(graphcmd) (`binscatter price mpg, nq(10), if foreign==0`, the shape of
// 034/DoFiles_Sept2025/Figure1_Replication.do:41). Same rule as the tokenizer:
// a top-level comma, blanks, `if` or `in`, a blank and a non-blank tail; text
// after the qualifier's own next top-level comma goes back onto the options.
std::string bsDropTrailingQualifier(const std::string& post) {
  size_t base = 0;
  while (base < post.size()) {
    const long long rel = eval::findUnquotedComma(post.substr(base));
    if (rel < 0) return post;
    const size_t at = base + static_cast<size_t>(rel);
    size_t j = at + 1;
    while (j < post.size() && std::isspace(static_cast<unsigned char>(post[j]))) ++j;
    if (j + 2 < post.size() && post[j] == 'i' && (post[j + 1] == 'f' || post[j + 1] == 'n') &&
        std::isspace(static_cast<unsigned char>(post[j + 2])) && !strutil::trim(post.substr(j + 2)).empty()) {
      std::string out = post.substr(0, at);
      const std::string qual = post.substr(at + 1);
      const long long more = eval::findUnquotedComma(qual);
      if (more >= 0) out += " " + qual.substr(static_cast<size_t>(more) + 1);
      return out;
    }
    base = at + 1;
  }
  return post;
}

// Splits the option tail on blanks outside parentheses and quotes (plain "..."
// and compound `"..."'), as Stata's `syntax` sees the options.
std::vector<RawOpt> bsRawOptions(const std::string& raw) {
  std::vector<RawOpt> out;
  const long long comma = eval::findUnquotedComma(raw);
  if (comma < 0) return out;
  const std::string tail = bsDropTrailingQualifier(raw.substr(static_cast<size_t>(comma) + 1));
  size_t i = 0;
  const size_t n = tail.size();
  while (i < n) {
    while (i < n && std::isspace(static_cast<unsigned char>(tail[i]))) ++i;
    if (i >= n) break;
    const size_t start = i;
    int depth = 0;
    int compound = 0;
    bool inQuote = false;
    while (i < n) {
      const char c = tail[i];
      if (c == '`' && i + 1 < n && tail[i + 1] == '"') {
        ++compound;
        i += 2;
        continue;
      }
      if (compound > 0 && c == '"' && i + 1 < n && tail[i + 1] == '\'') {
        --compound;
        i += 2;
        continue;
      }
      if (compound == 0 && c == '"') {
        inQuote = !inQuote;
        ++i;
        continue;
      }
      if (!inQuote && compound == 0) {
        if (c == '(') ++depth;
        else if (c == ')') --depth;
        else if (depth <= 0 && std::isspace(static_cast<unsigned char>(c))) break;
      }
      ++i;
    }
    RawOpt o;
    o.text = tail.substr(start, i - start);
    const size_t open = o.text.find('(');
    if (open == std::string::npos) {
      o.name = o.text;
    } else {
      o.name = o.text.substr(0, open);
      const size_t close = o.text.rfind(')');
      o.arg = close != std::string::npos && close > open ? o.text.substr(open + 1, close - open - 1)
                                                         : o.text.substr(open + 1);
    }
    for (auto& ch : o.name) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    out.push_back(std::move(o));
  }
  return out;
}

// The binscatter option a typed name abbreviates, or "" (it goes to the
// catch-all `*`, i.e. to twoway, ado).
std::string bsMatch(const std::string& typed) {
  for (const auto& o : kBsSyntax) {
    const std::string full = o.name;
    if (typed.size() >= o.minLen && typed.size() <= full.size() && full.compare(0, typed.size(), typed) == 0)
      return full;
  }
  return std::string();
}

// `string` options drop one pair of outer double quotes (or a compound pair).
std::string bsDequote(std::string v) {
  v = strutil::trim(v);
  if (v.size() >= 2 && v.front() == '"' && v.back() == '"') return v.substr(1, v.size() - 2);
  if (v.size() >= 4 && v.rfind("`\"", 0) == 0 && v.compare(v.size() - 2, 2, "\"'") == 0)
    return v.substr(2, v.size() - 4);
  return v;
}

std::vector<std::string> bsWords(const std::string& s) {
  std::vector<std::string> w;
  std::string cur;
  for (char c : s) {
    if (std::isspace(static_cast<unsigned char>(c))) {
      if (!cur.empty()) w.push_back(cur);
      cur.clear();
    } else {
      cur.push_back(c);
    }
  }
  if (!cur.empty()) w.push_back(cur);
  return w;
}

// `: word k of LIST`, empty past the end (as Stata's).
std::string bsWord(const std::vector<std::string>& w, size_t k) {
  return k >= 1 && k <= w.size() ? w[k - 1] : std::string();
}

// The number text a `local v = <exp>` stores (%18.0g, the macro layer's rule).
std::string bsNum(double v) {
  if (std::isnan(v)) return ".";
  return eval::stataMacroRealText(v);
}

// `file write (exp)`'s number text: %10.0g with the blanks removed, `.` missing.
std::string bsFileNum(double v) {
  if (std::isnan(v)) return ".";
  return piet::fmt::general(v, 10, 0);
}

// An error block with Stata's rc.
ojson bsRcError(const std::string& message, int rc, const std::string& code = "engine_error") {
  return ojson(eval::errorBlock(code, message, std::nullopt, std::nullopt, std::nullopt,
                                "r(" + std::to_string(rc) + ")"));
}

ojson bsText(const std::string& t) { return ojson{{"type", "display"}, {"text", t}}; }

// Stata's regress name column: 12 wide, right-aligned, a long name abbreviated
// with `~` (the regress family's rule, estimation_m4b.cpp coefStubM4b).
std::string bsStub(const std::string& name) {
  return padLeft(abbrevName(name, 12), 12);
}

// One reportreg regression, as `regress` prints it (log 991-1014 for the
// layout; the numbers are the engine's).
void bsRegressTable(const ojson& reg, const std::string& depName, const std::vector<std::string>& xNames,
                    ojson& out) {
  const double N = num(reg, "N");
  const double dfm = num(reg, "df_m"), dfr = num(reg, "df_r");
  const double mss = num(reg, "mss"), rss = num(reg, "rss"), tss = num(reg, "tss");
  auto ss = [](double v) { return piet::fmt::general(v, 11, 0); };
  auto key = [](const std::string& k, const std::string& v) { return "   " + padRight(k, 16) + "=" + padLeft(v, 10); };
  auto optF = [&](const char* k, int dec) -> std::string {
    if (!reg.contains(k) || !reg.at(k).is_number()) return ".";
    return fixedN(reg.at(k).get<double>(), dec);
  };
  char fbuf[64];
  std::snprintf(fbuf, sizeof fbuf, "F(%lld, %lld)", static_cast<long long>(dfm), static_cast<long long>(dfr));
  const std::string rmse = reg.contains("rmse") && reg.at("rmse").is_number()
                               ? piet::fmt::general(reg.at("rmse").get<double>(), 10, 5)
                               : std::string(".");
  out.push_back(bsText(""));
  out.push_back(bsText("      Source |       SS           df       MS      Number of obs   =" +
                       padLeft(commaGroup(static_cast<int64_t>(N)), 10)));
  out.push_back(bsText("-------------+----------------------------------" + key(fbuf, optF("F", 2))));
  out.push_back(bsText("       Model |" + padLeft(ss(mss), 12) + padLeft(commaGroup(static_cast<int64_t>(dfm)), 10) +
                       padLeft(ss(dfm > 0 ? mss / dfm : std::nan("")), 12) + key("Prob > F", optF("p_F", 4))));
  out.push_back(bsText("    Residual |" + padLeft(ss(rss), 12) + padLeft(commaGroup(static_cast<int64_t>(dfr)), 10) +
                       padLeft(ss(dfr > 0 ? rss / dfr : std::nan("")), 12) + key("R-squared", optF("r2", 4))));
  out.push_back(bsText("-------------+----------------------------------" + key("Adj R-squared", optF("r2_a", 4))));
  out.push_back(bsText("       Total |" + padLeft(ss(tss), 12) + padLeft(commaGroup(static_cast<int64_t>(N - 1)), 10) +
                       padLeft(ss(N > 1 ? tss / (N - 1) : std::nan("")), 12) + key("Root MSE", rmse)));
  out.push_back(bsText(""));
  out.push_back(bsText(std::string(78, '-')));
  out.push_back(bsText(bsStub(depName) + " |" + coefHeader("Std. err.", "t", "95")));
  out.push_back(bsText(std::string(13, '-') + "+" + std::string(64, '-')));
  const ojson coefs = reg.value("coefs", ojson::array());
  for (size_t j = 0; j < coefs.size(); ++j) {
    const ojson& c = coefs[j];
    const std::string nm = j < xNames.size() ? xNames[j] : std::string("_cons");
    if (c.value("omitted", false)) {
      out.push_back(bsText(bsStub("o." + nm) + " |" + padLeft("0", 11) + "  (omitted)"));
      continue;
    }
    auto cell = [&](const char* k) {
      return c.contains(k) && c.at(k).is_number() ? g9cell(c.at(k).get<double>()) : std::string(".");
    };
    auto fx = [&](const char* k, int d) {
      return c.contains(k) && c.at(k).is_number() ? fixedN(c.at(k).get<double>(), d) : std::string(".");
    };
    out.push_back(bsText(bsStub(nm) + " |" + padLeft(cell("b"), 11) + padLeft(cell("se"), 11) +
                         padLeft(fx("t", 2), 9) + padLeft(fx("p", 3), 8) + padLeft(cell("lo"), 13) +
                         padLeft(cell("hi"), 12)));
  }
  out.push_back(bsText(std::string(78, '-')));
}

// FU-1088: by(varname), absorb(varname) and CONTROLs(varlist numeric ts fv)
// (binscatter) are resolved by Stata's `syntax` before the ado runs,
// so a unique prefix stands for its variable, a wildcard or range expands and
// several prefix matches are r(111). The rule is THE one the by/bysort keys
// use (handlers::resolveByKeyNames); a factor-variable term keeps its
// operator (`i.`, `c.`, `ib2.`, `L.`) and its `#`/`##` joins, and only the
// name after the operator resolves (several names expand term by term, as
// fvexpand does). A parenthesised group is one term even when it holds
// spaces, and `op.(a b)` expands to `op.a op.b` as fvexpand does (each name
// resolved); a lag list `L(1/2).x` keeps its operator and resolves `x`.
// DESIGN DECISIONS (no oracle arm): a name that matches no variable is
// r(111) "variable <name> not found"; unbalanced parentheses are r(198)
// "invalid syntax"; a varname option that expands to several variables is
// r(103) "too many variables specified".
std::optional<ojson> bsResolveNames(SessionState& session, std::string& text, bool single) {
  std::vector<std::string> tokens;
  {
    std::string cur;
    int depth = 0;
    for (char c : text) {
      if (c == '(') ++depth;
      if (c == ')' && --depth < 0) return bsRcError("invalid syntax", 198);
      if (depth == 0 && (c == ' ' || c == '\t' || c == ',')) {
        if (!cur.empty()) tokens.push_back(cur);
        cur.clear();
      } else {
        cur += c;
      }
    }
    if (depth != 0) return bsRcError("invalid syntax", 198);
    if (!cur.empty()) tokens.push_back(cur);
  }
  if (tokens.empty()) return std::nullopt;
  Datastore* ds = session.datastore();
  std::vector<std::string> outTerms;
  size_t nameCount = 0;
  auto resolveOne = [&](const std::string& name, std::vector<std::string>& got) -> std::optional<ojson> {
    std::vector<std::string> names = {name};
    if (std::optional<ojson> err = resolveByKeyNames(session, names)) return err;
    for (const auto& n : names) {
      if (ds != nullptr && !ds->has(n)) return bsRcError("variable " + n + " not found", 111);
    }
    got = names;
    return std::nullopt;
  };
  for (const auto& tok : tokens) {
    if (tok.find_first_of(".#(") == std::string::npos) {
      std::vector<std::string> got;
      if (std::optional<ojson> err = resolveOne(tok, got)) return err;
      nameCount += got.size();
      outTerms.insert(outTerms.end(), got.begin(), got.end());
      continue;
    }
    // A factor-variable term: parts joined by `#` or `##` outside parentheses
    // (a `#` inside a group belongs to that group's element, BUG-1464).
    std::vector<std::string> combos = {std::string()};
    size_t pos = 0;
    while (pos <= tok.size()) {
      size_t hash = tok.size();
      for (size_t k = pos, d = 0; k < tok.size(); ++k) {
        if (tok[k] == '(') ++d;
        else if (tok[k] == ')' && d > 0) --d;
        else if (tok[k] == '#' && d == 0) {
          hash = k;
          break;
        }
      }
      size_t next = hash;
      while (next < tok.size() && tok[next] == '#') ++next;
      const std::string part = tok.substr(pos, hash - pos);
      const std::string sep = tok.substr(hash, next - hash);
      std::vector<std::string> got;
      const size_t dotOpen = part.rfind(".(");
      const size_t open = !part.empty() && part[0] == '(' ? 0
                          : dotOpen == std::string::npos  ? std::string::npos
                                                          : dotOpen + 1;
      if (open != std::string::npos && part.back() == ')') {
        // `op.(a b)` / `(a b)`: every name of the group under the operator.
        const std::string op = part.substr(0, open);
        for (const std::string& inner : splitWsOrComma(part.substr(open + 1, part.size() - open - 2))) {
          if (inner.find('#') != std::string::npos) {
            // An interaction in the group keeps its own operators and is not
            // under the group's: Stata reads `c.(a#b z)` as `a#b c.z` (StataBE
            // 19.5, BUG-1464: regress colnames 0b.a#0b.b ... 4.a#6.b z).
            std::string sub = inner;
            if (std::optional<ojson> err = bsResolveNames(session, sub, false)) return err;
            got.push_back(sub);
            continue;
          }
          const size_t d = inner.rfind('.');
          const std::string iop = d == std::string::npos ? std::string() : inner.substr(0, d + 1);
          std::vector<std::string> names;
          if (std::optional<ojson> err = resolveOne(inner.substr(d == std::string::npos ? 0 : d + 1), names))
            return err;
          for (const auto& n : names) got.push_back(op + iop + n);
        }
        if (got.empty()) return bsRcError("invalid syntax", 198);
      } else {
        const size_t dot = part.rfind('.');
        const std::string op = dot == std::string::npos ? std::string() : part.substr(0, dot + 1);
        const std::string nm = dot == std::string::npos ? part : part.substr(dot + 1);
        std::vector<std::string> names = {nm};
        if (!nm.empty()) {
          if (std::optional<ojson> err = resolveOne(nm, names)) return err;
        }
        for (const auto& n : names) got.push_back(op + n);
      }
      std::vector<std::string> grown;
      for (const auto& c : combos)
        for (const auto& g : got) grown.push_back(c + g + sep);
      combos.swap(grown);
      if (next >= tok.size()) break;
      pos = next;
    }
    nameCount += combos.size();
    outTerms.insert(outTerms.end(), combos.begin(), combos.end());
  }
  if (single && nameCount > 1) return bsRcError("too many variables specified", 103);
  std::string joined;
  for (const auto& t : outTerms) joined += (joined.empty() ? "" : " ") + t;
  text = joined;
  return std::nullopt;
}

// Writes `text` to the resolved path; false when the file cannot be opened.
bool bsWriteFile(const std::string& path, const std::string& text) {
  std::ofstream f(cecon::to_native_path(path), std::ios::binary | std::ios::trunc);
  if (!f) return false;
  f << text;
  return static_cast<bool>(f);
}

// What binscatter's sample and its regressions are built from (ado).
struct BsSpec {
  std::vector<std::string> ys;
  std::string x, by, controls, absorb, wtype, wvar;
  bool fit = false, qfit = false, residualized = false, noaddmean = false;
  std::string xReg, x2Reg;
  std::vector<std::string> yReg;
};

// Runs `line`; false when it answers an error.
bool bsRun(SessionState& session, const std::string& line) {
  for (const auto& b : dispatch(line, session))
    if (b.is_object() && b.value("type", std::string()) == "error") return false;
  return true;
}

// Drops the helper variables `names` as an ado's tempvars go: r() and the
// data's changed flag are left as they were.
void bsDropHelpers(SessionState& session, const std::vector<std::string>& names, bool changed) {
  Datastore* ds = session.datastore();
  std::string list;
  for (const auto& n : names)
    if (ds != nullptr && ds->has(n)) list += " " + n;
  if (!list.empty()) {
    SessionState::TempDropScope td(session);
    (void)dispatch("quietly drop" + list, session);
  }
  session.datasetChanged = changed;
}

// The ado's `touse` (ado) as the byte variable `name`: `marksample
// touse` (if/in, every y and x nonmissing, a missing or zero weight out), then
// `markout `touse' `by' `controls' `absorb', strok` -- every variable of a
// control term (`a#c.b` marks out a and b; a time-series operator marks out
// where the operated value is missing). False, with nothing added, when the
// marker cannot be built.
bool bsMarkTouse(SessionState& session, const eval::ParsedCommand& parsed, const BsSpec& sp,
                 const std::string& name) {
  Datastore* ds = session.datastore();
  if (ds == nullptr || ds->has(name)) return false;
  const bool changed = session.datasetChanged;
  std::string cond;
  auto markout = [&](const std::string& e) { cond += (cond.empty() ? "!missing(" : " & !missing(") + e + ")"; };
  for (const auto& v : sp.ys) markout(v);
  markout(sp.x);
  if (!sp.by.empty()) markout(sp.by);
  for (const auto& term : splitWsOrComma(sp.controls)) {
    for (size_t pos = 0; pos < term.size();) {
      size_t hash = term.find('#', pos);
      if (hash == std::string::npos) hash = term.size();
      const std::string part = term.substr(pos, hash - pos);
      pos = hash + 1;
      if (part.empty()) continue;
      const size_t dot = part.rfind('.');
      std::string ts;  // the time-series operators of the term, the factor ones dropped
      if (dot != std::string::npos) {
        std::string seg;
        for (char c : part.substr(0, dot + 1)) {
          if (c != '.') {
            seg += c;
            continue;
          }
          if (!seg.empty() && std::strchr("LFDSlfds", seg[0]) != nullptr) ts += seg + ".";
          seg.clear();
        }
      }
      markout(ts + part.substr(dot == std::string::npos ? 0 : dot + 1));
    }
  }
  if (!sp.absorb.empty()) markout(sp.absorb);
  if (!sp.wvar.empty()) cond += " & !missing(" + sp.wvar + ") & (" + sp.wvar + ") != 0";
  std::string qual;
  if (parsed.ifExpr.has_value() && !strutil::trim(*parsed.ifExpr).empty()) qual += " if " + *parsed.ifExpr;
  if (parsed.inRange.has_value() && !strutil::trim(*parsed.inRange).empty()) qual += " in " + *parsed.inRange;
  if (!bsRun(session, "quietly generate byte " + name + " = 0") ||
      !bsRun(session, "quietly replace " + name + " = " + cond + qual)) {
    bsDropHelpers(session, {name}, changed);
    return false;
  }
  return true;
}

// `ereturn post, esample(`touse')` (ado): e() replaced whole, e(sample) the
// ado's touse (StataBE 19.5, BUG-1293: `count if e(sample)` answers e(N), 59 and
// 29 with an if). The marker must hold the engine's N rows; false, with e() and
// the data as they were, when it cannot be built or does not.
bool bsPostEsample(SessionState& session, const eval::ParsedCommand& parsed, const BsSpec& sp, double N) {
  const bool changed = session.datasetChanged;
  const std::string touse = "__000001";
  if (!bsMarkTouse(session, parsed, sp, touse)) return false;
  double ones = 0;
  if (Datastore* ds = session.datastore(); ds != nullptr && ds->has(touse)) {
    const double* v = ds->col(touse).f64();
    for (int64_t i = 0; v != nullptr && i < ds->nRows(); ++i) ones += v[i] == 1.0 ? 1 : 0;
  }
  const bool posted = ones == N && bsRun(session, "quietly ereturn post, esample(" + touse + ")");
  bsDropHelpers(session, {touse}, changed);
  return posted;
}

// e() after a binscatter that stops at its bins -- discrete over N/2 unique
// values r(134), fastxtile's nquantiles() bounds r(198) (ado). The ado
// has run its regressions by then and e() is the last of them (StataBE 19.5,
// BUG-1294: `binscatter x y, discrete` r(134) leaves e(cmd) regress, e(depvar)
// x, colnames `y _cons`): the last fit line (ado: `reg y_r [x_r2] x_r [wt]
// if touse [& by==byval]`, the last y of the last by-group), or with no fit line
// the last residualization (ado: `reg|areg y controls [wt] if touse`). They
// are rerun on the ado's tempvars, which are dropped after.
void bsReplayRegressions(SessionState& session, const eval::ParsedCommand& parsed, const BsSpec& sp) {
  if (!sp.fit && !sp.residualized) return;
  const bool changed = session.datasetChanged;
  const std::string touse = "__000001";
  if (!bsMarkTouse(session, parsed, sp, touse)) return;
  std::vector<std::string> made = {touse};
  const std::string wt = sp.wtype.empty() ? std::string() : " [" + sp.wtype + "=" + sp.wvar + "]";
  bool ok = true;
  auto run = [&](const std::string& line) { ok = ok && bsRun(session, line); };
  auto fresh = [&](const std::string& n) {
    Datastore* ds = session.datastore();
    ok = ok && ds != nullptr && !ds->has(n);
    if (ok) made.push_back(n);
  };
  if (sp.residualized) {
    std::vector<std::string> src = {sp.x}, dst = {sp.xReg};
    src.insert(src.end(), sp.ys.begin(), sp.ys.end());
    dst.insert(dst.end(), sp.yReg.begin(), sp.yReg.end());
    for (size_t i = 0; i < src.size() && ok; ++i) {
      fresh(dst[i]);
      run("quietly " + std::string(sp.absorb.empty() ? "regress " : "areg ") + src[i] + " " + sp.controls + wt +
          " if " + touse + (sp.absorb.empty() ? std::string() : ", absorb(" + sp.absorb + ")"));
      run("quietly predict " + dst[i] + " if e(sample), residuals");
      if (!sp.noaddmean) {
        run("quietly summarize " + src[i] + wt + " if " + touse + ", meanonly");
        run("quietly replace " + dst[i] + " = " + dst[i] + " + r(mean)");
      }
    }
  }
  if (sp.fit && ok) {
    std::string conds = touse;
    if (!sp.by.empty()) {
      run("quietly summarize " + sp.by + " if " + touse + ", meanonly");
      const double* mx = session.eval.rValues.find("max");
      ok = ok && mx != nullptr;
      if (ok) {
        char buf[40];
        std::snprintf(buf, sizeof buf, "%.17g", *mx);
        conds += " & " + sp.by + " == " + buf;
      }
    }
    if (sp.qfit) {
      fresh(sp.x2Reg);
      run("quietly generate " + sp.x2Reg + " = " + sp.xReg + "^2");
    }
    run("quietly regress " + sp.yReg.back() + " " + (sp.qfit ? sp.x2Reg + " " : std::string()) + sp.xReg + wt +
        " if " + conds);
  }
  bsDropHelpers(session, made, changed);
}

}  // namespace

ojson runBinscatter(const eval::ParsedCommand& parsed, SessionState& session) {
  if (!session.currentHbin().has_value()) return ojson::array({ojson(eval::noDataLoaded())});
  // BUG-233: the one copy of this vocabulary lives in the file-scope
  // registration block (see top of file); the dispatcher has already
  // canonicalized the keys against it.
  if (auto err = eval::rejectUnknownOptions(parsed.opts, kVocabBinscatter.known, kVocabBinscatter.unimplemented)) {
    return ojson::array({ojson(*err)});
  }
  // varlist(min=2 numeric), ado: several y, then x (ado).
  if (parsed.varlist.size() < 2) return ojson::array({bsRcError("too few variables specified", 102, "syntax_error")});
  // [aweight fweight] (ado): any other weight is Stata's r(101).
  std::string wtype;
  if (parsed.weightType.has_value()) {
    std::string wt = strutil::lower(*parsed.weightType);
    if (wt == "aw" || wt == "aweight") wtype = "aweight";
    else if (wt == "fw" || wt == "fweight") wtype = "fweight";
    else {
      const std::string full = (wt == "pw" || wt == "pweight") ? "pweight"
                               : (wt == "iw" || wt == "iweight") ? "iweight"
                                                                 : wt;
      return ojson::array({bsRcError(full + "s not allowed", 101)});
    }
  }

  // ---- the options, read as Stata's `syntax` reads them (ado)
  std::map<std::string, RawOpt> own;
  std::vector<std::string> passThrough;  // `options' (the catch-all), as typed
  for (auto& o : bsRawOptions(parsed.raw)) {
    const std::string full = bsMatch(o.name);
    if (full.empty()) {
      passThrough.push_back(o.text);
      continue;
    }
    if (kBsServed.count(full) == 0) {
      return ojson::array({ojson(eval::unsupportedOption(
          full, "binscatter's " + full + "() changes the bins, the fit or the files binscatter writes, and this "
                "build does not implement it; it is refused rather than ignored."))});
    }
    own[full] = o;
  }
  auto has = [&](const char* k) { return own.count(k) != 0; };
  auto arg = [&](const char* k) -> std::string {
    auto it = own.find(k);
    return it == own.end() || !it->second.arg.has_value() ? std::string() : *it->second.arg;
  };

  // Nquantiles(integer 20)
  int nquantiles = 20;
  if (has("nquantiles")) {
    const std::string t = strutil::trim(arg("nquantiles"));
    char* end = nullptr;
    const long v = std::strtol(t.c_str(), &end, 10);
    if (t.empty() || end == t.c_str() || *end != '\0')
      return ojson::array({bsRcError("option nquantiles() incorrectly specified", 198, "syntax_error")});
    nquantiles = static_cast<int>(v);
  }
  // by(), controls() and absorb() resolve as `syntax` resolves them, before
  // the ado's own checks (FU-1088).
  std::string byName = strutil::trim(arg("by"));
  std::string controls = strutil::trim(arg("controls"));
  std::string absorb = strutil::trim(arg("absorb"));
  if (auto err = bsResolveNames(session, byName, true)) return ojson::array({*err});
  if (auto err = bsResolveNames(session, controls, false)) return ojson::array({*err});
  if (auto err = bsResolveNames(session, absorb, true)) return ojson::array({*err});
  std::string linetype = strutil::trim(arg("linetype"));
  const bool discrete = has("discrete");
  const bool reportreg = has("reportreg");
  const bool replace = has("replace");
  const std::string savedata = has("savedata") ? bsDequote(arg("savedata")) : std::string();
  ojson blocks = ojson::array();

  // ---- the checks before anything is computed (ado)
  if (linetype == "noline") {
    // ado (legacy spelling)
    blocks.push_back(bsText("NOTE: legacy line type 'noline' has been renamed 'none', and is supported only for "
                            "backward compatibility."));
    linetype = "none";
  }
  // Three refusals the ado makes with `di as error` and a bare `exit`: rc 0,
  // nothing computed, e() left as it was (log 973-980 BS_lt_bad, 1084-1091
  // BS_reportreg_none, 1101-1108 BS_disc_nq).
  if (linetype.empty()) linetype = "lfit";
  else if (linetype != "connect" && linetype != "lfit" && linetype != "qfit" && linetype != "none") {
    blocks.push_back(bsText("linetype() must either be connect, lfit, qfit, or none"));
    return blocks;
  }
  const std::string genxq = strutil::trim(arg("genxq"));
  if (!genxq.empty() && discrete) {
    // ado (xq() is refused above, so discrete is the only partner)
    blocks.push_back(bsText("Cannot specify more than one of genxq(), xq(), and discrete simultaneously."));
    return blocks;
  }
  if (!genxq.empty()) {
    // `confirm new variable` (ado)
    if (!strutil::isValidStataIdentifier(genxq)) return ojson::array({bsRcError(genxq + " invalid name", 198)});
    Datastore* ds = session.datastore();
    if (ds != nullptr && ds->has(genxq))
      return ojson::array({bsRcError("variable " + genxq + " already defined", 110)});
  }
  if (nquantiles != 20 && discrete) {
    blocks.push_back(bsText("Cannot specify nquantiles in combination with discrete or an xq variable."));
    return blocks;
  }
  if (reportreg && linetype != "lfit" && linetype != "qfit") {
    blocks.push_back(bsText("Cannot specify 'reportreg' when no fit line is being created."));
    return blocks;
  }
  // savedata() without replace: confirm new file f.csv, then f.do (ado;
  // log 1195-1207 BS_sd_noreplace r(602), the old file left as it was).
  const std::string csvName = savedata + ".csv";
  const std::string doName = savedata + ".do";
  const std::string csvPath = savedata.empty() ? std::string() : resolveUserPath(session, csvName);
  const std::string doPath = savedata.empty() ? std::string() : resolveUserPath(session, doName);
  if (!savedata.empty() && !replace) {
    if (cecon::native_exists(csvPath)) return ojson::array({bsRcError("file " + csvName + " already exists", 602)});
    if (cecon::native_exists(doPath)) return ojson::array({bsRcError("file " + doName + " already exists", 602)});
  }

  // ---- the engine
  const std::vector<std::string> yNames(parsed.varlist.begin(), parsed.varlist.end() - 1);
  const std::string xName = parsed.varlist.back();
  if (!byName.empty()) {
    Datastore* ds = session.datastore();
    if (ds != nullptr && ds->has(byName)) {
      const std::string t = ds->dtype(byName);
      if (t == "factor" || t.rfind("str", 0) == 0) {
        return ojson::array({ojson(eval::unsupportedOption(
            "by", "a string by() variable is grouped through egen group() with its own labels in binscatter; this "
                  "build serves numeric by() variables only."))});
      }
    }
  }
  std::string yJoined;
  for (const auto& y : yNames) yJoined += (yJoined.empty() ? "" : " ") + y;
  std::vector<std::string> argv = {"binscatter", "--y", yJoined, "--x", xName, "--linetype", linetype};
  if (has("nquantiles")) {
    argv.push_back("--nquantiles");
    argv.push_back(std::to_string(nquantiles));
  }
  if (!byName.empty()) { argv.push_back("--by"); argv.push_back(byName); }
  if (!controls.empty()) { argv.push_back("--controls"); argv.push_back(controls); }
  if (!absorb.empty()) { argv.push_back("--absorb"); argv.push_back(absorb); }
  if (discrete) argv.push_back("--discrete");
  if (has("medians")) argv.push_back("--medians");
  if (has("noaddmean")) argv.push_back("--noaddmean");
  if (reportreg) argv.push_back("--reportreg");
  if (!genxq.empty()) argv.push_back("--genxq");
  // `set type double`: the ado's residual and x^2 variables take c(type)
  // (predict / gen, binscatter.ado), so they stay double.
  if (session.defaultStorageType == "double") argv.push_back("--double");
  if (!wtype.empty()) {
    argv.push_back("--weight");
    argv.push_back(*parsed.weightVar);
    argv.push_back("--wtype");
    argv.push_back(wtype);
  }
  pushKeepif(argv, parsed);
  if (parsed.inRange.has_value() && !parsed.inRange->empty()) {
    Datastore* ds = session.datastore();
    InRangeBounds rr = parseInRangeBounds(*parsed.inRange, ds != nullptr ? ds->nRows() : 0);
    if (!rr.ok) {
      InRangeFilterResult bad;
      bad.ok = false;
      bad.negative = rr.negative;
      bad.refusal = rr.refusal;
      return ojson::array({inRangeRejectError(bad, *parsed.inRange, "binscatter")});
    }
    argv.push_back("--inrange");
    argv.push_back(std::to_string(rr.a) + "/" + std::to_string(rr.b));
  }
  argv.push_back("--data");
  argv.push_back(*session.currentHbin());
  if (!piet::settings::load().provenanceHash) argv.push_back("--no-provenance");

  const bool fit = linetype == "lfit" || linetype == "qfit";
  const bool qfit = linetype == "qfit";
  const bool residualized = !controls.empty() || !absorb.empty();
  // Tempvar names (ado): sortpreserve __000000, marksample __000001,
  // by()'s byvalmatrix the next, then one residual per variable (x first),
  // then qfit's x^2 (log 1038-1062 BS_reportreg_ctl, BS_reportreg_by,
  // BS_reportreg_qfit). DESIGN DECISION (no oracle arm): absorb()'s areg is
  // assumed to take no tempname of the caller's sequence, like regress.
  int tmp = 2 + (byName.empty() ? 0 : 1);
  auto tempname = [&]() {
    char buf[16];
    std::snprintf(buf, sizeof buf, "__%06d", tmp++);
    return std::string(buf);
  };
  std::string xReg = xName;
  std::vector<std::string> yReg = yNames;
  if (residualized) {
    xReg = tempname();
    for (auto& y : yReg) y = tempname();
  }
  const std::string x2Reg = qfit ? tempname() : std::string();
  BsSpec spec;
  spec.ys = yNames;
  spec.x = xName;
  spec.by = byName;
  spec.controls = controls;
  spec.absorb = absorb;
  spec.wtype = wtype;
  spec.wvar = wtype.empty() ? std::string() : strutil::trim(*parsed.weightVar);
  spec.fit = fit;
  spec.qfit = qfit;
  spec.residualized = residualized;
  spec.noaddmean = has("noaddmean");
  spec.xReg = xReg;
  spec.x2Reg = x2Reg;
  spec.yReg = yReg;

  ojson r;
  try {
    r = runCobraJson(argv);
  } catch (const CobraError& ce) {
    // The engine names Stata's rc in the error's detail ("r(134)") where the
    // ado has one (discrete over N/2 values, fastxtile's nquantiles bounds,
    // negative or non-integer weights, no observations).
    const std::string& d = ce.detail;
    if (d.size() > 3 && d.compare(0, 2, "r(") == 0 && d.back() == ')') {
      const std::string digits = d.substr(2, d.size() - 3);
      if (!digits.empty() && digits.find_first_not_of("0123456789") == std::string::npos) {
        blocks.push_back(bsRcError(ce.what(), std::stoi(digits)));
        // r(134)/r(198) come from the bins, after the ado's regressions
        if (digits == "134" || digits == "198") bsReplayRegressions(session, parsed, spec);
        return blocks;
      }
    }
    blocks.push_back(qeError(cobraErrorMessage(ce), ce.code));
    return blocks;
  }

  const size_t ynum = yNames.size();
  const ojson byvalsJ = r.value("byvals", ojson::array());
  std::vector<double> byvals;
  for (const auto& v : byvalsJ) byvals.push_back(v.get<double>());
  const size_t bynum = byName.empty() ? 1 : byvals.size();
  const int nqFinal = static_cast<int>(num(r, "nquantiles", 0));
  const ojson points = r.value("points", ojson::array());
  const ojson coefs = r.value("coefs", ojson::array());

  // by-value text: the value label when the by variable has one (ado)
  std::vector<std::string> byText;
  {
    const auto vl = session.valueLabels.find(byName);
    for (double v : byvals) {
      std::string t = bsNum(v);
      if (vl != session.valueLabels.end() && v == std::floor(v)) {
        const auto it = vl->second.find(static_cast<int>(v));
        if (it != vl->second.end()) t = it->second;
      }
      byText.push_back(t);
    }
  }

  // ---- reportreg tables and the bin warnings, in the ado's order
  size_t ycur = 0;
  for (const auto& m : r.value("messages", ojson::array())) {
    const std::string kind = m.value("kind", std::string());
    if (kind == "group") {
      blocks.push_back(bsText(std::string(255, '-')));
      if (m.contains("byval")) {
        const double bv = m.at("byval").get<double>();
        std::string t = bsNum(bv);
        for (size_t b = 0; b < byvals.size(); ++b)
          if (byvals[b] == bv) t = byText[b];
        blocks.push_back(bsText("-> " + byName + " = " + t));
      }
      ycur = 0;
    } else if (kind == "yvar") {
      blocks.push_back(bsText("y_var = " + m.value("name", std::string())));
      for (size_t i = 0; i < ynum; ++i)
        if (yNames[i] == m.value("name", std::string())) ycur = i;
    } else if (kind == "regress") {
      std::vector<std::string> xs;
      if (qfit) xs.push_back(x2Reg);
      xs.push_back(xReg);
      bsRegressTable(m.at("reg"), yReg[ycur], xs, blocks);
    } else if (kind == "no_obs") {
      blocks.push_back(bsText(reportreg ? "no observations"
                                        : "no observations for one of the fit lines. add 'reportreg' for more info."));
    } else if (kind == "nq_warning") {
      blocks.push_back(bsText("warning: nquantiles(" + std::to_string(m.value("requested", 0)) +
                              ") was specified, but only " + std::to_string(m.value("generated", 0)) +
                              " were generated. see help file under nquantiles() for explanation."));
    } else if (kind == "auto_discrete") {
      // ado: only genxq() is told it was ignored
      if (!genxq.empty())
        blocks.push_back(bsText("note: the x-variable has fewer unique values than the number of bins specified (" +
                                std::to_string(nquantiles) +
                                ").  It will therefore be treated as discrete, and genxq() will be ignored"));
    } else if (kind == "discrete_controls_warning") {
      blocks.push_back(bsText("warning: discrete is specified in combination with controls() or absorb(). note that "
                              "binning takes places after residualization, so the residualized x-variable may "
                              "contain many more unique values."));
    }
  }

  // An engine error after printed output: cmd_binscatter replies {error, rc,
  // messages} and the messages above stand before the error, as the ado
  // prints them (`discrete controls(z)`: the warning of ado, then r(134)
  // at ado). Nothing is posted; e() is the ado's last regression.
  if (r.contains("error")) {
    const ojson& e = r.at("error");
    const int rc = r.value("rc", 0);
    if (rc > 0) blocks.push_back(bsRcError(e.value("message", std::string()), rc));
    else blocks.push_back(qeError("[" + e.value("code", std::string("engine_error")) + "] " +
                                      e.value("message", std::string()),
                                  e.value("code", std::string("engine_error"))));
    if (rc == 134 || rc == 198) bsReplayRegressions(session, parsed, spec);
    return blocks;
  }

  // ---- the graph command (ado)
  std::string colors = strutil::trim(arg("colors"));
  if (colors.empty())
    colors = "navy maroon forest_green dkorange teal cranberry lavender khaki sienna emidblue emerald brown erose "
             "gold bluishgray";
  const std::vector<std::string> colorW = bsWords(colors);
  std::vector<std::string> mcolorW = bsWords(arg("mcolors"));
  std::vector<std::string> lcolorW = bsWords(arg("lcolors"));
  const bool single = ynum == 1 && bynum == 1 && linetype != "connect";
  if (mcolorW.empty()) mcolorW = single ? std::vector<std::string>{bsWord(colorW, 1)} : colorW;
  if (lcolorW.empty()) lcolorW = single ? std::vector<std::string>{bsWord(colorW, 2)} : colorW;
  const std::vector<std::string> msymW = bsWords(arg("msymbols"));
  const std::string connect = linetype == "connect" ? "c(l)" : std::string();

  // point (row, by) of y#: the engine's compacted bins; past the end = missing
  auto pt = [&](size_t yi, size_t b, size_t row, bool wantX) -> double {
    if (yi >= points.size() || b >= points[yi].size()) return std::nan("");
    const ojson& arr = points[yi][b][wantX ? "x" : "y"];
    if (row >= arr.size() || !arr[row].is_number()) return std::nan("");
    return arr[row].get<double>();
  };
  // `local a `a' word` keeps one blank between the old text and the new word
  auto append = [](std::string& macro, const std::string& more) {
    std::string t = macro + " " + more;
    const size_t first = t.find_first_not_of(' ');
    macro = first == std::string::npos ? std::string() : t.substr(first);
  };
  auto trimmed = [](const std::string& t) {
    const size_t a = t.find_first_not_of(' ');
    if (a == std::string::npos) return std::string();
    const size_t b = t.find_last_not_of(' ');
    return t.substr(a, b - a + 1);
  };
  std::string scatters, sdScatters, legendLabels, order;
  int c = 0, series = 0;
  for (size_t b = 0; b < bynum; ++b) {
    for (size_t yi = 0; yi < ynum; ++yi) {
      ++c;
      double xv = pt(yi, b, 0, true), yv = pt(yi, b, 0, false);
      if (std::isnan(xv) || std::isnan(yv)) continue;
      ++series;
      append(scatters, "(scatteri");
      if (!savedata.empty()) {
        if (byName.empty()) append(sdScatters, "(scatter " + yNames[yi] + " " + xName);
        else
          append(sdScatters, "(scatter " + yNames[yi] + "_by" + std::to_string(b + 1) + " " + xName + "_by" +
                                 std::to_string(b + 1));
      }
      for (size_t row = 0; !std::isnan(xv) && !std::isnan(yv);) {
        append(scatters, bsNum(yv) + " " + bsNum(xv));
        ++row;
        xv = pt(yi, b, row, true);
        yv = pt(yi, b, row, false);
      }
      const std::string symbol = msymW.empty() ? std::string() : "msymbol(" + bsWord(msymW, c) + ")";
      const std::string sopts = trimmed(connect + " mcolor(" + bsWord(mcolorW, c) + ") lcolor(" +
                                        bsWord(lcolorW, c) + ") " + symbol);
      scatters += ", " + sopts + ")";
      if (!savedata.empty()) sdScatters += ", " + sopts + ")";
      if (byName.empty()) {
        if (ynum == 1) legendLabels = "off";
        else append(legendLabels, "lab(" + std::to_string(series) + " " + yNames[yi] + ")");
      } else if (ynum == 1) {
        append(legendLabels, "lab(" + std::to_string(series) + " " + byName + "=" + byText[b] + ")");
      } else {
        append(legendLabels, "lab(" + std::to_string(series) + " " + yNames[yi] + ": " + byName + "=" + byText[b] + ")");
      }
      if (!byName.empty() || ynum > 1) append(order, std::to_string(series));
    }
  }
  std::string fits;
  if (fit) {
    c = 0;
    for (size_t b = 0; b < bynum; ++b) {
      for (size_t yi = 0; yi < ynum; ++yi) {
        ++c;
        // bounds: the group's first x point and its last non-missing one,
        // searched down from row nquantiles (ado)
        const double left = pt(yi, b, 0, true);
        double right = std::nan("");
        for (int row = nqFinal - 1; row >= 0 && std::isnan(right); --row) right = pt(yi, b, static_cast<size_t>(row), true);
        if (yi >= coefs.size() || b >= coefs[yi].size()) continue;
        const ojson& row = coefs[yi][b];
        std::vector<double> cf;
        for (const auto& v : row) cf.push_back(v.is_number() ? v.get<double>() : std::nan(""));
        const std::string quad = qfit ? bsNum(cf.at(0)) : std::string("0");
        const double lin = qfit ? cf.at(1) : cf.at(0);
        const double cons = qfit ? cf.at(2) : cf.at(1);
        if ((qfit && std::isnan(cf.at(0))) || std::isnan(lin) || std::isnan(cons)) continue;
        append(fits, "(function " + quad + "*x^2+" + bsNum(lin) + "*x+" + bsNum(cons) + ", range(" + bsNum(left) +
                         " " + bsNum(right) + ") lcolor(" + bsWord(lcolorW, c) + "))");
      }
    }
  }
  std::string ytitle = yJoined;
  if (ynum == 2) ytitle = yNames[0] + " and " + yNames[1];
  else if (ynum > 2) {
    ytitle.clear();
    for (size_t i = 0; i < ynum; ++i) ytitle += (i ? "; " : "") + yNames[i];
  }
  std::string optionsText;
  for (const auto& t : passThrough) optionsText += (optionsText.empty() ? "" : " ") + t;
  const std::string tail = ", graphregion(fcolor(white))  xtitle(" + xName + ") ytitle(" + ytitle + ") legend(" +
                           legendLabels + " order(" + order + ")) " + optionsText;
  const std::string graphcmd = trimmed("twoway " + scatters + " " + fits + tail);
  const std::string sdGraphcmd = trimmed("twoway " + sdScatters + " " + fits + tail);

  // ---- savedata (ado): f.csv, then f.do, each announced
  if (!savedata.empty()) {
    std::string csv;
    for (int row = 0; row <= nqFinal; ++row) {
      std::vector<std::string> cells;
      for (size_t b = 0; b < bynum; ++b) {
        const std::string bl = byName.empty() ? std::string() : "_by" + std::to_string(b + 1);
        if (row == 0) cells.push_back(xName + bl);
        else cells.push_back(bsFileNum(pt(0, b, static_cast<size_t>(row - 1), true)));
      }
      for (size_t yi = 0; yi < ynum; ++yi)
        for (size_t b = 0; b < bynum; ++b) {
          const std::string bl = byName.empty() ? std::string() : "_by" + std::to_string(b + 1);
          if (row == 0) cells.push_back(yNames[yi] + bl);
          else cells.push_back(bsFileNum(pt(yi, b, static_cast<size_t>(row - 1), false)));
        }
      for (size_t i = 0; i < cells.size(); ++i) csv += (i ? "," : "") + cells[i];
      csv += "\n";
    }
    if (replace && !cecon::native_exists(csvPath)) blocks.push_back(bsText("(file " + csvName + " not found)"));
    if (!bsWriteFile(csvPath, csv))
      return ojson::array({bsRcError("file " + csvName + " could not be opened", 603)});
    blocks.push_back(bsText("(file " + csvName + " written containing saved data)"));

    std::string dofile = "insheet using " + csvName + "\n\n";
    if (!byName.empty()) {
      std::vector<std::string> vars = {xName};
      vars.insert(vars.end(), yNames.begin(), yNames.end());
      for (const auto& v : vars)
        for (size_t b = 0; b < bynum; ++b)
          dofile += "label variable " + v + "_by" + std::to_string(b + 1) + " \"" + v + "; " + byName + "==" +
                    byText[b] + "\"\n";
      dofile += "\n";
    }
    dofile += sdGraphcmd + "\n";
    if (replace && !cecon::native_exists(doPath)) blocks.push_back(bsText("(file " + doName + " not found)"));
    if (!bsWriteFile(doPath, dofile))
      return ojson::array({bsRcError("file " + doName + " could not be opened", 603)});
    blocks.push_back(bsText("(file " + doName + " written containing commands to process saved data)"));
  }

  // ---- e() (ado): `ereturn post, esample(`touse')` replaces the whole
  // of e(); a touse that cannot be rebuilt posts e() without e(sample).
  if (!bsPostEsample(session, parsed, spec, num(r, "N"))) {
    session.lastModel_reset();
    session.eval.eScalars = eval::OrderedFloatDict();
    session.eval.eStrings.clear();
    session.eval.eMatrices.clear();
    session.ePostedLive = true;
    session.ePostedEsampleSerial = 0;
  }
  session.eval.setEScalar("N", num(r, "N"));
  session.eval.setEString("graphcmd", graphcmd);
  if (fit) {
    for (size_t k = ynum; k >= 1; --k) {
      eval::EvalMatrix m;
      for (size_t b = 0; b < coefs[k - 1].size(); ++b) {
        std::vector<double> rowv;
        for (const auto& v : coefs[k - 1][b]) rowv.push_back(v.is_number() ? v.get<double>() : std::nan(""));
        m.data.push_back(rowv);
        m.rownames.push_back(byName.empty() ? std::string("=") : "by" + std::to_string(b + 1) + ":=");
      }
      if (qfit) m.colnames = {xName + "^2", xName, "_cons"};
      else m.colnames = {xName, "_cons"};
      session.eval.eMatrices.emplace_back("y" + std::to_string(k) + "_coefs", std::move(m));
    }
  }
  if (!byName.empty()) {
    eval::EvalMatrix m;
    for (size_t b = 0; b < byvals.size(); ++b) {
      m.data.push_back({byvals[b]});
      m.rownames.push_back("by" + std::to_string(b + 1));
    }
    m.colnames = {byName};
    session.eval.eMatrices.emplace_back("byvalues", std::move(m));
  }

  // ---- genxq(): fastxtile's variable (ado): byte up to
  // 100 bins, labelled "<n> quantiles of <x_r>", missing outside the sample.
  std::optional<ojson> genxqMutation;
  if (!genxq.empty() && r.contains("xq") && r.at("xq").is_array()) {
    std::vector<double> vals;
    for (const auto& v : r.at("xq")) vals.push_back(v.is_number() ? v.get<double>() : std::nan(""));
    const std::string hbin = mintScratchPath(session.workdir(), "binscatter");
    forgetScratchMint(hbin);
    const std::string csvPayload = hbin.substr(0, hbin.size() - 5) + ".csv";
    registerScratchMint(csvPayload);
    std::string payload;
    for (double d : vals) {
      if (!std::isnan(d)) payload += std::to_string(static_cast<long long>(d));
      payload += "\n";
    }
    const std::string outHbin = mintScratchPath(session.workdir(), "binscatter");
    if (!bsWriteFile(csvPayload, payload)) {
      blocks.push_back(bsRcError("binscatter: could not write the genxq() values", 603, "data_io"));
      return blocks;
    }
    const CobraRunResult wr = runCobra({"setcol", "--data", *session.currentHbin(), "--var", genxq, "--values",
                                        csvPayload, "--dtype", "f64", "--new", "--out", outHbin});
    if (!wr.ok || !cecon::native_exists(outHbin)) {
      blocks.push_back(bsRcError("binscatter: could not create " + genxq, 603, "data_io"));
      return blocks;
    }
    session.load(outHbin);
    session.implicitFloatVars.erase(genxq);
    session.varMeta[genxq].stataType = nquantiles <= 100 ? "byte" : nquantiles <= 32740 ? "int" : "long";
    session.setVarLabel(genxq, std::to_string(nquantiles) + " quantiles of " + xReg);
    genxqMutation = mutationResult(session, "binscatter", {genxq}, {});
  }

  // ---- the picture (Arc G's twoway block): one scatter layer per series, the
  // fit lines evaluated over their range, connect lines through the points.
  std::vector<ojson> layers;
  int layerIdx = 0;
  for (size_t b = 0; b < bynum; ++b)
    for (size_t yi = 0; yi < ynum; ++yi) {
      ojson pts = ojson::array();
      for (size_t row = 0;; ++row) {
        const double xv = pt(yi, b, row, true), yv = pt(yi, b, row, false);
        if (std::isnan(xv) || std::isnan(yv)) break;
        pts.push_back(ojson::array({xv, yv}));
      }
      if (pts.empty()) continue;
      layers.push_back(qeGraphLayer(linetype == "connect" ? "line" : "scatter", pts, layerIdx));
      if (fit && yi < coefs.size() && b < coefs[yi].size()) {
        std::vector<double> cf;
        for (const auto& v : coefs[yi][b]) cf.push_back(v.is_number() ? v.get<double>() : std::nan(""));
        const double a2 = qfit ? cf[0] : 0.0, a1 = qfit ? cf[1] : cf[0], a0 = qfit ? cf[2] : cf[1];
        const double lo = pts.front()[0].get<double>(), hi = pts.back()[0].get<double>();
        ojson line = ojson::array();
        const int steps = qfit ? 40 : 1;
        for (int s = 0; s <= steps && !std::isnan(a1) && !std::isnan(a0); ++s) {
          const double xv = s == steps ? hi : lo + (hi - lo) * s / steps;
          line.push_back(ojson::array({xv, a2 * xv * xv + a1 * xv + a0}));
        }
        layers.push_back(qeGraphLayer("line", line, layerIdx));
      }
      ++layerIdx;
    }
  ojson graph = qeGraphBlock(layers, std::string(), xName, ytitle, session);
  blocks.push_back(registerQeGraph(std::move(graph), session));

  ojson out;
  out["type"] = "qe_result";
  out["estimator"] = "binscatter";
  out["N"] = r.value("N", 0);
  out["nquantiles"] = nqFinal;
  out["graphcmd"] = graphcmd;
  blocks.push_back(out);
  if (genxqMutation.has_value()) blocks.push_back(*genxqMutation);
  return blocks;
}

// ===========================================================================
// eventstudyinteract y rel1 rel2 ... [if], absorb(F1 F2) cohort(G)
//                                          control_cohort(NEVER) vce(cluster ID) level(#)
//
// Oracle: p3_stata.log:290-309. Stata's `absorb()` is space-separated and the
// engine wants a comma list; `vce(cluster id)` carries the cluster variable
// INSIDE the option and has to be split into `--vce cluster --cluster id`.
ojson runEventstudyinteract(const eval::ParsedCommand& parsed, SessionState& session) {
  if (!session.currentHbin().has_value()) return ojson::array({ojson(eval::noDataLoaded())});
  if (parsed.varlist.size() < 2) {
    return qeErrorBlocks(
        "syntax_error: eventstudyinteract requires a depvar followed by at least one "
        "relative-period indicator — `eventstudyinteract y F2 L2, absorb(id t) cohort(g) "
        "control_cohort(never)`.",
        "syntax_error");
  }
  // BUG-233: the one copy of this vocabulary now lives in the file-scope
  // registration block (see top of file); this binding keeps every existing
  // use — including the reject net below — reading the same set.
  const std::unordered_set<std::string>& kKnown = kVocabEventstudyinteract.known;
  const std::unordered_set<std::string>& kUnimplemented = kVocabEventstudyinteract.unimplemented;
  if (auto err = eval::rejectUnknownOptions(parsed.opts, kKnown, kUnimplemented)) {
    return ojson::array({ojson(*err)});
  }

  auto absorb = optStr(parsed, "absorb");
  auto cohort = optStr(parsed, "cohort");
  auto controlCohort = optStr(parsed, "control_cohort");
  if (!absorb.has_value() || absorb->empty()) {
    return qeErrorBlocks("eventstudyinteract requires `absorb(varlist)`.", "unsupported_option");
  }
  if (!cohort.has_value() || cohort->empty()) {
    return qeErrorBlocks("eventstudyinteract requires `cohort(varname)`.", "unsupported_option");
  }
  if (!controlCohort.has_value() || controlCohort->empty()) {
    return qeErrorBlocks("eventstudyinteract requires `control_cohort(varname)`.",
                          "unsupported_option");
  }

  std::vector<std::string> rel(parsed.varlist.begin() + 1, parsed.varlist.end());
  std::vector<std::string> argv = {"eventstudyinteract",
                                   "--y",
                                   parsed.varlist[0],
                                   "--rel",
                                   joinComma(rel),
                                   "--absorb",
                                   joinComma(splitWsOrComma(*absorb)),
                                   "--cohort",
                                   *cohort,
                                   "--control_cohort",
                                   *controlCohort};

  // vce(cluster id) -> --vce cluster --cluster id. A bare cluster(id) is
  // accepted too (some call sites spell it that way).
  std::string vceKind, clusterVar;
  if (auto vce = optStr(parsed, "vce"); vce.has_value() && !vce->empty()) {
    std::vector<std::string> toks = splitWsOrComma(*vce);
    if (!toks.empty()) {
      vceKind = toks[0];
      if (toks.size() > 1) clusterVar = toks[1];
    }
  }
  if (auto cl = optStr(parsed, "cluster"); cl.has_value() && !cl->empty()) {
    std::vector<std::string> toks = splitWsOrComma(*cl);
    if (!toks.empty()) {
      clusterVar = toks[0];
      if (vceKind.empty()) vceKind = "cluster";
    }
  }
  if (vceKind == "cluster" && clusterVar.empty()) {
    return qeErrorBlocks("eventstudyinteract: vce(cluster ...) requires a cluster variable — "
                          "e.g. `vce(cluster id)`.",
                          "syntax_error");
  }
  if (!vceKind.empty()) {
    argv.push_back("--vce");
    argv.push_back(vceKind);
  }
  if (!clusterVar.empty()) {
    argv.push_back("--cluster");
    argv.push_back(clusterVar);
  }
  // [aw=]/[pw=] (BUG-1283): the engine weights every regression of the IW
  // estimator (StataBE 19.5 e(N) 180, e(b_iw) .86176929 .12436249 1.1556403).
  std::string wtype;
  if (parsed.weightVar.has_value() && !parsed.weightVar->empty()) {
    const std::string wt = strutil::lower(parsed.weightType.value_or("aw"));
    wtype = wt == "pw" || wt == "pweight" ? "pw" : "aw";
    argv.push_back("--weight");
    argv.push_back(*parsed.weightVar);
    argv.push_back("--wtype");
    argv.push_back(wtype);
  }
  pushLevel(argv, parsed, session);
  pushKeepif(argv, parsed);
  argv.push_back("--data");
  argv.push_back(*session.currentHbin());

  EngineCall call = callEngine(argv);
  if (!call.ok) return ojson::array({call.error});
  const ojson& r = call.raw;

  Printer say;
  // The weights regression's own note comes first (`(sum of wgt is 162)`).
  if (!wtype.empty() && r.contains("weight") && r["weight"].is_object() &&
      r["weight"].contains("sum_w_weights") && r["weight"]["sum_w_weights"].is_number()) {
    char buf[64];
    std::snprintf(buf, sizeof buf, "(sum of wgt is %.16g)",
                  r["weight"]["sum_w_weights"].get<double>());
    say(buf);
  }
  say("(obs=" + commaGroup(inum(r, "n_obs_weights")) + ")");
  say("");
  const int64_t nAbsorb =
      (r.contains("absorb") && r.at("absorb").is_array()) ? static_cast<int64_t>(r.at("absorb").size()) : 0;
  // Stata's standard 78-column estimation header: title padded to 55, then a
  // 14-wide key, "=", and the value right-aligned in 8 (StataBE 19.5, BUG-1488:
  // `IW estimates for dynamic effects` + 23 blanks + `Number of obs =     180`,
  // `Adj R-squared = -0.0990`).
  auto hdr = [&](const std::string& title, const std::string& key, const std::string& val) {
    return padRight(title, 55) + padRight(key, 14) + "=" + padLeft(val, 8);
  };
  say(hdr("IW estimates for dynamic effects", "Number of obs", commaGroup(inum(r, "n_obs"))));
  say(hdr("Absorbing " + std::to_string(nAbsorb) + " HDFE groups",
          "F(" + std::to_string(inum(r, "df1")) + ", " + std::to_string(inum(r, "df2")) + ")",
          f2(num(r, "F"))));
  say(hdr("", "Prob > F", f4(num(r, "prob_F"))));
  say(hdr("", "R-squared", f4(num(r, "r2"))));
  say(hdr("", "Adj R-squared", f4(num(r, "r2_a"))));
  say(hdr("", "Root MSE", f4(num(r, "rmse"))));
  const int64_t nClust = inum(r, "n_clusters");
  if (nClust > 0) {
    say(padLeft("(Std. err. adjusted for " + commaGroup(nClust) + " clusters in " +
                    str(r, "cluster") + ")",
                78));
  }

  const std::string lvl = levelWord(num(r, "level", 95.0));
  const std::string rule78(78, '-');
  say(rule78);
  say(padLeft("", 12) + " |" + padLeft("Robust", 21));
  say(padLeft(str(r, "y"), 12) + " |" + coefHeader("std. err.", "t", lvl));
  say(std::string(13, '-') + "+" + std::string(64, '-'));

  ojson coeftable = ojson::array();
  for (const auto& name : orderedCoefNames(r, rel)) {
    auto b = mapNum(r, "coef", name);
    auto se = mapNum(r, "se", name);
    auto t = mapNum(r, "tstat", name);
    auto p = mapNum(r, "pvalue", name);
    auto lo = mapNum(r, "ci_lo", name);
    auto hi = mapNum(r, "ci_hi", name);
    // A period no row has: the engine's coefficient is missing and Stata lists the
    // row as omitted (StataBE 19.5: `          r9 |          0  (omitted)`).
    if (!b.has_value()) {
      say(padLeft(name, 12) + " |" + padLeft("0", 11) + "  (omitted)");
      coeftable.push_back(coefRow(name, b, se, t, p, lo, hi));
      continue;
    }
    say(padLeft(name, 12) + " |" + padLeft(g9cell(b.value_or(0.0)), 11) +
        padLeft(g9cell(se.value_or(0.0)), 11) + padLeft(f2(t.value_or(0.0)), 9) +
        padLeft(f3(p.value_or(0.0)), 8) + padLeft(g9cell(lo.value_or(0.0)), 13) +
        padLeft(g9cell(hi.value_or(0.0)), 12));
    coeftable.push_back(coefRow(name, b, se, t, p, lo, hi));
  }
  say(rule78);

  // ONE object serves both consumers: `e()`/`_b[]` (macros.cpp reads it as
  // LastModel::result) and eststo/esttab (which snapshots lastEstBlock and
  // wants an estimation-shaped block with coeftable/n_obs/r2/depvar).
  ojson e;
  e["type"] = "estimation";
  e["estimator"] = "eventstudyinteract";
  e["depvar"] = str(r, "y");
  e["coeftable"] = coeftable;
  e["n_obs"] = inum(r, "n_obs");
  e["n_clusters"] = nClust;
  e["F"] = num(r, "F");
  e["p_F"] = num(r, "prob_F");
  e["r2"] = num(r, "r2");
  e["adj_r2"] = num(r, "r2_a");
  e["rmse"] = num(r, "rmse");
  e["df_m"] = inum(r, "df1");
  e["df_r"] = inum(r, "df2");
  postEClass(session, "eventstudyinteract", str(r, "y"), *session.currentHbin(), e, true,
             wtype.empty() ? std::string() : *parsed.weightVar, wtype);
  session.lastEstBlock = e;
  // e(b_iw) and e(V_iw), named by the relative-period indicators (StataBE 19.5,
  // BUG-1524: `matrix list e(b_iw)` is 1 x 3, row r1, columns rm2 r0 r1; e(V_iw)
  // 3 x 3 on the same names; `ereturn list` shows e(V_iw) before e(b_iw)). A
  // period with no observation (the engine's missing coefficient) is 0 there, as
  // Stata's omitted column.
  if (r.contains("rel") && r.at("rel").is_array() && r.contains("V_iw") &&
      r.at("V_iw").is_array() && r.at("V_iw").size() == r.at("rel").size()) {
    eval::EvalMatrix bIw, vIw;
    bIw.rownames = {"r1"};
    bIw.data.emplace_back();
    for (const auto& nm : r.at("rel")) {
      const std::string name = nm.is_string() ? nm.get<std::string>() : std::string();
      bIw.colnames.push_back(name);
      bIw.data[0].push_back(mapNum(r, "coef", name).value_or(0.0));
    }
    vIw.rownames = vIw.colnames = bIw.colnames;
    for (const auto& row : r.at("V_iw")) {
      std::vector<double> vals;
      if (row.is_array())
        for (const auto& v : row) vals.push_back(v.is_number() ? v.get<double>() : std::nan(""));
      vals.resize(bIw.colnames.size(), std::nan(""));
      vIw.data.push_back(std::move(vals));
    }
    // Recorded with noteEOnFit, so the next fit's install drops them.
    auto put = [&session](const std::string& name, eval::EvalMatrix m) {
      auto& store = session.eval.eMatrices;
      store.erase(std::remove_if(store.begin(), store.end(),
                                 [&name](const auto& p) { return p.first == name; }),
                  store.end());
      store.emplace_back(name, std::move(m));
      session.noteEOnFit(name);
    };
    put("V_iw", std::move(vIw));
    put("b_iw", std::move(bIw));
  }

  ojson out = r;
  out["type"] = "qe_result";
  say.lines.push_back(out);
  return say.lines;
}

// ===========================================================================
// did_imputation y ivar tvar gvar [if], horizons(numlist) pretrends(#) autosample level(#)
//
// Oracle: p3_stata.log:320-332. Note the z column (not t) and the verbatim
// autosample warning line, which comes straight from the engine's own
// `autosample_message` rather than being reconstructed here.
ojson runDidImputation(const eval::ParsedCommand& parsed, SessionState& session) {
  if (!session.currentHbin().has_value()) return ojson::array({ojson(eval::noDataLoaded())});
  if (parsed.varlist.size() != 4) {
    return qeErrorBlocks(
        "syntax_error: did_imputation takes four variables positionally — "
        "`did_imputation Y ivar tvar gvar`.",
        "syntax_error");
  }
  // BUG-233: the one copy of this vocabulary now lives in the file-scope
  // registration block (see top of file); this binding keeps every existing
  // use — including the reject net below — reading the same set.
  const std::unordered_set<std::string>& kKnown = kVocabDidImputation.known;
  const std::unordered_set<std::string>& kUnimplemented = kVocabDidImputation.unimplemented;
  if (auto err = eval::rejectUnknownOptions(parsed.opts, kKnown, kUnimplemented)) {
    return ojson::array({ojson(*err)});
  }

  std::vector<std::string> argv = {"did_imputation", "--y",    parsed.varlist[0],
                                   "--ivar",         parsed.varlist[1],
                                   "--time",         parsed.varlist[2],
                                   "--gvar",         parsed.varlist[3]};
  pushOpt(argv, parsed, "horizons", "--horizons");
  pushOpt(argv, parsed, "pretrends", "--pretrends");
  if (optFlag(parsed, "autosample")) argv.push_back("--autosample");
  pushLevel(argv, parsed, session);
  pushKeepif(argv, parsed);
  argv.push_back("--data");
  argv.push_back(*session.currentHbin());

  EngineCall call = callEngine(argv);
  if (!call.ok) return ojson::array({call.error});
  const ojson& r = call.raw;

  Printer say;
  const std::string warn = str(r, "autosample_message");
  if (!warn.empty()) {
    say(warn);
    say("");
  }
  // Unlike the eventstudyinteract header, did_imputation has no left-hand
  // title, and Stata right-aligns the whole "Number of obs = N" phrase in the
  // 78-column band (one column further right than the titled form).
  say(padLeft("Number of obs = " + commaGroup(inum(r, "n_obs")), 78));

  const std::string lvl = levelWord(num(r, "level", 95.0));
  const std::string rule78(78, '-');
  say(rule78);
  say(padLeft(str(r, "y"), 12) + " |" + coefHeader("Std. err.", "z", lvl));
  say(std::string(13, '-') + "+" + std::string(64, '-'));

  // Stata's row order is every horizon's tau, THEN the pretrends — not the
  // alphabetical order the engine's key-sorted JSON hands over.
  std::vector<std::string> wanted;
  if (r.contains("horizons") && r.at("horizons").is_array()) {
    for (const auto& h : r.at("horizons")) {
      if (h.is_number()) {
        wanted.push_back("tau" + std::to_string(static_cast<long long>(h.get<double>())));
      }
    }
  }
  for (int64_t k = 1; k <= inum(r, "pretrends"); ++k) {
    wanted.push_back("pre" + std::to_string(k));
  }

  ojson coeftable = ojson::array();
  for (const auto& name : orderedCoefNames(r, wanted)) {
    auto b = mapNum(r, "coef", name);
    auto se = mapNum(r, "se", name);
    auto z = mapNum(r, "zstat", name);
    auto p = mapNum(r, "pvalue", name);
    auto lo = mapNum(r, "ci_lo", name);
    auto hi = mapNum(r, "ci_hi", name);
    say(padLeft(name, 12) + " |" + padLeft(g9cell(b.value_or(0.0)), 11) +
        padLeft(g9cell(se.value_or(0.0)), 11) + padLeft(f2(z.value_or(0.0)), 9) +
        padLeft(f3(p.value_or(0.0)), 8) + padLeft(g9cell(lo.value_or(0.0)), 13) +
        padLeft(g9cell(hi.value_or(0.0)), 12));
    coeftable.push_back(coefRow(name, b, se, z, p, lo, hi));
  }
  say(rule78);

  // e(): the coefficient vector and N. No F/R2 — did_imputation computes
  // neither, so e(F)/e(r2) stay absent rather than reporting a stale model's.
  ojson e;
  e["type"] = "estimation";
  e["estimator"] = "did_imputation";
  e["depvar"] = str(r, "y");
  e["coeftable"] = coeftable;
  e["n_obs"] = inum(r, "n_obs");
  e["n_clusters"] = inum(r, "n_clusters");
  postEClass(session, "did_imputation", str(r, "y"), *session.currentHbin(), e);
  session.lastEstBlock = e;

  ojson out = r;
  out["type"] = "qe_result";
  say.lines.push_back(out);
  return say.lines;
}

// ===========================================================================
// sdid y ivar tvar treated [if], vce(bootstrap|noinference) reps(#) seed(#) level(#)
//
// Oracle: p3_stata.log:366-378.
//
// OUR SE IS OURS. The bootstrap draws come from this project's own RNG stream,
// so the SE / t / CI cells legitimately differ from Stata's. The ATT and the
// table shape are the parity claim.
ojson runSdid(const eval::ParsedCommand& parsed, SessionState& session) {
  if (!session.currentHbin().has_value()) return ojson::array({ojson(eval::noDataLoaded())});
  if (parsed.varlist.size() != 4) {
    return qeErrorBlocks(
        "syntax_error: sdid takes four variables positionally — `sdid Y ivar tvar treated`.",
        "syntax_error");
  }
  // BUG-233: the one copy of this vocabulary now lives in the file-scope
  // registration block (see top of file); this binding keeps every existing
  // use — including the reject net below — reading the same set.
  const std::unordered_set<std::string>& kKnown = kVocabSdid.known;
  const std::unordered_set<std::string>& kUnimplemented = kVocabSdid.unimplemented;
  if (auto err = eval::rejectUnknownOptions(parsed.opts, kKnown, kUnimplemented)) {
    return ojson::array({ojson(*err)});
  }

  std::vector<std::string> argv = {"sdid",      "--y",    parsed.varlist[0],
                                   "--ivar",    parsed.varlist[1],
                                   "--time",    parsed.varlist[2],
                                   "--treated", parsed.varlist[3]};
  std::string vceKind;
  if (auto vce = optStr(parsed, "vce"); vce.has_value() && !vce->empty()) {
    std::vector<std::string> toks = splitWsOrComma(*vce);
    if (!toks.empty()) vceKind = toks[0];
    argv.push_back("--vce");
    argv.push_back(vceKind);
  }
  pushOpt(argv, parsed, "reps", "--reps");
  pushOpt(argv, parsed, "seed", "--seed");
  pushLevel(argv, parsed, session);
  pushKeepif(argv, parsed);
  argv.push_back("--data");
  argv.push_back(*session.currentHbin());

  // Stata prints the bootstrap progress ruler BEFORE the replications run;
  // ours is emitted around the (synchronous) engine call, which is the same
  // place in the transcript.
  Printer say;
  const int64_t repsRequested =
      [&]() -> int64_t {
        auto v = optStr(parsed, "reps");
        if (!v.has_value()) return 0;
        try { return std::stoll(*v); } catch (...) { return 0; }
      }();

  EngineCall call = callEngine(argv);
  if (!call.ok) return ojson::array({call.error});
  const ojson& r = call.raw;

  const std::string vceUsed = str(r, "vce", vceKind);
  if (vceUsed == "bootstrap" || vceUsed == "placebo" || vceUsed == "jackknife") {
    const int64_t done = inum(r, "reps_done", repsRequested);
    say("Bootstrap replications (" + std::to_string(done) + "). This may take some time.");
    // Stata's fixed 50-column ruler, one dot per completed replication,
    // wrapping at 50.
    const std::string ruler = "----+--- 1 ---+--- 2 ---+--- 3 ---+--- 4 ---+--- 5";
    say(ruler);
    for (int64_t i = 0; i < done; i += 50) {
      const int64_t n = std::min<int64_t>(50, done - i);
      say(std::string(static_cast<size_t>(n), '.'));
    }
  }
  say("");
  say("Synthetic Difference-in-Differences Estimator");
  say("");

  const std::string lvl = levelWord(num(r, "level", 95.0));
  const std::string rule77(77, '-');
  say(rule77);
  say(padLeft(str(r, "y"), 12) + " |" + "     ATT     Std. Err.     t      P>|t|    [" + lvl +
      "% Conf. Interval]");
  say(std::string(13, '-') + "+" + std::string(63, '-'));
  say(padLeft(parsed.varlist[3], 12) + " |" + padLeft(f5(num(r, "att")), 10) +
      padLeft(f5(num(r, "se")), 11) + padLeft(f2(num(r, "t")), 9) + padLeft(f3(num(r, "p")), 9) +
      padLeft(f5(num(r, "ci_lo")), 12) + padLeft(f5(num(r, "ci_hi")), 12));
  say(rule77);
  say(lvl + "% CIs and p-values are based on large-sample approximations.");
  say("Refer to Arkhangelsky et al., (2021) for theoretical derivations.");

  ojson e;
  e["type"] = "estimation";
  e["estimator"] = "sdid";
  e["depvar"] = str(r, "y");
  e["coeftable"] = ojson::array({coefRow(parsed.varlist[3], num(r, "att"), num(r, "se"),
                                          num(r, "t"), num(r, "p"), num(r, "ci_lo"),
                                          num(r, "ci_hi"))});
  e["n_obs"] = inum(r, "n_obs");
  e["n_units"] = inum(r, "n_units");
  postEClass(session, "sdid", str(r, "y"), *session.currentHbin(), e);
  session.lastEstBlock = e;

  ojson out = r;
  out["type"] = "qe_result";
  say.lines.push_back(out);
  return say.lines;
}

}  // namespace handlers
}  // namespace piet
