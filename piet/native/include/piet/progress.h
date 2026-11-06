// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// Synthetic run-progress generator for M0. Produces JSON objects matching
// the B1 schema FROZEN in internal note piet/CONTRACT.md (contract_B v1.11.0,
// `GET /api/run_progress`) field-for-field:
//
//   { running, current_line, total_lines, current_cmd, elapsed_ms,
//     blocks_ready, do_path, cancel_requested, stopped_reason }
//
// M0 does not run a real do-executor — that is M2+ (dispatcher/doexec land
// there). Here we fake a 5-step "run" (a fixed list of Stata-flavored
// command strings) advancing on a timer, purely to prove the push-channel
// end-to-end: piet::Shell::postEvent() delivers each tick to
// window.pietEvents in JS, and to --selftest's in-process listener.
#ifndef PIET_NATIVE_PROGRESS_H
#define PIET_NATIVE_PROGRESS_H

#include <atomic>
#include <functional>
#include <string>
#include <vector>

namespace piet {

// One tick of the B1 schema, already serialized as a JSON object string.
using ProgressSink = std::function<void(const std::string& jsonTick)>;

// Runs a synthetic 5-line "do-file" on the CALLING thread, sleeping briefly
// between lines and invoking `sink` after each line (including a final
// running:false/stopped_reason:"completed" tick, and — if `cancel` is set
// true mid-run by another thread — a running:false/stopped_reason:"cancelled"
// tick instead of finishing all 5 lines). Callers that want this off the UI
// thread must spawn their own std::thread and call this from it (see
// main.cpp's demo wiring and selftest.cpp's synchronous use).
void runSyntheticProgress(const ProgressSink& sink,
                           const std::atomic<bool>* cancel = nullptr);

}  // namespace piet

#endif  // PIET_NATIVE_PROGRESS_H
