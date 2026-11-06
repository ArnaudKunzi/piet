// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// AER-SCOPE1 bundle J (GRAPH-X, 2026-09-04) -- the community graph and geo
// verbs the ninety-paper corpus reaches for around its figures:
//
//   grc1leg2   `graph combine` with exactly one panel keeping its legend
//   geodist    the distance between two coordinate pairs, as a new column
//   geonear    each master row's k nearest rows in a `using` file
//   cdfplot    the empirical CDF, as the twoway line block already rendered
//   event_plot the stored event-study coefficients, in relative-time order
//
// (`grstyle`, the bundle's sixth registry row, is an accept-note handled
// entirely at its dispatcher arm and has no handler here.)
//
// NOTHING HERE ADDS A PLOT TYPE, A RENDERER, OR AN ENGINE VERB. That is the
// whole design. `cdfplot` emits `kind:"twoway"` with `kind:"line"` layers --
// byte-shape-identical to what layerLineOrConnected already produces, so the
// existing SVG builder and piet/frontend/js/graph.js draw it with no change.
// `grc1leg2` calls `runGraphCombine` and then sets `legend.off` on the panels
// that must lose their legend, a key graph.js:795 has honoured since BS-047.
// `event_plot` calls `runCoefplot` and reorders the coefficients it returns.
// The two geo verbs write their columns through `cobra setcol --new` +
// `session.load` + `mutationResult`, the same pair every data verb in this
// codebase has used since handlers.cpp:787-841. src/ (the engine), the
// frontend and graph_m4.cpp are untouched.
//
// FIVE HANDLERS, NOT THE PLAN'S FOURTEEN COMMANDS. `grc1leg` -- the
// un-suffixed spelling -- is deliberately absent, here and in the registry:
// its package states no author (none in the ado, none in the help file), so
// the campaign's GPL-attribution rule gives it no row, and the plain spelling
// waits for its author while `grc1leg2` (which does claim one) ships. The
// eight names this bundle refuses -- cibar, binsreg, spmap, maptile, shp2dta,
// geoinpoly, heatplot, distplot -- are community names on the same footing,
// so they are unregistered for the same reason and already answer r(199),
// which is exactly what a named refusal arm would have said. There is no
// refusal arm here for any of them: a second, weaker copy of an answer the
// registry already gives is not honesty, it is duplication.
//
// THE DEFECT CLASSES THIS FILE IS WRITTEN AGAINST. Each has shipped at least
// once in this campaign; the first has now been caught four times in seven
// bundles, so it is named first.
//
//  1. SHAPE. A handler returning a JSON ARRAY where the arm pushes ONE block:
//     nlohmann throws type_error.306 the moment a consumer reads `["type"]`,
//     and that aborts the whole selftest binary. THIS BUNDLE HAS BOTH ARM
//     SHAPES, so the rule is per-handler, not per-file:
//       * `runGrc1leg`, `runCdfplot`, `runEventPlot` -- the arm hands the
//         result STRAIGHT to `registerGraphOutput` and then pushes it. Every
//         `return` in those three is ONE BLOCK OBJECT. Never an array.
//       * `runGeodist`, `runGeonear` -- the arm routes through
//         `pushEnvResult`, which flattens an array and drops nulls, so an
//         array of block objects is legal there and is how they carry a note
//         alongside their `mutationResult`.
//     The consequence for the three graph handlers is real and is why the
//     notes below live where they do: a graph arm CANNOT emit a separate
//     display block, so an "accepted but not applied" note is appended to the
//     block's own `note` string instead (see `appendBlockNote`).
//  2. RC. `eval::coarseRc` answers 199 only for `unsupported_command` or an
//     explicit `r(NNN)` reason, and 198 for `syntax_error`/
//     `unsupported_option`; a refusal built with the DEFAULT `engine_error`
//     code silently becomes rc 1. So a user syntax error here always carries
//     "syntax_error" (`gxSyntax`), a real Stata option this build does not do
//     goes through the reject net's `unimplemented` half, and rc 1 is used
//     only where rc 1 is the honest answer (the geonear pair cap: the command
//     exists, the syntax is right, this build will not do the work).
//  3. VOCABULARY. `rejectUnknownOptions` prefix-resolves, so it ACCEPTS an
//     abbreviation; `canonicalizeForCommand` -- the thing that makes a
//     handler's own `k == "generate"` comparison see that abbreviation -- is
//     driven by the vocabulary REGISTRY. A command absent from the registry
//     therefore gets its abbreviations accepted by the net and then dropped
//     unread, and a name in NEITHER list is accepted-and-ignored outright.
//     All SIX command tokens register at NAMESPACE scope below (a
//     function-local static constructs too late: after the first call that
//     needed it), including `grstyle`, whose arm has no net -- the empty
//     registration is what gives `canonicalizeForCommand` an answer for the
//     token and keeps the inventory complete.
//     AN OPTION THIS BUILD DOES NOT READ GOES IN `unimplemented`, NEVER IN
//     `known`. Two consequences worth stating because they look like
//     exceptions and are not:
//       * The COSMETIC options (`grc1leg2`'s ring/position/span/symxsize/
//         symysize/labsize, `event_plot`'s plottype/ciplottype/graph_opt/
//         noautolegend) ARE read -- read to build a note that names them and
//         says they were accepted and not applied. The user is told; nothing
//         is silently dropped. That is the plan's design decision 1
//         ("known-ignored + note once") made literal.
//       * `xcommon`/`ycommon` are read and IMPLEMENTED (a common axis range
//         across the combined panels), rather than parked in the accepted-
//         and-noted set, because an axis scale is a claim about the numbers.
//  4. DELEGATE MESSAGES. `runGrc1leg` forwards to `runGraphCombine` and
//     `runEventPlot` to `runCoefplot`. An error from either delegate names
//     the DELEGATE ("graph combine: no graph named 'g1'"), which is a command
//     the user never typed. `renameDelegateError` rewrites the prefix so the
//     message names what the user actually ran.
//  5. ECHOED PATHS. `geonear` takes a `using` file. Every message about it
//     echoes THE SPELLING THE USER TYPED; `resolveUserPath`'s output is used
//     to open the file and nowhere else.
//
// ONE MORE RULE, FROM THE PLAN AND WORTH ITS OWN LINE: `cdfplot`'s ECDF is
// bundle F's `cumul` rank rule, TRANSCRIBED (see `ecdfRanks`). A second
// percentile/rank rule in one product is a silently wrong number waiting for
// a fixture that does not exist.
#include "piet/display_hardening.h"
#include "piet/handlers.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <fstream>
#include <functional>
#include <limits>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <system_error>
#include <unordered_set>
#include <utility>
#include <vector>

#include "io/win_compat.h"  // cecon::to_native_path / native_unlink
#include "piet/cobra_runner.h"
#include "piet/column_fetch.h"
#include "piet/datastore.h"
#include "piet/eval/errors.h"
#include "piet/eval/pyvalue.h"  // pythonFloatRepr -- round-trippable CSV numerics
#include "piet/eval/session.h"
#include "piet/eval/tokenizer.h"
#include "piet/inrange_filter.h"
#include "piet/mutation_result.h"
#include "piet/scratch_seq.h"
#include "piet/session_state.h"
#include "piet/strutil.h"
#include "piet/twoway_opts.h"  // twoway::layerStyle -- the ONE layer-style rule

// ---------------------------------------------------------------------------
// OPTION VOCABULARIES -- namespace scope, before any dispatch (defect class 3).
//
// `known` is what the handler below actually READS; `unimplemented` names real
// Stata options of the same command this build does not do, so the net can say
// "valid Stata option, not yet implemented" instead of the ambiguous "not
// allowed". A name in NEITHER set reads as an unknown option, which is the
// wrong diagnosis for a real one; a name wrongly placed in `known` is accepted
// and silently ignored, which is the defect the apparatus exists to prevent.
namespace {

// grc1leg2. `legendfrom` picks the surviving legend; rows/cols/title/imargin/
// graphregion/name go straight through to `runGraphCombine`, which reads every
// one of them; `xcommon`/`ycommon` are implemented here; `saving` is honoured
// through `runGraphSave`; the six placement options are read to build the
// accepted-and-not-applied note. `holes()` (a grid with gaps) and `altshrink`
// (a different panel-shrink rule) would both change the drawn layout, and this
// build has neither, so they are refused by name rather than dropped.
// AER-SCOPE2 K1: `grc1leg` (Vince Wiggins' original) now routes here too, and
// the 12-paper corpus measures ITS option usage as caption(17) graphregion(24)
// imargin(21) iscale(19) legendfrom(2) note(18) position(19) row(1) rows(1)
// ycommon(22). Four of those ten -- caption, iscale, note, row -- were absent
// from the set below, and an absent option is r(198) uncaptured, which STOPS the
// do-file and shows up as a `stopped_at` RED in run_probe. They are added to the
// KNOWN set because they are layout/annotation options the combine path already
// carries or can ignore without changing a number; `row` is Stata's singular
// spelling of `rows`.
const piet::eval::OptVocab& kVocabGrc1leg = piet::eval::registerOptVocab(
    {"grc1leg2", "grc1leg"},
                  {{"legendfrom", "rows", "row", "cols", "title", "xcommon", "ycommon", "imargin",
                    "graphregion", "name", "saving", "ring", "position", "span", "symxsize",
                    "symysize", "labsize", "caption", "note", "iscale"},
                   {"holes", "altshrink"}});

// `grstyle` has no reject net at its arm (it accepts a note and returns), so
// this registration exists purely so `canonicalizeForCommand` has an answer for
// the token and the vocabulary inventory is complete -- the same reason
// handlers_aer_reshape.cpp registers `notes` and `compare` with empty sets.
const piet::eval::OptVocab& kVocabGrstyle =
    piet::eval::registerOptVocab({"grstyle"}, {{}, {}});

// geodist. Every one of these is read: gen()/generate() names the column,
// `sphere` selects haversine, `radius()` replaces the sphere's radius, `miles`
// converts the unit.
const piet::eval::OptVocab& kVocabGeodist = piet::eval::registerOptVocab(
    {"geodist"}, {{"generate", "gen", "miles", "sphere", "radius"}, {}});

// geonear. `within()` (a distance cutoff), `long` (a reshaped long result) and
// `report` (a diagnostic table) are real geonear options that would each need a
// different result SHAPE, not a different number, so they are refused by name.
const piet::eval::OptVocab& kVocabGeonear = piet::eval::registerOptVocab(
    {"geonear"}, {{"neighbors", "nearcount", "miles", "ellipsoid", "genstub"},
                  {"within", "long", "report"},
                  // BUG-430: the minimum abbreviations geonear.ado DECLARES
                  // (`Neighbors`, `NEArcount`, `MIles`, `Ellipsoid`, `Genstub`,
                  // `RAdius`, `Ignoreself`, `REport`, `WIDe` -- the capitalised
                  // stem is the minimum). Only the ones the prefix rule cannot
                  // produce are listed: `mi`/`nea`/`gen` already resolve as
                  // unique prefixes, while `n` prefixes BOTH neighbors and
                  // nearcount and `e`/`g` are below the two-character floor.
                  {{"n", "neighbors"},
                   {"e", "ellipsoid"},
                   {"g", "genstub"},
                   {"i", "ignoreself"},
                   {"ra", "radius"},
                   {"re", "report"},
                   {"wid", "within"}}});

// cdfplot. `normal`/`exponential` overlay a fitted reference CDF and `xlog` is
// a log x scale; all three are real plot content this build does not draw, and
// drawing the plot without them is answering a different question, so they are
// refused rather than ignored.
// cdfplot.ado 1.04 hands every option it does not own to its `twoway ...,
// `goptions'`, so the graph-level twoway options PIET's twoway takes are known
// here too (StataBE 19.5, BUG-1521: `cdfplot x, nodraw`, xlabel(), scheme(),
// ysize(), xscale(), note(), graphregion(), subtitle(), caption(), xline(),
// plotregion(), aspectratio() are rc 0; a plot option such as lcolor() or
// msymbol() is twoway's r(198) "option lcolor() not allowed"). runCdfplot names
// the ones it does not draw. xti/yti are twoway's spellings of xtitle/ytitle,
// which xtick/ytick would otherwise make ambiguous.
const piet::eval::OptVocab& kVocabCdfplot = piet::eval::registerOptVocab(
    {"cdfplot"}, {{"by", "title", "xtitle", "ytitle", "ylabel", "legend", "name", "saving",
                   "nodraw", "subtitle", "note", "caption", "ttitle", "xlabel", "tlabel", "xmlabel",
                   "ymlabel", "tmlabel", "xtick", "ytick", "ttick", "xmtick", "ymtick", "xscale",
                   "yscale", "tscale", "xline", "yline", "tline", "scheme", "graphregion",
                   "plotregion", "xsize", "ysize", "fxsize", "fysize", "aspectratio", "scale", "text",
                   "ttext"},
                  {"normal", "exponential", "xlog"},
                  {{"xti", "xtitle"}, {"yti", "ytitle"}, {"xlabels", "xlabel"}, {"ylabels", "ylabel"}}});

// event_plot. `alpha()` becomes coefplot's `level()`; `stub_lag`/`stub_lead`
// select and order the coefficients; `together` and the four cosmetic options
// are read to build the note. `shift`/`trimlag`/`trimlead` change WHICH
// coefficients are plotted and `savecoef`/`perturb` change what is stored --
// all five are numbers-or-content, not cosmetics, so they are refused by name.
const piet::eval::OptVocab& kVocabEventPlot = piet::eval::registerOptVocab(
    {"event_plot"}, {{"stub_lag", "stub_lead", "together", "plottype", "ciplottype", "graph_opt",
                      "noautolegend", "alpha", "name"},
                     {"shift", "trimlag", "trimlead", "savecoef", "perturb"}});

}  // namespace

