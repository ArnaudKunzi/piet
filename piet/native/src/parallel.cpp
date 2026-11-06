// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// Group-level parallel execution — arc ① skeleton. See include/piet/parallel.h
// for the design authority, the User rulings, and the arc boundary (what this
// arc deliberately does NOT do).
#include "piet/parallel.h"
#include "piet/scratch_seq.h"  // BUG-389: registerScratchMint

#if !defined(_WIN32)
#include <sys/statvfs.h>
#endif
#include <cstdio>
#include <filesystem>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <optional>
#include <regex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#if defined(__APPLE__)
#include <sys/sysctl.h>
#endif

#include <map>
#if !defined(_WIN32)
#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

#include "piet/bundle_paths.h"
#include "piet/cobra_runner.h"
#include "piet/settings.h"
#include "piet/counters.h"
#include "piet/dispatcher.h"
#include "piet/doexec.h"
#include "piet/eval/errors.h"
#include "piet/strutil.h"

namespace piet::parallel {

namespace {

std::string trimCopy(const std::string& s) { return strutil::trim(s); }

ojson errBlock(const std::string& msg) { return eval::engineError(msg); }

// The parent<->worker protocol (spec in, result out) travels as CBOR, not JSON
// text: a local read raw from a Latin-1 file, or a block that `type`s one,
// holds bytes that are not UTF-8. A JSON dump throws on them (BUG-1131: the
// whole run ended in type_error.316), and replacing them would hand the task
// other bytes than the serial run sees (strlen 9 for "caf<E9> 12", not 7).
// CBOR carries a string's bytes as they are, both ways.
void writeProtocol(std::ostream& os, const ojson& doc) {
  const std::vector<std::uint8_t> bytes = ojson::to_cbor(doc);
  os.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
}

void writeProtocol(std::FILE* f, const ojson& doc) {
  const std::vector<std::uint8_t> bytes = ojson::to_cbor(doc);
  std::fwrite(bytes.data(), 1, bytes.size(), f);
}

// The bytes of `path`, or nullopt when it cannot be opened.
std::optional<std::string> readProtocolBytes(const std::string& path) {
  std::ifstream f(path, std::ios::binary);
  if (!f) return std::nullopt;
  return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

// Brace delta of one already-flattened body line, ignoring braces inside
// double-quoted strings (a `display "a { b"` must not open a block). Stata has
// no escape for `"` inside a plain double-quoted string, so a simple toggle is
// exactly right here; compound quotes (`"…"') are conservative — their inner
// braces are counted, which can only merge two tasks into one, never split a
// statement in half. Merging is the safe direction: a merged task still runs
// its statements in source order, in one worker.
int braceDelta(const std::string& line) {
  int d = 0;
  bool inStr = false;
  for (char c : line) {
    if (c == '"') {
      inStr = !inStr;
      continue;
    }
    if (inStr) continue;
    if (c == '{') ++d;
    if (c == '}') --d;
  }
  return d;
}

// kern.memorystatus_vm_pressure_level: 1 normal · 2 warn · 4 critical. Same
// gauge the fleet's own admission protocol reads, and for the same reason —
// §3.6 asks the parent to look before it spawns rather than let the machine
// discover the over-subscription for it. Non-Apple builds (the Windows parity
// track) get the neutral answer, never a fabricated one.
int readPressure() {
#if defined(__APPLE__)
  int level = 1;
  size_t sz = sizeof(level);
  if (sysctlbyname("kern.memorystatus_vm_pressure_level", &level, &sz, nullptr, 0) != 0) return 1;
  return level <= 0 ? 1 : level;
#else
  return 1;
#endif
}

}  // namespace

HeaderOptions parseHeader(const std::string& header) {
  HeaderOptions out;
  std::string hdr = trimCopy(header);

  // Strip the leading keyword. isLoopHeader already guaranteed it is `parallel`
  // (case-insensitively) — this only removes it and keeps the option tail.
  size_t sp = hdr.find_first_of(" \t,");
  std::string tail = (sp == std::string::npos) ? "" : trimCopy(hdr.substr(sp));
  if (tail.empty()) return out;  // bare `parallel {` — every default applies

  if (tail[0] != ',') {
    out.isError = true;
    out.error = errBlock("parallel: options must follow a comma (e.g. `parallel, workers(4) {`); "
                         "got '" + tail + "'");
    return out;
  }
  tail = trimCopy(tail.substr(1));
  if (tail.empty()) return out;  // `parallel, {` — comma with no options

  // Options are whitespace-separated `name` / `name(value)` tokens. Only
  // workers() exists in this arc; anything else is an honest error rather than
  // a silent no-op (see parseHeader's doc comment in parallel.h).
  static const std::regex reOpt(R"(([A-Za-z_][A-Za-z0-9_]*)(\(([^)]*)\))?)");
  auto begin = std::sregex_iterator(tail.begin(), tail.end(), reOpt);
  auto end = std::sregex_iterator();
  size_t consumed = 0;
  for (auto it = begin; it != end; ++it) {
    const std::smatch& m = *it;
    consumed += static_cast<size_t>(m.length(0));
    std::string name = strutil::lower(m.str(1));
    bool hasArg = m[2].matched;
    std::string arg = trimCopy(m.str(3));

    if (name == "workers") {
      if (!hasArg || arg.empty()) {
        out.isError = true;
        out.error = errBlock("parallel: option workers() requires a positive integer");
        return out;
      }
      bool numeric = true;
      for (char c : arg) {
        if (!std::isdigit(static_cast<unsigned char>(c))) numeric = false;
      }
      long v = numeric ? std::strtol(arg.c_str(), nullptr, 10) : 0;
      if (!numeric || v < 1) {
        out.isError = true;
        out.error = errBlock("parallel: workers(" + arg + ") is not a positive integer");
        return out;
      }
      out.workers = static_cast<int>(v);
      continue;
    }

    out.isError = true;
    out.error = errBlock("parallel: unrecognised option '" + name +
                         "' (this arc supports workers() only)");
    return out;
  }
  // Anything the option scanner could not account for (stray punctuation, an
  // unclosed paren) is refused rather than ignored — same honesty rule.
  if (consumed == 0) {
    out.isError = true;
    out.error = errBlock("parallel: cannot parse options '" + tail + "'");
  }
  return out;
}

namespace {
std::string humanMb(int64_t mb) {
  if (mb >= 1024) {
    char buf[32];
    std::snprintf(buf, sizeof buf, "%.1f GiB", static_cast<double>(mb) / 1024.0);
    return buf;
  }
  return std::to_string(mb) + " MB";
}
}  // namespace

Derivation deriveDefaultWorkers(const DerivationInputs& in) {
  // See parallel.h (User ruling 2026-09-02): every term is a constraint, the
  // smallest binds, and the banner names each one.
  Derivation d;
  const int64_t residentMb = in.residentBytes > 0 ? std::max<int64_t>(1, in.residentBytes / (1024 * 1024)) : 0;

  d.coresTerm = in.computeCores > 0 ? in.computeCores : std::max(1, in.perfCores);

  d.perWorkerMb = residentMb > 0 ? kWorkerBaseMb + kWorkerDataFactor * residentMb : kWorkerRssMb;
  const int64_t budget = in.memoryBudgetMb > 0 ? in.memoryBudgetMb : 0;
  d.memTerm = static_cast<int>(std::max<int64_t>(1, budget / d.perWorkerMb));

  int64_t n = std::min<int64_t>(d.coresTerm, d.memTerm);

  if (in.taskCount > 0) {
    d.taskTerm = static_cast<int>(std::min<int64_t>(in.taskCount, kMemoryCapWorkers));
    n = std::min<int64_t>(n, in.taskCount);
  }

  d.perTaskScratchMb = std::max<int64_t>(kTaskScratchMinMb, kTaskScratchFactor * residentMb);
  if (in.scratchFreeBytes >= 0) {
    const int64_t plannable = (in.scratchFreeBytes / (1024 * 1024)) * kScratchHeadroomPct / 100;
    d.diskTerm = static_cast<int>(std::max<int64_t>(1, plannable / d.perTaskScratchMb));
    n = std::min<int64_t>(n, plannable / d.perTaskScratchMb);
  }

  n = std::max<int64_t>(1, std::min<int64_t>(n, kMemoryCapWorkers));
  d.n = static_cast<int>(n);

  std::string r = "auto: cores " + std::to_string(d.coresTerm) +
                  (in.computeCores > 0 ? " (compute_cores)" : " (performance cores)") +
                  " · memory " + std::to_string(d.memTerm) + " (" + humanMb(budget) + " / " +
                  humanMb(d.perWorkerMb) + " per worker" +
                  (residentMb > 0 ? " @ " + humanMb(residentMb) + " resident" : ", resident unknown") + ")";
  if (in.taskCount > 0) r += " · tasks " + std::to_string(in.taskCount);
  if (in.scratchFreeBytes >= 0)
    r += " · disk " + std::to_string(d.diskTerm) + " (" + humanMb(in.scratchFreeBytes / (1024 * 1024)) +
         " free, half of it / " + humanMb(d.perTaskScratchMb) + " per task)";
  r += " -> " + std::to_string(d.n);
  d.reason = r;
  return d;
}

int perfCoreCount() {
#if defined(__APPLE__)
  int64_t p0 = 0;
  size_t sz = sizeof(p0);
  if (sysctlbyname("hw.perflevel0.logicalcpu", &p0, &sz, nullptr, 0) == 0 && p0 > 0)
    return static_cast<int>(p0);
#endif
  unsigned hc = std::thread::hardware_concurrency();
  return hc == 0 ? 1 : static_cast<int>(hc);
}

int64_t scratchFreeBytes(const std::string& dirIn) {
  std::string dir = dirIn;
  if (dir.empty()) {
    const char* e = std::getenv("PIET_WORKDIR");
    if (!e || !*e) e = std::getenv("TMPDIR");
    dir = (e && *e) ? std::string(e) : std::string("/tmp");
  }
#if !defined(_WIN32)
  // The directory may not exist yet (the block's scratch is created at fan-out);
  // walk up to the nearest existing ancestor — the volume is the same.
  std::filesystem::path pth(dir);
  std::error_code ec;
  while (!pth.empty() && !std::filesystem::exists(pth, ec)) pth = pth.parent_path();
  if (pth.empty()) return -1;
  struct statvfs st;
  if (statvfs(pth.c_str(), &st) != 0) return -1;
  return static_cast<int64_t>(st.f_bavail) * static_cast<int64_t>(st.f_frsize);
#else
  (void)dir;
  return -1;
#endif
}

int64_t residentBytesEstimate(const SessionState& session) {
  if (!session.currentHbin().has_value()) return 0;
  std::error_code ec;
  const auto sz = std::filesystem::file_size(*session.currentHbin(), ec);
  if (ec) return 0;
  return static_cast<int64_t>(sz);
}

WorkerBudget resolveWorkers(int requested) { return resolveWorkers(requested, ResolveContext{}); }

WorkerBudget resolveWorkers(int requested, const ResolveContext& ctx) {
  WorkerBudget b;
  b.requested = requested;
  unsigned hc = std::thread::hardware_concurrency();
  b.cores = hc == 0 ? 1 : static_cast<int>(hc);
  b.pressure = readPressure();

  // computeCores is read on BOTH paths now (ONESHOT-P9N4 C): the explicit path
  // still ignores memory_budget_mb — an explicit workers(N) is the user
  // overriding our memory arithmetic on purpose (parallel.h §8 #2) — but the
  // core budget is not a cap on N, it is the width being divided, and ruling #11
  // put that under compute_cores regardless of how N was chosen.
  const settings::Settings st = settings::load();

  if (requested > 0) {
    b.n = requested;
  } else {
    // User ruling 2026-09-02: derived from the budgets AND the work — see
    // parallel.h's derivation block. Reading the settings file directly is
    // correct even headless: settings::load() resolves the same path the GUI
    // writes and never throws. The effective memory budget is the recommended
    // one when the slider was never moved (0), exactly as the engine receives it.
    DerivationInputs in;
    in.computeCores = st.computeCores;
    in.perfCores = perfCoreCount();
    in.memoryBudgetMb = st.memoryBudgetMb > 0 ? st.memoryBudgetMb
                                              : settings::detectSysInfo().recommendedBudgetMb;
    in.residentBytes = ctx.residentBytes;
    in.taskCount = ctx.taskCount;
    in.scratchFreeBytes = scratchFreeBytes(ctx.scratchDir);
    const Derivation d = deriveDefaultWorkers(in);
    b.n = d.n;
    b.reason = d.reason;
  }

  // Pressure clamp — applies to an explicit workers() too, because the machine
  // does not care who asked. Reported, never silent.
  //
  // CORRECTED 2026-08-14 (P9 entry wave) — warn level is REPORTED, NOT CLAMPED.
  // The original arc① rule halved the budget at `kern.memorystatus_vm_pressure_level
  // >= 2` (warn). Measured on the reference machine (8 GiB M1): the gauge reads **2
  // with 45% of memory free**, i.e. macOS sits at warn under ordinary use, so halving
  // there is not a safety valve — it is a permanent off switch. (Re-measured
  // 2026-08-14 after the User freed memory: the gauge reads **1**. Both readings are
  // ordinary states of this machine, which is the point — the level is a function of
  // the moment, not of the workload, so it can gate a report but never a budget.) `parallel, workers(3)` resolved to 1 worker on every run, which made
  // the feature untestable and the probe's V-2 anti-vacuity cell unreachable
  // ([[gate-law-code-divergence]]: the law says N, the code silently gives 1).
  //
  // Critical (>= 4) still clamps to 1: that level does mean the kernel is in
  // trouble, and one worker is the honest response. Warn now only names itself in
  // the banner, so the user still learns the machine was under pressure without
  // the budget being silently rewritten. The real memory bound stays where it was
  // measured to belong — kMemoryCapWorkers (SE-062), on the default path.
  if (b.pressure >= 4) {
    if (b.n > 1) {
      b.n = 1;
      b.reason = "memory pressure critical (P=" + std::to_string(b.pressure) +
                 "): clamped to 1";
    }
  } else if (b.pressure >= 2) {
    const std::string note = "memory pressure warn (P=" + std::to_string(b.pressure) +
                             "), budget unchanged";
    b.reason = b.reason.empty() ? note : b.reason + "; " + note;
  }

  // §3.5 core budget, computed AFTER every clamp so the divisor is the worker
  // count that will actually run. Ruling #11: compute_cores is the total core
  // budget, so a user who set it is telling us the whole fan-out may use that
  // many cores — not that many cores EACH. 0 (auto) = the hardware width, which
  // is what this has always divided.
  {
    const int budgetCores = st.computeCores > 0 ? st.computeCores : b.cores;
    const int divisor = b.n > 0 ? b.n : 1;
    b.engineThreads = std::max(1, budgetCores / divisor);
  }
  // The memory budget divides the same way (2026-09-02): the user's number, or
  // the recommended one when the slider was never moved — the engine reads an
  // undeclared budget as unbounded, so a worker must always be told its share.
  {
    int64_t total = st.memoryBudgetMb > 0 ? st.memoryBudgetMb
                                          : settings::detectSysInfo().recommendedBudgetMb;
    if (total <= 0) total = 2048;
    const int divisor = b.n > 0 ? b.n : 1;
    b.engineMemoryMb = std::max<int64_t>(64, total / divisor);
  }
  return b;
}

std::vector<std::string> decomposeTasks(const std::vector<std::string>& bodyLines) {
  std::vector<std::string> tasks;
  std::string cur;
  int depth = 0;
  for (const auto& raw : bodyLines) {
    std::string line = trimCopy(raw);
    if (line.empty() && depth == 0) continue;  // blank filler between statements
    if (!cur.empty()) cur += "\n";
    cur += raw;
    depth += braceDelta(line);
    if (depth <= 0) {
      tasks.push_back(cur);
      cur.clear();
      depth = 0;
    }
  }
  if (!cur.empty()) tasks.push_back(cur);  // unbalanced tail — keep it, never drop
  return tasks;
}

ojson buildTaskSpec(const std::vector<std::string>& tasks, const SessionState& session,
                    const WorkerBudget& budget, const std::string& parentHead) {
  ojson spec = ojson::object();
  spec["schema"] = "piet.parallel.task_spec/2";
  spec["workers"] = budget.n;
  spec["cores"] = budget.cores;
  // §3.5 nested-parallelism budget: N × CECON_NUM_THREADS ≈ the core budget.
  // Recorded in the spec (rather than computed in the child) so the parent owns
  // the whole core budget and a worker never has to guess how many siblings it
  // has. The arithmetic itself lives in resolveWorkers (ONESHOT-P9N4 C) — it was
  // duplicated here and at the spawn site, which is one copy too many for a
  // number that must agree with itself, and neither copy honoured ruling #11's
  // compute_cores-as-total-budget.
  spec["engine_threads"] = budget.engineThreads;
  spec["engine_memory_mb"] = budget.engineMemoryMb;
  spec["parent_head"] = parentHead;  // "" = no snapshot (arc ①: nothing spawns)
  spec["cwd"] = session.eval.cwd;

  // §2.4 "들어가는 것": copied, read-only, never returned.
  //
  // CORRECTED 2026-08-14 (P9 entry wave). The arc① comment here said locals are
  // absent on purpose "because the parent expands macros before decomposition".
  // The parent does no such thing, and it must not: dispatcher.cpp builds a
  // statement's ECHO block from the RAW line and expands macros inside dispatch,
  // so shipping pre-expanded text would change the echoed bytes of every
  // macro-bearing statement and break the byte law (§2.5) this design rests on.
  // Task text therefore travels raw, and the child reproduces the parent's
  // expansion by inheriting the local table. Locals stay strictly one-way — a
  // child never returns them. This also makes a parent-minted `tempfile` expand
  // in the child to the parent's own concrete path, i.e. to exactly what serial
  // execution would have used.
  ojson locals = ojson::object();
  for (const auto& [k, v] : session.eval.locals.items()) locals[k] = v;
  spec["locals"] = locals;

  // BUG-347: session `set` state. Measured on the acceptance corpus — the
  // preamble runs `set graphics off`, a worker inherited nothing, and every
  // graph block a worker emitted carried `suppressed: false` where the serial
  // run said `true`: 33 blocks flipped on EVERY parallel run. That is a §2.5
  // byte-law violation the probe could not see, because its fixture never
  // issues a `set`. These four are what `set` actually persists on the
  // session (session_state.h): graphics on|off, level #, seed #, and
  // type float|double (c(type), BUG-471 T2 — without it a worker's untyped
  // gen/egen stores float after the parent's `set type double`). They are
  // one-way like every other §2.4 input — a child never returns them.
  // BUG-1432: `set extcompat` lives in a process-global rather than on the
  // session (settings.cpp), and a worker that is not told re-reads the durable
  // setting: after `set extcompat off` a task ran the community names the
  // serial run refuses r(199). It travels as the parent's live value.
  ojson sets = ojson::object();
  sets["graphics_off"] = session.graphicsOff;
  sets["level"] = session.level;
  if (session.seed.has_value()) sets["seed"] = *session.seed;
  sets["type"] = session.defaultStorageType;
  sets["extcompat"] = settings::extcompatEnabled();
  spec["set_state"] = sets;

  // Programs: without these a child cannot run a task that calls one, and the
  // failure does not even name itself — an unknown command hits the no-data
  // precondition first and reports "No dataset is loaded" (BUG-342's second
  // layer). arc① never shipped them because nothing was ever spawned.
  ojson progs = ojson::array();
  for (const auto& [name, def] : session.programs) {
    ojson pd = ojson::object();
    pd["name"] = def.name.empty() ? name : def.name;
    pd["rclass"] = def.rclass;
    ojson body = ojson::array();
    for (const auto& l : def.body) body.push_back(l);
    pd["body"] = body;
    progs.push_back(pd);
  }
  spec["programs"] = progs;

  // BUG-1430 (FU-223): per-variable metadata. A worker's session.load of the
  // parent head reads only what the head file states (format, label), so the
  // storage type a declaration, gen's implicit float or a widening replace
  // recorded was lost there: a task's `replace x = x/3` into the parent's
  // undeclared gen stored the double where the serial run rounds it to float
  // (`count if x == float(1/3)`: 0 in the worker, 3 serially). One-way like the
  // rest; the worker applies it to the head it loads.
  ojson varMeta = ojson::object();
  for (const auto& [name, m] : session.varMeta) {
    ojson e = ojson::object();
    if (m.label.has_value()) e["label"] = *m.label;
    if (m.format.has_value()) e["format"] = *m.format;
    if (m.stataType.has_value()) e["stata_type"] = *m.stataType;
    varMeta[name] = e;
  }
  spec["var_meta"] = varMeta;
  ojson implicitFloat = ojson::array();
  for (const auto& name : session.implicitFloatVars) implicitFloat.push_back(name);
  spec["implicit_float"] = implicitFloat;

  ojson globals = ojson::object();
  for (const auto& [k, v] : session.eval.globals.items()) globals[k] = v;
  spec["globals"] = globals;
  ojson scalars = ojson::object();
  for (const auto& [k, v] : session.eval.scalars.items()) scalars[k] = v;
  spec["scalars"] = scalars;

  ojson taskArr = ojson::array();
  for (size_t i = 0; i < tasks.size(); ++i) {
    ojson t = ojson::object();
    t["index"] = static_cast<int>(i);  // SOURCE order — the replay key (§2.5)
    t["source"] = tasks[i];
    taskArr.push_back(t);
  }
  spec["tasks"] = taskArr;
  return spec;
}

std::string dumpTaskSpecIfRequested(const ojson& spec) {
  const char* dir = std::getenv("PIET_PARALLEL_DUMP_SPEC");
  if (dir == nullptr || *dir == '\0') return "";
  std::error_code ec;
  std::filesystem::create_directories(dir, ec);
  std::filesystem::path p = std::filesystem::path(dir) / "task_spec.json";
  std::ofstream f(p);
  if (!f) return "";
  // A human-readable copy: a byte that is not UTF-8 shows as U+FFFD (BUG-1131).
  f << spec.dump(2, ' ', false, ojson::error_handler_t::replace) << "\n";
  f.close();
  return p.u8string();
}

int runParallelWorker(const std::string& specPath) {
  ojson out = ojson::object();
  ojson spec;
  {
    const std::optional<std::string> bytes = readProtocolBytes(specPath);
    if (!bytes) {
      out["ok"] = false;
      out["driver_error"] = "cannot open task spec: " + specPath;
      writeProtocol(stdout, out);
      return 1;
    }
    try {
      spec = ojson::from_cbor(*bytes);
    } catch (const std::exception& e) {
      out["ok"] = false;
      out["driver_error"] = std::string("bad task spec: ") + e.what();
      writeProtocol(stdout, out);
      return 1;
    }
  }

  // §3.3: the child's isolation is the PROCESS. Its own scratch workdir, its own
  // `cobra serve`, its own est-handle space — all free, none re-implemented. The
  // workdir comes from the same makeWorkdir contract every other headless entry
  // point uses; here we take it from the spec when the parent named one so the
  // parent can reap it, else from $PIET_WORKDIR/tmp like --run-block does.
  std::string workdir = spec.value("workdir", std::string(""));
  if (workdir.empty()) {
    const char* env = std::getenv("PIET_WORKDIR");
    workdir = (env && *env) ? std::string(env) : std::string("/tmp");
    workdir += "/piet_parallel_worker";
  }
  std::error_code ec;
  std::filesystem::create_directories(workdir, ec);

  // §3.5: the parent owns the total core budget; the child just obeys it.
  if (spec.contains("engine_threads") && spec["engine_threads"].is_number_integer()) {
    int nt = spec["engine_threads"].get<int>();
    if (nt >= 1) {
      ::setenv("CECON_NUM_THREADS", std::to_string(nt).c_str(), 1);
      ::setenv("VECLIB_MAXIMUM_THREADS", std::to_string(nt).c_str(), 1);
    }
  }
  if (spec.contains("engine_memory_mb") && spec["engine_memory_mb"].is_number_integer()) {
    const int64_t mb = spec["engine_memory_mb"].get<int64_t>();
    if (mb >= 1) ::setenv("CECON_MEMORY_BUDGET_MB", std::to_string(mb).c_str(), 1);
  }

  SessionState session(workdir);
  if (spec.contains("cwd") && spec["cwd"].is_string() && !spec["cwd"].get<std::string>().empty()) {
    session.cwd = spec["cwd"].get<std::string>();
    session.eval.cwd = *session.cwd;
  }
  if (spec.contains("globals") && spec["globals"].is_object()) {
    for (auto it = spec["globals"].begin(); it != spec["globals"].end(); ++it) {
      if (it.value().is_string()) session.eval.globals.set(it.key(), it.value().get<std::string>());
    }
  }
  if (spec.contains("scalars") && spec["scalars"].is_object()) {
    for (auto it = spec["scalars"].begin(); it != spec["scalars"].end(); ++it) {
      if (it.value().is_string()) session.eval.scalars.set(it.key(), it.value().get<std::string>());
    }
  }
  if (spec.contains("locals") && spec["locals"].is_object()) {
    for (auto it = spec["locals"].begin(); it != spec["locals"].end(); ++it) {
      if (it.value().is_string()) session.eval.locals.set(it.key(), it.value().get<std::string>());
    }
  }
  // BUG-347: apply the parent's `set` state before any task runs — a graph
  // emitted before this point would carry the wrong `suppressed` flag, which is
  // exactly the divergence this fixes.
  if (spec.contains("set_state") && spec["set_state"].is_object()) {
    const ojson& st = spec["set_state"];
    if (st.contains("graphics_off") && st["graphics_off"].is_boolean())
      session.graphicsOff = st["graphics_off"].get<bool>();
    if (st.contains("level") && st["level"].is_number_integer())
      session.level = st["level"].get<int>();
    if (st.contains("seed") && st["seed"].is_number_integer())
      session.seed = st["seed"].get<long long>();
    if (st.contains("type") && st["type"].is_string())
      session.defaultStorageType = st["type"].get<std::string>();
    if (st.contains("extcompat") && st["extcompat"].is_boolean())
      settings::setExtcompatEnabled(st["extcompat"].get<bool>());
  }
  if (spec.contains("programs") && spec["programs"].is_array()) {
    for (const auto& pd : spec["programs"]) {
      ProgramDef def;
      def.name = pd.value("name", std::string());
      if (def.name.empty()) continue;
      def.rclass = pd.value("rclass", false);
      for (const auto& l : pd.value("body", ojson::array()))
        if (l.is_string()) def.body.push_back(l.get<std::string>());
      session.programs[def.name] = def;
      // The archive too: a task whose own text contains `clear all` must not
      // lose the inherited definitions either (BUG-342, same rule in-child).
      session.programArchive[def.name] = def;
    }
  }
  // The parent's resident head, when it snapshotted one (arc ②). Loading it
  // read-only is what makes N children share one set of file pages (§2.4).
  std::string parentHead = spec.value("parent_head", std::string(""));
  if (!parentHead.empty()) {
    Datastore* headDs = nullptr;
    try {
      session.load(parentHead);
      headDs = session.datastore();  // fail loudly here, not on the first task's first verb
    } catch (const std::exception& e) {
      out["ok"] = false;
      out["driver_error"] = std::string("parent_head load failed: ") + e.what();
      writeProtocol(stdout, out);
      return 1;
    }
    // BUG-1430: the parent's per-variable metadata, for the columns the head
    // holds (a claim on a non-column is the BUG-190 family).
    if (headDs != nullptr && spec.contains("var_meta") && spec["var_meta"].is_object()) {
      for (auto it = spec["var_meta"].begin(); it != spec["var_meta"].end(); ++it) {
        if (!headDs->has(it.key()) || !it.value().is_object()) continue;
        const ojson& e = it.value();
        VarMeta& m = session.varMeta[it.key()];
        if (e.contains("label") && e["label"].is_string()) m.label = e["label"].get<std::string>();
        if (e.contains("format") && e["format"].is_string())
          m.format = e["format"].get<std::string>();
        if (e.contains("stata_type") && e["stata_type"].is_string())
          m.stataType = e["stata_type"].get<std::string>();
      }
    }
    if (headDs != nullptr && spec.contains("implicit_float") && spec["implicit_float"].is_array()) {
      for (const auto& name : spec["implicit_float"])
        if (name.is_string() && headDs->has(name.get<std::string>()))
          session.implicitFloatVars.insert(name.get<std::string>());
    }
  }

  ojson results = ojson::array();
  if (spec.contains("tasks") && spec["tasks"].is_array()) {
    for (const auto& t : spec["tasks"]) {
      ojson row = ojson::object();
      row["index"] = t.value("index", 0);
      std::string source = t.value("source", std::string(""));
      // G-4: baseline AFTER init, so the parent never folds in the worker's own
      // startup tax (head load, session construction) — costs the serial run
      // never pays and which would make the parallel emit differ for a reason
      // that has nothing to do with the tasks.
      std::vector<int64_t> before;
      before.reserve(piet::counters::foldableAccumulators().size());
      for (const auto& [name, slot] : piet::counters::foldableAccumulators()) {
        (void)name;
        before.push_back(*slot);
      }
      try {
        doexec::ExecuteResult r = doexec::executeDoSource(source, session, piet::dispatch);
        row["blocks"] = r.blocks;
        // §2.6: a task's error lives inside that task. `failed` is the halt
        // authority doexec itself computes (stoppedAtLine), the same signal
        // loops.cpp's runBody keys off — so an error a nested `capture`
        // swallowed is NOT a task failure here either.
        row["failed"] = r.stoppedAtLine.has_value();
        if (r.stoppedAtReason.has_value()) row["stopped_at_reason"] = *r.stoppedAtReason;
        // §2.6: a task's `exit` must be promoted to a parent halt, so it has to
        // survive the process boundary rather than dying with the child.
        if (r.stoppedAtExitCode.has_value()) row["exit_code"] = *r.stoppedAtExitCode;
      } catch (const std::exception& e) {
        // Never a silent missing result (§2.6's "조용한 결과 누락 금지").
        ojson eb = ojson::array();
        eb.push_back(errBlock(std::string("parallel worker: task threw: ") + e.what()));
        row["blocks"] = eb;
        row["failed"] = true;
      }
      {
        ojson delta = ojson::object();
        size_t i = 0;
        for (const auto& [name, slot] : piet::counters::foldableAccumulators()) {
          const int64_t d = *slot - before[i++];
          if (d != 0) delta[name] = d;
        }
        row["counters_delta"] = delta;
      }
      results.push_back(row);
    }
  }
  out["ok"] = true;
  out["tasks"] = results;
  writeProtocol(stdout, out);
  return 0;
}


// ---------------------------------------------------------------------------
// arc ② — real process fan-out (see parallel.h::runTasksInWorkers).

#if !defined(_WIN32)
namespace {

// Spawn one worker for `spec`, with its protocol stdout redirected to
// `outPath`. stderr is INHERITED on purpose: the child's diagnostics reach the
// user's terminal live, and stderr is never part of any byte comparison (the
// probe's stream-separation law). Returns the pid, or -1.
pid_t spawnWorker(const std::string& exe, const std::string& specPath,
                  const std::string& outPath, int engineThreads, int64_t engineMemoryMb,
                  const std::string& cobraBin) {
  pid_t pid = ::fork();
  if (pid < 0) return -1;
  if (pid == 0) {
    int fd = ::open(outPath.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) ::_exit(120);
    if (::dup2(fd, STDOUT_FILENO) < 0) ::_exit(121);
    if (fd != STDOUT_FILENO) ::close(fd);
    // The recursion guard: a task that itself contains a parallel block runs
    // that block serially instead of forking a second generation.
    ::setenv("PIET_PARALLEL_WORKER", "1", 1);
    // §3.5 core budget. The spec carries this too and the child applies it, but
    // setting it here as well means the value is in place before ANY engine can
    // start, and it is what closes the N^2 oversubscription hole: `--do` never
    // calls applyComputeCores, so without this a worker's cobra would size its
    // pool from hardware_concurrency() and N workers would each take the whole
    // machine.
    if (engineThreads >= 1) {
      ::setenv("CECON_NUM_THREADS", std::to_string(engineThreads).c_str(), 1);
      // Accelerate's BLAS pool reads this at library load — before the engine
      // could set it for itself — so the share goes in here, with the threads.
      ::setenv("VECLIB_MAXIMUM_THREADS", std::to_string(engineThreads).c_str(), 1);
    }
    // §3.5's memory half (2026-09-02): the worker's engine bounds its own
    // per-thread scratch to this share (src/core/threads.h threads_for_work).
    if (engineMemoryMb >= 1)
      ::setenv("CECON_MEMORY_BUDGET_MB", std::to_string(engineMemoryMb).c_str(), 1);
    // Parent and children must provably resolve the SAME engine binary; the
    // parent's resolution result is passed down rather than re-derived.
    if (!cobraBin.empty()) ::setenv("COBRA_BIN", cobraBin.c_str(), 1);
    // BUG-393 (2026-09-02): the ONE spawn site that did not carry the allocator
    // knob. cobra_session.cpp sets MallocSpaceEfficient=1 for every engine child
    // (FRAG 89%→0%, peak −53%, measured 2026-08-17); a parallel worker is a whole
    // piet_native + its own engine, so without it every worker ran the fragmenting
    // default. Same PIET_NO_SPACE_EFFICIENT off-arm so the A/B stays one env away.
    if (std::getenv("PIET_NO_SPACE_EFFICIENT") == nullptr) ::setenv("MallocSpaceEfficient", "1", 1);
    ::execl(exe.c_str(), exe.c_str(), "--parallel-worker", specPath.c_str(),
            static_cast<char*>(nullptr));
    ::_exit(122);  // exec failed
  }
  return pid;
}

// Read one worker's protocol object and fold its single task row into `out`.
void collectWorker(const std::string& outPath, int taskIndex, int childStatus,
                   TaskOutcome& out) {
  const std::optional<std::string> bytes = readProtocolBytes(outPath);
  if (!bytes) {
    out.driverError = "worker produced no output file";
    return;
  }
  ojson doc;
  try {
    doc = ojson::from_cbor(*bytes);
  } catch (const std::exception& e) {
    out.driverError = std::string("worker output was unreadable: ") + e.what();
    return;
  }
  if (!doc.value("ok", false)) {
    out.driverError = doc.value("driver_error", std::string("worker reported failure"));
    return;
  }
  for (const auto& row : doc.value("tasks", ojson::array())) {
    if (row.value("index", -1) != taskIndex) continue;
    out.delivered = true;
    out.blocks = row.value("blocks", ojson::array());
    out.failed = row.value("failed", false);
    if (row.contains("exit_code") && row["exit_code"].is_number_integer()) {
      out.hasExitCode = true;
      out.exitCode = row["exit_code"].get<int>();
    }
    out.stoppedAtReason = row.value("stopped_at_reason", std::string());
    out.countersDelta = row.value("counters_delta", ojson::object());
    return;
  }
  // A worker that exited without ever emitting this task's row is a death, not
  // a silent success. Never re-run it: the task may have completed some of its
  // side effects, and at-most-once is the only safe rule for work whose effects
  // are files on disk.
  out.driverError = "worker exited (status " + std::to_string(childStatus) +
                    ") without reporting this task";
}

}  // namespace
#endif  // !_WIN32

std::vector<TaskOutcome> runTasksInWorkers(const std::vector<std::string>& tasks,
                                           const SessionState& session,
                                           const WorkerBudget& budget,
                                           const std::string& parentHead,
                                           const std::string& scratchDir) {
  std::vector<TaskOutcome> outcomes(tasks.size());
#if defined(_WIN32)
  (void)session; (void)budget; (void)parentHead; (void)scratchDir;
  return {};  // POSIX-only in this arc; caller falls back to the serial leg.
#else
  if (tasks.empty()) return outcomes;
  std::optional<std::string> exe = piet::executablePath();
  if (!exe.has_value() || exe->empty()) return {};
  // The worker entry point (`--parallel-worker`) lives in piet_native's main.
  // Under the R1 split the selftest suite runs as the sibling `piet_selftest`,
  // whose main knows no such flag — so a fan-out from inside the selftest must
  // exec the PRODUCT binary next to it (the mirror of main.cpp's `--selftest`
  // exec into piet_selftest). Surfaced 2026-09-02 when the derived worker count
  // stopped being 1 on auto and the bare `parallel {` legs fanned out for real.
  {
    std::filesystem::path self(*exe);
    if (self.filename() == "piet_selftest") {
      std::filesystem::path sib = self.parent_path() / "piet_native";
      std::error_code sec;
      if (!std::filesystem::exists(sib, sec)) return {};  // serial leg, named in the banner
      exe = sib.string();
    }
  }
  std::error_code ec;
  std::filesystem::create_directories(scratchDir, ec);
  if (ec) return {};

  const int perWorkerThreads = budget.engineThreads;  // see resolveWorkers (§3.5)
  const std::string cobraBin = piet::resolveCobraBin();

  struct Running { int taskIndex; std::string specPath; std::string outPath; };
  std::map<pid_t, Running> running;
  size_t next = 0;
  const size_t slots = budget.n < 1 ? 1 : static_cast<size_t>(budget.n);

  auto launch = [&]() -> bool {
    if (next >= tasks.size()) return false;
    const size_t i = next++;
    const std::string specPath = scratchDir + "/spec_" + std::to_string(i) + ".cbor";
    const std::string outPath = scratchDir + "/out_" + std::to_string(i) + ".cbor";
    registerScratchMint(specPath);  // BUG-389: protocol files die with the block
    registerScratchMint(outPath);
    // One task per spec — the unit the pull queue hands out. buildTaskSpec is
    // reused verbatim so the child sees exactly the schema it already knows,
    // with the task's SOURCE index preserved as the replay key.
    ojson spec = buildTaskSpec({tasks[i]}, session, budget, parentHead);
    spec["tasks"][0]["index"] = static_cast<int>(i);
    spec["workdir"] = scratchDir + "/w_" + std::to_string(i);
    {
      std::ofstream sf(specPath, std::ios::binary);
      if (!sf) {
        outcomes[i].driverError = "could not write task spec";
        return true;  // consumed the task; report it rather than losing it
      }
      writeProtocol(sf, spec);
    }
    pid_t pid = spawnWorker(*exe, specPath, outPath, perWorkerThreads, budget.engineMemoryMb,
                            cobraBin);
    if (pid < 0) {
      outcomes[i].driverError = "fork failed";
      return true;
    }
    running.emplace(pid, Running{static_cast<int>(i), specPath, outPath});
    return true;
  };

  for (size_t s = 0; s < slots; ++s)
    if (!launch()) break;

  while (!running.empty()) {
    int status = 0;
    pid_t done = ::waitpid(-1, &status, 0);
    if (done < 0) break;
    auto it = running.find(done);
    if (it == running.end()) continue;  // not one of ours
    collectWorker(it->second.outPath, it->second.taskIndex, status,
                  outcomes[static_cast<size_t>(it->second.taskIndex)]);
    // Reap the finished task's scratch NOW, not at block exit. §3.3 gave each task
    // its own workdir "so the parent can reap it" — and then nothing ever did, so
    // every completed task's scratch stayed live for the whole block. Measured
    // 2026-08-16 on the 47-task acceptance corpus at workers(3): the run's scratch
    // reached 83.4 GB with each finished worker still holding 3.4-4.2 GB, which
    // exhausted a 24 GiB internal volume at task 23 of 47 and again (after the
    // getcols entrance cut per-task writes by 69%) at task 44. The per-SESSION
    // scratch budget cannot bound this: each worker is its own session, and a
    // session that has EXITED evicts nothing.
    //
    // Safe here and only here: the child is reaped (waitpid returned), and
    // collectWorker has already read its protocol file, so nothing else will ever
    // look at this directory. Best-effort — a failed remove is not a reason to fail
    // the task whose results are already in hand.
    // BUG-481: the task's spec and protocol files go with it. They used to stay
    // until process exit (the exit scratch census read ORPHAN 2 per task).
    {
      std::error_code rmEc;
      std::filesystem::remove_all(scratchDir + "/w_" + std::to_string(it->second.taskIndex),
                                  rmEc);
      std::filesystem::remove(it->second.specPath, rmEc);
      std::filesystem::remove(it->second.outPath, rmEc);
    }
    running.erase(it);
    launch();  // the freed slot pulls the next unassigned task
  }
  // The block's own directory, empty once every task is reaped (a non-empty one
  // -- waitpid failed with workers still out -- is left for the process teardown).
  {
    std::error_code rmEc;
    std::filesystem::remove(scratchDir, rmEc);
  }
  return outcomes;
#endif
}

}  // namespace piet::parallel
