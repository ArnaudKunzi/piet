// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// Session-state macro / r() expansion layer — C++ port of
// piet/backend/piet/router/macros.py plus its two out-of-module helpers:
//   - postfile.eval_macro_expr        (the `=expr' arithmetic-macro form)
//   - matrix_op.substitute_matrix_refs / get_matrix_element (WS-C pass)
//   - display_hardening._resolve_b_ref/_resolve_e_scalar/_fmt_numeric
//     (the est-scalar resolution _eval_arith depends on)
//
// Read the Python sources as the spec. Deliberate design notes carried over:
//   - UNDEFINED-MACRO POLICY: an undefined macro reference is an error block
//     (deliberate deviation from Stata's silent empty expansion).
//   - MULTI-PASS EXPANSION: bounded 16-pass loop, clean engine_error on
//     non-convergence (self-referential macros), never an infinite loop.
//   - Definition-side RHS expansion runs expandPass to a fixed point
//     (expandDefinitionText; EM3a, Stata's own rule), not one pass.
//
// NOT ported (dead code in the oracle): macros.handle_display /
// _resolve_scalars / _apply_stata_fmt — superseded by
// display_hardening.handle_display, which is an M3 dispatcher-handler
// concern (ROADMAP "known reliability caveats" flags the dead layer).
//
// TODO(M2): _b[]/_se[]/e() resolution reads EvalSession.lastModelResult —
// M2's session port replaces this with the real LastModel store.
#ifndef PIET_NATIVE_EVAL_MACROS_H
#define PIET_NATIVE_EVAL_MACROS_H

#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include "piet/eval/session.h"
#include "vendor/nlohmann/json.hpp"

