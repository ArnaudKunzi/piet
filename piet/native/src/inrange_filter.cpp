// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
#include "piet/inrange_filter.h"
#include "piet/translate.h"

#include <cctype>
#include <cstdio>
#include <ctime>
#include <string>
#include <vector>

#include "piet/cobra_runner.h"
#include "piet/datastore.h"
#include "piet/eval/errors.h"
#include "piet/scratch_seq.h"
#include "piet/session_state.h"
#include "piet/strmutate.h"
#include "piet/strutil.h"

namespace piet::handlers {

namespace {

// BS-092 crash root cause (BETA campaign Phase G): this name is built from
// whole-SECOND wall-clock time, and scratchSeqSuffix() is a deliberate no-op
// on POSIX (scratch_seq.h — it only breaks ties on Windows' can't-overwrite-
// a-mapped-file constraint). So on macOS/Linux, two calls sharing the same
// (workdir, prefix) within the SAME wall-clock second collide on the
// IDENTICAL path — previously harmless because every OTHER caller either (a)
// used a prefix unique to itself, or (b) immediately consumed its own file
// synchronously before anything else could reuse the name. BS-092's
// bysort/by-group `summarize` dispatch (dispatcher.cpp) broke assumption
// (b): it calls applyIfAndInFilter's haveIn branch (this file's "inidx"/
// "inif_filtered"/"inif_final" trio) once PER GROUP in a tight loop, purely
// to materialize a READ-ONLY slice — it never calls session.load() on the
// result. If an EARLIER mutation in the same wall-clock second (e.g.
// `keep in a/b` / `drop in a/b` — handlers_m3.cpp's runKeepIf/runDropIf,
// which route through this SAME haveIn branch and DO call session.load() on
// its "inif_final" output) left session.currentHbin() pointing at that exact
// generated filename, the by-group loop's later same-second call here
// silently OVERWRITES the live session's own backing file on disk with
// whatever transient per-group slice it happens to be computing —
// session.currentHbin() still points at the path, but the bytes under it
// now belong to a stranger (observed: `keep in 1/14` immediately followed by
// `bysort state: summarize beertax` clobbers the live 14-row session file
// with the LAST group's own 7-row slice; a later `summarize ... if` reads
// that clobbered file fresh off disk via a new cobra subprocess — no
// in-process cache shields it — and silently undercounts, then crashes on a
// resulting null stat field). Fix: a per-process monotonic counter makes
// every call's path genuinely unique regardless of wall-clock granularity,
// on every platform (dispatch is single-threaded — same precedent
// scratch_seq.h's Windows-only suffix already relies on, so a plain counter
// needs no atomics). No caller, including BS-090's, inspects or depends on
// the literal filename — this only removes a collision hazard, it never
// changes which rows any command selects.
std::string nextInRangePath(const std::string& workdir, const std::string& prefix) {
  auto ms = static_cast<long long>(std::time(nullptr)) * 1000;
  static unsigned long long seq = 0;
  char buf[80];
  std::snprintf(buf, sizeof(buf), "%lld_%llu", ms, seq++);
  std::string path = workdir + "/" + prefix + "_" + buf + scratchSeqSuffix() + ".hbin";
  registerScratchMint(path);  // BUG-389: the `if`/`in` projections were the largest orphan class
  return path;
}

// The materialized row-index column's name. File-scope because both the
// one-shot path and the hoisted RowIndexCache builder below must spell it
// identically — the cache's producer writes it and the per-slice consumer
// drops it again.
const char* const kIdxVar = "__piet_inidx__";
// addIfMaskColumn's 0/1 column, beside kIdxVar on the same cached base.
const char* const kIfMaskVar = "__piet_inif__";
// addIfMaskColumn's group-relative `_n`/`_N` for the row evaluator; they live
// only on its scratch frame.
const char* const kGroupObsVar = "__piet_byobs__";
const char* const kGroupNObsVar = "__piet_bynobs__";

// `_n` -> kGroupObsVar and `_N` -> kGroupNObsVar wherever they stand as names:
// not inside a string literal, not part of a longer name.
std::string renameObsNumbers(const std::string& expr) {
  auto isName = [](char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '_'; };
  std::string out;
  bool inStr = false;
  for (size_t i = 0; i < expr.size(); ++i) {
    const char c = expr[i];
    if (c == '"') inStr = !inStr;
    if (!inStr && c == '_' && i + 1 < expr.size() && (expr[i + 1] == 'n' || expr[i + 1] == 'N') &&
        (i == 0 || !isName(expr[i - 1])) && (i + 2 == expr.size() || !isName(expr[i + 2]))) {
      out += expr[i + 1] == 'n' ? kGroupObsVar : kGroupNObsVar;
      ++i;
      continue;
    }
    out += c;
  }
  return out;
}

bool namesObsNumber(const std::string& expr) { return renameObsNumbers(expr) != expr; }

// `, by(k1 k2)` for an engine --bygen spec; empty without keys.
std::string byOption(const std::vector<std::string>& groupKeys) {
  if (groupKeys.empty()) return std::string();
  std::string byOpt = ", by(";
  for (size_t i = 0; i < groupKeys.size(); ++i) byOpt += (i ? " " : "") + groupKeys[i];
  return byOpt + ")";
}

// Stata's two range refusals, verbatim (w2d_uncovered.log:5240 for the first;
// the second is StataBE 19.5's answer to `keep in 0` / `keep in -13/-1` on 12
// rows, not yet in a golden). Both are r(198).
const char* const kOutOfRange = "observation numbers out of range";
std::string invalidObsMessage(const std::string& token) {
  return "'" + token + "' invalid observation number";
}

// One bound of [U] 11.1.4's grammar: f/F, l/L, or an optionally signed run of
// decimal digits. Grammar only; resolution against a row count is below.
struct BoundToken {
  bool ok = false;
  enum Kind { kFirst, kLast, kFromStart, kFromEnd } kind = kFromStart;
  long long k = 0;   // magnitude for kFromStart / kFromEnd
  std::string text;  // trimmed source token (for Stata's quoted message)
};

BoundToken lexBound(const std::string& raw) {
  BoundToken t;
  t.text = strutil::trim(raw);
  const std::string tl = strutil::lower(t.text);
  if (tl == "f") {
    t.ok = true;
    t.kind = BoundToken::kFirst;
    return t;
  }
  if (tl == "l") {
    t.ok = true;
    t.kind = BoundToken::kLast;
    return t;
  }
  const bool neg = !t.text.empty() && t.text[0] == '-';
  const std::string digits = neg ? t.text.substr(1) : t.text;
  if (digits.empty()) return t;
  for (char c : digits) {
    if (!std::isdigit(static_cast<unsigned char>(c))) return t;
  }
  try {
    t.k = std::stoll(digits);
  } catch (...) {
    return t;  // beyond long long: no dataset has that many rows; malformed
  }
  t.ok = true;
  t.kind = neg ? BoundToken::kFromEnd : BoundToken::kFromStart;
  return t;
}

// `#1[/#2]` split: a bare `#` is `#/#`. A second '/' leaves a non-digit in
// p2, so lexBound refuses it as malformed.
void splitRange(const std::string& raw, std::string& p1, std::string& p2) {
  const std::string t = strutil::trim(raw);
  const size_t slash = t.find('/');
  p1 = slash == std::string::npos ? t : t.substr(0, slash);
  p2 = slash == std::string::npos ? t : t.substr(slash + 1);
}

}  // namespace

InRangeBounds parseInRangeBounds(const std::string& raw, int64_t nRowsTotal) {
  InRangeBounds res;
  std::string p1, p2;
  splitRange(raw, p1, p2);
  const BoundToken ta = lexBound(p1);
  const BoundToken tb = lexBound(p2);
  if (!ta.ok || !tb.ok) return res;  // malformed -> "unsupported form"
  const long long n = static_cast<long long>(nRowsTotal);
  // A numeric bound names an observation: 0 (or -0) is none, and -k beyond
  // the first row is none either. Stata reports the first such token.
  auto resolve = [n](const BoundToken& t, long long& out) {
    switch (t.kind) {
      case BoundToken::kFirst: out = 1; return true;
      case BoundToken::kLast: out = n; return true;
      case BoundToken::kFromStart: out = t.k; return t.k >= 1;
      case BoundToken::kFromEnd: out = n - t.k + 1; return t.k >= 1 && out >= 1;
    }
    return false;
  };
  if (!resolve(ta, res.a)) {
    res.refusal = invalidObsMessage(ta.text);
    return res;
  }
  if (!resolve(tb, res.b)) {
    res.refusal = invalidObsMessage(tb.text);
    return res;
  }
  // [U] 11.1.4: the implied first observation must not follow the implied
  // last, and both must exist. Clamping instead answered a different row set
  // than asked (`keep in 1/50` on 12 rows kept all 12; the oracle is r(198)
  // with the data untouched, w2d_uncovered.log:5245).
  if (res.a < 1 || res.b > n || res.a > res.b) {
    res.refusal = kOutOfRange;
    return res;
  }
  res.ok = true;
  return res;
}

namespace {

// applyIfAndInFilter's body. `keepIndex` (e(sample) T2, applyIfAndInFilterKeepingIndex
// below) leaves kIdxVar -- each surviving row's 1-based position in basePath --
// on the filtered copy instead of dropping it; nullptr/false is the historical
// call, byte for byte.
InRangeFilterResult filterIfAndIn(SessionState& session, const std::string& basePath,
                                  const std::optional<std::string>& ifExpr,
                                  const std::optional<std::string>& inRange, bool complement,
                                  const RowIndexCache* idx, bool keepIndex) {
  InRangeFilterResult res;
  bool haveIf = ifExpr.has_value() && !ifExpr->empty();
  bool haveIn = inRange.has_value() && !inRange->empty();
  if (!haveIf && !haveIn) {
    res.dataPath = basePath;
    return res;
  }
  if (!haveIn) {
    // if-only: a single --keepif, no idx materialization needed at all —
    // IDENTICAL shape (and cost) to the pre-BS-090 per-file prefilter
    // blocks this function replaces.
    //
    // fix/verb-overhead-sweep S2: this is the ONE branch that may run on the
    // §26 resident plane, and it does so only when the CALLER has opted in
    // (SessionState::ResidentEligible). That opt-in is deliberately not taken
    // here: this helper cannot see what its caller will do with `outPath`, and
    // the two answers are incompatible. `keep if`/`drop if` (handlers_m3.cpp
    // runKeepIf/runDropIf) session.load() it as the new head and never read it
    // as a file, so a virtual head is exactly right and saves both the engine's
    // full re-read + --out write AND PIET's card rebuild. But most callers of
    // this function — summarize/regress/collapse/tabulate/duplicates/ttest/
    // count and the by-group dispatch — instead read `outPath` back AS A FILE
    // (handlers.cpp's `Datastore(sumifPath)` is the plainest case) or hand it
    // to a stateless --data, and a virtual head would leave them opening a file
    // that was never written. Whether the result becomes the head is knowable
    // only at the call site, so the call site is where it is stated.
    std::string outPath = nextInRangePath(session.workdir(), "ifin_if");
    // BUG-533: the complement is `(EXPR)==0`, not `!(EXPR)`. Missing is TRUE in
    // Stata's `if`, so `drop if x` drops a missing-x row (w2_bykeep.log
    // ctl_drop_misstrue: `drop if z[_n-1]` drops row 1). `(EXPR)==0` is false
    // on a missing EXPR (missing compares above every number), so those rows
    // go. Until BUG-620 the engine's `!` passed a missing through as missing
    // and keep_if keeps a missing predicate, so `!(EXPR)` KEPT exactly the rows
    // Stata drops; the engine's `!` now reads a missing operand as true, as
    // Stata does, and both spellings agree. EXPR itself is the user's text,
    // read with the engine's own `!`, `&` and `|`: the same text the
    // estimation commands send as --keepif, so a refit that replays it
    // through here selects the fit's sample.
    std::string pred = complement ? ("(" + *ifExpr + ")==0") : *ifExpr;
    // BUG-369: `keep if L.x > 3` — see handlers.cpp::runGen for why the tsset
    // metadata has to ride the call rather than come from the file.
    std::vector<std::string> argv = {"eval", "--data", basePath};
    if (keepIndex) {
      // cmd_eval applies its ops in argv order (C1-② below), so the index is
      // numbered BEFORE --keepif removes rows: the position in basePath.
      argv.push_back("--bygen");
      argv.push_back(std::string(kIdxVar) + "=_n");
    }
    argv.push_back("--keepif");
    argv.push_back(pred);
    argv.push_back("--out");
    argv.push_back(outPath);
    runCobraJson(translate::injectTsContextForExpr(argv, pred, session));
    res.dataPath = outPath;
    return res;
  }

  // fix/verb-overhead-sweep S2: the `in`-range chain stays STATELESS whatever
  // the caller opted into. Eligibility is a session counter rather than a
  // call-stack property, so `keep if X in 1/100` would otherwise drag this
  // whole three-call chain onto the resident plane on the strength of an opt-in
  // that was only ever a statement about the FINAL --out. The chain does not
  // satisfy it: `baseDs` below opens basePath as a real file, and the
  // idx/filtered/final temps are each re-opened by the next call in the chain.
  // Suspending locally keeps that a property of this code rather than of who
  // happens to call it.
  SessionState::ResidentSuspend statelessChain(session);

  // Stage 1b: resolve the total row count `f`/`l` bounds are measured against.
  // With a hoisted RowIndexCache the caller already opened the base once and
  // paid for that count; without one, baseDs opens basePath (== the current
  // head) AS A FILE, so a virtual resident head must be snapshotted first.
  const bool usePreIndexed = idx != nullptr && !idx->path.empty();
  const std::string& indexedPath = usePreIndexed ? idx->path : basePath;
  int64_t nRowsTotal = 0;
  if (usePreIndexed) {
    nRowsTotal = idx->nRows;
  } else {
    // ONESHOT-LASTFLUSH: only the CURRENT head is ever virtual. When `basePath` is
    // some other real file — a §38 projection, a by-group slice — this used to
    // write the entire resident frame to disk to answer a row count that the file
    // in hand already carries. Nothing downstream of here reads the head.
    if (session.currentHbin().has_value() && basePath == *session.currentHbin())
      session.ensureMaterializedHead();
    Datastore baseDs(basePath);
    nRowsTotal = baseDs.nRows();
  }
  InRangeBounds rr = parseInRangeBounds(*inRange, nRowsTotal);
  if (!rr.ok) {
    res.ok = false;
    res.refusal = rr.refusal;
    return res;
  }

  std::string rangePred = std::string(kIdxVar) + ">=" + std::to_string(rr.a) + " & " +
                          std::string(kIdxVar) + "<=" + std::to_string(rr.b);
  // BUG-534: the `if` half is normalised to 0/1 with `!=0` before the AND.
  // Until BUG-620 the engine's `&` read a missing operand as FALSE, so
  // `keep if x in 1/5` lost (and `drop if x in 1/5` kept) the in-range rows
  // whose x is missing; Stata treats a missing condition as true
  // (w2_smalls.log keep_truthy). The engine's `&` now reads it as true too,
  // and the normalisation keeps both operands 0/1 either way, so the engine's
  // `!(...)` complement below is exact. runCount's native fast path applies
  // the same rule to an if+in count.
  std::string combinedIf =
      haveIf ? ("((" + *ifExpr + ")!=0) & (" + rangePred + ")") : rangePred;
  if (complement) combinedIf = "!(" + combinedIf + ")";

  // P2-PERF ARC-C C1-②: ONE round trip, not three. `cmd_eval` applies its ops
  // in ARGV ORDER within a single load of the frame (apps/cmds/cmds_data.cpp's
  // single `for (i)` pass over the flag vector — `--bygen` at :163, `--keepif`
  // at :212, `--drop` at :210 are all branches of that one loop), so
  // materializing the row index, filtering on it and dropping it again compose
  // in one call exactly as they did across three. What the old chain bought
  // with two extra full writes and two extra full reads was nothing but the
  // intermediate files themselves; no caller ever saw them.
  std::string finalPath = nextInRangePath(session.workdir(), "inif_final");
  if (usePreIndexed) {
    // C1-①: the index column is GROUP-INVARIANT, so a per-group loop builds it
    // once (buildRowIndexCache) and every slice below is a single --keepif over
    // the already-indexed copy. A pre-evaluated `if` column leaves the same way.
    std::vector<std::string> argv = {"eval", "--data", indexedPath, "--keepif", combinedIf,
                                     "--drop", kIdxVar};
    if (!idx->maskVar.empty()) {
      argv.push_back("--drop");
      argv.push_back(idx->maskVar);
    }
    argv.push_back("--out");
    argv.push_back(finalPath);
    runCobraJson(translate::injectTsContextForExpr(argv, combinedIf, session));
  } else if (keepIndex) {
    // e(sample) T2: the same single call, minus the --drop.
    runCobraJson(translate::injectTsContextForExpr(
        {"eval", "--data", basePath, "--bygen", std::string(kIdxVar) + "=_n", "--keepif",
         combinedIf, "--out", finalPath},
        combinedIf, session));
  } else {
    runCobraJson(translate::injectTsContextForExpr(
        {"eval", "--data", basePath, "--bygen", std::string(kIdxVar) + "=_n", "--keepif",
         combinedIf, "--drop", kIdxVar, "--out", finalPath},
        combinedIf, session));
  }

  res.dataPath = finalPath;
  return res;
}

}  // namespace

InRangeFilterResult applyIfAndInFilter(SessionState& session, const std::string& basePath,
                                       const std::optional<std::string>& ifExpr,
                                       const std::optional<std::string>& inRange, bool complement,
                                       const RowIndexCache* idx) {
  return filterIfAndIn(session, basePath, ifExpr, inRange, complement, idx, /*keepIndex=*/false);
}

// e(sample) T2 (internal note esample/TASKS.md T2 item 3; SPEC §3.1).
// DESIGN DECISION: an estimation command that pre-filters its `if`/`in` sample
// into a scratch copy (runRegress's regressDataPath) and asks the engine for its
// estimation sample (`--esample-out`) needs each scratch row's position in the
// working frame, so the engine can scatter the admitted rows back onto it
// (`--esample-index COL --esample-n N`). This is applyIfAndInFilter with
// complement=false and the row-index column the `in` path already materializes
// (kIdxVar, the RowIndexCache mechanism) KEPT on the copy -- added on the if-only
// path too, in the same single engine call. When neither `if` nor `in` is given
// the result is basePath itself and `*indexVar` is left empty: the fit reads the
// working frame and needs no index. The extra column rides along on the copy
// only; the copy is never adopted as the working frame. Declared at its one
// caller (handlers.cpp) because piet/inrange_filter.h is outside the task's
// write manifest.
InRangeFilterResult applyIfAndInFilterKeepingIndex(SessionState& session,
                                                   const std::string& basePath,
                                                   const std::optional<std::string>& ifExpr,
                                                   const std::optional<std::string>& inRange,
                                                   std::string* indexVar) {
  InRangeFilterResult res = filterIfAndIn(session, basePath, ifExpr, inRange,
                                          /*complement=*/false, nullptr, /*keepIndex=*/true);
  if (indexVar != nullptr) indexVar->clear();
  if (res.ok && res.dataPath != basePath && indexVar != nullptr) *indexVar = kIdxVar;
  return res;
}

RowIndexCache buildRowIndexCache(SessionState& session, const std::string& basePath) {
  // Same stateless discipline as the chain it feeds: the cache file is read
  // back AS A FILE by every slice call, so it must be real bytes on disk.
  SessionState::ResidentSuspend statelessChain(session);
  session.ensureMaterializedHead();
  RowIndexCache cache;
  {
    Datastore baseDs(basePath);
    cache.nRows = baseDs.nRows();
  }
  std::string idxPath = nextInRangePath(session.workdir(), "inidx");
  runCobraJson(
      {"eval", "--data", basePath, "--bygen", std::string(kIdxVar) + "=_n", "--out", idxPath});
  cache.path = idxPath;
  return cache;
}

std::optional<std::string> groupObsNumberFrame(SessionState& session, const std::string& dataPath,
                                              const std::string& cond,
                                              const std::vector<std::string>& groupKeys,
                                              const std::string& outPath) {
  if (groupKeys.empty() || !namesObsNumber(cond)) return std::nullopt;
  const std::string byOpt = byOption(groupKeys);
  SessionState::ResidentSuspend statelessChain(session);
  runCobraJson({"eval", "--data", dataPath, "--bygen", std::string(kGroupObsVar) + "=_n" + byOpt,
                "--bygen", std::string(kGroupNObsVar) + "=_N" + byOpt, "--out", outPath});
  return renameObsNumbers(cond);
}

std::optional<nlohmann::ordered_json> addIfMaskColumn(SessionState& session, RowIndexCache& cache,
                                                      const std::string& ifExpr,
                                                      const std::vector<std::string>& groupKeys) {
  SessionState::ResidentSuspend statelessChain(session);
  const std::string byOpt = byOption(groupKeys);
  std::string maskPath = nextInRangePath(session.workdir(), "inidx_if");
  try {
    if (strmutate::needsConditionMask(session, ifExpr)) {
      std::string framePath = cache.path;
      std::string cond = ifExpr;
      if (!groupKeys.empty() && namesObsNumber(ifExpr)) {
        framePath = nextInRangePath(session.workdir(), "inidx_byn");
        cond = *groupObsNumberFrame(session, cache.path, ifExpr, groupKeys, framePath);
      }
      strmutate::ConditionMaskInstall mi =
          strmutate::writeConditionMaskColumn(session, framePath, cond, kIfMaskVar, maskPath);
      if (!mi.ok) return mi.error;
      if (framePath != cache.path) {
        std::string dropped = nextInRangePath(session.workdir(), "inidx_if");
        runCobraJson({"eval", "--data", maskPath, "--drop", kGroupObsVar, "--drop",
                      kGroupNObsVar, "--out", dropped});
        maskPath = dropped;
      }
    } else {
      // `!=0` turns the condition into 0/1 by Stata's rule: a missing value is
      // not 0, so it selects the row (FU-197).
      const std::string spec = std::string(kIfMaskVar) + "=((" + ifExpr + ")!=0)" + byOpt;
      runCobraJson(translate::injectTsContextForExpr(
          {"eval", "--data", cache.path, "--bygen", spec, "--out", maskPath}, ifExpr, session));
    }
  } catch (const CobraError& ce) {
    return nlohmann::ordered_json(eval::engineError(cobraErrorMessage(ce), ce.code));
  }
  cache.path = maskPath;
  cache.maskVar = kIfMaskVar;
  return std::nullopt;
}

std::string sliceKeepingIfMask(SessionState& session, const RowIndexCache& cache, long long a,
                               long long b) {
  SessionState::ResidentSuspend statelessChain(session);
  const std::string rangePred = std::string(kIdxVar) + ">=" + std::to_string(a) + " & " +
                                std::string(kIdxVar) + "<=" + std::to_string(b);
  std::string outPath = nextInRangePath(session.workdir(), "inif_final");
  runCobraJson({"eval", "--data", cache.path, "--keepif", rangePred, "--drop", kIdxVar, "--out",
                outPath});
  return outPath;
}

nlohmann::ordered_json inRangeRejectError(const InRangeFilterResult& r, const std::string& inRangeRaw,
                                          const std::string& cmdLabel) {
  std::string refusal = r.refusal;
  if (refusal.empty()) {
    // A caller that resolved the bounds itself and did not copy `refusal`.
    // Well-formed text can only have failed resolution, so it is a range
    // refusal; which of Stata's two sentences needs the row count only for a
    // from-the-end bound past row 1, and that one reads as out of range here.
    std::string p1, p2;
    splitRange(inRangeRaw, p1, p2);
    const BoundToken ta = lexBound(p1);
    const BoundToken tb = lexBound(p2);
    if (ta.ok && tb.ok) {
      auto isZero = [](const BoundToken& t) {
        return (t.kind == BoundToken::kFromStart || t.kind == BoundToken::kFromEnd) && t.k == 0;
      };
      const bool aZero = isZero(ta);
      const bool bZero = isZero(tb);
      refusal = aZero ? invalidObsMessage(ta.text)
                      : bZero ? invalidObsMessage(tb.text) : std::string(kOutOfRange);
    }
  }
  if (!refusal.empty()) {
    return eval::errorBlock("syntax_error", refusal, "in " + inRangeRaw, std::nullopt,
                            std::nullopt, "r(198)");
  }
  return eval::unsupportedOption(
      "in " + inRangeRaw,
      "This `in` row-range form is not supported for " + cmdLabel + ". Supported forms: #, "
      "#/#, with # a positive or negative (from-the-end) observation number, f or l.");
}

}  // namespace piet::handlers
