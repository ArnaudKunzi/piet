// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// Shared fixture-path resolution for the unit tests.
//
// The validate/ goldens used to be reached through a hardcoded absolute macOS
// path, which made every one of these tests machine-specific. Resolution order
// is now:
//
//   1. the COBRA_VALIDATE_DIR environment variable, if set (runtime override —
//      point it at a checkout / share that actually holds the fixtures);
//   2. the COBRA_VALIDATE_DIR compile-time define, injected by CMake as
//      "${CMAKE_SOURCE_DIR}/tests/validate";
//   3. "tests/validate" relative to the current working directory, as a
//      last-ditch fallback for hand-built binaries.
//
// BUG-209 rider (an internal bug ledger entry): the *.dta fixtures under
// tests/validate are gitignored generated artifacts (privacy — see
// .gitignore's DATA section), so a fresh checkout of any of the above three
// has an empty/incomplete tests/validate/ even though the path resolution
// itself is correct. That is NOT the same failure as the other BUG-209 sites
// (a hard-coded ".." hop landing on the wrong directory) — the path is
// right, the files are just genuinely absent there. What IS the same failure
// class: silently treating that as unrecoverable when a perfectly good copy
// of the same fixtures exists one upward search away, in the maintainer's
// primary checkout. So validate_path() below adds a fourth step, tried only
// when steps 1-3 resolve to a path that turns out not to have the specific
// file:
//   a. search upward for the private coordination tree (a sibling directory
//      carrying releases/CURRENT and claims/) with piet::findCoordRoot — the
//      same helper the other sites use, included by relative path below
//      since tests/unit/ and piet/native/ are separate CMake projects with
//      no shared include directory;
//   b. take the directory that CONTAINS that tree (the workspace) and probe
//      each of its child directories for tests/validate/<subdir><file>;
//   c. exactly one child has the file -> use it. More than one -> keep only
//      the PRIMARY checkout(s): a child whose .git is a directory with a
//      worktrees/ subdirectory (the checkout the linked worktrees hang off;
//      linked worktrees have a .git FILE, plain clones have no worktrees/).
//      Still more than one, or none -> refuse loudly (SKIP naming every
//      candidate), the same no-guessing policy findCoordRoot applies.
// On a public checkout there is no coordination tree, so step (a) reports
// "not found" and the test SKIPs honestly. This fallback is skipped entirely
// when COBRA_VALIDATE_DIR was set explicitly (step 1): an explicit override
// means "look exactly here", and silently substituting a different directory
// would defeat the override.
//
// Usage in a test:
//     const cobra_test::ValidateBase base;                 // <validate>/
//     const cobra_test::ValidateBase base{"dta_fixtures/"}; // <validate>/dta_fixtures/
//     Dataset ds = Dataset::open(base + "foo.hbin");
//
// `base + name` resolves the path and, when the fixture is absent even after
// the upward-search fallback, prints a one-line note naming every path that
// was tried so the reader knows what to set. The path is still returned
// unchanged (best available guess), so the caller's own error handling (and
// every assertion) is untouched.
#ifndef COBRA_TESTS_UNIT_VALIDATE_DIR_H
#define COBRA_TESTS_UNIT_VALIDATE_DIR_H

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <set>
#include <string>

#include <system_error>
#include <vector>

#include "../../piet/native/include/piet/coord_root.h"  // BUG-209 shared upward search

