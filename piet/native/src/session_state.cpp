// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
#include "piet/session_state.h"
#include <execinfo.h>
#include <dlfcn.h>
#include "io/win_compat.h"  // cecon::to_native_path (BUG-050 wide open)

#include <algorithm>
#include <cctype>
#include <cmath>
#include <chrono>
#include <cstddef>
#include <cstdio>   // C4-L1 window trace (stderr fprintf, env-gated)
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <functional>  // std::greater<> (BUG-381 census sort)
#include <set>
#include <sstream>
#include <thread>
#include <unordered_set>

#include "piet/bcal.h"  // bcal::clearCache (resetProcessCaches)
#include "piet/cclass.h"  // BUG-1006: resolveCValue (macroCValue)
#include "piet/counters.h"
#include "piet/eval/errors.h"  // eval::coarseRc (the r(N) of a logged error)
#include "piet/eval/macros.h"  // eScalarKey (eNameOrderFor)
#include "piet/display_hardening.h"  // BUG-718: readVarObservation (macroVarObs)
#include "piet/scratch_seq.h"  // BUG-389: scratchMintSeq (sweep throttle)
#include "piet/storage_type.h"  // stataDefaultFormat (displayFormat)

namespace piet {

namespace {
// Monotonic seconds, matching Python's time.perf_counter() role (used only
// for elapsed-time deltas, never as a wall-clock timestamp).
double monotonicSeconds() {
  using namespace std::chrono;
  return duration<double>(steady_clock::now().time_since_epoch()).count();
}

// BUG-087/D9: see session_state.h's doc comment on isCurrentThreadADoWorker()
// for why this exists. thread_local storage duration — each OS thread gets
// its own copy, defaulting false; only main.cpp's runDoInBackground() worker
// lambda ever sets it true, once, at the top of its own thread body.
thread_local bool tl_isDoWorkerThread = false;
}  // namespace

// AER scope-2 B2-3 (review 2026-09-09). The business-calendar cache is a
// PROCESS singleton (bcal.h explains why: `display bofd(...)` evaluates in a
// process-wide immutable namespace that carries no session). Its key is the
// calendar NAME alone, because that is what Stata's cache is keyed by — so the
// only thing keeping two sessions in one process apart is this reset. It used
// to be claimed by a comment and performed by nobody: bcal::clearCache() had
// zero callers before this change.
void SessionState::resetProcessCaches() { bcal::clearCache(); }

void setCurrentThreadIsDoWorker(bool isWorker) { tl_isDoWorkerThread = isWorker; }
bool isCurrentThreadADoWorker() { return tl_isDoWorkerThread; }

nlohmann::ordered_json RunProgress::asDict() const {
  long long elapsed = elapsedMs;
  if (running && startedAt > 0.0) {
    elapsed = static_cast<long long>((monotonicSeconds() - startedAt) * 1000);
  }
  nlohmann::ordered_json j;
  j["running"] = running;
  j["current_line"] = currentLine;
  j["total_lines"] = totalLines;
  j["current_cmd"] = currentCmd;
  j["elapsed_ms"] = elapsed;
  j["blocks_ready"] = blocksReady;
  j["do_path"] = doPath.has_value() ? nlohmann::ordered_json(*doPath) : nlohmann::ordered_json(nullptr);
  j["cancel_requested"] = cancelRequested;
  j["stopped_reason"] = stoppedReason.has_value() ? nlohmann::ordered_json(*stoppedReason)
                                                   : nlohmann::ordered_json(nullptr);
  return j;
}

void SessionState::progressStart(int totalLines, std::optional<std::string> doPath) {
  std::lock_guard<std::mutex> lock(progressMutex_);
  progress_ = RunProgress{};
  progress_.running = true;
  progress_.totalLines = totalLines;
  progress_.startedAt = monotonicSeconds();
  progress_.doPath = std::move(doPath);
}

void SessionState::progressUpdate(int currentLine, const std::string& currentCmd, int blocksReady) {
  std::lock_guard<std::mutex> lock(progressMutex_);
  progress_.currentLine = currentLine;
  // Cut at 120 bytes, but never inside a UTF-8 character: a Korean statement
  // cut mid-codepoint is not valid UTF-8, and the snapshot is JSON (BUG-1131).
  size_t cut = currentCmd.size();
  if (cut > 120) {
    cut = 120;
    while (cut > 0 && (static_cast<unsigned char>(currentCmd[cut]) & 0xC0) == 0x80) --cut;
  }
  progress_.currentCmd = currentCmd.substr(0, cut);
  progress_.blocksReady = blocksReady;
}

void SessionState::progressFinish(const std::string& reason, std::optional<int> blocksReady) {
  std::lock_guard<std::mutex> lock(progressMutex_);
  if (progress_.startedAt > 0.0) {
    progress_.elapsedMs = static_cast<long long>((monotonicSeconds() - progress_.startedAt) * 1000);
  }
  progress_.running = false;
  progress_.stoppedReason = reason;
  if (blocksReady.has_value()) progress_.blocksReady = *blocksReady;
}

bool SessionState::progressRequestCancel() {
  {
    std::lock_guard<std::mutex> lock(progressMutex_);
    if (!progress_.running) return false;
    progress_.cancelRequested = true;
  }
  // BUG-087/D9: also wake any _request() blocked in requestUserInput()'s
  // wait — REUSES this same cancelRequested flag (requestUserInput()'s wait
  // predicate calls progressCancelRequested(), same as the do-executor's own
  // between-top-level-items poll does) rather than inventing a second
  // cancellation concept; this notify_all() is the only change needed to
  // make that flag actually WAKE a thread parked in a condition_variable
  // (a pending _request() sits INSIDE one top-level `display` command's
  // evaluation, a granularity the executor's own poll never reaches) instead
  // of only being observed by that poll. Safe to call unconditionally (a
  // no-op notify when nothing is waiting); no lock needed to call it.
  requestCv_.notify_all();
  return true;
}

bool SessionState::progressCancelRequested() const {
  std::lock_guard<std::mutex> lock(progressMutex_);
  // BUG-1068: a cancel belongs to the run it was asked of. The flag outlives
  // that run (the final tick reports it), and read bare it stopped the next
  // block executed on this session without owning a run -- at its first line,
  // "run cancelled" -- until some later run reset it. That happened whenever a
  // cancel landed after the run's last poll, or was the one that stopped it.
  return progress_.running && progress_.cancelRequested;
}

nlohmann::ordered_json SessionState::progressSnapshot() const {
  std::lock_guard<std::mutex> lock(progressMutex_);
  return progress_.asDict();
}

// BUG-087/D9 (BETA Phase F, D9 locked decision — contract_B additive MINOR):
// see session_state.h's doc comment for the full design. Assumes the caller
// (display_hardening.cpp) already checked hasRequestPostEvent() AND
// isCurrentThreadADoWorker() — this method only re-checks the former
// defensively (returns nullopt immediately, same shape as "cancelled", if
// somehow called with no postEvent hook wired).
std::optional<std::string> SessionState::requestUserInput(
    const std::string& macroName, const std::string& prompt,
    const std::vector<std::pair<std::string, std::string>>& choices,
    const nlohmann::ordered_json& extra) {
  if (!requestPostEvent_) return std::nullopt;

  std::string requestId;
  {
    std::lock_guard<std::mutex> lock(requestMutex_);
    requestId = std::to_string(++requestSeq_);
    pendingRequests_[requestId] = std::nullopt;
  }

  // WIRE SHAPE (LOCKED, contract_B additive — CONTRACT.md v2.3.0):
  // {"kind":"request_dialog","requestId":<string>,"prompt":<string>,
  //  "macroName":<string>}. "kind" is the discriminator existing RunProgress
  // ticks never carry (RunProgress::asDict() above has no such field), so
  // the frontend's piet-event listener can tell the two apart unambiguously.
  nlohmann::ordered_json evt;
  evt["kind"] = "request_dialog";
  evt["requestId"] = requestId;
  evt["prompt"] = prompt;
  evt["macroName"] = macroName;
  // SPEC II-4 (2026-09-02, additive): fixed choices + caller extras, only
  // when present, so the `_request()` push is byte-identical to before.
  if (!choices.empty()) {
    nlohmann::ordered_json arr = nlohmann::ordered_json::array();
    for (const auto& c : choices) {
      nlohmann::ordered_json one;
      one["value"] = c.first;
      one["label"] = c.second;
      arr.push_back(one);
    }
    evt["choices"] = arr;
  }
  for (auto it = extra.begin(); it != extra.end(); ++it) evt[it.key()] = it.value();
  // The prompt is the do-file's own text, which can hold a byte that is not
  // UTF-8: it goes out as U+FFFD, as every other block does (BUG-1131).
  requestPostEvent_(evt.dump(-1, ' ', false, nlohmann::ordered_json::error_handler_t::replace));

  std::unique_lock<std::mutex> lock(requestMutex_);
  bool cancelled = false;
  requestCv_.wait(lock, [&] {
    if (progressCancelRequested()) {
      cancelled = true;
      return true;
    }
    auto it = pendingRequests_.find(requestId);
    // Slot missing (already reaped by a prior wake) counts as "done" too —
    // defensive, should not normally happen (only this method erases its
    // own slot, once, right below).
    return it == pendingRequests_.end() || it->second.has_value();
  });

  std::optional<std::string> result;
  auto it = pendingRequests_.find(requestId);
  if (!cancelled && it != pendingRequests_.end()) result = it->second;
  if (it != pendingRequests_.end()) pendingRequests_.erase(it);
  return result;
}

void SessionState::resolveUserInput(const std::string& requestId, const std::string& value) {
  {
    std::lock_guard<std::mutex> lock(requestMutex_);
    auto it = pendingRequests_.find(requestId);
    if (it != pendingRequests_.end()) it->second = value;
    // Unknown/stale requestId: silent no-op (see this method's header doc
    // comment) — the wire contract's response is {"ok":true} regardless;
    // the caller (main.cpp's request.answer route) doesn't consult our
    // return value at all.
  }
  requestCv_.notify_all();
}

void SessionState::addHistory(const std::string& cmd, bool success) {
  history_.push_back(HistoryEntry{cmd, static_cast<double>(std::time(nullptr)), success});
  // cap at 500 entries (session.py add_history)
  if (history_.size() > 500) {
    history_.erase(history_.begin(), history_.begin() + (history_.size() - 500));
  }
}

nlohmann::ordered_json SessionState::historyJson() const {
  auto out = nlohmann::ordered_json::array();
  for (const auto& h : history_) {
    out.push_back({{"cmd", h.cmd}, {"ts", h.ts}, {"success", h.success}});
  }
  return out;
}

namespace {
// ---------------------------------------------------------------------------
// SMCL log writer (`log using FILE.smcl`, or `log using FILE, smcl`).
//
// Implements the SMCL log format per the log help file. Everything byte-level
// below was identified against StataBE 19.5 captures of `log using x.smcl`
// (scratch-r150/t7-l5/stata/cap1..cap8), not guessed:
//
//  * A fresh file starts "{smcl}\n{txt}{sf}{ul off}{.-}\n" and four label
//    lines (name / log / log type / opened on); the label values are in {res}.
//    Appending to a file that already has content, and `log on`, start with
//    "{smcl}\n{com}{sf}{ul off}{txt}{.-}\n" instead (the closing marker of the
//    previous segment has no newline, so the two run together).
//  * Stata writes a mode tag ({txt} {res} {com} {err}) only where the mode
//    CHANGES. Each command is a blank line in {txt}, then "{com}. cmd"; a
//    display line is {res}; an error is {err} followed by
//    "{txt}{search r(N), local:r(N);}" unless the command was captured.
//  * Literal braces in an echoed command line are escaped one for one as
//    {c -(} and {c )-}. What `display` prints is NOT escaped: its text is SMCL
//    already (the user typed the directives), and Stata copies it through.
//    (Text any other command prints is plain; there the braces are escaped.)
//  * `log off` / `log close` write the command line, then a footer of the
//    same four labels (paused on / closed on), "{txt}{.-}\n{smcl}\n" and a
//    final "{txt}{sf}{ul off}" with no newline. `log on` writes the
//    "resumed on" header but not its own command line. A paused log is closed
//    silently. A log left open at the end of the run gets no footer.
//  * Under `quietly` / `capture` (without `noisily`) those banners and footers
//    are suppressed output: the file gets only "{smcl}\n{com}{sf}{ul off}"
//    (no newline) where each would stand. `cap log off X` / `cap log on X`,
//    the 073 spelling, is that case.
//
// What differs from StataBE, by design: PIET's result blocks are not Stata's
// rendered tables, so a non-display block is written as the same "key: value"
// dump the text log uses (in {txt} mode, braces escaped) rather than as the
// ado's SMCL table; the loop-body numbering ("  2. ...") and the "> " line
// continuations of an echoed command are not reproduced; and the console-side
// output of `log using` / `log off` (the "(file ... not found)" note and the
// printed header) is not part of PIET's blocks, so the unnamed log never
// contains another log's banner; and the OPENING banner is always the full one
// -- a `quietly log using` gets the short marker in StataBE (the same rule as
// above), but the banner is written when the log opens, before the command
// line that says how it was spelled has reached the tee. The depth rule of LogState::depth is the text
// log's own and is kept: a log opened under a prefix (`quietly log using ...`)
// belongs to the prefix's inner frame and records only what that depth carries.
// A frame that flattens a whole `do` file or loop is written when that frame
// ends; the `log off` / `log on` / `log close` lines inside it take effect where
// they stand in the frame.
constexpr char kModeTxt = 't';
constexpr char kModeRes = 'r';
constexpr char kModeCom = 'c';
constexpr char kModeErr = 'e';

const char* smclModeTag(char mode) {
  switch (mode) {
    case kModeTxt: return "{txt}";
    case kModeRes: return "{res}";
    case kModeCom: return "{com}";
    default: return "{err}";
  }
}

// Moves the stream to `mode`, writing the tag only on a change.
void smclSetMode(LogState& log, char mode) {
  if (log.smclMode == mode) return;
  log.stream << smclModeTag(mode);
  log.smclMode = mode;
}

// Literal braces as SMCL spells them, one for one.
std::string smclEscape(const std::string& s) {
  std::string out;
  out.reserve(s.size());
  for (const char c : s) {
    if (c == '{') {
      out += "{c -(}";
    } else if (c == '}') {
      out += "{c )-}";
    } else {
      out.push_back(c);
    }
  }
  return out;
}

// "28 Oct 2026, 02:46:04": the day is blank-padded like c(current_date)
// (cclass.cpp), the clock zero-padded like c(current_time). The captures were
// taken on a two-digit day, so the blank padding of 1-9 is c(current_date)'s
// convention, not something a header capture showed.
std::string smclStamp() {
  static const char* const kMon[12] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                        "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
  const std::time_t t = std::time(nullptr);
  std::tm tmv{};
#ifdef _WIN32
  localtime_s(&tmv, &t);
#else
  localtime_r(&t, &tmv);
#endif
  const int mon = (tmv.tm_mon >= 0 && tmv.tm_mon < 12) ? tmv.tm_mon : 0;
  char buf[48];
  std::snprintf(buf, sizeof(buf), "%2d %s %04d, %02d:%02d:%02d", tmv.tm_mday, kMon[mon],
                tmv.tm_year + 1900, tmv.tm_hour, tmv.tm_min, tmv.tm_sec);
  return std::string(buf);
}

// The name / log / log type lines. `nameLinePrefix` is what precedes "name:" --
// the header's line follows the {txt} that ends its first line, the footers'
// carries its own {txt}.
void smclLabelLines(LogState& log, const char* nameLinePrefix) {
  log.stream << nameLinePrefix << "name:  {res}"
             << (log.name.empty() ? std::string("<unnamed>") : smclEscape(log.name)) << "\n"
             << "       {txt}log:  {res}" << smclEscape(log.path) << "\n"
             << "  {txt}log type:  {res}smcl\n";
}

// Under `quietly` / `capture` (no `noisily`) the banner and footer text are
// output the command suppresses, and only the structural markers reach the
// file: "{smcl}\n{com}{sf}{ul off}", with no newline after it.
void smclWriteQuietMarker(LogState& log) {
  log.stream << "{smcl}\n{com}{sf}{ul off}";
  log.smclMode = kModeCom;
  log.stream.flush();
}

// Opening banner. `continuation` = the file already holds a segment, or this
// is the resume of a paused log.
void smclWriteBanner(LogState& log, bool continuation, bool resumed, bool quiet = false) {
  if (quiet) return smclWriteQuietMarker(log);
  log.stream << "{smcl}\n"
             << (continuation ? "{com}{sf}{ul off}{txt}{.-}\n" : "{txt}{sf}{ul off}{.-}\n");
  smclLabelLines(log, "      ");
  log.stream << (resumed ? "{txt}resumed on:  {res}" : " {txt}opened on:  {res}") << smclStamp()
             << "\n";
  log.smclMode = kModeRes;
  log.stream.flush();
}

// Closing footer of a segment; `what` is "paused" or "closed".
void smclWriteFooter(LogState& log, const char* what, bool quiet = false) {
  if (quiet) return smclWriteQuietMarker(log);
  smclLabelLines(log, "      {txt}");
  log.stream << " {txt}" << what << " on:  {res}" << smclStamp() << "\n"
             << "{txt}{.-}\n{smcl}\n{txt}{sf}{ul off}";
  log.smclMode = kModeTxt;
  log.stream.flush();
}

// Applies every request in `ops` at once, in order, to a log whose tee will
// not meet their command lines (the frame already passed, or none of the log's
// depth is coming). Returns true when the log was closed. A request that finds
// the log already in the asked state (a second `log off`) writes nothing, as
// Stata does.
bool smclDrainOps(LogState& log) {
  bool closed = false;
  for (const LogOp op : log.ops) {
    if (op == LogOp::Pause) {
      if (!log.paused) {
        smclWriteFooter(log, "paused");
        log.paused = true;
      }
    } else if (op == LogOp::Resume) {
      if (log.paused) {
        smclWriteBanner(log, /*continuation=*/true, /*resumed=*/true);
        log.paused = false;
      }
    } else {
      if (!log.paused) smclWriteFooter(log, "closed");  // a paused log closes silently
      closed = true;
      break;
    }
  }
  log.ops.clear();
  return closed;
}

// The state `log` will be in once its queued requests are applied.
bool smclWillBePaused(const LogState& log) {
  bool paused = log.paused;
  for (const LogOp op : log.ops) {
    if (op == LogOp::Pause) paused = true;
    if (op == LogOp::Resume) paused = false;
  }
  return paused;
}

// Is the echo text `echo` (".  cap log off Main_log") the command line of a
// queued `op` aimed at `log`? The prefixes capture/quietly/noisily (and their
// abbreviations) are skipped; a target that is a macro cannot be compared and
// is taken to match. The tee uses this to put the footer after the right line
// when a container frame (a `do` file, a loop) carries many commands. `quiet`
// reports the prefixes' verdict on output: capture and quietly silence it,
// a later noisily restores it.
struct LogCommandEcho {
  bool matches = false;
  bool quiet = false;
};

// An echo line (".  cap noi display ...") split into words, with the command
// prefixes consumed: `first` is the index of the command word, `quiet` the
// prefixes' verdict (capture and quietly silence output, a later noisily
// restores it). A trailing ';' (delimiter mode) is dropped from each word.
struct EchoWords {
  std::vector<std::string> tok;
  size_t first = 0;
  bool quiet = false;
};
EchoWords smclSplitEcho(const std::string& echo) {
  EchoWords out;
  std::istringstream words(echo.size() > 1 && echo[0] == '.' ? echo.substr(1) : echo);
  for (std::string w; words >> w;) {
    while (!w.empty() && w.back() == ';') w.pop_back();
    out.tok.push_back(w);
  }
  auto abbrevOf = [](std::string w, const std::string& full, size_t minLen) {
    while (!w.empty() && w.back() == ':') w.pop_back();
    for (char& c : w) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return w.size() >= minLen && w.size() <= full.size() && full.compare(0, w.size(), w) == 0;
  };
  for (; out.first < out.tok.size(); ++out.first) {
    const std::string& w = out.tok[out.first];
    if (abbrevOf(w, "capture", 3) || abbrevOf(w, "quietly", 3)) {
      out.quiet = true;
    } else if (abbrevOf(w, "noisily", 2)) {
      out.quiet = false;
    } else {
      break;
    }
  }
  return out;
}

LogCommandEcho smclParseLogCommand(const std::string& echo, const LogState& log, LogOp op) {
  const EchoWords e = smclSplitEcho(echo);
  LogCommandEcho out;
  out.quiet = e.quiet;
  const size_t i = e.first;
  if (i + 1 >= e.tok.size() || e.tok[i] != "log") return out;
  const char* sub = op == LogOp::Pause ? "off" : op == LogOp::Resume ? "on" : "close";
  if (e.tok[i + 1] != sub) return out;
  if (i + 2 >= e.tok.size()) {
    out.matches = log.name.empty();
  } else {
    const std::string& target = e.tok[i + 2];
    out.matches = target == "_all" || target.find_first_of("`$") != std::string::npos ||
                  target == log.name;
  }
  return out;
}

// Did this command line come from `display` (or an abbreviation, `di` up)? Its
// output text is what the user typed, SMCL directives and all; the text any
// other command prints is plain and has its braces escaped.
bool smclEchoIsDisplay(const std::string& echo) {
  if (echo.empty() || echo[0] != '.') return false;
  const EchoWords e = smclSplitEcho(echo);
  if (e.first >= e.tok.size()) return false;
  const std::string& w = e.tok[e.first];
  return w.size() >= 2 && std::string("display").compare(0, w.size(), w) == 0;
}
}  // namespace

bool SessionState::openLog(const std::string& resolvedPath, bool appendMode,
                           const std::string& name, int depth, bool smcl) {
  // Defensive only — handlers_m3.cpp's runLog already refuses reopening a log
  // under a name that is already open, matching real Stata, so in normal use
  // there is nothing to drop here. Dropping (rather than e.g. asserting) keeps
  // this a total function.
  closeLog(name);
  // A `log close` of the same file that is still waiting for its footer must
  // write it before this open can truncate or extend the file.
  for (auto it = endingLogs_.begin(); it != endingLogs_.end();) {
    if ((*it)->path == resolvedPath) {
      smclDrainOps(**it);
      it = endingLogs_.erase(it);
    } else {
      ++it;
    }
  }
  auto st = std::make_unique<LogState>();
  st->path = resolvedPath;
  st->name = name;
  st->depth = depth;
  st->smcl = smcl;
  bool continuation = false;
  if (smcl && appendMode) {
    std::error_code ec;
    const auto size = std::filesystem::file_size(cecon::to_native_path(resolvedPath), ec);
    continuation = !ec && size > 0;
  }
  st->stream.open(cecon::to_native_path(resolvedPath),
                   std::ios::out | (appendMode ? std::ios::app : std::ios::trunc));
  if (!st->stream.is_open()) return false;
  if (smcl) {
    smclWriteBanner(*st, continuation, /*resumed=*/false);
    st->skipNextEcho = true;  // the `log using` line is not in the log
  }
  logs_.push_back(std::move(st));
  return true;
}

bool SessionState::closeLog(const std::string& name) {
  for (auto it = logs_.begin(); it != logs_.end(); ++it) {
    if ((*it)->name != name) continue;
    std::unique_ptr<LogState> log = std::move(*it);
    logs_.erase(it);
    if (!log->smcl) return true;  // ofstream's dtor flushes + closes
    log->ops.push_back(LogOp::Close);
    if (log->depth > dispatchDepth_) {
      // Opened deeper than the frame closing it: no later frame of that depth
      // will carry the command line, so the footer cannot wait for it.
      smclDrainOps(*log);
    } else {
      endingLogs_.push_back(std::move(log));
    }
    return true;
  }
  return false;
}

void SessionState::closeAllLogs() {
  for (const auto& n : logNames()) closeLog(n);
}

bool SessionState::pauseLog(const std::string& name) {
  for (auto& l : logs_) {
    if (l->name != name) continue;
    if (!l->smcl) {
      l->paused = true;
    } else if (!smclWillBePaused(*l)) {
      // The footer follows the `log off` command line, which logTee() writes
      // after this handler has returned -- unless no later frame of this log's
      // depth is coming (opened deeper than the frame pausing it).
      l->ops.push_back(LogOp::Pause);
      if (l->depth > dispatchDepth_) smclDrainOps(*l);
    }
    return true;
  }
  return false;
}

bool SessionState::resumeLog(const std::string& name) {
  for (auto& l : logs_) {
    if (l->name != name) continue;
    if (!l->smcl) {
      l->paused = false;
    } else if (smclWillBePaused(*l)) {
      // The resumed header stands where the `log on` line would (the line
      // itself is not recorded: the log was off when it ran).
      l->ops.push_back(LogOp::Resume);
      if (l->depth > dispatchDepth_) smclDrainOps(*l);
    }
    return true;
  }
  return false;
}

namespace {
// BS-006 (BUG-028): generic plain-text rendering of one dispatch() result
// block, for logTee() below. Deliberately NOT a byte-exact Stata SMCL
// renderer (see handlers_m3.cpp's runLog doc comment for the scope call —
// the gate is "real, sensible content", not fixed-width ASCII tables).
// "echo" (the ". command" echo line) and "display" (every scalar/`display`/
// note-style single-line result) already carry their own ready-made human
// text in a "text" field, used verbatim by logTee(); "error" blocks carry
// theirs in "message". Every OTHER block type (estimation/tabulate/
// summarize/browse/graph/describe/levelsof/table_result/data_mutation/
// _control) has no pre-rendered plain-text form ANYWHERE in this codebase —
// the webview frontend renders those straight from JSON — so jsonAsText()
// recursively dumps the block's own fields as indented "key: value" text:
// real field values (actual coefficients, row/column names, ...), not a
// stub placeholder, even though it isn't laid out like Stata's own tables.
std::string jsonScalarToText(const nlohmann::ordered_json& v) {
  if (v.is_null()) return ".";
  if (v.is_string()) return v.get<std::string>();
  if (v.is_boolean()) return v.get<bool>() ? "true" : "false";
  if (v.is_number_integer() || v.is_number_unsigned()) {
    return std::to_string(v.get<long long>());
  }
  if (v.is_number_float()) {
    std::ostringstream oss;
    oss << v.get<double>();
    return oss.str();
  }
  return v.dump();  // shouldn't happen for a scalar, but never throw
}

void jsonAsText(const nlohmann::ordered_json& v, std::ostream& out, const std::string& prefix) {
  if (v.is_object()) {
    for (auto it = v.begin(); it != v.end(); ++it) {
      if (it.key() == "type") continue;  // conveyed by logTee()'s own header line
      // F7 (B.3.0 "bounded transcripts"): never render a data_mutation block's
      // `varlist_after` array into the log. Stata logs nothing there, and this
      // single array was ~4.3 MB PER mutating verb — the dominant driver of the
      // incident run's 4.7 GB log. Skipping the KEY (rather than filtering a
      // copy) means the huge array is never even traversed/serialized here.
      // varlist_after appears only at a data_mutation block's top level, so a
      // blanket skip is safe (mirrors the "type" skip just above).
      if (it.key() == "varlist_after") continue;
      const auto& val = it.value();
      if (val.is_object() || val.is_array()) {
        out << prefix << it.key() << ":\n";
        jsonAsText(val, out, prefix + "    ");
      } else {
        out << prefix << it.key() << ": " << jsonScalarToText(val) << "\n";
      }
    }
  } else if (v.is_array()) {
    int idx = 0;
    for (const auto& elem : v) {
      if (elem.is_object() || elem.is_array()) {
        out << prefix << "[" << idx << "]\n";
        jsonAsText(elem, out, prefix + "    ");
      } else {
        out << prefix << "- " << jsonScalarToText(elem) << "\n";
      }
      ++idx;
    }
  } else {
    out << prefix << jsonScalarToText(v) << "\n";
  }
}

// A command line: a blank line in {txt}, then "{com}. cmd". PIET's prompt is
// ".  cmd" (two spaces); Stata's is ". cmd". An echo that does not start with
// "." is a note PIET printed (not a command) and stays {txt} output.
void smclWriteEcho(LogState& log, std::string t) {
  smclSetMode(log, kModeTxt);
  if (!t.empty() && t[0] == '.') {
    log.stream << "\n";
    if (t.compare(0, 3, ".  ") == 0) t.erase(1, 1);
    smclSetMode(log, kModeCom);
  }
  log.stream << smclEscape(t) << "\n";
}

// One non-echo block. `fromDisplay` = the command line before it was `display`.
void smclWriteBlock(LogState& log, const nlohmann::ordered_json& b, const std::string& type,
                    bool fromDisplay, const std::string& tabstatText, bool& consumedTabstat) {
  if (type == "display" && fromDisplay) {
    // The text is SMCL already -- it is what the user typed to `display`. If it
    // carries directives of its own it may have changed the mode, so forget it.
    const std::string text = b.value("text", std::string());
    smclSetMode(log, kModeRes);
    log.stream << text << "\n";
    if (text.find('{') != std::string::npos) log.smclMode = '?';
  } else if (type == "display") {
    // A line some other command printed (`type`, `macro list`, a note): plain
    // text, so a literal brace in it is escaped.
    smclSetMode(log, kModeRes);
    log.stream << smclEscape(b.value("text", std::string())) << "\n";
  } else if (type == "estimation" && b.value("estimator", std::string()) == "tabstat" &&
             !tabstatText.empty()) {
    smclSetMode(log, kModeTxt);
    log.stream << smclEscape(tabstatText);
    consumedTabstat = true;
  } else if (type == "error") {
    smclSetMode(log, kModeErr);
    log.stream << smclEscape(b.value("message", std::string("(error)"))) << "\n";
    if (!b.value("captured", false)) {
      // An error nothing captured ends the command with its return code, which
      // Stata links to the r() help; `capture noisily` prints none. "1" is the
      // generic bucket coarseRc() falls back to, not a code anything named.
      const std::string rc = eval::coarseRc(b);
      if (rc != "1") {
        smclSetMode(log, kModeTxt);
        log.stream << "{search r(" << rc << "), local:r(" << rc << ");}\n";
      }
    }
  } else {
    std::ostringstream dump;
    if (!type.empty()) dump << "[" << type << "]\n";
    jsonAsText(b, dump, "    ");
    smclSetMode(log, kModeTxt);
    log.stream << smclEscape(dump.str());
  }
}

// One dispatch frame's blocks into an SMCL log (the writer's layout is
// described above openLog). The quiet filters are the text path's own. A
// queued `log off` / `log on` / `log close` takes effect at the echo of its
// own command line -- wherever that falls in the frame, so a `do` file that
// pauses a log halfway pauses it halfway -- and at the frame's end if the
// line never appears. Returns true when the log was closed.
bool smclTeeFrame(LogState& log, const nlohmann::ordered_json& blocks, bool dropEcho,
                  bool dropOutput, const std::string& tabstatText, bool& consumedTabstat) {
  bool fromDisplay = false;  // is the command whose output this is `display`?
  for (const auto& b : blocks) {
    if (!b.is_object()) continue;
    const std::string type = b.value("type", std::string());
    if (type == "echo") fromDisplay = smclEchoIsDisplay(b.value("text", std::string()));
    if (type == "echo" && !log.ops.empty()) {
      const std::string t = b.value("text", std::string());
      const LogOp op = log.ops.front();
      const LogCommandEcho cmd = smclParseLogCommand(t, log, op);
      if (cmd.matches) {
        log.ops.erase(log.ops.begin());
        const bool quiet = cmd.quiet || dropOutput;
        if (op == LogOp::Resume) {
          if (log.paused) {  // the `log on` line itself is not recorded
            smclWriteBanner(log, /*continuation=*/true, /*resumed=*/true, quiet);
            log.paused = false;
          }
          continue;
        }
        if (!log.paused && !dropEcho) smclWriteEcho(log, t);
        if (!log.paused) smclWriteFooter(log, op == LogOp::Pause ? "paused" : "closed", quiet);
        if (op == LogOp::Close) return true;  // later blocks belong to someone else
        log.paused = true;
        continue;
      }
    }
    if (log.paused) continue;
    if (type == "echo" && log.skipNextEcho) {
      log.skipNextEcho = false;
      continue;
    }
    if (type == "echo" && dropEcho) continue;
    if (type != "echo" && type != "error" && dropOutput) continue;
    if (type == "echo") {
      smclWriteEcho(log, b.value("text", std::string()));
    } else {
      smclWriteBlock(log, b, type, fromDisplay, tabstatText, consumedTabstat);
    }
  }
  const bool closed = smclDrainOps(log);
  log.stream.flush();
  return closed;
}
}  // namespace

void SessionState::logTee(const nlohmann::ordered_json& blocks, int depth) {
  if ((logs_.empty() && endingLogs_.empty()) || !blocks.is_array()) {
    pendingTabstatLogText.clear();
    return;
  }
  // W:P8: every open, unpaused log whose depth matches this frame gets the
  // same transcript — Stata tees to all open logs, so a `name()`d log opened
  // mid-session records the commands that run while it is open without
  // stealing them from the outer log. See LogState::depth for why the frame
  // has to match rather than "any frame".
  // BUG-189: a quiet scope is invisible to the block filters at this point (they
  // run after dispatch returns), so the gate has to be asked here — see
  // SessionState::logEchoSuppressed for the measured Stata semantics. Errors are
  // never suppressed: `quietly` is output-only and does not swallow errors, and
  // a log that hides the error explaining why a run stopped is worse than a
  // noisy one.
  const bool dropEcho = logEchoSuppressed();
  const bool dropOutput = logOutputSuppressed();
  bool consumedTabstat = false;
  for (const auto& log : logs_) {
    // A paused SMCL log still reads the frame when a `log on` is waiting in it.
    if (log->paused && !(log->smcl && !log->ops.empty())) continue;
    if (log->depth != depth) continue;
    if (log->smcl) {
      smclTeeFrame(*log, blocks, dropEcho, dropOutput, pendingTabstatLogText, consumedTabstat);
      continue;
    }
    for (const auto& b : blocks) {
      if (!b.is_object()) continue;
      std::string type = b.value("type", std::string());
      if (type == "echo" && dropEcho) continue;
      if (type != "echo" && type != "error" && dropOutput) continue;
      if (type == "echo" || type == "display") {
        // A command echo stands after a blank line, as in Stata's own text
        // log (". cmd" lines are separated from the previous output by one;
        // the first line of a `quietly log using` log is that blank).
        const std::string t = b.value("text", std::string());
        if (type == "echo" && !t.empty() && t[0] == '.') log->stream << "\n";
        log->stream << t << "\n";
      } else if (type == "estimation" && b.value("estimator", std::string()) == "tabstat" &&
                 !pendingTabstatLogText.empty()) {
        log->stream << pendingTabstatLogText;
        consumedTabstat = true;
      } else if (type == "error") {
        log->stream << b.value("message", std::string("(error)")) << "\n";
        std::string code = b.value("code", std::string());
        if (!code.empty()) log->stream << "(" << code << ")\n";
      } else {
        if (!type.empty()) log->stream << "[" << type << "]\n";
        jsonAsText(b, log->stream, "    ");
      }
    }
    log->stream.flush();
  }
  // `log close`d SMCL logs: the frame at the log's own depth carries the
  // command line; the closed-on footer follows it and the file is released.
  for (auto it = endingLogs_.begin(); it != endingLogs_.end();) {
    if ((*it)->depth != depth) {
      ++it;
      continue;
    }
    smclTeeFrame(**it, blocks, dropEcho, dropOutput, pendingTabstatLogText, consumedTabstat);
    // The tee always finishes it: its request queue ends with the Close.
    it = endingLogs_.erase(it);
  }
  // Consumed by the log that wrote it; a tee at a frame no open log belongs to
  // (the dispatch wrapper's nested call) leaves it for the frame's own tee.
  if (consumedTabstat) pendingTabstatLogText.clear();
}

namespace {
// True iff `child` is lexically contained in `dir` (component-wise, after
// weakly_canonical) — stricter than a string prefix so ".../piet_native_ab"
// is never treated as under ".../piet_native_a".
bool isContainedIn(const std::filesystem::path& child, const std::filesystem::path& dir) {
  auto d = dir.begin();
  auto c = child.begin();
  for (; d != dir.end(); ++d, ++c) {
    if (c == child.end() || *c != *d) return false;
  }
  return c != child.end();  // child must have at least one component beyond dir
}
}  // namespace

namespace {
// Scratch-budget ceiling in bytes: env PIET_SCRATCH_BUDGET_MB overrides the
// default. "0" means evict immediately (no retained scratch — the eager policy).
// Read fresh each call so a test can flip it; the parse is trivial.
//
// The default was 4096 MB and is 0 as of 2026-08-31 (User ruling), because the
// census that measured what retention BUYS came back with "nothing":
//
//  - Functionally nothing, ONCE THE PIN LIST IS RIGHT — and it was not. The
//    claim "a path only reaches supersededScratch_ after passing every pin
//    exclusion, so nothing can read it again" was written here first and then
//    falsified by this codebase's own selftest within the hour: flipping the
//    default turned 8 checks red. Two pins were missing, and the 4096 MB
//    ceiling had been standing in for them — exclusions (7) LastModel's
//    dataPath/fitDataPath (every post-estimation replay) and (8) currentFile_
//    (a user file that resolves INSIDE the workdir). Both are now in
//    reclaimSupersededScratch with their own locks and seeded-RED certificates.
//    Read the claim as a property this file now MAINTAINS, not one it inherited:
//    a new reader of a superseded path must add its pin, and the eager default
//    is what makes that failure loud instead of a rare long-session mystery.
//  - Nothing measurable in wall. Two 47-artifact corpus runs, same workload:
//    eager 5016 s vs 4096 MB 4975 s — 0.8%, and the runs used different binaries
//    so even that sign is not quotable ([[iobound-storage-regime]]).
//  - It cost 3.9 GiB of peak. 8.916 vs 5.009 GiB high-water; the delta IS the
//    ceiling, to within rounding. Peak is what a user's free space must survive,
//    and the product promise is 10 GiB (internal note GA-SWEEP-ADJ1-MEMO.md A1), so
//    retention was spending 39% of the promise on files nothing reads.
//
// Full attribution: internal note GA-SWEEP-CENSUS-20260822.md ADDENDUM 6 and 7.
std::uintmax_t scratchBudgetBytes() {
  std::uintmax_t mb = 0;  // evict on supersede — retention buys nothing (above)
  if (const char* env = std::getenv("PIET_SCRATCH_BUDGET_MB"); env && *env) {
    char* end = nullptr;
    unsigned long long v = std::strtoull(env, &end, 10);
    if (end != env) mb = static_cast<std::uintmax_t>(v);
  }
  return mb * 1024ull * 1024ull;
}
}  // namespace

// True when `canonicalPath` (weakly canonical) is the on-disk base frame of the live e(sample)
// mask install: the head is still the install frame built from it. See exclusion
// (6c) in reclaimSupersededScratch and the "esample_base" claim in scratchOwners.
bool SessionState::isLiveEsampleBase(const std::string& canonicalPath) const {
  if (!esampleHeadIsInstallOf(esampleLastInstallBase)) return false;
  std::error_code ec;
  const std::filesystem::path bp =
      std::filesystem::weakly_canonical(cecon::to_native_path(esampleLastInstallBase), ec);
  return !ec && bp.u8string() == canonicalPath;
}

// 2.2.0 item E (SPEC II-0 ladder rung b): bounded scratch-budget eviction.
// REGISTER the superseded predecessor as reclaimable (subject to the safety
// exclusions), then evict the OLDEST registered files while the running total
// exceeds the budget. A short mutation burst stays under budget => zero
// deletion I/O on the critical path (Gate ② — an eager unlink contends for
// disk bandwidth with the next mutation's write, regressing p2_load3gen ~0.9s
// on the reference machine); a long session's scratch stays bounded.
//
// Safety exclusions (the "NEVER deletes user files" guarantee): register only
// a regular file that is (1) contained in workdir_ — the session-owned scratch
// dir, so a user's own .hbin/.dta anywhere else is untouchable — and (2) not
// referenced by any live preserve snapshot (preserveStack). Best-effort:
// every filesystem call is error_code'd, nothing throws into load().
void SessionState::reclaimSupersededScratch(const std::string& oldPath, bool abandoningHead) {
  if (oldPath.empty()) return;
  if (scratchTraceOn()) scratchTrace("RECLAIM", oldPath, "");
  // Debug/opt-out escape hatch (defaults ON — reclaiming the scratch
  // accumulation is the point of item E).
  if (const char* off = std::getenv("PIET_NO_SCRATCH_GC"); off && *off) return;

  namespace fs = std::filesystem;
  std::error_code ec;

  fs::path wd = fs::weakly_canonical(cecon::to_native_path(workdir_), ec);
  if (ec) return;
  fs::path op = fs::weakly_canonical(cecon::to_native_path(oldPath), ec);
  if (ec) return;

  // (0) BUG-390 (2026-09-02): never the LIVE head. Every earlier caller handed
  // this a path it had just superseded, so the primitive never needed to ask;
  // the byte-LRU over the `use` memos can evict the entry whose hbin is the
  // dataset the user is looking at right now (the memo for `use big1` is the
  // LRU victim the moment `use big2` memoizes, before big2 is adopted). The
  // path stays owned by the head and is reclaimed by load()'s ordinary
  // supersede when the head moves on.
  if (!abandoningHead && currentHbin_.has_value() && !currentHbin_->empty()) {
    fs::path hp = fs::weakly_canonical(cecon::to_native_path(*currentHbin_), ec);
    if (!ec && hp == op) {
      if (scratchTraceOn()) scratchTrace("SPARE(live-head)", "reclaim", oldPath);
      return;
    }
    if (ec) ec.clear();
  }
  // (1) session-owned scratch only — never a user file outside workdir_.
  if (!isContainedIn(op, wd)) {
    if (scratchTraceOn()) scratchTrace("SPARE(user-file)", "reclaim", oldPath);
    return;
  }
  // (2) never a live preserve snapshot (deep-copy files on the LIFO stack).
  for (const auto& frame : preserveStack) {
    fs::path pp = fs::weakly_canonical(cecon::to_native_path(frame.hbinPath), ec);
    if (!ec && pp == op) {
      if (scratchTraceOn()) scratchTrace("SPARE(preserve)", "reclaim", oldPath);
      return;
    }
  }
  // (3) PP-L2A / K1a: never a LIVE `use` conversion memo. Same shape as (2) —
  // the entry is the pin, so a memo that useConvCachePut has already replaced
  // (or that a stat mismatch retired) is reclaimable on its next supersede. A
  // cached hbin IS superseded on every following mutation (`use foo.dta` then
  // `gen`), so without this exclusion the budget eviction would delete exactly
  // the file the next `use foo.dta` wants to re-open — the cache would degrade
  // to a no-op under load, silently, which is the failure mode a pure
  // exists()-check fallback would hide rather than prevent.
  for (const auto& [src, entry] : useConvCache_) {
    (void)src;
    fs::path cp = fs::weakly_canonical(cecon::to_native_path(entry.hbinPath), ec);
    if (!ec && cp == op) return;
  }
  // (4) PP-L2B / K1b: never a LIVE `use` STASH memo's on-disk twin. For a
  // .dta/.csv source this is the same conversion output exclusion (3) already
  // pins (K1b memoizes alongside K1a), so this is belt-and-braces there; but it
  // makes K1b self-contained — its restore re-adopts hbinPath as a CLEAN head,
  // and a scratch-GC unlink of that twin between the stash and the re-`use`
  // would drop the restore to the dirty-virtual fallback (an extra
  // materialization the pin exists to prevent).
  for (const auto& [src, entry] : useStashCache_) {
    (void)src;
    fs::path cp = fs::weakly_canonical(cecon::to_native_path(entry.hbinPath), ec);
    if (!ec && cp == op) return;
  }
  // (5) P6 frames: never a LIVE frame's hbin. A frame is a named dataset the
  // user still owns — it is superseded the moment the session loads anything
  // else, which for frames is EVERY following command, so without this
  // exclusion the budget eviction deletes exactly the file the next
  // `frame F:`/`frlink`/`frget` needs. Same shape as (2)-(4): the entry is the
  // pin, so a frame that `frame drop` has already forgotten is reclaimable on
  // its next supersede.
  for (const auto& [name, path] : frames) {
    (void)name;
    fs::path fp = fs::weakly_canonical(cecon::to_native_path(path), ec);
    if (!ec && fp == op) return;
  }
  // (6) P6 resampling: never a file the CURRENTLY RUNNING command pinned. A
  // resampling prefix writes several scratch files per replicate, so its own
  // base dataset becomes the oldest entry and would be the first evicted —
  // while the loop is still drawing from it. See SessionState::ScratchPin.
  for (const auto& pinned : scratchPins) {
    fs::path pp2 = fs::weakly_canonical(cecon::to_native_path(pinned), ec);
    if (!ec && pp2 == op) return;
  }
  // (6b) e(sample) T2: never a head an estimation's mask install has just
  // moved off while the command that ran it may still hold the path
  // (SessionState::pinPreInstallHead).
  for (const auto& pinned : preInstallPins_) {
    fs::path pp3 = fs::weakly_canonical(cecon::to_native_path(pinned), ec);
    if (!ec && pp3 == op) return;
    if (ec) ec.clear();
  }
  // (6c) e(sample) T2: never the on-disk BASE frame of the live mask install.
  // While the head is still the install frame built from `esampleLastInstallBase`
  // (esampleHeadIsInstallOf) the next fit and the read-only replays alias the
  // base instead of writing the head to disk (handlers.cpp esampleInstallOnHead,
  // handlers.cpp's `if` filter source, runViaCobraSession's esample aliases).
  // The base is owned by the head, not by the model: `ereturn clear` (any
  // lastModel_reset) releases the LastModel pin (7) while the head still IS the
  // install of this base, and the crash-floor reclaim (flushDeferredReclaim, at
  // the next materialization) then offers the file with nothing left to spare it;
  // the detached unlink raced the next fit's `native_exists()` check into
  // "[data_io] cannot open .../gen_*.hbin", rc 1 (330-370 of 900 fits in 30
  // runs of a 30-fit `regress ... if ..., vce(...)` loop with an `ereturn clear`
  // between each; 0 with PIET_NO_SCRATCH_GC=1). The
  // entry is the pin: once the head moves (load()), esampleHeadIsInstallOf turns
  // false and the orphan sweep takes the file at the next dispatch exit.
  if (isLiveEsampleBase(op.u8string())) {
    if (scratchTraceOn()) scratchTrace("SPARE(esample-base)", "reclaim", oldPath);
    return;
  }
  // (7) BUG-381: never a file the LIVE estimation still names. `predict`,
  // `test`, `lincom`, `margins`, `boottest` and the resampling prefixes all
  // replay LastModel::dataPath / ::fitDataPath — paths captured when the model
  // was FIT — back to the engine as `--data` / `--fit-data`. Those files are
  // superseded the moment anything mutates the head, which for `regress y x` +
  // `predict a, xb` + `predict b, resid` is the FIRST predict, so without this
  // exclusion the second one reads a path that has already been unlinked.
  //
  // session_state.h says of these fields "setLastModel() replaces the struct
  // wholesale, so none of these can outlive the model they describe". That is
  // an argument about the STRUCT's lifetime and it is correct; it is not an
  // argument about the FILE's, and the two came apart here. The entry is the
  // pin, exactly as in (2)-(6): setLastModel replacing the struct (or
  // lastModel_reset) releases the path on its next supersede.
  //
  // This was always the contract — the 4096 MB budget merely made the window
  // wide enough to hide it. A session that pushed more than the ceiling of
  // scratch between the fit and the post-estimation command hit it anyway.
  if (lastModel_.has_value()) {
    for (const std::string* mp : {&lastModel_->dataPath, &lastModel_->fitDataPath}) {
      if (mp->empty()) continue;
      fs::path lp = fs::weakly_canonical(cecon::to_native_path(*mp), ec);
      if (!ec && lp == op) return;
      if (ec) ec.clear();
    }
  }
  // (8) BUG-381: never the user's OWN loaded file. Exclusion (1) refuses user
  // files by LOCATION — "outside workdir_" — which is the right test only while
  // no user file is inside it. `use <f>` where <f> resolves into the session
  // workdir gives a head that is simultaneously a user file and, by (1)'s test,
  // session scratch; the next `replace` supersedes it and the GC deletes the
  // user's data. currentFile_ is the load's ORIGINAL resolved path, so it is
  // the pin that states ownership directly instead of inferring it from where
  // the file happens to sit.
  if (!currentFile_.empty()) {
    fs::path cf = fs::weakly_canonical(cecon::to_native_path(currentFile_), ec);
    if (!ec && cf == op) return;
    if (ec) ec.clear();
  }
  // Defensive: only track an existing regular file (never a dir/symlink).
  if (!fs::is_regular_file(op, ec)) {
    if (scratchTraceOn()) scratchTrace("SPARE(no-file)", "reclaim", oldPath);
    return;
  }

  std::uintmax_t sz = fs::file_size(op, ec);
  if (ec) sz = 0;
  if (scratchTraceOn()) scratchTrace("REGISTER", "reclaim", op.u8string());
  supersededScratch_.emplace_back(op.u8string(), sz);
  supersededBytes_ += sz;

  // Evict oldest-first while over budget. Eviction is off the critical path
  // (a detached unlink) AND only happens once the retained scratch genuinely
  // exceeds the ceiling — so the common short-burst case never touches disk.
  const std::uintmax_t budget = scratchBudgetBytes();
  while (supersededBytes_ > budget && !supersededScratch_.empty()) {
    auto victim = supersededScratch_.front();
    supersededScratch_.erase(supersededScratch_.begin());
    supersededBytes_ -= std::min(supersededBytes_, victim.second);
    std::thread([p = victim.first]() {
      if (scratchTraceOn()) scratchTrace("UNLINK", "reclaim-thread", p);
      std::error_code rmEc;
      std::filesystem::remove(p, rmEc);  // best-effort — a failed unlink is not fatal.
    }).detach();
  }
}

// PP-L2A / K1a. Definition lives here (not the header) because releasing a
// superseded memo needs reclaimSupersededScratch, which is defined above.
void SessionState::useConvCachePut(const std::string& src, const UseConvCacheEntry& entry) {
  if (entry.statKey.empty() || entry.hbinPath.empty()) return;  // unkeyable => not cacheable
  auto it = useConvCache_.find(src);
  if (it != useConvCache_.end()) {
    const std::string stale = it->second.hbinPath;
    useConvCache_.erase(it);  // drop the exclusion-(3) pin FIRST ...
    if (!stale.empty() && stale != entry.hbinPath) {
      reclaimSupersededScratch(stale);  // ... so the ordinary GC can now take it
    }
  }
  useConvCache_[src] = entry;
  useCacheTouch_[src] = ++useCacheClock_;  // BUG-390
  enforceUseCacheCap();
}

// ── BUG-198: the .hbin format/label carriage plane (see session_state.h) ─────

std::vector<cecon::ColMetaPatch> SessionState::hbinMetaPatch(
    const std::vector<VarInfo>& card) const {
  std::vector<cecon::ColMetaPatch> out;
  for (const VarInfo& v : card) {
    auto it = varMeta.find(v.name);
    if (it == varMeta.end()) continue;
    cecon::ColMetaPatch p;
    // Engage a key ONLY when this session actually declared it. A column with a
    // label override and no format override must leave the file's own `format`
    // key alone — engaging it with the card's rendered value would write
    // VarInfo::format's "%9.0g" member-initializer default into the header and
    // turn "unstated" into a fabricated value, which is precisely the
    // absent-vs-empty law this key inherited from P1-A0.
    if (it->second.format.has_value()) p.format = *it->second.format;
    if (it->second.label.has_value()) p.label = *it->second.label;
    if (!p.format.has_value() && !p.label.has_value()) continue;
    p.name = v.name;
    out.push_back(std::move(p));
  }
  return out;
}

bool SessionState::hasHbinMetaOverrides(const std::vector<VarInfo>& card) const {
  if (varMeta.empty()) return false;  // the common case, answered without a walk
  for (const VarInfo& v : card) {
    auto it = varMeta.find(v.name);
    if (it == varMeta.end()) continue;
    if (it->second.format.has_value() || it->second.label.has_value()) return true;
  }
  return false;
}

void SessionState::seedVarMetaFromLoadedFile(Datastore* ds) {
  if (ds == nullptr) return;
  for (const VarInfo& v : ds->varlist()) {
    if (!ds->has(v.name)) continue;
    // The RAW header strings, deliberately — NOT VarInfo::format, which has
    // already had its "%9.0g" member-initializer default substituted for a
    // column the file said nothing about. Adopting that default would
    // manufacture an override out of the file's silence and then write it back
    // into the next save's header, turning "unstated" into a fabricated value
    // for every column of every dataset. "" = the key was absent (P1-A0 /
    // BUG-198 law) and contributes nothing.
    const cecon::Column& c = ds->col(v.name);
    if (!c.format.empty()) varMeta[v.name].format = c.format;
    if (!c.label.empty()) varMeta[v.name].label = c.label;
  }
}

void SessionState::seedVarMetaFromVirtualSave(const std::string& path) {
  auto it = virtualSaveMeta_.find(path);
  if (it == virtualSaveMeta_.end()) return;
  for (const cecon::ColMetaPatch& p : it->second) {
    // "" here means "the override was cleared", exactly as it means "erase the
    // key" on the disk arm — so it must not be adopted as a value.
    if (p.format.has_value() && !p.format->empty()) varMeta[p.name].format = *p.format;
    if (p.label.has_value() && !p.label->empty()) varMeta[p.name].label = *p.label;
  }
}

void SessionState::adoptVirtualSaveFormats(const std::string& path,
                                           const std::vector<std::string>& names) {
  auto it = virtualSaveMeta_.find(path);
  if (it == virtualSaveMeta_.end()) return;
  const std::set<std::string> want(names.begin(), names.end());
  for (const cecon::ColMetaPatch& p : it->second) {
    if (!want.count(p.name) || !p.format.has_value() || p.format->empty()) continue;
    VarMeta& vm = varMeta[p.name];
    if (!vm.format.has_value() || vm.format->empty()) vm.format = *p.format;
  }
}

// format-display-default-lifecycle: see the declaration (session_state.h).
std::string SessionState::defaultDisplayFormat(const std::optional<std::string>& stataType,
                                               bool isString) {
  if (!isString) return storage::stataDefaultFormat(stataType);
  // str# displays %#s with a minimum width of 9 (str1..str9 %9s, str10 %10s,
  // str20 %20s in the captured describe blocks; str5 %9s,
  // w2p_estim_ev.log:1827); strL and a width nothing recorded display %9s.
  if (stataType.has_value() && stataType->size() > 3 && stataType->compare(0, 3, "str") == 0) {
    const std::string digits = stataType->substr(3);
    if (std::all_of(digits.begin(), digits.end(),
                    [](unsigned char c) { return std::isdigit(c) != 0; })) {
      const unsigned long w = std::stoul(digits);
      return "%" + std::to_string(std::max<unsigned long>(9, w)) + "s";
    }
  }
  return "%9s";
}

std::string SessionState::displayFormat(const VarInfo& v) const {
  auto it = varMeta.find(v.name);
  if (it != varMeta.end() && it->second.format.has_value() && !it->second.format->empty())
    return *it->second.format;
  if (v.formatStated) return v.format;
  std::optional<std::string> type;
  if (it != varMeta.end() && it->second.stataType.has_value() && !it->second.stataType->empty())
    type = *it->second.stataType;
  return defaultDisplayFormat(type, v.dtype == "factor");
}

std::vector<std::pair<std::string, std::string>> SessionState::displayFormatMap(
    const std::vector<VarInfo>& card) const {
  std::vector<std::pair<std::string, std::string>> out;
  out.reserve(card.size());
  for (const VarInfo& v : card) out.emplace_back(v.name, displayFormat(v));
  return out;
}

void SessionState::pinDisplayFormat(const VarInfo& v) {
  auto it = varMeta.find(v.name);
  if (it != varMeta.end() && it->second.format.has_value() && !it->second.format->empty()) return;
  varMeta[v.name].format = displayFormat(v);
}

void SessionState::prepareStorageChange(const VarInfo& v) {
  if (v.dtype != "factor") {
    pinDisplayFormat(v);
    return;
  }
  auto it = varMeta.find(v.name);
  if (it == varMeta.end() || !it->second.format.has_value()) return;
  std::optional<std::string> type;
  if (it->second.stataType.has_value() && !it->second.stataType->empty())
    type = *it->second.stataType;
  if (*it->second.format == defaultDisplayFormat(type, true)) it->second.format.reset();
}

std::vector<VarInfo> SessionState::varlistWithMeta(Datastore* ds) const {
  if (ds == nullptr) return {};
  return varlistWithMetaFrom(ds->varlist());
}

// BUG-208. See the doc comment on the declaration for the design question this
// answers (why a projection and not a mirror-on-write, and not a layering
// inversion). Kept deliberately narrow: it refreshes eval.varLabels and touches
// nothing else, so the evalcase harness — which builds STANDALONE EvalSession
// objects that no SessionState owns — is unaffected and its pins keep meaning
// what they meant.
void SessionState::syncEvalVarLabels() {
  // Prefer the resident card: it already carries the resolved label and costs no
  // Datastore construction. nullptr means "not current" and the contract is an
  // honest fallback to the datastore, never trusting a stale card.
  const std::vector<VarInfo>* card = residentReadCard();
  std::vector<VarInfo> resolved;
  if (card != nullptr) {
    resolved = varlistWithMetaFrom(*card);
  } else {
    Datastore* ds = datastore();
    if (ds == nullptr) {
      // No dataset in memory: there are no variable labels to project. Clearing
      // is the honest answer — a label from a previous dataset must not survive
      // into a session that has dropped it.
      eval.varLabels.clear();
      // BUG-216/217: the plane is still AUTHORITATIVE — "no data" means "no
      // variables", so every `: format x` / `: variable label x` here is a
      // missing variable and Stata's r(111) is the right answer. Omitting the
      // sentinel would silently reopen the old lenient "" on exactly the case
      // where the reject is most obviously correct.
      eval.varLabels.set(kEvalVarPlaneKey, "1");
      return;
    }
    resolved = varlistWithMeta(ds);
  }
  // Rebuilt wholesale rather than merged: this is a projection, so a variable
  // that has been dropped or had its label cleared must DISAPPEAR from it. A
  // merge would let a stale label outlive the fact it describes, which is the
  // same class of defect as the one being fixed.
  eval.varLabels.clear();
  // BUG-216/217: mark the plane authoritative BEFORE the rows, so a zero-variable
  // dataset is still "authoritatively empty" rather than "never synced".
  eval.varLabels.set(kEvalVarPlaneKey, "1");
  for (const VarInfo& v : resolved) {
    if (!v.label.empty()) eval.varLabels.set(v.name, v.label);
    // BUG-216/217: the same wholesale rebuild, three more facts per variable.
    //
    // The EXISTENCE marker is the load-bearing one and it is unconditional —
    // that is the whole point. The label row above is still conditional (an
    // empty label is not stored, so `: variable label x` keeps answering ""
    // for a variable that exists and has none, which is Stata-faithful and
    // byte-identical to BUG-208's behaviour). What was missing is the ability
    // to tell that case apart from "no such variable", which Stata answers with
    // r(111). The marker supplies exactly that distinction and nothing else.
    eval.varLabels.set(std::string(kEvalVarExistsPrefix) + v.name, "1");
    // Format: the one resolver (displayFormat). `resolved` came through
    // varlistWithMetaFrom, which already overlays it; reading v.format is that
    // answer, and it is the same one describe, codebook and list report.
    auto fmtIt = varMeta.find(v.name);
    const std::string& resolvedFmt = v.format;
    eval.varLabels.set(std::string(kEvalVarFormatPrefix) + v.name, resolvedFmt);
    // BUG-250: declared storage subtype, describe's own resolution order —
    // recorded declaration first (creation arms + .dta import), then the
    // numeric default "float" (Stata's own `gen` default; also exactly what
    // `describe` answers for the same column). A string column with no
    // recorded width projects NOTHING on purpose — see the header's note.
    if (fmtIt != varMeta.end() && fmtIt->second.stataType.has_value() &&
        !fmtIt->second.stataType->empty()) {
      eval.varLabels.set(std::string(kEvalVarTypePrefix) + v.name, *fmtIt->second.stataType);
    } else if (v.dtype != "factor") {
      eval.varLabels.set(std::string(kEvalVarTypePrefix) + v.name, "float");
    }
    // Value-label SET NAME (`: value label x` answers the name, not the
    // mapping). Absent from varLblname = no value label attached, and Stata
    // answers "" with rc 0 for that — so the key is simply not written, and the
    // reader's default supplies "".
    auto lblIt = varLblname.find(v.name);
    if (lblIt != varLblname.end() && !lblIt->second.empty()) {
      eval.varLabels.set(std::string(kEvalVarLblsetPrefix) + v.name, lblIt->second);
    }
  }
}

std::vector<VarInfo> SessionState::varlistWithMetaFrom(const std::vector<VarInfo>& base) const {
  std::vector<VarInfo> result;
  result.reserve(base.size());
  for (const auto& v : base) {
    VarInfo entry = v;
    auto it = varMeta.find(v.name);
    if (it != varMeta.end() && it->second.label.has_value()) {
      entry.label = *it->second.label;
    }
    // format-display-default-lifecycle: the card states the format every
    // reader reports (displayFormat), not VarInfo's member default.
    entry.format = displayFormat(v);
    auto lblIt = varLblname.find(v.name);
    if (lblIt != varLblname.end()) {
      auto defIt = labelDefs.find(lblIt->second);
      if (defIt != labelDefs.end() && entry.dtype == "factor" && !entry.levels.empty()) {
        // Copy-on-write (A12): entry.levels is a non-owning VIEW into the
        // column's shared level dictionary. Remap a PRIVATE owned copy and
        // install it, so this value-label rewrite never mutates the shared
        // strings that var_detail and other cards read. Byte-identical to the
        // pre-A12 in-place rewrite of the (then-owned) per-entry copy.
        std::vector<std::string> remapped;
        remapped.reserve(entry.levels.size());
        for (std::size_t i = 0; i < entry.levels.size(); ++i) {
          const std::string& lv = entry.levels[i];
          auto mIt = defIt->second.find(lv);
          remapped.push_back(mIt != defIt->second.end() ? mIt->second : lv);
        }
        entry.levels = std::move(remapped);  // CoW install (owned); shared view untouched
      }
    }
    result.push_back(std::move(entry));
  }
  return result;
}

void SessionState::seedNMissForPureAdd(Datastore* ds, const std::string& action,
                                       const std::vector<std::string>& newVars,
                                       const std::vector<std::string>& droppedVars,
                                       const std::vector<std::string>& rewrittenVars) {
  if (std::getenv("PIET_NO_NMISS_CACHE")) return;  // C2-F13a escape hatch + A/B correctness gate
  if (ds == nullptr) return;

  // Start from the full cached table; each branch below carries the columns it
  // can PROVE byte-identical and drops/rescans the rest. Anything not matched
  // hits the final return and rescans fully — the conservative default.
  std::unordered_map<std::string, int64_t> seed = nMissCache_;

  // (1) C2-F13a pure-ADD {gen,egen,duplicates_tag,recode-gen}: adds newVars and
  //     leaves every existing column byte-identical. Carry all; new cols scan
  //     fresh. (Same condition as the pre-F9 code — behaviour unchanged here.)
  //     C1-CARD-REBUILD-REDUCE A11 siblings: encode/destring-gen/tostring-gen
  //     (mutationResult path) and winsor2/mkspline (hand-rebuild path) are all
  //     the SAME shape — the engine (encode/setcol/eval --winsor/--mkspline)
  //     appends or in-place-patches only the reported column(s) and byte-
  //     preserves every other column AND its dictionary. winsor2 passes the
  //     rewritten source(s) IN newVars (its emitted new_vars already lists them,
  //     replace or suffix), so the `erase(nv)` below rescans exactly the changed
  //     column and carries the rest. Proven byte-identical against a forced full
  //     scan (PIET_NO_NMISS_CACHE=1) per verb — the same A/B gate string
  //     gen/replace ships under.
  static const std::set<std::string> kPureAdd = {
      "gen", "egen", "duplicates_tag", "recode",
      "encode", "destring", "tostring", "winsor2", "mkspline"};
  if (!newVars.empty() && droppedVars.empty() && kPureAdd.count(action)) {
    for (const auto& nv : newVars) seed.erase(nv);  // new/changed cols scanned fresh
    ds->seedNMissing(std::move(seed));
    return;
  }

  // (2) F9 replace-style (empty newVars): rewrites rewrittenVars IN PLACE and
  //     touches nothing else (replace never adds/removes rows). Carry all EXCEPT
  //     the rewritten target(s). DORMANT until the call site passes rewrittenVars
  //     — with it empty we cannot tell WHICH column changed, so we fall through
  //     to a full rescan and never seed a stale count onto the rewritten column
  //     (the exact pre-F9 `replace` behaviour). Actions: string gen/replace
  //     (strmutate, action "gen") plus the C1 A11 in-place siblings destring
  //     replace / tostring replace (single-column `cobra setcol --var TARGET`,
  //     every other column byte-identical) — all row-invariant, single-column
  //     rewrites that pass rewrittenVars={target}.
  static const std::set<std::string> kRewriteInPlace = {"gen", "destring", "tostring"};
  if (kRewriteInPlace.count(action) && newVars.empty() && !rewrittenVars.empty()) {
    for (const auto& rv : rewrittenVars) seed.erase(rv);  // rewritten col scans fresh
    ds->seedNMissing(std::move(seed));
    return;
  }

  // (3) F9 variable drop/keep (action "drop"/"keep": a COLUMN subset, rows
  //     untouched). Every SURVIVING column is byte-identical — carry its count
  //     and just drop the removed keys. The observation filters `drop if`/
  //     `keep if` use action "keepif" (row-changing) and correctly miss this
  //     branch. `runKeep` already passes droppedVars = the REMOVED (not kept)
  //     columns, so the same erase-the-removed-keys logic serves both.
  if ((action == "drop" || action == "keep") && newVars.empty()) {
    for (const auto& dv : droppedVars) seed.erase(dv);
    ds->seedNMissing(std::move(seed));
    return;
  }

  // (4) F9 rename (action "rename", newVars={new}, droppedVars={old}): pure key
  //     move — the column data is byte-identical, only its name changed. Move
  //     the cached count old->new and carry the rest. If `old` was not cached
  //     we have nothing trustworthy to carry for `new`, so seed nothing.
  if (action == "rename" && newVars.size() == 1 && droppedVars.size() == 1) {
    auto it = seed.find(droppedVars[0]);
    if (it != seed.end()) {
      int64_t carried = it->second;
      seed.erase(it);
      seed[newVars[0]] = carried;
      ds->seedNMissing(std::move(seed));
    }
    return;
  }

  // (5) L4 O(1)-cards: reorderers sort/gsort (rows permuted, action "sort") and
  //     order (columns permuted, action "order"). A permutation of the rows or
  //     of the columns leaves every surviving column byte-identical — the same
  //     values, hence the same missing-count, level set, and type — and neither
  //     verb adds, drops, or filters rows (runOrder forwards only
  //     --first/--last/--before/--after, never --keepif; runSort/runGsort
  //     forward a bare varlist). Carry the whole cache; nothing is rescanned.
  //     Without this, the card rebuild's varlist() faults in and NaN-scans all N
  //     columns per reorder — the full-frame "card tax" (~1.9 s on the 108K×192
  //     corpus, ~35x the 8-col cost) this branch removes.
  if (action == "sort" || action == "order") {
    ds->seedNMissing(std::move(seed));
    return;
  }

  // Everything else: full rescan (seed nothing). Genuinely row- or column-set-
  // changing verbs (merge, reshape, expand, collapse, keepif, append, …)
  // deliberately land here.
}

// ===========================================================================
// MAXPERF Stage 1b: persistent `cobra serve` worker intercept + resident-frame
// data plane. See session_state.h for the field contract and cobra_session.h
// for the wire protocol. All of this is inert (residentToken_ stays empty,
// tryResidentMutationCard returns nullopt) until the first successful frame op,
// so a §25-only engine, PIET_NO_SERVE, or any non-dispatch call path stays
// byte-identical to the pre-Stage-1b one-shot behavior.
// ===========================================================================
namespace {
bool serveDisabled() {
  static const bool off = [] {
    const char* e = std::getenv("PIET_NO_SERVE");
    return e && *e;
  }();
  return off;
}
}  // namespace

// C4-L2A escape hatch (doc in session_state.h). Per-call getenv on purpose —
// the selftest A/B leg setenv/unsetenv's it around single dispatches, which a
// serveDisabled()-style static cache would silently ignore.
bool residentReadDisabled() {
  const char* e = std::getenv("PIET_NO_RESIDENT_READ");
  return e && *e;
}

CobraSession* SessionState::ensureCobraSession() {
  if (!cobraSession_) cobraSession_ = std::make_unique<CobraSession>(resolveCobraBin());
  return cobraSession_.get();
}

void SessionState::flushDeferredReclaim() {
  if (deferredReclaim_.empty()) return;
  std::vector<std::string> pending;
  pending.swap(deferredReclaim_);
  for (const auto& p : pending) reclaimSupersededScratch(p);
}

// e(sample) T2 -- see the declaration (session_state.h). The reading keys on
// lastModel_, whose only producers of "no model" are a fresh session and
// lastModel_reset() (`ereturn clear`, `clear all`/`clear results`/`clear
// matrix`, and the clearEResults that opens `ereturn post`) -- the same events
// that empty the e() stores. No model is 0 (w2_esample.log:95-129 A1-A5, :1634
// G11), and so is a POSTED e(), which leaves eval.lastModelResult set with no
// model behind it: a posted e() has no estimation sample (:1487 G5 |0|0|.|).
// Why not the macro layer's absent-e() predicate (resolveEScalar,
// eval/macros.cpp: `!session.lastModelResult`): the two agree on every event
// that installs or clears e(), but NOT on a model installed without a result
// JSON -- `pca` (handlers_aer_estim.cpp runPca: `LastModel lm; ...
// setLastModel(lm)` with lm.result unset). There resolveEScalar reads "no e()"
// while a model is live, and e(sample) = 0 would be a silent answer for a fit
// whose sample PIET does not have; keyed on lastModel_, it is the named r(198)
// of kUnavailable. The macro layer reads THIS function through
// EvalSession::esampleObs1 and uses its own predicate only when no session is
// attached.
SessionState::EsampleReading SessionState::esampleReading() {
  EsampleReading r;
  if (!lastModel_.has_value()) {
    // kZero: no model, or a posted e() without esample(). A posting WITH
    // esample() (handlers_ereturn.cpp runPost) reads its own mask column, under
    // the same cell checks a fit's mask gets below.
    if (!ePostedLive || ePostedEsampleSerial == 0 || !hasData()) return r;
    Datastore* ds = nullptr;
    try {
      ds = datastore();
    } catch (const std::exception& e) {
      r.kind = EsampleReading::Kind::kRefused;
      r.refusal = std::string("e(sample) cannot be read: ") + e.what();
      return r;
    }
    if (ds == nullptr || !ds->has(kEsampleColumn)) return r;  // the marker left with the data
    const cecon::Column& c = ds->col(kEsampleColumn);
    bool foreign = c.dtype != cecon::DType::F64;
    if (!foreign) {
      const double want = static_cast<double>(ePostedEsampleSerial);
      const double* v = c.f64();
      const int64_t n = ds->nRows();
      for (int64_t i = 0; i < n && !foreign; ++i)
        if (!std::isnan(v[i]) && v[i] != 0.0 && v[i] != want) foreign = true;
    }
    if (foreign) {
      r.kind = EsampleReading::Kind::kRefused;
      r.refusal = "e(sample) is not available in this version of PIET here: the "
                  "estimation-sample marker on the data belongs to other results than the "
                  "posted ones (`ereturn post ..., esample()`).";
      return r;
    }
    r.kind = EsampleReading::Kind::kColumn;
    r.serial = ePostedEsampleSerial;
    return r;
  }
  const LastModel& lm = *lastModel_;
  const std::string cmd = lm.stataCmd.empty() ? lm.cobraCmd : lm.stataCmd;
  switch (lm.esample) {
    case LastModel::EsampleState::kNone:
      return r;
    case LastModel::EsampleState::kColumn: {
      // The column follows the rows through every row verb because it IS a
      // column; a verb that replaces the data (use, clear, collapse, reshape,
      // drop _all) takes it away, and Stata's marker goes with the data too.
      if (!hasData()) return r;
      Datastore* ds = nullptr;
      try {
        ds = datastore();
      } catch (const std::exception& e) {
        // A head that cannot be opened (BUG-124/BUG-384) is named, never read
        // as "no column" -> 0, and never thrown past `capture`.
        r.kind = EsampleReading::Kind::kRefused;
        r.refusal = std::string("e(sample) cannot be read: ") + e.what();
        return r;
      }
      if (ds == nullptr || !ds->has(kEsampleColumn)) return r;  // the marker left with the data
      // The column on the rows may be another fit's (see LastModel::
      // esampleSerial): a nonzero cell that is not this model's serial means
      // the working frame came back from somewhere this model's rows never
      // were. Named, never counted. One pass over an f64 column.
      const cecon::Column& c = ds->col(kEsampleColumn);
      const double want = static_cast<double>(lm.esampleSerial);
      bool foreign = c.dtype != cecon::DType::F64 || lm.esampleSerial == 0;
      double lost = 0.0;  // < 0: a row verb PIET does not follow (piet/datastore.h EsampleLostBy)
      if (!foreign) {
        const double* v = c.f64();
        const int64_t n = ds->nRows();
        for (int64_t i = 0; i < n; ++i) {
          if (!std::isnan(v[i]) && v[i] < 0.0) {
            lost = v[i];
            break;
          }
          if (!std::isnan(v[i]) && v[i] != 0.0 && v[i] != want) foreign = true;
        }
      }
      if (lost < 0.0) {
        r.kind = EsampleReading::Kind::kRefused;
        r.refusal = std::string("e(sample) is not available after `") + esampleLostByVerb(lost) +
                    "` in this version of PIET: that command rebuilt the observations in a way "
                    "whose estimation-sample marker PIET cannot reproduce (Stata keeps it on one "
                    "row per group or per original observation, chosen by its own sort order).";
        return r;
      }
      if (foreign) {
        r.kind = EsampleReading::Kind::kRefused;
        r.refusal = "e(sample) is not available after " + cmd +
                    " in this version of PIET here: the estimation-sample marker on the data "
                    "belongs to another estimation (the data in memory is not the data these "
                    "results were fitted on, e.g. after a bootstrap/jackknife/permute prefix).";
        return r;
      }
      r.kind = EsampleReading::Kind::kColumn;
      r.serial = lm.esampleSerial;
      return r;
    }
    case LastModel::EsampleState::kStale:
      // T3: `estimates restore` reads e(sample) back from `_est_NAME`
      // (eststo_esttab.cpp runEstimatesRestore); the variable is gone (dropped,
      // or the data were replaced since the store).
      r.kind = EsampleReading::Kind::kRefused;
      r.refusal = "e(sample) is not available after `estimates restore` in this version of "
                  "PIET: the variable " +
                  (activeEstimatesName.empty() ? std::string("_est_<name>")
                                               : "_est_" + activeEstimatesName) +
                  " that holds these results' estimation sample is no longer in the data.";
      return r;
    case LastModel::EsampleState::kUnavailable:
      break;
  }
  r.kind = EsampleReading::Kind::kRefused;
  r.refusal = "e(sample) is not available after " + cmd +
              " in this version of PIET: the estimation sample of these results was not "
              "recorded (the engine does not report it for this command, or the results were "
              "stored from such a model and brought back by `estimates restore`).";
  return r;
}

std::optional<int> SessionState::esampleObs1(std::string* why) {
  const EsampleReading r = esampleReading();
  if (r.kind == EsampleReading::Kind::kRefused) {
    if (why != nullptr) *why = r.refusal;
    return std::nullopt;
  }
  if (r.kind == EsampleReading::Kind::kZero) return 0;
  // esampleReading just opened the head and found the column on it.
  Datastore* ds = datastore();
  if (ds == nullptr || ds->nRows() < 1) return 0;  // B29-style read of an empty frame
  const cecon::Column& c = ds->col(kEsampleColumn);
  if (c.dtype != cecon::DType::F64) return 0;
  // An admitted row holds the install's serial; 0 and missing (a row added
  // after the fit) read 0.
  return c.f64()[0] == static_cast<double>(r.serial) ? 1 : 0;
}

std::optional<eval::EvalSession::VarObsReading> SessionState::macroVarObs(
    const std::string& name, long long idx) {
  std::optional<VarObservation> o = readVarObservation(name, idx, *this);
  if (!o) return std::nullopt;
  return eval::EvalSession::VarObsReading{o->text, o->isString, o->inRange};
}

std::optional<std::string> SessionState::macroCValue(const std::string& name) {
  std::optional<CValue> v = resolveCValue(name, *this);
  if (!v) return std::nullopt;
  return v->text;
}

void SessionState::seedShadowFromFrameVars(const CobraFrameMeta& meta) {
  const std::vector<CobraFrameVar>& vars = meta.vars;
  // BUG-190. The frame meta the worker sends back describes DATA — name, type,
  // missing count, level count. It carries no display FORMAT and no variable
  // LABEL, because the engine has no such concept: those live entirely on the
  // PIET side. Rebuilding the card from it therefore used to DISCARD both, and
  // the render's fallback chain (handlers.cpp: varMeta -> the card's format ->
  // a dtype default) cannot tell "the card does not know" from "the card says
  // %9.0g", so it took the default as an answer.
  //
  // The visible consequence was not subtle and was not confined to `use`: on a
  // dataset carrying %12.0g, %10.0g, %td, %9s and %12s columns, a single
  // `gen zz = 1` reported EVERY one of them as %9.0g afterwards — a date
  // variable silently stopped being a date. Every resident verb re-syncs
  // through here, so gen/replace/egen/drop/keep all did it, and the freeze
  // corpus did not catch it because the corpus was captured with the loss
  // already in place: the pins agreed with the wrong value.
  //
  // The card is a CACHE of what PIET already knew a moment ago, so the fix is
  // to carry the PIET-side half across the re-seed by name rather than to
  // invent a wire field for it. A column that is genuinely new to this frame
  // keeps the VarInfo defaults, which is the honest answer for it.
  std::unordered_map<std::string, const VarInfo*> carried;
  std::vector<VarInfo> dsCard;  // storage for the datastore branch's VarInfos
  if (shadowValid_) {
    carried.reserve(shadowCard_.size());
    for (const auto& v : shadowCard_) carried.emplace(v.name, &v);
  } else if (datastore_ != nullptr) {
    // The FIRST resident verb after a `use` re-seeds with no card yet — but the
    // datastore that `use` just rendered its varlist from is still open, and it
    // is where the formats came from in the first place. Read it only when it
    // already exists (never construct one here: the whole point of the resident
    // plane is not to re-open the head).
    dsCard = datastore_->varlist();
    carried.reserve(dsCard.size());
    for (const auto& v : dsCard) carried.emplace(v.name, &v);
  }
  std::vector<VarInfo> next;
  next.reserve(vars.size());
  for (const auto& fv : vars) {
    VarInfo vi;
    vi.name = fv.name;
    vi.dtype = fv.type;
    vi.nMissing = fv.nMissing;
    vi.nLevels = fv.nLevels;
    auto it = carried.find(fv.name);
    if (it != carried.end()) {
      vi.format = it->second->format;
      vi.formatStated = it->second->formatStated;
      vi.label = it->second->label;
    } else {
      // BUG-365: a column with no carried entry is one PIET is meeting for the
      // first time — the using side of a resident joinby/merge. Its only source
      // for a display format is what the engine just reported; before the engine
      // sent it, such a column silently defaulted and a %10s came back %9.0g.
      //
      // ONLY when the engine actually stated something. "" means "the engine said
      // nothing" — a freshly COMPUTED column (gen/egen) has no source format, and
      // overwriting VarInfo's own default with that emptiness blanked the format on
      // 1,357 columns across 45 fixtures the first time this was written. Absent
      // is not a value.
      if (!fv.format.empty()) {
        vi.format = fv.format;
        vi.formatStated = true;
      }
      if (!fv.label.empty()) vi.label = fv.label;
    }
    next.push_back(std::move(vi));
  }
  shadowCard_ = std::move(next);
  shadowValid_ = true;
  // C4-L2A: rows ride with the card ("seedShadowFromFrameVars pair" rule).
  // Gated on meta.valid — a frame block the parser could not vouch for seeds
  // the card exactly as it always has, but rows stay unknown (-1) so
  // residentRowsIfCurrent() honestly declines rather than reporting a
  // default-constructed 0 as the dataset's _N.
  residentRows_ = meta.valid ? meta.rows : -1;
  // fix/sort-validate-lazy: tag with residentToken_, NOT currentHbin_ — every
  // call site sets residentToken_ to the exact path this frame-op just synced
  // to BEFORE calling this function (residentSyncWorkerToHead/residentRestore/
  // loadFromVirtual/runViaCobraSession's resync branch all do), but that path
  // is not always currentHbin_: a post-estimation re-run (test/margins/lincom)
  // can resync the worker onto lastModel.dataPath while currentHbin_ still
  // names the user's own working dataset. Tagging with residentToken_ makes
  // shadowVarlistIfCurrent() correctly report "not current" in that case
  // instead of mislabeling a foreign frame's card as the live dataset's.
  shadowCardHbin_ = residentToken_;
}

// MUTRES-WIRE (2026-08-22). See the header. Asks the worker for the resident
// varlist so a declined card fold can rebuild from metadata instead of opening
// the frame — which, for a virtual head, means writing the whole thing to disk
// first (measured 128.6 MB per occurrence).
//
// Every failure here is a FALLBACK signal, never an error: no worker, a serve
// plane that is off, a §25-only engine that rejects the op, or a response
// without vars all mean "use the datastore path exactly as before".
const CobraFrameMeta* SessionState::fetchWireVarsFromWorker() {
  if (serveDisabled() || cobraSession_ == nullptr) return nullptr;
  if (cobraSession_->serveUnavailable()) return nullptr;
  if (cobraSession_->framesState() == CobraSession::Frames::No) return nullptr;
  wireVars_ = CobraFrameMeta{};
  CobraRunResult r = cobraSession_->frameVars(&wireVars_);
  if (!r.ok || !wireVars_.valid || wireVars_.vars.empty()) return nullptr;
  return &wireVars_;
}

void SessionState::reseedShadowFrom(Datastore* ds) {
  if (ds == nullptr) {
    shadowValid_ = false;
    return;
  }
  shadowCard_ = ds->varlist();  // BASE list (pre-meta); the render step applies the remap.
  shadowValid_ = true;
  residentRows_ = ds->nRows();   // C4-L2A: rows ride with the card, real-rebuild flavor.
  shadowCardHbin_ = ds->path();  // fix/sort-validate-lazy: ds was just built from currentHbin_
                                 // (mutationResult calls invalidateDatastore()+datastore()
                                 // immediately before this), so ds->path() == *currentHbin_ here.
}

// -- MAXPERF Stage 2 (§27 session snapshots) client wiring -------------------
// Shared precondition: the resident plane must be usable AND the worker must be
// holding the CURRENT head. Ensures both, returning false when the plane is
// unavailable (caller falls back to the file plane). A dirty virtual head is
// materialized first (crash-recovery floor — see residentPreserve's doc); this
// is a no-op on the common clean/real head, where the snapshot stays RAM-only.
bool SessionState::residentSyncWorkerToHead() {
  if (serveDisabled() || !currentHbin_.has_value()) return false;
  CobraSession* cs = ensureCobraSession();
  if (!cs || cs->serveUnavailable()) return false;
  if (cs->framesState() != CobraSession::Frames::Yes) return false;
  ensureMaterializedHead("resident-sync");  // dirty virtual head -> on-disk floor (else a no-op).
  if (residentToken_ != *currentHbin_) {
    // Worker isn't holding this head (fresh `use` before any compute, or a
    // prior stateless mutation). Frame-use the real file so the stash/save
    // snapshots exactly the current data.
    CobraFrameMeta useMeta;
    CobraRunResult ur = cs->frameUse(*currentHbin_, &useMeta);
    if (cs->framesState() == CobraSession::Frames::No || !ur.ok) return false;
    ++residentLiveCount_;  // worker accepted a frame — see residentLiveCount()
    residentToken_ = *currentHbin_;
    residentDirty_ = false;
    seedShadowFromFrameVars(useMeta);
    (void)cs->consumeRespawned();
  }
  return true;
}

std::optional<std::string> SessionState::residentPreserve() {
  if (!residentSyncWorkerToHead()) return std::nullopt;
  // BUG-384 (root, 2026-09-01). runPreserve's resident arm records
  // frame.hbinPath = currentHbin_ on the strength of this function's old
  // contract comment — "materialized the head just before stashing". That was
  // FALSE on the common path: residentSyncWorkerToHead materializes only in its
  // DIVERGENT branch (token != head, where a resync forces it); a dirty virtual
  // head whose token equals the head — the normal state right after any opted-in
  // mutation — passed straight through, and the preserve recorded a twin that
  // did not exist. The trace that convicted it, on the internal-volume corpus:
  //     MINT mutation-out strmut_19478
  //     PRESERVE(twin-MISSING) resident strmut_19478
  //     ...
  //     RESTORE(twin-MISSING) -> fallback mints restore_N -> cascade -> ENOENT
  // Externally the hole was invisible because the NEXT command's datastore()
  // render usually materialized the head a moment later, making the twin real
  // retroactively; when the card-fold fast path skips that render — a timing-
  // and machine-dependent choice — the twin never comes into existence. That is
  // why four external acceptance runs passed 47/47 while five internal runs
  // died at artifact 35-39.
  //
  // The twin is this function's promise, so this function keeps it: snapshot
  // the head to disk BEFORE stashing. A no-op when the head is already real; on
  // snapshot failure dirty stays set and we fall back to the clone plane, which
  // has its own materialize-and-verify and reports honestly (BUG-124 shape).
  ensureMaterializedHead("preserve-floor");
  if (residentDirty_) return std::nullopt;  // could not write the twin -> clone fallback
  std::string name = "__preserve_" + std::to_string(stashSeq_++) + "__";
  CobraFrameMeta meta;
  CobraRunResult r = cobraSession_->frameStash(name, &meta);
  if (!r.ok) return std::nullopt;  // §27-absent engine / worker death -> clone fallback.
  (void)cobraSession_->consumeRespawned();
  return name;
}

// PP-L2B / K1b: stash the current head under an EXPLICIT, stable name (the
// caller's useStashName(src)) for later cache restore. Mirrors residentPreserve
// but names the stash itself (re-stashing an unchanged name overwrites the
// worker-RAM frame in place — how a changed source frees its predecessor
// without a drop op). residentSyncWorkerToHead() also materializes a dirty
// virtual head first, so the hbinPath the caller records as the recovery floor
// is a real on-disk twin. false => resident plane unavailable: the caller does
// not memoize and the next `use` pays the ordinary disk path.
bool SessionState::useStashSnapshot(const std::string& name) {
  if (!residentSyncWorkerToHead()) return false;
  CobraFrameMeta meta;
  CobraRunResult r = cobraSession_->frameStash(name, &meta);
  if (!r.ok) return false;
  (void)cobraSession_->consumeRespawned();
  return true;
}

bool SessionState::residentRestore(const std::string& stashName,
                                   const std::string& preserveHbinPath, bool keepStash) {
  if (serveDisabled() || !cobraSession_) return false;
  CobraSession* cs = cobraSession_.get();
  if (cs->serveUnavailable() || cs->framesState() != CobraSession::Frames::Yes) return false;
  CobraFrameMeta meta;
  // keepStash=true (PP-L2B / K1b) retains the stash for the next re-`use`; the
  // engine hands back a CoW copy (columns shared with the retained snapshot
  // until a later mutation), so a restored-then-mutated dataset never corrupts
  // the cache. keepStash=false (preserve/restore LIFO) consumes it.
  CobraRunResult r = cs->frameUnstash(stashName, /*keep=*/keepStash, &meta);
  if (!r.ok || !meta.valid) return false;  // stash lost to a worker restart / §27-absent.
  // Either restore ABANDONS whatever scratch was the live head (a mutation
  // output made after the preserve). The disk path's load() reclaims its
  // superseded predecessor; this resident path has no load(), so it must say so
  // itself or the file is stranded for the life of the process.
  //
  // BUG-381: this was `keepStash && ...`, i.e. the cache-`use` path only. The
  // reason given was that "preserve/restore keeps its old head as part of the
  // LIFO" — and that is not so. The LIFO holds the preserve SNAPSHOT
  // (frame.hbinPath); the head at restore time is a mutation output made AFTER
  // the preserve, which nothing keeps. Measured cost of the gap on the
  // 47-artifact corpus: 1145 stranded files, 4.538 GiB, 96.7% of a scratch
  // high-water that no budget setting could touch (that is what "orphan" means
  // — a file no ledger knows about is one no reap can reach). The shape is
  // `preserve` -> mutate -> `save` -> `restore` in a loop, which run_one_depvar
  // does three times per dependent variable; 160 depvars gave exactly 480
  // stranded `rename_*` files.
  //
  // Safe for both values of keepStash for the reason the old comment already
  // gave: reclaimSupersededScratch's own exclusions spare a pinned twin, a live
  // preserve snapshot (including an OUTER frame's, under nesting), a frame, and
  // a user file — and the `!= preserveHbinPath` guard below spares the exact
  // file re-adopted as the head immediately after. A virtual (tokenized) head
  // names no file and is dropped by reclaim's is_regular_file test.
  if (currentHbin_.has_value() && *currentHbin_ != preserveHbinPath) {
    reclaimSupersededScratch(*currentHbin_, /*abandoningHead=*/true);
  }
  // The engine restored the snapshot as the resident. residentPreserve() took
  // the stash IMMEDIATELY after materializing the head, so the preserve-time
  // file it recorded is byte-identical to the restored frame by construction.
  // Re-adopt that file as a CLEAN real head: the card build after restore
  // re-mmaps the existing file with ZERO writes. (Minting a dirty virtual token
  // here instead forced datastore() to re-materialize the full frame on every
  // restore — a control-confirmed +7% batch regression vs the clone plane,
  // which re-mmapped its already-on-disk clonefile.) The file is GC-protected
  // while the preserve frame is live (reclaimSupersededScratch skips paths on
  // preserveStack), so intra-session it exists; the exists() check below is a
  // belt-and-braces fallback, never an error.
  std::error_code ec;
  if (!preserveHbinPath.empty() &&
      std::filesystem::exists(cecon::to_native_path(preserveHbinPath), ec)) {
    currentHbin_ = preserveHbinPath;
    residentToken_ = preserveHbinPath;
    residentDirty_ = false;
    lastMaterializedHead_ = preserveHbinPath;  // real on-disk head — the new crash floor
    // NOTE: deliberately no flushDeferredReclaim() here — under an eager scratch
    // budget it could unlink the head just adopted. The next materialization (or
    // real-file load) flushes as usual, exactly like the dirty-token path below.
  } else {
    // No on-disk twin survives (scratch GC'd — shouldn't happen intra-session).
    // Fall back: adopt the resident as a VIRTUAL head under a fresh token, dirty
    // so the first PIET-side datastore() snapshots it to disk on demand (exactly
    // the gen/replace virtual-head discipline).
    std::string token = workdir_ + "/restore_" + std::to_string(stashSeq_++) + ".hbin";
    currentHbin_ = token;
    residentToken_ = token;
    residentDirty_ = true;
    if (scratchTraceOn()) scratchTrace("MINT", "restore-fallback", token);
  }
  datastore_.reset();
  // BUG-916: the computed-column cache describes the chain this restore just
  // abandoned (a tostring/destring made after the preserve, or the frame a
  // cache `use` replaced). load() clears it on a real-file head; this path has
  // no load(), so it must clear it itself, or the next setcol verb and every
  // NAME[i] read are served the abandoned column (w2b_dataio.log:90-96).
  residentComputedColClear();
  datasetChanged = true;  // restore replaces the in-memory data (matches load()).
  seedShadowFromFrameVars(meta);
  (void)cs->consumeRespawned();
  return true;
}

void SessionState::scratchTrace(const char* what, const std::string& a,
                                const std::string& b) {
  if (!scratchTraceOn()) return;
  auto base = [](const std::string& p) {
    size_t i = p.find_last_of('/');
    return i == std::string::npos ? p : p.substr(i + 1);
  };
  std::fprintf(stderr, "[scratch] %s %s <- %s\n", what, base(a).c_str(), base(b).c_str());
}

void SessionState::reapProjectionScratch() {
  if (projectionScratch_.empty()) return;
  const std::string head = currentHbin_.value_or(std::string());
  for (const auto& path : projectionScratch_) {
    if (!head.empty() && path == head) continue;
    if (scratchTraceOn()) scratchTrace("UNLINK", "projection-reap", path);
    std::error_code ec;
    std::filesystem::remove(cecon::to_native_path(path), ec);
  }
  projectionScratch_.clear();
}

// BUG-381 DIAGNOSTIC. See the declaration in session_state.h for why this walks
// rather than reasons from the notes above it.
// BUG-389 (2026-09-02): the one owner table the census reports from and the
// orphan sweep spares from. FIRST claim wins, so the order IS the attribution
// priority: a file that is both the live head and a tracked projection is the
// head, because that is what it is. Every path is entered twice — as written and
// canonicalized — so a caller can look up a directory-walk path without a
// realpath syscall per file.
std::map<std::string, std::string> SessionState::scratchOwners() const {
  namespace fs = std::filesystem;
  std::map<std::string, std::string> owner;
  auto claim = [&](const std::string& p, const char* who) {
    if (p.empty()) return;
    owner.emplace(p, who);
    std::error_code e;
    fs::path c = fs::weakly_canonical(cecon::to_native_path(p), e);
    if (!e) owner.emplace(c.u8string(), who);
  };
  claim(currentHbin_.value_or(std::string()), "head");
  // e(sample) T2: the install head's on-disk base, owned for as long as the head
  // is that install (reclaimSupersededScratch exclusion (6c)). Claimed right
  // after the head so the orphan sweep and the census agree with the reclaim.
  if (esampleHeadIsInstallOf(esampleLastInstallBase)) claim(esampleLastInstallBase, "esample_base");
  for (const auto& f : preserveStack) claim(f.hbinPath, "preserve");
  for (const auto& [k, e] : useConvCache_) { (void)k; claim(e.hbinPath, "use_conv_cache"); claim(e.metaPath, "use_conv_cache"); }
  for (const auto& [k, e] : useStashCache_) { (void)k; claim(e.hbinPath, "use_stash_cache"); claim(e.metaPath, "use_stash_cache"); }
  for (const auto& [n, fp] : frames) { (void)n; claim(fp, "frame"); }
  for (const auto& sp : scratchPins) claim(sp, "scratch_pin");
  for (const auto& pp : preInstallPins_) claim(pp, "pre_install_pin");  // e(sample) T2
  for (const auto& [sp, sz] : supersededScratch_) { (void)sz; claim(sp, "superseded_pending"); }
  for (const auto& pp : projectionScratch_) claim(pp, "projection_pending");
  for (const auto& [d, tp] : mintedTempfiles) { (void)d; claim(tp, "tempfile_pending"); }
  if (lastModel_.has_value()) {
    claim(lastModel_->dataPath, "last_model");
    claim(lastModel_->fitDataPath, "last_model");
  }
  for (const auto& [n, ph] : postfiles) { (void)n; claim(ph.target, "postfile_open"); }
  for (const auto& [n, fh] : fileHandles) { (void)n; claim(fh.path, "file_handle_open"); }
  claim(currentFile_, "user_file");
  // The crash-recovery floor of a virtual resident chain. When the head is a
  // virtual token, the last snapshot written to disk (ensureMaterializedHead,
  // or a real-file load) is what rollbackToFloor() re-adopts if the worker
  // dies; load() parks it on deferredReclaim_ and flushDeferredReclaim()
  // releases it at the NEXT materialization, when a newer floor replaces it.
  // Until then the file is owned — but this table never said so, so: the exit
  // census reported it ORPHAN (`gen str1 s = ""` materializes strmut_2 to read
  // rows, its setcol --out goes virtual, and strmut_2 is left as the floor —
  // probe_b4's ORPHAN n=1, ~65 KB), and for a REGISTERED mint (setobs_*, ...)
  // the dispatch-exit sweep deleted the live floor, so a rollback would have
  // re-adopted a file that no longer exists. Only the current floor is
  // claimed: an older one is on deferredReclaim_ and gets its reclaim there.
  // After user_file so a user's own file that is also the floor reads as the
  // user's.
  claim(lastMaterializedHead_, "crash_floor");
  claim(workdir_ + "/.piet_owner_pid", "owner_marker");
  return owner;
}

namespace {
// Every SessionState alive in this process (see sweepOrphanScratch's header
// note). Dispatch is single-threaded; the mutex only keeps a construct or
// destruct on another thread (a worker harness) from tearing the vector.
std::mutex& liveSessionsMu() {
  static std::mutex mu;
  return mu;
}
std::vector<const SessionState*>& liveSessions() {
  static std::vector<const SessionState*> v;
  return v;
}
}  // namespace

void SessionState::registerLiveSession() {
  std::lock_guard<std::mutex> lk(liveSessionsMu());
  liveSessions().push_back(this);
}

SessionState::~SessionState() {
  std::lock_guard<std::mutex> lk(liveSessionsMu());
  auto& v = liveSessions();
  v.erase(std::remove(v.begin(), v.end(), this), v.end());
}

int SessionState::sweepOrphanScratch(const char* site) {
  namespace fs = std::filesystem;
  if (const char* off = std::getenv("PIET_NO_ORPHAN_SWEEP"); off && *off) return 0;
  // The scratch-GC opt-out (PIET_NO_SCRATCH_GC, honoured by reclaimSupersededScratch) covers
  // this reaper too: a diagnostic that wants to LOOK at the minted files afterwards
  // (piet/native/tests/scratch_path_monotonic.py) must not have them swept from under it.
  if (const char* gc = std::getenv("PIET_NO_SCRATCH_GC"); gc && *gc) return 0;
  // No mint-sequence throttle (first cut had one): a verb can create workdir files
  // without nextPath (recode's mask/tmp pair did) and the following commands may
  // mint nothing, which left those two as the last orphans standing. The walk over
  // a session's handful of files is microseconds; correctness is not worth them.
  mintSeqAtLastSweep_ = scratchMintSeq();
  if (workdir_.empty()) return 0;
  std::error_code ec;
  const fs::path wd = cecon::to_native_path(workdir_);
  if (!fs::is_directory(wd, ec) || ec) return 0;
  std::map<std::string, std::string> owner = scratchOwners();
  {
    // e(sample) T2: a sibling session's files in the SAME workdir are owned.
    std::lock_guard<std::mutex> lk(liveSessionsMu());
    for (const SessionState* other : liveSessions()) {
      if (other == this || other->workdir_ != workdir_) continue;
      for (auto& kv : other->scratchOwners()) owner.emplace(kv.first, "live_session");
    }
  }
  int removed = 0;
  fs::recursive_directory_iterator it(wd, fs::directory_options::skip_permission_denied, ec), end;
  for (; !ec && it != end; it.increment(ec)) {
    std::error_code e;
    const fs::path& p = it->path();
    if (it->is_directory(e) && !e) {
      // Another owner's workdir (a parallel worker, a nested session) is theirs.
      std::error_code e2;
      if (fs::exists(p / ".piet_owner_pid", e2)) it.disable_recursion_pending();
      continue;
    }
    if (!it->is_regular_file(e) || e) continue;
    if (p.filename() == ".piet_owner_pid") continue;
    const std::string raw = p.u8string();
    // Candidates are ONLY paths this process minted (scratch_seq.h registry). A
    // user's `save` into the workdir, a file another process wrote, anything the
    // registry never saw — never touched, whatever it is named. (First cut swept
    // every unowned file and deleted the selftest's own freshly saved .dta.)
    if (!isRegisteredScratchMint(raw)) continue;
    if (owner.count(raw)) continue;
    std::error_code e3;
    fs::path c = fs::weakly_canonical(p, e3);
    if (!e3 && owner.count(c.u8string())) continue;
    if (scratchTraceOn()) scratchTrace("UNLINK", std::string("orphan-sweep:") + site, raw);
    std::error_code e4;
    if (fs::remove(p, e4) && !e4) {
      ++removed;
      forgetScratchMint(raw);
    }
  }
  lastOrphanSweepRemoved_ = removed;
  return removed;
}

// BUG-390: byte-bounded `use` memos ────────────────────────────────────────────
std::uintmax_t SessionState::useCacheCapBytes() {
  auto envMb = [](const char* name) -> long long {
    const char* v = std::getenv(name);
    if (!v || !*v) return -1;
    char* end = nullptr;
    long long mb = std::strtoll(v, &end, 10);
    return (end && *end == '\0' && mb >= 0) ? mb : -1;
  };
  long long mb = envMb("PIET_USE_CACHE_MB");
  if (mb < 0) {
    long long budget = envMb("CECON_MEMORY_BUDGET_MB");
    mb = budget > 0 ? budget / 2 : 1024;
  }
  return static_cast<std::uintmax_t>(mb) * 1024ull * 1024ull;
}

std::uintmax_t SessionState::useCacheBytes() const {
  namespace fs = std::filesystem;
  std::set<std::string> seen;
  std::uintmax_t total = 0;
  auto add = [&](const std::string& path) {
    if (path.empty() || !seen.insert(path).second) return;
    std::error_code e;
    auto sz = fs::file_size(cecon::to_native_path(path), e);
    if (!e) total += sz;
  };
  for (const auto& [k, e] : useConvCache_) { (void)k; add(e.hbinPath); }
  for (const auto& [k, e] : useStashCache_) { (void)k; add(e.hbinPath); }
  return total;
}

void SessionState::dropUseCacheEntry(const std::string& src) {
  std::vector<std::string> paths;
  if (auto it = useConvCache_.find(src); it != useConvCache_.end()) {
    paths.push_back(it->second.hbinPath);
    paths.push_back(it->second.metaPath);
    useConvCache_.erase(it);
  }
  if (auto it = useStashCache_.find(src); it != useStashCache_.end()) {
    if (cobraSession_ && cobraSession_->hasWorker() && !it->second.stashName.empty())
      (void)cobraSession_->frameStashDrop(it->second.stashName);
    paths.push_back(it->second.hbinPath);
    paths.push_back(it->second.metaPath);
    useStashCache_.erase(it);
  }
  useCacheTouch_.erase(src);
  // Entries erased FIRST so exclusions (3)/(4) no longer pin; every other pin
  // (live head, preserve twin, frame, ...) still applies inside the primitive.
  for (const auto& p : paths)
    if (!p.empty()) reclaimSupersededScratch(p);
}

void SessionState::dropUseCaches() {
  std::set<std::string> srcs;
  for (const auto& [k, e] : useConvCache_) { (void)e; srcs.insert(k); }
  for (const auto& [k, e] : useStashCache_) { (void)e; srcs.insert(k); }
  for (const auto& src : srcs) dropUseCacheEntry(src);
}

void SessionState::enforceUseCacheCap() {
  const std::uintmax_t cap = useCacheCapBytes();
  if (cap == 0) return;  // 0 = unbounded (explicit PIET_USE_CACHE_MB=0)
  auto sourceCount = [&]() {
    std::set<std::string> srcs;
    for (const auto& [k, e] : useConvCache_) { (void)e; srcs.insert(k); }
    for (const auto& [k, e] : useStashCache_) { (void)e; srcs.insert(k); }
    return srcs.size();
  };
  // Stop at ONE source, not one entry: a source's conv and stash memos share a
  // recency key and are evicted together, so counting entries evicted the last
  // remaining source too (the first cut emptied the cache on every put).
  while (sourceCount() > 1 && useCacheBytes() > cap) {
    // Evict the least recently used source. An entry never touched gets clock 0.
    std::string victim;
    std::uint64_t best = ~0ull;
    auto consider = [&](const std::string& src) {
      auto t = useCacheTouch_.find(src);
      std::uint64_t stamp = t == useCacheTouch_.end() ? 0 : t->second;
      if (stamp < best) { best = stamp; victim = src; }
    };
    for (const auto& [k, e] : useConvCache_) { (void)e; consider(k); }
    for (const auto& [k, e] : useStashCache_) { (void)e; consider(k); }
    if (victim.empty()) break;
    if (scratchTraceOn()) scratchTrace("EVICT", "use-cache-lru", victim);
    dropUseCacheEntry(victim);
  }
}

void SessionState::useStashCachePut(const std::string& src, const UseStashCacheEntry& entry) {
  if (entry.statKey.empty() || entry.stashName.empty() || entry.hbinPath.empty()) return;
  useStashCache_[src] = entry;
  useCacheTouch_[src] = ++useCacheClock_;
  enforceUseCacheCap();
}

// BUG-392: `clear all` beyond the dataset ─────────────────────────────────────
void SessionState::discardTopPreserve() {
  if (preserveStack.empty()) return;
  const PreserveFrame& top = preserveStack.back();
  if (!top.stashName.empty() && cobraSession_ && cobraSession_->hasWorker())
    (void)cobraSession_->frameStashDrop(top.stashName);
  preserveStack.pop_back();
}

const PreserveFrame* SessionState::ownPreserveFrame() const {
  if (preserveStack.empty() || preserveStack.back().level != preserveLevel()) return nullptr;
  return &preserveStack.back();
}

void SessionState::clearAllSessionObjects() {
  eval.mlState.reset();
  estimatesClear();
  // AER-SCOPE1 bundle G: `clear all` closes the open workbook. A surviving cell
  // map would write the next `putexcel A1 = ...` into a file the PREVIOUS
  // analysis named, at rc 0 -- the same reason every sibling below is dropped.
  putexcel = nlohmann::ordered_json::object();
  estAddedClear();  // AER-SCOPE1 bundle E: the estadd bag belongs to e(), which
                    // `clear all` drops -- leaving it would attach one model's
                    // additions to the next regression after a `clear all`.
  namedGraphs.clear();
  graphMeta.clear();  // graph-family AI-4
  currentGraph.clear();
  sersets.clear();    // serset-twoway SS: `clear all` drops every serset (serset.sthlp)
  postfiles.clear();  // runClear's `all` branch closed (wrote) each one first (BUG-1079)
  for (auto& [n, fh] : fileHandles) {
    (void)n;
    if (fh.fp) std::fclose(fh.fp);
    fh.fp = nullptr;
  }
  fileHandles.clear();
  std::vector<std::string> paths;
  for (const auto& [n, fp] : frames) { (void)n; paths.push_back(fp); }
  frames.clear();
  frameLinks.clear();
  // NOT the preserve stack. Oracle (StataBE 19.5, 2026-09-02): `sysuse auto` → `preserve` →
  // `clear all` → `restore` is rc 0 with N=74 — a preserve survives `clear all` (and `clear`),
  // and `restore` brings the data back. The first cut of this function dropped the stack and
  // the fuzzer caught it within the hour (restore after clear all → "no previous preserve").
  dropUseCaches();
  // Ledgers cleared FIRST, then the files: the reclaim primitive's exclusions
  // (frames, preserve twins, memos) no longer pin them, everything else still does.
  for (const auto& p : paths)
    if (!p.empty()) reclaimSupersededScratch(p);
  if (cobraSession_ && cobraSession_->hasWorker()) (void)cobraSession_->frameDrop();
}

std::string SessionState::scratchCensusJson() const {
  namespace fs = std::filesystem;
  std::error_code ec;
  const std::map<std::string, std::string> owner = scratchOwners();

  struct Agg { long long n = 0; long long bytes = 0; };
  std::map<std::string, Agg> byClass;
  std::map<std::string, Agg> orphanShape;  // basename with each digit run -> '#'
  std::vector<std::pair<long long, std::string>> orphanBig;
  long long totalN = 0, totalBytes = 0;

  fs::path wd = fs::weakly_canonical(cecon::to_native_path(workdir_), ec);
  if (ec) return std::string("{\"error\":\"workdir not canonicalizable\"}");
  fs::recursive_directory_iterator it(wd, fs::directory_options::skip_permission_denied, ec), end;
  for (; !ec && it != end; it.increment(ec)) {
    std::error_code e;
    if (!it->is_regular_file(e) || e) continue;
    long long sz = 0;
    { std::error_code e2; auto v = fs::file_size(it->path(), e2); if (!e2) sz = (long long)v; }
    std::error_code e3;
    fs::path cp = fs::weakly_canonical(it->path(), e3);
    const std::string key = e3 ? it->path().u8string() : cp.u8string();
    auto o = owner.find(key);
    const std::string cls = (o == owner.end()) ? std::string("ORPHAN") : o->second;
    auto& a = byClass[cls];
    a.n++;
    a.bytes += sz;
    totalN++;
    totalBytes += sz;
    if (cls == "ORPHAN") {
      const std::string b = it->path().filename().u8string();
      std::string shape;
      bool inNum = false;
      for (char ch : b) {
        if (ch >= '0' && ch <= '9') {
          if (!inNum) { shape += '#'; inNum = true; }
        } else {
          shape += ch;
          inNum = false;
        }
      }
      auto& s2 = orphanShape[shape];
      s2.n++;
      s2.bytes += sz;
      orphanBig.emplace_back(sz, b);
    }
  }

  nlohmann::ordered_json out = nlohmann::ordered_json::object();
  out["workdir"] = workdir_;
  out["files"] = totalN;
  out["bytes"] = totalBytes;
  nlohmann::ordered_json cls = nlohmann::ordered_json::object();
  for (const auto& [k, a] : byClass) {
    nlohmann::ordered_json r = nlohmann::ordered_json::object();
    r["n"] = a.n;
    r["bytes"] = a.bytes;
    cls[k] = r;
  }
  out["by_class"] = cls;
  // The orphan SHAPE table is the one that names a producer: a basename with its
  // digit runs collapsed is that call site's minting template.
  std::vector<std::pair<long long, std::string>> shapes;
  shapes.reserve(orphanShape.size());
  for (const auto& [k, a] : orphanShape) shapes.emplace_back(a.bytes, k);
  std::sort(shapes.begin(), shapes.end(), std::greater<>());
  nlohmann::ordered_json sh = nlohmann::ordered_json::array();
  for (size_t i = 0; i < shapes.size() && i < 25; ++i) {
    nlohmann::ordered_json r = nlohmann::ordered_json::object();
    r["shape"] = shapes[i].second;
    r["n"] = orphanShape[shapes[i].second].n;
    r["bytes"] = shapes[i].first;
    sh.push_back(r);
  }
  out["orphan_shapes"] = sh;
  std::sort(orphanBig.begin(), orphanBig.end(), std::greater<>());
  nlohmann::ordered_json big = nlohmann::ordered_json::array();
  for (size_t i = 0; i < orphanBig.size() && i < 15; ++i) {
    nlohmann::ordered_json r = nlohmann::ordered_json::object();
    r["name"] = orphanBig[i].second;
    r["bytes"] = orphanBig[i].first;
    big.push_back(r);
  }
  out["orphan_largest"] = big;
  return out.dump();
}

bool SessionState::residentSaveVirtual(const std::string& path) {
  if (!residentSyncWorkerToHead()) return false;
  CobraFrameMeta meta;
  CobraRunResult r = cobraSession_->frameSaveVirtual(path, &meta);
  if (!r.ok) return false;
  (void)cobraSession_->consumeRespawned();
  virtualSaves_.insert(path);
  return true;
}

bool SessionState::loadFromVirtual(const std::string& path) {
  if (!isVirtualSave(path)) return false;
  if (serveDisabled() || !cobraSession_) return false;
  CobraSession* cs = cobraSession_.get();
  if (cs->serveUnavailable() || cs->framesState() != CobraSession::Frames::Yes) return false;
  CobraFrameMeta meta;
  // §27: `use` checks the virtual-file table FIRST — a registered path restores
  // from RAM in O(columns). The client calls frameUse(path) unchanged; the hit
  // is transparent (a MISS would fall through to a disk open, but this path is
  // only reached for a registered entry, so a MISS means the worker restarted
  // and lost the table -> return false, and runUse surfaces "File not found").
  CobraRunResult r = cs->frameUse(path, &meta);
  if (cs->framesState() == CobraSession::Frames::No || !r.ok || !meta.valid) return false;
  ++residentLiveCount_;  // worker accepted a frame — see residentLiveCount()
  // BUG-381: this fast path replaces the head WITHOUT going through load(), so
  // it must reclaim the predecessor itself — load()'s reclaim is the only one
  // there is, and a `use` that hits the §27 virtual table never reaches it.
  // Measured on one campaign group: `append using <tempfile>` made the head,
  // the next `use <tempfile>` came through here, and the append output was
  // stranded for the life of the process. Same class as the restore gap above:
  // every DIRECT `currentHbin_ =` is a place load()'s bookkeeping does not run.
  if (currentHbin_.has_value() && *currentHbin_ != path) {
    reclaimSupersededScratch(*currentHbin_, /*abandoningHead=*/true);
  }
  currentHbin_ = path;
  residentToken_ = path;
  residentDirty_ = true;  // no disk file yet; datastore() materializes on demand.
  if (scratchTraceOn()) scratchTrace("MINT", "resident-use", path);
  datastore_.reset();
  residentComputedColClear();  // BUG-916: a new frame — the old chain's cache is over.
  datasetChanged = false;  // a fresh `use` leaves c(changed) false (matches load+setCurrentFile).
  seedShadowFromFrameVars(meta);
  (void)cs->consumeRespawned();
  return true;
}

// BUG-125 (Wave-γ save_virtual follow-up, found during the BUG-124 sweep): a
// save_virtual'd tempfile lives ONLY in the worker's RAM table. `use` restores
// it via the virtual-first fast path, but the second-file readers (merge/
// joinby/append `using`) read the path from DISK — shipped B.5.0 answered
// "file not found" for a tempfile the script had just saved (observed live:
// analysis.do's opponent_map joinby, 2 captured error blocks on a clean-disk
// run of the group-01 slice). On-demand materialization: the first time a disk
// reader asks, restore the entry as the resident (frameUse hits the virtual
// table) and snapshot it to its real path — byte-equal by the §27 parity law.
// The worker is left holding the TEMPFILE frame, so residentToken_ is cleared
// and the next resident verb re-frame-uses the real head (ordinary resync).
bool SessionState::materializeVirtualSave(const std::string& path) {
  if (!isVirtualSave(path)) return false;
  std::error_code ec;
  if (std::filesystem::exists(cecon::to_native_path(path), ec)) return true;  // already real
  if (serveDisabled() || !cobraSession_) return false;
  CobraSession* cs = cobraSession_.get();
  if (cs->serveUnavailable() || cs->framesState() != CobraSession::Frames::Yes) return false;
  // Anchor the CURRENT head on disk first — the frameUse below replaces the
  // resident frame, and a dirty virtual head would otherwise orphan (BUG-123's
  // materialize-before-resync law).
  ensureMaterializedHead("vsave-anchor");
  // BUG-384: frameUse(path) below REPLACES the worker's resident frame. If the
  // ensure above could not put the current head on disk (snapshot failure, or a
  // cleared token making it skip), proceeding would destroy the only copy of
  // the head's bytes — the exact mechanism the first traced repro caught
  // (CLEAR(dirty-head) vsave-anchor on a restore-fallback token). Refuse
  // instead: the caller's false-path falls back to the stateless/disk route,
  // which is slower and honest rather than fast and destructive.
  if (residentDirty_ && currentHbin_.has_value() && residentToken_ == *currentHbin_) {
    if (scratchTraceOn()) scratchTrace("VSAVE-REFUSE(dirty-head)", "vsave-anchor", *currentHbin_);
    return false;
  }
  CobraFrameMeta meta;
  CobraRunResult ur = cs->frameUse(path, &meta);  // virtual-table hit -> resident
  if (cs->framesState() == CobraSession::Frames::No || !ur.ok || !meta.valid) return false;
  CobraRunResult sr = cs->frameSnapshot(path);  // resident -> real file at `path`
  releaseResidentToken("vsave-anchor");  // worker holds the tempfile frame now, not the head
  shadowValid_ = false;  // the next head resync reseeds the shadow card
  (void)cs->consumeRespawned();
  if (!sr.ok) {
    std::error_code rmEc;
    std::filesystem::remove(cecon::to_native_path(path), rmEc);  // BUG-124: never leave a torn partial
    return false;
  }
  return true;
}

SessionState::SaveHeadPath SessionState::trySaveHeadDirect(const std::string& destTmp) {
  // Same guard as ensureMaterializedHead: only a dirty head whose resident frame
  // still IS currentHbin_ can be written straight to the temp. A clean head, a
  // divergent intermediate temp (currentHbin_ already real), or an engine with
  // frames unused all fall to the caller's copy-from-currentHbin_ path.
  if (!residentDirty_ || !cobraSession_ || !currentHbin_.has_value() ||
      residentToken_ != *currentHbin_) {
    return SaveHeadPath::CopyFromCurrent;
  }
  CobraRunResult r = cobraSession_->frameSnapshot(destTmp);
  if (!r.ok) {
    // A failed snapshot can leave a TORN temp (the engine's writer creates the
    // file before it can detect the failure — same hazard ensureMaterializedHead
    // guards). Remove it so the caller's copy path starts from a clean temp, and
    // leave the head dirty so that path's ensureMaterializedHead retries and, if
    // it too fails, reports honestly ("file not saved … unchanged").
    std::error_code rmEc;
    std::filesystem::remove(cecon::to_native_path(destTmp), rmEc);
    return SaveHeadPath::CopyFromCurrent;
  }
  // Head intentionally left VIRTUAL (residentDirty_ untouched, no addSnapshot
  // Bytes) — see the header. `save` no longer anchors the head as a side effect.
  return SaveHeadPath::WroteDirect;
}

// C4-L1 window trace (C4-RESIDENT-READ-SCOPE-brief §6): stderr-only, opt-in
// via $PIET_C4_TRACE. Permanentizes the C3 scratch instrumentation: one line
// per dirty-window CLOSURE (an EMH that actually snapshotted), naming the
// call site, the verb whose dispatch closed the window, and the 1-based window
// sequence number (== snapshot_count after the bump — EMH's addSnapshotBytes
// is the counter's only site, so the two are the same series by construction).
// This is what settles moved-vs-died for the D1/D2 legs: a window whose closer
// goes resident doesn't disappear from this trace unless NO later consumer in
// the same window materializes. With the env var absent nothing here runs, and
// the emit itself is stderr — stdout stays byte-identical either way (the
// freeze-fixture safety property).
static bool c4TraceEnabled() {
  static const bool on = [] {
    const char* e = std::getenv("PIET_C4_TRACE");
    return e && *e;
  }();
  return on;
}

void SessionState::noteC4TraceVerb(const std::string& rawCmd) {
  if (!c4TraceEnabled()) return;
  size_t b = rawCmd.find_first_not_of(" \t");
  if (b == std::string::npos) return;  // blank line — keep the previous verb
  size_t e = rawCmd.find_first_of(" \t,", b);
  c4TraceVerb_.assign(rawCmd, b, (e == std::string::npos ? rawCmd.size() : e) - b);
}

namespace {
// ROUTING / MATERIALIZATION CENSUS (PIET_ROUTELOG). One-time env read: both call
// sites are hot (952 eval routings on the reference slice alone), so the gate must
// be a predictable branch, not a getenv per call.
const char* routelog_path() {
  static const char* p = [] {
    const char* e = std::getenv("PIET_ROUTELOG");
    return (e && *e) ? e : nullptr;
  }();
  return p;
}
}  // namespace

// PHASE-0 DIAGNOSTIC (BUG-360 follow-up, 2026-08-20). The card fold declines on
// roughly half of a 460-rename corpus and the census cannot say which guard
// rejects — every rejection looks identical from outside (a full rebuild). This
// names the guard. Env-gated to a file, report-only, no behaviour change.
namespace {
const char* foldtrace_path() {
  static const char* p = [] {
    const char* e = std::getenv("PIET_FOLDTRACE");
    return (e && *e) ? e : nullptr;
  }();
  return p;
}
}  // namespace

void SessionState::foldTrace(const char* why, long long a, long long b) {
  if (const char* fp = foldtrace_path()) {
    if (std::FILE* f = std::fopen(fp, "a")) {
      std::fprintf(f, "%s\t%lld\t%lld\n", why, a, b);
      std::fclose(f);
    }
  }
}

void SessionState::ensureMaterializedHead(const char* c4Site) {
  // MATERIALIZATION CENSUS (env-gated, PIET_ROUTELOG). The scratch census says the
  // reference slice writes 45.7 GB, 31.7 GB of it under the `strmut` prefix — but
  // the routing census then showed all 224 setcol calls DO go resident, so those
  // bytes are not the setcol --out. The hypothesis this line tests is that they are
  // the VIRTUAL HEAD being flushed here, once per stateless op that needs it on
  // disk. Logged with the call site so the answer names a caller, not a guess.
  if (const char* rp = routelog_path()) {
    if (std::FILE* f = std::fopen(rp, "a")) {
      // On a FLUSH, name the caller. `datastore` accounts for 294 of 350 flushes
      // and that label is a site inside SessionState, not the thing that asked —
      // so the useful question ("who still needs a real Datastore when a shadow
      // card already carries the delta?") needs the frames above it. Symbolized
      // in-process with dladdr: an offline atos guesses the ASLR slide and names
      // the wrong function (measured, RAMDIET-CENSUS section 7.1).
      std::string chain;
      if (residentDirty_) {
        void* bt[10];
        const int n = ::backtrace(bt, 10);
        for (int i = 2; i < n && i < 7; ++i) {
          Dl_info info{};
          const char* sym = (dladdr(bt[i], &info) && info.dli_sname) ? info.dli_sname : "?";
          chain += (chain.empty() ? "" : "<") + std::string(sym);
        }
      }
      std::fprintf(f, "MATERIALIZE\t%s\t%s\t%s\n", residentDirty_ ? "flush" : "clean",
                   c4Site ? c4Site : "?", chain.c_str());
      std::fclose(f);
    }
  }
  if (!residentDirty_) return;  // clean head (or frames never used) — nothing to flush.
  if (!cobraSession_) {
    // BUG-384: no worker => the virtual bytes never existed anywhere reachable;
    // declaring the head clean here leaves it unbacked, and the audit says so.
    if (scratchTraceOn()) scratchTrace("CLEAR(no-worker)", "materialize", residentToken_);
    residentDirty_ = false;
    return;
  }
  if (!currentHbin_.has_value() || residentToken_ != *currentHbin_) {
    // The resident frame no longer corresponds to currentHbin_ — a divergent
    // intermediate temp (egen if/in, applyIfAndInFilter) advanced the frame
    // after currentHbin_ was already materialized. Snapshotting now would write
    // the WRONG frame over a real file, so skip. NOTE (BUG-384): the old tail
    // of this comment claimed "currentHbin_ is already real" — that is the
    // NORMAL case, not an invariant, and when a clearing site has already
    // dropped the token out from under a virtual head this skip is where the
    // head's last chance of materializing dies. The dispatch-exit audit is the
    // detector for that state; this trace names the site that skipped.
    if (scratchTraceOn())
      scratchTrace("MAT-SKIP(divergent)", c4Site ? c4Site : "?",
                   currentHbin_.value_or(std::string("-")));
    return;
  }
  CobraRunResult r = cobraSession_->frameSnapshot(*currentHbin_);
  // BUG-384: a snapshot failure was SILENT here ("the next real op reports it")
  // — and the very next op at one call site (materializeVirtualSave) DESTROYS
  // the worker frame, so nothing was left to report from. Say it when it fails.
  if (!r.ok) {
    std::fprintf(stderr,
                 "[piet_native] WARN snapshot-failed at %s: could not write %s (%s) — the head "
                 "stays a dirty virtual frame (BUG-384)\n",
                 c4Site ? c4Site : "?", currentHbin_->c_str(),
                 !r.error.empty() ? r.error.c_str()
                 : !r.stderr_text.empty()
                     ? r.stderr_text.c_str()  // the serve protocol carries the engine's
                                              // message in "stderr"; resultFromResponse
                                              // never mapped it into .error (BUG-384)
                     : "no engine detail");
  }
  if (r.ok) {
    // FLOOR ① site class SNAPSHOT (counters.h). Per op this is amp ≡ 1 — the
    // copy IS the snapshot's semantic work — so the number that can be wrong
    // here is the COUNT, not the ratio (32 snapshots where the semantics need
    // ~2). addSnapshotBytes bumps snapshot_count alongside the bytes for
    // exactly that reason; the count is the assertable half (A1 §1.3).
    {
      std::error_code ec;
      auto sz = std::filesystem::file_size(cecon::to_native_path(*currentHbin_), ec);
      counters::addSnapshotBytes(ec ? 0 : static_cast<int64_t>(sz));
      // GA sweep 2026-08-22: the SAME size, also billed to the caller that forced
      // this flush. The routelog already named the caller and the verb-diag ledger
      // already carried the bytes, but in two files with no join key — so the
      // materialization census could only rank callers by COUNT, and count is the
      // wrong unit: measured on this corpus the top 10% of snapshots carry 51% of
      // the bytes while the bottom half carry 3.3%. Ranking by count sent one fix
      // at 320 flushes worth 2 GB and ~0 s. One line, one join key, no new stat().
      if (const char* rp = routelog_path()) {
        if (std::FILE* f = std::fopen(rp, "a")) {
          std::fprintf(f, "MATBYTES\t%s\t%lld\n", c4Site ? c4Site : "?",
                       static_cast<long long>(ec ? 0 : sz));
          std::fclose(f);
        }
      }
    }
    // C4-L1 window trace: this snapshot IS a dirty-window closure — say who
    // closed it. Seq = snapshot_count post-bump (same series, see c4TraceEnabled).
    // FIX-C4COUNTER-SAMPLERSPAN/BUG-142: a closure fired from inside a
    // counter-neutral diagnostic walk (c4TraceSuppress_) has its count
    // restored, so no line prints — else trace and snapshot_count desync and
    // the line names an innocent verb (c4TraceVerb_ is stale post-dispatch).
    if (c4TraceEnabled() && !c4TraceSuppress_) {
      std::fprintf(stderr, "[c4-trace] %s|%s|%lld\n", c4Site,
                   c4TraceVerb_.empty() ? "-" : c4TraceVerb_.c_str(),
                   static_cast<long long>(counters::snapshotCount()));
    }
    residentDirty_ = false;
    lastMaterializedHead_ = *currentHbin_;
    flushDeferredReclaim();  // the virtual chain is now anchored on disk.
  } else {
    // BUG-124 sibling: a failed snapshot (ENOSPC, worker death mid-write) can
    // leave a TORN file at the token path — complete header, short blobs (the
    // engine's writer creates the file before it can detect the failure). Any
    // later reader (datastore()'s mmap, a stateless resync's --data) would
    // otherwise find it via an existence check. Remove it so the token path
    // truthfully returns to its "no file" state; dirty stays set and the next
    // real op reports the failure honestly (unchanged).
    std::error_code rmEc;
    std::filesystem::remove(cecon::to_native_path(*currentHbin_), rmEc);
  }
  // A failed snapshot (dead worker) leaves dirty set; the next real op reports it.
}

CobraRunResult SessionState::rollbackToFloor(const std::vector<std::string>& argv) {
  (void)argv;
  releaseResidentToken("rollback-floor");
  shadowValid_ = false;
  pendingDeltaValid_ = false;
  datastore_.reset();
  if (!lastMaterializedHead_.empty()) currentHbin_ = lastMaterializedHead_;
  residentComputedColClear();  // BUG-916: the floor predates the cached column.
  CobraRunResult r;
  r.exit_code = 3;  // data_io convention — surfaces as an engine error, never a silent wrong result.
  r.ok = false;
  nlohmann::json err;
  err["error"] = {{"code", "engine_error"},
                  {"message", "engine session restarted; state = last materialized snapshot"}};
  r.stderr_text = err.dump();
  return r;
}

CobraRunResult SessionState::finishSessionOp(CobraRunResult r, const std::vector<std::string>& argv) {
  if (!r.error.empty() && r.exit_code < 0) {
    // Hard worker failure (unavailable / died twice). Preserve correctness: a
    // real head can be reproduced by a one-shot spawn; a lost virtual head
    // rolls back to the last on-disk snapshot with an honest restart error.
    if (residentDirty_) return rollbackToFloor(argv);
    releaseResidentToken("hard-failure-oneshot");
    return runCobraOneShot(argv);
  }
  if (cobraSession_ && cobraSession_->consumeRespawned()) {
    // Transient death + respawn mid-op: the resident frame is gone. NOTE
    // (BUG-384 audit): if the head was a dirty VIRTUAL token, its bytes died
    // with the worker — clearing here leaves an unbacked head, which the
    // dispatch-exit audit reports. Recovery policy is a separate decision;
    // this site only stops being silent.
    releaseResidentToken("worker-respawn");
    shadowValid_ = false;
    if (!r.ok) return rollbackToFloor(argv);
  }
  return r;
}

CobraRunResult SessionState::runViaCobraSession(const std::vector<std::string>& argv) {
  // lastRunResident_ describes THIS call, so every exit below must set it —
  // including the two early one-shot exits and the two stateless fallbacks a
  // resident-routed call can still take. A stale `true` would let an opted-in
  // caller skip its disk-full guard on a run that really did write a file.
  lastRunResident_ = false;
  if (serveDisabled()) return runCobraOneShot(argv);
  CobraSession* cs = ensureCobraSession();
  if (cs->serveUnavailable()) return runCobraOneShot(argv);

  // Settings-change respawn (CECON_NUM_THREADS): persist resident state before
  // the thread-count respawn, then let the next frame op resync.
  if (cs->epochStale()) {
    ensureMaterializedHead("epoch-respawn");
    cs->respawnForEpoch();
    releaseResidentToken("epoch-respawn");
    shadowValid_ = false;
  }

  // Locate --data / --out and detect --keepif.
  std::optional<std::string> dataPath, outPath;
  int dataIdx = -1, outIdx = -1;
  bool hasKeepif = false;
  for (size_t i = 0; i < argv.size(); ++i) {
    if (argv[i] == "--keepif") {
      hasKeepif = true;
    } else if (i + 1 < argv.size() && argv[i] == "--data") {
      dataPath = argv[i + 1];
      dataIdx = static_cast<int>(i);
    } else if (i + 1 < argv.size() && argv[i] == "--out") {
      outPath = argv[i + 1];
      outIdx = static_cast<int>(i);
    }
  }

  // e(sample) T2: a read-only replay of the frame a fit read (LastModel::dataPath:
  // test/lincom/margins/estat) while the head is still that fit's mask-install
  // frame -- the same rows and values plus the hidden column -- reads the
  // resident head instead. Without it the replay resynced the worker to the base
  // file, writing the install frame to disk first ("resident-resync"), once per
  // fit. A --keepif or a by-group read keeps its stateless route (both read the
  // file); a mutation keeps its own --data.
  if (dataIdx >= 0 && !outPath.has_value() && !hasKeepif && statelessRead_ == 0 &&
      currentHbin_.has_value() && residentToken_ == *currentHbin_ &&
      esampleHeadIsInstallOf(*dataPath)) {
    std::vector<std::string> aliased = argv;
    aliased[static_cast<size_t>(dataIdx) + 1] = *currentHbin_;
    return runViaCobraSession(aliased);
  }
  // ... and the converse: a read-only call that will run STATELESS on that
  // install frame (a --keepif fit -- iv, xtreg, newey, prais, a TS regress --
  // or a by-group read) would write the whole frame to disk first
  // ("stateless-data"); it reads the on-disk base instead. Not when an argument
  // names the mask column, which only the install frame carries.
  if (dataIdx >= 0 && !outPath.has_value() && (hasKeepif || statelessRead_ > 0) &&
      currentHbin_.has_value() && *dataPath == *currentHbin_ && residentDirty_ &&
      residentToken_ == *currentHbin_ && esampleHeadIsInstallOf(esampleLastInstallBase)) {
    bool namesMask = false;
    for (const auto& s : argv) namesMask = namesMask || namesHiddenColumn(s);
    std::error_code bec;
    if (!namesMask &&
        std::filesystem::exists(cecon::to_native_path(esampleLastInstallBase), bec)) {
      std::vector<std::string> aliased = argv;
      aliased[static_cast<size_t>(dataIdx) + 1] = esampleLastInstallBase;
      return runViaCobraSession(aliased);
    }
  }

  CobraSession::Frames fr = cs->framesState();  // lazy-spawns the worker on first use.
  if (cs->serveUnavailable()) return runCobraOneShot(argv);  // old binary / spawn failed.
  const bool isMutation = outPath.has_value();

  // Route to the resident frame plane ONLY for read-only calls (no --out — no
  // temp file is created, so nothing can be read back as a stale/virtual file)
  // and for mutations explicitly opted in (gen/replace/egen/drop/keep — and, as
  // of S2, the if-only row filter). Everything else runs STATELESS (byte-equal
  // one-shot, real --out files):
  //   • no --data (version/convert/import-dta/postclose);
  //   • a §25-only engine (frames No);
  //   • every non-opted-in mutation (append/merge — whose exact stderr status
  //     counts are thus preserved — collapse/reshape/sort/duplicates/winsor/
  //     the egen-if/in + applyIfAndInFilter `in`-range chains, whose --out temps
  //     are read back AS FILES and so must stay real);
  //   • a READ-ONLY --keepif, which keeps its historical stateless routing.
  //
  // fix/verb-overhead-sweep S2: --keepif used to disqualify EVERY call, on the
  // strength of a §26 carve-out that says a resident frame-run cannot filter.
  // That text is stale — measured against the shipped 0.33.0 binary, bare
  // --keepif, --gen+--keepif and --drop+--keepif all filter correctly on a
  // resident frame and return in 58-96 ms against 527 ms stateless (the
  // stateless leg pays a full re-read of the on-disk frame plus a full --out
  // write; `drop if`/`keep if` on a 164 MB / 192-column frame was the single
  // largest verb cost in the campaign's attribution). The disqualification is
  // therefore lifted for the OPTED-IN MUTATION path only, and deliberately not
  // for read-only calls: a read-only --keepif writes no --out, so residency
  // would buy it nothing, while flipping ~9 call sites (summarize/regress/
  // tabulate/ttest/… via applyIfAndInFilter) onto a plane none of them asked
  // for. Opting in stays exactly what it has always meant — "this --out becomes
  // the session head and is never read back as a file" — so the caller, not
  // this router, remains the one place that fact is stated.
  // P2-PERF ARC-C C1-④: statelessRead_ > 0 is a caller saying "the --data I am
  // about to read is NOT my head, and I am about to say that G times" — see
  // SessionState::StatelessRead. Without it, a by-group loop's per-group reads
  // evict and resync the resident frame once per group.
  const bool goResident = fr != CobraSession::Frames::No && dataPath.has_value() &&
                          (isMutation ? residentEligible_ > 0 : (!hasKeepif && statelessRead_ == 0));

  // ROUTING CENSUS (env-gated, PIET_ROUTELOG). The resident plane's whole value is
  // that an opted-in mutation writes no full-frame --out: measured on the reference
  // slice, turning it OFF costs +28 such writes, +4.5 GB and +47 s of wall. But 196
  // writes REMAIN, so most calls are still leaving this router by the stateless
  // door and no existing counter says which door or why. One line per verb, with
  // the reason spelled the way the condition above reads.
  if (const char* rp = routelog_path()) {
    const char* why = goResident            ? "resident"
                    : fr == CobraSession::Frames::No ? "no-frames"
                    : !dataPath.has_value() ? "no-data"
                    : isMutation            ? "mutation-not-eligible"
                    : hasKeepif             ? "read-with-keepif"
                                            : "read-stateless-scope";
    if (std::FILE* f = std::fopen(rp, "a")) {
      std::string shape;
      for (size_t i = 1; i < argv.size(); ++i)
        if (argv[i].size() > 2 && argv[i][0] == '-' && argv[i][1] == '-')
          shape += (shape.empty() ? "" : ",") + argv[i];
      std::fprintf(f, "%s\t%s\t%s\n", argv.empty() ? "?" : argv[0].c_str(), why, shape.c_str());
      std::fclose(f);
    }
  }

  lastRunResident_ = goResident;
  if (!goResident) {
    // MUTRES-WIRE follow-up, 2026-08-22 — a claim of mine that measurement
    // refuted. The plan asserted pendingDelta_ could never be stale because it
    // is "overwritten wholesale at every mutation". It is not: it is written
    // ONLY on the resident leg (`r.ok && isMutation` below), so a STATELESS verb
    // left the previous resident run's varlist sitting there, and
    // mutationResult's wire branch would rebuild this frame's card from another
    // frame's columns. The completeness guard caught every one of them —
    // measured 115 wire-shape-mismatch events on a single corpus run, all of
    // them refusals that fell back correctly — but a guard catching a wrong
    // input is not a reason to keep feeding it one, and a stale list that
    // happened to have the RIGHT COLUMN COUNT would have passed.
    //
    // Cleared here, at the one place that knows this run is not producing a
    // delta: an empty vars is exactly what pendingWireDelta() reports as "no
    // wire available", so the caller falls back to the datastore path it always
    // used before.
    pendingDelta_.vars.clear();
    // Stateless. If --data is the CURRENT virtual head, snapshot it first — the
    // stateless worker reads --data from DISK (contract_A §25). (A read-only
    // --keepif, and every non-opted-in mutation, still resolve here: materialize
    // so the file reflects RAM, then run stateless with the original
    // --data/--out; clearing residentToken on an --out makes the next resident
    // verb frame-use the new real head.)
    if (dataPath.has_value() && *dataPath == residentToken_ && residentDirty_) {
      ensureMaterializedHead("stateless-data");
    }
    CobraRunResult r = cs->sendStateless(argv);
    // BUG-349 (ONESHOT-LASTFLUSH): this invalidation used to fire on EVERY stateless
    // mutation. Its stated reason — "a real --out was written, so the resident frame
    // is now stale" — only holds when the mutation READ the head: that is the case
    // where the branch above just materialized it and the new --out supersedes it.
    // A stateless mutation over some OTHER file (a §38 projection, a by-group slice)
    // leaves the worker's resident frame completely untouched; §25 stateless requests
    // never enter the frame session at all. Clearing the token there is not
    // conservative, it is destructive: it declares a DIRTY VIRTUAL head clean, and
    // the bytes that only ever existed in the worker are then unreachable — the next
    // reader gets `data_io: cannot open <the virtual head>`. Latent until now only
    // because every caller happened to filter the head itself.
    const bool consumedHead =
        dataPath.has_value() && !residentToken_.empty() && *dataPath == residentToken_;
    if (isMutation && consumedHead) {
      releaseResidentToken("stateless-consumed");  // a real --out was written; frame now stale.
    }
    return finishSessionOp(r, argv);
  }

  // Resident frame path. Resync (frame-use the REAL file) when the head moved
  // off the resident frame (fresh use/restore/import, or after a stateless
  // mutation). `*dataPath` is always a real file here.
  if (*dataPath != residentToken_) {
    // The worker's resident frame is about to be replaced by *dataPath. If the
    // OUTGOING resident frame is a dirty VIRTUAL head (its bytes exist only in
    // the worker), snapshot it to its currentHbin_ path FIRST — otherwise it is
    // lost, and a later verb whose --data is that (now orphaned) virtual head
    // frame-uses a file that was never written → data_io. This is the
    // post-estimation case: `gen x` (virtual head) then `test`/`margins`/
    // `lincom` re-run the base model with --data=lastModel.dataPath (a DIFFERENT
    // real file), resyncing away from the virtual head; the next data verb reads
    // currentHbin_ (still the gen head) and must find it on disk.
    ensureMaterializedHead("resident-resync");
    CobraFrameMeta useMeta;
    CobraRunResult ur = cs->frameUse(*dataPath, &useMeta);
    if (cs->framesState() == CobraSession::Frames::No) {
      lastRunResident_ = false;
      return finishSessionOp(cs->sendStateless(argv), argv);  // engine is §25-only.
    }
    if (!ur.ok) return finishSessionOp(ur, argv);  // bad file / dead worker.
    ++residentLiveCount_;  // worker accepted a frame — see residentLiveCount()
    residentToken_ = *dataPath;
    residentDirty_ = false;
    seedShadowFromFrameVars(useMeta);
    (void)cs->consumeRespawned();  // frameUse self-heals a transient respawn.
  }

  // frameRun with --data/--out stripped (the engine mutates the resident frame
  // in place, or reads it for a read-only verb; no scratch file is written).
  std::vector<std::string> inner;
  inner.reserve(argv.size());
  for (size_t i = 0; i < argv.size(); ++i) {
    int ii = static_cast<int>(i);
    // Guard on >= 0: a sentinel of -1 (flag absent) must NOT match index 0 via
    // `-1 + 1 == 0` — that would strip argv[0], the subcommand itself. Every
    // read verb has no --out (outIdx == -1), so the unguarded form silently
    // dropped the subcommand and the engine saw a headless argv (exit 2).
    if (dataIdx >= 0 && (ii == dataIdx || ii == dataIdx + 1)) continue;
    if (outIdx >= 0 && (ii == outIdx || ii == outIdx + 1)) continue;
    inner.push_back(argv[i]);
  }
  CobraFrameMeta runMeta;
  CobraRunResult r = cs->frameRun(inner, &runMeta);
  if (cs->framesState() == CobraSession::Frames::No) {
    lastRunResident_ = false;
    return finishSessionOp(cs->sendStateless(argv), argv);
  }
  if (r.ok && isMutation) {
    // Opted-in mutation succeeded: `out` is now a VIRTUAL head (no file). The
    // handler's imminent session.load(out) records the virtual chain; the
    // mutationResult shadow-card branch folds this delta.
    pendingDelta_ = runMeta;
    pendingDeltaValid_ = runMeta.valid;
    // S1b: pair the delta with what THIS argv asked the engine to do to the
    // column set/order (Kind::None for every verb but order/drop/keep). Kept
    // adjacent to the pendingDelta_ write on purpose — the two are consumed
    // together and must never drift apart.
    capturePendingCardPlan(argv);
    residentToken_ = *outPath;
    residentDirty_ = true;
    if (scratchTraceOn()) scratchTrace("MINT", "mutation-out", *outPath);
  }
  // Read-only (no --out) or exit!=0: resident token/dirty unchanged (the engine
  // guarantees no state advance on a non-zero frame run).
  return finishSessionOp(r, argv);
}

// fix/verb-overhead-sweep S1b. Recognize — with certainty, never by guess —
// the two resident mutations whose post-mutation card is an incremental fold
// of the pre-mutation card rather than a fresh read of the frame:
//   • `order`  — a column PERMUTATION. Every surviving column keeps its name,
//     type, missing-count and level count; only the ENTRY ORDER moves.
//   • `drop`/`keep` — a column REMOVAL (both compile to cobra `eval --drop`;
//     runKeep passes the complement). Every surviving column is byte-
//     identical; the card just loses entries.
// Neither is expressible through mutationResult's (newVars, droppedVars,
// rewrittenVars) hints — `order` passes all three empty — and the engine's run
// response carries only counts for them, so the argv the intercept is sending
// is the one place the change is stated. ANY argv shape not matched here
// leaves Kind::None and the card is rebuilt exactly as before.
void SessionState::capturePendingCardPlan(const std::vector<std::string>& argv) {
  pendingCardPlan_ = CardFoldPlan{};
  if (argv.empty()) return;

  if (argv[0] == "order") {
    // `order --data D --vars a,b[,…] [--first|--last|--before V|--after V] --out O`
    // (handlers_data_verbs.cpp runOrder).
    CardFoldPlan p;
    p.kind = CardFoldPlan::Kind::Order;
    bool last = false;
    std::string before, after, varspec;
    bool haveVars = false;
    for (size_t i = 1; i < argv.size();) {
      const std::string& f = argv[i];
      if (f == "--first") { i += 1; continue; }  // the default; recorded via the precedence below
      if (f == "--last") { last = true; i += 1; continue; }
      if (i + 1 >= argv.size()) return;  // dangling value-taking flag: shape not understood
      if (f == "--data" || f == "--out") { i += 2; continue; }
      if (f == "--vars") { varspec = argv[i + 1]; haveVars = true; i += 2; continue; }
      if (f == "--before") { before = argv[i + 1]; i += 2; continue; }
      if (f == "--after") { after = argv[i + 1]; i += 2; continue; }
      return;  // ANY other flag (--keepif, a future mode) — not a pure permutation.
    }
    if (!haveVars) return;
    // cmd_order's own precedence, mirrored exactly (apps/cmds/cmds_data.cpp
    // cmd_order): last > before > after > first, default first. NOT an
    // else-if over what runOrder happens to emit today — this must agree with
    // the engine even for an argv runOrder would never build.
    if (last) {
      p.mode = "last";
    } else if (!before.empty()) {
      p.mode = "before";
      p.anchor = before;
    } else if (!after.empty()) {
      p.mode = "after";
      p.anchor = after;
    } else {
      p.mode = "first";
    }
    // split_csv (apps/cmds/common.cpp), mirrored exactly: trim each element,
    // drop empties. A divergence here would silently permute by a name the
    // engine never saw, so it is copied rather than approximated.
    size_t start = 0;
    while (start <= varspec.size()) {
      size_t comma = varspec.find(',', start);
      std::string cur = varspec.substr(
          start, comma == std::string::npos ? std::string::npos : comma - start);
      size_t a = cur.find_first_not_of(" \t");
      if (a != std::string::npos) {
        size_t b = cur.find_last_not_of(" \t");
        p.vars.push_back(cur.substr(a, b - a + 1));
      }
      if (comma == std::string::npos) break;
      start = comma + 1;
    }
    if (p.vars.empty()) return;
    pendingCardPlan_ = std::move(p);
    return;
  }

  if (argv[0] == "rename") {
    // P2-PERF ARC-B/B2. `rename --data D --out O --pairs old=new[,old=new…]`
    // (handlers_data_verbs.cpp runRename). A rename is the purest fold there
    // is: the column keeps its position, its values, its dtype, its factor
    // dictionary and every count — only the name in slot i changes. Without a
    // plan here the card is rebuilt from the datastore, which for a resident
    // rename would mean materialising the very head the residency exists to
    // keep virtual.
    CardFoldPlan p;
    p.kind = CardFoldPlan::Kind::Rename;
    for (size_t i = 1; i < argv.size();) {
      const std::string& f = argv[i];
      if (i + 1 >= argv.size()) return;  // dangling / value-less flag: shape not understood
      if (f == "--data" || f == "--out") { i += 2; continue; }
      if (f == "--pairs") {
        // Same split as cmd_rename's own (comma-separated `old=new`), copied
        // rather than approximated: a divergence would rename a name the engine
        // never saw.
        const std::string& spec = argv[i + 1];
        size_t start = 0;
        while (start <= spec.size()) {
          size_t comma = spec.find(',', start);
          std::string cur =
              spec.substr(start, comma == std::string::npos ? std::string::npos : comma - start);
          size_t eq = cur.find('=');
          if (eq == std::string::npos) return;  // not a pair: shape not understood
          p.vars.push_back(cur.substr(0, eq));      // old
          p.renameTo.push_back(cur.substr(eq + 1));  // new
          if (comma == std::string::npos) break;
          start = comma + 1;
        }
        i += 2;
        continue;
      }
      return;  // ANY other flag — not a pure rename.
    }
    if (p.vars.empty() || p.vars.size() != p.renameTo.size()) return;
    pendingCardPlan_ = std::move(p);
    return;
  }

  if (argv[0] == "eval") {
    // `eval --data D --out O --drop v [--drop v …]` (handlers_m3.cpp runDrop/
    // runKeep). Every OTHER eval flag (--gen/--replace/--rename/--winsor/
    // --xtile/--ipolate/--keepif/--summarize) changes values, rows, or names,
    // so ONE of them anywhere in the argv refuses the whole plan: a fold that
    // guessed at a mixed rename+drop would ship a silently wrong card, which
    // is strictly worse than rebuilding it.
    CardFoldPlan p;
    p.kind = CardFoldPlan::Kind::Drop;
    for (size_t i = 1; i < argv.size();) {
      const std::string& f = argv[i];
      if (i + 1 >= argv.size()) return;  // dangling / value-less flag: shape not understood
      if (f == "--data" || f == "--out") { i += 2; continue; }
      if (f == "--drop") { p.vars.push_back(argv[i + 1]); i += 2; continue; }
      return;
    }
    if (p.vars.empty()) return;  // a no-op eval: nothing to fold, rebuild as before.
    pendingCardPlan_ = std::move(p);
  }
}

// Apply a captured plan to shadowCard_. Every guard below is an A12 fall-back:
// it answers "can I PROVE the post-mutation card equals this transformation of
// the pre-mutation card?", and any answer short of yes returns false with the
// card untouched, so mutationResult rebuilds it from the datastore exactly as
// it does today. The engine's own delta counts (dropped / cols / changed /
// added) are used as a CHECKSUM on the argv-derived plan: they were computed
// by diffing the real pre- and post-mutation frames, so a plan that disagrees
// with them describes a mutation that did not happen.
bool SessionState::applyPendingCardPlan(const CardFoldPlan& plan, const CobraFrameMeta& delta) {
  if (plan.kind == CardFoldPlan::Kind::None) return false;
  // A permutation and a removal both leave every SURVIVING column byte-
  // identical, so the engine must have reported no column changed, none added,
  // and an empty vars_delta. Anything else (a value rewrite, a new column, a
  // row-count change that re-materializes every buffer) is not this plan.
  //
  // Rename is exempt from the vars_delta clause and states its own conditions
  // below: the engine reports a renamed column THROUGH vars_delta (under its
  // new name), so an empty delta is exactly what a rename does NOT produce.
  // The other two clauses still bind it — a rename adds no column and changes
  // no value.
  if (plan.kind != CardFoldPlan::Kind::Rename) {
    if (delta.changed != 0 || delta.added != 0 || !delta.varsDelta.empty()) return false;
  }
  if (delta.cols <= 0) return false;  // an empty/columnless result — rebuild rather than reason about it.
  // The card must name every column exactly once, or "move name N" and "remove
  // name N" are ambiguous.
  std::unordered_map<std::string, size_t> idx;
  idx.reserve(shadowCard_.size());
  for (size_t i = 0; i < shadowCard_.size(); ++i) {
    if (!idx.emplace(shadowCard_[i].name, i).second) return false;
  }

  if (plan.kind == CardFoldPlan::Kind::Rename) {
    // Column COUNT unchanged, nothing dropped, and every old name present
    // exactly once while every new name is absent — the same question
    // runRename's own entry guard asks, re-asked here against the card the
    // fold is about to rewrite rather than the one it saw before the call.
    // MEASURED, not assumed: the engine reports a rename of k columns as
    // dropped=k, added=k, changed=0, cols unchanged, with vars_delta carrying
    // the k NEW names (`[rentrace] drop=1 add=1 chg=0 vd=1` on a one-pair
    // rename). It is a drop-and-add at the delta layer and a pure relabel at
    // the card layer; those two readings only agree if the counts line up
    // exactly with the pairs this plan claims, so require that.
    const int64_t k = static_cast<int64_t>(plan.vars.size());
    // PHASE-0: log every axis at once, before any short-circuit, so ONE run
    // says which guards would fire rather than only the first.
    foldTrace("rn-shape/k-drop-add", k * 1000000 + delta.dropped * 1000 + delta.added,
              delta.changed);
    foldTrace("rn-shape/vd-cols-card", static_cast<long long>(delta.varsDelta.size()) * 1000 + delta.cols,
              static_cast<long long>(shadowCard_.size()));
    if (delta.dropped != k || delta.added != k) {
      foldTrace("rn-counts", delta.dropped * 1000 + delta.added, delta.changed);
      return false;
    }
    // COMPLETE-DELTA rename. The `changed == 0` this branch used to require was
    // recorded as measured — and it is measured only of the frame it was taken
    // on. On the acceptance corpus HALF of a 322-rename run reports
    // `dropped=1, added=1, changed=11` with a varsDelta carrying ALL 12 columns
    // (PIET_FOLDTRACE, 2026-08-20). Those 154 declines fell into
    // mutationResult's full rebuild, and rename that way forced 220 of the
    // run's 476 head materializations — 46% of them, more than any other verb.
    //
    // A whole-frame report is not a reason to distrust the engine; it is MORE
    // information, not less. So when the delta covers every column, the card is
    // REBUILT FROM IT rather than folded — the identical discipline the row
    // filter branch below already uses, and for the identical reason: nothing
    // is assumed invariant, every metadata field comes from the engine, and the
    // result is committed only once whole.
    //
    // The fold path (changed == 0, varsDelta == just the k new names) is left
    // exactly as it was; this is an additional branch, not a relaxation of that
    // one. Order is carried from the card because a rename cannot permute
    // columns — the same reasoning, and the same limit, the row-filter branch
    // states about reading order off a delta that does not contract it.
    const bool completeDelta =
        delta.changed != 0 && static_cast<int64_t>(delta.varsDelta.size()) == delta.cols &&
        static_cast<int64_t>(shadowCard_.size()) == delta.cols;
    if (completeDelta) {
      std::unordered_map<std::string, std::string> newFor;  // old -> new
      for (size_t i = 0; i < plan.vars.size(); ++i) {
        if (idx.find(plan.vars[i]) == idx.end()) {
          foldTrace("rn-cd-oldmissing", 0, 0);
          return false;  // old name not in the card
        }
        if (!newFor.emplace(plan.vars[i], plan.renameTo[i]).second) {
          foldTrace("rn-cd-dupold", 0, 0);
          return false;  // same old named twice
        }
      }
      std::unordered_map<std::string, const CobraFrameVar*> byName;
      byName.reserve(delta.varsDelta.size());
      for (const auto& fv : delta.varsDelta) {
        if (!byName.emplace(fv.name, &fv).second) {
          foldTrace("rn-cd-dupdelta", 0, 0);
          return false;  // the engine named one column twice — refuse to guess
        }
      }
      std::vector<VarInfo> next;
      next.reserve(shadowCard_.size());
      for (const auto& existing : shadowCard_) {
        auto rn = newFor.find(existing.name);
        const std::string post = rn == newFor.end() ? existing.name : rn->second;
        auto it = byName.find(post);
        if (it == byName.end()) break;  // the engine did not report this column
        VarInfo vi = existing;          // keeps PIET-side display metadata (format/label)
        vi.name = post;
        vi.dtype = it->second->type;
        vi.nMissing = it->second->nMissing;
        vi.nLevels = it->second->nLevels;
        // Same rule as the row filter: trust the carried dictionary only while
        // it still agrees with the count the engine just reported.
        if (!vi.nLevels.has_value() || static_cast<size_t>(*vi.nLevels) != vi.levels.size()) {
          vi.levels.clear();
        }
        next.push_back(std::move(vi));
      }
      if (next.size() != shadowCard_.size()) {
        foldTrace("rn-cd-incomplete", static_cast<long long>(next.size()),
                  static_cast<long long>(shadowCard_.size()));
        return false;  // never ship a partial card
      }
      shadowCard_ = std::move(next);
      for (const auto& n : plan.vars) residentComputedColInvalidate(n);
      foldTrace("rn-cd-ok", k, delta.changed);
      return true;
    }
    if (delta.changed != 0) {
      foldTrace("rn-counts", delta.dropped * 1000 + delta.added, delta.changed);
      return false;
    }
    if (static_cast<int64_t>(delta.varsDelta.size()) != static_cast<size_t>(k)) {
      foldTrace("rn-vdsize", static_cast<long long>(delta.varsDelta.size()), k);
      return false;
    }
    if (static_cast<int64_t>(shadowCard_.size()) != delta.cols) {
      foldTrace("rn-cols", static_cast<long long>(shadowCard_.size()), delta.cols);
      return false;
    }
    std::vector<size_t> slots;
    slots.reserve(plan.vars.size());
    std::unordered_set<std::string> touched;
    std::unordered_set<std::string> newNames;
    for (size_t k = 0; k < plan.vars.size(); ++k) {
      auto it = idx.find(plan.vars[k]);
      if (it == idx.end()) return false;                  // old name not in the card
      if (!touched.insert(plan.vars[k]).second) return false;  // named twice
      if (idx.find(plan.renameTo[k]) != idx.end()) return false;  // new name already taken
      if (!newNames.insert(plan.renameTo[k]).second) return false;  // two olds -> one new
      slots.push_back(it->second);
    }
    // Every column the engine reported must be one this plan claims to have
    // created — never a third thing it renamed behind our back.
    for (const auto& fv : delta.varsDelta) {
      if (newNames.count(fv.name) == 0) return false;
    }
    for (size_t k = 0; k < slots.size(); ++k) shadowCard_[slots[k]].name = plan.renameTo[k];
    // Adopt whatever metadata the engine reported for the renamed columns —
    // it recomputed them against the real post-rename frame, so it outranks
    // the carried card. (A rename should not move any of these; taking them
    // from the engine means the card does not have to assume that.)
    for (const auto& fv : delta.varsDelta) {
      for (auto& existing : shadowCard_) {
        if (existing.name != fv.name) continue;
        existing.dtype = fv.type;
        existing.nMissing = fv.nMissing;
        existing.nLevels = fv.nLevels;
        break;
      }
    }
    // C4-L4: the cached computed column is keyed by NAME, so a rename of it
    // would leave the cache answering under a name that no longer exists.
    for (const auto& n : plan.vars) residentComputedColInvalidate(n);
    return true;
  }

  if (plan.kind == CardFoldPlan::Kind::Drop) {
    if (delta.dropped != static_cast<int64_t>(plan.vars.size())) return false;
    std::unordered_set<std::string> removed;
    for (const auto& n : plan.vars) {
      if (idx.find(n) == idx.end()) return false;   // not in the card (stale card, or engine no-op'd it)
      if (!removed.insert(n).second) return false;  // named twice — the engine's 2nd drop was a no-op
    }
    if (static_cast<int64_t>(shadowCard_.size() - removed.size()) != delta.cols) return false;
    std::vector<VarInfo> next;
    next.reserve(shadowCard_.size() - removed.size());
    for (auto& v : shadowCard_) {
      if (removed.find(v.name) == removed.end()) next.push_back(std::move(v));
    }
    shadowCard_ = std::move(next);  // erase preserves the survivors' relative order, as Frame::drop does.
    return true;
  }

  // Kind::Order — a pure permutation: the column COUNT must be unchanged.
  if (delta.dropped != 0) return false;
  if (static_cast<int64_t>(shadowCard_.size()) != delta.cols) return false;
  std::unordered_set<std::string> inList;
  for (const auto& n : plan.vars) {
    if (idx.find(n) == idx.end()) return false;
    // A duplicated --vars entry makes the engine name a column TWICE (see
    // Frame::order's rvalue overload, which falls back to the copy path for
    // exactly this): the result has more columns than the card. Refuse.
    if (!inList.insert(n).second) return false;
  }
  // Frame::order_names (src/data/frame.cpp), mirrored: `remaining` keeps the
  // pre-mutation order of everything not in the reorder list, and the list is
  // spliced in at one of four positions.
  std::vector<std::string> remaining;
  remaining.reserve(shadowCard_.size() - inList.size());
  for (const auto& v : shadowCard_) {
    if (inList.find(v.name) == inList.end()) remaining.push_back(v.name);
  }
  std::vector<std::string> newOrder;
  newOrder.reserve(shadowCard_.size());
  if (plan.mode == "first") {
    newOrder = plan.vars;
    newOrder.insert(newOrder.end(), remaining.begin(), remaining.end());
  } else if (plan.mode == "last") {
    newOrder = remaining;
    newOrder.insert(newOrder.end(), plan.vars.begin(), plan.vars.end());
  } else if (plan.mode == "before" || plan.mode == "after") {
    auto it = std::find(remaining.begin(), remaining.end(), plan.anchor);
    if (it == remaining.end()) return false;  // anchor absent or inside the list (the engine throws).
    std::ptrdiff_t cut = (it - remaining.begin()) + (plan.mode == "after" ? 1 : 0);
    newOrder.assign(remaining.begin(), remaining.begin() + cut);
    newOrder.insert(newOrder.end(), plan.vars.begin(), plan.vars.end());
    newOrder.insert(newOrder.end(), remaining.begin() + cut, remaining.end());
  } else {
    return false;  // an unrecognized mode string — never silently treat it as "first".
  }
  if (newOrder.size() != shadowCard_.size()) return false;
  std::vector<VarInfo> next;
  next.reserve(newOrder.size());
  for (const auto& n : newOrder) next.push_back(shadowCard_[idx[n]]);
  shadowCard_ = std::move(next);
  return true;
}

// fix/verb-overhead-sweep S2. Answers ONE question, and it is the load-bearing
// safety check of the row-filter fast path below: does `delta.varsDelta`
// describe every SURVIVING column of the post-mutation frame, so that a card
// can be rebuilt from it with nothing left to infer?
//
// WHY THIS IS A GUARD AND NOT AN ASSUMPTION. `vars_delta` is a DELTA, not a
// varlist, and its completeness for a row filter is INCIDENTAL rather than
// contracted — CoBrA Op verified the mechanism against the shipped 0.33.0
// engine and deliberately declined to contract it (contracting "complete
// vars_delta whenever card_stable is false" would oblige every structural verb
// to a full per-column n_missing rescan forever, which is precisely the card
// tax §30 exists to shed). What actually happens today: `changed` is computed
// by data-pointer identity, and Frame::keep_if (src/data/frame.cpp) allocates a
// fresh vector per column and moves it in, so every pointer moves and the delta
// HAPPENS to cover every column. A later lazy/CoW row selection — a rewrite
// this codebase already does elsewhere (§27 column-CoW, zero-copy views) —
// would preserve untouched pointers and this same field would go PARTIAL while
// the rows still change. That is a live risk, not a theoretical one.
//
// So: what may be relied on is `card_stable` as the validity signal and the
// correctness of the entries that ARE present. What may NOT be relied on is
// that all of them are present. Hence the count is compared against
// `delta.cols` — the POST-mutation column count — and never against the
// pre-mutation card's size alone: `collapse` already returns vars_delta=2 with
// dropped=13 TODAY, so a consumer reading vars_delta as "the whole varlist" is
// wrong on the shipped engine, not merely after some future change.
//
// The `added == 0 && dropped == 0` + exact name-set-equality requirements are
// stricter than bare completeness on purpose: together they prove the column
// SET is untouched, which is what lets the caller take the surviving column
// ORDER from the pre-mutation card instead of trusting the delta's own
// ordering (a delta has no contracted order either). Every case short of a
// provably complete, set-preserving delta returns false, and the caller then
// falls back to the full datastore rebuild — never to a partial card.
bool SessionState::residentCardDeltaComplete(const CobraFrameMeta& delta,
                                             const std::vector<VarInfo>& card) {
  if (delta.card_stable) return false;  // the card is still valid — not this branch's business.
  if (delta.added != 0) return false;  // column set moved (collapse/reshape).
  // INSPECT-2608: a bare `dropped != 0` also refused the case where the ONLY
  // columns dropped are ones this card never held — which is exactly what a
  // string-predicate row filter does now that it pushes a scratch mask column
  // and has the engine delete it in the same call (BUG-351). The card never
  // learned the mask, so nothing the card DESCRIBES moved, and rebuilding the
  // whole frame to discover that was 7 full materializations on a 7-filter
  // fixture. Refuse only when a dropped name is one the card actually holds;
  // when the engine reported a bare count with no names, keep the old
  // conservative refusal.
  if (delta.dropped != 0) {
    if (delta.droppedNames.size() != static_cast<size_t>(delta.dropped)) return false;
    for (const auto& gone : delta.droppedNames)
      for (const auto& have : card)
        if (have.name == gone) return false;
  }
  if (delta.cols <= 0) return false;  // an empty/columnless result — rebuild rather than reason about it.
  if (static_cast<int64_t>(delta.varsDelta.size()) != delta.cols) return false;  // PARTIAL delta.
  if (static_cast<int64_t>(card.size()) != delta.cols) return false;
  std::unordered_set<std::string> names;
  names.reserve(card.size());
  for (const auto& v : card) {
    if (!names.insert(v.name).second) return false;  // a card naming a column twice is unusable.
  }
  for (const auto& fv : delta.varsDelta) {
    if (names.erase(fv.name) != 1) return false;  // unknown name, or named twice by the delta.
  }
  return names.empty();  // every card entry was covered exactly once.
}

std::optional<nlohmann::ordered_json> SessionState::tryResidentMutationCard(
    const std::string& action, const std::vector<std::string>& newVars,
    const std::vector<std::string>& droppedVars, const std::vector<std::string>& rewrittenVars) {
  (void)rewrittenVars;  // the shadow fold keys on vars_delta, not the caller's hint lists.
  if (!pendingDeltaValid_) { foldTrace("no-pending-delta", 0, 0); return std::nullopt; }
  CobraFrameMeta delta = pendingDelta_;  // this delta belongs to exactly the just-completed mutation.
  CardFoldPlan plan = pendingCardPlan_;  // …and so does this plan (written at the same site).
  pendingDeltaValid_ = false;
  pendingCardPlan_ = CardFoldPlan{};
  if (!shadowValid_) {
    foldTrace("no-shadow-base", static_cast<long long>(plan.kind), 0);
    return std::nullopt;  // no base card to fold into.
  }

  if (plan.kind != CardFoldPlan::Kind::None) {
    // S1b: order/drop/keep. Checked BEFORE the vars_delta branch below, not
    // after: `order`'s vars_delta is EMPTY, so if a future engine ever reports
    // card_stable=true for it (Frame::order's structure_dirty_ is a write-path
    // fact — "a column permutation can't be patched onto the source" — not a
    // card fact, so that flag could legitimately move), the vars_delta branch
    // would "fold" it by changing nothing and ship an UNPERMUTED card. A plan
    // that declines therefore falls straight through to the rebuild rather
    // than into that branch.
    if (!applyPendingCardPlan(plan, delta)) {
      foldTrace("plan-declined", static_cast<long long>(plan.kind), delta.card_stable ? 1 : 0);
      if (!delta.card_stable) shadowValid_ = false;
      return std::nullopt;
    }
    foldTrace("plan-ok", static_cast<long long>(plan.kind), 0);
    // C4-L4: order/drop/keep restructures the column set/order — the cached
    // computed column may have been dropped or the frame reshaped; retire it
    // wholesale (conservative, and this branch is never the setcol hot path).
    residentComputedColClear();
  } else if (residentCardDeltaComplete(delta, shadowCard_)) {
    // fix/verb-overhead-sweep S2: a resident ROW FILTER (`keep if`/`drop if`).
    // card_stable is FALSE here and that is honest — a row filter changes
    // n_missing and n_levels per column, so the pre-filter card genuinely IS
    // invalid. The card is therefore REBUILT from the engine's freshly
    // RECOMPUTED vars_delta, never folded from what the card used to say.
    //
    // This distinction is the whole point of the branch. The S1b fold above
    // works because a permutation/removal provably cannot change any surviving
    // column's metadata; extending that argv-derived reasoning to a row filter
    // would ship stale missing-counts and factor levels — numbers that are
    // wrong in the variable panel and that no value pin would catch, since the
    // pinned card would simply keep agreeing with itself. Nothing here assumes
    // invariance: every metadata field below comes from the engine.
    //
    // Only the column ORDER is carried over, and only because
    // residentCardDeltaComplete proved added==0, dropped==0 and exact name-set
    // equality — i.e. the column set is untouched, and a row filter cannot
    // permute columns. Reading the order off the delta instead would trust an
    // ordering the delta does not contract.
    //
    // Note also that a false card_stable does NOT mean rows were dropped:
    // keep_if sets structure_dirty_ unconditionally, so a filter matching every
    // row lands here too. n_rows_after is taken from delta.rows below, so the
    // "did it shrink" question is never asked or assumed.
    std::unordered_map<std::string, const CobraFrameVar*> byName;
    byName.reserve(delta.varsDelta.size());
    for (const auto& fv : delta.varsDelta) byName.emplace(fv.name, &fv);
    // Built into a SEPARATE vector and committed only once whole: a mid-loop
    // bail must not be able to leave shadowCard_ half-engine/half-stale, which
    // is the partial-card outcome this branch exists to make impossible.
    std::vector<VarInfo> next;
    next.reserve(shadowCard_.size());
    for (const auto& existing : shadowCard_) {
      auto it = byName.find(existing.name);
      if (it == byName.end()) break;  // unreachable given the guard; refuse anyway.
      VarInfo vi = existing;  // keeps PIET-side display metadata (format/label).
      vi.dtype = it->second->type;
      vi.nMissing = it->second->nMissing;
      vi.nLevels = it->second->nLevels;
      // The level DICTIONARY is retained across a filter by the engine (a level
      // whose last row was filtered out keeps its slot), so the carried strings
      // stay right — but only trust them while they still agree with the count
      // the engine just reported. They are not emitted in varlist_after (only
      // n_levels is, datastore.cpp VarInfo::toJson); this keeps the in-memory
      // card from carrying a dictionary that contradicts its own count.
      if (!vi.nLevels.has_value() || static_cast<size_t>(*vi.nLevels) != vi.levels.size()) {
        vi.levels.clear();
      }
      next.push_back(std::move(vi));
    }
    if (next.size() != shadowCard_.size()) {
      shadowValid_ = false;
      return std::nullopt;  // fall back to the rebuild; never ship a partial card.
    }
    shadowCard_ = std::move(next);
  } else if (!delta.card_stable || delta.dropped > 0) {
    // This gate answers exactly one question: is the shadow metadata CARD
    // still valid (card_stable)? A row-count/column-set change makes it
    // invalid, same as no shadow base, same as a drop we cannot name from
    // vars_delta -> fall back to the real datastore rebuild (rare; never the
    // hot gen/replace/egen add-or-replace path). S2: an INVALID card whose
    // vars_delta is complete is handled by the branch above; reaching here with
    // card_stable false now means the delta could NOT rebuild it — a partial
    // delta, a moved column set, a mismatched name set — and this rebuild is
    // exactly the intended landing spot for all of them. `structure` deliberately
    // answers a different, write-path question (can a later `save`
    // byte-patch the file) and is no longer consulted here — see
    // CobraFrameMeta in cobra_session.h. Using `structure` here was the bug:
    // `sort` reports structure=true (no source path) but card_stable=true
    // (the card is still valid), so it was falling back on every call.
    // GA sweep 2026-08-22: this branch had NO foldTrace, so the census could see
    // that mutationResult forced 640 head materializations (82.28 GB — 71% of all
    // materialized bytes, the single largest coordinate left) but could not see
    // WHY, because the refusals that land here are invisible. Traced now with the
    // two sub-causes separated, since they want different fixes: an unstable card
    // is a rebuild we may be able to avoid, an unnameable drop is a delta the
    // engine could report more precisely.
    foldTrace(!delta.card_stable ? "cd-card-unstable" : "cd-unnamed-drop",
              static_cast<long long>(delta.dropped),
              static_cast<long long>(delta.varsDelta.size()));
    if (!delta.card_stable) shadowValid_ = false;
    return std::nullopt;
  } else {
    // Fold the changed/added columns into the shadow card, preserving column
    // order (append unknown names; update known names in place). PIET-side
    // label/levels metadata on an updated column is preserved.
    for (const auto& fv : delta.varsDelta) {
      // C4-L4: any fold touching the cached computed column's name supersedes
      // its values. (A setcol's OWN fold also lands here first — the handler
      // re-puts the fresh values right after mutationResult returns.)
      residentComputedColInvalidate(fv.name);
      bool found = false;
      for (auto& existing : shadowCard_) {
        if (existing.name == fv.name) {
          existing.dtype = fv.type;
          existing.nMissing = fv.nMissing;
          existing.nLevels = fv.nLevels;
          found = true;
          break;
        }
      }
      if (!found) {
        VarInfo vi;
        vi.name = fv.name;
        vi.dtype = fv.type;
        vi.nMissing = fv.nMissing;
        vi.nLevels = fv.nLevels;
        // BUG-365, and only when stated — see the sibling site above for why an
        // empty engine format must never overwrite the VarInfo default.
        if (!fv.format.empty()) {
          vi.format = fv.format;
          vi.formatStated = true;
        }
        if (!fv.label.empty()) vi.label = fv.label;
        shadowCard_.push_back(std::move(vi));
      }
    }
  }
  ++cardFoldCount_;  // selftest instrumentation: this card came from a fold, not a rebuild.
  // fix/sort-validate-lazy: this fold just brought shadowCard_ current for
  // the frame residentToken_ now names — the handler's session.load(out)
  // (called before mutationResult/this function) already advanced
  // currentHbin_ to that SAME path along the virtual-chain-continuation
  // branch (load() only leaves residentToken_ untouched when path ==
  // residentToken_), so tagging with residentToken_ here is equivalent to
  // tagging with currentHbin_ but stays consistent with
  // seedShadowFromFrameVars' own convention (see its doc comment).
  shadowCardHbin_ = residentToken_;
  // C4-L2A: rows ride with the card — the SAME delta.rows this function has
  // always trusted as n_rows_after (below), now also the card's row count.
  residentRows_ = delta.rows;

  nlohmann::ordered_json out;
  out["type"] = "data_mutation";
  out["action"] = action;
  auto vlist = varlistWithMetaFrom(shadowCard_);
  nlohmann::ordered_json varlistAfter = nlohmann::ordered_json::array();
  for (const auto& v : vlist) varlistAfter.push_back(v.toJson());
  updateNMissCache(vlist);  // keep the C2-F13a cache coherent for a later datastore-path verb.
  out["varlist_after"] = varlistAfter;
  out["n_rows_after"] = delta.rows;
  if (!newVars.empty()) out["new_vars"] = newVars;
  if (!droppedVars.empty()) out["dropped_vars"] = droppedVars;
  return out;
}

// SessionState::eNameOrderFor (declared with ENameContract in session_state.h).
std::optional<std::vector<std::string>> SessionState::eNameOrderFor(const std::string& kind) const {
  if (!eNameContract.has_value() || ePostedLive || !lastModel_.has_value() ||
      lastModel_->fitSerial != eNameContract->fitSerial ||
      eval.eStrings.get("cmd") != eNameContract->cmd)
    return std::nullopt;
  const ENameContract& c = *eNameContract;
  const std::optional<std::vector<std::string>>* list =
      kind == "scalars" ? &c.scalars : kind == "macros" ? &c.macros
                                    : kind == "matrices" ? &c.matrices : nullptr;
  if (list == nullptr || !list->has_value()) return std::nullopt;
  const std::vector<std::string>& names = **list;
  const auto listed = [&names](const std::string& n) {
    return std::find(names.begin(), names.end(), n) != names.end();
  };
  if (kind == "scalars") {
    for (const auto& item : eval.eScalars.items())
      if (!eval::isReservedStoreKey(item.first) && !listed(item.first)) return std::nullopt;
    const nlohmann::ordered_json* result =
        eval.lastModelResult.has_value() && eval.lastModelResult->is_object()
            ? &*eval.lastModelResult
            : nullptr;
    for (const auto& n : names) {
      if (std::find(c.missingScalars.begin(), c.missingScalars.end(), n) != c.missingScalars.end())
        continue;
      if (eval.eScalars.find(n) != nullptr) continue;
      std::string key = n;
      for (const auto& [k, v] : eval::eScalarKey())
        if (k == n) {
          key = v;
          break;
        }
      if (result == nullptr || !result->contains(key) || !result->at(key).is_number())
        return std::nullopt;
    }
  } else if (kind == "macros") {
    for (const auto& item : eval.eStrings.items())
      if (!eval::isReservedStoreKey(item.first) && !item.second.empty() && !listed(item.first))
        return std::nullopt;
    for (const auto& n : names) {
      const std::string* v = eval.eStrings.find(n);
      if (v == nullptr || v->empty()) return std::nullopt;
    }
  } else {
    for (const auto& item : eval.eMatrices)
      if (!listed(item.first)) return std::nullopt;
    for (const auto& n : names) {
      if (eval.findClassMatrix('e', n) != nullptr) continue;
      // e(b)/e(V) and the block-derived e(beta)/e(V_modelbased) resolve lazily.
      if (eval.resolveEMatrixFor(n, eval::EvalSession::EReader::kCopy).matrix.has_value())
        continue;
      return std::nullopt;
    }
  }
  return names;
}

}  // namespace piet
