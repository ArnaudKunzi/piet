// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// AER-SCOPE1 bundle B (RESULTS, 2026-09-03) -- r() made visible, and the
// matrix <-> dataset round trip.
//
//   return list      print r() the way `ereturn list` prints e()
//   svmat            matrix columns -> variables
//   mkmat            variables -> matrix
//   matlist          render a matrix (or e(b)/e(V)) as a text table
//
// WHY ALL FOUR SHARE ONE TU. They share three things and nothing else in the
// tree shares them: the r() stores, session.eval.matrices, and the numeric
// rendering rule. Splitting them would put the matrix-store helpers (the
// replace-in-place store, the e(b)/e(V) extraction) in a header nobody else
// needs, or duplicate them, which is exactly how the two-layer divergences in
// this codebase have started before.
//
// THREE DEFECT CLASSES THIS FILE IS WRITTEN AGAINST, all three already shipped
// once in this campaign:
//
//  1. SHAPE. dispatcher.cpp's bundle-B arms are plain
//     `blocks.push_back(ojson(handlers::runX(parsed, session)))` -- they do NOT
//     carry the `if (!(r.is_array() && r.empty()))` filter the `matrix`/`label`
//     arms have. So a handler here returns EITHER exactly one JSON OBJECT or an
//     EMPTY array (silent success). A non-empty array would land where an
//     object belongs and nlohmann throws type_error.306 at the first consumer
//     that reads ["type"].
//  2. RC. eval::coarseRc answers a specific Stata rc only for
//     code=="unsupported_command" (199), a `reason` spelled "r(NNN)", or one of
//     its measured message-substring rules. A refusal built with the default
//     "engine_error" code and a freshly-worded message silently becomes rc 1.
//     So every refusal below either uses a wording coarseRc already maps
//     ("variable X not found" -> 111, "... already defined" -> 110) or states
//     its rc explicitly through errorBlock's `reason` argument.
//  3. VOCABULARY. rejectUnknownOptions resolves a >=2-char unique prefix
//     against `known` and therefore ACCEPTS abbreviations; canonicalizeForCommand
//     is what makes the handler's canonical-key compare see them. It is driven
//     by the REGISTRY, so an unregistered command has its abbreviations accepted
//     by the net and then dropped unread by the compares -- accepted, ignored,
//     no diagnostic (BUG-231/BUG-233). All three option-taking handlers register
//     at namespace scope below.
#include "piet/handlers.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <fstream>
#include <limits>
#include <memory>
#include <optional>
#include <regex>
#include <set>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "io/win_compat.h"  // cecon::to_native_path, cecon::native_exists
#include "piet/cobra_runner.h"
#include "piet/datastore.h"
#include "piet/eval/errors.h"
#include "piet/eval/exprfns.h"  // stataExtendedMissingKind (BUG-1511); stataMissingToken for a `.a` cell (BUG-1399)
#include "piet/eval/session.h"
#include "piet/eval/tokenizer.h"
#include "piet/inrange_filter.h"
#include "piet/loops.h"
#include "piet/mata_subset.h"
#include "piet/mutation_result.h"
#include "piet/scratch_seq.h"
#include "piet/session_state.h"
#include "piet/storage_type.h"
#include "piet/stata_format.h"
#include "piet/strutil.h"

