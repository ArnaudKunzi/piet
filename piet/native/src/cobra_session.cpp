// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
#include "piet/cobra_session.h"

#include <chrono>
#include "piet/counters.h"
#include "piet/datastore.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <csignal>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace piet {

namespace {

std::atomic<long> g_engineSettingsEpoch{0};

// Build a CobraRunResult mirroring a §25/§26 response object (which carries
// the command's own {exit,stdout,stderr} as FIELDS — the worker's real
// stdout is the PROTOCOL stream, so the command output travels inside the
// JSON, never inline).
CobraRunResult resultFromResponse(const nlohmann::json& resp) {
  CobraRunResult r;
  r.exit_code = resp.value("exit", -1);
  r.stdout_text = resp.value("stdout", std::string());
  r.stderr_text = resp.value("stderr", std::string());
  r.ok = (r.exit_code == 0);
  return r;
}

// Same launch-failure shape runCobra returns when fork/exec fails, so callers
// (SessionState intercept / runCobraJson) treat a dead worker exactly like a
// binary that could not launch and fall back to one-shot spawning.
CobraRunResult ioFailure(const std::string& msg) {
  CobraRunResult r;
  r.error = msg;
  r.exit_code = -1;
  r.ok = false;
  return r;
}

void fillFrameVar(CobraFrameVar& v, const nlohmann::json& j) {
  v.name = j.value("name", std::string());
  v.type = j.value("type", std::string());
  v.nMissing = j.value("n_missing", int64_t{0});
  if (j.contains("n_levels") && j.at("n_levels").is_number()) {
    v.nLevels = j.at("n_levels").get<int64_t>();
  }
  // BUG-365: absent key => empty => "the engine said nothing", which is exactly what
  // an engine predating this key says about every column.
  v.format = j.value("format", std::string());
  v.label = j.value("label", std::string());
}

// Parse the §31 `"est":{...}` sibling of `frame`. Unlike fillFrameMeta this
// ALWAYS writes `out` — resetting it to {valid=false} when the response has no
// `est` — because the caller keeps it as retained per-worker state, and a mint
// left standing after a later response would be readable as belonging to that
// later call.
void fillEstMeta(CobraEstMeta& out, const nlohmann::json& resp) {
  out = CobraEstMeta{};
  if (!resp.contains("est") || !resp.at("est").is_object()) return;
  const auto& e = resp.at("est");
  out.handle = e.value("handle", std::string());
  if (out.handle.empty()) return;  // a handle-less est block addresses nothing.
  out.valid = true;
  out.k = e.value("k", int64_t{0});
  out.nObs = e.value("n_obs", int64_t{0});
  out.vcovType = e.value("vcov_type", std::string());
  out.fits = e.value("fits", int64_t{-1});
}

// Process-wide §31 mechanism counter — see CobraSession::postestHitCount().
std::atomic<long> g_postestHits{0};

// See activeCobraSession(). thread_local for the same reason cobra_runner.cpp's
// active-session stack is: the UI thread and the do-executor worker thread each
// drive a session, never concurrently, and a post-estimation call is always
// issued on the thread that just ran the fit.
thread_local CobraSession* tl_activeCobraSession = nullptr;

void fillFrameMeta(CobraFrameMeta* meta, const nlohmann::json& resp) {
  if (!meta) return;
  *meta = CobraFrameMeta{};
  if (!resp.contains("frame") || !resp.at("frame").is_object()) return;
  const auto& f = resp.at("frame");
  meta->valid = true;
  meta->rows = f.value("rows", int64_t{0});
  meta->cols = f.value("cols", int64_t{0});
  // contract_A §26 emits changed/added/dropped as ARRAYS of column names (the
  // engine's own frame-response shape, cecon_cli.cpp). Store the counts the
  // client's shadow logic needs (a name list is redundant with vars_delta for
  // changed/added; dropped>0 just gates a full rebuild). Tolerate a bare number
  // too, so a future scalar-count contract revision stays parseable.
  // e(sample) T2 (piet/datastore.h isHiddenColumn): the card is the USER's
  // varlist, so a hidden `__piet_*` column is not counted and not carried; the
  // name arrays let the counts below drop it exactly.
  auto count_of = [&f](const char* key) -> int64_t {
    if (!f.contains(key)) return 0;
    const auto& v = f.at(key);
    if (v.is_array()) {
      int64_t n = 0;
      for (const auto& e : v)
        if (!(e.is_string() && piet::isHiddenColumn(e.get<std::string>()))) ++n;
      return n;
    }
    if (v.is_number()) return v.get<int64_t>();
    return 0;
  };
  meta->changed = count_of("changed");
  meta->added = count_of("added");
  meta->dropped = count_of("dropped");
  meta->droppedNames.clear();
  if (f.contains("dropped") && f["dropped"].is_array()) {
    for (const auto& d : f["dropped"])
      if (d.is_string() && !piet::isHiddenColumn(d.get<std::string>()))
        meta->droppedNames.push_back(d.get<std::string>());
  }
  meta->structure = f.value("structure", false);
  // Default is `!meta->structure`, NOT a bare true/false: an older engine
  // that predates this field emits no "card_stable" key at all, and that
  // engine's `structure` was the only (overloaded) signal the fallback logic
  // had — it fell back whenever structure was true. Defaulting card_stable to
  // !structure reproduces exactly that old behaviour bit-for-bit against an
  // old engine. A bare `true` default would wrongly serve a stale metadata
  // card for every mutation an old engine reports as structural (a
  // wrong-answer bug, not a perf regression); a bare `false` default would
  // force every verb onto the slow rebuild path against an old engine.
  meta->card_stable = f.value("card_stable", !meta->structure);
  // See CobraFrameMeta::mergeCountsValid.
  meta->mergeCountsValid = false;
  if (f.contains("merge_counts") && f.at("merge_counts").is_object()) {
    const auto& mc = f.at("merge_counts");
    meta->mergeMasterOnly = mc.value("master_only", (int64_t)0);
    meta->mergeUsingOnly  = mc.value("using_only", (int64_t)0);
    meta->mergeMatched    = mc.value("matched", (int64_t)0);
    meta->mergeCountsValid = true;
  }
  // e(sample) T2: hidden columns leave the card here, before
  // seedShadowFromFrameVars or a delta fold can see them. `cols` is the frame's
  // column count and must match the card's, so it drops the hidden columns
  // whenever the response names them all: a full card (`vars`) or a FULL delta
  // (every column changed: sort, keep if, ...). A partial delta that names none
  // leaves `cols` as sent, so a frame that does carry one looks one column wider
  // than the card and the fold declines into a full rebuild -- slower, never a
  // wrong card.
  const int64_t rawCols = meta->cols;
  int64_t hiddenInVars = 0, hiddenInDelta = 0, rawDelta = 0;
  if (f.contains("vars_delta") && f.at("vars_delta").is_array()) {
    for (const auto& jv : f.at("vars_delta")) {
      CobraFrameVar v;
      fillFrameVar(v, jv);
      ++rawDelta;
      if (piet::isHiddenColumn(v.name)) {
        ++hiddenInDelta;
        continue;
      }
      meta->varsDelta.push_back(std::move(v));
    }
  }
  bool haveVars = false;
  if (f.contains("vars") && f.at("vars").is_array()) {
    haveVars = true;
    for (const auto& jv : f.at("vars")) {
      CobraFrameVar v;
      fillFrameVar(v, jv);
      if (piet::isHiddenColumn(v.name)) {
        ++hiddenInVars;
        continue;
      }
      meta->vars.push_back(std::move(v));
    }
  }
  if (haveVars) {
    meta->cols = rawCols - hiddenInVars;
  } else if (rawDelta == rawCols) {
    meta->cols = rawCols - hiddenInDelta;
  }
}

}  // namespace

