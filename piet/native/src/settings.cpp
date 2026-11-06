// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
#include "piet/settings.h"

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <thread>

#include "piet/cobra_session.h"  // Stage 1b: bump the worker-respawn epoch on a thread-count change
#include "piet/routes.h"

#if defined(__APPLE__)
#include <sys/sysctl.h>
#include <sys/types.h>
#elif defined(_WIN32)
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace piet::settings {

namespace {

[[maybe_unused]] std::string envOr(const char* name, const std::string& fallback) {
  const char* v = std::getenv(name);
  return (v && *v) ? std::string(v) : fallback;
}

// A UTF-8 settings path -> an fs::path that opens correctly per platform. On
// Windows settingsFilePath() returns UTF-8 (it reads %APPDATA% via the wide env
// API), so the string must go through u8path for the wide CreateFile to see
// real Unicode; on POSIX the byte string already IS the native path (behaviour
// unchanged from the historical fs::path(str) / narrow-stream open).
std::filesystem::path settingsNative(const std::string& p) {
#ifdef _WIN32
  return std::filesystem::u8path(p);
#else
  return std::filesystem::path(p);
#endif
}

}  // namespace

std::string settingsFilePath() {
#ifdef _WIN32
  // Windows: resolve the override / %APPDATA% through the WIDE env API and
  // return UTF-8. A narrow getenv yields ANSI (CP949 on a Korean box) bytes,
  // while MinGW's std::filesystem decodes narrow paths as UTF-8 — so
  // fs::path(getenv("APPDATA")) on a Hangul username (…\홍길동\AppData\Roaming)
  // feeds CP949 into a UTF-8 decoder and throws std::filesystem_error "Illegal
  // byte sequence": at minimum settings never persist, and uncaught it
  // terminates the process. Same wide->UTF-8 discipline makeWorkdir uses;
  // load()/save() then open via settingsNative() (u8path -> wide CreateFile).
  auto wenv = [](const wchar_t* k) -> std::wstring {
    DWORD n = GetEnvironmentVariableW(k, nullptr, 0);
    if (n == 0) return std::wstring();
    std::wstring v(n, L'\0');
    v.resize(GetEnvironmentVariableW(k, &v[0], n));
    return v;
  };
  auto w2u = [](const std::wstring& w) -> std::string {
    if (w.empty()) return std::string();
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), &s[0], n, nullptr, nullptr);
    return s;
  };
  // Test/gate isolation hook — a selftest or --ui-smoke run points this at a
  // throwaway file so it never touches the real user config.
  if (std::wstring over = wenv(L"PIET_SETTINGS_FILE"); !over.empty()) {
    return w2u(over);
  }
  std::wstring appdata = wenv(L"APPDATA");
  std::string base = (appdata.empty() ? std::string(".") : w2u(appdata)) + "\\PIET";
  return (std::filesystem::u8path(base) / "settings.json").u8string();
#else
  // Test/gate isolation hook — a selftest or --ui-smoke run points this at a
  // throwaway file so it never touches the real user config.
  if (const char* over = std::getenv("PIET_SETTINGS_FILE"); over && *over) {
    return over;
  }
  namespace fs = std::filesystem;
#if defined(__APPLE__)
  std::string base = envOr("HOME", ".") + "/Library/Application Support/PIET";
#else
  std::string xdg = envOr("XDG_CONFIG_HOME", "");
  std::string base = !xdg.empty() ? xdg + "/PIET" : envOr("HOME", ".") + "/.config/PIET";
#endif
  return (fs::path(base) / "settings.json").string();
#endif
}

