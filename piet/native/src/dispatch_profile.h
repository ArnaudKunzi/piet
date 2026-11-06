// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
#ifndef PIET_DISPATCH_PROFILE_H
#define PIET_DISPATCH_PROFILE_H

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

// COMPILE-TIME GATE. The zones sit on the per-line path whose entire budget is
// ~1 us after WS-D1, so even the two perfectly-predicted branches an
// always-compiled RAII scope costs are a tax on the number being reported.
// Default OFF: with PIET_ENABLE_DISPATCH_PROFILE undefined every Scope is an
// empty object and every note*() an empty inline, so the emitted code — and
// therefore stdout, and therefore every freeze fixture and answerbook pin — is
// identical to a build without this header. Turn it on for a profiling build:
//   cmake -DPIET_ENABLE_DISPATCH_PROFILE=ON …   (then PIET_DISPATCH_PROFILE=1
//   in the environment selects emission at run time)

#if defined(__aarch64__) || defined(_M_ARM64)
#define PIET_PROF_ARM_TICKS 1
#else
#include <chrono>
#endif

// DISPATCH ZONE PROFILER (P2-PERF WS-D1).
//
// WHY THIS EXISTS. Two sessions of this campaign each guessed at the ~3.3 us
// per-line fixed cost of dispatch(), implemented the guess, measured exactly
// zero, and reverted (session 1: hoisting evalBoolExpr's regexes; session 2:
// an O(1) index over the macro store). The plan's ruling for the third attempt
// was "a profile, not a guess", so this header exists to make the attribution
// reproducible by anyone, at any later date, without a sampling profiler and
// without reading the code by hand.
//
// CONVENTION — deliberately the same one src/core/eng_counters.h and PIET's
// counters.h already follow, for the same reasons: process-wide statics, no
// locking (dispatch is a synchronous single-threaded loop), emission opt-in.
// The differences from eng_counters are forced by what is being measured:
//
//   * ACCUMULATION IS OPT-IN HERE, not always-on. eng_counters increments an
//     integer on paths already doing file-sized work; these zones sit on the
//     per-line path whose whole budget is ~3 us, so an always-on timer read
//     would be a measurable tax on the very number it reports. `g_on` is set
//     once from the environment at first use and is a perfectly-predicted
//     branch thereafter.
//   * THE DUMP GOES TO stderr, ONCE, AT EXIT. Never stdout, never a block,
//     never a log: the answerbook/pangram gates byte-compare the display
//     surface, so a profiler that could reach stdout would be a parity defect
//     in waiting. With PIET_DISPATCH_PROFILE unset, stdout is byte-identical
//     to a build without this header, which is the freeze-fixture safety
//     property.
//
// ZONES ARE FLAT, NOT NESTED. dispatchImpl's pre-handler steps run in sequence,
// so each zone is opened and closed before the next one opens and the numbers
// sum without an inclusive/exclusive correction. A zone that returns early
// (which is the whole point for `local q 5`, which never leaves step 0a) is
// closed by the RAII destructor on the way out.
//
// TIMEBASE. On arm64 we read CNTVCT_EL0 directly (~2 ns) rather than going
// through mach_absolute_time (~20 ns), because a 20 ns read on both ends of a
// ~150 ns zone perturbs what it measures by a quarter. The counter's tick is
// coarse (24 MHz = 41.67 ns on this machine), so a single short zone reads 0 or
// 1 tick — but the quantization error is unbiased over a uniformly-distributed
// start phase, and every attribution below is a sum over >= 10^4 lines, where
// it averages out to well under a percent.
namespace piet {
namespace dprof {
#ifdef PIET_ENABLE_DISPATCH_PROFILE

enum Zone : int {
  kEntry = 0,      // noteC4TraceVerb + echoBlock + the blocks array
  kLoopIf,         // Pre-(-1): brace-block detect + braceless `if`
  kDoRun,          // Pre-(-0.5): `do`/`run FILE`
  kPrefixStrip,    // Pre-0: capture/quietly/noisily prefix stripper
  kColonPrefix,    // Pre-0b/0c + svy: the `KEYWORD: command` forms
  kSmallSteps,     // (a) continue (b) args (c) gettoken (c/d) tempdecl (e) return (f) program
  kCaptureDef,     // 0a: substituteCRefs + tryCaptureDefinition  <- `local` lands here
  kResample,       // 0a-bis: bootstrap/jackknife/permute/rolling prefixes
  kExpand,         // 0b: macro expansion (+ 0b-bis re-match)
  kTokenize,       // 1: tokenize + name resolution (lower/rstripColon)
  kNameGate,       // 1a-bis: the extcompat community-name gate
  kHandler,        // everything from step 1b onward (classify + handler)
  kWrapper,        // dispatch()'s own wrapper: depth counters + logTee
  kZoneCount
};

inline const char* zoneName(int z) {
  switch (z) {
    case kEntry: return "entry: echo block + blocks array";
    case kLoopIf: return "Pre-(-1): brace/if detect";
    case kDoRun: return "Pre-(-0.5): do/run FILE";
    case kPrefixStrip: return "Pre-0: capture/quietly prefix stripper";
    case kColonPrefix: return "Pre-0b/0c/svy: KEYWORD: forms";
    case kSmallSteps: return "(a)-(f): continue/args/gettoken/temp/return/program";
    case kCaptureDef: return "0a: substituteCRefs + tryCaptureDefinition";
    case kResample: return "0a-bis: resample prefixes";
    case kExpand: return "0b: macro expansion";
    case kTokenize: return "1: tokenize + name resolution";
    case kNameGate: return "1a-bis: community-name gate";
    case kHandler: return "1b+: classify + handler";
    case kWrapper: return "dispatch() wrapper: depth + logTee";
    default: return "?";
  }
}

inline bool profileRequested() {
  const char* e = std::getenv("PIET_DISPATCH_PROFILE");
  return e != nullptr && e[0] != '\0' && std::strcmp(e, "0") != 0;
}
// Read once at dynamic-init time (before main, therefore before any dispatch),
// so the per-line path never pays a getenv and never pays a "have I checked
// yet" branch — just one perfectly-predicted load-and-test.
inline bool g_on = profileRequested();
inline int64_t g_ticks[kZoneCount] = {0};
inline int64_t g_hits[kZoneCount] = {0};
inline int64_t g_lines = 0;
// Deterministic (non-timing) mechanism counters — the sworn witnesses of gate
// (2), against which the tick columns are only corroboration.
inline int64_t g_regexSearches = 0;   // std::regex_match/search calls on the per-line path
inline int64_t g_jsonNodes = 0;       // ojson objects constructed on the per-line path
inline int64_t g_prefixSetSorts = 0;  // stripPrefix() calls that sorted a prefix set

#if defined(PIET_PROF_ARM_TICKS)
inline uint64_t now() {
  uint64_t v;
  __asm__ __volatile__("mrs %0, cntvct_el0" : "=r"(v));
  return v;
}
inline double tickHz() {
  uint64_t f;
  __asm__ __volatile__("mrs %0, cntfrq_el0" : "=r"(f));
  return static_cast<double>(f);
}
#else
inline uint64_t now() {
  return static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(
          std::chrono::steady_clock::now().time_since_epoch())
          .count());
}
inline double tickHz() { return 1e9; }
#endif

inline bool enabled() { return g_on; }

inline void noteLine() {
  if (g_on) ++g_lines;
}
inline void noteRegex(int n = 1) {
  if (g_on) g_regexSearches += n;
}
inline void noteJsonNode(int n = 1) {
  if (g_on) g_jsonNodes += n;
}
inline void notePrefixSetSort() {
  if (g_on) ++g_prefixSetSorts;
}

// RAII scope. Constructed unconditionally at each zone site; when profiling is
// off both ctor and dtor collapse to one perfectly-predicted branch.
class Scope {
 public:
  explicit Scope(Zone z) : z_(z), t0_(g_on ? now() : 0) {
    if (g_on) ++g_hits[z_];
  }
  ~Scope() {
    if (g_on) g_ticks[z_] += static_cast<int64_t>(now() - t0_);
  }
  Scope(const Scope&) = delete;
  Scope& operator=(const Scope&) = delete;