long engineSettingsEpoch() { return g_engineSettingsEpoch.load(std::memory_order_relaxed); }
void bumpEngineSettingsEpoch() { g_engineSettingsEpoch.fetch_add(1, std::memory_order_relaxed); }

CobraSession::CobraSession(std::string cobraBin) : cobraBin_(std::move(cobraBin)) {}

CobraSession::~CobraSession() {
  // Never leave activeCobraSession() pointing at freed storage. Only clears the
  // binding on the thread running the destructor; another thread's binding to a
  // session it no longer owns would already be a lifetime bug in the owner
  // (SessionState holds its CobraSession by unique_ptr for its whole life).
  if (tl_activeCobraSession == this) tl_activeCobraSession = nullptr;
  killWorker();
}

// ---------------------------------------------------------------------------
// POSIX worker: fork/exec `cobra serve` with THREE pipes (stdin for requests,
// stdout for responses, stderr for the worker's own diagnostics — drained on
// a side thread so a full stderr OS buffer can never wedge the response
// stream). Mirrors runCobra's fork/exec+pipe discipline, extended to a
// persistent bidirectional child.
// ---------------------------------------------------------------------------
#ifndef _WIN32

void CobraSession::spawn() {
  piet::counters::addServeSpawn();  // W0: 1 on a healthy session; >1 = worker deaths
  spawned_ = false;
  frames_ = Frames::Unknown;
  readBuf_.clear();
  int inPipe[2] = {-1, -1};   // parent writes inPipe[1] -> child stdin inPipe[0]
  int outPipe[2] = {-1, -1};  // child stdout outPipe[1] -> parent reads outPipe[0]
  int errPipe[2] = {-1, -1};
  auto closeAll = [&]() {
    for (int* p : {inPipe, outPipe, errPipe}) {
      if (p[0] >= 0) close(p[0]);
      if (p[1] >= 0) close(p[1]);
    }
  };
  if (pipe(inPipe) != 0 || pipe(outPipe) != 0 || pipe(errPipe) != 0) {
    closeAll();
    serveUnavailable_ = true;
    return;
  }
  pid_t pid = fork();
  if (pid < 0) {
    closeAll();
    serveUnavailable_ = true;
    return;
  }
  if (pid == 0) {
    // Child: wire the three pipes to std streams, exec `cobra serve`.
    dup2(inPipe[0], STDIN_FILENO);
    dup2(outPipe[1], STDOUT_FILENO);
    dup2(errPipe[1], STDERR_FILENO);
    for (int* p : {inPipe, outPipe, errPipe}) {
      close(p[0]);
      close(p[1]);
    }
    // RAMDIET-AFTERCARE D (2026-08-15): shrink the engine's allocator footprint.
    //
    // The campaign's last open number was ~266 MB of dirty pages the allocator holds
    // after every allocation in them is freed — regions vmmap marks "(empty)".
    // `malloc_zone_pressure_relief` provably will not take them back (0.0 MB on twelve
    // consecutive calls, by both the footprint delta AND the allocator's own return
    // value), and neither fewer allocations nor in-place compaction moved the peak.
    // This knob does, and by a lot: paired against a control arm on the reference
    // slice, three alternated pairs, engine peak physical footprint
    //
    //     442.5 / 507.7 / 472.1 MB  ->  216.6 / 231.3 / 216.5 MB   (geomean -53.2%)
    //
    // for +1.9% wall (inside the campaign's 2% regression gate). It also collapses the
    // run-to-run spread — control peaks ranged over 317 MB across seven runs, the
    // knob's over 19 MB across six — which is worth as much as the mean on an 8 GiB
    // box, where the spread is what decides whether a parallel arm swaps.
    //
    // Set on the CHILD side, after fork and before exec, because libmalloc reads its
    // environment once at process init: setting it in the parent would change PIET's
    // own allocator too, which was never measured and is not what this claim covers.
    // A falsification arm (an invented `Malloc*` name) landed in the control band, so
    // the effect is this variable's, not "any Malloc* var is set".
    //
    // MECHANISM, established after adoption by the same peak snapshot: this does not
    // reclaim better, it stops fragmenting. Zone FRAG 368.6 MB (89%) -> 0 KB (0%), and
    // every "(empty)" region — 344 MB of MALLOC_SMALL, 31 MB of MALLOC_LARGE — is gone.
    // The pages eight attempts failed to give back were never a return problem; they
    // were a placement one.
    // OFF arm, same binary, one branch apart — the discipline COBRA_NO_CARRY /
    // COBRA_NO_SPAN already set here: a knob whose effect can only be measured by
    // rebuilding cannot be re-measured cheaply, and this one WILL need re-measuring
    // when the allocator or the workload moves.
    if (std::getenv("PIET_NO_SPACE_EFFICIENT") == nullptr)
      ::setenv("MallocSpaceEfficient", "1", 1);
    char* argv[] = {const_cast<char*>(cobraBin_.c_str()), const_cast<char*>("serve"), nullptr};
    execvp(cobraBin_.c_str(), argv);
    _exit(127);
  }
  // Parent: keep the write end of stdin + read ends of stdout/stderr.
  close(inPipe[0]);
  close(outPipe[1]);
  close(errPipe[1]);
  childPid_ = pid;
  inWr_ = inPipe[1];
  outRd_ = outPipe[0];
  errRd_ = errPipe[0];

  // Concurrent stderr drain (diagnostics only) — discard to EOF so the
  // worker never blocks writing a warning while we await a response.
  errDrain_ = std::thread([fd = errRd_]() {
    char buf[4096];
    while (true) {
      ssize_t n = read(fd, buf, sizeof(buf));
      if (n <= 0) break;
    }
  });

  // Handshake: one line {"serve":1,...} on stdout.
  std::string hs;
  if (!readLine(hs)) {
    killWorker();
    serveUnavailable_ = true;
    return;
  }
  try {
    nlohmann::json j = nlohmann::json::parse(hs);
    if (j.value("serve", 0) != 1) {
      killWorker();
      serveUnavailable_ = true;
      return;
    }
  } catch (const nlohmann::json::exception&) {
    killWorker();
    serveUnavailable_ = true;
    return;
  }
  spawned_ = true;
  spawnEpoch_ = engineSettingsEpoch();
}