namespace piet::handlers {

using ojson = nlohmann::ordered_json;

namespace {

// ---------------------------------------------------------------------------
// Option vocabularies -- NAMESPACE SCOPE, not function-local statics.
//
// The registry is consulted by the DISPATCHER (canonicalizeForCommand, before
// the handler runs). A function-local static would only be constructed the
// first time the handler is entered, i.e. AFTER the one call that needed it, so
// the very first `svmat A, name(col)` in a session would canonicalize against
// an empty registry and land in the handler still spelled "name". Namespace
// scope registers at static-init time, which is what the dispatcher's ordering
// requires. registerOptVocab is documented safe there (function-local static
// registry, no cross-TU init order).

// `names()` is svmat's only option.
const eval::OptVocab& kSvmatVocab = eval::registerOptVocab({"svmat"}, {{"names"}, {}});

// getmata.ado's `syntax ... [, DOUBLE FORCE ID(string) REPLACE UPdate]`. All
// five are declared: update and id() are UNIMPLEMENTED (neither is in the
// corpus) and must be refused by name, not reported as unknown options and not
// accepted and ignored.
const eval::OptVocab& kGetmataVocab =
    eval::registerOptVocab({"getmata"}, {{"double", "force", "replace"}, {"update", "id"}});

// `mat` is listed alongside `matrix` because [P] mkmat documents both
// spellings; a 1-char-longer prefix like `matr` still resolves (unique among
// the two), while `ma` is ambiguous and rejects -- which is Stata's answer too.
// roweq()/nchar() are declared UNIMPLEMENTED rather than omitted: omitting them
// makes them "unknown option" (a wrong diagnosis), and accepting them would
// silently ignore an equation-name or name-truncation request.
const eval::OptVocab& kMkmatVocab = eval::registerOptVocab(
    {"mkmat"},
    {{"matrix", "mat", "rownames", "nomissing", "obs", "rowprefix"}, {"roweq", "nchar"}});

// matlist's declared-but-unimplemented set is large on purpose: every one of
// these changes the RENDERING, so accepting one silently would print a table
// that is not the table the user asked for.
const eval::OptVocab& kMatlistVocab = eval::registerOptVocab(
    {"matlist"},
    {{"format", "title", "names", "nonames", "noblank", "twidth"},
     {"cspec", "rspec", "lines", "border", "left", "right", "rightindent", "showcoleq",
      "roweqonly", "coleqonly", "aligncolnames", "nohalf", "nodotz", "underscore", "linesize"}});

// ---------------------------------------------------------------------------
// Small shared helpers.

ojson aerError(const std::string& message, const std::string& code = "engine_error") {
  return ojson(eval::engineError(message, code));
}

// Refusal whose Stata rc is KNOWN but which coarseRc's message rules do not
// recognise. `reason` is the only channel that carries an exact rc (BUG-187),
// and it outranks the coarse mapping -- without it every one of these would
// come back as the generic rc 1 (defect class 2 in this file's header).
ojson aerErrorRc(const std::string& message, int rc) {
  return ojson(eval::errorBlock("engine_error", message, std::nullopt, std::nullopt, std::nullopt,
                                "r(" + std::to_string(rc) + ")"));
}

bool isErrorBlock(const ojson& b) {
  return b.is_object() && b.value("type", std::string()) == "error";
}

// mintScratchPath always names a ".hbin"; a setcol payload is a CSV. Registering
// the minted .hbin and then writing a .csv beside it would leave the file that
// actually exists unregistered, which is the BUG-389 orphan class the registry
// exists to close (sweepOrphanScratch only removes paths it knows). Same body as
// handlers_aer_abbrev.cpp's, and duplicated for the same reason that one is not
// shared: it is four lines behind a one-line intent, and a header for it would
// touch a merge surface this stint does not own.
std::string mintCsvPath(const std::string& workdir, const std::string& prefix) {
  const std::string hbin = mintScratchPath(workdir, prefix);
  forgetScratchMint(hbin);
  const std::string csv = hbin.substr(0, hbin.size() - 5) + ".csv";  // 5 == strlen(".hbin")
  registerScratchMint(csv);
  return csv;
}

// The headerless single-column payload `cobra setcol` reads
// (Frame::read_csv_column -- one raw value per row, row count == n_rows). An
// EMPTY line is how a missing value is spelled in that format; writing "nan" or
// "." instead would land a literal token in a numeric column.
bool writeValuesCsv(const std::string& path, const std::vector<double>& vals) {
  std::ofstream f(cecon::to_native_path(path));
  if (!f) return false;
  f.setf(std::ios::fmtflags(0), std::ios::floatfield);
  f.precision(17);
  for (double v : vals) {
    if (!std::isnan(v)) f << v;
    f << "\n";
  }
  return static_cast<bool>(f);
}

// getmata stores from Mata's double vector, compresses exact integer values,
// then permits a float copy unless `double` was requested. The fixed capture
// w2c_ml_getmata.log:1058/1092 pins byte for the integer vector even with
// `double`, and float-rounded fractional copies by default. Existing variables
// retain at least their original type (getmata.ado combine_num_vartype).
std::string getmataStoredType(const std::vector<double>& values, bool useDouble,
                              const std::string& original) {
  bool integer = true, exactFloat = true, floatFits = true;
  double lo = 0.0, hi = 0.0;
  for (double value : values) {
    if (std::isnan(value)) continue;
    integer = integer && std::isfinite(value) && std::trunc(value) == value;
    lo = std::min(lo, value);
    hi = std::max(hi, value);
    const double f = static_cast<double>(static_cast<float>(value));
    exactFloat = exactFloat && f == value;
    floatFits = floatFits && std::isfinite(f) && std::fabs(f) <= 0x1.fffffep+126;
  }
  std::string compact;
  if (integer) compact = storage::promotedType("byte", false, lo, hi);
  else compact = exactFloat ? "float" : "double";
  if (original.empty()) {
    if (compact == "double" && !useDouble && floatFits) return "float";
    return compact;
  }
  if (original == "double" || compact == "double") return "double";
  if (original == compact) return original;
  if (original == "float") return compact == "long" ? "double" : "float";
  if (original == "long") return compact == "float" ? "double" : "long";
  if (original == "int" && compact == "byte") return "int";
  return compact;
}

// cobra reports a structured failure as {"error":{"code","message"}} on stderr.
// Surfacing the raw blob would put JSON punctuation in front of the user;
// surfacing nothing would make a failed setcol indistinguishable from a
// successful one at the call site.
std::string cobraFailure(const CobraRunResult& r) {
  std::string text = r.stderr_text;
  if (!text.empty()) {
    try {
      const nlohmann::json parsed = nlohmann::json::parse(text);
      if (parsed.is_object() && parsed.contains("error") && parsed["error"].is_object() &&
          parsed["error"].contains("message")) {
        text = parsed["error"]["message"].get<std::string>();
      }
    } catch (const std::exception&) {
      // Not structured: the raw text below is the best available account.
    }
  }
  if (text.empty()) text = r.error;
  while (!text.empty() && (text.back() == '\n' || text.back() == '\r' || text.back() == ' ')) {
    text.pop_back();
  }
  if (text.empty()) text = "cobra setcol failed with exit code " + std::to_string(r.exit_code);
  return text;
}

// Widths count CHARACTERS (StataBE pads a Unicode name by characters:
// scratch-r150/uni-v/p4.do), so a multi-byte label stays aligned.
std::string rjust(const std::string& s, size_t w) {
  const size_t n = cecon::ident::codePointCount(s);
  return n >= w ? s : std::string(w - n, ' ') + s;
}

// Replace-or-append into session.eval.matrices. handlers_m3.cpp has the same
// three lines under the name storeMatrixResult, but that copy is file-local
// (anonymous namespace) -- appending a duplicate instead would leave
// findMatrix() answering the OLD entry forever, since it returns the first
// match. A scalar of the same name is destroyed: scalars and matrices share one
// namespace ([P] matrix define, "Namespace").
void storeMatrix(SessionState& session, const std::string& name, eval::EvalMatrix mat) {
  session.eval.scalars.erase(name);
  session.eval.stringScalars.erase(name);
  for (auto& p : session.eval.matrices) {
    if (p.first == name) {
      p.second = std::move(mat);
      return;
    }
  }
  session.eval.matrices.emplace_back(name, std::move(mat));
}

// ---------------------------------------------------------------------------
// `return list`

// Both `return list` sections pad the NAME to 22 and then write the value, and
// that width is not this file's invention: it is handlers_m3.cpp's
// runEreturnList padLabel, and the two commands print the SAME table from
// different stores. A second width here would let `ereturn list` and `return
// list` drift into disagreeing about their own column.
std::string padLabel22(const std::string& s) {
  const size_t n = cecon::ident::codePointCount(s);
  return n >= 22 ? s : std::string(22 - n, ' ') + s;
}

// runEreturnList's fmtScalar, plus the missing case. Its own scalars come from
// a cobra result that OMITS an absent key, so it never had to answer this; r()
// genuinely carries missing -- program.cpp's handleReturnStmt stages the literal
// "nan" whenever the `return scalar` expression did not evaluate, and Stata
// prints that as ".".
//
// The other missing is Stata's own double, 2^1023 and up (kStataMissingSentinel):
// summarize posts one observation's r(sd) and r(Var) that way, and centile a
// missing r(c_#), r(lb_#), r(ub_#). It is listed by its kind, `.` or `.a`..`.z`
// (StataBE 19.5, BUG-1511: `r(sd) =  .` where this printed 8.98846567431158e+307).
std::string fmtRScalar(double v) {
  if (std::isnan(v)) return ".";
  if (const auto kind = eval::stataExtendedMissingKind(v))
    return *kind == 0 ? std::string(".") : std::string(".") + static_cast<char>('a' + *kind - 1);
  std::ostringstream oss;
  oss.precision(16);
  oss << v;
  return oss.str();
}

// eval.rStrings doubles as the s-class store: `sreturn local` writes there under
// program.cpp's / eval/macros.cpp's kSClassKeyPrefix ("\x01s:"), because P4's
// "no new session stores" constraint left rStrings as the only persistent string
// store. `return list` prints r() ONLY, so those rows must be filtered here or
// `sreturn local x` shows up as an r() macro literally named "\x01s:x".
//
// The test is on the RESERVED LEAD BYTE rather than the exact three-byte prefix:
// \x01 cannot begin a real Stata name (strutil.h's isValidStataIdentifier
// rejects it), so every reserved-key convention this store may grow is excluded
// by construction instead of only today's single one.
constexpr char kReservedKeyLeadByte = '\x01';

}  // namespace

ojson runReturnList(SessionState& session) {
  std::ostringstream out;

  // MOST-RECENTLY-SET FIRST, which is why both loops run in reverse: the
  // OrderedDict/OrderedFloatDict contract is insertion order with
  // assignment-keeps-position, so the tail of items() is the newest key and
  // forward iteration would print Stata's list upside down.
  out << "\nscalars:\n";
  const auto& scalars = session.eval.rValues.items();
  for (auto it = scalars.rbegin(); it != scalars.rend(); ++it) {
    out << padLabel22("r(" + it->first + ")") << " =  " << fmtRScalar(it->second) << "\n";
  }

  out << "\nmacros:\n";
  const auto& macros = session.eval.rStrings.items();
  for (auto it = macros.rbegin(); it != macros.rend(); ++it) {
    if (!it->first.empty() && it->first[0] == kReservedKeyLeadByte) continue;
    out << padLabel22("r(" + it->first + ")") << " : \"" << it->second << "\"\n";
  }

  // r-class matrices (sktest's r(table)). CONDITIONAL, unlike the two headers
  // above: those are unconditional by an older decision, and every frozen
  // `return list` fixture in the tree was captured without a matrices: section.
  // Emitting one unconditionally would change the bytes of every existing
  // return-list pin at once -- a whole-tree re-capture to say "no matrices",
  // which is exactly the re-pin that hides real drift. So the section appears
  // only when a command actually published an r-matrix, and its absence keeps
  // meaning what it has always meant.
  if (!session.eval.rMatrices.empty()) {
    out << "\nmatrices:\n";
    const auto& rmats = session.eval.rMatrices;
    for (auto it = rmats.rbegin(); it != rmats.rend(); ++it) {
      const size_t nr = it->second.data.size();
      const size_t nc = nr > 0 ? it->second.data[0].size() : 0;
      out << padLabel22("r(" + it->first + ")") << " :  " << nr << " x " << nc << "\n";
    }
  }

  // Headers with no rows when a store is empty: that is the honest report that
  // r() has no scalars, and it is what real Stata prints. Suppressing the
  // header would make "nothing set" indistinguishable from "this build does not
  // track that half".
  // "display": output, not the command echo, so `noisily` shows it and `quietly`
  // hides it (BUG-1505, as runEreturnList).
  ojson block;
  block["type"] = "display";
  block["text"] = out.str();
  return block;
}

// ---------------------------------------------------------------------------
// svmat

namespace {

bool isNumericStorageType(const std::string& w) {
  return w == "double" || w == "float" || w == "long" || w == "int" || w == "byte";
}

bool isStringStorageType(const std::string& w) {
  if (w == "strl") return true;
  if (w.size() < 4 || w.compare(0, 3, "str") != 0) return false;
  return w.find_first_not_of("0123456789", 3) == std::string::npos;
}

}  // namespace

ojson runSvmat(const eval::ParsedCommand& parsed, SessionState& session) {
  // The reject net runs FIRST, before any matrix is resolved or any scratch file
  // is minted: a command that is going to be refused must cost nothing.
  if (auto err = eval::rejectUnknownOptions(parsed.opts, kSvmatVocab.known,
                                            kSvmatVocab.unimplemented)) {
    return ojson(*err);
  }

  const std::vector<std::string>& words = parsed.varlist;
  if (words.empty()) return aerError("svmat: no matrix specified");
  if (words.size() > 2) {
    return aerError("svmat: too many arguments; the syntax is `svmat [type] A [, names(...)]'",
                    "syntax_error");
  }

  if (words.size() == 2) {
    const std::string type = strutil::lower(words[0]);
    if (isStringStorageType(type)) {
      // A str# request is not a storage-type nicety here: it asks for a STRING
      // variable, and a matrix has no strings to put in one. Accepting it would
      // create a numeric column under a promise it does not keep.
      return aerError("svmat: type " + words[0] +
                          " is not allowed; a matrix holds numeric values only",
                      "syntax_error");
    }
    if (!isNumericStorageType(type)) {
      return aerError("svmat: `" + words[0] + "' is not a storage type", "syntax_error");
    }
    // ACCEPTED AND IGNORED, deliberately and visibly: PIET has exactly one
    // numeric storage type (f64), so double/float/long/int/byte all land in the
    // same column. Refusing them would stop do-files over a distinction this
    // engine does not have; honouring them is impossible. The values are exact
    // either way -- what is lost is only the on-disk width.
  }
  const std::string matName = words.back();

  const eval::EvalMatrix* mat = session.eval.findMatrix(matName);
  if (mat == nullptr) {
    // Stata answers r(111) for a matrix that does not exist, and coarseRc's
    // "variable"+"not found" rule does not cover the word "matrix".
    return aerErrorRc("svmat: matrix " + matName + " not found", 111);
  }
  const size_t nMatRows = mat->data.size();
  const size_t nMatCols = nMatRows > 0 ? mat->data[0].size() : 0;
  if (nMatRows == 0 || nMatCols == 0) {
    return aerError("svmat: matrix " + matName + " has no elements");
  }
  for (const auto& row : mat->data) {
    if (row.size() != nMatCols) {
      return aerError("svmat: matrix " + matName + " is ragged and cannot be read as columns");
    }
  }
  // COPY, not the pointer. findMatrix hands back a pointer INTO
  // session.eval.matrices, and every line below this one mutates the session
  // (runSetObs, session.load). Nothing on that path appends a matrix today, so
  // this guards a future one rather than a live bug -- but the failure it guards
  // is a dangling read that would write PLAUSIBLE numbers into the dataset,
  // which is the worst shape a defect can take here.
  const eval::EvalMatrix matrix = *mat;

  std::string namesArg;
  for (const auto& kv : parsed.opts) {
    if (kv.first != "names") continue;
    if (kv.second.isBool || kv.second.strValue.empty()) {
      return aerError("svmat: option names() requires an argument");
    }
    namesArg = strutil::trim(kv.second.strValue);
  }

  // eqcol is folded onto col rather than rejected: PIET's EvalMatrix carries no
  // equation names at all (eval/session.h has rownames/colnames and nothing
  // else), so "the column name with its equation stripped" IS the column name
  // here. Refusing it would stop a do-file over a distinction this build cannot
  // express; inventing an equation prefix would be a fabricated name.
  const std::string namesKey = strutil::lower(namesArg);
  const bool useColNames = (namesKey == "col" || namesKey == "eqcol" || namesKey == "matcol");
  const bool prefixWithMatName = (namesKey == "matcol");
  const bool useStub = !namesArg.empty() && !useColNames;
  const std::string stub = useStub ? namesArg : matName;

  std::vector<std::string> newVars;
  newVars.reserve(nMatCols);
  for (size_t j = 0; j < nMatCols; ++j) {
    std::string name;
    if (useColNames) {
      if (j >= matrix.colnames.size() || matrix.colnames[j].empty()) {
        return aerError("svmat: matrix " + matName + " has no column name for column " +
                        std::to_string(j + 1) + "; names(" + namesArg + ") cannot name it");
      }
      name = prefixWithMatName ? matName + matrix.colnames[j] : matrix.colnames[j];
    } else {
      name = stub + std::to_string(j + 1);
    }
    if (!strutil::isValidStataIdentifier(name)) {
      return aerError("svmat: `" + name + "' is not a valid variable name. " +
                          strutil::stataIdentifierRuleText(),
                      "syntax_error");
    }
    // Stata's reserved names ([U] 11.3) are well-formed identifiers that no
    // variable may take: `svmat b, names(col)` over e(b) meets `_cons` and
    // stops with "_cons invalid varname", r(198) (w2g_ebmat.log:1388-1393
    // e11_svmat). Reachable since e(b) carries Stata's `_cons` (eb-order T-P1)
    // where it used to carry the engine's `(intercept)`.
    {
      static const std::set<std::string> kReserved = {
          "_all", "_b",   "byte", "_coef", "_cons", "double", "float", "if",   "in",
          "int",  "long", "_n",   "_N",    "_pi",   "_pred",  "_rc",   "_skip", "strL",
          "using", "with"};
      static const std::regex reStrN(R"(^str[0-9]+$)");
      if (kReserved.count(name) || std::regex_match(name, reStrN))
        return aerErrorRc(name + " invalid varname", 198);
    }
    if (std::find(newVars.begin(), newVars.end(), name) != newVars.end()) {
      // Two matrix columns sharing a colname would otherwise have the second
      // silently overwrite the first, losing a column with no diagnostic.
      return aerError("svmat: names(" + namesArg + ") would create variable " + name + " twice");
    }
    newVars.push_back(name);
  }

  // COLLISION CHECK BEFORE ANY WRITE. Each column is a separate cobra setcol, so
  // discovering the clash on column 3 would leave columns 1 and 2 already in the
  // dataset -- a half-written svmat, which is worse than a refused one because
  // the user has no way to tell how far it got.
  Datastore* ds = session.datastore();
  const int64_t curRows = ds ? ds->nRows() : 0;
  if (ds != nullptr) {
    for (const auto& name : newVars) {
      if (ds->has(name)) {
        // "already defined" is the wording coarseRc maps to r(110) (confirm new
        // variable); the phrase is load-bearing, not decoration.
        return aerError("svmat: variable " + name + " already defined");
      }
    }
  }

  // Stata's documented behaviour: svmat EXTENDS the dataset when the matrix has
  // more rows than there are observations, and with no dataset at all this is
  // how the dataset comes into existence (runSetObs' own no-currentHbin branch
  // builds an N-row, 0-variable frame).
  const int64_t nOut = std::max<int64_t>(curRows, static_cast<int64_t>(nMatRows));
  const bool extended = nOut > curRows;
  if (extended) {
    ojson setObs = runSetObs(std::to_string(nOut), session);
    if (isErrorBlock(setObs)) return setObs;
  }
  if (!session.currentHbin().has_value()) {
    return ojson(eval::noDataLoaded());
  }

  // Each column is its own setcol + session.load, so a failure INSIDE the loop
  // leaves the dataset half written and there is nothing to roll back to (the
  // committed columns and the `set obs` extension are both already the session
  // head). The collision net above is what makes this reachable only by I/O
  // failure -- but when it IS reached, the count is the difference between a
  // user who knows to drop the partial columns and re-run and one who reads a
  // bare "could not write" as "nothing happened" and re-runs into a collision
  // refusal that names a variable they never asked for.
  auto writeFailure = [&](size_t done, const std::string& what) {
    std::string msg = "svmat: " + what;
    if (done > 0) {
      msg += "; " + std::to_string(done) + " of " + std::to_string(nMatCols) +
             " columns were already written and are still in the dataset";
    } else if (extended) {
      msg += "; the dataset was already extended to " + std::to_string(nOut) + " observations";
    }
    return aerError(msg);
  };

  for (size_t j = 0; j < nMatCols; ++j) {
    std::vector<double> vals;
    vals.reserve(static_cast<size_t>(nOut));
    for (int64_t i = 0; i < nOut; ++i) {
      // Rows past the matrix are system-missing, not zero: the matrix says
      // nothing about them, and a zero would be a fabricated observation.
      vals.push_back(static_cast<size_t>(i) < nMatRows
                         ? matrix.data[static_cast<size_t>(i)][j]
                         : std::numeric_limits<double>::quiet_NaN());
    }

    const std::string csvPath = mintCsvPath(session.workdir(), "svmat_vals");
    if (!writeValuesCsv(csvPath, vals)) {
      return writeFailure(j, "could not write the values for " + newVars[j]);
    }
    const std::string outPath = mintScratchPath(session.workdir(), "svmat");
    const CobraRunResult r =
        runCobra({"setcol", "--data", *session.currentHbin(), "--var", newVars[j], "--values",
                  csvPath, "--dtype", "f64", "--new", "--out", outPath});
    if (!r.ok) return writeFailure(j, cobraFailure(r));
    // BUG-124's torn-write guard, carried from every other stateless setcol
    // site: the engine's writer creates `--out` before it can detect a write
    // failure, and a full scratch volume has twice reached PIET as a ZERO exit
    // code. session.load on a path that is not there does not fail -- it records
    // the head and hands the next Datastore open a missing or truncated
    // mapping, which is a crash rather than a diagnostic.
    if (!cecon::native_exists(outPath)) {
      return writeFailure(j, "failed writing output dataset (disk full or I/O error)");
    }
    session.load(outPath);
  }

  // ONE mutationResult for the whole command, not one per column: the card the
  // UI reads must describe the dataset the user ends up with, and k separate
  // "gen" blocks would each claim to be the finished state.
  return mutationResult(session, "gen", newVars);
}

// ---------------------------------------------------------------------------
// getmata
//
// Oracle: tests/eval/golden/aer_w2/w2c_ml_getmata.log family (8) (getmata_030_seq
// :1058, getmata_exists :1074, getmata_replace :1083, getmata_double :1092,
// getmata_rows_ne :1107, getmata_force_fewer :1116, getmata_force_more :1130,
// getmata_nodata :1141, getmata_notfound :1151, getmata_matrix_paren :1167,
// getmata_paren_badcount :1177) and family (9) getmata_087_shape :1225. Rules
// and their ORDER are getmata.ado's (ado/base/g/getmata.ado): per element, the
// matrix must exist (111), have a column (459), match the name count (102 too
// few / 103 too many) and not collide with an existing variable unless
// `replace` (110); then names must be unique across the getlist (198); then,
// unless `force`, every vector's rows must equal _N (459). `force` is implied
// when the session has no observations AND no variables (st_nobs()==0 &
// c("k")==0). Every check runs BEFORE the first write, as runSvmat's does.

namespace {

struct GetmataElement {
  std::vector<std::string> vars;
  std::string mat;
  eval::EvalMatrix value;  // a COPY: the writes below mutate the session
  std::vector<bool> exists;
};

// getmata.ado's tokeninit(" ", "=", "()"): whitespace separates, `=` is a
// token of its own, and a parenthesised group is ONE token.
std::vector<std::string> getlistTokens(const std::string& s) {
  std::vector<std::string> out;
  size_t i = 0;
  while (i < s.size()) {
    const char ch = s[i];
    if (std::isspace(static_cast<unsigned char>(ch))) {
      ++i;
      continue;
    }
    if (ch == '=') {
      out.emplace_back("=");
      ++i;
      continue;
    }
    if (ch == '(') {
      const size_t close = s.find(')', i);
      const size_t end = close == std::string::npos ? s.size() : close + 1;
      out.push_back(s.substr(i, end - i));
      i = end;
      continue;
    }
    size_t j = i;
    while (j < s.size() && !std::isspace(static_cast<unsigned char>(s[j])) && s[j] != '=' &&
           s[j] != '(')
      ++j;
    out.push_back(s.substr(i, j - i));
    i = j;
  }
  return out;
}

bool isMataName(const std::string& n) { return cecon::ident::isValidName(n); }

}  // namespace

ojson runGetmata(const eval::ParsedCommand& parsed, SessionState& session) {
  if (auto err = eval::rejectUnknownOptions(parsed.opts, kGetmataVocab.known,
                                            kGetmataVocab.unimplemented)) {
    return ojson(*err);
  }
  bool force = false, replace = false;
  bool useDouble = session.defaultStorageType == "double";
  for (const auto& kv : parsed.opts) {
    if (kv.first == "force") force = true;
    if (kv.first == "replace") replace = true;
    if (kv.first == "double") useDouble = true;
  }

  std::string getlist;
  for (const auto& w : parsed.varlist) getlist += (getlist.empty() ? "" : " ") + w;
  if (strutil::trim(getlist).empty()) {
    return aerErrorRc("getmata: getlist required -- the syntax is `getmata name`, "
                      "`getmata varname=vecname` or `getmata (var1 var2 ...)=matname`",
                      100);
  }

  Datastore* ds = session.hasData() ? session.datastore() : nullptr;
  const int64_t curRows = ds ? ds->nRows() : 0;
  const size_t curVars = ds ? ds->names().size() : 0;
  if (curRows == 0 && curVars == 0) force = true;  // getmata: nothing in memory implies force

  // ---- parse and validate every element before touching anything ----------
  std::vector<GetmataElement> els;
  const std::vector<std::string> toks = getlistTokens(getlist);
  for (size_t t = 0; t < toks.size(); ++t) {
    GetmataElement el;
    bool expandStub = false;
    if (toks[t][0] == '(') {
      std::string inner = toks[t].substr(1);
      if (!inner.empty() && inner.back() == ')') inner.pop_back();
      std::istringstream iss(inner);
      std::string v;
      while (iss >> v) el.vars.push_back(v);
      if (el.vars.empty()) {
        return aerErrorRc("getmata: nothing found where evarnamelist expected", 198);
      }
      if (t + 1 >= toks.size() || toks[t + 1] != "=") {
        return aerErrorRc("getmata: " +
                              (t + 1 < toks.size() ? toks[t + 1] : std::string("nothing")) +
                              " found where equal sign (=) expected",
                          198);
      }
      if (t + 2 >= toks.size()) {
        return aerErrorRc("getmata: nothing found where matrix name expected", 198);
      }
      el.mat = toks[t + 2];
      t += 2;
      expandStub = el.vars.size() == 1 && el.vars[0].back() == '*';
    } else if (t + 1 < toks.size() && toks[t + 1] == "=") {
      if (t + 2 >= toks.size()) {
        return aerErrorRc("getmata: nothing found where matrix name expected", 198);
      }
      el.vars = {toks[t]};
      el.mat = toks[t + 2];
      t += 2;
    } else if (toks[t] == "=") {
      return aerErrorRc("getmata: = found where a variable name expected", 198);
    } else {
      el.vars = {toks[t]};
      el.mat = toks[t];
    }

    if (!isMataName(el.mat)) {
      return aerErrorRc("getmata: " + el.mat + " invalid vector or matrix name", 198);
    }
    // The Mata store ONLY (SPEC D3): a Stata matrix of the same name is a
    // different object and must not be found here.
    const eval::EvalMatrix* m = mata::findMataMatrix(session.eval, el.mat);
    if (m == nullptr) return aerErrorRc("vector or matrix " + el.mat + " not found", 111);
    el.value = *m;
    const size_t cols = mata::colsOf(el.value);
    if (cols == 0 && !expandStub) {
      return aerErrorRc("matrix " + el.mat + " has 0 columns", 459);
    }
    if (expandStub) {
      const std::string base = el.vars[0].substr(0, el.vars[0].size() - 1);
      el.vars.clear();
      for (size_t j = 1; j <= cols; ++j) el.vars.push_back(base + std::to_string(j));
    }
    if (el.vars.size() != cols) {
      const bool tooFew = el.vars.size() < cols;
      return aerErrorRc(std::string(tooFew ? "too few" : "too many") +
                            " variables specified for " + (cols == 1 ? "vector " : "matrix ") +
                            el.mat,
                        tooFew ? 102 : 103);
    }
    for (const auto& v : el.vars) {
      if (!strutil::isValidStataIdentifier(v)) {
        return aerErrorRc("getmata: " + v + " invalid variable name", 198);
      }
      const bool exists = ds != nullptr && ds->has(v);
      if (exists && !replace) {
        // Stata's own wording (getmata_exists, log :1070), with the rc stated
        // explicitly -- coarseRc keys r(110) off "already defined", not this.
        return aerErrorRc("variable " + v +
                              " already exists; either specify a different variable name or "
                              "specify getmata's replace option",
                          110);
      }
      if (exists && ds->dtype(v) == "factor") {
        return aerErrorRc("type mismatch: existing variable " + v + " is string and matrix " +
                              el.mat + " is real",
                          109);
      }
      el.exists.push_back(exists);
    }
    els.push_back(std::move(el));
  }

  {
    std::vector<std::string> all;
    for (const auto& el : els) all.insert(all.end(), el.vars.begin(), el.vars.end());
    std::vector<std::string> sorted = all;
    std::sort(sorted.begin(), sorted.end());
    for (size_t i = 1; i < sorted.size(); ++i) {
      if (sorted[i] == sorted[i - 1]) {
        return aerErrorRc("getmata: variable " + sorted[i] +
                              " multiply defined (it is named more than once in the getlist)",
                          198);
      }
    }
  }

  size_t minRows = std::numeric_limits<size_t>::max(), maxRows = 0;
  for (const auto& el : els) {
    minRows = std::min(minRows, el.value.data.size());
    maxRows = std::max(maxRows, el.value.data.size());
  }
  if (!force && (minRows != static_cast<size_t>(curRows) ||
                 maxRows != static_cast<size_t>(curRows))) {
    // getmata_rows_ne (log :1099-1107): r(459), and _N is left alone.
    const bool one = els.size() == 1;
    std::string msg = std::string(one ? "rows of matrix != _N; the matrix contains "
                                      : "rows of matrices != _N; the matrices contain ");
    msg += minRows == maxRows ? std::to_string(minRows) + (minRows == 1 ? " row" : " rows")
                              : "between " + std::to_string(minRows) + " and " +
                                    std::to_string(maxRows) + " rows";
    msg += " and the data have " + std::to_string(curRows) +
           (curRows == 1 ? " observation" : " observations") +
           ". Specify option force to align with the first observation and to extend the "
           "data if necessary.";
    return aerErrorRc(msg, 459);
  }

  // ---- write ---------------------------------------------------------------
  // `force` with more rows than _N EXTENDS the dataset (getmata_force_more,
  // getmata_nodata), through the same runSetObs path svmat uses; with fewer
  // rows the tail is missing, never 0 (getmata_force_fewer).
  const int64_t nOut = std::max<int64_t>(curRows, static_cast<int64_t>(maxRows));
  const bool extended = nOut > curRows || !session.hasData();
  if (extended) {
    ojson setObs = runSetObs(std::to_string(nOut), session);
    if (isErrorBlock(setObs)) return setObs;
  }
  if (!session.currentHbin().has_value()) return ojson(eval::noDataLoaded());

  size_t written = 0, total = 0;
  for (const auto& el : els) total += el.vars.size();
  auto writeFailure = [&](const std::string& what) {
    std::string msg = "getmata: " + what;
    if (written > 0) {
      msg += "; " + std::to_string(written) + " of " + std::to_string(total) +
             " variables were already written and are still in the dataset";
    } else if (extended) {
      msg += "; the dataset was already extended to " + std::to_string(nOut) + " observations";
    }
    return aerError(msg);
  };

  double kNew = 0, kExisting = 0;
  std::vector<std::string> touched;
  for (const auto& el : els) {
    const size_t rows = el.value.data.size();
    for (size_t j = 0; j < el.vars.size(); ++j) {
      std::vector<double> vals;
      vals.reserve(static_cast<size_t>(nOut));
      for (int64_t i = 0; i < nOut; ++i) {
        vals.push_back(static_cast<size_t>(i) < rows
                           ? el.value.data[static_cast<size_t>(i)][j]
                           : std::numeric_limits<double>::quiet_NaN());
      }
      const std::string original = el.exists[j]
                                       ? storage::resolveReplaceType(session, el.vars[j])
                                       : std::string();
      const std::string stored = rows == 0
                                     ? original.empty() ? (useDouble ? "double" : "float") : original
                                     : getmataStoredType(vals, useDouble, original);
      const std::string csvPath = mintCsvPath(session.workdir(), "getmata_vals");
      if (!writeValuesCsv(csvPath, vals)) {
        return writeFailure("could not write the values for " + el.vars[j]);
      }
      const std::string outPath = mintScratchPath(session.workdir(), "getmata");
      std::vector<std::string> args = {"setcol", "--data", *session.currentHbin(), "--var",
                                       el.vars[j], "--values", csvPath, "--dtype", "f64"};
      storage::appendStoreTypeArg(args, stored);
      // An existing variable is REPLACED in place (getmata.ado stores missing
      // over the whole column first, then the vector's rows -- which is what a
      // full-length column with a missing tail is).
      if (!el.exists[j]) args.push_back("--new");
      args.push_back("--out");
      args.push_back(outPath);
      const CobraRunResult r = runCobra(args);
      if (!r.ok) return writeFailure(cobraFailure(r));
      if (!cecon::native_exists(outPath)) {
        return writeFailure("failed writing output dataset (disk full or I/O error)");
      }
      session.load(outPath);
      storage::recordCreateType(session, el.vars[j], stored, stored);
      ++written;
      touched.push_back(el.vars[j]);
      if (el.exists[j]) kExisting += 1;
      else kNew += 1;
    }
  }

  // getmata is r-class: r() is replaced by exactly these two. eval::replaceRClass
  // is the one replacement shape: it leaves the reserved \x01 keys (s()) alone,
  // where clearing the whole rStrings dict destroyed them.
  eval::replaceRClass(session.eval);
  session.eval.rValues.set("K_new", kNew);
  session.eval.rValues.set("K_existing", kExisting);
  return mutationResult(session, "gen", touched);
}

// ---------------------------------------------------------------------------
// mkmat

// The if/in filter that keeps each row's position in the working frame
// (inrange_filter.cpp; declared at its callers, as in handlers.cpp).
InRangeFilterResult applyIfAndInFilterKeepingIndex(SessionState& session,
                                                   const std::string& basePath,
                                                   const std::optional<std::string>& ifExpr,
                                                   const std::optional<std::string>& inRange,
                                                   std::string* indexVar);

namespace {

// A row name derived from a variable's VALUE. Stata's mkmat rownames() names
// each row after that observation's value, so an integer must not print as
// "1.000000" -- an integer-valued double is emitted with no decimal point at
// all. PIET stores a genuine STRING variable as a factor column, so a factor's
// level string IS the value here; there is no value-label lookup, because
// rownames() takes the value and a value-labelled numeric's value is its number.
std::string rowNameFromValue(const Datastore& ds, const std::string& var, int64_t row) {
  const cecon::Column& c = ds.col(var);
  if (ds.dtype(var) == "factor") {
    const int32_t code = c.codes()[row];
    if (code < 0 || static_cast<size_t>(code) >= c.levels.size()) return ".";
    return c.levels[static_cast<size_t>(code)];
  }
  const double v = c.f64()[row];
  if (std::isnan(v)) return ".";
  const double rounded = std::round(v);
  if (v == rounded && std::fabs(rounded) < 1e15) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%lld", static_cast<long long>(rounded));
    return std::string(buf);
  }
  char buf[64];
  std::snprintf(buf, sizeof(buf), "%.10g", v);
  return std::string(buf);
}

}  // namespace

