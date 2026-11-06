// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// PIET 2.2.0 item D — persisted app-shell settings + hardware detection.
//
// The "Engine & scratch" half of the Settings panel is persisted NATIVELY
// here (a small JSON file in the per-user config dir); the "Display" half
// (editor/results font size, paste-autorun mirror) is frontend localStorage
// and never reaches this module. Classified app-shell (same family as
// dialog.open / legal.notices): NOT a Boundary-B kernel endpoint, no
// SessionState, no ResultBlock — contract_B is unchanged. The settings.get /
// settings.set routes (routes.h) are thin JSON wrappers over the free
// functions below, kept as free functions so selftest.cpp can exercise them
// without opening a real window.
#ifndef PIET_NATIVE_SETTINGS_H
#define PIET_NATIVE_SETTINGS_H

#include <cstdint>
#include <string>
#include <vector>

#include "vendor/nlohmann/json.hpp"

namespace piet::settings {

// Persisted engine/scratch settings. Empty string / 0 mean "use the default"
// (so a fresh install persists nothing surprising and the recommended budget
// is recomputed from RAM each launch until the user pins one).
struct Settings {
  std::string scratchPath;        // "" -> $PIET_WORKDIR / OS temp (restart-to-apply)
  std::string defaultWorkingDir;  // "" -> defaultUserCwd()      (restart-to-apply)
  int64_t memoryBudgetMb = 0;     // 0  -> recommended (SPEC II-2)
  int computeCores = 0;           // 0  -> auto (all cores)
  // P6: false -> estimation_m4.cpp's shared cobra-argv path appends
  // --no-provenance (interactive default: OFF, applies to the next
  // command). true -> nothing appended, so the engine CLI's own default
  // (provenance ON) is used. This flips only PIET's native default; the
  // cobra CLI itself is untouched and still defaults to provenance ON.
  bool provenanceHash = false;

  // N1 (P8.1-W2, contract_B 2.4.0): default folder the frontend composes
  // export paths against for the "Save to default export folder" flow (the
  // path it then hands to POST /api/file/save as an explicit `path`). "" ->
  // no default set (frontend falls back to the interactive save panel).
  std::string defaultExportDir;

  // N2 (P8.1-W2, contract_B 2.4.0): opaque, frontend-owned UI-state blob
  // (panel sizes, last-open tab, etc.) persisted natively so it survives a
  // reinstall/cache-clear — replaces localStorage as the durable store
  // (localStorage stays as a fallback). Native NEVER parses it; it is a
  // ≤64 KB string round-tripped verbatim. "" -> nothing stored yet.
  std::string uiState;

  // WS-B (extcompat, 2026-08-06). true -> PIET answers to the 17
  // community-package compatibility command names (registry.cpp's
  // communityNameTable()); false -> those spellings are rejected with r(199)
  // and only PIET's own names work. DEFAULT TRUE, and true is also what an
  // ABSENT settings file yields — a headless `--do` run, a selftest, and a
  // gate harness therefore behave exactly as they did before this field
  // existed, with no dialog and no extra log line.
  bool extcompat = true;

  // WS-B: ISO-8601 timestamp the user accepted the first-run notice, "" =
  // not yet consented. The frontend shows its consent dialog iff this is
  // empty; native never parses the value, it only round-trips it.
  std::string extcompatConsentAt;

