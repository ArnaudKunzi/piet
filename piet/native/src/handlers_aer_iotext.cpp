// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// AER-SCOPE1 bundle G (IO-TEXT, 2026-09-04) -- the older text I/O verbs, plus
// `putexcel`, plus the three file commands that had no home:
//
//   insheet     a delimited file -> the dataset, with Stata's own naming rules
//   outsheet    the dataset -> a delimited file, TAB by default
//   infile      free-format tokens -> the dataset (records wrap on varlist size)
//   infix       fixed columns -> the dataset
//   outfile     the dataset -> a headerless, space-or-comma separated file
//   saveold     `save`, with an honest note about the format actually written
//   putexcel    a cell-addressed .xlsx writer, stateful across statements
//   filefilter  a whole-file search/replace, publishing r(occurrences)
//   copy        one file to another (a URL source is refused by name)
//
// WHY THESE ARE SHIMS AND NOT NEW READERS. `insheet` IS `import delimited`
// once the delimiter and the header question are settled, and `outsheet` IS
// `export delimited` with a different default separator; `saveold` IS `save`.
// A second delimited reader in this build would be a second thing to keep in
// step with the engine's own CSV path, and the first divergence between them
// would be invisible -- two commands answering the same question with
// different numbers. So the loaders here decide the OPTIONS and hand the bytes
// to the machinery that already exists (`cobra convert`, runExportDelimited,
// runSaveData). `infile`, `infix` and `outfile` are the exceptions: their
// formats are not delimited files at all, so they parse/emit here and stage a
// CSV for the SAME converter, which is what keeps the type inference identical
// to every other load in the product.
//
// NO NEW ENGINE VERB, and no `src/` edit: everything below composes
// `convert`/`rename` and the PIET-side writers.
//
// THE FIVE DEFECT CLASSES THIS FILE IS WRITTEN AGAINST. Each has shipped at
// least once in this campaign; the first has been caught three times in five
// bundles, so it is named first.
//
//  1. SHAPE. A handler returning a JSON ARRAY where the arm pushes ONE block:
//     nlohmann throws type_error.306 the moment a consumer reads ["type"], and
//     that aborts the whole selftest binary. dispatcher.cpp's bundle-G arms
//     route through `pushEnvResult`, which flattens an array and drops nulls,
//     so an array IS legal -- but ONLY an array whose every element is a real
//     block object carrying a "type" key. Every `return` below is one object
//     with "type", or an ojson::array() of such objects (possibly empty, which
//     is how `putexcel clear` and `copy` say "nothing to print").
//  2. RC. `eval::coarseRc` answers 199 only for `unsupported_command` or an
//     explicit `r(NNN)` reason, and 198 for `syntax_error`; a refusal built
//     with the default `engine_error` code silently becomes rc 1. So:
//     command-level refusals carry `unsupported_command` + a `refused:<cause>`
//     reason (eval::refusedCommand, or the same shape by hand when the plan
//     pins the sentence), USER SYNTAX errors carry "syntax_error", and a known
//     Stata rc that coarseRc's message rules cannot recognise states itself
//     through the `reason` channel (`ioErrorRc` below). `copy` onto an
//     existing file without `replace` is r(602) and says so.
//  3. VOCABULARY. `rejectUnknownOptions` prefix-resolves and therefore ACCEPTS
//     abbreviations, while `canonicalizeForCommand` -- the thing that makes a
//     handler's own `k == "fullname"` comparison see them -- is driven by the
//     REGISTRY. A command absent from the vocabulary registry gets its
//     abbreviations accepted and then dropped unread. All nine names register
//     at NAMESPACE scope below (a function-local static constructs too late:
//     after the first call that needed it), and every handler's first act is
//     the reject net. An option this build does NOT read goes in the
//     `unimplemented` list, never in `known` -- bundle E shipped
//     `esttab, cells()` as known-and-unread and it had to be reverted.
//     `putexcel`'s FORMATTING options are the one deliberate exception the
//     plan specifies: accepted, ignored, and announced once per `set`.
//  4. ECHOED PATHS. Nothing here echoes a RESOLVED absolute path. Every
//     message and every echo names the spelling the user typed, because a
//     resolved path bakes the build machine's workdir into any golden that
//     captures it -- which has shipped once already. The shared delimited
//     writer names DelimWriteSpec::shown, and runSaveData names its own
//     spelling, so `saveold` passes its refusals through untouched.
//  5. R-CLASS. `filefilter` publishes r(occurrences) from the handler; its arm
//     has no table block to capture from.
//
// STATE THAT MUST NOT OUTLIVE WHAT IT DESCRIBES. `putexcel`'s open workbook
// lives in `SessionState::putexcel` (session_state.h). `clear all` drops the
// other session objects (estimates, the estadd bag, graphs, postfiles, file
// handles, frames) in `SessionState::clearAllSessionObjects`, and putexcel
// belongs in that list for the same reason -- but session_state.cpp is not in
// this bundle's write manifest, so the line is REPORTED, not added here.
#include "piet/handlers.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <fstream>
#include <limits>
#include <map>
#include <optional>
#include <regex>
#include <set>
#include <sstream>
#include <string>
#include <system_error>
#include <unordered_set>
#include <utility>
#include <vector>

#include "data/missing.h"  // cecon::missing_kind -- insheet's float columns carry `.` only
#include "io/win_compat.h"  // cecon::to_native_path / native_exists / native_unlink
#include "piet/cobra_runner.h"
#include "piet/datastore.h"
#include "piet/delimited_io.h"  // insheet: the one native delimited reader
#include "piet/display_hardening.h"  // evalScalarExpr -- `putexcel A1 = (expr)`
#include "piet/eval/errors.h"
#include "piet/eval/exprfns.h"  // applyStataFmt -- outfile writes through display formats
#include "piet/eval/pyvalue.h"  // pythonFloatRepr -- round-trippable numerics
#include "piet/eval/session.h"
#include "piet/eval/tokenizer.h"
#include "piet/inrange_filter.h"  // infix `in`: parseInRangeBounds
#include "piet/mutation_result.h"
#include "piet/scratch_seq.h"
#include "piet/session_state.h"
#include "piet/stata_format.h"  // piet::fmt::general -- outfile's %10.0g
#include "piet/strutil.h"
#include "xlsx.h"

// ---------------------------------------------------------------------------
// OPTION VOCABULARIES -- namespace scope, before any dispatch (defect class 3).
//
// `known` is the plan's "corpus options (implemented)" column; `unimplemented`
// is its "refused options" column, WIDENED wherever an option the plan called
// "ignored" would actually change the result. The split is the whole point: a
// name in NEITHER set reads as "unknown option", which is the wrong diagnosis
// for a real Stata option this build simply does not do, and a name wrongly
// placed in `known` is accepted and silently ignored, which is the defect the
// apparatus exists to prevent.
namespace {

// `double` asks for double storage: it keeps the numbers as read, where the
// default stores a non-integer column as float (delimited::typeColumn).
// Under `set type double` the default is double, and `nodouble` asks for the
// float columns (insheet's help). `clear` is read in
// the only sense it has here: an `insheet` always replaces what is in memory.
// `case` is read (it suppresses the lower-casing pass).
const piet::eval::OptVocab& kVocabInsheet = piet::eval::registerOptVocab(
    {"insheet"}, {{"clear", "names", "nonames", "comma", "tab", "delimiter", "delim", "case",
                   "double", "nodouble"},
                  {}});

// outsheet writes through writeDelimitedFile with its own switches: `nonames`
// drops the header row (w2ar dx_outs_noname, log:893), `noquote` drops the
// quotes it otherwise puts round every string (log:865). `names` IS the
// default and is therefore honoured by construction.
const piet::eval::OptVocab& kVocabOutsheet = piet::eval::registerOptVocab(
    {"outsheet"}, {{"comma", "replace", "noquote", "nolabel", "names", "nonames", "delimiter",
                    "delim"},
                   {"double"}});

// `automatic` asks infile to build value labels from the string values it
// reads, and `byvariable()` changes the record ORDER (all of v1, then all of
// v2). Both change what the dataset IS, so neither may be accepted unread.
// `using(data)` is the dictionary form's data file (runDictionaryRead).
const piet::eval::OptVocab& kVocabInfile =
    piet::eval::registerOptVocab({"infile"}, {{"clear", "using"}, {"automatic", "byvariable"}});

const piet::eval::OptVocab& kVocabInfix =
    piet::eval::registerOptVocab({"infix"}, {{"clear", "using"}, {}});

// `wide` asks for one observation per output line, which is exactly and only
// what this writer does -- honoured by construction. `dictionary` writes a
// companion .dct and `fast` changes the buffering contract; neither is done.
const piet::eval::OptVocab& kVocabOutfile = piet::eval::registerOptVocab(
    {"outfile"}, {{"replace", "noquote", "comma", "nolabel", "wide"}, {"dictionary", "fast"}});

// `nolabel`/`all`/`emptyok` change WHAT IS WRITTEN into the file, so they stay
// in the unimplemented list rather than being accepted and dropped.
const piet::eval::OptVocab& kVocabSaveold = piet::eval::registerOptVocab(
    {"saveold"}, {{"replace", "version"}, {"nolabel", "all", "emptyok", "orphans"}});

// putexcel's vocabulary, in three parts.
//
//   * the ones this handler READS: set's mode/sheet, the matrix `names` flag,
//     the range `merge` flag, and `open`, which is a no-op here because this
//     build has no separate open/close phase (every assignment rewrites the
//     workbook in full).
//   * the FORMATTING options, which are the plan's deliberate exception to the
//     known-means-read rule: they are accepted, ignored, and announced once per
//     `set` with the exact note decision 6 specifies. They do not change any
//     VALUE this build writes -- only how a spreadsheet would paint it -- and
//     the announcement is what keeps that honest.
//   * the ones that WOULD change a value or the document's structure, which are
//     refused by name.
const piet::eval::OptVocab& kVocabPutexcel = piet::eval::registerOptVocab(
    {"putexcel"},
    {{"replace", "modify", "sheet", "names", "merge", "open",
      // formatting -- accepted, ignored, announced (decision 6).
      "nformat", "bold", "italic", "underline", "font", "hcenter", "vcenter", "border",
      "fpattern", "txtwrap", "shrinkfit", "locked", "hidden", "txtindent", "left", "right",
      "top", "bottom"},
     {"formula", "asdate", "asdatetime", "colwise", "rowwise", "returnset", "overwritefmt",
      "dateformat", "keepcellformat"}});

// The formatting subset, named once so the note fires on exactly the options
// the note describes.
const std::set<std::string>& putexcelFormatOpts() {
  static const std::set<std::string> kSet = {
      "nformat", "bold", "italic",  "underline", "font",  "hcenter", "vcenter", "border",
      "fpattern", "txtwrap", "shrinkfit", "locked", "hidden", "txtindent", "left", "right",
      "top", "bottom"};
  return kSet;
}

// `ascii` asks for the byte-oriented interpretation this filter already uses.
// `unicode` asks for a UTF-16 one, which would change which bytes match, so it
// is refused rather than ignored.
const piet::eval::OptVocab& kVocabFilefilter = piet::eval::registerOptVocab(
    {"filefilter"}, {{"from", "to", "replace", "ascii"}, {"unicode"}});

// `text` only ever meant anything for a URL transfer (it selects the transfer
// mode), and a URL source is refused outright below -- so for the local copy
// this command performs it is honoured by construction, as is `asis`, which
// names the byte-for-byte copy this does. `public` changes the destination's
// PERMISSIONS, which this does not do, so it is refused rather than dropped.
const piet::eval::OptVocab& kVocabCopy =
    piet::eval::registerOptVocab({"copy"}, {{"replace", "text", "asis"}, {"public"}});

}  // namespace

