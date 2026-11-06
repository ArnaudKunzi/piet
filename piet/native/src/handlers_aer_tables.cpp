// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// AER-SCOPE1 bundle E (TABLES, 2026-09-04) -- the `regress -> estadd -> eststo
// -> estout|esttab using f.tex` pipeline, plus the other community table
// commands that leave a FILE behind.
//
//   estadd      attach a local/scalar/matrix to the ACTIVE e()
//   estpost     a pseudo-estimate whose e(mean)/e(sd)/... are 1xk matrices
//   estout      the stored estimates as TeX / fixed-width / delimited
//   outreg2     the same, plus a .txt ledger that `append` reads back
//   texsave     the DATASET as a LaTeX table
//   texdoc/tex  an open .tex handle plus the line verb that writes into it
//   frmttable   a session MATRIX as a LaTeX table (TeX only; RTF refused)
//   regsave     the estimates as a DATASET
//   parmest     the same builder, parmest's column vocabulary
//   mat2txt     a session matrix as tab-separated text
//   erepost     overwrite the active e(b) from a matrix
//
// WHY ONE TU. Every one of these renders THE SAME THING -- a table of stored
// estimates -- and the plan's design decision 5 is binding: ONE `Table` model
// and THREE renderers (`renderTex` / `renderFixed` / `renderDelim`), shared by
// estout, esttab-using, outreg2, frmttable and texsave. The rejected
// alternative was a bespoke `\begin{tabular}` builder per command: five string
// assemblers means five goldens that drift apart one bug-fix at a time, and
// this codebase has paid for that shape before (the two e()-scalar tables, the
// four mutationResult copies).
//
// THE FIVE DEFECT CLASSES THIS FILE IS WRITTEN AGAINST. Each shipped at least
// once during this campaign; the first has been caught three times in four
// bundles, so it is named first.
//
//  1. SHAPE. A handler returning a JSON ARRAY where the arm pushes ONE block:
//     nlohmann throws type_error.306 the moment a consumer reads ["type"], and
//     that aborted the whole selftest binary. dispatcher.cpp's bundle-E arms
//     route through `pushEnvResult`, which flattens an array and drops nulls,
//     so an array IS legal here -- but ONLY an array whose every element is a
//     real block object carrying a "type" key. Every `return` below is either
//     one object with "type", or an ojson::array() of such objects.
//  2. RC. `eval::coarseRc` answers 199 for `unsupported_command` or a `r(NNN)`
//     reason and 198 for `syntax_error`; a refusal built with the default
//     `engine_error` code silently becomes rc 1. So: command-level refusals go
//     through `eval::refusedCommand(cmd, cause)`, sub-verb refusals through
//     `eval::unsupportedCommand(..., kNotImplemented)`, USER SYNTAX errors
//     carry code "syntax_error" (rc 198), and anything with a known Stata rc
//     that coarseRc's message rules do not recognise states it through the
//     `reason` channel (bundle C had to repair six sites that defaulted to 1).
//  3. VOCABULARY. `rejectUnknownOptions` prefix-resolves and therefore ACCEPTS
//     abbreviations, while `canonicalizeForCommand` -- the thing that makes the
//     handler's own `k == "replace"` compare see them -- is driven by the
//     REGISTRY. An unregistered command gets its abbreviations accepted by the
//     net and then dropped unread: `outreg2 ..., repl` would run without
//     `replace` and say nothing. All twelve command names register at NAMESPACE
//     scope below (function-local statics construct too late -- after the first
//     call that needed them), and every handler's first act is the reject net.
//  4. ALIASING. A rewrite must move BOTH the routing name and the reported
//     name. No aliases in this bundle, so this class is structurally absent;
//     the one place `parsed` is reinterpreted (estadd/estpost/texdoc sub-verbs)
//     never rewrites `parsed.cmd`.
//  5. R-CLASS FORWARDING. r() is filled at the dispatcher CALL SITE, not in the
//     handler. None of these twelve verbs publishes r() in Stata (estpost and
//     estadd write e(), which this build carries in the estimates snapshot),
//     so no arm here needs `captureRFromBlock`.
//
// ------------------------------------------------------------------------
// THE SEAMS WITH THE FILES THIS LANE DOES NOT OWN.
//
//  (a) `runEststo` (eststo_esttab.cpp) attaches `snapshot["escalars"]` and
//      `snapshot["added"]` at store time, discarding a stale bag against the
//      SHARED fingerprint. That fingerprint is `SessionState::estAddFingerprint`
//      -- one function, written once, WRITTEN here by `estadd` and READ there by
//      `eststo`. Two private copies differing by one field would make the bag
//      never match, and the symptom would be an `estadd`ed row quietly missing
//      from the table at rc 0, so this file must never grow its own.
//      `materializeModel` below still has a read-time reconstruction path. It is
//      a BACKSTOP, not the mechanism: a snapshot that carries the fields wins
//      outright, and the fallback only fires for an estimate stored by some
//      other route (a snapshot built before this bundle, or one assembled from
//      LastModel's cached result rather than from lastEstBlock).
//
//  (b) `runEsttab` falls back to the ACTIVE estimate when no models are named
//      and the store is empty, which is what makes `estpost summarize x y` +
//      a bare `esttab` work. Two consequences land in THIS file:
//      `runEstpost` posts to `session.lastEstBlock` and never calls
//      `session.eststo(...)` (Stata's estpost stores nothing, and minting an
//      `m1` would change what a later `eststo` numbers itself); and
//      `writeEsttabFile` resolves the active estimate directly when the name it
//      is handed is not in the store, instead of reporting "model not found"
//      for a table that just rendered on screen.
#include "estimators/postest_names.h"  // interaction_key (test/lincom name rule)
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

#include "io/win_compat.h"  // cecon::to_native_path / native_exists / native_unlink
#include "piet/cobra_runner.h"
#include "piet/datastore.h"
#include "piet/dispatcher.h"  // estadd mean: `summarize ... if e(sample)`
#include "piet/display_hardening.h"  // piet::evalScalarExpr
#include "piet/eval/errors.h"
#include "piet/eval/exprfns.h"  // eval::applyStataFmt (estout vFormat)
#include "piet/eval/macros.h"  // piet::eval::eScalarKey
#include "piet/eval/session.h"
#include "piet/eval/tokenizer.h"
#include "piet/inrange_filter.h"
#include "piet/loops.h"  // loops::expandVarlist (texsave varlist)
#include "piet/mutation_result.h"
#include "piet/scratch_seq.h"
#include "piet/session_state.h"
#include "piet/strutil.h"

// ---------------------------------------------------------------------------
// OPTION VOCABULARIES -- namespace scope, before any dispatch (defect class 3).
//
// The `known` sets are the plan's command scope table verbatim; the
// `unimplemented` sets are the plan's "refusal options (named)" column. The
// split matters: an option in NEITHER set reads as "unknown option" (which is
// the wrong diagnosis for a real Stata option this build simply does not do),
// and an option wrongly placed in `known` is accepted and silently ignored,
// which is the defect this whole apparatus exists to prevent.
namespace {

const piet::eval::OptVocab& kVocabEstadd = piet::eval::registerOptVocab(
    {"estadd"},
    // W2AK: estadd_ysumm's statistic flags (estadd).
    {{"replace", "prefix", "quietly", "mean", "sum", "min", "max", "range", "sd", "var", "cv",
      "semean", "skewness", "kurtosis", "median", "p1", "p5", "p10", "p25", "p50", "p75", "p90",
      "p95", "p99", "iqr", "q", "all"},
     {}});

const piet::eval::OptVocab& kVocabEstpost = piet::eval::registerOptVocab(
    {"estpost"},
    {{"statistics", "stat", "stats", "columns", "by", "listwise", "nototal", "quietly", "detail"},
     // `elabels` asks for by-group VALUE LABELS in the posted matrix names.
     // Declared unimplemented rather than accepted: accepting it would name the
     // rows by their numeric codes while the user asked for labels, and the
     // difference is invisible in the resulting table.
     {"casewise", "missing", "elabels", "escal"}});

const piet::eval::OptVocab& kVocabEstout = piet::eval::registerOptVocab(
    {"estout"},
    {{"style", "replace", "append", "cells", "b", "se", "t", "p", "fmt", "stats", "mlabels",
      "collabels", "keep", "drop", "order", "star", "starlevels", "varlabels", "labels", "label",
      "title", "note",
      // B4-W5: the discriminating pair the oracle itself identified. `estout
      // nonumbers` alone produced a file byte-identical to the plain arm
      // (pins_oracle_t2.do's own header says so), because estout's DEFAULT
      // header is the model NAME -- so the arm that separates them is
      // `numbers` vs `nonumbers`, e_numbers.txt vs e_numbers_off.txt. Before
      // this both spellings were unknown options and answered rc 198.
      "numbers", "nonumbers",
      // `transform()` -- 12 of the 18 corpus uses are on this command. `eform` is
      // NOT added with it: it is measurably `transform(exp(@) exp(@))` PLUS its
      // own removal of `_cons` (w1b_transform.log u2 has no intercept row where
      // u1 does), it has zero corpus uses on either writer, and accepting it as
      // an alias would drop that second half silently.
      "transform",
      // W1a-4: the two geometry knobs of `style(tex)`. Both are MINIMA -- a
      // longer label or number overflows unpadded (w1b_estout_tex2.log s25,
      // w1b_estout_tex.log rc) -- and `nolabel` is here because it is what
      // decides the stub's default between 20 and 12, winning over `label` when
      // both are given (w1b_estout_opts.log q3).
      "varwidth", "modelwidth", "nolabel",
      // W1a-4, all measured in w1b_estout_opts{,2}.log. The four seam hooks are
      // ADDITIVE here, unlike esttab's, where they replace a block (BUG-500);
      // `begin()` prefixes every row; `legend` prints the ladder DESCENDING and
      // only when the cells carry stars; `extracols()` inserts empty columns
      // after the columns it names; `rename()` is a row-label map whose misses
      // are no-ops (y b).
      "prehead", "posthead", "prefoot", "postfoot", "begin", "legend", "extracols", "rename",
      // W2AK (2026-10-21): estout now runs on the derived writer (eoRender),
      // so the rest of estout's option list and MoreOptions' switches
      // are honoured as the ado reads them.
      "unstack", "nounstack", "eqlabels", "indicate", "refcat", "substitute", "delimiter",
      "end", "lastend", "mgroups", "labcol2", "incelldelimiter", "interaction", "hlinechar",
      "lz", "nolz", "abbrev", "noabbrev", "wrap", "nowrap", "omitted", "noomitted",
      "baselevels", "nobaselevels", "nolegend", "type", "notype", "matrix", "showtabs",
      "noshowtabs", "outfilenoteoff", "nooutfilenoteoff", "level"},
     {"transpose", "wide", "eform", "equations", "margin", "discrete", "dropped", "dmarker",
      "msign", "stardrop", "starkeep", "stardetach", "topfile", "bottomfile", "defaults",
      "smcltags", "smclrules", "smclmidrules", "smcleqrules"}});
// `esttab`'s option vocabulary is NOT registered here. It has exactly one owner
// (`eststo_esttab.cpp`'s kVocabEsttab), because a command whose option set is
// declared in two translation units is a command with two answers about what it
// accepts. `cells` and `p` are listed there as UNIMPLEMENTED: `runEsttab` reads
// nostar/b/se/t/star and neither of those two, so accepting them would be the
// accept-and-ignore class this campaign exists to prevent (bundle A shipped it
// once as `ivreg ..., ro`). `estout` DOES implement cells() -- that handler is
// in this file and reads the spec -- so the honest statement is per command,
// not per option name.

const piet::eval::OptVocab& kVocabOutreg2 = piet::eval::registerOptVocab(
    {"outreg2"},
    {{"replace", "append", "ctitle", "addtext", "addstat", "label", "tex", "excel", "nocons",
      "keep", "dec", "bdec", "sdec", "nor2", "noobs", "adjr2", "noaster",
      // w2at_table_writers (outreg2.ado's own syntax lines :418-468): the notes
      // pair, the R-squared and p-value decimals, drop() and sortvar().
      "nonotes", "addnote", "rdec", "pdec", "drop", "sortvar",
      // B4-W5: the four content options, each pinned to a captured arm in
      // tests/eval/golden/aer_b4/tables/ (o_tstat, o_tstat_tdec4, o_auto4,
      // o_stats). `noauto` comes with them because it is the switch outreg2's
      // own source uses to turn the adaptive number format off.
      "tstat", "tdec", "auto", "noauto", "stats",
      // `adec(#)` is the added-statistic decimal count -- 106 corpus uses over
      // three papers. `afmt()` is deliberately NOT here: it is the ado's only
      // escape from the separator bug, it has ZERO corpus uses, and its non-`f`
      // suffixes are unmeasured, so it stays an unknown option rather than an
      // accepted one this build would have to guess at.
      "adec",
      // `text` asks for the plain-text ledger, which this build ALWAYS writes
      // (the `.txt` file is its append source of truth), so it is accepted and
      // is a no-op rather than a rejection. Every one of the five captured
      // outreg2 arms spells it.
      "text",
      // BUG-1467: `se` is the positive form of outreg2's noSE option (the
      // default; listed so it is not read as an abbreviation of `seeout`), and
      // `nose` drops the standard-error rows (outreg2 se_skip).
      "se", "nose"},
     {"word", "alpha", "e", "sideway", "paren", "bracket", "onecol", "seeout", "symbol",
      "title"}});

const piet::eval::OptVocab& kVocabTexsave = piet::eval::registerOptVocab(
    {"texsave"},
    {{"replace", "title", "headerlines", "frag", "hlines", "align", "nofix", "footnote",
      "varlabels",
      // W2AK TW-TEXSAVE: the rest of texsave's syntax line, derived from it.
      "size", "width", "marker", "label", "location", "geometry", "landscape", "nonames",
      "autonumber", "rowsep", "sw", "noendash", "delimiter", "preamble", "headlines",
      "tablelines", "headerlines2", "footlines", "rowstretch", "rowheight", "colwidth",
      "headersep", "dataonly"},
     {"bold", "italics", "underline", "slanted", "smallcaps", "sansserif", "monospace",
      "emphasis", "valuelabels", "decimalalign"}});

// `force` is texdoc's "overwrite even if the file is open" flag; `cmdstrip` and
// friends belong to `texdoc do`/`texdoc stlog`, which this build does not run.
const piet::eval::OptVocab& kVocabTexdoc = piet::eval::registerOptVocab(
    {"texdoc"}, {{"replace", "append", "force"}, {"cmdstrip", "gropts", "nolog", "nooutput"}});

// `tex` (texdoc's line verb) takes no options at all: everything after the
// verb is literal TeX. Registering an EMPTY vocabulary is not a no-op -- it is
// what makes any `tex ..., foo` reach the net as an unknown key rather than
// being silently swallowed.
const piet::eval::OptVocab& kVocabTexLine =
    piet::eval::registerOptVocab({"tex"}, {{}, {}});

const piet::eval::OptVocab& kVocabFrmttable = piet::eval::registerOptVocab(
    {"frmttable"},
    {{"statmat", "sdec", "tex", "replace", "rtitles", "ctitles", "title", "note", "using",
      "fragment",
      // W1a-5h. `nocenter` drops the `\begin{center}` wrapper and nothing else
      // (w1b_frmttable_base.log h7 against h1); `sfmt()` names the numeric
      // format whose `fc` spelling is ALREADY the default this writer uses, so
      // `sfmt(fc,...)` is byte-identical to leaving it out (cells2 k7 against
      // k6) -- accepted as a measured no-op for that spelling, and any other
      // suffix is refused below rather than silently treated as `fc`.
      "nocenter", "sfmt",
      // W2AK TW-FRMT: the stored table (frmttable) -- `store()`
      // names the table a statmat() call builds (default: the unnamed one) and
      // `replay[()]` writes a stored table with the new output options.
      "replay", "store"},
     // `replay`/`replay1`/`store` are frmttable's stored-table accumulator
     // (frmttable keeps the built table in a Mata external
     // `_FrmtT<name>`): `store()` names it, `append`/`merge` accumulate into it,
     // and `replay` writes it out. Measured in w1b_frmttable_replay.log -- append
     // STACKS rows (g2) and merge adds COLUMNS (g4) -- and named unimplemented
     // rather than unknown, because the model is understood and absent, not
     // unrecognised. `multicol()` is the tex span option paper 004 pairs with
     // replay (gb).
     {"merge", "append", "varlabels", "substat", "annotate", "asymbol", "coljust", "hlines",
      "doubles", "multicol", "addrows"}});

const piet::eval::OptVocab& kVocabRegsave = piet::eval::registerOptVocab(
    {"regsave"},
    {{"replace", "append", "ci", "se", "pval", "tstat", "addlabel", "level", "cmdline"},
     {"table", "detail", "format", "asterisk", "covar", "autoid", "saveold"}});

const piet::eval::OptVocab& kVocabParmest = piet::eval::registerOptVocab(
    {"parmest"},
    {{"saving", "norestore", "list", "omit", "label", "idnum", "idstr", "level", "replace"},
     {"format", "escal", "eform", "flist", "elist", "stars", "rename", "bmat", "vmat"}});

const piet::eval::OptVocab& kVocabMat2txt = piet::eval::registerOptVocab(
    {"mat2txt"}, {{"matrix", "saving", "replace", "append", "title", "format", "note"}, {}});

const piet::eval::OptVocab& kVocabErepost = piet::eval::registerOptVocab(
    {"erepost"}, {{"rename"}, {"esample", "noesample", "cmd", "properties"}});

// `tabout` is refused at the ARM (dispatcher.cpp), so it never reaches a
// handler here. It still gets a vocabulary entry so that
// `canonicalizeForCommand` has something to answer for the token and the
// registry inventory guard sees a complete bundle. Everything is
// `unimplemented`: the whole grammar is what is missing, not a set of options.
const piet::eval::OptVocab& kVocabTabout =
    piet::eval::registerOptVocab({"tabout"}, {{}, {"using", "replace", "append", "style", "cells",
                                                  "format", "clab", "sum", "svy"}});

}  // namespace

namespace piet::eval {
// eval/macros.cpp: the text a `local v = <real>` stores. Declared here as
// handlers_lb_leebounds.cpp declares it (macros.h does not export it).
std::string stataMacroRealText(double d);
}  // namespace piet::eval

namespace piet::handlers {

using ojson = nlohmann::ordered_json;

namespace {

// The registrations above are consumed by the reject nets below; naming them
// here keeps `-Wunused-variable` honest about the two that no handler in this
// TU reads (tabout is refused at the arm, and `tex` has an empty vocabulary).
[[maybe_unused]] const piet::eval::OptVocab* const kVocabTouch[] = {
    &kVocabTabout, &kVocabTexLine};

// ===========================================================================
// Errors
// ===========================================================================

ojson tblError(const std::string& message, const std::string& code = "engine_error") {
  return ojson(eval::engineError(message, code));
}

// A USER SYNTAX error. `syntax_error` is what coarseRc maps to rc 198; the
// default `engine_error` maps to rc 1, and bundle C had to repair six sites
// that had made exactly that substitution.
ojson tblSyntax(const std::string& message) {
  return ojson(eval::engineError(message, "syntax_error"));
}

// An error whose Stata rc is KNOWN but which coarseRc's message rules do not
// recognise. `reason` is the only channel that carries an exact rc (BUG-187)
// and it outranks the coarse mapping.
ojson tblErrorRc(const std::string& message, int rc) {
  return ojson(eval::errorBlock("engine_error", message, std::nullopt, std::nullopt, std::nullopt,
                                "r(" + std::to_string(rc) + ")"));
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

bool isErrorBlock(const ojson& b) {
  return b.is_object() && b.value("type", std::string()) == "error";
}

// ===========================================================================
// Option access
// ===========================================================================

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
  // A bare flag parses as isBool/true; `tex(frag)` parses as a STRING value but
  // still means "tex was given". Both forms answer yes -- reading only the bool
  // form is how `outreg2 ..., tex(frag)` would have quietly written plain text.
  return v->isBool ? v->boolValue : true;
}

std::optional<std::string> optStr(const std::vector<std::pair<std::string, eval::OptValue>>& opts,
                                  const std::string& key) {
  const eval::OptValue* v = findOpt(opts, key);
  if (v == nullptr || v->isBool) return std::nullopt;
  return v->strValue;
}

// ===========================================================================
// Small text helpers
// ===========================================================================

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

// Split at TOP-LEVEL whitespace only: `b(star fmt(3)) se(par)` is two items,
// not five. Quotes count as depth too, so `label("Std. err.")` survives.
std::vector<std::string> splitTopLevel(const std::string& s, char sep = ' ') {
  std::vector<std::string> out;
  std::string cur;
  int depth = 0;
  bool inQuote = false;
  for (char c : s) {
    if (c == '"') inQuote = !inQuote;
    if (!inQuote) {
      if (c == '(' || c == '[' || c == '{') ++depth;
      if (c == ')' || c == ']' || c == '}') --depth;
    }
    const bool isSep = (sep == ' ') ? (c == ' ' || c == '\t') : (c == sep);
    if (isSep && depth == 0 && !inQuote) {
      if (!cur.empty()) {
        out.push_back(cur);
        cur.clear();
      }
      continue;
    }
    cur += c;
  }
  if (!cur.empty()) out.push_back(cur);
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

// Widths count CHARACTERS (StataBE pads a Unicode name by characters:
// scratch-r150/uni-v/p4.do), so a multi-byte name stays aligned.
std::string rpad(const std::string& s, size_t w) {
  const size_t n = cecon::ident::codePointCount(s);
  return n >= w ? s : s + std::string(w - n, ' ');
}

std::string lpad(const std::string& s, size_t w) {
  const size_t n = cecon::ident::codePointCount(s);
  return n >= w ? s : std::string(w - n, ' ') + s;
}

// The whole string is `r( NAME )`. NAME is read with core/ident.h's permissive
// tier (a byte >= 0x80 is a name byte), by hand rather than by regex: a narrow
// `char` bracket range over the high bytes is not portable across standard
// libraries.
bool parseWholeRName(const std::string& s, std::string* name) {
  if (s.size() < 4 || s[0] != 'r' || s[1] != '(') return false;
  size_t j = 2;
  while (j < s.size() && std::isspace(static_cast<unsigned char>(s[j]))) ++j;
  const size_t b = j;
  if (j >= s.size() || !cecon::ident::isIdentStartByte(s[j])) return false;
  while (j < s.size() && cecon::ident::isIdentByte(s[j])) ++j;
  const size_t e = j;
  while (j < s.size() && std::isspace(static_cast<unsigned char>(s[j]))) ++j;
  if (j + 1 != s.size() || s[j] != ')') return false;
  *name = s.substr(b, e - b);
  return true;
}

// `<integer>.<name>`: a factor-level coefficient (`2.قيمة`).
bool parseLevelName(const std::string& s, long long* level, std::string* var) {
  size_t i = 0;
  if (i < s.size() && s[i] == '-') ++i;
  const size_t d0 = i;
  while (i < s.size() && s[i] >= '0' && s[i] <= '9') ++i;
  if (i == d0 || i >= s.size() || s[i] != '.') return false;
  const size_t dot = i++;
  if (i >= s.size() || !cecon::ident::isIdentStartByte(s[i])) return false;
  for (size_t k = i; k < s.size(); ++k)
    if (!cecon::ident::isIdentByte(s[k])) return false;
  try {
    *level = std::stoll(s.substr(0, dot));
  } catch (const std::exception&) {
    return false;
  }
  *var = s.substr(i);
  return true;
}

// ===========================================================================
// Numbers
// ===========================================================================

// A Stata missing is `.`, never 0 -- in EVERY cell this file renders. A zero
// where a missing belongs reads as an estimated null effect, which is a
// different scientific claim from "not estimated".
constexpr const char* kMissingCell = ".";

// `fmt(3)` means `%9.3f`; `fmt(%9.2f)` is passed through. `fmt(a3)` is Stata's
// "adaptive" width and is REFUSED by the callers rather than silently rendered
// as a fixed 3 decimals -- refusing is the plan's decision 4.
std::string normalizeFmt(const std::string& raw, bool* okOut) {
  *okOut = true;
  const std::string f = strutil::trim(dequote(raw));
  if (f.empty()) {
    *okOut = false;
    return "%9.3f";
  }
  if (f[0] == '%') return f;
  if (f.find_first_not_of("0123456789") == std::string::npos) {
    return "%9." + f + "f";
  }
  *okOut = false;
  return "%9.3f";
}

// The `%[-][w].[p][fge]` subset the corpus exercises. Anything else falls back
// to three decimals, which is Stata's own table default.
// Defined below, next to the other number shapes; declared here because the
// `a#` branch of fmtNum needs it and fmtNum comes first in this file.
std::string sigDigits(double v, int d, int minDec, bool comma);

std::string fmtNum(double v, const std::string& fmt) {
  if (std::isnan(v)) return kMissingCell;
  // W1a-5b: `a#` is esttab's own ADAPTIVE format -- at least # significant
  // digits, which is what a footer row carries when nothing overrides it.
  // Measured: `esttab, scalars(mymean)` renders 6165.257 as `6165.3`
  // (w1b_sfmt_rule.log q5), which %9.3f cannot produce. The spelling is
  // accepted here so the footer can ask for it by name instead of the caller
  // reaching for a second formatter.
  if (!fmt.empty() && fmt[0] == 'a') {
    int d = 3;
    if (fmt.size() > 1) {
      try {
        size_t used = 0;
        const int n = std::stoi(fmt.substr(1), &used);
        if (used + 1 == fmt.size() && n >= 0 && n <= 12) d = n;
      } catch (const std::exception&) {
        d = 3;
      }
    }
    return sigDigits(v, d, /*minDec=*/1, /*comma=*/false);
  }
  int prec = 3;
  char kind = 'f';
  size_t i = 0;
  if (i < fmt.size() && fmt[i] == '%') ++i;
  while (i < fmt.size() && (fmt[i] == '-' || fmt[i] == '0' || fmt[i] == ' ')) ++i;
  while (i < fmt.size() && std::isdigit(static_cast<unsigned char>(fmt[i]))) ++i;
  if (i < fmt.size() && fmt[i] == '.') {
    ++i;
    std::string digits;
    while (i < fmt.size() && std::isdigit(static_cast<unsigned char>(fmt[i]))) digits += fmt[i++];
    if (!digits.empty()) prec = std::stoi(digits);
  }
  if (i < fmt.size() && (fmt[i] == 'f' || fmt[i] == 'g' || fmt[i] == 'e')) kind = fmt[i];
  // `%9.0g` / `%10.0g` is Stata's DEFAULT general format, not "one significant
  // digit": C's `%.0g` is treated as `%.1g` and would render 1.234 as "1".
  // `fmtStataDefaultG` is the measured nine-character rule display already uses,
  // so mat2txt's default format and `display` agree on the same number.
  if (kind == 'g' && prec == 0) return piet::fmtStataDefaultG(v);
  char buf[128];
  if (kind == 'f') {
    std::snprintf(buf, sizeof(buf), "%.*f", prec, v);
  } else if (kind == 'e') {
    std::snprintf(buf, sizeof(buf), "%.*e", prec, v);
  } else {
    std::snprintf(buf, sizeof(buf), "%.*g", prec, v);
  }
  return std::string(buf);
}

// --- estout's `a#` adaptive format -----------------------------------------
// estout `SignificantDigits` ("idea stolen from outreg2.ado",
// says its own comment -- which is why outreg2's `auto(#)` shares this body).
// `minDec` is the one place the two callers differ: esttab floors at ONE
// decimal (1946.069 -> 1946.1) and outreg2 floors at ZERO (-> 1,946). That
// single integer is the whole difference between t_plain.txt and o_plain.txt
// on the same coefficient, so it is a parameter rather than two copies.
// --- outreg2 `adec(#)`: three regimes of one width-12 format ---------------
// outreg2 `_addstat_parse` renders each added statistic as
//   string(value, "%12.<adec><afmt>")            (afmt defaults to `fc`)
// and splices the result back into the COMMA-DELIMITED addstat() string that
// outreg2 then re-parses. A thousands separator therefore comes back as a FIELD
// separator, which is why the corpus's own line -- `addstat(Mean dep var,
// `mdp') adec(2)`, 106 uses across three papers -- is r(198) in this Stata
// whenever that mean reaches four integer digits (w1b_adec_rule5.log,
// W1BAD5REC|corpus_macro|198|, beside W1BAD5REC|corpus_rmean_ok|0| on a mean of
// 21.3). The refusal belongs to the caller; this builds the string and counts
// the separators that survived.
//
// The `%12.` WIDTH is what makes this three regimes rather than two, and each
// one is measured (w1b_adec_rule{1,3,4,5}.log):
//   plain "%.<adec>f" over 12 chars -> e-notation, min(adec,5) decimals, and
//                                      adec(0) also five; no separator, rc 0
//   separated spelling within 12    -> separators kept -> outreg2 FAILS
//   separated spelling over 12      -> separators dropped, rc 0
// 6165.257 flips between adec(6) (`6,165.257000`, 12 chars, r(198)) and adec(7)
// (`6,165.2570000`, 13 chars, rc 0, rendering 6165.2570000). Testing |value| >=
// 1000 instead would get BOTH ends wrong: 999.96 at adec(1) IS `1,000.0` and
// refuses (W1BADREC|v999_96_adec1|198|), and 9999999 at adec(3) does not.
struct AdecCell {
  std::string text;
  int seps = 0;
};

AdecCell outreg2AdecCell(double v, int adec) {
  AdecCell out;
  char buf[512];
  std::snprintf(buf, sizeof(buf), "%.*f", adec, v);
  const std::string plain(buf);
  if (plain.size() > 12) {
    // Stata's width overflow. The mantissa carries adec decimals up to five, and
    // adec(0) carries five as well: 1e12 is `1.0e+12` at adec(1), `1.000e+12` at
    // adec(3), `1.00000e+12` at adec(0), adec(5), adec(6) and adec(11)
    // (w1b_adec_rule4.log u1/u2/u4/u5/u6 and w1b_adec_rule3.log t6).
    const int edec = (adec >= 1 && adec <= 5) ? adec : 5;
    std::snprintf(buf, sizeof(buf), "%.*e", edec, v);
    out.text = buf;
    return out;
  }
  std::string sep = plain;
  const size_t dot = sep.find('.');
  const size_t start = (!sep.empty() && sep[0] == '-') ? 1 : 0;
  const size_t end = (dot == std::string::npos) ? sep.size() : dot;
  int inserted = 0;
  for (size_t at = end; at > start + 3;) {
    at -= 3;
    sep.insert(at, ",");
    ++inserted;
  }
  if (inserted > 0 && sep.size() <= 12) {
    out.text = sep;
    out.seps = inserted;
    return out;
  }
  out.text = plain;  // the separated spelling did not fit, so the ado drops it
  return out;
}

std::string sigDigits(double v, int d, int minDec, bool comma) {
  if (std::isnan(v)) return kMissingCell;
  int dec = 0;
  if (v == std::trunc(v)) {
    dec = 0;  // "integer: print no decimal places"
  } else if (std::fabs(v) < 1.0) {
    // `right` = zeros after the decimal point. Stata's int() truncates toward
    // zero, which is what a C cast does, so -1.0128 -> -1 on both sides.
    const double frac = std::fabs(v - std::trunc(v));
    const int right = -static_cast<int>(std::log10(frac));
    dec = std::max(1, d + right);
    if (dec > 9) {
      char e[64];
      std::snprintf(e, sizeof(e), "%.*e", std::min(9, d - 1), v);
      return std::string(e);
    }
  } else {
    const int left = static_cast<int>(std::log10(std::fabs(v)) + 1);
    if (left > 9) {
      char e[64];
      std::snprintf(e, sizeof(e), "%.0e", v);
      return std::string(e);
    }
    dec = std::max(minDec, d - left + 1);
  }
  char buf[64];
  std::snprintf(buf, sizeof(buf), "%.*f", dec, v);
  std::string s(buf);
  if (!comma) return s;
  // %fc: thousands separators on the integer part only.
  const size_t dot = s.find('.');
  size_t start = (s[0] == '-') ? 1 : 0;
  size_t end = (dot == std::string::npos) ? s.size() : dot;
  for (size_t at = end; at > start + 3;) {
    at -= 3;
    s.insert(at, ",");
  }
  return s;
}

std::string fmtJson(const ojson& v, const std::string& fmt) {
  if (v.is_null()) return kMissingCell;
  if (v.is_string()) return v.get<std::string>();
  if (!v.is_number()) return kMissingCell;
  return fmtNum(v.get<double>(), fmt);
}

// An observation count renders as an integer (Stata's `%9.0g` on an integral
// value), never as "30.000".
std::string fmtCount(const ojson& v) {
  if (v.is_null()) return kMissingCell;
  if (v.is_string()) return v.get<std::string>();
  if (!v.is_number()) return kMissingCell;
  const double d = v.get<double>();
  if (std::isnan(d)) return kMissingCell;
  if (d == std::floor(d) && std::fabs(d) < 1e15) {
    return std::to_string(static_cast<long long>(d));
  }
  return fmtNum(d, "%9.3f");
}

struct StarLevel {
  const char* mark;
  double cutoff;
};

// estout/esttab's default ladder and outreg2's are DIFFERENT, and this is not
// cosmetic: the same p = 0.03 gets one star from estout and two from outreg2.
// The plan's Stata-semantics section fixes both explicitly for that reason.
const std::vector<StarLevel>& estoutStars() {
  static const std::vector<StarLevel> k = {{"***", 0.001}, {"**", 0.01}, {"*", 0.05}};
  return k;
}
const std::vector<StarLevel>& outreg2Stars() {
  static const std::vector<StarLevel> k = {{"***", 0.01}, {"**", 0.05}, {"*", 0.10}};
  return k;
}

std::string starsFor(const ojson& p, const std::vector<StarLevel>& ladder) {
  if (!p.is_number()) return "";
  const double pv = p.get<double>();
  if (std::isnan(pv)) return "";
  for (const auto& lv : ladder) {
    if (pv < lv.cutoff) return lv.mark;
  }
  return "";
}

std::string starNote(const std::vector<StarLevel>& ladder) {
  std::string s;
  for (size_t i = 0; i < ladder.size(); ++i) {
    if (i) s += ", ";
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%s p<%g", ladder[i].mark, ladder[i].cutoff);
    s += buf;
  }
  return s;
}

// `starlevels(* 0.10 ** 0.05 *** 0.01)` -- pairs of mark/threshold. A malformed
// list keeps the default rather than half-applying, which is the same rule
// eststo_esttab.cpp's parseStarOpt already follows for `star()`.
std::vector<StarLevel> parseStarLevels(const std::string& spec,
                                       const std::vector<StarLevel>& fallback,
                                       std::vector<std::string>* markStore) {
  const std::vector<std::string> toks = splitWs(dequote(spec));
  if (toks.size() < 2 || toks.size() % 2 != 0) return fallback;
  std::vector<StarLevel> out;
  markStore->clear();
  markStore->reserve(toks.size() / 2);
  for (size_t i = 0; i + 1 < toks.size(); i += 2) {
    try {
      size_t pos = 0;
      const double th = std::stod(toks[i + 1], &pos);
      if (pos != toks[i + 1].size()) return fallback;
      markStore->push_back(toks[i]);
      out.push_back(StarLevel{nullptr, th});
    } catch (...) {
      return fallback;
    }
  }
  for (size_t i = 0; i < out.size(); ++i) out[i].mark = (*markStore)[i].c_str();
  std::sort(out.begin(), out.end(),
            [](const StarLevel& a, const StarLevel& b) { return a.cutoff < b.cutoff; });
  return out;
}

// ===========================================================================
// THE TABLE MODEL AND ITS THREE RENDERERS (plan design decision 5)
// ===========================================================================

struct TRow {
  enum Kind { kCoef, kSe, kStat, kText, kRule };
  Kind kind = kCoef;
  std::string label;
  std::vector<std::string> cells;  // one per column; "" renders as blank
};

struct Table {
  std::string title;
  std::vector<std::string> colNums;   // "(1)", "(2)", ... ; empty = suppressed
  std::vector<std::string> colHeads;  // mtitles / ctitles / depvars
  // The head of the STUB column (the leftmost, label-bearing one). Blank for an
  // estimates table, whose stub holds coefficient names and needs no head;
  // texsave's leftmost column is a data column and its variable name would
  // otherwise be dropped on the floor.
  std::string stubHead;
  // BUG-486: the CELL-NAME row. estout writes one under the model heads naming
  // which statistic each column carries (`b`, or `b/se` for a stacked pair) --
  // `tests/eval/golden/aer_w1/w1b_tables/e2.txt` row 2, and e_plain.txt row 2
  // from the 09-09 capture. `collabels(none)` is exactly the option that
  // removes it (w1b_tables/e7.tex, which carries the model names and no cell
  // row). Empty for every other writer, whose renderers therefore do not
  // change shape.
  std::vector<std::string> colLabels;
  std::vector<TRow> body;
  std::vector<TRow> foot;
  std::vector<std::string> notes;
  std::string align;  // texsave's align(); empty = derived

  size_t ncols() const {
    size_t n = colHeads.size();
    n = std::max(n, colNums.size());
    n = std::max(n, colLabels.size());
    for (const auto& r : body) n = std::max(n, r.cells.size());
    for (const auto& r : foot) n = std::max(n, r.cells.size());
    return n;
  }
};

// LaTeX's ten specials. `nofix` (texsave) turns this off; everything else in
// this file escapes unconditionally, because an unescaped `_` in a variable
// name is the single most common way a generated .tex file fails to compile.
std::string texEscape(const std::string& s) {
  std::string out;
  out.reserve(s.size() + 8);
  for (char c : s) {
    switch (c) {
      case '_': case '&': case '%': case '$': case '#': case '{': case '}':
        out += '\\';
        out += c;
        break;
      case '~': out += "\\textasciitilde{}"; break;
      case '^': out += "\\textasciicircum{}"; break;
      case '\\': out += "\\textbackslash{}"; break;
      default: out += c;
    }
  }
  return out;
}

struct TexStyle {
  bool booktabs = false;   // \toprule/\midrule/\bottomrule instead of \hline
  bool fragment = false;   // rows only -- no tabular environment at all
  bool doubleTop = false;  // estout style(tex): \hline\hline above the header
  bool escape = true;
  bool standalone = false;  // wrap in \documentclass ... \begin{document}
  // The column specification is NOT cosmetic here: the plan's Stata-semantics
  // section fixes a different spelling per command (estout writes
  // `l*{k}{c}`, outreg2 writes `l c c`), and these strings are what the frozen
  // .tex goldens compare against.
  enum Align { kPlain, kStar, kSpaced };
  Align align = kPlain;
  // W1a-5c: the LETTER inside `l*{k}{c}`. esttab's `alignment()`/`align()`
  // picks it -- `alignment(S)` writes `l*{2}{S}` (w1b_esttab_resid.log y12),
  // and `alignment(c)` is byte-identical to the control because `c` is already
  // the default (y11). Anything else is the caller's own LaTeX column type
  // (siunitx's `S`, `D{.}{.}{-1}` from dcolumn) and is written through
  // unchanged rather than validated against a list this build cannot know.
  std::string alignLetter = "c";
};

std::string renderTex(const Table& t, const TexStyle& st) {
  const size_t k = t.ncols();
  auto esc = [&](const std::string& s) { return st.escape ? texEscape(s) : s; };
  auto rowOut = [&](const std::string& label, const std::vector<std::string>& cells) {
    std::string line = esc(label);
    for (size_t c = 0; c < k; ++c) {
      line += " & ";
      if (c < cells.size()) line += esc(cells[c]);
    }
    line += " \\\\";
    return line;
  };

  std::ostringstream o;
  const std::string top = st.booktabs ? "\\toprule" : (st.doubleTop ? "\\hline\\hline" : "\\hline");
  const std::string mid = st.booktabs ? "\\midrule" : "\\hline";
  const std::string bot = st.booktabs ? "\\bottomrule" : "\\hline";

  if (!st.fragment) {
    if (st.standalone) {
      o << "\\documentclass{article}\n\\begin{document}\n";
    }
    if (!t.title.empty()) o << "\\caption{" << esc(t.title) << "}\n";
    std::string align = t.align;  // texsave's align() wins outright
    if (align.empty()) {
      if (st.align == TexStyle::kStar) {
        align = "l*{" + std::to_string(k) + "}{" + st.alignLetter + "}";
      } else if (st.align == TexStyle::kSpaced) {
        align = "l";
        for (size_t c = 0; c < k; ++c) align += " c";
      } else {
        align = "l";
        for (size_t c = 0; c < k; ++c) align += "c";
      }
    }
    o << "\\begin{tabular}{" << align << "}\n";
    o << top << "\n";
  }

  if (!t.colNums.empty()) {
    o << rowOut("", t.colNums) << "\n";
  }
  if (!t.colHeads.empty() || !t.stubHead.empty()) {
    o << rowOut(t.stubHead, t.colHeads) << "\n";
  }
  // The cell-name row travels with the table, so it appears in every style.
  // Its PLACEMENT in tex is not separately captured -- w1b_tables/e7.tex is a
  // `collabels(none)` arm and carries no such row -- but a table model that
  // holds a row the tex writer silently drops is the worse of the two.
  if (!t.colLabels.empty()) {
    o << rowOut("", t.colLabels) << "\n";
  }
  if (!st.fragment && (!t.colNums.empty() || !t.colHeads.empty() || !t.stubHead.empty())) {
    o << mid << "\n";
  }

  for (const auto& r : t.body) {
    if (r.kind == TRow::kRule) {
      o << mid << "\n";
      continue;
    }
    o << rowOut(r.label, r.cells) << "\n";
  }

  if (!t.foot.empty()) {
    if (!st.fragment) o << mid << "\n";
    for (const auto& r : t.foot) {
      if (r.kind == TRow::kRule) {
        o << mid << "\n";
        continue;
      }
      o << rowOut(r.label, r.cells) << "\n";
    }
  }

  if (!st.fragment) {
    o << bot << "\n";
    o << "\\end{tabular}\n";
    for (const auto& n : t.notes) {
      // \multicolumn would need the column count inside the tabular; outside it
      // a note is ordinary text, which is what both estout and outreg2 emit.
      o << "\\begin{tablenotes}\n\\item " << esc(n) << "\n\\end{tablenotes}\n";
    }
    if (st.standalone) o << "\\end{document}\n";
  }
  return o.str();
}

// estout's four SEAM hooks and its row prefix. On esttab these four REPLACE a
// block of the table (BUG-500); on estout there is no block to replace -- a tab
// table has no rules -- and they are purely ADDITIVE lines at the seam
// (tests/eval/golden/aer_w1/w1b_estout_opts.log qb..qe). `prefoot` emits even
// when there are no stats rows, at the position the footer would have started
// (w1b_estout_opts2.log y9), and `begin(X)` prefixes EVERY row with no separator
// (qf: `BEGIN\te1`, `BEGINmpg\t-49.51222`).
struct EstoutHooks {
  std::optional<std::string> prehead, posthead, prefoot, postfoot;
  std::string begin;
};

std::string renderFixed(const Table& t, const EstoutHooks& hk = {}) {
  const size_t k = t.ncols();
  std::vector<size_t> width(k + 1, 0);
  auto measure = [&](const std::string& label, const std::vector<std::string>& cells) {
    width[0] = std::max(width[0], cecon::ident::codePointCount(label));  // characters
    for (size_t c = 0; c < k; ++c) {
      if (c < cells.size())
        width[c + 1] = std::max(width[c + 1], cecon::ident::codePointCount(cells[c]));
    }
  };
  measure("", t.colNums);
  measure("", t.colHeads);
  measure("", t.colLabels);
  for (const auto& r : t.body) measure(r.label, r.cells);
  for (const auto& r : t.foot) measure(r.label, r.cells);
  for (size_t c = 1; c <= k; ++c) width[c] = std::max<size_t>(width[c], 8);
  width[0] = std::max<size_t>(width[0], 12);

  size_t total = width[0];
  for (size_t c = 1; c <= k; ++c) total += 1 + width[c];
  const std::string rule(total, '-');

  std::ostringstream o;
  if (!t.title.empty()) o << t.title << "\n";
  auto emit = [&](const std::string& label, const std::vector<std::string>& cells) {
    std::string line = rpad(label, width[0]);
    for (size_t c = 0; c < k; ++c) {
      line += " " + lpad(c < cells.size() ? cells[c] : std::string(), width[c + 1]);
    }
    // Trailing blanks carry no information and would make every golden
    // sensitive to a column that happens to be empty.
    while (!line.empty() && line.back() == ' ') line.pop_back();
    o << hk.begin << line << "\n";
  };
  if (hk.prehead.has_value()) o << *hk.prehead << "\n";
  o << rule << "\n";
  if (!t.colNums.empty()) emit("", t.colNums);
  if (!t.colHeads.empty() || !t.stubHead.empty()) emit(t.stubHead, t.colHeads);
  if (!t.colLabels.empty()) emit("", t.colLabels);
  o << rule << "\n";
  if (hk.posthead.has_value()) o << *hk.posthead << "\n";
  for (const auto& r : t.body) {
    if (r.kind == TRow::kRule) { o << rule << "\n"; continue; }
    emit(r.label, r.cells);
  }
  if (!t.foot.empty()) {
    o << rule << "\n";
    if (hk.prefoot.has_value()) o << *hk.prefoot << "\n";
  for (const auto& r : t.foot) {
      if (r.kind == TRow::kRule) { o << rule << "\n"; continue; }
      emit(r.label, r.cells);
    }
  }
  o << rule << "\n";
  if (hk.postfoot.has_value()) o << *hk.postfoot << "\n";
  for (const auto& n : t.notes) o << n << "\n";
  return o.str();
}

// A delimited cell. Comma output quotes anything containing the delimiter or a
// quote; tab output does not, because outreg2's own `.txt` ledger is read back
// by this build's `append` parser and an added quote would not round-trip.
std::string delimCell(const std::string& s, char delim) {
  if (delim != ',') return s;
  if (s.find_first_of(",\"\n") == std::string::npos) return s;
  std::string out = "\"";
  for (char c : s) {
    if (c == '"') out += '"';
    out += c;
  }
  out += '"';
  return out;
}

// --- estout's style(tex) --------------------------------------------------
// BUG-486's SIXTH axis, and the largest single block left in this bundle: 148 of
// the corpus's 219 estout lines carry tex, over 11 papers. This writer handed
// them to `renderTex` -- the esttab emitter -- so all 148 got
// `\begin{tabular}{l*{2}{c}}`, `\hline\hline` and ` & e1 & e2 \\`, where the ado
// writes a bare FRAGMENT of fixed-width right-aligned cells and no rules at all:
//
//     `            &          t1&          t2\\`
//     `mpg         &   -49.51222&   -238.8943\\`
//
// (tests/eval/golden/aer_w1/w1b_estout_tex.log r1, and w1b_tables/e7.tex from the
// 2026-09-14 capture, which shows the same thing and was never diffed -- the
// "captured but not compared" shape BUG-486 already records about itself.)
//
// Three widths are the whole implementable content, and each is measured:
//   stub  = varwidth() if given, else 20 when `label` is in effect and 12 when it
//           is not. It is the LABEL FLAG and not the content: a two-character
//           label still gets 20 and a 25-character one overflows unpadded
//           (w1b_estout_tex2.log s12..s25 against the no-label control).
//   cell  = modelwidth() if given, else 12, right-aligned, also a MINIMUM (a
//           13-character number overflows: w1b_estout_tex.log rc).
//   star  = 3, LEFT-aligned after the cell, so `1.746559** ` keeps its trailing
//           space where `-238.8943***` has none (r4). Padding value+star together
//           to 15 would get the first of those wrong and the second right.
std::string renderEstoutTex(const Table& t, size_t stubW, size_t cellW,
                            size_t starW, const EstoutHooks& hk = {}) {
  const size_t k = t.ncols();
  auto padR = [](const std::string& v, size_t w) { return rpad(v, w); };
  auto padL = [](const std::string& v, size_t w) { return lpad(v, w); };
  // The star mark is concatenated into the cell text upstream (`cellFor` appends
  // it), and the two halves pad differently -- so it is split back off here. A
  // trailing run of `*`/`+` is the mark: no number this writer formats ends in
  // either, and `(omitted)` does not.
  auto splitStar = [](const std::string& c, std::string* star) {
    size_t e = c.size();
    while (e > 0 && (c[e - 1] == '*' || c[e - 1] == '+')) --e;
    *star = c.substr(e);
    return c.substr(0, e);
  };
  auto line = [&](const std::string& label, const std::vector<std::string>& cells) {
    std::string out = padR(label, stubW);
    for (size_t c = 0; c < k; ++c) {
      std::string star;
      const std::string v = c < cells.size() ? splitStar(cells[c], &star) : std::string();
      out += "&" + padL(v, cellW) + padR(star, starW);
    }
    return out + "\\\\";
  };

  std::ostringstream o;
  auto row = [&](const std::string& label, const std::vector<std::string>& cells) {
    o << hk.begin << line(label, cells) << "\n";
  };
  if (hk.prehead.has_value()) o << *hk.prehead << "\n";
  // `numbers` puts the (1)/(2) row ABOVE the model names (r a), which is the
  // opposite of esttab's order.
  if (!t.colNums.empty()) row(std::string(), t.colNums);
  if (!t.colHeads.empty()) row(t.stubHead, t.colHeads);
  if (!t.colLabels.empty()) row(std::string(), t.colLabels);
  if (hk.posthead.has_value()) o << *hk.posthead << "\n";
  for (const auto& r : t.body) row(r.label, r.cells);
  if (hk.prefoot.has_value()) o << *hk.prefoot << "\n";
  for (const auto& r : t.foot) row(r.label, r.cells);
  if (hk.postfoot.has_value()) o << *hk.postfoot << "\n";
  for (const auto& n : t.notes) o << n << "\n";
  return o.str();
}

std::string renderDelim(const Table& t, char delim, const EstoutHooks& hk = {}) {
  const size_t k = t.ncols();
  std::ostringstream o;
  auto emit = [&](const std::string& label, const std::vector<std::string>& cells) {
    std::string line = delimCell(label, delim);
    for (size_t c = 0; c < k; ++c) {
      line += delim;
      line += delimCell(c < cells.size() ? cells[c] : std::string(), delim);
    }
    o << hk.begin << line << "\n";
  };
  if (!t.title.empty()) o << delimCell(t.title, delim) << "\n";
  if (hk.prehead.has_value()) o << *hk.prehead << "\n";
  if (!t.colNums.empty()) emit("", t.colNums);
  if (!t.colHeads.empty() || !t.stubHead.empty()) emit(t.stubHead, t.colHeads);
  if (!t.colLabels.empty()) emit("", t.colLabels);
  if (hk.posthead.has_value()) o << *hk.posthead << "\n";
  for (const auto& r : t.body) {
    if (r.kind == TRow::kRule) { o << "\n"; continue; }
    emit(r.label, r.cells);
  }
  if (hk.prefoot.has_value()) o << *hk.prefoot << "\n";
  for (const auto& r : t.foot) {
    if (r.kind == TRow::kRule) { o << "\n"; continue; }
    emit(r.label, r.cells);
  }
  if (hk.postfoot.has_value()) o << *hk.postfoot << "\n";
  for (const auto& n : t.notes) o << delimCell(n, delim) << "\n";
  return o.str();
}

// ===========================================================================
// File writing
// ===========================================================================

// SIBLING TEMP + rename(2), never a write into the destination. Copied in shape
// from `export delimited`'s writer (handlers_data_verbs3.cpp:1245-1340): a
// failure never leaves a truncated file where a readable one used to be, and a
// destination that is ALSO a source (outreg2's `append` ledger) is safe because
// the caller has already read it fully into memory before calling this.
//
// `userPath` is what the user TYPED and is the only spelling that ever reaches
// the output; `resolved` is what gets opened. Bundle D echoed the resolved
// absolute path and baked a build machine's workdir into a golden.
ojson writeTextFileAtomic(const std::string& cmd, const std::string& resolved,
                          const std::string& userPath, const std::string& bytes) {
  const std::string tmp = resolved + ".piet_tmp";
  {
    std::ofstream f(cecon::to_native_path(tmp), std::ios::binary);
    if (!f) return tblError(cmd + ": cannot write " + userPath, "data_io");
    f.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    f.flush();
    if (!f) {
      f.close();
      std::error_code rmEc;
      std::filesystem::remove(cecon::to_native_path(tmp), rmEc);
      return tblError(cmd + ": failed writing " + userPath + " (disk full?)", "data_io");
    }
  }
  std::error_code mvEc;
  std::filesystem::rename(cecon::to_native_path(tmp), cecon::to_native_path(resolved), mvEc);
  if (mvEc) {
    std::error_code rmEc;
    std::filesystem::remove(cecon::to_native_path(tmp), rmEc);
    return tblError(cmd + ": failed writing " + userPath, "data_io");
  }
  return ojson();  // null == success; callers test .is_null()
}

std::string readWholeFile(const std::string& path, bool* okOut) {
  std::ifstream f(cecon::to_native_path(path), std::ios::binary);
  if (!f) {
    *okOut = false;
    return {};
  }
  std::ostringstream ss;
  ss << f.rdbuf();
  *okOut = true;
  return ss.str();
}

// The existence/replace/append gate every file-writing verb here shares.
// Returns a non-null error block when the write must not happen.
ojson guardOverwrite(const std::string& cmd, const std::string& resolved,
                     const std::string& userPath, bool replace, bool append) {
  if (cecon::native_exists(resolved) && !replace && !append) {
    // coarseRc maps "file ... already exists" to rc 602, which is Stata's own
    // code for this, so the wording is load-bearing rather than decorative.
    return tblError(cmd + ": file " + userPath +
                    " already exists (add `replace` to overwrite)");
  }
  return ojson();
}

// `using f` with no extension gets one. Stata's table commands each have their
// own default and getting it wrong writes a file the user cannot find.
std::string ensureExt(const std::string& path, const std::string& ext) {
  const size_t slash = path.find_last_of("/\\");
  const size_t dot = path.find_last_of('.');
  if (dot != std::string::npos && (slash == std::string::npos || dot > slash)) return path;
  return path + ext;
}

std::string lowerExt(const std::string& path) {
  const size_t slash = path.find_last_of("/\\");
  const size_t dot = path.find_last_of('.');
  if (dot == std::string::npos || (slash != std::string::npos && dot < slash)) return {};
  return strutil::lower(path.substr(dot));
}

// `using FILE` split out of a varlist. Returns the raw (still user-spelled)
// path, and erases `using` and everything after it from `words`.
std::optional<std::string> takeUsing(std::vector<std::string>& words) {
  for (size_t i = 0; i < words.size(); ++i) {
    if (strutil::lower(words[i]) == "using") {
      std::optional<std::string> path;
      if (i + 1 < words.size()) {
        size_t j = i + 1;
        path = dequote(strutil::rejoinQuotedWord(words, j));  // a quoted path with blanks
      }
      words.erase(words.begin() + static_cast<long>(i), words.end());
      return path;
    }
  }
  return std::nullopt;
}

// ===========================================================================
// Stored estimates: the bag, the e() scalars, and the staleness rule
// ===========================================================================

ojson emptyBag() {
  ojson b;
  b["scalars"] = ojson::object();
  b["locals"] = ojson::object();
  b["matrices"] = ojson::object();
  return b;
}

// Freeze the e() scalars a stored estimate should carry. `eScalarKey()` is the
// ONE table of Stata-spelling -> cobra-key (eval/macros.h); it was exposed at
// namespace scope for this call precisely so a third copy would not appear.
ojson buildEScalars(const ojson& est) {
  ojson out = ojson::object();
  if (!est.is_object()) return out;
  for (const auto& [stataName, engineKey] : eval::eScalarKey()) {
    if (out.contains(stataName)) continue;
    if (!est.contains(engineKey)) continue;
    const ojson& v = est.at(engineKey);
    if (!v.is_number()) continue;
    const double d = v.get<double>();
    if (std::isnan(d)) continue;
    // the engine's n_clusters 0 means "unclustered": Stata has no e(N_clust)
    // there, so the cell stays empty (BUG-1466; the e() resolver's own rule)
    if (stataName == "N_clust" && d == 0.0) continue;
    out[stataName] = d;
  }
  // `r2` is the one name whose snapshot spelling can differ from the block's:
  // makeSnapshot prefers r2_within when present, and a table asking for `r2`
  // must get the number the snapshot itself reports.
  return out;
}

// A stored estimate plus everything a renderer needs from it.
struct Model {
  std::string name;
  ojson snap;       // the eststo snapshot
  ojson added;      // {scalars, locals, matrices}
  ojson escalars;   // Stata-spelled e() scalars
};

// The snapshot's own `escalars`/`added` -- what `runEststo` attaches -- WIN.
// The fallback below is a backstop for an estimate that reached the store by
// some other route (a snapshot from before this bundle, or one built from
// LastModel's cached result), and it fires only when the shared fingerprint
// agrees. A mismatch discards rather than bleeds: attaching another model's
// additions is exactly the failure the fingerprint exists to stop.
Model materializeModel(SessionState& session, const std::string& name, const ojson& snap) {
  Model m;
  m.name = name;
  m.snap = snap;
  m.added = emptyBag();
  m.escalars = ojson::object();

  if (snap.contains("added") && snap["added"].is_object()) {
    m.added = snap["added"];
  } else if (!session.estAddedFp.empty() &&
             session.estAddedFp == SessionState::estAddFingerprint(snap)) {
    m.added = session.estAdded;
  }
  for (const char* k : {"scalars", "locals", "matrices"}) {
    if (!m.added.contains(k) || !m.added[k].is_object()) m.added[k] = ojson::object();
  }

  if (snap.contains("escalars") && snap["escalars"].is_object()) {
    m.escalars = snap["escalars"];
  } else if (session.lastEstBlock.has_value() &&
             SessionState::estAddFingerprint(*session.lastEstBlock) ==
                 SessionState::estAddFingerprint(snap)) {
    m.escalars = buildEScalars(*session.lastEstBlock);
  }
  // N and r2 live on the snapshot itself, so they are available even when the
  // live block has moved on. They never OVERWRITE a frozen escalars entry.
  if (!m.escalars.contains("N") && snap.contains("n_obs") && snap["n_obs"].is_number()) {
    m.escalars["N"] = snap["n_obs"].get<double>();
  }
  if (!m.escalars.contains("r2") && snap.contains("r2") && snap["r2"].is_number()) {
    m.escalars["r2"] = snap["r2"].get<double>();
  }
  return m;
}

// Resolve a namelist (possibly empty -> every stored estimate) into models.
// On failure `errOut` is a real error block and the return is empty.
bool activeModel(SessionState& session, const std::string& cmd, Model* out, ojson* errOut);

std::vector<Model> collectModels(SessionState& session, const std::string& cmd,
                                 const std::vector<std::string>& namesIn, ojson* errOut) {
  // W2 esttabnames (BUG-552): a written namelist goes through the ONE
  // shared expansion (SessionState::expandEstimatesNamelist): `A_*`, `A_?`,
  // `*`, `_all`, `.`, in STORE order; a token matching nothing is r(111)
  // "estimation result NAME not found" -- w2_smalls.log section G. The old
  // exact-name lookup answered `esttab A_*` with "model(s) not found" at rc 1.
  std::vector<std::string> names = namesIn.empty() ? session.estimatesNames() : namesIn;
  // With no namelist and nothing stored estout tables `.`, the active results,
  // and with none of those either it is r(301) "last estimates (.) not found"
  // (StataBE 19.5 `clear all` / `estout`, BUG-1248; it was rc 1).
  if (!namesIn.empty() || names.empty()) {
    SessionState::EstNamelist ex =
        session.expandEstimatesNamelist(names.empty() ? std::vector<std::string>{"."} : names);
    if (ex.rc != 0) {
      *errOut = tblErrorRc(ex.message, ex.rc);
      return {};
    }
    names = std::move(ex.names);
  }
  if (names.empty()) {  // `_all` over an empty store
    *errOut = tblError(cmd + ": no stored estimates. Run a regression, then `eststo m1` first.");
    return {};
  }
  std::vector<Model> out;
  for (const auto& n : names) {
    if (n == ".") {
      // The active results, carrying no store name: read straight from e().
      Model m;
      if (!activeModel(session, cmd, &m, errOut)) return {};
      m.name = ".";
      out.push_back(std::move(m));
      continue;
    }
    const ojson* snap = session.estimatesGet(n);
    if (snap == nullptr) {
      // Unreachable after the expansion (it returns stored names or "."); kept
      // so a future caller can never index a null snapshot.
      *errOut = tblErrorRc("estimation result " + n + " not found", 111);
      return {};
    }
    out.push_back(materializeModel(session, n, *snap));
  }
  return out;
}

// The coefficient rows of one model, keyed by name.
const ojson* coefRow(const ojson& snap, const std::string& name) {
  if (!snap.contains("coeftable") || !snap["coeftable"].is_array()) return nullptr;
  for (const auto& r : snap["coeftable"]) {
    if (r.value("name", std::string()) == name) return &r;
  }
  return nullptr;
}

// Union of coefficient names in first-appearance order across models, then
// keep()/drop()/order() applied in Stata's own precedence: `order()` promotes,
// `keep()` restricts, `drop()` removes.
// The ONE coefficient-selection filter. There were two: `coefNames` for
// estout/outreg2 and a second copy inside `writeEsttabFile` for esttab, and the
// glob fix below reached only the first one -- `keep(*2*)` selected `x2` on
// estout and emptied the table on esttab in the same build. A command whose row
// selection is implemented twice is a command with two answers about what `*`
// means, which is the shadow-evaluator class this codebase keeps paying for.
void applyCoefSelection(std::vector<std::string>* names,
                        const std::optional<std::string>& keepOpt,
                        const std::optional<std::string>& dropOpt,
                        const std::optional<std::string>& orderOpt, std::string* errOut) {
  std::vector<std::string>& sel = *names;
  // An `eq:name` token matches the equation-qualified spelling as well as the
  // bare one, because this build's coeftable carries no equation for a
  // single-equation model and `keep(*:*eigh*)` still keeps `weight` there
  // (w1b_keepglob.log k7). So a token with a colon is tried against BOTH the
  // name and `:name`, which is what makes the corpus's `keep(3:0.MI*...)` reach
  // rows this engine names without an equation.
  // An interaction also matches in another atom order or with/without `c.`:
  // Stata's `1.treatment#c.age_BL` is the engine's `age_BL#1.treatment` (AER 007
  // Joint_test's keep(); cecon::postest::interaction_key, the rule test/lincom use).
  auto tokenHits = [](const std::string& tok, const std::string& name) {
    if (piet::strutil::globMatch(tok, name)) return true;
    if (tok.find_first_of("*?") == std::string::npos) {
      const std::string kt = cecon::postest::interaction_key(tok);
      if (!kt.empty() && kt == cecon::postest::interaction_key(name)) return true;
    }
    const size_t colon = tok.find(':');
    if (colon == std::string::npos) return false;
    return piet::strutil::globMatch(tok.substr(colon + 1), name);
  };
  if (keepOpt.has_value()) {
    const std::vector<std::string> keep = splitWs(dequote(*keepOpt));
    for (const auto& k : keep) {
      bool any = false;
      for (const auto& n : sel) {
        if (tokenHits(k, n)) {
          any = true;
          break;
        }
      }
      if (!any && errOut != nullptr && errOut->empty()) {
        *errOut = "coefficient " + k + " not found";
      }
    }
    std::vector<std::string> filtered;
    for (const auto& n : sel) {
      for (const auto& k : keep) {
        if (tokenHits(k, n)) {
          filtered.push_back(n);
          break;
        }
      }
    }
    sel = filtered;
  }
  if (dropOpt.has_value()) {
    const std::vector<std::string> drop = splitWs(dequote(*dropOpt));
    std::vector<std::string> filtered;
    for (const auto& n : sel) {
      bool hit = false;
      for (const auto& d : drop) {
        if (tokenHits(d, n)) {
          hit = true;
          break;
        }
      }
      if (!hit) filtered.push_back(n);
    }
    sel = filtered;
  }
  if (orderOpt.has_value()) {
    const std::vector<std::string> order = splitWs(dequote(*orderOpt));
    std::vector<std::string> promoted;
    for (const auto& o : order) {
      for (const auto& n : sel) {
        if (!tokenHits(o, n)) continue;
        if (std::find(promoted.begin(), promoted.end(), n) == promoted.end()) promoted.push_back(n);
      }
    }
    for (const auto& n : sel) {
      if (std::find(promoted.begin(), promoted.end(), n) == promoted.end()) promoted.push_back(n);
    }
    sel = promoted;
  }
}

// BUG-496: every one of `keep()`/`drop()`/`order()`'s tokens is a PATTERN, not a
// name. `keep(*romer*)` is how all four of paper 015's transform lines select
// their coefficients and `keep(3:0.MI*#c.romer*)` how another does, and exact
// matching answered them with an EMPTY body at rc 0. `keep(mp?)` is `mpg` too
// (w1b_keepglob.log ka), so it is Stata's `match()`, not a prefix rule. `errOut`
// carries the other half: a keep() token that matches nothing is r(111)
// "coefficient zz* not found" (kg_glob_none), not a table with no rows.
std::vector<std::string> coefNames(const std::vector<Model>& models,
                                   const std::optional<std::string>& keepOpt,
                                   const std::optional<std::string>& dropOpt,
                                   const std::optional<std::string>& orderOpt,
                                   std::string* errOut = nullptr) {
  std::vector<std::string> names;
  std::set<std::string> seen;
  for (const auto& m : models) {
    if (!m.snap.contains("coeftable") || !m.snap["coeftable"].is_array()) continue;
    for (const auto& r : m.snap["coeftable"]) {
      const std::string n = r.value("name", std::string());
      if (seen.insert(n).second) names.push_back(n);
    }
  }
  applyCoefSelection(&names, keepOpt, dropOpt, orderOpt, errOut);
  return names;
}

// `varlabels(x "Income" y "Age")` / `coeflabels(...)` -- pairs. Plus `label`,
// which asks for the DATASET's variable labels instead.
std::map<std::string, std::string> parseLabelPairs(const std::string& spec) {
  std::map<std::string, std::string> out;
  const std::vector<std::string> toks = splitTopLevel(dequote(spec));
  for (size_t i = 0; i + 1 < toks.size(); i += 2) {
    out[toks[i]] = dequote(toks[i + 1]);
  }
  return out;
}

std::string labelFor(const std::string& name, const std::map<std::string, std::string>& explicitMap,
                     bool useVarLabels, SessionState& session) {
  auto it = explicitMap.find(name);
  if (it != explicitMap.end()) return it->second;
  if (useVarLabels) {
    const std::string lbl = session.eval.varLabels.get(name);
    if (!lbl.empty()) return lbl;
  }
  return name;
}

// BUG-486 removed this file's last caller of a `statLabel` auto-label map --
// estout labels a footer row with the key AS TYPED (`r2`, not `R-squared`;
// w1b_estout_defaults.log g3), and a pretty label comes from
// `stats(..., labels(...))`, which is the user's text and needs no map. The map
// is deleted rather than parked: a dead table of guesses is how a later reader
// concludes the guesses were measured. esttab's footer is built by its own
// writer and its labels were byte-verified by the 2026-09-09 capture.

// One footer statistic for one model: e() scalars first, then estadd's scalars,
// then estadd's locals (a TEXT row -- "Controls Yes"). Absent = blank cell, not
// zero: a missing statistic is not a statistic equal to nothing.
std::string statCell(const Model& m, const std::string& key, const std::string& fmt,
                     bool* isTextOut) {
  *isTextOut = false;
  // `ar2` is esttab's spelling of e(r2_a); both reach the same frozen scalar.
  std::vector<std::string> tryKeys{key};
  if (key == "ar2") tryKeys.push_back("r2_a");
  if (key == "r2_a") tryKeys.push_back("ar2");
  // W1a-5d: the pseudo-R2 arrives under TWO names depending on the estimator,
  // measured on this engine: `poisson` publishes `r2_p` (.0126126) while
  // `logit` and `probit` publish `r2_pseudo` (.0321488 / .0345996) and each
  // leaves the other missing; `nbreg` publishes neither. So a reader that
  // knows only one key answers `.` for two of the three families esttab's
  // `pr2` is actually used on -- and both live corpus uses are probit.
  if (key == "pr2" || key == "r2_p" || key == "r2_pseudo") {
    for (const char* alt : {"r2_p", "r2_pseudo"}) {
      if (std::find(tryKeys.begin(), tryKeys.end(), alt) == tryKeys.end()) tryKeys.push_back(alt);
    }
  }
  for (const auto& k : tryKeys) {
    if (m.escalars.contains(k) && m.escalars[k].is_number()) {
      // BUG-486: `N` is formatted by the CALLER's format like every other
      // statistic. estout renders `74` under %9.0g and `74.000` under the
      // fmt(3) its cells left running (w1b_estout_defaults.log g3 vs g4), so a
      // hard `fmtCount` here made one of those two impossible. esttab's writer
      // (the other caller) keeps its measured behaviour by passing `%9.0f` for
      // this key explicitly, which renders an integral count identically.
      return fmtNum(m.escalars[k].get<double>(), fmt);
    }
  }
  if (m.added["scalars"].contains(key)) {
    const ojson& v = m.added["scalars"][key];
    if (v.is_number()) return fmtNum(v.get<double>(), fmt);
  }
  if (m.added["locals"].contains(key)) {
    const ojson& v = m.added["locals"][key];
    *isTextOut = true;
    return v.is_string() ? v.get<std::string>() : std::string();
  }
  return "";
}

}  // namespace

// ---------------------------------------------------------------------------
// `transform()`. Declared in piet/handlers.h; see there for the grammar and for
// why it is shared rather than per-writer.
// ---------------------------------------------------------------------------
namespace {

// Stata's `gettoken` with no `bind` splits on WHITESPACE only, so `exp(@)` and
// `@/(-_b[weight])` are each one token and `pattern(1 1 0 0)` is FOUR. That last
// one is not a detail: it is why paper 083's `transform(@/M 1/M pattern(1 1 0 0)
// @*100 100 pattern(0 0 1 1))` -- no comma, so pattern() cannot be a suboption --
// reads `pattern(1` as a coefficient pattern and silently transforms nothing in
// Stata (w1b_transform.log tr_083_verb, rc 0 and byte-identical to the control).
std::vector<std::string> trTokens(const std::string& s) { return splitWs(s); }

// Split at the FIRST top-level comma: `transform(<list> [, pattern(...)])`.
void trSplitSubopts(const std::string& raw, std::string* list, std::string* subs) {
  int depth = 0;
  for (size_t i = 0; i < raw.size(); ++i) {
    const char c = raw[i];
    if (c == '(' || c == '[') ++depth;
    else if (c == ')' || c == ']') --depth;
    else if (c == ',' && depth == 0) {
      *list = raw.substr(0, i);
      *subs = raw.substr(i + 1);
      return;
    }
  }
  *list = raw;
  subs->clear();
}

// estout `MatchCoef`. A token with no colon matches the NAME; one with
// a colon matches equation and name separately, and an empty half is `*`. This
// engine's coeftable carries no equation for a single-equation model, and
// `transform(*:*mpg* @*2 2)` still transforms `mpg` there (w1b_transform.log
// r9) -- `globMatch("*", "")` is true, so that falls out rather than needing a
// case.
bool trMatch(const std::string& pat, const std::string& eq, const std::string& name) {
  const std::string p = strutil::trim(pat);
  if (p.empty() || p == "*") return true;
  const size_t colon = p.find(':');
  std::string eqPat, xPat;
  if (colon == std::string::npos) {
    xPat = p;
  } else {
    eqPat = strutil::trim(p.substr(0, colon));
    xPat = strutil::trim(p.substr(colon + 1));
  }
  if (eqPat.empty()) eqPat = "*";
  if (xPat.empty()) xPat = "*";
  return strutil::globMatch(eqPat, eq) && strutil::globMatch(xPat, name);
}

// `@` stands for a MATRIX ELEMENT in the ado (`subinstr "@" "\`b'"`, where `b` is
// `bc[i,j]`), which is atomic -- so the substituted literal is PARENTHESISED.
// Without that, `@^2` on a negative coefficient would parse as -(x^2) here and
// as (+x)^2 there, and the sign of every squared transform would differ.
std::string trSubstAt(const std::string& expr, double at) {
  char buf[64];
  std::snprintf(buf, sizeof(buf), "(%.17g)", at);
  std::string out;
  for (char c : expr) {
    if (c == '@') out += buf;
    else out += c;
  }
  return out;
}

// Make ONE model's estimates current for the duration of an expression, because
// `_b[]` in a transform resolves per model (y1). Save/restore rather than a
// second `_b[]` resolver: there are already two of those (macros.cpp and
// display_hardening.cpp) and a third keyed to a different context is how they
// come to disagree.
// eb-order T-P3: `_b[]`/`_se[]` read EvalSession::coefSource(), not
// lastModelResult, so the guard also sets the scoped coefficient source
// (coefOverride) to the stored model; the lastModelResult swap stays for the
// fitted e-scalars (e(N), e(r2)) that resolveEScalar reads from it.
struct TrScopedModel {
  piet::eval::EvalSession& s;
  std::optional<nlohmann::ordered_json> saved;
  const nlohmann::ordered_json* savedOverride;
  TrScopedModel(piet::eval::EvalSession& s_, const nlohmann::ordered_json& snap)
      : s(s_), saved(s_.lastModelResult), savedOverride(s_.coefOverride) {
    s.lastModelResult = snap;
    s.coefOverride = &*s.lastModelResult;
  }
  ~TrScopedModel() {
    s.lastModelResult = saved;
    s.coefOverride = savedOverride;
  }
  TrScopedModel(const TrScopedModel&) = delete;
  TrScopedModel& operator=(const TrScopedModel&) = delete;
};

}  // namespace

bool parseTransformOpt(const std::string& raw, TransformSpec* out, std::string* err) {
  std::string list, subs;
  trSplitSubopts(dequote(raw), &list, &subs);
  if (!strutil::trim(subs).empty()) {
    // `splitCall` is declared further down this file, so the one suboption is
    // parsed here rather than reaching forward for it.
    const std::string sub = strutil::trim(subs);
    const size_t open = sub.find('(');
    const size_t close = sub.rfind(')');
    if (open == std::string::npos || close == std::string::npos || close < open ||
        strutil::lower(strutil::trim(sub.substr(0, open))) != "pattern") {
      *err = "transform(): the only suboption is pattern(); `" + sub + "` names something else";
      return false;
    }
    const std::string inner = sub.substr(open + 1, close - open - 1);
    for (const auto& w : splitWs(inner)) {
      if (w != "0" && w != "1") {
        // estout says so in as many words, and it is r(198)
        // (w1b_transform2.log tr_pat_two).
        *err = "invalid pattern in transform(,pattern()) or eform()";
        return false;
      }
      out->pattern.push_back(w);
    }
  }
  // The ado's own loop, which needs no special case for one or two tokens: with
  // two, the first iteration already has an empty `df` and an empty rest, so
  // (coef, f) slides to (f, df) and the pattern becomes empty.
  const std::vector<std::string> toks = trTokens(list);
  size_t i = 0;
  while (i < toks.size()) {
    std::string coef = toks[i];
    std::string f = (i + 1 < toks.size()) ? toks[i + 1] : std::string();
    std::string df = (i + 2 < toks.size()) ? toks[i + 2] : std::string();
    const bool restEmpty = (i + 3 >= toks.size());
    if (df.empty() && restEmpty) {
      df = f;
      f = coef;
      coef.clear();
    }
    TransformGroup g;
    g.pat = coef;
    g.f = f;
    g.df = df;
    out->groups.push_back(g);
    i += 3;
  }
  return true;
}

TransformCell transformOneCell(const TransformSpec& spec, size_t modelIdx,
                               const std::string& coefName, double b,
                               const nlohmann::ordered_json& snap, SessionState& session) {
  TransformCell out;
  if (spec.empty() || !spec.appliesToModel(modelIdx) || std::isnan(b)) return out;
  std::string eq;
  if (const ojson* row = coefRow(snap, coefName)) eq = row->value("eq", std::string());
  for (const auto& g : spec.groups) {
    if (!trMatch(g.pat, eq, coefName)) continue;
    out.matched = true;
    if (g.f.empty()) break;
    TrScopedModel guard(session.eval, snap);
    // `@` is the coefficient for BOTH halves: the se's multiplier is the
    // derivative evaluated at b, not at the se (estout reads
    // `bc[i,1]`), which is why `eform` spells itself `exp(@) exp(@)`.
    const std::optional<double> nb = piet::evalScalarExpr(trSubstAt(g.f, b), session);
    if (!nb.has_value() || !std::isfinite(*nb)) {
      out.bMissing = true;  // exp(1946) is `.` in the oracle too (u1's _cons)
    } else {
      out.b = *nb;
    }
    if (!g.df.empty()) {
      const std::optional<double> d = piet::evalScalarExpr(trSubstAt(g.df, b), session);
      if (d.has_value() && std::isfinite(*d)) out.seFactor = std::fabs(*d);
      else out.seFactor = std::numeric_limits<double>::quiet_NaN();
    }
    break;
  }
  return out;
}

namespace {

// ===========================================================================
// estout's cells() sub-grammar (plan design decision 4)
//
//   cells(ITEM ...)  /  cells("ITEM ...")
//   ITEM := name | name(sub ...)
//   name := b | se | t | p | <an estadd matrix name, for estpost tables>
//   sub  := star | par | par(L R) | fmt(#|%fmt) | label(text)
//
// Each ITEM is its own ROW (stacked), which is estout's default layout. The
// forms deliberately NOT accepted, each refused BY NAME rather than ignored:
// `&` (side-by-side), `[ ]` (grouping), `pvalue()`, and equation notation --
// every one of them changes the SHAPE of the table, so accepting and ignoring
// would silently produce a different table than the one asked for.
// ===========================================================================

struct CellSpec {
  std::string name;
  bool star = false;
  bool par = false;
  std::string parL = "(";
  std::string parR = ")";
  std::string fmt = "%9.3f";
  bool fmtGiven = false;
  std::string label;
  // outreg2's DEFAULT number shape: significant digits, floor 0 decimals, with
  // thousands separators (outreg2 `dec` defaults to 3 and
  // `fmt` defaults to `fc`; turns the adaptive path off
  // the moment `dec()`/`bdec()` is given). `autoD < 0` means "use fmt".
  int autoD = -1;
};

// Split `name(inner)` into its two halves. Returns false when the parentheses
// do not balance, which is a syntax error rather than a name.
bool splitCall(const std::string& item, std::string* head, std::string* inner) {
  const size_t open = item.find('(');
  if (open == std::string::npos) {
    *head = item;
    inner->clear();
    return true;
  }
  if (item.empty() || item.back() != ')') return false;
  *head = item.substr(0, open);
  *inner = item.substr(open + 1, item.size() - open - 2);
  return true;
}

bool parseCells(const std::string& raw, std::vector<CellSpec>* out, std::string* errOut,
                bool* sideBySideOut = nullptr) {
  const std::string trimmed = strutil::trim(raw);
  const std::string spec = dequote(trimmed);
  // The tokenizer hands the option's argument over VERBATIM (tokenizer.cpp
  // parseOpts takes the text between the parentheses and only strips
  // whitespace), so the quotes are still here to be read -- and dequote, one
  // line down, is what used to erase them before anything could.
  if (sideBySideOut != nullptr) *sideBySideOut = (spec.size() != trimmed.size());
  if (spec.find('&') != std::string::npos) {
    *errOut =
        "estout: cells() side-by-side notation `&` is not implemented in this build; put each "
        "statistic on its own row instead";
    return false;
  }
  // The grouping notation is a bracket at the TOP LEVEL -- `cells([b se])`.
  // A bracket INSIDE a suboption is something else entirely: `par([ ])` names
  // the two delimiters, and that is what the corpus writes. Measured on the 90
  // packages: `par([` appears 327 times across 20 distinct spellings, and a
  // top-level `cells([` appears ZERO times. The depth-blind test this replaces
  // refused all 327 to guard against none of the 0, and the arm that caught it
  // is one of those lines verbatim (w1b_tables_pins.log c7,
  // `cells(se(par([ ]) fmt(2)))`, rc 0 in Stata).
  {
    int depth = 0;
    for (char ch : spec) {
      if (ch == '(') ++depth;
      else if (ch == ')') { if (depth > 0) --depth; }
      else if ((ch == '[' || ch == ']') && depth == 0) {
        *errOut = "estout: cells() grouping notation `[ ]` is not implemented in this build";
        return false;
      }
    }
  }
  for (const auto& item : splitTopLevel(spec)) {
    std::string head, inner;
    if (!splitCall(item, &head, &inner)) {
      *errOut = "estout: unbalanced parentheses in cells(" + raw + ")";
      return false;
    }
    if (head.find(':') != std::string::npos) {
      *errOut = "estout: cells() equation notation `" + head + "` is not implemented in this build";
      return false;
    }
    if (head == "pvalue") {
      *errOut = "estout: cells(pvalue()) is not implemented in this build; use `p`";
      return false;
    }
    CellSpec cs;
    cs.name = head;
    for (const auto& sub : splitTopLevel(inner)) {
      std::string sh, si;
      if (!splitCall(sub, &sh, &si)) {
        *errOut = "estout: unbalanced parentheses in cells(" + raw + ")";
        return false;
      }
      if (sh == "star") {
        cs.star = true;
      } else if (sh == "par") {
        cs.par = true;
        std::vector<std::string> lr = splitTopLevel(si);
        if (lr.size() == 1) {
          // `par([ ])` -- the two delimiters are LITERAL TEXT, and `[`/`]` are
          // not nesting here, but splitTopLevel counts them as depth and hands
          // the pair back as one token. Re-split on plain whitespace when that
          // happens, and only accept it when it yields exactly two. This is the
          // corpus's own spelling: `par([` appears 327 times across 20 distinct
          // lines, against zero uses of the top-level grouping the depth rule
          // exists for.
          const std::vector<std::string> ws = splitWs(si);
          if (ws.size() == 2) lr = ws;
        }
        if (lr.size() == 2) {
          cs.parL = dequote(lr[0]);
          cs.parR = dequote(lr[1]);
        } else if (!si.empty() && lr.size() != 0) {
          *errOut = "estout: par() takes either no argument or exactly two delimiters";
          return false;
        }
      } else if (sh == "fmt") {
        bool ok = false;
        cs.fmt = normalizeFmt(si, &ok);
        if (!ok) {
          // `fmt(a3)` is Stata's adaptive-width format. Rendering it as three
          // fixed decimals would be a different number of digits than asked
          // for, so it is refused rather than approximated (decision 4).
          *errOut = "estout: fmt(" + si + ") is not implemented in this build; use fmt(#) or a "
                                          "%fmt such as fmt(%9.3f)";
          return false;
        }
        cs.fmtGiven = true;
      } else if (sh == "label") {
        cs.label = dequote(si);
      } else if (!sh.empty()) {
        *errOut = "estout: cells() sub-option `" + sh + "` is not implemented in this build";
        return false;
      }
    }
    out->push_back(cs);
  }
  if (out->empty()) {
    *errOut = "estout: cells() is empty";
    return false;
  }
  return true;
}

// ===========================================================================
// The shared coefficient-body builder
// ===========================================================================

struct BodyOpts {
  std::vector<CellSpec> cells;
  // BUG-487: estout's cells() QUOTE is an operator. `cells(a b)` stacks the
  // items as ROWS; `cells("a b")` puts them SIDE BY SIDE -- k*n columns, one
  // body row per coefficient, ordered model-major (m1.b m1.se m2.b m2.se;
  // tests/eval/golden/aer_w1/w1b_estout_cells.log h2). With ONE item the two
  // shapes coincide, which the capture also pins (h5 == h6).
  bool sideBySide = false;
  std::vector<StarLevel> stars;
  std::optional<std::string> keep, drop, order;
  std::map<std::string, std::string> labels;
  bool useVarLabels = false;
  bool nocons = false;
  // o_plain.txt puts the intercept LAST and calls it `Constant` whether or not
  // `label` was given. Both default OFF so `estout`'s frozen fixture keeps its
  // bytes; only outreg2 turns them on.
  bool consLast = false;
  std::string consLabel;
};

// One body row per (coefficient x cell-spec). A model that does not estimate a
// given coefficient gets a BLANK cell, never a zero -- a mixed-estimator table
// is the plan's own edge case and a zero there is a claim of no effect.
void buildBody(Table* t, const std::vector<Model>& models, const BodyOpts& bo,
               SessionState& session, std::string* errOut = nullptr) {
  std::vector<std::string> names = coefNames(models, bo.keep, bo.drop, bo.order, errOut);
  if (bo.consLast) {
    std::vector<std::string> reordered;
    bool sawCons = false;
    for (const auto& n : names) {
      if (n == "_cons") {
        sawCons = true;
      } else {
        reordered.push_back(n);
      }
    }
    if (sawCons) reordered.push_back("_cons");
    names = reordered;
  }
  if (bo.nocons) {
    names.erase(std::remove_if(names.begin(), names.end(),
                               [](const std::string& n) {
                                 return n == "_cons" || n == "(intercept)";
                               }),
                names.end());
  }
  // One (model, coefficient, spec) cell. Shared by both layouts so the two can
  // never disagree about a NUMBER -- only about where it sits.
  auto cellFor = [&](const Model& m, const std::string& coef, const CellSpec& cs,
                     const ojson* r) -> std::optional<std::string> {
    if (r == nullptr) return std::nullopt;
    ojson v;
    if (cs.name == "b" || cs.name == "coef") {
      v = r->contains("b") ? (*r)["b"] : ojson(nullptr);
    } else if (cs.name == "se" || cs.name == "stderr") {
      v = r->contains("se") ? (*r)["se"] : ojson(nullptr);
    } else if (cs.name == "t") {
      v = r->contains("t") ? (*r)["t"] : ojson(nullptr);
    } else if (cs.name == "p") {
      v = r->contains("p") ? (*r)["p"] : ojson(nullptr);
    } else if (m.added["matrices"].contains(cs.name)) {
      const ojson& mat = m.added["matrices"][cs.name];
      if (mat.contains("colnames") && mat.contains("data") && mat["data"].is_array() &&
          !mat["data"].empty()) {
        const ojson& cn = mat["colnames"];
        for (size_t j = 0; j < cn.size(); ++j) {
          if (cn[j].is_string() && cn[j].get<std::string>() == coef) {
            if (j < mat["data"][0].size()) v = mat["data"][0][j];
            break;
          }
        }
      }
    }
    if (v.is_null()) return std::nullopt;
    std::string cell;
    if (cs.autoD >= 0) {
      cell = (v.is_number() && !std::isnan(v.get<double>()))
                 ? sigDigits(v.get<double>(), cs.autoD, /*minDec=*/0, /*comma=*/true)
                 : std::string(kMissingCell);
    } else {
      cell = fmtJson(v, cs.fmt);
    }
    if (cs.star && r->contains("p")) cell += starsFor((*r)["p"], bo.stars);
    if (cs.par) cell = cs.parL + cell + cs.parR;
    return cell;
  };

  // BUG-487, the WIDE layout: one row per coefficient, k*n cells, model-major.
  if (bo.sideBySide) {
    for (const auto& coef : names) {
      TRow row;
      row.kind = TRow::kCoef;
      row.label = labelFor(coef, bo.labels, bo.useVarLabels, session);
      if (coef == "_cons" && !bo.consLabel.empty()) row.label = bo.consLabel;
      bool anything = false;
      for (const auto& m : models) {
        const ojson* r = coefRow(m.snap, coef);
        for (const auto& cs : bo.cells) {
          auto c = cellFor(m, coef, cs, r);
          row.cells.push_back(c.value_or(std::string()));
          if (c.has_value() && !c->empty()) anything = true;
        }
      }
      if (anything) t->body.push_back(row);
    }
    return;
  }

  for (const auto& coef : names) {
    for (size_t ci = 0; ci < bo.cells.size(); ++ci) {
      const CellSpec& cs = bo.cells[ci];
      TRow row;
      row.kind = (ci == 0) ? TRow::kCoef : TRow::kSe;
      // Only the FIRST cell spec is labelled: `x / (0.045)` is one coefficient
      // shown two ways, not two coefficients.
      row.label = (ci == 0) ? labelFor(coef, bo.labels, bo.useVarLabels, session)
                            : (cs.label.empty() ? std::string() : cs.label);
      if (ci == 0 && coef == "_cons" && !bo.consLabel.empty()) row.label = bo.consLabel;
      for (const auto& m : models) {
        auto c = cellFor(m, coef, cs, coefRow(m.snap, coef));
        row.cells.push_back(c.value_or(std::string()));
      }
      // A row where every model is blank carries nothing.
      bool anything = false;
      for (const auto& c : row.cells) {
        if (!c.empty()) anything = true;
      }
      if (anything) t->body.push_back(row);
    }
  }
}

// The footer: `stats(N r2 ...)` in the order given. A key that is an estadd
// LOCAL renders as text ("Controls Yes"), which is decision 2's whole point.
void buildFoot(Table* t, const std::vector<Model>& models, const std::vector<std::string>& stats,
               const std::string& fmt, size_t stride = 1) {
  for (const auto& key : stats) {
    TRow row;
    row.kind = TRow::kStat;
    // BUG-486, axis 7. estout labels a stats() row with the KEY AS TYPED --
    // `r2`, not `R-squared` (w1b_estout_defaults.log g3: `r2 .2933891`). The
    // pretty label belongs to `stats(..., labels(...))`, which the caller has
    // not given. `statLabel` stays for that path and for esttab, which builds
    // its own footer and whose 09-09 capture verified those labels.
    row.label = key;
    bool anyText = false;
    for (const auto& m : models) {
      bool isText = false;
      // ...and `N` follows the SAME running format as everything else: `74`
      // under %9.0g (g3), `74.000` under fmt(3) (g4). The `%9.0f` special case
      // here was a guess and the capture refutes it.
      row.cells.push_back(statCell(m, key, fmt, &isText));
      if (isText) anyText = true;
      // BUG-487: under the wide layout a model owns `stride` columns and the
      // statistic sits in the FIRST of them, the rest blank -- `N  74    74  `
      // against a four-column body (w1b_estout_cells.log h7).
      for (size_t pad = 1; pad < stride; ++pad) row.cells.emplace_back();
    }
    if (anyText) row.kind = TRow::kText;
    t->foot.push_back(row);
  }
}

// Column heads: `mlabels()`/`mtitles()` if given, else the model's depvar (what
// esttab prints), else the stored name.
// `nameHeads` picks which side of the model a derived head comes from. estout
// heads its columns with the STORED NAME (`m1`, `m2` -- e2.txt/e_plain.txt row
// 1); esttab heads them with the dependent variable. This build derived the
// depvar for both, which is BUG-486's second axis.
void buildHeads(Table* t, const std::vector<Model>& models,
                const std::optional<std::string>& mlabels, bool numbers, bool heads,
                bool nameHeads = false) {
  if (numbers) {
    for (size_t i = 0; i < models.size(); ++i) {
      t->colNums.push_back("(" + std::to_string(i + 1) + ")");
    }
  }
  if (!heads) return;
  std::vector<std::string> labels;
  if (mlabels.has_value()) {
    // SPLIT FIRST, DEQUOTE EACH TOKEN -- the rule `parseLabelPairs` states and the
    // fourth site in this tree to have broken it. `mlabels("Model One" "Two")`
    // dequoted FIRST becomes `Model One" "Two`, whose remaining quotes make the
    // splitter's quote tracking swallow the separator, so the two labels came out
    // as `Model` and `One" "Two` (w1b_estout_tex.log r8).
    labels = splitTopLevel(*mlabels);
    for (auto& l : labels) l = dequote(l);
  }
  for (size_t i = 0; i < models.size(); ++i) {
    if (i < labels.size()) {
      t->colHeads.push_back(labels[i]);
      continue;
    }
    if (nameHeads) {
      t->colHeads.push_back(models[i].name);
      continue;
    }
    const std::string dv = models[i].snap.value("depvar", std::string());
    t->colHeads.push_back(dv.empty() ? models[i].name : dv);
  }
}

}  // namespace

namespace {

// ===========================================================================
// Raw-argument recovery
//
// `parsed.varlist` is WHITESPACE-split (tokenizer.cpp:685), so
// `estadd local controls "Yes, with FE"` arrives as four tokens with the quotes
// broken across two of them. Every place below that needs a user STRING reads
// it out of `parsed.raw` instead. The comma cut uses the tokenizer's own
// quote-aware scanner rather than a plain find(','), because a comma inside the
// text is not an option separator.
// ===========================================================================

std::string argsAfterHead(const std::string& raw) {
  size_t b = raw.find_first_not_of(" \t");
  if (b == std::string::npos) return {};
  size_t e = b;
  while (e < raw.size() &&
         (std::isalnum(static_cast<unsigned char>(raw[e])) != 0 || raw[e] == '_')) {
    ++e;
  }
  if (e == b) return {};
  const long long comma = eval::findUnquotedComma(raw);
  std::string body = (comma == -1) ? raw : raw.substr(0, static_cast<size_t>(comma));
  if (e >= body.size()) return {};
  return strutil::trim(body.substr(e));
}

// First `:` outside quotes and parens -- estadd's `... : m1 m2` namelist split.
long long findUnquotedColon(const std::string& s) {
  const std::vector<bool> quoted = eval::quotedSpanMask(s);
  int depth = 0;
  for (size_t i = 0; i < s.size(); ++i) {
    if (i < quoted.size() && quoted[i]) continue;
    if (s[i] == '(' || s[i] == '[') ++depth;
    else if (s[i] == ')' || s[i] == ']') --depth;
    else if (s[i] == ':' && depth == 0) return static_cast<long long>(i);
  }
  return -1;
}

// Peel the leading word off a string, returning it and advancing `rest`.
std::string takeWord(std::string* rest) {
  const std::string s = strutil::trim(*rest);
  const size_t sp = s.find_first_of(" \t");
  if (sp == std::string::npos) {
    *rest = "";
    return s;
  }
  *rest = strutil::trim(s.substr(sp));
  return s.substr(0, sp);
}

// ===========================================================================
// Fit-sample column values (estadd ysumm)
//
// The estimation sample is not the loaded dataset: `regress y x if region<=4`
// fits on a subset, and Stata's `estadd ysumm` summarises e(sample), not the
// frame. LastModel records the two shapes this codebase uses for that -- a
// materialized scratch file (`fitDataPath`) and/or an engine-side `--keepif`
// expression -- and this reproduces the same reconstruction handlers_resample.
// cpp:480-500 does, rather than reading the head and being quietly wrong by a
// few hundred observations (BUG-163's measured failure was 3,106 vs 4,649).
// ===========================================================================

bool fitSampleValues(SessionState& session, const std::string& var,
                     std::vector<double>* out, std::string* errOut) {
  if (!session.currentHbin().has_value()) {
    *errOut = "no dataset in memory";
    return false;
  }
  std::string basePath = *session.currentHbin();
  std::string keepif;
  // BUG-1209: the fit's own e(sample) mask on the data, when it has one, IS
  // its sample; replaying a fit condition that reads e(sample) itself (`xtreg
  // y x1 if e(sample), fe`) on the live data selected no rows (StataBE: ymean
  // 2.2668824339561078 there).
  const SessionState::EsampleReading sample = session.esampleReading();
  if (sample.kind == SessionState::EsampleReading::Kind::kColumn) {
    keepif = std::string("(") + kEsampleColumn + "==" + std::to_string(sample.serial) + ")";
  } else if (session.lastModel().has_value()) {
    const auto& lm = *session.lastModel();
    if (!lm.fitDataPath.empty()) basePath = lm.fitDataPath;
    keepif = lm.fitKeepif;
  }
  if (!keepif.empty()) {
    const std::string filtered = mintScratchPath(session.workdir(), "estadd_ysumm");
    const CobraRunResult cr =
        runCobra({"eval", "--data", basePath, "--keepif", keepif, "--out", filtered});
    if (!cr.ok || !cecon::native_exists(filtered)) {
      *errOut = "could not reconstruct the estimation sample";
      return false;
    }
    basePath = filtered;
  }
  try {
    Datastore ds(basePath);
    if (!ds.has(var)) {
      *errOut = "variable " + var + " not found";
      return false;
    }
    const cecon::Column& c = ds.col(var);
    if (c.dtype == cecon::DType::Factor) {
      *errOut = var + " is a string variable";
      return false;
    }
    const double* d = c.f64();
    const int64_t n = c.nbytes / static_cast<int64_t>(sizeof(double));
    out->clear();
    out->reserve(static_cast<size_t>(n));
    for (int64_t i = 0; i < n; ++i) {
      if (!std::isnan(d[i])) out->push_back(d[i]);
    }
  } catch (const std::exception& e) {
    *errOut = e.what();
    return false;
  }
  return true;
}

}  // namespace

// Portions derived from estout 3.33 (MIT).
// See THIRD_PARTY_NOTICES.
// ===========================================================================
// estadd (estout package)
// ===========================================================================

// ---------------------------------------------------------------------------
// BUG-408 / BUG-409 / BUG-421 — the ONE root cause behind all three.
//
// estadd and estpost published every addition into the TABLE bag
// (`session.estAdded` / the stored estimate's `added` object), which is what
// esttab/estout read — and into nothing else. The three surfaces that answer
// `e(NAME)` for a USER read a different store each:
//
//   `matrix M = e(mean)` / `matlist e(mean)`  -> session.eval.eMatrices
//   `display e(k)`                            -> display_hardening's resolver
//   `` `e(controls)' ``                       -> the macro layer's e() stores
//
// so estpost posted seven matrices nothing could copy (rc 111), estadd posted a
// scalar `display e(k)` answered `.` for, and `estadd local` posted a macro the
// macro layer could not see at all. The table was right and every other reader
// was blind: the two-layer divergence class this file's own header warns about.
//
// The fix is to publish into BOTH: the bag stays exactly as it was (every
// renderer, every frozen table fixture unchanged) and the session e() stores
// gain the same names. `pca` already does this — it is the shape a poster is
// supposed to have.
void mirrorIntoEStores(SessionState& session,
                       const std::vector<std::pair<std::string, double>>& scalars,
                       const std::vector<std::pair<std::string, std::string>>& locals,
                       const std::vector<std::pair<std::string, ojson>>& matrices) {
  // An estadd addition lives on top of the fitted e(): the next model install
  // drops it (SessionState::eOnFitNames), as Stata's new fit replaces e()
  // whole -- `estadd r(mean)` after one regress and `estadd mean` after the
  // next must find no e(mean) left over (w2ak_estout_family.log ek_ad_mean).
  for (const auto& [n, v] : scalars) session.noteEOnFit(n);
  for (const auto& [n, v] : locals) session.noteEOnFit(n);
  for (const auto& [n, m] : matrices) session.noteEOnFit(n);
  for (const auto& [n, v] : scalars) session.eval.setEScalar(n, v);
  for (const auto& [n, v] : locals) session.eval.setEString(n, v);
  for (const auto& [n, m] : matrices) {
    eval::EvalMatrix em;
    if (m.contains("rownames") && m["rownames"].is_array())
      em.rownames = m["rownames"].get<std::vector<std::string>>();
    if (m.contains("colnames") && m["colnames"].is_array())
      em.colnames = m["colnames"].get<std::vector<std::string>>();
    if (m.contains("data") && m["data"].is_array()) {
      for (const auto& row : m["data"]) {
        std::vector<double> r;
        if (row.is_array()) {
          for (const auto& cell : row) {
            // jsonNum's rule, transcribed rather than called: that helper is
            // defined further down this file, and a forward declaration for two
            // lines would be the wider change.
            r.push_back(cell.is_number() ? cell.get<double>()
                                         : std::numeric_limits<double>::quiet_NaN());
          }
        }
        em.data.push_back(std::move(r));
      }
    }
    bool replaced = false;
    for (auto& kv : session.eval.eMatrices) {
      if (kv.first == n) { kv.second = em; replaced = true; break; }
    }
    if (!replaced) session.eval.eMatrices.emplace_back(n, std::move(em));
  }
}

namespace {
// `r(name)` -> its number, for the expression estadd evaluates itself: the
// macro layer leaves bare r() on an estadd line alone (eval/macros.cpp), so
// `estadd r(mean)` and `estadd scalar r(sd)` reach the handler as written.
std::string estaddSubstR(const std::string& expr, SessionState& session) {
  std::string out;
  size_t i = 0;
  while (i < expr.size()) {
    // `r(` at a name boundary: the byte before is not a (Unicode-permissive)
    // identifier byte, so `دخلr(x)` is not an r() reference.
    size_t refEnd = 0;
    std::string name;
    if (expr[i] == 'r' && i + 1 < expr.size() && expr[i + 1] == '(' &&
        (i == 0 || !cecon::ident::isIdentByte(expr[i - 1]))) {
      size_t j = i + 2;
      while (j < expr.size() && std::isspace(static_cast<unsigned char>(expr[j]))) ++j;
      const size_t b = j;
      if (j < expr.size() && cecon::ident::isIdentStartByte(expr[j])) {
        while (j < expr.size() && cecon::ident::isIdentByte(expr[j])) ++j;
        name = expr.substr(b, j - b);
        while (j < expr.size() && std::isspace(static_cast<unsigned char>(expr[j]))) ++j;
        if (j < expr.size() && expr[j] == ')') refEnd = j + 1;
      }
    }
    if (refEnd == 0) {
      out += expr[i++];
      continue;
    }
    const double* v = session.eval.rValues.find(name);
    if (v != nullptr) {
      char buf[64];
      std::snprintf(buf, sizeof(buf), "%.17g", *v);
      out += std::isnan(*v) ? std::string(".") : std::string(buf);
    } else {
      out += expr.substr(i, refEnd - i);
    }
    i = refEnd;
  }
  return out;
}
}  // namespace

ojson runEstadd(const eval::ParsedCommand& parsedIn, SessionState& session) {
  // estadd `_on_colon_parse` splits at the colon BEFORE syntax runs,
  // so `estadd local fe "No", replace: n1` is options `replace` and the target
  // n1 (w2ak ek_ad_colon_repl, log:577). The generic option parser saw an
  // option named `replace:`; the options are re-read from the text before the
  // colon instead.
  eval::ParsedCommand parsed = parsedIn;
  std::string colonTargets;
  bool colonAfterComma = false;
  {
    const std::string full = parsed.raw;
    const long long comma = eval::findUnquotedComma(full);
    const long long colonAll = findUnquotedColon(full);
    if (comma >= 0 && colonAll > comma) {
      colonAfterComma = true;
      colonTargets = strutil::trim(full.substr(static_cast<size_t>(colonAll) + 1));
      const std::string optText =
          full.substr(static_cast<size_t>(comma) + 1, static_cast<size_t>(colonAll - comma - 1));
      parsed.opts.clear();
      for (const auto& w : splitTopLevel(optText)) {
        eval::OptValue ov;
        const size_t open = w.find('(');
        if (open != std::string::npos && w.back() == ')') {
          ov.isBool = false;
          ov.strValue = w.substr(open + 1, w.size() - open - 2);
          parsed.opts.emplace_back(strutil::lower(w.substr(0, open)), ov);
        } else {
          ov.isBool = true;
          ov.boolValue = true;
          parsed.opts.emplace_back(strutil::lower(w), ov);
        }
      }
      eval::canonicalizeForCommand(parsed.opts, "estadd");
    }
  }
  if (auto err = eval::rejectUnknownOptions(parsed.opts, kVocabEstadd.known,
                                            kVocabEstadd.unimplemented)) {
    return ojson(*err);
  }

  std::string args = argsAfterHead(parsed.raw);
  if (colonAfterComma) args = strutil::trim(args) + " : " + colonTargets;
  if (args.empty()) {
    // A BARE `estadd` has no sub-verb at all, which is a syntax error rather
    // than an unimplemented feature -- kUnknownSubcommand would name "" as the
    // subcommand.
    return tblSyntax("estadd: a subcommand is required (local, scalar, matrix or ysumm)");
  }

  // `estadd ... : m1 m2` targets ALREADY-STORED estimates rather than the
  // active e(). Split it off before anything else so the sub-verb parser never
  // sees the namelist.
  std::vector<std::string> targets;
  const long long colon = findUnquotedColon(args);
  if (colon >= 0) {
    targets = splitWs(args.substr(static_cast<size_t>(colon) + 1));
    args = strutil::trim(args.substr(0, static_cast<size_t>(colon)));
    if (targets.empty()) {
      return tblSyntax("estadd: `:' must be followed by one or more stored estimate names");
    }
  }

  std::string sub = strutil::lower(takeWord(&args));
  // estadd: the abbreviations exist as programs of their own.
  if (sub == "loc" || sub == "loca") sub = "local";
  if (sub == "sca" || sub == "scal" || sub == "scala") sub = "scalar";
  if (sub == "mat" || sub == "matr" || sub == "matri") sub = "matrix";
  // estadd: `estadd r(name)` is `estadd scalar r(name)` or
  // `estadd matrix r(name)`, whichever r() holds; neither is r(111).
  {
    std::string nm;
    if (parseWholeRName(sub, &nm)) {
      if (session.eval.rValues.find(nm) != nullptr) {
        args = sub + (args.empty() ? std::string() : " " + args);
        sub = "scalar";
      } else if (session.eval.findClassMatrix('r', nm) != nullptr) {
        args = nm + " = " + sub + (args.empty() ? std::string() : " " + args);
        sub = "matrix";
      } else {
        return tblErrorRc(sub + " not found", 111);
      }
    } else if (!strutil::isValidStataIdentifier(sub)) {
      return tblSyntax("invalid subcommand");
    }
  }

  // The sub-verbs that are real estadd features this build does not compute.
  // `unsupportedCommand(..., kNotImplemented)` -- NOT a bare engineError, whose
  // default code answers rc 1 instead of 199.
  static const std::set<std::string> kNotImplementedSubs = {"beta", "vif",
                                                            "expb",  "summ", "sd",
                                                            "svy",   "margins"};
  if (kNotImplementedSubs.count(sub) != 0) {
    return ojson(eval::unsupportedCommand("estadd " + sub, std::nullopt, std::nullopt,
                                          "refused:not_implemented_sub",
                                          eval::UnsupportedCommandKind::kNotImplemented));
  }
  if (sub != "local" && sub != "scalar" && sub != "matrix" && sub != "ysumm" &&
      sub != "ysummarize" && sub != "mean") {
    return ojson(eval::unsupportedCommand("estadd " + sub, std::nullopt, std::nullopt, "piet_gap",
                                          eval::UnsupportedCommandKind::kUnknownSubcommand));
  }

  // estadd confirm_esample: the e(sample) subcommands stop with r(498)
  // when the active results carry none (w2ak ek_ad_ysumm_noest, after
  // `ereturn clear`).
  if ((sub == "ysumm" || sub == "ysummarize" || sub == "mean") && targets.empty() &&
      !session.lastEstBlock.has_value()) {
    return tblErrorRc("e(sample) information not available", 498);
  }
  if (!session.lastEstBlock.has_value() && targets.empty()) {
    // Stata answers rc 301 ("last estimates not found") here, which coarseRc's
    // message rules do not know, so it is stated through `reason`.
    return tblErrorRc("estadd: no estimation result in memory. Run a regression first.", 301);
  }

  const bool replace = hasFlag(parsed.opts, "replace");
  const std::string fp =
      session.lastEstBlock.has_value()
          ? SessionState::estAddFingerprint(*session.lastEstBlock)
          : std::string();

  // ---- gather what this sub-verb wants to add -----------------------------
  // Collected into a staging bag first so a failure adds NOTHING: a half-applied
  // `estadd ysumm` would leave e() carrying four of its eight scalars with no
  // diagnostic about the other four.
  std::vector<std::pair<std::string, double>> addScalars;
  std::vector<std::pair<std::string, std::string>> addLocals;
  std::vector<std::pair<std::string, ojson>> addMatrices;

  if (sub == "local") {
    const std::string name = takeWord(&args);
    if (name.empty()) return tblSyntax("estadd local: a name is required");
    if (!strutil::isValidStataIdentifier(name)) {
      return tblSyntax("estadd local: invalid name " + name);
    }
    addLocals.emplace_back(name, dequote(strutil::trim(args)));
  } else if (sub == "scalar") {
    // estadd. `estadd scalar k = e(F)`, `estadd scalar k e(F)`,
    // `estadd scalar NAME` (copies the scalar NAME) and `estadd scalar r(sd)`
    // (posts e(sd) = r(sd)).
    std::string rest = args;
    std::string name, expr;
    const std::string restTrim = strutil::trim(rest);
    if (parseWholeRName(restTrim, &name)) {
      if (session.eval.rValues.find(name) == nullptr) return tblSyntax("invalid syntax");
      expr = restTrim;
    } else {
      const size_t eq = rest.find('=');
      if (eq == std::string::npos) {
        name = takeWord(&rest);
        expr = strutil::trim(rest);
      } else {
        name = strutil::trim(rest.substr(0, eq));
        expr = strutil::trim(rest.substr(eq + 1));
      }
      if (name.empty() || !strutil::isValidStataIdentifier(name)) {
        return tblSyntax("invalid syntax");
      }
      if (expr.empty()) {
        // `estadd scalar NAME` -> scalar(NAME)
        const std::string* sv = session.eval.scalars.find(name);
        if (sv == nullptr) return tblErrorRc("scalar " + name + " not found", 111);
        expr = *sv;
      }
    }
    if (auto pf = optStr(parsed.opts, "prefix")) name = strutil::trim(*pf) + name;
    expr = estaddSubstR(expr, session);
    // ONE e() reader for the whole bundle: `evalScalarExpr` is the chain
    // `display` already runs (whole-token resolution, then e()/_b[]
    // substitution, then arithmetic). A second reader here is how the two
    // e()-scalar tables in this tree started.
    std::optional<double> v = piet::evalScalarExpr(expr, session);
    if (!v.has_value()) {
      // evalScalarExpr answers "no value" for a MISSING result on purpose (a
      // table cell must not print it as 0). estadd stores missing as missing:
      // StataBE `estadd scalar bc = 0.098/0.000` is rc 0 with e(bc) = .
      // (scratch-r150/uni-oracle es.do; 007's tables divide by a mean shown as
      // 0.000). Only an expression that evaluates cleanly to a missing token
      // takes this path; anything that fails to evaluate is still refused.
      std::string resolveErr;
      const std::string joined = eval::resolveEstScalarRefs(expr, session.eval, &resolveErr);
      if (resolveErr.empty()) {
        std::string raised;
        bool isString = false;
        const auto r = eval::evalArith(joined, &session.eval, &raised, &isString);
        if (r.has_value() && !isString && raised.empty()) {
          const std::string t = strutil::trim(*r);
          if (t == "." || (t.size() == 2 && t[0] == '.' && t[1] >= 'a' && t[1] <= 'z'))
            v = std::numeric_limits<double>::quiet_NaN();
        }
      }
    }
    if (!v.has_value()) {
      return tblError("estadd scalar: could not evaluate `" + expr + "'");
    }
    addScalars.emplace_back(name, *v);
  } else if (sub == "matrix") {
    // estadd: `estadd matrix NAME` copies the matrix NAME,
    // `estadd matrix NAME [=] M` copies M (w2ak ek_e20 `estadd matrix qval :
    // m2 m6`).
    std::string rest = args;
    const size_t eq = rest.find('=');
    std::string name, src;
    if (eq == std::string::npos) {
      name = takeWord(&rest);
      src = strutil::trim(rest);
      if (src.empty()) src = name;
    } else {
      name = strutil::trim(rest.substr(0, eq));
      src = strutil::trim(rest.substr(eq + 1));
    }
    if (name.empty() || src.empty() || !strutil::isValidStataIdentifier(name)) {
      return tblSyntax("invalid syntax");
    }
    if (auto pf = optStr(parsed.opts, "prefix")) name = strutil::trim(*pf) + name;
    const eval::EvalMatrix* mat = session.eval.findMatrix(src);
    if (mat == nullptr) {
      // coarseRc maps "matrix ... not found" through its "not found" rule to
      // rc 111, which is Stata's own code for an unknown matrix name.
      return tblError("matrix " + src + " not found");
    }
    ojson m;
    m["rownames"] = mat->rownames;
    m["colnames"] = mat->colnames;
    ojson data = ojson::array();
    for (const auto& row : mat->data) data.push_back(row);
    m["data"] = data;
    addMatrices.emplace_back(name, m);
  } else if (sub == "mean") {
    // estadd estadd_mean: e(<prefix>mean) is a copy of e(b) whose
    // column j is the e(sample) mean of variable j (`su VAR [aw] if e(sample),
    // meanonly`, pweight read as aweight), .z where the column is not a
    // numeric variable (_cons, a factor level) and . where summarize fails
    // (w2ak_estout_family.log ek_ad_mean `0|3|5.175|2.25|.z|`).
    if (!session.lastModel().has_value()) {
      return tblErrorRc("e(sample) information not available", 498);
    }
    std::string prefix;
    if (auto pf = optStr(parsed.opts, "prefix")) prefix = strutil::trim(dequote(*pf));
    int brc = 0;
    std::string bwhy;
    const std::optional<eval::EvalMatrix> eb = session.eval.resolveEMatrix("b", &brc, &bwhy);
    if (!eb.has_value() || eb->data.empty()) {
      return tblErrorRc(bwhy.empty() ? std::string("matrix e(b) not found") : bwhy,
                        brc != 0 ? brc : 111);
    }
    std::string wtype;
    if (const std::string* wt = session.eval.eStrings.find("wtype")) wtype = *wt;
    if (wtype == "pweight") wtype = "aweight";
    std::string wexp;
    if (const std::string* we = session.eval.eStrings.find("wexp")) wexp = *we;
    const std::string wclause = wtype.empty() ? std::string() : " [" + wtype + wexp + "]";
    // The variables in memory and whether each is numeric: the resident card
    // when it is current, else the datastore (the card avoids writing a dirty
    // virtual head to disk just to read names).
    std::map<std::string, bool> numericOf;
    if (const std::vector<VarInfo>* card = session.residentReadCard()) {
      for (const auto& vi : *card) numericOf[vi.name] = vi.dtype != "factor";
    } else if (session.hasData()) {
      if (Datastore* ds = session.datastore())
        for (const auto& nm : ds->names()) numericOf[nm] = ds->dtype(nm) != "factor";
    }
    std::vector<double> means;
    for (const auto& full : eb->colnames) {
      const size_t colon = full.find(':');
      const std::string var = colon == std::string::npos ? full : full.substr(colon + 1);
      const auto nit = numericOf.find(var);
      const bool numericVar = nit != numericOf.end() && nit->second;
      if (!numericVar) {
        means.push_back(eval::stataMissingWithKind(26));  // .z, the matrix layer's spelling
        continue;
      }
      const ojson r =
          piet::dispatch("quietly summarize " + var + wclause + " if e(sample), meanonly", session);
      bool failed = false;
      if (r.is_array()) {
        for (const auto& b : r)
          if (b.is_object() && b.value("type", std::string()) == "error") failed = true;
      }
      const double* m = session.eval.rValues.find("mean");
      means.push_back(failed || m == nullptr ? std::numeric_limits<double>::quiet_NaN() : *m);
    }
    ojson m;
    m["rownames"] = eb->rownames;
    m["colnames"] = eb->colnames;
    ojson data = ojson::array();
    data.push_back(means);
    m["data"] = data;
    addMatrices.emplace_back(prefix + "mean", m);
  } else {  // ysumm
    if (!session.lastModel().has_value() && !session.lastEstBlock.has_value()) {
      return tblErrorRc("estadd ysumm: no estimation result in memory", 301);
    }
    // eb-order T-P3: a posting with no fitted model behind it (estpost) has no
    // e(sample): Stata's estadd stops with r(498) "e(sample) information not
    // available" (w2v_estpost_ematrix.log ep14_estadd_ysumm, ep14n_estadd_ysumm).
    // BUG-1210: so do results read by `estimates use`, which carry no e(sample).
    if (!session.lastModel().has_value() ||
        session.lastModel()->esample == LastModel::EsampleState::kNone) {
      return tblErrorRc("e(sample) information not available", 498);
    }
    std::string depvar;
    if (session.lastEstBlock.has_value()) depvar = session.lastEstBlock->value("depvar", "");
    if (depvar.empty() && session.lastModel().has_value()) depvar = session.lastModel()->depvar;
    if (depvar.empty() && session.lastModel().has_value()) depvar = session.lastModel()->y;
    if (depvar.empty()) {
      return tblError("estadd ysumm: the last estimation does not name a dependent variable");
    }
    std::vector<double> vals, wts;
    std::string ferr;
    const LastModel& lmw = *session.lastModel();
    if (!lmw.fitWeight.empty()) {
      // estadd: `summarize ... [aw]` over e(sample) after a
      // weighted fit (pweight read as aweight). The sample is rebuilt as the
      // complete cases of the model's own variables in the data in memory and
      // CHECKED against e(N) (the beta rule); a fit with an if/in filter is
      // refused rather than summarised over the wrong rows.
      if (!lmw.fitKeepif.empty()) {
        return tblError("estadd ysumm: a weighted fit with an if/in restriction is not "
                        "summarised in this build");
      }
      try {
        Datastore& ds = *session.datastore();
        std::vector<std::string> need{depvar, lmw.fitWeight};
        for (const auto& x : lmw.x) {
          if (ds.has(x)) need.push_back(x);
        }
        std::vector<const double*> cols;
        int64_t nrows = -1;
        for (const auto& v : need) {
          if (!ds.has(v) || ds.col(v).dtype == cecon::DType::Factor) {
            return tblError("estadd ysumm: variable " + v + " not found");
          }
          const cecon::Column& c = ds.col(v);
          const int64_t nn = c.nbytes / static_cast<int64_t>(sizeof(double));
          if (nrows < 0) nrows = nn;
          cols.push_back(c.f64());
        }
        for (int64_t i = 0; i < nrows; ++i) {
          bool ok = true;
          for (const double* c : cols) {
            if (std::isnan(c[i])) ok = false;
          }
          if (ok) {
            vals.push_back(cols[0][i]);
            wts.push_back(cols[1][i]);
          }
        }
      } catch (const std::exception& e) {
        return tblError(std::string("estadd ysumm: ") + e.what());
      }
      double nObs = 0.0;
      if (session.lastEstBlock.has_value() && session.lastEstBlock->contains("n_obs") &&
          (*session.lastEstBlock)["n_obs"].is_number()) {
        nObs = (*session.lastEstBlock)["n_obs"].get<double>();
      }
      if (nObs > 0 && static_cast<double>(vals.size()) != nObs) {
        return tblError("estadd ysumm: the estimation sample cannot be reproduced from the data "
                        "in memory");
      }
    } else if (!fitSampleValues(session, depvar, &vals, &ferr)) {
      return tblError("estadd ysumm: " + ferr);
    }
    const double n = static_cast<double>(vals.size());
    const double nan = std::numeric_limits<double>::quiet_NaN();
    // summarize's weights: aweights rescale to sum to N (mean = sum(wx)/sum(w),
    // var = sum(w(x-m)^2)/sum(w) * N/(N-1)); fweights count observations.
    const bool fw = lmw.fitWtype == "fweight" || lmw.fitWtype == "fw";
    double sw = 0.0, sum = 0.0;
    for (size_t i = 0; i < vals.size(); ++i) {
      const double wi = wts.empty() ? 1.0 : wts[i];
      sw += wi;
      sum += wi * vals[i];
    }
    const double mean = sw > 0 ? sum / sw : nan;
    double m2 = 0.0, m3 = 0.0, m4 = 0.0;
    for (size_t i = 0; i < vals.size(); ++i) {
      const double wi = wts.empty() ? 1.0 : wts[i] * (fw ? 1.0 : n / sw);
      const double d = vals[i] - mean;
      m2 += wi * d * d;
      m3 += wi * d * d * d;
      m4 += wi * d * d * d * d;
    }
    if (!wts.empty() && !fw) sum = mean * n;  // aweights: r(sum) is over rescaled weights
    const double nEff = (fw && !wts.empty()) ? sw : n;
    // Stata's summarize reports the SAMPLE variance (n-1) but POPULATION
    // skewness/kurtosis (n divisor, kurtosis NOT excess -- a normal draw
    // reports ~3, not ~0). Mixing those two conventions is the classic silent
    // wrong number here, so both are spelled out.
    const double var = nEff > 1 ? m2 / (nEff - 1) : nan;
    const double sd = n > 1 ? std::sqrt(var) : nan;
    const double m2p = nEff > 0 ? m2 / nEff : nan;
    const double skew = (nEff > 0 && m2p > 0) ? (m3 / nEff) / std::pow(m2p, 1.5) : nan;
    const double kurt = (nEff > 0 && m2p > 0) ? (m4 / nEff) / (m2p * m2p) : nan;
    double mn = nan, mx = nan;
    if (!vals.empty()) {
      mn = *std::min_element(vals.begin(), vals.end());
      mx = *std::max_element(vals.begin(), vals.end());
    }
    // estadd: the statistics asked for (default `sd max min
    // mean`), posted in the ado's order -- iqr semean cv range, then the
    // Stats list p99 ... sum mean filtered -- under prefix() (default `y`).
    std::string prefix = "y";
    if (auto pf = optStr(parsed.opts, "prefix")) prefix = strutil::trim(dequote(*pf));
    auto flag = [&](const char* f) { return hasFlag(parsed.opts, f); };
    const bool all = flag("all");
    std::vector<std::string> stats;
    const std::vector<std::string> kStats = {"p99", "p95", "p90", "p75", "p50", "p25", "p10", "p5",
                                             "p1",  "kurtosis", "skewness", "var", "sd", "max",
                                             "min", "sum", "mean"};
    bool p25 = flag("p25"), p50 = flag("p50"), p75 = flag("p75");
    if (flag("q")) p25 = p50 = p75 = true;
    if (flag("median")) p50 = true;
    for (const auto& st : kStats) {
      bool on = all || flag(st.c_str());
      if (st == "p25") on = all || p25;
      if (st == "p50") on = all || p50;
      if (st == "p75") on = all || p75;
      if (on) stats.push_back(st);
    }
    const bool iqr = all || flag("iqr"), semean = all || flag("semean"), cv = all || flag("cv"),
               range = all || flag("range");
    if (stats.empty() && !range && !cv && !semean && !iqr) stats = {"sd", "max", "min", "mean"};
    for (const auto& st : stats) {
      if (st[0] == 'p' && st != "p") {
        return ojson(eval::unsupportedOption(
            st, "estadd ysumm: the percentile statistics are not implemented in this build"));
      }
    }
    if (iqr) {
      return ojson(eval::unsupportedOption(
          "iqr", "estadd ysumm: the percentile statistics are not implemented in this build"));
    }
    std::map<std::string, double> val = {{"mean", mean}, {"sd", sd},   {"min", mn},
                                         {"max", mx},    {"sum", sum}, {"var", var},
                                         {"skewness", skew}, {"kurtosis", kurt}};
    if (semean) addScalars.emplace_back(prefix + "semean", n > 0 ? sd / std::sqrt(n) : nan);
    if (cv) addScalars.emplace_back(prefix + "cv", sd / mean);
    if (range) addScalars.emplace_back(prefix + "range", mx - mn);
    for (const auto& st : stats) addScalars.emplace_back(prefix + st, val[st]);
  }

  // ---- decide WHERE it lands ----------------------------------------------
  // `: m1 m2` writes into each named STORED estimate's own bag (a mutable
  // handle, which is why SessionState grew `estimatesGetMutable`); everything
  // else writes the live bag beside the active e().
  std::vector<ojson*> bags;
  if (!targets.empty()) {
    // W2 esttabnames (BUG-552): `estadd ... : A_*` -- the target list is a
    // namelist too (estadd.ado expands it against the stored names). The
    // active results (`.`) are not a stored bag; refused by name below.
    SessionState::EstNamelist ex = session.expandEstimatesNamelist(targets);
    if (ex.rc != 0) return tblErrorRc(ex.message, ex.rc);
    for (const auto& t : ex.names) {
      if (t == ".") {
        return tblError("estadd: `.` in the `: namelist` (the active, unstored results) is not "
                        "supported in this version of PIET; run `estadd` without the colon to "
                        "add to the active results");
      }
      ojson* snap = session.estimatesGetMutable(t);
      if (snap == nullptr) {
        return tblError("estadd: no stored estimation result named " + t);
      }
      if (!snap->contains("added") || !(*snap)["added"].is_object()) {
        (*snap)["added"] = emptyBag();
      }
      for (const char* k : {"scalars", "locals", "matrices"}) {
        if (!(*snap)["added"].contains(k)) (*snap)["added"][k] = ojson::object();
      }
      bags.push_back(&(*snap)["added"]);
    }
  } else {
    if (session.estAddedFp != fp) {
      // A DIFFERENT estimation has replaced e() since the bag was opened.
      // Discarding is the rule; keeping would attach one model's additions to
      // another model's table -- a plausible number on the right-looking row.
      session.estAddedClear();
    }
    // The bag shape is EXACTLY {scalars, locals, matrices}: `runEststo` copies
    // it verbatim into the snapshot and every renderer here reads those three
    // keys, so a fourth key or a missing one is a silently empty table cell.
    for (const char* k : {"scalars", "locals", "matrices"}) {
      if (!session.estAdded.contains(k) || !session.estAdded[k].is_object()) {
        session.estAdded[k] = ojson::object();
      }
    }
    bags.push_back(&session.estAdded);
    // W2AK: when the active results ARE a stored model (`eststo p1: ...` then a
    // bare `estadd`), the stored copy carries the addition too -- `esttab p1,
    // stats(postst)` prints it (w2ak ek_ad_after_tab L11, log:637; the 069
    // shape ek_d01 `eststo:` then `estadd scalar mean = r(mean)`). estadd.ado
    // runs the subcommand on e() with e(_estimates_name) set, and the store is
    // what esttab reads back.
    if (!session.activeEstimatesName.empty() && session.lastEstBlock.has_value()) {
      ojson* snap = session.estimatesGetMutable(session.activeEstimatesName);
      if (snap != nullptr) {
        if (!snap->contains("added") || !(*snap)["added"].is_object()) (*snap)["added"] = emptyBag();
        for (const char* k : {"scalars", "locals", "matrices"}) {
          if (!(*snap)["added"].contains(k)) (*snap)["added"][k] = ojson::object();
        }
        bags.push_back(&(*snap)["added"]);
      }
    }
  }

  // ---- collision check across EVERY target, before writing ANY of them -----
  if (!replace) {
    for (ojson* bag : bags) {
      for (const auto& [n, v] : addScalars) {
        (void)v;
        if ((*bag)["scalars"].contains(n)) {
          // "already defined" is one of coarseRc's own measured message rules
          // (-> rc 110, Stata's confirm-new-name code), so this wording is the
          // rc as much as it is the sentence.
          return tblError("estadd: e(" + n + ") already defined; use replace");
        }
      }
      for (const auto& [n, v] : addLocals) {
        (void)v;
        if ((*bag)["locals"].contains(n)) {
          return tblError("estadd: e(" + n + ") already defined; use replace");
        }
      }
      for (const auto& [n, v] : addMatrices) {
        (void)v;
        if ((*bag)["matrices"].contains(n)) {
          return tblError("estadd: e(" + n + ") already defined; use replace");
        }
      }
    }
  }

  for (ojson* bag : bags) {
    for (const auto& [n, v] : addScalars) (*bag)["scalars"][n] = v;
    for (const auto& [n, v] : addLocals) (*bag)["locals"][n] = v;
    for (const auto& [n, v] : addMatrices) (*bag)["matrices"][n] = v;
  }
  // BUG-408/409/421: the live e() gains the same names, so `display e(k)`,
  // `` `e(controls)' `` and `matrix M = e(mean)` can read what the table reads.
  // Only for the LIVE bag: `estadd ... : m1 m2` writes stored estimates, whose
  // e() is not the active one.
  if (targets.empty()) mirrorIntoEStores(session, addScalars, addLocals, addMatrices);
  // MIRROR the scalars into e() itself, so `display e(k)` answers 7 rather than
  // `.`. Stata's estadd genuinely ADDS to e() -- the bag is this build's way of
  // carrying the addition to `eststo`, not a substitute for e() being readable,
  // and a table row the macro layer cannot see is the pangram's own stated test
  // (S28 reads `e(s28k)` back immediately after adding it).
  //
  // Only names the estimate does NOT already carry: `estadd scalar r2 = 5` must
  // not overwrite the fitted R-squared behind `display e(r2)`. The addition
  // still reaches `stats(r2)` through the bag, so nothing is lost by declining
  // to shadow an engine key.
  // The two stores are written TOGETHER on purpose: `eval.lastModelResult` is
  // what macros.cpp's e() resolver reads and `lastEstBlock` is what the table
  // path reads, and updating one without the other is the two-layer divergence
  // this codebase keeps paying for.
  if (targets.empty() && session.lastEstBlock.has_value() && !addScalars.empty()) {
    ojson blk = *session.lastEstBlock;
    for (const auto& [n, v] : addScalars) {
      if (!blk.contains(n)) blk[n] = v;
    }
    session.lastEstBlock = blk;
    if (session.eval.lastModelResult.has_value()) {
      ojson res = *session.eval.lastModelResult;
      for (const auto& [n, v] : addScalars) {
        if (!res.contains(n)) res[n] = v;
      }
      session.eval.lastModelResult = res;
    }
  }
  // Re-stamp the fingerprint from the SHARED helper, unconditionally. It is
  // `SessionState::estAddFingerprint` and not a private copy on purpose:
  // `runEststo` READS it to decide whether the bag still belongs to the active
  // estimate, and two copies differing by one field would make the bag never
  // match -- an `estadd`ed row quietly missing from the table, at rc 0. The
  // mirror above cannot move it: the fingerprint is built from depvar,
  // estimator, n_obs and the coeftable LENGTH, none of which a new scalar key
  // touches.
  if (targets.empty() && session.lastEstBlock.has_value()) {
    session.estAddedFp = SessionState::estAddFingerprint(*session.lastEstBlock);
  }

  // One block per addition, as an ARRAY -- legal here because the arm routes
  // through `pushEnvResult` (which flattens) AND every element is a real block
  // object with a "type" key. See defect class 1 in this file's header.
  ojson out = ojson::array();
  if (!hasFlag(parsed.opts, "quietly")) {
    for (const auto& [n, v] : addScalars) {
      out.push_back(displayBlock("added scalar: e(" + n + ") = " + fmtNum(v, "%9.7g")));
    }
    for (const auto& [n, v] : addLocals) {
      out.push_back(displayBlock("added macro:  e(" + n + ") : \"" + v + "\""));
    }
    for (const auto& [n, v] : addMatrices) {
      (void)v;
      out.push_back(displayBlock("added matrix: e(" + n + ")"));
    }
  }
  return out;
}

// ===========================================================================
// estpost -- the pseudo-estimate (plan design decision 3)
//
// Stata's estpost posts a summary table into e() so that `esttab, cells(mean)`
// can read it. The posted objects are 1xk MATRICES named after the statistic
// (e(mean), e(sd), ...), with the variables as column names -- which is exactly
// the shape `estadd matrix` already stores, so estpost reuses that bag rather
// than inventing a second one.
//
// The numbers come from the SAME cores the native commands use:
//   summarize -> handlers::summarizeCoreStats (handlers.cpp)
//   tabstat   -> the `cobra tabstat --tidy` argv, re-assembled here
// `runTabstatCore` (estimation_m4.cpp:967-1013) is in an anonymous namespace
// and is therefore NOT callable from this TU. Re-assembling its six-line argv
// is cheaper than exposing a symbol out of that hot file, and the SOURCE of the
// numbers -- the engine -- is identical either way, which is the property that
// matters. A native re-implementation of the statistics would not be.
// ===========================================================================

namespace {

// A 1xk matrix in the `added.matrices` shape.
ojson rowMatrix(const std::vector<std::string>& colnames, const std::vector<double>& values) {
  ojson m;
  m["rownames"] = ojson::array({"y1"});
  m["colnames"] = colnames;
  ojson data = ojson::array();
  ojson row = ojson::array();
  for (double v : values) {
    if (std::isnan(v)) {
      row.push_back(nullptr);
    } else {
      row.push_back(v);
    }
  }
  data.push_back(row);
  m["data"] = data;
  return m;
}

double jsonNum(const ojson& v) {
  if (v.is_number()) return v.get<double>();
  return std::numeric_limits<double>::quiet_NaN();
}

// The statistic names estpost summarize posts, in Stata's own order. `N` is
// renamed to `count` because that is the e() name esttab's `cells(count)`
// asks for; the rest keep the summarize block's spelling.
struct SummStat {
  const char* eName;
  const char* blockKey;
  bool detailOnly;
};
const std::vector<SummStat>& summStats() {
  static const std::vector<SummStat> k = {
      {"count", "N", false}, {"sum_w", "sum_w", false}, {"mean", "mean", false},
      {"Var", "var", false}, {"sd", "sd", false}, {"min", "min", false},
      {"max", "max", false}, {"sum", "sum", false},
      {"p1", "p1", true},     {"p25", "p25", true},   {"p50", "p50", true},
      {"p75", "p75", true},   {"p99", "p99", true},
  };
  return k;
}

// Turn the posted matrices into the on-screen table. One row per variable, one
// column per statistic -- `columns(variables)` transposes it, which is what
// that option means and why it is not a silent no-op here.
Table estpostTable(const std::vector<std::string>& vars,
                   const std::vector<std::string>& statNames, const ojson& matrices,
                   bool byVariables, const std::string& title) {
  Table t;
  t.title = title;
  auto cell = [&](const std::string& stat, size_t varIdx) -> std::string {
    if (!matrices.contains(stat)) return "";
    const ojson& m = matrices.at(stat);
    if (!m.contains("data") || !m["data"].is_array() || m["data"].empty()) return "";
    const ojson& row = m["data"][0];
    if (varIdx >= row.size()) return "";
    if (row[varIdx].is_null()) return kMissingCell;
    return fmtNum(jsonNum(row[varIdx]), stat == "count" ? "%9.0f" : "%9.4f");
  };
  if (byVariables) {
    t.colHeads = vars;
    for (const auto& s : statNames) {
      TRow r;
      r.kind = TRow::kStat;
      r.label = s;
      for (size_t v = 0; v < vars.size(); ++v) r.cells.push_back(cell(s, v));
      t.body.push_back(r);
    }
  } else {
    t.colHeads = statNames;
    for (size_t v = 0; v < vars.size(); ++v) {
      TRow r;
      r.kind = TRow::kStat;
      r.label = vars[v];
      for (const auto& s : statNames) r.cells.push_back(cell(s, v));
      t.body.push_back(r);
    }
  }
  return t;
}

// Install the pseudo-estimate. `lastEstBlock` is what `eststo` snapshots, what
// `estadd` fingerprints, and -- since bundle E -- what a bare `esttab` falls
// back to. estpost therefore has to write it for anything downstream to see
// the table at all.
//
// `session.eststo(...)` is deliberately NOT called: Stata's estpost posts to
// e() and never stores, and minting an `m1` as a side effect would also change
// what a LATER `eststo` numbers itself.
// `setLastModel` is not called either: estpost is not a fitted model, and
// claiming one would let `predict`/`test` address a summary table. eb-order
// T-P3: nor may the PREVIOUS fit stay behind it -- with its LastModel alive,
// `_b[]`, test/lincom/nlcom/margins/predict and `estimates save` all answered
// from that fit at rc 0 (SPEC C, REPORT a12/a13) -- so the whole e() is reset
// first, as `clear results` does.
//
// THE COEFTABLE IS NOT EMPTY, and this is a deliberate departure from the
// plan's literal `coeftable:[]`. Stata's `ereturn post` requires a b vector,
// and estpost supplies one -- which is why a bare `esttab` after `estpost
// summarize` prints the means at all. An empty coeftable renders an EMPTY
// TABLE at rc 0, with the statistics sitting unreachable in e(mean). So e(b)
// carries the primary statistic and e(se) the dispersion one, and the 1xk
// matrices are posted alongside exactly as the plan specifies.
struct PseudoCoef {
  std::string name;
  double b = std::numeric_limits<double>::quiet_NaN();
  double se = std::numeric_limits<double>::quiet_NaN();
};

void postPseudoEstimate(SessionState& session, const std::string& estimator, double nObs,
                        const ojson& matrices, const std::vector<PseudoCoef>& coefs) {
  // eb-order T-P3 part 2: a NEW e() with no fitted model behind it. Reset
  // BEFORE anything is written: lastModel_reset also clears the block, the
  // estadd bag and the store name, which this function then writes again.
  session.lastModel_reset();
  ojson coeftable = ojson::array();
  for (const auto& c : coefs) {
    ojson row;
    row["name"] = c.name;
    row["b"] = std::isnan(c.b) ? ojson(nullptr) : ojson(c.b);
    row["se"] = std::isnan(c.se) ? ojson(nullptr) : ojson(c.se);
    // No t and no p: a summary statistic has no null hypothesis, and a zero
    // here would draw significance stars onto a column of means.
    row["t"] = nullptr;
    row["p"] = nullptr;
    coeftable.push_back(row);
  }
  ojson block;
  block["type"] = "estimation";
  block["estimator"] = estimator;
  block["depvar"] = "";
  // An INTEGER, because the shared fingerprint reads n_obs with get<long long>()
  // and because e(N) is a count.
  block["n_obs"] = static_cast<long long>(nObs);
  block["coeftable"] = coeftable;
  // eb-order T-P3: the coeftable above is esttab's table, not an e(b). Stata's
  // estpost posts no e(b)/e(V) (estpost), so `_b[x1]`, `matrix
  // list e(b)` and `: colnames e(b)` refuse and test/lincom/predict are r(321)
  // (w2v_estpost_ematrix.log ep13_local_bx1 111, ep13_list_eb 111, ep14_test
  // 321). SessionState::blockPostsEb reads the mark.
  block["posts_e_b"] = false;
  session.lastEstBlock = block;
  // W2 esttabnames: `ereturn post` drops the hidden e(_estimates_name), so
  // these results carry no store name (`esttab .` is the live posting).
  session.activeEstimatesName.clear();
  session.estAddedClear();
  session.estAdded["matrices"] = matrices;
  // BUG-408: estpost is a NEW e(), so the session stores are replaced, not
  // added to — then the posted matrices land in them and `matrix M = e(mean)`
  // resolves. Without the clear, a previous estimation's e(b)/e(V) would sit
  // beside estpost's summary matrices and `ereturn list` would show both.
  session.eval.eScalars.clear();
  session.eval.eStrings.clear();
  session.eval.eMatrices.clear();
  // eb-order T-P3: e(cmd) `estpost` and e(subcmd) the wrapped command, which
  // estpost.ado posts for every subcommand (summarize, tabstat;
  // ~/Library/Application Support/Stata/ado/plus/e/estpost.ado).
  session.eval.setEString("cmd", "estpost");
  {
    const std::string pre = "estpost_";
    session.eval.setEString("subcmd", estimator.rfind(pre, 0) == 0 ? estimator.substr(pre.size())
                                                                    : estimator);
  }
  {
    std::vector<std::pair<std::string, ojson>> mm;
    for (const auto& [k, v] : matrices.items()) mm.emplace_back(k, v);
    std::vector<std::pair<std::string, double>> scalars = {{"N", nObs}};
    // estpost.ado posts k for summarize, while tabstat posts only N.
    if (estimator == "estpost_summarize")
      scalars.emplace_back("k", static_cast<double>(coefs.size()));
    mirrorIntoEStores(session, scalars, {}, mm);
  }
  // Stamped from the SHARED helper, after the block is complete -- the
  // coeftable length is part of the fingerprint.
  session.estAddedFp = SessionState::estAddFingerprint(block);
}

// The two statistics that become e(b) and e(se). `mean`/`sd` when they were
// computed, else the first two requested -- so `estpost tabstat x, stat(sum)`
// still posts something addressable rather than a blank column.
void pickPrimary(const std::vector<std::string>& statNames, std::string* bStat,
                 std::string* seStat) {
  const auto have = [&](const char* n) {
    return std::find(statNames.begin(), statNames.end(), n) != statNames.end();
  };
  *bStat = have("mean") ? "mean" : (statNames.empty() ? std::string() : statNames[0]);
  if (have("sd")) {
    *seStat = "sd";
  } else {
    for (const auto& s : statNames) {
      if (s != *bStat) { *seStat = s; break; }
    }
  }
}

// Read one cell out of a posted 1xk matrix by column position.
double matCell(const ojson& matrices, const std::string& stat, size_t j) {
  if (stat.empty() || !matrices.contains(stat)) return std::numeric_limits<double>::quiet_NaN();
  const ojson& m = matrices.at(stat);
  if (!m.contains("data") || !m["data"].is_array() || m["data"].empty()) {
    return std::numeric_limits<double>::quiet_NaN();
  }
  const ojson& row = m["data"][0];
  if (j >= row.size() || row[j].is_null()) return std::numeric_limits<double>::quiet_NaN();
  return jsonNum(row[j]);
}

}  // namespace

ojson runEstpost(const eval::ParsedCommand& parsed, SessionState& session) {
  if (auto err = eval::rejectUnknownOptions(parsed.opts, kVocabEstpost.known,
                                            kVocabEstpost.unimplemented)) {
    return ojson(*err);
  }
  if (parsed.varlist.empty()) {
    return tblSyntax("estpost: a subcommand is required (summarize or tabstat)");
  }
  const std::string sub = strutil::lower(parsed.varlist[0]);

  static const std::set<std::string> kNotImplementedSubs = {
      "tabulate", "tab", "ttest", "correlate", "corr", "margins", "ci", "stci", "svy"};
  if (kNotImplementedSubs.count(sub) != 0) {
    return ojson(eval::unsupportedCommand("estpost " + sub, std::nullopt, std::nullopt,
                                          "refused:not_implemented_sub",
                                          eval::UnsupportedCommandKind::kNotImplemented));
  }
  const bool isSumm = (sub == "summarize" || sub == "sum" || sub == "summ");
  const bool isTabstat = (sub == "tabstat");
  if (!isSumm && !isTabstat) {
    return ojson(eval::unsupportedCommand("estpost " + sub, std::nullopt, std::nullopt, "piet_gap",
                                          eval::UnsupportedCommandKind::kUnknownSubcommand));
  }

  if (!session.currentHbin().has_value() || session.datastore() == nullptr) {
    return ojson(eval::noDataLoaded());
  }

  std::vector<std::string> vars(parsed.varlist.begin() + 1, parsed.varlist.end());
  if (vars.empty()) {
    if (isTabstat) return tblSyntax("estpost tabstat: at least one variable is required");
    vars = session.datastore()->names();
  }
  for (const auto& v : vars) {
    if (!session.datastore()->has(v)) {
      return tblError("variable " + v + " not found", "unknown_variable");
    }
  }

  const std::optional<std::string> byOpt = optStr(parsed.opts, "by");
  const bool quiet = hasFlag(parsed.opts, "quietly");
  std::optional<std::string> statsOpt = optStr(parsed.opts, "statistics");
  if (!statsOpt.has_value()) statsOpt = optStr(parsed.opts, "stat");
  if (!statsOpt.has_value()) statsOpt = optStr(parsed.opts, "stats");
  const std::optional<std::string> colsOpt = optStr(parsed.opts, "columns");
  bool byVariables = false;
  if (colsOpt.has_value()) {
    const std::string c = strutil::lower(dequote(*colsOpt));
    if (c == "variables" || c == "var") {
      byVariables = true;
    } else if (c != "statistics" && c != "stat" && c != "stats") {
      return tblSyntax("estpost: columns(" + *colsOpt +
                       ") must be columns(statistics) or columns(variables)");
    }
  }

  // `listwise` is casewise deletion ACROSS the variable list, and it is folded
  // into the row filter rather than applied afterwards -- doing it afterwards
  // would summarize each variable over its own complete cases, which is the
  // question `listwise` exists to stop asking.
  std::string ifExprText = parsed.ifExpr.value_or(std::string());
  if (hasFlag(parsed.opts, "listwise")) {
    std::string cond;
    for (const auto& v : vars) {
      if (!cond.empty()) cond += " & ";
      cond += "!missing(" + v + ")";
    }
    ifExprText = ifExprText.empty() ? cond : ("(" + ifExprText + ") & (" + cond + ")");
  }
  std::optional<std::string> effIf;
  if (!ifExprText.empty()) effIf = ifExprText;

  std::string dataPath = *session.currentHbin();
  try {
    const auto filt = applyIfAndInFilter(session, dataPath, effIf, parsed.inRange);
    if (!filt.ok) return inRangeRejectError(filt, parsed.inRange.value_or(""), "estpost");
    dataPath = filt.dataPath;
  } catch (const CobraError& ce) {
    return tblError(cobraErrorMessage(ce), ce.code);
  }

  ojson matrices = ojson::object();
  std::vector<std::string> statNames;
  double nObs = 0;

  if (isSumm) {
    if (byOpt.has_value()) {
      // estpost summarize has no by() in Stata; saying so names the command
      // that does, instead of accepting the option and summarizing the pooled
      // sample under a grouped heading.
      return tblSyntax(
          "estpost summarize: by() is not part of estpost summarize; use `estpost tabstat "
          "..., by(g)'");
    }
    ojson summ;
    try {
      // The SAME shape runSummarize uses (handlers.cpp:1280-1310): the session's
      // own Datastore when nothing was filtered, and a temporary one wrapped in
      // a StatelessRead when it was. Opening a fresh Datastore on the head
      // unconditionally would evict the resident frame and force the very
      // re-materialisation the resident plane exists to avoid.
      if (dataPath == *session.currentHbin()) {
        summ = summarizeCoreStats(*session.datastore(), vars, /*detail=*/true);
      } else {
        Datastore tmp(dataPath);
        SessionState::StatelessRead stateless(session);
        summ = summarizeCoreStats(tmp, vars, /*detail=*/true);
      }
    } catch (const std::exception& e) {
      return tblError(std::string("estpost summarize: ") + e.what());
    }
    if (isErrorBlock(summ)) return summ;
    const ojson& stats = summ.contains("stats") ? summ["stats"] : ojson::object();
    // A factor variable has no mean; summarizeCoreStats reports N with null
    // statistics for it (handlers.cpp:1193-1208) and estpost carries that
    // through as MISSING, never as zero.
    const bool wantDetail = hasFlag(parsed.opts, "detail");
    for (const auto& st : summStats()) {
      if (st.detailOnly && !wantDetail) continue;
      std::vector<double> vals;
      bool any = false;
      for (const auto& v : vars) {
        double d = std::numeric_limits<double>::quiet_NaN();
        if (stats.contains(v) && stats[v].contains(st.blockKey)) {
          d = jsonNum(stats[v][st.blockKey]);
          if (!std::isnan(d)) any = true;
        }
        vals.push_back(d);
      }
      if (!any && std::string(st.eName) != "count") continue;
      matrices[st.eName] = rowMatrix(vars, vals);
      statNames.push_back(st.eName);
    }
    for (const auto& v : vars) {
      if (stats.contains(v) && stats[v].contains("N")) {
        nObs = std::max(nObs, jsonNum(stats[v]["N"]));
      }
    }
    {
      std::string bStat, seStat;
      pickPrimary(statNames, &bStat, &seStat);
      std::vector<PseudoCoef> coefs;
      for (size_t j = 0; j < vars.size(); ++j) {
        coefs.push_back(PseudoCoef{vars[j], matCell(matrices, bStat, j),
                                   matCell(matrices, seStat, j)});
      }
      postPseudoEstimate(session, "estpost_summarize", nObs, matrices, coefs);
    }
  } else {
    // The `cobra tabstat --tidy` argv, re-assembled (see the block comment
    // above for why it is re-assembled and not called).
    std::vector<std::string> args = {"tabstat", "--vars", joinWith(vars, ",")};
    if (byOpt.has_value() && !byOpt->empty()) {
      args.push_back("--by");
      args.push_back(dequote(*byOpt));
    }
    std::string statCsv = "mean,sd,min,max,n";
    if (statsOpt.has_value() && !statsOpt->empty()) {
      statCsv = joinWith(splitTopLevel(dequote(*statsOpt), ' '), ",");
      // `statistics(mean, sd)` -- a comma-separated list already; collapse the
      // double separator rather than sending an empty stat name to the engine.
      std::vector<std::string> cleaned;
      for (const auto& piece : splitTopLevel(statCsv, ',')) {
        const std::string p = strutil::trim(piece);
        if (!p.empty()) cleaned.push_back(p);
      }
      statCsv = joinWith(cleaned, ",");
    }
    args.push_back("--stats");
    args.push_back(statCsv);
    args.push_back("--data");
    args.push_back(dataPath);
    args.push_back("--tidy");

    ojson raw;
    try {
      raw = runCobraJson(args);
    } catch (const CobraError& ce) {
      return tblError(cobraErrorMessage(ce), ce.code);
    }
    if (raw.contains("stats") && raw["stats"].is_array()) {
      for (const auto& s : raw["stats"]) {
        if (s.is_string()) statNames.push_back(s.get<std::string>());
      }
    }
    if (raw.contains("n_obs")) nObs = jsonNum(raw["n_obs"]);

    const bool grouped = byOpt.has_value() && raw.contains("groups") && raw["groups"].is_array();
    const bool nototal = hasFlag(parsed.opts, "nototal");
    std::vector<std::string> colnames;
    std::map<std::string, std::vector<double>> byStat;
    auto addBlock = [&](const std::string& prefix, const ojson& block) {
      for (const auto& v : vars) {
        colnames.push_back(prefix.empty() ? v : (prefix + ":" + v));
        for (const auto& s : statNames) {
          double d = std::numeric_limits<double>::quiet_NaN();
          if (block.contains(v) && block[v].contains(s)) d = jsonNum(block[v][s]);
          byStat[s].push_back(d);
        }
      }
    };
    if (grouped) {
      for (const auto& g : raw["groups"]) {
        const std::string gname =
            g.contains("value") ? fmtCount(g["value"]) : std::string("g");
        if (g.contains("stats")) addBlock(gname, g["stats"]);
      }
      if (!nototal && raw.contains("total")) addBlock("Total", raw["total"]);
    } else if (raw.contains("total")) {
      addBlock("", raw["total"]);
    }
    for (const auto& s : statNames) {
      matrices[s] = rowMatrix(colnames, byStat[s]);
    }
    {
      std::string bStat, seStat;
      pickPrimary(statNames, &bStat, &seStat);
      std::vector<PseudoCoef> coefs;
      for (size_t j = 0; j < colnames.size(); ++j) {
        coefs.push_back(PseudoCoef{colnames[j], matCell(matrices, bStat, j),
                                   matCell(matrices, seStat, j)});
      }
      postPseudoEstimate(session, "estpost_tabstat", nObs, matrices, coefs);
    }
    if (!colnames.empty()) vars = colnames;
  }

  if (quiet) return ojson::array();
  const Table t = estpostTable(vars, statNames, matrices,
                               byVariables, isSumm ? "Summary statistics"
                                                   : "Summary statistics (tabstat)");
  return echoBlock(renderFixed(t));
}

namespace {

// The active e() as a Model, with the intercept renamed the way `makeSnapshot`
// does -- `_cons` is the spelling every table and every user writes.
bool activeModel(SessionState& session, const std::string& cmd, Model* out, ojson* errOut) {
  if (!session.lastEstBlock.has_value()) {
    if (session.lastModel().has_value() && session.lastModel()->result.has_value()) {
      // A fit that filled LastModel but not lastEstBlock (five TUs assign the
      // latter directly) still has its raw result cached.
      ojson snap = *session.lastModel()->result;
      out->name = cmd;
      out->snap = snap;
      out->added = emptyBag();
      out->escalars = buildEScalars(snap);
      return true;
    }
    *errOut = tblErrorRc(cmd + ": no estimation result in memory. Run a regression first.", 301);
    return false;
  }
  ojson snap = *session.lastEstBlock;
  if (snap.contains("coeftable") && snap["coeftable"].is_array()) {
    for (auto& row : snap["coeftable"]) {
      if (row.contains("name") && row["name"].is_string() &&
          row["name"].get<std::string>() == "(intercept)") {
        row["name"] = "_cons";
      }
    }
  }
  out->name = cmd;
  out->snap = snap;
  out->added = emptyBag();
  if (session.estAddedFp == SessionState::estAddFingerprint(*session.lastEstBlock)) {
    out->added = session.estAdded;
  }
  for (const char* k : {"scalars", "locals", "matrices"}) {
    if (!out->added.contains(k) || !out->added[k].is_object()) out->added[k] = ojson::object();
  }
  out->escalars = buildEScalars(snap);
  if (!out->escalars.contains("N") && snap.contains("n_obs")) {
    out->escalars["N"] = jsonNum(snap["n_obs"]);
  }
  if (snap.contains("coeftable")) out->snap["base_levels"] = factorBaseLevels(snap["coeftable"], session);
  return true;
}

// One place where a rendered table becomes a file. `append` reads the existing
// bytes FIRST and hands the concatenation to the atomic writer, so the
// destination is never opened for writing while it is still a source (the
// dest==source law, BUG-138).
ojson emitTable(const std::string& cmd, SessionState& session, const std::string& userPath,
                const std::string& bytes, bool replace, bool append) {
  const std::string resolved = resolveUserPath(session, userPath);
  ojson guard = guardOverwrite(cmd, resolved, userPath, replace, append);
  if (!guard.is_null()) return guard;
  std::string payload = bytes;
  if (append && cecon::native_exists(resolved)) {
    bool ok = false;
    const std::string existing = readWholeFile(resolved, &ok);
    if (!ok) return tblError(cmd + ": cannot read " + userPath + " to append to it", "data_io");
    payload = existing;
    if (!payload.empty() && payload.back() != '\n') payload += "\n";
    payload += bytes;
  }
  ojson werr = writeTextFileAtomic(cmd, resolved, userPath, payload);
  if (!werr.is_null()) return werr;
  // The USER's spelling, never the resolved absolute path: bundle D echoed the
  // resolved path and baked a build machine's workdir into a golden.
  return displayBlock("(output written to " + userPath + ")");
}

}  // namespace


// ===========================================================================
// esttab ... using FILE   (B4-W5)
//
// The dispatcher hands this the block `runEsttab` ALREADY built, so the file
// and the screen cannot disagree about a COEFFICIENT: every b/se/t/p cell below
// is read out of that block and nothing is re-estimated here. What the writer
// owns is esttab's own PRESENTATION contract -- which statistic sits in the
// parentheses, how many digits it carries, which rows exist and in what order,
// and the legend -- and that contract is now pinned to real StataBE output:
// tests/eval/golden/aer_b4/tables/t_*.txt|tex, 16 arms captured 2026-09-09 by
// pins_oracle_t.do / _t2.do against the installed esttab 2.1.4 + estout
// 3.33. Where this file and that capture disagree, the capture is right.
//
// The defaults the capture REFUTED (all four were live in this build, all four
// are silent -- the table renders, it is just not esttab's table):
//   * the parenthesised statistic is `t`, not the standard error
//     (t_plain.txt legend: "t statistics in parentheses"), esttab
//     `if "`tfmt'"=="" local tfmt 2` -> %11.2f, so (-0.57), not (86.156);
//   * the coefficient format is `a3`, estout's ADAPTIVE significant-digit
//     format (esttab `if "`bfmt'"=="" local bfmt a3`), not %9.3f:
//     -49.51 / 1.747 / 1946.1, three different decimal counts in one column;
//   * the default footer is N alone -- no R-squared row (t_plain.txt);
//   * `_cons` is the LAST coefficient row, not the first.
// The star ladder (* 0.05 ** 0.01 *** 0.001, esttab) was already right
// in `estoutStars()`; only the legend's ORDER was reversed against the capture.
//
// SCOPE NOTE, stated because silence is what this campaign removes: the JSON
// block `runEsttab` returns to the SCREEN still carries this build's older
// defaults (se in parentheses, %9.3f, an R-squared footer row, `_cons` first).
// Correcting it changes bytes pinned by internal capture esttab.json
// and coefplot.json, which are outside this task's write manifest. The FILE
// writer is the surface the 27 captured arms measure and the surface the AER
// corpus writes; the screen block's divergence is named in the W5 report as a
// re-pin request, not fixed here behind the maintainer's back.
// ===========================================================================

// ===========================================================================
// Factor base levels (W2AK BASELEV): Stata's e(b) after `i.var` carries the
// base level as a column (`0b.clinic`, b = 0 and var 0); this engine's
// coefficient table does not (EB-BASE). estout shows it by default as a `0` /
// `(.)` row before the first non-base level (estout `baselevels`, the
// `b.` stripped by estout_omitted_and_base). The base is the level of `var`
// present in e(sample) that has no coefficient -- read here from the data and
// the hidden e(sample) column while the fit is still the live one, so the
// stored snapshot carries it even after the data move on.
// ===========================================================================
ojson factorBaseLevels(const ojson& coeftable, SessionState& session) {
  ojson out = ojson::array();
  if (!coeftable.is_array() || !session.hasData()) return out;
  std::vector<std::string> vars;
  std::map<std::string, std::vector<std::pair<long long, std::string>>> levels;
  for (const auto& r : coeftable) {
    const std::string n = r.value("name", std::string());
    long long lv = 0;
    std::string var;
    if (!parseLevelName(n, &lv, &var)) continue;
    if (levels.count(var) == 0) vars.push_back(var);
    levels[var].emplace_back(lv, n);
  }
  if (vars.empty()) return out;
  try {
    Datastore* ds = session.datastore();
    if (ds == nullptr) return out;
    const double* mask = nullptr;
    double serial = 0.0;
    const SessionState::EsampleReading er = session.esampleReading();
    if (er.kind == SessionState::EsampleReading::Kind::kColumn && ds->has(kEsampleColumn)) {
      mask = ds->col(kEsampleColumn).f64();
      serial = static_cast<double>(er.serial);
    }
    for (const auto& var : vars) {
      if (!ds->has(var) || ds->col(var).dtype == cecon::DType::Factor) continue;
      const cecon::Column& c = ds->col(var);
      const double* d = c.f64();
      const int64_t n = c.nbytes / static_cast<int64_t>(sizeof(double));
      std::set<long long> present;
      for (int64_t i = 0; i < n; ++i) {
        if (mask != nullptr && mask[i] != serial) continue;
        if (std::isnan(d[i]) || d[i] != std::floor(d[i])) continue;
        present.insert(static_cast<long long>(d[i]));
      }
      std::set<long long> coefLv;
      for (const auto& lv : levels[var]) coefLv.insert(lv.first);
      long long base = 0;
      bool found = false;
      for (long long v : present) {
        if (coefLv.count(v) == 0) {
          base = v;
          found = true;
          break;
        }
      }
      if (!found) continue;
      std::string before;
      long long bestAbove = 0;
      for (const auto& lv : levels[var]) {
        if (lv.first > base && (before.empty() || lv.first < bestAbove)) {
          before = lv.second;
          bestAbove = lv.first;
        }
      }
      ojson e;
      e["var"] = var;
      e["level"] = base;
      e["name"] = std::to_string(base) + "." + var;
      e["before"] = before;   // empty: after the last level
      e["after"] = levels[var].back().second;
      out.push_back(e);
    }
  } catch (const std::exception&) {
    return ojson::array();
  }
  return out;
}

// Portions derived from estout 3.33 (MIT).
// See THIRD_PARTY_NOTICES.
// ===========================================================================
// estout core (W2AK, 2026-10-21): ONE table writer for esttab and estout.
//
// esttab.ado is "translate the options through a per-mode table, then call
// estout" (esttab is the table, the translation), and this
// build used to carry two independent hand-built writers instead -- the old
// `writeEsttabFile` and `runEstout` -- which agreed with neither the ados nor
// each other on rules, gaps, hooks, widths or csv quoting. What follows is
// derived from estout.ado 3.33's WRITE phase (estout and the
// helpers it calls: WriteCaption, WriteBegin/WriteEnd, WriteEqrow,
// WriteStrRow, vFormat, SignificantDigits, Stars, Abbrev, InsertAtVariables,
// CompileVarl, ExpandEqVarlist, DropOrKeep, Order) driven by an
// `EoSpec` the caller fills. esttab's translation lives in `esttabSpec` below.
//
// Reading the ado is the spec: every rule here follows the behaviour read from it.
// What is NOT derived is said where it would have been (rtf/html/smcl
// writers, element-level keep()/drop(), statslayout, margin/discrete).
// ===========================================================================
namespace {

const double kEoNaN = std::numeric_limits<double>::quiet_NaN();

// Stata macro-list tokenization (`foreach x of local`, `word # of`, gettoken):
// blank-separated words; "..." and the compound `"..."' are ONE word with one
// level of quotes removed.
std::vector<std::string> eoTokens(const std::string& s) {
  std::vector<std::string> out;
  const size_t n = s.size();
  size_t i = 0;
  while (i < n) {
    while (i < n && (s[i] == ' ' || s[i] == '\t')) ++i;
    if (i >= n) break;
    if (s[i] == '`' && i + 1 < n && s[i + 1] == '"') {
      size_t j = i + 2;
      int depth = 1;
      std::string cur;
      while (j < n) {
        if (s[j] == '`' && j + 1 < n && s[j + 1] == '"') {
          ++depth;
          cur += "`\"";
          j += 2;
          continue;
        }
        if (s[j] == '"' && j + 1 < n && s[j + 1] == '\'') {
          if (--depth == 0) {
            j += 2;
            break;
          }
          cur += "\"'";
          j += 2;
          continue;
        }
        cur += s[j++];
      }
      out.push_back(cur);
      i = j;
      continue;
    }
    if (s[i] == '"') {
      const size_t j = s.find('"', i + 1);
      if (j == std::string::npos) {
        out.push_back(s.substr(i + 1));
        break;
      }
      out.push_back(s.substr(i + 1, j - i - 1));
      i = j + 1;
      continue;
    }
    size_t j = i;
    while (j < n && s[j] != ' ' && s[j] != '\t') ++j;
    out.push_back(s.substr(i, j - i));
    i = j;
  }
  return out;
}

// estout: a hook with no `"` is ONE
// line, verbatim; otherwise every word is a line.
std::vector<std::string> eoHookLines(const std::string& raw) {
  if (raw.find('"') == std::string::npos) return {raw};
  return eoTokens(raw);
}

// Split `list , subopts` at the first comma outside quotes and parentheses.
void eoSplitComma(const std::string& raw, std::string* lhs, std::string* rhs) {
  int depth = 0;
  bool inQ = false;
  for (size_t i = 0; i < raw.size(); ++i) {
    const char c = raw[i];
    if (c == '"') inQ = !inQ;
    if (inQ) continue;
    if (c == '(' || c == '[' || c == '{') ++depth;
    if (c == ')' || c == ']' || c == '}') --depth;
    if (c == ',' && depth == 0) {
      *lhs = raw.substr(0, i);
      *rhs = raw.substr(i + 1);
      return;
    }
  }
  *lhs = raw;
  rhs->clear();
}

// `name(arg) flag name2(arg2)` -> (lower(name), arg). Parentheses nest and
// quotes protect, so `prefix(\multicolumn{@span}{c}{)` keeps its braces.
std::vector<std::pair<std::string, std::optional<std::string>>> eoSubopts(const std::string& s) {
  std::vector<std::pair<std::string, std::optional<std::string>>> out;
  size_t i = 0;
  const size_t n = s.size();
  while (i < n) {
    while (i < n && (s[i] == ' ' || s[i] == '\t' || s[i] == ',')) ++i;
    if (i >= n) break;
    size_t j = i;
    while (j < n && s[j] != '(' && s[j] != ' ' && s[j] != '\t') ++j;
    std::string name = strutil::lower(s.substr(i, j - i));
    if (j < n && s[j] == '(') {
      int depth = 0;
      bool inQ = false;
      size_t k = j;
      for (; k < n; ++k) {
        const char c = s[k];
        if (c == '"') inQ = !inQ;
        if (inQ) continue;
        if (c == '(') ++depth;
        if (c == ')' && --depth == 0) break;
      }
      out.emplace_back(name, s.substr(j + 1, (k < n ? k : n) - j - 1));
      i = (k < n) ? k + 1 : n;
    } else {
      out.emplace_back(name, std::nullopt);
      i = j;
    }
  }
  return out;
}

// Stata `syntax` abbreviation: the CAPITALS of `spec` are the minimum, the
// whole word (lower-cased) the maximum.
bool eoAbbr(const std::string& given, const char* spec) {
  const std::string full = strutil::lower(spec);
  size_t minLen = 0;
  while (spec[minLen] != '\0' && std::isupper(static_cast<unsigned char>(spec[minLen]))) ++minLen;
  if (minLen == 0) minLen = full.size();
  return given.size() >= minLen && given.size() <= full.size() &&
         full.compare(0, given.size(), given) == 0;
}

std::string eoReplaceAll(std::string s, const std::string& from, const std::string& to) {
  if (from.empty()) return s;
  size_t at = 0;
  while ((at = s.find(from, at)) != std::string::npos) {
    s.replace(at, from.size(), to);
    at += to.size();
  }
  return s;
}

std::string eoTrim(const std::string& s) { return strutil::trim(s); }

// `di %-#s`, `di %#s`, `di %~#s`.
// Widths count CHARACTERS, not bytes (StataBE, scratch-r150/uni-v/p4.do).
std::string eoPadR(const std::string& s, int w) {
  const size_t n = cecon::ident::codePointCount(s);
  return (w <= 0 || static_cast<int>(n) >= w) ? s : s + std::string(w - n, ' ');
}
std::string eoPadL(const std::string& s, int w) {
  const size_t n = cecon::ident::codePointCount(s);
  return (w <= 0 || static_cast<int>(n) >= w) ? s : std::string(w - n, ' ') + s;
}
std::string eoCentre(const std::string& s, int w) {
  const size_t n = cecon::ident::codePointCount(s);
  if (w <= 0 || static_cast<int>(n) >= w) return s;
  const size_t left = (w - n) / 2;
  return std::string(left, ' ') + s + std::string(w - n - left, ' ');
}

// Stata's abbrev(s, n) for a NAME, as BUG-490 measured it (w1b_esttab_abbrev2)
// with the 32-character window estout's own Abbrev runs it on
// (estout; ek_e43 L8 `Second regressor w~l`): keep the trailing
// digit run (or the last character), as much of the head as fits, `~` between.
// Every length is in CHARACTERS (a Unicode name is cut and counted by code
// points, never mid-sequence): StataBE abbreviates by characters.
std::string eoStataAbbrev(std::string s, int w) {
  namespace id = cecon::ident;
  if (w <= 0 || static_cast<int>(id::codePointCount(s)) <= w) return s;
  s = id::truncateCodePoints(s, 32);
  if (static_cast<int>(id::codePointCount(s)) <= w) return s;
  if (w < 3) return id::truncateCodePoints(s, static_cast<size_t>(w));
  size_t k = 0;  // trailing ASCII digits: one byte, one character each
  while (k < s.size() && std::isdigit(static_cast<unsigned char>(s[s.size() - 1 - k]))) ++k;
  size_t tailBytes = k;
  if (k == 0) {  // no digit run: the last CHARACTER
    size_t i = s.size();
    do {
      --i;
    } while (i > 0 && (static_cast<unsigned char>(s[i]) & 0xC0) == 0x80);
    tailBytes = s.size() - i;
    k = 1;
  }
  if (k > static_cast<size_t>(w) - 2) {
    k = static_cast<size_t>(w) - 2;
    tailBytes = k;
  }
  return id::truncateCodePoints(s, static_cast<size_t>(w) - 1 - k) + "~" +
         s.substr(s.size() - tailBytes);
}

// estout Abbrev.
std::string eoAbbrev(int width, const std::string& value, bool abbrev) {
  if (!abbrev) return value;
  if (width > 32) return cecon::ident::truncateCodePoints(value, static_cast<size_t>(width));
  if (width > 0 && static_cast<int>(cecon::ident::codePointCount(value)) > width)
    return eoStataAbbrev(value, width);
  return value;
}

// ---- numbers ----------------------------------------------------------------
// The value states estout's matrices carry: a number, `.` (missing -- printed),
// and `.y`/`.z` (not in this model -- the element's vacant() text).
struct EoV {
  enum Kind { kVacant, kMissing, kNum, kText } kind = kVacant;
  double v = 0.0;
  std::string text;
};
EoV eoNum(double v) {
  EoV r;
  if (std::isnan(v)) {
    r.kind = EoV::kMissing;
  } else {
    r.kind = EoV::kNum;
    r.v = v;
  }
  return r;
}

// estout vFormat (without par/dmarker/msign, which the caller adds).
// Returns false when the format is not one Stata accepts (r(198)).
// estout reads every cell through a macro first (`local value = St[r,m]`,
// estout), so it formats the value's 16-digit macro text read
// back: StataBE writes estadd's 0.715/5.2 (0x1.1999999999999p-3, which
// `display %9.3f` shows as 0.137) as 0.138 (uni-oracle/et2.do; AER 007's
// Table_A9_Panel_A beta/control mean).
bool eoFormatNumber(double vIn, const std::string& fmtIn, std::string* out) {
  double v = vIn;
  if (std::isfinite(v)) {
    const std::string mt = eval::stataMacroRealText(v);
    if (mt != ".") v = std::strtod(mt.c_str(), nullptr);
  }
  std::string fmt = eoTrim(fmtIn);
  if (fmt.empty()) fmt = "%9.0g";
  if (std::isnan(v)) {
    *out = ".";
    return true;
  }
  if (fmt[0] == 'a') {
    // SignificantDigits (estout).
    int d = 3;
    if (fmt.size() > 1) {
      try {
        size_t used = 0;
        d = std::stoi(fmt.substr(1), &used);
        if (used + 1 != fmt.size()) return false;
      } catch (...) {
        return false;
      }
    }
    *out = sigDigits(v, d, /*minDec=*/1, /*comma=*/false);
    return true;
  }
  bool isInt = !fmt.empty();
  for (char c : fmt) {
    if (!std::isdigit(static_cast<unsigned char>(c))) isInt = false;
  }
  if (isInt) {
    // `fmt(#)` is %(#+9).#f (estout).
    const int dec = std::stoi(fmt);
    char buf[512];
    std::snprintf(buf, sizeof(buf), "%.*f", dec, v);
    *out = buf;
    return true;
  }
  if (fmt == "%g" || fmt == "g") fmt = "%9.0g";
  if (fmt[0] != '%') return false;
  *out = eoTrim(eval::applyStataFmt(v, fmt));
  return true;
}

// The full vFormat: number, then (when `lz` is off) the leading zero dropped,
// then par() around it.
bool eoVFormat(double v, const std::string& fmt, bool lz, const std::vector<std::string>& par,
               std::string* out) {
  std::string val;
  if (!eoFormatNumber(v, fmt, &val)) return false;
  if (!lz) {
    if (val.rfind("0.", 0) == 0 || val.find("-0.") != std::string::npos) {
      const size_t at = val.find("0.");
      if (at != std::string::npos) val.replace(at, 2, ".");
    }
  }
  if (!par.empty()) {
    val = par[0] + val + (par.size() > 1 ? par[1] : std::string());
  }
  *out = val;
  return true;
}

// ---- the cell elements ------------------------------------------------------
struct EoElem {
  std::string name;              // b se var t z p ci ci_l ci_u _star _sign _sigsign beta, or a matrix
  std::vector<std::string> fmt;  // per coefficient ROW (`word r of fmt`), last repeats
  bool star = false;
  bool hasPar = false;
  std::vector<std::string> par;  // ( ) by default, [ , ] for ci
  std::string label;
  bool labelSet = false;
  std::string vacant;
  std::string pvalue = "p";
  std::vector<std::string> pattern;
  bool abs = false;
};

struct EoCells {
  std::vector<EoElem> elems;
  // rows[i][j] = the elements `&`-joined in row i, column j; an empty vector is
  // the `.` placeholder.
  std::vector<std::vector<std::vector<int>>> rows;
  size_t ncols = 0;
  bool none = false;
};

// Suboptions of one element (estout ParseValueSubopts).
bool eoParseElemSubopts(const std::string& inner, EoElem* el, std::string* err) {
  for (const auto& [nm, arg] : eoSubopts(inner)) {
    if (eoAbbr(nm, "Star")) {
      el->star = true;
    } else if (eoAbbr(nm, "NOStar")) {
      el->star = false;
    } else if (eoAbbr(nm, "Fmt")) {
      el->fmt = eoTokens(arg.value_or(""));
    } else if (eoAbbr(nm, "Label")) {
      const std::vector<std::string> t = eoTokens(arg.value_or(""));
      el->label = arg.has_value() ? (arg->find('"') == std::string::npos ? *arg
                                                                          : (t.empty() ? "" : t[0]))
                                  : "";
      el->labelSet = true;
    } else if (nm == "par") {
      el->hasPar = true;
      if (arg.has_value()) {
        el->par = eoTokens(*arg);
      }
    } else if (nm == "nopar") {
      el->hasPar = false;
    } else if (eoAbbr(nm, "Vacant")) {
      const std::vector<std::string> t = eoTokens(arg.value_or(""));
      el->vacant = t.empty() ? std::string() : t[0];
    } else if (eoAbbr(nm, "PVALue")) {
      el->pvalue = eoTrim(arg.value_or("p"));
    } else if (eoAbbr(nm, "PATtern")) {
      el->pattern = eoTokens(arg.value_or(""));
      for (const auto& p : el->pattern) {
        if (p != "0" && p != "1") {
          *err = "\"" + arg.value_or("") + "\" invalid in " + el->name + "(... pattern())";
          return false;
        }
      }
    } else if (nm == "abs") {
      el->abs = true;
    } else if (nm == "noabs") {
      el->abs = false;
    } else if (eoAbbr(nm, "Transpose") || eoAbbr(nm, "NOTranspose")) {
      // a 1xk matrix is read as a row either way in this build
    } else if (eoAbbr(nm, "Keep") || eoAbbr(nm, "Drop") || nm == "mrow") {
      *err = "estout: cells() element suboption " + nm + "() is not implemented in this build";
      return false;
    } else {
      *err = "option " + nm + " not allowed";
      return false;
    }
  }
  if (el->hasPar && el->par.empty()) {
    if (el->name == "ci") el->par = {"[", ",", "]"};
    else if (el->name == "ci_l") el->par = {"[", ""};
    else if (el->name == "ci_u") el->par = {"", "]"};
    else el->par = {"(", ")"};
  }
  return true;
}

// One row's text (`b se`, `b & se`, `b(fmt(3)) . se`): columns separated by
// blanks, elements of one column joined by `&`.
bool eoParseCellRow(const std::string& row, EoCells* out, std::string* err) {
  std::vector<std::vector<int>> cols;
  std::vector<int> cur;
  bool joinNext = false;
  bool haveCur = false;
  size_t i = 0;
  const size_t n = row.size();
  auto flush = [&]() {
    if (haveCur) cols.push_back(cur);
    cur.clear();
    haveCur = false;
  };
  while (i < n) {
    const char c = row[i];
    if (c == ' ' || c == '\t') {
      ++i;
      continue;
    }
    if (c == '&') {
      joinNext = true;
      ++i;
      continue;
    }
    // one token: name[(...)]
    size_t j = i;
    while (j < n && row[j] != '(' && row[j] != ' ' && row[j] != '\t' && row[j] != '&') ++j;
    std::string name = row.substr(i, j - i);
    std::string inner;
    if (j < n && row[j] == '(') {
      int depth = 0;
      bool inQ = false;
      size_t k = j;
      for (; k < n; ++k) {
        if (row[k] == '"') inQ = !inQ;
        if (inQ) continue;
        if (row[k] == '(') ++depth;
        if (row[k] == ')' && --depth == 0) break;
      }
      if (k >= n) {
        *err = "unmatched parentheses in cells()";
        return false;
      }
      inner = row.substr(j + 1, k - j - 1);
      j = k + 1;
    }
    i = j;
    if (!joinNext) flush();
    joinNext = false;
    haveCur = true;
    if (name == ".") continue;  // the placeholder: an empty column
    if (name.find('[') != std::string::npos) {
      *err = "estout: cells() row selection `name[#]` is not implemented in this build";
      return false;
    }
    EoElem el;
    el.name = name;
    if (!eoParseElemSubopts(inner, &el, err)) return false;
    out->elems.push_back(el);
    cur.push_back(static_cast<int>(out->elems.size()) - 1);
  }
  flush();
  if (!cols.empty()) {
    out->ncols = std::max(out->ncols, cols.size());
    out->rows.push_back(cols);
  }
  return true;
}

// estout (the cells() parse; none / CellsCheck): an
// unquoted bound word is a ROW of one element; a quoted word (or a
// parenthesised group) is a row of side-by-side columns.
bool eoParseCells(const std::string& raw, EoCells* out, std::string* err) {
  const std::string s = eoTrim(raw);
  if (s == "none") {
    out->none = true;
    return true;
  }
  size_t i = 0;
  const size_t n = s.size();
  while (i < n) {
    while (i < n && (s[i] == ' ' || s[i] == '\t')) ++i;
    if (i >= n) break;
    if (s[i] == '"' || (s[i] == '`' && i + 1 < n && s[i + 1] == '"')) {
      const bool compound = s[i] == '`';
      size_t j = i + (compound ? 2 : 1);
      size_t end = std::string::npos;
      if (compound) {
        end = s.find("\"'", j);
      } else {
        end = s.find('"', j);
      }
      if (end == std::string::npos) end = n;
      if (!eoParseCellRow(s.substr(j, end - j), out, err)) return false;
      i = end + (compound ? 2 : 1);
      continue;
    }
    if (s[i] == '(') {
      int depth = 0;
      size_t k = i;
      for (; k < n; ++k) {
        if (s[k] == '(') ++depth;
        if (s[k] == ')' && --depth == 0) break;
      }
      if (k >= n) {
        *err = "unmatched parentheses in cells()";
        return false;
      }
      if (!eoParseCellRow(s.substr(i + 1, k - i - 1), out, err)) return false;
      i = k + 1;
      continue;
    }
    // an unquoted word bound through its parentheses
    size_t j = i;
    int depth = 0;
    bool inQ = false;
    while (j < n) {
      const char c = s[j];
      if (c == '"') inQ = !inQ;
      if (!inQ) {
        if (c == '(') ++depth;
        if (c == ')') --depth;
        if ((c == ' ' || c == '\t') && depth == 0) break;
      }
      ++j;
    }
    std::string word = s.substr(i, j - i);
    i = j;
    // `b & se` written unquoted still joins: estout's gettoken parses `&`.
    if (word == "&") {
      if (out->rows.empty()) {
        *err = "invalid cells()";
        return false;
      }
      // join the next element into the last row's last column
      while (i < n && (s[i] == ' ' || s[i] == '\t')) ++i;
      size_t k = i;
      depth = 0;
      inQ = false;
      while (k < n) {
        const char c = s[k];
        if (c == '"') inQ = !inQ;
        if (!inQ) {
          if (c == '(') ++depth;
          if (c == ')') --depth;
          if ((c == ' ' || c == '\t') && depth == 0) break;
        }
        ++k;
      }
      EoCells tmp;
      tmp.elems = out->elems;
      if (!eoParseCellRow(s.substr(i, k - i), &tmp, err)) return false;
      if (!tmp.rows.empty() && !tmp.rows.back().empty()) {
        auto& lastCol = out->rows.back().back();
        for (int idx : tmp.rows.back().front()) lastCol.push_back(idx);
        out->elems = tmp.elems;
      }
      i = k;
      continue;
    }
    if (!eoParseCellRow(word, out, err)) return false;
  }
  return true;
}

// ---- label options ----------------------------------------------------------
// estout ParseLabelsSubopts.
struct EoLabelOpt {
  bool given = false;
  bool none = false;
  std::vector<std::string> list;
  bool span = false;
  bool replace = false;
  bool numbers = false;
  bool titles = false, notitles = false;
  bool depvars = false, nodepvars = false;
  bool merge = false;
  int first = -1, last = -1;  // -1 unset; 0 no...; 1 given
  std::string prefix, suffix, erepeat, lhs;
  std::vector<std::string> begin, end, blist, elist;
  std::vector<std::string> pattern;
};

std::string eoStringArg(const std::optional<std::string>& a) {
  // `Prefix(string)`: syntax strips ONE pair of outer quotes.
  if (!a.has_value()) return std::string();
  const std::string t = eoTrim(*a);
  if (t.size() >= 2 && t.front() == '"' && t.back() == '"') return t.substr(1, t.size() - 2);
  if (t.size() >= 4 && t.rfind("`\"", 0) == 0 && t.compare(t.size() - 2, 2, "\"'") == 0) {
    return t.substr(2, t.size() - 4);
  }
  return *a;
}

bool eoParseLabelOpt(const std::string& raw, EoLabelOpt* o, std::string* err) {
  o->given = true;
  if (eoTrim(raw) == "none") {
    o->none = true;
    return true;
  }
  std::string lhs, rhs;
  eoSplitComma(raw, &lhs, &rhs);
  o->list = eoTokens(lhs);
  auto lines = [](const std::optional<std::string>& a) {
    const std::string v = a.value_or("");
    if (v.find('"') == std::string::npos) return std::vector<std::string>{v};
    return eoTokens(v);
  };
  for (const auto& [nm, arg] : eoSubopts(rhs)) {
    if (eoAbbr(nm, "NUMbers")) o->numbers = true;
    else if (eoAbbr(nm, "NONUMbers")) o->numbers = false;
    else if (eoAbbr(nm, "TItles")) o->titles = true;
    else if (eoAbbr(nm, "NOTItles")) o->notitles = true;
    else if (eoAbbr(nm, "DEPvars")) o->depvars = true;
    else if (eoAbbr(nm, "NODEPvars")) o->nodepvars = true;
    else if (nm == "none") o->none = true;
    else if (nm == "nonone") o->none = false;
    else if (nm == "span") o->span = true;
    else if (nm == "nospan") o->span = false;
    else if (eoAbbr(nm, "Prefix")) o->prefix = eoStringArg(arg);
    else if (eoAbbr(nm, "Suffix")) o->suffix = eoStringArg(arg);
    else if (eoAbbr(nm, "Begin")) o->begin = lines(arg);
    else if (eoAbbr(nm, "End")) o->end = lines(arg);
    else if (eoAbbr(nm, "Replace")) o->replace = true;
    else if (eoAbbr(nm, "NOReplace")) o->replace = false;
    else if (eoAbbr(nm, "BList")) o->blist = eoTokens(arg.value_or(""));
    else if (eoAbbr(nm, "EList")) o->elist = eoTokens(arg.value_or(""));
    else if (eoAbbr(nm, "ERepeat")) o->erepeat = eoStringArg(arg);
    else if (eoAbbr(nm, "First")) o->first = 1;
    else if (eoAbbr(nm, "NOFirst")) o->first = 0;
    else if (eoAbbr(nm, "Last")) o->last = 1;
    else if (eoAbbr(nm, "NOLast")) o->last = 0;
    else if (nm == "lhs") o->lhs = eoStringArg(arg);
    else if (eoAbbr(nm, "PATtern")) o->pattern = eoTokens(arg.value_or(""));
    else if (eoAbbr(nm, "Merge")) o->merge = true;
    else if (eoAbbr(nm, "NOMerge")) o->merge = false;
    else {
      *err = "option " + nm + " not allowed";
      return false;
    }
  }
  // estout: merge implies the `:` suffix unless one is given.
  if (o->merge && o->suffix.empty()) o->suffix = ":";
  return true;
}

// ---- coefficients and models --------------------------------------------------
struct EoCoefVals {
  double b = kEoNaN, se = kEoNaN, t = kEoNaN, p = kEoNaN, cil = kEoNaN, ciu = kEoNaN;
  bool hasCi = false;
};

using EoKey = std::pair<std::string, std::string>;  // (equation, coefficient); "__" = none

struct EoModel {
  std::string name;              // as listed (a store name, or ".")
  const Model* model = nullptr;  // statistics; null in matrix mode
  std::string depname;           // first word of e(depvar)
  std::string title;             // e(_estimates_title)
  std::vector<EoKey> coefRows;   // e(b) order, `_cons` last within each equation
  std::map<EoKey, EoCoefVals> vals;
  // stat -> its rows (in the matrix's column order) and values
  std::map<std::string, std::vector<EoKey>> extraOrder;
  std::map<std::string, std::map<EoKey, double>> extra;
  std::map<std::string, double> beta;  // coefficient -> standardized beta (esttab, beta)
};

bool eoIsCoefStat(const std::string& s) {
  static const std::set<std::string> k = {"b",  "se",   "var",   "t",     "z",     "p",
                                          "ci", "ci_l", "ci_u",  "_star", "_sign", "_sigsign"};
  return k.count(s) != 0;
}

EoV eoElemValue(const EoModel& m, const EoKey& key, const std::string& stat) {
  if (eoIsCoefStat(stat)) {
    auto it = m.vals.find(key);
    if (it == m.vals.end()) return EoV{};
    const EoCoefVals& c = it->second;
    if (stat == "b" || stat == "_star" || stat == "_sign" || stat == "_sigsign") return eoNum(c.b);
    if (stat == "se") return eoNum(c.se);
    if (stat == "var") return eoNum(std::isnan(c.se) ? kEoNaN : c.se * c.se);
    if (stat == "t" || stat == "z") return eoNum(c.t);
    if (stat == "p") return eoNum(c.p);
    if (stat == "ci_l") return eoNum(c.cil);
    if (stat == "ci_u") return eoNum(c.ciu);
    return EoV{};
  }
  if (stat == "beta") {
    auto it = m.beta.find(key.second);
    if (it == m.beta.end()) {
      if (m.vals.count(key) != 0 && key.second == "_cons") return EoV{};
      return EoV{};
    }
    return eoNum(it->second);
  }
  auto ex = m.extra.find(stat);
  if (ex == m.extra.end()) return EoV{};
  auto it = ex->second.find(key);
  if (it == ex->second.end()) it = ex->second.find(EoKey{"__", key.second});
  if (it == ex->second.end()) return EoV{};
  return eoNum(it->second);
}

// One footer statistic (estout GetStats): e() scalars, then estadd scalars,
// then estadd LOCALS as text (`return(m#_stat)`, estout).
EoV eoStatValue(const EoModel& em, const std::string& key) {
  if (em.model == nullptr) return EoV{};
  const Model& m = *em.model;
  std::vector<std::string> tryKeys{key};
  if (key == "ar2") tryKeys.push_back("r2_a");
  if (key == "r2_a") tryKeys.push_back("ar2");
  if (key == "pr2" || key == "r2_p" || key == "r2_pseudo") {
    for (const char* alt : {"r2_p", "r2_pseudo"}) {
      if (std::find(tryKeys.begin(), tryKeys.end(), alt) == tryKeys.end()) tryKeys.push_back(alt);
    }
  }
  for (const auto& k : tryKeys) {
    if (m.escalars.contains(k) && m.escalars[k].is_number()) return eoNum(m.escalars[k].get<double>());
  }
  if (m.added.contains("scalars") && m.added["scalars"].contains(key)) {
    const ojson& v = m.added["scalars"][key];
    if (v.is_number()) return eoNum(v.get<double>());
    if (v.is_null()) return eoNum(kEoNaN);
  }
  if (m.added.contains("locals") && m.added["locals"].contains(key)) {
    const ojson& v = m.added["locals"][key];
    EoV r;
    r.kind = EoV::kText;
    r.text = v.is_string() ? v.get<std::string>() : std::string();
    if (r.text.empty()) return EoV{};
    return r;
  }
  return EoV{};
}

// ---- the spec -----------------------------------------------------------------
struct EoStatRow {
  std::string key;
  std::string fmt;
  bool star = false;
};

struct EoIndicate {
  std::string name;
  std::string vars;
};

struct EoSpec {
  std::vector<EoModel> models;
  bool matrixMode = false;
  EoCells cells;
  // coefficient selection
  std::optional<std::string> keep, drop, order;
  bool dropRelax = false;
  std::vector<EoIndicate> indicate;
  std::string indicateYes = "Yes", indicateNo = "No";
  std::vector<std::pair<std::string, std::string>> refcat;  // coef -> label
  bool refcatBelow = false;
  std::string refcatLabel = "ref.";
  bool omitted = true, baselevels = true;
  bool unstack = false;
  bool equationsGiven = false;
  // head
  EoLabelOpt mgroups, mlabels, eqlabels, collabels, varlabels, statslabels;
  bool numbers = false;
  std::string numL = "(", numR = ")";
  bool label = false;
  // labcol2
  bool labcol2 = false;
  std::vector<std::string> labcol2Vals, labcol2Title;
  int labcol2Width = -1;
  // stats
  std::vector<EoStatRow> stats;
  // widths and frame
  std::vector<int> modelwidth{0};
  int varwidth = 0;
  bool abbrev = false;
  bool wrap = false;
  std::string begin, delimiter = "\t", end, lastend;
  bool lastendSet = false;
  std::string incelldelimiter = " ";
  std::vector<std::string> prehead, posthead, prefoot, postfoot;
  std::string title, note;
  std::vector<std::pair<std::string, std::string>> starlevels;  // symbol, level (verbatim)
  std::string starlevelsLabel = " p<", starlevelsDelim = ", ";
  bool legend = false;
  std::vector<std::pair<std::string, std::string>> substitute;
  std::string interaction = " # ";
  bool lz = true;
  std::string hlinechar = "-";
  std::vector<int> extracols;
  std::vector<std::pair<std::string, std::string>> rename;  // [eq:]from -> to
  int level = 95;
  SessionState* session = nullptr;
};

struct EoError {
  std::string msg;
  int rc = 198;
};

bool eoMatrixModel(SessionState& session, const std::string& spec, EoSpec* sp, std::string* err);

// estout Stars.
std::string eoStars(const std::vector<std::pair<std::string, std::string>>& lv, const EoV& p) {
  if (p.kind != EoV::kNum || p.v < 0.0 || p.v > 1.0) return std::string();
  std::string out;
  for (const auto& [sym, lvl] : lv) {
    double th = 0.0;
    try {
      th = std::stod(lvl);
    } catch (...) {
      continue;
    }
    if (th <= p.v) break;
    out = sym;
  }
  return out;
}

// estout CheckStarvals (the legend text).
std::string eoStarLegend(const EoSpec& sp) {
  std::string leg;
  for (const auto& [sym, lvl] : sp.starlevels) {
    if (!leg.empty()) leg += sp.starlevelsDelim;
    std::string lab = sp.starlevelsLabel;
    if (lab.find('@') != std::string::npos) lab = eoReplaceAll(lab, "@", lvl);
    else lab += lvl;
    leg += sym + lab;
  }
  return leg;
}

// ---- CompileVarl (estout): a coefficient's label under `label` -------
std::string eoVarLabel(SessionState& session, const std::string& var, bool* exists) {
  *exists = false;
  auto vm = session.varMeta.find(var);
  if (vm != session.varMeta.end()) {
    *exists = true;
    if (vm->second.label.has_value()) return *vm->second.label;
  }
  const std::string l = session.eval.varLabels.get(var);
  if (!l.empty()) {
    *exists = true;
    return l;
  }
  if (!*exists && session.hasData()) {
    try {
      Datastore* ds = session.datastore();
      *exists = ds != nullptr && ds->has(var);
    } catch (const std::exception&) {
      *exists = false;
    }
  }
  return std::string();
}

std::string eoCompileVarl(SessionState& session, const std::string& vnameIn,
                          const std::string& interaction) {
  // Stata names an interaction cell with its factor atoms first, then its
  // continuous ones, each in the order written (StataBE uni-oracle/io.do:
  // c.x##i.g -> 1.g#c.x, c.x#i.a#i.b -> 1.a#1.b#c.x, c.z#c.x#i.g ->
  // 1.g#c.z#c.x); the engine keeps the written order (`age_BL#1.treatment`),
  // so 007's joint-test row read `Age x Treatment=1` for Stata's
  // `Treatment=1 x Age`.
  std::string vname = vnameIn;
  if (vname.find('#') != std::string::npos) {
    std::vector<std::string> fac, con;
    size_t a = 0;
    while (a <= vname.size()) {
      size_t h = vname.find('#', a);
      if (h == std::string::npos) h = vname.size();
      const std::string atom = vname.substr(a, h - a);
      (!atom.empty() && std::isdigit(static_cast<unsigned char>(atom[0])) ? fac : con).push_back(atom);
      a = h + 1;
    }
    vname.clear();
    for (const auto* part : {&fac, &con})
      for (const auto& atom : *part) vname += (vname.empty() ? "" : "#") + atom;
  }
  std::string out;
  size_t i = 0;
  while (i <= vname.size()) {
    const size_t hash = vname.find('#', i);
    const std::string vi = vname.substr(i, hash == std::string::npos ? std::string::npos : hash - i);
    std::string xlabi;
    if (!vi.empty()) {
      const size_t dot = vi.find('.');
      if (dot == std::string::npos) {
        bool ex = false;
        xlabi = eoVarLabel(session, vi, &ex);
      } else {
        const std::string li0 = vi.substr(0, dot);
        const std::string vii = vi.substr(dot + 1);
        bool ex = false;
        const std::string vl = eoVarLabel(session, vii, &ex);
        if (ex) {
          bool isNum = !li0.empty();
          for (char c : li0) {
            if (!std::isdigit(static_cast<unsigned char>(c)) && c != '-') isNum = false;
          }
          if (!isNum) {
            xlabi = vl.empty() ? vii : vl;
            std::string li = li0;
            if (!li.empty() && li[0] == 'c') li = li.substr(1);
            if (!li.empty()) xlabi = li + "." + xlabi;
          } else {
            auto vlab = session.valueLabels.find(vii);
            int code = 0;
            try {
              code = std::stoi(li0);
            } catch (...) {
            }
            if (vlab != session.valueLabels.end()) {
              auto c = vlab->second.find(code);
              // `: label lbl #` answers the number itself for an unlabelled code
              xlabi = (c != vlab->second.end()) ? c->second : li0;
            } else {
              xlabi = (vl.empty() ? vii : vl) + "=" + li0;
            }
          }
        }
      }
    }
    if (xlabi.empty()) xlabi = vi;
    out += xlabi;
    if (hash == std::string::npos) break;
    out += interaction;
    i = hash + 1;
  }
  return out;
}

// ---- coefficient selection: ExpandEqVarlist / DropOrKeep / Order ---------------
// A spec item: (eq, name) where eq "" = any equation and name "" = the whole
// equation (`eq:`).
using EoSpecItem = std::pair<std::string, std::string>;

bool eoExpandEqVarlist(const std::string& list, const std::vector<EoKey>& rows, bool append,
                       std::vector<EoSpecItem>* out, std::vector<EoKey>* appendRows,
                       EoError* err) {
  std::vector<std::string> ueqs, ucoefs;
  for (const auto& r : rows) {
    if (std::find(ueqs.begin(), ueqs.end(), r.first) == ueqs.end()) ueqs.push_back(r.first);
    if (std::find(ucoefs.begin(), ucoefs.end(), r.second) == ucoefs.end()) ucoefs.push_back(r.second);
  }
  if (rows.empty()) ueqs.push_back("__");
  std::string body = list;
  bool relax = false;
  {
    std::string l, r;
    eoSplitComma(list, &l, &r);
    if (!eoTrim(r).empty()) {
      for (const auto& [nm, arg] : eoSubopts(r)) {
        if (eoAbbr(nm, "Relax")) relax = true;
      }
      body = l;
    }
  }
  for (const std::string& eqx : eoTokens(body)) {
    std::string eq, x;
    const size_t colon = eqx.find(':');
    bool eqGiven = false;
    std::string eq0;
    if (colon == std::string::npos) {
      x = eqx;
    } else if (colon == 0) {
      x = eqx.substr(1);
    } else {
      eq = eqx.substr(0, colon);
      eq0 = eq;
      if (eq == "_") eq = "__";
      x = eqx.substr(colon + 1);
      eqGiven = true;
    }
    std::vector<std::string> eqmatch;
    if (!eqGiven || std::find(ueqs.begin(), ueqs.end(), eq) != ueqs.end()) {
      if (eqGiven) eqmatch.push_back(eq);
    } else {
      for (const auto& e : ueqs) {
        if (strutil::globMatch(eq, e)) eqmatch.push_back(e);
      }
      if (eqmatch.empty() && !relax && !(append && !x.empty())) {
        err->msg = "equation " + eq0 + " not found";
        err->rc = 111;
        return false;
      }
    }
    if (x.empty()) {
      for (const auto& e : eqmatch) out->emplace_back(e, "");
      continue;
    }
    if (!eqGiven) {
      if (std::find(ucoefs.begin(), ucoefs.end(), x) != ucoefs.end()) {
        out->emplace_back("", x);
        continue;
      }
      bool any = false;
      for (const auto& c : ucoefs) {
        if (strutil::globMatch(x, c)) {
          out->emplace_back("", c);
          any = true;
        }
      }
      if (!any && x.find_first_of("*?") == std::string::npos) {
        // The same interaction in another atom order / `c.` spelling.
        const std::string kx = cecon::postest::interaction_key(x);
        for (const auto& c : ucoefs) {
          if (!kx.empty() && kx == cecon::postest::interaction_key(c)) {
            out->emplace_back("", c);
            any = true;
            break;
          }
        }
      }
      if (!any) {
        if (append) {
          if (appendRows != nullptr) appendRows->emplace_back("__", x);
          out->emplace_back("", x);
        } else if (!relax) {
          err->msg = "coefficient " + x + " not found";
          err->rc = 111;
          return false;
        }
      }
      continue;
    }
    bool any = false;
    for (const auto& r : rows) {
      if (std::find(eqmatch.begin(), eqmatch.end(), r.first) == eqmatch.end()) continue;
      if (strutil::globMatch(x, r.second)) {
        out->emplace_back(r.first, r.second);
        any = true;
      }
    }
    if (!any) {
      if (append) {
        if (appendRows != nullptr) appendRows->emplace_back(eq, x);
        out->emplace_back(eq, x);
      } else if (!relax) {
        err->msg = "coefficient " + eq0 + ":" + x + " not found";
        err->rc = 111;
        return false;
      }
    }
  }
  return true;
}

bool eoSpecHits(const std::vector<EoSpecItem>& spec, const EoKey& row) {
  for (const auto& [eq, name] : spec) {
    if (name.empty()) {
      if (eq == row.first) return true;
      continue;
    }
    if (!eq.empty() && eq != row.first) continue;
    if (name == row.second) return true;
  }
  return false;
}

void eoDropOrKeep(bool keep, const std::vector<EoSpecItem>& spec, std::vector<EoKey>* rows) {
  std::vector<EoKey> res;
  for (const auto& r : *rows) {
    if (eoSpecHits(spec, r) == keep) res.push_back(r);
  }
  *rows = res;
}

// estout Order.
void eoOrder(const std::string& specList, const std::vector<EoSpecItem>& expanded,
             std::vector<EoKey>* rows) {
  (void)specList;
  std::vector<std::string> eqlist;
  for (const auto& r : *rows) {
    if (std::find(eqlist.begin(), eqlist.end(), r.first) == eqlist.end()) eqlist.push_back(r.first);
  }
  std::vector<EoKey> bt = *rows, res;
  auto takeAll = [&](const EoSpecItem& sp) {
    for (;;) {
      auto it = std::find_if(bt.begin(), bt.end(), [&](const EoKey& r) {
        if (sp.second.empty()) return r.first == sp.first;
        return r.first == sp.first && r.second == sp.second;
      });
      if (it == bt.end()) return;
      res.push_back(*it);
      bt.erase(it);
    }
  };
  size_t i = 0;
  while (i < expanded.size()) {
    if (expanded[i].first.empty()) {
      // a run of bare names: per equation, the names in order, then the rest of
      // that equation (estout)
      std::vector<std::string> vars;
      while (i < expanded.size() && expanded[i].first.empty()) vars.push_back(expanded[i++].second);
      for (const auto& eq : eqlist) {
        for (const auto& v : vars) takeAll({eq, v});
        takeAll({eq, ""});
      }
    } else {
      takeAll(expanded[i]);
      ++i;
    }
  }
  for (const auto& r : bt) res.push_back(r);
  *rows = res;
}

// mat_capp ..., cons (estout): rows of `src` not yet in `dst` join at
// the end of their equation -- before that equation's `_cons` -- and a new
// equation joins at the end.
void eoCapp(std::vector<EoKey>* dst, const std::vector<EoKey>& src) {
  for (const auto& k : src) {
    if (std::find(dst->begin(), dst->end(), k) != dst->end()) continue;
    // the end of k's equation block
    size_t last = dst->size();
    bool seen = false;
    for (size_t i = 0; i < dst->size(); ++i) {
      if ((*dst)[i].first == k.first) {
        seen = true;
        last = i + 1;
      }
    }
    if (!seen) {
      dst->push_back(k);
      continue;
    }
    size_t at = last;
    if (k.second != "_cons" && at > 0 && (*dst)[at - 1].second == "_cons" &&
        (*dst)[at - 1].first == k.first) {
      --at;
    }
    dst->insert(dst->begin() + static_cast<long>(at), k);
  }
}

// Stata's `piece(n, w, s), nobreak`: the n-th word-wrapped chunk of s.
std::string eoPiece(int n, int width, const std::string& s) {
  const std::vector<std::string> words = splitWs(s);
  std::vector<std::string> pieces;
  std::string cur;
  for (const auto& wd : words) {
    if (cur.empty()) {
      cur = wd;
    } else if (static_cast<int>(cecon::ident::codePointCount(cur) + 1 +
                                cecon::ident::codePointCount(wd)) <= width) {  // characters
      cur += " " + wd;
    } else {
      pieces.push_back(cur);
      cur = wd;
    }
  }
  if (!cur.empty()) pieces.push_back(cur);
  return (n >= 1 && static_cast<size_t>(n) <= pieces.size()) ? pieces[n - 1] : std::string();
}

// The whole write phase. `out` receives the file text (every line ends `\n`).
bool eoRender(const EoSpec& sp, std::string* out, EoError* err, int* ccolsOut = nullptr) {
  SessionState& session = *sp.session;
  std::vector<EoModel> models = sp.models;  // TableIsAMess may rename equations
  const size_t M = models.size();
  // rename() (estout RenameCoefs), per model before the union: every
  // row named `from` (in `eq` when it is given) is called `to`.
  if (!sp.rename.empty()) {
    for (auto& em : models) {
      auto ren = [&](const EoKey& k) {
        EoKey out = k;
        for (const auto& [from, to] : sp.rename) {
          std::string equ, x = from;
          const size_t colon = from.find(':');
          if (colon != std::string::npos) {
            equ = from.substr(0, colon);
            if (equ == "_") equ = "__";
            x = from.substr(colon + 1);
          }
          if (!equ.empty() && equ != k.first) continue;
          if (out.second == x) out.second = to;
        }
        return out;
      };
      for (auto& k : em.coefRows) k = ren(k);
      std::map<EoKey, EoCoefVals> nv;
      for (const auto& [k, v] : em.vals) nv[ren(k)] = v;
      em.vals = nv;
      for (auto& [st, ord] : em.extraOrder) {
        for (auto& k : ord) k = ren(k);
      }
      for (auto& [st, mp] : em.extra) {
        std::map<EoKey, double> nm;
        for (const auto& [k, v] : mp) nm[ren(k)] = v;
        mp = nm;
      }
      std::map<std::string, double> nb;
      for (const auto& [k, v] : em.beta) nb[ren(EoKey{"__", k}).second] = v;
      em.beta = nb;
    }
  }
  const EoCells& cells = sp.cells;

  // ---- which statistics name rows (GetCoefs) ----------------------------------
  bool anyCoefStat = false;
  std::vector<std::string> matStats;
  for (const auto& el : cells.elems) {
    if (eoIsCoefStat(el.name) || el.name == "beta") {
      anyCoefStat = true;
    } else if (std::find(matStats.begin(), matStats.end(), el.name) == matStats.end()) {
      matStats.push_back(el.name);
    }
  }
  bool tablehasstars = false;
  for (const auto& el : cells.elems) {
    if (el.star || el.name == "_star" || el.name == "_sigsign") tablehasstars = true;
  }
  // r(ccols): the coefficient statistics a model carries (estout plus
  // the p column stars add).
  int ccols = 0;
  {
    std::set<std::string> vals;
    for (const auto& el : cells.elems) vals.insert(el.name);
    if (tablehasstars) {
      for (const auto& el : cells.elems) {
        if (el.star) vals.insert(el.pvalue);
      }
    }
    ccols = static_cast<int>(vals.size());
  }
  auto rowsOf = [&](const EoModel& m) {
    std::vector<EoKey> rows;
    if (anyCoefStat) rows = m.coefRows;
    bool modelHasNoEq = rows.empty();
    for (const auto& k : rows) {
      if (k.first == "__") modelHasNoEq = true;
    }
    for (const auto& st : matStats) {
      auto it = m.extraOrder.find(st);
      if (it == m.extraOrder.end()) continue;
      bool allNoEq = true;
      for (const auto& k : it->second) {
        if (k.first != "__") allNoEq = false;
      }
      // SEQMerge (estout): an equation-less matrix fills by name into
      // a model that has equations rather than adding rows.
      if (allNoEq && !modelHasNoEq) continue;
      eoCapp(&rows, it->second);
    }
    return rows;
  };
  std::vector<std::vector<EoKey>> mrows(M);
  std::vector<EoKey> B;
  auto buildUnion = [&]() {
    B.clear();
    for (size_t m = 0; m < M; ++m) {
      mrows[m] = rowsOf(models[m]);
      eoCapp(&B, mrows[m]);
    }
  };
  buildUnion();
  bool hasB = !B.empty();

  // ---- TableIsAMess (estout) ----------------------------------
  if (!sp.equationsGiven && !sp.unstack && hasB) {
    std::vector<std::string> ueq;
    for (const auto& k : B) {
      if (std::find(ueq.begin(), ueq.end(), k.first) == ueq.end()) ueq.push_back(k.first);
    }
    if (ueq.size() > 1) {
      bool mess = false;
      for (size_t m = 1; m < M && !mess; ++m) {
        bool has = false;
        for (const auto& k : mrows[m]) {
          if (k.first == ueq[0]) has = true;
        }
        if (!has) mess = true;
      }
      if (mess) {
        for (auto& em : models) {
          const std::vector<EoKey> r0 = rowsOf(em);
          if (r0.empty()) continue;
          const std::string first = r0.front().first;
          auto ren = [&](const EoKey& k) { return k.first == first ? EoKey{"main", k.second} : k; };
          for (auto& k : em.coefRows) k = ren(k);
          std::map<EoKey, EoCoefVals> nv;
          for (const auto& [k, v] : em.vals) nv[ren(k)] = v;
          em.vals = nv;
          for (auto& [st, ord] : em.extraOrder) {
            for (auto& k : ord) k = ren(k);
          }
          for (auto& [st, mp] : em.extra) {
            std::map<EoKey, double> nm;
            for (const auto& [k, v] : mp) nm[ren(k)] = v;
            mp = nm;
          }
        }
        buildUnion();
      }
    }
  }

  // ---- order / indicate / keep / drop (estout) -------------------------
  std::vector<std::pair<std::string, std::vector<std::string>>> indicateLbls;  // name, per col
  if (sp.order.has_value() && !B.empty()) {
    std::vector<EoSpecItem> ex;
    std::vector<EoKey> app;
    if (!eoExpandEqVarlist(*sp.order, B, /*append=*/true, &ex, &app, err)) return false;
    for (const auto& a : app) {
      if (std::find(B.begin(), B.end(), a) == B.end()) B.push_back(a);
    }
    eoOrder(*sp.order, ex, &B);
  }
  for (const auto& ind : sp.indicate) {
    std::vector<EoSpecItem> ex;
    if (!B.empty()) {
      if (!eoExpandEqVarlist(ind.vars, B, false, &ex, nullptr, err)) return false;
    }
    std::vector<std::string> lbls;
    for (size_t m = 0; m < M; ++m) {
      bool yes = false;
      for (const auto& k : mrows[m]) {
        if (eoSpecHits(ex, k) && std::find(B.begin(), B.end(), k) != B.end()) yes = true;
      }
      lbls.push_back(yes ? sp.indicateYes : sp.indicateNo);
    }
    std::string name = ind.name;
    if (name.empty()) {
      const std::vector<std::string> t = eoTokens(ind.vars);
      name = t.empty() ? std::string() : t[0];
    }
    indicateLbls.emplace_back(name, lbls);
    eoDropOrKeep(false, ex, &B);
  }
  if (sp.keep.has_value() && !B.empty()) {
    std::vector<EoSpecItem> ex;
    if (!eoExpandEqVarlist(*sp.keep, B, false, &ex, nullptr, err)) return false;
    eoDropOrKeep(true, ex, &B);
  }
  if (sp.drop.has_value() && !B.empty()) {
    std::string dl = *sp.drop;
    if (sp.dropRelax) dl += ", relax";
    std::vector<EoSpecItem> ex;
    if (!eoExpandEqVarlist(dl, B, false, &ex, nullptr, err)) return false;
    eoDropOrKeep(false, ex, &B);
  }
  hasB = !B.empty();
  if (ccolsOut != nullptr) *ccolsOut = hasB ? ccols : -1;

  // ---- names and equations of the final set ------------------------------------------
  size_t R = B.size();
  std::vector<std::string> eqlist;
  for (const auto& k : B) eqlist.push_back(k.first);
  std::vector<std::string> eqs;
  std::vector<int> eqsdims;
  auto uniqEqs = [&]() {
    eqs.clear();
    eqsdims.clear();
    std::string last;
    bool any = false;
    for (const auto& e : eqlist) {
      if (!any || e != last) {
        eqs.push_back(e);
        eqsdims.push_back(0);
      }
      ++eqsdims.back();
      last = e;
      any = true;
    }
    if (eqs.empty()) {
      eqs.push_back("__");
      eqsdims.push_back(0);
    }
  };
  uniqEqs();
  std::vector<std::string> varlist;  // unstack: the coefficient names
  std::vector<std::string> eqswide{"__"};
  if (sp.unstack && R > 0) {
    // RerrangeEqs: tie each equation's rows together.
    std::vector<std::string> ue;
    for (const auto& e : eqlist) {
      if (std::find(ue.begin(), ue.end(), e) == ue.end()) ue.push_back(e);
    }
    std::vector<EoKey> nb;
    for (const auto& e : ue) {
      for (const auto& k : B) {
        if (k.first == e) nb.push_back(k);
      }
    }
    B = nb;
    eqlist.clear();
    for (const auto& k : B) eqlist.push_back(k.first);
    uniqEqs();
    for (const auto& k : B) {
      if (std::find(varlist.begin(), varlist.end(), k.second) == varlist.end()) varlist.push_back(k.second);
    }
    // order() names first, `_cons` last unless order() names it (estout)
    std::vector<std::string> ordNames;
    if (sp.order.has_value()) {
      for (const auto& t : eoTokens(*sp.order)) {
        const size_t c = t.find(':');
        ordNames.push_back(c == std::string::npos ? t : t.substr(c + 1));
      }
    }
    std::vector<std::string> nv;
    for (const auto& o : ordNames) {
      if (std::find(varlist.begin(), varlist.end(), o) != varlist.end() &&
          std::find(nv.begin(), nv.end(), o) == nv.end()) {
        nv.push_back(o);
      }
    }
    for (const auto& v : varlist) {
      if (std::find(nv.begin(), nv.end(), v) == nv.end()) nv.push_back(v);
    }
    if (std::find(ordNames.begin(), ordNames.end(), "_cons") == ordNames.end()) {
      auto it = std::find(nv.begin(), nv.end(), "_cons");
      if (it != nv.end()) {
        nv.erase(it);
        nv.push_back("_cons");
      }
    }
    varlist = nv;
    R = varlist.size();
    eqswide = eqs;
  }
  const size_t numeqs = eqs.size();
  auto rowExists = [&](const EoKey& k) { return std::find(B.begin(), B.end(), k) != B.end(); };

  // ---- model labels (estout) -----------------------------------------------
  std::vector<std::string> mlabels = sp.mlabels.list;
  {
    const bool titles = sp.mlabels.titles || (!sp.mlabels.notitles && sp.label);
    for (size_t i = mlabels.size(); i < M; ++i) {
      std::string lab;
      if (sp.mlabels.depvars) {
        const std::string var = models[i].depname;
        if (sp.label) {
          const size_t dot = var.find('.');
          const std::string tail = dot == std::string::npos ? var : var.substr(dot + 1);
          const std::string head = dot == std::string::npos ? std::string() : var.substr(0, dot + 1);
          bool ex = false;
          std::string l = eoVarLabel(session, tail, &ex);
          if (l.empty()) l = tail;
          lab = head + l;
        } else {
          lab = var;
        }
      } else if (titles) {
        lab = models[i].title;
        if (lab.empty()) lab = models[i].name;
      } else {
        lab = models[i].name;
      }
      mlabels.push_back(lab);
    }
    if (sp.mlabels.numbers) {
      for (size_t i = 0; i < M; ++i) {
        const std::string num = "(" + std::to_string(i + 1) + ")";
        mlabels[i] = mlabels[i].empty() ? num : num + " " + mlabels[i];
      }
    }
  }

  // ---- equation labels (estout) ----------------------------------------------
  std::vector<std::string> eqlabels = sp.eqlabels.list;
  for (size_t i = eqlabels.size(); i < numeqs; ++i) {
    const std::string& eq = eqs[i];
    std::string value;
    if (sp.label) {
      bool ex = false;
      value = eoVarLabel(session, eq, &ex);
    }
    if (value.empty()) value = (eq == "__") ? "_" : eq;
    eqlabels.push_back(value);
  }
  // EqReplaceCons: under eqlabels(none) a one-row equation's `_cons` takes the
  // equation's label.
  std::vector<std::string> varlabelPairs = sp.varlabels.list;
  if (sp.eqlabels.none && numeqs > 1 && !sp.unstack && !(sp.eqlabels.given && sp.eqlabels.list.size() == 1 && sp.eqlabels.list[0].empty())) {
    std::vector<std::string> vlabv;
    for (size_t i = 0; i < varlabelPairs.size(); i += 2) vlabv.push_back(varlabelPairs[i]);
    std::map<std::string, int> cnt;
    for (const auto& e : eqlist) ++cnt[e];
    std::vector<std::string> add;
    std::string last;
    size_t ei = 0;
    std::string eqlab;
    for (size_t i = 0; i < eqlist.size(); ++i) {
      const std::string& eq = eqlist[i];
      if (i == 0 || eq != last) {
        eqlab = ei < eqlabels.size() ? eqlabels[ei] : std::string();
        ++ei;
      }
      last = eq;
      if (cnt[eq] > 1 || eq == "__") continue;
      if (B[i].second == "_cons" &&
          std::find(vlabv.begin(), vlabv.end(), eq + ":_cons") == vlabv.end()) {
        add.push_back(eq + ":_cons");
        add.push_back(eqlab);
      }
    }
    add.insert(add.end(), varlabelPairs.begin(), varlabelPairs.end());
    varlabelPairs = add;
  }

  // ---- column labels (estout) ----------------------------------------------------
  std::vector<std::string> collabels;
  bool collabelsOn = false;
  if (sp.collabels.given && !sp.collabels.none && !sp.collabels.list.empty()) {
    collabels = sp.collabels.list;
    collabelsOn = true;
  } else if (!sp.collabels.none) {
    for (size_t j = 0; j < cells.ncols; ++j) {
      std::string temp;
      for (const auto& row : cells.rows) {
        if (j >= row.size()) continue;
        for (int idx : row[j]) {
          if (!temp.empty()) temp += "/";
          const EoElem& el = cells.elems[static_cast<size_t>(idx)];
          std::string lab = el.labelSet ? el.label : el.name;
          if (!el.labelSet) {
            if (el.name == "ci") lab = "ci" + std::to_string(sp.level);
            if (el.name == "ci_l") lab = "min" + std::to_string(sp.level);
            if (el.name == "ci_u") lab = "max" + std::to_string(sp.level);
          }
          temp += lab;
        }
      }
      collabels.push_back(temp);
    }
    collabelsOn = !collabels.empty();
  }

  // ---- table layout (estout) -------------------------------------------------------
  std::vector<int> modelsrow, eqsrow, colsrow, starsrow;  // -1 = an extracols column
  // vblock[i][c] = (element index, off-for-this-model); an empty list is `.`
  const size_t nrows = cells.rows.size();
  std::vector<std::vector<std::vector<std::pair<int, bool>>>> vblock(nrows);
  bool statsStarAny = false;
  for (const auto& st : sp.stats) {
    if (st.star) statsStarAny = true;
  }
  for (size_t m = 0; m < M; ++m) {
    for (size_t e = 0; e < eqswide.size(); ++e) {
      size_t stc = 0;
      if (sp.unstack && R > 0) {
        bool any = false;
        for (const auto& k : mrows[m]) {
          if (k.first == eqswide[e] && rowExists(k)) any = true;
        }
        if (!any) continue;
      }
      eqsrow.push_back(static_cast<int>(e));
      modelsrow.push_back(static_cast<int>(m));
      size_t k = 0;
      bool something = false;
      for (size_t j = 0; j < cells.ncols; ++j) {
        bool nocol = true, colhasstats = false, starcol = false;
        std::vector<std::vector<std::pair<int, bool>>> col(nrows);
        for (size_t i = 0; i < nrows; ++i) {
          if (j >= cells.rows[i].size()) continue;
          for (int idx : cells.rows[i][j]) {
            const EoElem& el = cells.elems[static_cast<size_t>(idx)];
            colhasstats = true;
            const bool off = m < el.pattern.size() && el.pattern[m] == "0";
            if (!off) {
              nocol = false;
              if (el.star) starcol = true;
            }
            col[i].emplace_back(idx, off);
          }
        }
        if (!colhasstats) nocol = false;
        if (nocol) continue;
        colsrow.push_back(static_cast<int>(j));
        if (++k > 1) {
          modelsrow.push_back(static_cast<int>(m));
          eqsrow.push_back(static_cast<int>(e));
        }
        if (stc++ == 0 && statsStarAny) starcol = true;
        starsrow.push_back(starcol ? 1 : 0);
        for (size_t i = 0; i < nrows; ++i) vblock[i].push_back(col[i]);
        something = true;
      }
      if (!something) {
        for (size_t i = 0; i < nrows; ++i) vblock[i].emplace_back();
        colsrow.push_back(0);
        bool starcol = (stc++ == 0 && statsStarAny);
        starsrow.push_back(starcol ? 1 : 0);
      }
    }
  }
  int neqs = 0;
  {
    int m0 = -1, e0 = -1;
    for (size_t i = 0; i < modelsrow.size(); ++i) {
      if (modelsrow[i] != m0 || eqsrow[i] != e0) ++neqs;
      m0 = modelsrow[i];
      e0 = eqsrow[i];
    }
  }
  if (!sp.extracols.empty()) {
    std::vector<int> ec = sp.extracols;
    std::sort(ec.begin(), ec.end());
    auto ins = [&](auto& row, auto symb) {
      using T = typename std::decay_t<decltype(row)>::value_type;
      std::decay_t<decltype(row)> nv;
      size_t ci = 0;
      for (size_t i = 0; i < row.size(); ++i) {
        while (ci < ec.size() && ec[ci] <= static_cast<int>(i + 1)) {
          nv.push_back(T(symb));
          ++ci;
        }
        nv.push_back(row[i]);
      }
      while (ci < ec.size()) {
        nv.push_back(T(symb));
        ++ci;
      }
      row = nv;
    };
    ins(modelsrow, -1);
    ins(eqsrow, -1);
    ins(colsrow, -1);
    ins(starsrow, -1);
    for (auto& r : vblock) ins(r, std::vector<std::pair<int, bool>>{});
  }
  const size_t nphys = starsrow.size();
  const int ncols = static_cast<int>(nphys) + 1 + (sp.labcol2 ? 1 : 0);

  // ---- widths (estout) --------------------------------------------------------------
  const std::vector<int>& mwv = sp.modelwidth;
  auto mw = [&](size_t c) { return mwv.empty() ? 0 : mwv[c % mwv.size()]; };
  const bool modelwidthzero = (mwv.size() == 1 && mwv[0] == 0) || mwv.empty();
  const int labcol2width = sp.labcol2Width >= 0 ? sp.labcol2Width : (mwv.empty() ? 0 : mwv[0]);
  std::vector<std::pair<std::string, std::string>> starlevels = sp.starlevels;
  if ((tablehasstars || statsStarAny) && starlevels.empty()) {
    starlevels = {{"*", "0.05"}, {"**", "0.01"}, {"***", "0.001"}};
  }
  int starwidth = 0;
  if (!modelwidthzero && (tablehasstars || statsStarAny)) {
    for (const auto& lv : starlevels) starwidth = std::max(starwidth, static_cast<int>(lv.first.size()));
  }
  const bool wrap = sp.wrap && sp.varwidth >= 2;
  const int delwidth = static_cast<int>(sp.delimiter.size());
  int totcharwidth = sp.varwidth;
  if (sp.labcol2) totcharwidth += delwidth + labcol2width;
  for (size_t c = 0; c < nphys; ++c) {
    totcharwidth += delwidth + mw(c);
    if (starsrow[c] != 0) totcharwidth += starwidth;
  }
  std::string hline;
  {
    const int hl = std::min(totcharwidth, 400);
    for (int i = 0; i < hl; ++i) hline += sp.hlinechar;
  }
  const bool anySpan = sp.mgroups.span || sp.mlabels.span || sp.eqlabels.span || sp.collabels.span;
  const int delwidthSpan = anySpan ? (modelwidthzero ? 0 : delwidth) : delwidth;
  const std::string starlegend =
      (tablehasstars || statsStarAny) ? [&]() {
        EoSpec tmp;
        tmp.starlevels = starlevels;
        tmp.starlevelsLabel = sp.starlevelsLabel;
        tmp.starlevelsDelim = sp.starlevelsDelim;
        return eoStarLegend(tmp);
      }()
                                      : std::string();
  const std::string lastend = sp.lastendSet ? sp.lastend : sp.end;

  // ---- output helpers -----------------------------------------------------------------------------
  std::string buf;
  auto w = [&](const std::string& s) { buf += s; };
  auto nl = [&]() { buf += "\n"; };
  auto insertAt = [&](const std::string& v, int type, const std::string& span) {
    std::string s = eoReplaceAll(v, "@span", span);
    if (type == 1) return s;
    s = eoReplaceAll(s, "@M", std::to_string(M));
    s = eoReplaceAll(s, "@E", std::to_string(neqs));
    s = eoReplaceAll(s, "@width", std::to_string(totcharwidth));
    s = eoReplaceAll(s, "@hline", hline);
    if (type == 2) return s;
    s = eoReplaceAll(s, "@title", sp.title);
    s = eoReplaceAll(s, "@note", sp.note);
    s = eoReplaceAll(s, "@discrete", std::string());
    s = eoReplaceAll(s, "@starlegend", starlegend);
    return s;
  };
  auto insertAtLines = [&](const std::vector<std::string>& v, int type) {
    std::vector<std::string> o;
    for (const auto& l : v) o.push_back(insertAt(l, type, std::to_string(ncols)));
    return o;
  };
  auto writeStrLines = [&](const std::vector<std::string>& lines) {
    bool first = true;
    for (const auto& l : lines) {
      if (!first) nl();
      w(l);
      first = false;
    }
  };
  auto writeBegin = [&](const std::vector<std::string>& pre, const std::string& begin,
                        const std::string& post) {
    writeStrLines(pre);
    w(begin);
    w(post);
  };
  auto writeEnd = [&](const std::string& end, const std::vector<std::string>& post,
                      const std::vector<std::string>& post2) {
    w(end);
    writeStrLines(post);
    writeStrLines(post2);
    nl();
  };
  auto fmtV = [&](const std::string& s) { return eoPadR(s, sp.varwidth); };
  auto fmtL2 = [&](const std::string& s) { return eoCentre(s, labcol2width); };
  std::vector<std::string> l2title = sp.labcol2Title;
  std::vector<std::string> l2vals = sp.labcol2Vals;
  auto popChunk = [](std::vector<std::string>* v) {
    std::string c;
    if (!v->empty()) {
      c = v->front();
      v->erase(v->begin());
    }
    return c;
  };
  auto writeL2 = [&](std::vector<std::string>* src) {
    if (!sp.labcol2) return;
    const std::string chunk = popChunk(src);
    w(sp.delimiter);
    w(fmtL2(eoAbbrev(labcol2width, chunk, sp.abbrev)));
  };
  auto starPad = [&](size_t c) {
    if (c < starsrow.size() && starsrow[c] == 1) w(std::string(static_cast<size_t>(starwidth), ' '));
  };

  // WriteCaption (estout). Group ids are 1-based; -1 is an extracols column.
  auto writeCaption = [&](const std::vector<int>& row, const std::vector<int>& rowtwo,
                          const std::vector<std::string>& labels, bool span,
                          const std::string& repeat, const std::string& prefix,
                          const std::string& suffix) {
    int c0 = 2 + (sp.labcol2 ? 1 : 0);
    int nspan = 0;
    int spanwidth = -delwidthSpan;
    std::string repeatlist;
    int lastr = -9, lastrtwo = -9;
    auto label = [&](int r) {
      return (r >= 1 && static_cast<size_t>(r) <= labels.size()) ? labels[static_cast<size_t>(r) - 1]
                                                                 : std::string();
    };
    for (size_t c = 0; c < row.size(); ++c) {
      const int r = row[c];
      const int rtwo = c < rowtwo.size() ? rowtwo[c] : -9;
      const int cw = mw(c);
      if (r == -1) {
        ++c0;
        w(sp.delimiter);
        w(eoPadL("", cw));
        continue;
      }
      if (!span) {
        std::string value;
        if (r != lastr || rtwo != lastrtwo || rowtwo.empty()) {
          value = eoAbbrev(cw, label(r), sp.abbrev);
          value = prefix + value + suffix;
          value = insertAt(value, 1, "1");
        }
        w(sp.delimiter);
        w(eoPadL(value, cw));
        starPad(c);
        lastr = r;
        lastrtwo = rtwo;
      } else {
        ++nspan;
        spanwidth += cw + delwidthSpan;
        if (starsrow[c] == 1) spanwidth += starwidth;
        const int nextr = (c + 1 < row.size()) ? row[c + 1] : -9;
        const int nextrtwo = (c + 1 < rowtwo.size()) ? rowtwo[c + 1] : -9;
        if (r != nextr || rtwo != nextrtwo || rowtwo.empty()) {
          std::string value = eoAbbrev(spanwidth, label(r), sp.abbrev);
          value = prefix + value + suffix;
          value = insertAt(value, 1, std::to_string(nspan));
          w(sp.delimiter);
          w(spanwidth > 0 ? eoPadR(value, spanwidth) : value);
          repeatlist += insertAt(repeat, 1, std::to_string(c0) + "-" + std::to_string(c0 + nspan - 1));
          c0 += nspan;
          nspan = 0;
          spanwidth = -delwidthSpan;
        }
      }
    }
    return repeatlist;
  };
  // WriteEqrow (estout)
  auto writeEqrow = [&](const std::string& value0, bool span, const std::string& prefix,
                        const std::string& suffix) {
    if (!span) {
      std::string value = eoAbbrev(sp.varwidth, value0, sp.abbrev);
      value = prefix + value + suffix;
      value = insertAt(value, 1, "1");
      w(fmtV(value));
      if (sp.labcol2) {
        w(sp.delimiter);
        w(fmtL2(""));
      }
      for (size_t c = 0; c < nphys; ++c) {
        w(sp.delimiter);
        w(eoPadL("", mw(c)));
        if (starsrow[c] == 1) w(std::string(static_cast<size_t>(starwidth), ' '));
      }
      return;
    }
    int nspan = 1;
    int spanwidth = sp.varwidth;
    if (sp.labcol2) {
      ++nspan;
      spanwidth += delwidthSpan + labcol2width;
    }
    for (size_t c = 0; c < nphys; ++c) {
      ++nspan;
      spanwidth += delwidthSpan + mw(c);
      if (starsrow[c] == 1) spanwidth += starwidth;
    }
    std::string value = eoAbbrev(spanwidth, value0, sp.abbrev);
    value = prefix + value + suffix;
    value = insertAt(value, 1, std::to_string(nspan));
    w(spanwidth > 0 ? eoPadR(value, spanwidth) : value);
  };
  // WriteStrRow (estout): one label per (model, equation).
  auto writeStrRow = [&](const std::vector<std::string>& labels) {
    int lastm = -9, laste = -9;
    for (size_t c = 0; c < nphys; ++c) {
      const int cw = mw(c);
      if (modelsrow[c] == -1) {
        w(sp.delimiter);
        w(eoPadL("", cw));
        continue;
      }
      std::string value;
      if (modelsrow[c] != lastm || eqsrow[c] != laste) {
        const size_t idx = static_cast<size_t>(modelsrow[c]) * eqswide.size() +
                           static_cast<size_t>(eqsrow[c]);
        value = idx < labels.size() ? labels[idx] : std::string();
        value = eoAbbrev(cw, value, sp.abbrev);
      }
      w(sp.delimiter);
      w(eoPadL(value, cw));
      starPad(c);
      lastm = modelsrow[c];
      laste = eqsrow[c];
    }
  };
  auto fmtFor = [](const EoElem& el, size_t r) {
    if (el.fmt.empty()) return std::string("%9.0g");
    return r < el.fmt.size() ? el.fmt[r] : el.fmt.back();
  };
  auto pValue = [&](const EoModel& m, const EoKey& key, const EoElem& el) {
    return eoElemValue(m, key, el.pvalue);
  };

  // ---- prehead -------------------------------------------------------------------------------------
  {
    std::vector<std::string> pre = sp.prehead, post = sp.postfoot;
    // estout: with no hooks at all, title/note become them.
    if (sp.prehead.empty() && sp.posthead.empty() && sp.prefoot.empty() && sp.postfoot.empty()) {
      if (!sp.title.empty()) pre = {sp.title};
    }
    for (const auto& line : pre) {
      w(insertAt(line, 0, std::to_string(ncols)));
      nl();
    }
  }

  std::vector<int> modelIds, eqIds, colIds;
  for (size_t c = 0; c < nphys; ++c) {
    modelIds.push_back(modelsrow[c] < 0 ? -1 : modelsrow[c] + 1);
    eqIds.push_back(eqsrow[c] < 0 ? -1 : eqsrow[c] + 1);
    colIds.push_back(colsrow[c] < 0 ? -1 : colsrow[c] + 1);
  }

  // ---- head: model groups ------------------------------------------------------------------------------
  if (sp.mgroups.given && !sp.mgroups.none && !sp.mgroups.list.empty()) {
    const std::vector<std::string> mgb = insertAtLines(sp.mgroups.begin, 2);
    const std::vector<std::string> mge = insertAtLines(sp.mgroups.end, 2);
    std::string tb = sp.begin, te = sp.end;
    if (sp.mgroups.replace) {
      if (!mgb.empty()) tb.clear();
      if (!mge.empty()) te.clear();
    }
    // MgroupsPattern (estout)
    std::vector<int> pat;
    {
      int i = 0, m0 = -9, j = 0;
      for (size_t c = 0; c < nphys; ++c) {
        const int m = modelIds[c];
        if (m == -1) {
          pat.push_back(-1);
          continue;
        }
        if (m != m0) {
          ++i;
          std::string p = (static_cast<size_t>(i) <= sp.mgroups.pattern.size())
                              ? sp.mgroups.pattern[static_cast<size_t>(i) - 1]
                              : std::string();
          if (i == 1) p = "1";
          if (p == "1") ++j;
        }
        pat.push_back(j);
        m0 = m;
      }
    }
    writeBegin(mgb, tb, fmtV(eoAbbrev(sp.varwidth, sp.mgroups.lhs, sp.abbrev)));
    writeL2(&l2title);
    const std::string rep = writeCaption(pat, pat, sp.mgroups.list, sp.mgroups.span,
                                         sp.mgroups.erepeat, sp.mgroups.prefix, sp.mgroups.suffix);
    writeEnd(te, mge, {rep});
  }
  // ---- head: model numbers ------------------------------------------------------------------------------
  if (sp.numbers) {
    w(sp.begin);
    w(fmtV(""));
    writeL2(&l2title);
    std::vector<std::string> nums;
    for (size_t i = 0; i < M; ++i) nums.push_back(std::to_string(i + 1));
    writeCaption(modelIds, modelIds, nums, sp.mlabels.span, "", sp.numL, sp.numR);
    w(sp.end);
    nl();
  }
  // ---- head: model labels -------------------------------------------------------------------------------
  {
    bool none = sp.mlabels.none;
    if (M == 1 && models[0].name == "." && mlabels.size() == 1 && mlabels[0] == ".") none = true;
    if (!none) {
      const std::vector<std::string> mb = insertAtLines(sp.mlabels.begin, 2);
      const std::vector<std::string> me = insertAtLines(sp.mlabels.end, 2);
      std::string tb = sp.begin, te = sp.end;
      if (sp.mlabels.replace) {
        if (!mb.empty()) tb.clear();
        if (!me.empty()) te.clear();
      }
      writeBegin(mb, tb, fmtV(eoAbbrev(sp.varwidth, sp.mlabels.lhs, sp.abbrev)));
      writeL2(&l2title);
      const std::string rep = writeCaption(modelIds, modelIds, mlabels, sp.mlabels.span,
                                           sp.mlabels.erepeat, sp.mlabels.prefix, sp.mlabels.suffix);
      writeEnd(te, me, {rep});
    }
  }
  // ---- head: equation labels (unstack) -----------------------------------------------------------------
  std::vector<std::string> eqlabelsbegin, eqlabelsend;
  if (!sp.eqlabels.none) {
    eqlabelsbegin = insertAtLines(sp.eqlabels.begin, 2);
    eqlabelsend = insertAtLines(sp.eqlabels.end, 2);
  }
  if (!(eqswide.size() == 1 && eqswide[0] == "__") && !sp.eqlabels.none) {
    std::string tb = sp.begin, te = sp.end;
    if (sp.eqlabels.replace) {
      if (!eqlabelsbegin.empty()) tb.clear();
      if (!eqlabelsend.empty()) te.clear();
    }
    writeBegin(eqlabelsbegin, tb, fmtV(eoAbbrev(sp.varwidth, sp.eqlabels.lhs, sp.abbrev)));
    writeL2(&l2title);
    const std::string rep = writeCaption(eqIds, modelIds, eqlabels, sp.eqlabels.span,
                                         sp.eqlabels.erepeat, sp.eqlabels.prefix, sp.eqlabels.suffix);
    writeEnd(te, eqlabelsend, {rep});
  }
  // ---- head: column labels ----------------------------------------------------------------------------------
  if (collabelsOn && !sp.collabels.none) {
    const std::vector<std::string> cb = insertAtLines(sp.collabels.begin, 2);
    const std::vector<std::string> ce = insertAtLines(sp.collabels.end, 2);
    std::string tb = sp.begin, te = sp.end;
    if (sp.collabels.replace) {
      if (!cb.empty()) tb.clear();
      if (!ce.empty()) te.clear();
    }
    writeBegin(cb, tb, fmtV(eoAbbrev(sp.varwidth, sp.collabels.lhs, sp.abbrev)));
    writeL2(&l2title);
    const std::string rep = writeCaption(colIds, {}, collabels, sp.collabels.span,
                                         sp.collabels.erepeat, sp.collabels.prefix, sp.collabels.suffix);
    writeEnd(te, ce, {rep});
  }
  // ---- posthead ------------------------------------------------------------------------------------------------
  for (const auto& line : sp.posthead) {
    w(insertAt(line, 0, std::to_string(ncols)));
    nl();
  }

  // ---- body (estout) ------------------------------------------------------------------------------
  const bool varlabelsnone = sp.varlabels.none;
  const bool varlabelsFirst = sp.varlabels.first != 0;
  const bool varlabelsLast = sp.varlabels.last != 0;
  const bool eqlabelsFirst = sp.eqlabels.first != 0;
  const bool eqlabelsLast = sp.eqlabels.last != 0;
  const std::vector<std::string> varlabelsbegin = insertAtLines(sp.varlabels.begin, 2);
  std::vector<std::string> varlabelsend = insertAtLines(sp.varlabels.end, 2);
  const size_t nind = indicateLbls.size();
  auto varInList = [&](const std::string& var, const std::string& eqvar, const std::string& eq,
                       const std::vector<std::string>& list, bool* found) {
    *found = false;
    for (size_t l = 0; l + 1 <= list.size(); l += 2) {
      std::string lvar = list[l];
      const std::string lab = l + 1 < list.size() ? list[l + 1] : std::string();
      if (sp.unstack) {
        if (var == lvar) {
          *found = true;
          return lab;
        }
      } else {
        if (lvar.rfind("_:", 0) == 0) lvar = "_" + lvar;
        if (lvar == var || lvar == eqvar || lvar == eq + ":") {
          *found = true;
          return lab;
        }
      }
    }
    return std::string();
  };
  std::vector<std::string> varlabelsbegin0 = varlabelsbegin;
  std::vector<std::string> eqlabelsbegin0 = eqlabelsFirst ? eqlabelsbegin : std::vector<std::string>{};
  std::vector<std::string> refcatcoefs, refcatnames;
  for (const auto& [c, n] : sp.refcat) {
    refcatcoefs.push_back(c);
    refcatnames.push_back(n);
  }
  if (nrows > 0 || true) {
    const size_t RI = R + nind;
    size_t e = 0;
    size_t eqdim = R + nind;
    size_t weqcnt = 0;
    std::string theeqlabel;
    std::string eqr, eqrlast;
    bool eqrInit = false;
    for (size_t r = 0; r < R; ++r) {
      std::vector<std::string> varlabelsend0 = varlabelsend;
      const std::string var = sp.unstack ? varlist[r] : B[r].second;
      std::string eqvar;
      if (!sp.unstack) {
        eqvar = B[r].first + ":" + var;
        if (!(eqs.size() == 1 && eqs[0] == "__")) {
          eqrlast = eqr;
          eqr = B[r].first;
          if ((!eqrInit || eqr != eqrlast) && !sp.eqlabels.none) {
            const std::string value = e < eqlabels.size() ? eqlabels[e] : std::string();
            ++e;
            eqdim = static_cast<size_t>(e - 1 < eqsdims.size() ? eqsdims[e - 1] : 0);
            weqcnt = 0;
            if (e == numeqs) {
              if (!eqlabelsLast) eqlabelsend.clear();
              eqdim += nind;
            }
            if (sp.eqlabels.merge) {
              theeqlabel = sp.eqlabels.prefix + value + sp.eqlabels.suffix;
            } else {
              std::string tb = sp.begin, te = sp.end;
              if (sp.eqlabels.replace) {
                if (!eqlabelsbegin0.empty()) tb.clear();
                if (!eqlabelsend.empty()) te.clear();
              }
              writeBegin(eqlabelsbegin0, tb, "");
              writeEqrow(value, sp.eqlabels.span, sp.eqlabels.prefix, sp.eqlabels.suffix);
              writeEnd(te, eqlabelsend, {});
              eqlabelsbegin0 = eqlabelsbegin;
            }
          }
          eqrInit = true;
        }
      }
      ++weqcnt;
      if (weqcnt == 1 && !varlabelsFirst) varlabelsbegin0.clear();

      // rows of the cell block this coefficient writes
      std::vector<size_t> rv;
      for (size_t i = 0; i < nrows; ++i) {
        bool skiprow = true, rowhasstats = false;
        for (size_t c = 0; c < nphys && skiprow; ++c) {
          EoKey key = B.empty() ? EoKey{} : (sp.unstack ? EoKey{} : B[r]);
          if (sp.unstack) {
            const int eqi = eqsrow[c];
            key = EoKey{eqi >= 0 ? eqswide[static_cast<size_t>(eqi)] : std::string(), var};
          }
          for (const auto& [idx, off] : vblock[i][c]) {
            (void)idx;
            if (rowExists(key)) {
              rowhasstats = true;
              if (off) continue;
              skiprow = false;
              break;
            }
          }
        }
        if (!rowhasstats) skiprow = false;
        if (i + 1 == nrows && rv.empty()) skiprow = false;
        if (!skiprow) rv.push_back(i);
      }
      const size_t nrv = rv.size();

      // refcat() above the coefficient
      size_t isref = 0;
      for (size_t i = 0; i < refcatcoefs.size(); ++i) {
        if (refcatcoefs[i] == var) {
          isref = i + 1;
          break;
        }
      }
      auto refcatRow = [&]() {
        std::vector<std::string> lbls;
        for (size_t m = 0; m < M; ++m) {
          for (const auto& eq : (sp.unstack ? eqswide : std::vector<std::string>{eqr.empty() ? B[r].first : eqr})) {
            bool has = false;
            for (const auto& k : mrows[m]) {
              if (k.first == eq && k.second == var) has = true;
            }
            lbls.push_back(has ? sp.refcatLabel : std::string());
          }
        }
        return lbls;
      };
      auto writeRefcat = [&](bool last) {
        std::string tb = sp.begin, te = sp.end;
        if (last && nind == 0 && sp.stats.empty()) te = lastend;
        std::vector<std::string> ve0 = varlabelsend0;
        if (last && weqcnt == eqdim && !varlabelsLast) ve0.clear();
        if (sp.varlabels.replace) {
          if (!varlabelsbegin0.empty()) tb.clear();
          if (!ve0.empty()) te.clear();
        }
        std::string value = varlabelsnone ? std::string()
                                          : eoAbbrev(sp.varwidth, refcatnames[isref - 1], sp.abbrev);
        value = fmtV(sp.varlabels.prefix + value + sp.varlabels.suffix);
        writeBegin(varlabelsbegin0, tb, value);
        writeL2(&l2vals);
        writeStrRow(refcatRow());
        writeEnd(te, last ? ve0 : varlabelsend0, {});
        varlabelsbegin0 = varlabelsbegin;
      };
      if (isref != 0 && !sp.refcatBelow) writeRefcat(false);

      // the label
      std::string tmpbegin = sp.begin;
      std::string varl;
      if (!varlabelsnone) {
        bool found = false;
        const std::string bl = varInList(var, eqvar, eqr, sp.varlabels.blist, &found);
        if (found) {
          const std::vector<std::string> lines =
              insertAtLines(bl.find('"') == std::string::npos ? std::vector<std::string>{bl} : eoTokens(bl), 2);
          writeStrLines(lines);
          if (sp.varlabels.replace) {
            tmpbegin.clear();
            varlabelsbegin0.clear();
          }
        }
        varl = sp.label ? eoCompileVarl(session, var, sp.interaction) : var;
        const std::string lab = varInList(var, eqvar, eqr, varlabelPairs, &found);
        if (found) varl = lab;
      }
      if (sp.varlabels.replace && !varlabelsbegin0.empty()) tmpbegin.clear();
      int wrap_i = 1;
      std::string value;
      if (wrap && nrv > 1) {
        value = eoPiece(wrap_i, sp.varwidth, theeqlabel + varl);
        value = eoAbbrev(sp.varwidth, value, sp.abbrev);
      } else {
        value = eoAbbrev(sp.varwidth, theeqlabel + varl, sp.abbrev);
      }
      value = fmtV(sp.varlabels.prefix + value + sp.varlabels.suffix);
      writeBegin(varlabelsbegin0, tmpbegin, value);
      writeL2(&l2vals);
      varlabelsbegin0 = varlabelsbegin;

      // the cells
      bool newrow = false;
      for (size_t i : rv) {
        for (size_t c = 0; c < nphys; ++c) {
          if (newrow && c == 0) {
            std::string v;
            if (wrap && nrv > 1) {
              std::string space;
              for (;;) {
                const std::string temp = eoPiece(++wrap_i, sp.varwidth, varl);
                if (temp.empty()) break;
                v += space + temp;
                if (static_cast<size_t>(wrap_i) < nrv) break;
                space = " ";
              }
              v = fmtV(sp.varlabels.prefix + eoAbbrev(sp.varwidth, v, sp.abbrev) + sp.varlabels.suffix);
            } else {
              v = std::string(static_cast<size_t>(std::max(sp.varwidth, 0)), ' ');
            }
            w(sp.end);
            nl();
            w(sp.begin);
            w(v);
            if (sp.labcol2) {
              w(sp.delimiter);
              w(fmtL2(""));
            }
          }
          const int mi = modelsrow[c];
          const auto& elems = vblock[i][c];
          int cwEff = mw(c);
          std::string thevalue;
          EoKey key = sp.unstack ? EoKey{} : B[r];
          bool skipcoef = false;
          if (sp.unstack && mi >= 0) {
            const int eqi = eqsrow[c];
            key = EoKey{eqi >= 0 ? eqswide[static_cast<size_t>(eqi)] : std::string(), var};
            if (!rowExists(key)) skipcoef = true;
          }
          for (size_t t = 0; t < elems.size(); ++t) {
            const EoElem& el = cells.elems[static_cast<size_t>(elems[t].first)];
            const bool off = elems[t].second || mi < 0;
            std::string v;
            if (off) {
              v.clear();
            } else if (skipcoef) {
              v = el.vacant;
            } else {
              const EoModel& em = models[static_cast<size_t>(mi)];
              const size_t fr = sp.unstack ? r : r;
              if (el.name == "ci") {
                const EoV lo = eoElemValue(em, key, "ci_l");
                const EoV hi = eoElemValue(em, key, "ci_u");
                if (lo.kind == EoV::kVacant) {
                  v = el.vacant;
                } else {
                  std::string a, b2;
                  const std::vector<std::string> pl{el.par.size() > 0 ? el.par[0] : "",
                                                    el.par.size() > 1 ? el.par[1] : ""};
                  const std::vector<std::string> pu{"", el.par.size() > 2 ? el.par[2] : ""};
                  if (!eoVFormat(lo.kind == EoV::kNum ? lo.v : kEoNaN, fmtFor(el, fr), sp.lz,
                                 el.hasPar ? pl : std::vector<std::string>{}, &a) ||
                      !eoVFormat(hi.kind == EoV::kNum ? hi.v : kEoNaN, fmtFor(el, fr), sp.lz,
                                 el.hasPar ? pu : std::vector<std::string>{}, &b2)) {
                    err->msg = fmtFor(el, fr) + ": invalid format";
                    err->rc = 198;
                    return false;
                  }
                  v = a + b2;
                }
              } else {
                const EoV x = eoElemValue(em, key, el.name);
                if (x.kind == EoV::kVacant) {
                  v = el.vacant;
                } else if (el.name == "_star") {
                  v = eoStars(starlevels, pValue(em, key, el));
                  if (el.hasPar && el.par.size() >= 1) {
                    v = el.par[0] + v + (el.par.size() > 1 ? el.par[1] : std::string());
                  }
                } else if (el.name == "_sign" || el.name == "_sigsign") {
                  err->msg = "estout: cells(" + el.name + ") is not implemented in this build";
                  err->rc = 198;
                  return false;
                } else {
                  double num = x.kind == EoV::kNum ? x.v : kEoNaN;
                  if (el.abs && (el.name == "t" || el.name == "z") && !std::isnan(num)) num = std::fabs(num);
                  if (!eoVFormat(num, fmtFor(el, fr), sp.lz,
                                 el.hasPar ? el.par : std::vector<std::string>{}, &v)) {
                    err->msg = fmtFor(el, fr) + ": invalid format";
                    err->rc = 198;
                    return false;
                  }
                }
              }
            }
            thevalue += v;
            if (c < starsrow.size() && starsrow[c] == 1) {
              cwEff = mw(c) + starwidth;
              std::string sv;
              if (!off && el.star && !skipcoef) {
                sv = eoStars(starlevels, pValue(models[static_cast<size_t>(mi)], key, el));
              }
              if (t + 1 == elems.size()) sv = eoPadR(sv, starwidth);
              thevalue += sv;
            }
            if (t + 1 < elems.size()) thevalue += sp.incelldelimiter;
          }
          w(sp.delimiter);
          w(eoPadL(thevalue, cwEff));
        }
        newrow = true;
      }

      // end of the table row
      std::string tmpend = sp.end;
      if (!(isref != 0 && sp.refcatBelow)) {
        if (weqcnt == eqdim && !varlabelsLast) varlabelsend0.clear();
        if (r + 1 == RI && sp.stats.empty()) tmpend = lastend;
      }
      if (sp.varlabels.replace && !varlabelsend0.empty()) tmpend.clear();
      std::vector<std::string> elistLines;
      {
        bool found = false;
        const std::string el = varInList(var, eqvar, eqr, sp.varlabels.elist, &found);
        if (found) {
          elistLines = insertAtLines(el.find('"') == std::string::npos ? std::vector<std::string>{el}
                                                                       : eoTokens(el),
                                     2);
          if (sp.varlabels.replace) varlabelsend0.clear();
        }
      }
      writeEnd(tmpend, varlabelsend0, elistLines);
      if (isref != 0 && sp.refcatBelow) writeRefcat(r + 1 == RI);
    }
  }

  // ---- indicator sets (estout) --------------------------------------------------------------
  for (size_t i = 0; i < nind; ++i) {
    std::string tb = sp.begin, te = sp.end;
    if (i + 1 == nind) {
      if (sp.stats.empty()) te = lastend;
      if (!varlabelsLast) varlabelsend.clear();
    }
    if (sp.varlabels.replace) {
      if (!varlabelsbegin0.empty()) tb.clear();
      if (!varlabelsend.empty()) te.clear();
    }
    std::string value = varlabelsnone ? std::string()
                                      : eoAbbrev(sp.varwidth, indicateLbls[i].first, sp.abbrev);
    value = fmtV(sp.varlabels.prefix + value + sp.varlabels.suffix);
    writeBegin(varlabelsbegin0, tb, value);
    writeL2(&l2vals);
    writeStrRow(indicateLbls[i].second);
    writeEnd(te, varlabelsend, {});
  }

  // ---- prefoot ------------------------------------------------------------------------------------------------
  for (const auto& line : sp.prefoot) {
    w(insertAt(line, 0, std::to_string(ncols)));
    nl();
  }

  // ---- statistics (estout) ----------------------------------------------------------------------
  {
    const std::vector<std::string> slb = insertAtLines(sp.statslabels.begin, 2);
    std::vector<std::string> sle = insertAtLines(sp.statslabels.end, 2);
    std::vector<std::string> slb0 = slb;
    const bool sFirst = sp.statslabels.first != 0;
    const bool sLast = sp.statslabels.last != 0;
    const size_t S = sp.stats.size();
    for (size_t r = 0; r < S; ++r) {
      const EoStatRow& st = sp.stats[r];
      std::string stat = r < sp.statslabels.list.size() ? sp.statslabels.list[r] : std::string();
      if (stat.empty()) stat = st.key;
      if (sp.statslabels.none) stat.clear();
      if (r == 0 && !sFirst) slb0.clear();
      std::string tb = sp.begin;
      if (sp.statslabels.replace && !slb0.empty()) tb.clear();
      const std::string value =
          fmtV(sp.statslabels.prefix + eoAbbrev(sp.varwidth, stat, sp.abbrev) + sp.statslabels.suffix);
      writeBegin(slb0, tb, value);
      if (r == 0 && !sFirst) slb0 = slb;
      writeL2(&l2vals);
      int lastm = -9;
      size_t stc = 0;
      for (size_t c = 0; c < nphys; ++c) {
        const int cw = mw(c);
        const int m = modelsrow[c];
        if (m == -1) {
          w(sp.delimiter);
          w(eoPadL("", cw));
          continue;
        }
        if (m != lastm) stc = 0;
        ++stc;
        std::string cell;
        bool has = false;
        if (stc == 1) {
          const EoV v = eoStatValue(models[static_cast<size_t>(m)], st.key);
          if (v.kind == EoV::kText) {
            cell = v.text;
            has = true;
          } else if (v.kind == EoV::kNum || v.kind == EoV::kMissing) {
            if (!eoVFormat(v.kind == EoV::kNum ? v.v : kEoNaN, st.fmt, sp.lz, {}, &cell)) {
              err->msg = st.fmt + ": invalid format";
              err->rc = 198;
              return false;
            }
            has = true;
          }
        }
        w(sp.delimiter);
        w(eoPadL(cell, cw));
        if (starsrow[c] == 1) {
          if (st.star && has) {
            const EoV p = eoStatValue(models[static_cast<size_t>(m)], "p");
            w(eoPadR(eoStars(starlevels, p), starwidth));
          } else {
            w(std::string(static_cast<size_t>(starwidth), ' '));
          }
        }
        lastm = m;
      }
      std::string te = sp.end;
      if (r + 1 == S) {
        if (!sLast) sle.clear();
        te = lastend;
      }
      if (sp.statslabels.replace && !sle.empty()) te.clear();
      writeEnd(te, sle, {});
    }
  }

  // ---- postfoot and legend ---------------------------------------------------------------------------------------
  {
    std::vector<std::string> post = sp.postfoot;
    if (sp.prehead.empty() && sp.posthead.empty() && sp.prefoot.empty() && sp.postfoot.empty()) {
      if (!sp.note.empty()) post = {sp.note};
    }
    for (const auto& line : post) {
      w(insertAt(line, 0, std::to_string(ncols)));
      nl();
    }
  }
  if (sp.legend && !starlegend.empty()) {
    w(starlegend);
    nl();
  }

  // ---- substitute() on every line (estout) ------------------------------------------------------------
  if (!sp.substitute.empty()) {
    std::string res;
    size_t start = 0;
    while (start < buf.size()) {
      size_t eol = buf.find('\n', start);
      if (eol == std::string::npos) eol = buf.size();
      std::string line = buf.substr(start, eol - start);
      for (const auto& [from, to] : sp.substitute) {
        if (!(from.empty() && to.empty())) line = eoReplaceAll(line, from, to);
      }
      res += line;
      if (eol < buf.size()) res += "\n";
      start = eol + 1;
    }
    buf = res;
  }
  *out = buf;
  return true;
}

}  // namespace

namespace {

// The estimator's equation naming in e(b) -- the rules of eval/macros.cpp's
// kEStripeTable that a table needs: logit/probit name the one equation after
// the depvar, poisson after the depvar with `.` as `_`, mlogit one equation per
// outcome, everything else none (`__`, estout's spelling of `_`).
std::string eoEquationOf(const std::string& estimator, const std::string& depname) {
  if (estimator == "logit" || estimator == "probit" || estimator == "logistic") return depname;
  if (estimator == "poisson") {
    std::string d = depname;
    std::replace(d.begin(), d.end(), '.', '_');
    return d;
  }
  return "__";
}

std::string eoOutcomeName(const ojson& o) {
  if (o.contains("outcome") && o["outcome"].is_number()) {
    const double v = o["outcome"].get<double>();
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%.15g", v);
    return buf;
  }
  return o.value("depvar", std::string());
}

double eoJ(const ojson& r, const char* k) {
  return (r.contains(k) && r[k].is_number()) ? r[k].get<double>() : kEoNaN;
}

// One stored model as the write phase reads it: e(b)'s rows with b/se/t/p/ci,
// the estadd/estpost matrices, and the transform() applied the way
// _estout_getres does (b, se and the interval; t and p stay untransformed).
EoModel eoModelFrom(const Model& m, const std::string& listedName, SessionState& session,
                    const TransformSpec* tr, size_t mi, bool dropOmitted, bool withBase = true) {
  EoModel em;
  em.name = listedName;
  em.model = &m;
  {
    const std::vector<std::string> dv = splitWs(m.snap.value("depvar", std::string()));
    em.depname = dv.empty() ? std::string() : dv[0];
  }
  if (m.snap.contains("title") && m.snap["title"].is_string()) em.title = m.snap["title"].get<std::string>();
  const std::string est = m.snap.value("estimator", std::string());

  auto addTable = [&](const ojson& ct, const std::string& eq, bool baseEq) {
    std::vector<EoKey> rows;
    bool hasCons = false;
    for (const auto& row : ct) {
      std::string name = row.value("name", std::string());
      // An equation-striped row (`treat:lower`, a posted e(b) column with its
      // equation, e.g. leebounds) carries its own equation, as estout reads it
      // from e(b)'s coleq; the bare name is the coefficient.
      std::string rowEq = eq;
      bool ownEq = false;
      if (const size_t colon = name.find(':'); colon != std::string::npos && colon > 0 &&
                                               colon + 1 < name.size()) {
        rowEq = name.substr(0, colon);
        name = name.substr(colon + 1);
        ownEq = true;
      }
      if (name == "(intercept)") name = "_cons";
      EoCoefVals v;
      v.b = eoJ(row, "b");
      v.se = eoJ(row, "se");
      v.t = eoJ(row, "t");
      v.p = eoJ(row, "p");
      v.cil = eoJ(row, "ci_lo");
      v.ciu = eoJ(row, "ci_hi");
      const bool omitted =
          !baseEq && (row.value("omitted", false) || (v.b == 0.0 && v.se == 0.0));
      if (omitted && dropOmitted) continue;
      // estout: var==0 -> se, t and p are missing, and the
      // interval collapses to b.
      if (v.se == 0.0 || omitted || baseEq) {
        v.se = kEoNaN;
        v.t = kEoNaN;
        v.p = kEoNaN;
        v.cil = v.b;
        v.ciu = v.b;
      } else if (std::isnan(v.t) && !std::isnan(v.b) && !std::isnan(v.se)) {
        v.t = v.b / v.se;
      }
      if (tr != nullptr && !tr->empty() && !std::isnan(v.b)) {
        const double b0 = v.b;
        const TransformCell tc = transformOneCell(*tr, mi, name, b0, m.snap, session);
        if (tc.matched) {
          if (tc.bMissing) v.b = kEoNaN;
          else if (tc.b.has_value()) v.b = *tc.b;
          if (tc.seFactor.has_value() && !std::isnan(v.se)) {
            v.se = std::isnan(*tc.seFactor) ? kEoNaN : v.se * std::fabs(*tc.seFactor);
          }
          for (double* ci : {&v.cil, &v.ciu}) {
            if (std::isnan(*ci)) continue;
            const TransformCell tci = transformOneCell(*tr, mi, name, *ci, m.snap, session);
            if (tci.bMissing) *ci = kEoNaN;
            else if (tci.b.has_value()) *ci = *tci.b;
          }
        }
      }
      const EoKey key{rowEq, name};
      if (em.vals.count(key) != 0) continue;
      em.vals[key] = v;
      if (ownEq) {
        rows.push_back(key);  // e(b) order within its own equation, `_cons` included
      } else if (name == "_cons") {
        hasCons = true;
      } else {
        rows.push_back(key);
      }
    }
    if (hasCons) rows.emplace_back(eq, "_cons");  // e(b) puts `_cons` last
    em.coefRows.insert(em.coefRows.end(), rows.begin(), rows.end());
  };

  if (m.snap.contains("outcomes") && m.snap["outcomes"].is_array() &&
      !(m.snap.contains("coeftable") && m.snap["coeftable"].is_array() && !m.snap["coeftable"].empty())) {
    // mlogit: one equation per outcome; the base outcome's equation is all
    // zeros with missing standard errors (e(b) carries it, e(V) is 0 there).
    ojson names = ojson::array();
    for (const auto& o : m.snap["outcomes"]) {
      if (o.contains("coeftable") && o["coeftable"].is_array() && !o["coeftable"].empty()) {
        names = o["coeftable"];
        break;
      }
    }
    for (const auto& o : m.snap["outcomes"]) {
      const std::string eq = eoOutcomeName(o);
      if (o.value("base", false) || !o.contains("coeftable") || o["coeftable"].empty()) {
        ojson zero = ojson::array();
        for (const auto& r : names) {
          ojson z;
          z["name"] = r.value("name", std::string());
          z["b"] = 0.0;
          z["se"] = 0.0;
          zero.push_back(z);
        }
        addTable(zero, eq, true);
      } else {
        addTable(o["coeftable"], eq, false);
      }
    }
  } else if (m.snap.contains("coeftable") && m.snap["coeftable"].is_array()) {
    const std::string eq = eoEquationOf(est, em.depname);
    const size_t first = em.coefRows.size();
    addTable(m.snap["coeftable"], eq, false);
    // BASELEV: the snapshot's (or, for the live results, the data's) factor
    // base levels become `L.var` rows with b 0 and a missing se.
    ojson bl = m.snap.contains("base_levels") ? m.snap["base_levels"] : ojson();
    if (bl.is_array() && withBase) {
      for (const auto& e : bl) {
        const std::string nm = e.value("name", std::string());
        const std::string before = e.value("before", std::string());
        const std::string after = e.value("after", std::string());
        if (nm.empty()) continue;
        const EoKey key{eq, nm};
        if (em.vals.count(key) != 0) continue;
        EoCoefVals v;
        v.b = 0.0;
        v.cil = v.ciu = 0.0;
        em.vals[key] = v;
        auto pos = em.coefRows.end();
        if (!before.empty()) {
          pos = std::find(em.coefRows.begin() + static_cast<long>(first), em.coefRows.end(),
                          EoKey{eq, before});
        } else if (!after.empty()) {
          pos = std::find(em.coefRows.begin() + static_cast<long>(first), em.coefRows.end(),
                          EoKey{eq, after});
          if (pos != em.coefRows.end()) ++pos;
        }
        em.coefRows.insert(pos, key);
      }
    }
  }

  // estadd matrix / estpost: 1 x k matrices keyed by column name.
  if (m.added.contains("matrices") && m.added["matrices"].is_object()) {
    for (const auto& [mname, mat] : m.added["matrices"].items()) {
      if (!mat.is_object() || !mat.contains("colnames") || !mat.contains("data")) continue;
      const ojson& cn = mat["colnames"];
      const ojson& data = mat["data"];
      if (!cn.is_array() || !data.is_array() || data.empty() || !data[0].is_array()) continue;
      std::vector<EoKey> order;
      std::map<EoKey, double> vals;
      for (size_t j = 0; j < cn.size(); ++j) {
        if (!cn[j].is_string()) continue;
        std::string col = cn[j].get<std::string>();
        std::string eq = "__";
        const size_t colon = col.find(':');
        if (colon != std::string::npos) {
          eq = col.substr(0, colon);
          col = col.substr(colon + 1);
        }
        if (col == "(intercept)") col = "_cons";
        const EoKey key{eq, col};
        order.push_back(key);
        vals[key] = (j < data[0].size() && data[0][j].is_number()) ? data[0][j].get<double>() : kEoNaN;
      }
      // `_cons` last, as mat_capp's `cons` would place it
      auto cit = std::find_if(order.begin(), order.end(), [](const EoKey& k) { return k.second == "_cons"; });
      if (cit != order.end()) {
        const EoKey ck = *cit;
        order.erase(cit);
        order.push_back(ck);
      }
      em.extraOrder[mname] = order;
      em.extra[mname] = vals;
    }
  }
  return em;
}

}  // namespace

namespace {

// ---- esttab's per-mode table (esttab), the modes this build writes --
struct EtMode {
  std::string mode;   // fixed tab csv tex booktabs
  std::string mode0;  // fixed tab csv tex
  std::vector<std::string> open, close, caption, toprule, midrule, bottomrule, topgap, midgap,
      bottomgap, eqruleBegin;
  bool hasEqrule = false;
  std::vector<std::string> ssl, lsl;
  std::vector<std::pair<std::string, std::string>> starlevels;
  std::string starlevlab;  // the label() of starlevels, or empty
  bool starlevlabSet = false;
  std::string begin, delimiter, end;
  bool beginSet = false, delimiterSet = false, endSet = false;
  int varwidth = -1, modelwidth = -1;  // -1 = none
  bool abbrev = false;
  std::vector<std::pair<std::string, std::string>> substitute;
  std::string interaction = " # ";
  std::string tstatlab = "t statistics", zstatlab = "z statistics", pvallab = "p-values", cilab;
};

EtMode esttabMode(const std::string& mode, bool label, bool plain, int level) {
  EtMode m;
  m.mode = mode;
  m.mode0 = (mode == "booktabs") ? "tex" : (mode == "scsv" ? "csv" : mode);
  const std::vector<std::string> fssl{"N", "R-sq", "adj. R-sq", "pseudo R-sq", "AIC", "BIC"};
  const std::vector<std::string> flsl{"Observations",       "R-squared", "Adjusted R-squared",
                                      "Pseudo R-squared", "AIC",       "BIC"};
  const std::vector<std::pair<std::string, std::string>> fstars{
      {"*", "0.05"}, {"**", "0.01"}, {"***", "0.001"}};
  m.cilab = std::to_string(level) + "% confidence intervals";
  if (m.mode0 == "fixed") {
    m.caption = {"@title"};
    m.toprule = m.midrule = m.bottomrule = {"@hline"};
    m.topgap = m.midgap = m.bottomgap = {""};
    m.eqruleBegin = {"@hline", ""};
    m.hasEqrule = true;
    m.ssl = fssl;
    m.lsl = flsl;
    m.starlevels = fstars;
    m.delimiter = " ";
    m.delimiterSet = true;
    m.varwidth = label ? 20 : 12;
    m.modelwidth = 12;
    m.abbrev = true;
  } else if (m.mode0 == "tab") {
    m.caption = {"@title"};
    m.topgap = m.midgap = m.bottomgap = {""};
    m.ssl = fssl;
    m.lsl = flsl;
    m.starlevels = fstars;
    m.delimiter = "\t";
    m.delimiterSet = true;
  } else if (m.mode0 == "csv") {
    const std::string csvlhs = plain ? "\"" : "=\"";
    m.caption = {csvlhs + "@title\""};
    m.topgap = m.midgap = m.bottomgap = {""};
    m.ssl = fssl;
    m.lsl = flsl;
    m.starlevels = fstars;
    m.begin = csvlhs;
    m.beginSet = true;
    m.delimiter = std::string("\"") + (mode == "scsv" ? ";" : ",") + csvlhs;
    m.delimiterSet = true;
    m.end = "\"";
    m.endSet = true;
  } else {  // tex, booktabs
    const bool bt = mode == "booktabs";
    m.open = {"\\begin{table}[htbp]\\centering"};
    m.close = {"\\end{table}"};
    m.caption = {"\\caption{@title}"};
    m.toprule = {bt ? "\\toprule" : "\\hline\\hline"};
    m.midrule = {bt ? "\\midrule" : "\\hline"};
    m.bottomrule = {bt ? "\\bottomrule" : "\\hline\\hline"};
    m.midgap = {bt ? "\\addlinespace" : "[1em]"};
    m.eqruleBegin = {bt ? "\\midrule" : "\\hline", ""};
    m.hasEqrule = true;
    m.ssl = {"\\(N\\)", "\\(R^{2}\\)", "adj. \\(R^{2}\\)", "pseudo \\(R^{2}\\)", "\\textit{AIC}",
             "\\textit{BIC}"};
    m.lsl = {"Observations",        "\\(R^{2}\\)",   "Adjusted \\(R^{2}\\)",
             "Pseudo \\(R^{2}\\)", "\\textit{AIC}", "\\textit{BIC}"};
    m.starlevels = {{"\\sym{*}", "0.05"}, {"\\sym{**}", "0.01"}, {"\\sym{***}", "0.001"}};
    m.starlevlab = " \\(p<@\\)";
    m.starlevlabSet = true;
    m.delimiter = "&";
    m.delimiterSet = true;
    m.end = "\\\\";
    m.endSet = true;
    m.varwidth = label ? 20 : 12;
    m.modelwidth = 12;
    m.tstatlab = "\\textit{t} statistics";
    m.zstatlab = "\\textit{z} statistics";
    m.pvallab = "\\textit{p}-values";
    m.cilab = std::to_string(level) + "\\% confidence intervals";
    m.substitute = {{"_", "\\_"}, {"\\_cons ", "\\_cons"}};
    m.interaction = " $\\times$ ";
  }
  return m;
}

// `star(* 0.10 ** 0.05)` -> pairs; false when the list is not symbol/level pairs.
bool eoParseStarPairs(const std::string& raw, std::vector<std::pair<std::string, std::string>>* out,
                      std::string* labelOut, std::string* delimOut, EoError* err) {
  std::string lhs, rhs;
  eoSplitComma(raw, &lhs, &rhs);
  const std::vector<std::string> t = eoTokens(lhs);
  if (t.size() % 2 != 0) {
    err->msg = "unmatched list of significance symbols and levels";
    err->rc = 198;
    return false;
  }
  double prev = 1.0;
  for (size_t i = 0; i + 1 < t.size(); i += 2) {
    double lv = 0.0;
    try {
      size_t used = 0;
      lv = std::stod(t[i + 1], &used);
      if (used != t[i + 1].size()) throw std::invalid_argument("x");
    } catch (...) {
      err->msg = "'" + t[i + 1] + "' found where number expected";
      err->rc = 7;
      return false;
    }
    if (lv > prev || lv <= 0.0) {
      err->msg = "significance levels out of order or out of range (0,1]";
      err->rc = 198;
      return false;
    }
    prev = lv;
    out->emplace_back(t[i], t[i + 1]);
  }
  for (const auto& [nm, arg] : eoSubopts(rhs)) {
    if (eoAbbr(nm, "Label") && labelOut != nullptr) *labelOut = eoStringArg(arg);
    if (eoAbbr(nm, "Delimiter") && delimOut != nullptr) *delimOut = eoStringArg(arg);
  }
  return true;
}

std::vector<int> eoIntList(const std::string& raw) {
  std::vector<int> v;
  for (const auto& t : eoTokens(raw)) {
    try {
      v.push_back(std::stoi(t));
    } catch (...) {
    }
  }
  return v;
}

}  // namespace

namespace {

// estout's stats(keys, fmt() labels() star) (estout ParseStatsSubopts,
// ProcessStatslayout): per-key formats run forward, the default being the
// first cell element's first format.
bool eoParseStats(const std::string& raw, const std::string& defaultFmt, EoSpec* sp,
                  EoError* err) {
  std::string lhs, rhs;
  eoSplitComma(raw, &lhs, &rhs);
  std::vector<std::string> keys;
  for (const auto& k : eoTokens(lhs)) {
    if (std::find(keys.begin(), keys.end(), k) == keys.end()) keys.push_back(k);
  }
  std::vector<std::string> fmts;
  std::set<std::string> starred;
  bool nostar = false;
  for (const auto& [nm, arg] : eoSubopts(rhs)) {
    if (eoAbbr(nm, "Fmt")) {
      fmts = eoTokens(arg.value_or(""));
    } else if (eoAbbr(nm, "Labels")) {
      if (!eoParseLabelOpt(arg.value_or(""), &sp->statslabels, &err->msg)) {
        err->rc = 198;
        return false;
      }
    } else if (nm == "star") {
      if (arg.has_value()) {
        for (const auto& s : eoTokens(*arg)) {
          starred.insert(s);
          if (std::find(keys.begin(), keys.end(), s) == keys.end()) keys.push_back(s);
        }
      } else if (!keys.empty()) {
        starred.insert(keys.front());
      }
    } else if (eoAbbr(nm, "NOStar")) {
      nostar = true;
    } else if (eoAbbr(nm, "LAYout") || eoAbbr(nm, "PChar")) {
      err->msg = "estout: stats(..., " + nm + "()) is not implemented in this build";
      err->rc = 198;
      return false;
    } else {
      err->msg = "option " + nm + " not allowed";
      err->rc = 198;
      return false;
    }
  }
  if (fmts.empty()) fmts.push_back(defaultFmt);
  std::string format = "%9.0g";
  for (size_t i = 0; i < keys.size(); ++i) {
    if (i < fmts.size() && !fmts[i].empty()) format = fmts[i];
    EoStatRow r;
    r.key = keys[i];
    r.fmt = format;
    r.star = !nostar && starred.count(keys[i]) != 0;
    sp->stats.push_back(r);
  }
  return true;
}

}  // namespace

namespace {

// esttab, beta: b * sd(x)/sd(y) over e(sample) (estadd, called by
// esttab). PIET's store carries no e(sample) mask, so the sample is
// RECONSTRUCTED as the complete cases of {depvar, regressors} and CHECKED
// against e(N): a mismatch refuses by name rather than printing a number from
// the wrong rows (internal notes esttab-beta).
bool esttabBetaVals(SessionState& session, const std::vector<Model>& models,
                    std::vector<std::map<std::string, double>>* out, std::string* errOut) {
  out->assign(models.size(), {});
  if (!session.currentHbin().has_value()) {
    *errOut = "esttab, beta needs sd(x)/sd(y) over e(sample) and no dataset is in memory";
    return false;
  }
  for (size_t mi = 0; mi < models.size(); ++mi) {
    const Model& m = models[mi];
    const std::string depvar = m.snap.value("depvar", std::string());
    double nObs = 0.0;
    if (m.snap.contains("n_obs") && m.snap["n_obs"].is_number()) nObs = m.snap["n_obs"].get<double>();
    if (depvar.empty() || nObs <= 1.0) {
      *errOut = "esttab, beta needs e(sample) and model " + m.name +
                " carries no dependent variable or no observation count";
      return false;
    }
    std::vector<std::string> vars{depvar};
    std::vector<std::string> coefs;
    if (m.snap.contains("coeftable") && m.snap["coeftable"].is_array()) {
      for (const auto& r : m.snap["coeftable"]) {
        const std::string n = r.value("name", std::string());
        if (n == "_cons" || n == "(intercept)") continue;
        vars.push_back(n);
        coefs.push_back(n);
      }
    }
    std::string ferr;
    try {
      Datastore& ds = *session.datastore();
      std::map<std::string, const double*> cols;
      int64_t nrows = -1;
      for (const auto& v : vars) {
        if (!ds.has(v) || ds.col(v).dtype == cecon::DType::Factor) {
          ferr = "sd(" + v + ") is not available: " + v +
                 " is not a numeric variable in the dataset now in memory";
          break;
        }
        const cecon::Column& c = ds.col(v);
        const int64_t n = c.nbytes / static_cast<int64_t>(sizeof(double));
        if (nrows < 0) nrows = n;
        if (n != nrows) {
          ferr = "the dataset columns disagree on length";
          break;
        }
        cols[v] = c.f64();
      }
      if (ferr.empty()) {
        std::vector<size_t> rows;
        for (int64_t i = 0; i < nrows; ++i) {
          bool ok = true;
          for (const auto& kv : cols) {
            if (std::isnan(kv.second[i])) ok = false;
          }
          if (ok) rows.push_back(static_cast<size_t>(i));
        }
        if (static_cast<double>(rows.size()) != nObs) {
          ferr = "this build reconstructs e(sample) as the complete cases of the model's own "
                 "variables, and for " + m.name + " that is " + std::to_string(rows.size()) +
                 " rows against e(N)=" + std::to_string(static_cast<long long>(nObs)) +
                 " -- the sample cannot be reproduced, so no standardized coefficient is printed";
        } else {
          auto sdOf = [&](const std::string& v) {
            const double* p = cols[v];
            double s = 0.0;
            for (size_t i : rows) s += p[i];
            const double mean = s / static_cast<double>(rows.size());
            double m2 = 0.0;
            for (size_t i : rows) {
              const double d = p[i] - mean;
              m2 += d * d;
            }
            return std::sqrt(m2 / (static_cast<double>(rows.size()) - 1.0));
          };
          const double sdy = sdOf(depvar);
          for (const auto& r : m.snap["coeftable"]) {
            const std::string cn = r.value("name", std::string());
            if (cols.count(cn) == 0 || !r.contains("b") || !r["b"].is_number()) continue;
            (*out)[mi][cn] = r["b"].get<double>() * sdOf(cn) / sdy;
          }
        }
      }
    } catch (const std::exception& e) {
      ferr = std::string("the dataset could not be read (") + e.what() + ")";
    }
    if (!ferr.empty()) {
      *errOut = "esttab, beta: " + ferr;
      return false;
    }
  }
  return true;
}

// estout posts r() (estout): r(cmdline), r(names),
// r(nmodels), r(ccols) -- missing when no coefficient statistic resolved.
void eoPostR(SessionState& session, const std::string& cmdline, const std::vector<EoModel>& models,
             int ccols) {
  eval::replaceRClass(session.eval);
  std::string names;
  for (const auto& m : models) {
    if (!names.empty()) names += " ";
    names += m.name;
  }
  session.eval.rStrings.set("cmdline", cmdline);
  session.eval.rStrings.set("names", names);
  session.eval.rValues.set("nmodels", static_cast<double>(models.size()));
  session.eval.rValues.set("ccols", ccols < 0 ? kEoNaN : static_cast<double>(ccols));
}

}  // namespace

namespace {
ojson esttabImpl(const ojson& esttabBlock, const std::string& path,
                 const std::vector<std::pair<std::string, eval::OptValue>>& opts,
                 SessionState& session, bool toFile);
}  // namespace

ojson writeEsttabFile(const ojson& esttabBlock, const std::string& path,
                      const std::vector<std::pair<std::string, eval::OptValue>>& opts,
                      SessionState& session) {
  return esttabImpl(esttabBlock, path, opts, session, /*toFile=*/true);
}

ojson esttabScreen(const ojson& esttabBlock,
                   const std::vector<std::pair<std::string, eval::OptValue>>& opts,
                   SessionState& session) {
  return esttabImpl(esttabBlock, std::string(), opts, session, /*toFile=*/false);
}

namespace {
ojson esttabImpl(const ojson& esttabBlock, const std::string& path,
                 const std::vector<std::pair<std::string, eval::OptValue>>& opts,
                 SessionState& session, bool toFile) {
  if (!esttabBlock.is_object() || !esttabBlock.contains("coef_rows")) {
    return tblError("esttab: nothing to write");
  }
  std::string userPath = dequote(path);
  auto has = [&](const char* k) { return hasFlag(opts, k); };
  auto str = [&](const char* k) { return optStr(opts, k); };
  auto eoFail = [](const EoError& e) { return tblErrorRc(e.msg, e.rc); };

  // ---- models (unchanged resolution: runEsttab already expanded the namelist) --
  std::vector<std::string> names;
  if (esttabBlock.contains("names") && esttabBlock["names"].is_array()) {
    for (const auto& n : esttabBlock["names"]) {
      if (n.is_string()) names.push_back(n.get<std::string>());
    }
  }
  // `runEsttab` falls back to the ACTIVE estimate when the caller named no
  // models and the store is empty; the label it reports for that is not a
  // stored name, so the active estimate is resolved directly here.
  // esttab MatrixMode: `esttab matrix(A, fmt())` tabulates a
  // matrix instead of estimates (the dispatcher hands the spec over).
  const bool matrixmode = esttabBlock.contains("matrix") && esttabBlock["matrix"].is_string();
  std::vector<Model> models;
  bool allStored = !names.empty();
  for (const auto& n : names) {
    if (n != "." && session.estimatesGet(n) == nullptr) allStored = false;
  }
  if (matrixmode) {
    // no estimates
  } else if (allStored) {
    ojson err;
    models = collectModels(session, "esttab", names, &err);
    if (models.empty()) return err;
  } else {
    Model m;
    ojson err;
    if (!activeModel(session, "esttab", &m, &err)) return err;
    if (!names.empty()) m.name = names[0];
    models.push_back(m);
  }

  // ---- format modes (esttab) --------------------------------------------
  std::vector<std::string> modes;
  for (const char* md : {"smcl", "fixed", "tab", "csv", "scsv", "rtf", "html", "html5", "tex",
                         "booktabs", "md", "mmd"}) {
    if (has(md)) modes.push_back(md);
  }
  if (modes.size() > 1) {
    return tblErrorRc("only one allowed of smcl, fixed, tab, csv, scsv, rtf, html, html5, tex, "
                      "booktabs, md, or mmd",
                      198);
  }
  std::string suffix;
  {
    // _getfilesuffix: the text after the last `.` of the file name, kept as typed.
    const size_t slash = userPath.find_last_of("/\\");
    const size_t dot = userPath.find_last_of('.');
    if (dot != std::string::npos && (slash == std::string::npos || dot > slash)) {
      suffix = userPath.substr(dot);
    }
  }
  std::string mode = modes.empty() ? std::string() : modes[0];
  if (!toFile) {
    // The screen form (esttab `else local mode smcl`): smcl's widths,
    // delimiter and abbreviation are fixed mode's; the text itself is
    // the UI block's, so only the checks and r() matter here.
    if (mode.empty() || mode == "smcl" || mode == "rtf" || mode == "html" || mode == "html5" ||
        mode == "md" || mode == "mmd") {
      mode = "fixed";
    }
  } else if (mode.empty()) {
    if (suffix == ".html" || suffix == ".htm") mode = "html";
    else if (suffix == ".tex") mode = "tex";
    else if (suffix == ".csv") mode = "csv";
    else if (suffix == ".rtf") mode = "rtf";
    else if (suffix == ".smcl") mode = "smcl";
    else if (suffix == ".md") mode = "md";
    else if (suffix == ".mmd") mode = "mmd";
    else mode = "fixed";
  }
  if (toFile && suffix.empty()) {
    // esttab: a `using` name with no suffix gets the mode's.
    if (mode == "fixed" || mode == "tab") suffix = ".txt";
    else if (mode == "csv" || mode == "scsv") suffix = ".csv";
    else if (mode == "rtf") suffix = ".rtf";
    else if (mode == "html" || mode == "html5") suffix = ".html";
    else if (mode == "tex" || mode == "booktabs") suffix = ".tex";
    else if (mode == "smcl") suffix = ".smcl";
    else if (mode == "md") suffix = ".md";
    else if (mode == "mmd") suffix = ".mmd";
    userPath += suffix;
  }
  if (mode == "smcl" || mode == "rtf" || mode == "html" || mode == "html5" || mode == "md" ||
      mode == "mmd") {
    return ojson(eval::unsupportedOption(
        mode, "esttab: the " + mode + " writer is not implemented in this build; fixed, tab, csv, "
              "scsv, tex and booktabs are"));
  }

  // ---- option locals (esttab) ------------------------------------------------
  const bool plain = has("plain");
  const bool label = has("label");
  const bool fragment = has("fragment");
  const bool wide = has("wide");
  const bool noT = has("not");
  const bool zOn = has("z") || str("z").has_value();
  const bool seOn = has("se") || str("se").has_value();
  const bool pOn = has("p") || str("p").has_value();
  const bool ciOn = has("ci") || str("ci").has_value();
  const bool betaOn = has("beta") || str("beta").has_value();
  const bool staraux = has("staraux");
  const bool brackets = has("brackets");
  const bool cellsGiven = str("cells").has_value();
  const bool nocellsopt = !cellsGiven;
  const bool stardetach = has("stardetach");
  int level = 95;
  if (auto lv = str("level")) {
    try {
      level = std::stoi(dequote(*lv));
    } catch (...) {
      return tblErrorRc("level(" + *lv + ") invalid", 198);
    }
    if (level < 10 || level > 99) return tblErrorRc("level(" + *lv + ") invalid", 198);
    if (level != 95) {
      // The interval is READ from the estimator (it carries its own level), so
      // another level cannot be honoured; refused rather than mislabelled.
      return ojson(eval::unsupportedOption(
          "level", "esttab reports the confidence interval the estimation stored (95% by "
                   "default); this build cannot re-derive it at level(" + dequote(*lv) + ")"));
    }
  }
  std::optional<std::string> bfmtOpt = str("b");
  std::string bfmt = bfmtOpt.has_value() ? eoTrim(dequote(*bfmtOpt)) : std::string();
  std::string tfmt = str("t").has_value() ? eoTrim(dequote(*str("t"))) : std::string();
  std::string zfmt = str("z").has_value() ? eoTrim(dequote(*str("z"))) : std::string();
  std::string sefmt = str("se").has_value() ? eoTrim(dequote(*str("se"))) : std::string();
  std::string pfmt = str("p").has_value() ? eoTrim(dequote(*str("p"))) : std::string();
  std::string cifmt = str("ci").has_value() ? eoTrim(dequote(*str("ci"))) : std::string();
  std::string betafmt = str("beta").has_value() ? eoTrim(dequote(*str("beta"))) : std::string();
  const bool r2On = has("r2") || str("r2").has_value();
  const bool ar2On = has("ar2") || str("ar2").has_value();
  const bool pr2On = has("pr2") || str("pr2").has_value();
  const bool aicOn = has("aic") || str("aic").has_value();
  const bool bicOn = has("bic") || str("bic").has_value();
  std::string r2fmt = str("r2").value_or(""), ar2fmt = str("ar2").value_or(""),
              pr2fmt = str("pr2").value_or(""), aicfmt = str("aic").value_or(""),
              bicfmt = str("bic").value_or("");
  bool star = has("star") || str("star").has_value();
  if (star && has("nostar")) return tblErrorRc("options star and nostar not both allowed", 198);
  const bool noconstant = (has("noconstant") || has("nocons") ||
                           (betaOn && !has("constant")));

  EtMode md = esttabMode(mode, label, plain, level);
  const std::string mode0 = md.mode0;
  const std::string csvlhs = plain ? "\"" : "=\"";

  // widths (esttab)
  int modeVarwidth = md.varwidth, modeModelwidth = md.modelwidth;
  if (has("compress")) {
    if (modeModelwidth >= 0) modeModelwidth -= 3;
    if (modeVarwidth >= 0) modeVarwidth -= label ? 4 : 2;
  }
  EoSpec sp;
  sp.session = &session;
  sp.level = level;
  if (auto mwo = str("modelwidth")) {
    sp.modelwidth = eoIntList(*mwo);
    if (sp.modelwidth.empty()) sp.modelwidth = {0};
  } else if (nocellsopt && modeModelwidth >= 0 && ciOn) {
    if (wide) sp.modelwidth = {modeModelwidth, 2 * modeModelwidth - 2};
    else sp.modelwidth = {2 * modeModelwidth - 2};
  } else {
    sp.modelwidth = {modeModelwidth >= 0 ? modeModelwidth : 0};
  }
  if (auto vwo = str("varwidth")) {
    const std::vector<int> v = eoIntList(*vwo);
    sp.varwidth = v.empty() ? 0 : v[0];
  } else {
    sp.varwidth = modeVarwidth >= 0 ? modeVarwidth : 0;
  }

  // switches (esttab)
  bool depvars = has("depvars"), numbers = has("numbers"), parentheses = has("parentheses"),
       notes = has("notes"), gaps = has("gaps"), lines = has("lines"), eqlines = has("eqlines");
  if (!plain && !matrixmode) {
    if (!has("nostar")) star = true;
    if (!has("nodepvars")) depvars = true;
    if (!has("nonumbers")) numbers = true;
    if (!has("noparentheses")) parentheses = true;
    if (!has("nonotes")) notes = true;
    if (!wide && (!noT || zOn || seOn || pOn || ciOn || str("aux").has_value()) && nocellsopt &&
        !has("nogaps")) {
      gaps = true;
    }
  }
  if (!plain && !has("nolines")) lines = true;
  if (lines && !has("noeqlines")) eqlines = true;
  if (mode0 == "tab" || mode0 == "csv") {
    lines = false;
    eqlines = false;
  }
  const bool legend = notes && !has("nolegend") && nocellsopt && !matrixmode;
  if (plain) {
    if (bfmt.empty()) bfmt = "%9.0g";
    if (tfmt.empty()) tfmt = bfmt;
    if (zfmt.empty()) zfmt = bfmt;
    if (sefmt.empty()) sefmt = bfmt;
    if (pfmt.empty()) pfmt = bfmt;
    if (cifmt.empty()) cifmt = bfmt;
    if (betafmt.empty()) betafmt = bfmt;
  }

  // ---- cells (esttab) ----------------------------------------------------------
  std::vector<std::string> thenote;
  if (bfmt.empty()) bfmt = "a3";
  EoError eerr;
  if (matrixmode) {
    std::string merr;
    if (!eoMatrixModel(session, esttabBlock["matrix"].get<std::string>(), &sp, &merr)) {
      return tblErrorRc(merr, 111);
    }
  } else if (cellsGiven) {
    std::string cerr;
    if (!eoParseCells(*str("cells"), &sp.cells, &cerr)) return tblErrorRc(cerr, 198);
    // esttab hands cells() to estout unchanged, and estout gives
    // every fmt-less element the FIRST element's format (default %9.0g):
    // w2ak_estout_family.log ek_e20_cells_L7 `[0.500]` for qval(par([ ])) after
    // b(star fmt(3)).
    if (!sp.cells.elems.empty()) {
      if (sp.cells.elems[0].fmt.empty()) sp.cells.elems[0].fmt = {"%9.0g"};
      for (auto& el : sp.cells.elems) {
        if (el.fmt.empty()) el.fmt = sp.cells.elems[0].fmt;
      }
    }
  } else {
    std::string note1;
    auto addElem = [&](EoElem el, int row, bool sideBySide) {
      sp.cells.elems.push_back(el);
      const int idx = static_cast<int>(sp.cells.elems.size()) - 1;
      if (sideBySide && !sp.cells.rows.empty()) {
        sp.cells.rows.back().push_back({idx});
      } else {
        sp.cells.rows.push_back({{idx}});
      }
      (void)row;
      sp.cells.ncols = 0;
      for (const auto& r : sp.cells.rows) sp.cells.ncols = std::max(sp.cells.ncols, r.size());
    };
    EoElem mainEl;
    const bool bstar = star && !staraux;
    if (betaOn) {
      if (str("main").has_value()) return tblErrorRc("beta() and main() not allowed both", 198);
      if (betafmt.empty()) betafmt = "3";
      mainEl.name = "beta";
      mainEl.fmt = {betafmt};
      mainEl.star = bstar;
      if (notes) note1 = "Standardized beta coefficients";
    } else if (auto mo = str("main")) {
      const std::vector<std::string> t = eoTokens(*mo);
      mainEl.name = t.empty() ? std::string("b") : t[0];
      mainEl.fmt = {t.size() > 1 ? t[1] : bfmt};
      mainEl.star = bstar;
      if (notes) note1 = mainEl.name + " coefficients";
    } else {
      mainEl.name = "b";
      mainEl.fmt = {bfmt};
      mainEl.star = bstar;
    }
    addElem(mainEl, 0, false);
    if (!noT || zOn || seOn || pOn || ciOn || str("aux").has_value()) {
      if (has("onecell")) {
        return ojson(eval::unsupportedOption("onecell", "esttab: onecell is not implemented in this build"));
      }
      std::string auxname, auxfmt;
      if (auto ao = str("aux")) {
        const std::vector<std::string> t = eoTokens(*ao);
        if (!t.empty()) auxname = t[0];
        if (t.size() > 1) auxfmt = t[1];
      }
      std::vector<std::string> auxl;
      if (zOn) auxl.push_back("z");
      if (seOn) auxl.push_back("se");
      if (pOn) auxl.push_back("p");
      if (ciOn) auxl.push_back("ci");
      if (!auxname.empty()) auxl.push_back(auxname);
      if (auxl.size() > 1) return tblErrorRc("only one allowed of z, se, p, ci, and aux()", 198);
      const std::string aux = auxl.empty() ? std::string("t") : auxl[0];
      bool abs = has("abs");
      if (aux != "t" && aux != "z") abs = false;
      EoElem auxEl;
      auxEl.name = aux;
      bool bracketsOn = brackets;
      if (parentheses || brackets) {
        auxEl.hasPar = true;
        if (aux == "ci") {
          bracketsOn = true;
          auxEl.par = {"[", ",", "]"};
        } else if (brackets) {
          auxEl.par = {"[", "]"};
        } else {
          auxEl.par = {"(", ")"};
        }
      }
      if (notes) {
        std::string tn = note1;
        if (!tn.empty()) tn += "; ";
        if (!auxname.empty()) {
          tn += auxname;
        } else if (aux == "t" || aux == "z") {
          if (abs) tn += "Absolute ";
          tn += (aux == "t") ? md.tstatlab : md.zstatlab;
        } else if (aux == "se") {
          tn += "Standard errors";
        } else if (aux == "p") {
          tn += md.pvallab;
        } else if (aux == "ci") {
          tn += md.cilab;
        }
        if (!parentheses) tn += wide ? " in second column" : " in second row";
        else if (bracketsOn) tn += " in brackets";
        else tn += " in parentheses";
        note1 = tn;
      }
      if (tfmt.empty()) tfmt = "2";
      if (zfmt.empty()) zfmt = "2";
      if (sefmt.empty()) sefmt = bfmt;
      if (pfmt.empty()) pfmt = "3";
      if (cifmt.empty()) cifmt = bfmt;
      if (auxfmt.empty()) auxfmt = bfmt;
      if (auxname.empty()) {
        auxfmt = aux == "t" ? tfmt : aux == "z" ? zfmt : aux == "se" ? sefmt : aux == "p" ? pfmt : cifmt;
      }
      auxEl.fmt = {auxfmt};
      auxEl.abs = abs;
      auxEl.star = staraux && star;
      addElem(auxEl, 1, wide);
    }
    if (!note1.empty()) thenote.push_back(note1);
  }

  // ---- stats (esttab) -------------------------------------------------------------
  std::string sfmt = str("sfmt").value_or("");
  std::optional<std::string> statsOpt = str("stats");
  if (!statsOpt.has_value()) statsOpt = str("s");  // estout `Stats()`: `s` is its minimum
  if (matrixmode) {
    // esttab builds no stats() in matrix mode
  } else if (auto st = statsOpt) {
    const std::string firstFmt =
        (!sp.cells.elems.empty() && !sp.cells.elems[0].fmt.empty()) ? sp.cells.elems[0].fmt[0] : "%9.0g";
    if (!eoParseStats(*st, firstFmt, &sp, &eerr)) return eoFail(eerr);
  } else {
    if (sfmt.empty()) sfmt = bfmt;
    if (r2fmt.empty()) r2fmt = plain ? bfmt : "3";
    if (ar2fmt.empty()) ar2fmt = plain ? bfmt : "3";
    if (pr2fmt.empty()) pr2fmt = plain ? bfmt : "3";
    if (aicfmt.empty()) aicfmt = bfmt;
    if (bicfmt.empty()) bicfmt = bfmt;
    std::vector<std::string> stalabs = label ? md.lsl : md.ssl;
    const std::string obslab = stalabs.empty() ? std::string("N") : stalabs[0];
    std::vector<std::string> sta, stalab, stafmt;
    const bool noobs = has("noobs");
    const bool obslast = has("obslast");
    if (!noobs && !obslast) {
      sta.push_back("N");
      stalab.push_back(obslab);
      stafmt.push_back("%18.0g");
    }
    const char* flagKeys[] = {"r2", "r2_a", "r2_p", "aic", "bic"};
    const bool flagOn[] = {r2On, ar2On, pr2On, aicOn, bicOn};
    const std::string flagFmt[] = {r2fmt, ar2fmt, pr2fmt, aicfmt, bicfmt};
    for (int i = 0; i < 5; ++i) {
      if (!flagOn[i]) continue;
      sta.push_back(flagKeys[i]);
      stalab.push_back(static_cast<size_t>(i + 1) < stalabs.size() ? stalabs[static_cast<size_t>(i) + 1] : std::string());
      stafmt.push_back(eoTrim(dequote(flagFmt[i])));
    }
    if (auto sc = str("scalars")) {
      const std::vector<std::string> sfl = eoTokens(sfmt);
      size_t i = 0;
      for (const auto& addstat : eoTokens(*sc)) {
        ++i;
        std::string name, lab;
        {
          const std::string t = eoTrim(addstat);
          const size_t sp1 = t.find_first_of(" \t");
          name = sp1 == std::string::npos ? t : t.substr(0, sp1);
          lab = sp1 == std::string::npos ? std::string() : t.substr(sp1 + 1);
        }
        if (std::find(sta.begin(), sta.end(), name) != sta.end()) continue;
        if (name == "N" && !noobs && obslast) continue;
        if (eoTrim(lab).empty()) lab = name;
        std::string f = (i <= sfl.size()) ? sfl[i - 1] : (sfl.empty() ? std::string() : sfl.back());
        sta.push_back(name);
        stalab.push_back(lab);
        stafmt.push_back(f);
      }
    }
    if (!noobs && obslast) {
      sta.push_back("N");
      stalab.push_back(obslab);
      stafmt.push_back("%18.0g");
    }
    for (size_t i = 0; i < sta.size(); ++i) {
      EoStatRow r;
      r.key = sta[i];
      r.fmt = stafmt[i].empty() ? std::string("%9.0g") : stafmt[i];
      sp.stats.push_back(r);
    }
    sp.statslabels.given = true;
    sp.statslabels.list = stalab;
  }

  // ---- table header (esttab) ---------------------------------------------------------
  std::string mspanPrefix, mspanSuffix;
  bool mspan = false;
  if (auto ml = str("mlabels")) {
    if (!eoParseLabelOpt(*ml, &sp.mlabels, &eerr.msg)) return tblErrorRc(eerr.msg, 198);
  } else {
    if (mode0 == "tex") {
      mspan = true;
      mspanPrefix = "\\multicolumn{@span}{c}{";
      mspanSuffix = "}";
    }
    auto setSpan = [&]() {
      sp.mlabels.span = mspan;
      sp.mlabels.prefix = mspanPrefix;
      sp.mlabels.suffix = mspanSuffix;
    };
    if (depvars) {
      sp.mlabels.given = true;
      sp.mlabels.depvars = true;
      setSpan();
    }
    if (has("nomtitles")) {
      sp.mlabels = EoLabelOpt{};
      sp.mlabels.given = true;
      sp.mlabels.none = true;
    }
    if (has("mtitles")) {
      sp.mlabels = EoLabelOpt{};
      sp.mlabels.given = true;
      sp.mlabels.titles = true;
      if (auto mt = str("mtitles")) sp.mlabels.list = eoTokens(*mt);
      setSpan();
    }
  }
  if (auto cl = str("collabels")) {
    if (!eoParseLabelOpt(*cl, &sp.collabels, &eerr.msg)) return tblErrorRc(eerr.msg, 198);
  } else if (nocellsopt && !plain && !matrixmode) {
    sp.collabels.given = true;
    sp.collabels.none = true;
  }
  sp.numbers = numbers && !has("nonumbers");
  if (mode0 == "tex" && sp.numbers) {
    sp.numL = "\\multicolumn{@span}{c}{(";
    sp.numR = ")}";
  }
  if (auto mg = str("mgroups")) {
    if (!eoParseLabelOpt(*mg, &sp.mgroups, &eerr.msg)) return tblErrorRc(eerr.msg, 198);
  }

  // ---- notes and the legend (esttab) ----------------------------------------------------
  sp.title = str("title").has_value() ? eoStringArg(str("title")) : std::string();
  if (auto nt = str("note")) {
    sp.note = eoStringArg(nt);
    thenote = {"@note"};
  }
  if (auto an = str("addnotes")) {
    if (an->find('"') == std::string::npos) {
      thenote.push_back(*an);
    } else {
      for (const auto& t : eoTokens(*an)) thenote.push_back(t);
    }
  }
  if (legend && (star || !nocellsopt)) thenote.push_back("@starlegend");

  // star levels and the mode strings (esttab)
  if (auto sl = str("starlevels")) {
    std::string lab = sp.starlevelsLabel, del = sp.starlevelsDelim;
    if (!eoParseStarPairs(*sl, &sp.starlevels, &lab, &del, &eerr)) return eoFail(eerr);
    sp.starlevelsLabel = lab;
    sp.starlevelsDelim = del;
  } else if (star) {
    std::vector<std::pair<std::string, std::string>> lv = md.starlevels;
    if (auto s2 = str("star")) {
      lv.clear();
      if (!eoParseStarPairs(*s2, &lv, nullptr, nullptr, &eerr)) return eoFail(eerr);
      if (mode0 == "tex") {
        for (auto& p : lv) p.first = "\\sym{" + p.first + "}";
      }
    }
    sp.starlevels = lv;
    if (md.starlevlabSet) sp.starlevelsLabel = md.starlevlab;
  }
  if (auto v = str("begin")) sp.begin = eoStringArg(v);
  else if (md.beginSet) sp.begin = md.begin;
  if (auto v = str("delimiter")) sp.delimiter = eoStringArg(v);
  else if (md.delimiterSet) sp.delimiter = md.delimiter;
  if (auto v = str("end")) sp.end = eoStringArg(v);
  else if (md.endSet) sp.end = md.end;
  if (auto v = str("substitute")) {
    const std::vector<std::string> t = eoTokens(*v);
    for (size_t i = 0; i < t.size(); i += 2) sp.substitute.emplace_back(t[i], i + 1 < t.size() ? t[i + 1] : "");
  } else {
    sp.substitute = md.substitute;
  }
  sp.interaction = str("interaction").has_value() ? eoStringArg(str("interaction")) : md.interaction;
  if (has("noabbrev")) sp.abbrev = false;
  else if (has("abbrev")) sp.abbrev = true;
  else sp.abbrev = md.abbrev;
  sp.wrap = has("wrap");
  if (auto ic = str("incelldelimiter")) sp.incelldelimiter = eoStringArg(ic);
  sp.lz = !has("nolz");
  if (auto hl = str("hlinechar")) sp.hlinechar = eoStringArg(hl);

  // estout's own style defaults on top (estout): `style(esttab)` is
  // estout's `tab`; a user style(tex)/style(fixed) fills what esttab left empty.
  {
    std::string style = str("style").has_value() ? strutil::lower(eoTrim(dequote(*str("style")))) : "esttab";
    if (style == "esttab") style = "tab";
    if (style == "tex") {
      if (!str("end").has_value() && !md.endSet) sp.end = "\\\\";
      if (!str("interaction").has_value() && md.interaction == " # " && false) sp.interaction = " $\\times$ ";
    } else if (style == "fixed") {
      if (!has("noabbrev")) sp.abbrev = true;
    } else if (style != "tab") {
      return ojson(eval::unsupportedOption(
          "style", "esttab: style(" + style + ") has no writer in this build; tab, fixed and tex do"));
    }
  }

  // ---- pre-/posthead, pre-/postfoot (esttab) ---------------------------------------------------
  std::vector<std::string> opening, closing, toprule, bottomrule, topgap, bottomgap;
  std::string open2, close2;
  if (!fragment) {
    if (mode0 == "tex") {
      const bool title = !sp.title.empty();
      const bool floatOn = (title || has("float")) && !has("nofloat");
      if (floatOn) opening.insert(opening.end(), md.open.begin(), md.open.end());
      else if (star) opening.push_back("{");
      if (star) opening.push_back("\\def\\sym#1{\\ifmmode^{#1}\\else\\(^{#1}\\)\\fi}");
      if (title) opening.insert(opening.end(), md.caption.begin(), md.caption.end());
      const std::string lstub = str("labcol2").has_value() ? "lc" : "l";
      const auto widthOpt = str("width");
      const std::string extracolsep =
          widthOpt.has_value() ? "@{\\hskip\\tabcolsep\\extracolsep\\fill}" : "";
      open2 = widthOpt.has_value() ? "\\begin{tabular*}{" + eoStringArg(widthOpt) + "}"
                                   : std::string("\\begin{tabular}");
      close2 = widthOpt.has_value() ? "\\end{tabular*}" : "\\end{tabular}";
      std::string colspec;
      std::optional<std::string> alignOpt = str("alignment");
      if (!alignOpt.has_value()) alignOpt = str("align");  // ALIGNment: `align` is its minimum
      if (matrixmode) {
        // MakeTeXColspecMat (esttab): `*{cols}{c}`, or the alignment
        // given, written without the `*{@E}{...}` wrapper.
        colspec.clear();
      } else if (auto al = alignOpt) {
        colspec = eoStringArg(al);
      } else if (nocellsopt) {
        // MakeTeXColspec (esttab)
        colspec = (star && stardetach && !staraux) ? "r@{}l" : "c";
        if (wide && !noT) colspec += (star && stardetach && staraux) ? "r@{}l" : "c";
      } else {
        // MakeTeXColspecAlt (esttab): one `c` per item of the widest row.
        size_t cnt = 1;
        for (const auto& r : sp.cells.rows) {
          size_t n = 0;
          for (const auto& c : r) n += std::max<size_t>(c.size(), 1);
          cnt = std::max(cnt, n);
        }
        colspec = std::string(cnt, 'c');
      }
      if (matrixmode) {
        const std::string mcs = alignOpt.has_value()
                                    ? eoStringArg(alignOpt)
                                    : "*{" + std::to_string(sp.cells.ncols) + "}{c}";
        opening.push_back(open2 + "{" + extracolsep + lstub + mcs + "}");
      } else {
        opening.push_back(open2 + "{" + extracolsep + lstub + "*{@E}{" + colspec + "}}");
      }
      // notes
      for (const auto& ch : thenote) closing.push_back("\\multicolumn{@span}{l}{\\footnotesize " + ch + "}\\\\");
      closing.push_back(close2);
      if (floatOn) closing.insert(closing.end(), md.close.begin(), md.close.end());
      else if (star) closing.push_back("}");
    } else {
      opening.insert(opening.end(), md.open.begin(), md.open.end());
      if (!sp.title.empty()) opening.insert(opening.end(), md.caption.begin(), md.caption.end());
      for (const auto& ch : thenote) {
        closing.push_back(mode0 == "csv" ? csvlhs + ch + "\"" : ch);
      }
      closing.insert(closing.end(), md.close.begin(), md.close.end());
    }
    toprule = md.toprule;
    bottomrule = md.bottomrule;
    topgap = md.topgap;
    bottomgap = md.bottomgap;
  }
  const std::vector<std::string> midrule = md.midrule, midgap = md.midgap;
  if (auto ph = str("prehead")) {
    sp.prehead = eoHookLines(*ph);
  } else {
    if (lines) opening.insert(opening.end(), toprule.begin(), toprule.end());
    else if (gaps) opening.insert(opening.end(), topgap.begin(), topgap.end());
    sp.prehead = opening;
  }
  if (auto ph = str("posthead")) {
    sp.posthead = eoHookLines(*ph);
  } else if (lines) {
    sp.posthead = midrule;
  } else if (gaps) {
    sp.posthead = midgap;
  }
  if (auto pf = str("prefoot")) {
    sp.prefoot = eoHookLines(*pf);
  } else if (!sp.stats.empty() && !sp.cells.none) {
    if (lines) sp.prefoot = midrule;
    else if (gaps) sp.prefoot = midgap;
  }
  if (auto pf = str("postfoot")) {
    sp.postfoot = eoHookLines(*pf);
  } else {
    std::vector<std::string> c2;
    if (lines) c2 = bottomrule;
    else if (gaps) c2 = bottomgap;
    c2.insert(c2.end(), closing.begin(), closing.end());
    sp.postfoot = c2;
  }

  // varlabels (esttab)
  if (auto vl = str("varlabels")) {
    if (!eoParseLabelOpt(*vl, &sp.varlabels, &eerr.msg)) return tblErrorRc(eerr.msg, 198);
  } else {
    std::vector<std::string> pairs;
    if (auto cl = str("coeflabels")) pairs = eoTokens(*cl);
    if (label) {
      pairs.push_back("_cons");
      pairs.push_back("Constant");
    }
    sp.varlabels.list = pairs;
    if (gaps) {
      sp.varlabels.end = {""};
      sp.varlabels.end.insert(sp.varlabels.end.end(), midgap.begin(), midgap.end());
      sp.varlabels.last = 0;
    }
    sp.varlabels.given = !pairs.empty() || gaps;
  }
  // eqlabels (esttab)
  if (auto el = str("eqlabels")) {
    if (!eoParseLabelOpt(*el, &sp.eqlabels, &eerr.msg)) return tblErrorRc(eerr.msg, 198);
  }
  if ((eqlines || gaps) && !has("unstack") && !sp.eqlabels.none) {
    const bool ok = sp.eqlabels.begin.empty() && !sp.eqlabels.replace && sp.eqlabels.first < 0;
    if (ok) {
      if (eqlines && md.hasEqrule) {
        sp.eqlabels.begin = md.eqruleBegin;
      } else if (gaps) {
        sp.eqlabels.begin = midgap;
        sp.eqlabels.begin.push_back("");
      }
      sp.eqlabels.first = 0;
    }
  }
  sp.label = label && !has("nolabel");
  sp.unstack = has("unstack");
  sp.omitted = !has("noomitted") && !has("noomit");
  sp.baselevels = !has("nobaselevels") && !has("nobase");

  // coefficient selection (esttab for noconstant)
  sp.keep = str("keep");
  sp.order = str("order");
  if (auto d = str("drop")) {
    sp.drop = d;
  } else if (noconstant) {
    sp.drop = std::string("_cons");
    sp.dropRelax = true;
  }
  if (auto rc = str("refcat")) {
    std::string lhs, rhs;
    eoSplitComma(*rc, &lhs, &rhs);
    const std::vector<std::string> t = eoTokens(lhs);
    for (size_t i = 0; i + 1 < t.size(); i += 2) sp.refcat.emplace_back(t[i], t[i + 1]);
    for (const auto& [nm, arg] : eoSubopts(rhs)) {
      if (eoAbbr(nm, "NOLabel")) sp.refcatLabel.clear();
      else if (eoAbbr(nm, "Label")) sp.refcatLabel = eoStringArg(arg);
      else if (eoAbbr(nm, "Below")) sp.refcatBelow = true;
    }
  }
  if (auto ind = str("indicate")) {
    std::string lhs, rhs;
    eoSplitComma(*ind, &lhs, &rhs);
    for (const auto& g : eoTokens(lhs)) {
      EoIndicate ig;
      const size_t eq = g.find('=');
      if (eq == std::string::npos) {
        ig.vars = g;
      } else {
        ig.name = eoTrim(g.substr(0, eq));
        ig.vars = g.substr(eq + 1);
      }
      sp.indicate.push_back(ig);
    }
    for (const auto& [nm, arg] : eoSubopts(rhs)) {
      if (eoAbbr(nm, "Labels")) {
        const std::vector<std::string> t = eoTokens(arg.value_or(""));
        if (!t.empty()) sp.indicateYes = t[0];
        if (t.size() > 1) sp.indicateNo = t[1];
      }
    }
  }
  if (auto lc = str("labcol2")) {
    std::string lhs, rhs;
    eoSplitComma(*lc, &lhs, &rhs);
    sp.labcol2 = true;
    sp.labcol2Vals = eoTokens(lhs);
    for (const auto& [nm, arg] : eoSubopts(rhs)) {
      if (eoAbbr(nm, "Title")) {
        const std::string a = arg.value_or("");
        sp.labcol2Title = a.find('"') == std::string::npos ? std::vector<std::string>{a} : eoTokens(a);
      } else if (eoAbbr(nm, "Width")) {
        const std::vector<int> v = eoIntList(arg.value_or(""));
        if (!v.empty()) sp.labcol2Width = v[0];
      }
    }
  }
  if (auto xc = str("extracols")) sp.extracols = eoIntList(*xc);
  if (auto rn = str("rename")) {
    std::string rl = *rn;
    std::replace(rl.begin(), rl.end(), ',', ' ');
    const std::vector<std::string> t = eoTokens(rl);
    for (size_t i = 0; i + 1 < t.size(); i += 2) {
      if (t[i + 1].find(':') != std::string::npos || t[i + 1].empty()) {
        return tblErrorRc("invalid rename()", 198);
      }
      sp.rename.emplace_back(t[i], t[i + 1]);
    }
  }
  if (str("equations").has_value()) {
    return ojson(eval::unsupportedOption("equations", "esttab: equations() is not implemented in this build"));
  }

  // ---- the models' rows --------------------------------------------------------------------------------------
  TransformSpec trSpec;
  if (auto tr = str("transform")) {
    std::string trErr;
    if (!parseTransformOpt(*tr, &trSpec, &trErr)) return tblSyntax("esttab: " + trErr);
  }
  std::vector<std::map<std::string, double>> betas;
  if (betaOn) {
    std::string berr;
    if (!esttabBetaVals(session, models, &betas, &berr)) {
      return ojson(eval::unsupportedOption("beta", berr));
    }
  }
  if (sp.label) session.syncEvalVarLabels();
  for (size_t mi = 0; mi < models.size() && !matrixmode; ++mi) {
    EoModel em = eoModelFrom(models[mi], models[mi].name, session, trSpec.empty() ? nullptr : &trSpec,
                             mi, !sp.omitted, sp.baselevels);
    if (betaOn) em.beta = betas[mi];
    sp.models.push_back(em);
  }
  if (!matrixmode) {
    for (auto& em : sp.models) em.model = nullptr;
    for (size_t mi = 0; mi < sp.models.size(); ++mi) sp.models[mi].model = &models[mi];
  }

  std::string text;
  int ccols = -1;
  if (!eoRender(sp, &text, &eerr, &ccols)) return eoFail(eerr);
  if (!toFile) {
    eoPostR(session, "estout", sp.models, ccols);
    session.eval.rClassCmd = "esttab";
    // BUG-407: a cells()/main()/aux() layout is one the UI's estimates_table
    // block cannot carry, so the screen shows the rendered table itself: smcl's
    // text is fixed mode's (esttab), after the blank line esttab
    // prints first (StataBE 19.5 `esttab m1, cells(b)` and five other layouts
    // match this text line for line).
    if (cellsGiven || str("main").has_value() || str("aux").has_value()) {
      std::string body = text;
      while (!body.empty() && body.back() == '\n') body.pop_back();
      return displayBlock("\n" + body);
    }
    return ojson();
  }
  ojson result = emitTable("esttab", session, userPath, text, has("replace"), has("append"));
  if (!isErrorBlock(result)) {
    eoPostR(session, "estout", sp.models, ccols);
    session.eval.rClassCmd = "esttab";
  }
  // Options this build still accepts without honouring: said once, never silent.
  std::vector<std::string> seen;
  for (const char* n : {"page", "longtable", "standalone", "fonttbl", "attributes"}) {
    if (has(n) || str(n).has_value()) seen.push_back(n);
  }
  if (!seen.empty() && !isErrorBlock(result)) {
    ojson arr = ojson::array();
    arr.push_back(result);
    arr.push_back(displayBlock("(note: " + joinWith(seen, ", ") +
                               " accepted and not applied by this build)"));
    return arr;
  }
  return result;
}
}  // namespace

namespace {
// estout's MatrixMode (estout; esttab hands the same namelist
// on): one model named after the matrix, one side-by-side cell column per
// matrix column labelled with its name, the rows the matrix's rows, fmt()
// cycling over the columns.
bool eoMatrixModel(SessionState& session, const std::string& spec, EoSpec* sp, std::string* err) {
  // `e(NAME[, subopts])` / `r(NAME[, subopts])` (MatrixMode's e()/r() options,
  // estout) wrap the same `name [, Fmt() Transpose]` that
  // matrix() takes (ParseMatrixOpt); the model is called `e(NAME)`.
  std::string inner = eoTrim(spec);
  char cls = 0;
  if (inner.size() >= 3 && (inner[0] == 'e' || inner[0] == 'r') && inner[1] == '(' &&
      inner.back() == ')') {
    cls = inner[0];
    inner = inner.substr(2, inner.size() - 3);
  }
  std::string mname = inner, mlhs, mrhs;
  eoSplitComma(mname, &mlhs, &mrhs);
  const std::string bareName = eoTrim(mlhs);
  const std::string matName = cls == 0 ? bareName : std::string(1, cls) + "(" + bareName + ")";
  std::vector<std::string> fmts;
  bool transpose = false;
  for (const auto& [nm, arg] : eoSubopts(mrhs)) {
    if (eoAbbr(nm, "Fmt")) fmts = eoTokens(arg.value_or(""));
    else if (eoAbbr(nm, "Transpose")) transpose = true;
  }
  const eval::EvalMatrix* mat = nullptr;
  std::optional<eval::EvalMatrix> resolved;
  if (cls == 'e') {
    // `confirm matrix e(b)` (:4751) reads the live e(); e(b)/e(V) of a fit are
    // built by the one resolver every other matrix reader uses.
    int rc = 0;
    std::string why;
    resolved = session.eval.resolveEMatrix(bareName, &rc, &why);
    if (resolved.has_value()) mat = &*resolved;
  } else if (cls == 'r') {
    mat = session.eval.findClassMatrix('r', bareName);
  } else {
    for (const auto& kv : session.eval.matrices) {
      if (kv.first == matName) mat = &kv.second;
    }
  }
  if (mat == nullptr) {
    *err = "matrix " + matName + " not found";
    return false;
  }
  const size_t mr = mat->data.size();
  const size_t mc = mr == 0 ? 0 : mat->data[0].size();
  auto rowName = [&](size_t i) { return i < mat->rownames.size() ? mat->rownames[i] : "r" + std::to_string(i + 1); };
  auto colName = [&](size_t j) { return j < mat->colnames.size() ? mat->colnames[j] : "c" + std::to_string(j + 1); };
  auto at = [&](size_t i, size_t j) {
    return (i < mr && j < mat->data[i].size()) ? mat->data[i][j] : kEoNaN;
  };
  const size_t nr = transpose ? mc : mr;
  const size_t nc = transpose ? mr : mc;
  EoModel em;
  em.name = matName;
  sp->cells = EoCells{};
  sp->cells.rows.emplace_back();
  size_t fi = 0;
  for (size_t j = 0; j < nc; ++j) {
    EoElem el;
    el.name = "c" + std::to_string(j + 1);
    if (!fmts.empty()) {
      el.fmt = {fmts[std::min(fi, fmts.size() - 1)]};
      ++fi;
      if (fi >= fmts.size()) fi = 0;
    }
    el.label = transpose ? rowName(j) : colName(j);
    el.labelSet = true;
    sp->cells.elems.push_back(el);
    sp->cells.rows.back().push_back({static_cast<int>(j)});
    std::vector<EoKey> order;
    std::map<EoKey, double> vals;
    for (size_t i = 0; i < nr; ++i) {
      // A row's equation (`y:x1` of a system e(b)) is its own EoKey equation
      // (QuotedRowNames/roweq, :4759-4761; an empty one is "__", :4789).
      const std::string full = transpose ? colName(i) : rowName(i);
      const size_t colon = full.find(':');
      const EoKey key = colon == std::string::npos
                            ? EoKey{"__", full}
                            : EoKey{full.substr(0, colon), full.substr(colon + 1)};
      order.push_back(key);
      vals[key] = transpose ? at(j, i) : at(i, j);
    }
    em.extraOrder[el.name] = order;
    em.extra[el.name] = vals;
  }
  sp->cells.ncols = nc;
  for (auto& el : sp->cells.elems) {
    if (el.fmt.empty()) el.fmt = {"%9.0g"};
  }
  sp->models.push_back(em);
  return true;
}

// `matrix(A, fmt(...))` written where a namelist goes; returns its argument.
std::optional<std::string> eoMatrixNamelist(const std::vector<std::string>& words) {
  std::string joined;
  for (const auto& w : words) {
  if (!joined.empty()) joined += " ";
  joined += w;
  }
  const std::string t = eoTrim(joined);
  if (t.rfind("matrix(", 0) == 0 && t.back() == ')') return t.substr(7, t.size() - 8);
  // `estout e(b, transpose)` / `estout r(table)`: MatrixMode's e()/r() spelled
  // where the namelist goes (estout `MatrixMode, `anything''). Kept
  // wrapped so eoMatrixModel knows the class.
  if ((t.rfind("e(", 0) == 0 || t.rfind("r(", 0) == 0) && t.back() == ')') return t;
  return std::nullopt;
}
}  // namespace

// ===========================================================================
// estout (W2AK): estout.ado's option parse (estout) onto the shared
// writer above.
// ===========================================================================
ojson runEstout(const eval::ParsedCommand& parsed, SessionState& session) {
  if (auto err = eval::rejectUnknownOptions(parsed.opts, kVocabEstout.known,
                                            kVocabEstout.unimplemented)) {
    return ojson(*err);
  }
  const auto& opts = parsed.opts;
  auto has = [&](const char* k) { return hasFlag(opts, k); };
  auto str = [&](const char* k) { return optStr(opts, k); };
  auto eoFail = [](const EoError& e) { return tblErrorRc(e.msg, e.rc); };

  std::vector<std::string> words = parsed.varlist;
  const std::optional<std::string> usingPath = takeUsing(words);

  EoSpec sp;
  sp.session = &session;
  EoError eerr;

  // ---- matrix mode (estout) -------------------------------------
  std::optional<std::string> matrixOpt;
  for (const char* k : {"matrix", "e", "r"}) {
    if (auto v = str(k)) {
      if (matrixOpt.has_value()) return tblErrorRc("only one of matrix(), e(), or r() allowed", 198);
      matrixOpt = (std::string(k) == "matrix") ? *v : std::string(k) + "(" + *v + ")";
    }
  }
  if (!matrixOpt.has_value()) matrixOpt = eoMatrixNamelist(words);
  std::vector<Model> models;
  if (!matrixOpt.has_value()) {
    ojson err;
    models = collectModels(session, "estout", words, &err);
    if (models.empty()) return err;
  }

  // ---- style defaults (estout) ----------------------------------------
  std::string style = str("style").has_value() ? strutil::lower(eoTrim(dequote(*str("style"))))
                                               : (usingPath.has_value() ? "" : "smcl");
  if (style == "esttab") style = "tab";
  if (style != "" && style != "tab" && style != "fixed" && style != "tex" && style != "smcl") {
    if (style == "html" || style == "mmd" || style == "md") {
      return ojson(eval::refusedCommand("estout", "no_" + style + "_writer"));
    }
    return tblErrorRc(style + " style not available (file estout_" + style + ".def not found)", 601);
  }
  const bool label = (has("label") || has("labels")) && !has("nolabel");
  bool abbrev = has("abbrev") && !has("noabbrev");
  std::vector<int> mwv;
  if (auto mwo = str("modelwidth")) mwv = eoIntList(*mwo);
  int varwidth = -1;
  if (auto vwo = str("varwidth")) {
    const std::vector<int> v = eoIntList(*vwo);
    if (!v.empty()) varwidth = v[0];
  }
  sp.delimiter = "\t";
  sp.interaction = " # ";
  if (auto v = str("delimiter")) sp.delimiter = eoStringArg(v);
  if (auto v = str("begin")) sp.begin = eoStringArg(v);
  if (auto v = str("end")) sp.end = eoStringArg(v);
  if (auto v = str("lastend")) {
    sp.lastend = eoStringArg(v);
    sp.lastendSet = true;
  }
  if (auto v = str("interaction")) sp.interaction = eoStringArg(v);
  const bool eqMerge = false;
  const double varwidthfactor = 1.0 + ((eqMerge && !has("unstack")) ? 0.5 : 0.0);
  if (style == "smcl" || style == "fixed" || style == "tex") {
    if (varwidth < 0) varwidth = static_cast<int>((label ? 20 : 12) * varwidthfactor);
    if (mwv.empty()) mwv = {12};
  }
  if (style == "smcl" || style == "fixed") {
    if (!has("noabbrev")) abbrev = true;
    if (!str("delimiter").has_value()) sp.delimiter = " ";
  } else if (style == "tex") {
    if (!str("delimiter").has_value()) sp.delimiter = "&";
    if (!str("end").has_value()) sp.end = "\\\\";
    if (!str("interaction").has_value()) sp.interaction = " $\\times$ ";
  }
  sp.varwidth = varwidth < 0 ? 0 : varwidth;
  sp.modelwidth = mwv.empty() ? std::vector<int>{0} : mwv;
  sp.abbrev = abbrev;
  sp.wrap = has("wrap") && !has("nowrap");
  sp.lz = !has("nolz");
  if (auto ic = str("incelldelimiter")) sp.incelldelimiter = eoStringArg(ic);
  if (auto hl = str("hlinechar")) sp.hlinechar = eoStringArg(hl);
  if (auto v = str("substitute")) {
    const std::vector<std::string> t = eoTokens(*v);
    for (size_t i = 0; i < t.size(); i += 2) sp.substitute.emplace_back(t[i], i + 1 < t.size() ? t[i + 1] : "");
  }
  sp.label = label;
  if (label) session.syncEvalVarLabels();
  sp.unstack = has("unstack") && !has("nounstack");
  sp.omitted = !has("noomitted");
  sp.baselevels = !has("nobaselevels");
  sp.legend = has("legend") && !has("nolegend");
  sp.title = str("title").has_value() ? eoStringArg(str("title")) : std::string();
  sp.note = str("note").has_value() ? eoStringArg(str("note")) : std::string();
  if (auto ph = str("prehead")) sp.prehead = eoHookLines(*ph);
  if (auto ph = str("posthead")) sp.posthead = eoHookLines(*ph);
  if (auto pf = str("prefoot")) sp.prefoot = eoHookLines(*pf);
  if (auto pf = str("postfoot")) sp.postfoot = eoHookLines(*pf);
  if (auto xc = str("extracols")) sp.extracols = eoIntList(*xc);

  // ---- cells (estout, 356-365, 398-407) ------------------------------------
  if (auto c = str("cells")) {
    std::string cerr;
    if (!eoParseCells(*c, &sp.cells, &cerr)) return tblErrorRc(cerr, 198);
  } else {
    // estout's default is cells(b); the bare se/t/p flags this build has long
    // accepted add a parenthesised second row (BUG-486's arms).
    EoElem b;
    b.name = "b";
    sp.cells.elems.push_back(b);
    sp.cells.rows.push_back({{0}});
    for (const char* f : {"se", "t", "p"}) {
      if (has(f)) {
        EoElem e;
        e.name = f;
        e.hasPar = true;
        e.par = {"(", ")"};
        sp.cells.elems.push_back(e);
        sp.cells.rows.push_back({{static_cast<int>(sp.cells.elems.size()) - 1}});
        break;
      }
    }
    sp.cells.ncols = 1;
    if (auto f = str("fmt")) sp.cells.elems[0].fmt = eoTokens(*f);
  }
  // Formats: the first element's fmt, default %9.0g, is every fmt-less element's.
  if (!sp.cells.elems.empty()) {
    if (sp.cells.elems[0].fmt.empty()) sp.cells.elems[0].fmt = {"%9.0g"};
    for (auto& el : sp.cells.elems) {
      if (el.fmt.empty()) el.fmt = sp.cells.elems[0].fmt;
    }
  }
  if (auto st = str("stats")) {
    const std::string firstFmt =
        (!sp.cells.elems.empty() && !sp.cells.elems[0].fmt.empty()) ? sp.cells.elems[0].fmt[0] : "%9.0g";
    if (!eoParseStats(*st, firstFmt, &sp, &eerr)) return eoFail(eerr);
  }
  bool anyStar = false;
  for (const auto& el : sp.cells.elems) {
    if (el.star) anyStar = true;
  }
  if (auto sl = str("starlevels")) {
    std::string lab = sp.starlevelsLabel, del = sp.starlevelsDelim;
    if (!eoParseStarPairs(*sl, &sp.starlevels, &lab, &del, &eerr)) return eoFail(eerr);
    sp.starlevelsLabel = lab;
    sp.starlevelsDelim = del;
  }
  (void)anyStar;

  // ---- labels -----------------------------------------------------------------------------
  auto parseLab = [&](const char* k, EoLabelOpt* o) -> bool {
    if (auto v = str(k)) return eoParseLabelOpt(*v, o, &eerr.msg);
    return true;
  };
  if (!parseLab("mlabels", &sp.mlabels) || !parseLab("collabels", &sp.collabels) ||
      !parseLab("eqlabels", &sp.eqlabels) || !parseLab("varlabels", &sp.varlabels) ||
      !parseLab("mgroups", &sp.mgroups)) {
    return tblErrorRc(eerr.msg, 198);
  }
  sp.numbers = has("numbers") && !has("nonumbers");
  if (auto nb = str("numbers")) {
    const std::vector<std::string> t = eoTokens(*nb);
    sp.numL = t.size() > 0 ? t[0] : std::string();
    sp.numR = t.size() > 1 ? t[1] : std::string();
  }
  if (auto lc = str("labcol2")) {
    std::string lhs, rhs;
    eoSplitComma(*lc, &lhs, &rhs);
    sp.labcol2 = true;
    sp.labcol2Vals = eoTokens(lhs);
    for (const auto& [nm, arg] : eoSubopts(rhs)) {
      if (eoAbbr(nm, "Title")) {
        const std::string a = arg.value_or("");
        sp.labcol2Title = a.find('"') == std::string::npos ? std::vector<std::string>{a} : eoTokens(a);
      } else if (eoAbbr(nm, "Width")) {
        const std::vector<int> v = eoIntList(arg.value_or(""));
        if (!v.empty()) sp.labcol2Width = v[0];
      }
    }
  }

  // ---- coefficient selection --------------------------------------------------------------
  sp.keep = str("keep");
  sp.drop = str("drop");
  sp.order = str("order");
  if (auto rn = str("rename")) {
    std::string rl = *rn;
    std::replace(rl.begin(), rl.end(), ',', ' ');
    const std::vector<std::string> t = eoTokens(rl);
    for (size_t i = 0; i + 1 < t.size(); i += 2) {
      if (t[i + 1].find(':') != std::string::npos || t[i + 1].empty()) {
        return tblErrorRc("invalid rename()", 198);
      }
      sp.rename.emplace_back(t[i], t[i + 1]);
    }
  }
  if (auto rc = str("refcat")) {
    std::string lhs, rhs;
    eoSplitComma(*rc, &lhs, &rhs);
    const std::vector<std::string> t = eoTokens(lhs);
    for (size_t i = 0; i + 1 < t.size(); i += 2) sp.refcat.emplace_back(t[i], t[i + 1]);
    for (const auto& [nm, arg] : eoSubopts(rhs)) {
      if (eoAbbr(nm, "NOLabel")) sp.refcatLabel.clear();
      else if (eoAbbr(nm, "Label")) sp.refcatLabel = eoStringArg(arg);
      else if (eoAbbr(nm, "Below")) sp.refcatBelow = true;
    }
  }
  if (auto ind = str("indicate")) {
    std::string lhs, rhs;
    eoSplitComma(*ind, &lhs, &rhs);
    for (const auto& g : eoTokens(lhs)) {
      EoIndicate ig;
      const size_t eq = g.find('=');
      if (eq == std::string::npos) {
        ig.vars = g;
      } else {
        ig.name = eoTrim(g.substr(0, eq));
        ig.vars = g.substr(eq + 1);
      }
      sp.indicate.push_back(ig);
    }
    for (const auto& [nm, arg] : eoSubopts(rhs)) {
      if (eoAbbr(nm, "Labels")) {
        const std::vector<std::string> t = eoTokens(arg.value_or(""));
        if (!t.empty()) sp.indicateYes = t[0];
        if (t.size() > 1) sp.indicateNo = t[1];
      }
    }
  }

  // ---- the models ---------------------------------------------------------------------------
  TransformSpec trSpec;
  if (auto tr = str("transform")) {
    std::string trErr;
    if (!parseTransformOpt(*tr, &trSpec, &trErr)) return tblSyntax(trErr);
  }
  if (matrixOpt.has_value()) {
    std::string merr;
    if (!eoMatrixModel(session, *matrixOpt, &sp, &merr)) return tblErrorRc(merr, 111);
  } else {
    for (size_t mi = 0; mi < models.size(); ++mi) {
      sp.models.push_back(eoModelFrom(models[mi], models[mi].name, session,
                                      trSpec.empty() ? nullptr : &trSpec, mi, !sp.omitted,
                                      sp.baselevels));
    }
    for (size_t mi = 0; mi < sp.models.size(); ++mi) sp.models[mi].model = &models[mi];
  }

  std::string text;
  int ccols = -1;
  if (!eoRender(sp, &text, &eerr, &ccols)) return eoFail(eerr);
  ojson result;
  if (!usingPath.has_value()) {
    result = echoBlock(text);
  } else {
    result = emitTable("estout", session, *usingPath, text, has("replace"), has("append"));
  }
  if (!isErrorBlock(result)) {
    eoPostR(session, "estout", sp.models, ccols);
    session.eval.rClassCmd = "estout";
  }
  return result;
}

// Portions derived from outreg2 2.3.2 (GPL-3.0).
// See THIRD_PARTY_NOTICES.
// ===========================================================================
// outreg2
//
// outreg2 reports the ACTIVE estimation (or the stored ones named in
// `outreg2 [m1 m2] using ...`), and its
// `append` crosses SESSIONS -- run the same do-file twice and the second run's
// append lands on the first run's file. So the `.txt` ledger IS the source of
// truth for append (plan decision 7); an in-memory ledger would answer
// differently on the second run, which is the rejected alternative.
// ===========================================================================

namespace {

// `addtext(Controls, Yes, Year FE, Yes)` / `addstat(F, e(F), R2adj, e(r2_a))`
// -- both are FLAT comma lists of alternating name and value.
std::vector<std::pair<std::string, std::string>> parseCommaPairs(const std::string& spec) {
  std::vector<std::pair<std::string, std::string>> out;
  const std::vector<std::string> toks = splitTopLevel(dequote(spec), ',');
  for (size_t i = 0; i + 1 < toks.size(); i += 2) {
    out.emplace_back(strutil::trim(dequote(strutil::trim(toks[i]))),
                     strutil::trim(dequote(strutil::trim(toks[i + 1]))));
  }
  return out;
}

// ---------------------------------------------------------------------------
// outreg2 2.3.2 (17aug2014), derived from its own source rather than from the
// shape of its output. The ado works on a STRING DATASET whose rows are the
// ledger's lines and whose columns are its tab-separated cells, and every
// file it writes is a rendering of that one grid:
//
//   makeFile     builds the new column (report / reportCol / rowtype2)
//   appendFile   merges it into the ledger already on disk, by a sort key
//   cleanFile    adds the LABELS column and finds the head/bottom borders
//   _texout      the .tex file, from the label-merged grid
//   _xmlout      the Excel-2003 workbook, from the same grid
//
// So this port keeps a grid (O2Grid) and derives each stage, following
// the ado's own structure. The oracle is tests/eval/golden/aer_w2/w2at_table_writers
// (every file read back line by line) and aer_b4/tables/o_*.txt.
// ---------------------------------------------------------------------------

using O2Grid = std::vector<std::vector<std::string>>;

// Stata's missing value inside this port.
const double kO2Missing = std::numeric_limits<double>::quiet_NaN();

bool o2IsMissing(double v) { return std::isnan(v); }

// `round(x - int(round(x, 1e-10)), 1e-10) == 0` -- the integer test autogen
// and autodigits2 share (outreg2).
bool o2Whole(double v) {
  const double r = std::floor(v / 1e-10 + 0.5) * 1e-10;
  const double d = (v - std::trunc(r)) / 1e-10;
  return d >= -0.5 && d < 0.5;
}

// string(v, "%12.<d>e")
std::string o2Exp(double v, int d) {
  char buf[64];
  std::snprintf(buf, sizeof(buf), "%.*e", std::max(0, d), v);
  return buf;
}

// string(v, "%12.<dec>fc") -- the width-12 comma format every fixed cell uses
// (fmtgen). Its three overflow regimes are outreg2AdecCell's, measured.
std::string o2Fixed(double v, int dec) {
  if (o2IsMissing(v)) return kMissingCell;
  return outreg2AdecCell(v, dec).text;
}

// The digit count autogen (:6655) and autodigits2 (:6771) derive. They differ
// in ONE branch: past `auto` integer digits autogen gives auto-left-1 (then
// floors at 0) and autodigits2 gives 0 -- the same number after the floor.
int o2AutoDec(double v, int autoD, int less) {
  int aadec = 0;
  if (!o2Whole(v)) {
    const double frac = std::fabs(v - std::trunc(v));
    const int times =
        std::abs(static_cast<int>(std::trunc(std::log(frac) / std::log(10.0) - 1)));
    const int left = static_cast<int>(std::trunc(std::log(std::fabs(v)) / std::log(10.0) + 1));
    if (left > 0) {
      aadec = (left <= autoD) ? autoD - left + 1 : autoD - left - 1;
    } else {
      aadec = times + autoD - 1;
    }
  }
  aadec -= less;
  return aadec < 0 ? 0 : aadec;
}

// autogen (:6655-6740): %12.<aadec>fc, (auto-1)e from seven decimals on, auto e
// beyond +-1e6. A MISSING value gets an empty format and `string(., "")` is
// empty -- which is why o_stats.txt's depvar row is blank and o09's (bdec(3),
// so fmtgen) reads `.`.
std::string o2AutoCell(double v, int autoD, int less) {
  if (o2IsMissing(v)) return std::string();
  if (v > 1000000 || v < -1000000) return o2Exp(v, autoD);
  const int aadec = o2AutoDec(v, autoD, less);
  if (aadec >= 7) return o2Exp(v, autoD - 1);
  return o2Fixed(v, aadec);
}

// autodigits2 as _addstat_parse uses it (:10420): string(v, "%12.<n>f") -- no
// separators, because r(valstr) is `<n>f`.
std::string o2AutoDigits2Cell(double v, int autoD, int less) {
  if (o2IsMissing(v)) return kMissingCell;
  if (v > 1000000 || v < -1000000) return o2Exp(v, autoD);
  const int aadec = o2AutoDec(v, autoD, less);
  if (aadec >= 7) return o2Exp(v, autoD - 1);
  const AdecCell c = outreg2AdecCell(v, aadec);
  if (c.seps == 0) return c.text;
  char buf[512];
  std::snprintf(buf, sizeof(buf), "%.*f", aadec, v);
  return buf;
}

// Split an option body at top-level commas, keeping EMPTY items (a leading,
// doubled or trailing comma is an empty ctitle row, :6029-6050). Each item is
// trimmed and loses one pair of outer double quotes, as gettoken strips them.
std::vector<std::string> o2SplitCommas(const std::string& s) {
  std::vector<std::string> out;
  std::string cur;
  int depth = 0;
  bool inQ = false;
  for (char c : s) {
    if (c == '"') inQ = !inQ;
    if (!inQ) {
      if (c == '(') ++depth;
      if (c == ')' && depth > 0) --depth;
      if (c == ',' && depth == 0) {
        out.push_back(dequote(strutil::trim(cur)));
        cur.clear();
        continue;
      }
    }
    cur += c;
  }
  out.push_back(dequote(strutil::trim(cur)));
  return out;
}

bool o2IsFactorPart(const std::string& p) {
  // `1.g`, `0b.g`, `2o.g`, `1bn.g` -- a level, then a dot.
  const size_t dot = p.find('.');
  if (dot == std::string::npos || dot == 0) return false;
  if (!std::isdigit(static_cast<unsigned char>(p[0]))) return false;
  for (size_t i = 0; i < dot; ++i) {
    const char c = p[i];
    if (!std::isdigit(static_cast<unsigned char>(c)) && c != 'b' && c != 'o' && c != 'n') {
      return false;
    }
  }
  return true;
}

// Stata's e(b) spelling of an interaction: factor parts first, continuous parts
// as `c.<var>` (w2at o21: `regress y c.x1#i.g` writes `1.g#c.x1`). This build's
// coeftable keeps the typed order and drops the `c.` (`x1#1.g`).
std::string o2StataName(const std::string& n) {
  if (n.find('#') == std::string::npos) return n;
  std::vector<std::string> fac, con;
  size_t pos = 0;
  while (true) {
    const size_t h = n.find('#', pos);
    std::string part = n.substr(pos, h == std::string::npos ? std::string::npos : h - pos);
    if (o2IsFactorPart(part)) {
      fac.push_back(part);
    } else {
      if (part.rfind("c.", 0) != 0) part = "c." + part;
      con.push_back(part);
    }
    if (h == std::string::npos) break;
    pos = h + 1;
  }
  std::string out;
  for (const auto& p : fac) out += (out.empty() ? "" : "#") + p;
  for (const auto& p : con) out += (out.empty() ? "" : "#") + p;
  return out;
}

// fvts_label (:10145-10241): the LABELS cell of one e(b) name.
std::string o2FvtsLabel(const std::string& name, SessionState& session) {
  if (name.find('#') != std::string::npos) return std::string();  // poundsign split: no branch
  Datastore* ds = session.datastore();
  auto exists = [&](const std::string& v) { return ds != nullptr && ds->has(v); };
  auto varLabelOf = [&](const std::string& v) {
    std::string l = strutil::trim(session.eval.varLabels.get(v));
    return l.empty() ? v : l;
  };
  const size_t dot = name.find('.');
  if (dot == std::string::npos) {
    return exists(name) ? varLabelOf(name) : std::string();
  }
  const std::string first = name.substr(0, dot);
  const std::string third = name.substr(dot + 1);
  if (first.empty() || third.empty() || third.find('.') != std::string::npos) return std::string();
  if (!exists(third)) return std::string();
  const std::string varlabel = varLabelOf(third);
  std::string end;
  std::string bare;
  bool firstIsNumber = !first.empty();
  for (char c : first) {
    if (!std::isdigit(static_cast<unsigned char>(c))) firstIsNumber = false;
  }
  if (!firstIsNumber) {
    std::string baseOrOmitted;
    if (first == "co") {
      baseOrOmitted = "continuous omitted";
    } else {
      bare = first.substr(0, first.size() - 1);
      const char suffix = first.back();
      bool bareNum = !bare.empty();
      for (char c : bare) {
        if (!std::isdigit(static_cast<unsigned char>(c))) bareNum = false;
      }
      if (bareNum) {
        if (suffix == 'o') baseOrOmitted = "omitted";
        if (suffix == 'b') baseOrOmitted = "base";
        if (suffix == 'n') {
          bare = first.substr(0, first.size() >= 2 ? first.size() - 2 : 0);
          baseOrOmitted = "base";
        }
      }
    }
    end = ", " + baseOrOmitted;
  }
  // `: value label third` then `: label lblname first` -- which answers the
  // value itself when the set has no text for it.
  std::string labelOfValue;
  auto lb = session.varLblname.find(third);
  if (lb != session.varLblname.end() && !lb->second.empty()) {
    auto set = session.labelDefs.find(lb->second);
    if (set != session.labelDefs.end()) {
      auto hit = set->second.find(first);
      labelOfValue = (hit != set->second.end()) ? hit->second : first;
    }
  }
  if (labelOfValue.empty() && !bare.empty()) return varlabel + " = " + bare + end;
  if (labelOfValue.empty()) return varlabel + " = " + first + end;
  return varlabel + " = " + first + ", " + labelOfValue + end;
}

// `fvtsunab <token>, onebyone` (:612-622, :10081): a keep()/drop() token
// unabbreviated against the DATASET's variables. A token that is not a
// variable list (`_cons`) expands to nothing -- which is why `drop(_cons)`
// keeps the Constant row (w2at o10).
struct O2Token {
  std::string text;      // a name to match exactly (Stata spelling)
  bool anyLevel = false; // `i.var`: every level of var
};
std::vector<O2Token> o2ExpandTokens(const std::string& spec, SessionState& session) {
  std::vector<O2Token> out;
  Datastore* ds = session.datastore();
  for (const auto& tok : splitWs(dequote(spec))) {
    if (tok.find('#') != std::string::npos) {
      out.push_back({o2StataName(tok), false});
      continue;
    }
    const size_t dot = tok.find('.');
    if (dot != std::string::npos) {
      const std::string head = tok.substr(0, dot);
      if (head == "i") {
        out.push_back({tok.substr(dot + 1), true});
      } else if (head == "c") {
        out.push_back({tok.substr(dot + 1), false});
      } else {
        out.push_back({tok, false});
      }
      continue;
    }
    if (ds == nullptr) continue;
    for (const auto& v : ds->names()) {
      if (strutil::globMatch(tok, v)) out.push_back({v, false});
    }
  }
  return out;
}

bool o2TokenHits(const std::vector<O2Token>& toks, const std::string& stataName) {
  for (const auto& t : toks) {
    if (t.anyLevel) {
      const size_t dot = stataName.find('.');
      if (dot != std::string::npos && stataName.find('#') == std::string::npos &&
          o2IsFactorPart(stataName) && stataName.substr(dot + 1) == t.text) {
        return true;
      }
    } else if (t.text == stataName) {
      return true;
    }
  }
  return false;
}

// One row of the new column -- makeFile's report / reportCol / varname /
// rowtype2 (-1 header, 0 top blank, 1 body, 2 bottom blank, 3 statistics and
// notes; :6091-6098).
struct O2Row {
  std::string report;
  std::string cell;
  std::string varname;
  int rowtype = 1;
};

// The sort key Stata's `sort` uses: missing sorts after every number.
int o2Cmp(double a, double b) {
  const bool ma = o2IsMissing(a), mb = o2IsMissing(b);
  if (ma && mb) return 0;
  if (ma) return 1;
  if (mb) return -1;
  return a < b ? -1 : (a > b ? 1 : 0);
}

int o2CmpKeys(const std::vector<double>& a, const std::vector<double>& b) {
  for (size_t i = 0; i < a.size() && i < b.size(); ++i) {
    const int c = o2Cmp(a[i], b[i]);
    if (c != 0) return c;
  }
  return 0;
}

std::string o2TrimCell(const std::string& s) { return strutil::trim(s); }

// The Vorder header pass both halves of appendFile run (:3019-3025, :3487-3496):
// the top blank row is 0, each header row above it one less, and the topmost
// of them -99 -- so header rows merge by their DISTANCE from the body, and the
// column-number row always meets the column-number row.
void o2HeaderOrder(const std::vector<int>& rowtype, std::vector<double>* vorder) {
  const size_t n = rowtype.size();
  for (size_t i = 0; i < n; ++i) (*vorder)[i] = rowtype[i] == 0 ? 0.0 : kO2Missing;
  for (int pass = 0; pass < 100 && n > 0 && o2IsMissing((*vorder)[0]); ++pass) {
    bool moved = false;
    for (size_t i = 0; i + 1 < n; ++i) {
      if (rowtype[i] == -1 && o2IsMissing((*vorder)[i]) && !o2IsMissing((*vorder)[i + 1])) {
        (*vorder)[i] = (*vorder)[i + 1] - 1;
        moved = true;
      }
    }
    if (!moved) break;
  }
  double mn = kO2Missing;
  for (size_t i = 0; i < n; ++i) {
    if (!o2IsMissing((*vorder)[i]) && (o2IsMissing(mn) || (*vorder)[i] < mn)) mn = (*vorder)[i];
  }
  if (!o2IsMissing(mn)) {
    for (size_t i = 0; i < n; ++i) {
      if ((*vorder)[i] == mn) (*vorder)[i] = -99;
    }
  }
}

// sortvar() tokens: `tsunab` each one, keeping it literally when that fails
// (:545-568).
std::vector<std::string> o2SortvarList(const std::string& spec, SessionState& session) {
  std::vector<std::string> out;
  Datastore* ds = session.datastore();
  for (const auto& tok : splitWs(dequote(spec))) {
    bool any = false;
    if (ds != nullptr && tok.find('.') == std::string::npos) {
      for (const auto& v : ds->names()) {
        if (strutil::globMatch(tok, v)) {
          if (std::find(out.begin(), out.end(), v) == out.end()) out.push_back(v);
          any = true;
        }
      }
    }
    if (!any && std::find(out.begin(), out.end(), tok) == out.end()) out.push_back(tok);
  }
  return out;
}

// appendFile (:2991-3843). `f1` is the ledger as it stands, LABELS column and
// title rows already stripped; `f2` the new column. Returns the merged raw
// grid: f1's columns, then the new one.
O2Grid o2AppendFile(const O2Grid& f1, const std::vector<O2Row>& f2In,
                    const std::vector<std::string>& sortvar,
                    std::vector<std::string>* bodyNamesOut) {
  const size_t k1 = f1.empty() ? 1 : f1[0].size();
  const size_t n1 = f1.size();

  // ---- the master (file 1) ------------------------------------------------
  struct M {
    std::vector<std::string> v;
    int rowtype = -1;
    std::string mergeOn;
    double varnum = kO2Missing, vorder = kO2Missing, vorder0 = kO2Missing,
           vorder1 = kO2Missing, sortvarCol = kO2Missing, vord = kO2Missing,
           eqOrder = 1;
    bool constant = false;
  };
  std::vector<M> m(n1);
  std::string variables1;
  for (size_t i = 0; i < n1; ++i) {
    m[i].v = f1[i];
    if (variables1.empty() && !f1[i][0].empty()) variables1 = f1[i][0];
  }
  {
    // rowmiss, and only the row right under VARIABLES may count as blank.
    std::vector<int> rowmiss(n1, 0);
    for (size_t i = 0; i < n1; ++i) {
      for (const auto& c : f1[i]) rowmiss[i] += c.empty() ? 0 : 1;
      const std::string prev = i == 0 ? std::string() : f1[i - 1][0];
      if (prev != variables1) rowmiss[i] = 1;
    }
    std::vector<int> rt(n1, -1);
    std::vector<bool> after(n1, false);
    for (size_t i = 0; i < n1; ++i) {
      if (rowmiss[i] == 0) rt[i] = 0;
      if (i > 0 && (rowmiss[i - 1] == 0 || after[i - 1])) after[i] = true;
    }
    // The bottom row: the LAST row whose first cell is empty (:3384-3398).
    size_t bottom = 0;  // 1-based; 0 = none
    for (size_t i = n1; i >= 1; --i) {
      if (f1[i - 1][0].empty()) {
        bottom = i;
        break;
      }
    }
    for (size_t i = 0; i < n1; ++i) {
      if (after[i]) rt[i] = 1;
      if (bottom != 0 && i + 1 == bottom) rt[i] = 2;
      if (bottom != 0 && i + 1 > bottom) rt[i] = 3;
      m[i].rowtype = rt[i];
    }
  }
  for (auto& r : m) {
    if (r.rowtype == -1) r.v[0].clear();
  }
  std::vector<std::string> v2plus(n1);
  for (size_t i = 0; i < n1; ++i) {
    v2plus[i] = (k1 >= 2 ? o2TrimCell(m[i].v[1]) : std::string()) +
                (k1 >= 3 ? o2TrimCell(m[i].v[2]) : std::string());
    m[i].mergeOn = m[i].v[0];
  }
  for (size_t i = 0; i < n1; ++i) {
    if (o2TrimCell(m[i].v[0]).empty()) {
      m[i].mergeOn = (i == 0 ? std::string() : m[i - 1].mergeOn) + "!";
    }
  }
  {
    std::vector<int> rt(n1);
    std::vector<double> vo(n1);
    for (size_t i = 0; i < n1; ++i) rt[i] = m[i].rowtype;
    o2HeaderOrder(rt, &vo);
    for (size_t i = 0; i < n1; ++i) m[i].vorder = vo[i];
  }
  for (auto& r : m) {
    if (r.rowtype == 2) r.mergeOn = "bottomRow";
    if (r.rowtype == 0) r.mergeOn = "topRow";
    if (r.rowtype == -1) r.mergeOn = "_000";
    if (!o2IsMissing(r.vorder) && r.vorder < 1) r.varnum = r.vorder;
  }
  for (size_t i = 0; i < n1; ++i) {
    if (m[i].rowtype >= 2) m[i].vorder = 3.7;
  }
  for (size_t i = 1; i < n1; ++i) {
    if (!o2IsMissing(m[i].vorder) && m[i].vorder >= 3.7 && m[i].rowtype == 3) {
      m[i].vorder = m[i - 1].vorder + .0001;
    }
  }
  for (auto& r : m) {
    if (r.rowtype == 2) r.vorder = 2;
    if (r.v[0] == "Observations") r.vorder = 3.5;
    if (r.v[0] == "R-squared") r.vorder = 3.6;
  }
  for (size_t i = 1; i < n1; ++i) {
    const double p = m[i - 1].vorder;
    if (o2IsMissing(m[i].vorder) && !o2IsMissing(p) && p <= 1) m[i].vorder = 1;
  }
  for (size_t i = 0; i < n1; ++i) {
    const std::string v2 = k1 >= 2 ? m[i].v[1] : std::string();
    if (v2.empty() && v2plus[i].empty() && !o2IsMissing(m[i].vorder) && m[i].vorder > 3.5) {
      m[i].vorder = 4;
    }
  }
  double maxnum = 1;
  if (!sortvar.empty()) {
    for (size_t t = 0; t < sortvar.size(); ++t) {
      for (auto& r : m) {
        if (r.v[0] == sortvar[t]) r.vorder0 = static_cast<double>(t + 1);
      }
    }
    maxnum = static_cast<double>(sortvar.size() + 1);
  }
  for (auto& r : m) r.sortvarCol = r.vorder0;
  for (size_t i = 0; i < n1; ++i) {
    if (m[i].vorder == 1 && !o2TrimCell(m[i].mergeOn).empty() &&
        !o2TrimCell(m[i].v[0]).empty() && o2IsMissing(m[i].vorder0)) {
      m[i].vorder0 = static_cast<double>(i + 1) + maxnum;
    }
  }
  for (size_t i = 1; i < n1; ++i) {
    if (o2IsMissing(m[i].vorder0) && m[i].vorder == 1) m[i].vorder0 = m[i - 1].vorder0;
  }
  for (auto& r : m) {
    if (!o2IsMissing(r.vorder0) && r.vorder0 < maxnum) r.sortvarCol = r.vorder0;
  }
  for (size_t i = 0; i < n1; ++i) {
    if (m[i].vorder == 1 && !m[i].v[0].empty()) m[i].vorder1 = m[i].vorder0;
  }
  for (size_t i = 1; i < n1; ++i) {
    if (o2IsMissing(m[i].vorder1) && m[i].vorder == 1 && !o2IsMissing(m[i - 1].vorder1)) {
      m[i].vorder1 = m[i - 1].vorder1 + .01;
    }
  }
  for (size_t i = 0; i < n1; ++i) {
    m[i].eqOrder = m[i].rowtype == 2 ? kO2Missing : 1;
    m[i].vord = static_cast<double>(i + 1);
    if (m[i].rowtype == 1 && !m[i].v[0].empty() && bodyNamesOut != nullptr) {
      bodyNamesOut->push_back(m[i].v[0]);
    }
  }
  std::stable_sort(m.begin(), m.end(), [](const M& a, const M& b) {
    return o2CmpKeys({a.vorder, a.eqOrder, a.vorder1, a.vord},
                     {b.vorder, b.eqOrder, b.vorder1, b.vord}) < 0;
  });
  for (size_t i = 0; i < n1; ++i) {
    m[i].vord = static_cast<double>(i + 1);
    m[i].constant = m[i].v[0] == "Constant" || m[i].v[0] == "_cons";
    if (i > 0 && m[i].mergeOn == m[i - 1].mergeOn + "!" && m[i - 1].constant) {
      m[i].constant = true;
    }
  }

  // ---- the using side (file 2, the new column) ----------------------------
  struct U {
    std::string report, cell, varname, mergeOn;
    int rowtype = 1;
    double varnum = kO2Missing, vorder = kO2Missing, vorder0 = kO2Missing,
           vorder1 = kO2Missing, sortvarCol = kO2Missing, vord = kO2Missing, mrgrow = 0;
    bool constant = false;
  };
  std::vector<U> u(f2In.size());
  const size_t n2 = u.size();
  std::string variables2;
  for (size_t i = 0; i < n2; ++i) {
    u[i].report = f2In[i].report;
    u[i].cell = f2In[i].cell;
    u[i].varname = f2In[i].varname;
    u[i].rowtype = f2In[i].rowtype;
    u[i].mrgrow = static_cast<double>(i + 1);
    if (variables2.empty() && !u[i].report.empty()) variables2 = u[i].report;
  }
  for (auto& r : u) {
    if (r.report == variables2 && r.rowtype == -1) r.report.clear();
  }
  {
    std::vector<int> rt(n2);
    std::vector<double> vo(n2);
    for (size_t i = 0; i < n2; ++i) rt[i] = u[i].rowtype;
    o2HeaderOrder(rt, &vo);
    for (size_t i = 0; i < n2; ++i) u[i].vorder = vo[i];
  }
  for (auto& r : u) {
    if (r.rowtype >= 2) r.vorder = 3.8;
  }
  for (size_t i = 1; i < n2; ++i) {
    if (!o2IsMissing(u[i].vorder) && u[i].vorder >= 3.8 && u[i].rowtype == 3) {
      u[i].vorder = u[i - 1].vorder + .0001;
    }
  }
  for (auto& r : u) {
    if (r.rowtype == 2) r.vorder = 2;
    if (r.report == "Observations") r.vorder = 3.5;
    if (r.report == "R-squared") r.vorder = 3.6;
  }
  for (size_t i = 1; i < n2; ++i) {
    const double p = u[i - 1].vorder;
    if (o2IsMissing(u[i].vorder) && !o2IsMissing(p) && p <= 1) u[i].vorder = 1;
  }
  for (size_t i = 1; i < n2; ++i) {
    const double p = u[i - 1].vorder;
    if (u[i].report.empty() && (p == 2 || p == 2.5)) u[i].vorder = 2.5;
  }
  if (!sortvar.empty()) {
    for (size_t t = 0; t < sortvar.size(); ++t) {
      for (auto& r : u) {
        if (r.varname == sortvar[t]) r.vorder0 = static_cast<double>(t + 1);
      }
    }
  }
  for (auto& r : u) r.sortvarCol = r.vorder0;
  for (size_t i = 0; i < n2; ++i) {
    if (u[i].vorder == 1 && !u[i].report.empty() && o2IsMissing(u[i].vorder0)) {
      u[i].vorder0 = static_cast<double>(i + 1) + maxnum;
    }
  }
  for (size_t i = 1; i < n2; ++i) {
    if (o2IsMissing(u[i].vorder0) && u[i].vorder == 1) u[i].vorder0 = u[i - 1].vorder0;
  }
  for (size_t i = 0; i < n2; ++i) {
    if (u[i].vorder == 1 && !u[i].report.empty()) u[i].vorder1 = u[i].vorder0;
  }
  for (size_t i = 1; i < n2; ++i) {
    if (o2IsMissing(u[i].vorder1) && u[i].vorder == 1 && !o2IsMissing(u[i - 1].vorder1)) {
      u[i].vorder1 = u[i - 1].vorder1 + .01;
    }
  }
  std::stable_sort(u.begin(), u.end(), [](const U& a, const U& b) {
    return o2CmpKeys({a.vorder, a.vorder1, a.mrgrow}, {b.vorder, b.vorder1, b.mrgrow}) < 0;
  });
  for (size_t i = 0; i < n2; ++i) {
    u[i].mergeOn = u[i].report;
    if (o2TrimCell(u[i].mergeOn).empty()) {
      u[i].mergeOn = (i == 0 ? std::string() : u[i - 1].mergeOn) + "!";
    }
  }
  for (size_t i = 0; i < n2; ++i) {
    if (u[i].rowtype == 2) u[i].mergeOn = "bottomRow";
    if (u[i].rowtype == 0) u[i].mergeOn = "topRow";
    if (u[i].rowtype == -1) u[i].mergeOn = "_000";
    if (!o2IsMissing(u[i].vorder) && u[i].vorder < 1) u[i].varnum = u[i].vorder;
    u[i].vord = static_cast<double>(i + 1);
    u[i].constant = u[i].report == "Constant" || u[i].report == "_cons" ||
                    u[i].varname == "Constant" || u[i].varname == "_cons";
    if (i > 0 && u[i].mergeOn == u[i - 1].mergeOn + "!" && u[i - 1].constant) {
      u[i].constant = true;
    }
  }

  // ---- `merge mergeVar varnum using` ---------------------------------------
  // Old-style match merge: rows pair up by (mergeVar, varnum); inside a key
  // the k-th master row meets the k-th using row, and the shorter side's last
  // row repeats -- Stata's m:m rule.
  struct J {
    int mi = -1, ui = -1;
  };
  std::vector<J> joined;
  {
    auto keyLess = [](const std::string& a, double av, const std::string& b, double bv) {
      if (a != b) return a < b;
      return o2Cmp(av, bv) < 0;
    };
    std::vector<int> mo(n1), uo(n2);
    for (size_t i = 0; i < n1; ++i) mo[i] = static_cast<int>(i);
    for (size_t i = 0; i < n2; ++i) uo[i] = static_cast<int>(i);
    std::stable_sort(mo.begin(), mo.end(), [&](int a, int b) {
      return keyLess(m[a].mergeOn, m[a].varnum, m[b].mergeOn, m[b].varnum);
    });
    std::stable_sort(uo.begin(), uo.end(), [&](int a, int b) {
      return keyLess(u[a].mergeOn, u[a].varnum, u[b].mergeOn, u[b].varnum);
    });
    size_t a = 0, b = 0;
    while (a < n1 || b < n2) {
      if (b >= n2 ||
          (a < n1 && keyLess(m[mo[a]].mergeOn, m[mo[a]].varnum, u[uo[b]].mergeOn, u[uo[b]].varnum))) {
        joined.push_back({mo[a], -1});
        ++a;
        continue;
      }
      if (a >= n1 ||
          keyLess(u[uo[b]].mergeOn, u[uo[b]].varnum, m[mo[a]].mergeOn, m[mo[a]].varnum)) {
        joined.push_back({-1, uo[b]});
        ++b;
        continue;
      }
      size_t ae = a, be = b;
      while (ae < n1 && m[mo[ae]].mergeOn == m[mo[a]].mergeOn &&
             o2Cmp(m[mo[ae]].varnum, m[mo[a]].varnum) == 0) {
        ++ae;
      }
      while (be < n2 && u[uo[be]].mergeOn == u[uo[b]].mergeOn &&
             o2Cmp(u[uo[be]].varnum, u[uo[b]].varnum) == 0) {
        ++be;
      }
      const size_t cm = ae - a, cu = be - b;
      for (size_t q = 0; q < std::max(cm, cu); ++q) {
        joined.push_back({mo[a + std::min(q, cm - 1)], uo[b + std::min(q, cu - 1)]});
      }
      a = ae;
      b = be;
    }
  }

  struct R {
    int mi = -1, ui = -1;
    double vorder = kO2Missing, eq0 = 1, sortvarCol = kO2Missing, vo1 = kO2Missing,
           vo2 = kO2Missing, merge2 = 0, vord1 = kO2Missing, vord2 = kO2Missing,
           vorder2_0 = kO2Missing, vorder1_0 = kO2Missing;
  };
  std::vector<R> rows;
  rows.reserve(joined.size());
  for (const auto& j : joined) {
    R r;
    r.mi = j.mi;
    r.ui = j.ui;
    const double vorder1 = j.mi >= 0 ? m[j.mi].vorder : kO2Missing;
    const double vorder2 = j.ui >= 0 ? u[j.ui].vorder : kO2Missing;
    r.vorder = vorder2;
    if (o2IsMissing(r.vorder) || (!o2IsMissing(vorder1) && vorder1 > 3.5 && vorder1 < 4)) {
      r.vorder = vorder1;
    }
    r.merge2 = (j.mi < 0) ? 1 : 0;
    r.vord1 = j.mi >= 0 ? m[j.mi].vord : kO2Missing;
    r.vord2 = j.ui >= 0 ? u[j.ui].vord : kO2Missing;
    r.vo1 = j.mi >= 0 ? m[j.mi].vorder1 : kO2Missing;
    r.vo2 = j.ui >= 0 ? u[j.ui].vorder1 : kO2Missing;
    r.vorder1_0 = j.mi >= 0 ? m[j.mi].vorder0 : kO2Missing;
    r.vorder2_0 = j.ui >= 0 ? u[j.ui].vorder0 : kO2Missing;
    const bool c1 = j.mi >= 0 && m[j.mi].constant;
    const bool c2 = j.ui >= 0 && u[j.ui].constant;
    const bool constant = j.mi >= 0 ? (c1 || c2) : c2;
    r.eq0 = constant ? 1.5 : 1;
    const double s1 = j.mi >= 0 ? m[j.mi].sortvarCol : kO2Missing;
    const double s2 = j.ui >= 0 ? u[j.ui].sortvarCol : kO2Missing;
    r.sortvarCol = o2IsMissing(s1) ? s2 : s1;
    rows.push_back(r);
  }
  // Fill the gaps a differing number of statistic rows leaves (:3640-3648).
  {
    std::stable_sort(rows.begin(), rows.end(),
                     [](const R& a, const R& b) { return o2Cmp(a.vo2, b.vo2) < 0; });
    for (size_t i = 1; i < rows.size(); ++i) {
      R& r = rows[i];
      const R& p = rows[i - 1];
      if (!o2IsMissing(r.vorder2_0) && r.vorder2_0 == p.vorder2_0 && o2IsMissing(r.vord1) &&
          !o2IsMissing(p.vord1) && r.vorder == 1) {
        r.vord1 = p.vord1 + .01;
      }
    }
    std::stable_sort(rows.begin(), rows.end(),
                     [](const R& a, const R& b) { return o2Cmp(a.vo1, b.vo1) < 0; });
    for (size_t i = 1; i < rows.size(); ++i) {
      R& r = rows[i];
      const R& p = rows[i - 1];
      if (!o2IsMissing(r.vorder1_0) && r.vorder1_0 == p.vorder1_0 && o2IsMissing(r.vord2) &&
          !o2IsMissing(p.vord2) && r.vorder == 1) {
        r.vord2 = p.vord2 + .01;
      }
    }
    for (auto& r : rows) {
      if (r.vorder == 1) {
        r.vo2 = r.vord2;
        r.vo1 = r.vord1;
      }
    }
  }
  size_t countV1 = 0, countV2 = 0;
  for (const auto& r : rows) {
    if (!o2IsMissing(r.vorder2_0) && r.vorder2_0 >= 1) ++countV2;
    if (!o2IsMissing(r.vorder1_0) && r.vorder1_0 >= 1) ++countV1;
  }
  const bool usingFirst = countV2 > countV1;
  std::stable_sort(rows.begin(), rows.end(), [&](const R& a, const R& b) {
    if (usingFirst) {
      return o2CmpKeys({a.vorder, a.eq0, a.sortvarCol, a.vo2, a.vo1, a.merge2, a.vord2, a.vord1},
                       {b.vorder, b.eq0, b.sortvarCol, b.vo2, b.vo1, b.merge2, b.vord2,
                        b.vord1}) < 0;
    }
    return o2CmpKeys({a.vorder, a.eq0, a.sortvarCol, a.vo1, a.vo2, a.merge2, a.vord1, a.vord2},
                     {b.vorder, b.eq0, b.sortvarCol, b.vo1, b.vo2, b.merge2, b.vord1,
                      b.vord2}) < 0;
  });

  O2Grid out;
  std::vector<int> rowtype1;
  out.reserve(rows.size());
  for (const auto& r : rows) {
    std::vector<std::string> line(k1 + 1);
    if (r.mi >= 0) {
      for (size_t c = 0; c < k1; ++c) line[c] = m[r.mi].v[c];
    }
    if (r.ui >= 0) {
      line[k1] = u[r.ui].cell;
      if (line[0].empty() && !u[r.ui].report.empty()) line[0] = u[r.ui].report;
    }
    out.push_back(std::move(line));
    rowtype1.push_back(r.mi >= 0 ? m[r.mi].rowtype : -2);  // -2 == missing
  }
  if (!out.empty()) {
    if (k1 == 2) out[0][1] = "(1)";
    out[0][k1] = "(" + std::to_string(k1) + ")";
  }
  for (size_t i = 0; i + 1 < out.size(); ++i) {
    if (rowtype1[i + 1] == 0) out[i][0] = variables2;
  }
  for (size_t i = 0; i < out.size(); ++i) {
    if (rowtype1[i] == 0 && (i == 0 || out[i - 1][0] != variables2)) out[i][0] = variables2;
  }
  return out;
}

// cleanFile's two borders (:9178-9200).
void o2Borders(const O2Grid& g, size_t* headRow, size_t* bottomRow) {
  const size_t n = g.size();
  auto rowmiss = [&](size_t i) {
    int c = 0;
    for (const auto& s : g[i]) c += s.empty() ? 0 : 1;
    return c;
  };
  size_t num = 0;
  int content = 1;
  while (content == 1 && num < n) {
    content = rowmiss(n - 1 - num);
    ++num;
  }
  *bottomRow = n - num + 1;
  size_t h = 1;
  std::string first;
  while (first.empty() && h <= n) {
    first = g[h - 1][0];
    ++h;
  }
  *headRow = h - 1;
}

std::string o2RenderLedger(const O2Grid& g) {
  std::string out;
  for (const auto& row : g) {
    for (size_t c = 0; c < row.size(); ++c) {
      if (c) out += '\t';
      out += row[c];
    }
    out += '\n';
  }
  return out;
}

O2Grid o2ParseLedger(const std::string& text) {
  O2Grid g;
  size_t pos = 0;
  size_t width = 0;
  while (pos < text.size()) {
    size_t nl = text.find('\n', pos);
    std::string line = text.substr(pos, nl == std::string::npos ? std::string::npos : nl - pos);
    pos = (nl == std::string::npos) ? text.size() : nl + 1;
    if (!line.empty() && line.back() == '\r') line.pop_back();
    std::vector<std::string> cells;
    size_t p = 0;
    while (true) {
      const size_t t = line.find('\t', p);
      cells.push_back(line.substr(p, t == std::string::npos ? std::string::npos : t - p));
      if (t == std::string::npos) break;
      p = t + 1;
    }
    width = std::max(width, cells.size());
    g.push_back(std::move(cells));
  }
  for (auto& r : g) r.resize(width);
  return g;
}

std::string o2Subinstr(std::string s, const std::string& from, const std::string& to) {
  if (from.empty()) return s;
  std::string out;
  size_t pos = 0;
  while (true) {
    const size_t at = s.find(from, pos);
    if (at == std::string::npos) {
      out += s.substr(pos);
      break;
    }
    out += s.substr(pos, at - pos) + to;
    pos = at + from.size();
  }
  return out;
}

// has_eqn: two or more `$` (:7212-7215).
bool o2HasEqn(const std::string& s) {
  const size_t a = s.find('$');
  return a != std::string::npos && s.find('$', a + 1) != std::string::npos;
}

struct O2TexOpts {
  bool fragment = false;
  bool landscape = false;
};

// _texout (:7059-7383) over the label-merged grid (1-based borders).
std::string o2RenderTex(O2Grid g, size_t headBorder, size_t bottomBorder, const O2TexOpts& o) {
  const size_t n = g.size();
  const size_t k = g.empty() ? 2 : std::max<size_t>(2, g[0].size());
  for (auto& r : g) r.resize(k);
  // `$<$` for the rows from bottomBorder on: v1 re-tokenised on blanks and `<`.
  for (size_t r = bottomBorder; r >= 1 && r <= n; ++r) {
    const std::string src = g[r - 1][0];
    std::vector<std::string> toks;
    std::string cur;
    for (char c : src) {
      if (c == ' ') {
        if (!cur.empty()) toks.push_back(cur);
        cur.clear();
      } else if (c == '<') {
        if (!cur.empty()) toks.push_back(cur);
        cur.clear();
        toks.push_back("<");
      } else {
        cur += c;
      }
    }
    if (!cur.empty()) toks.push_back(cur);
    std::string out;
    bool noSpace = false;
    for (const auto& t : toks) {
      if (t == "<") {
        out += "$<$";
        noSpace = true;
      } else if (noSpace) {
        out += t;
        noSpace = false;
      } else {
        out += " " + t;
      }
    }
    g[r - 1][0] = out;
  }
  for (size_t r = 1; r <= n; ++r) {
    std::string& v = g[r - 1][0];
    if (!o2HasEqn(v)) v = o2Subinstr(v, "_", "\\_");
    v = o2Subinstr(v, "%", "\\%");
    v = o2Subinstr(v, "#", "\\#");
    for (size_t c = 1; c < k; ++c) {
      std::string& s = g[r - 1][c];
      if (r <= headBorder) {
        if (!o2HasEqn(s)) s = o2Subinstr(s, "_", "\\_");
        s = o2Subinstr(s, "%", "\\%");
        s = o2Subinstr(s, "#", "\\#");
      }
      if (r >= headBorder + 1 && r <= bottomBorder) {
        s = o2Subinstr(s, "<", "$<$");
        s = o2Subinstr(s, ">", "$>$");
        s = o2Subinstr(s, "{", "\\{");
        s = o2Subinstr(s, "}", "\\}");
        s = o2Subinstr(s, "|", "$|$");
        s = o2Subinstr(s, "_", "\\_");
        s = o2Subinstr(s, "%", "\\$");
        s = o2Subinstr(s, "#", "\\#");
      }
    }
  }
  std::string out;
  if (!o.fragment) {
    const std::string pw = o.landscape ? "11in" : "8.5in";
    const std::string ph = o.landscape ? "8.5in" : "11in";
    out += std::string("\\documentclass[") + (o.landscape ? "landscape" : "") + "]{article}\n";
    out += "\\setlength{\\pdfpagewidth}{" + pw + "} \\setlength{\\pdfpageheight}{" + ph + "}\n";
    out += "\\begin{document}\n";
  }
  out += "\\begin{tabular}{l" + std::string(k - 1, 'c') + "} \\hline\n";
  const std::string numcols = std::to_string(k);
  for (size_t r = 1; r <= n; ++r) {
    std::string pad0, pad1, padN;
    if (bottomBorder < n && r >= bottomBorder + 1) {
      pad0 = "\\multicolumn{" + numcols + "}{c}{";
      padN = "} \\\\";
    }
    if (r <= bottomBorder) {
      pad1 = " & ";
      padN += " \\\\";
      if (headBorder != 0 && r == headBorder) padN += " \\hline";
      if (r == bottomBorder) padN += " \\hline";
    }
    std::string line = pad0 + g[r - 1][0] + pad1;
    for (size_t c = 1; c < k; ++c) {
      line += g[r - 1][c];
      if (c + 1 < k) line += pad1;
    }
    line += padN;
    out += line + "\n";
  }
  out += "\\end{tabular}\n";
  if (!o.fragment) out += "\\end{document}\n";
  return out;
}

// _xmlout + xmlstack (:7652-8112), without `legible`: one line, no newline.
std::string o2RenderXml(const O2Grid& g, size_t headBorder, size_t bottomBorder) {
  const size_t n = g.size();
  const size_t k = g.empty() ? 1 : g[0].size();
  std::string o;
  o += "<?xml version=\"1.0\" encoding=\"US-ASCII\" standalone=\"yes\"?>";
  o += "<?mso-application progid=\"Excel.Sheet\"?>";
  o += "<Workbook xmlns=\"urn:schemas-microsoft-com:office:spreadsheet\"";
  o += " xmlns:o=\"urn:schemas-microsoft-com:office:office\"";
  o += " xmlns:x=\"urn:schemas-microsoft-com:office:excel\"";
  o += " xmlns:ss=\"urn:schemas-microsoft-com:office:spreadsheet\"";
  o += " xmlns:html=\"http://www.w3.org/TR/REC-html40\">";
  o += "<DocumentProperties xmlns=\"urn:schemas-microsoft-com:office:office\">";
  o += "<Author></Author><LastAuthor></LastAuthor><Created></Created><LastSaved></LastSaved>";
  o += "<Company></Company><Version></Version></DocumentProperties>";
  o += "<ExcelWorkbook  xmlns=\"urn:schemas-microsoft-com:office:excel\">";
  o += "<ProtectStructure>False</ProtectStructure><ProtectWindows>False</ProtectWindows>";
  o += "</ExcelWorkbook><Styles>";
  o += "<Style ss:ID=\"Default\" ss:Name=\"Normal\"><Alignment ss:Vertical=\"Bottom\"/><Borders/>";
  o += "<Font ss:FontName=\"Calibri\" ss:Size=\"10\"/><Interior/><NumberFormat/><Protection/>";
  o += "</Style>";
  o += "<Style ss:ID=\"s1\"><Font ss:FontName=\"Calibri\" ss:Bold=\"1\" ss:Size=\"12\"/>";
  o += "<NumberFormat/></Style>";
  o += "<Style ss:ID=\"s21\"><Alignment ss:Horizontal=\"Center\" ss:Vertical=\"Bottom\"/>";
  o += "<NumberFormat/><Font ss:FontName=\"Calibri\" ss:Size=\"10\"/><Borders>";
  o += "<Border ss:Position=\"Top\" ss:LineStyle=\"Continuous\" ss:Weight=\"1\"/></Borders>";
  o += "</Style>";
  o += "<Style ss:ID=\"s22\"><Alignment ss:Horizontal=\"Center\" ss:Vertical=\"Bottom\"/>";
  o += "<Font ss:FontName=\"Calibri\" ss:Size=\"10\"/><NumberFormat/></Style>";
  o += "<Style ss:ID=\"s23\"><Alignment ss:Horizontal=\"Center\" ss:Vertical=\"Bottom\"/>";
  o += "<NumberFormat/><Font ss:FontName=\"Calibri\" ss:Size=\"10\"/><Borders>";
  o += "<Border ss:Position=\"Bottom\" ss:LineStyle=\"Continuous\" ss:Weight=\"1\"/></Borders>";
  o += "</Style>";
  o += "<Style ss:ID=\"s24\"><Font ss:FontName=\"Calibri\" ss:Size=\"10\"/><NumberFormat/>";
  o += "</Style>";
  o += "<Style ss:ID=\"s31\"><Font ss:FontName=\"Calibri\" ss:Size=\"10\"/><Borders>";
  o += "<Border ss:Position=\"Top\" ss:LineStyle=\"Continuous\" ss:Weight=\"1\"/></Borders>";
  o += "</Style>";
  o += "<Style ss:ID=\"s32\"><Font ss:FontName=\"Calibri\" ss:Size=\"10\"/><Borders/></Style>";
  o += "<Style ss:ID=\"s33\"><Font ss:FontName=\"Calibri\" ss:Size=\"10\"/><Borders>";
  o += "<Border ss:Position=\"Bottom\" ss:LineStyle=\"Continuous\" ss:Weight=\"1\"/></Borders>";
  o += "</Style>";
  o += "</Styles><Names></Names><Worksheet ss:Name=\"Sheet1\">";
  o += "<Table ss:ExpandedColumnCount=\"" + std::to_string(k) + "\" ss:ExpandedRowCount=\"" +
       std::to_string(n + 1) + "\" x:FullColumns=\"1\" x:FullRows=\"1\">";
  // Column 1's width follows the longest first cell between the borders.
  size_t len = 1;
  for (size_t r = headBorder; r >= 1 && r <= bottomBorder && r <= n; ++r) {
    len = std::max(len, g[r - 1][0].size());
  }
  int w1 = len > 10 ? static_cast<int>(len * 6) : 100;
  if (len > 40) w1 -= static_cast<int>(static_cast<double>(len) * .2);
  if (len > 60) w1 = 220;
  for (size_t c = 0; c < k; ++c) {
    o += "<Column ss:AutoFitWidth=\"0\" ss:Width=\"" + std::to_string(c == 0 ? w1 : 58) + "\"/>";
  }
  o += "<Row></Row>";
  auto esc = [](const std::string& s) {
    return o2Subinstr(o2Subinstr(s, "<", "&lt;"), ">", "&gt;");
  };
  auto stack = [&](size_t r, const char* style, const char* style1, bool firstOnly) {
    if (r < 1 || r > n) return;
    o += "<Row>";
    const size_t kk = firstOnly ? 1 : k;
    for (size_t c = 0; c < kk; ++c) {
      o += std::string("<Cell ss:StyleID=\"") + (c == 0 ? style1 : style) +
           "\"><Data ss:Type=\"String\">" + esc(g[r - 1][c]) + "</Data></Cell>";
    }
    o += "</Row>";
  };
  size_t total = 1;
  stack(total, "s21", "s31", false);
  ++total;
  for (size_t r = total, hb = headBorder; r <= hb; ++r) {
    stack(r, "s22", "s32", false);
    ++total;
  }
  stack(total, "s21", "s31", false);
  ++total;
  if (bottomBorder >= 1) {
    for (size_t r = total, last = bottomBorder - 1; r <= last; ++r) {
      stack(r, "s22", "s32", false);
      ++total;
    }
  }
  stack(total, "s23", "s33", false);
  ++total;
  // The note rows ("goldfish") -- written only when MORE than one row is left:
  // `if N > total` with total already one past the bottom border, so a table
  // with a single note line drops it (w2at o14).
  if (n > total) {
    for (size_t r = total; r <= n; ++r) stack(r, "s24", "s24", true);
  }
  o += "</Table><WorksheetOptions xmlns=\"urn:schemas-microsoft-com:office:excel\">";
  o += "<ProtectedObjects>False</ProtectedObjects><ProtectedScenarios>False</ProtectedScenarios>";
  o += "</WorksheetOptions></Worksheet></Workbook>";
  return o;
}

}  // namespace

ojson runOutreg2(const eval::ParsedCommand& parsed, SessionState& session) {
  // BEFORE the option net, deliberately. `word` is a real outreg2 option, so it
  // sits in the `unimplemented` set -- and the net would therefore answer
  // "valid Stata option, not yet implemented" (rc 198) for it. The honest
  // answer is that the whole OUTPUT FORMAT has no writer here, which is a
  // command-level refusal at rc 199, so it has to be decided first.
  if (hasFlag(parsed.opts, "word")) {
    return ojson(eval::refusedCommand("outreg2", "no_rtf_writer"));
  }
  if (auto err = eval::rejectUnknownOptions(parsed.opts, kVocabOutreg2.known,
                                            kVocabOutreg2.unimplemented)) {
    return ojson(*err);
  }

  std::vector<std::string> words = parsed.varlist;
  const std::optional<std::string> usingPath = takeUsing(words);
  if (!usingPath.has_value() || usingPath->empty()) {
    return tblSyntax("outreg2: `using FILENAME' is required");
  }
  // `outreg2 [m_a m_b] using ...` -- the stored estimates, one column each
  // Whatever else precedes `using` is a varlist, which keep()
  // supersedes.
  std::vector<std::string> estNames;
  std::string varlistSpec;
  {
    std::string pre = joinWith(words, " ");
    const size_t open = pre.find('[');
    const size_t close = pre.find(']');
    if (open != std::string::npos && close != std::string::npos && close > open) {
      estNames = splitWs(pre.substr(open + 1, close - open - 1));
      pre = pre.substr(0, open) + " " + pre.substr(close + 1);
    }
    varlistSpec = strutil::trim(pre);
  }

  const auto& opts = parsed.opts;
  // :465-469.
  if (optStr(opts, "keep").has_value() && optStr(opts, "drop").has_value()) {
    return tblErrorRc("cannot specify both keep( ) and drop( )", 198);
  }

  // ---- file names (outreg2) ---------------------------
  // A name with no dot gets `.txt`; a tex / xls-family suffix turns that
  // output ON and moves the ledger to <stem>.txt. The companions are always
  // <stem>.<suffix>. (Dots in a DIRECTORY name are not the extension -- the ado
  // reads them as one, which is its bug, not a format.)
  std::string userFile = *usingPath;
  {
    const size_t slash = userFile.find_last_of("/\\");
    const std::string base = slash == std::string::npos ? userFile : userFile.substr(slash + 1);
    if (base.find('.') == std::string::npos) userFile += ".txt";
  }
  const size_t lastDot = userFile.find_last_of('.');
  const std::string stem = userFile.substr(0, lastDot);
  const std::string suffix = userFile.substr(lastDot + 1);
  bool texOn = hasFlag(opts, "tex");
  bool excelOn = hasFlag(opts, "excel");
  std::string texFile = "tex", excelFile = "xml";
  std::string ledgerUser = userFile;
  if (suffix == "rtf" || suffix == "doc" || suffix == "docx") {
    return ojson(eval::refusedCommand("outreg2", "no_rtf_writer"));
  }
  if (suffix == "xls" || suffix == "xlsx" || suffix == "xml" || suffix == "xlm" ||
      suffix == "csv") {
    excelOn = true;
    excelFile = suffix;
    ledgerUser = stem + ".txt";
  }
  if (suffix == "tex") {
    texOn = true;
    ledgerUser = stem + ".txt";
  }
  // `replace` wins over `append`; neither means append (:519-528).
  const bool replaceMode = hasFlag(opts, "replace");

  O2TexOpts texOpts;
  if (auto tx = optStr(opts, "tex")) {
    for (const auto& w : splitWs(strutil::lower(dequote(*tx)))) {
      if (w.size() >= 1 && std::string("fragment").rfind(w, 0) == 0) {
        texOpts.fragment = true;
      } else if (w.size() >= 1 && std::string("landscape").rfind(w, 0) == 0) {
        texOpts.landscape = true;
      } else if (w == "nopretty" || w == "nopr") {
        // the default
      } else {
        return ojson(eval::unsupportedOption(
            "tex", "outreg2 tex(" + w + "): this build writes tex(), tex(fragment) and "
                   "tex(landscape); `" + w + "` is not implemented"));
      }
    }
  }

  // ---- decimals and formats (:5503-5543) -----------------------------------
  // `numlist int >=0 <=11 max=1` (:449-457): -1 = not a number, -2 = out of range.
  auto readDec = [&](const char* key) -> std::optional<int> {
    if (auto v = optStr(opts, key)) {
      const std::string t = strutil::trim(dequote(*v));
      try {
        size_t pos = 0;
        const int n = std::stoi(t, &pos);
        if (pos != t.size()) return -1;
        return (n >= 0 && n <= 11) ? n : -2;
      } catch (...) {
      }
      return -1;
    }
    return std::nullopt;
  };
  const std::optional<int> dec0 = readDec("dec"), bdec0 = readDec("bdec"),
                           sdec0 = readDec("sdec"), tdec0 = readDec("tdec"),
                           pdec0 = readDec("pdec"), rdec0 = readDec("rdec");
  for (const auto& [nm, v] : {std::pair<const char*, std::optional<int>>{"dec", dec0},
                              {"bdec", bdec0},
                              {"sdec", sdec0},
                              {"tdec", tdec0},
                              {"pdec", pdec0},
                              {"rdec", rdec0}}) {
    if (v.has_value() && *v == -1) {
      return tblErrorRc(std::string(nm) + "() invalid -- invalid numlist", 121);
    }
    if (v.has_value() && *v == -2) {
      return tblErrorRc(std::string(nm) +
                            "() invalid -- invalid number, outside of allowed range",
                        125);
    }
  }
  const int decBase = dec0.value_or(3);
  const int bdec = bdec0.value_or(decBase), sdec = sdec0.value_or(decBase),
            tdec = tdec0.value_or(decBase), pdec = pdec0.value_or(decBase),
            rdec = rdec0.value_or(decBase);
  int autoD = 3;
  if (auto av = optStr(opts, "auto")) {
    try {
      autoD = std::stoi(dequote(*av));
    } catch (...) {
      return tblErrorRc("auto() invalid -- integer required", 198);
    }
  }
  const int less = 0;
  // `dec()` or `bdec()` switches the adaptive shape off for EVERY statistic
  // (:5541-5543).
  const bool noauto = hasFlag(opts, "noauto") || dec0.has_value() || bdec0.has_value();

  // `adec(#)`: outreg2 `numlist int >=0 <=11 max=1`, each bound with its
  // own rc and message (w1b_adec_rule.log d2/g1/g2/g3).
  int adec = -1;
  if (auto av = optStr(opts, "adec")) {
    const std::vector<std::string> toks = splitWs(dequote(*av));
    if (toks.size() != 1) return tblErrorRc("adec() invalid -- single number required", 123);
    size_t pos = 0;
    double d = 0;
    bool numeric = true;
    try {
      d = std::stod(toks[0], &pos);
    } catch (const std::exception&) {
      numeric = false;
    }
    if (!numeric || pos != toks[0].size()) {
      return tblErrorRc("adec() invalid -- invalid number, outside of allowed range", 125);
    }
    if (d != std::trunc(d)) {
      return tblErrorRc("adec() invalid -- invalid number, noninteger value", 126);
    }
    if (d < 0 || d > 11) {
      return tblErrorRc("adec() invalid -- invalid number, outside of allowed range", 125);
    }
    adec = static_cast<int>(d);
    if (!optStr(opts, "addstat")) {
      return tblErrorRc("cannot choose adec option without addstat option", 198);
    }
  }

  // ---- stats() (:1491-1611) --------------------------------------------------
  const bool tstatOpt = hasFlag(opts, "tstat");
  // `nose` keeps the coefficient row only (StataBE: the file loses its two
  // standard-error rows, the legend stays), and is refused beside stats().
  const bool noSe = hasFlag(opts, "nose");
  std::vector<std::string> statsList;
  const bool statsGiven = optStr(opts, "stats").has_value();
  if (statsGiven && noSe)
    return tblErrorRc("cannot specify both st:ats( ) and nose options", 198);
  if (noSe) {
    statsList = {"coef"};
  } else if (statsGiven) {
    std::string spec = dequote(*optStr(opts, "stats"));
    for (char& c : spec) {
      if (c == ',') c = ' ';
    }
    statsList = splitWs(spec);
    if (statsList.empty()) return tblErrorRc("stats() invalid", 198);
    if (tstatOpt) return tblErrorRc("cannot specify both st:ats( ) and tstat options", 198);
  } else {
    statsList = {"coef", tstatOpt ? "tstat" : "se"};
  }
  for (auto& s : statsList) {
    s = strutil::lower(s);
    if (s != "coef" && s != "se" && s != "tstat" && s != "pval") {
      return ojson(eval::unsupportedOption(
          "stats", "outreg2 stats(): this build knows coef, se, tstat and pval; `" + s +
                       "` names none of them"));
    }
  }
  // parenthesis: the EVEN positions of the list (:5784-5800), `se` when none.
  std::vector<bool> paren(statsList.size(), false);
  std::string parenFirst;
  for (size_t i = 1; i < statsList.size(); i += 2) {
    paren[i] = true;
    if (parenFirst.empty()) parenFirst = statsList[i];
  }
  if (parenFirst.empty()) parenFirst = "se";
  // The legend (:6215-6243). `statsList` carries a leading blank when stats()
  // was typed, so only the option forms meet the exact "coef se" test.
  std::string statxt;
  if (!statsGiven && !tstatOpt) {
    statxt = "Standard errors";
  } else if (parenFirst == "se") {
    statxt = "Standard errors";
  } else if (parenFirst == "tstat") {
    statxt = "t-statistics";  // z when e(df_r) is missing, settled per model below
  } else {
    statxt = parenFirst;
  }
  // `stats()` other than (coef se) shows the dependent variable as its own,
  // empty, first row (:4225-4229).
  const bool depvarRow =
      statsGiven && !(statsList.size() == 2 && statsList[0] == "coef" && statsList[1] == "se");

  const bool label = hasFlag(opts, "label");
  // eval.varLabels is a PROJECTION of the variable plane, refreshed on demand
  // (session_state.h, BUG-208): the label column and the depvar ctitle read it.
  if (label) session.syncEvalVarLabels();
  const bool nocons = hasFlag(opts, "nocons");
  const bool noaster = hasFlag(opts, "noaster");
  const bool nonotes = hasFlag(opts, "nonotes");
  const bool noobs = hasFlag(opts, "noobs");
  const bool nor2 = hasFlag(opts, "nor2");
  const bool adjr2 = hasFlag(opts, "adjr2");

  std::vector<O2Token> keepToks, dropToks;
  bool keepGiven = false;
  if (auto kp = optStr(opts, "keep")) {
    keepToks = o2ExpandTokens(*kp, session);
    keepGiven = true;
  } else if (!varlistSpec.empty()) {
    keepToks = o2ExpandTokens(varlistSpec, session);
    keepGiven = true;
  }
  if (auto dp = optStr(opts, "drop")) dropToks = o2ExpandTokens(*dp, session);
  const std::vector<std::string> sortvar =
      optStr(opts, "sortvar") ? o2SortvarList(*optStr(opts, "sortvar"), session)
                              : std::vector<std::string>{};

  // ---- addstat, evaluated ONCE before the estimates loop (:610, :10245) ------
  std::vector<std::pair<std::string, std::string>> addstatRows;
  if (auto as = optStr(opts, "addstat")) {
    std::vector<std::string> items = o2SplitCommas(*as);
    std::vector<std::pair<std::string, std::string>> stats;
    for (size_t i = 0; i + 1 < items.size(); i += 2) stats.emplace_back(items[i], items[i + 1]);
    for (size_t j = 0; j < stats.size(); ++j) {
      const std::string& name = stats[j].first;
      const std::string expr = strutil::trim(stats[j].second);
      if (name.empty()) return tblErrorRc("empty strings not allowed in addstat() option", 6);
      std::optional<double> v = piet::evalScalarExpr(expr, session);
      if (!v.has_value() && session.datastore() != nullptr && session.datastore()->has(expr)) {
        // `local value = VAR` reads the first observation.
        v = piet::evalScalarExpr(expr + "[1]", session);
      }
      std::string cell;
      if (!v.has_value()) {
        // `local value = <expr>` (:10289): an e() reference that is missing
        // writes `.`; an r() one dereferences to nothing; any other name is
        // evaluated as an expression and dies there -- `addstat(Controls, Yes)`
        // is r(111) "Yes not found" (w2at o20, log 3846).
        if (expr.find("e(") != std::string::npos) {
          cell = kMissingCell;
        } else if (expr.rfind("r(", 0) == 0) {
          cell.clear();
        } else {
          return tblErrorRc(expr + " not found", 111);
        }
      } else if (adec >= 0) {
        const AdecCell ac = outreg2AdecCell(*v, adec);
        if (ac.seps == 1) {
          // BUG-493, followed bug-for-bug on the User's 2026-09-18 ruling: the
          // separator re-enters outreg2's addstat parse as a field separator. A
          // TRAILING pair is left without a value (r(198)); an earlier one pushes
          // the next pair's name into a value slot (`B not found`, r(111)).
          // w1b_adec_rule.log d4/corpus_big and d3.
          if (j + 1 < stats.size()) {
            return tblErrorRc(stats[j + 1].first + " not found", 111);
          }
          return tblErrorRc(
              "outreg2: `" + name + "' is " + ac.text + " under adec(" + std::to_string(adec) +
                  "), and outreg2 re-reads that thousands separator as an addstat() field"
                  " separator -- Stata answers `invalid syntax' here too. Lower adec() until"
                  " the separator no longer fits, or scale the statistic.",
              198);
        }
        if (ac.seps >= 2) {
          // The one place this build does NOT follow the ado: two separators make
          // outreg2 write a truncated value in a mis-parsed table (w1b_adec_rule3
          // t5, rc 0). Refused rather than published.
          return tblErrorRc(
              "outreg2: `" + name + "' is " + ac.text + " under adec(" + std::to_string(adec) +
                  "), which outreg2 2.x mis-parses into two rows with a truncated value (it"
                  " reports 9 for 9999999). This build refuses rather than write that table;"
                  " lower adec() or scale the statistic.",
              198);
        }
        cell = ac.text;
      } else {
        // BUG-494: no adec() -- `string(v)` (%9.0g) first, then autodigits2 on
        // that text (:10289-10301, :10406-10420).
        double vv = *v;
        const std::string g9 = piet::fmtStataDefaultG(*v);
        try {
          vv = std::stod(g9);
        } catch (...) {
        }
        cell = o2AutoDigits2Cell(vv, autoD, less);
      }
      addstatRows.emplace_back(name, cell);
    }
  }
  std::vector<std::pair<std::string, std::string>> addtextRows;
  if (auto at = optStr(opts, "addtext")) {
    std::vector<std::string> items = o2SplitCommas(*at);
    for (size_t i = 0; i < items.size(); i += 2) {
      addtextRows.emplace_back(items[i], i + 1 < items.size() ? items[i + 1] : std::string());
    }
  }
  std::vector<std::string> addnotes;
  if (auto an = optStr(opts, "addnote")) addnotes = o2SplitCommas(*an);

  // ---- the models -----------------------------------------------------------
  std::vector<Model> models;
  {
    ojson err;
    if (estNames.empty()) {
      Model m;
      if (!activeModel(session, "outreg2", &m, &err)) return err;
      models.push_back(std::move(m));
    } else {
      models = collectModels(session, "outreg2", estNames, &err);
      if (models.empty()) return err.is_null() ? tblErrorRc("estimation result not found", 111)
                                               : err;
    }
  }

  const std::string ledgerResolved = resolveUserPath(session, ledgerUser);
  bool fileExist = cecon::native_exists(ledgerResolved);
  bool append = !replaceMode;
  O2Grid ledger;  // the raw (pre-cleanFile) ledger
  if (append && fileExist) {
    bool ok = false;
    const std::string existing = readWholeFile(ledgerResolved, &ok);
    if (!ok) return tblError("outreg2: cannot read " + ledgerUser, "data_io");
    ledger = o2ParseLedger(existing);
    if (ledger.empty() || ledger[0].size() < 2) {
      return tblError("outreg2: " + ledgerUser + " is not an outreg2 table");
    }
  }
  std::vector<std::string> coefNamesAll;  // varname_list1 + varname_list2

  for (size_t e = 0; e < models.size(); ++e) {
    const Model& m = models[e];
    if (e == 1 && !append) append = true;  // :1708-1711
    const std::string depvar = m.snap.value("depvar", std::string());
    std::string ctDepvar = depvar;
    if (label && !depvar.empty()) {
      const std::string l = session.eval.varLabels.get(depvar);
      if (!l.empty()) ctDepvar = l;
    }
    std::string ctitleList;
    if (auto ct = optStr(opts, "ctitle")) {
      ctitleList = *ct;  // per-item quotes are o2SplitCommas' to strip
    } else if (!estNames.empty()) {
      ctitleList = m.name + ", " + ctDepvar;  // :2150-2152
    } else {
      ctitleList = ctDepvar;
    }
    std::vector<std::string> ctitles = o2SplitCommas(ctitleList);
    if (ctitles.empty()) ctitles.push_back(std::string());

    std::vector<O2Row> col;
    col.push_back({"", "(1)", "", -1});
    for (size_t i = 0; i < ctitles.size(); ++i) {
      col.push_back({i + 1 == ctitles.size() ? "VARIABLES" : "", ctitles[i], "", -1});
    }
    col.push_back({"", "", "", 0});

    // The body (makeFile :4080-4900, :5503-5937).
    const bool dfMissing = !m.escalars.contains("df_r") || !m.escalars["df_r"].is_number();
    auto num = [](const ojson& r, const char* k) -> double {
      if (!r.contains(k) || !r[k].is_number()) return kO2Missing;
      return r[k].get<double>();
    };
    auto statCell = [&](const std::string& st, double b, double se, double t, double p) {
      double v = kO2Missing;
      bool fixed = noauto;
      int d = 3;
      if (st == "coef") {
        v = b;
        fixed = fixed || bdec0.has_value();
        d = bdec;
      } else if (st == "se") {
        v = se;
        fixed = fixed || sdec0.has_value();
        d = sdec;
      } else if (st == "tstat") {
        v = t;
        fixed = fixed || tdec0.has_value();
        d = tdec;
      } else {
        v = p;
        fixed = fixed || pdec0.has_value();
        d = pdec;
      }
      return fixed ? o2Fixed(v, d) : o2AutoCell(v, autoD, less);
    };
    auto pushCoef = [&](const std::string& report, const std::string& varname, double b,
                        double se, double t, double p, bool omitted) {
      for (size_t si = 0; si < statsList.size(); ++si) {
        std::string cell;
        if (omitted) {
          cell = si == 0 ? "-" : std::string();
        } else {
          cell = statCell(statsList[si], b, se, t, p);
          if (statsList[si] == "coef" && !noaster && !o2IsMissing(p)) {
            const double ap = std::fabs(p);
            if (ap < .01) {
              cell += "***";
            } else if (ap < .05) {
              cell += "**";
            } else if (ap < .10) {
              cell += "*";
            }
          }
          if (paren[si] && !cell.empty()) cell = "(" + cell + ")";
        }
        col.push_back({si == 0 ? report : std::string(), cell, varname, 1});
      }
      coefNamesAll.push_back(varname);
    };
    if (depvarRow && !depvar.empty()) {
      pushCoef(depvar, depvar, kO2Missing, kO2Missing, kO2Missing, kO2Missing, false);
    }
    if (m.snap.contains("coeftable") && m.snap["coeftable"].is_array()) {
      // e(b) order: this build's coeftable leads with the intercept, e(b) (and
      // so outreg2's rows) ends with it -- o_plain.txt, w2at o05a.
      std::vector<const ojson*> order;
      for (const auto& r : m.snap["coeftable"]) {
        const std::string nm = r.value("name", std::string());
        if (nm != "_cons" && nm != "(intercept)") order.push_back(&r);
      }
      for (const auto& r : m.snap["coeftable"]) {
        const std::string nm = r.value("name", std::string());
        if (nm == "_cons" || nm == "(intercept)") order.push_back(&r);
      }
      for (const ojson* rp : order) {
        const ojson& r = *rp;
        std::string raw = r.value("name", std::string());
        if (raw == "(intercept)") raw = "_cons";
        const std::string name = o2StataName(raw);
        const bool isCons = name == "_cons";
        if (keepGiven && !isCons && !o2TokenHits(keepToks, name)) continue;
        if (!dropToks.empty() && o2TokenHits(dropToks, name)) continue;
        if (isCons && nocons) continue;
        // nobase (:4891): a base level is not reported.
        const size_t dot = name.find('.');
        if (dot != std::string::npos && name.find('#') == std::string::npos &&
            o2IsFactorPart(name) && name[dot - 1] == 'b') {
          continue;
        }
        const double b = num(r, "b"), se = num(r, "se");
        // fvts_type "o" (`2o.g`, `o.x2`) with a zero estimate prints `-` (:5729).
        const bool omitted = dot != std::string::npos && dot > 0 &&
                             name.find('#') == std::string::npos && name[dot - 1] == 'o' &&
                             b == 0 && se == 0;
        pushCoef(isCons ? "Constant" : name, name, b, se, num(r, "t"), num(r, "p"), omitted);
      }
    }
    col.push_back({"", "", "", 2});
    if (!noobs) {
      const double nObs = m.escalars.contains("N") && m.escalars["N"].is_number()
                              ? m.escalars["N"].get<double>()
                              : kO2Missing;
      col.push_back({"Observations", o2Fixed(nObs, 0), "", 3});
    }
    if (!nor2) {
      const char* key = adjr2 ? "r2_a" : "r2";
      const double rsq = m.escalars.contains(key) && m.escalars[key].is_number()
                             ? m.escalars[key].get<double>()
                             : kO2Missing;
      if (adjr2 && o2IsMissing(rsq)) {
        return tblErrorRc(
            "Adjusted R-squared (e(r2_a)) not defined; cannot use adjr2 option", 198);
      }
      if (!o2IsMissing(rsq)) {
        col.push_back({adjr2 ? "Adjusted R-squared" : "R-squared", o2Fixed(rsq, rdec), "", 3});
      }
    }
    for (const auto& [n, v] : addtextRows) col.push_back({n, v, "", 3});
    for (const auto& [n, v] : addstatRows) col.push_back({n, v, "", 3});
    if (!nonotes && (!append || !fileExist)) {
      std::string st = statxt;
      if (st == "t-statistics" && dfMissing) st = "z-statistics";
      // `robust = e(vcetype)`, and "Robust" prefixes the lowered legend
      // (:1931, :6235-6237): vce(robust) and vce(cluster) both say so. A stored
      // estimate answers with ITS e(), as `estimates restore` would install it.
      std::string vcetype = session.eval.eStrings.get("vcetype");
      if (!estNames.empty()) {
        auto sm = session.estimatesModels.find(m.name);
        vcetype = (sm != session.estimatesModels.end() && sm->second != nullptr)
                      ? sm->second->eStrings.get("vcetype")
                      : std::string();
      }
      if (vcetype == "Robust") st = "Robust " + strutil::lower(st);
      col.push_back({st + " in parentheses", "", "", 3});
      if (!noaster) col.push_back({"*** p<0.01, ** p<0.05, * p<0.1", "", "", 3});
    }
    if (!append || !fileExist) {
      for (const auto& n : addnotes) col.push_back({n, "", "", 3});
    }

    if (append && fileExist) {
      // appendFile strips the LABELS column and the title rows of what is on
      // disk before merging (:3276-3311).
      O2Grid f1 = ledger;
      bool hasLabels = false;
      for (const auto& r : f1) {
        if (r.size() >= 2 && r[1] == "LABELS") hasLabels = true;
      }
      if (hasLabels) {
        for (auto& r : f1) {
          if (r.size() >= 2) r.erase(r.begin() + 1);
        }
      }
      while (!f1.empty() && !f1[0][0].empty() && (f1[0].size() < 2 || f1[0][1].empty())) {
        f1.erase(f1.begin());
      }
      if (f1.empty() || f1[0].size() < 2) {
        return tblError("outreg2: " + ledgerUser + " is not an outreg2 table");
      }
      ledger = o2AppendFile(f1, col, sortvar, &coefNamesAll);
    } else {
      ledger.clear();
      for (const auto& r : col) ledger.push_back({r.report, r.cell});
      fileExist = true;
    }
  }

  // ---- cleanFile (:9087-9384) -----------------------------------------------
  size_t headRow = 0, bottomRow = 0;
  o2Borders(ledger, &headRow, &bottomRow);
  O2Grid clean = ledger;
  if (label) {
    std::map<std::string, std::string> labels;
    for (const auto& n : coefNamesAll) {
      if (n.empty() || labels.count(n)) continue;
      labels[n] = (n == "_cons" || n == "Constant") ? std::string("Constant")
                                                     : o2FvtsLabel(n, session);
    }
    labels["_cons"] = "Constant";
    labels["Constant"] = "Constant";
    for (size_t i = 0; i < clean.size(); ++i) {
      auto it = labels.find(clean[i][0]);
      std::string l = it == labels.end() ? std::string() : it->second;
      if (i + 1 == headRow) l = "LABELS";
      clean[i].insert(clean[i].begin() + 1, l);
    }
  }

  ojson out = ojson::array();
  {
    ojson werr =
        writeTextFileAtomic("outreg2", ledgerResolved, ledgerUser, o2RenderLedger(clean));
    if (!werr.is_null()) return werr;
  }

  // The grid the companions render: the label column merged into the name
  // column, LABELS -> VARIABLES (:2754-2766).
  O2Grid shown = clean;
  if (label) {
    for (auto& r : shown) {
      if (r.size() >= 2 && r[1].empty()) r[1] = r[0];
      r.erase(r.begin());
      if (r[0] == "LABELS") r[0] = "VARIABLES";
    }
  }
  if (texOn) {
    const std::string texUser = stem + "." + texFile;
    const std::string texResolved = resolveUserPath(session, texUser);
    ojson werr = writeTextFileAtomic("outreg2", texResolved, texUser,
                                     o2RenderTex(shown, headRow, bottomRow, texOpts));
    if (!werr.is_null()) return werr;
    out.push_back(displayBlock(texUser));
  }
  if (excelOn) {
    const std::string xmlUser = stem + "." + excelFile;
    const std::string xmlResolved = resolveUserPath(session, xmlUser);
    ojson werr = writeTextFileAtomic("outreg2", xmlResolved, xmlUser,
                                     o2RenderXml(shown, headRow, bottomRow));
    if (!werr.is_null()) return werr;
    out.push_back(displayBlock(xmlUser));
  }
  out.push_back(displayBlock("dir : seeout"));
  return out;
}

// Portions derived from texsave 1.6.1 (MIT).
// See THIRD_PARTY_NOTICES.
// ===========================================================================
// texsave -- the DATASET as a LaTeX table
// ===========================================================================

namespace {
// A variable's display format, as `list`/outsheet render it (graph_m4.cpp
// displayFormatOf's pattern).
std::string displayFormatOfVar(SessionState& session, Datastore& ds, const std::string& var) {
  if (ds.has(var)) {
    const std::vector<VarInfo> card = ds.formatCard(std::vector<std::string>{var});
    if (!card.empty()) return session.displayFormat(card.front());
  }
  VarInfo bare;
  bare.name = var;
  return session.displayFormat(bare);
}
}  // namespace

ojson runTexsave(const eval::ParsedCommand& parsed, SessionState& session) {
  // W2AK TW-TEXSAVE: derived from texsave 1.6.1 (t/texsave).
  if (auto err = eval::rejectUnknownOptions(parsed.opts, kVocabTexsave.known,
                                            kVocabTexsave.unimplemented)) {
    return ojson(*err);
  }
  if (!session.currentHbin().has_value() || session.datastore() == nullptr) {
    return ojson(eval::noDataLoaded());
  }
  const auto& opts = parsed.opts;
  auto has = [&](const char* k) { return hasFlag(opts, k); };
  auto str = [&](const char* k) { return optStr(opts, k); };
  auto sarg = [&](const char* k) {  // `string` options: one pair of outer quotes off
    return str(k).has_value() ? eoStringArg(str(k)) : std::string();
  };
  std::vector<std::string> words = parsed.varlist;
  const std::optional<std::string> usingPath = takeUsing(words);
  if (!usingPath.has_value() || usingPath->empty()) {
    return tblSyntax("using required");
  }
  Datastore* ds = session.datastore();
  // [varlist]: empty is every variable; `*`, `a-b` expand.
  std::vector<std::string> cols;
  if (words.empty()) {
    for (const auto& n : ds->names()) {
      if (!isHiddenColumn(n)) cols.push_back(n);
    }
  } else {
    for (const auto& w : words) {
      auto vr = loops::expandVarlist(w, session);
      if (vr.isError) return ojson(vr.error);
      for (const auto& t : vr.tokens) cols.push_back(t);
    }
  }
  for (const auto& v : cols) {
    if (!ds->has(v)) return tblErrorRc("variable " + v + " not found", 111);
  }
  const bool names = !has("nonames");
  const bool fix = !has("nofix");
  const bool endash = !has("noendash");
  const bool frag = has("frag");
  if (has("varlabels") && !names) {
    return tblErrorRc("option varlabels not allowed with option nonames", 198);
  }
  const std::string delimiter = str("delimiter").has_value() ? sarg("delimiter") : "&";
  const std::string horiz = "\\midrule";
  const std::string rowsep = str("rowsep").has_value() ? " \\addlinespace[" + sarg("rowsep") + "]" : "";
  const std::string headersep = str("headersep").has_value()
                                    ? " \\addlinespace[" + sarg("headersep") + "]"
                                    : " \\addlinespace[\\belowrulesep]";
  std::string userPath = dequote(*usingPath);
  if (userPath.find('.') == std::string::npos) userPath += ".tex";
  std::string marker = sarg("marker");
  if (str("label").has_value()) {
    if (!marker.empty()) return tblErrorRc("Cannot specify both the label and marker options", 198);
    marker = sarg("label");
  }
  std::string location = "tbp";
  if (str("location").has_value()) {
    location.clear();
    for (char c : sarg("location")) {
      if (c == ' ') continue;
      if (std::string("htbpH").find(c) == std::string::npos) {
        return tblErrorRc("location() option contains invalid characters", 198);
      }
      location += c;
    }
  }
  const std::string width = str("width").has_value() ? sarg("width") : "\\linewidth";
  // footnote(text [, size() width() addlinespace()])
  std::string footnote, footnotesize, footnotewidth, footaddls;
  if (auto fo = str("footnote")) {
    std::string lhs, rhs;
    eoSplitComma(*fo, &lhs, &rhs);
    const std::vector<std::string> t = eoTokens(lhs);
    footnote = t.empty() ? std::string() : t[0];
    for (const auto& [nm, arg] : eoSubopts(rhs)) {
      if (nm == "size") footnotesize = eoStringArg(arg);
      else if (nm == "width") footnotewidth = eoStringArg(arg);
      else if (nm == "addlinespace") footaddls = eoStringArg(arg);
      else return tblErrorRc("Invalid syntax for footnote() option", 198);
    }
  }
  std::string footnotespace;
  if (!footnote.empty()) footnotespace = "\\addlinespace[\\belowrulesep]";
  if (!footnote.empty() && !footaddls.empty()) footnotespace = "\\addlinespace[" + footaddls + "]";
  if (footnotewidth.empty()) footnotewidth = width;
  if (footnotesize.empty()) footnotesize = "footnotesize";
  std::string size = sarg("size");
  for (std::string* o : {&size, &footnotesize}) {
    if (o->empty()) continue;
    static const char* kSizes[] = {"tiny",  "scriptsize", "footnotesize", "small", "normalsize",
                                   "large", "Large",      "LARGE",        "huge",  "Huge"};
    bool isInt = true;
    for (char c : *o) {
      if (!std::isdigit(static_cast<unsigned char>(c))) isInt = false;
    }
    if (isInt) {
      const int k = std::stoi(*o);
      if (k < 1 || k > 10) return tblErrorRc("size() value must be between 1 and 10", 198);
      *o = kSizes[k - 1];
    } else {
      bool ok = false;
      for (const char* z : kSizes) {
        if (*o == z) ok = true;
      }
      if (!ok) return tblErrorRc(*o + " is an invalid option for size()", 198);
    }
  }
  const size_t nvars = cols.size();
  std::string align = sarg("align");
  if (align.empty()) {
    align = "@{}l";
    for (size_t i = 2; i <= nvars; ++i) align += "C";
    align += "@{}";
  }
  // ---- header ----------------------------------------------------------------------
  std::string headerHeaderlines;
  if (auto hl = str("headerlines")) {
    for (const auto& t : eoTokens(*hl)) headerHeaderlines += t + " \\tabularnewline ";
  }
  if (auto hl = str("headerlines2")) {
    for (const auto& t : eoTokens(*hl)) headerHeaderlines += t + " ";
  }
  std::string headerAutonumber;
  if (has("autonumber")) {
    for (size_t i = 1; i < nvars; ++i) headerAutonumber += delimiter + "{(" + std::to_string(i) + ")}";
    headerAutonumber += " \\tabularnewline";
    if (names) headerAutonumber += " " + horiz;
  }
  std::string headerColnames;
  if (names) {
    if (has("varlabels")) session.syncEvalVarLabels();
    for (size_t i = 0; i < nvars; ++i) {
      if (i) headerColnames += delimiter;
      const std::string lbl = has("varlabels") ? session.eval.varLabels.get(cols[i]) : cols[i];
      headerColnames += "{" + lbl + "}";
    }
    headerColnames += " \\tabularnewline";
  }
  std::string title = sarg("title");
  auto fixText = [&](std::string s, bool header) {
    for (const char* sym : {"_", "%", "#", "$", "&", "~"}) {
      if (header && std::string(sym) == "&") continue;
      s = eoReplaceAll(s, sym, std::string("\\") + sym);
    }
    if (!header) {
      s = eoReplaceAll(s, "{", "\\{");
      s = eoReplaceAll(s, "}", "\\}");
    }
    return eoReplaceAll(s, "^", "\\^{}");
  };
  if (fix) {
    headerColnames = fixText(headerColnames, true);
    footnote = fixText(footnote, false);
    title = fixText(title, false);
  }
  // ---- the data, outsheet-style (delimiter, nonames, noquote, nolabel) ------------
  const int64_t n = ds->nRows();
  std::vector<std::string> lines;
  {
    std::vector<std::string> fmts;
    for (const auto& c : cols) fmts.push_back(displayFormatOfVar(session, *ds, c));
    static const std::regex reEndash(R"((^|[^A-Za-z\-])-[0-9])");
    for (int64_t r = 0; r < n; ++r) {
      std::string line;
      for (size_t c = 0; c < nvars; ++c) {
        if (c) line += delimiter;
        const cecon::Column& col = ds->col(cols[c]);
        std::string cell;
        if (col.dtype == cecon::DType::Factor) {
          const int32_t code = col.codes()[r];
          if (code >= 0) cell = col.levels[static_cast<size_t>(code)];
          if (fix) cell = fixText(cell, false);
          if (endash) {
            std::smatch m;
            int guard = 0;
            while (std::regex_search(cell, m, reEndash) && guard++ < 1000) {
              const std::string s1 = m[0].str();
              std::string s2 = s1;
              const size_t dash = s2.find('-');
              s2.replace(dash, 1, "--");
              const size_t at = cell.find(s1);
              cell.replace(at, s1.size(), s2);
            }
          }
        } else {
          const double v = col.f64()[r];
          cell = std::isnan(v) ? std::string(".") : eoTrim(eval::applyStataFmt(v, fmts[c]));
        }
        line += cell;
      }
      lines.push_back(line);
    }
  }
  // hlines(): `\midrule ` before the group that follows each listed row
  std::vector<std::string> body;
  {
    std::vector<long long> hl;
    if (auto h = str("hlines")) {
      for (const auto& t : eoTokens(*h)) {
        long long v = 0;
        try {
          v = std::stoll(t);
        } catch (...) {
          return tblErrorRc("invalid numlist", 121);
        }
        if (std::llabs(v) > n) {
          return tblErrorRc("hlines() cannot cannot include values larger than " + std::to_string(n) +
                                ", the size of the table",
                            198);
        }
        if (v < 0) v = n + v;
        hl.push_back(v);
      }
      std::sort(hl.begin(), hl.end());
    }
    std::string pending;
    long long rowAt = 0;
    size_t gi = 0;
    for (size_t g = 0; g <= hl.size(); ++g) {
      const long long hi = g < hl.size() ? hl[g] : n;
      if (g > 0) pending += horiz + " ";
      (void)gi;
      for (; rowAt < hi && rowAt < n; ++rowAt) {
        body.push_back(pending + lines[static_cast<size_t>(rowAt)] + " \\tabularnewline" + rowsep);
        pending.clear();
      }
      if (g < hl.size()) rowAt = std::max(rowAt, hi);
    }
    if (!pending.empty()) body.push_back(pending);  // a trailing empty group
  }
  // ---- assemble (texsave) -----------------------------------------------
  std::vector<std::string> out;
  auto put = [&](const std::string& s) { out.push_back(s); };
  auto putTokens = [&](const char* k) {
    if (auto v = str(k)) {
      for (const auto& t : eoTokens(*v)) put(t);
    }
  };
  if (!has("dataonly")) {
    if (!frag) {
      put("\\documentclass{article}");
      put("\\usepackage{booktabs}");
      put("\\usepackage{tabularx}");
      put("\\usepackage[margin=1in]{geometry}");
      if (has("landscape")) put("\\usepackage{pdflscape}");
    }
    putTokens("preamble");
    if (!frag) {
      put("\\begin{document}");
      put("");
    }
    if (has("landscape")) put("\\begin{landscape}");
    if (str("geometry").has_value()) put("\\newgeometry{" + sarg("geometry") + "}");
    putTokens("headlines");
    if (has("sw")) {
      put("%TCIMACRO{\\TeXButton{B}{\\begin{table}[" + location + "] \\centering}}");
      put("%BeginExpansion");
    }
    put("\\begin{table}[" + location + "] \\centering");
    putTokens("tablelines");
    if (str("rowstretch").has_value()) put("\\renewcommand{\\arraystretch}{" + sarg("rowstretch") + "}");
    if (str("rowheight").has_value()) put("\\setlength\\extrarowheight{" + sarg("rowheight") + "}");
    if (str("colwidth").has_value()) put("\\setlength{\\tabcolsep}{" + sarg("colwidth") + "}");
    put("\\newcolumntype{R}{>{\\raggedleft\\arraybackslash}X}");
    put("\\newcolumntype{L}{>{\\raggedright\\arraybackslash}X}");
    put("\\newcolumntype{C}{>{\\centering\\arraybackslash}X}");
    put("");
    if (has("sw")) put("%EndExpansion");
    if (!title.empty()) put("\\caption{" + title + "}");
    if (!marker.empty()) put("\\label{" + marker + "}");
    if (!size.empty()) put("{\\" + size);
    put("\\begin{tabularx}{" + width + "}{" + align + "}");
    put("");
    put("\\toprule");
    if (!headerAutonumber.empty()) put(headerAutonumber);
    if (!headerHeaderlines.empty()) put(headerHeaderlines);
    if (!headerColnames.empty()) put(headerColnames);
    if (!headerHeaderlines.empty() || !headerAutonumber.empty() || !headerColnames.empty()) {
      put(horiz + headersep);
    }
  }
  for (const auto& b : body) put(b);
  if (!has("dataonly")) {
    put("\\bottomrule " + footnotespace);
    put("");
    put("\\end{tabularx}");
    if (!footnote.empty()) put("\\\\ \\parbox{" + footnotewidth + "}{\\" + footnotesize + " " + footnote + "}");
    if (!size.empty()) put("}");
    if (has("sw")) {
      put("%TCIMACRO{\\TeXButton{E}{\\end{table}}}%");
      put("%BeginExpansion");
    }
    put("\\end{table}");
    if (has("sw")) put("%EndExpansion");
    putTokens("footlines");
    if (str("geometry").has_value()) put("\\restoregeometry");
    if (has("landscape")) put("\\end{landscape}");
    if (!frag) put("\\end{document}");
  }
  std::string bytes;
  for (const auto& l : out) bytes += l + "\n";
  return emitTable("texsave", session, userPath, bytes, has("replace"), false);
}

// ===========================================================================
// texdoc / tex
//
// The open file is a PATH held in `session.eval.globals`, and every `tex` line
// opens-appends-closes. At the five-line scale the corpus uses that costs
// nothing measurable, and it avoids adding a SessionState field on a surface
// this lane does not own. The cost, stated: `$__piet_texdoc_path` is visible to
// `macro list`. That is the trade the plan made (decision 9).
// ===========================================================================

namespace {
const char* const kTexdocPathKey = "__piet_texdoc_path";
}  // namespace

ojson runTexdoc(const eval::ParsedCommand& parsed, SessionState& session) {
  if (auto err = eval::rejectUnknownOptions(parsed.opts, kVocabTexdoc.known,
                                            kVocabTexdoc.unimplemented)) {
    return ojson(*err);
  }
  if (parsed.varlist.empty()) {
    return tblSyntax("texdoc: a subcommand is required (init or close)");
  }
  const std::string sub = strutil::lower(parsed.varlist[0]);

  static const std::set<std::string> kNotImplementedSubs = {"do", "stlog", "local", "graph",
                                                            "write", "substitute", "append"};
  if (kNotImplementedSubs.count(sub) != 0) {
    return ojson(eval::unsupportedCommand("texdoc " + sub, std::nullopt, std::nullopt,
                                          "refused:not_implemented_sub",
                                          eval::UnsupportedCommandKind::kNotImplemented));
  }

  if (sub == "close") {
    // W2AK TW-TEXDOC: nothing open is a no-op note, rc 0 (texdoc.ado close).
    if (session.eval.globals.get(kTexdocPathKey).empty()) {
      return displayBlock("(texdoc not initialized; nothing to do)");
    }
    const std::string path = session.eval.globals.get(kTexdocPathKey);
    session.eval.globals.erase(kTexdocPathKey);
    return displayBlock("(texdoc closed " + path + ")");
  }
  if (sub != "init") {
    return ojson(eval::unsupportedCommand("texdoc " + sub, std::nullopt, std::nullopt, "piet_gap",
                                          eval::UnsupportedCommandKind::kUnknownSubcommand));
  }

  std::vector<std::string> words(parsed.varlist.begin() + 1, parsed.varlist.end());
  // `texdoc init using f.tex` and `texdoc init f.tex` are both written in the
  // wild; `using` is optional here, unlike everywhere else in this file.
  std::optional<std::string> path = takeUsing(words);
  if (!path.has_value() && !words.empty()) path = dequote(words[0]);
  if (!path.has_value() || path->empty()) {
    return tblSyntax("texdoc init: a filename is required");
  }
  // W2AK TW-TEXDOC (texdoc): outside `texdoc do`, init is a no-op
  // unless `force` is given -- no file is written (w2at wt_x03 reads it back
  // r(601)). `force` only bypasses that check; it is not `replace` (wt_x05:
  // an existing file without replace/append is r(602) and stays intact).
  if (!hasFlag(parsed.opts, "force")) {
    return displayBlock("(texdoc do not running; nothing to do)");
  }
  const std::string userPath = ensureExt(*path, ".tex");
  const std::string resolved = resolveUserPath(session, userPath);
  const bool replace = hasFlag(parsed.opts, "replace");
  const bool append = hasFlag(parsed.opts, "append");
  ojson guard = guardOverwrite("texdoc init", resolved, userPath, replace, append);
  if (!guard.is_null()) return guard;
  if (!append) {
    // TRUNCATE at init, so a `replace` that is never followed by a `tex` line
    // still leaves an empty file rather than the previous run's contents.
    ojson werr = writeTextFileAtomic("texdoc init", resolved, userPath, std::string());
    if (!werr.is_null()) return werr;
  }
  session.eval.globals.set(kTexdocPathKey, resolved);
  return displayBlock("(texdoc opened " + userPath + ")");
}

ojson runTexdocLine(const eval::ParsedCommand& parsed, SessionState& session) {
  // NO reject net here, and that is deliberate rather than an omission: `tex`
  // takes no options at all -- everything after the verb is literal TeX -- so
  // the tokenizer's comma split (`tex Hello, world`) produces a phantom option
  // key from ordinary prose. Reading the whole raw remainder is the only way
  // the text survives, and there is no option vocabulary to be silent about.
  std::string text;
  {
    const std::string& raw = parsed.raw;
    size_t b = raw.find_first_not_of(" \t");
    if (b != std::string::npos) {
      size_t e = b;
      while (e < raw.size() &&
             (std::isalnum(static_cast<unsigned char>(raw[e])) != 0 || raw[e] == '_')) {
        ++e;
      }
      // Every blank between the verb and the text goes, as in tex.ado's `0':
      // StataBE writes 007's `tex<TAB><TAB><TAB>& (1) ...` (Table_A3.tex) from
      // its `&`, the inner tab stops kept.
      if (e < raw.size()) {
        const size_t t = raw.find_first_not_of(" \t", e);
        if (t != std::string::npos) text = raw.substr(t);
      }
    }
  }
  // A bare `tex` writes a blank line, which is how texdoc users separate
  // paragraphs; it is not an error.

  const std::string resolved = session.eval.globals.get(kTexdocPathKey);
  if (resolved.empty()) {
    // tex: no open document is a note, rc 0 (w2at wt_x07).
    return displayBlock("(texdoc not initialized; nothing to do)");
  }
  bool ok = false;
  std::string existing;
  if (cecon::native_exists(resolved)) {
    existing = readWholeFile(resolved, &ok);
    if (!ok) return tblError("tex: cannot read the open texdoc file", "data_io");
  }
  existing += text;
  existing += "\n";
  // Same temp+rename writer as every other file in this TU: the destination IS
  // the source on every `tex` line after the first.
  ojson werr = writeTextFileAtomic("tex", resolved, "the open texdoc file", existing);
  if (!werr.is_null()) return werr;
  return ojson::array();  // silent success -- texdoc lines produce no output
}

// ===========================================================================
// frmttable -- a session MATRIX as a LaTeX table (TeX only)
// ===========================================================================

// --- frmttable's own LaTeX, which is not estout's ---------------------------
// This writer used to hand its Table to `renderTex`, the estout/esttab emitter,
// and the two shapes are not the same table. Measured against the ado
// (tests/eval/golden/aer_w1/w1b_frmttable_{base,names,cells2}.log), on the
// SIMPLEST possible call -- `frmttable using f.tex, statmat(N) sdec(2) tex`:
//   oracle  \begin{tabular}{ccc}
//           \hline \noalign{\smallskip}1.00 & 2.00 & 3.00\\
//   before  \begin{tabular}{lccc}
//           \hline
//            & c1 & c2 & c3 \\
//           \hline
//           r1 & 1.00 & 2.00 & 3.00 \\
// -- a stub column and a header row that the oracle does not emit at all, plus a
// different rule idiom. Four things had to be measured before this could be
// written, and each one is a rule no amount of reading the option list gives:
//
//  1. DEFAULT-SPELLED matrix names count as ABSENT. Every Stata matrix has
//     names, and `matrix N = (1,2,3\4,5,6)` carries `r1 r2` / `c1 c2 c3` -- for
//     which the oracle emits no stub and no header (base h1). Setting those SAME
//     names explicitly changes nothing (names i2), and naming them anything else
//     turns the stub or the header on (i3 rownames only, i4 colnames only).
//  2. The TABLE WIDTH is the wider of the data (plus stub) and the longest
//     ctitles row: `ctitles("", "c1", "c2", "c3")` over a three-column matrix
//     with no stub gives `{cccc}` and pads every data row with a trailing empty
//     cell (base h5), while a SHORTER ctitles pads the header instead (i8).
//  3. `rtitles` longer than the matrix ADDS rows (ia: a third row of empty data).
//  4. The default numeric format carries THOUSANDS SEPARATORS -- 1234.5 at
//     sdec(2) is `1,234.50` with no sfmt() at all (cells2 k6), which this build
//     was dropping. And the sdec() list's LAST entry repeats for the remaining
//     columns (k2: `sdec(0,1)` over three columns is 0, 1, 1), while extra
//     entries are ignored (k4).
namespace {

// frmttable's `%fc`: fixed decimals with thousands separators on the integer
// part. A missing value is an EMPTY cell, which is not a flourish -- paper 004
// builds three of its rows out of `J(1,7,.)` spacers (base j1/j2).
std::string frmtCell(double v, int dec) {
  if (std::isnan(v)) return std::string();
  char buf[64];
  std::snprintf(buf, sizeof(buf), "%.*f", dec, v);
  std::string out(buf);
  const size_t dot = out.find('.');
  const size_t start = (!out.empty() && out[0] == '-') ? 1 : 0;
  const size_t end = (dot == std::string::npos) ? out.size() : dot;
  for (size_t at = end; at > start + 3;) {
    at -= 3;
    out.insert(at, ",");
  }
  return out;
}

// `rtitles("a" \ "b")` / `ctitles("", "x" \ "", "y")`: rows are separated by a
// top-level backslash and cells within a row by a comma. Quote-aware on both,
// which is what lets a cell hold LaTeX -- `"\makecell[l]{\it{Panel A}}"` is one
// cell and its backslashes are not separators (names i7).
std::vector<std::vector<std::string>> frmtTitleRows(const std::string& raw) {
  std::vector<std::vector<std::string>> rows;
  // SPLIT FIRST, DEQUOTE EACH CELL -- the same rule `parseLabelPairs` states a
  // few hundred lines up, and the first cut of this function broke it. Dequoting
  // `"a" \ "b"` FIRST strips the outer pair and leaves `a" \ "b`, whose remaining
  // quotes make the splitter's quote tracking swallow the separator, so every
  // row collapsed into one cell reading `a" \ "b`. It reproduced on nine of the
  // nineteen oracle arms at once.
  for (const auto& r : splitTopLevel(raw, '\\')) {
    std::vector<std::string> cells;
    for (const auto& c : splitTopLevel(r, ',')) cells.push_back(dequote(strutil::trim(c)));
    rows.push_back(cells);
  }
  return rows;
}

// Stata's own default names for an unnamed matrix, which frmttable treats as no
// names at all.
bool frmtDefaultNames(const std::vector<std::string>& names, const char* stem) {
  for (size_t i = 0; i < names.size(); ++i) {
    if (names[i] != stem + std::to_string(i + 1)) return false;
  }
  return !names.empty();
}

}  // namespace

namespace {
const char* const kFrmtStorePrefix = "__piet_frmttable_";

// One table's inputs, as stored for `replay` (statmat copied by value -- the
// matrix may change before the replay).
ojson frmtInputs(const eval::ParsedCommand& parsed, const eval::EvalMatrix& mat) {
  ojson in;
  in["data"] = ojson::array();
  for (const auto& row : mat.data) {
    ojson r = ojson::array();
    for (double v : row) r.push_back(std::isnan(v) ? ojson(nullptr) : ojson(v));
    in["data"].push_back(r);
  }
  in["rownames"] = mat.rownames;
  in["colnames"] = mat.colnames;
  for (const char* k : {"sdec", "sfmt", "rtitles", "ctitles", "title", "note"}) {
    if (auto v = optStr(parsed.opts, k)) in[k] = *v;
  }
  return in;
}

// Render stored inputs as frmttable's LaTeX (the measured rules above).
bool frmtRenderTex(const ojson& in, bool fragment, bool nocenter, std::string* out,
                   std::string* err) {
  eval::EvalMatrix m;
  for (const auto& r : in["data"]) {
    std::vector<double> row;
    for (const auto& v : r) row.push_back(v.is_number() ? v.get<double>() : kEoNaN);
    m.data.push_back(row);
  }
  m.rownames = in.value("rownames", std::vector<std::string>{});
  m.colnames = in.value("colnames", std::vector<std::string>{});
  const eval::EvalMatrix* mat = &m;
  auto opt = [&](const char* k) -> std::optional<std::string> {
    if (in.contains(k) && in[k].is_string()) return in[k].get<std::string>();
    return std::nullopt;
  };
  // sdec(): a comma list, per column, LAST ENTRY REPEATING (cells2 k2/k4).
  std::vector<int> sdec;
  if (auto sd = opt("sdec")) {
    for (const auto& tok : splitTopLevel(dequote(*sd), ',')) {
      try {
        size_t pos = 0;
        const int n = std::stoi(strutil::trim(tok), &pos);
        if (n >= 0 && n <= 12) sdec.push_back(n);
      } catch (const std::exception&) {
      }
    }
  }
  if (sdec.empty()) sdec.push_back(3);
  auto decFor = [&](size_t c) { return sdec[std::min(c, sdec.size() - 1)]; };
  // sfmt(): per column, last repeating, an empty trailing item ignored; `f`
  // is fixed WITHOUT thousands separators, `fc` (the default) with them
  // (w2at wt_f03 `2001.000` under sfmt(f,f,f,)).
  std::vector<std::string> sfmt;
  if (auto sf = opt("sfmt")) {
    for (const auto& tok : splitTopLevel(dequote(*sf), ',')) {
      const std::string t = strutil::trim(tok);
      sfmt.push_back(t);
    }
    while (!sfmt.empty() && sfmt.back().empty()) sfmt.pop_back();
    for (const auto& f : sfmt) {
      if (f != "f" && f != "fc" && !f.empty()) {
        *err = "frmttable: sfmt(" + f + ") is not implemented in this build; f and fc are";
        return false;
      }
    }
  }
  auto cellFor = [&](double v, size_t c) {
    const std::string f = sfmt.empty() ? std::string("fc") : sfmt[std::min(c, sfmt.size() - 1)];
    if (std::isnan(v)) return std::string();
    if (f == "f") {
      char buf[64];
      std::snprintf(buf, sizeof(buf), "%.*f", decFor(c), v);
      return std::string(buf);
    }
    return frmtCell(v, decFor(c));
  };

  std::vector<std::vector<std::string>> ctRows;
  if (auto ct = opt("ctitles")) ctRows = frmtTitleRows(*ct);
  std::vector<std::string> rtitles;
  if (auto rt = opt("rtitles")) {
    for (const auto& row : frmtTitleRows(*rt)) rtitles.push_back(row.empty() ? std::string() : row[0]);
  }
  const bool rowNamesReal = !mat->rownames.empty() && !frmtDefaultNames(mat->rownames, "r");
  const bool colNamesReal = !mat->colnames.empty() && !frmtDefaultNames(mat->colnames, "c");
  const bool stub = !rtitles.empty() || rowNamesReal;
  if (ctRows.empty() && colNamesReal) {
    std::vector<std::string> head;
    if (stub) head.push_back(std::string());
    for (const auto& c : mat->colnames) head.push_back(c);
    ctRows.push_back(head);
  }
  const size_t dataCols = mat->data.empty() ? mat->colnames.size() : mat->data[0].size();
  size_t width = dataCols + (stub ? 1 : 0);
  for (const auto& r : ctRows) width = std::max(width, r.size());
  const size_t nRows = std::max(mat->data.size(), rtitles.size());
  auto joinRow = [&](std::vector<std::string> cells) {
    cells.resize(width);
    return joinWith(cells, " & ") + "\\\\";
  };
  std::vector<std::string> lines;
  if (!fragment) lines.push_back("\\documentclass[]{article}\\pagestyle{empty}\\begin{document}");
  if (!nocenter) lines.push_back("\\begin{center}");
  if (auto ti = opt("title")) {
    lines.push_back("\\smallskip\\begin{large}" + dequote(*ti) + "\\end{large}\\\\");
    lines.push_back("\\smallskip");
  }
  std::string spec = stub ? "l" : "";
  for (size_t c = (stub ? 1 : 0); c < width; ++c) spec += "c";
  lines.push_back("\\begin{tabular}{" + spec + "}");
  bool first = true;
  for (const auto& hr : ctRows) {
    lines.push_back((first ? "\\hline \\noalign{\\smallskip}" : "") + joinRow(hr));
    first = false;
  }
  for (size_t r = 0; r < nRows; ++r) {
    std::vector<std::string> cells;
    if (stub) {
      cells.push_back(r < rtitles.size()
                          ? rtitles[r]
                          : (r < mat->rownames.size() && rowNamesReal ? mat->rownames[r]
                                                                      : std::string()));
    }
    if (r < mat->data.size()) {
      for (size_t c = 0; c < mat->data[r].size(); ++c) cells.push_back(cellFor(mat->data[r][c], c));
    }
    std::string prefix;
    if (first) prefix = "\\hline \\noalign{\\smallskip}";
    else if (r == 0 && !ctRows.empty()) prefix = "\\noalign{\\smallskip}\\hline \\noalign{\\smallskip}";
    lines.push_back(prefix + joinRow(cells));
    first = false;
  }
  lines.push_back("\\noalign{\\smallskip}\\hline\\end{tabular}\\\\");
  if (auto n = opt("note")) {
    lines.push_back("\\begin{footnotesize}" + dequote(*n) + "\\end{footnotesize}\\\\");
    lines.push_back("\\smallskip");
  }
  if (!nocenter) lines.push_back("\\end{center}");
  if (!fragment) lines.push_back("\\end{document}");
  out->clear();
  for (const auto& l : lines) *out += l + "\n";
  return true;
}
}  // namespace

ojson runFrmttable(const eval::ParsedCommand& parsed, SessionState& session) {
  if (auto err = eval::rejectUnknownOptions(parsed.opts, kVocabFrmttable.known,
                                            kVocabFrmttable.unimplemented)) {
    return ojson(*err);
  }
  std::vector<std::string> words = parsed.varlist;
  const std::optional<std::string> usingPath = takeUsing(words);
  const std::optional<std::string> statmat = optStr(parsed.opts, "statmat");
  ojson inputs;
  if (statmat.has_value()) {
    // With statmat() the table is built from it; replay() is ignored and
    // store() names it (frmttable; w2at wt_f04).
    const eval::EvalMatrix* mat = session.eval.findMatrix(dequote(*statmat));
    // An absent matrix ends in Mata's conformability error, r(3200) (wt_f06).
    if (mat == nullptr) return tblErrorRc("conformability error", 3200);
    inputs = frmtInputs(parsed, *mat);
    const std::string storeName =
        optStr(parsed.opts, "store").has_value() ? strutil::trim(dequote(*optStr(parsed.opts, "store")))
                                                 : std::string();
    session.eval.globals.set(kFrmtStorePrefix + storeName, inputs.dump());
  } else if (hasFlag(parsed.opts, "replay")) {
    const std::string name = optStr(parsed.opts, "replay").has_value()
                                 ? strutil::trim(dequote(*optStr(parsed.opts, "replay")))
                                 : std::string();
    const std::string blob = session.eval.globals.get(kFrmtStorePrefix + name);
    if (blob.empty()) return tblErrorRc("frmttable: no stored table" + (name.empty() ? std::string() : " " + name), 198);
    try {
      inputs = ojson::parse(blob);
    } catch (...) {
      return tblErrorRc("frmttable: the stored table could not be read", 198);
    }
  } else {
    return tblSyntax("frmttable: statmat(MATRIX) or replay is required by this build -- the "
                     "forms that build a table WITHOUT one (addrows) need frmttable's accumulator, "
                     "which is not implemented");
  }
  if (!usingPath.has_value() || usingPath->empty()) {
    // The screen form: the table is built and kept as the stored table, rc 0
    // (w2at wt_f01a, log 4395-4410).
    return ojson::array();
  }
  // frmttable's DEFAULT file is RTF (.doc) -- base h8 -- which this build has no
  // writer for; refused BY NAME rather than writing LaTeX under a .doc name.
  if (!hasFlag(parsed.opts, "tex")) {
    return ojson(eval::refusedCommand("frmttable", "no_rtf_writer"));
  }
  std::string body, rerr;
  if (!frmtRenderTex(inputs, hasFlag(parsed.opts, "fragment"), hasFlag(parsed.opts, "nocenter"),
                     &body, &rerr)) {
    return ojson(eval::unsupportedOption("sfmt", rerr));
  }
  return emitTable("frmttable", session, ensureExt(*usingPath, ".tex"), body,
                   hasFlag(parsed.opts, "replace"), false);
}

// ===========================================================================
// regsave / parmest -- the estimates as a DATASET (plan design decision 9)
//
// One builder, two column vocabularies. The rows go out as CSV, `cobra convert`
// turns them into an .hbin, and then either `export-dta` writes the requested
// file (session untouched) or `session.load` makes the table the dataset in
// memory. The CI columns come from the coeftable the ENGINE produced -- this
// build does not own an inverse-t, and recomputing a 90% interval from a 95%
// fit with a normal quantile would be a quietly wrong number, which is why a
// `level()` other than the fitted one is refused below rather than approximated.
// ===========================================================================

namespace {

struct DsColumn {
  std::string name;
  bool isString = false;
  std::vector<double> num;
  std::vector<std::string> str;
};

// CSV escaping for the convert step. `cobra convert --delim ,` reads a plain
// RFC4180 file; a variable name or label containing a comma would otherwise
// shift every later column by one.
std::string csvCell(const std::string& s) {
  if (s.find_first_of(",\"\n\r") == std::string::npos) return s;
  std::string out = "\"";
  for (char c : s) {
    if (c == '"') out += '"';
    out += c;
  }
  out += '"';
  return out;
}

// Build the .hbin. Returns the path, or an error block through `errOut`.
bool materializeDataset(SessionState& session, const std::string& cmd,
                        const std::vector<DsColumn>& cols, size_t nRows, std::string* pathOut,
                        ojson* errOut) {
  if (cols.empty() || nRows == 0) {
    *errOut = tblError(cmd + ": the estimation has no coefficients to save");
    return false;
  }
  // mintScratchPath always names a ".hbin"; the CSV beside it has to be
  // registered under its OWN name or the scratch census counts it as an orphan
  // (BUG-389 -- sweepOrphanScratch only removes paths it knows).
  const std::string hbin = mintScratchPath(session.workdir(), cmd + "_ds");
  std::string csv = mintScratchPath(session.workdir(), cmd + "_csv");
  forgetScratchMint(csv);
  csv = csv.substr(0, csv.size() - 5) + ".csv";  // 5 == strlen(".hbin")
  registerScratchMint(csv);
  {
    std::ofstream f(cecon::to_native_path(csv), std::ios::binary);
    if (!f) {
      *errOut = tblError(cmd + ": could not write the intermediate table", "data_io");
      return false;
    }
    for (size_t c = 0; c < cols.size(); ++c) {
      if (c) f << ",";
      f << csvCell(cols[c].name);
    }
    f << "\n";
    f.setf(std::ios::fmtflags(0), std::ios::floatfield);
    f.precision(17);
    for (size_t r = 0; r < nRows; ++r) {
      for (size_t c = 0; c < cols.size(); ++c) {
        if (c) f << ",";
        if (cols[c].isString) {
          f << csvCell(r < cols[c].str.size() ? cols[c].str[r] : std::string());
        } else {
          const double v = r < cols[c].num.size() ? cols[c].num[r]
                                                  : std::numeric_limits<double>::quiet_NaN();
          // An EMPTY field is how the loader spells a missing value; writing
          // "nan" would land a literal token in a numeric column and writing 0
          // would invent an estimate.
          if (!std::isnan(v)) f << v;
        }
      }
      f << "\n";
    }
    f.flush();
    if (!f) {
      *errOut = tblError(cmd + ": failed writing the intermediate table (disk full?)", "data_io");
      return false;
    }
  }
  const CobraRunResult cr =
      runCobra({"convert", "--data", csv, "--out", hbin, "--delim", ","});
  if (!cr.ok || !cecon::native_exists(hbin)) {
    *errOut = tblError(cmd + ": could not build the result dataset");
    return false;
  }
  *pathOut = hbin;
  return true;
}

// Common tail: `using`/`saving()` writes a .dta and leaves the session alone;
// otherwise the built table BECOMES the dataset in memory.
ojson finishDataset(SessionState& session, const std::string& cmd, const std::string& hbin,
                    const std::optional<std::string>& outUser, bool replace, bool appendUsing,
                    const std::vector<std::string>& newVars) {
  if (outUser.has_value() && !outUser->empty()) {
    const std::string userPath = ensureExt(*outUser, ".dta");
    const std::string resolved = resolveUserPath(session, userPath);
    // `append` is a legitimate reason for the destination to already exist, so
    // it counts as permission here -- without this an `append` onto the file it
    // was asked to extend errors with "already exists".
    ojson guard = guardOverwrite(cmd, resolved, userPath, replace, appendUsing);
    if (!guard.is_null()) return guard;
    std::string src = hbin;
    if (appendUsing && cecon::native_exists(resolved)) {
      // `append` stacks onto the EXISTING saved table -- one row block per
      // model across a do-file, which is what regsave's append means.
      //
      // The existing file is a `.dta`, and `cobra append --using` opens its
      // argument with Dataset::open, which reads `.hbin` ONLY (src/io/
      // columnar.cpp:724 parses an hbin header). Handing it the .dta directly
      // fails; worse, a failure here after the export below would already have
      // replaced the file the append was supposed to extend. So it is imported
      // first, and every step is checked before anything touches the
      // destination.
      const std::string prior = mintScratchPath(session.workdir(), cmd + "_prior");
      const CobraRunResult imp =
          runCobra({"import-dta", "--data", resolved, "--out", prior});
      if (!imp.ok || !cecon::native_exists(prior)) {
        return tblError(cmd + ": cannot read " + userPath +
                        " to append to it (append needs a .dta this build can read)");
      }
      const std::string merged = mintScratchPath(session.workdir(), cmd + "_app");
      const CobraRunResult cr =
          runCobra({"append", "--data", prior, "--using", src, "--out", merged});
      if (!cr.ok || !cecon::native_exists(merged)) {
        return tblError(cmd + ": could not append to " + userPath);
      }
      src = merged;
    }
    // export-dta writes the destination directly, so it goes to a sibling temp
    // first and is renamed into place -- the same guarantee every other writer
    // in this file gives, and the reason a failed export cannot destroy the
    // file it was meant to replace.
    const std::string tmp = resolved + ".piet_tmp";
    const CobraRunResult cr = runCobra({"export-dta", "--data", src, "--out", tmp});
    if (!cr.ok || !cecon::native_exists(tmp)) {
      std::error_code rmEc;
      std::filesystem::remove(cecon::to_native_path(tmp), rmEc);
      return tblError(cmd + ": could not write " + userPath +
                      " (this build needs a cobra engine with `export-dta')");
    }
    std::error_code mvEc;
    std::filesystem::rename(cecon::to_native_path(tmp), cecon::to_native_path(resolved), mvEc);
    if (mvEc) {
      std::error_code rmEc;
      std::filesystem::remove(cecon::to_native_path(tmp), rmEc);
      return tblError(cmd + ": failed writing " + userPath, "data_io");
    }
    return displayBlock("file " + userPath + " saved");
  }
  // No destination: the table replaces the dataset in memory. `droppedVars` is
  // every column that was there before, because none of them survive.
  std::vector<std::string> dropped;
  if (session.datastore() != nullptr) dropped = session.datastore()->names();
  // W2AK TW-DS: through a .dta and the `use` path, so the columns carry
  // Stata storage types -- str# for the name column, byte for counts, float
  // for float-exact values -- exactly as the saved file does (w2at wt_p03 /
  // wt_r01 v1..v8: `: type parm` was r(101) on the CSV-built frame).
  {
    const std::string tmpDta = mintScratchPath(session.workdir(), cmd + "_mem");
    std::string dta = tmpDta.substr(0, tmpDta.size() - 5) + ".dta";
    forgetScratchMint(tmpDta);
    registerScratchMint(dta);
    const CobraRunResult cr = runCobra({"export-dta", "--data", hbin, "--out", dta});
    if (cr.ok && cecon::native_exists(dta)) {
      const ojson u = runUse(dta, session);
      if (!isErrorBlock(u)) return mutationResult(session, cmd, newVars, dropped);
    }
  }
  session.load(hbin);
  return mutationResult(session, cmd, newVars, dropped);
}

// The coefficient rows of the ACTIVE estimation, for both builders.
struct CoefRowView {
  std::string name;
  double b = std::numeric_limits<double>::quiet_NaN();
  double se = std::numeric_limits<double>::quiet_NaN();
  double t = std::numeric_limits<double>::quiet_NaN();
  double p = std::numeric_limits<double>::quiet_NaN();
  double lo = std::numeric_limits<double>::quiet_NaN();
  double hi = std::numeric_limits<double>::quiet_NaN();
};

// The rows in e(b)'s order and with e(b)'s names (eval::stataEbColumns): `_cons`
// last, and a factor's base level as a row of its own, b 0 and se 0 with t and
// p missing and both limits 0 (w2at_table_writers.log:4726-4773 wt_p01 `x1 x2
// 0b.g 1.g 2.g _cons`, row 3 `0|0|.|.|0|0`; regsave wt_r01 row 3 `0b.g` 0 0 0 0).
std::vector<CoefRowView> coefViews(const ojson& snap) {
  std::vector<CoefRowView> out;
  if (!snap.contains("coeftable") || !snap["coeftable"].is_array()) return out;
  for (const auto& col : eval::stataEbColumns(snap)) {
    CoefRowView v;
    v.name = col.name;
    if (col.row == nullptr) {
      v.b = 0.0;
      v.se = 0.0;
      v.lo = 0.0;
      v.hi = 0.0;
      out.push_back(v);
      continue;
    }
    const ojson& r = *col.row;
    if (r.contains("b")) v.b = jsonNum(r["b"]);
    if (r.contains("se")) v.se = jsonNum(r["se"]);
    if (r.contains("t")) v.t = jsonNum(r["t"]);
    if (r.contains("p")) v.p = jsonNum(r["p"]);
    if (r.contains("ci_lo")) v.lo = jsonNum(r["ci_lo"]);
    if (r.contains("ci_hi")) v.hi = jsonNum(r["ci_hi"]);
    out.push_back(v);
  }
  // e(b)'s order: `_cons` last (w2at wt_p06 reads x1, x2, _cons).
  std::stable_partition(out.begin(), out.end(), [](const CoefRowView& v) { return v.name != "_cons"; });
  // The factor base levels e(b) carries as `0b.g` columns (b 0, var 0: t and p
  // missing, the limits 0; w2at wt_p01_r3*, wt_r01_r3*). The engine's table has
  // no such column (EB-BASE); activeModel read the base level from the live
  // data and e(sample) (factorBaseLevels), the same rows esttab shows.
  if (snap.contains("base_levels") && snap["base_levels"].is_array()) {
    for (const auto& e : snap["base_levels"]) {
      CoefRowView v;
      v.name = std::to_string(e.value("level", 0LL)) + "b." + e.value("var", std::string());
      // e(b) itself carries the base column when the fit recorded its stripe
      // (eval::stataEbColumns, eb_stripe): it is the single source then.
      if (std::any_of(out.begin(), out.end(), [&](const CoefRowView& r) { return r.name == v.name; }))
        continue;
      v.b = 0.0;
      v.se = 0.0;
      v.lo = 0.0;
      v.hi = 0.0;
      const std::string before = e.value("before", std::string());
      const std::string after = e.value("after", std::string());
      auto pos = out.end();
      if (!before.empty()) {
        pos = std::find_if(out.begin(), out.end(), [&](const CoefRowView& r) { return r.name == before; });
      } else if (!after.empty()) {
        pos = std::find_if(out.begin(), out.end(), [&](const CoefRowView& r) { return r.name == after; });
        if (pos != out.end()) ++pos;
      }
      out.insert(pos, v);
    }
  }
  return out;
}

// `level(#)` other than the level the model was FITTED at cannot be honoured
// without an inverse-t this build does not have. Refused by name; the fix the
// message names (refit with level(#)) produces the right interval.
std::optional<ojson> rejectForeignLevel(
    const std::vector<std::pair<std::string, eval::OptValue>>& opts, const std::string& cmd) {
  const std::optional<std::string> lv = optStr(opts, "level");
  if (!lv.has_value()) return std::nullopt;
  const std::string s = strutil::trim(dequote(*lv));
  if (s == "95" || s == "95.0") return std::nullopt;
  return tblError(cmd + ": level(" + s +
                  ") is not implemented in this build; the confidence limits come from the fit, "
                  "so refit the model with level(" + s + ") instead");
}

}  // namespace

ojson runRegsave(const eval::ParsedCommand& parsed, SessionState& session) {
  if (auto err = eval::rejectUnknownOptions(parsed.opts, kVocabRegsave.known,
                                            kVocabRegsave.unimplemented)) {
    return ojson(*err);
  }
  Model m;
  ojson err;
  // regsave reads e(b): with none it is r(111) "matrix e(b) not found"
  // (w2at wt_r05, log 5348-5358).
  if (!activeModel(session, "regsave", &m, &err)) return tblErrorRc("matrix e(b) not found", 111);
  if (auto lvErr = rejectForeignLevel(parsed.opts, "regsave")) return *lvErr;

  std::vector<std::string> words = parsed.varlist;
  const std::optional<std::string> usingPath = takeUsing(words);
  std::vector<CoefRowView> rows = coefViews(m.snap);
  // regsave builds its table with svmat, i.e. FLOAT columns: b, se and r2
  // are float-rounded, and the interval is float(float(b) -/+ t*float(se))
  // with t the fit's own critical value (w2at wt_r04; regsave.ado's svmat).
  auto fl = [](double v) { return std::isnan(v) ? v : static_cast<double>(static_cast<float>(v)); };
  // regsave: t_ci = invttail(e(df_r), .025) exactly (the normal
  // quantile without df); the fit's own (b - lo)/se differs in the last bits
  // and flips the float rounding of a limit (w2at wt_r01_r5c4).
  double tExact = kEoNaN;
  if (m.escalars.contains("df_r") && m.escalars["df_r"].is_number()) {
    char buf[96];
    std::snprintf(buf, sizeof(buf), "invttail(%.17g, 0.025)", m.escalars["df_r"].get<double>());
    if (auto v = piet::evalScalarExpr(buf, session)) tExact = *v;
  }
  for (auto& r : rows) {
    // `gen t_ci = ...` without `double` is a FLOAT column too (regsave).
    const double tcrit = !std::isnan(tExact)
                             ? fl(tExact)
                             : ((!std::isnan(r.lo) && r.se > 0) ? (r.b - r.lo) / r.se : kEoNaN);
    // regsave: svmat writes e(V)'s diagonal into a FLOAT column
    // and then `replace stderr = sqrt(stderr)` -- the root of the float
    // variance, rounded to float again (w2at wt_r01_r6c3 `+1.3b8862X-002`).
    const double fb = fl(r.b), fse = std::isnan(r.se) ? r.se : fl(std::sqrt(fl(r.se * r.se)));
    r.b = fb;
    r.se = fse;
    if (!std::isnan(tcrit)) {
      r.lo = fl(fb - tcrit * fse);
      r.hi = fl(fb + tcrit * fse);
    }
    r.t = fl(r.t);
    r.p = fl(r.p);
  }

  std::vector<DsColumn> cols;
  auto addNum = [&](const std::string& name) -> DsColumn& {
    cols.push_back(DsColumn{name, false, {}, {}});
    return cols.back();
  };
  {
    DsColumn v{"var", true, {}, {}};
    for (const auto& r : rows) v.str.push_back(r.name);
    cols.push_back(v);
  }
  {
    DsColumn& c = addNum("coef");
    for (const auto& r : rows) c.num.push_back(r.b);
  }
  {
    DsColumn& c = addNum("stderr");
    for (const auto& r : rows) c.num.push_back(r.se);
  }
  if (hasFlag(parsed.opts, "tstat")) {
    DsColumn& c = addNum("tstat");
    for (const auto& r : rows) c.num.push_back(r.t);
  }
  if (hasFlag(parsed.opts, "pval")) {
    DsColumn& c = addNum("pval");
    for (const auto& r : rows) c.num.push_back(r.p);
  }
  if (hasFlag(parsed.opts, "ci")) {
    DsColumn& lo = addNum("ci_lower");
    for (const auto& r : rows) lo.num.push_back(r.lo);
    DsColumn& hi = addNum("ci_upper");
    for (const auto& r : rows) hi.num.push_back(r.hi);
  }
  {
    DsColumn& c = addNum("N");
    const double n = m.escalars.contains("N") ? m.escalars["N"].get<double>()
                                              : std::numeric_limits<double>::quiet_NaN();
    for (size_t i = 0; i < rows.size(); ++i) c.num.push_back(n);
  }
  {
    DsColumn& c = addNum("r2");
    const double r2 = m.escalars.contains("r2") ? m.escalars["r2"].get<double>()
                                                : std::numeric_limits<double>::quiet_NaN();
    for (size_t i = 0; i < rows.size(); ++i) c.num.push_back(fl(r2));
  }
  if (auto al = optStr(parsed.opts, "addlabel")) {
    for (const auto& [name, val] : parseCommaPairs(*al)) {
      DsColumn c{name, true, {}, {}};
      for (size_t i = 0; i < rows.size(); ++i) c.str.push_back(val);
      cols.push_back(c);
    }
  }
  if (hasFlag(parsed.opts, "cmdline")) {
    DsColumn c{"cmdline", true, {}, {}};
    const std::string cl = m.snap.value("estimator", std::string()) + " " +
                           m.snap.value("depvar", std::string());
    for (size_t i = 0; i < rows.size(); ++i) c.str.push_back(strutil::trim(cl));
    cols.push_back(c);
  }

  std::string hbin;
  ojson dsErr;
  if (!materializeDataset(session, "regsave", cols, rows.size(), &hbin, &dsErr)) return dsErr;
  std::vector<std::string> newVars;
  for (const auto& c : cols) newVars.push_back(c.name);
  return finishDataset(session, "regsave", hbin, usingPath, hasFlag(parsed.opts, "replace"),
                       hasFlag(parsed.opts, "append"), newVars);
}

ojson runParmest(const eval::ParsedCommand& parsed, SessionState& session) {
  if (auto err = eval::rejectUnknownOptions(parsed.opts, kVocabParmest.known,
                                            kVocabParmest.unimplemented)) {
    return ojson(*err);
  }
  Model m;
  ojson err;
  if (!activeModel(session, "parmest", &m, &err)) return err;
  if (auto lvErr = rejectForeignLevel(parsed.opts, "parmest")) return *lvErr;

  // `saving("f.dta", replace)` -- the replace lives INSIDE the parentheses,
  // which is parmest's own syntax and not the outer `replace` option.
  std::optional<std::string> savePath;
  bool saveReplace = hasFlag(parsed.opts, "replace");
  if (auto sv = optStr(parsed.opts, "saving")) {
    const std::vector<std::string> parts = splitTopLevel(*sv, ',');
    if (!parts.empty()) savePath = dequote(strutil::trim(parts[0]));
    for (size_t i = 1; i < parts.size(); ++i) {
      const std::string w = strutil::lower(strutil::trim(parts[i]));
      if (w == "replace" || w == "repl") saveReplace = true;
    }
  }

  // parmest checks saving() before it builds anything: an existing file
  // without replace is r(498) with two lines (w2at wt_p07, log 5130-5138).
  if (savePath.has_value() && !saveReplace) {
    const std::string up = ensureExt(*savePath, ".dta");
    if (cecon::native_exists(resolveUserPath(session, up))) {
      return tblErrorRc("file " + up + " already exists\nsaving(" + *optStr(parsed.opts, "saving") +
                            ") invalid",
                        498);
    }
  }
  const std::vector<CoefRowView> rows = coefViews(m.snap);
  const double dfr = m.escalars.contains("df_r") ? m.escalars["df_r"].get<double>()
                                                 : std::numeric_limits<double>::quiet_NaN();

  std::vector<DsColumn> cols;
  {
    DsColumn v{"parm", true, {}, {}};
    for (const auto& r : rows) v.str.push_back(r.name);
    cols.push_back(v);
  }
  auto addNum = [&](const std::string& name) -> DsColumn& {
    cols.push_back(DsColumn{name, false, {}, {}});
    return cols.back();
  };
  { DsColumn& c = addNum("estimate"); for (const auto& r : rows) c.num.push_back(r.b); }
  { DsColumn& c = addNum("stderr");   for (const auto& r : rows) c.num.push_back(r.se); }
  {
    DsColumn& c = addNum("dof");
    for (size_t i = 0; i < rows.size(); ++i) c.num.push_back(dfr);
  }
  if (hasFlag(parsed.opts, "omit")) {
    // `omit` ADDS a byte `omit` after dof, 1 on base and omitted parameters
    // (w2at wt_p02_v5); it removes nothing.
    DsColumn& c = addNum("omit");
    for (const auto& r : rows) c.num.push_back((r.b == 0.0 && (r.se == 0.0 || std::isnan(r.se))) ? 1.0 : 0.0);
  }
  { DsColumn& c = addNum("t");        for (const auto& r : rows) c.num.push_back(r.t); }
  { DsColumn& c = addNum("p");        for (const auto& r : rows) c.num.push_back(r.p); }
  // `min95`/`max95` are parmest's names for the fitted 95% limits; a foreign
  // level() was already refused above, so the names cannot lie about the level.
  { DsColumn& c = addNum("min95");    for (const auto& r : rows) c.num.push_back(r.lo); }
  { DsColumn& c = addNum("max95");    for (const auto& r : rows) c.num.push_back(r.hi); }
  if (hasFlag(parsed.opts, "label")) {
    DsColumn c{"label", true, {}, {}};
    for (const auto& r : rows) c.str.push_back(session.eval.varLabels.get(r.name));
    cols.push_back(c);
  }
  if (auto idn = optStr(parsed.opts, "idnum")) {
    DsColumn& c = addNum("idnum");
    double v = std::numeric_limits<double>::quiet_NaN();
    try {
      size_t pos = 0;
      v = std::stod(dequote(*idn), &pos);
      if (pos != dequote(*idn).size()) v = std::numeric_limits<double>::quiet_NaN();
    } catch (...) {
    }
    for (size_t i = 0; i < rows.size(); ++i) c.num.push_back(v);
  }
  if (auto ids = optStr(parsed.opts, "idstr")) {
    DsColumn c{"idstr", true, {}, {}};
    for (size_t i = 0; i < rows.size(); ++i) c.str.push_back(dequote(*ids));
    cols.push_back(c);
  }
  const size_t nRows = cols.empty() ? 0 : (cols[0].isString ? cols[0].str.size()
                                                            : cols[0].num.size());

  std::string hbin;
  ojson dsErr;
  if (!materializeDataset(session, "parmest", cols, nRows, &hbin, &dsErr)) return dsErr;
  std::vector<std::string> newVars;
  for (const auto& c : cols) newVars.push_back(c.name);
  return finishDataset(session, "parmest", hbin, savePath, saveReplace, false, newVars);
}

// Portions derived from mat2txt 1.1.2 (GPL-3.0).
// See THIRD_PARTY_NOTICES.
// ===========================================================================
// mat2txt
// ===========================================================================

ojson runMat2txt(const eval::ParsedCommand& parsed, SessionState& session) {
  if (auto err = eval::rejectUnknownOptions(parsed.opts, kVocabMat2txt.known,
                                            kVocabMat2txt.unimplemented)) {
    return ojson(*err);
  }
  const std::optional<std::string> matName = optStr(parsed.opts, "matrix");
  if (!matName.has_value()) return tblSyntax("mat2txt: matrix(NAME) is required");
  const std::optional<std::string> saving = optStr(parsed.opts, "saving");
  if (!saving.has_value()) return tblSyntax("mat2txt: saving(FILENAME) is required");

  const eval::EvalMatrix* mat = session.eval.findMatrix(dequote(*matName));
  if (mat == nullptr) return tblError("matrix " + dequote(*matName) + " not found");

  // W2AK TW-MAT2TXT: mat2txt.ado 1.1.2, derived. The header is a tab and
  // each column name followed by a tab; every row is its name, a tab, then each
  // cell `file write %fmt (x) _tab` -- PADDED to the format's width (default
  // %10.0g) with a trailing tab (w2at wt_m01 L2 `r1~t        12~t...`). The
  // format list is per column with the last one repeating.
  std::vector<std::string> fmts = {"%10.0g"};
  if (auto f = optStr(parsed.opts, "format")) {
    fmts = eoTokens(*f);
    if (fmts.empty()) fmts = {"%10.0g"};
  }
  std::string path = dequote(*saving);
  if (path.find('.') == std::string::npos) path += ".txt";
  auto cellText = [&](double v, const std::string& fmt) {
    std::string val = std::isnan(v) ? std::string(".") : eoTrim(eval::applyStataFmt(v, fmt));
    static const std::regex reW(R"(^%(-?)0?(\d+))");
    std::smatch m;
    if (std::regex_search(fmt, m, reW)) {
      const int w = std::stoi(m[2].str());
      val = m[1].length() > 0 ? eoPadR(val, w) : eoPadL(val, w);
    }
    return val;
  };
  std::string bytes;
  if (auto ti = optStr(parsed.opts, "title")) bytes += eoStringArg(ti) + "\n";
  bytes += "\t";
  for (const auto& cn : mat->colnames) bytes += cn + "\t";
  bytes += "\n";
  for (size_t r = 0; r < mat->data.size(); ++r) {
    bytes += (r < mat->rownames.size() ? mat->rownames[r] : ("r" + std::to_string(r + 1))) + "\t";
    for (size_t c = 0; c < mat->data[r].size(); ++c) {
      const std::string& fmt = fmts[std::min(c, fmts.size() - 1)];
      bytes += cellText(mat->data[r][c], fmt) + "\t";
    }
    bytes += "\n";
  }
  if (auto n = optStr(parsed.opts, "note")) bytes += eoStringArg(n) + "\n";
  return emitTable("mat2txt", session, path, bytes, hasFlag(parsed.opts, "replace"),
                   hasFlag(parsed.opts, "append"));
}

// ===========================================================================
// erepost
// ===========================================================================

ojson runErepost(const eval::ParsedCommand& parsed, SessionState& session) {
  if (auto err = eval::rejectUnknownOptions(parsed.opts, kVocabErepost.known,
                                            kVocabErepost.unimplemented)) {
    return ojson(*err);
  }
  if (!session.lastEstBlock.has_value()) {
    return tblErrorRc("erepost: no estimation result in memory. Run a regression first.", 301);
  }
  std::string args = argsAfterHead(parsed.raw);
  if (args.empty()) return tblSyntax("erepost: the syntax is `erepost b = MATRIX'");

  // `erepost b = M` / `erepost V = M`. Both spellings are accepted; only `b` is
  // implemented, and `V` is refused BY NAME rather than accepted-and-ignored,
  // because a table built from a reposted b with the OLD V would report
  // standard errors that belong to different coefficients.
  const size_t eq = args.find('=');
  if (eq == std::string::npos) return tblSyntax("erepost: the syntax is `erepost b = MATRIX'");
  const std::string lhs = strutil::trim(args.substr(0, eq));
  const std::string rhs = strutil::trim(args.substr(eq + 1));
  if (lhs == "V" || lhs == "v") {
    return ojson(eval::unsupportedCommand("erepost V=", std::nullopt, std::nullopt,
                                          "refused:not_implemented_sub",
                                          eval::UnsupportedCommandKind::kNotImplemented));
  }
  if (lhs != "b") return tblSyntax("erepost: only `erepost b = MATRIX' is supported");

  const eval::EvalMatrix* mat = session.eval.findMatrix(rhs);
  if (mat == nullptr) return tblError("matrix " + rhs + " not found");
  if (mat->data.empty()) return tblError("erepost: matrix " + rhs + " is empty");

  // Stata's e(b) is 1 x k. A k x 1 column vector is not read as the same vector:
  // erepost posts M with `ereturn post`, which takes a row vector, so it is r(503)
  // whatever its row names (StataBE 19.5, BUG-1494: `matrix Zc = (7\8\9)`, named
  // `x1 x2 _cons` or not, after `regress y x1 x2`). Anything with two real
  // dimensions says so with both shapes named.
  std::vector<double> vals;
  std::vector<std::string> names;
  if (mat->data.size() == 1) {
    vals = mat->data[0];
    names = mat->colnames;
  } else if (mat->data[0].size() == 1) {
    return tblErrorRc("conformability error", 503);
  } else {
    return tblError("erepost: " + rhs + " is " + std::to_string(mat->data.size()) + "x" +
                    std::to_string(mat->data[0].size()) + " but e(b) is a vector");
  }

  ojson block = *session.lastEstBlock;
  if (!SessionState::blockPostsEb(block) || !session.lastModel().has_value()) {
    // eb-order T-P3: the live e() has no e(b) (an estpost posting), or one with
    // no fitted model behind it (a first erepost on such a posting), so erepost
    // posts M as the new e(b) under M's own names -- erepost.ado `eret post `b''
    // with no e(V) to carry (w2v_estpost_ematrix.log ep14e_colnames_eb `c1 c2`,
    // ep14e_local_bc1 7, ep14e_local_bx1 r(111); a second repost takes M2's
    // names, ep14e2_colnames_eb `x1 x2`, ep14e2_local_bx1 7).
    // The posting's macros and other matrices stay (ep14e_ecmd).
    ojson coeftable = ojson::array();
    for (size_t i = 0; i < vals.size(); ++i) {
      ojson row;
      row["name"] = i < names.size() && !names[i].empty() ? names[i] : ("c" + std::to_string(i + 1));
      row["b"] = std::isnan(vals[i]) ? ojson(nullptr) : ojson(vals[i]);
      row["se"] = nullptr;
      row["t"] = nullptr;
      row["p"] = nullptr;
      coeftable.push_back(row);
    }
    block["coeftable"] = coeftable;
    block.erase("posts_e_b");
    // eb-order ROUND 3: the rows ARE the posted M (names, order, values), so
    // the e(b) resolver reads them as posted, not through a command's stripe
    // row or its omitted-column guard (a reposted 0 has no se).
    block["reposted_b"] = true;
    session.lastEstBlock = block;
    // BUG-1202: a fit that posted e(b) as a matrix (glm, oaxaca, teffects) is
    // read from that matrix by `matrix b = e(b)` and el(e(b),i,j), so it becomes
    // M too (StataBE: el(e(b),1,1) and _b[x1] are both M's 4 after glm).
    for (auto& p : session.eval.eMatrices) {
      if (p.first != "b") continue;
      eval::EvalMatrix nb;
      nb.data = {vals};
      nb.rownames = p.second.rownames.empty() ? std::vector<std::string>{"y1"} : p.second.rownames;
      for (size_t i = 0; i < vals.size(); ++i)
        nb.colnames.push_back(i < names.size() && !names[i].empty() ? names[i]
                                                                   : "c" + std::to_string(i + 1));
      p.second = std::move(nb);
    }
    return displayBlock("(e(b) reposted from " + rhs + "; e(V) is not reposted)");
  }
  if (!block.contains("coeftable") || !block["coeftable"].is_array()) {
    return tblError("erepost: the last estimation has no coefficient vector");
  }
  const size_t k = block["coeftable"].size();
  // BUG-1203: Stata's e(b) after tobit/ologit/nbreg (eval::ebHasUnsentColumns)
  // has ancillary columns the table lacks, so an M no wider than the table is
  // Stata's conformability error (StataBE: a 3-column M after `tobit yc x1 x2`
  // is r(503) and _b is unchanged); a wider one cannot be laid over a table
  // without those columns, and says so.
  if (eval::ebHasUnsentColumns(session.eval)) {
    if (vals.size() <= k) return tblErrorRc("conformability error", 503);
    return tblErrorRc("erepost: e(b) after this estimation has ancillary columns this version "
                      "of PIET does not hold, so it cannot be reposted",
                      198);
  }
  // BUG-1202: glm, oaxaca and teffects post e(b) as a matrix, which `matrix b =
  // e(b)` and el(e(b),i,j) read ahead of the table. That matrix is Stata's e(b):
  // M is conformable with it, the repost writes it, and each table row (what
  // _b[] reads) takes the value of its column by name.
  eval::EvalMatrix* postedB = nullptr;
  for (auto& p : session.eval.eMatrices)
    if (p.first == "b") postedB = &p.second;
  if (postedB != nullptr && (postedB->data.size() != 1 ||
                             postedB->colnames.size() != postedB->data[0].size()))
    postedB = nullptr;
  if (vals.size() != (postedB != nullptr ? postedB->data[0].size() : k))
    return tblErrorRc("conformability error", 503);  // StataBE: a 1x2 M after a 3-column fit

  // M is positional over e(b) as Stata lists it -- the canonical order, `_cons`
  // last -- not over the engine's coeftable, which puts `(intercept)` first:
  // after `regress y x1 x2`, `erepost b = (1,2,3)` gives `_b[x1]` 1 and
  // `_b[_cons]` 3 (w2g_ebmat.log e8_bx1 +1.0X+000, e8_bcons +1.8X+001).
  std::vector<size_t> posOf(k);
  for (size_t i = 0; i < k; ++i) posOf[i] = i;
  {
    std::string why;
    const std::optional<eval::EvalMatrix> canon = eval::canonicalEMatrix(block, 'b', &why);
    if (canon.has_value() && canon->colnames.size() == k) {
      for (size_t i = 0; i < k; ++i) {
        std::string nm = block["coeftable"][i].value("name", "");
        const size_t colon = nm.find(':');
        const std::string eq = colon == std::string::npos ? std::string() : nm.substr(0, colon + 1);
        std::string bare = colon == std::string::npos ? nm : nm.substr(colon + 1);
        if (bare == "(intercept)") bare = "_cons";
        const auto it = std::find(canon->colnames.begin(), canon->colnames.end(), eq + bare);
        if (it != canon->colnames.end())
          posOf[i] = static_cast<size_t>(it - canon->colnames.begin());
      }
    }
    // A repeated name maps two rows to one column: keep the engine's positions.
    std::vector<char> seen(k, 0);
    bool perm = true;
    for (size_t i = 0; i < k && perm; ++i) {
      if (seen[posOf[i]]) perm = false;
      seen[posOf[i]] = 1;
    }
    if (!perm)
      for (size_t i = 0; i < k; ++i) posOf[i] = i;
  }

  const bool rename = hasFlag(parsed.opts, "rename");
  // BUG-1494: erepost posts M together with the fit's own e(V) (`ereturn post b
  // V`), which takes M only under e(b)'s column names, in e(b)'s order: an
  // unnamed M (c1 c2 ...), other names, or e(b)'s names permuted is r(507) "name
  // conflict" with e(b) left alone, and an M narrower than e(b) (a factor fit's
  // base column) r(503). `rename` gives V M's names instead. StataBE 19.5,
  // scratch-laneA2/agents/rprog/x1494.do: case counts (`X1` is 507), an `o.`
  // name counts (`x1 x3 _cons` against `o.x1 x3 _cons` is 507), the default
  // equation `_:` is no equation (`_:x1 _:x2 _:_cons` posts), a row name does not
  // matter. e(b)'s names are read as `matrix Z = e(b)` copies them.
  if (!rename) {
    std::vector<std::string> ebNames;
    if (const std::optional<eval::EvalMatrix> eb = session.eval.resolveEMatrix("b"))
      ebNames = eb->colnames;
    else if (const std::optional<eval::EvalMatrix> canon = eval::canonicalEMatrix(block, 'b'))
      ebNames = canon->colnames;  // e(b) the resolver will not build (a renamed `_cons`)
    const auto stripeName = [](const std::vector<std::string>& nm, size_t i) {
      std::string s = i < nm.size() ? nm[i] : std::string();
      if (s.empty()) return "c" + std::to_string(i + 1);
      if (s.compare(0, 2, "_:") == 0) s.erase(0, 2);
      return s;
    };
    if (!ebNames.empty()) {
      if (ebNames.size() != vals.size()) return tblErrorRc("conformability error", 503);
      for (size_t i = 0; i < vals.size(); ++i)
        if (stripeName(names, i) != stripeName(ebNames, i)) return tblErrorRc("name conflict", 507);
    }
  }
  if (rename) {
    // The rows take M's names, so canonicalEMatrix can no longer find `_cons`
    // by name: lay the rows out in e(b)'s order first, so `: colnames e(b)`
    // lists M's names in M's order.
    ojson reordered = ojson::array();
    for (size_t p = 0; p < k; ++p) reordered.push_back(ojson());
    for (size_t i = 0; i < k; ++i) reordered[posOf[i]] = block["coeftable"][i];
    block["coeftable"] = reordered;
    for (size_t i = 0; i < k; ++i) posOf[i] = i;
  }
  // An omitted (collinear) column stays omitted after the repost -- Stata's e(b)
  // keeps its `o.` name (w2v_estripe_census.log esc_regress_omit `o.x1 x2 x3c
  // _cons`) -- but the b 0 / se 0 pair that marks it is about to be
  // overwritten, so record it on the row first for the e(b) resolver's
  // omitted-column guard (eval/macros.cpp resolveEMatrixFor G1). A row marked
  // by an earlier repost keeps its mark; a repost's own 0 is not an omission.
  if (!block.value("reposted_b", false)) {
    for (auto& row : block["coeftable"]) {
      if (!row.is_object()) continue;
      const auto bIt = row.find("b");
      const auto seIt = row.find("se");
      const bool bZero = bIt != row.end() && bIt->is_number() && bIt->get<double>() == 0.0;
      const bool seZero = seIt == row.end() || seIt->is_null() ||
                          (seIt->is_number() && seIt->get<double>() == 0.0);
      if (bZero && seZero) row["omitted"] = true;
    }
  }
  // Each row's column of M: its canonical position, or for a posted e(b) the
  // column of the same name (`_cons` for `(intercept)`, an equation prefix
  // the row does not carry ignored).
  std::vector<std::optional<size_t>> colOf(k);
  for (size_t i = 0; i < k; ++i) {
    if (postedB == nullptr) {
      colOf[i] = posOf[i];
      continue;
    }
    std::string nm = block["coeftable"][i].value("name", "");
    if (nm == "(intercept)") nm = "_cons";
    for (size_t j = 0; j < postedB->colnames.size() && !colOf[i]; ++j) {
      const std::string& c = postedB->colnames[j];
      const size_t colon = c.find(':');
      if (c == nm || (colon != std::string::npos && nm.find(':') == std::string::npos &&
                      c.compare(colon + 1, std::string::npos, nm) == 0))
        colOf[i] = j;
    }
  }
  for (size_t i = 0; i < k; ++i) {
    ojson& row = block["coeftable"][i];
    if (colOf[i]) {
      const size_t p = *colOf[i];
      row["b"] = std::isnan(vals[p]) ? ojson(nullptr) : ojson(vals[p]);
      if (rename && p < names.size() && !names[p].empty()) row["name"] = names[p];
    }
    // NO V was reposted, so se/t/p no longer belong to these coefficients.
    // They become NULL -- Stata's missing -- and NOT 0: a zero standard error
    // reads as an infinitely precise estimate, and a zero p as significance.
    row["se"] = nullptr;
    row["t"] = nullptr;
    row["p"] = nullptr;
    if (row.contains("ci_lo")) row["ci_lo"] = nullptr;
    if (row.contains("ci_hi")) row["ci_hi"] = nullptr;
  }
  // The posted e(b) as M (written below, after the LastModel refresh, which
  // reinstalls the fit's own e() stores).
  std::optional<eval::EvalMatrix> newPostedB;
  if (postedB != nullptr) {
    newPostedB = *postedB;
    newPostedB->data[0] = vals;
    if (rename)
      for (size_t j = 0; j < newPostedB->colnames.size() && j < names.size(); ++j)
        if (!names[j].empty()) newPostedB->colnames[j] = names[j];
  }
  block["reposted_b"] = true;  // eb-order ROUND 3: see the estpost branch above
  session.lastEstBlock = block;
  // `_b[x]` in both layers reads this block (eb-order T-P3,
  // EvalSession::coefSource), so the write above is all `_b[]` needs. LastModel
  // still caches the raw result separately, mirrored into
  // `eval.lastModelResult` for the fitted e-scalars, so the copy below keeps
  // that cache on the reposted coefficients (a copy keeps the fit serial, so
  // setLastModel leaves this block in place).
  if (session.lastModel().has_value()) {
    LastModel lm = *session.lastModel();
    if (lm.result.has_value()) {
      ojson res = *lm.result;
      if (res.contains("coeftable") && res["coeftable"].is_array() &&
          res["coeftable"].size() == k) {
        for (size_t i = 0; i < k; ++i) {
          res["coeftable"][i] = block["coeftable"][i];
        }
        lm.result = res;
        session.setLastModel(lm);
      }
    }
  }
  if (newPostedB.has_value()) {
    bool put = false;
    for (auto& p : session.eval.eMatrices) {
      if (p.first != "b") continue;
      p.second = *newPostedB;
      put = true;
    }
    if (!put) session.eval.eMatrices.emplace_back("b", std::move(*newPostedB));
  }
  return displayBlock("(e(b) reposted from " + rhs +
                      "; e(V) is not reposted, so e(se) is missing)");
}

}  // namespace piet::handlers
