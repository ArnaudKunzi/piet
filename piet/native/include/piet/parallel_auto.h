// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// P9 automode · L1 (candidate discovery) + the region predicate + L4 (honest
// reporting). Default OFF — this whole layer is inert unless PIET_PARALLEL_AUTO
// is set, which is the User's ruling for the first release.
//
// L1 finds maximal runs of consecutive TOP-LEVEL flat statements; the predicate
// decides whether such a run may execute as independent tasks; L4 says what
// happened, on stderr, in one line either way. Silence is forbidden in both
// directions: a user who does not know a region was refused cannot act on it,
// and a user who does not know one was taken cannot explain their own output.
//
// The predicate is ONE-SIDED by construction — every clause is written so that
// missing information means REFUSE. That is what makes a false positive
// structurally impossible rather than merely unlikely.
#ifndef PIET_NATIVE_PARALLEL_AUTO_H
#define PIET_NATIVE_PARALLEL_AUTO_H

#include <cstddef>
#include <string>
#include <vector>

#include "piet/session_state.h"

namespace piet::parallel {

// Is the automatic recognizer switched on for this process?
//   PIET_PARALLEL_AUTO=1          — the master switch (absent => OFF).
//   PIET_PARALLEL_AUTO_WORKERS=N  — optional; otherwise the resolved default.
// Always OFF inside a worker (PIET_PARALLEL_WORKER), so one generation only.
bool autoModeEnabled();
int autoModeWorkers();
// The raw PIET_PARALLEL_AUTO_WORKERS request (0 = derive), for callers that
// resolve with a ResolveContext of their own (doexec's region arm).
int autoModeRequestedWorkers();

// Test-only: force a region the predicate REFUSED to be approved anyway, so the
// byte gate can be shown to go RED on real unsafe work (probe cell G-8). Inert
// unless the master switch is also on, and it announces itself loudly on stderr
// every time it fires — an override that can be set quietly is a foot-gun, not
// a gate.
bool autoModeUnsafeForce();

// The smallest run worth handing to a worker pool. Every region pays N process
// spawns plus N engine spawns, so a 2-task region can use at most 2 workers and
// saves less than one task's wall in the best case; 3 is where the default pool
// is first fully usable. A false negative is only slowness (the safety axiom),
// so this threshold is a tuning constant and not a correctness one.
inline constexpr size_t kAutoMinRegion = 3;

struct RegionPlan {
  bool approved = false;
  size_t count = 0;          // statements in the APPROVED region (may be < stmts.size()
                             // after an end-shrink, see below)
  std::string refusalReason;  // one line, user-facing; empty when approved
  bool reportable = false;    // a candidate run long enough to deserve a refusal line
  // Index (within the candidate run) of the statement that killed it, so the
  // caller can resume scanning AFTER it instead of writing off the whole tail.
  // One bad statement in position 1 must not make the other thirty unreachable —
  // that is how a recognizer ends up finding nothing on a real file. Equals
  // stmts.size() when the refusal is a property of the whole run.
  size_t failedAt = 0;
};

// Decide whether `stmts` (already the consecutive flat run starting at a
// top-level item) may run as independent parallel tasks. `following` is the raw
// text of the top-level items AFTER the run, used by the successor walks that
// decide whether anything the region leaves behind is ever read.
RegionPlan planRegion(const std::vector<std::string>& stmts,
                      const std::vector<std::string>& following, SessionState& session);

// L4. Both go to stderr, never stdout — stdout is byte-compared.
void reportApproved(size_t tasks, int workers, int firstLine, int lastLine);
void reportRefused(int firstLine, int lastLine, const std::string& reason);

// A region the predicate APPROVED but that runs serially anyway — because the
// derived budget is one worker, or fan-out could not start. The User's ruling on
// refusal reporting ("banner + one line, never silent") applies here too, and
// arguably more: a user who set the recognizer loose and sees nothing has no way
// to tell "found nothing" from "found it and ran it serially".
void reportApprovedButSerial(int firstLine, int lastLine, const std::string& why);

}  // namespace piet::parallel

#endif  // PIET_NATIVE_PARALLEL_AUTO_H
