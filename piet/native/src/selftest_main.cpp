// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// R1 (selftest split, 2026-09-01): the 24,069-line suite used to be linked into
// the PRODUCT binary — 3.8 MB of test code in every user's app. It now builds
// as this separate executable over the same piet_core objects, so the suite
// still exercises the exact code the product ships. `piet_native --selftest`
// exec()s this binary from its own directory, keeping every documented
// invocation and every gate script working unchanged.
#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

#if !defined(_WIN32)
#include <unistd.h>
#endif

int runSelftest();  // selftest.cpp (external linkage, deliberately un-namespaced)

int main() {
#if !defined(_WIN32)
  // BUG-1068: the legs name their workdirs under temp_directory_path() with FIXED
  // names (piet_native_selftest_m4b, ..._getcols, ...), so two selftests that
  // share a TMPDIR -- the merge gate's leg next to a lane's, a rerun overlapping
  // a run -- write, clean and delete each other's scratch mid-run. Measured:
  // two concurrent runs over one TMPDIR failed 47 and 30 checks that pass alone.
  // Every such name now lands in a directory this process alone owns.
  const char* inherited = std::getenv("TMPDIR");
  std::string base = inherited != nullptr && *inherited != '\0' ? inherited : "/tmp";
  while (base.size() > 1 && base.back() == '/') base.pop_back();
  std::string tmpl = base + "/piet_selftest_XXXXXX";
  std::vector<char> buf(tmpl.begin(), tmpl.end());
  buf.push_back('\0');
  std::string own;
  if (::mkdtemp(buf.data()) != nullptr) {
    own = buf.data();
    ::setenv("TMPDIR", own.c_str(), 1);
  }
  const int rc = runSelftest();
  if (!own.empty() && std::getenv("PIET_KEEP_SCRATCH") == nullptr) {
    std::error_code ec;
    std::filesystem::remove_all(own, ec);
  }
  return rc;
#else
  return runSelftest();
#endif
}
