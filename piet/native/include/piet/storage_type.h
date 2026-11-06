// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
#pragma once
// BUG-471 T2: the ONE place PIET decides which Stata storage type a numeric
// store takes. The engine rounds (`cobra eval --store-type`, T1); this file only
// decides the token that rides the argv and the label the type plane records.
//
// Oracle: tests/eval/golden/aer_w2/w2e_float_default.log (StataBE 19.5). Every
// rule below cites the arm that pins it; nothing here is assumed.
//   create (generate / egen / bysort: generate): the declared type, else c(type)
//     (gen_untyped_13 :96 float; settype_double_gen :1252 double).
//   replace: the variable's own type (replace_float_23 :761, replace_double_23
//     :777, replace_untyped_23 :793), widened by Stata's lattice when an integer
//     type receives a value it cannot hold (replace_byte_promote :810,
//     replace_int_frac :843, replace_long_frac :859, replace_byte_overflow :876,
//     replace_int_overflow :893, replace_byte_promote_settype_double :912), or
//     kept and truncated under `nopromote` (replace_byte_nopromote :827).
//
// Layers: none of this is expression semantics. The macro/arith layer's float()
// lives in eval/exprfns.cpp; the CiParser is untouched.

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "vendor/nlohmann/json.hpp"

namespace piet {
class SessionState;
}

namespace piet::storage {

// byte|int|long|float|double — the numeric storage-type tokens.
bool isNumericType(const std::string& t);
bool isIntegerType(const std::string& t);

// generate / egen / bysort: generate. `declaredToken` is the lowercased token
// stripStorageType kept ("" = none). A non-numeric token (str#) is the string
// path's business and resolves to "".
std::string resolveCreateType(const SessionState& session, const std::string& declaredToken);

// The label a successful creation records (dispatcher.cpp recordDeclaredStataType's
// rule, for a creator that resolved its type itself, e.g. predict): no declaration
// and a float store = BUG-295's implicit plane; anything else = varMeta stataType.
// `stored` "" (nothing numeric was stored) records nothing.
void recordCreateType(SessionState& session, const std::string& varname,
                      const std::string& declaredToken, const std::string& stored);

// replace: varMeta[v].stataType when it is a numeric type; else "float" when v
// is on implicitFloatVars (an untyped in-session creation); else "double" (no
// rounding). The last case is the unrecorded-creator fallback (a column some
// path created without recording its type): it must not start rounding on
// replace, and it is COUNTED (session.unrecordedReplaceFallbacks and the
// process total below).
std::string resolveReplaceType(SessionState& session, const std::string& varname);
int64_t unrecordedReplaceFallbackTotal();

// FU-243: the type claim belongs to the variable, not to its name. A verb that
// gives a variable a new name (rename/ren: `move`) or a copy of itself
// (clonevar: copy) carries the source's claim -- the declared varMeta stataType,
// or the implicit float of an untyped creation -- to the target, so a later
// `replace` on the target resolves (resolveReplaceType above) to what it did on
// the source. Before, the implicit float stayed under the old name and the
// target fell to the double fallback: w2ah_data_verbs.log ah_fu243_clone /
// ah_fu243_rename store float (+1.24924a0000000X-002), main stored the double
// 2/7 under a `float` label. Only the claim moves; labels and formats are the
// caller's. A source with no claim leaves the target with none.
void carryTypeClaim(SessionState& session, const std::string& from, const std::string& to,
                    bool move);

// The argv tail for a resolved type: {"--store-type", t}, or nothing for "" and
// "double" (the engine's default, so every double store keeps its old argv).
void appendStoreTypeArg(std::vector<std::string>& args, const std::string& type);

// Stata's replace lattice on the values a replace stores (non-missing only).
// `anyFraction`: some value is not an integer. [lo, hi]: their range.
std::string promotedType(const std::string& current, bool anyFraction, double lo, double hi);

// A plain numeric literal right-hand side (`2.7`, `-1e3`), or a missing literal
// (`.`, `.a`) which returns NaN. nullopt for anything else.
std::optional<double> numericLiteral(const std::string& rhs);

// The scratch name the replace probe assigns (the caller builds the probe's
// --gen / --bygen argument around it).
extern const char* const kReplaceProbeVar;

struct ReplaceTypeDecision {
  std::string storeType;                          // the type to store with
  std::optional<nlohmann::ordered_json> error;    // a refusal block, nothing stored
  int engineCalls = 0;                            // calls the decision itself made
};

// Decide a replace's final store type. Non-integer `resolved` and `noPromote`
// return `resolved` with no engine call. A literal right-hand side is decided
// from the literal, except a widening literal under if/in (a replace touching no
// row widens nothing). Otherwise ONE read-only engine eval computes the
// touched values into a scratch column of a projection (or of the head file)
// and summarizes them: the session's frame, r() and e() are not touched (no
// dispatch(), no --out, stateless routing). `probeFlag`/`probeArg` are the
// --gen/--bygen pair assigning kReplaceProbeVar exactly as the store will
// assign the target; `colExprs` name every column the probe reads.
// `drawContext`: the store will carry --rng-seed/--rng-stream, so the probe
// uses the SAME stream (the session counter is rewound after it).
ReplaceTypeDecision decideReplaceStoreType(SessionState& session, const std::string& resolved,
                                           bool noPromote, const std::string& varname,
                                           const std::string& rhs, bool qualified,
                                           const std::string& probeFlag,
                                           const std::string& probeArg,
                                           const std::vector<std::string>& colExprs,
                                           bool drawContext);

// The display format `generate` gives a numeric storage type (describe:
// double %10.0g, long %12.0g, byte/int %8.0g, float %9.0g). StataBE keeps the
// format a variable was created with when its type later changes
// (exprfns_contract_oracle.log:646/736 "b1 was byte now float" -> b1 float
// %8.0g; :655/737 "i1 was int now long" -> i1 long %8.0g; :735 `recast double
// y` of a float -> y double %9.0g), so callers pass the creation-time type.
// No type (an implicit float, or no claim) is %9.0g.
std::string stataDefaultFormat(const std::optional<std::string>& t);

// After a successful replace store: when the lattice widened the type, the
// label follows the store (a double under a byte label is a wrong answer), and
// a variable with no explicit format keeps the default format of the type it
// held before (stataDefaultFormat above).
void recordReplaceType(SessionState& session, const std::string& varname,
                       const std::string& resolved, const std::string& stored);

// egen: the function name of `fn(...)` (lowercased, "" if none).
std::string egenFunctionName(const std::string& rhs);
// tag(), seq() and mode() ignore the requested type (egen tag() makes a byte;
// egen seq() a byte; egen mode() the source's own type): no --store-type.
bool egenIgnoresStoreType(const std::string& fn);
// The label those three take after a successful egen (egen_tag_type :1123
// `byte`; seq = `fixedType`, the type its widening replace left (runEgen's
// fixedTypeOut; byte when empty, egen_seq_type :1137); mode = `fixedType`, the
// str# its compress left, else the source variable's type). Any other function
// takes `fixedType` when runEgen handed one back -- the type
// runEgenAdoFunction's functions recorded for their result -- and otherwise returns
// false (the caller records the declared/c(type) label).
bool recordEgenFixedType(SessionState& session, const std::string& newvar,
                         const std::string& rhs, const std::string& fixedType = std::string());

}  // namespace piet::storage
