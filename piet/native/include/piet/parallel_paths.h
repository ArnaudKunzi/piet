// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// P9 automode · call-site path substitution — the last thing standing between
// the recognizer and a real corpus.
//
// THE PROBLEM. Rule 10 of the region predicate refuses any task that writes a
// file whose path it cannot resolve, because two tasks writing the same path is
// a silent corruption and "probably different" is not a proof. On the acceptance
// corpus every task ends in
//
//     graph export "`pm_output_dir'/plot_`pm_season_scope'_`pm_analysis_mode'_`pm_group_name'.png"
//
// three program levels below the statement being judged. Nothing about that is
// unknowable — it is just several hops away.
//
// WHAT IS ACTUALLY NEEDED, measured rather than assumed (analysis.do, 2026-08-14):
//   * `plot_multi_group` and `plot_multi_depvars` take their parameters
//     POSITIONALLY, and NOT through `args`: each opens with
//     `local rest `"`0'"'` followed by a run of `gettoken NAME rest : rest`,
//     which is positional extraction spelled long-hand. A design that only
//     understood `args` would have resolved nothing here.
//   * The outer program forwards those six values, in order, to the inner one.
//   * The path is built from positions 3, 5, 4 and 6.
//   * At the 47 call sites, positions 3/4/5 are the SAME caller locals every
//     time and position 6 is a literal tag that is distinct in all 47.
//
// THE RULE, and why it is still one-sided. A path becomes a TEMPLATE: an
// ordered list of literal chunks and symbols, where a symbol is either a
// parameter hole (bound to a position, so a call site fills it) or an opaque
// local (a name this layer cannot evaluate). Two tasks' templates are
// COMPARABLE only when their opaque-symbol sequences are identical — same name
// in the same place means the same run-time value, whatever it is. When they are
// comparable, they are disjoint iff their literal skeletons differ. Anything
// else — an unparsable path, a symbol that resolves to nothing, a differing
// opaque sequence — is UNRESOLVED and refuses, exactly as before.
#ifndef PIET_NATIVE_PARALLEL_PATHS_H
#define PIET_NATIVE_PARALLEL_PATHS_H

#include <map>
#include <string>
#include <vector>

#include "piet/session_state.h"

namespace piet::parallel {

struct PathPart {
  // paramPos > 0  -> a hole to be filled by the call site's argument at that
  //                  1-based position.
  // paramPos == 0 -> `text` is either a literal chunk (isLiteral) or the name of
  //                  an opaque local (!isLiteral).
  bool isLiteral = true;
  std::string text;
  int paramPos = 0;
  // A symbol that resolves to a `tempfile`. This is a DISCRIMINATOR, not an
  // unknown: `tempfile` mints a fresh path every time it executes, so two tasks
  // going through one provably do NOT share a file — and across worker
  // processes, doubly so. Modelling it as an ordinary opaque symbol made every
  // task render the same key and reported a collision between tasks that cannot
  // collide (measured: `opponent_map`, then `overall_dta`).
  bool isTemp = false;
};

struct PathTemplate {
  std::vector<PathPart> parts;
};

// Every file-destination template a program can write, following forwarded
// calls up to `maxDepth` levels. `ok` false means at least one write could not
// be modelled — the caller must refuse rather than proceed on the rest.
struct ProgramPaths {
  bool ok = false;
  std::vector<PathTemplate> templates;
  std::string why;  // set when ok is false
};
ProgramPaths programWritePaths(const std::string& program, SessionState& session, int maxDepth = 3);

// Split a Stata argument list the way the interpreter does for positional
// parsing: whitespace-separated, with "..." and `"..."' kept whole. The quotes
// are stripped from the returned tokens.
std::vector<std::string> splitStataArgs(const std::string& text);

// Fill a template's holes from one call site's actual arguments and render a
// comparison key. Opaque symbols survive as `\x01name\x01` so that two keys
// differing only inside an opaque region compare unequal, while two keys sharing
// the same opaque names line up. `ok` false = a hole had no argument.
struct RenderedPath {
  bool ok = false;
  std::string key;
};
RenderedPath renderPath(const PathTemplate& t, const std::vector<std::string>& args);

}  // namespace piet::parallel

#endif  // PIET_NATIVE_PARALLEL_PATHS_H