void CobraSession::killWorker() {
  if (inWr_ >= 0) {
    close(inWr_);  // EOF on the worker's stdin -> it exits its read loop.
    inWr_ = -1;
  }
  if (childPid_ > 0) {
    // Give it the stdin-EOF exit; SIGTERM as a backstop, then reap.
    kill(childPid_, SIGTERM);
    int status = 0;
    waitpid(childPid_, &status, 0);
    childPid_ = -1;
  }
  if (errDrain_.joinable()) errDrain_.join();  // exits once errRd_ EOFs.
  if (outRd_ >= 0) {
    close(outRd_);
    outRd_ = -1;
  }
  if (errRd_ >= 0) {
    close(errRd_);
    errRd_ = -1;
  }
  spawned_ = false;
}

bool CobraSession::writeLine(const std::string& line) {
  if (inWr_ < 0) return false;
  size_t off = 0;
  while (off < line.size()) {
    ssize_t n = write(inWr_, line.data() + off, line.size() - off);
    if (n < 0) {
      if (errno == EINTR) continue;
      return false;  // EPIPE etc. -> worker gone.
    }
    off += static_cast<size_t>(n);
  }
  return true;
}

bool CobraSession::readLine(std::string& out) {
  // Drain any already-buffered line first, else read chunks until '\n'.
  for (;;) {
    auto nl = readBuf_.find('\n');
    if (nl != std::string::npos) {
      out.assign(readBuf_, 0, nl);
      readBuf_.erase(0, nl + 1);
      return true;
    }
    if (outRd_ < 0) return false;
    char buf[65536];
    ssize_t n = read(outRd_, buf, sizeof(buf));
    if (n < 0) {
      if (errno == EINTR) continue;
      return false;
    }
    if (n == 0) return false;  // EOF -> worker gone.
    readBuf_.append(buf, static_cast<size_t>(n));
  }
}