ojson runMkmat(const eval::ParsedCommand& parsed, SessionState& session) {
  if (auto err = eval::rejectUnknownOptions(parsed.opts, kMkmatVocab.known,
                                            kMkmatVocab.unimplemented)) {
    return ojson(*err);
  }
  if (parsed.varlist.empty()) return aerError("mkmat: no variables specified");
  if (!session.currentHbin().has_value()) return ojson(eval::noDataLoaded());

  std::string spec;
  for (size_t i = 0; i < parsed.varlist.size(); ++i) {
    if (i) spec += " ";
    spec += parsed.varlist[i];
  }
  loops::VarlistExpandResult ex = loops::expandVarlist(spec, session);
  if (ex.isError) return ojson(ex.error);
  const std::vector<std::string> vars = ex.tokens;
  if (vars.empty()) return aerError("mkmat: no variables specified");

  std::string matName;
  std::string rowVar;
  std::string rowPrefix = "r";
  bool nomissing = false;
  bool useObsNumbers = false;
  for (const auto& kv : parsed.opts) {
    const std::string& k = kv.first;
    const eval::OptValue& v = kv.second;
    if (k == "matrix" || k == "mat") {
      if (v.isBool || v.strValue.empty()) return aerError("mkmat: option matrix() requires a name");
      matName = strutil::trim(v.strValue);
    } else if (k == "rownames") {
      if (v.isBool || v.strValue.empty()) {
        return aerError("mkmat: option rownames() requires a variable name");
      }
      rowVar = strutil::trim(v.strValue);
    } else if (k == "rowprefix") {
      if (v.isBool || v.strValue.empty()) {
        return aerError("mkmat: option rowprefix() requires a string");
      }
      rowPrefix = strutil::trim(v.strValue);
    } else if (k == "nomissing") {
      nomissing = true;
    } else if (k == "obs") {
      useObsNumbers = true;
    }
  }
  // StataBE 19.5: `mkmat x y, matrix(M) obs rownames(v)` is r(198), with or
  // without if/in -- a row takes one name.
  if (useObsNumbers && !rowVar.empty())
    return aerErrorRc("options obs and rownames() are exclusive", 198);
  if (!matName.empty() && !strutil::isValidStataIdentifier(matName)) {
    return aerError("mkmat: `" + matName + "' is not a valid matrix name. " +
                        strutil::stataIdentifierRuleText(),
                    "syntax_error");
  }

  const std::string basePath = *session.currentHbin();
  InRangeFilterResult fr;
  // `obs` names a row after the observation it came from, so a filtered copy
  // keeps each row's position in the data (indexVar names that column).
  std::string indexVar;
  try {
    fr = useObsNumbers ? applyIfAndInFilterKeepingIndex(session, basePath, parsed.ifExpr,
                                                        parsed.inRange, &indexVar)
                       : applyIfAndInFilter(session, basePath, parsed.ifExpr, parsed.inRange);
  } catch (const CobraError& ce) {
    return aerError("mkmat: " + cobraErrorMessage(ce), ce.code);
  }
  if (!fr.ok) {
    return inRangeRejectError(fr, parsed.inRange.value_or(std::string()), "mkmat");
  }

  // applyIfAndInFilter hands back basePath itself when neither qualifier is
  // present, and a fresh scratch .hbin otherwise. Reading through
  // session.datastore() in the first case reuses the already-open mapping; the
  // second needs its own, because the session head is NOT the filtered file and
  // mkmat must never move it (this is a read-only command).
  std::unique_ptr<Datastore> filtered;
  Datastore* ds = nullptr;
  if (fr.dataPath == basePath) {
    ds = session.datastore();
  } else {
    try {
      filtered = std::make_unique<Datastore>(fr.dataPath);
    } catch (const std::exception& e) {
      return aerError(std::string("mkmat: could not read the filtered data: ") + e.what());
    }
    ds = filtered.get();
  }
  if (ds == nullptr) return ojson(eval::noDataLoaded());

  for (const auto& v : vars) {
    if (!ds->has(v)) {
      // coarseRc maps "variable"+"not found" to r(111); the wording is
      // load-bearing.
      return aerError("mkmat: variable " + v + " not found");
    }
    if (ds->dtype(v) != "f64") {
      // Stata refuses these too: a matrix holds numbers, and PIET's `factor`
      // dtype is how a genuine string variable is stored. Stata answers r(109).
      return aerErrorRc("mkmat: " + v + " is a string variable; mkmat needs numeric variables",
                        109);
    }
  }
  if (!rowVar.empty() && !ds->has(rowVar)) {
    return aerError("mkmat: variable " + rowVar + " not found");
  }

  const int64_t nRows = ds->nRows();
  std::vector<const double*> cols;
  cols.reserve(vars.size());
  for (const auto& v : vars) {
    const cecon::Column& c = ds->col(v);
    const double* p = c.f64();
    if (p == nullptr || c.n_values() != nRows) {
      return aerError("mkmat: could not read " + v + " as a full-length numeric column");
    }
    cols.push_back(p);
  }
  const double* obsIndex = nullptr;
  if (!indexVar.empty()) {
    if (!ds->has(indexVar) || ds->col(indexVar).f64() == nullptr)
      return aerError("mkmat: could not read the observation numbers of the filtered data");
    obsIndex = ds->col(indexVar).f64();
  }

  std::vector<std::vector<double>> rows;
  std::vector<std::string> rowNames;
  for (int64_t i = 0; i < nRows; ++i) {
    std::vector<double> row;
    row.reserve(cols.size());
    bool anyMissing = false;
    for (const double* p : cols) {
      const double v = p[i];
      if (std::isnan(v)) anyMissing = true;
      row.push_back(v);
    }
    // LISTWISE, not per-cell: `nomissing` drops the OBSERVATION when any
    // selected variable is missing there, which is what keeps the k columns of
    // the result aligned to one another. Without the option a missing value is
    // carried into the matrix as missing rather than silently dropped.
    if (nomissing && anyMissing) continue;
    rows.push_back(std::move(row));
    // The row NUMBER a default name is built from is the position in the
    // matrix: `r3` is its third row. `obs` names the row after its observation
    // instead, prefix kept (BUG-403; StataBE 19.5, 2026-11-03 oracle: rows from
    // observations 1 and 3 are r1 r3, `if y > 3` gives r4 r5 r6, and
    // rowprefix(p) p3 p4 ...).
    const size_t k = rows.size();
    if (!rowVar.empty()) {
      rowNames.push_back(rowNameFromValue(*ds, rowVar, i));
    } else if (useObsNumbers) {
      const int64_t obs =
          obsIndex != nullptr ? static_cast<int64_t>(std::llround(obsIndex[i])) : i + 1;
      rowNames.push_back(rowPrefix + std::to_string(obs));
    } else {
      rowNames.push_back(rowPrefix + std::to_string(k));
    }
  }

  if (rows.empty()) {
    // Stata's r(2000). coarseRc has no message rule for this phrase, so the rc
    // is stated explicitly.
    return aerErrorRc("mkmat: no observations", 2000);
  }

  if (!matName.empty()) {
    eval::EvalMatrix m;
    m.data = std::move(rows);
    m.rownames = rowNames;
    m.colnames = vars;
    storeMatrix(session, matName, std::move(m));
  } else {
    // No matrix(): one N x 1 matrix per variable, each named after the variable
    // it came from -- [P] mkmat's documented default.
    for (size_t j = 0; j < vars.size(); ++j) {
      eval::EvalMatrix m;
      m.data.reserve(rows.size());
      for (const auto& row : rows) m.data.push_back({row[j]});
      m.rownames = rowNames;
      m.colnames = {vars[j]};
      storeMatrix(session, vars[j], std::move(m));
    }
  }

  // EMPTY array = silent success, which is what Stata's mkmat is. A non-empty
  // array here would be defect class 1 (see this file's header): the dispatcher
  // arm pushes this value as ONE block without flattening it.
  return ojson::array();
}