namespace piet::eval {

// Result of expand_macros / _expand_pass: the expanded line, or an error
// block dict (mirrors Python's `str | dict` return).
struct ExpandResult {
  bool isError = false;
  std::string line;            // when !isError
  nlohmann::ordered_json error;  // when isError
  // BUG-1527: expandMacros' line before its compound-quote fold, when the fold
  // changed it ("" otherwise) -- the command as typed, which graph describe
  // reports.
  std::string typed;
};

// macros.has_macro_refs
bool hasMacroRefs(const std::string& rawLine);

// macros._expand_pass — ONE substitution pass (exposed because the
// definition-capture paths call it directly).
//
// `unsetRAsMissing` — when true, a reference to an UNSET r() scalar expands
// to the Stata system-missing literal `.` instead of raising the
// "r(name) is not set" engine_error. This is the Stata semantics for r()
// inside a numeric EXPRESSION (an unset r() evaluates to missing, `.`), and
// is used ONLY by the `if`/`while` control-flow CONDITION path (runIf/runWhile
// in loops.cpp) so the idiomatic guard `if !_rc & !missing(r(estimate))`
// survives after a failed `capture`-d r-class command — mirroring how an
// unresolved `scalar(NAME)` already becomes the missing sentinel in
// evalBoolExpr. The default (false) keeps the honest-error behaviour for the
// command-line path (`gen z = r(mean)` etc.), which goldens pin.
//
// `verbatim` (EM3a, macval): when true, the text a `` `macval(NAME)' `` reference
// splices is ENCODED (each '`' -> kVerbatimTick, each '$' -> kVerbatimDollar) so
// a later pass over the same line cannot re-expand it; the caller decodes
// (decodeVerbatim) once its loop is done. When false the value is spliced as is,
// which is right for a caller that runs exactly one pass.
ExpandResult expandPass(const std::string& line, EvalSession& session,
                        bool unsetRAsMissing = false, bool verbatim = false);

// macros.expand_macros — bounded multi-pass loop + compound-quote normalization +
// convergence guard. See expandPass for `unsetRAsMissing`.
// `keepVerbatim` (EM3a): leave the macval() sentinels in the returned line. Only
// the dispatcher's step 0b asks for it: tokenize() runs its unexpanded-macro net
// on the encoded line (a macval value's own ` and $ are text, not references)
// and decodes right after it. Every other caller gets real characters back.
ExpandResult expandMacros(const std::string& rawLine, EvalSession& session,
                          bool unsetRAsMissing = false, bool keepVerbatim = false);

// EM3a: the right-hand side of a macro DEFINITION (`local x `"..."'`, `local x
// text`, `return local x ...`). Stata expands the text completely: a reference
// whose value holds another reference is expanded again (`local cp2 `"`xb'"'`
// with xb = p`mtl'q stores pLVALq, w2af_macro_text_file.log:1021 mt_mv_copy),
// while `macval(NAME)' inserts the value verbatim. Loops expandPass to a fixed
// point (bounded like expandMacros) and returns decoded text; no convergence
// error and no compound-quote rewrite -- the stored value keeps its quotes.
ExpandResult expandDefinitionText(const std::string& text, EvalSession& session);

// EM3a sentinels: a '`' or '$' that is TEXT (a macval() value, or Stata's `\``
// / `\$` escapes) travels through the expansion passes as one of these bytes.
inline constexpr char kVerbatimTick = '\x11';
inline constexpr char kVerbatimDollar = '\x12';
bool hasVerbatim(const std::string& s);
std::string encodeVerbatim(const std::string& s);
std::string decodeVerbatim(std::string s);

// EM3b: a compound-quoted span `"..."' starting at `i` (s[i] is '`' or
// kVerbatimTick, s[i+1] is '"'): the index one past its closing "' with Stata's
// nesting (a `" opens a level, a "' closes one), or npos when it never closes.
size_t compoundSpanEnd(const std::string& s, size_t i);
bool startsCompound(const std::string& s, size_t i);
// EM3b: every top-level compound span whose body holds no '"' (and so no nested
// span) becomes a plain "body"; one that holds a quote stays `"body"', the only
// spelling that can carry it. Plain "..." literals are copied through.
std::string normalizeCompoundQuotes(const std::string& s);

// BUG-216/217: the extended-macro-function ERROR channel.
//
// `local NAME : <func> ...` could previously only SUCCEED. When no arm matched
// the colon form, the definition path stored the LITERAL MACRO TEXT — so
// `local f : format x` put the eight characters `: format x` into `f`, a value
// that is neither the answer, nor empty, nor an error, and that no consumer can
// distinguish from data (`format v \`f'` then applies a garbage format and
// `if "\`t'" == "double"` silently takes the false branch). Stata's answer for
// an unknown extended macro function is r(101), and for one naming a variable
// that does not exist, r(111).
//
// An OUT-PARAM rather than an exception, deliberately: the dispatcher's first
// `catch` sits downstream of both tryCaptureDefinition call sites, so an
// exception thrown from this layer would escape rather than be translated into
// an rc. Defaulted to nullptr so every existing caller — including the whole
// selftest corpus — stays byte-identical on a path this hot.
//
// Contract: `rc` non-empty means the line WAS a definition form AND it failed;
// tryCaptureDefinition still returns true (it consumed the line), and the caller
// must surface the error INSTEAD of treating the capture as silent success. `rc`
// is the bare digits, ready for the "r(NNN)" reason string that
// eval::coarseRc() honours as an exact code.
struct MacroFnError {
  std::string rc;       // "" = no error; else "101" / "111"
  std::string message;  // Stata-shaped diagnostic
  bool failed() const { return !rc.empty(); }
};

// BUG-293(b): the same failed-expression classifier the `local NAME = <expr>`
// arm uses (BUG-246), exposed so `display` can stop echoing an unevaluable
// expression back as literal text at rc 0. Returns true when it converted the
// failure into a Stata rc (err filled); false means the shape is one it
// deliberately leaves unclaimed — a string-literal RHS, or a bare-identifier
// NameError (the dataset-variable read this layer cannot do) — and the caller
// keeps its legacy fallback. A `session` whose variable reader is installed
// decides that bare name as the `local NAME =` arm does: `NAME not found`
// r(111) (`file write h (nosuchvar)`, StataBE 19.5).
bool classifyFailedExpr(const std::string& expr, const std::string& raised, MacroFnError* err,
                        const EvalSession* session = nullptr);

// BUG-1420: Stata's message for the first `c(NAME)` in `expr` (outside a string
// literal) whose NAME is no c() key of StataBE 19.5 at all -- "c(NAME)
// undefined", r(133) -- or nullopt. A key Stata has and PIET does not serve is
// not "undefined" and is not claimed.
std::optional<std::string> undefinedCRef(const std::string& expr);
// BUG-1330: true for a name StataBE's c() answers (`creturn list`), whether or
// not this build serves it (piet::resolveCValue).
bool isStataCClassName(const std::string& name);
// Its r(133) refusal of a bare c(name) nothing answered: StataBE's `c(name)
// undefined`, or, for a real name this build does not serve, a named refusal.
std::string cClassRefusalMessage(const std::string& name);
// The code that refusal carries: 133 for a name StataBE has no value for, as
// Stata codes `c(name) undefined`; 198, PIET's unsupported-feature code, for a
// real name this build does not serve. Stata answers that one at rc 0
// (c(framesmax) is 100), so 133 would claim an error Stata never raises.
std::string cClassRefusalRc(const std::string& name);

// macros.try_capture_definition — returns true when the line was captured
// as a definition (local/global/scalar/increment forms); false = not a
// definition (caller proceeds to expansion/tokenization). See MacroFnError for
// `errorOut`: true + a set errorOut means "consumed, and it is an error".
bool tryCaptureDefinition(const std::string& rawLine, EvalSession& session,
                          MacroFnError* errorOut = nullptr);

// D1-① DRIFT GUARD. tryCaptureDefinition recognises `local NAME rest` /
// `global NAME rest` with a hand-written single-pass scan instead of
// reLocalDef()/reGlobalDef(), because the WS-D1 profile put those two regex
// matches at ~2 us of the ~3.3 us every dispatched line paid. A hand-written
// twin of a regex that can silently diverge from it is a defect, so this
// entry point re-runs BOTH the scan and the regex over a corpus of forms
// (matched, unmatched, and the awkward boundaries: no-space, tab runs,
// embedded newlines, non-identifier names, case variants) and reports the
// first disagreement. selftest.cpp calls it; the answer is a gate, not a
// diagnostic. Returns true when every case agrees.
bool selftestMacroDefScanMatchesRegex(std::string* firstDisagreement);

// EXPRLAYER DRIFT GUARDS (P2-PERF, same contract as the one above — each
// re-runs BOTH the scan and the regex it replaced over a corpus and reports
// the first disagreement; the answer is a gate, not a diagnostic).
//
//  * safe-arith:   isSafeArith / isSafeArithExt   vs reSafeArith / reSafeArithExt
//  * bare-paren:   hasBareRParen / hasBareEParen  as STRICT pre-filters for
//                  reRRef / reERef (a false answer must imply no match)
//  * scalar scans: substituteScalarCalls, substituteBareName, matchScalarDrop,
//                  matchScalarDef vs their four regexes
bool selftestSafeArithScanMatchesRegex(std::string* firstDisagreement);
bool selftestBarePrefilterIsStrict(std::string* firstDisagreement);
bool selftestScalarScansMatchRegex(std::string* firstDisagreement);
bool selftestLocalRefScanMatchesRegex(std::string* firstDisagreement);

// EXPANSION-SEMANTICS GUARD (P2-PERF EXPRLAYER). expandPass' pre-filters and
// the hand-written scans around it must not move macro-expansion semantics.
// This runs the S32-class corpus — undefined macro, `=expr' inline evaluation,
// nesting, `' quoting, positional macros, r()/e()/s() references, global forms
// — through expandMacros/expandPass and asserts the expected result of each.
// It is the leg the parity hazard list names first.
bool selftestExpansionSemantics(std::string* firstFailure);

// macros._eval_arith — returns the result string or nullopt (Python None).
// Pass session=nullptr for the many session-less callers.
// BUG-088: when the extended (string-fn) path raises a genuine NameError (an
// undefined name — e.g. an unsupported extended macro function `_request(...)`),
// and `raisedError` is non-null, it is set to the honest diagnostic message.
// The return value is still nullopt; a caller that cares (display) can then
// surface the error instead of echoing the literal source text. All other
// failure modes (FnObject, SyntaxError, bad-arity) leave `raisedError` untouched.
// BUG-293: `resultIsString`, when non-null, reports whether the expression
// evaluated to a STRING rather than a number. display needs it to decide
// whether its W:P5 default `%9.0g` reformat applies — `display
// string(42.5,"%9.2f")` is a string result and Stata prints it verbatim
// (`42.50`), where reformatting it as a number gives `42.5`.
std::optional<std::string> evalArith(const std::string& expr, EvalSession* session,
                                     std::string* raisedError = nullptr,
                                     bool* resultIsString = nullptr);

// postfile.eval_macro_expr — the `=expr' evaluator.
std::optional<std::string> evalMacroExpr(const std::string& expr, EvalSession& session);

// matrix_op.substitute_matrix_refs — NAME[i,j] / colsof / rowsof -> literals.
std::string substituteMatrixRefs(const std::string& expr, const EvalSession& session);

// Replace a BARE identifier with a literal, `\bNAME\b(?!\s*\()` semantics — a
// same-named function CALL is spared. Exposed for the dispatcher's row-predicate
// substitution point (BUG-432): evalArith substitutes every stored scalar because
// it has no frame in hand, and the predicate path must NOT, because there a
// variable of the same name wins. Same rule, one caller-supplied name at a time.
std::string substituteBareName(const std::string& in, const std::string& name,
                               const std::string& value);

// name-binding-display-literals: the one string-literal guard (BUG-501's
// protectStringLiterals) for a substitution pass outside this file. Each "..."
// literal becomes a `"0L<k>L0"` placeholder no name can match, its text kept
// in `spans` (a pass may append its own literals there); restoreLiterals puts
// every placeholder back. Text inside a literal is data, never a name: the row
// plane's stored-scalar pass ran over raw text, so `count if arm == "control"`
// with a scalar `control` compared with "5" (w2av_name_binding bd_row_cntlit).
std::string protectLiterals(const std::string& s, std::vector<std::string>* spans);
std::string restoreLiterals(const std::string& s, const std::vector<std::string>& spans);

// name-binding-display-literals: the messages of the inline `=exp' references
// that failed as Stata's evaluator fails (r(111), r(133), r(109)) and so
// expanded to nothing at rc 0 since the last call, oldest first; the queue is
// emptied. StataBE prints each (`foo not found`) and goes on (w2av_name_binding
// bd_dy_ilunb); the dispatcher prints them ahead of the command's own output.
std::vector<std::string> takeInlineExpansionNotes();

// FU-842: the text a VALUE reference (a scalar by name or scalar(), _b[]/_se[],
// bare e()/r(), a matrix element) is pasted as. A value is ONE operand in Stata,
// so a negative number goes in parenthesised: with zneg = -2, `zneg^2` is 4 and
// `-zneg^2` is -4, where the bare text `-2^2` / `--2^2` reads -4 / 4. Anything
// that is not a negative number (a positive value, a missing token, a quoted
// string) comes back unchanged. A MACRO (`local', `r(x)', `e(x)') stays raw
// text: that is Stata's own rule, and why this is applied per value site.
std::string valueRefText(const std::string& text);

// BUG-512: bare `_N` -> the observation count, bare `_n` -> 1, for an expression
// evaluated in a MACRO context (w2_smalls.log section A). Quote-aware; the count
// comes through EvalSession::obsCount and is asked for only when a bare `_N` is
// present; a session with no hook gets its input back unchanged. evalArith and
// evalMacroExpr call it themselves -- it is public for the evaluators that keep
// a namespace of their own (postfile's `post` expressions).
// NOT for a row-plane expression (gen/replace/if-qualifiers), where `_n` is the
// row and both names belong to the engine.
std::string substituteObsSpecials(const std::string& in, const EvalSession& session);

// macros.capture_r_from_block — r() capture from summarize / lincom / nlcom
// result blocks (replaces the whole r() store, Stata semantics).
void captureRFromBlock(const nlohmann::ordered_json& block, EvalSession& session);

// display_hardening._fmt_numeric (Stata %9.0g-style display formatting).
std::string fmtNumeric(double v);

// macros._resolve_est_scalar_refs — resolves _b[var]/_se[var]/scalar(NAME)/
// e(name) references to numeric literals (never e(b)/e(V), which are
// matrices). M3a: exposed for loops.cpp's _eval_bool_expr port (if/while
// condition evaluation), which calls this best-effort (mirrors the oracle's
// `try: ... except Exception: pass` around the call — a resolution failure
// leaves the expression unchanged rather than raising).
// BUG-456: `err` (optional) receives the r(111) message for a `_b[NAME]` /
// `_se[NAME]` whose NAME is not in the model (first one wins); null keeps the
// historical silent `.`. A bare e(name) whose store entry is a STRING is
// substituted as a double-quoted literal (never a number), an absent name as
// numeric missing.
std::string resolveEstScalarRefs(std::string expr, const EvalSession& session,
                                 std::string* err = nullptr);
// BUG-456 follow-up (2026-09-09): resolveEstScalarRefs' `err` carries TWO codes
// -- the historical r(111) (an unknown `_b[NAME]`, no prefix) and r(109) (a
// NUMBER in a STRING-typed argument slot: `strpos(e(nosuch),"reg")`, measured
// r(109) in StataNow 19.5 BE), which is prefixed `r(109): `. This splits the
// prefix off *msg and returns the reason string for errorBlock ("r(111)" when
// there is none), so every call site maps the channel the same way.
std::string splitErrCode(std::string* msg);
// BUG-456 follow-up: is there a NUMERIC CONSTANT in a STRING-typed argument
// slot (`strpos(.,"reg")`, `strlen(20.0)`, a MIXED `inlist(20.0,"74")`)? Stata
// answers r(109) "type mismatch" for every one of them, whether the number was
// written by hand, resolved from e()/`_b[]`, or substituted by the macro layer.
// Fills *call with the offending call's text. stringSlotMismatchError() builds
// the `r(109): ...` message the err channel carries, so the row/gen planes and
// the resolver report the SAME sentence.
bool stringFnNumericArg(const std::string& expr, std::string* call);
std::string stringSlotMismatchError(const std::string& call);
// Strict pre-filters (EXPRLAYER-3): is there an `r(` / `e(` at a word boundary?
// Exposed (BUG-456) so the dispatcher's row-predicate substitution point keys
// its est-ref pass on the SAME test the resolver uses -- a second spelling of
// the trigger is how the two layers start disagreeing.
bool hasBareRParen(const std::string& s);
bool hasBareEParen(const std::string& s);

// e(sample) T2 (internal note esample/SPEC.md §5.1): the token
// `e(sample)` (`\be\(\s*sample\s*\)`) OUTSIDE double-quoted string literals --
// `display "e(sample)"` is text (w2_esample.log B21b). hasEsampleToken is the
// pre-test; replaceEsampleToken swaps every occurrence for `with`. The row layer
// (dispatcher.cpp rewriteEsampleToken) uses both; the macro layer reaches the
// same token through resolveEstScalarRefs' e() arm, whose regex shares the shape.
bool hasEsampleToken(const std::string& s);
std::string replaceEsampleToken(const std::string& s, const std::string& with);

// display_hardening.py's _E_SCALAR_KEY: Stata's e() scalar spellings -> the
// keys cobra actually emits (`e(N)` -> n_obs, `e(r2_a)` -> adj_r2, ...). A name
// ABSENT from this table falls through as key==name, so the table lists only
// the renames plus the identity entries that document a key the engine does not
// emit yet.
//
// AER-SCOPE1 bundle E (2026-09-04) exposed it: `eststo` freezes the e() scalars
// into its snapshot so `esttab, stats(N r2_a)` has something to read, and doing
// that from a private copy would have made a THIRD table to keep in step. There
// are already two -- display_hardening.cpp holds a std::map twin with the same
// content, and both copies carry a comment saying so.
const std::vector<std::pair<std::string, std::string>>& eScalarKey();

}  // namespace piet::eval

#endif  // PIET_NATIVE_EVAL_MACROS_H
