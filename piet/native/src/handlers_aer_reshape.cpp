// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// AER-SCOPE1 bundle F (RESHAPE, 2026-09-04) -- the seventeen shape/sample verbs
// that change what the rows and columns of the dataset ARE:
//
//   split       one string column -> k string columns
//   xpose       the whole frame, transposed
//   recast      a declared Stata storage type (METADATA only)
//   insobs      blank observations, at the end or at a position
//   stack       k columns -> one column of k*n rows plus `_stack`
//   separate    one column -> one column per level of a by() variable
//   mvencode    missing -> a numeric code
//   sample      a random subset, by percent or by count
//   splitsample a group id assigning every row to one of nsplit() groups
//   drawnorm    multivariate normal draws (small dense Cholesky)
//   input       a do-file DATA block -> a dataset (or appended rows)
//   notes       the Stata `char` note table, on _dta and on variables
//   compare     two variables, side by side (display only)
//   cumul       the empirical cdf of a variable
//   bsample     a bootstrap resample of the data in memory
//   pctile      percentiles into a new variable
//   _pctile     the same percentiles into r(r1)..r(rN)
//
// WHY ONE TU, AND WHY NO NEW ENGINE VERB. Every one of these is a COMPOSITION
// of verbs cobra already has -- `setcol`, `convert`, `append`, `merge`, `eval`.
// Adding a `split` or an `xpose` verb to the engine would make this a
// two-binary change (engine + PIET), which is a different lane and a different
// release; the composition costs a CSV round trip per command and buys the
// whole group at once. Reading is `fetchColumns` with the materialized head as
// the fallback, writing is a cobra verb + `session.load` + `mutationResult` --
// the pair handlers.cpp:787-841 established and every data verb since has used.
//
// RANDOMNESS IS PHILOX, KEYED PER ROW (design decision 7). `sample`,
// `splitsample`, `bsample` and `drawnorm` never touch a global generator: each
// row's key is a pure function of (seed, row), so `set seed 7` followed by the
// same command twice gives the identical rows, and the answer does not depend
// on the order the rows were visited in. The seed is the command's own
// `seed()`/`rseed()` if it has one, else `session.seed`, else 123456789.
//
// THE FIVE DEFECT CLASSES THIS FILE IS WRITTEN AGAINST. Each has shipped at
// least once in this campaign; the first has been caught three times in four
// bundles, so it is named first.
//
//  1. SHAPE. A handler returning a JSON ARRAY where the arm pushes ONE block:
//     nlohmann throws type_error.306 the moment a consumer reads ["type"], and
//     that aborted the whole selftest binary. dispatcher.cpp's bundle-F arms
//     route through `pushEnvResult`, which flattens an array and drops nulls,
//     so an array IS legal -- but ONLY an array whose every element is a real
//     block object carrying a "type" key. Every `return` below is one object
//     with "type", or an ojson::array() of such objects (possibly empty, which
//     is how `recast` and `_pctile` say "nothing to print").
//  2. RC. `eval::coarseRc` answers 199 only for `unsupported_command` or an
//     explicit `r(NNN)` reason, and 198 for `syntax_error`; a refusal built
//     with the default `engine_error` code silently becomes rc 1. So:
//     command-level refusals go through `eval::refusedCommand(cmd, cause)`,
//     feature refusals through `eval::unsupportedCommand(..., kNotImplemented)`,
//     USER SYNTAX errors carry "syntax_error", and an error with a known Stata
//     rc that coarseRc's message rules cannot recognise states it through the
//     `reason` channel (`rsErrorRc` below). Bundle C had to repair six sites
//     that had defaulted to rc 1.
//  3. VOCABULARY. `rejectUnknownOptions` prefix-resolves and therefore ACCEPTS
//     abbreviations, while `canonicalizeForCommand` -- the thing that makes a
//     handler's own `k == "generate"` comparison see them -- is driven by the
//     REGISTRY. A command absent from the vocabulary registry gets its
//     abbreviations accepted by the net and then dropped unread. All seventeen
//     command names register at NAMESPACE scope below (a function-local static
//     constructs too late: after the first call that needed it), and every
//     handler's first act is the reject net. An option this build does NOT read
//     goes in the `unimplemented` list, never in `known` -- bundle E shipped
//     `esttab, cells()` as known-and-unread and it had to be reverted.
//  4. ECHOED PATHS. Nothing here echoes a RESOLVED absolute path; there is no
//     `using` file in this bundle at all, so the class is structurally absent.
//  5. R-CLASS. `_pctile`'s entire purpose is r(r1)..r(rN), and its arm
//     deliberately does not call `captureRFromBlock` (there is no table block
//     to capture FROM), so the handler fills `session.eval.rValues` itself.
//     `split` fills r(nvars)/r(varlist) the same way.
//
// TWO FILES THIS LANE DOES NOT TOUCH, AND WHY THE COPY BELOW IS DELIBERATE.
// `handlers_resample.cpp` owns `writeIndexCsv` and `kRowIdx` in an ANONYMOUS
// namespace, so they are unreachable from here by construction; `bsample`'s
// three-step index/merge shape is the same one `bootstrap` uses, so the twelve
// lines are copied rather than hoisted. Hoisting them would mean editing that
// file, which the plan puts out of scope for this bundle -- the shared-helper
// obligation (COMMS 4.9, third copy) is FILED here, not discharged: when a
// third caller appears, these two and resample's originals become one helper.
// `session_state.h` is likewise untouched: `ScratchPin` and
// `noteProjectionScratch` are used exactly as they already exist.
#include "piet/handlers.h"
#include "piet/dispatcher.h"  // dispatch(): cumul marks its if/in sample with generate

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <fstream>
#include <limits>
#include <map>
#include <memory>
#include <numeric>
#include <optional>
#include <regex>
#include <ctime>
#include <set>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "data/frame_internal.h"  // canonical numeric storage rounding for recast
#include "data/missing.h"         // cumul's by() groups: each missing code is its own
#include "core/rng.h"         // Philox4x32-10, the engine's own counter-based RNG
#include "core/rng_mt64.h"    // Stata's session stream: bsample's fast path
#include "bsample_draws.h"    // the fast path's draw, shared with the bootstrap prefix
#include "core/rng_normal.h"  // uniform01 / normal_icdf (AS241) -- drawnorm
#include "io/win_compat.h"    // cecon::to_native_path / native_unlink
#include "piet/cobra_runner.h"
#include "piet/column_fetch.h"
#include "piet/datastore.h"
#include "piet/display_hardening.h"  // piet::fmtStataDefaultG — Stata's %9.0g default
#include "piet/eval/errors.h"
#include "piet/eval/pyvalue.h"  // pythonFloatRepr -- round-trippable CSV numerics
#include "piet/eval/session.h"
#include "piet/eval/tokenizer.h"
#include "piet/mutation_result.h"
#include "piet/scratch_seq.h"
#include "piet/session_state.h"
#include "piet/storage_type.h"  // FU-944: input's c(type) and float store
#include "piet/strutil.h"
#include "estimation_m4_internal.h"  // esampleHeadMask / esampleRewriteHeadMask
#include "scratch_columns.h"          // cumul's if/in restriction and weight columns

// ---------------------------------------------------------------------------
// OPTION VOCABULARIES -- namespace scope, before any dispatch (defect class 3).
//
// `known` is the plan's "corpus options (implemented)" column verbatim;
// `unimplemented` is its "refused options" column. The split is the whole
// point: a name in NEITHER set reads as "unknown option", which is the wrong
// diagnosis for a real Stata option this build simply does not do, and a name
// wrongly placed in `known` is accepted and silently ignored, which is the
// defect the apparatus exists to prevent.
namespace {

// `p()` is split's own abbreviation for `parse()`; both are exact members, so
// the prefix resolver never has to choose between them.
const piet::eval::OptVocab& kVocabSplit = piet::eval::registerOptVocab(
    {"split"}, {{"parse", "p", "gen", "generate", "limit", "destring"},
                // `ignore()` deletes characters before parsing, `notrim` keeps
                // the padding, `force` splits a numeric variable. None is read
                // here, and each would change WHAT THE PARTS ARE, so none may
                // be accepted-and-ignored.
                // `g` is split.ado's Generate() minimum (aer_w2 w2b_dataio
                // split_p_g `p("-") g(Job)` rc 0); it prefixes both
                // registered spellings, so it is declared.
                {"ignore", "notrim", "force"},
                {{"g", "generate"}, {"ge", "generate"}}});

// `promote` asks for the narrowest storage type that holds the transposed
// values. It IS read below (it produces a note), because this build's frame is
// f64 throughout and there is nothing to promote -- saying so is honest, where
// silently accepting it would claim a storage decision that never happened.
// `format` asks for the transposed values to be rendered through a display
// format, which would change the VALUES this build writes, so it is refused.
const piet::eval::OptVocab& kVocabXpose = piet::eval::registerOptVocab(
    {"xpose"}, {{"clear", "varname", "promote"}, {"format"}});

const piet::eval::OptVocab& kVocabRecast =
    piet::eval::registerOptVocab({"recast"}, {{"force"}, {}});

const piet::eval::OptVocab& kVocabInsobs =
    piet::eval::registerOptVocab({"insobs"}, {{"before", "after"}, {}});

const piet::eval::OptVocab& kVocabStack = piet::eval::registerOptVocab(
    {"stack"}, {{"into", "clear", "wide", "group"}, {}});

// `veryshortlabel` and `sequential` change the NAMES/LABELS separate produces,
// so accepting them unread would rename columns behind the user's back.
const piet::eval::OptVocab& kVocabSeparate = piet::eval::registerOptVocab(
    {"separate"}, {{"by", "shortlabel", "generate"}, {"veryshortlabel", "sequential"}});

const piet::eval::OptVocab& kVocabMvencode =
    piet::eval::registerOptVocab({"mvencode"}, {{"mv", "override"}, {}});

// `by()` makes sample draw WITHIN groups; forwarding it as a plain sample would
// answer a different question at rc 0.
const piet::eval::OptVocab& kVocabSample =
    piet::eval::registerOptVocab({"sample"}, {{"count"}, {"by"}});

const piet::eval::OptVocab& kVocabSplitsample = piet::eval::registerOptVocab(
    {"splitsample"}, {{"generate", "nsplit", "rseed", "show"},
                      {"split", "cluster", "strata", "balance"}});

// `double` asks for double storage, which is what this build's frame already
// is, so it is honoured by construction rather than ignored. `cstorage()`
// declares an alternative corr()/cov() matrix layout and `forcepsd` silently
// repairs a non-PD matrix -- both would change the numbers drawn.
const piet::eval::OptVocab& kVocabDrawnorm = piet::eval::registerOptVocab(
    {"drawnorm"}, {{"n", "means", "sds", "corr", "cov", "seed", "double", "clear"},
                   {"cstorage", "forcepsd"}});

// `input` takes no option this build reads. Registering the EMPTY known set is
// not a no-op: it is what makes `input x, automatic` reach the net at all.
const piet::eval::OptVocab& kVocabInput =
    piet::eval::registerOptVocab({"input"}, {{}, {"automatic", "label"}});

// `notes` has no option tail at all -- everything after the colon is NOTE TEXT,
// including any comma in it, so this handler reads `parsed.raw` and never
// `parsed.opts`, and never runs the reject net (it would reject the note's own
// words). The registration exists so the vocabulary inventory is complete and
// `canonicalizeForCommand` has an answer for the token.
const piet::eval::OptVocab& kVocabNotes =
    piet::eval::registerOptVocab({"notes", "note"}, {{}, {}});

const piet::eval::OptVocab& kVocabCompare =
    piet::eval::registerOptVocab({"compare"}, {{}, {}});

const piet::eval::OptVocab& kVocabCumul = piet::eval::registerOptVocab(
    {"cumul"}, {{"gen", "generate", "equal", "freq", "by"},
                {},
                {{"g", "generate"}, {"ge", "generate"}, {"f", "freq"}}});

const piet::eval::OptVocab& kVocabBsample = piet::eval::registerOptVocab(
    {"bsample"}, {{"cluster", "idcluster"}, {"strata", "weight"}});

// `altdef` selects Stata's ALTERNATIVE percentile definition. This build has
// exactly one percentile rule (see stataPercentile below) and answering a
// different definition's question with this one's number is the silent-wrong
// -number shape, so it is refused rather than accepted.
const piet::eval::OptVocab& kVocabPctile = piet::eval::registerOptVocab(
    {"pctile"}, {{"nquantiles", "nq", "percentiles", "p", "genp"}, {"altdef"}});

const piet::eval::OptVocab& kVocabPctileR = piet::eval::registerOptVocab(
    {"_pctile"}, {{"nquantiles", "nq", "percentiles", "p"}, {"altdef"}});

}  // namespace

