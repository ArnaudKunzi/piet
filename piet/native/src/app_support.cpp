// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// R1 (selftest split, 2026-09-01): app-support helpers extracted VERBATIM from
// main.cpp's `namespace piet` block (old lines 101-333) so that BOTH
// executables — piet_native (product) and piet_selftest (the 24k-line test
// suite, no longer linked into the product) — can share them. selftest.cpp
// already consumed all five entry points via forward declarations; nothing
// about their behavior changes, only which TU owns them.
#ifndef _WIN32
#include <signal.h>   // kill(pid, 0) — BUG-133 orphan-scratch liveness probe
#include <unistd.h>
#endif
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

#include "io/win_compat.h"
#include "piet/handlers.h"  // resolveUserPath
#include "piet/session_state.h"
#include "piet/settings.h"

namespace piet {

// Seeds SessionState::cwd for a GUI-launched session. Finder/dock launches
// (and any launch not from an interactive shell) have getcwd()=="/" — that
// bogus root cwd is what made `use "fatalities.csv"` resolve to
// "/fatalities.csv" instead of a real directory (the M2/M3-era resolvePath
// fallback only kicks in for an UNSET cwd, and "/" is a perfectly "set",
// just wrong, value). Prefers $HOME/Documents — a real, user-writable
// directory the person actually has files in, same instinct as any desktop
// app's default save location — falling back to $HOME, falling back to the
// process's own cwd as an absolute last resort (so this never seeds an
// empty value on purpose). Exposed at namespace scope (not file-local) so
// selftest.cpp's cwd-seeded-at-startup leg can call the SAME function
// runRealFrontend() uses, not a paraphrase.
std::string defaultUserCwd() {
  namespace fs = std::filesystem;
#ifdef _WIN32
  std::string home = cecon::env_utf8("USERPROFILE");
#else
  std::string home = cecon::env_utf8("HOME");
#endif
  if (!home.empty()) {
    fs::path docs = cecon::to_native_path(home) / "Documents";
    std::error_code ec;
    // Prefer Documents/PIET when it exists: the Windows installer creates it
    // (demo kit: pangram_tour.do + fatalities.csv), so a fresh install opens
    // with `use "fatalities.csv"` already resolvable — no cd, no copying.
    // Where no installer made the folder (dev builds, macOS today) it simply
    // does not exist and the seed stays Documents, unchanged.
    fs::path pietDocs = docs / "PIET";
    if (fs::is_directory(pietDocs, ec)) return pietDocs.u8string();
    if (fs::is_directory(docs, ec)) return docs.u8string();
    return home;
  }
  std::error_code ec;
  fs::path cur = fs::current_path(ec);
  return ec ? std::string(".") : cur.u8string();
}

// Result of resolving + validating a `file.read` bridge request (WS-F1
// scope addendum, native-paths stabilization wave). Factored out of the
// inline bindRoute lambda below (bindAllRoutes) so selftest.cpp can
// exercise every rejection rule directly — piet::Shell's constructor opens
// a REAL WKWebView/GTK/WebView2 window (see shell.h's file header), so
// --selftest (headless by design) cannot construct one to reach an
// inline-lambda-only route the way it reaches piet::routes::* handlers.
struct FileReadResult {
  bool ok = false;
  std::string errorMessage;  // set iff !ok
  std::string source;        // set iff ok
  std::string resolvedPath;  // set iff ok
};

FileReadResult readDoFileForBridge(const SessionState& session, const std::string& rawPath) {
  namespace fs = std::filesystem;
  FileReadResult r;

  std::string resolved = resolveUserPath(session, rawPath);

  // .do extension, case-insensitive — checked before touching the
  // filesystem at all (cheap reject for the common "wrong file" mistake).
  std::string ext;
  size_t dot = resolved.find_last_of('.');
  size_t slash = resolved.find_last_of('/');
  if (dot != std::string::npos && (slash == std::string::npos || dot > slash)) {
    ext = resolved.substr(dot + 1);
    std::transform(ext.begin(), ext.end(), ext.begin(),
                   [](unsigned char c) { return std::tolower(c); });
  }
  if (ext != "do") {
    r.errorMessage = "file.read: only .do files are readable (resolved to '" + resolved + "')";
    return r;
  }

  std::error_code ec;
  if (!fs::exists(cecon::to_native_path(resolved), ec) || ec) {
    r.errorMessage = "file.read: file not found: " + resolved;
    return r;
  }
  if (!fs::is_regular_file(cecon::to_native_path(resolved), ec) || ec) {
    r.errorMessage = "file.read: not a regular file: " + resolved;
    return r;
  }
  constexpr uintmax_t kMaxBytes = 2 * 1024 * 1024;  // 2 MB
  uintmax_t size = fs::file_size(cecon::to_native_path(resolved), ec);
  if (ec) {
    r.errorMessage = "file.read: could not stat file: " + resolved;
    return r;
  }
  if (size > kMaxBytes) {
    r.errorMessage = "file.read: file too large (" + std::to_string(size) + " bytes, max " +
                      std::to_string(kMaxBytes) + ")";
    return r;
  }

  std::ifstream f(cecon::to_native_path(resolved), std::ios::binary);
  if (!f) {
    r.errorMessage = "file.read: could not open file: " + resolved;
    return r;
  }
  std::ostringstream ss;
  ss << f.rdbuf();

  r.ok = true;
  r.source = ss.str();
  r.resolvedPath = resolved;
  return r;
}

// Opening a do-file in the app makes its folder the working directory, so a
// relative `use "data.dta"` in it finds the data beside it instead of looking in
// the seeded Documents folder. It defers to the user: a directory they chose (a
// `cd`, or a default working directory in Settings) stays, so a project that
// runs `code/a.do` from its root behaves as in Stata, and Settings can turn the
// behaviour off outright. Goes through `cd`'s own handler so every consumer of
// the working directory (the business-calendar search path included) moves with
// it. Returns the new directory, or "" when nothing moved. Only the app's open
// gestures call this: a typed `do FILE` keeps the working directory, as Stata's
// does and as scripts that chain do-files by relative path rely on.
std::string followDoFileDirectory(SessionState& session, const std::string& resolvedDoFile) {
  namespace fs = std::filesystem;
  if (session.cwdChosenByUser || !settings::load().followDoFileFolder) return std::string();
  const fs::path dir = cecon::to_native_path(resolvedDoFile).parent_path();
  if (dir.empty()) return std::string();
  std::error_code ec;
  if (session.cwd.has_value() && !session.cwd->empty() &&
      fs::equivalent(cecon::to_native_path(*session.cwd), dir, ec) && !ec) {
    return std::string();
  }
  const nlohmann::ordered_json block = handlers::runCd(cecon::from_native_path(dir), session);
  if (block.value("type", std::string()) != "echo") return std::string();
  return block.value("text", std::string());
}

// ── BUG-133: orphan-scratch reaping ─────────────────────────────────────────
// makeWorkdir() mkdtemp's a per-process `piet_native_XXXXXX` scratch dir that a
// RAII ScratchDirGuard (below) removes at NORMAL process exit. But the fleet
// routinely bounds runaway engine subprocesses with SIGTERM→SIGKILL (kill_tree;
// see IDIOMS.md "Process lifecycle"), and NEITHER signal runs C++ destructors —
// SIGTERM's default disposition terminates without stack unwinding and SIGKILL
// is uncatchable — so the guard never fires and the whole multi-GB scratch tree
// is orphaned. A real 7.5 GB orphan is what pushed an UNRELATED live session to
// ENOSPC (BUG-133). Since kill_tree bounding is routine, this recurs.
//
// Class fix (BUGLOG fix-direction (b)): stamp each scratch dir with the creating
// process's pid, and on the next launch sweep the base and reap only the dirs
// whose owner is PROVABLY dead. (We deliberately do NOT install a SIGTERM
// handler to reap on the way out — remove_all is not async-signal-safe, and the
// startup sweep already covers BOTH signals, which the RAII guard cannot.)
//
// SAFETY (07-18 INCIDENT: a wildcard rm once hit a LIVE scratch dir):
//   * reap ONLY dirs carrying OUR marker file that names a DEAD pid;
//   * an UNMARKED `piet_native_*` dir (a pre-fix binary's, or another tool's) is
//     left ALONE — conservative skip, never a wildcard delete;
//   * a marker whose pid is still alive, or whose liveness is uncertain
//     (EPERM / another user's pid / unparseable), is treated as LIVE and kept;
//   * an mtime floor keeps us off a dir abandoned only moments ago (whose writer
//     may still be flushing) and blunts pid-reuse races;
//   * removal always names a SPECIFIC path (base/entry), never a glob.
// The only failure mode this leaves is a false-negative — an orphan lingers one
// more launch; reaping a live dir is structurally excluded.

// Marker file dropped at the top of every mkdtemp'd scratch dir. The leading dot
// keeps it clear of the named hbin/csv files cobra writes into the same dir.
const char* scratchOwnerMarkerName() { return ".piet_owner_pid"; }

// Minimum age (from the dir's own mtime) before a dead-owner dir is eligible —
// belt-and-suspenders against pid reuse and against a just-abandoned dir. A
// genuine orphan is simply collected on a later launch instead.
static constexpr int kScratchReapMinAgeSec = 120;

// Is `pid` a live process? Conservative: anything other than a definitive "no
// such process" answer is reported ALIVE, so an uncertain owner is never reaped.
static bool scratchPidAlive(long pid) {
  if (pid <= 0) return true;  // unparseable / absent → treat as live (skip)
#ifdef _WIN32
  HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE,
                         static_cast<DWORD>(pid));
  if (h == nullptr)
    // ERROR_INVALID_PARAMETER ⇒ no such pid (dead); access-denied etc. ⇒ live.
    return GetLastError() != ERROR_INVALID_PARAMETER;
  DWORD code = 0;
  bool alive = GetExitCodeProcess(h, &code) && code == STILL_ACTIVE;
  CloseHandle(h);
  return alive;
#else
  if (::kill(static_cast<pid_t>(pid), 0) == 0) return true;  // deliverable ⇒ alive
  return errno != ESRCH;  // ESRCH ⇒ gone; EPERM (other user's pid) ⇒ keep (live)
#endif
}