namespace cobra_test {

// Root of the validate fixture directory, always with a trailing '/'.
// Forward slashes work fine on Windows too.
inline std::string validate_dir() {
    std::string d;
    if (const char* env = std::getenv("COBRA_VALIDATE_DIR")) {
        if (*env) d = env;
    }
#ifdef COBRA_VALIDATE_DIR
    if (d.empty()) d = COBRA_VALIDATE_DIR;
#endif
    if (d.empty()) d = "tests/validate";
    if (!d.empty() && d.back() != '/' && d.back() != '\\') d += '/';
    return d;
}

// Step 4(b)-(c) of the file header: among the workspace's child directories,
// find the single checkout whose tests/validate/ holds `rel`. Returns the
// resolved file path, or "" with `why` naming every candidate / the miss.
inline std::string workspace_fixture(const std::filesystem::path& workspace, const std::string& rel,
                                     std::string& why) {
    namespace fs = std::filesystem;
    std::vector<fs::path> hitChild, hitFile;  // parallel: checkout dir, resolved fixture path
    std::error_code ec;
    fs::directory_iterator it(workspace, fs::directory_options::skip_permission_denied, ec);
    if (ec) {
        why = "cannot list workspace " + workspace.string() + " (" + ec.message() + ")";
        return std::string();
    }
    const fs::directory_iterator end;
    while (it != end) {
        std::error_code cec;
        const bool isDir = it->is_directory(cec);
        if (!cec && isDir) {
            const fs::path file = it->path() / "tests" / "validate" / rel;
            std::ifstream p(file.string().c_str(), std::ios::binary);
            if (p.good()) {
                hitChild.push_back(it->path());
                hitFile.push_back(file);
            }
        }
        it.increment(ec);
        if (ec) break;  // unreadable mid-listing: decide on what was probed
    }
    if (hitFile.size() == 1) return hitFile.front().string();
    if (hitFile.size() > 1) {
        std::vector<fs::path> primary;
        for (size_t i = 0; i < hitChild.size(); ++i) {
            std::error_code gec;
            if (fs::is_directory(hitChild[i] / ".git" / "worktrees", gec)) primary.push_back(hitFile[i]);
        }
        if (primary.size() == 1) return primary.front().string();
        why = "ambiguous: " + std::to_string(hitFile.size()) + " checkouts under " + workspace.string() +
              " carry tests/validate/" + rel + " and " + std::to_string(primary.size()) +
              " of them are primary checkouts (.git/worktrees/) — refusing to guess:";
        for (const fs::path& h : hitFile) why += " " + h.string();
        return std::string();
    }
    why = "no child of " + workspace.string() + " carries tests/validate/" + rel;
    return std::string();
}

// Resolve `name` under the validate directory (optionally under `subdir`).
// Emits a hint on stderr the first time a resolved path turns out to be missing.
// See the file header (BUG-209 rider) for the upward-search fallback below.
inline std::string validate_path(const std::string& name, const std::string& subdir = std::string()) {
    const bool explicitOverride = [] {
        const char* env = std::getenv("COBRA_VALIDATE_DIR");
        return env && *env;
    }();

    const std::string dir = validate_dir() + subdir;
    const std::string path = dir + name;
    std::ifstream probe(path.c_str(), std::ios::binary);
    if (probe.good()) return path;

    // Primary resolution missed. If the caller explicitly pointed us at a
    // directory (COBRA_VALIDATE_DIR set), that is a deliberate "look exactly
    // here" — do not silently substitute a different one.
    std::string fallbackPath;
    std::string fallbackNote;
    if (!explicitOverride) {
        piet::CoordRootResult coord = piet::findCoordRoot(dir);
        if (coord.found) {
            const std::filesystem::path workspace = std::filesystem::path(coord.path).parent_path();
            std::string why;
            fallbackPath = workspace_fixture(workspace, subdir + name, why);
            if (!fallbackPath.empty()) return fallbackPath;
            fallbackNote = "    also tried the workspace beside the coordination tree (" + coord.path +
                           "): " + why + "\n";
        } else {
            fallbackNote = "    upward search for the coordination tree (releases/CURRENT + claims/) "
                           "to try the primary checkout also failed: " + coord.explain() + "\n";
        }
    }

    // GA sweep 2026-08-29 (leg 5). This used to WARN and then hand back a path
    // that does not exist, so the caller opened it and died on an uncaught
    // EngineError further down — which ctest reports as a FAILURE. On a fresh
    // clone that is four tests claiming defects they never measured (the DATA
    // fixtures are deliberately not committed; .gitignore says so).
    //
    // A gate that cannot run must say it cannot run. Exit 77 is ctest's
    // SKIP_RETURN_CODE, declared for these tests in CMakeLists.txt — the same
    // contract tests/validate/_fixtures.py gives the python gates, and the same
    // rule BUG-362 established for the eval harness: never report a verdict you
    // did not measure.
    std::fprintf(stderr,
                 "SKIP: fixture not present: %s\n"
                 "      resolved under %s\n"
                 "%s"
                 "      (data fixtures are not committed — see .gitignore; set "
                 "COBRA_VALIDATE_DIR to a directory that has them)\n",
                 path.c_str(), dir.c_str(), fallbackNote.c_str());
    std::exit(77);
}

// GA sweep 2026-08-29 (leg 5). Fifteen unit tests carry their OWN copy of a
// wave_fixture() helper that resolves a pangram artifact outside validate/, so
// they never pass through validate_path() and never got its honest skip — they
// just failed to read the file and reported FAIL. Same fixture class, same rule,
// one shared door: call this instead of returning a path nobody checked.
inline std::string require_fixture(const std::string& path, const char* what = nullptr) {
    std::ifstream probe(path.c_str(), std::ios::binary);
    if (probe.good()) return path;
    std::fprintf(stderr,
                 "SKIP: fixture not present: %s\n"
                 "      (data fixtures are not committed — see .gitignore; set "
                 "COBRA_VALIDATE_DIR or COBRA_PANGRAM_ARTIFACTS to a directory that has them)\n",
                 what ? what : path.c_str());
    std::exit(77);
}

// Drop-in replacement for the old `const std::string base = "<abs path>/";`.
// `base + "file.csv"` yields the resolved path (and warns if it is missing).
struct ValidateBase {
    std::string subdir;

    ValidateBase() = default;
    explicit ValidateBase(std::string sub) : subdir(std::move(sub)) {}

    std::string operator+(const std::string& name) const { return validate_path(name, subdir); }
    std::string str() const { return validate_dir() + subdir; }
    operator std::string() const { return str(); }
};

}  // namespace cobra_test

#endif  // COBRA_TESTS_UNIT_VALIDATE_DIR_H
