// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// translate — C++ port of piet/backend/piet/router/translate.py's
// time-series operator detection + context injection (MI-083,
// CONTRACT-engine §16). M3a scope: ONLY the TS-token/_inject_ts_context
// slice needed to turn the 2 Eval M2-ratification carve-out cells GREEN
// (`feols y L.x1[...]` with/without xtset). The full build_estimation_argv
// / build_iv_argv / build_csdid_argv / ... translate surface stays on the
// Python oracle beyond this milestone (M3b+); the M2 vertical-slice
// handlers.cpp::runRegress keeps building its own argv inline and calls
// into this module only for the TS pieces.
#ifndef PIET_NATIVE_TRANSLATE_H
#define PIET_NATIVE_TRANSLATE_H

#include <string>
#include <vector>

#include "piet/session_state.h"

namespace piet::translate {

// translate.py's _RE_TS_TOKEN: true if `token` starts with a time-series
// operator sequence (L./F./D./S., optionally numbered or numlist-
// parenthesised) followed by a dot, e.g. `L.x`, `L2.x`, `L(1/3).x`.
bool isTsToken(const std::string& token);

// _has_ts_token: true if ANY token in `tokens` is a TS-operator expression.
bool hasTsToken(const std::vector<std::string>& tokens);

// BUG-370: one time-series operator found inside an expression, with the byte
// range it occupies. `token` is the WHOLE thing the engine's expression parser
// would consume — `L.y`, `L2.D.gdp` — base variable included, so a caller can
// substitute a materialised column in its place by position.
struct TsTokenRef {
  std::string token;
  size_t begin = 0;
  size_t end = 0;  // one past the last byte
};

// Every TS operator in `expr`, in order of appearance. The token boundary is
// decided by cecon::is_ts_prefix — the SAME predicate frame.cpp's parser uses to
// take the TS reading — so what this reports and what the engine resolves cannot
// drift apart. Quote-aware. exprHasTsToken is defined on top of this.
std::vector<TsTokenRef> extractTsTokens(const std::string& expr);

// BUG-369: true if an EXPRESSION (gen/replace RHS, an `if` condition) carries a
// time-series operator in the `<LFDS-run>.<ident>` form cobra's expression parser
// resolves. Quote-aware. The paren-numlist variants isTsToken accepts are
// varlist-only and deliberately not matched here.
bool exprHasTsToken(const std::string& expr);

// injectTsContext, gated on exprHasTsToken(expr) — send the session's tsset/xtset
// metadata only when the expression actually needs it, so an expression with no
// TS operator cannot start failing because a since-dropped timevar is named.
std::vector<std::string> injectTsContextForExpr(const std::vector<std::string>& argv,
                                                const std::string& expr,
                                                const SessionState& session);

// B3(c): the session's Stata RNG stream (SessionState::seed.stream(), mt64) as
// `--rng-mt64 <c(rngstate)>`, appended only when the expression takes a draw
// (runiform/runiformint/rnormal/uniform). The engine draws from that state in
// Stata's order and reports the raw draws it took as `rng_draws`; the caller
// passes the response to absorbRngDraws, which advances the session stream past
// them. A read-only probe that must see the same draws as the store simply does
// not absorb. (The legacy `rngStream` counter still advances: other Philox
// consumers key on it.)
std::vector<std::string> injectRngContextForExpr(const std::vector<std::string>& argv,
                                                 const std::string& expr,
                                                 SessionState& session);

// Whether the expression calls a draw function (runiform/runiformint/rnormal/
// uniform) outside a string literal -- the test injectRngContextForExpr keys on.
bool exprHasDrawToken(const std::string& expr);

// Advance the session stream by the engine's reported `rng_draws` (absent = 0).
void absorbRngDraws(const nlohmann::json& engineResponse, SessionState& session);

// BUG-1123: a generate's engine response may report `store_overflow`, the values
// its --store-type could not hold and stored as missing. Copied onto the
// data_mutation block, where the dispatcher's "(N missing values generated)" note
// leaves them out of its count, as Stata's does, and drops the field again.
void carryStoreOverflow(const nlohmann::json& engineResponse, nlohmann::ordered_json& block);

// _inject_ts_context: appends --xtset/--tsset to `argv` per session's
// panelvar/timevar (CONTRACT-engine §16):
//   panelvar set -> --xtset panelvar[,timevar]
//   only timevar set -> --tsset timevar
//   neither set -> no-op (cobra surfaces ts_no_tsset cleanly)
// Returns a NEW vector; does not mutate `argv`.
std::vector<std::string> injectTsContext(const std::vector<std::string>& argv,
                                          const SessionState& session);

}  // namespace piet::translate

#endif  // PIET_NATIVE_TRANSLATE_H
