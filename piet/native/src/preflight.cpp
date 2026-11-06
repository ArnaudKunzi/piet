// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
#include "piet/preflight.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

#include "piet/handlers.h"   // resolveUserPath
#include "piet/settings.h"

namespace piet::preflight {

namespace {

std::string humanBytes(int64_t b) {
  char buf[48];
  const double mb = static_cast<double>(b) / (1024.0 * 1024.0);
  if (mb >= 1024.0) std::snprintf(buf, sizeof buf, "%.1f GiB", mb / 1024.0);
  else std::snprintf(buf, sizeof buf, "%.0f MB", mb);
  return buf;
}

std::string policy() {
  const char* e = std::getenv("PIET_PREFLIGHT");
  if (!e || !*e) return "ask";
  std::string s(e);
  for (auto& ch : s) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
  return s;  // "off" | "strict" | anything else = ask
}

int64_t masterBytesOf(SessionState& session) {
  if (!session.currentHbin().has_value()) return 0;
  std::error_code ec;
  const auto sz = std::filesystem::file_size(*session.currentHbin(), ec);
  if (!ec && sz > 0) return static_cast<int64_t>(sz);
  // No file yet (a virtual head): rows x cols x 8 from the metadata.
  if (Datastore* ds = session.datastore()) return ds->nRows() * static_cast<int64_t>(ds->nCols()) * 8;
  return 0;
}

int64_t usingBytesOf(SessionState& session, const std::string& raw) {
  if (raw.empty()) return 0;
  std::string p = raw;
  // strip surrounding quotes the way the handlers do before resolving
  if (p.size() >= 2 && ((p.front() == '"' && p.back() == '"') || (p.front() == '\'' && p.back() == '\'')))
    p = p.substr(1, p.size() - 2);
  const std::string resolved = resolveUserPath(session, p);
  std::error_code ec;
  const auto sz = std::filesystem::file_size(resolved, ec);
  if (ec) return 0;  // not there (yet): estimate from the master alone
  return static_cast<int64_t>(sz);
}

ojson echoNote(const std::string& text) {
  ojson b;
  b["type"] = "echo";
  b["text"] = text;
  return b;
}

ojson refusal(const std::string& text) {
  ojson b;
  b["type"] = "error";
  b["code"] = "memory_preflight";
  b["message"] = text;
  return b;
}

}  // namespace

int64_t estimateBytes(const std::string& verb, int64_t master, int64_t usingB, int64_t factor) {
  master = std::max<int64_t>(0, master);
  usingB = std::max<int64_t>(0, usingB);
  const int64_t big = std::max(master, usingB);
  if (verb == "merge" || verb == "joinby") return master + usingB + 2 * big;
  if (verb == "append") return 2 * (master + usingB);
  if (verb == "reshape") return 3 * master;
  if (verb == "expand") return master * (1 + std::max<int64_t>(1, factor));
  if (verb == "cross") return master + usingB + 8 * big;
  // AER-SCOPE1 bundle F (RESHAPE). Each of these three round-trips the WHOLE
  // frame through a CSV, so the peak is the frame plus the text plus the
  // rebuilt frame rather than an in-place edit.
  if (verb == "xpose") return 3 * master;
  if (verb == "bsample") return 3 * master;
  // stack emits k copies of the frame's rows into one column pair.
  if (verb == "stack") return master * (1 + std::max<int64_t>(1, factor));
  return master + usingB;
}

int64_t budgetBytes() {
  const settings::Settings st = settings::load();
  int64_t mb = st.memoryBudgetMb > 0 ? st.memoryBudgetMb : settings::detectSysInfo().recommendedBudgetMb;
  if (mb <= 0) mb = 2048;
  return mb * 1024 * 1024;
}

Verdict gate(const std::string& verb, SessionState& session, const std::string& usingPathRaw,
             int64_t factor, ojson& blocks) {
  const std::string pol = policy();
  if (pol == "off") return Verdict::Proceed;
  const int64_t master = masterBytesOf(session);
  if (master <= 0) return Verdict::Proceed;  // nothing loaded: the handler's own error applies
  const int64_t usingB = usingBytesOf(session, usingPathRaw);
  const int64_t need = estimateBytes(verb, master, usingB, factor);
  const int64_t budget = budgetBytes();
  if (need <= budget) return Verdict::Proceed;

  const int64_t suggestMb = ((need / (1024 * 1024)) / 128 + 1) * 128;  // next 128 MB step above
  const std::string basis =
      verb == "expand"
          ? "data " + humanBytes(master) + " x " + std::to_string(1 + std::max<int64_t>(1, factor))
          : "data " + humanBytes(master) + (usingB > 0 ? " + using " + humanBytes(usingB) : std::string()) +
                ", model: " + verb;
  const std::string what = verb + " would need about " + humanBytes(need) + " (" + basis +
                           "); the memory budget is " + humanBytes(budget) + ".";

  if (pol == "strict") {
    blocks.push_back(refusal("memory pre-flight: " + what +
                             " Refused because PIET_PREFLIGHT=strict; raise the budget in Settings, "
                             "or unset PIET_PREFLIGHT to run anyway."));
    return Verdict::Cancel;
  }

  // Can we ask? Only a do-file run under a real Shell, on its worker thread.
  if (session.hasRequestPostEvent() && piet::isCurrentThreadADoWorker()) {
    std::vector<std::pair<std::string, std::string>> choices = {
        {"proceed", "Proceed anyway"},
        {"raise", "Raise the budget to " + std::to_string(suggestMb) + " MB and proceed"},
        {"cancel", "Cancel this command"}};
    ojson extra;
    extra["suggest_budget_mb"] = suggestMb;
    auto answer = session.requestUserInput("_piet_preflight", "Memory pre-flight: " + what, choices, extra);
    if (!answer.has_value() || *answer == "cancel") {
      blocks.push_back(refusal("memory pre-flight: " + what + " Cancelled" +
                               (answer.has_value() ? " by you." : " (the run was stopped).")));
      return Verdict::Cancel;
    }
    return Verdict::Proceed;  // "proceed" or "raise" (the frontend already persisted the new budget)
  }

  // Cannot ask (headless, or the single command line): warn and proceed.
  std::fprintf(stderr, "[piet_native] memory pre-flight: %s Proceeding (set PIET_PREFLIGHT=strict to refuse).\n",
               what.c_str());
  blocks.push_back(echoNote("note: memory pre-flight: " + what + " Proceeding."));
  return Verdict::Proceed;
}

}  // namespace piet::preflight