SysInfo detectSysInfo() {
  SysInfo info;

  // CPU count — one portable source (respects a constrained sandbox, same as
  // src/core/threads.cpp's fallback reasoning).
  unsigned hc = std::thread::hardware_concurrency();
  info.cpuCount = hc > 0 ? static_cast<int>(hc) : 1;

  // Total physical RAM — per-platform (SPEC II-1; the Python-era psutil is
  // gone).
  int64_t ramBytes = 0;
#if defined(__APPLE__)
  int64_t memsize = 0;
  size_t len = sizeof(memsize);
  if (sysctlbyname("hw.memsize", &memsize, &len, nullptr, 0) == 0) ramBytes = memsize;
#elif defined(_WIN32)
  MEMORYSTATUSEX ms;
  ms.dwLength = sizeof(ms);
  if (GlobalMemoryStatusEx(&ms)) ramBytes = static_cast<int64_t>(ms.ullTotalPhys);
#else
  long pages = sysconf(_SC_PHYS_PAGES);
  long pageSize = sysconf(_SC_PAGE_SIZE);
  if (pages > 0 && pageSize > 0) ramBytes = static_cast<int64_t>(pages) * pageSize;
#endif
  info.totalRamMb = ramBytes / (1024 * 1024);
  // SPEC II-2: half of total RAM — a safe working-set ceiling on an 8 GB
  // machine (-> 4096 MB). Never negative / zero-div; floor at 256 MB.
  info.recommendedBudgetMb = info.totalRamMb > 0 ? info.totalRamMb / 2 : 2048;
  if (info.recommendedBudgetMb < 256) info.recommendedBudgetMb = 256;
  return info;
}

Settings load() {
  Settings s;
  std::ifstream f(settingsNative(settingsFilePath()), std::ios::binary);
  if (!f) return s;
  std::ostringstream ss;
  ss << f.rdbuf();
  try {
    auto j = nlohmann::json::parse(ss.str());
    if (j.is_object()) {
      if (j.contains("scratch_path") && j["scratch_path"].is_string())
        s.scratchPath = j["scratch_path"].get<std::string>();
      if (j.contains("default_working_dir") && j["default_working_dir"].is_string())
        s.defaultWorkingDir = j["default_working_dir"].get<std::string>();
      if (j.contains("memory_budget_mb") && j["memory_budget_mb"].is_number_integer())
        s.memoryBudgetMb = j["memory_budget_mb"].get<int64_t>();
      if (j.contains("compute_cores") && j["compute_cores"].is_number_integer())
        s.computeCores = j["compute_cores"].get<int>();
      if (j.contains("provenance_hash") && j["provenance_hash"].is_boolean())
        s.provenanceHash = j["provenance_hash"].get<bool>();
      // N1/N2 (P8.1-W2): additive, absent in older stores (defaults to "").
      if (j.contains("default_export_dir") && j["default_export_dir"].is_string())
        s.defaultExportDir = j["default_export_dir"].get<std::string>();
      if (j.contains("ui_state") && j["ui_state"].is_string())
        s.uiState = j["ui_state"].get<std::string>();
      // WS-B (extcompat): additive, absent in every store written before
      // this build. Absent -> the struct default (true / "") stands, which
      // is what makes a headless run with no settings file behave exactly
      // as it did before the setting existed.
      if (j.contains("extcompat") && j["extcompat"].is_boolean())
        s.extcompat = j["extcompat"].get<bool>();
      if (j.contains("extcompat_consent_at") && j["extcompat_consent_at"].is_string())
        s.extcompatConsentAt = j["extcompat_consent_at"].get<std::string>();
      if (j.contains("follow_do_file_folder") && j["follow_do_file_folder"].is_boolean())
        s.followDoFileFolder = j["follow_do_file_folder"].get<bool>();
    }
  } catch (const nlohmann::json::exception&) {
    // Malformed file -> defaults (never throw into a launch/route path).
    return Settings{};
  }
  return s;
}

bool save(const Settings& s) {
  std::string path = settingsFilePath();
  std::error_code ec;
  std::filesystem::create_directories(settingsNative(path).parent_path(), ec);
  nlohmann::ordered_json j;
  j["scratch_path"] = s.scratchPath;
  j["default_working_dir"] = s.defaultWorkingDir;
  j["memory_budget_mb"] = s.memoryBudgetMb;
  j["compute_cores"] = s.computeCores;
  j["provenance_hash"] = s.provenanceHash;
  j["default_export_dir"] = s.defaultExportDir;  // N1 (P8.1-W2)
  j["ui_state"] = s.uiState;                      // N2 (P8.1-W2)
  j["extcompat"] = s.extcompat;                    // WS-B
  j["extcompat_consent_at"] = s.extcompatConsentAt;  // WS-B
  j["follow_do_file_folder"] = s.followDoFileFolder;
  std::ofstream f(settingsNative(path), std::ios::binary | std::ios::trunc);
  if (!f) return false;
  f << j.dump(2) << "\n";
  return static_cast<bool>(f);
}