#else  // _WIN32

void CobraSession::spawn() {
  piet::counters::addServeSpawn();  // W0: 1 on a healthy session; >1 = worker deaths
  spawned_ = false;
  frames_ = Frames::Unknown;
  readBuf_.clear();
  SECURITY_ATTRIBUTES sa{};
  sa.nLength = sizeof(sa);
  sa.bInheritHandle = TRUE;
  HANDLE inRd = nullptr, inWr = nullptr, outRd = nullptr, outWr = nullptr, errRd = nullptr,
         errWr = nullptr;
  auto fail = [&]() {
    for (HANDLE h : {inRd, inWr, outRd, outWr, errRd, errWr}) {
      if (h) CloseHandle(h);
    }
    serveUnavailable_ = true;
  };
  if (!CreatePipe(&inRd, &inWr, &sa, 0) || !CreatePipe(&outRd, &outWr, &sa, 0) ||
      !CreatePipe(&errRd, &errWr, &sa, 0)) {
    fail();
    return;
  }
  // Parent-held ends must not be inherited by the child.
  SetHandleInformation(inWr, HANDLE_FLAG_INHERIT, 0);
  SetHandleInformation(outRd, HANDLE_FLAG_INHERIT, 0);
  SetHandleInformation(errRd, HANDLE_FLAG_INHERIT, 0);

  std::string cmd = "\"" + cobraBin_ + "\" serve";
  int wlen = MultiByteToWideChar(CP_UTF8, 0, cmd.c_str(), (int)cmd.size(), nullptr, 0);
  std::wstring wcmd(static_cast<size_t>(wlen), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, cmd.c_str(), (int)cmd.size(), &wcmd[0], wlen);
  std::vector<wchar_t> cmdbuf(wcmd.begin(), wcmd.end());
  cmdbuf.push_back(L'\0');

  STARTUPINFOW si{};
  si.cb = sizeof(si);
  si.dwFlags = STARTF_USESTDHANDLES;
  si.hStdInput = inRd;
  si.hStdOutput = outWr;
  si.hStdError = errWr;
  PROCESS_INFORMATION pi{};
  // CREATE_NO_WINDOW — same reason as cobra_runner.cpp's spawn: cobra.exe is
  // console-subsystem, and a GUI parent must not flash a black cmd window for
  // it. This one is the persistent `cobra serve` session, so without the flag
  // the console would sit there for the whole app lifetime, not just a blink.
  BOOL ok = CreateProcessW(nullptr, cmdbuf.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW,
                           nullptr, nullptr, &si, &pi);
  CloseHandle(inRd);
  CloseHandle(outWr);
  CloseHandle(errWr);
  if (!ok) {
    if (inWr) CloseHandle(inWr);
    if (outRd) CloseHandle(outRd);
    if (errRd) CloseHandle(errRd);
    serveUnavailable_ = true;
    return;
  }
  CloseHandle(pi.hThread);
  childProc_ = pi.hProcess;
  inWr_ = inWr;
  outRd_ = outRd;
  errRd_ = errRd;
  errDrain_ = std::thread([h = errRd_]() {
    char buf[4096];
    DWORD n = 0;
    while (ReadFile((HANDLE)h, buf, sizeof(buf), &n, nullptr) && n > 0) {
    }
  });

  std::string hs;
  if (!readLine(hs)) {
    killWorker();
    serveUnavailable_ = true;
    return;
  }
  try {
    nlohmann::json j = nlohmann::json::parse(hs);
    if (j.value("serve", 0) != 1) {
      killWorker();
      serveUnavailable_ = true;
      return;
    }
  } catch (const nlohmann::json::exception&) {
    killWorker();
    serveUnavailable_ = true;
    return;
  }
  spawned_ = true;
  spawnEpoch_ = engineSettingsEpoch();
}

