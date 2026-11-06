// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
#pragma once
// Fetch just the columns an expression names, instead of flushing the whole
// resident frame to disk to read them (contract_A §38 `getcols`).
//
// WHY THIS IS SHARED and not replicated per TU, against this codebase's usual
// per-TU convention: the two functions below are a SAFETY PAIR, and the safety
// lives in how they agree. `referencedColumns` refuses to answer unless it can
// prove every identifier against the same shadow card the caller will later
// trust, and `fetchColumns` refuses to half-succeed. Three copies of that pair
// would be three chances for one copy's predicate to drift loose, and a loose
// predicate here is not a slow path — it is a projection missing a column the
// evaluator then reads, which is the silent-wrong-answer shape this project
// spends most of its gates on.
//
// The measured reason any of this exists: PIET's row-wise consumers had no way
// to read values except to make the engine materialize the whole frame — 294 of
// 350 head flushes on the reference slice, ~161 MB each (AFTERCARE2-WRITECENSUS,
// 2026-08-16). The string-mutation entrance moved first (154+42); this header is
// what lets `count`, `levelsof` and the row-wise counters follow.
//
// The shared pair's RED certificate is DEMONSTRATED for the new callers, not argued
// from the sharing: with one value in the fetched file perturbed before it is opened,
// `count if edu > 2` moves 576 -> 577 and `levelsof edu` moves "1 2 3 4 5" ->
// "1 2 2.5 3 4 5". The first perturbation tried (+1.0) was absorbed by levelsof —
// it mapped one integer level onto another that already existed — which is a fact
// about the SEED, not about coverage, and is why the certificate uses a value that
// must create a distinct level.

#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace piet {

class SessionState;
class Datastore;

// The columns `exprs` name, as a fetch list, or nullopt when the answer cannot
// be trusted — no shadow varlist card, or nothing to fetch. `alsoInclude`, when
// non-empty and present on the card, is added unconditionally (a caller that
// needs a target column's existing values as well as its expression's inputs).
//
// Unknown identifiers are LEFT ALONE on purpose: locals, scalars, function names
// and keywords all appear in a Stata expression and the engine resolves them
// itself. What must never happen is missing a real column, so the predicate is
// "every identifier the card confirms is taken", never "collect what looks right".
std::optional<std::vector<std::string>> referencedColumns(
    SessionState& session, const std::vector<const std::string*>& exprs,
    const std::string& alsoInclude);

// Ask the engine for those columns and open the result. Returns nullptr on EVERY
// failure (no live worker, frames not available, head not resident-dirty, the
// request refused, the file unopenable) after removing any torn file — the caller
// then takes its ordinary materialize-the-head path. This is a pure optimization:
// a miss must cost a fallback, never an answer. On success `*pathOut` is the
// projection's path, which the caller owns for the rest of its statement.
std::unique_ptr<Datastore> fetchColumns(SessionState& session,
                                        const std::vector<std::string>& cols,
                                        const std::string& workdir, std::string* pathOut);

}  // namespace piet
