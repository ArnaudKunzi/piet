// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// Structured error payloads — C++ port of piet/backend/piet/router/errors.py.
//
// Mirrors CONTRACT.md CommandErrorResult exactly: {type:"error", code, message,
// token?, line_number?, context?, reason?}. The set of valid `code` values is
// frozen (CONTRACT.md) — same ERROR_CODES allow-list as the Python oracle.
#ifndef PIET_NATIVE_EVAL_ERRORS_H
#define PIET_NATIVE_EVAL_ERRORS_H

#include <initializer_list>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "piet/eval/tokenizer.h"  // OptValue (rejectUnknownOptions)
#include "vendor/nlohmann/json.hpp"

namespace piet::eval {

// Frozen set of valid CommandErrorResult codes (errors.py ERROR_CODES).
bool isKnownErrorCode(const std::string& code);

// Build a CommandErrorResult block. Unknown `code` values are coerced to
// "engine_error", exactly like the Python error_block() guard.
nlohmann::json errorBlock(const std::string& code, const std::string& message,
                           std::optional<std::string> token = std::nullopt,
                           std::optional<int> lineNumber = std::nullopt,
                           std::optional<std::string> context = std::nullopt,
                           std::optional<std::string> reason = std::nullopt);

// --- Named rejection helpers (errors.py) — same messages, verbatim. ---------

nlohmann::json unsupportedLoop(const std::string& token,
                                std::optional<int> lineNumber = std::nullopt,
                                std::optional<std::string> context = std::nullopt);

nlohmann::json unsupportedMacro(const std::string& token,
                                 std::optional<int> lineNumber = std::nullopt,
                                 std::optional<std::string> context = std::nullopt);

nlohmann::json unsupportedProgram(const std::string& token,
                                   std::optional<int> lineNumber = std::nullopt,
                                   std::optional<std::string> context = std::nullopt);

nlohmann::json unsupportedDelimiter(const std::string& token,
                                     std::optional<int> lineNumber = std::nullopt,
                                     std::optional<std::string> context = std::nullopt);

// BUG-105 secondary ①: `unsupported_command` is raised for causes that are not
// the same fact, and no single sentence is honest about all of them. The CALLER
// knows which one it is, so it is passed in rather than guessed at inside the
// error layer — mirroring dispatcher.cpp's `byIllegalInStata` variant split
// (decided at the call site by a lookup, never by a second hardcoded list down
// here). Adding a cause here obliges a matching arm in unsupportedCommand().
enum class UnsupportedCommandKind {
  kUnknownToken,       // nothing in PIET's command table matched this name
  kNotImplemented,     // a recognized Stata command/feature PIET does not implement
  kUnknownSubcommand,  // the head command IS implemented; this subcommand is not
};

// NOTE: unlike its siblings above, this helper's message text deliberately no
// longer mirrors errors.py verbatim — the frozen Python wording ("If this is a
// valid Stata command, it may be added in a later milestone") is the BUG-105
// secondary ① defect itself. Do not "restore" it.
nlohmann::json unsupportedCommand(const std::string& cmd,
                                   std::optional<int> lineNumber = std::nullopt,
                                   std::optional<std::string> context = std::nullopt,
                                   std::string reason = "piet_gap",
                                   UnsupportedCommandKind kind =
                                       UnsupportedCommandKind::kUnknownToken);

// AER scope-1 (2026-09-03): the "registered, and refused with a NAMED cause"
// shape the campaign needs ~30 times. A refusal arm emits `unsupported_command`
// whose token IS the command word it was handed, which is exactly the shape
// testRegistryRoutes() reads as route drift. Carrying `refused:<cause>` instead
// of the generic `piet_gap` reason lets the drift guard tell the two apart, so
// a registered-and-refused command can be honest about WHY without pretending
// to be an unrecognised token. Callers pass the cause, never a sentence.
nlohmann::json refusedCommand(const std::string& cmd, const std::string& cause,
                               std::optional<int> lineNumber = std::nullopt);

nlohmann::json unsupportedOption(const std::string& opt, const std::string& hint = "",
                                  std::optional<int> lineNumber = std::nullopt,
                                  std::optional<std::string> context = std::nullopt,
                                  std::optional<std::string> reason = std::nullopt);

nlohmann::json noDataLoaded(std::optional<int> lineNumber = std::nullopt);

nlohmann::json engineError(const std::string& message, const std::string& code = "engine_error",
                            std::optional<int> lineNumber = std::nullopt,
                            std::optional<std::string> context = std::nullopt);

// BS-100 (BETA A4): coarse Stata rc taxonomy — maps an error BLOCK (as
// produced by errorBlock()/engineError() above, or a re-thrown CobraError's
// block) to a coarse Stata return code, replacing the historical boolean
// collapse (every captured error -> "1") dispatcher.cpp's `capture` prefix
// handling used to hardcode.
//
// Mapping (deliberately small — exact-code completeness is BS-118,
// post-beta):
//   unsupported_command                          -> "199" (unrecognized command)
//   syntax_error/unsupported_option/unsupported_macro/
//   unsupported_loop/unsupported_program/unsupported_delimiter -> "198" (invalid syntax)
//   no_data_loaded                                -> "111" (variable/dataset not found)
//   engine_error, message names a missing file    -> "601" (file not found)
//   engine_error, message names an existing file/dir -> "602" (file already exists)
//   engine_error, message names a dir that could not be created -> "693"
//     (mkdir: already exists OR missing parent — real Stata gives the
//     SAME message/rc for both; BETA campaign C4/BS-045)
//   engine_error, message says "variable"/"scalar" + "not found" -> "111"
//     (confirm variable/scalar; BETA campaign C1/C4)
//   engine_error, message says "unknown variable" -> "111" (an unknown name in
//     a row expression: engine shapes `unknown variable X in '...'` and
//     `unknown variable 'X'`; w2n_followups E12-E22)
//   engine_error, message is a confirm type/format mismatch ("is not
//     numeric"/"is not a string variable"/"is not an integer value"/
//     "found where number expected"/"invalid name") -> "7" (type mismatch;
//     BETA campaign C1/C4)
//   engine_error, message says "already defined" -> "110" (confirm new
//     variable/file conflict; BETA campaign C4/BS-042)
//   engine_error, anything else (the largest bucket — most cobra
//     computation/data errors: singular matrix, column not found,
//     non-convergence, ...) -> "1", UNCHANGED from before this map existed.
// Any block missing a recognized `code` at all also falls back to "1".
// Takes ordered_json (not plain json) — matches every call site's own JSON
// type (dispatcher.cpp/estimation_m4.cpp/etc. all alias `ojson =
// nlohmann::ordered_json`), sidestepping any cross-basic_json-specialization
// conversion question entirely.
std::string coarseRc(const nlohmann::ordered_json& block);

// FU-852: the literal of an impossible date-literal refusal -- `tq(2010q5)` out of
// Stata's "tq(2010q5) invalid" (r(198)) or of the engine Parser's "expr:
// tq(2010q5) invalid in '<expression>'" -- else "". The name must be one of the
// literal pseudo-functions (cecon::literal_scale_for).
std::string invalidDateLiteral(const std::string& msg);

// FU-875: Stata's r(132) text -- "too few ')' or ']'" / "too many ')' or ']'"
// (cecon::bracket_imbalance) -- for an engine Parser syntax error ("expr: <what>
// in '<expression>'") whose expression does not pair its brackets, else "".
std::string unbalancedExpr(const std::string& msg);

// FU-875: Stata's refusal of a tin()/twithin() call the engine Parser cannot
// expand (src/data/frame.cpp, "expr: tin() requires tsset in '<expression>'" and
// its siblings), else "", with the rc in *rc: no tsset is "time-series operators
// not allowed", r(101), whatever the arguments; on a tsset series a call without
// its comma, or an endpoint off the series' scale, is "invalid syntax", r(198)
// (StataBE 19.5: `count if tin(2010q2)`, `tin(2010q2, 2010q3, 2010q4)` and
// `tin(2010, 2011)` on a %tq series).
std::string timeSpanRefusal(const std::string& msg, std::string* rc);

// ---------------------------------------------------------------------------
// BS-097: shared option consume-or-reject net (PIET's analog of the engine's
// g_argv_consumed/check_unconsumed_flags E6 net — apps/cmds/common.cpp on
// the cobra side). cobra needs argv-INDEX bookkeeping because a raw argv
// flag and its value are two separate tokens (a legitimate option's value
// must never be mistaken for a leftover unrecognized flag just because it
// starts with "--"); PIET's tokenizer already solves that structurally —
// parseOpts() binds each option's paren-value to its key before a dispatch
// arm ever sees it (eval/tokenizer.cpp) — so "consumed" reduces to a plain
// per-KEY question: is this key one the calling arm/handler actually reads?
//
// `opts` is the command's parsed post-comma option list (ParsedCommand::opts
// or a handler's own opts parameter); `known` is the COMPLETE set of option
// keys that arm/handler's existing code actually consumes (every spelling —
// callers must enumerate real aliases, e.g. {"vce","vcov"} or {"gen",
// "generate"}). Returns nullopt when every key in `opts` is recognized
// (nothing to reject — the common case); otherwise an r(198)-parity
// CommandErrorResult (code "unsupported_option", message
// "option `KEY` not allowed", the EXACT wording dispatcher.cpp's summarize
// arm / handlers.cpp's runRegress / estimation_m4.cpp's shared xtreg/logit/
// probit builder already use — see testClusterVceAndUnknownOptionFixes in
// selftest.cpp) naming the FIRST unrecognized key, so a caller just does:
//
//   static const std::unordered_set<std::string> kKnown = {"pairwise", "sig"};
//   if (auto err = eval::rejectUnknownOptions(parsed.opts, kKnown)) {
//     blocks.push_back(ojson(*err));
//     return blocks;
//   }
//
// Conservative by construction: this only fires on option NAMES `known`
// never lists; it never revisits a key the caller's own logic already
// accepted, so it cannot false-reject anything that worked before a given
// arm adopted this helper (same guarantee the 3 pre-existing hand-rolled
// nets this generalizes already documented individually).
//
// BUG-105 secondary ⑤: the plain reject above reads identically for a
// genuine typo (`merge ..., keepusign`) and for a real Stata option this
// build simply hasn't wired up yet (`merge ..., force`) — both say "not
// allowed" with no way to tell which. `knownButUnimplemented` (optional,
// defaults to empty — every pre-existing caller keeps the 2-arg call and
// today's wording unchanged) names real Stata option keys the calling
// arm has deliberately chosen not to implement; a key found there gets the
// honest "valid Stata option, not yet implemented" wording instead of the
// ambiguous default (mirrors the hand-curated `beta` special case in
// handlers.cpp's regress reject-net, generalized here for callers that
// already use this shared helper).
// BUG-148: Stata option abbreviations. `resolveOptionAbbrev` maps a key to the unique canonical
// name it prefixes (exact spelling wins; ambiguous or <2 chars are left alone, so the caller still
// rejects them). `canonicalizeOptionKeys` rewrites a parsed opts vector IN PLACE — call it BEFORE
// the handler's own `k == "fullname"` comparisons, or an accepted abbreviation is silently ignored
// instead of honoured (that half is what makes this a 2-part fix, not a whitelist patch).
std::string resolveOptionAbbrev(const std::string& key,
                                const std::unordered_set<std::string>& canonical);
void canonicalizeOptionKeys(std::vector<std::pair<std::string, OptValue>>& opts,
                            const std::unordered_set<std::string>& canonical);

// B4 P0-B: the same rule over BOTH of a net's vocabularies at once. Ambiguity is
// a fact about STATA's option list, and `knownButUnimplemented` holds real Stata
// options, so a prefix matching one member of each set is ambiguous and must
// resolve to nothing. Searching them in sequence (unique-in-`known` wins, the
// other set consulted only on a miss) let a real rival sit out the test:
// `export delimited ..., no` is r(198) in StataBE, ambiguous among nolabel /
// novarnames / noquote, and PIET resolved it to `nolabel` and honoured it.
// Which set the single winner lands in still decides the wording at the net.
std::string resolveOptionAbbrevAcross(const std::string& key,
                                      const std::unordered_set<std::string>& known,
                                      const std::unordered_set<std::string>& unimplemented);

std::optional<nlohmann::json> rejectUnknownOptions(
    const std::vector<std::pair<std::string, OptValue>>& opts,
    const std::unordered_set<std::string>& known,
    const std::unordered_set<std::string>& knownButUnimplemented = {});

// ---------------------------------------------------------------------------
// BUG-233 / FIX:OPTNET-ABBREV-SWEEP — THE OPTION VOCABULARY REGISTRY.
//
// BUG-231 was not a bug in one handler. It is a bug in the SHAPE every option
// net has: `rejectUnknownOptions` resolves a >=2-char unique prefix against the
// net's `known` set (BUG-148), so an abbreviation PASSES the gate — and then the
// handler's own `k == "fullname"` comparisons still see the RAW key and skip it.
// Accepted, unread, gone, with no diagnostic and a different answer. Measured on
// 4 nets in 3 files this stint (winsor2/cuts, ttest/unequal, esttab/nostar,
// levelsof/local); the shape is present in every net whose `known` set is
// non-empty, which is 57 of the 77 call sites.
//
// The repair cannot live at the net: `rejectUnknownOptions` receives `opts` by
// const& because every handler receives `const ParsedCommand&`, and several nets
// (poisson, reg3, ttest, csdid, ...) declare their `known` set AFTER the loops
// that need the canonical key, so no in-place rewrite AT the net is ordering-
// correct. The only point that is ordering-correct for EVERY net is the
// dispatcher, before routing — where `parsed` is still a mutable local. That is
// what WS-K found for BUG-231 and closed with a hand-declared table of nine
// oracle-measured minimums; this generalizes it from a table to the real
// vocabularies without copying a single spelling set.
//
// A net REGISTERS the one copy of its vocabulary here and then reads it back for
// its own reject call, so the set is still written exactly once:
//
//   static const eval::OptVocab& kTtestVocab = eval::registerOptVocab(
//       {"ttest"}, {{"level", "by", "unequal", "welch", "paired", "reverse",
//                    "unpaired"}, {}});
//   ...
//   if (auto err = eval::rejectUnknownOptions(parsed.opts, kTtestVocab.known,
//                                             kTtestVocab.unimplemented)) { ... }
//
// `cmds` are the dispatcher command tokens the net serves. Registering the same
// token twice UNIONS the vocabularies — deliberate: a command with two nets
// (csdid and its estat sub-net) must canonicalize against both, and a prefix
// that is unique in one set but ambiguous in the union is then left alone, i.e.
// falls back to today's behaviour. Ambiguity never mis-binds.
//
// ONE-SIDEDNESS, which is the whole safety argument: canonicalizeForCommand
// applies EXACTLY the resolution rule `rejectUnknownOptions` already applies
// internally, against the same vocabulary. So the set of option spellings this
// build ACCEPTS is bit-for-bit unchanged; the only thing that changes is that an
// already-accepted abbreviation now reaches the handler under its canonical
// name instead of being dropped. This sweep cannot invent a Stata abbreviation,
// because it never widens acceptance — that is why it needs no oracle capture,
// unlike WS-K's minimums table, which did widen and therefore did.
struct OptVocab {
  std::unordered_set<std::string> known;
  std::unordered_set<std::string> unimplemented;
  // BUG-430 — DECLARED spellings, for the abbreviations a prefix rule cannot
  // reach. `resolveOptionAbbrev` has a 2-character floor and resolves by unique
  // prefix, and neither can express what an ado's `syntax` line declares: in
  // geonear's `Neighbors(...)` / `NEArcount(...)`, `n()` means NEIGHBORS even
  // though it prefixes both. That is not an ambiguity Stata tolerates and we
  // resolve — it is a declaration Stata makes and we had no way to record.
  //
  // Populate from the package's own `syntax` line (the capitalised stem IS the
  // minimum abbreviation), alias -> canonical, and only for spellings the prefix
  // rule cannot produce. This is BUG-105's declared-spelling-set ruling applied
  // to options instead of command names; the alternative — lowering the global
  // floor — would make every one-letter key bind by accident, which is the trap
  // the floor exists to stop.
  std::unordered_map<std::string, std::string> aliases;
};

// Registers (or unions into) the vocabulary for each token in `cmds` and returns
// a stable reference to the merged entry. Safe at static-init time: the registry
// is a function-local static, so there is no cross-TU initialization order.
const OptVocab& registerOptVocab(std::initializer_list<const char*> cmds, OptVocab v);

// Nullptr when nothing registered `cmd`.
const OptVocab* findOptVocab(const std::string& cmd);

// THE single resolution point. Rewrites `opts`' keys in place to the canonical
// spelling registered for `cmd`. No-op when `cmd` has no registered vocabulary.
void canonicalizeForCommand(std::vector<std::pair<std::string, OptValue>>& opts,
                            const std::string& cmd);

// Guard support (BUG-233 DoD ⑤). Returns every registered command token, so a
// self-test can assert the sweep's inventory instead of trusting a grep.
std::vector<std::string> registeredOptVocabCommands();

// ---------------------------------------------------------------------------
// THE GUARD, and why it lives inside rejectUnknownOptions rather than in a lint.
//
// The class this sweep closes can regrow in exactly one way: someone adds a net
// (or a new command token to an existing one) and does not register its
// vocabulary, so canonicalizeForCommand never fires for it and the handler is
// back to comparing raw keys against an abbreviation the net accepted. A source
// lint could look for that, but a lint only sees the shapes it was taught.
//
// rejectUnknownOptions can see the defect DIRECTLY, at the only moment it
// matters. If a key arrives that is not an exact member of the net's vocabulary
// but resolves to one, then the abbreviation reached the net UNRESOLVED — which
// is precisely the statement "the upstream resolution point did not cover this
// net". The helper still resolves it (behaviour is unchanged, and it must stay
// unchanged for any net that legitimately has no command token of its own), but
// it records the event. A self-test drives abbreviations through dispatch and
// asserts the count is zero; a net that regrows the defect makes it non-zero.
//
// Counts LATE resolutions only — a key that was already canonical when it
// arrived (the post-sweep normal case) is not counted, and neither is a key
// that resolves to nothing and gets rejected.
unsigned long optNetLateResolutionCount();
void resetOptNetLateResolutionCount();
// The most recent late-resolved key, as "raw->canonical", for the failure
// message. Empty when the count is zero.
std::string optNetLastLateResolution();

}  // namespace piet::eval

#endif  // PIET_NATIVE_EVAL_ERRORS_H