namespace {

// See settings.h: a variable PIET exports is shadowed by PIET_OWNS_<name> so a
// later Auto can tell "mine" from "a pin someone else placed".
std::string ownerMarker(const char* name) { return std::string("PIET_OWNS_") + name; }

void setEnv(const std::string& name, const std::string& value) {
#ifndef _WIN32
  setenv(name.c_str(), value.c_str(), 1);
#else
  _putenv_s(name.c_str(), value.c_str());
#endif
}

void unsetEnv(const std::string& name) {
#ifndef _WIN32
  unsetenv(name.c_str());
#else
  _putenv_s(name.c_str(), "");
#endif
}

void setEnvOwned(const char* name, const std::string& value) {
  setEnv(name, value);
  setEnv(ownerMarker(name), value);
}

// True when `name` is set AND PIET did not set it (no shadow, or a shadow that
// disagrees — e.g. a parent `parallel` block's per-worker share overwrote it).
bool envIsForeignPin(const char* name) {
  const char* cur = std::getenv(name);
  if (!cur) return false;
  const std::string marker = ownerMarker(name);
  const char* own = std::getenv(marker.c_str());
  return !(own && std::string(own) == cur);
}

// Release `name` if PIET set it; leave a foreign pin exactly as found. The
// shadow is dropped either way so a stale shadow can never claim a later pin.
void releaseEnvOwned(const char* name) {
  if (std::getenv(name) && !envIsForeignPin(name)) unsetEnv(name);
  unsetEnv(ownerMarker(name));
}

}  // namespace

void applyComputeCores(int cores) {
  if (cores > 0) {
    // Inherited by every subsequent cobra fork/exec. cobra's ThreadPool reads
    // CECON_NUM_THREADS (src/core/threads.cpp); Accelerate's BLAS reads
    // VECLIB_MAXIMUM_THREADS at library load, which is why it has to be in the
    // environment BEFORE the engine starts rather than set from inside it (the
    // Linux OpenBLAS arm re-execs for the same reason, apps/cecon_cli.cpp).
    setEnvOwned("CECON_NUM_THREADS", std::to_string(cores));
    setEnvOwned("VECLIB_MAXIMUM_THREADS", std::to_string(cores));
  } else {
    // Auto: the engine sizes itself from the hardware again — unless someone
    // other than PIET pinned it (BUG-386 fix; the foreign-pin rule is what keeps
    // a gate's or a parent worker's pin intact).
    releaseEnvOwned("CECON_NUM_THREADS");
    releaseEnvOwned("VECLIB_MAXIMUM_THREADS");
  }

  // Stage 1b: cobra reads its environment only at process start, so a live
  // `cobra serve` worker spawned under the old value must be respawned. Bump
  // the process-global epoch; each CobraSession compares it before its next
  // request and respawns (re-reading the env) + resyncs. Harmless at startup
  // (no worker spawned yet). See piet::CobraSession::epochStale().
  piet::bumpEngineSettingsEpoch();
}

void applyMemoryBudget(int64_t memoryBudgetMb) {
  if (memoryBudgetMb > 0) {
    setEnvOwned("CECON_MEMORY_BUDGET_MB", std::to_string(memoryBudgetMb));
  } else if (!envIsForeignPin("CECON_MEMORY_BUDGET_MB")) {
    // Auto = the recommended budget (SPEC II-2, half of RAM). The engine treats
    // an undeclared budget as unbounded, so "auto" must still name a number —
    // otherwise the memory setting would govern nothing engine-side until the
    // user moved the slider once.
    int64_t mb = detectSysInfo().recommendedBudgetMb;
    if (mb <= 0) mb = 2048;
    setEnvOwned("CECON_MEMORY_BUDGET_MB", std::to_string(mb));
  }
  piet::bumpEngineSettingsEpoch();
}

void applyEngineBudget(const Settings& s) {
  applyComputeCores(s.computeCores);
  applyMemoryBudget(s.memoryBudgetMb);
}

// ── WS-B (extcompat, 2026-08-06) ────────────────────────────────────────────