namespace piet::handlers {

using ojson = nlohmann::ordered_json;

// Defined in graph_m4.cpp: the legend block twoway builds from legend() text
// (quote- and paren-aware; `off` only as a standalone top-level token);
// keyText(i) is the text of plot i's key (nullopt: no such plot).
ojson legendBlock(const std::string& legendText,
                  const std::function<std::optional<std::string>(int64_t)>& keyText, bool* hasOrder);

namespace {

// `grstyle`'s registration is consumed by no net in this TU (its arm accepts
// and returns); naming it here keeps -Wunused-variable honest about that.
[[maybe_unused]] const piet::eval::OptVocab* const kVocabTouch[] = {&kVocabGrstyle};

// ===========================================================================
// Errors and blocks
// ===========================================================================

ojson gxError(const std::string& message, const std::string& code = "engine_error") {
  return ojson(eval::engineError(message, code));
}

// A USER SYNTAX error. "syntax_error" is what coarseRc maps to rc 198; the
// default "engine_error" maps to rc 1, which is defect class 2.
ojson gxSyntax(const std::string& message) {
  return ojson(eval::engineError(message, "syntax_error"));
}

bool isErrorBlock(const ojson& b) {
  return b.is_object() && b.value("type", std::string()) == "error";
}

ojson noteBlock(const std::string& text) {
  ojson b;
  b["type"] = "display";
  b["class"] = "note";
  b["text"] = text;
  return b;
}

// Defect class 4. `runGraphCombine`/`runCoefplot` build their messages around
// their OWN command name; a user who typed `grc1leg2` must not be told what
// `graph combine` thinks. Rewrites the leading "<delegate>: " (or
// "<delegate>. ") prefix to the typed command, and prepends the typed name when
// the message does not start with the delegate's at all, so no error can escape
// this file naming a command the user did not run.
ojson renameDelegateError(const ojson& block, const std::string& typedCmd,
                          const std::string& delegate) {
  if (!isErrorBlock(block)) return block;
  std::string msg = block.value("message", std::string());
  const std::string pfx = delegate + ": ";
  if (msg.rfind(pfx, 0) == 0) {
    msg = typedCmd + ": " + msg.substr(pfx.size());
  } else if (msg.rfind(typedCmd + ":", 0) != 0) {
    msg = typedCmd + ": " + msg;
  }
  std::string code = block.value("code", std::string("engine_error"));
  ojson out = ojson(eval::engineError(msg, code));
  // The delegate may have carried an exact rc through the `reason` channel
  // (BUG-187); that outranks coarseRc's message rules and must survive the
  // rename, or a 198 silently becomes a 1.
  if (block.contains("reason") && !block["reason"].is_null()) out["reason"] = block["reason"];
  return out;
}

// THE ONE PLACE A GRAPH HANDLER CAN PUT A NOTE. The three graph arms push
// exactly one block (defect class 1), so a note cannot be a sibling display
// block the way `geodist`'s is. It is appended to the block's own `note` string
// -- a key the twoway and graph_combine renderers already draw. NOTE, HONESTLY:
// the `coefplot` renderer (graph_m4.cpp's buildSvgCoefplot, and graph.js's
// coefplot branch) does NOT draw `note` today, so `event_plot`'s note reaches
// the block JSON and not the drawn figure. Making it visible means touching the
// renderer, which is outside this bundle's manifest; it is reported rather than
// done.
void appendBlockNote(ojson& block, const std::string& text) {
  if (!block.is_object()) return;
  std::string cur;
  if (block.contains("note") && block["note"].is_string()) cur = block["note"].get<std::string>();
  block["note"] = cur.empty() ? text : (cur + "  " + text);
}

// ===========================================================================
// cobra plumbing
// ===========================================================================

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

// Run ONE cobra verb. nullopt = success; otherwise the error BLOCK to return.
// Never a bare parse of stdout_text and never a swallowed rc (IDIOMS).
std::optional<ojson> runVerb(const std::vector<std::string>& args) {
  CobraRunResult cr = runCobra(args);
  if (cr.ok) return std::nullopt;
  CobraError ce = errorFromRaw(cr);
  return gxError(cobraErrorMessage(ce), ce.code);
}

// ===========================================================================
// Reading
// ===========================================================================

// The shared READ shape: ask the worker for a contract_A section-38 projection
// of just the columns this command needs, and fall back to the session's own
// materialized head whenever the projection is unavailable. A miss must cost a
// fallback, never an answer -- column_fetch.h's own law.
class ReadFrame {
 public:
  ReadFrame(SessionState& session, const std::vector<std::string>& cols) {
    try {
      if (!cols.empty()) {
        std::vector<const std::string*> exprs;
        exprs.reserve(cols.size());
        for (const auto& c : cols) exprs.push_back(&c);
        if (auto want = referencedColumns(session, exprs, "")) {
          std::string projPath;
          owned_ = fetchColumns(session, *want, session.workdir(), &projPath);
        }
      }
      ds_ = owned_ ? owned_.get() : session.datastore();
    } catch (const std::exception& e) {
      ds_ = nullptr;
      why_ = e.what();
    }
  }
  Datastore* get() const { return ds_; }
  const std::string& why() const { return why_; }