 private:
  Zone z_;
  uint64_t t0_;
};

// ---------------------------------------------------------------------------
// EXPRESSION-LAYER ZONES (P2-PERF EXPRLAYER).
//
// WHY A SECOND FAMILY. The Zone family above is FLAT — dispatchImpl's steps run
// in sequence, so its ticks sum and a % column is meaningful. The expression
// layer is not flat: expandPass runs INSIDE tryCaptureDefinition which runs
// inside kCaptureDef, and evalArith's namespace build runs inside evalArith.
// Mixing nested zones into that array would make its % column a lie. This
// family is therefore reported separately, INCLUSIVE, with no % column — each
// row is "total wall inside this function, and how many times it was entered",
// which is what an attribution needs and all it can honestly claim.
enum EZone : int {
  kEExpandPass = 0,    // eval::expandPass — the whole body
  kEEvalArith,         // eval::evalArith — the whole body
  kEEvalArithNs,       //   ...of which: mathNamespace + addStataFns construction
  kEEvalArithScalars,  //   ...of which: the bare-scalar-name substitution loop
  kEResolveEst,        // eval::resolveEstScalarRefs
  kEBoolExpr,          // loops.cpp evalBoolExpr — the whole body (if/while)
  kEBoolRewrite,       //   ...of which: the Stata->Python operator regex chain
  kEBoolIdentScan,     //   ...of which: the referenced-identifier regex scan
  kEBoolNs,            //   ...of which: mathNamespace + addStataFns + addScalarFn
  kEBoolBind,          //   ...of which: _rc + resident card + scalar binding
  kEPyParseEval,       // eval::evalPyExprDetailed — lex + parse + evaluate
  kEEvalMacroExpr,     // eval::evalMacroExpr (`=expr' form)
  kEEvalArithWhitelist,//   evalArith: the two whole-string whitelist regex_match calls
  kEEvalArithPre,      //   evalArith: strip + ^->** + ln + matrix + _rc pre-passes
  kEExpandRBlock,      //   expandPass: the r()/`r()' reference block
  kEBoolStrlit,        //   evalBoolExpr: compound-quote + string-literal extraction
  kEBoolTail,          //   evalBoolExpr: cRefs + scalar-unresolved + bare-dot + restore
  kEBoolBindRc,        //     ...of the bind zone: the _rc lookup
  kEBoolBindCard,      //     ...of the bind zone: resident card / column values
  kEBoolBindScalars,   //     ...of the bind zone: session scalar binding
  kECapDefScalarSub,   //   tryCaptureDefinition: per-scalar name substitution
  kEZoneCount
};

inline const char* ezoneName(int z) {
  switch (z) {
    case kEExpandPass: return "expandPass";
    case kEEvalArith: return "evalArith";
    case kEEvalArithNs: return "  evalArith: namespace build";
    case kEEvalArithScalars: return "  evalArith: bare-scalar substitution";
    case kEResolveEst: return "resolveEstScalarRefs";
    case kEBoolExpr: return "evalBoolExpr (if/while)";
    case kEBoolRewrite: return "  evalBoolExpr: operator rewrite";
    case kEBoolIdentScan: return "  evalBoolExpr: ident scan";
    case kEBoolNs: return "  evalBoolExpr: namespace build";
    case kEBoolBind: return "  evalBoolExpr: _rc/card/scalar bind";
    case kEPyParseEval: return "evalPyExprDetailed (lex+parse+eval)";
    case kEEvalMacroExpr: return "evalMacroExpr (`=expr')";
    case kEEvalArithWhitelist: return "  evalArith: whitelist regex_match x2";
    case kEEvalArithPre: return "  evalArith: pre-passes (^/ln/matrix/_rc)";
    case kEExpandRBlock: return "  expandPass: r() reference block";
    case kEBoolStrlit: return "  evalBoolExpr: string-literal extraction";
    case kEBoolTail: return "  evalBoolExpr: cRefs/sentinel/restore";
    case kEBoolBindRc: return "    bind: _rc lookup";
    case kEBoolBindCard: return "    bind: card / column values";
    case kEBoolBindScalars: return "    bind: session scalars";
    case kECapDefScalarSub: return "  tryCaptureDefinition: scalar name subst";
    default: return "?";
  }
}

inline int64_t g_eticks[kEZoneCount] = {0};
inline int64_t g_ehits[kEZoneCount] = {0};

// Deterministic mechanism counters for this arc — the sworn witnesses beside
// every wall claim (gate (2)). Named, not numbered, so a later reader does not
// have to map indices back to intentions.
inline int64_t g_nsBuilds = 0;      // full PyNamespace constructions (fn-map inserts)
inline int64_t g_nsFnInserts = 0;   // individual function-map insertions
inline int64_t g_exprCacheHits = 0;
inline int64_t g_exprCacheMisses = 0;
inline int64_t g_macroGenBumps = 0;
inline int64_t g_numericSlotHits = 0;
inline int64_t g_blocksElided = 0;

inline void noteNsBuild(int fnInserts) {
  if (g_on) {
    ++g_nsBuilds;
    g_nsFnInserts += fnInserts;
  }
}
inline void noteExprCache(bool hit) {
  if (g_on) {
    if (hit) ++g_exprCacheHits; else ++g_exprCacheMisses;
  }
}
inline void noteMacroGenBump() {
  if (g_on) ++g_macroGenBumps;
}
inline void noteNumericSlotHit() {
  if (g_on) ++g_numericSlotHits;
}
inline void noteBlockElided() {
  if (g_on) ++g_blocksElided;
}

class EScope {
 public:
  explicit EScope(EZone z) : z_(z), t0_(g_on ? now() : 0) {
    if (g_on) ++g_ehits[z_];
  }
  ~EScope() {
    if (g_on) g_eticks[z_] += static_cast<int64_t>(now() - t0_);
  }
  EScope(const EScope&) = delete;
  EScope& operator=(const EScope&) = delete;