namespace piet::handlers {

using ojson = nlohmann::ordered_json;

namespace {

// ===========================================================================
// Errors and blocks
// ===========================================================================

ojson ioError(const std::string& message, const std::string& code = "engine_error") {
  return ojson(eval::engineError(message, code));
}

// A USER SYNTAX error. "syntax_error" is what coarseRc maps to rc 198; the
// default "engine_error" maps to rc 1, which is bundle C's shipped defect.
ojson ioSyntax(const std::string& message) {
  return ojson(eval::engineError(message, "syntax_error"));
}

// An error whose Stata rc is KNOWN but which coarseRc's message rules cannot be
// relied on to recognise. `reason` is the only channel carrying an exact rc
// (BUG-187) and it outranks the coarse mapping.
ojson ioErrorRc(const std::string& message, int rc) {
  return ojson(eval::errorBlock("engine_error", message, std::nullopt, std::nullopt, std::nullopt,
                                "r(" + std::to_string(rc) + ")"));
}

// A refusal whose SENTENCE the plan pins, built in refusedCommand's own shape
// so `testRegistryRoutes` still tells it apart from the generic gap: code
// `unsupported_command` (rc 199), `token` == the command word, and a
// `refused:<cause>` reason rather than `piet_gap`.
ojson ioRefusal(const std::string& cmd, const std::string& cause, const std::string& message) {
  return ojson(eval::errorBlock("unsupported_command", message, cmd, std::nullopt, std::nullopt,
                                "refused:" + cause));
}

ojson displayBlock(const std::string& text) {
  ojson b;
  b["type"] = "display";
  b["text"] = text;
  return b;
}


// "This command printed nothing" -- an EMPTY array, which `pushEnvResult`
// flattens into zero blocks. Never a bare object without a "type" key
// (defect class 1).
ojson silent() { return ojson::array(); }

bool isErrorBlock(const ojson& b) {
  return b.is_object() && b.value("type", std::string()) == "error";
}

// ===========================================================================
// Small text helpers
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

// STRICT-CONSUME numeric parsing (IDIOMS, C1-F11): the WHOLE token must parse,
// and C's hex forms are rejected. A bare std::stod lies about `5foo` (-> 5),
// which is exactly how a mistyped field becomes a silently wrong number.
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

// ===========================================================================
// The pre-comma word list, QUOTE-AWARE
// ===========================================================================
//
// `parsed.varlist` cannot be used for a path: the tokenizer splits the
// pre-comma text on bare whitespace (eval/tokenizer.cpp's splitWs is an
// istringstream loop), so `using "my file.csv"` arrives as three tokens with
// stray quote characters. `scanOneToken` is the shared quote-aware scanner and
// it strips one level of quoting, which is exactly what a filename needs.

// Drop the leading command word (the first bare token; a command word is never
// quoted, so a plain whitespace scan is exact here).
std::string dropCommandWord(const std::string& in) {
  const std::string s = trimOf(in);
  size_t i = 0;
  while (i < s.size() && !std::isspace(static_cast<unsigned char>(s[i]))) ++i;
  return trimOf(s.substr(i));
}

// Everything on `raw` before the first unquoted, unparenthesised comma, with
// the leading command word removed.
std::string preCommaArgs(const std::string& raw) {
  std::string s = raw;
  const long long comma = eval::findUnquotedComma(s);
  if (comma >= 0) s = s.substr(0, static_cast<size_t>(comma));
  return dropCommandWord(s);
}

std::vector<std::string> quoteAwareWords(const std::string& s) {
  std::vector<std::string> out;
  std::string rest = s;
  while (true) {
    auto [tok, remainder] = eval::scanOneToken(rest, true);
    if (tok.empty() && remainder.empty()) break;
    if (!tok.empty()) out.push_back(tok);
    if (remainder == rest) break;  // defensive: never spin on a token-less tail
    rest = remainder;
    if (rest.empty()) break;
  }
  return out;
}

// Split `[varlist] using FILE` (or a bare `FILE`) the way every `using`-taking
// command in this bundle needs it. Returns false when no filename is present.
bool splitUsing(const std::vector<std::string>& words, std::vector<std::string>* varlist,
                std::string* pathArg) {
  varlist->clear();
  pathArg->clear();
  bool sawUsing = false;
  for (const auto& w : words) {
    if (!sawUsing && lowerOf(w) == "using") {
      sawUsing = true;
      continue;
    }
    if (sawUsing)
      *pathArg = w;
    else
      varlist->push_back(w);
  }
  if (!sawUsing) {
    // `insheet "f.csv"` and `saveold f` -- the filename is the LAST bare word.
    if (varlist->empty()) return false;
    *pathArg = varlist->back();
    varlist->pop_back();
  }
  return !pathArg->empty();
}

// Remove an `if`/`in` qualifier's words from a command's pre-comma words:
// each run starts at an `if` or `in` word and ends before `using` (or at the
// end), so both `outsheet using f if g != 0` and `outfile s g if year == 2001
// using f` keep their varlist and file. The qualifier itself reaches the
// handler parsed (parsed.ifExpr / parsed.inRange).
std::vector<std::string> dropQualifierWords(const std::vector<std::string>& words) {
  std::vector<std::string> out;
  bool inQual = false;
  for (const auto& w : words) {
    const std::string lw = lowerOf(w);
    if (lw == "using") inQual = false;
    else if (lw == "if" || lw == "in") inQual = true;
    if (!inQual) out.push_back(w);
  }
  return out;
}

// ===========================================================================
// Files
// ===========================================================================

bool pathExists(const std::string& p) {
  std::error_code ec;
  return std::filesystem::exists(cecon::to_native_path(p), ec) && !ec;
}

// Two paths naming the SAME file. Canonicalized so `./x` and `x` compare equal;
// a path that cannot be canonicalized falls back to a byte compare, which is
// conservative in the safe direction (it may miss an alias, never invent one).
bool sameFile(const std::string& a, const std::string& b) {
  if (a == b) return true;
  std::error_code ea, eb;
  const auto ca = std::filesystem::weakly_canonical(cecon::to_native_path(a), ea);
  const auto cb = std::filesystem::weakly_canonical(cecon::to_native_path(b), eb);
  if (ea || eb) return false;
  return ca == cb;
}

// "Is the destination also a source?" asked of the SESSION: writing a delimited
// file over the .hbin the session is reading from would truncate the dataset
// mid-command and leave `_N` describing bytes that no longer exist.
std::optional<ojson> refuseIfSessionHbin(SessionState& session, const std::string& verb,
                                         const std::string& resolved,
                                         const std::string& spelling) {
  if (!session.currentHbin().has_value()) return std::nullopt;
  if (!sameFile(resolved, *session.currentHbin())) return std::nullopt;
  return ioError(verb + ": " + spelling +
                 " is the file this session's own dataset is read from — writing over it would "
                 "destroy the data in memory. Choose another destination.");
}

// The shared "may I create this output?" check. NAMES THE SPELLING THE USER
// TYPED, never the resolved path (defect class 4), in Stata's own words and
// code: `file o.out already exists` r(602) for outsheet, outfile, filefilter and
// copy alike (StataBE 19.5, BUG-410).
std::optional<ojson> refuseExisting(const std::string& resolved, const std::string& spelling,
                                    bool replace) {
  if (replace || !pathExists(resolved)) return std::nullopt;
  return ioErrorRc("file " + spelling + " already exists", 602);
}

// A file named without a suffix is NAME<suffix>, in the path and in every
// message: .out for outsheet, .raw for outfile and for the data infile and
// infix read, .dct for a dictionary. StataBE 19.5 `infile y using bare` is
// `file bare.raw not found` r(601) even beside a file called bare, and
// `infile using d1` reads d1.dct. A dot before the last '/' names a directory.
void assumeSuffix(std::string* resolved, std::string* spelling, const char* suffix) {
  const size_t slash = resolved->find_last_of('/');
  const size_t dot = resolved->find_last_of('.');
  if (dot != std::string::npos && (slash == std::string::npos || dot > slash)) return;
  *resolved += suffix;
  *spelling += suffix;
}

// Write `bytes` to `resolved` through a SIBLING temp + rename(2), so a failure
// never truncates whatever was there and an in-place rewrite (filefilter over
// its own input, which is separately refused, or an output that happens to be a
// source of the same command) can never read torn bytes. nullopt = written.
std::optional<ojson> commitBytes(const std::string& verb, const std::string& resolved,
                                 const std::string& spelling, const std::string& bytes) {
  const std::string tmp = resolved + ".piet_tmp";
  {
    std::ofstream f(cecon::to_native_path(tmp), std::ios::binary | std::ios::trunc);
    // A file Stata cannot create (its directory missing): `file nodir/o.raw
    // could not be opened` r(603), outfile and filefilter alike (BUG-410).
    if (!f) return ioErrorRc("file " + spelling + " could not be opened", 603);
    f.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    f.flush();
    if (!f) {
      f.close();
      std::error_code rmEc;
      std::filesystem::remove(cecon::to_native_path(tmp), rmEc);
      return ioError(verb + ": failed writing " + spelling + " (disk full or I/O error)",
                     "data_io");
    }
  }
  std::error_code mvEc;
  std::filesystem::rename(cecon::to_native_path(tmp), cecon::to_native_path(resolved), mvEc);
  if (mvEc) {
    std::error_code rmEc;
    std::filesystem::remove(cecon::to_native_path(tmp), rmEc);
    return ioError(verb + ": failed writing " + spelling + " — " + mvEc.message(), "data_io");
  }
  return std::nullopt;
}

// Read a whole file. nullopt on failure (the caller words its own error, so the
// message can name the spelling rather than the resolved path).
std::optional<std::string> readWholeFile(const std::string& resolved) {
  std::ifstream f(cecon::to_native_path(resolved), std::ios::binary);
  if (!f) return std::nullopt;
  std::ostringstream ss;
  ss << f.rdbuf();
  if (f.bad()) return std::nullopt;
  return ss.str();
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
  return ioError(cobraErrorMessage(ce), ce.code);
}

// ===========================================================================
// CSV transport (the ONE staging shape the three readers share)
// ===========================================================================

// One CSV field, quoted only when it must be -- the same convention
// handlers_data_verbs3.cpp's export writer uses.
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

// A whole table on its way to `cobra convert`. Factor columns are named
// explicitly rather than left to type inference, because a declared string
// column whose visible values happen to be all-numeric would otherwise come
// back f64 -- the declared type silently discarded.
struct CsvTable {
  std::vector<std::string> names;
  std::vector<char> isFactor;                  // parallel to `names`
  std::vector<std::vector<std::string>> rows;  // rows[r][c]; "" == missing
};

// Stage `t` as a CSV and convert it to an hbin. nullopt = success and *outPath
// holds the new frame; otherwise the error block to return. A torn file is
// removed rather than left where a later `load()` could mmap it (BUG-124).
std::optional<ojson> writeAndConvert(SessionState& session, const CsvTable& t,
                                     const std::string& prefix, std::string* outPath) {
  const std::string csvPath = nextPath(session.workdir(), prefix) + ".csv";
  registerScratchMint(csvPath);            // BUG-389
  session.noteProjectionScratch(csvPath);  // read once by `convert`, then dead
  {
    std::ofstream f(cecon::to_native_path(csvPath), std::ios::binary);
    if (!f) return ioError(prefix + ": cannot stage the converted rows", "data_io");
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
      return ioError(prefix + ": failed staging the converted rows (disk full or I/O error)",
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

// Adopt `hbin` as the session's dataset, exactly the way `use` does: the
// resident chain is DROPPED (a load from disk never continues it), the display
// metadata plane is cleared (a staged CSV declares no labels), and the source
// the user named becomes c(filename). Unlike `use`, c(changed) is 1: the data
// read have not been saved (StataBE 19.5, BUG-1478: 1 after `infile a b using
// f.raw, clear`, as after import delimited).
void adoptLoadedFrame(SessionState& session, const std::string& hbin,
                      const std::string& sourceResolved) {
  session.load(hbin, SessionState::LoadIntent::FreshRead);
  session.clearMetadata();
  session.setCurrentFile(sourceResolved);
  session.datasetChanged = true;
  session.invalidateDatastore();
}

// ===========================================================================
// Delimiters and headers
// ===========================================================================

// `delimiter(D)`, spelled Stata's way. Returns false and fills `err` on a
// spelling this build cannot turn into one character.
bool readDelimiterOpt(const std::vector<std::pair<std::string, eval::OptValue>>& opts, char* out,
                      std::string* err) {
  auto v = optStrAny(opts, {"delimiter", "delim"});
  if (!v.has_value()) return true;  // absent -> the caller's default
  const std::string raw = *v;
  const std::string low = lowerOf(raw);
  if (low == "tab" || raw == "\\t") {
    *out = '\t';
    return true;
  }
  if (low == "comma") {
    *out = ',';
    return true;
  }
  if (low == "semicolon") {
    *out = ';';
    return true;
  }
  if (raw.size() == 1) {
    *out = raw[0];
    return true;
  }
  *err = "delimiter(): expected a single character, `tab`, `comma` or `semicolon` (got '" + raw +
         "')";
  return false;
}

// ===========================================================================
// Reading the loaded dataset (the two writers)
// ===========================================================================

// The value-label table attached to `name`, or nullptr. Stata's `outsheet` and
// `outfile` write a labeled numeric as its LABEL text by default; `nolabel`
// writes the code. The labels live in the SESSION, which is why both writers
// are PIET-side rather than engine-side.
const std::map<std::string, std::string>* labelsFor(SessionState& session,
                                                    const std::string& name) {
  auto lv = session.varLblname.find(name);
  if (lv == session.varLblname.end()) return nullptr;
  auto ld = session.labelDefs.find(lv->second);
  if (ld == session.labelDefs.end()) return nullptr;
  return &ld->second;
}

// A labeled numeric's label text, if it has one for this value.
const std::string* labelTextFor(const std::map<std::string, std::string>* labels, double v) {
  if (labels == nullptr || std::isnan(v)) return nullptr;
  auto it = labels->find(eval::pythonFloatRepr(v));
  if (it == labels->end() && v == std::floor(v)) {
    it = labels->find(std::to_string(static_cast<long long>(v)));
  }
  if (it == labels->end()) return nullptr;
  return &it->second;
}

// ===========================================================================
// A1 addresses
// ===========================================================================

struct CellAddr {
  int row = 0;
  int col = 0;
};

// "bc12" -> {row 12, col 55}. Case-insensitive (Stata's addresses are).
// Returns false on anything that is not <letters><digits>.
bool parseAddr(const std::string& s, CellAddr* out) {
  size_t i = 0;
  long long col = 0;
  while (i < s.size() && std::isalpha(static_cast<unsigned char>(s[i]))) {
    col = col * 26 + (std::toupper(static_cast<unsigned char>(s[i])) - 'A' + 1);
    if (col > 16384) return false;  // past XFD, the last column a workbook has
    ++i;
  }
  if (i == 0 || i == s.size()) return false;
  long long row = 0;
  for (size_t j = i; j < s.size(); ++j) {
    if (!std::isdigit(static_cast<unsigned char>(s[j]))) return false;
    row = row * 10 + (s[j] - '0');
    if (row > 1048576) return false;  // past the last row a workbook has
  }
  if (row < 1) return false;
  out->row = static_cast<int>(row);
  out->col = static_cast<int>(col);
  return true;
}

std::string colRefOf(int col1) {  // 1 -> "A"
  std::string s;
  int n = col1;
  while (n > 0) {
    const int rem = (n - 1) % 26;
    s.insert(s.begin(), static_cast<char>('A' + rem));
    n = (n - 1) / 26;
  }
  return s;
}

std::string addrOf(int row, int col) { return colRefOf(col) + std::to_string(row); }

}  // namespace

// ===========================================================================
// insheet
// ===========================================================================
//
// insheet reads through the one native delimited module (delimited_io.cpp) in
// its own mode: the delimiter from the first line (tab, else comma, else the
// line is one field), trimmed fields with the untrimmed width, Stata's naming
// rule (lower case unless `case`, illegal characters removed, the cell as the
// label) and the storage types import delimited uses (w2ar log:3526-3690).

ojson runInsheet(const eval::ParsedCommand& parsed, SessionState& session) {
  if (auto err = eval::rejectUnknownOptions(parsed.opts, kVocabInsheet.known,
                                            kVocabInsheet.unimplemented)) {
    return ojson(*err);
  }
  if (hasFlag(parsed.opts, "double") && hasFlag(parsed.opts, "nodouble"))
    return ioSyntax("options double and nodouble may not be combined");

  std::vector<std::string> varlist;
  std::string pathArg;
  if (!splitUsing(quoteAwareWords(preCommaArgs(parsed.raw)), &varlist, &pathArg)) {
    return ioSyntax("insheet: missing the file. Usage: insheet [varlist] using \"file\" [, clear "
                    "names nonames comma tab delimiter(D) case double]");
  }
  for (const auto& v : varlist) {
    if (!strutil::isValidStataIdentifier(v)) return ioSyntax(v + ": invalid variable name");
  }

  // insheet reads into an EMPTY dataset: without `clear` over data in memory
  // it is r(18) and the data stay (w2ar dx_ins_noclear, log:3678); with
  // `clear` the data go first, so a missing file leaves k=0 (dx_ins_missing
  // 601 k=0, log:3690).
  const bool clear = hasFlag(parsed.opts, "clear");
  if (!clear && session.hasData()) {
    Datastore* ds = nullptr;
    try {
      ds = session.datastore();
    } catch (const std::exception&) {
      ds = nullptr;
    }
    if (ds != nullptr && ds->nCols() > 0) {
      return ioErrorRc("you must start with an empty dataset", 18);
    }
  }
  if (clear && session.hasData()) {
    ojson cleared = runClear("", session);
    if (isErrorBlock(cleared)) return cleared;
  }

  const std::string resolved = resolveUserPath(session, pathArg);
  if (!pathExists(resolved)) return ioErrorRc("file " + typedUserPath(pathArg) + " not found", 601);
  std::string text;
  {
    std::ifstream f(cecon::to_native_path(resolved), std::ios::binary);
    if (!f) return ioError("insheet: cannot read " + pathArg, "data_io");
    std::ostringstream ss;
    ss << f.rdbuf();
    text = ss.str();
  }

  // -- the delimiter: delimiter()/comma/tab, else the first line's own -- tab
  // when it holds one, else comma when it holds one, else none (the whole
  // line is one variable: dx_ins_028_names, log:3630).
  char delim = 0;
  {
    std::string derr;
    if (!readDelimiterOpt(parsed.opts, &delim, &derr)) return ioError(derr, "unsupported_option");
  }
  if (hasFlag(parsed.opts, "comma")) delim = ',';
  if (hasFlag(parsed.opts, "tab")) delim = '\t';
  if (delim == 0) {
    const size_t eol = text.find('\n');
    const std::string head = text.substr(0, eol == std::string::npos ? text.size() : eol);
    if (head.find('\t') != std::string::npos) delim = '\t';
    else if (head.find(',') != std::string::npos) delim = ',';
  }

  const delimited::InsheetRows rows = delimited::parseInsheet(text, delim);
  if (rows.fields.empty()) {
    // An empty file: Stata answers r(612) (dx_ins_empty, log:3668).
    return ioErrorRc("file " + pathArg + " is empty -- unexpected end of file", 612);
  }

  // -- the header: `names`/`nonames`, a varlist (implies nonames), else
  // Stata's automatic rule: a first row whose every field is a number is data.
  const bool wantNames = hasFlag(parsed.opts, "names");
  const bool wantNoNames = hasFlag(parsed.opts, "nonames") || !varlist.empty();
  if (wantNames && wantNoNames && varlist.empty()) {
    return ioSyntax("insheet: `names` and `nonames` cannot both be given");
  }
  bool header = true;
  if (wantNoNames) {
    header = false;
  } else if (!wantNames) {
    bool allNumeric = true;
    for (const auto& f : rows.fields[0]) {
      double d = 0.0;
      if (f.empty() || !delimited::parseNumber(f, &d)) {
        allNumeric = false;
        break;
      }
    }
    header = !allNumeric;
  }

  const size_t first = header ? 1 : 0;
  // A data row with MORE fields than the names row widens the dataset, as
  // import delimited does: the extra columns are v<column>, missing on the
  // shorter rows (BUG-414: `a,b,c` over `4,5` and `6,7,8,9,10` reads a b c v4
  // v5, 3 obs, StataBE insheet rc 0). Where a header name already holds
  // v<column>, insheet takes the lowest free v# (`v3,b` over `3,4,5` reads v3 b
  // v1) and import delimited refuses (delimited::Fallback).
  size_t k = header ? rows.fields[0].size() : 0;
  for (size_t r = first; r < rows.fields.size(); ++r) k = std::max(k, rows.fields[r].size());
  std::vector<std::string> headerCells(k);
  if (header) {
    for (size_t c = 0; c < rows.fields[0].size() && c < k; ++c) headerCells[c] = rows.fields[0][c];
  }
  delimited::NameResult nr = delimited::legalNames(
      headerCells, hasFlag(parsed.opts, "case") ? delimited::Case::kPreserve : delimited::Case::kLower,
      delimited::Fallback::kLowestFree);
  if (!header) {
    for (auto& l : nr.labels) l.clear();
  }
  if (varlist.size() > k) return ioErrorRc("too many variables specified", 103);
  for (size_t i = 0; i < varlist.size(); ++i) nr.names[i] = varlist[i];

  delimited::Table table;
  table.names = nr.names;
  table.labels = nr.labels;
  std::vector<size_t> widthFloor(k, 0);
  for (size_t r = first; r < rows.fields.size(); ++r) {
    std::vector<std::string> row(k);
    for (size_t c = 0; c < rows.fields[r].size() && c < k; ++c) {
      row[c] = rows.fields[r][c];
      widthFloor[c] = std::max(widthFloor[c], rows.widths[r][c]);
    }
    table.rows.push_back(std::move(row));
  }
  // Float columns unless double storage is asked for: by `double`, or by the
  // c(type) default `set type double` sets with no `nodouble` to undo it
  // (insheet's help; w2k_binscatter.log:1477 BS_077_data_o1 reads
  // float(1.6209999)).
  const bool asDouble =
      !hasFlag(parsed.opts, "nodouble") &&
      (hasFlag(parsed.opts, "double") || session.defaultStorageType != "float");
  table.types.resize(k);
  for (size_t c = 0; c < k; ++c) {
    std::vector<const std::string*> cells;
    cells.reserve(table.rows.size());
    for (const auto& row : table.rows) cells.push_back(&row[c]);
    table.types[c] = delimited::typeColumn(cells, false, false, asDouble, widthFloor[c],
                                           /*precisionToDouble=*/false);
  }
  if (auto err = delimited::stageAndAdopt(session, table, "insheet", resolved)) return *err;
  return mutationResult(session, "insheet");
}

// ===========================================================================
// outsheet
// ===========================================================================
//
// Decision 2. `export delimited` with the defaults swapped: Stata's `outsheet`
// writes TAB-separated by default (`comma` switches it) and writes the variable
// names as row 1. Everything else -- the value-label rendering, the missing
// spelling, the sibling-temp + rename commit -- is runExportDelimited's, so the
// two commands cannot disagree about the bytes.
ojson runOutsheet(const eval::ParsedCommand& parsed, SessionState& session) {
  if (auto err = eval::rejectUnknownOptions(parsed.opts, kVocabOutsheet.known,
                                            kVocabOutsheet.unimplemented)) {
    return ojson(*err);
  }
  if (!session.hasData()) return ojson(eval::noDataLoaded());

  std::vector<std::string> varlist;
  std::string pathArg;
  if (!splitUsing(dropQualifierWords(quoteAwareWords(preCommaArgs(parsed.raw))), &varlist,
                  &pathArg)) {
    return ioSyntax("outsheet: missing the file. Usage: outsheet [varlist] using \"file\" "
                    "[, comma replace noquote nolabel nonames delimiter(D)]");
  }

  // outsheet's own rules (w2ar log:810-929): TAB by default (`comma`,
  // `delimiter()` change it), every numeric through its display format
  // (`1.235e+10` for a %10.0g double), strings and label texts ALWAYS quoted
  // with an inner quote left as is (`"q"r"`, log:815), dates formatted and
  // bare; `noquote` drops the quotes (log:865) and `nonames` the header
  // (log:893). `if` restricts the rows (dx_outs_if, log:838).
  DelimWriteSpec spec;
  spec.verb = "outsheet";
  spec.delim = '\t';
  {
    std::string derr;
    if (!readDelimiterOpt(parsed.opts, &spec.delim, &derr)) return ioError(derr, "unsupported_option");
  }
  if (hasFlag(parsed.opts, "comma")) spec.delim = ',';
  spec.noLabel = hasFlag(parsed.opts, "nolabel");
  spec.header = !hasFlag(parsed.opts, "nonames");
  spec.dataFmt = true;
  spec.quote = hasFlag(parsed.opts, "noquote") ? DelimWriteSpec::Quote::kNone
                                               : DelimWriteSpec::Quote::kAllVerbatim;

  const bool replace = hasFlag(parsed.opts, "replace");
  std::string resolved = resolveUserPath(session, pathArg);
  // "If filename is specified without a suffix, .out is assumed" (outsheet
  // help).
  assumeSuffix(&resolved, &pathArg, ".out");
  if (auto err = refuseExisting(resolved, pathArg, replace)) return *err;
  if (auto err = refuseIfSessionHbin(session, "outsheet", resolved, pathArg)) return *err;

  spec.shown = pathArg;  // the writer names the file as typed (BUG-410)
  return writeDelimitedFile(varlist, resolved, spec, parsed.ifExpr, parsed.inRange, session);
}

// ===========================================================================
// infile (free format)
// ===========================================================================
//
// Decision 3. Stata's free-format reader: values are separated by whitespace,
// commas or line breaks INTERCHANGEABLY, a double-quoted run is one value, `.`
// is missing, and a RECORD is however many values the varlist declares --
// source line breaks carry no meaning at all. A `str#` prefix declares the next
// variable a string; `_skip(#)` consumes # values without storing them.
namespace {

// One entry of an `infile` varlist: a variable, or a skip.
struct InfileSlot {
  std::string name;    // empty for a `_skip(#)` slot
  bool isString = false;
  int skip = 0;        // >0 only for a skip slot
  std::string declaredType;  // "str3", "int", ... -- recorded as metadata
};

bool isTypeToken(const std::string& tok, bool* isString) {
  const std::string t = lowerOf(tok);
  if (t == "byte" || t == "int" || t == "long" || t == "float" || t == "double") {
    *isString = false;
    return true;
  }
  if (t == "strl") {
    *isString = true;
    return true;
  }
  if (t.size() > 3 && t.compare(0, 3, "str") == 0) {
    for (size_t i = 3; i < t.size(); ++i) {
      if (!std::isdigit(static_cast<unsigned char>(t[i]))) return false;
    }
    *isString = true;
    return true;
  }
  return false;
}

// Every free-format value in `bytes`, in order. A quoted run is one value even
// when it contains a delimiter; `.` stays `.` (the caller decides it is
// missing, because a string column's `.` is the literal character).
std::vector<std::string> freeFormatValues(const std::string& bytes, std::vector<char>* wasQuoted) {
  std::vector<std::string> out;
  size_t i = 0;
  const size_t n = bytes.size();
  while (i < n) {
    const char c = bytes[i];
    if (c == ',' || std::isspace(static_cast<unsigned char>(c))) {
      ++i;
      continue;
    }
    if (c == '"') {
      std::string tok;
      ++i;
      while (i < n && bytes[i] != '"') {
        tok.push_back(bytes[i]);
        ++i;
      }
      if (i < n) ++i;  // the closing quote
      out.push_back(tok);
      wasQuoted->push_back(1);
      continue;
    }
    std::string tok;
    while (i < n && bytes[i] != ',' && !std::isspace(static_cast<unsigned char>(bytes[i]))) {
      tok.push_back(bytes[i]);
      ++i;
    }
    out.push_back(tok);
    wasQuoted->push_back(0);
  }
  return out;
}

}  // namespace

// ===========================================================================
// infix / infile dictionaries (fixed columns)
// ===========================================================================
//
// One fixed-column reader behind `infix` (command-line and dictionary specs)
// and `infile`'s dictionary form. The rules are the oracle's (w2ar
// log:3755-4064):
//   * an untyped numeric is float, its values rounded to float (log:3772);
//   * a bare `str` takes its column range's width, and a declared `str#`
//     narrower than its range is widened to it (`str3 name 8-12` -> str5,
//     log:3801); the width is the range's, not the data's (`8-40` -> str33);
//   * a string field ends at a NUL byte and is trimmed of blanks, except an
//     infile `%#S` field, which keeps them (log:4011 `[b c  ]`);
//   * a blank, `.` or unreadable numeric field is missing; a range past the
//     end of a line reads "" / missing (log:3838);
//   * `if`/`in` keep the records they select (log:3826);
//   * the reader wants an empty dataset: without `clear` over data, r(18)
//     with the data kept; with `clear` the data go first, so a missing or
//     unreadable dictionary leaves k=0 (log:3876-3900, 4052-4064).
namespace {

// `using ("data")` with a blank before the parenthesis is the same option as
// `using("data")` in Stata (w2ar dx_inf_030_space rc 0, log:3968); the
// tokenizer hands it over as a bare `using` followed by a key `"data")`.
eval::ParsedCommand joinSpacedOptionValues(const eval::ParsedCommand& parsed) {
  eval::ParsedCommand out = parsed;
  out.opts.clear();
  const auto& o = parsed.opts;
  for (size_t i = 0; i < o.size(); ++i) {
    if (o[i].second.isBool && i + 1 < o.size()) {
      const std::string& nk = o[i + 1].first;
      if (nk.size() >= 2 && (nk[0] == '(' || nk[0] == '"') && nk.back() == ')') {
        std::string v = nk.substr(0, nk.size() - 1);
        if (!v.empty() && v[0] == '(') v.erase(0, 1);
        out.opts.emplace_back(o[i].first, eval::OptValue::ofString(v));
        ++i;
        continue;
      }
    }
    out.opts.push_back(o[i]);
  }
  return out;
}

struct FixSlot {
  std::string name;
  int from = 1;  // 1-based, inclusive
  int to = 1;
  bool isString = false;
  bool keepBlanks = false;  // infile %#S
  std::string declaredType;  // "" untyped numeric, "str" bare, "strN", or a numeric type
  std::string label;
};

// The storage type a slot's column takes.
std::string fixSlotType(const FixSlot& s) {
  if (!s.isString) return s.declaredType.empty() ? std::string("float") : s.declaredType;
  const int width = std::max(1, s.to - s.from + 1);
  if (lowerOf(s.declaredType) == "strl") return "strL";
  int declared = 0;
  if (s.declaredType.size() > 3) declared = std::atoi(s.declaredType.c_str() + 3);
  return "str" + std::to_string(std::max(width, declared));
}

// The empty-dataset rule both readers share; nullopt = go on.
std::optional<ojson> requireEmptyOrClear(const eval::ParsedCommand& parsed, SessionState& session) {
  const bool clear = hasFlag(parsed.opts, "clear");
  if (!session.hasData()) return std::nullopt;
  if (!clear) {
    Datastore* ds = nullptr;
    try {
      ds = session.datastore();
    } catch (const std::exception&) {
      ds = nullptr;
    }
    if (ds != nullptr && ds->nCols() > 0) {
      return ioErrorRc("you must start with an empty dataset", 18);
    }
    return std::nullopt;
  }
  ojson cleared = runClear("", session);
  if (isErrorBlock(cleared)) return cleared;
  return std::nullopt;
}

// Read `bytes` through `slots`, stage it, adopt it and apply if/in.
ojson readFixedAndAdopt(const std::string& verb, const std::string& bytes,
                        const std::vector<FixSlot>& slots, const std::string& source,
                        const eval::ParsedCommand& parsed, SessionState& session) {
  delimited::Table table;
  for (const auto& s : slots) {
    table.names.push_back(s.name);
    table.labels.push_back(s.label);
    delimited::ColumnType ct;
    ct.isString = s.isString;
    ct.stataType = fixSlotType(s);
    table.types.push_back(ct);
  }
  size_t start = 0;
  const size_t n = bytes.size();
  while (start < n) {
    size_t eol = bytes.find('\n', start);
    if (eol == std::string::npos) eol = n;
    std::string line = bytes.substr(start, eol - start);
    start = eol + 1;
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (line.empty()) continue;  // a blank line is not a record
    if (line.find('\t') != std::string::npos) {
      // Columns are counted with tabs expanded to stops every 8 (w2ar
      // dx_fx_fb16_L1map `1_______5___` for "1<tab>5<tab>2.5", log:2512).
      std::string expanded;
      for (char ch : line) {
        if (ch == '\t') {
          do expanded += ' ';
          while (expanded.size() % 8 != 0);
        } else {
          expanded += ch;
        }
      }
      line = std::move(expanded);
    }
    std::vector<std::string> row;
    row.reserve(slots.size());
    for (const auto& s : slots) {
      std::string field;
      if (static_cast<size_t>(s.from) <= line.size()) {
        const size_t begin = static_cast<size_t>(s.from) - 1;
        const size_t end = std::min(static_cast<size_t>(s.to), line.size());
        field = line.substr(begin, end - begin);
      }
      if (s.isString) {
        const size_t nul = field.find('\0');
        if (nul != std::string::npos) field.resize(nul);
        if (!s.keepBlanks) {
          field = trimOf(field);
          // A string field that opens with a double quote loses that quote
          // (w2ar dx_outf_if_L1 `"b c"  "two"` reads `b c"  "two"`, log:4990;
          // a lone `"` reads "", dx_fx_fd17_L1map).
          if (!field.empty() && field.front() == '"') field.erase(0, 1);
        }
        row.push_back(field);
        continue;
      }
      field = trimOf(field);
      double d = 0.0;
      if (field.empty() || field == "." || !parseStrictDouble(field, &d)) {
        row.emplace_back();
      } else {
        char buf[64];
        std::snprintf(buf, sizeof(buf), "%.17g", d);
        row.push_back(buf);
      }
    }
    table.rows.push_back(std::move(row));
  }
  if (parsed.inRange.has_value() && !parsed.inRange->empty()) {
    // `in` names RECORDS to read: a range past the last record reads what
    // there is (an empty file `in 1` is N=0 at rc 0, w2ar dx_fx_fc15_L1map).
    const int64_t nRec = static_cast<int64_t>(table.rows.size());
    InRangeBounds rr = parseInRangeBounds(*parsed.inRange, nRec);
    if (!rr.ok && parsed.inRange->find_first_of("lL-") == std::string::npos) {
      rr = parseInRangeBounds(*parsed.inRange, std::numeric_limits<int32_t>::max());
    }
    if (!rr.ok) {
      InRangeFilterResult fr;
      fr.ok = false;
      fr.refusal = rr.refusal;
      return inRangeRejectError(fr, *parsed.inRange, verb);
    }
    const int64_t a = std::min<int64_t>(rr.a - 1, nRec);
    const int64_t b = std::min<int64_t>(rr.b, nRec);
    std::vector<std::vector<std::string>> kept(table.rows.begin() + static_cast<std::ptrdiff_t>(a),
                                               table.rows.begin() + static_cast<std::ptrdiff_t>(std::max(a, b)));
    table.rows = std::move(kept);
  }
  if (auto err = delimited::stageAndAdopt(session, table, verb, source)) return *err;
  if (parsed.ifExpr.has_value() && !parsed.ifExpr->empty()) {
    InRangeFilterResult fr;
    try {
      fr = applyIfAndInFilter(session, *session.currentHbin(), parsed.ifExpr, std::nullopt);
    } catch (const CobraError& ce) {
      return ioError(cobraErrorMessage(ce), ce.code);
    }
    if (fr.dataPath != *session.currentHbin()) {
      session.load(fr.dataPath);
      session.invalidateDatastore();
    }
  }
  return mutationResult(session, verb);
}

// One dictionary file: its optional `using` data file, its entries, and the
// data lines that follow `}` when it names no file.
struct Dictionary {
  std::string usingFile;
  std::vector<std::string> entries;  // the lines between { and }
  std::string trailingData;
  bool ok = false;
};

Dictionary readDictionary(const std::string& text) {
  Dictionary d;
  const size_t open = text.find('{');
  if (open == std::string::npos) return d;
  const size_t close = text.find('}', open);
  if (close == std::string::npos) return d;
  const std::string head = trimOf(text.substr(0, open));
  // [infix|infile] dictionary [using FILE]
  std::vector<std::string> hw = quoteAwareWords(head);
  size_t i = 0;
  if (i < hw.size() && (lowerOf(hw[i]) == "infix" || lowerOf(hw[i]) == "infile")) ++i;
  if (i >= hw.size() || lowerOf(hw[i]) != "dictionary") return d;
  ++i;
  if (i < hw.size()) {
    if (lowerOf(hw[i]) != "using" || i + 1 >= hw.size()) return d;
    d.usingFile = hw[i + 1];
    i += 2;
  }
  if (i != hw.size()) return d;
  const std::string body = text.substr(open + 1, close - open - 1);
  size_t p = 0;
  while (p <= body.size()) {
    size_t e = body.find('\n', p);
    if (e == std::string::npos) e = body.size();
    std::string line = trimOf(body.substr(p, e - p));
    if (!line.empty() && line.back() == '\r') line.pop_back();
    p = e + 1;
    if (line.empty() || line[0] == '*' || line.compare(0, 2, "//") == 0) continue;
    d.entries.push_back(line);
  }
  size_t after = close + 1;
  if (after < text.size() && text[after] == '\r') ++after;
  if (after < text.size() && text[after] == '\n') ++after;
  d.trailingData = after < text.size() ? text.substr(after) : std::string();
  d.ok = true;
  return d;
}

// `#[-#]` -> from/to.
bool parseColRange(const std::string& spec, int* from, int* to) {
  long long a = 0, b = 0;
  const size_t dash = spec.find('-');
  if (dash == std::string::npos) {
    if (!parseStrictInt(spec, &a) || a < 1) return false;
    b = a;
  } else if (!parseStrictInt(spec.substr(0, dash), &a) || !parseStrictInt(spec.substr(dash + 1), &b) ||
             a < 1 || b < a) {
    return false;
  }
  *from = static_cast<int>(a);
  *to = static_cast<int>(b);
  return true;
}

// infix's command-line / dictionary spec: `[type] name #[-#]` repeated.
std::optional<ojson> parseInfixSpec(const std::vector<std::string>& words, std::vector<FixSlot>* slots) {
  std::string pendingType;
  bool pendingIsString = false;
  std::string pendingName;
  for (const auto& w : words) {
    bool isStr = false;
    if (pendingName.empty() && (isTypeToken(w, &isStr) || lowerOf(w) == "str")) {
      if (lowerOf(w) == "str") isStr = true;
      pendingType = lowerOf(w);
      pendingIsString = isStr;
      continue;
    }
    if (!w.empty() && std::isdigit(static_cast<unsigned char>(w[0]))) {
      if (pendingName.empty()) {
        return ioSyntax("infix: column range `" + w + "` does not follow a variable name");
      }
      FixSlot s;
      if (!parseColRange(w, &s.from, &s.to)) {
        return ioSyntax("infix: `" + w + "` is not a column range (expected #-# with the first "
                        "column no greater than the last)");
      }
      s.name = pendingName;
      s.isString = pendingIsString;
      s.declaredType = pendingType;
      slots->push_back(s);
      pendingName.clear();
      pendingType.clear();
      pendingIsString = false;
      continue;
    }
    if (!pendingName.empty()) return ioSyntax("infix: variable " + pendingName + " has no column range");
    if (!strutil::isValidStataIdentifier(w)) {
      return ioSyntax("infix: invalid variable name `" + w + "' — " +
                      strutil::stataIdentifierRuleText());
    }
    pendingName = w;
  }
  if (!pendingName.empty()) return ioSyntax("infix: variable " + pendingName + " has no column range");
  if (slots->empty()) return ioSyntax("infix: no variables named");
  std::set<std::string> seen;
  for (const auto& s : *slots) {
    if (!seen.insert(s.name).second) return ioSyntax("infix: variable " + s.name + " named twice");
  }
  return std::nullopt;
}

// infile's dictionary entries: `[_column(#)] [type] name [%fmt] ["label"]`.
// The field starts at _column(#) (else where the previous one ended) and is
// as wide as the input format says (%4f, %5s, %5S).
std::optional<ojson> parseInfileDictionary(const std::vector<std::string>& entries,
                                           std::vector<FixSlot>* slots) {
  int nextCol = 1;
  for (const auto& line : entries) {
    std::vector<std::string> w = quoteAwareWords(line);
    size_t i = 0;
    FixSlot s;
    int col = nextCol;
    if (i < w.size() && lowerOf(w[i]).compare(0, 8, "_column(") == 0 && w[i].back() == ')') {
      long long c = 0;
      if (!parseStrictInt(w[i].substr(8, w[i].size() - 9), &c) || c < 1)
        return ioSyntax("infile: invalid dictionary entry `" + line + "`");
      col = static_cast<int>(c);
      ++i;
    }
    bool isStr = false;
    if (i < w.size() && isTypeToken(w[i], &isStr)) {
      s.declaredType = lowerOf(w[i]);
      s.isString = isStr;
      ++i;
    }
    if (i >= w.size() || !strutil::isValidStataIdentifier(w[i]))
      return ioSyntax("infile: invalid dictionary entry `" + line + "`");
    s.name = w[i++];
    int width = 0;
    if (i < w.size() && !w[i].empty() && w[i][0] == '%') {
      const std::string f = w[i++];
      size_t k = 1;
      while (k < f.size() && std::isdigit(static_cast<unsigned char>(f[k]))) ++k;
      width = std::atoi(f.substr(1, k - 1).c_str());
      // `%#S` keeps the field's blanks.
      if (k < f.size() && f[k] == 'S') s.keepBlanks = true;
    }
    if (i < w.size()) {
      const std::string lab = w[i++];
      if (lab.size() >= 2 && lab.front() == '"' && lab.back() == '"')
        s.label = lab.substr(1, lab.size() - 2);
      else
        return ioSyntax("infile: invalid dictionary entry `" + line + "`");
    }
    if (i != w.size()) return ioSyntax("infile: invalid dictionary entry `" + line + "`");
    if (width <= 0 && s.isString && s.declaredType.size() > 3)
      width = std::atoi(s.declaredType.c_str() + 3);
    if (width <= 0) return ioSyntax("infile: dictionary entry `" + line + "` has no field width");
    s.from = col;
    s.to = col + width - 1;
    nextCol = s.to + 1;
    if (s.isString) {
      // An infile string keeps its DECLARED width (str5 over %5s, log:3958).
      if (s.declaredType.size() <= 3) s.declaredType = "str" + std::to_string(width);
      s.to = s.from + width - 1;
    }
    slots->push_back(s);
  }
  if (slots->empty()) return ioSyntax("infile: the dictionary declares no variables");
  return std::nullopt;
}

// Reads the dictionary at `dictPath` and the data it points to (the using()
// option, the dictionary's own `using FILE`, or the lines after `}`).
ojson runDictionaryRead(const std::string& verb, const std::string& dictArg,
                        const eval::ParsedCommand& parsed, SessionState& session) {
  std::string dictPath = resolveUserPath(session, dictArg);
  std::string dictShown = typedUserPath(dictArg);
  assumeSuffix(&dictPath, &dictShown, ".dct");
  session.materializeVirtualSave(dictPath);
  if (!pathExists(dictPath)) return ioErrorRc("file " + dictShown + " not found", 601);
  auto dictBytes = readWholeFile(dictPath);
  if (!dictBytes.has_value()) return ioError(verb + ": cannot read " + dictShown, "data_io");
  const Dictionary d = readDictionary(*dictBytes);
  if (!d.ok) return ioSyntax(verb + ": " + dictShown + " is not a dictionary");
  std::vector<FixSlot> slots;
  if (verb == "infix") {
    std::vector<std::string> words;
    for (const auto& e : d.entries)
      for (const auto& w : quoteAwareWords(e)) words.push_back(w);
    if (auto err = parseInfixSpec(words, &slots)) return *err;
  } else {
    if (auto err = parseInfileDictionary(d.entries, &slots)) return *err;
  }
  std::string dataArg;
  if (auto u = optStrAny(parsed.opts, {"using"})) dataArg = *u;
  else if (!d.usingFile.empty()) dataArg = dequote(d.usingFile);
  std::string bytes;
  std::string source = dictPath;
  if (!dataArg.empty()) {
    std::string dataPath = resolveUserPath(session, dataArg);
    std::string dataShown = typedUserPath(dataArg);
    assumeSuffix(&dataPath, &dataShown, ".raw");
    session.materializeVirtualSave(dataPath);
    if (!pathExists(dataPath)) return ioErrorRc("file " + dataShown + " not found", 601);
    auto b = readWholeFile(dataPath);
    if (!b.has_value()) return ioError(verb + ": cannot read " + dataShown, "data_io");
    bytes = *b;
    source = dataPath;
  } else {
    bytes = d.trailingData;
  }
  return readFixedAndAdopt(verb, bytes, slots, source, parsed, session);
}

}  // namespace


ojson runInfile(const eval::ParsedCommand& parsedIn, SessionState& session) {
  const eval::ParsedCommand parsed = joinSpacedOptionValues(parsedIn);
  if (auto err = eval::rejectUnknownOptions(parsed.opts, kVocabInfile.known,
                                            kVocabInfile.unimplemented)) {
    return ojson(*err);
  }

  std::vector<std::string> words;
  std::string pathArg;
  if (!splitUsing(quoteAwareWords(preCommaArgs(parsed.raw)), &words, &pathArg)) {
    return ioSyntax("infile: missing the file. Usage: infile varlist using \"file\" [, clear]");
  }
  if (auto err = requireEmptyOrClear(parsed, session)) return *err;
  if (words.empty()) {
    // `infile using dict [, using(data)]` -- the dictionary form, 166 of the
    // corpus's 168 infile lines (w2ar dx_inf_010_dict log:3953, the dictionary
    // naming its data file dx_inf_037 log:4007).
    return runDictionaryRead("infile", pathArg, parsed, session);
  }

  // -- the varlist spec ----------------------------------------------------
  std::vector<InfileSlot> slots;
  {
    std::string pendingType;
    bool pendingIsString = false;
    for (const auto& w : words) {
      bool isStr = false;
      if (isTypeToken(w, &isStr)) {
        pendingType = lowerOf(w);
        pendingIsString = isStr;
        continue;
      }
      const std::string lw = lowerOf(w);
      if (lw.compare(0, 6, "_skip(") == 0 && !lw.empty() && lw.back() == ')') {
        long long k = 0;
        if (!parseStrictInt(w.substr(6, w.size() - 7), &k) || k < 1) {
          return ioSyntax("infile: `" + w + "` — _skip() takes a positive whole number of values");
        }
        InfileSlot s;
        s.skip = static_cast<int>(k);
        slots.push_back(s);
        pendingType.clear();
        pendingIsString = false;
        continue;
      }
      if (!strutil::isValidStataIdentifier(w)) {
        return ioSyntax("infile: invalid variable name `" + w + "' — " +
                        strutil::stataIdentifierRuleText());
      }
      InfileSlot s;
      s.name = w;
      s.isString = pendingIsString;
      s.declaredType = pendingType;
      slots.push_back(s);
      pendingType.clear();
      pendingIsString = false;
    }
  }
  size_t width = 0;
  std::vector<std::string> names;
  for (const auto& s : slots) {
    width += (s.skip > 0) ? static_cast<size_t>(s.skip) : 1u;
    if (s.skip == 0) names.push_back(s.name);
  }
  if (names.empty()) return ioSyntax("infile: no variables named");
  {
    std::set<std::string> seen;
    for (const auto& n : names) {
      if (!seen.insert(n).second) return ioSyntax("infile: variable " + n + " named twice");
    }
  }

  std::string resolved = resolveUserPath(session, pathArg);
  std::string shown = typedUserPath(pathArg);
  assumeSuffix(&resolved, &shown, ".raw");
  if (!pathExists(resolved)) return ioErrorRc("file " + shown + " not found", 601);
  auto bytes = readWholeFile(resolved);
  if (!bytes.has_value()) return ioError("infile: cannot read " + shown, "data_io");

  std::vector<char> quoted;
  const std::vector<std::string> values = freeFormatValues(*bytes, &quoted);

  CsvTable table;
  for (const auto& s : slots) {
    if (s.skip > 0) continue;
    table.names.push_back(s.name);
    table.isFactor.push_back(s.isString ? 1 : 0);
  }
  // The record loop. A trailing PARTIAL record is padded with missing, which is
  // what Stata does (it warns; PIET says so in a display line below).
  const size_t nRec = width == 0 ? 0 : (values.size() + width - 1) / width;
  bool padded = false;
  for (size_t r = 0; r < nRec; ++r) {
    std::vector<std::string> row;
    size_t v = r * width;
    for (const auto& s : slots) {
      if (s.skip > 0) {
        v += static_cast<size_t>(s.skip);
        continue;
      }
      if (v >= values.size()) {
        row.emplace_back();  // "" == missing
        padded = true;
        ++v;
        continue;
      }
      const std::string& tok = values[v];
      const bool wasQuoted = quoted[v] != 0;
      ++v;
      if (s.isString) {
        row.push_back(tok);
        continue;
      }
      if (!wasQuoted && tok == ".") {
        row.emplace_back();
        continue;
      }
      double d = 0.0;
      if (!parseStrictDouble(tok, &d)) {
        return ioError("infile: `" + tok + "` cannot be read as a number for variable " + s.name +
                       " (declare it a string with a `str#` prefix if it is text)");
      }
      row.push_back(eval::pythonFloatRepr(d));
    }
    table.rows.push_back(std::move(row));
  }

  std::string hbin;
  if (auto err = writeAndConvert(session, table, "infile", &hbin)) return *err;
  adoptLoadedFrame(session, hbin, resolved);

  // The DECLARED types are metadata this build records but does not enforce --
  // the frame is f64/factor throughout. Recording them is what makes a later
  // `save x.dta` write the type the user asked for (the --in-meta sidecar).
  for (const auto& s : slots) {
    if (s.skip > 0 || s.declaredType.empty()) continue;
    session.varMeta[s.name].stataType = s.declaredType;
  }

  ojson out = ojson::array();
  if (padded) {
    out.push_back(displayBlock("(infile: the last record was short — the missing values were "
                               "filled with `.`)"));
  }
  out.push_back(mutationResult(session, "infile"));
  return out;
}

ojson runInfix(const eval::ParsedCommand& parsedIn, SessionState& session) {
  const eval::ParsedCommand parsed = joinSpacedOptionValues(parsedIn);
  if (auto err = eval::rejectUnknownOptions(parsed.opts, kVocabInfix.known,
                                            kVocabInfix.unimplemented)) {
    return ojson(*err);
  }

  std::vector<std::string> words;
  std::string pathArg;
  if (!splitUsing(dropQualifierWords(quoteAwareWords(preCommaArgs(parsed.raw))), &words,
                  &pathArg)) {
    return ioSyntax("infix: missing the file. Usage: infix [type] var #-# [...] using \"file\" "
                    "[, clear]");
  }
  std::vector<FixSlot> slots;
  if (!words.empty()) {
    if (auto err = parseInfixSpec(words, &slots)) return *err;
  }
  if (auto err = requireEmptyOrClear(parsed, session)) return *err;
  if (words.empty()) {
    // `infix using dict [, using(data)]` -- the dictionary form (w2ar
    // dx_ifx_dict, log:3857).
    return runDictionaryRead("infix", pathArg, parsed, session);
  }
  std::string resolved = resolveUserPath(session, pathArg);
  std::string shown = typedUserPath(pathArg);
  assumeSuffix(&resolved, &shown, ".raw");
  session.materializeVirtualSave(resolved);  // BUG-125: a save_virtual'd tempfile becomes real
  if (!pathExists(resolved)) return ioErrorRc("file " + shown + " not found", 601);
  auto bytes = readWholeFile(resolved);
  if (!bytes.has_value()) return ioError("infix: cannot read " + shown, "data_io");
  return readFixedAndAdopt("infix", *bytes, slots, resolved, parsed, session);
}

// ===========================================================================
// outfile
// ===========================================================================
//
// Decision 4. Stata's oldest writer: no header row, one observation per line,
// values separated by a single space (or a comma with `comma`), strings in
// double quotes (unless `noquote`), numbers in `%10.0g` and missing as `.` --
// in the comma form too, where an empty field would be read back as missing by
// some readers and as an empty string by others.
ojson runOutfile(const eval::ParsedCommand& parsed, SessionState& session) {
  if (auto err = eval::rejectUnknownOptions(parsed.opts, kVocabOutfile.known,
                                            kVocabOutfile.unimplemented)) {
    return ojson(*err);
  }
  if (!session.hasData()) return ojson(eval::noDataLoaded());

  std::vector<std::string> varlist;
  std::string pathArg;
  if (!splitUsing(dropQualifierWords(quoteAwareWords(preCommaArgs(parsed.raw))), &varlist,
                  &pathArg)) {
    return ioSyntax("outfile: missing the file. Usage: outfile [varlist] using \"file\" "
                    "[, replace comma noquote nolabel wide]");
  }

  const bool replace = hasFlag(parsed.opts, "replace");
  const bool noQuote = hasFlag(parsed.opts, "noquote");
  const bool noLabel = hasFlag(parsed.opts, "nolabel");
  const bool comma = hasFlag(parsed.opts, "comma");
  const bool wide = hasFlag(parsed.opts, "wide");

  std::string resolved = resolveUserPath(session, pathArg);
  // "If filename is specified without a suffix, .raw is assumed" (outfile
  // help; StataBE `outfile using o2` names `o2.raw`).
  assumeSuffix(&resolved, &pathArg, ".raw");
  if (auto err = refuseExisting(resolved, pathArg, replace)) return *err;
  if (auto err = refuseIfSessionHbin(session, "outfile", resolved, pathArg)) return *err;

  Datastore* base = nullptr;
  try {
    base = session.datastore();
  } catch (const std::exception& e) {
    return ioError(std::string("outfile: the dataset in memory could not be read — ") + e.what(),
                   "data_io");
  }
  if (base == nullptr) return ojson(eval::noDataLoaded());
  std::vector<std::string> cols = varlist;
  if (cols.empty()) cols = base->names();
  for (const auto& v : cols) {
    if (!base->has(v)) return ioError("variable " + v + " not found", "unknown_variable");
  }

  // Each variable's field, laid out the way Stata's outfile does (w2ar
  // log:941-999): a numeric right-aligned in its display format's width, a
  // string `"quoted"` and left-aligned in its str# width plus the two quotes,
  // a date (or a value label) quoted as text; fields two blanks apart; a
  // record wider than 80 columns continues on the next line unless `wide`.
  // `noquote` drops the quotes but the line keeps its quoted length.
  struct OutCol {
    const std::map<std::string, std::string>* labels = nullptr;
    std::string fmt;
    char dateFamily = 0;
    bool isString = false;
    int width = 0;   // the field's width, quotes included
  };
  std::vector<OutCol> oc;
  oc.reserve(cols.size());
  for (const auto& name : cols) {
    OutCol o;
    const cecon::Column& col = base->col(name);
    if (col.dtype == cecon::DType::Factor) {
      o.isString = true;
      int w = 0;
      auto vm = session.varMeta.find(name);
      if (vm != session.varMeta.end() && vm->second.stataType.has_value() &&
          vm->second.stataType->size() > 3 && vm->second.stataType->compare(0, 3, "str") == 0 &&
          std::isdigit(static_cast<unsigned char>((*vm->second.stataType)[3]))) {
        w = std::atoi(vm->second.stataType->c_str() + 3);
      }
      for (const auto& lv : col.levels) w = std::max(w, static_cast<int>(lv.size()));
      o.width = std::max(w, 1) + 2;
    } else {
      if (!noLabel) o.labels = labelsFor(session, name);
      o.fmt = session.displayFormat(base->varinfo(name));
      std::string f = o.fmt;
      if (!f.empty() && f[0] == '%') f.erase(0, 1);
      if (!f.empty() && f[0] == '-') f.erase(0, 1);
      int w = std::atoi(f.c_str());
      if (w <= 0) w = 9;
      if (f.size() >= 2 && f[0] == 't') {
        o.dateFamily = f.size() >= 2 ? f[1] : 0;
        w += 2;  // a date is written quoted
      }
      if (o.labels != nullptr) {
        // A labeled numeric is written quoted: its field is the wider of the
        // format and the longest label, plus the quotes.
        int lw = 0;
        for (const auto& kv : *o.labels) lw = std::max(lw, static_cast<int>(kv.second.size()));
        w = std::max(w, lw) + 2;
      }
      o.width = w;
    }
    oc.push_back(o);
  }

  Datastore* ds = base;
  std::unique_ptr<Datastore> filtered;
  // `outfile s g if year == 2001 using f` puts the qualifier BEFORE `using`;
  // the expression ends where `using` begins (w2ar dx_outf_if, log:4990).
  std::optional<std::string> ifExpr = parsed.ifExpr;
  if (ifExpr.has_value()) {
    static const std::regex reUsingTail(R"(\s+using\s.*$)");
    ifExpr = std::regex_replace(*ifExpr, reUsingTail, "");
  }
  if ((ifExpr.has_value() && !ifExpr->empty()) ||
      (parsed.inRange.has_value() && !parsed.inRange->empty())) {
    InRangeFilterResult fr;
    try {
      fr = applyIfAndInFilter(session, base->path(), ifExpr, parsed.inRange);
    } catch (const CobraError& ce) {
      return ioError(cobraErrorMessage(ce), ce.code);
    }
    if (!fr.ok) return inRangeRejectError(fr, parsed.inRange.value_or(std::string()), "outfile");
    filtered = std::make_unique<Datastore>(fr.dataPath);
    ds = filtered.get();
  }
  std::vector<const cecon::Column*> colp;
  for (const auto& name : cols) colp.push_back(&ds->col(name));

  auto quoted = [&](const std::string& t) { return noQuote ? t : "\"" + t + "\""; };
  auto rtrim = [](std::string t) {
    while (!t.empty() && t.back() == ' ') t.pop_back();
    return t;
  };

  std::string body;
  const int64_t n = ds->nRows();
  for (int64_t r = 0; r < n; ++r) {
    size_t budget = 0;  // each field costs its width plus 2 against the 80 columns
    if (comma) {
      // The comma form: values only, comma-separated.
      for (size_t c = 0; c < oc.size(); ++c) {
        if (c) body += ',';
        const cecon::Column& col = *colp[c];
        if (col.dtype == cecon::DType::Factor) {
          const int32_t code = col.codes()[r];
          body += quoted(code < 0 ? std::string() : col.levels[static_cast<size_t>(code)]);
          continue;
        }
        const double v = col.f64()[r];
        if (const std::string* lbl = labelTextFor(oc[c].labels, v)) {
          body += quoted(*lbl);
          continue;
        }
        body += std::isnan(v) ? std::string(".") : fmt::general(v, 10, 0);
      }
      body += '\n';
      continue;
    }
    std::string line, layout;  // `layout` = the line as quoted mode would lay it out
    for (size_t c = 0; c < oc.size(); ++c) {
      const OutCol& o = oc[c];
      const cecon::Column& col = *colp[c];
      std::string text;     // the field's characters
      bool leftAlign = false;
      bool isQuotedKind = false;
      if (col.dtype == cecon::DType::Factor) {
        const int32_t code = col.codes()[r];
        text = code < 0 ? std::string() : col.levels[static_cast<size_t>(code)];
        leftAlign = true;
        isQuotedKind = true;
      } else {
        const double v = col.f64()[r];
        if (const std::string* lbl = labelTextFor(o.labels, v)) {
          text = *lbl;
          leftAlign = true;
          isQuotedKind = true;
        } else if (std::isnan(v)) {
          text = ".";
        } else if (o.dateFamily != 0) {
          std::string d;
          try {
            d = eval::applyStataFmt(v, o.fmt);
          } catch (...) {
            d.clear();
          }
          text = trimOf(d);
          leftAlign = true;
          isQuotedKind = true;
        } else {
          std::string t;
          try {
            t = eval::applyStataFmt(v, o.fmt);
          } catch (...) {
            t = fmt::general(v, 10, 0);
          }
          text = trimOf(t);
        }
      }
      const std::string shown = isQuotedKind ? quoted(text) : text;
      const std::string laid = isQuotedKind ? "\"" + text + "\"" : text;
      const size_t w = static_cast<size_t>(o.width);
      auto pad = [&](const std::string& t, size_t width) {
        if (t.size() >= width) return t;
        return leftAlign ? t + std::string(width - t.size(), ' ')
                         : std::string(width - t.size(), ' ') + t;
      };
      const size_t fieldW = std::max(w, laid.size());
      // A record continues on a new line once its fields (each with its 2
      // separator columns) would pass 80 (dx_outf_all writes the date of
      // every record on its own line, log:969-978).
      if (!layout.empty() && !wide && budget + fieldW + 2 > 80) {
        const std::string lt = rtrim(layout);
        std::string ln = rtrim(line);
        if (ln.size() < lt.size()) ln += std::string(lt.size() - ln.size(), ' ');
        body += ln + "\n";
        line.clear();
        layout.clear();
        budget = 0;
      }
      budget += fieldW + 2;
      if (!layout.empty()) {
        line += "  ";
        layout += "  ";
      }
      // noquote: the field is two narrower, the line keeps its length at the
      // end (dx_outf_noquote_wide `       2  b c             2.02  `).
      line += pad(shown, isQuotedKind && noQuote ? (w >= 2 ? w - 2 : 0) : w);
      layout += pad(laid, w);
    }
    const std::string lt = rtrim(layout);
    std::string ln = rtrim(line);
    if (ln.size() < lt.size()) ln += std::string(lt.size() - ln.size(), ' ');
    body += ln + "\n";
  }

  if (auto err = commitBytes("outfile", resolved, pathArg, body)) return *err;
  return displayBlock("file " + pathArg + " saved");
}

// ===========================================================================
// saveold
// ===========================================================================
//
// Decision 5, and the whole reason this row exists. `saveold, version(12)` asks
// for a format this build cannot write; PIET writes 118 and SAYS so, rather
// than producing 118 bytes under a name that promises Stata 12. The version the
// user asked for is recorded as a dataset characteristic, so it survives into
// the file itself through the --in-meta sidecar and can be read back.
ojson runSaveold(const eval::ParsedCommand& parsed, SessionState& session) {
  if (auto err = eval::rejectUnknownOptions(parsed.opts, kVocabSaveold.known,
                                            kVocabSaveold.unimplemented)) {
    return ojson(*err);
  }

  const std::vector<std::string> words = quoteAwareWords(preCommaArgs(parsed.raw));
  if (words.empty()) {
    return ioSyntax("saveold: missing the file. Usage: saveold \"file\" [, replace version(#)]");
  }
  if (words.size() > 1) {
    return ioSyntax("saveold: expected one filename, got `" + words[0] + " " + words[1] + "`");
  }
  std::string spelling = words[0];
  // The extension: Stata's `saveold` defaults to `.dta`, and this build CAN
  // write one, so the default is honoured rather than substituted.
  {
    const std::filesystem::path p(cecon::to_native_path(spelling));
    if (!p.has_extension()) spelling += ".dta";
  }

  const auto version = optStr(parsed.opts, "version");
  if (version.has_value() && version->empty()) {
    return ioSyntax("saveold: version() needs a value, e.g. version(12)");
  }
  if (version.has_value()) {
    // saveold writes releases for Stata 11 through 14 only; any other
    // version() is r(198) and no file is written (w2ar dx_svo_v10
    // `version(10)` 198 conf=601, log:4219).
    long long v = 0;
    if (!parseStrictInt(*version, &v) || v < 11 || v > 14) {
      return ioErrorRc("version(" + *version + ") invalid; saveold writes Stata 11 through 14 "
                       "datasets",
                       198);
    }
  }

  // The release follows version(): 11 and 12 write format 115, 13 (and the
  // bare form) 117, 14 format 118 -- genuine bytes of that release, whatever
  // the file's extension (a tempfile included), as Stata's saveold does
  // (w2ar_data_io_family.log dx_svo_019 rel 117, dx_svo_017_v12 / dx_svo_v11
  // first byte 115, dx_svo_075_v13 117, dx_svo_v14 118).
  int release = 117;
  if (version.has_value()) {
    long long v = 0;
    (void)parseStrictInt(*version, &v);
    release = v <= 12 ? 115 : (v == 13 ? 117 : 118);
  }
  // runSaveData names the spelling it was handed (`effectiveFp`) in its echo
  // and in every refusal (BUG-410), so its block goes out as it is.
  ojson saved = runSaveData(spelling, hasFlag(parsed.opts, "replace"), session, release);
  if (isErrorBlock(saved)) return saved;

  // EXACT text, pinned by the pangram section and by a do-file stdout check.
  const std::string note = "(saveold: PIET writes Stata format " + std::to_string(release) + ")";
  ojson out = ojson::array();
  out.push_back(std::move(saved));
  // `display`, NOT `echo`: type "echo" is the channel that renders the typed
  // command line back (`. saveold "f", replace`). A note sent there reads as if
  // the USER wrote it, and nothing downstream counts it as output -- which is
  // exactly how it went missing: the file was written, the sentence was built,
  // and both selftest legs saw `<no display lines>`.
  out.push_back(displayBlock(note));
  return out;
}

// ===========================================================================
// putexcel
// ===========================================================================
//
// Decision 6. `putexcel` is the only STATEFUL command in this bundle: `set`
// names the workbook, each `A1 = ...` updates one cell of an accumulated map,
// and the whole workbook is rewritten from that map on every assignment. The
// map lives in `SessionState::putexcel` because it must survive between
// statements; the shape is the one session_state.h documents, plus a
// `spelling` field holding the filename AS THE USER TYPED IT, because every
// message and echo must name that and not the resolved path (defect class 4).
namespace {

// The state, unpacked. `ok == false` means no `putexcel set` has run.
struct PutexcelState {
  bool ok = false;
  std::string path;      // resolved -- what the writer opens
  std::string spelling;  // as typed -- what messages say
  std::string sheet = "Sheet1";
  bool noteShown = false;
};

PutexcelState readPutexcelState(SessionState& session) {
  PutexcelState st;
  const ojson& s = session.putexcel;
  if (!s.is_object() || !s.contains("path")) return st;
  st.ok = true;
  st.path = s.value("path", std::string());
  st.spelling = s.value("spelling", std::string());
  st.sheet = s.value("sheet", std::string("Sheet1"));
  st.noteShown = s.value("noteShown", false);
  return st;
}

// The accumulated cells, as the writer wants them. `cells` is an object keyed
// by upper-case A1 address; each value is {"n": <number>} or {"s": "<text>"}.
std::vector<xlsx::CellOut> cellsOf(const ojson& state) {
  std::vector<xlsx::CellOut> out;
  if (!state.is_object() || !state.contains("cells") || !state["cells"].is_object()) return out;
  for (auto it = state["cells"].begin(); it != state["cells"].end(); ++it) {
    CellAddr a;
    if (!parseAddr(it.key(), &a)) continue;
    xlsx::CellOut c;
    c.row = a.row;
    c.col = a.col;
    const ojson& v = it.value();
    if (v.is_object() && v.contains("n") && v["n"].is_number()) {
      c.isNumber = true;
      c.number = v["n"].get<double>();
    } else if (v.is_object() && v.contains("s") && v["s"].is_string()) {
      c.text = v["s"].get<std::string>();
    } else {
      continue;
    }
    out.push_back(c);
  }
  return out;
}

std::vector<std::string> mergesOf(const ojson& state) {
  std::vector<std::string> out;
  if (!state.is_object() || !state.contains("merges") || !state["merges"].is_array()) return out;
  for (const auto& m : state["merges"]) {
    if (m.is_string()) out.push_back(m.get<std::string>());
  }
  return out;
}

// Rewrite the workbook from the accumulated map. nullopt = written.
std::optional<ojson> rewriteWorkbook(SessionState& session, const PutexcelState& st) {
  try {
    xlsx::writeCells(st.path, st.sheet, cellsOf(session.putexcel), mergesOf(session.putexcel));
  } catch (const std::exception& e) {
    // The writer's message names the RESOLVED path; re-word it around the
    // spelling the user typed.
    std::string why = e.what();
    const size_t at = why.find(st.path);
    if (at != std::string::npos) {
      why = why.substr(0, at) + st.spelling + why.substr(at + st.path.size());
    }
    return ioError("putexcel: " + why, "data_io");
  }
  return std::nullopt;
}

// How many worksheet parts a .xlsx names.
//
// The reader in xlsx.h hands back the FIRST worksheet and says nothing about
// how many there are, and this bundle's manifest does not include xlsx.h, so
// there is no sheet-inventory call to make. What CAN be done without one is
// count the worksheet part names, which a ZIP stores UNCOMPRESSED in its own
// headers whatever it does with the parts themselves. That is enough to answer
// the only question that matters here -- "would rewriting this file throw away
// sheets the user still has?" -- and it errs toward refusing, never toward
// silently dropping a sheet. Returns 0 when the file cannot be read at all.
// The byte-scan that used to live here is GONE, and its removal is the point:
// it searched the raw file for "xl/worksheets/" and stopped at the first quote,
// '<' or control byte -- but a STORED zip places the next record's "PK"
// signature immediately after the filename, so it read `sheet1.xmlPK` as a
// second distinct sheet. Every workbook this build writes therefore looked
// multi-sheet, and `putexcel set ..., modify` refused all of them. Replaced by
// `xlsx::worksheetCount`, which uses the same ZIP reader `xlsx::read` does.

// Scan one `putexcel` VALUE starting at `i`, leaving `i` just past it.
// Returns false when the text at `i` is not a value this grammar accepts.
bool scanValue(const std::string& s, size_t& i, std::string* out, char* kind) {
  while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i]))) ++i;
  if (i >= s.size()) return false;
  if (s[i] == '"') {
    std::string tok;
    ++i;
    while (i < s.size() && s[i] != '"') {
      tok.push_back(s[i]);
      ++i;
    }
    if (i >= s.size()) return false;  // unterminated
    ++i;
    *out = tok;
    *kind = 's';
    return true;
  }
  // `matrix(NAME)` and `(expr)` both end at the matching close paren.
  const std::string rest = lowerOf(s.substr(i, 7));
  const bool isMatrix = rest.compare(0, 7, "matrix(") == 0;
  const size_t open = isMatrix ? i + 6 : i;
  if (s[open] == '(') {
    int depth = 0;
    size_t j = open;
    for (; j < s.size(); ++j) {
      if (s[j] == '(') ++depth;
      else if (s[j] == ')') {
        --depth;
        if (depth == 0) break;
      }
    }
    if (j >= s.size()) return false;  // unbalanced
    *out = s.substr(open + 1, j - open - 1);
    *kind = isMatrix ? 'm' : 'e';
    i = j + 1;
    return true;
  }
  std::string tok;
  while (i < s.size() && !std::isspace(static_cast<unsigned char>(s[i]))) {
    tok.push_back(s[i]);
    ++i;
  }
  if (tok.empty()) return false;
  *out = tok;
  *kind = 'n';
  return true;
}

}  // namespace