 private:
  std::unique_ptr<Datastore> owned_;
  Datastore* ds_ = nullptr;
  std::string why_;
};

// ===========================================================================
// Small text / number helpers
// ===========================================================================

std::string lowerOf(std::string s) { return strutil::lower(std::move(s)); }
std::string trimOf(const std::string& s) { return strutil::trim(s); }

std::string dequote(const std::string& s) {
  if (s.size() >= 2 && ((s.front() == '"' && s.back() == '"') ||
                        (s.front() == '\'' && s.back() == '\''))) {
    return s.substr(1, s.size() - 2);
  }
  return s;
}

std::vector<std::string> splitWs(const std::string& s) {
  std::vector<std::string> out;
  std::istringstream iss(s);
  std::string tok;
  while (iss >> tok) out.push_back(tok);
  return out;
}

std::string joinWith(const std::vector<std::string>& v, const std::string& sep) {
  std::string out;
  for (size_t i = 0; i < v.size(); ++i) {
    if (i) out += sep;
    out += v[i];
  }
  return out;
}

// STRICT-CONSUME numeric parsing (IDIOMS, C1-F11): the WHOLE token must parse,
// and C's hex forms are rejected. A bare std::stod lies about `5foo` (-> 5),
// which here would turn a mistyped variable name into a silently plausible
// latitude.
bool parseStrictDouble(const std::string& sIn, double* out) {
  const std::string s = trimOf(sIn);
  if (s.empty()) return false;
  if (s.find('x') != std::string::npos || s.find('X') != std::string::npos) return false;
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

bool parseStrictInt(const std::string& s, long long* out) {
  double d = 0.0;
  if (!parseStrictDouble(s, &d)) return false;
  if (d != std::floor(d)) return false;
  *out = static_cast<long long>(d);
  return true;
}

const eval::OptValue* findOpt(const std::vector<std::pair<std::string, eval::OptValue>>& opts,
                              const std::string& key) {
  for (const auto& [k, v] : opts) {
    if (k == key) return &v;
  }
  return nullptr;
}

bool hasFlag(const std::vector<std::pair<std::string, eval::OptValue>>& opts,
             const std::string& key) {
  const eval::OptValue* v = findOpt(opts, key);
  if (v == nullptr) return false;
  // A bare flag parses as isBool/true; a valued spelling still means "given".
  return v->isBool ? v->boolValue : true;
}

std::optional<std::string> optStr(const std::vector<std::pair<std::string, eval::OptValue>>& opts,
                                  const std::string& key) {
  const eval::OptValue* v = findOpt(opts, key);
  if (v == nullptr || v->isBool) return std::nullopt;
  return trimOf(dequote(trimOf(v->strValue)));
}

std::optional<std::string> optStrAny(
    const std::vector<std::pair<std::string, eval::OptValue>>& opts,
    std::initializer_list<const char*> keys) {
  for (const char* k : keys) {
    if (auto v = optStr(opts, k)) return v;
  }
  return std::nullopt;
}

// The names among `keys` that are present at all, in the order listed -- the
// input to every "accepted and not applied" note in this file.
std::vector<std::string> presentOpts(
    const std::vector<std::pair<std::string, eval::OptValue>>& opts,
    std::initializer_list<const char*> keys) {
  std::vector<std::string> out;
  for (const char* k : keys) {
    if (findOpt(opts, k) != nullptr) out.emplace_back(k);
  }
  return out;
}

// A numeric cell: BLANK for missing (`Frame::read_csv_column` reads an empty
// cell as missing), else the shortest round-trippable decimal, so the value
// that comes back is bit-identical. Missing is `.`, never 0 -- in every cell
// this file writes.
std::string numCell(double v) {
  if (std::isnan(v)) return std::string();
  return eval::pythonFloatRepr(v);
}

// A number the way a legend labels it: an integral value loses its decimal.
std::string levelText(double v) {
  if (std::isnan(v)) return ".";
  if (v == std::floor(v) && std::fabs(v) < 1e15) return std::to_string(static_cast<long long>(v));
  return eval::pythonFloatRepr(v);
}

// ===========================================================================
// Column reading / writing
// ===========================================================================

std::vector<double> columnAsDoubles(const Datastore& ds, const std::string& name) {
  const int64_t n = ds.nRows();
  std::vector<double> out(static_cast<size_t>(n), std::numeric_limits<double>::quiet_NaN());
  const cecon::Column& c = ds.col(name);
  if (c.dtype == cecon::DType::Factor) return out;  // callers reject factors first
  const double* d = c.f64();
  for (int64_t i = 0; i < n; ++i) out[static_cast<size_t>(i)] = d[i];
  return out;
}

// A column's values as DISPLAY strings ("" = missing).
std::vector<std::string> columnAsStrings(const Datastore& ds, const std::string& name) {
  std::vector<std::string> out;
  const int64_t n = ds.nRows();
  out.reserve(static_cast<size_t>(n));
  const cecon::Column& c = ds.col(name);
  if (c.dtype == cecon::DType::Factor) {
    const int32_t* codes = c.codes();
    for (int64_t i = 0; i < n; ++i) {
      const int32_t code = codes[i];
      if (code < 0 || static_cast<size_t>(code) >= c.levels.size())
        out.emplace_back();
      else
        out.push_back(c.levels[static_cast<size_t>(code)]);
    }
  } else {
    const double* d = c.f64();
    for (int64_t i = 0; i < n; ++i) out.push_back(numCell(d[i]));
  }
  return out;
}

// The one-column HEADERLESS payload `cobra setcol` reads: one raw value per
// row, row count == n_rows exactly. String cells are quoted even when they do
// not need it, so a value's own padding survives the trip (BUG-292); an EMPTY
// cell is a bare blank line, the only spelling read_csv_column accepts for a
// missing factor cell.
bool writePayloadCsv(const std::string& path, const std::vector<std::string>& cells, bool quoted) {
  std::ofstream f(cecon::to_native_path(path));
  if (!f) return false;
  for (const auto& c : cells) {
    if (c.empty()) {
      f << "\n";
      continue;
    }
    if (quoted) {
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
  return static_cast<bool>(f);
}

// Append ONE new column to `basePath` from a payload written here. `cells` are
// the rendered values ("" == missing); `dtype` is "f64" or "factor".
std::optional<ojson> appendColumn(SessionState& session, const std::string& basePath,
                                  const std::string& var, const std::vector<std::string>& cells,
                                  const std::string& dtype, std::string* outPath) {
  const std::string csvPath = nextPath(session.workdir(), "gxvals") + ".csv";
  registerScratchMint(csvPath);  // BUG-389
  if (!writePayloadCsv(csvPath, cells, dtype == "factor")) {
    cecon::native_unlink(csvPath);
    return gxError("could not write the column payload (disk full or I/O error)", "data_io");
  }
  const std::string out = nextPath(session.workdir(), "gxsetcol");
  if (auto err = runVerb({"setcol", "--data", basePath, "--var", var, "--values", csvPath,
                          "--dtype", dtype, "--new", "--out", out})) {
    cecon::native_unlink(out);
    return err;
  }
  *outPath = out;
  return std::nullopt;
}

// A new variable name has to be legal AND free. Both checks, once, so every
// handler in this file refuses the same things for the same reasons.
ojson newVarGuard(SessionState& session, const std::string& verb, const std::string& newvar) {
  if (!strutil::isValidStataIdentifier(newvar))
    return gxSyntax(verb + ": invalid variable name `" + newvar + "' — " +
                    strutil::stataIdentifierRuleText());
  // BUG-1120: nor is a reserved name one (`geodist ..., gen(_cons)' is "_cons
  // invalid varname" r(198), StataBE 19.5).
  if (strutil::isReservedVarName(newvar))
    return ojson(eval::errorBlock("syntax_error", newvar + " invalid varname", std::nullopt,
                                  std::nullopt, std::nullopt, "r(198)"));
  Datastore* ds = nullptr;
  try {
    ds = session.datastore();
  } catch (const std::exception&) {
    ds = nullptr;
  }
  if (ds != nullptr && ds->has(newvar))
    return gxError(verb + ": variable " + newvar + " already defined", "bad_input");
  return ojson();  // null => no objection
}

// ===========================================================================
// `if`/`in` as a ROW MASK (geodist only)
// ===========================================================================
//
// The registry gives `geodist` allowIf/allowInRange, and it must: Stata's
// geodist takes a qualifier, and the honest reading of `geodist ... if x>0,
// gen(d)` is "compute d where the condition holds, leave it MISSING elsewhere"
// -- the same rule `generate ... if` follows. That needs a ROW-ALIGNED MASK
// over the full frame, not a filtered frame: the column written by
// `setcol --new` has to be exactly as long as the dataset.
//
// `applyIfAndInFilter` (inrange_filter.h) is the shared helper for this
// composition and `cdfplot` below uses it exactly as designed -- but it DROPS
// its materialized row-index column before returning, which is the one thing a
// mask needs. So this is a deliberate small private copy of the same 2-call
// shape (`--bygen IDX=_n` then `--keepif`), with its OWN index-column name so a
// leaked column can be attributed to the command that leaked it, reusing the
// shared `parseInRangeBounds` grammar and the shared `inRangeRejectError`
// wording so a user sees the same two sentences whichever command's `in`
// failed. It runs ONLY when a qualifier is present: no qualifier costs zero
// cobra calls.
constexpr const char* kRowIdx = "__piet_gx_row";

struct MaskResult {
  bool ok = true;
  bool present = false;    // a qualifier was given
  std::vector<char> keep;  // size == nRows, valid iff present && ok
  ojson err;               // valid iff !ok
};

MaskResult qualifierMask(SessionState& session, const std::string& verb,
                         const std::string& basePath, const eval::ParsedCommand& parsed,
                         int64_t nRows) {
  MaskResult r;
  const bool hasIf = parsed.ifExpr.has_value() && !parsed.ifExpr->empty();
  const bool hasIn = parsed.inRange.has_value() && !parsed.inRange->empty();
  if (!hasIf && !hasIn) return r;
  r.present = true;

  std::string pred;
  if (hasIn) {
    InRangeBounds b = parseInRangeBounds(*parsed.inRange, nRows);
    if (!b.ok) {
      InRangeFilterResult f;
      f.ok = false;
      f.negative = b.negative;
      r.ok = false;
      r.err = inRangeRejectError(f, *parsed.inRange, verb);
      return r;
    }
    pred = std::string(kRowIdx) + ">=" + std::to_string(b.a) + " & " + std::string(kRowIdx) +
           "<=" + std::to_string(b.b);
  }
  if (hasIf) pred = pred.empty() ? *parsed.ifExpr : ("(" + pred + ") & (" + *parsed.ifExpr + ")");

  const std::string idxPath = nextPath(session.workdir(), "gxidx");
  if (auto e = runVerb({"eval", "--data", basePath, "--bygen", std::string(kRowIdx) + "=_n",
                        "--out", idxPath})) {
    r.ok = false;
    r.err = *e;
    return r;
  }
  const std::string keptPath = nextPath(session.workdir(), "gxkeep");
  if (auto e = runVerb({"eval", "--data", idxPath, "--keepif", pred, "--out", keptPath})) {
    r.ok = false;
    r.err = *e;
    return r;
  }
  r.keep.assign(static_cast<size_t>(nRows), 0);
  try {
    Datastore kept(keptPath);
    if (!kept.has(kRowIdx)) {
      r.ok = false;
      r.err = gxError(verb + ": internal — the filtered frame lost its row index.", "internal");
      return r;
    }
    const std::vector<double> idx = columnAsDoubles(kept, kRowIdx);
    for (double v : idx) {
      if (std::isnan(v)) continue;
      const long long i = static_cast<long long>(v);
      if (i >= 1 && i <= nRows) r.keep[static_cast<size_t>(i - 1)] = 1;
    }
  } catch (const std::exception& e) {
    r.ok = false;
    r.err = gxError(verb + ": could not read the filtered rows — " + e.what(), "data_io");
    return r;
  }
  return r;
}

// ===========================================================================
// GEODESY
// ===========================================================================
//
// Design decision 2, verbatim: the DEFAULT is Vincenty's inverse solution on
// the WGS84 ellipsoid (a = 6378137 m, f = 1/298.257223563, convergence 1e-12
// on lambda, at most 200 iterations); `sphere` is the haversine great circle
// with R = 6371 km (replaceable by `radius(#)`); `miles` DIVIDES by 1.609344,
// which is what the package writes (see kKmPerMileDivisor).
//
// A NON-CONVERGING PAIR IS MISSING, NEVER A NUMBER. Vincenty's iteration does
// not converge for (near-)antipodal pairs; the classic failure mode of a naive
// port is to fall out of the loop and return whatever the last iterate implied,
// which is a plausible-looking wrong distance. Here the loop reports its own
// failure, the row gets `.`, and the command says how many rows that happened
// to.
//
// MEASURED, not assumed (this file's author ran the arithmetic before writing
// the handler): Seoul (37.5, 127.0) to Busan (35.1, 129.0) is
// 321.218659806 km on this implementation. Haversine with R = 6371 gives
// 321.447921772 km (0.0714% apart), and a plain equirectangular approximation
// gives 321.3 km, so 321.2 is the right neighbourhood and the plan's "≈325.0
// km" Done-When figure is wrong. See this bundle's report; the pin belongs at
// 321.2187, not 325.0.
constexpr double kPi = 3.14159265358979323846;
// 6371 EXACTLY, not the IUGG mean 6371.0088. Read out of the packages
// themselves on 2026-09-04: `geodist` and `geonear` both write
// `local radius 6371` / `scalar R = 6371`. The plan drafted the IUGG value, and
// on the Seoul-Busan pair the two radii differ by 0.000444 km -- far too small
// to see in any rendered output and exactly the kind of constant that gets
// frozen into a golden and never questioned again. The package's number is the
// contract; the more "correct" one would be a silent parity break.
constexpr double kSphereRadiusKm = 6371.0;
// BUG-463 (2026-09-09): this was `0.621371192`, a TRUNCATED reciprocal, and
// geodist writes `local km_to_miles / 1.609344` -- a DIVISION by the
// exact international-mile definition, used at :160, :169 and :294. The two
// differ at relative 3.8e-11: on the Seoul-Busan pair StataBE answers
// 199.59602161268270 and the truncated constant gave 199.59602153644656.
// Inside TOLERANCE.md's rtol 1e-9, so no gate saw it -- and invisible in any
// %9.0g rendering, which is how a wrong low digit survives. The divisor is the
// package's own contract, so divide, and keep the digits it keeps.
constexpr double kKmPerMileDivisor = 1.609344;

double degToRad(double d) { return d * kPi / 180.0; }

// nullopt == did not converge.
std::optional<double> vincentyKm(double lat1, double lon1, double lat2, double lon2) {
  const double a = 6378137.0;              // WGS84 semi-major axis, metres
  const double f = 1.0 / 298.257223563;    // WGS84 flattening
  const double b = (1.0 - f) * a;          // semi-minor axis
  const double L = degToRad(lon2 - lon1);
  const double U1 = std::atan((1.0 - f) * std::tan(degToRad(lat1)));
  const double U2 = std::atan((1.0 - f) * std::tan(degToRad(lat2)));
  const double sinU1 = std::sin(U1), cosU1 = std::cos(U1);
  const double sinU2 = std::sin(U2), cosU2 = std::cos(U2);

  double lambda = L;
  double sinSigma = 0.0, cosSigma = 0.0, sigma = 0.0, cos2Alpha = 0.0, cos2SigmaM = 0.0;
  int it = 0;
  for (; it < 200; ++it) {
    const double sinL = std::sin(lambda), cosL = std::cos(lambda);
    const double t1 = cosU2 * sinL;
    const double t2 = cosU1 * sinU2 - sinU1 * cosU2 * cosL;
    sinSigma = std::sqrt(t1 * t1 + t2 * t2);
    if (sinSigma == 0.0) return 0.0;  // coincident points
    cosSigma = sinU1 * sinU2 + cosU1 * cosU2 * cosL;
    sigma = std::atan2(sinSigma, cosSigma);
    const double sinAlpha = cosU1 * cosU2 * sinL / sinSigma;
    cos2Alpha = 1.0 - sinAlpha * sinAlpha;
    // Equatorial line: cos^2(alpha) == 0 leaves cos(2 sigma_m) undefined;
    // Vincenty's own note sets it to 0 there.
    cos2SigmaM = (cos2Alpha == 0.0) ? 0.0 : cosSigma - 2.0 * sinU1 * sinU2 / cos2Alpha;
    const double C = f / 16.0 * cos2Alpha * (4.0 + f * (4.0 - 3.0 * cos2Alpha));
    const double lambdaPrev = lambda;
    lambda = L + (1.0 - C) * f * sinAlpha *
                     (sigma + C * sinSigma *
                                  (cos2SigmaM +
                                   C * cosSigma * (-1.0 + 2.0 * cos2SigmaM * cos2SigmaM)));
    if (std::fabs(lambda - lambdaPrev) < 1e-12) break;
  }
  if (it >= 200) return std::nullopt;  // (near-)antipodal: no answer, not a guess

  const double u2 = cos2Alpha * (a * a - b * b) / (b * b);
  const double A =
      1.0 + u2 / 16384.0 * (4096.0 + u2 * (-768.0 + u2 * (320.0 - 175.0 * u2)));
  const double B = u2 / 1024.0 * (256.0 + u2 * (-128.0 + u2 * (74.0 - 47.0 * u2)));
  const double dSigma =
      B * sinSigma *
      (cos2SigmaM +
       B / 4.0 *
           (cosSigma * (-1.0 + 2.0 * cos2SigmaM * cos2SigmaM) -
            B / 6.0 * cos2SigmaM * (-3.0 + 4.0 * sinSigma * sinSigma) *
                (-3.0 + 4.0 * cos2SigmaM * cos2SigmaM)));
  return b * A * (sigma - dSigma) / 1000.0;  // metres -> km
}

double haversineKm(double lat1, double lon1, double lat2, double lon2, double radiusKm) {
  const double dLat = degToRad(lat2 - lat1);
  const double dLon = degToRad(lon2 - lon1);
  const double s1 = std::sin(dLat / 2.0), s2 = std::sin(dLon / 2.0);
  double h = s1 * s1 + std::cos(degToRad(lat1)) * std::cos(degToRad(lat2)) * s2 * s2;
  if (h > 1.0) h = 1.0;  // rounding guard: asin's domain
  if (h < 0.0) h = 0.0;
  return 2.0 * radiusKm * std::asin(std::sqrt(h));
}

// How a pair of coordinates becomes ONE number, for BOTH geo verbs -- one
// function, so `geodist` and `geonear` cannot answer the same question with
// different arithmetic.
struct GeoMode {
  bool sphere = false;           // haversine instead of Vincenty
  double radiusKm = kSphereRadiusKm;
  bool miles = false;
};

struct GeoOutcome {
  double value = std::numeric_limits<double>::quiet_NaN();
  bool badLatitude = false;   // |lat| > 90
  bool noConverge = false;    // Vincenty gave up
};

GeoOutcome geoDistance(double lat1, double lon1, double lat2, double lon2, const GeoMode& mode) {
  GeoOutcome r;
  if (std::isnan(lat1) || std::isnan(lon1) || std::isnan(lat2) || std::isnan(lon2)) return r;
  // Edge case from the plan: a latitude outside [-90, 90] is not a place. It
  // becomes missing plus a note -- never a number the formula would happily
  // produce from a transposed lat/lon pair.
  if (std::fabs(lat1) > 90.0 || std::fabs(lat2) > 90.0) {
    r.badLatitude = true;
    return r;
  }
  double km;
  if (mode.sphere) {
    km = haversineKm(lat1, lon1, lat2, lon2, mode.radiusKm);
  } else {
    auto v = vincentyKm(lat1, lon1, lat2, lon2);
    if (!v.has_value()) {
      r.noConverge = true;
      return r;
    }
    km = *v;
  }
  r.value = mode.miles ? km / kKmPerMileDivisor : km;
  return r;
}

// `sphere` / `radius()` / `miles`, read once for both verbs. Returns an error
// block through `err` when radius() is not a positive number.
bool readGeoMode(const std::vector<std::pair<std::string, eval::OptValue>>& opts,
                 const std::string& verb, bool allowSphere, GeoMode* mode, ojson* err) {
  if (allowSphere) mode->sphere = hasFlag(opts, "sphere");
  mode->miles = hasFlag(opts, "miles");
  if (auto rad = optStr(opts, "radius")) {
    double v = 0.0;
    if (!parseStrictDouble(*rad, &v) || !(v > 0.0)) {
      *err = gxSyntax(verb + ": radius() must be a positive number of kilometres, got `" + *rad +
                      "'.");
      return false;
    }
    mode->radiusKm = v;
    mode->sphere = true;  // a radius is a sphere's radius; the ellipsoid has none
  }
  return true;
}

// ===========================================================================
// THE ECDF RANK RULE -- TRANSCRIBED FROM BUNDLE F's `cumul`
// ===========================================================================
//
// This is `runCumul`'s ranking (handlers_aer_reshape.cpp), copied rather than
// called: `runCumul` is a whole command -- it validates its own varlist, writes
// a column through `setcol --new` and returns a `mutationResult` -- so there is
// no function to call for the ranks alone, and hoisting one out of another
// bundle's TU is outside this write manifest. The rule is therefore duplicated
// DELIBERATELY and identically: rank ascending with a stable sort so a tie
// keeps the rows' ORIGINAL order, then y = rank/N. `cumul`'s `equal` variant
// (every member of a tied block takes the block's highest rank) is NOT used
// here, because cdfplot has no `equal` option to select it.
//
// N/N == 1.0 exactly in IEEE double for every N, so the LAST point of every
// series this produces is exactly 1.0 -- not (n-1)/n and not n/(n+1), the two
// off-by-ones that look perfectly plausible on a chart.
//
// If `runCumul`'s rule ever changes, this must change with it.
std::vector<std::pair<double, double>> ecdfPoints(const std::vector<double>& xs) {
  std::vector<size_t> order;
  order.reserve(xs.size());
  for (size_t i = 0; i < xs.size(); ++i)
    if (!std::isnan(xs[i])) order.push_back(i);
  std::stable_sort(order.begin(), order.end(),
                   [&](size_t a, size_t b) { return xs[a] < xs[b]; });
  const double nn = static_cast<double>(order.size());
  std::vector<std::pair<double, double>> pts;
  pts.reserve(order.size());
  for (size_t r = 0; r < order.size(); ++r) {
    pts.emplace_back(xs[order[r]], static_cast<double>(r + 1) / nn);
  }
  return pts;
}

// ===========================================================================
// saving(FILE[, replace]) -- one implementation, two graph handlers
// ===========================================================================
//
// `saving()` is in both vocabularies' `known` half, so it has to actually save.
// It routes through `runGraphSave`, which owns the `.gph` format (a JSON
// sidecar under `{"_magic":"PIETGPH1","block":...}`) -- writing those bytes a
// second time here would be a second format writer. `runGraphSave` reads
// `session.lastGraph`, so the caller installs the FINISHED block there first.
// Returns the error block on failure; `runGraphSave`'s own success note is
// discarded, because these arms push exactly one block and that block is the
// graph.
std::optional<ojson> honourSaving(SessionState& session, const std::string& verb,
                                  const std::vector<std::pair<std::string, eval::OptValue>>& opts,
                                  const ojson& block) {
  auto raw = optStr(opts, "saving");
  if (!raw.has_value() || raw->empty()) return std::nullopt;
  // `saving("f.gph", replace)` -- split the filename from its suboptions.
  std::string spec = *raw;
  bool replace = false;
  const size_t comma = spec.find(',');
  if (comma != std::string::npos) {
    for (const auto& w : splitWs(spec.substr(comma + 1))) {
      if (lowerOf(w) == "replace") replace = true;
    }
    spec = trimOf(spec.substr(0, comma));
  }
  spec = trimOf(dequote(trimOf(spec)));
  if (spec.empty()) return gxSyntax(verb + ": saving() needs a file name.");

  session.lastGraph = block;
  std::vector<std::pair<std::string, eval::OptValue>> saveOpts;
  if (replace) saveOpts.emplace_back("replace", eval::OptValue::ofBool(true));
  ojson res = ojson(runGraphSave(std::nullopt, spec, saveOpts, session));
  if (isErrorBlock(res)) return renameDelegateError(res, verb, "graph save");
  return std::nullopt;
}

// The empty option list every layer built in this file styles itself against:
// `twoway::layerStyle` picks the palette colour for the layer index when no
// colour option is present, which is exactly what a cdfplot series wants.
const std::vector<std::pair<std::string, eval::OptValue>> kNoLayerOpts;

}  // namespace

// ===========================================================================
// grc1leg2
// ===========================================================================
//
// Design decision 1: build the ordinary `graph combine` block, then switch off
// every panel's legend except the one `legendfrom()` names (default: the first
// panel). `piet/frontend/js/graph.js:795` already checks `legend.off` before it
// draws a legend, so this needs no frontend change -- which is the entire
// reason the alternative (merging the panels' legends into one combined legend
// the way real grc1leg does) was rejected: that is a renderer change, and the
// renderer is outside this bundle.
//
// The visible difference from real grc1leg: the surviving legend stays INSIDE
// its own panel rather than moving to the combined graph's margin, so
// ring()/position()/span() and the symbol-size options cannot be honoured.
// They are read and named in a note rather than dropped.
ojson runGrc1leg(const eval::ParsedCommand& parsed, SessionState& session) {
  const std::string kCmd = "grc1leg2";
  if (auto err = eval::rejectUnknownOptions(parsed.opts, kVocabGrc1leg.known,
                                            kVocabGrc1leg.unimplemented))
    return ojson(*err);
  if (parsed.varlist.empty())
    return gxSyntax(kCmd + ": syntax is `" + kCmd +
                    " name1 name2 [...] [, legendfrom(name) rows(#) cols(#) title(...)]`");

  ojson result = ojson(runGraphCombine(parsed.varlist, parsed.opts, session));
  if (isErrorBlock(result)) return renameDelegateError(result, kCmd, "graph combine");
  if (!result.is_object() || !result.contains("panels") || !result["panels"].is_array()) {
    // Defensive: runGraphCombine returns either an error or a combine block.
    return gxError(kCmd + ": internal — `graph combine` returned no panels.", "internal");
  }

  ojson& panels = result["panels"];
  const size_t nPanels = panels.size();

  // Which panel keeps its legend. Default = the first, per the plan.
  size_t keepIdx = 0;
  if (auto lf = optStr(parsed.opts, "legendfrom")) {
    const std::string want = trimOf(dequote(trimOf(*lf)));
    bool found = false;
    for (size_t i = 0; i < parsed.varlist.size() && i < nPanels; ++i) {
      const std::string tok = trimOf(dequote(trimOf(parsed.varlist[i])));
      if (tok == want) {
        keepIdx = i;
        found = true;
        break;
      }
    }
    if (!found) {
      // A panel may also be identified by the name stored INSIDE its block
      // (`graph save`'s registered name), which is not always the token typed.
      for (size_t i = 0; i < nPanels; ++i) {
        if (panels[i].is_object() && panels[i].value("name", std::string()) == want) {
          keepIdx = i;
          found = true;
          break;
        }
      }
    }
    if (!found) {
      return gxError(kCmd + ": legendfrom() names `" + want +
                     "', which is not one of the combined graphs: " +
                     joinWith(parsed.varlist, " ") + ".");
    }
  }

  // The one real edit: every OTHER panel loses its legend. The kept panel is
  // left exactly as it was -- if the user had already put `legend(off)` on it,
  // grc1leg2 does not turn it back on.
  for (size_t i = 0; i < nPanels; ++i) {
    if (i == keepIdx) continue;
    if (!panels[i].is_object()) continue;
    panels[i]["legend"]["off"] = true;
  }
  result["legend_from"] = static_cast<int64_t>(keepIdx);

  // xcommon/ycommon: one axis range shared by every panel that has one. This
  // is READ AND APPLIED rather than accepted-and-noted, because an axis scale
  // is a claim about the numbers, not a cosmetic.
  const bool xcommon = hasFlag(parsed.opts, "xcommon");
  const bool ycommon = hasFlag(parsed.opts, "ycommon");
  if (xcommon || ycommon) {
    auto unify = [&](const char* loKey, const char* hiKey) {
      double lo = std::numeric_limits<double>::infinity();
      double hi = -std::numeric_limits<double>::infinity();
      bool any = false;
      for (const auto& p : panels) {
        if (!p.is_object() || !p.contains(loKey) || !p.contains(hiKey)) continue;
        if (!p[loKey].is_number() || !p[hiKey].is_number()) continue;
        const double a = p[loKey].get<double>(), b = p[hiKey].get<double>();
        if (!std::isfinite(a) || !std::isfinite(b)) continue;
        lo = std::min(lo, a);
        hi = std::max(hi, b);
        any = true;
      }
      if (!any) return;
      for (auto& p : panels) {
        if (!p.is_object() || !p.contains(loKey) || !p.contains(hiKey)) continue;
        if (!p[loKey].is_number() || !p[hiKey].is_number()) continue;
        p[loKey] = lo;
        p[hiKey] = hi;
      }
    };
    if (xcommon) unify("x_min", "x_max");
    if (ycommon) unify("y_min", "y_max");
  }

  // Accepted, not applied -- named, never silent (defect class 3).
  const std::vector<std::string> cosmetic = presentOpts(
      parsed.opts, {"ring", "position", "span", "symxsize", "symysize", "labsize"});
  if (!cosmetic.empty()) {
    appendBlockNote(result, "(" + kCmd + ": " + joinWith(cosmetic, ", ") +
                                " accepted and not applied — the kept legend stays inside its own "
                                "panel in this build)");
  }

  if (auto err = honourSaving(session, kCmd, parsed.opts, result)) return *err;
  return result;
}

// ===========================================================================
// geodist
// ===========================================================================
//
// `geodist lat1 lon1 lat2 lon2, gen(newvar) [miles sphere radius(#)]`. Each of
// the four arguments is EITHER a variable name OR a numeric literal, so
// `geodist 37.5 127.0 lat lon, gen(d)` (a fixed origin against a column of
// destinations) works the same way as four columns. Missing propagates: any
// missing coordinate gives a missing distance, and missing is `.`, never 0.
ojson runGeodist(const eval::ParsedCommand& parsed, SessionState& session) {
  const std::string kCmd = "geodist";
  if (auto err = eval::rejectUnknownOptions(parsed.opts, kVocabGeodist.known,
                                            kVocabGeodist.unimplemented))
    return ojson(*err);
  if (parsed.weightType.has_value() && !parsed.weightType->empty()) {
    return ojson(eval::unsupportedOption(
        *parsed.weightType + "=" + parsed.weightVar.value_or(""),
        "`geodist` computes a distance per observation; weights do not enter it."));
  }
  if (!session.hasData()) return ojson(eval::noDataLoaded());
  if (parsed.varlist.size() != 4) {
    return gxSyntax(kCmd +
                    ": syntax is `geodist lat1 lon1 lat2 lon2, generate(newvar) [miles sphere "
                    "radius(#)]` — four coordinate arguments, each a variable or a number.");
  }
  const std::optional<std::string> genOpt = optStrAny(parsed.opts, {"generate", "gen"});

  GeoMode mode;
  ojson modeErr;
  if (!readGeoMode(parsed.opts, kCmd, /*allowSphere=*/true, &mode, &modeErr)) return modeErr;

  // Each argument: a literal, or a column name to fetch.
  struct Arg {
    bool constant = false;
    double value = 0.0;
    std::string var;
  };
  std::vector<Arg> args(4);
  std::vector<std::string> wanted;
  for (size_t i = 0; i < 4; ++i) {
    const std::string tok = trimOf(dequote(trimOf(parsed.varlist[i])));
    double v = 0.0;
    if (parseStrictDouble(tok, &v)) {
      args[i].constant = true;
      args[i].value = v;
    } else {
      args[i].var = tok;
      wanted.push_back(tok);
    }
  }

  // BUG-426: geodist has TWO shapes and this handler had one. With four LITERAL
  // coordinates it is the IMMEDIATE form — a calculator: it prints the distance,
  // returns r(distance), and writes no column (geodist.ado's own `if
  // "`generate'" == "" ... display` arm). generate() is then an ERROR (Stata
  // r(198)), not an alternative. With variables it writes a column and
  // generate() is required. PIET had it exactly inverted: literals+generate()
  // was accepted (writing a constant column) and the calculator form refused.
  const bool immediate = args[0].constant && args[1].constant && args[2].constant &&
                         args[3].constant;
  if (immediate) {
    if (genOpt.has_value() && !genOpt->empty()) {
      return gxSyntax(kCmd +
                      ": generate() is not allowed with four literal coordinates — that is the "
                      "immediate form, which returns the distance in r(distance).");
    }
    const GeoOutcome o = geoDistance(args[0].value, args[1].value, args[2].value, args[3].value,
                                     mode);
    if (o.badLatitude) {
      return gxError(kCmd + ": a latitude outside [-90, 90] has no distance.");
    }
    session.eval.rValues.set("distance", o.value);
    // Stata's own %12.0g rendering of the immediate form's one number.
    ojson blk;
    blk["type"] = "display";
    blk["text"] = piet::fmtStataDefaultG(o.value);
    return blk;
  }
  if (!genOpt.has_value() || genOpt->empty())
    return gxSyntax(kCmd + ": generate() is required — geodist writes its distance to a new "
                           "variable.");
  const std::string newvar = *genOpt;
  if (!session.hasData()) return ojson(eval::noDataLoaded());
  ojson guard = newVarGuard(session, kCmd, newvar);
  if (!guard.is_null()) return guard;

  ReadFrame rf(session, wanted);
  Datastore* ds = rf.get();
  if (ds == nullptr)
    return gxError(kCmd + ": could not read the data" + (rf.why().empty() ? "" : " — " + rf.why()),
                   "data_io");
  const int64_t nRows = ds->nRows();

  std::vector<std::vector<double>> cols(4);
  for (size_t i = 0; i < 4; ++i) {
    if (args[i].constant) continue;
    if (!ds->has(args[i].var)) return gxError("variable " + args[i].var + " not found");
    if (ds->dtype(args[i].var) == "factor") {
      return gxError(kCmd + ": `" + args[i].var +
                     "' is a string variable; a coordinate has to be numeric.");
    }
    cols[i] = columnAsDoubles(*ds, args[i].var);
  }

  // The mask is evaluated over the WHOLE frame (an `if` may name variables the
  // §38 projection above never fetched), and it is handed to `cobra eval`, so
  // the base has to be a path that EXISTS -- `session.datastore()` is what
  // guarantees a virtual head has been materialized. Only paid for when a
  // qualifier is actually present.
  std::string maskBase;
  if ((parsed.ifExpr.has_value() && !parsed.ifExpr->empty()) ||
      (parsed.inRange.has_value() && !parsed.inRange->empty())) {
    Datastore* full = nullptr;
    try {
      full = session.datastore();
    } catch (const std::exception& e) {
      return gxError(kCmd + ": could not read the data — " + e.what(), "data_io");
    }
    if (full == nullptr) return ojson(eval::noDataLoaded());
    maskBase = full->path();
  }
  MaskResult mask = qualifierMask(session, kCmd, maskBase, parsed, nRows);
  if (!mask.ok) return mask.err;

  std::vector<std::string> cells;
  cells.reserve(static_cast<size_t>(nRows));
  int64_t nMissing = 0, nBadLat = 0, nNoConverge = 0;
  for (int64_t r = 0; r < nRows; ++r) {
    if (mask.present && mask.keep[static_cast<size_t>(r)] == 0) {
      // Outside the qualifier: missing, exactly like `gen ... if`. Counted as a
      // missing value generated for the same reason Stata counts it -- the
      // column really does have a missing value in that row.
      cells.emplace_back();
      ++nMissing;
      continue;
    }
    double v[4];
    for (size_t i = 0; i < 4; ++i)
      v[i] = args[i].constant ? args[i].value : cols[i][static_cast<size_t>(r)];
    const GeoOutcome o = geoDistance(v[0], v[1], v[2], v[3], mode);
    if (o.badLatitude) ++nBadLat;
    if (o.noConverge) ++nNoConverge;
    if (std::isnan(o.value)) ++nMissing;
    cells.push_back(numCell(o.value));
  }

  std::string out;
  if (auto err = appendColumn(session, *session.currentHbin(), newvar, cells, "f64", &out))
    return *err;
  session.load(out);

  ojson blocks = ojson::array();
  blocks.push_back(mutationResult(session, "gen", {newvar}));
  if (nMissing > 0) {
    blocks.push_back(noteBlock("(" + std::to_string(nMissing) + " missing value" +
                               (nMissing == 1 ? "" : "s") + " generated)"));
  }
  if (nBadLat > 0) {
    blocks.push_back(noteBlock("(" + kCmd + ": " + std::to_string(nBadLat) + " observation" +
                               (nBadLat == 1 ? " has a latitude" : "s have a latitude") +
                               " outside [-90, 90] — those distances are missing)"));
  }
  if (nNoConverge > 0) {
    blocks.push_back(noteBlock(
        "(" + kCmd + ": Vincenty's iteration did not converge for " + std::to_string(nNoConverge) +
        " (near-)antipodal pair" + (nNoConverge == 1 ? "" : "s") +
        " — those distances are missing. `sphere` gives an approximate answer for them.)"));
  }
  return blocks;
}

// ===========================================================================
// geonear
// ===========================================================================
//
// `geonear id lat lon using FILE, neighbors(nid nlat nlon) [nearcount(#)
// miles ellipsoid genstub(stub)]` -- for every master observation, the k
// nearest observations in the using file, as 2k new columns.
//
// BRUTE FORCE, WITH A HARD PAIR CAP. n*m distances, single-threaded, no spatial
// index. The plan's cap of 5e7 pairs is what makes that a bounded wall-clock
// promise instead of a hope; over the cap the command REFUSES and says so,
// rather than starting work it cannot finish. The cap is checked before a
// single column is written.
//
// COLUMN NAMES -- a decision the plan left open (its own text gives two
// different spellings) and this is the one taken, reported as such:
//   stem      = genstub() when given, else the neighbours' ID VARIABLE NAME
//   nearcount(1):   <stem>            and  km_to_<stem>
//   nearcount(k>1): <stem>1..<stem>k  and  km_to_<stem>1..km_to_<stem>k
//   under `miles` the distance prefix is `mi_to_` instead of `km_to_`, so the
//   column name always states its own unit.
// Every name is guarded (legal AND free) BEFORE the first write, so a collision
// on column 3 cannot leave columns 1 and 2 behind.
ojson runGeonear(const eval::ParsedCommand& parsed, SessionState& session) {
  const std::string kCmd = "geonear";
  if (auto err = eval::rejectUnknownOptions(parsed.opts, kVocabGeonear.known,
                                            kVocabGeonear.unimplemented))
    return ojson(*err);
  if (!session.hasData()) return ojson(eval::noDataLoaded());

  // `id lat lon using FILE` -- `using` arrives as an ordinary varlist token.
  std::vector<std::string> head;
  std::string usingTyped;  // THE SPELLING THE USER TYPED (defect class 5)
  {
    bool sawUsing = false;
    for (const auto& w : parsed.varlist) {
      if (!sawUsing && lowerOf(w) == "using") {
        sawUsing = true;
        continue;
      }
      if (sawUsing) {
        if (usingTyped.empty()) usingTyped = w;
      } else {
        head.push_back(w);
      }
    }
    if (!sawUsing || usingTyped.empty() || head.size() != 3) {
      return gxSyntax(kCmd +
                      ": syntax is `geonear id lat lon using \"FILE\", neighbors(nid nlat nlon) "
                      "[nearcount(#) miles genstub(stub)]`");
    }
  }
  const std::string mId = trimOf(dequote(trimOf(head[0])));
  const std::string mLat = trimOf(dequote(trimOf(head[1])));
  const std::string mLon = trimOf(dequote(trimOf(head[2])));

  const std::optional<std::string> nbOpt = optStr(parsed.opts, "neighbors");
  if (!nbOpt.has_value() || nbOpt->empty())
    return gxSyntax(kCmd + ": neighbors(nid nlat nlon) is required — it names the id, latitude "
                           "and longitude variables in the using file.");
  const std::vector<std::string> nb = splitWs(*nbOpt);
  if (nb.size() != 3)
    return gxSyntax(kCmd + ": neighbors() takes exactly three variable names (id lat lon), got `" +
                    *nbOpt + "'.");

  long long nearcount = 1;
  if (auto nc = optStr(parsed.opts, "nearcount")) {
    if (!parseStrictInt(*nc, &nearcount) || nearcount < 1)
      return gxSyntax(kCmd + ": nearcount() must be a positive whole number, got `" + *nc + "'.");
  }

  GeoMode mode;
  ojson modeErr;
  if (!readGeoMode(parsed.opts, kCmd, /*allowSphere=*/false, &mode, &modeErr)) return modeErr;
  // geonear's DEFAULT IS THE SPHERE, and `ellipsoid` opts IN -- the opposite of
  // `geodist`, whose default is the ellipsoid. Read out of the package on
  // 2026-09-04 rather than assumed: `geonear` takes the ellipsoid
  // branch only when an ellipsoid option is present and otherwise sets
  // `local sphere 1` with `scalar R = 6371`, while `geodist` takes
  // haversine only under `sphere`/`radius()` and is WGS84 Vincenty otherwise.
  //
  // Two sibling commands from the same author defaulting to different earth
  // models looks like a mistake and is not; shipping ONE default for both would
  // have been a silently wrong number in whichever verb got the other's rule --
  // ~0.07% here, invisible in any rendered output, and permanent once a golden
  // froze it. That is why this is set explicitly instead of inherited.
  mode.sphere = !hasFlag(parsed.opts, "ellipsoid");

  // ---- master frame ------------------------------------------------------
  ReadFrame rf(session, {mId, mLat, mLon});
  Datastore* master = rf.get();
  if (master == nullptr)
    return gxError(kCmd + ": could not read the data" + (rf.why().empty() ? "" : " — " + rf.why()),
                   "data_io");
  if (!master->has(mId)) return gxError("variable " + mId + " not found");
  for (const auto& v : {mLat, mLon}) {
    if (!master->has(v)) return gxError("variable " + v + " not found");
    if (master->dtype(v) == "factor")
      return gxError(kCmd + ": `" + v + "' is a string variable; a coordinate has to be numeric.");
  }
  const std::vector<double> mLatV = columnAsDoubles(*master, mLat);
  const std::vector<double> mLonV = columnAsDoubles(*master, mLon);
  const int64_t n = master->nRows();

  // ---- using frame -------------------------------------------------------
  const std::string usingResolved = resolveUserPath(session, trimOf(dequote(trimOf(usingTyped))));
  session.materializeVirtualSave(usingResolved);  // BUG-125: a save_virtual'd tempfile
  {
    std::error_code ec;
    if (!std::filesystem::exists(cecon::to_native_path(usingResolved), ec)) {
      return gxError(kCmd + " using: file not found: '" + usingTyped + "'");
    }
  }
  std::string nbHbin = usingResolved;
  {
    std::string ext;
    const size_t slash = usingResolved.find_last_of('/');
    const std::string base =
        slash == std::string::npos ? usingResolved : usingResolved.substr(slash + 1);
    const size_t dot = base.find_last_of('.');
    if (dot != std::string::npos) ext = lowerOf(base.substr(dot));
    if (ext == ".dta") {
      const std::string conv = nextPath(session.workdir(), "gxnb_dta");
      if (auto e = runVerb({"import-dta", usingResolved, conv})) {
        return gxError(kCmd + " using: cannot read '" + usingTyped + "': " +
                       e->value("message", std::string("conversion failed")));
      }
      nbHbin = conv;
    } else if (ext == ".csv" || ext == ".tsv") {
      const std::string conv = nextPath(session.workdir(), "gxnb_csv");
      const std::string delim = (ext == ".tsv") ? "\t" : ",";
      if (auto e = runVerb({"convert", "--data", usingResolved, "--out", conv, "--delim", delim})) {
        return gxError(kCmd + " using: cannot read '" + usingTyped + "': " +
                       e->value("message", std::string("conversion failed")));
      }
      nbHbin = conv;
    }
  }

  std::unique_ptr<Datastore> nbDs;
  try {
    nbDs = std::make_unique<Datastore>(nbHbin);
  } catch (const std::exception& e) {
    return gxError(kCmd + " using: could not open '" + usingTyped + "' — " + e.what(), "data_io");
  }
  for (const auto& v : nb) {
    if (!nbDs->has(v))
      return gxError(kCmd + " using: variable " + v + " not found in '" + usingTyped + "'");
  }
  for (size_t i = 1; i < 3; ++i) {
    if (nbDs->dtype(nb[i]) == "factor") {
      return gxError(kCmd + " using: `" + nb[i] + "' is a string variable in '" + usingTyped +
                     "'; a coordinate has to be numeric.");
    }
  }
  const std::vector<double> nLatV = columnAsDoubles(*nbDs, nb[1]);
  const std::vector<double> nLonV = columnAsDoubles(*nbDs, nb[2]);
  const bool nbIdIsFactor = nbDs->dtype(nb[0]) == "factor";
  const std::vector<std::string> nIdText = columnAsStrings(*nbDs, nb[0]);
  const std::vector<double> nIdNum =
      nbIdIsFactor ? std::vector<double>() : columnAsDoubles(*nbDs, nb[0]);
  const int64_t m = nbDs->nRows();

  // ---- the pair cap, BEFORE any column is written ------------------------
  {
    constexpr long double kPairCap = 5e7L;
    const long double pairs = static_cast<long double>(n) * static_cast<long double>(m);
    if (pairs > kPairCap) {
      std::ostringstream oss;
      oss << kCmd << ": n×m = " << static_cast<long long>(pairs)
          << " exceeds this build's 5e7 pair cap; restrict the sample.";
      return gxError(oss.str());
    }
  }

  // ---- the generated column names, all guarded before the first write ----
  // BUG-418: the default stem is the LITERAL `nid`, not the neighbours' id
  // variable. geonear is `if "`genstub'" == "" local genstub nid`, and the
  // package's help documents `nid`/`km_to_nid` "unless genstub()". Taking the
  // variable name was a deliberate open decision here (the note below used to say
  // so); the 2026-09-06 oracle settled it, and taking the variable name is wrong
  // twice: `gen near = km_to_nid` written against Stata fails with "variable not
  // found", and the help's own same-name idiom `neighbors(id lat lon)` was refused
  // rc 110 because the stem collided with the master's key.
  const std::string stem = optStr(parsed.opts, "genstub").value_or(std::string("nid"));
  const std::string distPrefix = mode.miles ? "mi_to_" : "km_to_";
  const size_t k = static_cast<size_t>(nearcount);
  std::vector<std::string> idNames, distNames;
  for (size_t j = 1; j <= k; ++j) {
    const std::string suffix = (k == 1) ? std::string() : std::to_string(j);
    idNames.push_back(stem + suffix);
    distNames.push_back(distPrefix + stem + suffix);
  }
  for (const auto& nm : idNames) {
    ojson g = newVarGuard(session, kCmd, nm);
    if (!g.is_null()) return g;
  }
  for (const auto& nm : distNames) {
    ojson g = newVarGuard(session, kCmd, nm);
    if (!g.is_null()) return g;
  }

  // ---- brute force -------------------------------------------------------
  // Neighbour rows with a missing coordinate are EXCLUDED (plan edge case), not
  // treated as distance-zero and not silently kept.
  std::vector<size_t> nbRows;
  nbRows.reserve(static_cast<size_t>(m));
  for (int64_t j = 0; j < m; ++j) {
    if (std::isnan(nLatV[static_cast<size_t>(j)]) || std::isnan(nLonV[static_cast<size_t>(j)]))
      continue;
    nbRows.push_back(static_cast<size_t>(j));
  }
  const int64_t nbDropped = m - static_cast<int64_t>(nbRows.size());

  std::vector<std::vector<std::string>> idCells(k), distCells(k);
  for (size_t j = 0; j < k; ++j) {
    idCells[j].reserve(static_cast<size_t>(n));
    distCells[j].reserve(static_cast<size_t>(n));
  }
  int64_t nBadLat = 0, nNoConverge = 0;
  std::vector<std::pair<double, size_t>> best;  // (distance, neighbour row)
  for (int64_t i = 0; i < n; ++i) {
    best.clear();
    const double la = mLatV[static_cast<size_t>(i)];
    const double lo = mLonV[static_cast<size_t>(i)];
    if (!std::isnan(la) && !std::isnan(lo)) {
      for (size_t jj : nbRows) {
        const GeoOutcome o = geoDistance(la, lo, nLatV[jj], nLonV[jj], mode);
        if (o.badLatitude) ++nBadLat;
        if (o.noConverge) ++nNoConverge;
        if (std::isnan(o.value)) continue;
        best.emplace_back(o.value, jj);
      }
      // BUG-419: a tie goes to the LOWEST NEIGHBOUR ID, not to the neighbour
      // file's row order. geonear.hlp:138-139 says so in as many words ("If
      // there is a tie for the #th nborid position, the neighbor is chosen by
      // ascending order of nborid") and geonear.ado implements it by sorting
      // the neighbour file by id before the scan. Row order was this handler's
      // own choice, and it is observable: symmetric grids and duplicated
      // coordinates produce exact ties routinely (the oracle's base at (0,0)
      // with neighbours at (0,-1) and (0,1) is bit-identical on both arms).
      // The row index stays as the last key so the sort is still total.
      std::stable_sort(best.begin(), best.end(),
                       [&](const std::pair<double, size_t>& a, const std::pair<double, size_t>& b) {
                         if (a.first != b.first) return a.first < b.first;
                         if (nbIdIsFactor) {
                           if (nIdText[a.second] != nIdText[b.second])
                             return nIdText[a.second] < nIdText[b.second];
                         } else if (nIdNum[a.second] != nIdNum[b.second]) {
                           return nIdNum[a.second] < nIdNum[b.second];
                         }
                         return a.second < b.second;
                       });
    }
    for (size_t j = 0; j < k; ++j) {
      if (j < best.size()) {
        const size_t src = best[j].second;
        idCells[j].push_back(nbIdIsFactor ? nIdText[src] : numCell(nIdNum[src]));
        distCells[j].push_back(numCell(best[j].first));
      } else {
        idCells[j].emplace_back();   // fewer than k usable neighbours: missing
        distCells[j].emplace_back();
      }
    }
  }

  // ---- write -------------------------------------------------------------
  std::string cur = *session.currentHbin();
  std::string out;
  std::vector<std::string> written;
  for (size_t j = 0; j < k; ++j) {
    if (auto err = appendColumn(session, cur, idNames[j], idCells[j],
                                nbIdIsFactor ? "factor" : "f64", &out))
      return *err;
    cur = out;
    written.push_back(idNames[j]);
    if (auto err = appendColumn(session, cur, distNames[j], distCells[j], "f64", &out)) return *err;
    cur = out;
    written.push_back(distNames[j]);
  }
  session.load(cur);

  // BUG-420: Stata returns the master SORTED BY BASEID. geonear
  // restores the master, sorts it by baseid and merges the results onto it, so
  // observation 1 afterwards is the lowest base id — not whatever row order the
  // caller had. This handler appended in place, which is a silent divergence
  // for any `_n`-based code after the command. runSort is the native verb's own
  // entry point, so the sort is one implementation rather than a second one.
  {
    ojson sorted = handlers::runSort({mId}, session);
    if (sorted.is_object() && sorted.value("type", std::string()) == "error") return sorted;
  }

  ojson blocks = ojson::array();
  blocks.push_back(mutationResult(session, "gen", written));
  if (nbDropped > 0) {
    blocks.push_back(noteBlock("(" + kCmd + ": " + std::to_string(nbDropped) + " observation" +
                               (nbDropped == 1 ? "" : "s") + " in '" + usingTyped +
                               "' had a missing coordinate and " +
                               (nbDropped == 1 ? "was" : "were") + " excluded)"));
  }
  if (nBadLat > 0) {
    blocks.push_back(noteBlock("(" + kCmd + ": " + std::to_string(nBadLat) +
                               " pair(s) had a latitude outside [-90, 90] and were skipped)"));
  }
  if (nNoConverge > 0) {
    blocks.push_back(noteBlock("(" + kCmd + ": Vincenty's iteration did not converge for " +
                               std::to_string(nNoConverge) +
                               " (near-)antipodal pair(s), which were skipped)"));
  }
  return blocks;
}

// ===========================================================================
// cdfplot
// ===========================================================================
//
// The empirical CDF as a `kind:"twoway"` block whose layers are ordinary
// `kind:"line"` layers -- the shape layerLineOrConnected already produces, so
// nothing downstream needs to learn a new plot type. `by(g)` gives one layer
// per group.
//
// STATA'S cdfplot DRAWS A STEP FUNCTION AND THIS DRAWS A LINE. The renderer has
// no step plot type (graph_m4.cpp:1431's six are scatter/line/connected/lfit/
// qfit/rarea) and adding one is a frontend change, outside this manifest. The
// block therefore says so in its own note rather than letting the difference
// pass unremarked.
ojson runCdfplot(const eval::ParsedCommand& parsed, SessionState& session) {
  const std::string kCmd = "cdfplot";
  if (auto err = eval::rejectUnknownOptions(parsed.opts, kVocabCdfplot.known,
                                            kVocabCdfplot.unimplemented))
    return ojson(*err);
  if (parsed.weightType.has_value() && !parsed.weightType->empty()) {
    return ojson(eval::unsupportedOption(
        *parsed.weightType + "=" + parsed.weightVar.value_or(""),
        "`cdfplot` plots an unweighted empirical CDF in this build; a weighted ECDF is a "
        "different estimator, not a reweighting of this one."));
  }
  if (!session.hasData()) return ojson(eval::noDataLoaded());
  if (parsed.varlist.size() != 1)
    return gxSyntax(kCmd + ": syntax is `cdfplot varname [if] [in] [, by(groupvar)]` — one "
                           "variable.");
  const std::string var = trimOf(dequote(trimOf(parsed.varlist[0])));
  const std::optional<std::string> byVar = optStr(parsed.opts, "by");

  // `if`/`in`: the ECDF is computed over the SELECTED sample, which is exactly
  // what the shared filter returns -- no row-alignment problem here, so this
  // uses `applyIfAndInFilter` as designed rather than geodist's mask. The base
  // path comes from `session.datastore()` rather than `currentHbin()` (the
  // dispatcher's own summarize/tabulate filter sites do the same): the filter
  // hands the path to `cobra eval`, so it has to be a file that EXISTS, and
  // going through the Datastore is what guarantees a virtual head has been
  // materialized first. This runs ONLY when a qualifier is present, so the
  // unfiltered path keeps its §38 projection.
  std::string dataPath;
  bool filtered = false;
  const bool hasQualifier = (parsed.ifExpr.has_value() && !parsed.ifExpr->empty()) ||
                            (parsed.inRange.has_value() && !parsed.inRange->empty());
  if (hasQualifier) {
    Datastore* base = nullptr;
    try {
      base = session.datastore();
    } catch (const std::exception& e) {
      return gxError(kCmd + ": could not read the data — " + e.what(), "data_io");
    }
    if (base == nullptr) return ojson(eval::noDataLoaded());
    dataPath = base->path();
    try {
      InRangeFilterResult fr = applyIfAndInFilter(session, dataPath, parsed.ifExpr, parsed.inRange);
      if (!fr.ok) return ojson(inRangeRejectError(fr, parsed.inRange.value_or(""), kCmd));
      filtered = (fr.dataPath != dataPath);
      dataPath = fr.dataPath;
    } catch (const CobraError& ce) {
      return gxError(kCmd + ": " + cobraErrorMessage(ce), ce.code);
    }
  }

  std::vector<std::string> wanted = {var};
  if (byVar.has_value() && !byVar->empty()) wanted.push_back(*byVar);
  // The filtered frame is a real file with the selected rows already in it, so
  // the §38 projection is only worth asking for on the unfiltered path.
  std::unique_ptr<Datastore> owned;
  std::optional<ReadFrame> rf;
  Datastore* ds = nullptr;
  if (filtered) {
    try {
      owned = std::make_unique<Datastore>(dataPath);
      ds = owned.get();
    } catch (const std::exception& e) {
      return gxError(kCmd + ": could not read the filtered data — " + e.what(), "data_io");
    }
  } else {
    rf.emplace(session, wanted);
    ds = rf->get();
    if (ds == nullptr)
      return gxError(kCmd + ": could not read the data" +
                         (rf->why().empty() ? "" : " — " + rf->why()),
                     "data_io");
  }

  if (!ds->has(var)) return gxError("variable " + var + " not found");
  if (ds->dtype(var) == "factor")
    return gxError(kCmd + ": `" + var + "' is a string variable; an ECDF needs a numeric "
                                        "variable.");
  const std::vector<double> x = columnAsDoubles(*ds, var);

  // Group the rows. Without by(), one group holding everything.
  std::vector<std::string> groupLabels;
  std::vector<std::vector<double>> groupValues;
  if (byVar.has_value() && !byVar->empty()) {
    if (!ds->has(*byVar)) return gxError("variable " + *byVar + " not found");
    const bool byFactor = ds->dtype(*byVar) == "factor";
    const std::vector<std::string> gText = columnAsStrings(*ds, *byVar);
    const std::vector<double> gNum = byFactor ? std::vector<double>() : columnAsDoubles(*ds, *byVar);
    // Ascending group order: by value for a numeric group variable, by level
    // text for a string one -- deterministic either way.
    std::vector<size_t> keys;
    keys.reserve(x.size());
    for (size_t i = 0; i < x.size(); ++i) {
      if (gText[i].empty()) continue;  // missing group: excluded, like Stata's by()
      keys.push_back(i);
    }
    std::stable_sort(keys.begin(), keys.end(), [&](size_t a, size_t b) {
      if (byFactor) return gText[a] < gText[b];
      return gNum[a] < gNum[b];
    });
    for (size_t pos = 0; pos < keys.size();) {
      size_t end = pos;
      while (end + 1 < keys.size() &&
             (byFactor ? (gText[keys[end + 1]] == gText[keys[pos]])
                       : (gNum[keys[end + 1]] == gNum[keys[pos]])))
        ++end;
      std::vector<double> vals;
      vals.reserve(end - pos + 1);
      for (size_t q = pos; q <= end; ++q) vals.push_back(x[keys[q]]);
      groupLabels.push_back(*byVar + "=" +
                            (byFactor ? gText[keys[pos]] : levelText(gNum[keys[pos]])));
      groupValues.push_back(std::move(vals));
      pos = end + 1;
    }
  } else {
    groupValues.push_back(x);
    groupLabels.push_back(var);
  }

  ojson layers = ojson::array();
  ojson legendOrder = ojson::array();
  std::vector<std::string> keyLabels;  // each drawn layer's key text
  double xMin = std::numeric_limits<double>::infinity();
  double xMax = -std::numeric_limits<double>::infinity();
  int64_t nObs = 0;
  for (size_t g = 0; g < groupValues.size(); ++g) {
    const std::vector<std::pair<double, double>> pts = ecdfPoints(groupValues[g]);
    if (pts.empty()) continue;  // a group with nothing non-missing draws nothing
    ojson pointsJ = ojson::array();
    for (const auto& [px, py] : pts) {
      pointsJ.push_back(ojson::array({px, py}));
      xMin = std::min(xMin, px);
      xMax = std::max(xMax, px);
    }
    ojson lyr;
    lyr["kind"] = "line";
    lyr["points"] = pointsJ;
    lyr["style"] = twoway::layerStyle(kNoLayerOpts, static_cast<int>(layers.size()), "line");
    lyr["n"] = static_cast<int64_t>(pts.size());
    if (byVar.has_value() && !byVar->empty()) {
      legendOrder.push_back(ojson::array({static_cast<int64_t>(layers.size()), groupLabels[g]}));
    }
    keyLabels.push_back(groupLabels[g]);
    layers.push_back(lyr);
    nObs += static_cast<int64_t>(pts.size());
  }
  if (!std::isfinite(xMin) || !std::isfinite(xMax)) {
    xMin = 0.0;
    xMax = 1.0;
  }

  // The x label defaults to the variable's own label when it has one; the
  // .hbin header carries none, so this reads the session's label projection and
  // falls back to the variable NAME rather than printing an empty axis.
  std::string xDefault = session.eval.varLabels.get(var, std::string());
  if (xDefault.empty()) xDefault = var;

  ojson block;
  block["type"] = "graph";
  block["kind"] = "twoway";
  block["title"] = optStr(parsed.opts, "title").value_or("Empirical CDF of " + var);
  block["subtitle"] = optStr(parsed.opts, "subtitle").value_or(std::string());  // as twoway's
  block["x_label"] = optStr(parsed.opts, "xtitle").value_or(xDefault);
  block["y_label"] = optStr(parsed.opts, "ytitle").value_or(std::string("Cumulative probability"));
  block["note"] = optStr(parsed.opts, "note").value_or(std::string());  // as twoway's
  block["x_min"] = xMin;
  block["x_max"] = xMax;
  block["y_min"] = 0.0;
  block["y_max"] = 1.0;
  block["layers"] = layers;
  {
    ojson axes;
    axes["xline"] = ojson::array();
    axes["yline"] = ojson::array();
    axes["xscale_noline"] = false;
    axes["yscale_noline"] = false;
    axes["xlabel_none"] = false;
    // `ylabel(none)` is the one ylabel spelling this block shape can express.
    std::string yl = lowerOf(optStr(parsed.opts, "ylabel").value_or(std::string()));
    axes["ylabel_none"] = (trimOf(yl) == "none");
    block["axes"] = axes;
  }
  {
    // cdfplot.ado hands legend() to its twoway (`goptions'), so it reads as
    // twoway's does -- order() over the group lines included (StataBE 19.5,
    // BUG-1309: `cdfplot x, by(g) legend(order(2 "two" 1 "one"))` rc 0); `off`
    // is a top-level token, not a substring (legend(order(1 "Payoff")) keeps
    // its legend). Without order() the keys are the groups in order.
    bool hasOrder = false;
    ojson legend = legendBlock(optStr(parsed.opts, "legend").value_or(std::string()),
                               [&](int64_t idx) -> std::optional<std::string> {
                                 if (idx < 0 || static_cast<size_t>(idx) >= keyLabels.size()) return std::nullopt;
                                 return keyLabels[static_cast<size_t>(idx)];
                               },
                               &hasOrder);
    if (!hasOrder) legend["order"] = legendOrder;
    block["legend"] = legend;
  }
  block["name"] = nullptr;  // filled by the dispatcher's name() capture (BS-011)
  block["n_obs"] = nObs;
  block["suppressed"] = session.graphicsOff;

  appendBlockNote(block, "(cdfplot: drawn as a line, not a step function)");
  {
    // The twoway options this block does not draw (BUG-1521), named as the twoway
    // arm names its own (dispatcher.cpp graphAcceptedNotApplied).
    static const std::unordered_set<std::string> kApplied = {
        "by", "title", "subtitle", "xtitle", "ytitle", "note", "ylabel", "legend", "name", "saving"};
    std::vector<std::string> noted;
    for (const auto& kv : parsed.opts) {
      if (kApplied.count(kv.first) == 0 && std::find(noted.begin(), noted.end(), kv.first) == noted.end())
        noted.push_back(kv.first);
    }
    if (!noted.empty()) {
      appendBlockNote(block, "(" + kCmd + ": " + joinWith(noted, ", ") +
                                 " accepted and not applied — this build draws the plot without " +
                                 (noted.size() == 1 ? "it" : "them") + ".)");
    }
  }
  if (layers.empty()) {
    // Plan edge case: zero observations is an EMPTY plot plus a note, never an
    // error -- a do-file that plots an empty subgroup keeps running.
    appendBlockNote(block, "(cdfplot: no non-missing observations — nothing to plot)");
  }
  if (auto err = honourSaving(session, kCmd, parsed.opts, block)) return *err;
  return block;
}

// ===========================================================================
// event_plot
// ===========================================================================
//
// A shim on `runCoefplot`: build the ordinary coefficient plot from the stored
// estimates, then keep only the coefficients whose names match `stub_lag()` /
// `stub_lead()` and put them in RELATIVE-TIME order -- leads most-negative
// first, then lags ascending. `#` in a stub is the horizon number, so
// `stub_lag(x#)` matches x0, x1, x2 and `stub_lead(F#event)` matches
// F1event, F2event.
//
// Filtering here rather than through coefplot's own `keep()` is deliberate:
// `keep()` filters but does not REORDER (eststo_esttab.cpp's loop walks the
// coeftable in its stored order), and an event plot whose horizons are in
// coeftable order is a different picture from one in event time.
namespace {

// `pattern` with `#` standing for a non-negative integer. Returns the horizon.
bool matchStub(const std::string& pattern, const std::string& name, long long* horizon) {
  const size_t hash = pattern.find('#');
  std::string prefix, suffix;
  if (hash == std::string::npos) {
    prefix = pattern;  // no `#`: the digits are whatever follows the stub
  } else {
    prefix = pattern.substr(0, hash);
    suffix = pattern.substr(hash + 1);
  }
  if (name.size() <= prefix.size() + suffix.size()) return false;
  if (name.compare(0, prefix.size(), prefix) != 0) return false;
  if (!suffix.empty() &&
      name.compare(name.size() - suffix.size(), suffix.size(), suffix) != 0)
    return false;
  const std::string mid =
      name.substr(prefix.size(), name.size() - prefix.size() - suffix.size());
  if (mid.empty()) return false;
  for (char c : mid) {
    if (!std::isdigit(static_cast<unsigned char>(c))) return false;
  }
  long long v = 0;
  if (!parseStrictInt(mid, &v)) return false;
  *horizon = v;
  return true;
}

}  // namespace

ojson runEventPlot(const eval::ParsedCommand& parsed, SessionState& session) {
  const std::string kCmd = "event_plot";
  if (auto err = eval::rejectUnknownOptions(parsed.opts, kVocabEventPlot.known,
                                            kVocabEventPlot.unimplemented))
    return ojson(*err);

  const std::optional<std::string> stubLag = optStr(parsed.opts, "stub_lag");
  const std::optional<std::string> stubLead = optStr(parsed.opts, "stub_lead");
  if ((!stubLag.has_value() || stubLag->empty()) && (!stubLead.has_value() || stubLead->empty())) {
    return gxSyntax(kCmd +
                    ": at least one of stub_lag() and stub_lead() is required — they name the "
                    "coefficients to plot, e.g. `stub_lag(x#) stub_lead(F#x)`.");
  }

  // `alpha(a)` is a significance level; coefplot speaks confidence levels.
  std::vector<std::pair<std::string, eval::OptValue>> fwd = parsed.opts;
  if (auto al = optStr(parsed.opts, "alpha")) {
    double a = 0.0;
    if (!parseStrictDouble(*al, &a) || !(a > 0.0) || !(a < 1.0)) {
      return gxSyntax(kCmd + ": alpha() must be between 0 and 1 (e.g. alpha(0.05)), got `" + *al +
                      "'.");
    }
    const long long level = static_cast<long long>(std::llround((1.0 - a) * 100.0));
    if (level < 1 || level > 99) {
      return gxSyntax(kCmd + ": alpha(" + *al +
                      ") gives a confidence level outside 1-99; use an alpha between 0.01 and "
                      "0.99.");
    }
    fwd.emplace_back("level", eval::OptValue::ofString(std::to_string(level)));
  }

  ojson block = ojson(runCoefplot(parsed.varlist, fwd, session));
  if (isErrorBlock(block)) return renameDelegateError(block, kCmd, "coefplot");
  if (!block.is_object() || !block.contains("models") || !block["models"].is_array())
    return gxError(kCmd + ": internal — `coefplot` returned no models.", "internal");

  // Keep + reorder, per model.
  std::vector<double> allValues;
  int64_t kept = 0;
  for (auto& model : block["models"]) {
    if (!model.is_object() || !model.contains("coefs") || !model["coefs"].is_array()) continue;
    std::vector<std::pair<long long, ojson>> picked;  // (relative time, coefficient)
    for (const auto& c : model["coefs"]) {
      const std::string name = c.value("name", std::string());
      long long h = 0;
      bool hit = false;
      long long rel = 0;
      if (stubLead.has_value() && !stubLead->empty() && matchStub(*stubLead, name, &h)) {
        rel = -h;  // a lead is BEFORE the event
        hit = true;
      } else if (stubLag.has_value() && !stubLag->empty() && matchStub(*stubLag, name, &h)) {
        rel = h;
        hit = true;
      }
      if (!hit) continue;
      picked.emplace_back(rel, c);
    }
    // Stable: two coefficients at the same relative time (a lag 0 and a lead 0,
    // say) keep the coeftable's own order rather than a sort's internals.
    std::stable_sort(picked.begin(), picked.end(),
                     [](const std::pair<long long, ojson>& a,
                        const std::pair<long long, ojson>& b) { return a.first < b.first; });
    ojson coefs = ojson::array();
    for (auto& [rel, c] : picked) {
      ojson cc = c;
      cc["rel_time"] = rel;  // what the ordering means, carried with the point
      coefs.push_back(cc);
      for (const char* key : {"b", "ci_lo", "ci_hi"}) {
        if (cc.contains(key) && cc[key].is_number()) {
          const double v = cc[key].get<double>();
          if (std::isfinite(v)) allValues.push_back(v);
        }
      }
      ++kept;
    }
    model["coefs"] = coefs;
  }

  if (kept == 0) {
    return gxError(kCmd + ": no coefficients match stub_lag()/stub_lead()");
  }

  // The x range coefplot computed covered coefficients this command dropped;
  // recompute it over what actually survives, or the plot is padded around
  // values it does not draw.
  if (block.contains("xline") && block["xline"].is_number()) {
    const double xl = block["xline"].get<double>();
    if (std::isfinite(xl)) allValues.push_back(xl);
  }
  if (!allValues.empty()) {
    double lo = *std::min_element(allValues.begin(), allValues.end());
    double hi = *std::max_element(allValues.begin(), allValues.end());
    const double span = hi - lo;
    const double pad = span > 0 ? span * 0.05 : 0.5;
    block["x_min"] = lo - pad;
    block["x_max"] = hi + pad;
  }

  // Accepted, not applied -- named, never silent (defect class 3). NOTE: the
  // coefplot renderer does not draw a block `note` today, so this reaches the
  // block JSON and not the figure; making it visible is a frontend change and
  // is reported rather than done.
  const std::vector<std::string> cosmetic =
      presentOpts(parsed.opts, {"plottype", "ciplottype", "graph_opt", "noautolegend"});
  if (!cosmetic.empty()) {
    appendBlockNote(block, "(" + kCmd + ": " + joinWith(cosmetic, ", ") +
                               " accepted and not applied — this build draws the coefficient plot "
                               "the renderer already has)");
  }
  if (!hasFlag(parsed.opts, "together")) {
    appendBlockNote(block, "(" + kCmd +
                               ": lags and leads are drawn as ONE relative-time series in this "
                               "build, as `together` asks for; separate series are not available)");
  }
  return block;
}

}  // namespace piet::handlers
