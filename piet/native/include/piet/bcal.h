// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
#ifndef PIET_NATIVE_BCAL_H
#define PIET_NATIVE_BCAL_H
// Stata business calendars ([D] bcal, [D] Datetime business calendars) —
// AER scope-2 B2-3, the blocker of paper 066.
//
// `bcal create` (induceFromDates) infers a calendar from the gaps between
// the sorted values of a date column and writes the .stbcal text; its
// behaviour follows [D] bcal and measured StataBE 19.5 runs.
//
// The reader (`parseStbcal`) is the OTHER half and has a different authority:
// the .stbcal loader is Stata's built-in `_bcalendar`, whose behaviour was
// measured probe-by-probe (REPORT.md §4, 85 grammar probes in capture4.log).
// Two manual statements are contradicted by that measurement and the
// measurement wins: the loader's default `dateformat` is dmy (the manual says
// ymd), and a `from _ to _:` window on an `omit date` carrying a FULL date is
// rc 197, not accepted (REFUTE-0 T6).
//
// bofd()/dofb() are integer index arithmetic over the resulting business-day
// list; both answer MISSING for a calendar that cannot be found, which is what
// Stata itself answers (rc 0, measured).

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace piet::bcal {

// A loaded calendar. `days` is the ascending list of business days as Stata
// %td day numbers; `center` is the centerdate, whose position in `days` is the
// zero of the business index.
struct BusinessCalendar {
  std::int64_t lo = 0;   // range start (%td)
  std::int64_t hi = 0;   // range end (%td)
  std::int64_t center = 0;
  std::vector<std::int64_t> days;
  std::unordered_map<std::int64_t, std::int64_t> idx;  // %td -> position in days
  std::int64_t centerPos = 0;
  std::int64_t minTb = 0;  // -centerPos
  std::int64_t maxTb = 0;  // days.size()-1-centerPos
  std::string name;
  std::string purpose;
  std::string fn;  // resolved path the text came from ("" when parsed in memory)
};

// Parse failure. `rc` is the Stata return code the loader answers (197 for
// every grammar/consistency failure this reader can produce; the callers that
// need Stata's "act as if the file was never found" behaviour treat any
// failure as "not found" and answer missing).
struct StbcalError {
  int rc = 197;
  std::string message;
};

struct ParseResult {
  bool ok = false;
  BusinessCalendar cal;
  StbcalError err;
};

// Reader for .stbcal TEXT (REPORT.md §4 as corrected by REFUTE-0 T6/T7).
ParseResult parseStbcal(const std::string& text, const std::string& name);

// --- index arithmetic ([D] Datetime business calendars, items 5 and 8) ------
// Both take and return Stata doubles: NaN in / NaN out is "missing", and the
// truncation is toward zero (measured: -30.5, -30.9 and -30.1 all -> -30, so
// floor is refuted).
double bofd(const BusinessCalendar& cal, double d);
double dofb(const BusinessCalendar& cal, double i);

// --- `bcal create` induction ([D] bcal, bcal create) -----------------------
struct InduceOpts {
  std::string calName;
  std::string dateformat = "ymd";  // bcal create ALWAYS writes it explicitly
  std::string purpose;
  bool haveRangeLo = false, haveRangeHi = false;
  std::int64_t rangeLo = 0, rangeHi = 0;
  bool haveCenter = false;
  std::int64_t center = 0;
  std::int64_t maxgap = 10;
  std::string todayLine;  // "* Created/replaced on  8 Sep 2026" (caller's clock)
  int stataVersion10 = 195;  // written as "version 19.5"
};

struct InduceResult {
  bool ok = false;
  int rc = 0;
  std::string message;
  std::string text;  // the .stbcal bytes, when ok
  // Whether create prints the two range() Notes (range() starts before the
  // first data date / ends after the last).
  bool leadNote = false, trailNote = false;
};

// `values` are the from() column's values for the observations that pass
// if/in/excludemissing, in ANY order. NaN is Stata missing (it sorts after
// every number), so a missing value trips the "cannot be after Dec 31, 9999"
// bound.
InduceResult induceFromDates(const std::vector<double>& values, const InduceOpts& opts);

// --- the process's calendar cache ------------------------------------------
// STATA'S CACHE IS THE SEMANTICS, not an optimisation: once a calendar is
// used, erasing its file changes nothing and a hand edit is invisible until
// `bcal load` (measured, REPORT.md §2). The key is the LOWERCASED NAME ALONE
// and the cache is only ever refreshed explicitly.
//
// The name-only key is measured, not assumed (review of 2026-09-09,
// /private/tmp/fix-bcal/st/cap.log, StataBE 19.5):
//   create `cdcal` in cwd, `cd sub`            bofd = 1   (B_in_sub)
//   `bcal create cdcal, centerdate(...)` there  bofd = -1  (B_sub_recreate)
//   `cd ..` (the cwd file still says 1)         bofd = -1  (B_back)
//   create, ERASE the file, `cd sub`, `cd ..`   1 / 1 / 1  (C_*)
// The earlier (search directory, name) key therefore made a `cd` silently
// DROP a loaded calendar and a `cd` back resurrect a stale one -- a missing
// and a wrong value, both at rc 0. `cd` still re-points the SEARCH DIRECTORY
// (handlers_m3.cpp runCd), which is what lets a not-yet-loaded calendar be
// found in the new cwd (oracle l_cd_bofd_in_sub = 1); resolution is by
// directory, memory is by name.
//
// It is a PROCESS singleton and not a SessionState member because the
// expression surfaces that must reach it do not all carry a session:
// `display bofd(...)` and `local x = bofd(...)` go through
// eval::evalArith -> stataMathNamespace(), a process-wide immutable
// namespace with no per-call additions (macros.cpp:1554). Two sessions in one
// process (the --run-block harness, and every selftest leg) are kept apart by
// SessionState's constructor CALLING clearCache() -- session_state.cpp's
// resetProcessCaches(). Before this review that call did not exist and the
// isolation this comment claimed was carried, accidentally, by the search
// directory sitting in the key.
void setSearchDir(std::string dir);
const std::string& searchDir();
void clearCache();

// The calendar-NAME rule, and the ONE place it lives. A calendar name is a
// valid Stata name of at most 10 characters ([D] bcal, printed p.29), and the
// rule binds the READ path exactly as hard as it binds `bcal create`
// (measured, StataBE 19.5, an identical valid 5-line .stbcal present in cwd):
//   `bcal load elevenchars`      r(198) "elevenchars is an invalid calendar
//                                        name or %fmt"
//   `bcal describe elevenchars`  r(198), same message
//   `bofd("elevenchars", d)`     missing, rc 0
//   `bcal load ab-cd`            r(198)   ·  `bcal load 9bad`   r(198)
//   `bcal load _undersc`         r(0)     ·  `bcal load Upper`  r(0)
//   `bcal load tenchars10`       r(0)     (10 characters is inside the rule)
//   `bcal load nosuchcal`        r(601)   (a legal name with no file is the
//                                        OTHER refusal and keeps its own rc)
// `bcal dir` does NOT apply it: an eleven-character .stbcal in the directory is
// still listed (measured, r(calendars) = "elevenchars elevenchar").
// `bcal create` reports the two halves of the same predicate with two
// different messages, so it tests them separately rather than calling this.
bool isValidCalName(const std::string& name);

// The search directory a session's cwd names. An UNSET or empty session cwd
// means the PROCESS's own working directory -- the dispatcher-level setter and
// the handler's own lookup MUST derive the identical string, or `install()`
// after `bcal create` keys the cache under one directory while every
// expression surface reads it under another (review finding, non-blocking 3).
std::string searchDirFor(const std::string& sessionCwd);

// The one loader every surface goes through. `name` may carry a leading
// "%tb"; a ".stbcal" SUFFIX is rejected (measured: `bofd("a_wk.stbcal", d)`
// is missing); a name outside isValidCalName() is rejected too. Returns
// nullptr for "not found, unreadable, or not a legal calendar name", which
// every expression caller turns into Stata missing with rc 0.
std::shared_ptr<const BusinessCalendar> lookup(const std::string& name);

// `bcal load` / `bcal create ..., replace` refresh: parse the file at `path`
// under `name` and install it, replacing any cached entry. `errOut` carries
// the reader's complaint when this fails.
std::shared_ptr<const BusinessCalendar> loadFile(const std::string& name,
                                                 const std::string& path, StbcalError* errOut);

// Install an already-parsed calendar (the create path, which has just written
// and re-read the file).
void install(const std::string& name, std::shared_ptr<const BusinessCalendar> cal);

// Strip a leading "%tb" and any ":detail" tail; returns "" when `spec` does
// not name a calendar. Shared by the format layer and `bcal describe %tbX`.
std::string calNameFromFormat(const std::string& spec);

// --- small shared date helpers (public so the handler and the display layer
// do not grow second copies) ------------------------------------------------
std::int64_t daysFromCivil(std::int64_t y, unsigned m, unsigned d);
// Stata dow(): 0 = Sunday. 01jan1960 (day 0) is a Friday.
int dowOf(std::int64_t td);
// Render `td` under a bcal-create dateformat ("ymd".."dmy") the way Stata's
// derived %td display format does (2020jan02 for ymd, 02jan2020 for dmy).
std::string renderUnderDateformat(std::int64_t td, const std::string& dateformat);
// A complete date token under a dateformat, by the reader's own date rules, so
// that bcal create's range() and centerdate() and the file they end up in cannot
// disagree. nullopt when it does not read as a date in that order.
std::optional<std::int64_t> readWholeDate(const std::string& token, const std::string& dateformat);

}  // namespace piet::bcal

#endif  // PIET_NATIVE_BCAL_H