ojson runPutexcel(const std::string& rawCmd, SessionState& session) {
  // The RAW line, because the tokenizer splits on `=` and on the comma inside
  // `matrix(M), names`. So the option tail has to be found and parsed here too
  // -- and then run through the SAME reject net every other handler uses, so a
  // typo is caught rather than dropped (defect class 3).
  std::string statement = rawCmd;
  std::vector<std::pair<std::string, eval::OptValue>> opts;
  {
    const long long comma = eval::findUnquotedComma(statement);
    if (comma >= 0) {
      const std::string tail = statement.substr(static_cast<size_t>(comma) + 1);
      statement = statement.substr(0, static_cast<size_t>(comma));
      if (!trimOf(tail).empty()) {
        try {
          opts = eval::parseOpts(tail);
        } catch (const eval::TokenizerError& te) {
          return ojson(te.block);
        }
      }
    }
  }
  // The dispatcher canonicalizes `parsed.opts`; this handler never sees that
  // vector, so it runs the SAME resolution against the SAME registered
  // vocabulary before its own net -- otherwise an abbreviation the net accepts
  // would still reach the `k == "replace"` comparisons below unresolved.
  eval::canonicalizeForCommand(opts, "putexcel");
  if (auto err = eval::rejectUnknownOptions(opts, kVocabPutexcel.known,
                                            kVocabPutexcel.unimplemented)) {
    return ojson(*err);
  }

  statement = dropCommandWord(statement);
  if (statement.empty()) {
    return ioSyntax("putexcel: nothing to do. Usage: putexcel set \"file\" [, replace|modify "
                    "sheet(S)] | putexcel A1 = <value> ... | putexcel A1:B2, merge | putexcel "
                    "clear|describe|save");
  }

  // The formatting note, at most once per `set` (decision 6). Fired here so it
  // covers a formatting option given ON the `set` line as well as on a cell
  // assignment.
  bool sawFormatOpt = false;
  for (const auto& [k, v] : opts) {
    (void)v;
    if (putexcelFormatOpts().count(k) != 0) {
      sawFormatOpt = true;
      break;
    }
  }

  const std::vector<std::string> words = quoteAwareWords(statement);
  const std::string head = words.empty() ? std::string() : lowerOf(words[0]);
  ojson out = ojson::array();

  // ---- putexcel set FILE [, replace|modify sheet(S)] ---------------------
  if (head == "set") {
    if (words.size() < 2) return ioSyntax("putexcel set: missing the workbook filename");
    if (words.size() > 2) {
      return ioSyntax("putexcel set: expected one filename, got `" + words[1] + " " + words[2] +
                      "`");
    }
    const std::string spelling = words[1];
    const std::string resolved = resolveUserPath(session, spelling);
    const bool replace = hasFlag(opts, "replace");
    const bool modify = hasFlag(opts, "modify");
    if (replace && modify) {
      return ioSyntax("putexcel set: `replace` and `modify` cannot both be given");
    }
    const bool exists = pathExists(resolved);
    // Stata's own default for an existing file is `modify`.
    const bool wantModify = modify || (exists && !replace);
    std::string sheet = optStr(opts, "sheet").value_or(std::string("Sheet1"));
    if (sheet.empty()) sheet = "Sheet1";

    ojson state = ojson::object();
    state["path"] = resolved;
    state["spelling"] = spelling;
    state["sheet"] = sheet;
    state["mode"] = wantModify ? "modify" : "replace";
    state["noteShown"] = false;
    state["cells"] = ojson::object();
    state["merges"] = ojson::array();

    if (wantModify && exists) {
      if (xlsx::worksheetCount(resolved) > 1) {
        return ioRefusal("putexcel", "single_sheet_only",
                         "putexcel: this build writes single-sheet workbooks, and " + spelling +
                             " has more than one worksheet — rewriting it would discard the "
                             "others. Write to a new file instead.");
      }
      if (optStr(opts, "sheet").has_value()) {
        // The reader available here cannot say what the existing sheet is
        // CALLED, so honouring `sheet()` would mean renaming whatever is there
        // and hoping it was the right one.
        return ioRefusal("putexcel", "single_sheet_only",
                         "putexcel: this build writes single-sheet workbooks, so `sheet()` cannot "
                         "be combined with modifying an existing workbook — drop `sheet()`, or "
                         "write to a new file with `replace`.");
      }
      xlsx::Sheet loaded;
      try {
        loaded = xlsx::read(resolved, false);
      } catch (const std::exception&) {
        return ioError("putexcel: " + spelling + " is not a workbook this build can read");
      }
      for (size_t r = 0; r < loaded.rows.size(); ++r) {
        for (size_t c = 0; c < loaded.rows[r].size(); ++c) {
          const xlsx::Cell& cell = loaded.rows[r][c];
          if (cell.blank) continue;
          ojson v = ojson::object();
          if (cell.isNumber)
            v["n"] = cell.number;
          else
            v["s"] = cell.text;
          state["cells"][addrOf(static_cast<int>(r) + 1, static_cast<int>(c) + 1)] = std::move(v);
        }
      }
      out.push_back(displayBlock(
          "(putexcel: " + spelling +
          " was read for its VALUES; any formatting it carried is not preserved by this build)"));
    }

    session.putexcel = std::move(state);
    if (sawFormatOpt) {
      session.putexcel["noteShown"] = true;
      out.push_back(displayBlock("(putexcel: cell formatting options are accepted and ignored in "
                                 "this build — values only)"));
    }
    return out;
  }

  // ---- putexcel clear ----------------------------------------------------
  if (head == "clear") {
    session.putexcel = ojson::object();
    return silent();
  }

  PutexcelState st = readPutexcelState(session);
  if (!st.ok) {
    return ioError("putexcel set must be called first — name the workbook with `putexcel set "
                   "\"file\"` before " +
                   std::string(head == "describe" ? "describing it" : "writing a cell") + ".");
  }

  // ---- putexcel describe -------------------------------------------------
  if (head == "describe") {
    out.push_back(displayBlock("putexcel: workbook " + st.spelling));
    out.push_back(displayBlock("putexcel: sheet     " + st.sheet));
    out.push_back(displayBlock(
        "putexcel: cells     " +
        std::to_string(session.putexcel.contains("cells") ? session.putexcel["cells"].size() : 0)));
    return out;
  }

  // ---- putexcel save / open ---------------------------------------------
  if (head == "save" || head == "open") {
    // Not a no-op that hides something: the workbook on disk is already current
    // (every assignment rewrites it in full), so there is nothing left to do
    // and saying so is the honest answer.
    return displayBlock("(putexcel: " + st.spelling +
                        " is rewritten after every assignment, so `" + head +
                        "` has nothing left to do)");
  }

  if (sawFormatOpt && !st.noteShown) {
    session.putexcel["noteShown"] = true;
    out.push_back(displayBlock("(putexcel: cell formatting options are accepted and ignored in "
                               "this build — values only)"));
  }

  // ---- putexcel A1:B2 [, merge] -----------------------------------------
  if (words.size() == 1 && statement.find('=') == std::string::npos) {
    const std::string range = words[0];
    const size_t colon = range.find(':');
    if (colon == std::string::npos) {
      return ioSyntax("putexcel: `" + range +
                      "` — expected `A1 = <value>`, a range like `A1:B2, merge`, or one of "
                      "set/save/clear/describe");
    }
    CellAddr a, b;
    if (!parseAddr(range.substr(0, colon), &a) || !parseAddr(range.substr(colon + 1), &b)) {
      return ioSyntax("putexcel: `" + range + "` is not a cell range");
    }
    if (b.row < a.row || b.col < a.col) {
      return ioSyntax("putexcel: `" + range +
                      "` runs backwards — name the top-left cell first");
    }
    if (!hasFlag(opts, "merge")) {
      return ioSyntax("putexcel: `" + range +
                      "` names a range but says nothing to do with it — add `, merge`");
    }
    const std::string canon = addrOf(a.row, a.col) + ":" + addrOf(b.row, b.col);
    bool already = false;
    for (const auto& m : session.putexcel["merges"]) {
      if (m.is_string() && m.get<std::string>() == canon) already = true;
    }
    if (!already) session.putexcel["merges"].push_back(canon);
    if (auto err = rewriteWorkbook(session, st)) return *err;
    return out;
  }

  // ---- putexcel A1 = <value> [B1 = <value> ...] --------------------------
  {
    size_t i = 0;
    size_t assignments = 0;
    auto setCell = [&](int row, int col, bool isNumber, double num, const std::string& text) {
      ojson v = ojson::object();
      if (isNumber)
        v["n"] = num;
      else
        v["s"] = text;
      session.putexcel["cells"][addrOf(row, col)] = std::move(v);
    };
    while (true) {
      while (i < statement.size() && std::isspace(static_cast<unsigned char>(statement[i]))) ++i;
      if (i >= statement.size()) break;
      size_t j = i;
      while (j < statement.size() && !std::isspace(static_cast<unsigned char>(statement[j])) &&
             statement[j] != '=') {
        ++j;
      }
      const std::string addrTok = statement.substr(i, j - i);
      CellAddr at;
      if (!parseAddr(addrTok, &at)) {
        return ioSyntax("putexcel: `" + addrTok +
                        "` is not a cell address (expected something like `A1`)");
      }
      i = j;
      while (i < statement.size() && std::isspace(static_cast<unsigned char>(statement[i]))) ++i;
      if (i >= statement.size() || statement[i] != '=') {
        return ioSyntax("putexcel: expected `=` after " + addrTok);
      }
      ++i;
      std::string value;
      char kind = 0;
      if (!scanValue(statement, i, &value, &kind)) {
        return ioSyntax("putexcel: " + addrTok +
                        " = <value> — expected a quoted string, a number, `(expression)` or "
                        "`matrix(NAME)`");
      }
      ++assignments;
      if (kind == 's') {
        setCell(at.row, at.col, false, 0.0, value);
      } else if (kind == 'n') {
        double d = 0.0;
        if (!parseStrictDouble(value, &d)) {
          return ioSyntax("putexcel: `" + value +
                          "` is neither a number nor a quoted string — quote it to write it as "
                          "text");
        }
        setCell(at.row, at.col, true, d, std::string());
      } else if (kind == 'e') {
        std::optional<double> d = evalScalarExpr(value, session);
        if (!d.has_value()) {
          return ioError("putexcel: (" + value + ") does not resolve to a number");
        }
        setCell(at.row, at.col, true, *d, std::string());
      } else {  // 'm' -- matrix(NAME)
        const std::string mname = trimOf(value);
        const eval::EvalMatrix* m = session.eval.findMatrix(mname);
        if (m == nullptr) return ioErrorRc("putexcel: matrix " + mname + " not found", 111);
        const bool names = hasFlag(opts, "names");
        int r0 = at.row;
        if (names) {
          // Column names across the top, row names down the left -- so the
          // block starts one row and one column in from the address given.
          for (size_t c = 0; c < m->colnames.size(); ++c) {
            setCell(r0, at.col + 1 + static_cast<int>(c), false, 0.0, m->colnames[c]);
          }
          ++r0;
        }
        for (size_t r = 0; r < m->data.size(); ++r) {
          const int rowNo = r0 + static_cast<int>(r);
          int c0 = at.col;
          if (names) {
            setCell(rowNo, c0, false, 0.0,
                    r < m->rownames.size() ? m->rownames[r] : std::string());
            ++c0;
          }
          for (size_t c = 0; c < m->data[r].size(); ++c) {
            const double v = m->data[r][c];
            // Missing is `.`, never 0.
            if (std::isnan(v))
              setCell(rowNo, c0 + static_cast<int>(c), false, 0.0, ".");
            else
              setCell(rowNo, c0 + static_cast<int>(c), true, v, std::string());
          }
        }
      }
    }
    if (assignments == 0) {
      return ioSyntax("putexcel: `" + statement +
                      "` — expected `A1 = <value>`, a range like `A1:B2, merge`, or one of "
                      "set/save/clear/describe");
    }
  }

  if (auto err = rewriteWorkbook(session, st)) return *err;
  return out;
}

