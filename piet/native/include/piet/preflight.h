// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// SPEC II-4 — "Pre-flight warning, never OOM" (wired 2026-09-02, User directive).
//
// Before a data-size-scaling operation (merge · joinby · append · reshape ·
// expand · cross) the dispatcher asks this gate whether the operation's
// estimated memory cost fits the Settings memory budget. Over budget:
//   · a do-file run in the app (a real Shell, on the do-worker thread) ASKS —
//     the same request_dialog push `_request()` uses, now with fixed choices:
//     proceed / raise the budget (the frontend persists the suggestion through
//     settings.set, then answers) / cancel;
//   · anywhere it cannot ask (headless `--do`, the single command line on the
//     UI thread) it WARNS on stderr and in an echo block and proceeds, because
//     a batch run must never hang on a question and the UI thread must never
//     block; PIET_PREFLIGHT=strict makes it REFUSE instead (an honest
//     `memory_preflight` error, nothing executed) and PIET_PREFLIGHT=off
//     disables the gate.
// The estimate is a MODEL, deliberately simple and stated in the message so a
// user can see what was assumed; it errs high. It never changes what an
// operation computes — only whether, and after what question, it starts.
#ifndef PIET_NATIVE_PREFLIGHT_H
#define PIET_NATIVE_PREFLIGHT_H

#include <cstdint>
#include <string>
#include <vector>

#include "piet/session_state.h"
#include "vendor/nlohmann/json.hpp"

namespace piet::preflight {

using ojson = nlohmann::ordered_json;

// Pure cost model, in bytes. `factor` is expand's copy count (0 when n/a).
//   merge/joinby : master + using + 2 x max(master, using)
//   append       : 2 x (master + using)
//   reshape      : 3 x master
//   expand k     : master x (1 + k)
//   cross        : master + using + 8 x max(master, using)
int64_t estimateBytes(const std::string& verb, int64_t masterBytes, int64_t usingBytes,
                      int64_t factor);

// The effective memory budget in bytes (the setting, or the recommended
// half-of-RAM when the slider was never moved).
int64_t budgetBytes();

enum class Verdict { Proceed, Cancel };

// The gate. `usingPathRaw` is the `using` file as typed ("" when none); `factor`
// as above. May append an echo (warning) or error (refusal / cancellation)
// block to `blocks`. Returns Cancel when the operation must not run.
Verdict gate(const std::string& verb, SessionState& session, const std::string& usingPathRaw,
             int64_t factor, ojson& blocks);

}  // namespace piet::preflight

#endif  // PIET_NATIVE_PREFLIGHT_H