void CobraSession::killWorker() {
  if (inWr_) {
    CloseHandle((HANDLE)inWr_);
    inWr_ = nullptr;
  }
  if (childProc_) {
    WaitForSingleObject((HANDLE)childProc_, 2000);
    TerminateProcess((HANDLE)childProc_, 0);
    CloseHandle((HANDLE)childProc_);
    childProc_ = nullptr;
  }
  if (errDrain_.joinable()) errDrain_.join();
  if (outRd_) {
    CloseHandle((HANDLE)outRd_);
    outRd_ = nullptr;
  }
  if (errRd_) {
    CloseHandle((HANDLE)errRd_);
    errRd_ = nullptr;
  }
  spawned_ = false;
}

bool CobraSession::writeLine(const std::string& line) {
  if (!inWr_) return false;
  size_t off = 0;
  while (off < line.size()) {
    DWORD n = 0;
    if (!WriteFile((HANDLE)inWr_, line.data() + off, (DWORD)(line.size() - off), &n, nullptr)) {
      return false;
    }
    off += n;
  }
  return true;
}

bool CobraSession::readLine(std::string& out) {
  for (;;) {
    auto nl = readBuf_.find('\n');
    if (nl != std::string::npos) {
      out.assign(readBuf_, 0, nl);
      readBuf_.erase(0, nl + 1);
      return true;
    }
    if (!outRd_) return false;
    char buf[65536];
    DWORD n = 0;
    if (!ReadFile((HANDLE)outRd_, buf, sizeof(buf), &n, nullptr) || n == 0) return false;
    readBuf_.append(buf, n);
  }
}

#endif  // _WIN32

bool CobraSession::ensureSpawned() {
  if (serveUnavailable_) return false;
  if (!spawned_) spawn();
  return spawned_;
}

namespace {
// T6: RAII wall-timer that bills a blocking engine round-trip to the shared
// engine-wait accumulator (counters.h). Two integer ops when nothing reads it.
struct EngineWaitScope {
  std::chrono::steady_clock::time_point t0 = std::chrono::steady_clock::now();
  ~EngineWaitScope() {
    piet::counters::addEngineWaitNs(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now() - t0).count());
  }
};
}  // namespace

// W0 (P2PERF §W0): bill one round trip to its SHAPE. Every request funnels
// through CobraSession::request, so this is the single choke point where the
// stateless/frame split is still visible — one branch on a JSON key that is
// already in hand, before the dump. Classified per logical request, not per
// retry attempt: a retry is a worker death, which `serve_spawns` reports.
static void billEngineRequest(const nlohmann::json& req) {
  if (req.contains("argv")) {
    piet::counters::addEngineReqStateless();
    return;
  }
  auto f = req.find("frame");
  if (f == req.end() || !f->contains("op")) {
    piet::counters::addEngineReqOther();
    return;
  }
  const std::string op = f->at("op").get<std::string>();
  if (op == "run") piet::counters::addEngineReqFrameRun();
  else if (op == "use") piet::counters::addEngineReqFrameUse();
  else if (op == "snapshot") piet::counters::addEngineReqSnapshot();
  else if (op == "postest") piet::counters::addEngineReqPostest();
  else piet::counters::addEngineReqOther();
}

