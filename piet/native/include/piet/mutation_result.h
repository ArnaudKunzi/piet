// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// Shared `mutationResult` builder — the varlist_after / n_rows_after
// data_mutation envelope every handler TU that mutates the loaded dataset
// returns (mirrors piet/backend/piet/mutate.py's `_mutation_result`).
//
// Consolidated from FOUR near-identical per-TU copies (handlers.cpp,
// handlers_m3.cpp, handlers_data_verbs.cpp, egen.cpp) per Eval's rule-of-
// three call (COMMS §4 item 9): the duplication had already caused three
// separate MEASURED problems — an ordering defect present in all four that
// a single-copy fix would have left broken in the other three, an
// instrumentation attempt that patched the wrong copy and produced no
// output, and a new gate leg that exercised only one copy while the other
// three stayed unwatched.
//
// THREE SEPARATE meaning-bearing parameters. Do not merge these into one
// list, and do not let a parameter's POSITION alone carry its meaning —
// that exact ambiguity (two of the four prior copies used position 3 for
// `droppedVars`, one used it for `rewrittenVars`, one had no 3rd list
// parameter at all) is what made the duplication dangerous to touch:
//   - newVars:       column(s) genuinely ADDED by this mutation.
//   - droppedVars:   column(s) REMOVED (drop/keep), or — for a rename —
//                    the OLD name being retired. SessionState::
//                    seedNMissForPureAdd (session_state.cpp) reuses this
//                    same slot for a rename's old-name by design; see its
//                    own doc comment (session_state.h) for the full
//                    branch table.
//   - rewrittenVars: column(s) rewritten IN PLACE by a `replace`-style
//                    action=="gen" call with newVars empty.
// Routing one of these into the wrong slot never changes a computed VALUE
// — it only silently stops seedNMissForPureAdd's nMiss carry-forward fast
// path — so only the fullScanCount()==0 gate legs (sort/gsort/order), not
// a value-correctness test, can catch a misroute.
#ifndef PIET_NATIVE_MUTATION_RESULT_H
#define PIET_NATIVE_MUTATION_RESULT_H

#include <string>
#include <vector>

#include "vendor/nlohmann/json.hpp"

namespace piet {
class SessionState;
}

namespace piet::handlers {

// Stage 1b resident-frame fast path first (SessionState::
// tryResidentMutationCard), else invalidates+rebuilds the Datastore. Output
// keys: `new_vars` iff newVars is non-empty, `dropped_vars` iff droppedVars
// is non-empty; rewrittenVars never surfaces in the output JSON — it only
// steers which seedNMissForPureAdd branch fires — matching all four prior
// copies exactly. See the .cpp definition for the seedNMissForPureAdd-
// BEFORE-reseedShadowFrom ordering-fix rationale (all four prior copies
// carried this fix; do not reorder it).
nlohmann::ordered_json mutationResult(SessionState& session, const std::string& action,
                                      const std::vector<std::string>& newVars = {},
                                      const std::vector<std::string>& droppedVars = {},
                                      const std::vector<std::string>& rewrittenVars = {});

}  // namespace piet::handlers

#endif  // PIET_NATIVE_MUTATION_RESULT_H