// ===========================================================================
// filefilter
// ===========================================================================
//
// Decision 7. A whole-file search/replace with Stata's own escape vocabulary,
// publishing r(occurrences). The input and output must be DIFFERENT files: this
// command reads one and writes the other, and letting them be the same would
// mean truncating the source before it has been fully read.
namespace {

// Stata's `filefilter` escapes. The three-character tokens are tested first, so
// `\BS` never resolves as `\B` + `S`.
std::string unescapeFilter(const std::string& s) {
  std::string out;
  for (size_t i = 0; i < s.size(); ++i) {
    if (s[i] != '\\' || i + 1 >= s.size()) {
      out.push_back(s[i]);
      continue;
    }
    const std::string three = s.substr(i, 3);
    if (three == "\\BS") { out.push_back('\\'); i += 2; continue; }
    if (three == "\\LQ") { out.push_back('`');  i += 2; continue; }
    if (three == "\\RQ") { out.push_back('\''); i += 2; continue; }
    switch (s[i + 1]) {
      case 'Q': out.push_back('"'); ++i; continue;
      case '$': out.push_back('$'); ++i; continue;
      case 'n': out.push_back('\n'); ++i; continue;
      case 'r': out.push_back('\r'); ++i; continue;
      case 't': out.push_back('\t'); ++i; continue;
      case 'M': out.push_back('\r'); ++i; continue;
      case 'U': out.push_back('\n'); ++i; continue;
      case 'W': out += "\r\n"; ++i; continue;
      case '\\': out.push_back('\\'); ++i; continue;
      default: out.push_back(s[i]); continue;
    }
  }
  return out;
}

constexpr std::size_t kFilefilterCap = 256u * 1024u * 1024u;  // 256 MiB

}  // namespace