bool testIsolationActive() {
#ifdef _WIN32
  return GetEnvironmentVariableW(L"PIET_SETTINGS_FILE", nullptr, 0) != 0;
#else
  const char* over = std::getenv("PIET_SETTINGS_FILE");
  return over != nullptr && *over != '\0';
#endif
}

namespace {
// -1 = not resolved yet, 0 = off, 1 = on. Atomic because the dispatch gate
// reads it from the do-run thread while the bridge routes (settings.set) can
// write it from the UI thread; a torn read here would be a wrong answer, not
// just a stale one. Relaxed ordering is enough — this guards a single bool,
// not a publication of other state.
std::atomic<int> g_extcompat{-1};
}  // namespace

bool extcompatEnabled() {
  int v = g_extcompat.load(std::memory_order_relaxed);
  if (v < 0) {
    v = load().extcompat ? 1 : 0;
    g_extcompat.store(v, std::memory_order_relaxed);
  }
  return v != 0;
}

void setExtcompatEnabled(bool on) {
  g_extcompat.store(on ? 1 : 0, std::memory_order_relaxed);
}

void invalidateExtcompatCache() {
  g_extcompat.store(-1, std::memory_order_relaxed);
}

// ── Component packs (WS-B step 6 — structure only; docs/PACKS.md) ───────────

std::string packsDir() {
  // Sibling of settings.json, whatever resolved that path (including the
  // $PIET_SETTINGS_FILE isolation hook — a gate run therefore scans its own
  // throwaway directory and can never see a real user's packs).
  return (settingsNative(settingsFilePath()).parent_path() / "packs").u8string();
}

std::vector<std::string> enumeratePacks() {
  std::vector<std::string> ids;
  std::error_code ec;
  std::filesystem::path root = settingsNative(packsDir());
  if (!std::filesystem::is_directory(root, ec)) return ids;   // the normal case
  for (const auto& entry : std::filesystem::directory_iterator(root, ec)) {
    if (ec) break;
    if (!entry.is_directory(ec)) continue;
    // The presence of a manifest is the ONLY thing checked. Its contents are
    // deliberately not parsed here: enumerating is a listing, and anything
    // that reads a pack's declarations is the loader that this release does
    // not have (docs/PACKS.md §"Not implemented").
    if (!std::filesystem::exists(entry.path() / "manifest.json", ec)) continue;
    ids.push_back(entry.path().filename().u8string());
  }
  std::sort(ids.begin(), ids.end());   // stable order for anything that diffs it
  return ids;
}

nlohmann::ordered_json toGetJson(const Settings& s, const SysInfo& sys,
                                 const std::string& file) {
  nlohmann::ordered_json j;
  j["scratch_path"] = s.scratchPath;
  j["default_working_dir"] = s.defaultWorkingDir;
  j["memory_budget_mb"] = s.memoryBudgetMb;
  j["compute_cores"] = s.computeCores;
  j["provenance_hash"] = s.provenanceHash;
  j["default_export_dir"] = s.defaultExportDir;  // N1 (P8.1-W2)
  j["ui_state"] = s.uiState;                      // N2 (P8.1-W2): returned verbatim
  // WS-B: the persisted pair, plus the ONE derived answer the frontend needs.
  // `consent_required` is computed here rather than in JS so the test-mode
  // auto-accept lives at a single native site: under gate/test isolation the
  // answer is always false, which is why the first-run dialog can never stall
  // --ui-smoke or any headless harness.
  j["extcompat"] = s.extcompat;
  j["extcompat_consent_at"] = s.extcompatConsentAt;
  j["consent_required"] = s.extcompatConsentAt.empty() && !testIsolationActive();
  j["follow_do_file_folder"] = s.followDoFileFolder;
  // WS-B step 6: the boot-time pack scan. settings.get is the one call the
  // frontend already makes at boot (hydrateUiState), so the enumeration rides
  // it rather than inventing a route for a directory listing. Enumerate-only:
  // see enumeratePacks() and docs/PACKS.md. Empty on every machine today.
  j["packs"] = enumeratePacks();
  nlohmann::ordered_json sj;
  sj["total_ram_mb"] = sys.totalRamMb;
  sj["cpu_count"] = sys.cpuCount;
  sj["recommended_budget_mb"] = sys.recommendedBudgetMb;
  j["sys"] = sj;
  j["settings_file"] = file;
  return j;
}

}  // namespace piet::settings