 private:
  EZone z_;
  uint64_t t0_;
};

// Manual begin/end for the two sub-zones whose extent is not a C++ scope. A
// heap-allocated RAII object would have cost the SHIPPING build an operator new
// per call (EScope is empty when the profiler is compiled out, but `new` is
// not), which is exactly the tax this header exists to avoid.
// Non-reentrant by construction: no zone below can nest inside itself.
inline uint64_t g_ependingT0[kEZoneCount] = {0};
inline void ezoneBegin(EZone z) {
  if (g_on) {
    ++g_ehits[z];
    g_ependingT0[z] = now();
  }
}
inline void ezoneEnd(EZone z) {
  if (g_on) g_eticks[z] += static_cast<int64_t>(now() - g_ependingT0[z]);
}

// Emitted once, from a static destructor, to stderr only.
inline void dump() {
  if (!g_on || g_lines == 0) return;
  const double hz = tickHz();
  int64_t total = 0;
  for (int i = 0; i < kZoneCount; ++i) total += g_ticks[i];
  const double totNs = static_cast<double>(total) / hz * 1e9;
  std::fprintf(stderr, "\n[dispatch-profile] lines=%lld  measured=%.3f us/line\n",
               static_cast<long long>(g_lines), totNs / static_cast<double>(g_lines) / 1000.0);
  std::fprintf(stderr, "%-52s %10s %8s %10s\n", "zone", "us/line", "%", "hits/line");
  for (int i = 0; i < kZoneCount; ++i) {
    if (g_hits[i] == 0 && g_ticks[i] == 0) continue;
    const double ns = static_cast<double>(g_ticks[i]) / hz * 1e9;
    std::fprintf(stderr, "%-52s %10.4f %7.1f%% %10.2f\n", zoneName(i),
                 ns / static_cast<double>(g_lines) / 1000.0,
                 total ? 100.0 * static_cast<double>(g_ticks[i]) / static_cast<double>(total) : 0.0,
                 static_cast<double>(g_hits[i]) / static_cast<double>(g_lines));
  }
  std::fprintf(stderr, "[dispatch-profile] counters/line: regex_search=%.2f json_nodes=%.2f "
                       "prefix_set_sorts=%.2f\n",
               static_cast<double>(g_regexSearches) / static_cast<double>(g_lines),
               static_cast<double>(g_jsonNodes) / static_cast<double>(g_lines),
               static_cast<double>(g_prefixSetSorts) / static_cast<double>(g_lines));

  // Expression layer — INCLUSIVE, nested, no % column (see the EZone note).
  std::fprintf(stderr, "\n[expr-profile] inclusive wall per function (nested; rows do NOT sum)\n");
  std::fprintf(stderr, "%-40s %12s %12s %12s\n", "zone", "total_ms", "calls", "us/call");
  for (int i = 0; i < kEZoneCount; ++i) {
    if (g_ehits[i] == 0) continue;
    const double ns = static_cast<double>(g_eticks[i]) / hz * 1e9;
    std::fprintf(stderr, "%-40s %12.3f %12lld %12.3f\n", ezoneName(i), ns / 1e6,
                 static_cast<long long>(g_ehits[i]),
                 ns / 1000.0 / static_cast<double>(g_ehits[i]));
  }
  std::fprintf(stderr,
               "[expr-profile] counters: ns_builds=%lld ns_fn_inserts=%lld "
               "expr_cache_hits=%lld expr_cache_misses=%lld macro_gen_bumps=%lld "
               "numeric_slot_hits=%lld blocks_elided=%lld\n",
               static_cast<long long>(g_nsBuilds), static_cast<long long>(g_nsFnInserts),
               static_cast<long long>(g_exprCacheHits),
               static_cast<long long>(g_exprCacheMisses),
               static_cast<long long>(g_macroGenBumps),
               static_cast<long long>(g_numericSlotHits),
               static_cast<long long>(g_blocksElided));
}

struct AtExitDumper {
  ~AtExitDumper() { dump(); }
};
inline AtExitDumper g_atExit;

#else  // !PIET_ENABLE_DISPATCH_PROFILE — the shipping shape: nothing at all.

enum Zone : int {
  kEntry = 0, kLoopIf, kDoRun, kPrefixStrip, kColonPrefix, kSmallSteps, kCaptureDef,
  kResample, kExpand, kTokenize, kNameGate, kHandler, kWrapper, kZoneCount
};
enum EZone : int {
  kEExpandPass = 0, kEEvalArith, kEEvalArithNs, kEEvalArithScalars, kEResolveEst,
  kEBoolExpr, kEBoolRewrite, kEBoolIdentScan, kEBoolNs, kEBoolBind, kEPyParseEval,
  kEEvalMacroExpr, kEEvalArithWhitelist, kEEvalArithPre, kEExpandRBlock, kEBoolStrlit,
  kEBoolTail, kEBoolBindRc, kEBoolBindCard, kEBoolBindScalars, kECapDefScalarSub,
  kEZoneCount
};
inline bool enabled() { return false; }
inline void noteLine() {}
inline void noteRegex(int = 1) {}
inline void noteJsonNode(int = 1) {}
inline void notePrefixSetSort() {}
inline void noteNsBuild(int) {}
inline void noteExprCache(bool) {}
inline void noteMacroGenBump() {}
inline void noteNumericSlotHit() {}
inline void noteBlockElided() {}
class Scope {
 public:
  explicit Scope(Zone) {}
  Scope(const Scope&) = delete;
  Scope& operator=(const Scope&) = delete;
};
class EScope {
 public:
  explicit EScope(EZone) {}
  EScope(const EScope&) = delete;
  EScope& operator=(const EScope&) = delete;
};
inline void ezoneBegin(EZone) {}
inline void ezoneEnd(EZone) {}

#endif  // PIET_ENABLE_DISPATCH_PROFILE

}  // namespace dprof
}  // namespace piet

#endif  // PIET_DISPATCH_PROFILE_H
