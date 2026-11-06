// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// P9 automode · L2 — the effect table and the approval predicate's evidence base.
//
// THE SAFETY AXIOM (design brief §10.2, User-ratified): the recognizer says
// "parallelizable" ONLY when a whitelist holds for every statement in the
// region. One unknown command, one unresolvable path, one summary it cannot
// compute — and the region runs serially. A false negative costs speed; a false
// positive is forbidden, because it silently produces wrong output.
//
// That axiom is why this is a SPARSE table and not a field on CommandSpec.
// registry.h's append-only default-valued fields (byIllegalInStata,
// communityNames) are exactly the right pattern for a property whose default is
// harmless. Here the default must be "I do not know", and a default-valued
// column on all 162 registry rows would read as "known, effect-free" for every
// row nobody has classified yet — the forbidden direction. Absence from THIS
// table is UNKNOWN, and unknown refuses.
#ifndef PIET_NATIVE_PARALLEL_EFFECTS_H
#define PIET_NATIVE_PARALLEL_EFFECTS_H

#include <algorithm>
#include <string>
#include <vector>

#include "piet/session_state.h"

namespace piet::parallel {

// What one statement (or one whole program) does, as far as the approval
// predicate is concerned. Every field is "may it" rather than "does it" — the
// summary of a program is the union over its body, so a `true` means some path
// through it can do this.
struct Effects {
  // Macro state comes in two kinds and conflating them refuses everything.
  // LOCALS are scoped to the invocation that wrote them (program.h's
  // pushScope/popScope), so a program writing its own locals leaves nothing
  // behind — summarizeProgram drops this flag on the way out. A local written by
  // a TOP-LEVEL statement is a different matter: the next statement can read it,
  // and a worker never returns it, so the region refuses.
  bool writesLocals = false;
  // Globals, scalars and matrices are session-wide. These survive the writer and
  // a worker still never returns them, so they refuse at any depth.
  bool writesGlobalState = false;
  bool writesResults = false;     // r() / e()
  bool readsResults = false;      // reads r()/e() it did not itself set
  bool readsDataset = false;
  bool writesDataset = false;  // mutates the resident frame (sort counts)
  bool datasetSelfSeed = false;  // begins by loading its own data (`use ..., clear`)
  bool control = false;  // exit/log/set/cd/do/program/preserve/... — region-fatal
  bool rng = false;      // consumes or seeds the RNG stream
  // Touches the session's ESTIMATES store (`estimates store/restore`, `eststo`).
  // Like r()/e() and the frame, a worker's store dies with it — so this is not
  // fatal in itself, it is a thing the successor walk has to prove nobody reads.
  bool usesEstimates = false;
  // Names of session-global scalars/matrices/globals this statement (or program)
  // writes. Kept by NAME because "does anyone read it afterwards" is answerable
  // and "does it write session state" is not — the second question refuses every
  // real program, the first refuses only the ones that would actually diverge.
  std::vector<std::string> globalWrites;
  // A write whose TARGET name is itself built from a macro. Nothing can be
  // proven about a name that does not exist yet, so this refuses.
  bool globalWriteUnresolved = false;
  bool writesFiles = false;         // writes at least one file
  bool filePathsUnresolved = false;  // ...and at least one path is not knowable here

  void merge(const Effects& o) {
    writesLocals |= o.writesLocals;
    writesGlobalState |= o.writesGlobalState;
    writesResults |= o.writesResults;
    readsResults |= o.readsResults;
    readsDataset |= o.readsDataset;
    writesDataset |= o.writesDataset;
    control |= o.control;
    rng |= o.rng;
    usesEstimates |= o.usesEstimates;
    globalWriteUnresolved |= o.globalWriteUnresolved;
    for (const auto& n : o.globalWrites)
      if (std::find(globalWrites.begin(), globalWrites.end(), n) == globalWrites.end())
        globalWrites.push_back(n);
    writesFiles |= o.writesFiles;
    filePathsUnresolved |= o.filePathsUnresolved;
    // datasetSelfSeed is deliberately NOT unioned: it is a property of how a
    // body BEGINS, not something any later line can confer. summarizeProgram
    // sets it from the first data-touching line only.
  }
};

// Look up one command by its canonical name (post registry resolution).
// Returns nullptr for anything not classified — which the caller must treat as
// a refusal, never as "no effects".
const Effects* lookupCommand(const std::string& canonical);

// How many commands are classified. Coverage is monotone with performance and
// orthogonal to safety (brief §10.4), so this number is a progress metric, not
// a correctness one.
size_t classifiedCommandCount();

// Summarize ONE already-macro-expanded statement. `known` false means the
// statement could not be classified and its region must be refused; `why` then
// carries a one-line reason fit for the user-facing refusal banner.
struct StatementSummary {
  bool known = false;
  Effects eff;
  std::string why;
};
StatementSummary summarizeStatement(const std::string& stmt, SessionState& session);

// Summarize a user program by walking its body, recursing into the programs it
// calls. Cached per session and invalidated by session.programsEpoch, which
// `program define` / `program drop` bump.
StatementSummary summarizeProgram(const std::string& name, SessionState& session);

// Walk `program`'s body IN ORDER and report the first place it could observe a
// session-global name from `names` that it has not itself written yet. That is
// the only way a task could see another task's leftovers, so proving its absence
// is what lets a region write session-globals at all. Returns true when the body
// is clean; otherwise `why` names the line.
bool programGlobalsAreSelfWritten(const std::string& program, const std::vector<std::string>& names,
                                  SessionState& session, std::string* why);

// True when `text` mentions `name` as a whole word (the conservative
// "could this read it" test — a coincidental mention refuses).
bool mentionsWord(const std::string& text, const std::string& name);

}  // namespace piet::parallel

#endif  // PIET_NATIVE_PARALLEL_EFFECTS_H
