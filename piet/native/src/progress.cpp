// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
#include "piet/progress.h"

#include <chrono>
#include <thread>

#include "vendor/nlohmann/json.hpp"

namespace piet {

namespace {

// A fake 5-line "do-file" — just enough to exercise total_lines/current_line
// advancing and current_cmd truncation, without a real interpreter (that's
// M2+). Deliberately Stata-flavored so the demo page reads naturally.
const std::vector<std::string>& syntheticScript() {
  static const std::vector<std::string> kScript = {
      "sysuse auto, clear",
      "summarize price mpg weight",
      "regress price mpg weight",
      "predict resid, residuals",
      "esttab using results.csv, replace",
  };
  return kScript;
}

nlohmann::json makeTick(bool running, int currentLine, int totalLines,
                         const std::string& currentCmd, long long elapsedMs,
                         int blocksReady, bool cancelRequested,
                         const std::string* stoppedReason) {
  nlohmann::json j;
  j["running"] = running;
  j["current_line"] = currentLine;
  j["total_lines"] = totalLines;
  j["current_cmd"] = currentCmd.substr(0, 120);  // contract: truncated ~120 chars
  j["elapsed_ms"] = elapsedMs;
  j["blocks_ready"] = blocksReady;
  j["do_path"] = nullptr;  // M0 synthetic run has no backing .do file
  j["cancel_requested"] = cancelRequested;
  j["stopped_reason"] = stoppedReason ? nlohmann::json(*stoppedReason) : nlohmann::json(nullptr);
  return j;
}

}  // namespace

void runSyntheticProgress(const ProgressSink& sink, const std::atomic<bool>* cancel) {
  const auto& script = syntheticScript();
  const int total = static_cast<int>(script.size());
  const auto start = std::chrono::steady_clock::now();

  for (int i = 0; i < total; ++i) {
    if (cancel && cancel->load()) {
      auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                         std::chrono::steady_clock::now() - start)
                         .count();
      std::string reason = "cancelled";
      auto tick = makeTick(/*running=*/false, i, total, script[i], elapsed, i,
                            /*cancelRequested=*/true, &reason);
      sink(tick.dump());
      return;
    }

    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                       std::chrono::steady_clock::now() - start)
                       .count();
    auto tick = makeTick(/*running=*/true, i + 1, total, script[i], elapsed, i,
                          /*cancelRequested=*/false, nullptr);
    sink(tick.dump());

    std::this_thread::sleep_for(std::chrono::milliseconds(150));
  }

  auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                     std::chrono::steady_clock::now() - start)
                     .count();
  std::string reason = "completed";
  auto finalTick = makeTick(/*running=*/false, total, total, script.back(), elapsed, total,
                             /*cancelRequested=*/false, &reason);
  sink(finalTick.dump());
}

}  // namespace piet
