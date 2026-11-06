// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// winsor -- Nicholas J. Cox's winsor.ado (AER-SCOPE1 bundle A, 2026-09-03).
//
//     winsor VAR, gen(NEWVAR) p(#) | h(#) [highonly] [lowonly]
//
// WHY THIS IS A SEPARATE HANDLER AND NOT A ROUTE INTO runWinsor2 (winsor2.cpp).
// The two commands agree on intent and disagree on the number. winsor2 clamps at
// the `_pctile` value, which AVERAGES the two neighbouring observations when N*p
// lands on an integer; winsor.ado replaces the h lowest observations with the
// (h+1)-th ORDER STATISTIC -- an actually observed value, never an interpolation.
// The disagreement is invisible downstream (both produce a plausible winsorized
// column of the right shape), so aliasing one to the other would be a silent
// wrong answer rather than a missing feature. Hence: order statistics only below,
// and no call into the winsor2 machinery.
//
// OPEN QUESTION, deliberately recorded rather than guessed at. Cox derives the
// tail count from p() as floor(p*N) here; the alternative reading of winsor.ado
// is a ROUNDED count, round(p*N). The two differ for exactly the samples where
// p*N is not already an integer, which is most of them, so this is a real fork
// and not a rounding nicety. Resolving it needs `viewsource winsor.ado` in real
// Stata; the answer changes exactly one expression -- the `hFromP` line below --
// and nothing else in this file.
//
// The SAME source check settles a second fork on that same line, so record it
// here rather than let a reader think `hFromP` has only one unknown in it: the
// count multiplied by p() is the NON-MISSING count (the plan's decision, and
// what `n` below is), not _N. The two coincide on a column with no missing
// values -- which is every fixture this bundle freezes -- so no gate in the
// tree can tell them apart today.
#include "piet/handlers.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <fstream>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "io/win_compat.h"  // cecon::to_native_path, cecon::native_exists
#include "piet/cobra_runner.h"
#include "piet/column_fetch.h"
#include "piet/datastore.h"
#include "piet/eval/errors.h"
#include "piet/mutation_result.h"
#include "piet/scratch_seq.h"
#include "piet/session_state.h"