// ── Bridge routes (app-shell family; declared in routes.h) ──────────────────
namespace piet::routes {

BridgeResult settingsGet(const std::string& /*body*/) {
  auto s = piet::settings::load();
  auto sys = piet::settings::detectSysInfo();
  return BridgeResult::success(
      piet::settings::toGetJson(s, sys, piet::settings::settingsFilePath()).dump());
}

BridgeResult settingsSet(const std::string& body) {
  // Partial merge onto the persisted settings (the Apply button sends the full
  // engine/scratch group, but a partial body is honoured too).
  piet::settings::Settings s = piet::settings::load();
  try {
    auto j = nlohmann::json::parse(body.empty() ? "{}" : body);
    if (j.is_object()) {
      if (j.contains("scratch_path") && j["scratch_path"].is_string())
        s.scratchPath = j["scratch_path"].get<std::string>();
      if (j.contains("default_working_dir") && j["default_working_dir"].is_string())
        s.defaultWorkingDir = j["default_working_dir"].get<std::string>();
      if (j.contains("memory_budget_mb") && j["memory_budget_mb"].is_number())
        s.memoryBudgetMb = j["memory_budget_mb"].get<int64_t>();
      if (j.contains("compute_cores") && j["compute_cores"].is_number())
        s.computeCores = j["compute_cores"].get<int>();
      if (j.contains("provenance_hash") && j["provenance_hash"].is_boolean())
        s.provenanceHash = j["provenance_hash"].get<bool>();
      // N1 (P8.1-W2): additive default-export-folder.
      if (j.contains("default_export_dir") && j["default_export_dir"].is_string())
        s.defaultExportDir = j["default_export_dir"].get<std::string>();
      // N2 (P8.1-W2): opaque ui_state blob. Guard the ≤64 KB contract cap
      // BEFORE persisting — an oversize blob is rejected with a bridge error
      // and the prior on-disk value is left untouched (we return before
      // save()).
      if (j.contains("ui_state") && j["ui_state"].is_string()) {
        std::string blob = j["ui_state"].get<std::string>();
        constexpr size_t kMaxUiState = 64 * 1024;
        if (blob.size() > kMaxUiState) {
          nlohmann::ordered_json err;
          err["code"] = "ui_state_too_large";
          err["message"] = "settings.set: ui_state exceeds the 64 KB cap (" +
                           std::to_string(blob.size()) + " bytes)";
          return BridgeResult::failure(err.dump());
        }
        s.uiState = std::move(blob);
      }
      // WS-B: the consent dialog and the Settings panel both write this pair
      // through the same partial-merge path as every other field.
      if (j.contains("extcompat") && j["extcompat"].is_boolean())
        s.extcompat = j["extcompat"].get<bool>();
      if (j.contains("extcompat_consent_at") && j["extcompat_consent_at"].is_string())
        s.extcompatConsentAt = j["extcompat_consent_at"].get<std::string>();
      if (j.contains("follow_do_file_folder") && j["follow_do_file_folder"].is_boolean())
        s.followDoFileFolder = j["follow_do_file_folder"].get<bool>();
    }
  } catch (const nlohmann::json::exception& e) {
    nlohmann::ordered_json err;
    err["code"] = "bad_json";
    err["message"] = std::string("settings.set: invalid JSON body: ") + e.what();
    return BridgeResult::failure(err.dump());
  }

  if (!piet::settings::save(s)) {
    nlohmann::ordered_json err;
    err["code"] = "io_error";
    err["message"] = "settings.set: could not write " + piet::settings::settingsFilePath();
    return BridgeResult::failure(err.dump());
  }
  // Compute cores + memory budget apply immediately (next command, via the
  // engine-settings epoch); scratch/workdir are restart-to-apply (read by
  // makeWorkdir / cwd seed at launch).
  piet::settings::applyEngineBudget(s);
  // WS-B: a persisted extcompat choice applies to the very next command, so
  // publish it to the dispatch gate's cache instead of waiting for a restart.
  piet::settings::setExtcompatEnabled(s.extcompat);

  auto sys = piet::settings::detectSysInfo();
  return BridgeResult::success(
      piet::settings::toGetJson(s, sys, piet::settings::settingsFilePath()).dump());
}

}  // namespace piet::routes