ojson runFilefilter(const eval::ParsedCommand& parsed, SessionState& session) {
  if (auto err = eval::rejectUnknownOptions(parsed.opts, kVocabFilefilter.known,
                                            kVocabFilefilter.unimplemented)) {
    return ojson(*err);
  }

  const std::vector<std::string> words = quoteAwareWords(preCommaArgs(parsed.raw));
  if (words.size() != 2) {
    return ioSyntax("filefilter: expected two filenames. Usage: filefilter oldfile newfile, "
                    "from(\"...\") to(\"...\") [replace]");
  }
  const auto fromOpt = optStr(parsed.opts, "from");
  const auto toOpt = optStr(parsed.opts, "to");
  if (!fromOpt.has_value()) return ioSyntax("filefilter: from() is required");
  if (!toOpt.has_value()) return ioSyntax("filefilter: to() is required");
  const std::string from = unescapeFilter(*fromOpt);
  const std::string to = unescapeFilter(*toOpt);
  if (from.empty()) {
    return ioSyntax("filefilter: from() cannot be empty — there would be a match between every "
                    "pair of characters");
  }

  const std::string inSpelling = words[0];
  const std::string outSpelling = words[1];
  const std::string inPath = resolveUserPath(session, inSpelling);
  const std::string outPath = resolveUserPath(session, outSpelling);
  if (!pathExists(inPath)) {
    return ioErrorRc("file " + typedUserPath(inSpelling) + " not found", 601);
  }
  // "Is the destination also a source?" -- here it can be, and must not be.
  if (sameFile(inPath, outPath)) {
    return ioError("filefilter: the input and the output must be different files (" + inSpelling +
                   " names both) — filter into a new file and rename it afterwards.");
  }
  if (auto err = refuseExisting(outPath, outSpelling, hasFlag(parsed.opts, "replace"))) {
    return *err;
  }
  if (auto err = refuseIfSessionHbin(session, "filefilter", outPath, outSpelling)) return *err;

  {
    std::error_code ec;
    const auto sz = std::filesystem::file_size(cecon::to_native_path(inPath), ec);
    if (!ec && sz > kFilefilterCap) {
      return ioError("filefilter: " + inSpelling + " is " + std::to_string(sz) +
                     " bytes; this build filters files up to 268435456 bytes (it holds the whole "
                     "file in memory).");
    }
  }
  auto bytes = readWholeFile(inPath);
  if (!bytes.has_value()) return ioError("filefilter: cannot read " + inSpelling, "data_io");

  std::string result;
  result.reserve(bytes->size());
  long long occurrences = 0;
  size_t at = 0;
  while (true) {
    const size_t hit = bytes->find(from, at);
    if (hit == std::string::npos) {
      result.append(*bytes, at, std::string::npos);
      break;
    }
    result.append(*bytes, at, hit - at);
    result += to;
    at = hit + from.size();
    ++occurrences;
  }

  if (auto err = commitBytes("filefilter", outPath, outSpelling, result)) return *err;

  // r-class (defect class 5): the arm has no table block to capture from, so
  // the handler publishes r(occurrences) itself.
  session.eval.rValues.set("occurrences", static_cast<double>(occurrences));
  return displayBlock("(" + std::to_string(occurrences) + " occurrence" +
                      (occurrences == 1 ? "" : "s") + " replaced)");
}

