// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// frames — Stata's `frame` family (P6 wave): additional named datasets living
// beside the current one, plus the `frlink`/`frget` link-and-copy pair.
//
// DESIGN — frames are a PIET session concept, and the engine never learns the
// word. Each frame is one hbin inside the session's PID-unique workdir
// (SessionState::frames maps name -> path), and every operation below is
// expressed with engine verbs that already exist:
//
//   frame put VARLIST [if], into(F)     -> cobra eval --keepif ... --drop ...
//   frame F: CMD                        -> swap the session's dataset to F's
//                                          hbin, dispatch(CMD) normally, swap
//                                          back (and keep whatever CMD wrote,
//                                          so `frame F: gen x = 1` persists)
//   frlink m:1 KEY, frame(F) gen(G)     -> cobra merge m:1 against a helper
//                                          built from F carrying a row index
//   frget NEW=VAR, from(G)              -> the same merge, keyed on the link
//
// So there is no new join code here and no new engine surface: the link
// variable IS an m:1 merge result, which means unmatched rows get missing for
// free and key-uniqueness is enforced by the merge the engine already ships.
//
// SCOPE (honest rejects, not silent narrowing): the current frame is always
// `default`. `frame create`, `frame change`/`cwf` and `pwf` are rejected by
// name — "which frame is current" is a product decision with UI consequences
// (which frame does the browse grid show?) that this wave does not settle, and
// a half-supported current-frame concept is a bug factory. Everything the
// answerbook exercises, plus the obvious closure pairs, IS supported.
#include "piet/handlers.h"

#include <algorithm>
#include <atomic>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "io/win_compat.h"  // native_exists / native_process_id
#include "piet/cobra_runner.h"
#include "piet/datastore.h"
#include "piet/dispatcher.h"
#include "piet/eval/errors.h"
#include "piet/session_state.h"
#include "piet/scratch_seq.h"