namespace piet::handlers {

using ojson = nlohmann::ordered_json;

namespace {

// BUG-233: the vocabulary is registered so the dispatcher canonicalizes an
// accepted abbreviation BEFORE this handler's own `k == "gen"` comparisons run.
// Without the registration a legal `winsor x, gene(y) p(.01)` passes the reject
// net (a unique >=2-char prefix) and is then dropped unread by the key
// comparisons below -- accepted, ignored, no diagnostic. `g`/`gen`/`generate`
// are all listed because winsor.ado accepts all three spellings, and the net's
// `known` set must enumerate real aliases rather than assume prefix resolution
// will cover them (a 1-char key like `g` is never prefix-resolved).
const eval::OptVocab& kWinsorCoxVocab = eval::registerOptVocab(
    {"winsor"}, {{"g", "gen", "generate", "p", "h", "highonly", "lowonly"}, {}});

ojson wcError(const std::string& message, const std::string& code = "engine_error") {
  return ojson(eval::engineError(message, code));
}

// Shortest round-tripping decimal, for echoing the user's own p()/h() back in a
// rejection message without inventing digits they did not type.
std::string fmtNum(double v) {
  char buf[64];
  for (int prec = 1; prec <= 17; ++prec) {
    std::snprintf(buf, sizeof(buf), "%.*g", prec, v);
    if (std::strtod(buf, nullptr) == v) return std::string(buf);
  }
  std::snprintf(buf, sizeof(buf), "%.17g", v);
  return std::string(buf);
}

// Whole-string numeric parse: a trailing-garbage value such as p(0.01x) must be
// REJECTED, not silently truncated to 0.01 (std::stod's own behavior), because a
// truncated tail fraction is a different winsorization with no diagnostic.
bool parseDouble(const std::string& s, double* out) {
  if (s.empty()) return false;
  const char* begin = s.c_str();
  char* end = nullptr;
  const double v = std::strtod(begin, &end);
  if (end == begin) return false;
  while (*end == ' ' || *end == '\t') ++end;
  if (*end != '\0') return false;
  if (!std::isfinite(v)) return false;
  *out = v;
  return true;
}

// mintScratchPath always names a ".hbin"; the setcol payload is a CSV. Registering
// the minted .hbin name and then writing a .csv beside it would leave the file
// that actually exists unregistered, which is exactly the BUG-389 orphan class the
// mint registry exists to close (sweepOrphanScratch only removes paths it knows).
// So: take the helper's uniqueness, hand back the registration for the name that
// is never created, and register the name that is.
std::string mintCsvPath(const std::string& workdir, const std::string& prefix) {
  const std::string hbin = mintScratchPath(workdir, prefix);
  forgetScratchMint(hbin);
  const std::string csv = hbin.substr(0, hbin.size() - 5) + ".csv";  // 5 == strlen(".hbin")
  registerScratchMint(csv);
  return csv;
}

// The headerless single-column payload `cobra setcol` reads (Frame::read_csv_column
// -- one raw value per row, row count == n_rows). An EMPTY line is how a missing
// value is spelled in that format; writing "nan" or "." instead would land a
// literal token in a numeric column.
bool writeValuesCsv(const std::string& path, const std::vector<double>& vals) {
  std::ofstream f(cecon::to_native_path(path));
  if (!f) return false;
  f.setf(std::ios::fmtflags(0), std::ios::floatfield);
  f.precision(17);
  for (double v : vals) {
    if (!std::isnan(v)) f << v;
    f << "\n";
  }
  return static_cast<bool>(f);
}

// cobra reports a structured failure as {"error":{"code","message"}} on stderr.
// Surfacing the raw stderr blob instead would put JSON punctuation in front of the
// user; surfacing nothing would make a failed setcol indistinguishable from a
// successful one at the call site.
std::string cobraFailure(const CobraRunResult& r) {
  std::string text = r.stderr_text;
  if (!text.empty()) {
    try {
      const nlohmann::json parsed = nlohmann::json::parse(text);
      if (parsed.is_object() && parsed.contains("error") && parsed["error"].is_object() &&
          parsed["error"].contains("message")) {
        text = parsed["error"]["message"].get<std::string>();
      }
    } catch (const std::exception&) {
      // Not structured: the raw text below is the best available account.
    }
  }
  if (text.empty()) text = r.error;
  while (!text.empty() && (text.back() == '\n' || text.back() == '\r' || text.back() == ' ')) {
    text.pop_back();
  }
  if (text.empty()) text = "cobra setcol failed with exit code " + std::to_string(r.exit_code);
  return text;
}

}  // namespace

ojson runWinsorCox(const eval::ParsedCommand& parsed, SessionState& session) {
  // The reject net runs FIRST, before any column is read or any scratch file is
  // minted: a command that is going to be refused must cost nothing.
  if (auto err = eval::rejectUnknownOptions(parsed.opts, kWinsorCoxVocab.known,
                                            kWinsorCoxVocab.unimplemented)) {
    return ojson(*err);
  }

  if (parsed.varlist.empty()) return wcError("winsor: no variable specified");
  if (parsed.varlist.size() > 1) {
    // winsor.ado's syntax is `varname`, singular. Winsorizing only the first of a
    // list and dropping the rest would be a silent partial answer.
    return wcError("winsor: only one variable allowed; winsor takes a single varname");
  }
  const std::string varname = parsed.varlist[0];

  std::string genName;
  bool haveP = false;
  bool haveH = false;
  double pOpt = 0.0;
  double hOpt = 0.0;
  bool highOnly = false;
  bool lowOnly = false;

  for (const auto& kv : parsed.opts) {
    const std::string& k = kv.first;
    const eval::OptValue& v = kv.second;
    if (k == "g" || k == "gen" || k == "generate") {
      if (v.isBool || v.strValue.empty()) {
        return wcError("winsor: option gen() requires a new variable name");
      }
      genName = v.strValue;
    } else if (k == "p") {
      if (v.isBool || !parseDouble(v.strValue, &pOpt)) {
        return wcError("winsor: option p() requires a number");
      }
      haveP = true;
    } else if (k == "h") {
      if (v.isBool || !parseDouble(v.strValue, &hOpt)) {
        return wcError("winsor: option h() requires a number");
      }
      haveH = true;
    } else if (k == "highonly") {
      highOnly = true;
    } else if (k == "lowonly") {
      lowOnly = true;
    }
  }

  // winsor.ado has no `replace` mode at all, so gen() is not merely the usual
  // way to name the output -- it is the only one.
  if (genName.empty()) return wcError("winsor: option gen() required");
  if (haveP && haveH) {
    return wcError("winsor: options p() and h() may not be combined");
  }
  if (!haveP && !haveH) {
    return wcError("winsor: one of p() or h() is required");
  }
  if (highOnly && lowOnly) {
    // Both together clamp NOTHING, i.e. a copy of the input under a name that
    // claims to be winsorized. Refusing is the only non-silent reading.
    return wcError("winsor: options highonly and lowonly may not be combined");
  }
  if (haveP && !(pOpt > 0.0 && pOpt < 0.5)) {
    return wcError("winsor: p(" + fmtNum(pOpt) + ") invalid -- p() must satisfy 0 < p < 0.5");
  }
  if (haveH && (hOpt != std::floor(hOpt))) {
    return wcError("winsor: h(" + fmtNum(hOpt) + ") is not an integer value");
  }

  if (!session.currentHbin().has_value()) return eval::noDataLoaded();

  // Reading ONE column (plus, when it exists, the gen() target) instead of
  // materializing the head. `genName` rides in as an EXPRESSION rather than via
  // alsoInclude on purpose: referencedColumns only reports identifiers the shadow
  // card CONFIRMS, so "the projection came back carrying genName" is precisely the
  // statement "that variable already exists" -- which is the check this handler
  // owes before it writes. alsoInclude would have been added unconditionally and
  // told us nothing. When the card cannot be trusted the projection is nullopt and
  // both questions fall back to the real datastore below, where they are direct.
  const std::string* genExpr = &genName;
  std::string miniPath;
  std::unique_ptr<Datastore> mini;
  if (auto cols = referencedColumns(session, {genExpr}, varname)) {
    mini = fetchColumns(session, *cols, session.workdir(), &miniPath);
  }
  if (mini && !mini->has(varname)) mini.reset();  // subject absent -> not usable
  Datastore* ds = mini ? mini.get() : session.datastore();
  if (ds == nullptr) return eval::noDataLoaded();

  if (!ds->has(varname)) {
    // "variable ... not found" is the wording eval::coarseRc maps to Stata's
    // r(111); rephrasing it would silently downgrade the rc to the generic 1.
    return wcError("winsor: variable " + varname + " not found");
  }
  if (ds->has(genName)) {
    // "already defined" is the wording coarseRc maps to r(110) (confirm new
    // variable). Same reason as above: the phrase is load-bearing.
    return wcError("winsor: variable " + genName + " already defined");
  }
  if (ds->dtype(varname) != "f64") {
    return wcError("winsor: " + varname +
                   " is a string variable; winsor needs a numeric variable");
  }

  const cecon::Column& column = ds->col(varname);
  const double* data = column.f64();
  const int64_t count = column.nbytes / static_cast<int64_t>(sizeof(double));
  if (data == nullptr || count != ds->nRows()) {
    // setcol's payload contract is row-count == n_rows; a short payload would be
    // accepted as a differently-shaped column rather than refused.
    return wcError("winsor: could not read " + varname + " as a full-length numeric column");
  }

  std::vector<double> sorted;
  sorted.reserve(static_cast<size_t>(count));
  for (int64_t i = 0; i < count; ++i) {
    if (!std::isnan(data[i])) sorted.push_back(data[i]);
  }
  if (sorted.empty()) {
    return wcError("winsor: " + varname + " has no non-missing observations");
  }
  std::sort(sorted.begin(), sorted.end());
  const int64_t n = static_cast<int64_t>(sorted.size());

  // Range-check h() BEFORE the narrowing cast: converting an out-of-range double
  // to int64_t is undefined behavior, so a typo like h(1e30) must be refused here
  // rather than by the h*2 >= n test below, which would never see an honest value.
  if (haveH && !(hOpt >= 0.0 && hOpt <= static_cast<double>(n))) {
    return wcError("winsor: h(" + fmtNum(hOpt) + ") is out of range for N = " +
                   std::to_string(n) + " non-missing observations");
  }

  // THE fork recorded at the top of this file: floor, not round.
  const int64_t hFromP = static_cast<int64_t>(std::floor(pOpt * static_cast<double>(n)));
  const int64_t h = haveH ? static_cast<int64_t>(hOpt) : hFromP;
  if (h <= 0) {
    if (haveH) return wcError("winsor: h(" + fmtNum(hOpt) + ") invalid -- h() must be > 0");
    return wcError("winsor: p(" + fmtNum(pOpt) + ") gives a tail count of 0 with N = " +
                   std::to_string(n) + " non-missing observations; raise p() or use h()");
  }
  if (h * 2 >= n) {
    // The two tails must not meet: at h == n/2 every observation is a tail
    // observation and lo would exceed hi, i.e. an empty clamp interval.
    return wcError("winsor: tail count " + std::to_string(h) +
                   " is too large for N = " + std::to_string(n) +
                   " non-missing observations; winsor needs h < N/2");
  }

  // ORDER STATISTICS: the (h+1)-th smallest and the (h+1)-th largest OBSERVED
  // values. No interpolation and no averaging of neighbours -- that averaging is
  // winsor2's percentile rule, and reproducing it here would collapse the very
  // difference this handler exists to preserve.
  const double lo = sorted[static_cast<size_t>(h)];
  const double hi = sorted[static_cast<size_t>(n - h - 1)];

  const bool applyLow = !highOnly;
  const bool applyHigh = !lowOnly;

  std::vector<double> out;
  out.reserve(static_cast<size_t>(count));
  for (int64_t i = 0; i < count; ++i) {
    const double v = data[i];
    if (std::isnan(v)) {
      out.push_back(v);  // missing stays missing; it was never in the sort either
      continue;
    }
    double clamped = v;
    if (applyLow && clamped < lo) clamped = lo;
    if (applyHigh && clamped > hi) clamped = hi;
    out.push_back(clamped);
  }

  const std::string csvPath = mintCsvPath(session.workdir(), "winsor_cox_vals");
  if (!writeValuesCsv(csvPath, out)) {
    return wcError("winsor: could not write the winsorized values");
  }

  // The new column is patched into the REAL head, not into the projection above:
  // the projection carries only the columns this handler fetched, so writing to it
  // would silently drop every other variable in the dataset.
  const std::string outPath = mintScratchPath(session.workdir(), "winsor_cox");
  const CobraRunResult r =
      runCobra({"setcol", "--data", *session.currentHbin(), "--var", genName, "--values", csvPath,
                "--dtype", "f64", "--new", "--out", outPath});
  if (!r.ok) return wcError("winsor: " + cobraFailure(r));
  // BUG-124's torn-write guard, carried from the two other stateless setcol
  // sites (strmutate.cpp, handlers_data_verbs2.cpp's destring) because the
  // `r.ok` check above does not cover the case it exists for: the engine's
  // writer creates `--out` before it can detect a write failure, and a full
  // scratch volume has twice reached PIET as a ZERO exit code. `session.load`
  // on a path that is not there does not fail -- it records the head and hands
  // the next Datastore open a missing or truncated mapping, which is a crash
  // rather than a diagnostic. This handler never opens a ResidentEligible
  // scope, so it is always on the stateless arm and `--out` is always a real
  // file here; the probe cannot false-positive the way it would on the
  // resident arm, where the router strips `--out` and nothing is written.
  if (!cecon::native_exists(outPath)) {
    return wcError("winsor: failed writing output dataset (disk full or I/O error)");
  }

  session.load(outPath);
  // "gen" and not "winsor": the mutation this performed IS a pure add of one new
  // column, which is the branch seedNMissForPureAdd must take for the untouched
  // columns to carry their missing-counts forward.
  return mutationResult(session, "gen", {genName});
}

}  // namespace piet::handlers