namespace piet::handlers {

using ojson = nlohmann::ordered_json;

namespace {

// The registrations above are consumed by the reject nets below; naming the two
// that no net reads keeps -Wunused-variable honest about them (`notes` and
// `compare` have no option tail to reject).
[[maybe_unused]] const piet::eval::OptVocab* const kVocabTouch[] = {&kVocabNotes, &kVocabCompare};

// ===========================================================================
// Errors
// ===========================================================================

ojson rsError(const std::string& message, const std::string& code = "engine_error") {
  return ojson(eval::engineError(message, code));
}

// A USER SYNTAX error. "syntax_error" is what coarseRc maps to rc 198; the
// default "engine_error" maps to rc 1, which is bundle C's shipped defect.
ojson rsSyntax(const std::string& message) {
  return ojson(eval::engineError(message, "syntax_error"));
}

// An error whose Stata rc is KNOWN but which coarseRc's message rules cannot
// recognise. `reason` is the only channel carrying an exact rc (BUG-187) and it
// outranks the coarse mapping.
ojson rsErrorRc(const std::string& message, int rc) {
  return ojson(eval::errorBlock("engine_error", message, std::nullopt, std::nullopt, std::nullopt,
                                "r(" + std::to_string(rc) + ")"));
}

bool isErrorBlock(const ojson& b) {
  return b.is_object() && b.value("type", std::string()) == "error";
}

ojson displayBlock(const std::string& text) {
  ojson b;
  b["type"] = "display";
  b["text"] = text;
  return b;
}

ojson echoBlock(const std::string& text) {
  ojson b;
  b["type"] = "echo";
  b["text"] = text;
  return b;
}

// "This command printed nothing" -- an EMPTY array, which `pushEnvResult`
// flattens into zero blocks. Never a bare object without a "type" key
// (defect class 1).
ojson silent() { return ojson::array(); }

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
  return rsError(cobraErrorMessage(ce), ce.code);
}

// ===========================================================================
// Reading
// ===========================================================================

// The shared READ shape: ask the worker for a contract_A section-38 projection
// of just the columns this command needs, and fall back to the session's own
// materialized head whenever the projection is unavailable (no live worker,
// head not resident-dirty, request refused). A miss must cost a fallback, never
// an answer -- column_fetch.h's own law. The projection file is registered with
// the session's projection ledger by fetchColumns itself.
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
// CSV transport
// ===========================================================================

// One CSV field, quoted only when it must be (delimiter, quote, CR or LF) --
// the same convention handlers_data_verbs3.cpp's export writer uses.
std::string csvField(const std::string& s, char delim) {
  bool needQuote = false;
  for (char c : s) {
    if (c == delim || c == '"' || c == '\n' || c == '\r') {
      needQuote = true;
      break;
    }
  }
  if (!needQuote) return s;
  std::string out = "\"";
  for (char c : s) {
    if (c == '"')
      out += "\"\"";
    else
      out += c;
  }
  out += "\"";
  return out;
}

// A numeric cell: BLANK for missing (`Frame::read_csv_column` and
// `Frame::from_csv` both read an empty cell as missing), else the shortest
// round-trippable decimal, so the value that comes back is bit-identical.
// Missing is `.`, never 0 -- in every cell this file writes.
std::string numCell(double v) {
  if (std::isnan(v)) return std::string();
  return eval::pythonFloatRepr(v);
}

// The one-column HEADERLESS payload `cobra setcol` reads: one raw value per
// row, row count == n_rows exactly. String cells are quoted even when they do
// not need it, so a value's own leading/trailing padding survives the trip
// (BUG-292); an EMPTY cell is written as a bare blank line, which is the only
// spelling read_csv_column accepts for a missing factor cell.
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

// A one-column CSV of int64 values, for `cobra convert` to turn into an hbin.
//
// COPIED from handlers_resample.cpp's anonymous namespace (:211), deliberately
// and with the file's own permission withheld: `handlers_resample.cpp` is out
// of this bundle's write manifest, and both this and `kRowIdx` below live in
// its ANONYMOUS namespace, so there is no declaration to reach. See this file's
// header comment for the hoist that is filed rather than done.
bool writeIndexCsv(const std::string& path, const std::string& colName,
                   const std::vector<int64_t>& vals) {
  std::ofstream f(cecon::to_native_path(path));
  if (!f) return false;
  f << colName << "\n";
  for (int64_t v : vals) f << v << "\n";
  f.flush();
  return static_cast<bool>(f);
}

// The row-index column bsample's index join keys on. Underscore-prefixed and
// long enough that it cannot collide with a user variable in practice; it never
// survives into the session's own dataset. (Copied alongside writeIndexCsv --
// same reason, and deliberately a DIFFERENT name from resample's `__piet_rs_row`
// so a leaked column can be attributed to the command that leaked it.)
constexpr const char* kRowIdx = "__piet_bs_row";

// A whole table on its way to `cobra convert`: a header row, then the cells.
// Factor columns are named explicitly rather than left to type inference,
// because a column whose visible rows happen to be all-numeric (or all-blank,
// which is every padded row an `insobs, before()` writes) would otherwise come
// back f64 and collide with the master's dtype at the append.
struct CsvTable {
  std::vector<std::string> names;
  std::vector<char> isFactor;                     // parallel to `names`
  std::vector<std::vector<std::string>> rows;     // rows[r][c]; "" == missing
};

// Write `t` and convert it to an hbin. nullopt = success and *outPath holds the
// new frame; otherwise the error block to return. A torn file is removed rather
// than left where a later `load()` could mmap it (BUG-124).
std::optional<ojson> writeAndConvert(SessionState& session, const CsvTable& t,
                                     const std::string& prefix, std::string* outPath) {
  const std::string csvPath = nextPath(session.workdir(), prefix) + ".csv";
  registerScratchMint(csvPath);  // BUG-389
  {
    std::ofstream f(cecon::to_native_path(csvPath), std::ios::binary);
    if (!f) return rsError(prefix + ": cannot stage " + csvPath, "data_io");
    for (size_t c = 0; c < t.names.size(); ++c) {
      if (c) f << ',';
      f << csvField(t.names[c], ',');
    }
    f << "\n";
    for (const auto& row : t.rows) {
      // BUG-1078: a row whose every cell is missing would be a BLANK line, and
      // cobra's CSV reader skips blank lines (BUG-697's pad rows, same reader),
      // so a one-variable `input` with a `""` or `.` row lost it and stopped on
      // the row-count assert. A quoted empty first field keeps the row; convert
      // reads it as missing for a factor and for an f64 column alike.
      bool allEmpty = true;
      for (size_t c = 0; c < t.names.size() && allEmpty; ++c) {
        if (c < row.size() && !row[c].empty()) allEmpty = false;
      }
      for (size_t c = 0; c < t.names.size(); ++c) {
        if (c) f << ',';
        if (c == 0 && allEmpty) {
          f << "\"\"";
        } else if (c < row.size()) {
          f << csvField(row[c], ',');
        }
      }
      f << "\n";
    }
    f.flush();
    if (!f) {
      cecon::native_unlink(csvPath);
      return rsError(prefix + ": failed staging " + csvPath + " (disk full or I/O error)",
                     "data_io");
    }
  }
  std::string factorList;
  for (size_t c = 0; c < t.names.size(); ++c) {
    if (!t.isFactor[c]) continue;
    if (!factorList.empty()) factorList += ",";
    factorList += t.names[c];
  }
  const std::string hbin = nextPath(session.workdir(), prefix);
  std::vector<std::string> args = {"convert", "--data", csvPath, "--out", hbin, "--delim", ","};
  if (!factorList.empty()) {
    args.push_back("--factor");
    args.push_back(factorList);
  }
  if (auto err = runVerb(args)) {
    cecon::native_unlink(hbin);
    return err;
  }
  *outPath = hbin;
  return std::nullopt;
}

// Append ONE new column to `basePath` from a payload written here. `cells` are
// the rendered values ("" == missing); `dtype` is "f64" or "factor".
std::optional<ojson> appendColumn(SessionState& session, const std::string& basePath,
                                  const std::string& var, const std::vector<std::string>& cells,
                                  const std::string& dtype, std::string* outPath) {
  const std::string csvPath = nextPath(session.workdir(), "rsvals") + ".csv";
  registerScratchMint(csvPath);  // BUG-389
  if (!writePayloadCsv(csvPath, cells, dtype == "factor")) {
    cecon::native_unlink(csvPath);
    return rsError("could not write the column payload (disk full or I/O error)", "data_io");
  }
  const std::string out = nextPath(session.workdir(), "setcol");
  if (auto err = runVerb({"setcol", "--data", basePath, "--var", var, "--values", csvPath,
                          "--dtype", dtype, "--new", "--out", out})) {
    cecon::native_unlink(out);
    return err;
  }
  *outPath = out;
  return std::nullopt;
}

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
// which is exactly how a mistyped option value becomes a silently wrong number.
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

// A number the way Stata names a variable after it: an integral value loses its
// decimal point, and the characters an identifier cannot carry become `_`.
std::string levelSuffix(double v) {
  std::string s;
  if (v == std::floor(v) && std::fabs(v) < 1e15) {
    s = std::to_string(static_cast<long long>(v));
  } else {
    s = eval::pythonFloatRepr(v);
  }
  std::string out;
  for (char c : s) out += (std::isalnum(static_cast<unsigned char>(c)) ? c : '_');
  return out;
}

// A level's DISPLAY text, for a label: the plain number, not the sanitized name.
std::string levelText(double v) {
  if (v == std::floor(v) && std::fabs(v) < 1e15) return std::to_string(static_cast<long long>(v));
  return eval::pythonFloatRepr(v);
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

// The first spelling of `keys` that is present, as a string.
std::optional<std::string> optStrAny(
    const std::vector<std::pair<std::string, eval::OptValue>>& opts,
    std::initializer_list<const char*> keys) {
  for (const char* k : keys) {
    if (auto v = optStr(opts, k)) return v;
  }
  return std::nullopt;
}

// ===========================================================================
// THE ONE PERCENTILE RULE
// ===========================================================================
//
// TRANSCRIBED, not re-derived, from dispatcher.cpp's winsor2 arm (the `pct`
// lambda beside `// Stata's percentile rule, the same one Frame::xtile and
// summarize,detail use`), which is itself the engine's `Frame::xtile` rule. Two
// percentile rules in one product is a silently wrong number waiting for a
// fixture that does not exist: `pctile`/`_pctile` here and `xtile`/`winsor2`
// there must answer the same question with the same arithmetic, so this is a
// verbatim copy and any change to either is a change to both.
//
// `sorted` must be ascending and free of missing.
double stataPercentile(const std::vector<double>& sorted, double p) {
  const int64_t n = static_cast<int64_t>(sorted.size());
  if (n == 0) return std::numeric_limits<double>::quiet_NaN();
  if (n == 1) return sorted[0];
  const double Pi = static_cast<double>(n) * p / 100.0;
  const double fl = std::floor(Pi);
  if (fl == Pi && Pi >= 1 && static_cast<int64_t>(Pi) < n)
    return (sorted[static_cast<size_t>(Pi) - 1] + sorted[static_cast<size_t>(Pi)]) / 2.0;
  int64_t idx = static_cast<int64_t>(fl) + 1;
  if (idx > n) idx = n;
  return sorted[static_cast<size_t>(idx) - 1];
}

// The weighted form of the same rule, [D] pctile "Methods and formulas"
// (/Applications/StataNow/docs/d.pdf, pctile p.776): with x(j) ascending and
// w(j) its weight, N = Σw, P = N p/100 and W(i) = Σ_{j<=i} w(j), take the first
// i with W(i) > P; the percentile is (x(i-1) + x(i))/2 when W(i-1) = P, else
// x(i). With every w(j) = 1 this is stataPercentile exactly. The weights are
// the raw ones, never rescaled: Stata compares W(i-1) = P on the weights as
// given, and a rescaled aweight moves that equality (w2aj_qualifiers
// qf_pt_aw_sp r(r2) 6.875 is the tie average; rescaled sums give 7.25).
//
// `sorted` is ascending in x, free of missing, every weight non-zero.
double stataWeightedPercentile(const std::vector<std::pair<double, double>>& sorted, double p) {
  if (sorted.empty()) return std::numeric_limits<double>::quiet_NaN();
  double total = 0.0;
  for (const auto& e : sorted) total += e.second;
  const double P = total * p / 100.0;
  double W = 0.0;
  for (size_t i = 0; i < sorted.size(); ++i) {
    const double before = W;
    W += sorted[i].second;
    if (W > P) {
      if (i > 0 && before == P) return (sorted[i - 1].first + sorted[i].first) / 2.0;
      return sorted[i].first;
    }
  }
  return sorted.back().first;
}

// ===========================================================================
// Randomness (design decision 7)
// ===========================================================================

// The seed a sampling command draws on: its own option first, then the session
// (`set seed #`), then Stata's own conventional default. Never a clock and
// never a global generator -- `set seed 7` plus the same command twice must
// give the identical rows.
uint64_t drawSeed(SessionState& session, const std::optional<std::string>& optSeed) {
  if (optSeed.has_value()) {
    long long v = 0;
    if (parseStrictInt(*optSeed, &v)) return static_cast<uint64_t>(v);
  }
  if (session.seed.has_value()) return static_cast<uint64_t>(*session.seed);
  return 123456789ull;
}

// Row j's key: a pure function of (seed, j), so it does not depend on the order
// the rows were visited in or on how many draws came before.
uint32_t rowKey32(uint64_t seed, int64_t j) {
  return cecon::Philox::draw32(seed, 0, static_cast<uint64_t>(j));
}

// ===========================================================================
// Column reading helpers
// ===========================================================================

// A column's values as DISPLAY strings ("" = missing): the level text for a
// factor, the round-trip decimal for a number.
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

std::vector<double> columnAsDoubles(const Datastore& ds, const std::string& name) {
  const int64_t n = ds.nRows();
  std::vector<double> out(static_cast<size_t>(n), std::numeric_limits<double>::quiet_NaN());
  const cecon::Column& c = ds.col(name);
  if (c.dtype == cecon::DType::Factor) return out;  // callers reject factors first
  const double* d = c.f64();
  for (int64_t i = 0; i < n; ++i) out[static_cast<size_t>(i)] = d[i];
  return out;
}

// A new variable name has to be legal AND free. Both checks, once, so every
// handler in this file refuses the same things for the same reasons.
ojson newVarGuard(SessionState& session, const std::string& verb, const std::string& newvar) {
  if (!strutil::isValidStataIdentifier(newvar))
    return rsSyntax(verb + ": invalid variable name `" + newvar + "' — " +
                    strutil::stataIdentifierRuleText());
  // BUG-1120: nor is a reserved name one: `pctile _cons = x', `cumul x,
  // gen(_cons)', `drawnorm a _cons' and `splitsample, gen(_cons)' are each
  // "_cons invalid varname" r(198) (StataBE 19.5).
  if (strutil::isReservedVarName(newvar)) return rsErrorRc(newvar + " invalid varname", 198);
  Datastore* ds = nullptr;
  try {
    ds = session.datastore();
  } catch (const std::exception&) {
    ds = nullptr;
  }
  if (ds != nullptr && ds->has(newvar))
    return rsError(verb + ": variable " + newvar + " already defined", "bad_input");
  return ojson();  // null => no objection
}

// The row-count agreement the plan makes a STOP-and-report: after a command
// that changes the number of observations, what `mutationResult` will publish
// (the reloaded frame's own n_rows) must equal what the command decided to
// produce. `mutationResult` reads the frame, so a disagreement means the write
// did not do what the handler asked -- an honest error beats a wrong `_N`.
std::optional<ojson> assertRowCount(SessionState& session, const std::string& verb,
                                    int64_t expected) {
  Datastore* ds = nullptr;
  try {
    ds = session.datastore();
  } catch (const std::exception& e) {
    return rsError(std::string(verb) + ": could not reopen the result frame — " + e.what(),
                   "data_io");
  }
  if (ds == nullptr) return std::nullopt;
  if (ds->nRows() == expected) return std::nullopt;
  return rsError(verb + ": internal row-count disagreement — the result frame has " +
                     std::to_string(ds->nRows()) + " observation(s) where " +
                     std::to_string(expected) + " were written. Refusing to report a `_N` the "
                     "data does not have.",
                 "internal");
}

// ===========================================================================
// `if`/`in` honesty
// ===========================================================================
//
// Every registry row in this bundle carries allowIf=false / allowInRange=false,
// so the TOKENIZER refuses a qualifier before dispatch ever reaches a handler
// here. This guard is the belt to that suspenders: the reason a qualifier is
// refused (a new column has to be as long as the frame; a percentile's
// cutpoints would come from a different sample than the user asked for) belongs
// next to the code that would otherwise silently do the wrong thing.
std::optional<ojson> refuseQualifiers(const eval::ParsedCommand& parsed, const std::string& verb) {
  if (parsed.ifExpr.has_value() && !parsed.ifExpr->empty()) {
    return ojson(eval::unsupportedOption(
        "if " + *parsed.ifExpr,
        "`" + verb + "` in this build computes over every observation. Restrict first "
        "(`preserve` + `keep if ...`), then run " + verb + "."));
  }
  if (parsed.inRange.has_value() && !parsed.inRange->empty()) {
    return ojson(eval::unsupportedOption(
        "in " + *parsed.inRange,
        "`" + verb + "` in this build computes over every observation. Restrict first "
        "(`preserve` + `keep in ...`), then run " + verb + "."));
  }
  return std::nullopt;
}

std::optional<ojson> refuseWeights(const eval::ParsedCommand& parsed, const std::string& verb) {
  if (parsed.weightType.has_value() && !parsed.weightType->empty()) {
    return ojson(eval::unsupportedOption(
        *parsed.weightType + "=" + parsed.weightVar.value_or(""),
        "`" + verb + "` does not take weights in this build; a weighted percentile is a "
        "different estimator, not a reweighting of this one."));
  }
  return std::nullopt;
}

}  // namespace

// ===========================================================================
// split
// ===========================================================================
//
// PIET-side decomposition, then k `setcol --new` calls. Deliberately NOT
// `gen stubj = word(v, j)`: `word()` is whitespace-only, and `parse("-")` is
// the corpus's most common form by a wide margin. Deliberately not a new cobra
// `split` verb either -- that would make this a two-binary change.
ojson runSplit(const eval::ParsedCommand& parsed, SessionState& session) {
  if (auto err = eval::rejectUnknownOptions(parsed.opts, kVocabSplit.known,
                                            kVocabSplit.unimplemented))
    return ojson(*err);
  if (auto q = refuseQualifiers(parsed, "split")) return *q;
  if (!session.hasData()) return ojson(eval::noDataLoaded());
  if (parsed.varlist.empty())
    return rsSyntax("split: syntax is `split varname [, parse(chars) gen(stub) limit(#) destring]`");
  if (parsed.varlist.size() > 1)
    return rsError("split: this build splits ONE variable per command; `" +
                       joinWith(parsed.varlist, " ") +
                       "` names " + std::to_string(parsed.varlist.size()) +
                       ". Run split once per variable.",
                   "unimplemented");

  const std::string var = parsed.varlist[0];
  ReadFrame rf(session, {var});
  Datastore* ds = rf.get();
  if (ds == nullptr)
    return rsError("split: could not read the data" + (rf.why().empty() ? "" : " — " + rf.why()),
                   "data_io");
  if (!ds->has(var)) return rsError("variable " + var + " not found");
  if (ds->dtype(var) != "factor")
    return rsError("split: `" + var +
                   "' is a numeric variable; split works on string variables (Stata's `force` "
                   "option, which splits the numeric display text, is not implemented here).");

  // parse(): every CHARACTER given is a separator, and EVERY occurrence
  // separates, so `a--b` under parse("-") yields three parts and the middle one
  // is empty. With no parse(), the separator is whitespace: consecutive runs
  // collapse and leading/trailing whitespace is ignored (Stata's default).
  const std::optional<std::string> parseOpt = optStrAny(parsed.opts, {"parse", "p"});
  const std::string sepChars = parseOpt.value_or(std::string());
  const bool wsMode = !parseOpt.has_value();

  long long limit = -1;
  if (auto lv = optStr(parsed.opts, "limit")) {
    if (!parseStrictInt(*lv, &limit) || limit < 0)
      return rsSyntax("split: limit() requires a non-negative whole number, got `" + *lv + "'");
  }
  const bool destring = hasFlag(parsed.opts, "destring");
  std::string stub = optStrAny(parsed.opts, {"gen", "generate"}).value_or(var);
  if (!strutil::isValidStataIdentifier(stub + "1"))
    return rsSyntax("split: `" + stub + "' is not a usable variable-name stub — " +
                    strutil::stataIdentifierRuleText());

  const std::vector<std::string> values = columnAsStrings(*ds, var);

  auto splitOne = [&](const std::string& s) {
    std::vector<std::string> parts;
    if (wsMode) {
      std::string cur;
      for (char c : s) {
        if (c == ' ' || c == '\t') {
          if (!cur.empty()) {
            parts.push_back(cur);
            cur.clear();
          }
        } else {
          cur += c;
        }
      }
      if (!cur.empty()) parts.push_back(cur);
    } else {
      std::string cur;
      for (char c : s) {
        if (sepChars.find(c) != std::string::npos) {
          parts.push_back(cur);
          cur.clear();
        } else {
          cur += c;
        }
      }
      parts.push_back(cur);
      // A wholly empty source cell yields no parts at all, not one empty part.
      if (parts.size() == 1 && parts[0].empty()) parts.clear();
    }
    return parts;
  };

  std::vector<std::vector<std::string>> allParts;
  allParts.reserve(values.size());
  size_t k = 0;
  for (const auto& v : values) {
    allParts.push_back(splitOne(v));
    k = std::max(k, allParts.back().size());
  }
  if (limit >= 0) k = std::min(k, static_cast<size_t>(limit));

  // Every new name has to be legal and free BEFORE the first write, so a
  // collision on part 3 cannot leave parts 1 and 2 behind.
  std::vector<std::string> newVars;
  for (size_t j = 1; j <= k; ++j) {
    const std::string name = stub + std::to_string(j);
    ojson guard = newVarGuard(session, "split", name);
    if (!guard.is_null()) return guard;
    newVars.push_back(name);
  }

  std::string cur = *session.currentHbin();
  std::vector<std::string> partTypes(k);
  for (size_t j = 0; j < k; ++j) {
    std::vector<std::string> cells;
    cells.reserve(values.size());
    bool allNumeric = true;
    bool anyValue = false;
    for (const auto& parts : allParts) {
      if (j < parts.size()) {
        cells.push_back(parts[j]);
        if (!parts[j].empty()) {
          anyValue = true;
          double tmp = 0.0;
          if (!parseStrictDouble(parts[j], &tmp)) allNumeric = false;
        }
      } else {
        cells.emplace_back();
      }
    }
    // `destring` promotes part j to f64 only when EVERY non-empty value in that
    // part is a number -- per part, not per command, which is what makes
    // `split "1-a"` keep part 2 a string instead of failing the whole call.
    const std::string dtype = (destring && anyValue && allNumeric) ? "f64" : "factor";
    if (dtype == "f64") {
      // read_csv_column parses the f64 payload itself; blanks stay missing.
      // destring's storage rule: the smallest type that holds every value
      // (aer_w2 w2b_dataio split_destring: K2 byte).
      bool allInt = true, allFloat = true;
      double lo = 0.0, hi = 0.0;
      bool seen = false;
      for (auto& c : cells) {
        if (c.empty()) continue;
        double tmp = 0.0;
        parseStrictDouble(c, &tmp);
        if (!seen) { lo = hi = tmp; seen = true; }
        lo = std::min(lo, tmp);
        hi = std::max(hi, tmp);
        if (tmp != std::floor(tmp)) allInt = false;
        if (static_cast<double>(static_cast<float>(tmp)) != tmp) allFloat = false;
        c = numCell(tmp);
      }
      std::string t = "double";
      if (allInt && lo >= -127 && hi <= 100) t = "byte";
      else if (allInt && lo >= -32767 && hi <= 32740) t = "int";
      else if (allInt && lo >= -2147483647.0 && hi <= 2147483620.0) t = "long";
      else if (allFloat) t = "float";
      partTypes[j] = t;
    }
    std::string out;
    if (auto err = appendColumn(session, cur, newVars[j], cells, dtype, &out)) return *err;
    cur = out;
  }

  if (k > 0) session.load(cur);
  for (size_t j = 0; j < k; ++j) {
    if (!partTypes[j].empty()) {
      session.implicitFloatVars.erase(newVars[j]);
      session.varMeta[newVars[j]].stataType = partTypes[j];
    }
  }
  // r(nvars)/r(varlist), the two things a do-file reads back from split. Filled
  // HERE because the arm does not call captureRFromBlock for this command.
  session.eval.rValues.set("nvars", static_cast<double>(k));
  session.eval.rStrings.set("varlist", joinWith(newVars, " "));
  // split's whole r() set (r(nvars) and r(varlist)), so an r() it does not post reads missing (w2b append_ge(src):
  // r(sum) after split reads `.`).
  eval::declareRPosted(session.eval, "split", {"nvars", "varlist"});
  if (k == 0) {
    // Zero parts (an all-empty column, or limit(0)) is not an error: it is a
    // dataset with nothing to add, and r(nvars)=0 says so.
    return silent();
  }
  return mutationResult(session, "split", newVars);
}

// ===========================================================================
// xpose
// ===========================================================================

ojson runXpose(const eval::ParsedCommand& parsed, SessionState& session) {
  if (auto err = eval::rejectUnknownOptions(parsed.opts, kVocabXpose.known,
                                            kVocabXpose.unimplemented))
    return ojson(*err);
  if (auto q = refuseQualifiers(parsed, "xpose")) return *q;
  if (!session.hasData()) return ojson(eval::noDataLoaded());
  if (!hasFlag(parsed.opts, "clear")) {
    // Stata's own rc for "this would lose the data in memory".
    return rsErrorRc("xpose: you must specify the `clear` option — transposing replaces every "
                     "variable and observation in memory.",
                     4);
  }
  const bool wantVarname = hasFlag(parsed.opts, "varname");

  Datastore* ds = nullptr;
  try {
    ds = session.datastore();
  } catch (const std::exception& e) {
    return rsError(std::string("xpose: ") + e.what(), "data_io");
  }
  if (ds == nullptr) return ojson(eval::noDataLoaded());
  const int64_t n = ds->nRows();
  if (n == 0) return rsErrorRc("xpose: no observations", 2000);

  const std::vector<std::string> before = ds->names();
  std::vector<std::string> numeric;
  int dropped = 0;
  for (const auto& nm : before) {
    if (ds->dtype(nm) == "f64")
      numeric.push_back(nm);
    else
      ++dropped;
  }
  if (numeric.empty())
    return rsError("xpose: every variable is a string variable; there is nothing numeric to "
                   "transpose (this build drops string variables, as Stata does).");

  // The transpose: one row per surviving ORIGINAL column, one column per
  // original observation, named v1..vn. `varname` puts the original names in a
  // leading string column `_varname`.
  CsvTable t;
  if (wantVarname) {
    t.names.push_back("_varname");
    t.isFactor.push_back(1);
  }
  for (int64_t i = 0; i < n; ++i) {
    t.names.push_back("v" + std::to_string(i + 1));
    t.isFactor.push_back(0);
  }
  t.rows.reserve(numeric.size());
  for (const auto& nm : numeric) {
    std::vector<std::string> row;
    row.reserve(t.names.size());
    if (wantVarname) row.push_back(nm);
    const double* d = ds->col(nm).f64();
    for (int64_t i = 0; i < n; ++i) row.push_back(numCell(d[i]));
    t.rows.push_back(std::move(row));
  }

  std::string outPath;
  if (auto err = writeAndConvert(session, t, "xpose", &outPath)) return *err;
  session.load(outPath);
  if (auto bad = assertRowCount(session, "xpose", static_cast<int64_t>(numeric.size())))
    return *bad;

  ojson out = ojson::array();
  if (dropped > 0) {
    out.push_back(echoBlock("(xpose: " + std::to_string(dropped) + " string variable(s) dropped)"));
  }
  if (hasFlag(parsed.opts, "promote")) {
    // READ, not ignored: this build stores every column as a double, so there
    // is no narrower type to promote to. Saying so beats accepting a storage
    // decision that never happened.
    out.push_back(echoBlock(
        "(xpose: promote has no effect in this build — every transposed variable is stored as "
        "double)"));
  }
  out.push_back(mutationResult(session, "xpose", t.names, before));
  return out;
}

// ===========================================================================
// recast
// ===========================================================================
//
// Exact recasts change only metadata; force narrows the actual values through
// the same store boundary as generate/replace (w2e_float_default:1565–1637).
ojson runRecast(const eval::ParsedCommand& parsed, SessionState& session) {
  if (auto err = eval::rejectUnknownOptions(parsed.opts, kVocabRecast.known,
                                            kVocabRecast.unimplemented))
    return ojson(*err);
  if (auto q = refuseQualifiers(parsed, "recast")) return *q;
  if (!session.hasData()) return ojson(eval::noDataLoaded());
  if (parsed.varlist.size() < 2)
    return rsSyntax("recast: syntax is `recast type varlist [, force]`");

  const std::string type = lowerOf(parsed.varlist[0]);
  static const std::set<std::string> kNumericTypes = {"byte", "int", "long", "float", "double"};
  bool isNumeric = kNumericTypes.count(type) > 0;
  bool isString = false;
  {
    static const std::regex reStr(R"(^str([0-9]+)$)");
    std::smatch m;
    if (std::regex_match(type, m, reStr)) {
      isString = true;
      const long long w = std::stoll(m.str(1));
      if (w < 1 || w > 2045)
        return rsSyntax("recast: `" + type + "' — a str# width must be between 1 and 2045");
    } else if (type == "strl") {
      return rsError("recast: strL is not implemented in this build (the .dta writer has no strL "
                     "write path).",
                     "unimplemented");
    }
  }
  if (!isNumeric && !isString)
    return rsSyntax("recast: `" + parsed.varlist[0] +
                    "' is not a storage type — expected byte, int, long, float, double or str#");

  Datastore* ds = nullptr;
  try {
    ds = session.datastore();
  } catch (const std::exception& e) {
    return rsError(std::string("recast: ") + e.what(), "data_io");
  }
  if (ds == nullptr) return ojson(eval::noDataLoaded());

  // Validate EVERY name before writing ANY metadata: a half-applied recast is
  // metadata that disagrees with the command the user typed.
  for (size_t i = 1; i < parsed.varlist.size(); ++i) {
    const std::string& v = parsed.varlist[i];
    if (!ds->has(v)) return rsError("variable " + v + " not found");
    const bool colIsString = ds->dtype(v) == "factor";
    if (colIsString && isNumeric)
      return rsError("recast: `" + v +
                     "' is a string variable and " + type +
                     " is numeric; recast never converts between the two (use `destring`).");
    if (!colIsString && isString)
      return rsError("recast: `" + v + "' is a numeric variable and " + type +
                     " is a string type; recast never converts between the two (use `tostring`).");
  }
  const bool force = hasFlag(parsed.opts, "force");
  const std::map<std::string, cecon::StoreType> storeTypes = {
      {"byte", cecon::StoreType::Byte}, {"int", cecon::StoreType::Int},
      {"long", cecon::StoreType::Long}, {"float", cecon::StoreType::Float},
      {"double", cecon::StoreType::Double}};
  // Decide each variable before mutation. Non-force loss is a successful no-op
  // for that variable, not a declaration of a type its values cannot hold.
  std::vector<std::string> accepted, changed;
  std::vector<std::string> args = {"eval", "--data", *session.currentHbin()};
  std::set<std::string> seen;
  for (size_t i = 1; i < parsed.varlist.size(); ++i) {
    const std::string& name = parsed.varlist[i];
    if (!seen.insert(name).second) continue;
    bool changes = false;
    if (isNumeric) {
      const auto& col = ds->col(name);
      for (int64_t row = 0; row < ds->nRows(); ++row) {
        const double old = col.f64()[row];
        const double value = cecon::frame_detail::round_to_store(old, storeTypes.at(type));
        if (!std::isnan(old) && (std::isnan(value) || value != old)) { changes = true; break; }
      }
    } else {
      const auto& col = ds->col(name);
      const size_t width = static_cast<size_t>(std::stoll(type.substr(3)));
      for (const auto& value : col.levels)
        if (value.size() > width) { changes = true; break; }
      // A string force truncation needs a byte-preserving string store. Do not
      // mislabel an unchanged payload as a narrower string in its absence.
      if (changes && force)
        return rsError("recast: forced string truncation is not implemented", "unimplemented");
    }
    if (changes && !force) continue;
    accepted.push_back(name);
    if (changes) {
      changed.push_back(name);
      args.insert(args.end(), {"--gen", name + " = " + name});
    }
  }
  if (!changed.empty()) {
    storage::appendStoreTypeArg(args, type);
    const std::string out = nextPath(session.workdir(), "recast");
    args.insert(args.end(), {"--out", out});
    if (auto err = runVerb(args)) return *err;
    session.load(out);
  }
  for (const auto& name : accepted) {
    session.varMeta[name].stataType = type;
    session.implicitFloatVars.erase(name);
  }
  if (!changed.empty()) return mutationResult(session, "gen", {}, {}, changed);
  return silent();
}

// ===========================================================================
// insobs
// ===========================================================================

ojson runInsobs(const eval::ParsedCommand& parsed, SessionState& session) {
  if (auto err = eval::rejectUnknownOptions(parsed.opts, kVocabInsobs.known,
                                            kVocabInsobs.unimplemented))
    return ojson(*err);
  if (auto q = refuseQualifiers(parsed, "insobs")) return *q;
  if (parsed.varlist.empty())
    return rsSyntax("insobs: syntax is `insobs # [, before(#) | after(#)]`");
  long long m = 0;
  if (!parseStrictInt(parsed.varlist[0], &m) || m <= 0)
    return rsSyntax("insobs: `" + parsed.varlist[0] +
                    "' is not a positive whole number of observations");

  const std::optional<std::string> beforeOpt = optStr(parsed.opts, "before");
  const std::optional<std::string> afterOpt = optStr(parsed.opts, "after");
  if (beforeOpt.has_value() && afterOpt.has_value())
    return rsSyntax("insobs: before() and after() cannot both be specified");

  if (!session.hasData()) {
    if (beforeOpt.has_value() || afterOpt.has_value())
      return rsError("insobs: before()/after() need data in memory to insert into");
    return runSetObs(std::to_string(m), session);
  }

  Datastore* ds = nullptr;
  try {
    ds = session.datastore();
  } catch (const std::exception& e) {
    return rsError(std::string("insobs: ") + e.what(), "data_io");
  }
  if (ds == nullptr) return ojson(eval::noDataLoaded());
  const int64_t n = ds->nRows();

  // Where the blank rows go. All three forms take the SAME string-safe path
  // below; the plain form is simply "insert before observation n+1".
  //
  // The plan had the plain form delegate to `set obs N+m` -- one implementation
  // of "pad the frame with blank observations" rather than two
  // (handlers_m3.cpp:2747). MEASURED 2026-09-04, it cannot be reused: that path
  // is broken twice over on main, both pre-existing, both filed rather than
  // fixed here.
  //
  //   BUG-411 -- it calls `runCobraJson` on `append`, which prints a plain
  //   text line ("append: wrote ... = N rows"), so every `set obs N` that GROWS
  //   a loaded frame dies with "cobra produced non-JSON output". A two-line
  //   do-file reproduces it. `set obs` on an EMPTY session takes a different
  //   branch and works, which is why it survived this long.
  //
  //   BUG-412 -- underneath that, its pad CSV types every string column as
  //   numeric, so `cobra append` promotes the master's string column and
  //   replaces each value with its factor CODE: a frame whose `state` reads
  //   "al" comes back reading 0. BUG-411 is the only reason nobody has been
  //   bitten yet -- the visible failure masks the silent one, exactly as
  //   BUG-401's over-block masked BUG-404.
  //
  // The path below does not have BUG-412 because `writeAndConvert` carries a
  // per-column factor flag, so the pad frame is built with the master's own
  // dtypes and `append` has nothing to promote. Verified on `fatalities`:
  // state[1]=="al" before, state[3]=="al" after `insobs 2, before(1)`.
  long long pos = 0;
  if (!beforeOpt.has_value() && !afterOpt.has_value()) {
    pos = n + 1;
  } else if (beforeOpt.has_value()) {
    if (!parseStrictInt(*beforeOpt, &pos) || pos < 1 || pos > n + 1)
      return rsSyntax("insobs: before(" + *beforeOpt + ") must be an observation number between 1 and " +
                      std::to_string(n + 1));
  } else {
    if (!parseStrictInt(*afterOpt, &pos) || pos < 0 || pos > n)
      return rsSyntax("insobs: after(" + *afterOpt + ") must be an observation number between 0 and " +
                      std::to_string(n));
    pos += 1;  // `after(k)` is `before(k+1)`
  }

  // Positional insert, in four engine calls: stamp a sequence, append the blank
  // rows carrying a fractional sequence value that lands them exactly where the
  // user asked, sort, drop the stamp.
  const std::string seqVar = "__piet_insobs_seq";
  if (ds->has(seqVar))
    return rsError("insobs: the scratch column " + seqVar +
                   " already exists in this dataset; rename it and retry.");

  const std::string basePath = *session.currentHbin();
  const std::string seqPath = nextPath(session.workdir(), "insobs_seq");
  if (auto err = runVerb({"eval", "--data", basePath, "--egen",
                          std::string(seqVar) + " = seq()", "--out", seqPath}))
    return *err;
  // The blank-row frame and the sort below each write scratch, which pushes the
  // reclaim budget over and makes these two -- the oldest at that moment -- the
  // first eviction candidates while they are still the thing being read from.
  SessionState::ScratchPin pin(session, {basePath, seqPath});

  CsvTable t;
  for (const auto& nm : ds->names()) {
    t.names.push_back(nm);
    t.isFactor.push_back(ds->dtype(nm) == "factor" ? 1 : 0);
  }
  t.names.push_back(seqVar);
  t.isFactor.push_back(0);
  // Every new cell is MISSING -- `.`, never 0 -- and the sequence stamp is the
  // half-integer just below the target position, so an ascending sort drops the
  // new rows immediately before observation `pos` without disturbing any other
  // row's order.
  const std::string stamp = eval::pythonFloatRepr(static_cast<double>(pos) - 0.5);
  for (long long r = 0; r < m; ++r) {
    std::vector<std::string> row(t.names.size());
    row.back() = stamp;
    t.rows.push_back(std::move(row));
  }
  std::string padPath;
  if (auto err = writeAndConvert(session, t, "insobs_pad", &padPath)) return *err;

  const std::string appended = nextPath(session.workdir(), "insobs");
  if (auto err = runVerb({"append", "--data", seqPath, "--using", padPath, "--out", appended}))
    return *err;
  session.load(appended);

  ojson sorted = runSort({seqVar}, session);
  if (isErrorBlock(sorted)) return sorted;

  const std::string finalPath = nextPath(session.workdir(), "insobs_done");
  if (auto err = runVerb({"eval", "--data", *session.currentHbin(), "--drop", seqVar, "--out",
                          finalPath}))
    return *err;
  session.load(finalPath);

  if (auto bad = assertRowCount(session, "insobs", n + m)) return *bad;
  return mutationResult(session, "insobs");
}

// ===========================================================================
// stack
// ===========================================================================

ojson runStack(const eval::ParsedCommand& parsed, SessionState& session) {
  if (auto err = eval::rejectUnknownOptions(parsed.opts, kVocabStack.known,
                                            kVocabStack.unimplemented))
    return ojson(*err);
  if (auto q = refuseQualifiers(parsed, "stack")) return *q;
  if (!session.hasData()) return ojson(eval::noDataLoaded());
  if (parsed.varlist.empty())
    return rsSyntax("stack: syntax is `stack varlist, into(newvars) [clear wide group(#)]`");
  const bool wide = hasFlag(parsed.opts, "wide");

  Datastore* ds = nullptr;
  try {
    ds = session.datastore();
  } catch (const std::exception& e) {
    return rsError(std::string("stack: ") + e.what(), "data_io");
  }
  if (ds == nullptr) return ojson(eval::noDataLoaded());
  for (const auto& v : parsed.varlist)
    if (!ds->has(v)) return rsError("variable " + v + " not found");

  const size_t nv = parsed.varlist.size();
  // [D] stack: into(newvars) names one block's variables, so without group()
  // the varlist is cut into blocks of as many variables as into() names
  // (`stack x1 y x2 y, into(a b) clear` stacks (x1,y) over (x2,y);
  // w2_esample_copies.do:281, StataBE `0|0|122|`, .log:702). Only with
  // neither option is every variable its own block.
  std::optional<std::string> intoOpt = optStr(parsed.opts, "into");
  long long groups = static_cast<long long>(nv);
  if (auto g = optStr(parsed.opts, "group")) {
    if (!parseStrictInt(*g, &groups) || groups < 1)
      return rsSyntax("stack: group() requires a positive whole number, got `" + *g + "'");
  } else if (intoOpt.has_value()) {
    const size_t k = splitWs(*intoOpt).size();
    if (k == 0 || nv % k != 0)
      return rsSyntax("stack: " + std::to_string(nv) + " variable(s) do not divide into blocks of " +
                      std::to_string(k) + ", the number of into() names");
    groups = static_cast<long long>(nv / k);
  }
  if (static_cast<size_t>(groups) > nv || nv % static_cast<size_t>(groups) != 0)
    return rsSyntax("stack: " + std::to_string(nv) + " variable(s) do not divide into " +
                    std::to_string(groups) + " equal group(s)");
  const size_t per = nv / static_cast<size_t>(groups);

  std::vector<std::string> intoNames;
  if (intoOpt.has_value()) {
    intoNames = splitWs(*intoOpt);
  } else if (wide) {
    return rsSyntax("stack: into() is required with `wide` — without it the stacked columns would "
                    "take the names of the variables `wide` is keeping.");
  } else {
    // Stata's own default: the new variables take the first group's names. Safe
    // because the stacked data replace the originals.
    intoNames.assign(parsed.varlist.begin(), parsed.varlist.begin() + static_cast<long>(per));
  }
  if (intoNames.size() != per)
    return rsSyntax("stack: into() names " + std::to_string(intoNames.size()) +
                    " variable(s) but each group has " + std::to_string(per));
  for (const auto& nm : intoNames)
    if (!strutil::isValidStataIdentifier(nm))
      return rsSyntax("stack: invalid variable name `" + nm + "' — " +
                      strutil::stataIdentifierRuleText());

  const int64_t n = ds->nRows();
  const std::vector<std::string> before = ds->names();

  // Column texts, read once. A stacked column is a factor if ANY of the source
  // columns feeding that position is one: mixing a string into a numeric column
  // has to widen, never truncate.
  std::map<std::string, std::vector<std::string>> textOf;
  for (const auto& nm : before) textOf[nm] = columnAsStrings(*ds, nm);

  CsvTable t;
  t.names.push_back("_stack");
  t.isFactor.push_back(0);
  for (size_t c = 0; c < per; ++c) {
    bool anyFactor = false;
    for (long long g = 0; g < groups; ++g)
      if (ds->dtype(parsed.varlist[static_cast<size_t>(g) * per + c]) == "factor") anyFactor = true;
    t.names.push_back(intoNames[c]);
    t.isFactor.push_back(anyFactor ? 1 : 0);
  }
  if (wide) {
    for (const auto& nm : before) {
      if (std::find(t.names.begin(), t.names.end(), nm) != t.names.end())
        return rsSyntax("stack: `wide` keeps the original variables, and into() reuses the name `" +
                        nm + "' — choose different into() names.");
      t.names.push_back(nm);
      t.isFactor.push_back(ds->dtype(nm) == "factor" ? 1 : 0);
    }
  }
  t.rows.reserve(static_cast<size_t>(n) * static_cast<size_t>(groups));
  for (long long g = 0; g < groups; ++g) {
    for (int64_t i = 0; i < n; ++i) {
      std::vector<std::string> row;
      row.reserve(t.names.size());
      row.push_back(std::to_string(g + 1));
      for (size_t c = 0; c < per; ++c)
        row.push_back(textOf[parsed.varlist[static_cast<size_t>(g) * per + c]][static_cast<size_t>(i)]);
      if (wide)
        for (const auto& nm : before) row.push_back(textOf[nm][static_cast<size_t>(i)]);
      t.rows.push_back(std::move(row));
    }
  }

  std::string outPath;
  if (auto err = writeAndConvert(session, t, "stack", &outPath)) return *err;
  session.load(outPath);
  if (auto bad = assertRowCount(session, "stack", n * groups)) return *bad;
  // BUG-1147: `clear` only silences stack's warning; without it Stata warns, waits
  // for a key (a no-op in a do-file), and stacks all the same, whatever the data in
  // memory: `stack x1 x2 y z1, into(a b)` is rc 0, _N 4 (StataBE 19.5, stack.ado
  // never reads c(changed)). The warning comes after the varlist checks, so a
  // refused stack does not print it, and `quietly` drops it.
  if (hasFlag(parsed.opts, "clear")) return mutationResult(session, "stack", t.names, before);
  ojson out = ojson::array();
  out.push_back(displayBlock("Warning: data in memory will be lost.\n"
                             "         Press any key to continue, Ctrl-Break to abort."));
  out.push_back(mutationResult(session, "stack", t.names, before));
  return out;
}

// ===========================================================================
// separate
// ===========================================================================

ojson runSeparate(const eval::ParsedCommand& parsed, SessionState& session) {
  if (auto err = eval::rejectUnknownOptions(parsed.opts, kVocabSeparate.known,
                                            kVocabSeparate.unimplemented))
    return ojson(*err);
  if (auto q = refuseQualifiers(parsed, "separate")) return *q;
  if (!session.hasData()) return ojson(eval::noDataLoaded());
  if (parsed.varlist.size() != 1)
    return rsSyntax("separate: syntax is `separate varname, by(groupvar) [generate(stub) "
                    "shortlabel]`");
  const std::string var = parsed.varlist[0];
  const std::optional<std::string> byOpt = optStr(parsed.opts, "by");
  if (!byOpt.has_value() || byOpt->empty())
    return rsSyntax("separate: by() is required");
  const std::vector<std::string> byVars = splitWs(*byOpt);
  if (byVars.size() != 1)
    return rsError("separate: this build separates by ONE variable; by(" + *byOpt + ") names " +
                       std::to_string(byVars.size()) + ".",
                   "unimplemented");
  const std::string gvar = byVars[0];
  const bool shortLabel = hasFlag(parsed.opts, "shortlabel");
  const std::string stub = optStrAny(parsed.opts, {"generate", "gen"}).value_or(var);

  ReadFrame rf(session, {var, gvar});
  Datastore* ds = rf.get();
  if (ds == nullptr)
    return rsError("separate: could not read the data" + (rf.why().empty() ? "" : " — " + rf.why()),
                   "data_io");
  if (!ds->has(var)) return rsError("variable " + var + " not found");
  if (!ds->has(gvar)) return rsError("variable " + gvar + " not found");
  if (ds->dtype(var) == "factor")
    return rsError("separate: `" + var +
                   "' is a string variable; separate builds numeric columns.");

  // The distinct levels of the by() variable, and for each one the expression
  // that selects it. A factor by() compares against the level TEXT, which the
  // engine's expression parser resolves directly (`factorvar == "literal"`).
  const bool gIsFactor = ds->dtype(gvar) == "factor";
  std::vector<std::string> suffixes;   // name suffix per level
  std::vector<std::string> labelText;  // level as the label shows it
  std::vector<std::string> predicates;
  if (gIsFactor) {
    const cecon::Column& c = ds->col(gvar);
    std::set<std::string> seen;
    const int32_t* codes = c.codes();
    for (int64_t i = 0; i < ds->nRows(); ++i) {
      const int32_t code = codes[i];
      if (code < 0 || static_cast<size_t>(code) >= c.levels.size()) continue;
      seen.insert(c.levels[static_cast<size_t>(code)]);
    }
    for (const auto& lv : seen) {
      if (lv.find('"') != std::string::npos)
        return rsError("separate: the level `" + lv + "' of " + gvar +
                       " contains a double quote, which this build cannot express in the "
                       "selection expression.");
      std::string sfx;
      for (char ch : lv) sfx += (std::isalnum(static_cast<unsigned char>(ch)) ? ch : '_');
      suffixes.push_back(sfx);
      labelText.push_back(lv);
      predicates.push_back(gvar + "==\"" + lv + "\"");
    }
  } else {
    std::vector<double> g = columnAsDoubles(*ds, gvar);
    std::set<double> seen;
    for (double v : g)
      if (!std::isnan(v)) seen.insert(v);
    for (double lv : seen) {
      suffixes.push_back(levelSuffix(lv));
      labelText.push_back(levelText(lv));
      predicates.push_back(gvar + "==" + eval::pythonFloatRepr(lv));
    }
  }
  if (suffixes.empty())
    return rsError("separate: " + gvar + " has no non-missing values to separate by");
  if (suffixes.size() > 100)
    return rsError("separate: " + gvar + " has " + std::to_string(suffixes.size()) +
                   " distinct values; this build refuses to create more than 100 columns from one "
                   "`separate` (raise the by() variable's granularity instead).");

  std::vector<std::string> newVars;
  for (const auto& sfx : suffixes) {
    const std::string nm = stub + sfx;
    ojson guard = newVarGuard(session, "separate", nm);
    if (!guard.is_null()) return guard;
    if (std::find(newVars.begin(), newVars.end(), nm) != newVars.end())
      return rsError("separate: two levels of " + gvar + " both produce the variable name `" + nm +
                     "'; use generate() to pick a different stub.");
    newVars.push_back(nm);
  }

  // One `--gen` per engine call (cobra's eval takes one assignment at a time),
  // each step adopting its own output as the head before the next reads it —
  // the same shape a sequence of `gen` commands already takes (handlers.cpp's
  // runGen), which is what makes the resident-frame opt-in safe: no
  // intermediate is ever read as a file behind the session's back.
  SessionState::ResidentEligible resident(session);
  for (size_t j = 0; j < newVars.size(); ++j) {
    const std::string out = nextPath(session.workdir(), "separate");
    const std::string expr = newVars[j] + "=cond((" + predicates[j] + "),(" + var + "),.)";
    if (auto err = runVerb({"eval", "--data", *session.currentHbin(), "--gen", expr, "--out", out}))
      return *err;
    session.load(out);
  }
  for (size_t j = 0; j < newVars.size(); ++j) {
    session.varMeta[newVars[j]].label =
        shortLabel ? labelText[j] : (var + ", " + gvar + " == " + labelText[j]);
  }
  return mutationResult(session, "separate", newVars);
}

// ===========================================================================
// mvencode
// ===========================================================================

ojson runMvencode(const eval::ParsedCommand& parsed, SessionState& session) {
  if (auto err = eval::rejectUnknownOptions(parsed.opts, kVocabMvencode.known,
                                            kVocabMvencode.unimplemented))
    return ojson(*err);
  // `if` restricts both the override check and the recode to the marked sample
  // (mvencode marks the sample without requiring a varlist, then every
  // count/replace is restricted to it); 007's Wave 2 cleaning runs `mvencode ... if consentall == 1`.
  // `in` stays refused.
  std::string ifCond;
  if (parsed.ifExpr.has_value() && !parsed.ifExpr->empty()) ifCond = *parsed.ifExpr;
  {
    eval::ParsedCommand noIf = parsed;
    noIf.ifExpr.reset();
    if (auto q = refuseQualifiers(noIf, "mvencode")) return *q;
  }
  if (!session.hasData()) return ojson(eval::noDataLoaded());
  if (parsed.varlist.empty())
    return rsSyntax("mvencode: syntax is `mvencode varlist | _all, mv(#) [override]`");

  const std::optional<std::string> mvOpt = optStr(parsed.opts, "mv");
  if (!mvOpt.has_value())
    return rsSyntax("mvencode: mv(#) is required");
  // The per-missing-code rule list, `mv(. = 9 \ .a = 8)`, encodes EACH extended
  // missing separately. This build's frame collapses every extended missing to
  // plain `.` (the .dta reader/writer Q5 ruling), so honouring only the first
  // rule would silently recode `.a` as if it were `.`.
  if (mvOpt->find('=') != std::string::npos || mvOpt->find('\\') != std::string::npos)
    return rsError("mvencode: the per-code rule list `mv(" + *mvOpt +
                       ")` is not implemented — this build stores one missing value (`.`), so a "
                       "rule list that distinguishes .a/.b/... cannot be honoured.",
                   "unimplemented");
  double mvValue = 0.0;
  if (!parseStrictDouble(*mvOpt, &mvValue))
    return rsSyntax("mvencode: mv(" + *mvOpt + ") requires a number");
  const bool allowOverride = hasFlag(parsed.opts, "override");

  Datastore* ds = nullptr;
  try {
    ds = session.datastore();
  } catch (const std::exception& e) {
    return rsError(std::string("mvencode: ") + e.what(), "data_io");
  }
  if (ds == nullptr) return ojson(eval::noDataLoaded());

  std::vector<std::string> targets;
  const bool all = parsed.varlist.size() == 1 && lowerOf(parsed.varlist[0]) == "_all";
  if (all) {
    for (const auto& nm : ds->names())
      if (ds->dtype(nm) == "f64") targets.push_back(nm);
  } else {
    for (const auto& v : parsed.varlist) {
      if (!ds->has(v)) return rsError("variable " + v + " not found");
      // Stata skips string variables rather than failing the command.
      if (ds->dtype(v) == "f64") targets.push_back(v);
    }
  }
  if (targets.empty())
    return rsError("mvencode: no numeric variable to encode (string variables are skipped)");

  // PRE-SCAN, before any write: encoding into a value the data already holds
  // makes the two indistinguishable afterwards, which is why Stata demands
  // `override` for it.
  if (!allowOverride && ifCond.empty()) {
    for (const auto& v : targets) {
      const double* d = ds->col(v).f64();
      for (int64_t i = 0; i < ds->nRows(); ++i) {
        if (d[i] == mvValue)
          return rsError("mvencode: " + v + " already contains " + eval::pythonFloatRepr(mvValue) +
                         "; specify `override` to encode missing as a value the data already uses.");
      }
    }
  } else if (!allowOverride) {
    // Within the sample only: count the clashes the way the ado does.
    for (const auto& v : targets) {
      ojson r = dispatch("quietly count if " + v + " == " + eval::pythonFloatRepr(mvValue) +
                             " & (" + ifCond + ")",
                         session);
      for (const auto& b : r)
        if (b.is_object() && b.value("type", std::string()) == "error") return b;
      const double* n = session.eval.rValues.find("N");
      if (n != nullptr && *n > 0)
        return rsError("mvencode: " + v + " already contains " + eval::pythonFloatRepr(mvValue) +
                       "; specify `override` to encode missing as a value the data already uses.");
    }
  }

  // One `--gen` per engine call, each step adopting its own output as the head
  // before the next reads it — byte-for-byte the shape a sequence of `replace`
  // commands already takes (handlers.cpp's runReplace), which is what makes the
  // resident-frame opt-in safe here: no intermediate is ever read as a file
  // behind the session's back.
  SessionState::ResidentEligible resident(session);
  for (const auto& v : targets) {
    const std::string out = nextPath(session.workdir(), "mvencode");
    const std::string cond =
        ifCond.empty() ? "missing(" + v + ")" : "missing(" + v + ") & (" + ifCond + ")";
    const std::string expr = v + "=cond(" + cond + "," + eval::pythonFloatRepr(mvValue) + "," + v + ")";
    if (auto err = runVerb({"eval", "--data", *session.currentHbin(), "--gen", expr, "--out", out}))
      return *err;
    session.load(out);
  }
  // In-place rewrites: they belong in the rewrittenVars slot (the LAST
  // parameter), not droppedVars (the third) — mutation_result.h's own warning.
  return mutationResult(session, "gen", {}, {}, targets);
}

// ===========================================================================
// sample
// ===========================================================================

ojson runSample(const eval::ParsedCommand& parsed, SessionState& session) {
  if (auto err = eval::rejectUnknownOptions(parsed.opts, kVocabSample.known,
                                            kVocabSample.unimplemented))
    return ojson(*err);
  if (auto q = refuseQualifiers(parsed, "sample")) return *q;
  if (!session.hasData()) return ojson(eval::noDataLoaded());
  if (parsed.varlist.empty())
    return rsSyntax("sample: syntax is `sample # [, count]` (# is a percent unless `count` is "
                    "given)");
  double amount = 0.0;
  if (!parseStrictDouble(parsed.varlist[0], &amount))
    return rsSyntax("sample: `" + parsed.varlist[0] + "' is not a number");
  const bool byCount = hasFlag(parsed.opts, "count");

  Datastore* ds = nullptr;
  try {
    ds = session.datastore();
  } catch (const std::exception& e) {
    return rsError(std::string("sample: ") + e.what(), "data_io");
  }
  if (ds == nullptr) return ojson(eval::noDataLoaded());
  const int64_t n = ds->nRows();

  int64_t k = 0;
  if (byCount) {
    if (amount != std::floor(amount) || amount < 0)
      return rsSyntax("sample: with `count`, # must be a non-negative whole number");
    k = static_cast<int64_t>(amount);
    if (k > n)
      return rsError("sample: cannot draw " + std::to_string(k) + " observation(s) from " +
                     std::to_string(n));
  } else {
    if (!(amount >= 0.0 && amount <= 100.0))
      return rsSyntax("sample: a percent must be between 0 and 100");
    // Round-half-up, as the plan's Stata-semantics ruling states.
    k = static_cast<int64_t>(std::floor(static_cast<double>(n) * amount / 100.0 + 0.5));
    if (k > n) k = n;
  }

  const std::string keepVar = "__piet_sample_keep";
  if (ds->has(keepVar))
    return rsError("sample: the scratch column " + keepVar +
                   " already exists in this dataset; rename it and retry.");

  // A key per ROW, then keep the k smallest. Equivalent to drawing k rows
  // without replacement, and reproducible without any ordering assumption: the
  // row index breaks a key tie so the answer never depends on sort stability.
  const uint64_t seed = drawSeed(session, std::nullopt);
  std::vector<std::pair<uint32_t, int64_t>> keys;
  keys.reserve(static_cast<size_t>(n));
  for (int64_t j = 0; j < n; ++j) keys.emplace_back(rowKey32(seed, j), j);
  std::sort(keys.begin(), keys.end());
  std::vector<char> keep(static_cast<size_t>(n), 0);
  for (int64_t j = 0; j < k; ++j) keep[static_cast<size_t>(keys[static_cast<size_t>(j)].second)] = 1;

  std::vector<std::string> cells;
  cells.reserve(static_cast<size_t>(n));
  for (int64_t j = 0; j < n; ++j) cells.push_back(keep[static_cast<size_t>(j)] ? "1" : "0");

  std::string flagged;
  if (auto err = appendColumn(session, *session.currentHbin(), keepVar, cells, "f64", &flagged))
    return *err;
  const std::string out = nextPath(session.workdir(), "sample");
  if (auto err = runVerb({"eval", "--data", flagged, "--keepif", std::string(keepVar) + "==1",
                          "--drop", keepVar, "--out", out}))
    return *err;
  session.load(out);
  if (auto bad = assertRowCount(session, "sample", k)) return *bad;
  return mutationResult(session, "sample");
}

// ===========================================================================
// splitsample
// ===========================================================================

ojson runSplitsample(const eval::ParsedCommand& parsed, SessionState& session) {
  if (auto err = eval::rejectUnknownOptions(parsed.opts, kVocabSplitsample.known,
                                            kVocabSplitsample.unimplemented))
    return ojson(*err);
  if (auto q = refuseQualifiers(parsed, "splitsample")) return *q;
  if (!session.hasData()) return ojson(eval::noDataLoaded());

  const std::string gen = optStrAny(parsed.opts, {"generate", "gen"}).value_or("_sample");
  long long nsplit = 2;
  if (auto v = optStr(parsed.opts, "nsplit")) {
    if (!parseStrictInt(*v, &nsplit) || nsplit < 2)
      return rsSyntax("splitsample: nsplit() requires a whole number of at least 2, got `" + *v +
                      "'");
  }
  // BUG-1120 review: generate() is read as a newvarlist, so a storage type is
  // "byte invalid varname" r(198) and if/in/using/with open a qualifier the option
  // does not take, "if not allowed" r(101) (StataBE 19.5).
  if (auto r = strutil::newVarOptionRefusal(gen)) return rsErrorRc(r->message, r->rc);
  ojson guard = newVarGuard(session, "splitsample", gen);
  if (!guard.is_null()) return guard;

  Datastore* ds = nullptr;
  try {
    ds = session.datastore();
  } catch (const std::exception& e) {
    return rsError(std::string("splitsample: ") + e.what(), "data_io");
  }
  if (ds == nullptr) return ojson(eval::noDataLoaded());
  const int64_t n = ds->nRows();
  if (n == 0) return rsErrorRc("splitsample: no observations", 2000);

  const uint64_t seed = drawSeed(session, optStr(parsed.opts, "rseed"));
  std::vector<std::pair<uint32_t, int64_t>> keys;
  keys.reserve(static_cast<size_t>(n));
  for (int64_t j = 0; j < n; ++j) keys.emplace_back(rowKey32(seed, j), j);
  std::sort(keys.begin(), keys.end());
  // Rank r (0-based) -> group floor(r*nsplit/n)+1. Equal sizes when nsplit
  // divides n; otherwise the remainder lands on the LOW groups first, which is
  // the plan's stated convention.
  std::vector<int64_t> grp(static_cast<size_t>(n), 0);
  for (int64_t r = 0; r < n; ++r) {
    const int64_t g = (r * nsplit) / n + 1;
    grp[static_cast<size_t>(keys[static_cast<size_t>(r)].second)] = g;
  }
  std::vector<std::string> cells;
  cells.reserve(static_cast<size_t>(n));
  for (int64_t j = 0; j < n; ++j) cells.push_back(std::to_string(grp[static_cast<size_t>(j)]));

  std::string out;
  if (auto err = appendColumn(session, *session.currentHbin(), gen, cells, "f64", &out)) return *err;
  session.load(out);

  ojson blocks = ojson::array();
  if (hasFlag(parsed.opts, "show")) {
    std::vector<int64_t> counts(static_cast<size_t>(nsplit) + 1, 0);
    for (int64_t j = 0; j < n; ++j) counts[static_cast<size_t>(grp[static_cast<size_t>(j)])] += 1;
    std::ostringstream oss;
    oss << gen << "  |      Freq.\n";
    oss << "------------+------------\n";
    for (long long g = 1; g <= nsplit; ++g) {
      char buf[128];
      std::snprintf(buf, sizeof(buf), "%11lld | %10lld\n", g,
                    static_cast<long long>(counts[static_cast<size_t>(g)]));
      oss << buf;
    }
    blocks.push_back(displayBlock(oss.str()));
  }
  blocks.push_back(mutationResult(session, "splitsample", {gen}));
  return blocks;
}

// ===========================================================================
// drawnorm
// ===========================================================================

namespace {

// Dense lower Cholesky in place. false = not positive definite, which is a
// REFUSAL rather than a repaired matrix: drawing from a silently patched
// covariance answers a question the user did not ask.
bool cholesky(std::vector<std::vector<double>>& a) {
  const size_t k = a.size();
  std::vector<std::vector<double>> L(k, std::vector<double>(k, 0.0));
  for (size_t i = 0; i < k; ++i) {
    for (size_t j = 0; j <= i; ++j) {
      double s = a[i][j];
      for (size_t m = 0; m < j; ++m) s -= L[i][m] * L[j][m];
      if (i == j) {
        if (!(s > 0.0)) return false;
        L[i][j] = std::sqrt(s);
      } else {
        L[i][j] = s / L[j][j];
      }
    }
  }
  a = std::move(L);
  return true;
}

// A numeric option that may be a NUMLIST or the name of a session matrix.
// Returns nullopt with `*err` set when it is neither.
std::optional<std::vector<double>> numsOrMatrixRow(SessionState& session, const std::string& verb,
                                                   const std::string& optName,
                                                   const std::string& spec, ojson* err) {
  const std::vector<std::string> toks = splitWs(spec);
  std::vector<double> out;
  bool allNums = !toks.empty();
  for (const auto& t : toks) {
    double v = 0.0;
    if (!parseStrictDouble(t, &v)) {
      allNums = false;
      break;
    }
    out.push_back(v);
  }
  if (allNums) return out;
  if (toks.size() == 1) {
    if (const eval::EvalMatrix* m = session.eval.findMatrix(toks[0])) {
      out.clear();
      for (const auto& row : m->data)
        for (double v : row) out.push_back(v);
      return out;
    }
  }
  *err = rsSyntax(verb + ": " + optName + "(" + spec +
                  ") is neither a list of numbers nor the name of a matrix in memory");
  return std::nullopt;
}

}  // namespace

ojson runDrawnorm(const eval::ParsedCommand& parsed, SessionState& session) {
  if (auto err = eval::rejectUnknownOptions(parsed.opts, kVocabDrawnorm.known,
                                            kVocabDrawnorm.unimplemented))
    return ojson(*err);
  if (auto q = refuseQualifiers(parsed, "drawnorm")) return *q;
  if (parsed.varlist.empty())
    return rsSyntax("drawnorm: syntax is `drawnorm newvarlist, n(#) [means() sds() corr()|cov() "
                    "seed() clear]`");
  const size_t k = parsed.varlist.size();
  for (const auto& nm : parsed.varlist)
    if (!strutil::isValidStataIdentifier(nm))
      return rsSyntax("drawnorm: invalid variable name `" + nm + "' — " +
                      strutil::stataIdentifierRuleText());

  if (hasFlag(parsed.opts, "clear")) {
    // `clear` here is Stata's ordinary `clear`: drop the data in memory before
    // drawing. Routed through runClear so this is ONE implementation of what
    // clearing means, not a second one that forgets a plane.
    ojson cleared = runClear("", session);
    if (isErrorBlock(cleared)) return cleared;
  }

  std::vector<double> means(k, 0.0);
  std::vector<double> sds(k, 1.0);
  if (auto m = optStr(parsed.opts, "means")) {
    ojson err;
    auto v = numsOrMatrixRow(session, "drawnorm", "means", *m, &err);
    if (!v.has_value()) return err;
    if (v->size() != k)
      return rsSyntax("drawnorm: means() has " + std::to_string(v->size()) + " element(s) for " +
                      std::to_string(k) + " variable(s)");
    means = *v;
  }
  if (auto s = optStr(parsed.opts, "sds")) {
    ojson err;
    auto v = numsOrMatrixRow(session, "drawnorm", "sds", *s, &err);
    if (!v.has_value()) return err;
    if (v->size() != k)
      return rsSyntax("drawnorm: sds() has " + std::to_string(v->size()) + " element(s) for " +
                      std::to_string(k) + " variable(s)");
    for (double x : *v)
      if (!(x >= 0.0)) return rsSyntax("drawnorm: sds() must be non-negative");
    sds = *v;
  }

  const std::optional<std::string> corrOpt = optStr(parsed.opts, "corr");
  const std::optional<std::string> covOpt = optStr(parsed.opts, "cov");
  if (corrOpt.has_value() && covOpt.has_value())
    return rsSyntax("drawnorm: corr() and cov() cannot both be specified");
  if (covOpt.has_value() && (optStr(parsed.opts, "sds").has_value()))
    return rsSyntax("drawnorm: cov() already carries the scale; sds() cannot be combined with it");

  // Sigma: cov() outright, or diag(sds) * corr * diag(sds). Default corr = I.
  std::vector<std::vector<double>> sigma(k, std::vector<double>(k, 0.0));
  auto squareFrom = [&](const std::string& optName, const std::string& spec,
                        std::vector<std::vector<double>>* outM, ojson* err) -> bool {
    if (const eval::EvalMatrix* m = session.eval.findMatrix(spec)) {
      if (m->data.size() != k)
        return (*err = rsSyntax("drawnorm: matrix " + spec + " is " +
                                std::to_string(m->data.size()) + " rows for " + std::to_string(k) +
                                " variable(s)"),
                false);
      for (size_t i = 0; i < k; ++i) {
        if (m->data[i].size() != k)
          return (*err = rsSyntax("drawnorm: matrix " + spec + " is not square"), false);
        (*outM)[i] = m->data[i];
      }
      return true;
    }
    // A flat numlist: either k*k values (full matrix, row-major) or the lower
    // triangle Stata's own `corr(1 \ .5 1)` spelling writes.
    std::vector<std::string> toks;
    for (const auto& t : splitWs(spec))
      if (t != "\\") toks.push_back(t);
    std::vector<double> vals;
    for (const auto& t : toks) {
      double v = 0.0;
      if (!parseStrictDouble(t, &v)) {
        *err = rsSyntax("drawnorm: " + optName + "(" + spec +
                        ") is neither a matrix in memory nor a list of numbers");
        return false;
      }
      vals.push_back(v);
    }
    if (vals.size() == k * k) {
      for (size_t i = 0; i < k; ++i)
        for (size_t j = 0; j < k; ++j) (*outM)[i][j] = vals[i * k + j];
      return true;
    }
    if (vals.size() == k * (k + 1) / 2) {
      size_t p = 0;
      for (size_t i = 0; i < k; ++i)
        for (size_t j = 0; j <= i; ++j) {
          (*outM)[i][j] = vals[p];
          (*outM)[j][i] = vals[p];
          ++p;
        }
      return true;
    }
    *err = rsSyntax("drawnorm: " + optName + "() has " + std::to_string(vals.size()) +
                    " element(s); " + std::to_string(k * k) + " (full) or " +
                    std::to_string(k * (k + 1) / 2) + " (lower triangle) expected");
    return false;
  };

  if (covOpt.has_value()) {
    ojson err;
    if (!squareFrom("cov", *covOpt, &sigma, &err)) return err;
  } else {
    std::vector<std::vector<double>> corr(k, std::vector<double>(k, 0.0));
    for (size_t i = 0; i < k; ++i) corr[i][i] = 1.0;
    if (corrOpt.has_value()) {
      ojson err;
      if (!squareFrom("corr", *corrOpt, &corr, &err)) return err;
    }
    for (size_t i = 0; i < k; ++i)
      for (size_t j = 0; j < k; ++j) sigma[i][j] = sds[i] * corr[i][j] * sds[j];
  }

  std::vector<std::vector<double>> L = sigma;
  if (!cholesky(L))
    return rsError("drawnorm: correlation matrix is not positive definite");

  const uint64_t seed = drawSeed(session, optStr(parsed.opts, "seed"));

  int64_t n = 0;
  Datastore* ds = nullptr;
  if (session.hasData()) {
    try {
      ds = session.datastore();
    } catch (const std::exception& e) {
      return rsError(std::string("drawnorm: ") + e.what(), "data_io");
    }
  }
  if (ds != nullptr) {
    n = ds->nRows();
    if (auto v = optStr(parsed.opts, "n")) {
      long long want = 0;
      if (!parseStrictInt(*v, &want))
        return rsSyntax("drawnorm: n() requires a whole number, got `" + *v + "'");
      if (want != n)
        return rsError("drawnorm: n(" + *v + ") disagrees with the " + std::to_string(n) +
                       " observation(s) already in memory; drop the data first (`clear`) or omit "
                       "n().");
    }
    for (const auto& nm : parsed.varlist) {
      ojson guard = newVarGuard(session, "drawnorm", nm);
      if (!guard.is_null()) return guard;
      // BUG-1120: `drawnorm _all' is "_all not allowed" r(101) (StataBE 19.5).
      if (nm == "_skip" || nm == "_all") return rsErrorRc(nm + " not allowed", 101);
    }
  } else {
    auto v = optStr(parsed.opts, "n");
    if (!v.has_value())
      return rsSyntax("drawnorm: with no data in memory, n(#) is required");
    long long want = 0;
    if (!parseStrictInt(*v, &want) || want <= 0)
      return rsSyntax("drawnorm: n() requires a positive whole number, got `" + *v + "'");
    n = want;
  }

  // x = mu + L z, with z_i drawn from the (seed, variable, row) counter. Two
  // draws of the same command give the same numbers, and the value of variable
  // i at row j never depends on how many draws came before it.
  std::vector<std::vector<double>> draws(static_cast<size_t>(n), std::vector<double>(k, 0.0));
  for (int64_t r = 0; r < n; ++r) {
    std::vector<double> z(k);
    for (size_t i = 0; i < k; ++i)
      z[i] = cecon::normal_icdf(
          cecon::uniform01(seed, static_cast<uint64_t>(i), static_cast<uint64_t>(r)));
    for (size_t i = 0; i < k; ++i) {
      double acc = means[i];
      for (size_t j = 0; j <= i; ++j) acc += L[i][j] * z[j];
      draws[static_cast<size_t>(r)][i] = acc;
    }
  }

  if (ds == nullptr) {
    CsvTable t;
    for (const auto& nm : parsed.varlist) {
      t.names.push_back(nm);
      t.isFactor.push_back(0);
    }
    t.rows.reserve(static_cast<size_t>(n));
    for (int64_t r = 0; r < n; ++r) {
      std::vector<std::string> row;
      row.reserve(k);
      for (size_t i = 0; i < k; ++i) row.push_back(numCell(draws[static_cast<size_t>(r)][i]));
      t.rows.push_back(std::move(row));
    }
    std::string outPath;
    if (auto err = writeAndConvert(session, t, "drawnorm", &outPath)) return *err;
    session.load(outPath);
    if (auto bad = assertRowCount(session, "drawnorm", n)) return *bad;
    return mutationResult(session, "drawnorm", parsed.varlist);
  }

  std::string cur = *session.currentHbin();
  for (size_t i = 0; i < k; ++i) {
    std::vector<std::string> cells;
    cells.reserve(static_cast<size_t>(n));
    for (int64_t r = 0; r < n; ++r) cells.push_back(numCell(draws[static_cast<size_t>(r)][i]));
    std::string out;
    if (auto err = appendColumn(session, cur, parsed.varlist[i], cells, "f64", &out)) return *err;
    cur = out;
  }
  session.load(cur);
  return mutationResult(session, "drawnorm", parsed.varlist);
}

// ===========================================================================
// input  (the block fold + the handler)
// ===========================================================================

namespace {

// The byte that joins an `input` block's rows into one line. 0x1F (US) does not
// appear in Stata source, which is exactly why `program`'s own folding could
// not use a printable sentinel either.
constexpr char kInputSep = '\x1F';
// Appended to a folded block that ran off the end of the file without `end`.
constexpr char kInputUnterminated = '\x1E';

const std::regex& reInputHead() {
  static const std::regex re(R"(^input\b(.*)$)", std::regex::icase);
  return re;
}

const std::regex& reInputEnd() {
  static const std::regex re(R"(^end\s*$)", std::regex::icase);
  return re;
}

// A `program [define] NAME` header. captureInputBlocks runs BEFORE
// program::captureProgams (it must -- see doexec.cpp), so it is the one that
// would otherwise consume a program body's `end` if that body contained a line
// starting with `input`. This is deliberately a LOOSE match rather than a copy
// of program.cpp's own header regex: its only job is "do not fold inside here",
// and for that, over-matching is safe (an `input` block is never nested in
// something this matches by accident) while under-matching is not.
const std::regex& reProgramHeadForSkip() {
  static const std::regex re(R"(^program\b(?!\s+(?:drop|dir|list)\b))", std::regex::icase);
  return re;
}

// One free-format Stata data token: a quoted string, or a run of non-space.
// Returns false at end of input.
bool nextDataToken(const std::string& s, size_t* p, std::string* tok, bool* wasQuoted) {
  while (*p < s.size() && std::isspace(static_cast<unsigned char>(s[*p]))) ++(*p);
  if (*p >= s.size()) return false;
  *wasQuoted = false;
  if (s[*p] == '"') {
    *wasQuoted = true;
    ++(*p);
    std::string out;
    while (*p < s.size() && s[*p] != '"') out += s[(*p)++];
    if (*p < s.size()) ++(*p);  // closing quote
    *tok = out;
    return true;
  }
  std::string out;
  while (*p < s.size() && !std::isspace(static_cast<unsigned char>(s[*p]))) out += s[(*p)++];
  *tok = out;
  return true;
}

bool isStorageTypeWord(const std::string& w, bool* isStr) {
  const std::string t = strutil::lower(w);
  if (t == "byte" || t == "int" || t == "long" || t == "float" || t == "double") {
    *isStr = false;
    return true;
  }
  static const std::regex reStr(R"(^str([0-9]+)$)");
  if (std::regex_match(t, reStr)) {
    *isStr = true;
    return true;
  }
  return false;
}

}  // namespace

// Fold `input ... end` into ONE line, exactly the way `program ... end` is
// folded a line earlier in doexec.cpp. `input`'s body is DATA, not statements:
// letting the execution loop see its rows would dispatch each one as a command,
// and the tokenizer would split them on their own commas.
//
// The line-number offset this introduces is the same class `program` blocks
// already have, and is documented rather than corrected: a later error in the
// same do-file reports the line of the FOLDED block, not of the row inside it.
//
// KNOWN LIMIT, same as `program`'s own capture pass: only a line that BEGINS
// with `input` is folded, so `capture noisily input x y` ... `end` is not a
// block this pass recognises and its rows would reach the dispatcher as
// commands. Matching prefixed forms belongs in one place for both constructs,
// not separately here.
std::vector<std::string> captureInputBlocks(const std::vector<std::string>& lines) {
  std::vector<std::string> out;
  size_t i = 0;
  const size_t n = lines.size();
  while (i < n) {
    const std::string stripped = strutil::trim(lines[i]);
    // Pass a `program ... end` region through verbatim. Without this the two
    // folds race for the same terminator: whichever runs first wins, and the
    // other one silently swallows the rest of the file.
    if (std::regex_search(stripped, reProgramHeadForSkip())) {
      out.push_back(lines[i]);
      ++i;
      while (i < n) {
        const std::string pb = strutil::trim(lines[i]);
        out.push_back(lines[i]);
        ++i;
        if (std::regex_match(pb, reInputEnd())) break;  // same `end` shape
      }
      continue;
    }
    std::smatch m;
    if (!std::regex_search(stripped, m, reInputHead())) {
      out.push_back(lines[i]);
      ++i;
      continue;
    }
    // `input` with the varlist (and any option tail) on the head line.
    std::string folded = "input" + m.str(1);
    ++i;
    bool terminated = false;
    while (i < n) {
      const std::string body = strutil::trim(lines[i]);
      ++i;
      if (std::regex_match(body, reInputEnd())) {
        terminated = true;
        break;
      }
      if (body.empty()) continue;  // blank lines inside a data block carry nothing
      folded += kInputSep;
      folded += body;
    }
    if (!terminated) folded += kInputUnterminated;
    out.push_back(folded);
  }
  return out;
}

ojson runInput(const std::string& rawCmd, SessionState& session) {
  const size_t sep = rawCmd.find(kInputSep);
  if (sep == std::string::npos) {
    // A bare `input` typed with no block: the fold never fired, so there is no
    // data to read. Say that, rather than succeeding and doing nothing.
    return rsSyntax("input: requires a do-file block terminated by `end` — interactive input is "
                    "not available in this build.");
  }
  std::string body = rawCmd;
  bool unterminated = false;
  if (!body.empty() && body.back() == kInputUnterminated) {
    unterminated = true;
    body.pop_back();
  }

  // Split the fold back into the head line and the data rows.
  std::vector<std::string> parts;
  {
    std::string cur;
    for (char c : body) {
      if (c == kInputSep) {
        parts.push_back(cur);
        cur.clear();
      } else {
        cur += c;
      }
    }
    parts.push_back(cur);
  }
  if (unterminated) return rsSyntax("input: block not terminated by `end`");

  std::string head = parts[0];
  {
    static const std::regex reHead(R"(^\s*input\b\s*(.*)$)", std::regex::icase);
    std::smatch m;
    if (std::regex_search(head, m, reHead)) head = m.str(1);
  }
  // The option tail, if any. `input` reads none of them, so the whole tail goes
  // through the reject net -- `automatic`/`label` come back as "valid Stata
  // option, not yet implemented" rather than as unknown keys.
  std::string varlistText = head;
  {
    const long long comma = eval::findUnquotedComma(head);
    if (comma >= 0) {
      varlistText = head.substr(0, static_cast<size_t>(comma));
      std::vector<std::pair<std::string, eval::OptValue>> opts =
          eval::parseOpts(head.substr(static_cast<size_t>(comma) + 1));
      eval::canonicalizeForCommand(opts, "input");
      if (auto err = eval::rejectUnknownOptions(opts, kVocabInput.known, kVocabInput.unimplemented))
        return ojson(*err);
    }
  }

  // varlist, with `str#`/`byte`/... type words binding to the NEXT name only
  // (`input double y x1 x2` types y alone: w2e_float_default.log:2149
  // input_double_types `double|float`).
  std::vector<std::string> names;
  std::vector<char> declaredStr;
  std::vector<std::string> declaredNumType;  // "" = untyped (c(type) applies)
  std::vector<std::string> declaredStrType;  // the `str#` word as declared, "" otherwise
  {
    bool pendingStr = false;
    bool pendingType = false;
    std::string pendingWord;
    for (const auto& w : splitWs(varlistText)) {
      bool isStr = false;
      if (isStorageTypeWord(w, &isStr)) {
        pendingType = true;
        pendingStr = isStr;
        pendingWord = strutil::lower(w);
        continue;
      }
      if (!strutil::isValidStataIdentifier(w))
        return rsSyntax("input: invalid variable name `" + w + "' — " +
                        strutil::stataIdentifierRuleText());
      names.push_back(w);
      declaredStr.push_back(pendingType && pendingStr ? 1 : 0);
      declaredNumType.push_back(pendingType && !pendingStr ? pendingWord : std::string());
      declaredStrType.push_back(pendingType && pendingStr ? pendingWord : std::string());
      pendingType = false;
      pendingStr = false;
      pendingWord.clear();
    }
    if (pendingType)
      return rsSyntax("input: a storage type must be followed by a variable name");
  }

  Datastore* ds = nullptr;
  if (session.hasData()) {
    try {
      ds = session.datastore();
    } catch (const std::exception& e) {
      return rsError(std::string("input: ") + e.what(), "data_io");
    }
  }

  if (ds != nullptr) {
    // With data in memory this build APPENDS to the existing variables, in the
    // existing order. A different varlist is refused rather than silently
    // creating a second, differently shaped dataset.
    if (!names.empty() && names != ds->names())
      return rsError("input: with data in memory this build appends to the existing variables "
                     "only (`" + joinWith(ds->names(), " ") + "`).");
    names = ds->names();
    declaredStr.assign(names.size(), 0);
    declaredNumType.assign(names.size(), std::string());
    for (size_t c = 0; c < names.size(); ++c)
      declaredStr[c] = (ds->dtype(names[c]) == "factor") ? 1 : 0;
  } else if (names.empty()) {
    return rsSyntax("input: with no data in memory a varlist is required (`input x y`)");
  }

  const size_t k = names.size();
  if (k == 0) return rsSyntax("input: no variables to read into");

  // Free format: the rows are one TOKEN STREAM that wraps every k values, which
  // is what Stata's own reader does -- so a row split across two source lines
  // reads the same as a row written on one.
  std::vector<std::string> tokens;
  std::vector<char> quoted;
  for (size_t r = 1; r < parts.size(); ++r) {
    size_t p = 0;
    std::string tok;
    bool wasQuoted = false;
    while (nextDataToken(parts[r], &p, &tok, &wasQuoted)) {
      tokens.push_back(tok);
      quoted.push_back(wasQuoted ? 1 : 0);
    }
  }
  if (tokens.size() % k != 0)
    return rsSyntax("input: the data ended in the middle of an observation (" +
                    std::to_string(tokens.size()) + " value(s) for " + std::to_string(k) +
                    " variable(s))");
  const size_t nNew = tokens.size() / k;

  // FU-944: the storage type each numeric column takes, and so the value it
  // holds. Untyped is c(type) (float by default: w2e_float_default.log:2120
  // input_default_types `+1.19999a0000000X+000|float|float` for 1.1;
  // w2g_ebmat.log:141 e15_data_sum is the float data's sum), a declared type is
  // its own, and rows appended to data in memory take the existing column's
  // type. A float column stores float(v); every other type keeps the value read
  // (an integer type's non-integer input is not captured; nothing is invented).
  std::vector<std::string> storeType(k);
  for (size_t c = 0; c < k; ++c) {
    if (declaredStr[c]) continue;
    storeType[c] = ds != nullptr ? storage::resolveReplaceType(session, names[c])
                                 : storage::resolveCreateType(session, declaredNumType[c]);
  }

  CsvTable t;
  for (size_t c = 0; c < k; ++c) {
    t.names.push_back(names[c]);
    t.isFactor.push_back(declaredStr[c]);
  }
  t.rows.reserve(nNew);
  for (size_t r = 0; r < nNew; ++r) {
    std::vector<std::string> row(k);
    for (size_t c = 0; c < k; ++c) {
      const std::string& tok = tokens[r * k + c];
      const bool wasQuoted = quoted[r * k + c] != 0;
      if (!wasQuoted && (tok == "." || tok.empty())) continue;  // missing stays missing
      if (!t.isFactor[c]) {
        double v = 0.0;
        if (!parseStrictDouble(tok, &v))
          return rsSyntax("input: `" + tok + "' is not a number, and " + names[c] +
                          " is a numeric variable (declare it `str#` to read text)");
        if (storeType[c] == "float") v = static_cast<double>(static_cast<float>(v));
        row[c] = numCell(v);
        continue;
      }
      row[c] = tok;
    }
    t.rows.push_back(std::move(row));
  }

  std::string newPath;
  if (auto err = writeAndConvert(session, t, "input", &newPath)) return *err;

  if (ds == nullptr) {
    session.load(newPath);
    if (auto bad = assertRowCount(session, "input", static_cast<int64_t>(nNew))) return *bad;
    // The types `: type`/describe and the .dta writer read, the way gen records
    // them (dispatcher.cpp, BUG-295): an untyped float column on the implicit
    // plane, a declaration (or a non-float c(type)) on varMeta.
    for (size_t c = 0; c < k; ++c) {
      // A declared `str#` is the column's storage type (`: type` answers it:
      // w2ar dx_inp_079_str `input str10 date`, str10 %10s, log:4625).
      if (declaredStr[c] && c < declaredStrType.size() && !declaredStrType[c].empty()) {
        session.varMeta[names[c]].stataType = declaredStrType[c];
        continue;
      }
      if (declaredStr[c] || storeType[c].empty()) continue;
      if (declaredNumType[c].empty() && storeType[c] == "float") {
        session.varMeta[names[c]].stataType.reset();
        session.implicitFloatVars.insert(names[c]);
      } else {
        session.implicitFloatVars.erase(names[c]);
        session.varMeta[names[c]].stataType = storeType[c];
      }
    }
    // NO newVars list here: this is a whole new dataset, not columns ADDED to
    // an existing one, and `newVars` is what steers seedNMissForPureAdd's
    // carry-forward — feeding it every column of a frame that has no
    // predecessor asks it to carry counts from a frame that does not exist.
    return mutationResult(session, "input");
  }
  const int64_t before = ds->nRows();
  const std::string out = nextPath(session.workdir(), "input_append");
  if (auto err = runVerb({"append", "--data", *session.currentHbin(), "--using", newPath, "--out",
                          out}))
    return *err;
  session.load(out);
  if (auto bad = assertRowCount(session, "input", before + static_cast<int64_t>(nNew))) return *bad;
  return mutationResult(session, "input");
}

// ===========================================================================
// notes
// ===========================================================================
//
// Straight onto `session.characteristics`, in Stata's own storage shape:
// `_dta[note0]` is the count and `_dta[note1]`, `_dta[note2]`, ... the texts;
// a variable's notes live under its own name. That shape is what makes
// persistence FREE -- `save f.dta` carries the char table verbatim through the
// export-dta sidecar and `use` restores it, with no code in this file.

namespace {

std::string charGet(SessionState& session, const std::string& var, const std::string& name) {
  for (const auto& c : session.characteristics)
    if (c.varname == var && c.charname == name) return c.value;
  return std::string();
}

void charSet(SessionState& session, const std::string& var, const std::string& name,
             const std::string& value) {
  for (auto& c : session.characteristics) {
    if (c.varname == var && c.charname == name) {
      c.value = value;
      return;
    }
  }
  session.characteristics.push_back(DtaCharacteristic{var, name, value});
}

int noteCount(SessionState& session, const std::string& var) {
  const std::string s = charGet(session, var, "note0");
  long long v = 0;
  if (s.empty() || !parseStrictInt(s, &v) || v < 0) return 0;
  return static_cast<int>(v);
}

// [D] notes: the word TS in a note's text is replaced by the date and time
// (`notes: ..., TS` -> `..., 21 Oct 2026 14:05`; w2ao_label_family lb_notes_ts).
std::string noteTimestamp(const std::string& text) {
  static const std::regex reTs(R"((^|\s)TS(?=\s|$))");
  if (!std::regex_search(text, reTs)) return text;
  const std::time_t now = std::time(nullptr);
  std::tm tmv{};
#if defined(_WIN32)
  localtime_s(&tmv, &now);
#else
  localtime_r(&now, &tmv);
#endif
  char buf[64];
  std::strftime(buf, sizeof(buf), "%e %b %Y %H:%M", &tmv);
  std::string out;
  std::string rest = text;
  std::smatch m;
  while (std::regex_search(rest, m, reTs)) {
    out += m.prefix().str() + m.str(1) + buf;
    rest = m.suffix().str();
  }
  return out + rest;
}

void noteAdd(SessionState& session, const std::string& var, const std::string& textIn) {
  const std::string text = noteTimestamp(textIn);
  const int n = noteCount(session, var) + 1;
  charSet(session, var, "note" + std::to_string(n), text);
  charSet(session, var, "note0", std::to_string(n));
}

void noteDropAll(SessionState& session, const std::string& var) {
  const int n = noteCount(session, var);
  auto& v = session.characteristics;
  v.erase(std::remove_if(v.begin(), v.end(),
                         [&](const DtaCharacteristic& c) {
                           if (c.varname != var) return false;
                           if (c.charname == "note0") return true;
                           for (int i = 1; i <= n; ++i)
                             if (c.charname == "note" + std::to_string(i)) return true;
                           return false;
                         }),
          v.end());
}

std::string renderNotes(SessionState& session, const std::vector<std::string>& targets) {
  std::ostringstream oss;
  bool first = true;
  for (const auto& tgt : targets) {
    const int n = noteCount(session, tgt);
    if (n == 0) continue;
    if (!first) oss << "\n";
    first = false;
    oss << tgt << ":\n";
    for (int i = 1; i <= n; ++i) {
      oss << "  " << i << ". " << charGet(session, tgt, "note" + std::to_string(i)) << "\n";
    }
  }
  return oss.str();
}

}  // namespace

ojson runNotes(const eval::ParsedCommand& parsed, SessionState& session) {
  // NOT `parsed.opts`: everything after a `notes:` colon is note TEXT, and the
  // tokenizer's comma rule would carve an option list out of the user's own
  // sentence. The raw line is the only faithful source here.
  static const std::regex reHead(R"(^\s*(?:notes?)\s*(:)?\s*([\s\S]*)$)", std::regex::icase);
  std::smatch m;
  if (!std::regex_search(parsed.raw, m, reHead))
    return rsSyntax("notes: syntax is `notes[ varname]: text`, `notes list [varlist]` or "
                    "`notes drop _dta|varname`");
  const bool colonOnHead = m[1].matched;
  std::string rest = trimOf(m.str(2));

  if (colonOnHead) {
    if (rest.empty()) return rsSyntax("notes: `notes:` needs some text after the colon");
    noteAdd(session, "_dta", rest);
    return silent();
  }

  auto allTargets = [&]() {
    std::vector<std::string> targets{"_dta"};
    Datastore* ds = nullptr;
    try {
      ds = session.datastore();
    } catch (const std::exception&) {
      ds = nullptr;
    }
    if (ds != nullptr)
      for (const auto& nm : ds->names()) targets.push_back(nm);
    return targets;
  };

  if (rest.empty()) {
    const std::string text = renderNotes(session, allTargets());
    // Nothing recorded prints nothing, not a blank line.
    if (text.empty()) return silent();
    return displayBlock(text);
  }

  const std::vector<std::string> words = splitWs(rest);
  const std::string verb = lowerOf(words[0]);

  if (verb == "list") {
    std::vector<std::string> targets;
    if (words.size() == 1) {
      targets = allTargets();
    } else {
      for (size_t i = 1; i < words.size(); ++i) targets.push_back(words[i]);
    }
    const std::string text = renderNotes(session, targets);
    if (text.empty()) return silent();
    return displayBlock(text);
  }

  if (verb == "drop") {
    if (words.size() < 2)
      return rsSyntax("notes drop: name what to drop from (`notes drop _dta` or `notes drop "
                      "varname`)");
    // `notes drop varname in 3` drops ONE numbered note; this build drops all
    // of a target's notes or none, so the numbered form is refused by name
    // rather than quietly widened into "drop everything".
    for (size_t i = 1; i < words.size(); ++i)
      if (lowerOf(words[i]) == "in")
        return ojson(eval::unsupportedOption(
            "in", "`notes drop ... in #` (dropping one numbered note) is not implemented; this "
                  "build drops every note on a target."));
    for (size_t i = 1; i < words.size(); ++i) noteDropAll(session, words[i]);
    return silent();
  }

  if (verb == "replace" || verb == "search" || verb == "renumber") {
    return ojson(eval::unsupportedCommand("notes " + verb, std::nullopt, std::nullopt,
                                          "notes_" + verb,
                                          eval::UnsupportedCommandKind::kUnknownSubcommand));
  }

  // `notes varname: text` — the colon may be glued to the name or stand alone.
  const size_t colon = rest.find(':');
  if (colon == std::string::npos)
    return rsSyntax("notes: syntax is `notes[ varname]: text`, `notes list [varlist]` or "
                    "`notes drop _dta|varname`");
  const std::string target = trimOf(rest.substr(0, colon));
  const std::string text = trimOf(rest.substr(colon + 1));
  if (target.empty()) {
    if (text.empty()) return rsSyntax("notes: `notes:` needs some text after the colon");
    noteAdd(session, "_dta", text);
    return silent();
  }
  if (splitWs(target).size() != 1)
    return rsSyntax("notes: `" + target + "` — one variable name (or `_dta`) before the colon");
  // An empty text after `varname:` is rc 0 and adds nothing (w2ao_label_family
  // lb_note_empty `note h_seq:` rc 0, note count unchanged).
  if (text.empty()) return silent();
  if (target != "_dta") {
    Datastore* ds = nullptr;
    try {
      ds = session.datastore();
    } catch (const std::exception&) {
      ds = nullptr;
    }
    if (ds == nullptr) return ojson(eval::noDataLoaded());
    if (!ds->has(target)) return rsError("variable " + target + " not found");
  }
  noteAdd(session, target, text);
  return silent();
}

// ===========================================================================
// compare
// ===========================================================================

ojson runCompare(const eval::ParsedCommand& parsed, SessionState& session) {
  if (auto err = eval::rejectUnknownOptions(parsed.opts, kVocabCompare.known,
                                            kVocabCompare.unimplemented))
    return ojson(*err);
  if (auto q = refuseQualifiers(parsed, "compare")) return *q;
  if (!session.hasData()) return ojson(eval::noDataLoaded());
  if (parsed.varlist.size() != 2)
    return rsSyntax("compare: syntax is `compare var1 var2`");
  const std::string& v1 = parsed.varlist[0];
  const std::string& v2 = parsed.varlist[1];

  ReadFrame rf(session, {v1, v2});
  Datastore* ds = rf.get();
  if (ds == nullptr)
    return rsError("compare: could not read the data" + (rf.why().empty() ? "" : " — " + rf.why()),
                   "data_io");
  for (const auto& v : {v1, v2}) {
    if (!ds->has(v)) return rsError("variable " + v + " not found");
    if (ds->dtype(v) == "factor")
      return rsError("compare: `" + v +
                     "' is a string variable; compare works on numeric variables.");
  }
  const std::vector<double> a = columnAsDoubles(*ds, v1);
  const std::vector<double> b = columnAsDoubles(*ds, v2);
  const size_t n = a.size();

  struct Bucket {
    int64_t count = 0;
    double lo = std::numeric_limits<double>::quiet_NaN();
    double hi = std::numeric_limits<double>::quiet_NaN();
    double sum = 0.0;
    void add(double d) {
      if (count == 0 || d < lo) lo = d;
      if (count == 0 || d > hi) hi = d;
      sum += d;
      ++count;
    }
    void bump() { ++count; }
    double avg() const {
      return count ? sum / static_cast<double>(count) : std::numeric_limits<double>::quiet_NaN();
    }
  };
  Bucket lt, eq, gt, joint;
  int64_t missA = 0, missB = 0, missBoth = 0;
  for (size_t i = 0; i < n; ++i) {
    const bool ma = std::isnan(a[i]);
    const bool mb = std::isnan(b[i]);
    if (ma && mb) {
      ++missBoth;
      continue;
    }
    // "var1 missing only" means var1 is missing and var2 is not — the counter
    // is named for the variable that IS missing, not for the one that survives.
    if (ma) {
      ++missA;
      continue;
    }
    if (mb) {
      ++missB;
      continue;
    }
    const double d = a[i] - b[i];
    joint.add(d);
    if (d < 0)
      lt.add(d);
    else if (d > 0)
      gt.add(d);
    else
      eq.bump();
  }

  auto num = [](double v) {
    if (std::isnan(v)) return std::string(".");
    return piet::fmtStataDefaultG(v);
  };
  auto line = [&](const std::string& label, const Bucket& bk, bool withStats) {
    char buf[256];
    if (!withStats || bk.count == 0) {
      std::snprintf(buf, sizeof(buf), "%-22s %10lld\n", label.c_str(),
                    static_cast<long long>(bk.count));
      return std::string(buf);
    }
    std::snprintf(buf, sizeof(buf), "%-22s %10lld  %12s %12s %12s\n", label.c_str(),
                  static_cast<long long>(bk.count), num(bk.lo).c_str(), num(bk.avg()).c_str(),
                  num(bk.hi).c_str());
    return std::string(buf);
  };

  std::ostringstream oss;
  oss << "                                   ------- difference -------\n";
  oss << "                            count       minimum      average      maximum\n";
  oss << "--------------------------------------------------------------------------\n";
  oss << line(v1 + "<" + v2, lt, true);
  oss << line(v1 + "=" + v2, eq, false);
  oss << line(v1 + ">" + v2, gt, true);
  oss << "                       ----------\n";
  oss << line("   jointly defined", joint, true);
  if (missA > 0) {
    Bucket b1;
    b1.count = missA;
    oss << line("   " + v1 + " missing only", b1, false);
  }
  if (missB > 0) {
    Bucket b2;
    b2.count = missB;
    oss << line("   " + v2 + " missing only", b2, false);
  }
  if (missBoth > 0) {
    Bucket b3;
    b3.count = missBoth;
    oss << line("   jointly missing", b3, false);
  }
  oss << "                       ----------\n";
  {
    Bucket total;
    total.count = static_cast<int64_t>(n);
    oss << line("   total", total, false);
  }
  return displayBlock(oss.str());
}

// ===========================================================================
// cumul
// ===========================================================================

namespace {

// One `cumul` call, once its syntax has been accepted.
struct CumulRequest {
  std::string source;
  std::string target;
  std::vector<std::string> groupVars;
  bool frequencyUnits = false;
  bool tiesShareValue = false;
};

size_t optionCount(const std::vector<std::pair<std::string, eval::OptValue>>& opts,
                   std::initializer_list<const char*> spellings) {
  size_t count = 0;
  for (const auto& [key, value] : opts)
    for (const char* spelling : spellings)
      if (key == spelling) ++count;
  return count;
}

// The syntax checks, in the order Stata answers them when several faults meet
// (per the black-box oracle in the item's folder). `by()` comes last, so the
// checks that need the data can run in the same pass.
std::optional<ojson> readCumulRequest(const eval::ParsedCommand& parsed, SessionState& session,
                                      CumulRequest* request) {
  const Datastore* data = session.datastore();
  if (data == nullptr) return ojson(eval::noDataLoaded());

  std::vector<std::string> sourceWords = parsed.varlist;
  if (sourceWords.empty()) return rsErrorRc("varlist required", 100);
  if (auto ambiguous = resolveByKeyNames(session, sourceWords)) return *ambiguous;
  if (sourceWords.size() > 1) return rsErrorRc("too many variables specified", 103);
  request->source = sourceWords.front();
  if (!data->has(request->source))
    return rsErrorRc("variable " + request->source + " not found", 111);

  const auto newNameWords = splitWs(optStrAny(parsed.opts, {"generate", "gen"}).value_or(""));
  if (newNameWords.empty()) return rsErrorRc("option generate() required", 198);
  if (auto unknown = eval::rejectUnknownOptions(parsed.opts, kVocabCumul.known,
                                                kVocabCumul.unimplemented))
    return ojson(*unknown);
  if (optionCount(parsed.opts, {"generate", "gen"}) > 1)
    return rsErrorRc("option gen() not allowed", 198);
  if (optionCount(parsed.opts, {"by"}) > 1) return rsErrorRc("option by() not allowed", 198);
  if (newNameWords.size() > 1) return rsErrorRc(newNameWords[1] + " not allowed", 101);
  request->target = newNameWords.front();
  if (auto refused = strutil::newVarNameRefusal(request->target))
    return rsErrorRc(refused->message, refused->rc);
  if (!strutil::isValidStataIdentifier(request->target))
    return rsErrorRc(request->target + " invalid name", 198);
  if (data->has(request->target))
    return rsErrorRc("variable " + request->target + " already defined", 110);

  request->groupVars = splitWs(optStr(parsed.opts, "by").value_or(""));
  if (auto ambiguous = resolveByKeyNames(session, request->groupVars)) return *ambiguous;
  for (const std::string& name : request->groupVars)
    if (!data->has(name))
      return rsErrorRc("variable " + name + " not found\n(error in option by())", 111);

  request->frequencyUnits = hasFlag(parsed.opts, "freq");
  request->tiesShareValue = hasFlag(parsed.opts, "equal");
  return std::nullopt;
}

// One 64-bit word per observation, equal exactly when two observations belong
// together under this by() variable. Each missing code and the empty string are
// groups of their own; 0 and -0 are one value.
std::vector<uint64_t> groupingWords(const Datastore& data, const std::string& name) {
  std::vector<uint64_t> words(static_cast<size_t>(data.nRows()));
  if (data.col(name).dtype == cecon::DType::Factor) {
    std::map<std::string, uint64_t> seen;
    const auto cells = columnAsStrings(data, name);
    for (size_t row = 0; row < cells.size(); ++row)
      words[row] = seen.emplace(cells[row], seen.size()).first->second;
    return words;
  }
  const auto cells = columnAsDoubles(data, name);
  for (size_t row = 0; row < cells.size(); ++row) {
    double cell = cells[row] == 0.0 ? 0.0 : cells[row];
    if (std::isnan(cell)) cell = cecon::stata_missing(cecon::missing_kind(cell));
    std::memcpy(&words[row], &cell, sizeof cell);
  }
  return words;
}

// Fills `ecdf` for one by() group, given as sample rows in ascending value order.
// Both sums are held as a float stores them: the running total is exact double
// arithmetic rounded on its way into storage, and the share is formed from those
// stored numbers (the arithmetic measured on StataBE, the item's oracle section B;
// the textbook function is [R] cumul's empirical distribution).
void fillGroupEcdf(const std::vector<int64_t>& rows, size_t begin, size_t end,
                   const std::vector<double>& value, const std::vector<double>* weight,
                   const CumulRequest& request, std::vector<double>* ecdf) {
  std::vector<float> stored(end - begin);
  double running = 0.0;
  for (size_t k = begin; k < end; ++k) {
    running += weight != nullptr ? (*weight)[static_cast<size_t>(rows[k])] : 1.0;
    stored[k - begin] = static_cast<float>(running);
  }
  const double groupTotal = stored.back();
  for (size_t k = begin; k < end;) {
    size_t tieEnd = k + 1;
    if (request.tiesShareValue)
      while (tieEnd < end && value[static_cast<size_t>(rows[tieEnd])] == value[static_cast<size_t>(rows[k])])
        ++tieEnd;
    const float reached = stored[tieEnd - 1 - begin];
    const double result =
        request.frequencyUnits ? reached : static_cast<float>(reached / groupTotal);
    for (; k < tieEnd; ++k) (*ecdf)[static_cast<size_t>(rows[k])] = result;
  }
}

}  // namespace

// `cumul`, [R] cumul: the empirical distribution function of a variable, as a new
// float variable, within by() groups.
ojson runCumul(const eval::ParsedCommand& parsed, SessionState& session) {
  if (!session.hasData()) return ojson(eval::noDataLoaded());
  CumulRequest request;
  if (auto refusal = readCumulRequest(parsed, session, &request)) return *refusal;

  const bool haveIf = parsed.ifExpr.has_value() && !trimOf(*parsed.ifExpr).empty();
  const bool haveIn = parsed.inRange.has_value() && !trimOf(*parsed.inRange).empty();
  const bool inWithBy = haveIn && !request.groupVars.empty();
  std::string qualifier;
  if (haveIf) qualifier += " if " + *parsed.ifExpr;
  if (haveIn && !inWithBy) qualifier += " in " + *parsed.inRange;

  ScratchColumns scratch(session);
  std::string restrictionColumn;
  if (!qualifier.empty())
    if (auto failed = scratch.add("1", qualifier, &restrictionColumn)) return *failed;
  if (inWithBy) return rsErrorRc("in may not be combined with by()", 190);

  std::string weightColumn;
  const bool weighted = parsed.weightVar.has_value() && !parsed.weightVar->empty();
  if (weighted)
    if (auto failed = scratch.add(*parsed.weightVar, "", &weightColumn)) return *failed;

  const auto rowCount = static_cast<size_t>(session.datastore()->nRows());
  std::vector<char> restricted(rowCount, 1);
  if (!restrictionColumn.empty()) {
    const auto marks = scratch.values(restrictionColumn);
    for (size_t row = 0; row < rowCount; ++row) restricted[row] = std::isnan(marks[row]) ? 0 : 1;
  }

  // A weight only has to be valid inside the if/in restriction.
  std::vector<double> weights;
  if (weighted) {
    weights = scratch.values(weightColumn);
    const std::string kind = parsed.weightType.value_or("");
    const bool frequencyWeight = kind == "fw" || kind == "fweight";
    bool negative = false, fractional = false;
    for (size_t row = 0; row < rowCount; ++row) {
      if (!restricted[row] || std::isnan(weights[row])) continue;
      negative = negative || weights[row] < 0.0;
      fractional = fractional || weights[row] != std::floor(weights[row]);
    }
    if (negative) return rsErrorRc("negative weights encountered", 402);
    if (frequencyWeight && fractional)
      return rsErrorRc("may not use noninteger frequency weights", 401);
  }

  const Datastore& data = *session.datastore();
  std::vector<double> sourceValues;  // stays empty for a string variable: no sample
  if (data.col(request.source).dtype != cecon::DType::Factor)
    sourceValues = columnAsDoubles(data, request.source);

  std::vector<int64_t> sample;
  for (size_t row = 0; row < sourceValues.size(); ++row) {
    if (!restricted[row] || std::isnan(sourceValues[row])) continue;
    if (weighted && (std::isnan(weights[row]) || weights[row] == 0.0)) continue;
    sample.push_back(static_cast<int64_t>(row));
  }

  std::vector<std::vector<uint64_t>> groupColumns;
  for (const std::string& name : request.groupVars) groupColumns.push_back(groupingWords(data, name));
  const auto sameGroup = [&](int64_t a, int64_t b) {
    return std::all_of(groupColumns.begin(), groupColumns.end(), [&](const std::vector<uint64_t>& g) {
      return g[static_cast<size_t>(a)] == g[static_cast<size_t>(b)];
    });
  };
  // Stable, so tied observations accumulate in dataset order and PIET is
  // deterministic where Stata's tie order is unspecified.
  std::stable_sort(sample.begin(), sample.end(), [&](int64_t a, int64_t b) {
    for (const auto& g : groupColumns) {
      const uint64_t wordA = g[static_cast<size_t>(a)], wordB = g[static_cast<size_t>(b)];
      if (wordA != wordB) return wordA < wordB;
    }
    return sourceValues[static_cast<size_t>(a)] < sourceValues[static_cast<size_t>(b)];
  });

  std::vector<double> ecdf(rowCount, std::numeric_limits<double>::quiet_NaN());
  for (size_t begin = 0; begin < sample.size();) {
    size_t end = begin + 1;
    while (end < sample.size() && sameGroup(sample[begin], sample[end])) ++end;
    fillGroupEcdf(sample, begin, end, sourceValues, weighted ? &weights : nullptr, request, &ecdf);
    begin = end;
  }

  scratch.dropAll();
  (void)session.datastore();  // the head file the setcol below reads
  std::vector<std::string> cells;
  cells.reserve(rowCount);
  for (double v : ecdf) cells.push_back(numCell(v));
  std::string written;
  if (auto failed = appendColumn(session, *session.currentHbin(), request.target, cells, "f64", &written))
    return *failed;
  session.load(written);
  session.setVarLabel(request.target, "ECDF of " + request.source);
  session.varMeta[request.target].stataType = "float";
  scratch.noteDataChanged();
  return mutationResult(session, "cumul", {request.target});
}

// ===========================================================================
// bsample
// ===========================================================================

bool bsampleIsStataFastPath(const eval::ParsedCommand& parsed) {
  if (!parsed.varlist.empty()) return false;  // `bsample #` is the SRSWR path
  for (const auto& [k, v] : parsed.opts)
    if (k == "cluster" || k == "idcluster" || k == "strata" || k == "weight") return false;
  return true;
}

ojson runBsample(const eval::ParsedCommand& parsed, SessionState& session) {
  if (auto err = eval::rejectUnknownOptions(parsed.opts, kVocabBsample.known,
                                            kVocabBsample.unimplemented))
    return ojson(*err);
  if (auto q = refuseQualifiers(parsed, "bsample")) return *q;
  if (auto w = refuseWeights(parsed, "bsample")) return *w;
  if (!session.hasData()) return ojson(eval::noDataLoaded());

  Datastore* ds = nullptr;
  try {
    ds = session.datastore();
  } catch (const std::exception& e) {
    return rsError(std::string("bsample: ") + e.what(), "data_io");
  }
  if (ds == nullptr) return ojson(eval::noDataLoaded());
  const int64_t n = ds->nRows();
  if (n == 0) return rsErrorRc("bsample: no observations", 2000);

  const std::optional<std::string> clusterOpt = optStr(parsed.opts, "cluster");
  const std::optional<std::string> idclusterOpt = optStr(parsed.opts, "idcluster");
  if (idclusterOpt.has_value() && !clusterOpt.has_value())
    return rsSyntax("bsample: idcluster() requires cluster()");

  if (ds->has(kRowIdx))
    return rsError("bsample: the scratch column " + std::string(kRowIdx) +
                   " already exists in this dataset; rename it and retry.");
  if (idclusterOpt.has_value()) {
    ojson guard = newVarGuard(session, "bsample", *idclusterOpt);
    if (!guard.is_null()) return guard;
  }

  // The GROUPS being drawn: either every row on its own, or the rows of each
  // distinct cluster value. A factor cluster groups on the code (and every
  // missing cluster value is ONE group, as Stata treats it).
  std::vector<std::vector<int64_t>> groups;  // 1-based row indices
  if (!clusterOpt.has_value()) {
    groups.reserve(static_cast<size_t>(n));
    for (int64_t i = 0; i < n; ++i) groups.push_back({i + 1});
  } else {
    const std::vector<std::string> cvars = splitWs(*clusterOpt);
    if (cvars.size() != 1)
      return rsError("bsample: this build clusters on ONE variable; cluster(" + *clusterOpt +
                         ") names " + std::to_string(cvars.size()) + ".",
                     "unimplemented");
    const std::string& cv = cvars[0];
    if (!ds->has(cv)) return rsError("variable " + cv + " not found");
    std::map<std::string, size_t> seen;
    const cecon::Column& c = ds->col(cv);
    for (int64_t i = 0; i < n; ++i) {
      std::string key;
      if (c.dtype == cecon::DType::Factor) {
        const int32_t code = c.codes()[i];
        key = code < 0 ? std::string(".") : ("c" + std::to_string(code));
      } else {
        const double v = c.f64()[i];
        key = std::isnan(v) ? std::string(".") : eval::pythonFloatRepr(v);
      }
      auto it = seen.find(key);
      if (it == seen.end()) {
        it = seen.emplace(key, groups.size()).first;
        groups.emplace_back();
      }
      groups[it->second].push_back(i + 1);
    }
  }

  const int64_t nGroups = static_cast<int64_t>(groups.size());
  int64_t draws = nGroups;  // default size = _N, or G when clustering
  if (!parsed.varlist.empty()) {
    long long want = 0;
    if (!parseStrictInt(parsed.varlist[0], &want) || want < 0)
      return rsSyntax("bsample: `" + parsed.varlist[0] + "' is not a non-negative whole number");
    draws = want;
    if (!clusterOpt.has_value() && draws > n)
      return rsError("bsample: cannot draw " + std::to_string(draws) +
                     " observation(s) without replacement from " + std::to_string(n) +
                     " — bsample draws WITH replacement, so # may not exceed _N in Stata either.");
    if (clusterOpt.has_value() && draws > nGroups)
      return rsError("bsample: cannot draw " + std::to_string(draws) + " cluster(s) from " +
                     std::to_string(nGroups));
  }

  std::vector<int64_t> idx;
  std::vector<int64_t> idcluster;
  int64_t nKept = -1;  // the fast path's originals: rows [0, nKept) of the result
  if (bsampleIsStataFastPath(parsed)) {
    // Stata's own draw on the session stream (core/rng_mt64.h), so `set seed s` +
    // bsample gives Stata's rows; shared with the bootstrap prefix (bsample_draws.h).
    for (int64_t row : bsdraw::srswrFast(session.seed.stream(), n, &nKept)) idx.push_back(row + 1);
  } else {
    const uint64_t seed = drawSeed(session, std::nullopt);
    for (int64_t d = 0; d < draws; ++d) {
      const uint32_t u = rowKey32(seed, d);
      const int64_t g = static_cast<int64_t>(u % static_cast<uint32_t>(nGroups));
      for (int64_t row : groups[static_cast<size_t>(g)]) {
        idx.push_back(row);
        idcluster.push_back(d + 1);
      }
    }
  }

  // Three steps, exactly the shape handlers_resample.cpp's bootstrap replicate
  // uses: stamp the base with a row index, convert the drawn indices to a
  // frame, m:1-join them back. `merge`'s default keeps using-only rows, which
  // would silently add every UNDRAWN observation to the resample, so the
  // using-only side is dropped explicitly.
  const std::string basePath = *session.currentHbin();
  const std::string baseIdx = nextPath(session.workdir(), "bsample_baseidx");
  if (auto err = runVerb({"eval", "--data", basePath, "--egen",
                          std::string(kRowIdx) + " = seq()", "--out", baseIdx}))
    return *err;
  // The steps below each mint scratch, which pushes the reclaim budget over and
  // makes these two -- the oldest at that moment -- the first eviction
  // candidates while they are still the thing being drawn from.
  SessionState::ScratchPin pin(session, {basePath, baseIdx});

  const std::string csv = nextPath(session.workdir(), "bsample_idx") + ".csv";
  registerScratchMint(csv);  // BUG-389
  if (!writeIndexCsv(csv, kRowIdx, idx)) {
    cecon::native_unlink(csv);
    return rsError("bsample: could not write the resample index (disk full or I/O error)",
                   "data_io");
  }
  const std::string idxHbin = nextPath(session.workdir(), "bsample_idxh");
  if (auto err = runVerb({"convert", "--data", csv, "--out", idxHbin, "--delim", ","})) return *err;

  std::string drawFrame = idxHbin;
  if (idclusterOpt.has_value()) {
    // The draw ORDINAL, which is what `idcluster()` means: two draws of the same
    // cluster must be distinguishable downstream.
    std::vector<std::string> cells;
    cells.reserve(idcluster.size());
    for (int64_t v : idcluster) cells.push_back(std::to_string(v));
    std::string withId;
    if (auto err = appendColumn(session, idxHbin, *idclusterOpt, cells, "f64", &withId))
      return *err;
    drawFrame = withId;
  }

  const std::string merged = nextPath(session.workdir(), "bsample_merged");
  if (auto err = runVerb({"merge", "--data", drawFrame, "--using", baseIdx, "--type", "m:1",
                          "--key", kRowIdx, "--out", merged}))
    return *err;
  const std::string out = nextPath(session.workdir(), "bsample");
  if (auto err = runVerb({"eval", "--data", merged, "--keepif", "_merge!=2", "--drop", "_merge",
                          "--drop", kRowIdx, "--out", out}))
    return *err;
  session.load(out);
  if (auto bad = assertRowCount(session, "bsample", static_cast<int64_t>(idx.size()))) return *bad;
  if (nKept >= 0) {
    // e(sample) T2 (StataBE w2_esample_copies X2 `10|61|m0=20|m0_diff=10`): the
    // marker travels with each kept original row through the sort and keep, and
    // the rows `expand` creates are outside the sample (expand's rule,
    // runExpandIf). The merge carried the mask onto the copies; clear them.
    if (std::optional<std::vector<double>> cells = esampleHeadMask(session)) {
      bool any = false;
      for (size_t i = static_cast<size_t>(nKept); i < cells->size(); ++i) {
        any = any || (*cells)[i] != 0.0;
        (*cells)[i] = 0.0;
      }
      if (any && !esampleRewriteHeadMask(session, *cells))
        return rsError("bsample: the sample was drawn, but the estimation-sample marker could "
                       "not be cleared on the expanded copies; e(sample) is not reliable until "
                       "the next estimation.");
    }
  }
  return mutationResult(session, "bsample", idclusterOpt.has_value()
                                                ? std::vector<std::string>{*idclusterOpt}
                                                : std::vector<std::string>{});
}

// ===========================================================================
// pctile / _pctile
// ===========================================================================

namespace {

// The percentile list a `pctile`/`_pctile` command asks for. Either
// `percentiles()`/`p()` verbatim, or the nq-1 interior quantile points.
std::optional<std::vector<double>> percentileList(
    const std::vector<std::pair<std::string, eval::OptValue>>& opts, const std::string& verb,
    int defaultNq, ojson* err) {
  const std::optional<std::string> pOpt = optStrAny(opts, {"percentiles", "p"});
  const std::optional<std::string> nqOpt = optStrAny(opts, {"nquantiles", "nq"});
  if (pOpt.has_value() && nqOpt.has_value()) {
    *err = rsSyntax(verb + ": percentiles() and nquantiles() cannot both be specified");
    return std::nullopt;
  }
  std::vector<double> out;
  if (pOpt.has_value()) {
    // A numlist may be comma- or space-separated; both spellings appear in the
    // corpus and Stata accepts either.
    std::string spec = *pOpt;
    for (char& c : spec)
      if (c == ',') c = ' ';
    for (const auto& t : splitWs(spec)) {
      double v = 0.0;
      if (!parseStrictDouble(t, &v)) {
        *err = rsSyntax(verb + ": `" + t + "' in percentiles() is not a number");
        return std::nullopt;
      }
      if (!(v > 0.0 && v < 100.0)) {
        *err = rsSyntax(verb + ": percentiles() takes values strictly between 0 and 100, got " + t);
        return std::nullopt;
      }
      out.push_back(v);
    }
    if (out.empty()) {
      *err = rsSyntax(verb + ": percentiles() is empty");
      return std::nullopt;
    }
    return out;
  }
  long long nq = defaultNq;
  if (nqOpt.has_value()) {
    if (!parseStrictInt(*nqOpt, &nq) || nq < 2) {
      *err = rsSyntax(verb + ": nquantiles() requires a whole number of at least 2, got `" +
                      *nqOpt + "'");
      return std::nullopt;
    }
  }
  for (long long i = 1; i < nq; ++i)
    out.push_back(100.0 * static_cast<double>(i) / static_cast<double>(nq));
  return out;
}

// The sorted, missing-free values of a numeric column, or an error block.
std::optional<std::vector<double>> sortedValues(SessionState& session, const std::string& verb,
                                                const std::string& var, ojson* err) {
  ReadFrame rf(session, {var});
  Datastore* ds = rf.get();
  if (ds == nullptr) {
    *err = rsError(verb + ": could not read the data" + (rf.why().empty() ? "" : " — " + rf.why()),
                   "data_io");
    return std::nullopt;
  }
  if (!ds->has(var)) {
    *err = rsError("variable " + var + " not found");
    return std::nullopt;
  }
  if (ds->dtype(var) == "factor") {
    *err = rsError(verb + ": `" + var + "' is a string variable; percentiles need a numeric one.");
    return std::nullopt;
  }
  std::vector<double> v;
  const double* d = ds->col(var).f64();
  const int64_t n = ds->nRows();
  v.reserve(static_cast<size_t>(n));
  for (int64_t i = 0; i < n; ++i)
    if (!std::isnan(d[i])) v.push_back(d[i]);
  std::sort(v.begin(), v.end());
  return v;
}

// pctile, _pctile and xtile parse "aweight fweight pweight"
// (pctile's and xtile's weight syntax)
// and mark the sample with the weight: a missing or zero weight leaves the
// sample, a negative one is r(402), a non-integer fweight r(401). `rowWeight`
// (when non-null) receives every row's weight, NaN where the row is out of the
// sample, so xtile can leave those rows missing.
struct WeightedValues {
  std::vector<std::pair<double, double>> sorted;  // (x, w) ascending in x
};

std::optional<WeightedValues> sortedWeightedValues(SessionState& session, const std::string& verb,
                                                   const std::string& var,
                                                   const std::string& weightVar,
                                                   const std::string& weightType, ojson* err,
                                                   std::vector<double>* rowX = nullptr,
                                                   std::vector<double>* rowWeight = nullptr) {
  ReadFrame rf(session, {var, weightVar});
  Datastore* ds = rf.get();
  if (ds == nullptr) {
    *err = rsError(verb + ": could not read the data" + (rf.why().empty() ? "" : " — " + rf.why()),
                   "data_io");
    return std::nullopt;
  }
  for (const std::string* name : {&var, &weightVar}) {
    if (!ds->has(*name)) {
      *err = rsError("variable " + *name + " not found");
      return std::nullopt;
    }
    if (ds->dtype(*name) == "factor") {
      *err = rsErrorRc(verb + ": `" + *name + "' is a string variable", 109);
      return std::nullopt;
    }
  }
  const std::string wt = lowerOf(weightType);
  const bool isFreq = wt == "fw" || wt == "fweight";
  const double* x = ds->col(var).f64();
  const double* w = ds->col(weightVar).f64();
  const int64_t n = ds->nRows();
  WeightedValues out;
  out.sorted.reserve(static_cast<size_t>(n));
  if (rowX) rowX->assign(static_cast<size_t>(n), std::numeric_limits<double>::quiet_NaN());
  if (rowWeight) rowWeight->assign(static_cast<size_t>(n), std::numeric_limits<double>::quiet_NaN());
  for (int64_t i = 0; i < n; ++i) {
    const double wi = w[i];
    if (std::isnan(wi)) continue;
    if (wi < 0.0) {
      *err = rsErrorRc("negative weights encountered", 402);
      return std::nullopt;
    }
    if (isFreq && wi != std::floor(wi)) {
      *err = rsErrorRc("may not use noninteger frequency weights", 401);
      return std::nullopt;
    }
    if (wi == 0.0 || std::isnan(x[i])) continue;
    out.sorted.emplace_back(x[i], wi);
    if (rowX) (*rowX)[static_cast<size_t>(i)] = x[i];
    if (rowWeight) (*rowWeight)[static_cast<size_t>(i)] = wi;
  }
  std::stable_sort(out.sorted.begin(), out.sorted.end(),
                   [](const std::pair<double, double>& a, const std::pair<double, double>& b) {
                     return a.first < b.first;
                   });
  return out;
}

bool hasWeight(const eval::ParsedCommand& parsed) {
  return parsed.weightType.has_value() && !parsed.weightType->empty() &&
         parsed.weightVar.has_value() && !parsed.weightVar->empty();
}

}  // namespace

ojson runPctile(const eval::ParsedCommand& parsed, SessionState& session) {
  if (auto err = eval::rejectUnknownOptions(parsed.opts, kVocabPctile.known,
                                            kVocabPctile.unimplemented))
    return ojson(*err);
  // `if`/`in` is refused for the same reason `xtile` refuses it
  // (handlers_data_verbs3.cpp): with a qualifier Stata derives the cutpoints
  // from the RESTRICTED sample, and forwarding a --keepif would compute them
  // over every row and then blank the rest -- the same bins with a different
  // name, which is a wrong number, not a missing feature.
  if (auto q = refuseQualifiers(parsed, "pctile")) return *q;
  if (!session.hasData()) return ojson(eval::noDataLoaded());

  const std::string joined = joinWith(parsed.varlist, " ");
  const size_t eq = joined.find('=');
  if (eq == std::string::npos)
    return rsSyntax("pctile: syntax is `pctile newvar = varname, nquantiles(#) | percentiles(list) "
                    "[genp(newvar2)]`");
  const std::string newvar = trimOf(joined.substr(0, eq));
  const std::string src = trimOf(joined.substr(eq + 1));
  if (newvar.empty() || src.empty())
    return rsSyntax("pctile: syntax is `pctile newvar = varname, nquantiles(#) | percentiles(list) "
                    "[genp(newvar2)]`");
  ojson guard = newVarGuard(session, "pctile", newvar);
  if (!guard.is_null()) return guard;
  // BUG-1120: `pctile _skip = x' is "_skip not allowed" r(101) (StataBE 19.5).
  if (newvar == "_skip" || newvar == "_all") return rsErrorRc(newvar + " not allowed", 101);
  const std::optional<std::string> genp = optStr(parsed.opts, "genp");
  if (genp.has_value()) {
    ojson g2 = newVarGuard(session, "pctile", *genp);
    if (!g2.is_null()) return g2;
    if (*genp == newvar)
      return rsSyntax("pctile: genp() must name a different variable from the one being generated");
  }

  ojson err;
  auto pcts = percentileList(parsed.opts, "pctile", 2, &err);
  if (!pcts.has_value()) return err;
  std::optional<std::vector<double>> sorted;
  std::optional<WeightedValues> weighted;
  if (hasWeight(parsed)) {
    weighted = sortedWeightedValues(session, "pctile", src, *parsed.weightVar, *parsed.weightType,
                                    &err);
    if (!weighted.has_value()) return err;
  } else {
    sorted = sortedValues(session, "pctile", src, &err);
    if (!sorted.has_value()) return err;
  }

  Datastore* ds = nullptr;
  try {
    ds = session.datastore();
  } catch (const std::exception& e) {
    return rsError(std::string("pctile: ") + e.what(), "data_io");
  }
  if (ds == nullptr) return ojson(eval::noDataLoaded());
  const int64_t n = ds->nRows();

  // Row i holds percentile i; every row past the list is MISSING -- `.`, never
  // 0, which would read as a real percentile of zero.
  std::vector<std::string> cells(static_cast<size_t>(n));
  std::vector<std::string> pcells(static_cast<size_t>(n));
  for (size_t i = 0; i < pcts->size() && static_cast<int64_t>(i) < n; ++i) {
    cells[i] = numCell(weighted ? stataWeightedPercentile(weighted->sorted, (*pcts)[i])
                                : stataPercentile(*sorted, (*pcts)[i]));
    pcells[i] = numCell((*pcts)[i]);
  }

  std::string cur = *session.currentHbin();
  std::vector<std::string> newVars{newvar};
  std::string out;
  if (auto e2 = appendColumn(session, cur, newvar, cells, "f64", &out)) return *e2;
  cur = out;
  if (genp.has_value()) {
    if (auto e3 = appendColumn(session, cur, *genp, pcells, "f64", &out)) return *e3;
    cur = out;
    newVars.push_back(*genp);
  }
  session.load(cur);
  return mutationResult(session, "pctile", newVars);
}

ojson runPctileR(const eval::ParsedCommand& parsed, SessionState& session) {
  if (auto err = eval::rejectUnknownOptions(parsed.opts, kVocabPctileR.known,
                                            kVocabPctileR.unimplemented))
    return ojson(*err);
  if (auto q = refuseQualifiers(parsed, "_pctile")) return *q;
  if (!session.hasData()) return ojson(eval::noDataLoaded());
  if (parsed.varlist.size() != 1)
    return rsSyntax("_pctile: syntax is `_pctile varname, nquantiles(#) | percentiles(list)`");

  ojson err;
  auto pcts = percentileList(parsed.opts, "_pctile", 2, &err);
  if (!pcts.has_value()) return err;
  std::optional<std::vector<double>> sorted;
  std::optional<WeightedValues> weighted;
  if (hasWeight(parsed)) {
    weighted = sortedWeightedValues(session, "_pctile", parsed.varlist[0], *parsed.weightVar,
                                    *parsed.weightType, &err);
    if (!weighted.has_value()) return err;
  } else {
    sorted = sortedValues(session, "_pctile", parsed.varlist[0], &err);
    if (!sorted.has_value()) return err;
  }

  // r(r1)..r(rN) IS this command -- there is no column and no table. The arm
  // deliberately does not call captureRFromBlock (there is no block to capture
  // FROM), so the fill happens here or not at all (defect class 5). Stale
  // r(r#) from a longer previous call are cleared so a shorter list cannot be
  // read as if it were still long.
  for (int i = 1; i <= 1000; ++i) {
    const std::string k = "r" + std::to_string(i);
    if (!session.eval.rValues.contains(k)) break;
    session.eval.rValues.erase(k);
  }
  for (size_t i = 0; i < pcts->size(); ++i) {
    session.eval.rValues.set("r" + std::to_string(i + 1),
                             weighted ? stataWeightedPercentile(weighted->sorted, (*pcts)[i])
                                      : stataPercentile(*sorted, (*pcts)[i]));
  }
  // Deliberately NO r(N): Stata's `_pctile` stores r(r1)..r(rN) and nothing
  // else, and publishing an extra scalar would let a do-file read a number this
  // command does not promise.
  // Stata's `_pctile` prints nothing.
  return silent();
}

// xtile with [aw=]/[fw=]/[pw=]: xtile takes the cutpoints from
// `pctile ... [weight], n(#)` (it accepts aweight fweight pweight),
// so they are stataWeightedPercentile at 100k/nq. A row outside the weighted
// sample (missing or zero weight: `mark touse [wt]`) stays missing. The
// category is 1 + the number of cutpoints strictly below the value, which is
// xtile's numbering with duplicate cutpoints kept (the engine's
// xtile_assign rule, src/data/frame.cpp).
ojson runXtileWeighted(const std::string& newvar, const std::string& srcvar, int nq,
                       const std::string& weightVar, const std::string& weightType,
                       SessionState& session) {
  ojson err;
  std::vector<double> rowX, rowW;
  auto weighted =
      sortedWeightedValues(session, "xtile", srcvar, weightVar, weightType, &err, &rowX, &rowW);
  if (!weighted.has_value()) return err;
  if (weighted->sorted.empty()) return rsErrorRc("no observations", 2000);
  std::vector<double> cut;
  for (int k = 1; k < nq; ++k)
    cut.push_back(stataWeightedPercentile(weighted->sorted, 100.0 * k / nq));
  std::vector<std::string> cells(rowX.size());
  for (size_t i = 0; i < rowX.size(); ++i) {
    if (std::isnan(rowX[i]) || std::isnan(rowW[i])) continue;
    int g = 1;
    for (double c : cut)
      if (rowX[i] > c) ++g;
    cells[i] = numCell(static_cast<double>(g));
  }
  std::string out;
  if (auto e = appendColumn(session, *session.currentHbin(), newvar, cells, "f64", &out)) return *e;
  session.load(out);
  return mutationResult(session, "gen", {newvar});
}

}  // namespace piet::handlers