namespace piet {
namespace handlers {

using ojson = nlohmann::ordered_json;

namespace {

// Routed through eval::engineError so frames errors carry the same normalized
// code and coarse-rc treatment as every other handler's (an unknown code would
// otherwise skip the rc derivation entirely — the rc comes from the message
// shape, see errors.cpp::coarseRc).
ojson frameError(const std::string& message, const std::string& code = "engine_error") {
  return ojson(eval::engineError(message, code));
}

// Same scratch-naming discipline as every other handler file (BUG-062: the
// token carries the PID so two processes sharing a workdir cannot collide).
std::string nextPath(const std::string& workdir, const std::string& prefix) {
  static std::atomic<uint64_t> counter{0};
  static const long long pid = static_cast<long long>(cecon::native_process_id());
  std::string path = workdir + "/" + prefix + "_" + std::to_string(pid) + "_" +
                     std::to_string(counter.fetch_add(1, std::memory_order_relaxed)) + ".hbin";
  registerScratchMint(path);  // BUG-389
  return path;
}

// File-local, like every other handler file's copy — see handlers_data_verbs.cpp
// for why the raw-stderr path exists (merge/rename print status text to stderr
// and JSON only on error, so runCobraJson's contract does not fit).
CobraError errorFromRaw(const CobraRunResult& r) {
  std::string stderrText = r.stderr_text;
  while (!stderrText.empty() &&
         (stderrText.back() == '\n' || stderrText.back() == '\r' || stderrText.back() == ' ')) {
    stderrText.pop_back();
  }
  try {
    nlohmann::json payload = nlohmann::json::parse(stderrText);
    nlohmann::json err = payload.contains("error") ? payload.at("error") : payload;
    if (err.is_object() && err.contains("message")) {
      return CobraError(err.value("code", std::string("engine_error")),
                        err.at("message").get<std::string>(), err.value("detail", std::string()));
    }
  } catch (const nlohmann::json::exception&) {
    // not JSON — fall through to the plain-text form
  }
  return CobraError("engine_error", stderrText.empty() ? "cobra failed" : stderrText);
}

std::string cobraErrorStr(const CobraError& ce) { return cobraErrorMessage(ce); }

// S07-ROUNDTRIP escape hatch (the C1/C2/C4/K1 convention: one env var that
// restores the pre-optimization path verbatim, so the equivalence claim gets a
// real A/B arm inside ONE binary and the merge gets its seeded-RED). Per-call
// getenv on purpose — a selftest leg setenv/unsetenvs around single dispatches,
// which a static cache would ignore.
bool mergeFoldDisabled() {
  const char* e = std::getenv("PIET_NO_MERGE_FOLD");
  return e && *e;
}

// S07-ROUNDTRIP. frlink and frget both expressed their LEFT join as two engine
// round trips: `merge` (all three _merge codes, _merge column generated) then
// `eval --keepif "_merge!=2" --drop _merge` to throw away the half that should
// never have been built. The second trip re-reads and re-writes the WHOLE
// merged frame — measured at 58 ms (frlink) and 63 ms (frget) per call on the
// 48k-row P2PERF fixture, 723 ms of S07's 1.89 s engine wait across its six
// reps — to delete rows and one column the FIRST trip could simply not have
// emitted.
//
// `merge --keep master,matched --nogenerate` is that same left join in one
// trip. Equivalence is structural, not empirical hope (src/data/frame.cpp
// merge_frames): `keep_codes` filters the SAME `plan` vector in the SAME order
// that Frame::keep_if would afterwards, `nogenerate` is literally the branch
// that appends `_merge`, column emission order (keys, master non-keys, using
// non-keys) is unchanged, and keep_if prunes no factor levels — so the two
// paths differ in the number of trips and in nothing else. Held to that claim
// by a byte-comparison leg in selftest.cpp, both arms in one binary.
void appendLeftJoinFold(std::vector<std::string>& args) {
  args.push_back("--keep");
  args.push_back("master,matched");
  args.push_back("--nogenerate");
}

// Stata prints counts with comma grouping ("4,800"). The renderer the rest of
// PIET uses for this lives with the display layer; frames only ever needs the
// integer form, so this is the one small local helper rather than a dependency.
std::string commaGroup(int64_t v) {
  std::string s = std::to_string(v < 0 ? -v : v);
  std::string out;
  int c = 0;
  for (auto it = s.rbegin(); it != s.rend(); ++it) {
    if (c && c % 3 == 0) out.push_back(',');
    out.push_back(*it);
    ++c;
  }
  if (v < 0) out.push_back('-');
  std::reverse(out.begin(), out.end());
  return out;
}

// A workdir path for a frame's own hbin. Frames outlive individual commands, so
// they get their own prefix rather than sharing the per-command scratch names.
std::string framePathFor(SessionState& session, const std::string& name) {
  return nextPath(session.workdir(), "frame_" + name);
}

// `frame drop F` on an unknown F is r(111) in Stata ("frame F not found") —
// the same class an unknown VARIABLE gets, so it carries the same stable code
// the rest of PIET uses for that class (isid's own r(111) path, and every
// other "not found" reject) rather than inventing a second spelling.
// The rc for a missing frame comes from the MESSAGE ("frame NAME not found" ->
// r(111), errors.cpp::coarseRc), which is how every other not-found reject in
// this codebase lands its rc; the code field stays the generic one.
constexpr const char* kNotFoundCode = "engine_error";

}  // namespace

// ---------------------------------------------------------------------------
// frame put VARLIST [if COND], into(NAME)
//
// Copies columns (optionally row-filtered) out of the CURRENT dataset into a
// new frame. The current dataset is untouched — that is the whole point of the
// verb, and the reason this cannot just be `keep`.
ojson runFramePut(const std::vector<std::string>& varlist, const std::optional<std::string>& ifExpr,
                  const std::string& into, SessionState& session) {
  if (!session.currentHbin().has_value()) return eval::noDataLoaded();
  Datastore* ds = session.datastore();
  if (ds == nullptr) return eval::noDataLoaded();
  if (into.empty()) return frameError("frame put: into(FRAMENAME) is required");
  if (varlist.empty()) return frameError("frame put: variable list required");

  for (const auto& v : varlist) {
    if (!ds->has(v)) return frameError("variable " + v + " not found");
  }

  const std::string outPath = framePathFor(session, into);
  std::vector<std::string> args = {"eval", "--data", *session.currentHbin(), "--out", outPath};
  // Ops apply IN THE ORDER GIVEN, so the row filter must precede the column
  // drops: `frame put id region if t==1` filters on `t`, which is one of the
  // columns being dropped.
  if (ifExpr.has_value() && !ifExpr->empty()) {
    args.push_back("--keepif");
    args.push_back(*ifExpr);
  }
  std::set<std::string> keep(varlist.begin(), varlist.end());
  for (const auto& nm : ds->names()) {
    if (!keep.count(nm)) {
      args.push_back("--drop");
      args.push_back(nm);
    }
  }

  CobraRunResult cr = runCobra(args);
  if (!cr.ok) {
    CobraError ce = errorFromRaw(cr);
    return frameError(cobraErrorStr(ce), ce.code);
  }
  if (!cecon::native_exists(outPath)) {
    return frameError("frame put: failed writing frame '" + into + "'");
  }

  // Replacing an existing frame of the same name is Stata's behaviour for
  // `frame put ... into()` only when the frame does not already exist; Stata
  // errors instead. Keep that: a silent overwrite would lose data.
  if (session.frames.count(into)) {
    return frameError("frame " + into + " already exists");
  }
  session.frames[into] = outPath;
  return ojson::array();  // silent success, per the isid/preserve convention
}

// ---------------------------------------------------------------------------
// frame drop NAME
ojson runFrameDrop(const std::string& name, SessionState& session) {
  auto it = session.frames.find(name);
  if (it == session.frames.end()) {
    return frameError("frame " + name + " not found", kNotFoundCode);
  }
  // Forget the link variables that pointed into it — a link into a dropped
  // frame is dangling, and `frget` through it must not silently answer from a
  // stale path.
  for (auto lit = session.frameLinks.begin(); lit != session.frameLinks.end();) {
    if (lit->second == name) {
      lit = session.frameLinks.erase(lit);
    } else {
      ++lit;
    }
  }
  const std::string droppedPath = it->second;
  session.frames.erase(it);
  // BUG-389 (2026-09-02): `frame drop` used to forget the map entry and leave the
  // file — no ledger knew it any more, so no reap could ever reach it (the LEAK
  // AUDIT's ORPHAN class). Route it through the ONE reclaim primitive: every pin
  // (live head, preserve twins, use memos, other frames sharing the path, the
  // running command's ScratchPin, the last model's data paths) is honoured there,
  // and the budget decides eager-vs-deferred exactly as for a superseded head.
  if (!droppedPath.empty() && droppedPath != session.currentHbin().value_or(std::string())) {
    session.reclaimScratchFile(droppedPath);
  }
  return ojson::array();  // silent success
}

// ---------------------------------------------------------------------------
// frame NAME: CMD
//
// Runs CMD with the named frame as the current dataset, then restores. Any
// mutation CMD performed stays with the frame (Stata semantics: `frame F: gen
// x = 1` really does add x to F), which falls out of pointing the frame's entry
// at whatever dataset the session is holding when CMD returns.
ojson runFramePrefix(const std::string& name, const std::string& innerCmd, SessionState& session) {
  auto it = session.frames.find(name);
  if (it == session.frames.end()) {
    return frameError("frame " + name + " not found", kNotFoundCode);
  }
  const std::string framePathIn = it->second;
  // BUG-396 (2026-09-02): the default frame's head must SURVIVE the swap. Loading
  // the named frame's file supersedes the current head, and under the eager
  // scratch budget (default 0 since 8f7d8e1b) reclaimSupersededScratch unlinks a
  // superseded path the moment nothing pins it — the head we are about to come
  // back to was nothing's pin. The P6 answerbook convicted it: `frame p6lookup:
  // count` → RECLAIM/UNLINK frget_merged → `session.load(frget_merged)` re-adopted
  // a path that no longer existed → the next `preserve` reported "disk full or
  // I/O error". Two halves: (a) a dirty VIRTUAL head names no file, so the
  // round trip back could never have worked — materialize it first (a no-op on
  // the common real head); (b) pin the real path for the span of the prefix, the
  // same ScratchPin the resampling prefixes use (reclaim exclusion 6).
  session.ensureMaterializedHead("frame-prefix");
  const std::optional<std::string> savedHbin = session.currentHbin();
  SessionState::ScratchPin keepDefaultHead(session, {savedHbin.value_or(std::string())});

  session.load(framePathIn);
  ojson inner;
  try {
    inner = dispatch(innerCmd, session);
  } catch (...) {
    if (savedHbin.has_value()) session.load(*savedHbin);
    throw;
  }
  // Whatever the inner command left as the current dataset IS the frame now.
  // It must be a REAL FILE first: a mutation leaves the head as a dirty virtual
  // resident token whose path names a file that does not exist yet, and storing
  // that token would hand the next `frame F:` an unopenable path. datastore()
  // is the established materialization point (ensureMaterializedHead), the same
  // one preserve leans on before recording its own snapshot path.
  if (session.currentHbin().has_value()) {
    try {
      (void)session.datastore();
      session.frames[name] = *session.currentHbin();
    } catch (const std::exception& e) {
      if (savedHbin.has_value()) session.load(*savedHbin);
      return frameError(std::string("frame ") + name + ": could not save the frame's dataset: " +
                        e.what());
    }
  }
  if (savedHbin.has_value()) session.load(*savedHbin);

  ojson blocks = ojson::array();
  for (const auto& b : inner) {
    if (b.is_object() && b.value("type", "") == "echo") continue;  // the prefix owns the echo
    blocks.push_back(b);
  }
  return blocks;
}

// ---------------------------------------------------------------------------
// frlink m:1 KEY [KEY2 ...], frame(F) generate(G)
//
// Creates G in the current dataset holding, for each row, the 1-based
// observation number of the matching row in frame F (missing when unmatched).
// Implemented as an m:1 merge against a helper built from F that carries the
// row index and the keys and nothing else — so key uniqueness, missing handling
// and factor-key matching are the engine's existing merge semantics rather than
// a second join implementation with its own bugs.
ojson runFrlink(const std::string& linkType, const std::vector<std::string>& keylist,
                const std::string& frameName, const std::string& genName, SessionState& session) {
  if (!session.currentHbin().has_value()) return eval::noDataLoaded();
  Datastore* master = session.datastore();
  if (master == nullptr) return eval::noDataLoaded();

  if (linkType != "m:1" && linkType != "1:1") {
    return frameError("frlink: only `m:1` and `1:1` links are supported in this build (got `" +
                      linkType + "`)");
  }
  if (keylist.empty()) return frameError("frlink: key variable list required");
  if (frameName.empty()) return frameError("frlink: frame(FRAMENAME) is required");
  if (genName.empty()) return frameError("frlink: generate(NEWVAR) is required");

  auto it = session.frames.find(frameName);
  if (it == session.frames.end()) {
    return frameError("frame " + frameName + " not found", kNotFoundCode);
  }
  if (master->has(genName)) {
    return frameError("variable " + genName + " already defined");
  }
  for (const auto& k : keylist) {
    if (!master->has(k)) return frameError("variable " + k + " not found");
  }

  Datastore linked(it->second);
  for (const auto& k : keylist) {
    if (!linked.has(k)) {
      return frameError("frlink: key variable '" + k + "' not found in frame " + frameName);
    }
  }

  // An m:1 link requires the key to identify the LINKED frame's observations.
  // The merge underneath would catch a violation anyway, but it would say
  // "merge m:1: using dataset is not unique on the key(s)" — describing a
  // mechanism the user never invoked. Check it here so the message names the
  // command that was actually typed and the frame it was pointed at
  // ([[piet-diagnostic-honesty-classes]]).
  if (linkType == "m:1" || linkType == "1:1") {
    std::set<std::string> seen;
    const int64_t nLinked = linked.nRows();
    std::vector<const cecon::Column*> kc;
    kc.reserve(keylist.size());
    for (const auto& k : keylist) kc.push_back(&linked.col(k));
    for (int64_t r = 0; r < nLinked; ++r) {
      std::string key;
      for (const auto* c : kc) {
        if (c->dtype == cecon::DType::Factor) {
          key += std::to_string(c->codes()[r]);
        } else {
          const double v = c->f64()[r];
          key.append(reinterpret_cast<const char*>(&v), sizeof v);
        }
        key.push_back('\x1f');
      }
      if (!seen.insert(key).second) {
        std::string vars;
        for (size_t i = 0; i < keylist.size(); ++i) {
          if (i) vars += " ";
          vars += keylist[i];
        }
        // Wording deliberately carries "does not uniquely identify the
        // observations" — the exact discriminator errors.cpp::coarseRc uses to
        // land Stata's own r(459) for this class (isid shares it).
        return frameError("variable " + vars +
                          " does not uniquely identify the observations in frame " + frameName);
      }
    }
  }

  // Helper = keys + a 1..N row index named as the link variable. `--egen seq()`
  // is the engine's running-1..N generator (a bare `_n` needs a bysort context).
  const std::string helper = nextPath(session.workdir(), "frlink_helper");
  std::vector<std::string> hargs = {"eval", "--data", it->second, "--egen", genName + " = seq()",
                                    "--out", helper};
  std::set<std::string> keySet(keylist.begin(), keylist.end());
  for (const auto& nm : linked.names()) {
    if (!keySet.count(nm)) {
      hargs.push_back("--drop");
      hargs.push_back(nm);
    }
  }
  CobraRunResult hr = runCobra(hargs);
  if (!hr.ok) {
    CobraError ce = errorFromRaw(hr);
    return frameError(cobraErrorStr(ce), ce.code);
  }

  std::string joinedKeys;
  for (size_t i = 0; i < keylist.size(); ++i) {
    if (i) joinedKeys += ",";
    joinedKeys += keylist[i];
  }
  // A link is a LEFT join: every master row survives, an unmatched one gets a
  // missing link, and a frame row with no master row must NOT be added. A bare
  // `--keep master` is the WRONG half (Stata's _merge==1 only) and the default
  // keeps using-only rows, so the left join is spelled `--keep master,matched`
  // — with `--nogenerate` because _merge existed only to be filtered on and
  // dropped again. See appendLeftJoinFold: that pair replaces a second
  // whole-frame round trip with nothing at all.
  const bool fold = !mergeFoldDisabled();
  const std::string mergedPath = nextPath(session.workdir(), "frlink_merged");
  const std::string outPath = fold ? mergedPath : nextPath(session.workdir(), "frlink");
  std::vector<std::string> margs = {"merge",  "--data",  *session.currentHbin(),
                                    "--using", helper,   "--type", linkType,
                                    "--key",   joinedKeys, "--out", mergedPath};
  if (fold) appendLeftJoinFold(margs);
  CobraRunResult cr = runCobra(margs);
  if (!cr.ok) {
    CobraError ce = errorFromRaw(cr);
    return frameError(cobraErrorStr(ce), ce.code);
  }
  if (!fold) {
    CobraRunResult lr = runCobra({"eval", "--data", mergedPath, "--keepif", "_merge!=2", "--drop",
                                  "_merge", "--out", outPath});
    if (!lr.ok) {
      CobraError ce = errorFromRaw(lr);
      return frameError(cobraErrorStr(ce), ce.code);
    }
  }
  if (!cecon::native_exists(outPath)) {
    return frameError("frlink: failed writing linked dataset");
  }

  session.load(outPath);
  session.frameLinks[genName] = frameName;

  // Stata reports how the link came out. All-matched has its own wording; a
  // partial match names the count that did NOT match, because that is the
  // number the user has to act on.
  Datastore* after = session.datastore();
  int64_t unmatched = 0;
  int64_t total = 0;
  if (after != nullptr && after->has(genName)) {
    total = after->nRows();
    unmatched = after->nMissing(genName);
  }
  const std::string msg =
      unmatched == 0
          ? "(all observations in frame default matched)"
          : "(" + commaGroup(unmatched) + " of " + commaGroup(total) +
                " observations in frame default not matched)";
  return ojson{{"type", "display"}, {"text", msg}};
}

// ---------------------------------------------------------------------------
// frget NEWNAME=VAR [NEWNAME2=VAR2 ...], from(LINKVAR)
//
// Copies variables out of the linked frame into the current dataset, matched
// through the link variable frlink created. Same mechanism: an m:1 merge keyed
// on the link, against a helper carrying the row index and the wanted columns.
ojson runFrget(const std::vector<std::pair<std::string, std::string>>& renames,
               const std::string& fromLink, SessionState& session) {
  if (!session.currentHbin().has_value()) return eval::noDataLoaded();
  Datastore* master = session.datastore();
  if (master == nullptr) return eval::noDataLoaded();
  if (renames.empty()) return frameError("frget: variable list required");
  if (fromLink.empty()) return frameError("frget: from(LINKVAR) is required");

  auto lit = session.frameLinks.find(fromLink);
  if (lit == session.frameLinks.end()) {
    return frameError("frget: " + fromLink +
                      " is not a linkage variable — create one with `frlink` first");
  }
  auto fit = session.frames.find(lit->second);
  if (fit == session.frames.end()) {
    return frameError("frame " + lit->second + " not found", kNotFoundCode);
  }
  if (!master->has(fromLink)) {
    return frameError("variable " + fromLink + " not found");
  }

  Datastore linked(fit->second);
  for (const auto& [newName, srcName] : renames) {
    if (!linked.has(srcName)) {
      return frameError("frget: variable '" + srcName + "' not found in frame " + lit->second);
    }
    if (master->has(newName)) {
      return frameError("variable " + newName + " already defined");
    }
  }

  // Helper carries the SAME 1..N index the link holds, plus the wanted columns
  // under their new names.
  const std::string helper = nextPath(session.workdir(), "frget_helper");
  std::vector<std::string> hargs = {"eval", "--data", fit->second, "--egen", fromLink + " = seq()",
                                    "--out", helper};
  std::set<std::string> wanted;
  for (const auto& [newName, srcName] : renames) wanted.insert(srcName);
  for (const auto& nm : linked.names()) {
    if (!wanted.count(nm)) {
      hargs.push_back("--drop");
      hargs.push_back(nm);
    }
  }
  CobraRunResult hr = runCobra(hargs);
  if (!hr.ok) {
    CobraError ce = errorFromRaw(hr);
    return frameError(cobraErrorStr(ce), ce.code);
  }

  // Rename each source column to its target name before the join, so the merge
  // never has to reconcile two columns of the same name.
  std::string current = helper;
  for (const auto& [newName, srcName] : renames) {
    if (newName == srcName) continue;
    const std::string renamed = nextPath(session.workdir(), "frget_rename");
    CobraRunResult rr = runCobra(
        {"rename", "--data", current, "--pairs", srcName + "=" + newName, "--out", renamed});
    if (!rr.ok) {
      CobraError ce = errorFromRaw(rr);
      return frameError(cobraErrorStr(ce), ce.code);
    }
    current = renamed;
  }

  // Same left-join shape as frlink (see there, and appendLeftJoinFold): the
  // rows that exist only on the using side are never built rather than built
  // and then filtered away. A master row whose link is missing simply gets
  // missing values for the copied variables, which is Stata's behaviour.
  const bool fold = !mergeFoldDisabled();
  const std::string mergedPath = nextPath(session.workdir(), "frget_merged");
  const std::string outPath = fold ? mergedPath : nextPath(session.workdir(), "frget");
  std::vector<std::string> margs = {"merge",   "--data", *session.currentHbin(),
                                    "--using", current,  "--type", "m:1",
                                    "--key",   fromLink, "--out",  mergedPath};
  if (fold) appendLeftJoinFold(margs);
  CobraRunResult cr = runCobra(margs);
  if (!cr.ok) {
    CobraError ce = errorFromRaw(cr);
    return frameError(cobraErrorStr(ce), ce.code);
  }
  if (!fold) {
    CobraRunResult lr = runCobra({"eval", "--data", mergedPath, "--keepif", "_merge!=2", "--drop",
                                  "_merge", "--out", outPath});
    if (!lr.ok) {
      CobraError ce = errorFromRaw(lr);
      return frameError(cobraErrorStr(ce), ce.code);
    }
  }
  if (!cecon::native_exists(outPath)) return frameError("frget: failed writing output dataset");

  session.load(outPath);
  const std::string noun = renames.size() == 1 ? " variable copied" : " variables copied";
  return ojson{{"type", "display"},
               {"text", "(" + std::to_string(renames.size()) + noun + " from linked frame)"}};
}

}  // namespace handlers
}  // namespace piet
