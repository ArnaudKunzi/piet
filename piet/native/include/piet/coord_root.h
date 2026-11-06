// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
#pragma once
// piet/native/include/piet/coord_root.h
//
// BUG-209 (an internal bug ledger entry): three independent sites each
// hard-coded a FIXED number of ".." hops from their own source/script
// location to reach the private coordination tree (a sibling directory
// carrying releases/CURRENT and claims/ — the release pointer, the engine
// pin, and the build-lock claims dir). Every hop count assumed the checkout
// sits exactly one level under the workspace directory. A worktree pool one
// level DEEPER made every fixed-hop site silently resolve to a
// plausible-but-wrong path (a sibling that doesn't exist) instead of failing.
//
// This header is the shared fix: WALK parent directories looking for a
// directory that is structurally the coordination tree, instead of counting
// hops. It is deliberately dependency-free (std::filesystem only) so it can
// be pulled in verbatim by both build trees in this monorepo (piet/native's
// standalone CMake project, and cobra's top-level one) via a plain relative
// #include, with no CMakeLists.txt changes on either side — see
// tests/unit/validate_dir.h, which #includes this same file by relative
// path for exactly that reason.
//
// A public checkout has no coordination tree anywhere above it; every
// caller treats "not found" as an honest environmental limit (skip), so this
// search is inert outside the maintainer's workspace.
//
// A silent wrong answer is the bug this closes, so every failure mode below
// is loud and names what it looked at. There is no "best guess" branch.

#include <filesystem>
#include <sstream>
#include <string>
#include <system_error>
#include <vector>

namespace piet {

// A directory qualifies as THE coordination tree purely by structure: it
// must contain a regular file "releases/CURRENT" (the release pointer) AND a
// directory "claims/" (the build-lock and task-claim dirs). No name test —
// the two markers together are carried only by the real coordination tree,
// so an unrelated directory (a source checkout, a scratch dir, a stray
// backup without both markers) cannot answer for it.
inline bool looksLikeCoordRoot(const std::filesystem::path& candidate) {
  std::error_code ec;
  if (!std::filesystem::is_regular_file(candidate / "releases" / "CURRENT", ec)) return false;
  ec.clear();
  return std::filesystem::is_directory(candidate / "claims", ec);
}

struct CoordRootResult {
  bool found = false;
  std::string path;                     // absolute path to the coordination tree; valid only when found
  std::vector<std::string> searched;    // every child directory actually probed, in probe order
  std::vector<std::string> candidates;  // qualifying matches found (>1 => ambiguous, see below)
  int levels = 0;                       // ancestor levels whose children were probed

  // Human-readable explanation for a failed search, naming exactly what was
  // looked at — the point of this helper is that "not there" is stated, not
  // guessed at. Two distinct failure shapes, told apart on purpose:
  //   - ambiguous (candidates.size() > 1): more than one directory beside an
  //     ancestor of the start point qualifies. Silently taking the nearest is
  //     exactly the "resolved to a plausible-but-wrong path" failure this
  //     helper exists to prevent, so this case refuses to pick and lists
  //     every candidate.
  //   - genuinely absent (candidates.empty()): no qualifying coordination
  //     tree exists within the search bound at all. This is the honest
  //     "there is no coordination tree above me" case — any standalone or
  //     public checkout — and callers should treat it as a real
  //     environmental limit (skip), not a bug.
  std::string explain() const {
    std::ostringstream ss;
    if (candidates.size() > 1) {
      ss << "ambiguous: " << candidates.size()
         << " candidate coordination trees (releases/CURRENT + claims/) found while walking "
            "upward — refusing to guess: ";
      for (size_t i = 0; i < candidates.size(); ++i) {
        if (i) ss << ", ";
        ss << candidates[i];
      }
    } else {
      ss << "no coordination tree (a directory carrying releases/CURRENT and claims/) found "
            "beside any of " << levels << " ancestor level(s) of the start directory — probed "
         << searched.size() << " child dir(s)";
      if (!searched.empty()) ss << ": ";
      // Every probe is recorded in `searched`; the message names the first
      // kMaxListed and counts the rest so a skip line stays one readable line.
      const size_t kMaxListed = 40;
      for (size_t i = 0; i < searched.size() && i < kMaxListed; ++i) {
        if (i) ss << ", ";
        ss << searched[i];
      }
      if (searched.size() > kMaxListed) ss << ", ... (+" << (searched.size() - kMaxListed) << " more)";
    }
    return ss.str();
  }
};

// Walks ancestors of `startDir`. At each ancestor level it probes EVERY
// immediate child directory of that ancestor with looksLikeCoordRoot() (the
// coordination tree sits BESIDE the source checkouts, as a sibling under the
// workspace directory — so the search checks each ancestor's children, not
// whether the ancestor itself qualifies). The child that is the walk's own
// path (the start checkout itself, or the ancestor just climbed out of) is
// skipped: it contains the start point, so it is never the sibling tree.
// Children are probed in directory_iterator order; non-directories and
// unreadable entries are skipped without aborting the walk.
//
// Bound: stops after `maxDepth` ancestor levels (default 10) or at the
// filesystem root, whichever comes first. A worktree-pool checkout sits two
// levels below the workspace directory, so 10 is generous slack, not a tight
// fit — the bound exists so a misconfigured or relocated checkout gets a
// named, immediate failure instead of wandering forever.
//
// Deliberately does NOT stop at the first match: it keeps walking to the
// bound so that a second, unexpected coordination tree further up (e.g. this
// checkout living inside a backup copy of the workspace) is DETECTED and
// reported as ambiguous instead of being silently shadowed by whichever one
// happened to be nearer. See CoordRootResult::explain().
inline CoordRootResult findCoordRoot(std::filesystem::path startDir, int maxDepth = 10) {
  namespace fs = std::filesystem;
  CoordRootResult result;
  std::error_code ec;
  fs::path dir = fs::absolute(startDir, ec);
  if (ec) dir = startDir;  // absolute() failing (rare) still leaves a usable relative walk
  dir = dir.lexically_normal();
  // "a/b/" normalises with an empty filename; climb to "a/b" so the first
  // ancestor probed is a/, not b/ itself.
  if (!dir.has_filename() && dir.has_parent_path() && dir.parent_path() != dir) dir = dir.parent_path();

  for (int depth = 0; depth <= maxDepth; ++depth) {
    fs::path parent = dir.parent_path();
    if (parent.empty() || parent == dir) break;  // reached filesystem root; nothing further up
    ++result.levels;

    ec.clear();
    fs::directory_iterator it(parent, fs::directory_options::skip_permission_denied, ec);
    if (!ec) {
      const fs::directory_iterator end;
      while (it != end) {
        const fs::path child = it->path();
        std::error_code cec;
        const bool isDir = it->is_directory(cec);
        if (!cec && isDir && child != dir) {
          result.searched.push_back(child.string());
          if (looksLikeCoordRoot(child)) {
            std::error_code wec;
            fs::path canon = fs::weakly_canonical(child, wec);
            result.candidates.push_back(wec ? child.string() : canon.string());
          }
        }
        it.increment(ec);
        if (ec) break;  // unreadable mid-listing: keep what was probed, move up a level
      }
    }
    dir = parent;
  }

  if (result.candidates.size() == 1) {
    result.found = true;
    result.path = result.candidates.front();
  }
  return result;
}

}  // namespace piet