// ===========================================================================
// copy
// ===========================================================================
//
// Decision 8. A local file copy. A URL source is refused BY NAME rather than
// attempted: this build makes no network request from a Stata verb, and a
// `copy` that quietly produced nothing (or, worse, an empty file) would be the
// silent-failure shape.
ojson runCopy(const eval::ParsedCommand& parsed, SessionState& session) {
  if (auto err = eval::rejectUnknownOptions(parsed.opts, kVocabCopy.known,
                                            kVocabCopy.unimplemented)) {
    return ojson(*err);
  }

  const std::vector<std::string> words = quoteAwareWords(preCommaArgs(parsed.raw));
  if (words.size() != 2) {
    return ioSyntax("copy: expected two filenames. Usage: copy from to [, replace]");
  }
  const std::string srcSpelling = words[0];
  const std::string dstSpelling = words[1];

  {
    const std::string low = lowerOf(srcSpelling);
    if (low.compare(0, 7, "http://") == 0 || low.compare(0, 8, "https://") == 0 ||
        low.compare(0, 6, "ftp://") == 0) {
      return ioRefusal("copy", "no_network",
                       "copy: URL sources are not supported in this build (no network access from "
                       "Stata verbs)");
    }
  }

  const std::string src = resolveUserPath(session, srcSpelling);
  std::string dstSpell = dstSpelling;
  std::string dst = resolveUserPath(session, dstSpelling);
  if (!pathExists(src)) return ioErrorRc("file " + typedUserPath(srcSpelling) + " not found", 601);
  {
    std::error_code ec;
    if (std::filesystem::is_directory(cecon::to_native_path(dst), ec) && !ec) {
      const std::filesystem::path p(cecon::to_native_path(src));
      dst = (std::filesystem::path(cecon::to_native_path(dst)) / p.filename()).string();
      dstSpell = dstSpelling + "/" + p.filename().string();
    }
  }
  if (sameFile(src, dst)) {
    return ioError("copy: the source and the destination are the same file (" + srcSpelling + ")");
  }
  const bool replace = hasFlag(parsed.opts, "replace");
  if (auto err = refuseExisting(dst, dstSpell, replace)) return *err;
  if (auto err = refuseIfSessionHbin(session, "copy", dst, dstSpell)) return *err;

  // Sibling temp + rename, so a failed copy never leaves a half file where a
  // readable one used to be.
  const std::string tmp = dst + ".piet_tmp";
  {
    std::error_code ec;
    std::filesystem::remove(cecon::to_native_path(tmp), ec);
    ec.clear();
    std::filesystem::copy_file(cecon::to_native_path(src), cecon::to_native_path(tmp),
                               std::filesystem::copy_options::overwrite_existing, ec);
    if (ec) {
      std::error_code rmEc;
      std::filesystem::remove(cecon::to_native_path(tmp), rmEc);
      return ioError("copy: " + srcSpelling + " -> " + dstSpell + ": " + ec.message(), "data_io");
    }
  }
  {
    std::error_code ec;
    std::filesystem::rename(cecon::to_native_path(tmp), cecon::to_native_path(dst), ec);
    if (ec) {
      std::error_code rmEc;
      std::filesystem::remove(cecon::to_native_path(tmp), rmEc);
      return ioError("copy: " + srcSpelling + " -> " + dstSpell + ": " + ec.message(), "data_io");
    }
  }
  return silent();  // Stata's copy prints nothing
}

}  // namespace piet::handlers
