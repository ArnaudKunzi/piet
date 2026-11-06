// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// Bundle-vs-dev path resolution — M5 GA (ROADMAP §2.0.0 "M5 GA": the
// no-Python PIET 2.0.0 .app). Two call sites need "am I running out of a
// packaged .app, and if so where are my Resources?": main.cpp (which
// frontend tree to load — the real piet/frontend/ dev checkout vs. a
// bundled Resources/frontend/ copy) and cobra_runner.cpp (which cobra
// binary to exec — the Art 7 dev pin vs. a bundled Resources/cobra copy).
//
// A macOS app bundle built by the CMake `bundle_app` target (see
// CMakeLists.txt) has this layout:
//   PIET.app/Contents/MacOS/piet_native      <- the running executable
//   PIET.app/Contents/Resources/frontend/    <- piet/frontend/, copied in
//   PIET.app/Contents/Resources/cobra        <- the pinned cobra binary
//   PIET.app/Contents/Resources/CoBrA.icns
//   PIET.app/Contents/Info.plist
//
// Detection is purely path-shape based (no env var, no build-time flag):
// resolve the running executable's real path via _NSGetExecutablePath
// (macOS) and check whether its containing directory ends in
// ".app/Contents/MacOS". This means a bundle-built binary run from ANY
// location (double-clicked, `open`'d, or invoked by an absolute path in a
// headless acceptance script) resolves its Resources correctly — exactly
// the property the M5 acceptance gate needs (run the PACKAGED binary
// headlessly via --run-block/do-exec and have it still find its bundled
// cobra + frontend, no dev-tree fallback silently masking a packaging bug).
#ifndef PIET_NATIVE_BUNDLE_PATHS_H
#define PIET_NATIVE_BUNDLE_PATHS_H

#include <optional>
#include <string>

namespace piet {

// Returns the running executable's own absolute, symlink-resolved path, or
// std::nullopt if the platform API failed (should not happen on macOS).
std::optional<std::string> executablePath();

// If the running executable lives at .../Foo.app/Contents/MacOS/exe, returns
// the ".../Foo.app/Contents/Resources" directory (WITHOUT checking that it
// exists — callers combine this with the specific file/dir they need and
// check that). Returns std::nullopt when not running from inside a
// .app/Contents/MacOS directory (dev build, `cmake --build` binary run
// in-place, --run-block headless harness invocation via a raw build/
// path, non-macOS platform).
std::optional<std::string> bundleResourcesDir();

}  // namespace piet

#endif  // PIET_NATIVE_BUNDLE_PATHS_H