  // true -> opening a do-file in the app moves the working directory to its
  // folder, unless the user has already chosen a directory (a `cd`, or a default
  // working directory set above). false -> opening never moves it, as in Stata.
  // Absent in every store written before this field existed, which reads as true.
  bool followDoFileFolder = true;
};

// Detected hardware (SPEC II-1): read once per settings.get. RAM via
// sysctl/GlobalMemoryStatusEx/sysconf; cores via std::thread::hardware_
// concurrency(). recommendedBudgetMb = half of total RAM (SPEC II-2's "safe
// default for an 8 GB machine" -> 4096 MB).
struct SysInfo {
  int64_t totalRamMb = 0;
  int cpuCount = 0;
  int64_t recommendedBudgetMb = 0;
};

// On-disk settings file. $PIET_SETTINGS_FILE overrides (selftest / --ui-smoke
// isolation so a gate run never clobbers real user settings); otherwise the
// per-user config dir (~/Library/Application Support/PIET on macOS,
// %APPDATA%\PIET on Windows, $XDG_CONFIG_HOME|~/.config/PIET on Linux).
std::string settingsFilePath();

SysInfo detectSysInfo();

// Read settingsFilePath() (defaults on absent/malformed — never throws).
Settings load();

// Write settingsFilePath() (creates the parent dir). Returns false on IO error.
bool save(const Settings& s);

// Route the two performance settings to the engine subprocess through this
// process's environment (cobra_runner.cpp / cobra_session.cpp fork+exec inherit
// it), which is the ONE channel the engine reads (src/core/threads.h):
//
//   compute_cores    > 0 -> CECON_NUM_THREADS + VECLIB_MAXIMUM_THREADS = cores
//                           (the engine's own pool AND Accelerate's BLAS pool on
//                           macOS both follow the core budget; ruling #11 made
//                           compute_cores the TOTAL budget, which a `parallel`
//                           block divides among its workers — parallel.cpp)
//                    <= 0 -> auto: RELEASE what PIET set earlier in this process.
//                           BUG-386: it used to leave the previous cap in force,
//                           so Auto after "4" stayed at 4 until a restart.
//   memory_budget_mb > 0 -> CECON_MEMORY_BUDGET_MB = mb
//                    <= 0 -> the recommended budget (half of RAM, detectSysInfo)
//
// Ownership: each variable PIET sets is shadowed by PIET_OWNS_<name> holding the
// same value. Auto releases a variable only while the shadow matches it, so a
// pin PIET did NOT set — a gate's, a harness's, or the per-worker share a parent
// `parallel` block handed this process — survives exactly as PIET_WORKDIR does
// (main.cpp applyPersistedSettings). An explicit user value overrides any pin:
// the user acted, the pin did not.
//
// Both bump the engine-settings epoch so a live `cobra serve` respawns and
// re-reads the environment before its next request (cobra_session.h).
void applyComputeCores(int cores);
void applyMemoryBudget(int64_t memoryBudgetMb);
// Both of the above, in that order — the call every settings-application site
// makes (GUI launch, `--do` launch, the settings.set route).
void applyEngineBudget(const Settings& s);

// True when $PIET_SETTINGS_FILE is set — the gate/test isolation hook
// settingsFilePath() already honours (selftest, --ui-smoke, any harness that
// must not touch the real user config). WS-B reads it for ONE decision: a run
// under isolation never asks for consent, so the first-run dialog can never
// stall an automated gate. This is the test-mode auto-accept, and it is
// decided natively rather than by a frontend flag so no product code path
// depends on a magic query string.
bool testIsolationActive();

// WS-B: the live extcompat state for THIS process. Read once from the
// persisted settings on first use and then cached in an atomic (the dispatch
// gate consults it on every command line — a file read there would be a
// per-line I/O tax on the hottest path in the interpreter). `set extcompat
// on|off` moves the cached value for the session WITHOUT writing to disk;
// only the consent dialog / Settings panel (settings.set) persist a choice,
// which also refreshes this cache.
bool extcompatEnabled();
void setExtcompatEnabled(bool on);
// Drop the cache so the next extcompatEnabled() re-reads from disk. Used by
// settings.set after a successful write, and by selftest between cases.
void invalidateExtcompatCache();

// ── Component packs (WS-B step 6 — STRUCTURE ONLY) ──────────────────────────
// `<config dir>/packs/` — the sibling of settings.json, so one directory holds
// everything this app keeps for a user. See docs/PACKS.md for the manifest
// convention this enumerates against.
std::string packsDir();

// The boot-time scan. It ENUMERATES and nothing else: it returns the ids of
// the subdirectories that carry a `manifest.json`, and it does not read that
// manifest, register a command, load a library, execute anything, or touch the
// network. Loading is deliberately unimplemented (a pack that could run code
// is a design question, not a directory listing) and network install is
// forbidden outright by the offline-by-default rule in the Constitution — the
// opt-in design is written down in docs/PACKS.md instead of being built.
// A missing/unreadable packs dir is the normal case and yields an empty list.
std::vector<std::string> enumeratePacks();

// JSON body for the settings.get route: the persisted fields, a `sys` object
// (SysInfo), and `settings_file` (the resolved path, shown in the panel).
nlohmann::ordered_json toGetJson(const Settings& s, const SysInfo& sys,
                                 const std::string& file);

}  // namespace piet::settings

#endif  // PIET_NATIVE_SETTINGS_H