// S07-ROUNDTRIP instrument. billEngineRequest() answers "how many round trips
// of each SHAPE", which was enough to see that S07's frames half pays ~8
// stateless trips per rep but not enough to say WHICH verb sends them — the
// verb timings aggregate at the `capture` prefix, so the inner command is
// invisible. This prints one line per request to stderr under
// $PIET_TRACE_ENGINE, naming the shape and the first argv token (or the frame
// op), which is what attributes a trip to a verb. Inert otherwise: one getenv
// at first call, then one bool test per request; nothing on stdout ever, so no
// freeze fixture and no golden can see it.
struct EngineTraceScope {
  bool on;
  std::string label;
  std::chrono::steady_clock::time_point t0;
  explicit EngineTraceScope(const nlohmann::json& req) : on(enabled()) {
    if (!on) return;
    if (auto a = req.find("argv"); a != req.end() && a->is_array() && !a->empty()) {
      label = "stateless " + a->at(0).get<std::string>();
      // The `--data` leaf tells apart a trip over the 48k-row master from one
      // over a 2k-row helper — the whole question when pricing a chain of them.
      for (size_t i = 1; i + 1 < a->size(); ++i) {
        if (a->at(i) == "--data") {
          std::string p = a->at(i + 1).get<std::string>();
          size_t s = p.find_last_of("/\\");
          label += " " + (s == std::string::npos ? p : p.substr(s + 1));
          break;
        }
      }
    } else if (auto f = req.find("frame"); f != req.end() && f->contains("op")) {
      label = "frame " + f->at("op").get<std::string>();
      // MECHOPT W1: `frame use`/`frame snapshot` carry a "path" — append its
      // basename with the same idiom the stateless branch uses above. Without
      // this the census could not tell a virtual-use hit from a disk adoption,
      // which is exactly the split the frame-use bucket's diagnosis turned on.
      if (auto p = f->find("path"); p != f->end() && p->is_string()) {
        std::string pv = p->get<std::string>();
        size_t s = pv.find_last_of("/\\");
        label += " " + (s == std::string::npos ? pv : pv.substr(s + 1));
      }
    } else {
      label = "other";
    }
    t0 = std::chrono::steady_clock::now();
  }
  ~EngineTraceScope() {
    if (!on) return;
    double ms = std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::steady_clock::now() - t0).count() / 1e6;
    std::fprintf(stderr, "[engreq] %-40s %8.2f ms\n", label.c_str(), ms);
  }
  static bool enabled() {
    static const bool v = [] {
      const char* e = std::getenv("PIET_TRACE_ENGINE");
      return e != nullptr && *e != '\0';
    }();
    return v;
  }
  EngineTraceScope(const EngineTraceScope&) = delete;
  EngineTraceScope& operator=(const EngineTraceScope&) = delete;
};

CobraRunResult CobraSession::request(nlohmann::json req, CobraFrameMeta* metaOut) {
  EngineWaitScope __ews;
  billEngineRequest(req);
  EngineTraceScope __ets{req};
  std::lock_guard<std::mutex> lock(mu_);
  tl_activeCobraSession = this;  // §31 reachability — see activeCobraSession().
  for (int attempt = 0; attempt < 2; ++attempt) {
    if (!ensureSpawned()) return ioFailure("cobra serve unavailable");
    req["id"] = nextId_++;
    std::string line = req.dump();
    line.push_back('\n');
    std::string respLine;
    if (writeLine(line) && readLine(respLine)) {
      consecutiveDeaths_ = 0;
      try {
        nlohmann::json resp = nlohmann::json::parse(respLine);
        fillFrameMeta(metaOut, resp);
        // §31: rewrite the retained est block on EVERY response (clearing it
        // when there is none), and bump the sequence. The sequence is what lets
        // a caller distinguish "my request reached this worker and minted
        // nothing" from "my request never reached this worker at all" (serve
        // disabled, one-shot fallback) — in the second case a mint left over
        // from an EARLIER model would otherwise be readable as this call's, and
        // spending it later is precisely the wrong-model answer §31 is built to
        // prevent. Under mu_, like every other field this method touches.
        fillEstMeta(lastEst_, resp);
        ++responseSeq_;
        return resultFromResponse(resp);
      } catch (const nlohmann::json::exception&) {
        return ioFailure("cobra serve: malformed response");
      }
    }
    // Write/read failed -> the worker died mid-exchange.
    killWorker();
    if (++consecutiveDeaths_ >= 2) {
      serveUnavailable_ = true;  // permanent fallback to one-shot spawning.
      return ioFailure("cobra serve: worker died repeatedly");
    }
    respawnedSinceLastCheck_ = true;  // respawn once and retry on the fresh worker.
  }
  return ioFailure("cobra serve: worker unrecoverable");
}

