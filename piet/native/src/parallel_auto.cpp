// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// P9 automode · L1 + predicate + L4. See parallel_auto.h.
#include "piet/parallel_auto.h"

#include <cstdio>
#include <cstdlib>
#include <cstdlib>
#include <regex>

#include "piet/parallel.h"
#include "piet/parallel_effects.h"
#include "piet/parallel_paths.h"
#include <algorithm>
#include <map>

#include "piet/strutil.h"

namespace piet::parallel {

namespace {

bool envOn(const char* name) {
  const char* v = std::getenv(name);
  return v != nullptr && *v != '\0' && std::string(v) != "0";
}

// Does this statement's text reference r()/e() explicitly? The effect table's
// readsResults flag is per-COMMAND ("this verb may read results"), which is the
// right granularity for refusing `lincom`, but `display` carries it too and
// most displays read nothing. A textual check on the expanded statement is the
// discriminator, and it errs toward refusing (a mention inside a string literal
// still refuses).
bool mentionsResults(const std::string& s) {
  static const std::regex re(R"(\b[re]\(|\b_b\[|\b_se\[)");
  return std::regex_search(s, re);
}

}  // namespace

bool autoModeEnabled() {
  if (envOn("PIET_PARALLEL_WORKER")) return false;  // one generation only
  return envOn("PIET_PARALLEL_AUTO");
}

int autoModeRequestedWorkers() {
  const char* v = std::getenv("PIET_PARALLEL_AUTO_WORKERS");
  int requested = 0;
  if (v != nullptr && *v != '\0') {
    long n = std::strtol(v, nullptr, 10);
    if (n > 0) requested = static_cast<int>(n);
  }
  return requested;
}

int autoModeWorkers() { return resolveWorkers(autoModeRequestedWorkers()).n; }

bool autoModeUnsafeForce() {
  return autoModeEnabled() && envOn("PIET_PARALLEL_AUTO_UNSAFE_FORCE");
}

RegionPlan planRegion(const std::vector<std::string>& stmts,
                      const std::vector<std::string>& following, SessionState& session) {
  RegionPlan plan;
  plan.count = stmts.size();
  plan.reportable = stmts.size() >= kAutoMinRegion;
  if (stmts.size() < kAutoMinRegion) {
    plan.refusalReason = "fewer than " + std::to_string(kAutoMinRegion) + " consecutive statements";
    plan.failedAt = stmts.size();
    return plan;
  }

  // Rule 13 — parent-side state a child cannot see. Checked first because it is
  // a property of the SESSION, not of any statement, so no amount of statement
  // analysis could discover it.
  if (!session.preserveStack.empty()) {
    plan.refusalReason = "a `preserve` is open";
    plan.failedAt = stmts.size();
    return plan;
  }

  // Resolved write destinations per statement, filled by rule 10 below and
  // compared pairwise once every statement has been summarized.
  std::map<size_t, std::vector<std::string>> pathKeys;
  std::string pathWhy;
  std::vector<StatementSummary> sums;
  sums.reserve(stmts.size());
  bool anyWritesResults = false;
  bool anyWritesDataset = false;
  bool anyUsesEstimates = false;

  for (size_t i = 0; i < stmts.size(); ++i) {
    StatementSummary s = summarizeStatement(stmts[i], session);
    if (!s.known) {  // rule 3 — the whole axiom in one line
      plan.refusalReason = s.why;
      plan.failedAt = i;
      return plan;
    }
    if (s.eff.control) {  // rule 4
      plan.refusalReason = "statement " + std::to_string(i + 1) +
                           " touches session control (exit/log/set/preserve/postfile/...)";
      plan.failedAt = i;
      return plan;
    }
    if (s.eff.writesLocals) {  // rule 5 — TIGHTER than the brief's "pairwise
      // disjoint": a worker returns NO macro state, so even disjoint writes are
      // simply lost. Locals are decided here; session-globals are decided BY NAME
      // below, because for those the answerable question is "does anyone read it".
      plan.refusalReason =
          "statement " + std::to_string(i + 1) + " writes a local, which a worker never returns";
      plan.failedAt = i;
      return plan;
    }
    if (s.eff.globalWriteUnresolved) {
      plan.refusalReason = "statement " + std::to_string(i + 1) +
                           " writes a session-global whose name is built from a macro";
      plan.failedAt = i;
      return plan;
    }
    if (s.eff.rng) {  // rule 6
      plan.refusalReason = "statement " + std::to_string(i + 1) + " consumes the RNG stream";
      plan.failedAt = i;
      return plan;
    }
    if (s.eff.readsResults && mentionsResults(stmts[i])) {  // rule 7
      plan.refusalReason = "statement " + std::to_string(i + 1) +
                           " reads r()/e(), which a task never inherits";
      plan.failedAt = i;
      return plan;
    }
    if (s.eff.writesFiles && s.eff.filePathsUnresolved) {  // rule 10
      // Before refusing, try to RESOLVE the destination by substituting this
      // call site's arguments into the program's path templates. That is the
      // whole question rule 10 asks — "can I prove these two tasks write to
      // different files" — and for a program call it is answerable.
      const std::string headTok = strutil::trim(stmts[i]);
      const size_t sp0 = headTok.find_first_of(" \t,");
      const std::string prog = sp0 == std::string::npos ? headTok : headTok.substr(0, sp0);
      bool resolved = false;
      if (session.programs.count(prog) != 0) {
        ProgramPaths pp = programWritePaths(prog, session);
        if (pp.ok) {
          const std::vector<std::string> actuals =
              splitStataArgs(sp0 == std::string::npos ? std::string()
                                                      : strutil::trim(stmts[i].substr(sp0)));
          std::vector<std::string> keys;
          bool allOk = true;
          for (const PathTemplate& t : pp.templates) {
            RenderedPath r = renderPath(t, actuals);
            if (!r.ok) { allOk = false; break; }
            keys.push_back(r.key);
          }
          if (allOk) {
            resolved = true;
            pathKeys[i] = keys;
          }
        }
        if (!resolved && !pp.why.empty()) pathWhy = pp.why;
      }
      if (!resolved) {
        plan.refusalReason =
            "statement " + std::to_string(i + 1) + " writes a file whose path this layer cannot " +
            "resolve, so disjointness is unproven" + (pathWhy.empty() ? "" : " (" + pathWhy + ")");
        plan.failedAt = i;
        return plan;
      }
    }
    anyWritesResults |= s.eff.writesResults;
    anyWritesDataset |= s.eff.writesDataset;
    anyUsesEstimates |= s.eff.usesEstimates;
    sums.push_back(s);
  }

  // Rule 10b — pairwise disjointness of the destinations rule 10 resolved.
  // Two keys are COMPARABLE only when their opaque-symbol runs line up; an
  // identical key means the same file, which is a collision whatever the
  // symbols evaluate to.
  for (const auto& [i, ki] : pathKeys) {
    for (const auto& [j, kj] : pathKeys) {
      if (j <= i) continue;
      for (const std::string& a : ki) {
        for (const std::string& b : kj) {
          if (a == b) {
            plan.refusalReason = "statements " + std::to_string(i + 1) + " and " +
                                 std::to_string(j + 1) + " write the same file";
            plan.failedAt = j;
            return plan;
          }
        }
      }
    }
  }

  // Rule 9a — in-region dataset ordering. A task that mutates the frame is only
  // safe if every LATER task re-seeds its own data, because all tasks receive
  // the SAME block-entry head and none of them sees the others' mutations.
  for (size_t i = 0; i < sums.size(); ++i) {
    if (!sums[i].eff.writesDataset) continue;
    for (size_t j = i + 1; j < sums.size(); ++j) {
      if (sums[j].eff.readsDataset && !sums[j].eff.datasetSelfSeed) {
        plan.refusalReason = "statement " + std::to_string(j + 1) +
                             " reads the dataset that statement " + std::to_string(i + 1) +
                             " changes, and does not load its own";
        plan.failedAt = j;
        return plan;
      }
    }
  }

  // Rules 8 and 9b — the successor walks. Whatever the region leaves in r()/e()
  // or in the resident frame is NOT what serial execution would have left there,
  // because a child returns neither. So the region is only safe if nothing after
  // it reads either one before something overwrites it.
  auto walkSuccessorsFor = [&](bool forResults, bool forEstimates,
                               const std::vector<std::string>& tailStmts) -> std::string {
    for (const std::string& next : tailStmts) {
      StatementSummary s = summarizeStatement(next, session);
      if (!s.known)
        return "what follows the region (`" + strutil::trim(next).substr(0, 40) +
               "`) cannot be summarized, so it is unknown whether it reads what the region left";
      if (forEstimates) {
        // handled below, after the shared unknown check
      } else if (forResults) {
        if (s.eff.readsResults && mentionsResults(next))
          return "a later statement reads r()/e() left by the region";
        if (s.eff.writesResults) return "";  // overwritten before any read — safe
      } else {
        if (s.eff.datasetSelfSeed) return "";  // re-seeded before any read — safe
        if (s.eff.readsDataset || s.eff.writesDataset)
          return "a later statement uses the dataset the region left behind";
      }
      if (forEstimates && s.eff.usesEstimates)
        return "a later statement uses the estimates store the region left behind";
      if (s.eff.control)
        return "";  // a control statement ends the window this walk cares about
    }
    return "";  // reached the end of the file: nothing reads it
  };

  // Rule 5b — session-global writes, decided BY NAME.
  //
  // A worker's scalars/matrices/globals die with it, so the region is safe iff
  // (a) no task can OBSERVE a name it did not itself write — otherwise it would
  // be reading a sibling's leftovers, which serial ordering provided and parallel
  // does not — and (b) nothing after the region reads one either. Both are
  // answerable; "does it write session state at all" is not, and refusing on that
  // question alone rejected every program in the acceptance corpus.
  std::vector<std::string> globalNames;
  for (const auto& s2 : sums)
    for (const auto& n : s2.eff.globalWrites)
      if (std::find(globalNames.begin(), globalNames.end(), n) == globalNames.end())
        globalNames.push_back(n);
  if (!globalNames.empty()) {
    for (size_t i = 0; i < stmts.size(); ++i) {
      const std::string headTok = strutil::trim(stmts[i]);
      const size_t sp = headTok.find_first_of(" \t,");
      const std::string prog = sp == std::string::npos ? headTok : headTok.substr(0, sp);
      std::string why;
      if (session.programs.count(prog) != 0) {
        if (!programGlobalsAreSelfWritten(prog, globalNames, session, &why)) {
          plan.refusalReason = why;
          plan.failedAt = i;
          return plan;
        }
      } else {
        // A non-program statement mentioning one of the names is refused unless it
        // is the very statement that writes it.
        for (const std::string& n : globalNames) {
          const bool writesIt =
              std::find(sums[i].eff.globalWrites.begin(), sums[i].eff.globalWrites.end(), n) !=
              sums[i].eff.globalWrites.end();
          if (!writesIt && mentionsWord(stmts[i], n)) {
            plan.refusalReason = "statement " + std::to_string(i + 1) +
                                 " reads session-global `" + n + "` written by another task";
            plan.failedAt = i;
            return plan;
          }
        }
      }
    }
  }

  // END-SHRINK. If what the region leaves behind IS read afterwards, the region
  // is not wrong — it is one statement too long. Dropping its last statement
  // hands that statement back to the serial path, which then leaves exactly what
  // serial would have left, and the shorter region is re-tested. Shrinking stops
  // at kAutoMinRegion, below which there is nothing worth fanning out.
  std::vector<std::string> tail = following;
  size_t take = stmts.size();
  while (true) {
    std::string why;
    if (anyWritesResults) why = walkSuccessorsFor(true, false, tail);
    if (why.empty() && anyWritesDataset) why = walkSuccessorsFor(false, false, tail);
    if (why.empty() && anyUsesEstimates) why = walkSuccessorsFor(false, true, tail);
    if (why.empty() && !globalNames.empty()) {
      for (const std::string& next : tail) {
        StatementSummary ns = summarizeStatement(next, session);
        if (ns.eff.control) break;  // the window this question lives in has closed
        bool hit = false;
        for (const std::string& n : globalNames)
          if (mentionsWord(next, n)) {
            why = "a later statement reads session-global `" + n + "` left by the region";
            hit = true;
            break;
          }
        if (hit) break;
      }
    }
    if (why.empty()) break;
    if (take <= kAutoMinRegion) {
      plan.refusalReason = why;
      plan.failedAt = stmts.size();
      return plan;
    }
    --take;
    tail.insert(tail.begin(), stmts[take]);
    // The dropped statement now runs serially, so recompute what the SHORTER
    // region actually leaves behind rather than reusing the full run's flags.
    anyWritesResults = false;
    anyWritesDataset = false;
    anyUsesEstimates = false;
    for (size_t i = 0; i < take; ++i) {
      anyWritesResults |= sums[i].eff.writesResults;
      anyWritesDataset |= sums[i].eff.writesDataset;
      anyUsesEstimates |= sums[i].eff.usesEstimates;
    }
    if (!anyWritesResults && !anyWritesDataset && !anyUsesEstimates) break;
  }
  plan.count = take;

  plan.approved = true;
  plan.refusalReason.clear();
  return plan;
}

void reportApproved(size_t tasks, int workers, int firstLine, int lastLine) {
  std::fprintf(stderr, "[piet_native] auto-parallel: %zu tasks / %d workers (lines %d-%d)\n", tasks,
               workers, firstLine, lastLine);
}

void reportApprovedButSerial(int firstLine, int lastLine, const std::string& why) {
  std::fprintf(stderr, "[piet_native] auto-parallel: lines %d-%d are parallelizable but ran "
                       "serially — %s\n",
               firstLine, lastLine, why.c_str());
}

void reportRefused(int firstLine, int lastLine, const std::string& reason) {
  std::fprintf(stderr, "[piet_native] auto-parallel: refused lines %d-%d: %s\n", firstLine, lastLine,
               reason.c_str());
}

}  // namespace piet::parallel
