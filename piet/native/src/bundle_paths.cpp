// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
#include "piet/bundle_paths.h"
#include "io/win_compat.h"  // cecon::to_native_path (BUG-052 wide GetFileAttributesW)

#include <climits>
#include <cstdlib>
#include <vector>

#if defined(__APPLE__)
#include <mach-o/dyld.h>
#elif defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

namespace piet {

std::optional<std::string> executablePath() {
#if defined(__APPLE__)
  // _NSGetExecutablePath may return a path containing symlinks (e.g. if the
  // .app itself is symlinked); realpath() resolves it to the canonical
  // filesystem path so the ".app/Contents/MacOS" suffix check below is
  // reliable regardless of how the binary was launched (Finder double-click,
  // `open PIET.app`, or a direct absolute-path exec from a shell/script).
  uint32_t size = 0;
  _NSGetExecutablePath(nullptr, &size);  // first call: query required size
  if (size == 0) return std::nullopt;
  std::vector<char> buf(size);
  if (_NSGetExecutablePath(buf.data(), &size) != 0) return std::nullopt;

  char resolved[PATH_MAX];
  if (realpath(buf.data(), resolved) == nullptr) {
    // realpath failed (shouldn't happen for a running executable) — fall
    // back to the unresolved path rather than nullopt, still usable for the
    // suffix check in the common case (no symlinks involved).
    return std::string(buf.data());
  }
  return std::string(resolved);
#elif defined(_WIN32)
  // WP-W5 fold-in (Porting-Op flag): the Windows analogue of
  // _NSGetExecutablePath. GetModuleFileNameW(nullptr, ...) returns the
  // running executable's absolute path; UTF-16 -> UTF-8 narrowed here so the
  // rest of the codebase stays std::string like the mac branch.
  wchar_t wbuf[MAX_PATH];
  DWORD n = GetModuleFileNameW(nullptr, wbuf, MAX_PATH);
  if (n == 0 || n >= MAX_PATH) return std::nullopt;  // failed or truncated
  int len = WideCharToMultiByte(CP_UTF8, 0, wbuf, static_cast<int>(n),
                                nullptr, 0, nullptr, nullptr);
  if (len <= 0) return std::nullopt;
  std::string out(static_cast<size_t>(len), '\0');
  WideCharToMultiByte(CP_UTF8, 0, wbuf, static_cast<int>(n), &out[0], len,
                      nullptr, nullptr);
  return out;
#else
  // Linux bundle-path resolution is a Phase D (porting) concern —
  // M5 GA is macOS-only (ROADMAP §2.0.0 M5). Returning nullopt here makes
  // bundleResourcesDir() correctly report "not bundled" so callers fall
  // back to their dev-tree resolution on any non-Apple build.
  return std::nullopt;
#endif
}

std::optional<std::string> bundleResourcesDir() {
  std::optional<std::string> exe = executablePath();
  if (!exe.has_value()) return std::nullopt;

#if defined(_WIN32)
  // Windows packaged layout (Phase D packager, forthcoming): a `resources`
  // directory beside piet_native.exe — the flat analogue of the mac bundle's
  // .app/Contents/Resources. A dev-tree/build-dir invocation has no such
  // sibling directory, so this correctly reports "not bundled" there and
  // callers fall back to their dev-tree resolution, same contract as mac.
  auto lastSep = exe->find_last_of("/\\");
  if (lastSep == std::string::npos) return std::nullopt;
  std::string resources = exe->substr(0, lastSep) + "\\resources";
  DWORD attrs = GetFileAttributesW(cecon::to_native_path(resources).c_str());
  if (attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY)) {
    return resources;
  }
  return std::nullopt;
#else
  const std::string suffix = ".app/Contents/MacOS";
  // exe is ".../Foo.app/Contents/MacOS/piet_native" — find the directory
  // component (strip the final "/piet_native") and check IT ends in
  // ".app/Contents/MacOS".
  auto lastSlash = exe->find_last_of('/');
  if (lastSlash == std::string::npos) return std::nullopt;
  std::string dir = exe->substr(0, lastSlash);

  if (dir.size() < suffix.size()) return std::nullopt;
  if (dir.compare(dir.size() - suffix.size(), suffix.size(), suffix) != 0) return std::nullopt;

  // ".../Foo.app/Contents/MacOS" -> ".../Foo.app/Contents/Resources"
  std::string contentsDir = dir.substr(0, dir.size() - std::string("/MacOS").size());
  return contentsDir + "/Resources";
#endif
}

}  // namespace piet