void CobraSession::respawnForEpoch() {
  std::lock_guard<std::mutex> lock(mu_);
  if (!spawned_) return;
  killWorker();
  spawn();  // re-reads CECON_NUM_THREADS; resets spawnEpoch_ to the current value.
}

CobraSession::Frames CobraSession::framesState() {
  ensureSpawned();
  return frames_;
}

CobraRunResult CobraSession::sendStateless(const std::vector<std::string>& argv) {
  nlohmann::json req;
  req["argv"] = argv;
  return request(std::move(req), nullptr);
}

CobraRunResult CobraSession::frameUse(const std::string& path, CobraFrameMeta* metaOut) {
  nlohmann::json req;
  req["frame"] = {{"op", "use"}, {"path", path}};
  CobraRunResult r = request(std::move(req), metaOut);
  if (r.exit_code == 2) {
    frames_ = Frames::No;  // engine is §25-only.
  } else if (r.exit_code == 0) {
    frames_ = Frames::Yes;
  }
  return r;
}

CobraRunResult CobraSession::frameVars(CobraFrameMeta* metaOut) {
  nlohmann::json req;
  req["frame"] = {{"op", "vars"}};
  return request(std::move(req), metaOut);
}

CobraRunResult CobraSession::frameRun(const std::vector<std::string>& argv, CobraFrameMeta* metaOut) {
  nlohmann::json req;
  req["frame"] = {{"op", "run"}, {"argv", argv}};
  CobraRunResult r = request(std::move(req), metaOut);
  // Capability detection (frames Yes/No) is frameUse's job — a §25-only engine
  // rejects the FIRST frame op with exit 2. Once frames_ is Yes, a frame-run's
  // exit 2 is a VERB usage error (bad flags), NOT a capability signal, so it
  // must NOT permanently disable the resident plane for the whole session.
  // Only confirm Yes on a clean run; leave a No/Unknown downgrade to frameUse.
  if (r.exit_code == 0) {
    frames_ = Frames::Yes;
  } else if (r.exit_code == 2 && frames_ == Frames::Unknown) {
    frames_ = Frames::No;  // first-ever frame op was a run and the engine rejected it.
  }
  return r;
}

// §38 (contract_A 1.25.0). Ask the resident frame for a column SUBSET, written to
// `out` as an ordinary .hbin. Same discipline as §27 above and for the same reason:
// this must NOT touch frames_. Capability detection belongs to frameUse/frameRun —
// a §26-only engine answers exit 2 "unknown op" here, and that is a per-request
// miss the caller falls back from (it materializes the head the old way), not a
// reason to tear the whole resident plane down for the rest of the session.
CobraRunResult CobraSession::frameGetcols(const std::vector<std::string>& cols,
                                          const std::string& out) {
  nlohmann::json req;
  req["frame"] = {{"op", "getcols"}, {"cols", cols}, {"out", out}};
  return request(std::move(req), nullptr);
}

CobraRunResult CobraSession::frameSnapshot(const std::string& path) {
  nlohmann::json req;
  req["frame"] = {{"op", "snapshot"}, {"path", path}};
  return request(std::move(req), nullptr);
}

CobraRunResult CobraSession::frameVirtualDrop(const std::string& path) {
  if (!spawned_) {  // no worker => nothing registered => nothing to forget
    CobraRunResult r;
    r.ok = true;
    r.exit_code = 0;
    return r;
  }
  nlohmann::json req;
  req["frame"] = {{"op", "vdrop"}, {"path", path}};
  return request(std::move(req), nullptr);
}

CobraRunResult CobraSession::frameDrop() {
  nlohmann::json req;
  req["frame"] = {{"op", "drop"}};
  return request(std::move(req), nullptr);
}

CobraRunResult CobraSession::frameStashDrop(const std::string& name, int64_t* stashesLeft) {
  if (stashesLeft) *stashesLeft = -1;
  if (!spawned_) {  // no worker => no stash => nothing to forget
    CobraRunResult r;
    r.ok = true;
    r.exit_code = 0;
    if (stashesLeft) *stashesLeft = 0;
    return r;
  }
  nlohmann::json req;
  req["frame"] = {{"op", "stashdrop"}, {"name", name}};
  CobraRunResult r = request(std::move(req), nullptr);
  if (r.ok && stashesLeft) {
    try {
      auto j = nlohmann::json::parse(r.stdout_text);
      if (j.is_object() && j.contains("stashes")) *stashesLeft = j["stashes"].get<int64_t>();
    } catch (...) {
    }
  }
  return r;
}

