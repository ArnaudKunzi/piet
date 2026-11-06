// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
#pragma once
#include <cstdio>
#include <ctime>
#include <mutex>
#include <string>
#include <unordered_set>

namespace piet {

// BUG-051 (Windows-only). Per-command scratch .hbin names are built from
// time()*1000 (SECOND granularity), so two mutations in the same wall-clock
// second produce the SAME filename. On Windows the previous same-named hbin is
// still memory-mapped by piet's session Datastore (cecon::Dataset::open ->
// MapViewOfFile, datastore.cpp), and a mapped file cannot be overwritten — the
// 2nd `gen` fails "cannot write ...\\gen_<ms>.hbin". POSIX permits rewriting a
// mapped file, which is why the same code works on macOS/Linux.
//
// This returns a per-process monotonic suffix so every scratch path is unique
// and a mutation never targets a file another handle still maps. Dispatch is
// single-threaded (one command at a time), so a plain static counter is
// sufficient (no atomics).
//
// MAXPERF Stage 1b (contract_A §26 resident-frame serve session): the counter
// is now emitted on EVERY platform, not just Windows. Previously a no-op on
// POSIX (time()*1000 is second-granular, so two mutations in one wall-clock
// second shared a filename — harmless one-shot because each file was consumed
// synchronously before reuse). Under the resident session the scratch path is
// the VIRTUAL chain-head TOKEN: two same-second mutations aliasing one path
// makes the second's result indistinguishable from the first's, so a later
// read frame-uses the STALE materialized file and loses the newer column
// (observed: `gen ybin` + `gen cst` in one second → `logit` sees no `cst`).
// The literal name is never inspected by any caller (BS-092 analysis), and
// command OUTPUT never contains a scratch path, so making POSIX unique too is
// byte-invisible to every gate (freeze/selftest/goldens) — it only removes the
// collision hazard.
// BUG-389 (2026-09-02): the mint counter, readable. SessionState::sweepOrphanScratch
// runs only when a mint happened since its last pass, so a top-level command that
// minted nothing pays no directory walk.
inline unsigned long long& scratchMintSeq() {
  static unsigned long long seq = 0;
  return seq;
}
inline std::string scratchSeqSuffix() {
  return "_" + std::to_string(scratchMintSeq()++);
}

// BUG-389 (2026-09-02): the mint REGISTRY. Every scratch path this process mints
// is recorded here at mint time, and SessionState::sweepOrphanScratch removes,
// at each outermost dispatch exit, exactly the registered paths no ledger owns.
// A file the registry does not know — a user's own `save` into the workdir, a
// file another process wrote — is never touched, whatever it is named. Process-
// global on purpose: one session per process (`--do`, the GUI), and a parallel
// worker is its own process with its own registry.
inline std::unordered_set<std::string>& scratchMintRegistry() {
  static std::unordered_set<std::string> reg;
  return reg;
}
inline std::mutex& scratchMintRegistryMutex() {
  static std::mutex mu;
  return mu;
}
inline void registerScratchMint(const std::string& path) {
  if (path.empty()) return;
  std::lock_guard<std::mutex> lk(scratchMintRegistryMutex());
  scratchMintRegistry().insert(path);
}
inline void forgetScratchMint(const std::string& path) {
  std::lock_guard<std::mutex> lk(scratchMintRegistryMutex());
  scratchMintRegistry().erase(path);
}
inline bool isRegisteredScratchMint(const std::string& path) {
  std::lock_guard<std::mutex> lk(scratchMintRegistryMutex());
  return scratchMintRegistry().count(path) > 0;
}
// The one body behind every `nextPath(workdir, prefix)` in the tree (they used to
// be five verbatim copies). Registers the path it mints.
inline std::string mintScratchPath(const std::string& workdir, const std::string& prefix) {
  auto ms = static_cast<long long>(std::time(nullptr)) * 1000;
  char buf[64];
  std::snprintf(buf, sizeof(buf), "%lld", ms);
  std::string path = workdir + "/" + prefix + "_" + buf + scratchSeqSuffix() + ".hbin";
  registerScratchMint(path);
  return path;
}

}  // namespace piet