// ---------------------------------------------------------------------------
// matlist

namespace {

// The display layer's own spec grammar (stata_format.h documents why each layer
// keeps its own regex and shares only the renderer): a FULL match, so `%td`,
// `%9s` and anything else non-numeric is refused by name instead of being
// handed to a numeric renderer that would answer something plausible for it.
bool parseNumericFmt(const std::string& raw, fmt::StataFmtSpec* sp) {
  static const std::regex re(R"(^%(-)?(0)?([0-9]+)?(?:\.([0-9]+))?([fgeFGE])$)");
  std::smatch m;
  if (!std::regex_match(raw, m, re)) return false;
  // A four-digit cap before std::stoi, not after: stoi THROWS on
  // `format(%99999999999.2f)`, and that exception has no catcher between here
  // and the dispatcher, so an unbounded width is a crash from user input rather
  // than a diagnostic. No Stata display format is wider than four digits.
  if (m[3].str().size() > 4 || m[4].str().size() > 4) return false;
  sp->leftJustify = m[1].matched;
  sp->zeroFill = m[2].matched;
  sp->hasWidth = m[3].matched;
  sp->width = m[3].matched ? std::stoi(m[3].str()) : 0;
  sp->prec = m[4].matched ? std::stoi(m[4].str()) : 0;
  sp->conv = static_cast<char>(std::tolower(static_cast<unsigned char>(m[5].str()[0])));
  return true;
}

std::string renderCell(double v, const fmt::StataFmtSpec& sp, size_t width) {
  std::string s;
  if (auto miss = eval::stataMissingToken(v)) {
    // Missing is its token at every width (`.`, or `.a` for a matrix cell that
    // holds one, BUG-1399) -- renderValue takes a finite double and has no
    // missing concept, so the check cannot be pushed down into it.
    s = *miss;
  } else if (std::optional<std::string> r = fmt::renderValue(v, sp)) {
    s = *r;
  } else {
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%.9g", v);
    s = buf;
  }
  // padToField rather than a bare right-justify: right-justified IS its default,
  // so the ordinary `%9.0g` column comes out exactly as specified, and the one
  // documented exception (the `-` flag) is honoured instead of dropped. The net
  // ACCEPTS `format(%-9.2f)`, so ignoring the flag would be a silently different
  // table. A widthless spec has nothing for it to pad to, hence the column's own
  // width there.
  return sp.hasWidth ? fmt::padToField(s, sp) : rjust(s, width);
}

}  // namespace