// Read the pid a scratch dir was stamped with, or -1 if it carries no readable
// marker (→ caller treats it as unmarked and skips it).
static long readScratchOwnerPid(const std::filesystem::path& dir) {
  std::ifstream f(dir / scratchOwnerMarkerName());
  if (!f) return -1;
  long pid = -1;
  f >> pid;
  if (!f) return -1;
  return pid;
}

// Stamp a freshly-created scratch dir with the creating process's pid so a later
// launch can tell whether it was orphaned. Best-effort: a marker we fail to
// write just means this dir won't be auto-reaped (it falls to the conservative
// "unmarked → skip" rule, never to a mis-reap). Exposed at namespace scope so
// selftest.cpp can stamp a synthetic dir with the SAME writer, not a paraphrase.
void writeScratchOwnerMarker(const std::string& dir) {
  std::ofstream f(cecon::to_native_path(dir) / scratchOwnerMarkerName(),
                  std::ios::binary | std::ios::trunc);
  if (!f) return;
#ifdef _WIN32
  f << static_cast<long>(GetCurrentProcessId()) << '\n';
#else
  f << static_cast<long>(::getpid()) << '\n';
#endif
}

// Sweep `base` for our `piet_native_*` scratch dirs whose owner pid is provably
// dead AND whose mtime is old enough, and remove exactly those. Returns the
// number reaped. Exposed at namespace scope so selftest.cpp drives it directly.
int reapDeadScratch(const std::string& base) {
  namespace fs = std::filesystem;
  // Honour the same escape hatch as ScratchDirGuard: if the operator asked to
  // keep scratch (debugging a run's hbins), collect nobody's — old or new.
  if (const char* keep = std::getenv("PIET_KEEP_SCRATCH"); keep && *keep) return 0;
  std::error_code ec;
  fs::path baseNative = cecon::to_native_path(base);
  if (!fs::is_directory(baseNative, ec)) return 0;
  int reaped = 0;
  fs::directory_iterator it(baseNative,
                            fs::directory_options::skip_permission_denied, ec),
      end;
  if (ec) return 0;
  for (; it != end; it.increment(ec)) {
    if (ec) break;
    const fs::path& p = it->path();
    std::error_code dec;
    if (!fs::is_directory(p, dec) || dec) continue;
    // Only our own scratch names (both the mkdtemp form and the PID-qualified
    // fallback form share this prefix).
    if (p.filename().string().rfind("piet_native_", 0) != 0) continue;
    long owner = readScratchOwnerPid(p);
    if (owner <= 0) continue;               // unmarked → conservative skip
    if (scratchPidAlive(owner)) continue;   // live/uncertain owner → never touch
    std::error_code tec;
    auto ftime = fs::last_write_time(p, tec);
    if (tec) continue;
    auto age = fs::file_time_type::clock::now() - ftime;
    if (age < std::chrono::seconds(kScratchReapMinAgeSec)) continue;  // too fresh
    std::error_code rmec;
    fs::remove_all(p, rmec);  // SPECIFIC path, never a glob
    if (!rmec) ++reaped;
  }
  return reaped;
}

}  // namespace piet
