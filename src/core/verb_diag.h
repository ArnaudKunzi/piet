// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// COBRA_VERB_DIAG — env-gated, per-request PHASE TIMING + SCRATCH-BYTE
// instrumentation for a handful of verb paths (joinby, merge, the --keepif row
// filter, preserve/restore), added for a performance-campaign attribution
// pass (measure before any fix). Same idiom as the two existing precedents —
// COBRA_OPEN_PHASE_DIAG (io/columnar.cpp Dataset::open) and COBRA_HDRLOG
// (data/frame.cpp write_hbin_full): a cached `static const bool` getenv check,
// std::chrono::steady_clock for the timing, and ALL emission to stderr — never
// stdout, which stays byte-identical whether the env var is set or not (the
// freeze fixtures diff stdout). When the env var is unset, the OFF path is one
// bool check: no clock calls, no allocations.
//
// This header only carries the shared timing/stat plumbing; each call site
// builds and prints its own "[verb-diag] verb=<name> ..." line (the phase set
// differs per verb), see apps/cmds/cmds_data.cpp (joinby/merge/eval --keepif),
// apps/cmds/common.cpp (maybe_keepif's scratch write), and apps/cecon_cli.cpp
// (serve stash/unstash).
// TRANSPORT NOTE (2026-08-22, found by live-fire — the reason this header has an
// emit function at all): stderr alone makes this instrument INVISIBLE where it
// matters. PIET drives the engine as a persistent `cobra serve` worker and
// DISCARDS the child's stderr on the success path, so a corpus run — the only
// workload we actually want attributed — shows zero diag lines while a
// standalone `cobra eval` shows them fine. COBRA_HDRLOG already solved this by
// writing to a FILE named by its env var. So: COBRA_VERB_DIAG=1 (or any value
// that is not a path) keeps the stderr behaviour for hand probes, and
// COBRA_VERB_DIAG=/some/path appends there instead, which is what survives the
// PIET transport. Append, not truncate, because one corpus run is many engine
// requests across a long-lived worker (and possibly several workers).
#pragma once

#include <chrono>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <map>
#include <string>
#include <system_error>

namespace cecon {

using VerbDiagClock = std::chrono::steady_clock;

// Cached once per process (magic-static; every inline-function instance across
// translation units shares the same one under the One Definition Rule, so the
// env var is read exactly once regardless of how many .cpp files call this).
inline bool verb_diag_enabled() {
    static const bool on = std::getenv("COBRA_VERB_DIAG") != nullptr;
    return on;
}

// `enabled` is passed in (rather than re-read here) so a call site pays for
// the getenv-backed bool exactly once and reuses it across every phase probe
// in the same request — matching the OPEN_PHASE_DIAG/HDRLOG single-check shape.
inline VerbDiagClock::time_point verb_diag_now(bool enabled) {
    return enabled ? VerbDiagClock::now() : VerbDiagClock::time_point{};
}

inline double verb_diag_ms(bool enabled, VerbDiagClock::time_point t0) {
    if (!enabled) return 0.0;
    return std::chrono::duration<double, std::milli>(VerbDiagClock::now() - t0).count();
}

// stat() a just-written file for its final byte size; 0 on any error (a
// diagnostic probe must never turn a successful write into a hard failure).
inline int64_t verb_diag_file_size(const std::filesystem::path& p) {
    std::error_code ec;
    const auto sz = std::filesystem::file_size(p, ec);
    return ec ? 0 : static_cast<int64_t>(sz);
}

// The sink, resolved once: a path (contains '/') -> that file, opened in append
// mode per line so concurrent workers interleave whole lines rather than
// clobbering each other's buffers; anything else -> stderr.
inline const char* verb_diag_path() {
    static const char* p = [] {
        const char* v = std::getenv("COBRA_VERB_DIAG");
        return (v && std::strchr(v, '/')) ? v : nullptr;
    }();
    return p;
}

// ---- WHOLE-REQUEST PARTITION (COBRA_VERB_DIAG, same sink) ------------------
// The four-verb probes above answer "where does joinby's time go". They cannot
// answer "is joinby where the engine's time goes" — measured 2026-08-22, they
// covered 41.4% of engine wall and the residual 58.6% was invisible. Worse, two
// PIET-side labels turned out to bill work that happens under a DIFFERENT engine
// op: `preserve` (304.6 s PIET-side) spends 0.0 s in the engine's stash op,
// because residentPreserve() materializes the head first, and that write is a
// separate request. A partition that misses 59% cannot rank fixes.
//
// So: every serve request is timed and billed to its own op name, and the totals
// are dumped once at process exit. This is a COMPLETE partition by construction
// — nothing can hide in a residual, because the residual is one of the rows.
struct VerbDiagReqStat { int64_t n = 0; double ms = 0.0; };
inline std::map<std::string, VerbDiagReqStat>& verb_diag_reqs() {
    static std::map<std::string, VerbDiagReqStat> m;
    return m;
}
inline void verb_diag_bill(const std::string& op, double ms) {
    auto& s = verb_diag_reqs()[op];
    ++s.n;
    s.ms += ms;
}
// Flushed by an explicit call at the end of the serve loop, NOT by atexit —
// PIET SIGTERMs its workers, so atexit never runs (the same reason COBRA_HDRLOG
// flushes on a cadence). Also safe to call more than once.
inline void verb_diag_flush_requests();

// Every call site emits through here instead of fprintf(stderr, ...) directly,
// so the transport decision lives in exactly one place.
inline void verb_diag_emit(const char* fmt, ...) {
    std::va_list ap;
    if (const char* p = verb_diag_path()) {
        if (FILE* f = std::fopen(p, "a")) {
            va_start(ap, fmt);
            std::vfprintf(f, fmt, ap);
            va_end(ap);
            std::fclose(f);
            return;
        }
        // Fall through to stderr rather than losing the line silently — a
        // diagnostic that vanishes when its file is unwritable is the exact
        // class this campaign is auditing (BUG-362).
    }
    va_start(ap, fmt);
    std::vfprintf(stderr, fmt, ap);
    va_end(ap);
}

inline void verb_diag_flush_requests() {
    if (!verb_diag_enabled()) return;
    auto& m = verb_diag_reqs();
    if (m.empty()) return;
    // Emits the DELTA since the previous flush (the map is cleared after), so a
    // reader sums all [req-diag] rows per op. Percentages are deliberately NOT
    // computed here: a per-flush percentage of a partial window is a number that
    // reads like a total and is not one.
    for (const auto& [op, s] : m) {
        verb_diag_emit("[req-diag] op=%s n=%lld total_ms=%.3f\n",
                       op.c_str(), (long long)s.n, s.ms);
    }
    m.clear();
}

}  // namespace cecon