ojson runMatlist(const eval::ParsedCommand& parsed, SessionState& session) {
  if (auto err = eval::rejectUnknownOptions(parsed.opts, kMatlistVocab.known,
                                            kMatlistVocab.unimplemented)) {
    return ojson(*err);
  }

  // EXP comes off the RAW line, not off parsed.varlist: the expression may be
  // `e(b)`, whose parentheses the varlist tokenizer has no reason to keep in one
  // piece, and re-joining split tokens would be a guess about how they were
  // split. findUnquotedComma is the same option-boundary scan the tokenizer
  // itself uses, so `format(%9.3f)`'s inner text can never be mistaken for the
  // boundary.
  std::string rest = strutil::trim(parsed.raw);
  const size_t sp0 = rest.find_first_of(" \t");
  rest = (sp0 == std::string::npos) ? std::string() : strutil::trim(rest.substr(sp0));
  const long long comma = eval::findUnquotedComma(rest);
  if (comma >= 0) rest = rest.substr(0, static_cast<size_t>(comma));
  const std::string expr = strutil::trim(rest);
  if (expr.empty()) return aerError("matlist: no matrix specified");

  eval::EvalMatrix owned;
  const eval::EvalMatrix* mat = nullptr;
  // e(<name>) / r(<name>), not just e(b)/e(V). The old spelling was `([bV])`,
  // which was right while b and V were the only e-matrices anything posted;
  // pca posts seven (Ev, L, C, Psi, means, sds, components) and sktest posts
  // r(table), and under the old regex every one of them was unreachable -- the
  // command would publish a matrix that no syntax could name.
  // Parsed by hand, not a regex: the name may be Unicode (bytes >= 0x80 are
  // identifier bytes, core/ident.h), which a narrow-char bracket range cannot
  // express portably.
  char cls = 0;
  std::string name;
  {
    size_t p = 0;
    if (expr.size() > 2 && (expr[0] == 'e' || expr[0] == 'r') && expr[1] == '(') {
      p = 2;
      while (p < expr.size() && std::isspace(static_cast<unsigned char>(expr[p]))) ++p;
      const size_t b = p;
      if (p < expr.size() && cecon::ident::isIdentStartByte(expr[p])) {
        while (p < expr.size() && cecon::ident::isIdentByte(expr[p])) ++p;
        name = expr.substr(b, p - b);
        while (p < expr.size() && std::isspace(static_cast<unsigned char>(expr[p]))) ++p;
        if (p + 1 == expr.size() && expr[p] == ')') cls = expr[0];
        else name.clear();
      }
    }
  }
  if (cls != 0) {
    if (cls == 'e') {
      // eb-order T-P1: the one resolver `matrix N = e(NAME)` uses
      // (EvalSession::resolveEMatrix), so `matlist e(b)` and the copied matrix
      // cannot disagree. Its second copy of the coeftable synthesis (with a
      // diagonal e(V)) is gone. A refusal carries the resolver's rc: r(301)
      // with no e() at all (w2g_ebmat.log:2238 e15c_fresh_matlist_eb), r(111)
      // for a name the live e() does not have -- never the coarse rc 1.
      int rc = 111;
      std::string why;
      std::optional<eval::EvalMatrix> resolved = session.eval.resolveEMatrix(name, &rc, &why);
      // A live e() with no e(b)/e(V): rc 0, nothing to list (w2g_ebmat.log:2096
      // e15c_matlist_eb after pca; w2v_estpost_ematrix.log ep13_matlist_eb).
      if (!resolved.has_value() && session.eval.eMatrixAbsentFromLiveE(name))
        resolved = eval::EvalMatrix{};
      if (!resolved.has_value()) return aerErrorRc("matlist: " + why, rc);
      owned = std::move(*resolved);
      mat = &owned;
    } else if (const eval::EvalMatrix* posted = session.eval.findClassMatrix(cls, name)) {
      mat = posted;
    } else {
      return aerErrorRc("matlist: matrix " + expr + " not found", 111);
    }
  } else {
    mat = session.eval.findMatrix(expr);
    if (mat == nullptr) return aerErrorRc("matlist: matrix " + expr + " not found", 111);
  }

  std::string fmtRaw = "%9.0g";
  std::string title;
  std::string namesArg = "all";
  bool nonames = false;
  bool noblank = false;
  int twidth = 12;
  for (const auto& kv : parsed.opts) {
    const std::string& k = kv.first;
    const eval::OptValue& v = kv.second;
    if (k == "format") {
      if (v.isBool || v.strValue.empty()) return aerError("matlist: option format() requires a %fmt");
      fmtRaw = strutil::trim(v.strValue);
    } else if (k == "title") {
      if (v.isBool) return aerError("matlist: option title() requires a string");
      title = strutil::trim(v.strValue);
      if (title.size() >= 2 && title.front() == '"' && title.back() == '"') {
        title = title.substr(1, title.size() - 2);
      }
    } else if (k == "names") {
      if (v.isBool || v.strValue.empty()) {
        return aerError("matlist: option names() requires all, rows, columns or none");
      }
      namesArg = strutil::lower(strutil::trim(v.strValue));
    } else if (k == "nonames") {
      nonames = true;
    } else if (k == "noblank") {
      noblank = true;
    } else if (k == "twidth") {
      if (v.isBool) return aerError("matlist: option twidth() requires a number");
      try {
        twidth = std::stoi(strutil::trim(v.strValue));
      } catch (const std::exception&) {
        return aerError("matlist: option twidth() requires a number");
      }
      if (twidth < 1 || twidth > 200) {
        return aerError("matlist: twidth(" + strutil::trim(v.strValue) +
                        ") is out of range; twidth() must be between 1 and 200");
      }
    }
  }

  if (namesArg != "all" && namesArg != "rows" && namesArg != "columns" && namesArg != "none") {
    return aerError("matlist: names(" + namesArg + ") is not one of all, rows, columns, none",
                    "syntax_error");
  }
  fmt::StataFmtSpec spec;
  if (!parseNumericFmt(fmtRaw, &spec)) {
    // A `%td`/`%9s` here is not a rendering nicety: matlist renders NUMBERS, and
    // silently falling back to the default would print a different table from
    // the one that was asked for, with no sign that the format was dropped.
    return aerError("matlist: format(" + fmtRaw +
                        ") is not a numeric display format (%w.Pf, %w.Pg or %w.Pe)",
                    "syntax_error");
  }
  const size_t cellWidth = spec.hasWidth && spec.width > 0 ? static_cast<size_t>(spec.width) : 9;

  const bool showRows = !nonames && (namesArg == "all" || namesArg == "rows");
  const bool showCols = !nonames && (namesArg == "all" || namesArg == "columns");

  const size_t nrows = mat->data.size();
  const size_t ncols = nrows > 0 ? mat->data[0].size() : 0;
  // A 0x0 matrix (`matrix b = e(b)` after estpost/pca, or e(b) itself there) lists
  // nothing at rc 0 (w2g_ebmat.log:2096 e15c_matlist_eb; w2v_estpost_ematrix.log
  // ep13_matlist_eb, ep15c_matlist_eb) -- it used to be an rc-1 error.
  if (nrows == 0 || ncols == 0) {
    ojson empty;
    empty["type"] = "echo";
    empty["text"] = "";
    return empty;
  }

  // StataBE 19.5's default layout (BUG-1412; 2026-11-03 oracle, line for line):
  //
  //                | e1                   | e2
  //                |         x          y |         z
  //   -------------+----------------------+----------
  //   q            |                      |
  //              a |         1          2 |         3
  //   -------------+----------------------+----------
  //   w            |                      |
  //              b |         4          5 |         6
  //
  // - A column is one blank, the cell (the format's width w, 9 for %9.0g), one
  //   blank; the first has no leading blank without row names. The row-name
  //   field is twidth() wide plus one blank, then `|`.
  // - A column-equation change puts a `|` before the column, on every line.
  //   With any equation, a header line carries each group's equation once,
  //   left-aligned in the first column's w; with any row equation, each run
  //   of rows with one equation opens with a line holding its equation (blank
  //   for none) and a separator line comes before every run but the first.
  // - A factor/operator name puts its `op.` (or the part up to the last `#`)
  //   on a line above, right-aligned to end one place past the cell, `bn`
  //   dropped. Names are cut as abbrev() cuts them, to w (columns, equations)
  //   or twidth (rows).
  // - Separator lines and the blank stretch of an equation line stop one
  //   place short of the last column's right edge.
  // - A symmetric matrix prints its lower triangle.
  const auto abbrev = [](const std::string& s, size_t n) {
    if (s.size() <= n) return s;
    return n > 4 ? s.substr(0, n - 2) + "~" + s.back() : s.substr(0, n);
  };
  const auto splitEq = [](std::string full, std::string* eq) {
    const size_t colon = full.find(':');
    eq->clear();
    if (colon != std::string::npos) {
      *eq = full.substr(0, colon);
      full = full.substr(colon + 1);
    }
    if (*eq == "_") eq->clear();  // Stata's name for "no equation" among named ones
    return full;
  };
  const size_t tw = static_cast<size_t>(twidth);
  const size_t w = cellWidth;
  std::vector<std::string> req(nrows), rname(nrows), ceq(ncols), cname(ncols), cop(ncols);
  bool anyRowEq = false, anyColEq = false, anyOp = false;
  for (size_t i = 0; i < nrows; ++i) {
    std::string full = i < mat->rownames.size() ? matrixNameAsListed(mat->rownames[i]) : "";
    if (full.empty()) full = "r" + std::to_string(i + 1);
    rname[i] = abbrev(splitEq(full, &req[i]), tw);
    anyRowEq = anyRowEq || !req[i].empty();
  }
  for (size_t j = 0; j < ncols; ++j) {
    std::string full = j < mat->colnames.size() ? matrixNameAsListed(mat->colnames[j]) : "";
    if (full.empty()) full = "c" + std::to_string(j + 1);
    std::string name = splitEq(full, &ceq[j]);
    anyColEq = anyColEq || !ceq[j].empty();
    size_t cut = name.rfind('#');
    if (cut == std::string::npos) cut = name.rfind('.');
    if (cut != std::string::npos && cut > 0 && cut + 1 < name.size()) {
      cop[j] = abbrev(name.substr(0, cut), w) + name[cut];
      name = name.substr(cut + 1);
      anyOp = true;
    }
    cname[j] = abbrev(name, w);
  }
  bool lowerOnly = nrows == ncols;
  for (size_t i = 0; i < nrows && lowerOnly; ++i) {
    for (size_t j = 0; j < i && lowerOnly; ++j) {
      const double a = mat->data[i][j], b = mat->data[j][i];
      lowerOnly = a == b || (std::isnan(a) && std::isnan(b));
    }
  }

  const auto lead = [&](size_t j) -> size_t { return j == 0 && !showRows ? 0 : 1; };
  const auto full = [&](size_t j) { return lead(j) + w + 1; };
  const auto fill = [&](size_t j) { return full(j) - (j + 1 == ncols ? 1 : 0); };
  const auto bar = [&](size_t j) {
    return j + 1 < ncols && anyColEq && ceq[j + 1] != ceq[j] ? "|" : "";
  };
  const auto ljust = [](const std::string& s, size_t width) {
    return s.size() >= width ? s : s + std::string(width - s.size(), ' ');
  };
  const std::string headLead = showRows ? std::string(tw + 1, ' ') + "|" : std::string();
  std::ostringstream out;
  const auto separator = [&] {
    if (showRows) out << std::string(tw + 1, '-') << "+";
    for (size_t j = 0; j < ncols; ++j) out << std::string(fill(j), '-') << (*bar(j) ? "+" : "");
    out << "\n";
  };

  if (!noblank) out << "\n";
  if (!title.empty()) out << title << "\n\n";
  if (showCols && anyColEq) {
    out << headLead;
    for (size_t j = 0; j < ncols; ++j) {
      if (j == 0 || ceq[j] != ceq[j - 1])
        out << std::string(lead(j), ' ') << ljust(abbrev(ceq[j], w), w) << ' ';
      else
        out << std::string(fill(j), ' ');
      out << bar(j);
    }
    out << "\n";
  }
  if (showCols && anyOp) {
    out << headLead;
    for (size_t j = 0; j < ncols; ++j) {
      if (cop[j].empty()) out << std::string(full(j), ' ');
      else out << std::string(lead(j), ' ') << rjust(cop[j], w + 1);
      out << bar(j);
    }
    out << "\n";
  }
  if (showCols) {
    out << headLead;
    for (size_t j = 0; j < ncols; ++j)
      out << std::string(lead(j), ' ') << rjust(cname[j], w) << ' ' << bar(j);
    out << "\n";
  }
  for (size_t i = 0; i < nrows; ++i) {
    const bool newGroup = anyRowEq && (i == 0 || req[i] != req[i - 1]);
    if (i == 0 ? showCols : newGroup) separator();
    if (newGroup && showRows) {
      out << ljust(abbrev(req[i], tw), tw) << " |";
      for (size_t j = 0; j < ncols; ++j) out << std::string(fill(j), ' ') << bar(j);
      out << "\n";
    }
    if (showRows) out << rjust(rname[i], tw) << " |";
    for (size_t j = 0; j < ncols; ++j) {
      if (lowerOnly && j > i) {
        out << std::string(full(j), ' ');
      } else {
        const double v = j < mat->data[i].size() ? mat->data[i][j]
                                                 : std::numeric_limits<double>::quiet_NaN();
        out << std::string(lead(j), ' ') << renderCell(v, spec, w) << ' ';
      }
      out << bar(j);
    }
    out << "\n";
  }

  ojson block;
  block["type"] = "display";  // output, not the command echo (BUG-1505, runReturnList)
  block["text"] = out.str();
  return block;
}

}  // namespace piet::handlers