CobraRunResult CobraSession::frameCounters() {
  if (!spawned_) {
    // No worker was ever needed (or serve is disabled): there are no engine-side
    // counters to read, and spawning one HERE would both fabricate a spawn the
    // run never made and answer with an all-zero worker. Report absence instead.
    return ioFailure("cobra serve: no worker in this session");
  }
  nlohmann::json req;
  req["frame"] = {{"op", "counters"}};
  return request(std::move(req), nullptr);
}

// §27 (contract_A 1.16.0 DRAFT). Deliberately do NOT touch frames_: capability
// detection is frameUse/frameRun's job (§26). Once frames_ is Yes a §27 op that
// the engine rejects (exit 2 unknown-op on a §26-only build, or exit 3 no-frame)
// is a per-request failure the SessionState client falls back on — it must NOT
// flip the whole resident plane to No.
CobraRunResult CobraSession::frameStash(const std::string& name, CobraFrameMeta* metaOut) {
  nlohmann::json req;
  req["frame"] = {{"op", "stash"}, {"name", name}};
  return request(std::move(req), metaOut);
}

CobraRunResult CobraSession::frameUnstash(const std::string& name, bool keep,
                                          CobraFrameMeta* metaOut) {
  nlohmann::json req;
  req["frame"] = {{"op", "unstash"}, {"name", name}, {"keep", keep}};
  return request(std::move(req), metaOut);
}

CobraRunResult CobraSession::frameSaveVirtual(const std::string& path, CobraFrameMeta* metaOut) {
  nlohmann::json req;
  req["frame"] = {{"op", "save_virtual"}, {"path", path}};
  return request(std::move(req), metaOut);
}

// §31 (contract_A 1.20.0 DRAFT, unstamped — see postestState()'s comment on why
// this is feature-detected and never version-gated).
//
// Like the §27 ops above this does NOT touch frames_: whether the engine speaks
// §31 is an independent question from whether it speaks §26, and an engine that
// has the resident data plane but not the estimation store must keep the
// resident plane. Unlike them it DOES record its own capability, because the
// answer is stable for the worker's lifetime and re-asking on every one of the
// 53 post-estimation calls in a dependent-variable pass would spend a round trip
// per call to re-learn the same No.
CobraRunResult CobraSession::framePostest(const std::string& handle,
                                          const std::vector<std::string>& argv,
                                          CobraEstMeta* metaOut) {
  if (metaOut) *metaOut = CobraEstMeta{};
  if (postest_ == Postest::No) {
    // Known-incapable engine: fail without a round trip. Same shape as a dead
    // worker, which is already the caller's "fall back to the full
    // specification" signal.
    // ASCII-only in the message itself: every other runtime string in this file
    // is ASCII, and the section marker lives in comments.
    return ioFailure("cobra serve: engine has no postest op (contract_A section 31)");
  }
  nlohmann::json req;
  req["frame"] = {{"op", "postest"}, {"est", handle}, {"argv", argv}};
  CobraRunResult r = request(std::move(req), nullptr);

  if (r.exit_code == 0) {
    postest_ = Postest::Yes;
    g_postestHits.fetch_add(1, std::memory_order_relaxed);
    if (metaOut) *metaOut = lastEst_;
  } else if (r.exit_code == 2) {
    // Unknown frame op — this engine predates §31. Unambiguous HERE (unlike
    // frameRun's exit 2) because the caller owns the whole argv and never sends
    // the two shapes §31 itself answers with exit 2: a verb other than
    // lincom/nlcom/test, and an argv carrying --data/--out. Latch it so the
    // remaining calls in this session skip the probe entirely.
    postest_ = Postest::No;
  }
  // Every other non-zero exit — notably exit 3, the explicit "no stored model
  // '<handle>' (expired or never minted)" — is a per-request MISS, not a
  // capability downgrade. postest_ stays as it was; the caller drops the handle
  // and re-runs the full specification, which re-mints.
  return r;
}

long CobraSession::postestHitCount() { return g_postestHits.load(std::memory_order_relaxed); }
void CobraSession::resetPostestHitCount() { g_postestHits.store(0, std::memory_order_relaxed); }

CobraSession* activeCobraSession() { return tl_activeCobraSession; }

}  // namespace piet
