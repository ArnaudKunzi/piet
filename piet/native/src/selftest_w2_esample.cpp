// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// e(sample) design, selftest TU (internal note esample/TASKS.md).
// Section T0: `estimates restore NAME` makes a stored model active again.
// Section T2 (testW2EsampleT2, below): the mask itself -- the hidden column a
// wired fit installs, the row-layer rewrite, the macro layer's observation-1
// value, the named r(198) for a model with no mask, and the hiding rule.
// Section T3/T4 (testW2EsampleT3): `_est_<name>`, estimates restore/use/
// esample:/drop/clear and eststo carrying the mask, logit/probit/poisson, and
// the by-prefix `if` row rewrite (w2_esample.log section G, H1/H2/H7, I1).
//
// Oracles (StataBE, frozen logs):
//   tests/eval/golden/aer_w2/w2_cmdtriage.log section C (lines 369-390) --
//     `estimates restore m1` rc 0 "(results m1 are active now)", then e(df_m)
//     and _b[a] are m1's; bare `est restore` r(111) "estimates __000000 not
//     found"; `estimates restore nosuch` r(111) "estimation result nosuch not
//     found".
//   tests/eval/golden/aer_w2/w2_esample.log G2 (restore A after a second fit:
//     rc 0, e(N) 28, _b[x1] 2.081516065514025) and G12 (C then D restored in
//     turn; the log records only their e(sample) COUNTS, 28 and 31 -- the mask
//     is T3, so this TU pins e(N), which equals those counts, and leaves the
//     count half of G12 to T3).
//
// The restore itself landed with W2 esttabnames (BUG-553; its arms live in
// selftest_w2_esttabnames.cpp), so the w2_cmdtriage/G2 arms here are
// regression pins that do not fail on the base binary. The arms that DO fail
// on it are BUG-636: after `estimates use` of a .ster saved by an earlier
// session -- and after storing and restoring those results -- test/testparm/
// lincom/nlcom answered `[data_io] cannot open .../piet_native_XXXX/...hbin`
// at rc 1 instead of refusing by name as predict already did.
//
// The r(198) refusal is a PIET-specific NAMED REFUSAL BY DESIGN, not an oracle
// claim: Stata keeps a model's data in memory, so no StataBE arm exists for a
// reclaimed working frame. The oracle-backed half of BUG-636 is the controls:
// every verb class still answers rc 0 wherever the fit data is reachable.
#include "selftest_w2.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <optional>
#include <string>
#include <system_error>
#include <vector>

#include "piet/cobra_runner.h"
#include "piet/datastore.h"
#include "piet/dispatcher.h"
#include "piet/doexec.h"
#include "piet/eval/errors.h"
#include "piet/session_state.h"
#include "vendor/nlohmann/json.hpp"

namespace {

using ojson = nlohmann::ordered_json;
namespace fs = std::filesystem;

// w2_cmdtriage.do's frame (the columns section C reads), verbatim.
const char* const kTriageFrame[] = {
    "clear",
    "set obs 12",
    "generate double a = _n*1.5",
    "generate double b = mod(_n*5, 7)",
    "generate double y = 1 + 0.5*a - 0.25*b + mod(_n, 4)",
};

// w2_esample.do's frame (lines 11-28), verbatim minus `version`/`clear all`/
// `set more off`/`xtset`, which section G's regress arms do not read.
const char* const kEsampleFrame[] = {
    "clear",
    "set obs 61",
    "generate int id = ceil(_n/6)",
    "replace id = 11 in 61",
    "generate int t = mod(_n-1,6)+1",
    "replace t = 1 in 61",
    "generate double x1 = mod(_n*7,11) + mod(_n,5)/10",
    "generate double x2 = mod(_n*3,5) + mod(_n*13,7)/10",
    "generate double z1 = mod(_n*5,13) + x1/2",
    "generate double w  = 1 + mod(_n,4)",
    "generate int cl = 1 + mod(_n,7)",
    "generate double y  = 1 + 2*x1 - x2 + id/3 + mod(_n*11,17)/5",
    "generate byte d = (y > 12)",
    "replace y  = . in 7",
    "replace x1 = . in 13",
    "replace cl = . in 20",
    "replace w  = 0 in 26",
    "replace w  = . in 31",
};

struct LineResult {
  std::string rc = "0";
  std::string msg;
  std::string text;  // every display block's text, newline-joined
  std::string dump;  // the whole block stream minus echoes, for byte comparisons
};

// Every JSON read is guarded: a wrong shape is a FAIL with a name, never an
// abort (type_error.306 took the whole selftest binary down once already).
LineResult runLine(const std::string& cmd, piet::SessionState& session) {
  LineResult r;
  try {
    const ojson blocks = piet::dispatch(cmd, session);
    if (!blocks.is_array()) {
      r.rc = "shape";
      r.msg = "the block stream is not an array";
      return r;
    }
    for (const auto& b : blocks) {
      if (!b.is_object()) {
        r.rc = "shape";
        r.msg = "a non-object reached the block stream (array pushed as one block?)";
        return r;
      }
      const std::string t = b.value("type", std::string());
      if (t != "echo") r.dump += b.dump() + "\n";
      if (t == "display" && b.contains("text") && b["text"].is_string()) {
        if (!r.text.empty()) r.text += "\n";
        r.text += b["text"].get<std::string>();
      }
      if (t != "error") continue;
      r.rc = piet::eval::coarseRc(b);
      r.msg = b.value("message", std::string());
    }
  } catch (const std::exception& e) {
    r.rc = "threw";
    r.msg = e.what();
  }
  return r;
}

// A number read back through a scalar: `display %18.0g _b[x]` is not usable
// for the value half of an arm (BUG-554, OPEN: resolveBRef substitutes _b[]
// as 9-significant-digit text before the format applies).
std::optional<double> readNumber(const std::string& expr, piet::SessionState& session) {
  (void)runLine("scalar __w2es_v = " + expr, session);
  LineResult r = runLine("display %18.0g __w2es_v", session);
  try {
    std::string t = r.text;
    while (!t.empty() && t.front() == ' ') t.erase(t.begin());
    size_t pos = 0;
    double v = std::stod(t, &pos);
    if (pos == 0) return std::nullopt;
    return v;
  } catch (const std::exception&) {
    return std::nullopt;
  }
}

bool near(std::optional<double> got, double want) {
  return got.has_value() && std::fabs(*got - want) <= 1e-12 * std::max(1.0, std::fabs(want));
}

std::string num(std::optional<double> v) {
  return v.has_value() ? std::to_string(*v) : std::string("<none>");
}


// ---- T2 helpers --------------------------------------------------------------
// `count if COND`: the line's rc, and r(N) when it ran.
struct CountRes {
  std::string rc;
  std::string msg;
  std::optional<double> n;
};
CountRes countIf(const std::string& cond, piet::SessionState& session) {
  LineResult r = runLine("count if " + cond, session);
  CountRes c;
  c.rc = r.rc;
  c.msg = r.msg;
  if (r.rc == "0") c.n = readNumber("r(N)", session);
  return c;
}

std::string show(const CountRes& c) {
  return "rc " + c.rc + " n " + num(c.n) + (c.msg.empty() ? "" : " `" + c.msg + "`");
}

// The `if e(sample) {` block of w2_esample.do B19/B29c, verbatim, through the
// do-file executor (a block is not one dispatch line). Returns "rc|inif".
std::string cmdIfBlock(piet::SessionState& session) {
  (void)piet::doexec::executeDoSource(
      "local inif = 0\n"
      "capture noisily {\n"
      "    if e(sample) {\n"
      "        local inif = 1\n"
      "    }\n"
      "}\n"
      "local rc = _rc\n",
      session, piet::dispatch);
  return session.eval.locals.get("rc", "<unset>") + "|" +
         session.eval.locals.get("inif", "<unset>");
}

// Every variable name a `describe`/`ds`/data_mutation can show, joined.
std::string dsVarlist(piet::SessionState& session) {
  (void)runLine("ds", session);
  return runLine("display \"`r(varlist)'\"", session).text;
}
}  // namespace


// ============================================================================
// T2 -- the mask in PIET (internal note esample/TASKS.md T2,
// SPEC.md §4-§5). Oracle: tests/eval/golden/aer_w2/w2_esample.log (StataBE
// 19.5), cited by line and PREC arm. Main (before T2) refused every e(sample)
// read in the row and macro planes with r(198) and printed `.` at rc 0 for
// `display e(sample)` (measured on the frozen main binary, 2026-10-13: A1-A5,
// B1-B29c all `198`, A2/B16 `.`), so each arm below fails there.
// ============================================================================
// ---- r150 scratch-GC race: the install head owns its on-disk base frame ------
// A fit that installs the e(sample) mask moves the head to `esample_install_*.hbin`
// and keeps the frame it read on disk as the BASE (esampleLastInstallBase): the
// next fit's `if` filter, the model's own frame and the read-only replays alias
// it for as long as the head is still that install. The base was owned only by
// the LastModel pin, so `ereturn clear` (which resets the model) let the reclaim
// primitive unlink it on a detached thread while the next fit still named it:
// `[data_io] cannot open .../gen_*.hbin`, rc 1, in 13/30 fits of a
// `regress ... if ..., vce(...)` loop on the unfixed build (0/30 with
// PIET_NO_SCRATCH_GC=1). Seeded-RED = drop the isLiveEsampleBase exclusion in
// SessionState::reclaimSupersededScratch (leg 1 and 2) or the "esample_base" claim
// in SessionState::scratchOwners (leg 1, 3 and 4).
static void setScratchBudgetMb(const char* mb) {
#ifndef _WIN32
  if (mb) setenv("PIET_SCRATCH_BUDGET_MB", mb, 1); else unsetenv("PIET_SCRATCH_BUDGET_MB");
#else
  _putenv_s("PIET_SCRATCH_BUDGET_MB", mb ? mb : "");
#endif
}

static long long censusClassN(piet::SessionState& s, const char* cls) {
  try {
    const auto j = nlohmann::json::parse(s.scratchCensusJson());
    if (j.contains("by_class") && j["by_class"].contains(cls)) return j["by_class"][cls].value("n", 0LL);
  } catch (const std::exception&) {
    return -1;
  }
  return 0;
}

static void testW2EsampleBaseOwnership(const fs::path& root) {
  std::printf("[selftest] --- W2 esample T2: the install head owns its base frame "
              "(scratch-GC race, r150) ---\n");
  std::error_code ec;
  // Retain superseded scratch (budget 4096 MB) so the legs read OWNERSHIP from the
  // census instead of racing the detached unlink thread; leg 5 runs the shipped
  // eager default.
  setScratchBudgetMb("4096");
  {
    const std::string wd = (root / "t2_base_owner").u8string();
    fs::create_directories(wd, ec);
    piet::SessionState session(wd);
    for (const char* s0 : kEsampleFrame) (void)runLine(s0, session);
    const LineResult f1 = runLine("quietly regress y x1 x2 if id <= 5, vce(cluster cl)", session);
    const std::string base = session.esampleLastInstallBase;
    const bool installed = session.esampleHeadIsInstallOf(base);
    const bool baseOnDisk = !base.empty() && fs::exists(fs::path(base), ec);
    w2check(f1.rc == "0" && installed && baseOnDisk,
            "scratch-gc esample base setup: a filtered fit installs the mask, the head is its "
            "install frame and the base frame exists on disk. Got rc " + f1.rc + " `" + f1.msg +
                "`, installed " + std::to_string(installed) + ", base `" + base + "`");
    // Leg 1: the model pin goes away, the head is still the install.
    const LineResult clr = runLine("ereturn clear", session);
    w2check(clr.rc == "0" && session.esampleHeadIsInstallOf(base) &&
                censusClassN(session, "esample_base") == 1,
            "scratch-gc esample base leg 1: after `ereturn clear` (the LastModel pin released) "
            "the head is still the install of the base and the owner table names the base "
            "`esample_base` (n 1), so the orphan sweep and the census cannot lose it. Got census "
            "esample_base n " + std::to_string(censusClassN(session, "esample_base")));
    // Leg 2: the reclaim primitive itself spares it.
    const long long pendingBefore = censusClassN(session, "superseded_pending");
    session.reclaimScratchFile(base);
    w2check(censusClassN(session, "superseded_pending") == pendingBefore &&
                fs::exists(fs::path(base), ec),
            "scratch-gc esample base leg 2: reclaimSupersededScratch(base) while the head is the "
            "install registers nothing for unlink (superseded_pending n unchanged at " +
                std::to_string(pendingBefore) + ") and the file stays");
    // Leg 3: and the aliasing reader still works on it.
    const LineResult f2 = runLine("quietly regress y x1 x2 if id <= 5, vce(cluster cl)", session);
    w2check(f2.rc == "0" && fs::exists(fs::path(base), ec),
            "scratch-gc esample base leg 3: the next filtered fit after `ereturn clear` runs rc 0 "
            "off the aliased base. Got rc " + f2.rc + " `" + f2.msg + "`");
    // Leg 4: the claim is exactly as long as the install head. Once the head moves
    // nothing owns the base and the next dispatch exit's sweep takes it (no leak).
    (void)runLine("ereturn clear", session);
    (void)runLine("generate double zz = 1", session);
    const bool released = !session.esampleHeadIsInstallOf(base);
    w2check(released && censusClassN(session, "esample_base") == 0 && !fs::exists(fs::path(base), ec),
            "scratch-gc esample base leg 4: after the head moves (`generate`) the base is no "
            "longer claimed and the orphan sweep has removed it -- the pin does not leak. Got "
            "released " + std::to_string(released) + ", esample_base n " +
                std::to_string(censusClassN(session, "esample_base")) + ", base exists " +
                std::to_string(fs::exists(fs::path(base), ec)));
  }
  setScratchBudgetMb(nullptr);
  // Leg 5: the shipped eager default, the stress shape. Unfixed: every iteration
  // after the first fails with the unlink's errno 2 (30/30 runs measured).
  {
    const std::string wd = (root / "t2_base_loop").u8string();
    fs::create_directories(wd, ec);
    piet::SessionState session(wd);
    for (const char* s0 : kEsampleFrame) (void)runLine(s0, session);
    int bad = 0;
    std::string firstBad;
    for (int i = 0; i < 12; ++i) {
      (void)runLine("ereturn clear", session);
      const LineResult f = runLine("quietly regress y x1 x2 if id <= 5, vce(cluster cl)", session);
      if (f.rc != "0") {
        if (bad == 0) firstBad = f.rc + " `" + f.msg + "`";
        ++bad;
      }
    }
    w2check(bad == 0,
            "scratch-gc esample base leg 5: 12 x (`ereturn clear`; filtered `regress ..., "
            "vce(cluster)`) under the eager scratch default all run rc 0 (the unlinked-base "
            "failure was rc 1 `[data_io] cannot open .../gen_*.hbin`). Failures " +
                std::to_string(bad) + ", first " + firstBad);
  }
}

static void testW2EsampleT2(const fs::path& root) {
  std::printf("[selftest] --- W2 esample T2: hidden mask column, row rewrite, obs-1 hook, "
              "named r(198) (w2_esample.log A-F, H9) ---\n");
  std::error_code ec;

  // ---- A1-A5: no estimation results in memory --------------------------------
  {
    const std::string wd = (root / "t2_none").u8string();
    fs::create_directories(wd, ec);
    piet::SessionState session(wd);
    for (const char* s0 : kEsampleFrame) (void)runLine(s0, session);
    (void)runLine("xtset id t", session);
    const CountRes a1 = countIf("e(sample)", session);
    const LineResult a2 = runLine("display e(sample)", session);
    const LineResult a3 = runLine("local s = e(sample)", session);
    const std::string s3 = session.eval.locals.get("s", "<unset>");
    const LineResult a4 = runLine("scalar sA = e(sample)", session);
    const auto v4 = readNumber("sA", session);
    const LineResult a5 = runLine("generate byte es0 = e(sample)", session);
    (void)runLine("quietly summarize es0", session);
    const auto n5 = readNumber("r(N)", session);
    const auto sum5 = readNumber("r(sum)", session);
    w2check(a1.rc == "0" && near(a1.n, 0.0) && a2.rc == "0" && a2.text == "0" && a3.rc == "0" &&
                s3 == "0" && a4.rc == "0" && near(v4, 0.0) && a5.rc == "0" && near(n5, 61.0) &&
                near(sum5, 0.0),
            "W2 esample T2 A1-A5: with no estimation results e(sample) is 0 in every layer at rc "
            "0 -- `count if e(sample)` 0, `display e(sample)` 0, `local s = e(sample)` 0, "
            "`scalar sA = e(sample)` 0, `generate byte es0 = e(sample)` 61 obs summing to 0 "
            "(w2_esample.log:95 A1 |0|0|, :100 A2 |0|, :109 A3 |0|0|, :118 A4 |0|0|, :129 A5 "
            "|0|61|0|). Main: 198/`.`/198/198/198. Got A1 " + show(a1) + ", A2 rc " + a2.rc +
                " `" + a2.text + a2.msg + "`, A3 rc " + a3.rc + " s=" + s3 + ", A4 rc " + a4.rc +
                " " + num(v4) + ", A5 rc " + a5.rc + " N " + num(n5) + " sum " + num(sum5));
  }
  // FU-429 / the regression input: the oracle's own preamble, `xtset` then a
  // captured `count if e(sample)` and the r(N) read the scoreboard stopped on.
  {
    const std::string wd = (root / "t2_fu429").u8string();
    fs::create_directories(wd, ec);
    piet::SessionState session(wd);
    for (const char* s0 : {"clear", "set obs 12", "generate id = 1 + (_n > 5)", "generate t = _n",
                           "xtset id t", "capture noisily count if e(sample)", "local rc = _rc",
                           "local n = cond(`rc', ., r(N))"})
      (void)runLine(s0, session);
    const LineResult d = runLine("display \"`rc'|`n'|\"", session);
    w2check(d.rc == "0" && d.text == "0|0|",
            "W2 esample T2 FU-429: after `xtset id t`, a captured `count if e(sample)` with no "
            "model is rc 0 with r(N) 0, so `cond(`rc', ., r(N))` reads 0 (w2_esample.log:95 A1 "
            "|0|0|). Main: r(198), then r(111) on the r(N) read. Got rc " + d.rc + " `" + d.text +
                d.msg + "`");
  }

  // ---- B: regress y x1 x2 if id<=5 (e(N) 28), the row-plane spellings ------
  {
    const std::string wd = (root / "t2_base").u8string();
    fs::create_directories(wd, ec);
    piet::SessionState session(wd);
    for (const char* s0 : kEsampleFrame) (void)runLine(s0, session);
    (void)runLine("xtset id t", session);
    const std::string varsBefore = dsVarlist(session);
    (void)runLine("datasignature", session);
    const std::string sigBefore = runLine("display \"`r(datasignature)'\"", session).text;
    const LineResult b0 = runLine("regress y x1 x2 if id<=5", session);
    const auto n0 = readNumber("e(N)", session);
    const CountRes b1 = countIf("e(sample)", session);
    const CountRes b2 = countIf("!e(sample)", session);
    const CountRes b3 = countIf("e(sample)==1", session);
    const CountRes b4 = countIf("e(sample)!=1", session);
    const CountRes b5 = countIf("e(sample) & t==1", session);
    const CountRes b6 = countIf("e(sample) | id==6", session);
    w2check(b0.rc == "0" && near(n0, 28.0) && b1.rc == "0" && near(b1.n, 28.0) && near(b2.n, 33.0) &&
                near(b3.n, 28.0) && near(b4.n, 33.0) && near(b5.n, 3.0) && near(b6.n, 34.0),
            "W2 esample T2 B0-B6: after `regress y x1 x2 if id<=5` (e(N) 28) `count if` "
            "e(sample) 28, !e(sample) 33, e(sample)==1 28, e(sample)!=1 33, e(sample) & t==1 3, "
            "e(sample) | id==6 34 (w2_esample.log:156-228). Main: r(198) each. Got e(N) " +
                num(n0) + "; " + show(b1) + "; " + show(b2) + "; " + show(b3) + "; " + show(b4) +
                "; " + show(b5) + "; " + show(b6));

    // The hiding rule (SPEC §4.3), checked while the mask answers: the column
    // is on the rows but in no user-facing listing, and datasignature does not
    // see it. Sibling of the two selftest.cpp leak guards (BUG-370(b)
    // `__piet_tsif`, INSPECT-2608 `__piet_filtermask`).
    const std::string varsAfter = dsVarlist(session);
    (void)runLine("datasignature", session);
    const std::string sigAfter = runLine("display \"`r(datasignature)'\"", session).text;
    bool describeLeak = true;
    {
      const ojson blocks = piet::dispatch("describe", session);
      for (const auto& b : blocks) {
        if (!b.is_object() || b.value("type", std::string()) != "describe") continue;
        describeLeak = false;
        if (b.contains("variables") && b["variables"].is_array())
          for (const auto& v : b["variables"])
            if (v.is_object() && v.value("name", std::string()).rfind("__piet_", 0) == 0)
              describeLeak = true;
      }
    }
    bool mutationLeak = true;
    {
      const ojson blocks = piet::dispatch("generate double gx = 1", session);
      for (const auto& b : blocks) {
        if (!b.is_object() || b.value("type", std::string()) != "data_mutation") continue;
        mutationLeak = false;
        if (b.contains("varlist_after") && b["varlist_after"].is_array())
          for (const auto& v : b["varlist_after"])
            if (v.is_object() && v.value("name", std::string()).rfind("__piet_", 0) == 0)
              mutationLeak = true;
      }
    }
    (void)runLine("drop gx", session);
    const CountRes still = countIf("e(sample)", session);
    w2check(near(b1.n, 28.0) && near(still.n, 28.0) && varsAfter == varsBefore &&
                varsAfter.find("__piet") == std::string::npos && !describeLeak && !mutationLeak &&
                !sigBefore.empty() && sigAfter == sigBefore,
            "W2 esample T2 hiding rule: with the mask live (count 28), `ds` lists the same "
            "variables as before the fit, `describe` and a data_mutation's varlist_after carry no "
            "`__piet_` name, and `datasignature` is unchanged by the fit (Stata's e(sample) "
            "marker is not a variable: w2_esample.log:1393 G0e_ds lists only user variables). "
            "Got count " + show(still) + ", ds before `" + varsBefore + "` after `" + varsAfter +
                "`, describe leak " + std::to_string(describeLeak) + ", mutation leak " +
                std::to_string(mutationLeak) + ", signature `" + sigBefore + "` -> `" + sigAfter +
                "`");

    const LineResult b7 = runLine("generate double zb7 = 1 if e(sample)", session);
    const CountRes c7 = countIf("zb7<.", session);
    const LineResult b8 = runLine("generate byte zb8 = e(sample)", session);
    (void)runLine("quietly summarize zb8", session);
    const auto n8 = readNumber("r(N)", session);
    const auto s8 = readNumber("r(sum)", session);
    const LineResult b9 = runLine("egen double zb9 = total(x1) if e(sample), by(id)", session);
    const CountRes c9 = countIf("zb9<.", session);
    const LineResult b10 = runLine("replace zb7 = 2 if e(sample)", session);
    const CountRes c10 = countIf("zb7==2", session);
    const LineResult b11 = runLine("summarize y if e(sample)", session);
    const auto n11 = readNumber("r(N)", session);
    const auto m11 = readNumber("r(mean)", session);
    w2check(b7.rc == "0" && near(c7.n, 28.0) && b8.rc == "0" && near(n8, 61.0) && near(s8, 28.0) &&
                b9.rc == "0" && near(c9.n, 28.0) && b10.rc == "0" && near(c10.n, 28.0) &&
                b11.rc == "0" && near(n11, 28.0) &&
                m11.has_value() && std::fabs(*m11 - 12.1547619047619) < 1e-9,
            "W2 esample T2 B7-B11: gen-if 28 rows, `generate byte zb8 = e(sample)` 61 obs summing "
            "to 28, `egen ... if e(sample), by(id)` 28, `replace ... if e(sample)` 28, "
            "`summarize y if e(sample)` N 28 mean 12.1547619047619 (w2_esample.log:240-288). "
            "Main: r(198) each. Got " + b7.rc + "/" + num(c7.n) + ", " + b8.rc + " " + num(n8) +
                "/" + num(s8) + ", " + b9.rc + "/" + num(c9.n) + ", " + b10.rc + "/" +
                num(c10.n) + ", " + b11.rc + " " + num(n11) + " " + num(m11));

    const LineResult b12 = runLine("regress y x1 if e(sample)", session);
    const auto n12 = readNumber("e(N)", session);
    const auto bx12 = readNumber("_b[x1]", session);
    const CountRes b12b = countIf("e(sample)", session);
    w2check(b12.rc == "0" && near(n12, 28.0) && bx12.has_value() &&
                std::fabs(*bx12 - 2.042795576123796) < 1e-9 && near(b12b.n, 28.0),
            "W2 esample T2 B12/B12b: `regress y x1 if e(sample)` fits the 28 masked rows "
            "(_b[x1] 2.042795576123796) and the mask is then the NEW fit's, 28 (w2_esample.log:311, "
            ":323). Main: r(198). Got rc " + b12.rc + " `" + b12.msg + "` e(N) " + num(n12) +
                " _b[x1] " + num(bx12) + ", count " + show(b12b));

    (void)runLine("quietly regress y x1 x2 if id<=5", session);
    const LineResult b13 = runLine("predict double pb13 if e(sample), xb", session);
    const CountRes c13 = countIf("pb13<.", session);
    const LineResult b14 = runLine("predict double pb14, xb", session);
    const CountRes c14 = countIf("pb14<.", session);
    const LineResult b15 = runLine("predict double pb15 if e(sample), residuals", session);
    const CountRes c15 = countIf("pb15<.", session);
    w2check(b13.rc == "0" && near(c13.n, 28.0) && b14.rc == "0" && near(c14.n, 60.0) &&
                b15.rc == "0" && near(c15.n, 28.0),
            "W2 esample T2 B13-B15 (FU-46): `predict ... if e(sample), xb` fills the 28 sample "
            "rows, the unmasked control 60, `predict ... if e(sample), residuals` 28 "
            "(w2_esample.log:337, :349, :361; esttab-beta.log:488 is the served shape). Main: "
            "r(198) (w2n_followups PRECFAIL|B04|rc=198). Got " + b13.rc + " `" + b13.msg + "` " +
                num(c13.n) + ", " + b14.rc + " " + num(c14.n) + ", " + b15.rc + " `" + b15.msg +
                "` " + num(c15.n));

    // B16-B19: the scalar plane reads observation 1 (row 1 is in the sample).
    const LineResult b16 = runLine("display e(sample)", session);
    const LineResult b17 = runLine("local s = e(sample)", session);
    const std::string s17 = session.eval.locals.get("s", "<unset>");
    const LineResult b18 = runLine("scalar sB = e(sample)", session);
    const auto v18 = readNumber("sB", session);
    const std::string b19 = cmdIfBlock(session);
    w2check(b16.rc == "0" && b16.text == "1" && b17.rc == "0" && s17 == "1" && b18.rc == "0" &&
                near(v18, 1.0) && b19 == "0|1",
            "W2 esample T2 B16-B19: in a scalar context e(sample) is observation 1's value, 1 -- "
            "`display`, `local s =`, `scalar sB =`, and `if e(sample) {` takes the branch "
            "(w2_esample.log:365 B16 |1|, :376 B17 |0|1|, :385 B18 |0|1|, :400 B19 |0|1|). Main: "
            "`.` at rc 0, 198, 198, rc 198. Got display rc " + b16.rc + " `" + b16.text +
                b16.msg + "`, local rc " + b17.rc + " " + s17 + ", scalar rc " + b18.rc + " " +
                num(v18) + ", if-block rc|inif " + b19);

    // B20-B21b: macro-delivered spellings and a string literal.
    (void)runLine("global gB20 \"generate byte zb20 = e(sample)\"", session);
    const LineResult b20 = runLine("$gB20", session);
    (void)runLine("quietly summarize zb20", session);
    const auto s20 = readNumber("r(sum)", session);
    (void)runLine("local condB21 \"e(sample) & t==1\"", session);
    const CountRes b21 = countIf("`condB21'", session);
    const LineResult b21b = runLine("display \"e(sample)\"", session);
    w2check(b20.rc == "0" && near(s20, 28.0) && near(b21.n, 3.0) && b21b.text == "e(sample)",
            "W2 esample T2 B20-B21b: `$gB20` (a global holding `generate byte zb20 = e(sample)`) "
            "sums to 28, `count if `condB21'` 3, and a string literal \"e(sample)\" prints as text "
            "(w2_esample.log:414 |0|61|28|, :428 |0|3|, :433 |e(sample)|). Main: 198/198. Got " +
                b20.rc + " " + num(s20) + ", " + show(b21) + ", `" + b21b.text + "`");

    // B22-B25b, B28: keep/drop (under preserve), assert, tabulate, list, in.
    (void)runLine("preserve", session);
    const LineResult b22 = runLine("keep if e(sample)", session);
    const auto n22 = readNumber("_N", session);
    (void)runLine("restore", session);
    (void)runLine("preserve", session);
    const LineResult b23 = runLine("drop if e(sample)", session);
    const auto n23 = readNumber("_N", session);
    (void)runLine("restore", session);
    const LineResult b24 = runLine("assert e(sample) | id>5", session);
    const LineResult b24b = runLine("assert e(sample)", session);
    const LineResult b25 = runLine("tabulate t if e(sample)", session);
    const auto n25 = readNumber("r(N)", session);
    const auto r25 = readNumber("r(r)", session);
    const LineResult b25b = runLine("list id t if e(sample) & t==6, noobs", session);
    const CountRes b28 = countIf("e(sample) in 1/10", session);
    w2check(b22.rc == "0" && near(n22, 28.0) && b23.rc == "0" && near(n23, 33.0) &&
                b24.rc == "9" && b24b.rc == "9" && b25.rc == "0" && near(n25, 28.0) &&
                near(r25, 6.0) && b25b.rc == "0" && near(b28.n, 9.0),
            "W2 esample T2 B22-B28: `keep if e(sample)` leaves 28, `drop if e(sample)` 33, "
            "`assert e(sample) | id>5` and `assert e(sample)` are r(9) (rows 7 and 13 are out), "
            "`tabulate t if e(sample)` N 28 r 6, `list ... if e(sample) & t==6` rc 0, `count if "
            "e(sample) in 1/10` 9 (w2_esample.log:446-535). Main: r(198) each. Got " + b22.rc +
                " " + num(n22) + ", " + b23.rc + " " + num(n23) + ", assert " + b24.rc + "/" +
                b24b.rc + ", tab " + b25.rc + " " + num(n25) + " " + num(r25) + ", list " +
                b25b.rc + ", " + show(b28));

    // B29-B29c: observation 1 OUT of the sample.
    (void)runLine("sort id t", session);
    const LineResult b29 = runLine("regress y x1 x2 if id>=2", session);
    const auto n29 = readNumber("e(N)", session);
    const LineResult d29 = runLine("display e(sample)", session);
    (void)runLine("local s = e(sample)", session);
    const std::string s29b = session.eval.locals.get("s", "<unset>");
    const std::string b29c = cmdIfBlock(session);
    w2check(b29.rc == "0" && near(n29, 53.0) && d29.text == "0" && s29b == "0" && b29c == "0|0",
            "W2 esample T2 B29-B29c: after `regress ... if id>=2` (e(N) 53) observation 1 is out, "
            "so `display e(sample)` 0, `local s = e(sample)` 0, and `if e(sample) {` skips "
            "(w2_esample.log:587 |0|53|0|, :596 |0|0|, :611 |0|0|). Main: `.`/198/198. Got e(N) " +
                num(n29) + ", display `" + d29.text + d29.msg + "`, local " + s29b +
                ", if-block " + b29c);
  }

  // ---- C, D: admission rules the engine owns (count == e(N)) -----------------
  {
    const std::string wd = (root / "t2_rules").u8string();
    fs::create_directories(wd, ec);
    piet::SessionState session(wd);
    for (const char* s0 : kEsampleFrame) (void)runLine(s0, session);
    (void)runLine("xtset id t", session);
    struct Arm {
      const char* fit;
      double want;
      const char* cite;
    };
    const Arm arms[] = {
        {"regress y x1 x2", 59, "C1 :638 |0|59|59|"},
        {"regress y x1 x2, cluster(cl)", 58, "C2 :665 |0|58|58|7|"},
        {"regress y x1 x2 [pw=w]", 57, "C3 :692 |0|57|57|"},
        {"regress y x1 x2 [aw=w]", 57, "C4 :719 |0|57|57|"},
        {"regress y L.x1 x2", 49, "C5 :747 |0|49|49|"},
        {"reghdfe y x1 x2, absorb(id)", 58, "D1 :819 |0|58|58|1|"},
        {"reghdfe y x1 x2, absorb(id) keepsingletons", 59, "D2 :862 |0|59|59|"},
        {"reghdfe y x1 x2, absorb(id) cluster(cl)", 57, "D5 :963 |0|57|57|7|"},
        {"regress y x1 x2 if id<=5 in 1/24", 22, "H9 :1983 |0|22|22|"},
    };
    for (const Arm& a : arms) {
      const LineResult f = runLine(a.fit, session);
      const auto n = readNumber("e(N)", session);
      const CountRes c = countIf("e(sample)", session);
      w2check(f.rc == "0" && near(n, a.want) && c.rc == "0" && near(c.n, a.want),
              std::string("W2 esample T2 ") + a.cite + ": after `" + a.fit + "` `count if "
              "e(sample)` equals e(N) = " + num(a.want) + " (the engine's admitted rows: listwise, "
              "cluster, weights, TS loss, singletons; w2_esample.log). Main: r(198). Got fit rc " +
                  f.rc + " `" + f.msg + "` e(N) " + num(n) + ", count " + show(c));
    }
    // D1b: the reghdfe singleton (id 11) is out of the mask.
    (void)runLine("reghdfe y x1 x2, absorb(id)", session);
    const CountRes d1b = countIf("e(sample) & id==11", session);
    // D3/D4/H8: areg and xtreg,fe keep the singleton id 11 (Stata has no
    // singleton rule there); the engine's reghdfe default dropped it.
    const LineResult d3 = runLine("xtreg y x1 x2, fe", session);
    const auto n3 = readNumber("e(N)", session);
    const auto g3 = readNumber("e(N_g)", session);
    const CountRes c3 = countIf("e(sample)", session);
    const CountRes c3s = countIf("e(sample) & id==11", session);
    const LineResult d4 = runLine("areg y x1 x2, absorb(id)", session);
    const auto n4 = readNumber("e(N)", session);
    const CountRes c4 = countIf("e(sample)", session);
    const LineResult h8 = runLine("areg y x1 x2, absorb(id) cluster(cl)", session);
    const auto n8 = readNumber("e(N)", session);
    const CountRes c8 = countIf("e(sample)", session);
    w2check(d1b.rc == "0" && near(d1b.n, 0.0) && d3.rc == "0" && near(n3, 59.0) &&
                near(g3, 11.0) && near(c3.n, 59.0) && near(c3s.n, 1.0) && d4.rc == "0" &&
                near(n4, 59.0) && near(c4.n, 59.0) && h8.rc == "0" && near(n8, 58.0) &&
                near(c8.n, 58.0),
            "W2 esample T2 D1b/D3/D4/H8: after `reghdfe ..., absorb(id)` the dropped singleton id "
            "11 is out of the mask, 0 (w2_esample.log:826 |0|); `xtreg y x1 x2, fe` keeps it, "
            "e(N) 59, N_g 11, count 59 with id 11 in (:897 |0|59|59|11|); `areg ..., absorb(id)` "
            "59 (:925 |0|59|59|); `areg ..., absorb(id) cluster(cl)` 58 (:1957 |0|58|58|). Main: "
            "count r(198), e(N) 58/N_g 10, 58, 57. Got D1b " + show(d1b) + ", xtreg rc " + d3.rc +
                " e(N) " + num(n3) + " N_g " + num(g3) + " " + show(c3) + " id11 " + num(c3s.n) +
                ", areg rc " + d4.rc + " e(N) " + num(n4) + " " + show(c4) + ", areg cl rc " +
                h8.rc + " e(N) " + num(n8) + " " + show(c8));
    // The refits keep the singleton too (--keep-singletons): after `xtreg ...,
    // fe vce(cluster id)` the singleton is its own cluster, so a refit that
    // dropped it would count 10 clusters and answer a different SE.
    (void)runLine("quietly xtreg y x1 x2, fe vce(cluster id)", session);
    const auto bS = readNumber("_b[x1]", session);
    const auto sS = readNumber("_se[x1]", session);
    const LineResult lS = runLine("lincom x1", session);
    const auto lEst = readNumber("r(estimate)", session);
    const auto lSe = readNumber("r(se)", session);
    w2check(lS.rc == "0" && bS.has_value() && sS.has_value() && near(lEst, *bS) &&
                lSe.has_value() && std::fabs(*lSe - *sS) <= 1e-9 * *sS,
            "W2 esample T2 singleton refit: after `xtreg y x1 x2, fe vce(cluster id)` (id 11 is "
            "a one-observation cluster) `lincom x1` is _b[x1] with _se[x1] -- the refit keeps "
            "the singleton the fit kept. Got rc " + lS.rc + " `" + lS.msg + "` " + num(lEst) +
                "/" + num(bS) + " se " + num(lSe) + "/" + num(sS));
    // boottest keeps the singleton the fit kept (--keep-singletons, FU-733):
    // after `xtreg y x1 x2, fe vce(cluster id)` (e(N) 59, id 11 a one-row
    // cluster) it bootstraps 11 clusters, r(df_r) 10 (w2_esample_tsrefit.log:393
    // W3 served, W3_st :399 df_r=10). After `areg ..., absorb(id) vce(cluster
    // cl)` (e(N) 58) it is served too (FU-732, W1 :336 p .3125, W1_st :342
    // df_r=6) with r(t) the areg table's _b/_se. Before: the xtreg case a named
    // r(198) (the bootstrap dropped id 11: 58 rows, 10 clusters), the areg case
    // r(198) `after areg ..., vce(cluster)`.
    (void)runLine("quietly xtreg y x1 x2, fe vce(cluster id)", session);
    const LineResult bt = runLine("boottest x1, reps(99) seed(1) nograph", session);
    const auto btDf = readNumber("r(df_r)", session);
    (void)runLine("quietly areg y x1 x2, absorb(id) vce(cluster cl)", session);
    const auto abB = readNumber("_b[x1]", session);
    const auto abS = readNumber("_se[x1]", session);
    const LineResult ab = runLine("boottest x1, reps(99) seed(1) nograph", session);
    const auto abDf = readNumber("r(df_r)", session);
    const auto abT = readNumber("r(t)", session);
    const bool abTOk = abB.has_value() && abS.has_value() && abT.has_value() && *abS > 0 &&
                       std::fabs(*abT - *abB / *abS) <= 1e-12 * std::fabs(*abB / *abS);
    w2check(bt.rc == "0" && near(btDf, 10.0) && ab.rc == "0" && near(abDf, 6.0) && abTOk,
            "W2 esample T2 boottest keeps the fit's singleton: after `xtreg y x1 x2, fe "
            "vce(cluster id)` (e(N) 59) it is served over 11 clusters, r(df_r) 10 (w2_esample_"
            "tsrefit W3 :393/:399); after `areg ..., absorb(id) vce(cluster cl)` it is served, "
            "r(df_r) 6, r(t) = _b/_se (W1 :336/:342). Before: r(198) both. Got xtreg " + bt.rc +
                " `" + bt.msg + "` df_r " + num(btDf) + ", areg " + ab.rc + " `" + ab.msg +
                "` df_r " + num(abDf) + " t " + num(abT));
  }

  // ---- newey/prais (engine verbs with --esample-out) and a resampling prefix --
  // No StataBE arm reads e(sample) after newey/prais; the invariant every
  // wired fit owes is count == e(N) (TASKS T2 acceptance 2), here with an
  // `if` (newey) and listwise deletion (row 5's y). After `bootstrap ...:
  // regress` the e() is bootstrap's own posting, whose e(sample) is the
  // resampled estimation sample: 20 rows (t>10, row 5 outside it) -- StataBE
  // scratch-r150/lee/bs/o3.log `C3|esample|20|20|3|` (lane BS re-pin: until
  // uni-bs PIET left the last replicate's e() live and refused r(198)).
  {
    const std::string wd = (root / "t2_ts").u8string();
    fs::create_directories(wd, ec);
    piet::SessionState session(wd);
    for (const char* s0 : {"clear", "set obs 30", "generate t = _n", "tsset t",
                           "generate double x = mod(_n*7,11)", "generate double y = 2*x + mod(_n,4)",
                           "replace y = . in 5"})
      (void)runLine(s0, session);
    // `force`: row 5's missing y leaves t with a gap, which newey refuses without
    // it, r(198) "t is not regularly spaced" (StataBE 19.5, xtts m5.do S1; S2 with
    // force: e(N) 26 and 26 rows in e(sample)).
    const LineResult nw = runLine("newey y x if t>3, lag(1) force", session);
    const auto nNw = readNumber("e(N)", session);
    const CountRes cNw = countIf("e(sample)", session);
    const LineResult pr = runLine("prais y x, twostep", session);
    const auto nPr = readNumber("e(N)", session);
    const CountRes cPr = countIf("e(sample)", session);
    (void)runLine("quietly regress y x", session);
    (void)runLine("quietly bootstrap bx=_b[x], reps(3) seed(1): regress y x if t>10", session);
    const CountRes cBs = countIf("e(sample)", session);
    w2check(nw.rc == "0" && near(nNw, 26.0) && near(cNw.n, 26.0) && pr.rc == "0" &&
                near(nPr, 29.0) && near(cPr.n, 29.0) && cBs.rc == "0" && near(cBs.n, 20.0),
            "W2 esample T2 newey/prais/bootstrap: `count if e(sample)` equals e(N) after `newey "
            "y x if t>3` (26) and `prais` (29); after `bootstrap ...: regress y x if t>10` it is "
            "bootstrap's own e(sample), 20 rows (StataBE o3.log C3). Got "
            "newey " + nw.rc + " e(N) " + num(nNw) + " " + show(cNw) + ", prais " + pr.rc +
                " e(N) " + num(nPr) + " " + show(cPr) + ", bootstrap " + show(cBs));
  }

  // ---- H3-H6, D6: the iv and xtreg,fe routes (estimation_m4b.cpp runIvRegress,
  // estimation_m4.cpp runXtreg); C6/C6b: a TS regressor under `if` (FU-475);
  // D1's e(N_hdfe) (FU-479). Main refused every e(sample) read here (r(198)),
  // answered C6 e(N) 30 at rc 0 and D1 e(N_hdfe) `.`.
  {
    const std::string wd = (root / "t2_ivxt").u8string();
    fs::create_directories(wd, ec);
    piet::SessionState session(wd);
    for (const char* s0 : kEsampleFrame) (void)runLine(s0, session);
    (void)runLine("xtset id t", session);
    struct Arm {
      const char* fit;
      double want;
      const char* cite;
    };
    const Arm arms[] = {
        {"ivregress 2sls y x2 (x1 = z1)", 59, "H3 :1752 |0|59|59|"},
        {"ivreg2 y x2 (x1 = z1), cluster(cl)", 58, "H4 :1804 |0|58|58|"},
        {"ivreghdfe y x2 (x1 = z1), absorb(id)", 58, "H5 :1865 |0|58|58|"},
        {"xtreg y x1 x2 if t>=2, fe", 50, "H6 :1900 |0|50|50|"},
        {"regress y L.x1 x2 if t>=3", 40, "C6 :775 |0|40|40|"},
    };
    for (const Arm& a : arms) {
      const LineResult f = runLine(a.fit, session);
      const auto n = readNumber("e(N)", session);
      const CountRes c = countIf("e(sample)", session);
      w2check(f.rc == "0" && near(n, a.want) && c.rc == "0" && near(c.n, a.want),
              std::string("W2 esample T2 ") + a.cite + ": after `" + a.fit + "` e(N) and `count "
              "if e(sample)` are both " + num(a.want) + " (w2_esample.log). Main: count r(198)" +
                  (a.want == 40 ? ", e(N) 30 (the `if` pre-filter ran before L.)" : "") +
                  ". Got fit rc " + f.rc + " `" + f.msg + "` e(N) " + num(n) + ", count " +
                  show(c));
    }
    // C6b: the t==3 rows, whose L.x1 reads the t==2 row outside the `if`.
    (void)runLine("quietly regress y L.x1 x2 if t>=3", session);
    const CountRes c6b = countIf("e(sample) & t==3", session);
    // FU-475's refit half: lincom/test/nlcom refit through the engine, which
    // now applies the replayed `if` as a row mask after L. (cmds_postest.cpp
    // postest_refit), so `lincom L.x1` IS _b[L.x1] with _se[L.x1]. So do the
    // margins and predict refits (cmd_margins / cmd_predict --fit-keepif,
    // plan_keepif): `predict ... if e(sample)` fills the fit's 40 rows and
    // their mean, like bare `margins`, is the sample mean of y (least squares
    // with a constant). predict on a MOVED head (a second predict) sends the
    // fit's frame with the `if` (`--fit-data` + `--fit-keepif`): the same 40-row
    // fit, so its xb equals the first on every sample row, and it fills the 49
    // rows whose L.x1 and x2 exist. Main answered the xtreg,fe twin's
    // lincom -.8123398 against _b[L.x1] -.7953699 at rc 0, and predict/margins
    // after any L. fit were ts_no_tsset r(111).
    const auto bL = readNumber("_b[L.x1]", session);
    const auto sL = readNumber("_se[L.x1]", session);
    const LineResult lc = runLine("lincom L.x1", session);
    const auto lcEst = readNumber("r(estimate)", session);
    const auto lcSe = readNumber("r(se)", session);
    (void)runLine("quietly summarize y if e(sample)", session);
    const auto yMean = readNumber("r(mean)", session);
    const LineResult mg = runLine("margins", session);
    const auto mgB = readNumber("el(r(b),1,1)", session);
    const LineResult pd = runLine("predict double p6 if e(sample)", session);
    const CountRes pdN = countIf("!missing(p6)", session);
    (void)runLine("quietly summarize p6", session);
    const auto pdMean = readNumber("r(mean)", session);
    const LineResult pdMoved = runLine("predict double p7", session);  // the head moved (p6)
    const CountRes p7N = countIf("!missing(p7)", session);
    const CountRes p7Diff = countIf("e(sample) & abs(p7 - p6) > 1e-9", session);
    auto close9 = [](std::optional<double> a, std::optional<double> b) {
      return a.has_value() && b.has_value() &&
             std::fabs(*a - *b) <= 1e-9 * std::max(1.0, std::fabs(*b));
    };
    const bool predictServed = pd.rc == "0" && near(pdN.n, 40.0) && close9(pdMean, yMean) &&
                               mg.rc == "0" && close9(mgB, yMean) && pdMoved.rc == "0" &&
                               near(p7N.n, 49.0) && p7Diff.rc == "0" && near(p7Diff.n, 0.0);
    (void)runLine("quietly xtreg y L.x1 x2 if t>=3, fe", session);
    const auto nXt = readNumber("e(N)", session);
    const auto bXt = readNumber("_b[L.x1]", session);
    const auto sXt = readNumber("_se[L.x1]", session);
    const LineResult lcXt = runLine("lincom L.x1", session);
    const auto lcXtEst = readNumber("r(estimate)", session);
    const auto lcXtSe = readNumber("r(se)", session);
    // FU-759 (w2_esample_tsrefit.log:251-263 T7 `|0|40|+1.9d11111111113X+003|`):
    // xb after the xtreg,fe twin is x b + _cons over the fit's 40 rows, whose
    // mean (an FE fit with a constant) is the sample mean of y -- T1's mean.
    // BUG-1259: u is served there too (StataBE 19.5 scratch-laneA/agents/N2/e12.do,
    // `predict u` after this fit: 40 rows), and so is xb on a moved head (the
    // live frame refits once its columns are proved the fit's): the same xb on
    // e(sample). Main: `predict double p8 if e(sample), xb` r(198), p8 never created.
    (void)runLine("quietly summarize y if e(sample)", session);
    const auto yXtMean = readNumber("r(mean)", session);
    const LineResult pXt = runLine("predict double p8 if e(sample), xb", session);
    const CountRes pXtN = countIf("!missing(p8)", session);
    (void)runLine("quietly summarize p8", session);
    const auto pXtMean = readNumber("r(mean)", session);
    const LineResult pXtU = runLine("predict double p9 if e(sample), u", session);
    const CountRes pXtUN = countIf("!missing(p9)", session);
    const LineResult pXtMoved = runLine("predict double p10, xb", session);
    const CountRes pXtMovedDiff = countIf("e(sample) & abs(p10 - p8) > 1e-9", session);
    w2check(pXt.rc == "0" && near(pXtN.n, 40.0) && close9(pXtMean, yXtMean) && pXtU.rc == "0" &&
                near(pXtUN.n, 40.0) && pXtMoved.rc == "0" && pXtMovedDiff.rc == "0" &&
                near(pXtMovedDiff.n, 0.0),
            "W2 esample T2 T7 (w2_esample_tsrefit.log:263 |0|40|): after `xtreg y L.x1 x2 if t>=3, "
            "fe`, `predict double p8 if e(sample), xb` fills the fit's 40 rows and their mean is "
            "the sample mean of y; `u` fills the 40 rows (BUG-1259) and xb on a moved head is p8 "
            "on e(sample). Main: xb r(198). Got xb " + pXt.rc + " `" + pXt.msg + "` n " +
                num(pXtN.n) + " mean " + num(pXtMean) + " vs y " + num(yXtMean) + ", u " + pXtU.rc +
                " `" + pXtU.msg + "` n " + num(pXtUN.n) + ", moved " + pXtMoved.rc + " `" +
                pXtMoved.msg + "` diff " + num(pXtMovedDiff.n));
    const bool refitsServed =
        lc.rc == "0" && bL.has_value() && sL.has_value() && near(lcEst, *bL) &&
        lcSe.has_value() && std::fabs(*lcSe - *sL) <= 1e-9 * *sL && near(nXt, 40.0) &&
        lcXt.rc == "0" && bXt.has_value() && sXt.has_value() && near(lcXtEst, *bXt) &&
        lcXtSe.has_value() && std::fabs(*lcXtSe - *sXt) <= 1e-9 * *sXt;
    // `in` with a TS regressor: no boundary-safe route, named r(198).
    const LineResult inr = runLine("regress y L.x1 x2 in 10/50", session);
    w2check(near(c6b.n, 10.0) && refitsServed && predictServed && inr.rc == "198" &&
                inr.msg.find("`in` range") != std::string::npos,
            "W2 esample T2 C6b (w2_esample.log:782 |10|): the 10 t==3 rows are in the sample of "
            "`regress y L.x1 x2 if t>=3`; `lincom L.x1` after it and after `xtreg y L.x1 x2 if "
            "t>=3, fe` (e(N) 40) equals _b[L.x1] with _se[L.x1] (the refit applies the `if` after "
            "L.); `predict double p6 if e(sample)` fills 40 rows whose mean, like bare `margins`, "
            "is the sample mean of y; a second predict (moved head) fills the 49 rows with L.x1 "
            "and x2 and equals the first on the sample; `regress y L.x1 x2 in 10/50` is a named "
            "r(198). Main: count r(198), lincom served the 30-row pre-filtered fit "
            "(xtreg twin -.8123398 vs _b -.7953699), predict/margins r(111) ts_no_tsset, the `in` "
            "fit lost its boundary rows at rc 0. Got C6b " + show(c6b) + ", lincom rc " + lc.rc +
                " `" + lc.msg + "` " +
                num(lcEst) + "/" + num(bL) + " se " + num(lcSe) + "/" + num(sL) + ", xtreg e(N) " +
                num(nXt) + " lincom rc " + lcXt.rc + " `" + lcXt.msg + "` " + num(lcXtEst) + "/" +
                num(bXt) + " se " + num(lcXtSe) + "/" + num(sXt) + ", predict " + pd.rc + " `" +
                pd.msg + "` n " + num(pdN.n) + " mean " + num(pdMean) + " vs y " + num(yMean) +
                ", margins " + mg.rc + " `" + mg.msg + "` " + num(mgB) + ", moved-head predict " +
                pdMoved.rc + " `" + pdMoved.msg + "` n " + num(p7N.n) + " diff " + show(p7Diff) +
                ", in-fit " + inr.rc + " `" + inr.msg + "`");
    // D1: e(N_hdfe) after reghdfe absorb(id); D6: reghdfe 6.x has no IV syntax,
    // `reghdfe y x2 (x1=z1) ...` is r(132) and e() keeps the previous fit, whose
    // e(sample) still counts e(N) (w2_esample.log:967-975 `132|57|57|`).
    (void)runLine("reghdfe y x1 x2, absorb(id)", session);
    const auto nh = readNumber("e(N_hdfe)", session);
    const LineResult d6 = runLine("reghdfe y x2 (x1=z1) [aw=w], absorb(id) cluster(cl)", session);
    const auto n6 = readNumber("e(N)", session);
    const CountRes c6 = countIf("e(sample)", session);
    w2check(near(nh, 1.0) && d6.rc == "132" && n6.has_value() && near(c6.n, *n6),
            "W2 esample T2 D1 (w2_esample.log:819 |0|58|58|1|) and D6 (:967-975): e(N_hdfe) is 1 "
            "after `reghdfe ..., absorb(id)`; `reghdfe y x2 (x1=z1) [aw=w], absorb(id) "
            "cluster(cl)` is r(132) and `count if e(sample)` == e(N) of the fit before it. Got "
            "N_hdfe " + num(nh) + ", D6 rc " + d6.rc + " e(N) " + num(n6) + " count " + show(c6));
  }

  // ---- E, F: the mask follows the rows; a second fit replaces it --------------
  {
    const std::string wd = (root / "t2_follow").u8string();
    fs::create_directories(wd, ec);
    piet::SessionState session(wd);
    for (const char* s0 : kEsampleFrame) (void)runLine(s0, session);
    (void)runLine("quietly regress y x1 x2 if id<=5", session);
    (void)runLine("preserve", session);
    (void)runLine("sort t id", session);
    const CountRes e1 = countIf("e(sample)", session);
    const CountRes e1b = countIf("e(sample) & id<=5", session);
    (void)runLine("restore", session);
    (void)runLine("preserve", session);
    (void)runLine("generate double qe2 = 1", session);
    (void)runLine("replace y = . if id==2", session);
    const CountRes e2 = countIf("e(sample)", session);
    (void)runLine("restore", session);
    (void)runLine("preserve", session);
    (void)runLine("drop if id==1", session);
    const CountRes e4 = countIf("e(sample)", session);
    (void)runLine("restore", session);
    (void)runLine("preserve", session);
    (void)runLine("keep if id<=7", session);
    const CountRes e5 = countIf("e(sample)", session);
    (void)runLine("restore", session);
    (void)runLine("preserve", session);
    (void)runLine("keep id t", session);
    const CountRes keepVars = countIf("e(sample)", session);
    (void)runLine("restore", session);
    const CountRes e12 = countIf("e(sample)", session);
    w2check(near(e1.n, 28.0) && near(e1b.n, 28.0) && near(e2.n, 28.0) && near(e4.n, 22.0) &&
                near(e5.n, 28.0) && near(keepVars.n, 28.0) && near(e12.n, 28.0),
            "W2 esample T2 E1-E5/E12: the mask follows the rows -- after `sort t id` 28 (all "
            "id<=5), after gen + `replace y = .` 28, after `drop if id==1` 22, after `keep if "
            "id<=7` 28, after `keep id t` 28 (TASKS T2 acceptance 5), after restore 28 "
            "(w2_esample.log:996 |0|28|28|, :1017 |0|28|, :1036 |0|22|28|, :1055 |0|28|, :1229 "
            "|0|28|). Main: r(198). Got " + show(e1) + "/" + num(e1b.n) + ", " + show(e2) +
                ", " + show(e4) + ", " + show(e5) + ", keep id t " + show(keepVars) + ", " +
                show(e12));

    // E14: save + use drops the marker (export-dta writes no hidden column);
    // E15: `clear` drops the data and the marker with it, e() stays.
    const std::string dta = (fs::path(wd) / "copyE14.dta").u8string();
    const std::string csv = (fs::path(wd) / "copyE14.csv").u8string();
    (void)runLine("export delimited using \"" + csv + "\", replace", session);
    std::string header;
    {
      std::FILE* fh = std::fopen(csv.c_str(), "r");
      if (fh != nullptr) {
        char buf[512];
        if (std::fgets(buf, sizeof(buf), fh) != nullptr) header = buf;
        std::fclose(fh);
      }
    }
    (void)runLine("save \"" + dta + "\", replace", session);
    (void)runLine("preserve", session);
    const LineResult use = runLine("use \"" + dta + "\", clear", session);
    const CountRes e14 = countIf("e(sample)", session);
    const std::string usedVars = dsVarlist(session);
    const auto n14 = readNumber("e(N)", session);
    (void)runLine("restore", session);
    (void)runLine("preserve", session);
    (void)runLine("clear", session);
    const CountRes e15 = countIf("e(sample)", session);
    (void)runLine("restore", session);
    const CountRes back = countIf("e(sample)", session);
    w2check(use.rc == "0" && e14.rc == "0" && near(e14.n, 0.0) && near(n14, 28.0) &&
                usedVars.find("__piet") == std::string::npos && !header.empty() &&
                header.find("__piet") == std::string::npos && e15.rc == "0" &&
                near(e15.n, 0.0) && near(back.n, 28.0),
            "W2 esample T2 E14/E15 + writers: `export delimited` writes no `__piet_` column; "
            "after `save` + `use` the marker is gone, `count if e(sample)` 0 with e(N) still 28; "
            "after `clear` 0; `restore` brings the rows and the mask back, 28 (w2_esample.log:1272 "
            "|0|0|0|28|, :1290 |0|0|28|, :1229). Main: r(198). Got header `" + header +
                "`, use rc " + use.rc + " " + show(e14) + " e(N) " + num(n14) + " vars `" +
                usedVars + "`, clear " + show(e15) + ", back " + show(back));

    // E11 (BUG-785): dropping every variable empties the observations too, so
    // `set obs 5` grows an empty dataset and the new rows are outside the
    // sample. Main: `set obs 5` was "cannot reduce the number of observations
    // from 61 to 5", an uncaptured stop.
    (void)runLine("preserve", session);
    const LineResult dropAll = runLine("drop _all", session);
    const LineResult setObs = runLine("set obs 5", session);
    const CountRes e11 = countIf("e(sample)", session);
    const auto n11 = readNumber("_N", session);
    (void)runLine("restore", session);
    const CountRes e11back = countIf("e(sample)", session);
    w2check(dropAll.rc == "0" && setObs.rc == "0" && e11.rc == "0" && near(e11.n, 0.0) &&
                near(n11, 5.0) && near(e11back.n, 28.0),
            "W2 esample T2 E11 (BUG-785): `drop _all` then `set obs 5` is rc 0 with _N 5 and "
            "`count if e(sample)` 0; `restore` brings the 28 back (w2_esample.log:1205 \"was 0, "
            "now 5\", :1215 |0|0|28|). Main: set obs rc " + setObs.rc + " `" + setObs.msg +
                "`. Got drop " + dropAll.rc + ", set obs " + setObs.rc + " `" + setObs.msg +
                "`, " + show(e11) + " _N " + num(n11) + ", after restore " + show(e11back));

    const LineResult f1 = runLine("regress y x1 if id>=6", session);
    const CountRes cf1 = countIf("e(sample)", session);
    (void)runLine("quietly summarize y", session);
    const CountRes cf2 = countIf("e(sample)", session);
    (void)runLine("quietly correlate y x1", session);
    const CountRes cf3 = countIf("e(sample)", session);
    w2check(f1.rc == "0" && near(cf1.n, 31.0) && near(cf2.n, 31.0) && near(cf3.n, 31.0),
            "W2 esample T2 F1-F3: a second fit replaces the mask (31), and an r-class "
            "`summarize`/`correlate` leaves it alone (w2_esample.log:1318 |0|31|31|, :1327 |31|, "
            ":1336 |31|). Main: r(198). Got " + show(cf1) + ", " + show(cf2) + ", " + show(cf3));

    // G10/G11: `estimates clear` keeps the active results, `ereturn clear`
    // ends them (lastModel_reset, the one producer of "no model").
    (void)runLine("quietly regress y x1 x2 if id<=5", session);
    (void)runLine("estimates store B", session);
    (void)runLine("estimates clear", session);
    const CountRes g10 = countIf("e(sample)", session);
    (void)runLine("quietly regress y x1 x2 if id<=5", session);
    const LineResult g11c = runLine("ereturn clear", session);
    const CountRes g11 = countIf("e(sample)", session);
    const LineResult g11d = runLine("display e(sample)", session);
    w2check(near(g10.n, 28.0) && g11c.rc == "0" && g11.rc == "0" && near(g11.n, 0.0) &&
                g11d.text == "0",
            "W2 esample T2 G10/G11: after `estimates clear` the active model's mask still counts "
            "28; after `ereturn clear` e(sample) is 0 in the row and scalar planes "
            "(w2_esample.log:1621 |0|28|28|, :1634 |0|0|.|). Main: r(198). Got " + show(g10) +
                ", ereturn clear rc " + g11c.rc + " " + show(g11) + " display `" + g11d.text +
                g11d.msg + "`");
  }

  // ---- E6/E10 and the copy verbs: which rows keep the marker -----------------
  // Oracles: w2_esample.log:1076 E6 `expand 2 if id==3` -> |0|0|28|67|, :1193
  // E10 `collapse (sum) y, by(id)` -> |0|0|3|11|; and the StataBE capture
  // w2_esample_copies (archived 2026-10-13 by the supervisor's stata_capture
  // job: internal capture w2_esample_copies.log),
  // whose fit `regress y x1 x2 if
  // inS` (23 rows) gives each id group a different first/last/any/all pattern:
  // K1 collapse by(id) 4 (first_diff=0), K2 the same after `gsort id -t` 3, K4
  // by(id half) 8, K5 no by() 0, K6 `contract id` 4, X1 `expand 2 if id==1` 23
  // with 0 created, X3 `cross` (3 using rows) 23 of 183, X5 `fillin id t` 23 of
  // 66, M2 `merge m:1` 23 of 62 (using-only rows 0), X6 `xpose, clear` 0 of 4,
  // X7b `stack x1 x2, into(a) clear` 0 of 122, X7 `stack x1 y x2 y, into(a b)
  // clear` 0 of 122 (into() names one block: (x1,y) over (x2,y); PIET refused
  // it r(198) before), X4 joinby 19 of 72 (one copy of each of the 19 marked
  // master rows keeps the marker; which copy follows Stata's sort tie order,
  // X4b, so PIET keeps the first and only the count is pinned). K3 (data not
  // in by-group order: Stata's own tie order picks the row, 2) and X2 bsample
  // (Stata's random draw) are named r(198) here, PIET refusals by design. The
  // golden is tests/eval/golden/aer_w2/w2_esample_copies.{do,log} (landed
  // d6613eff); every record above is that log's, byte for byte.
  // Main: E6 rc 1 (`constant count only`) with the copies counted where expand
  // ran, E10 0 at rc 0, every other arm r(198) (no mask).
  {
    const std::string wd = (root / "t2_copies").u8string();
    fs::create_directories(wd, ec);
    piet::SessionState session(wd);
    for (const char* s0 : kEsampleFrame) (void)runLine(s0, session);
    (void)runLine("xtset id t", session);
    // E6 / E10 on the oracle's own fit.
    (void)runLine("quietly regress y x1 x2 if id<=5", session);
    (void)runLine("preserve", session);
    const LineResult e6 = runLine("expand 2 if id==3", session);
    const CountRes c6 = countIf("e(sample)", session);
    const auto n6 = readNumber("_N", session);
    const CountRes c6new = countIf("e(sample) & _n > 61", session);
    (void)runLine("restore", session);
    (void)runLine("preserve", session);
    const LineResult e10 = runLine("collapse (sum) y, by(id)", session);
    const CountRes c10 = countIf("e(sample)", session);
    const auto n10 = readNumber("_N", session);
    (void)runLine("restore", session);
    w2check(e6.rc == "0" && near(c6.n, 28.0) && near(n6, 67.0) && near(c6new.n, 0.0) &&
                e10.rc == "0" && near(c10.n, 3.0) && near(n10, 11.0),
            "W2 esample T2 E6/E10: `expand 2 if id==3` is rc 0, _N 67, the 28 sample rows keep "
            "the marker and the 6 created rows are out (w2_esample.log:1076 |0|0|28|67|); after "
            "`collapse (sum) y, by(id)` each result row carries its group's first observation's "
            "marker: 3 of 11 (:1193 |0|0|3|11|; ids 1, 4, 5 -- rows 7 and 13 open ids 2 and 3). "
            "Main: expand rc 1, collapse 0. Got expand rc " + e6.rc + " `" + e6.msg + "` " +
                show(c6) + " _N " + num(n6) + " created " + show(c6new) + ", collapse rc " +
                e10.rc + " `" + e10.msg + "` " + show(c10) + " _N " + num(n10));

    // The capture's own fit.
    for (const char* s0 :
         {"generate byte inS = 0", "replace inS = 1 if id==1", "replace inS = 1 if id==2 & t>=2",
          "replace inS = 1 if id==4 & t<=5", "replace inS = 1 if id==5 & t==3",
          "replace inS = 1 if id==6 & inlist(t,1,6)", "replace inS = 1 if id==7 & t<=4",
          "quietly regress y x1 x2 if inS", "generate byte m0 = e(sample)"})
      (void)runLine(s0, session);
    const CountRes p0 = countIf("e(sample)", session);
    struct Arm {
      const char* setup;  // may be empty
      const char* verb;
      double want;        // < 0: the named r(198)
      double wantN;
      const char* cite;
    };
    const std::string kfile = (fs::path(wd) / "kfile.dta").u8string();
    const std::string jfile = (fs::path(wd) / "jfile.dta").u8string();
    const std::string mfile = (fs::path(wd) / "m1file.dta").u8string();
    (void)runLine("preserve", session);
    for (const char* s0 : {"clear", "set obs 4", "generate int id = cond(_n<=3, _n, 20)",
                           "generate int k = 10*_n"})
      (void)runLine(s0, session);
    (void)runLine("save \"" + mfile + "\", replace", session);
    for (const char* s0 : {"clear", "set obs 3", "generate int k = _n"}) (void)runLine(s0, session);
    (void)runLine("save \"" + kfile + "\", replace", session);
    for (const char* s0 : {"clear", "set obs 12", "generate int id = ceil(_n/2)",
                           "generate int k = mod(_n,2)"})
      (void)runLine(s0, session);
    (void)runLine("save \"" + jfile + "\", replace", session);
    (void)runLine("restore", session);
    const std::string crossLine = "cross using \"" + kfile + "\"";
    const std::string joinLine = "joinby id using \"" + jfile + "\"";
    const std::string mergeLine = "merge m:1 id using \"" + mfile + "\"";
    const Arm arms[] = {
        {"", "collapse (sum) y, by(id)", 4, 11, "K1 |0|4|11|first_diff=0|"},
        {"gsort id -t", "collapse (sum) y, by(id)", 3, 11, "K2 |0|3|11|first_diff=0|"},
        {"generate byte half = (t >= 4)", "collapse (sum) y, by(id half)", 8, 21,
         "K4 |0|8|21|first_diff=0|"},
        {"", "collapse (sum) y", 0, 1, "K5 |0|0|1|"},
        {"", "contract id", 4, 11, "K6 |0|4|11|"},
        {"", "contract id m0", 6, 16, "K7 |0|6|16|m0_diff=0|"},
        {"", "expand 2 if id==1", 23, 67, "X1 |0|23|67|created=0|"},
        {"", crossLine.c_str(), 23, 183, "X3 |0|23|183|, X3b k1=23"},
        {"", "fillin id t", 23, 66, "X5 |0|23|66|created=0|"},
        {"", mergeLine.c_str(), 23, 62, "M2 |0|23|62|using_only=0|"},
        {"keep id t x1 x2", "xpose, clear", 0, 4, "X6 |0|0|4|"},
        {"", "stack x1 x2, into(a) clear", 0, 122, "X7b |0|0|122|"},
        {"", "stack x1 y x2 y, into(a b) clear", 0, 122, "X7 |0|0|122|s1=0|s2=0| (.log:702)"},
        {"", joinLine.c_str(), 19, 72, "X4 |0|19|72|m0=38|m0_diff=19| (.log:470)"},
        {"sort t id", "collapse (sum) y, by(id)", -1, 11, "K3 (Stata 2, tie order)"},
        // bsample.ado's fast method on the Stata stream (core/rng_mt64.h): after
        // `set seed 20261013` the drawn rows are Stata's, and the marker stays on
        // the 10 marked originals drawn (w2_esample_copies.log X2 `0|10|61|m0=20|`).
        {"set seed 20261013", "bsample", 10, 61, "X2 |0|10|61|m0=20|m0_diff=10|"},
    };
    std::string fails;
    for (const Arm& a : arms) {
      (void)runLine("preserve", session);
      if (*a.setup != '\0') (void)runLine(a.setup, session);
      const LineResult v = runLine(a.verb, session);
      const CountRes c = countIf("e(sample)", session);
      const auto n = readNumber("_N", session);
      (void)runLine("restore", session);
      const bool ok = v.rc == "0" && near(n, a.wantN) &&
                      (a.want < 0 ? c.rc == "198" &&
                                        c.msg.find("not available after") != std::string::npos
                                  : c.rc == "0" && near(c.n, a.want));
      if (!ok)
        fails += std::string(" [") + a.verb + ": verb rc " + v.rc + " `" + v.msg + "` _N " +
                 num(n) + " " + show(c) + "; want " + a.cite + "]";
    }
    const CountRes back = countIf("e(sample)", session);
    w2check(near(p0.n, 23.0) && fails.empty() && near(back.n, 23.0),
            "W2 esample T2 copy verbs (StataBE capture w2_esample_copies): after `regress y x1 x2 "
            "if inS` (23) collapse/contract carry each group's first observation's marker (K1 4, "
            "K2 3, K4 8, K6 4, K7 6), collapse without by() 0 (K5), expand/cross/fillin keep the "
            "originals and leave created rows out (X1/X3/X5 23), merge m:1 reads its using-only "
            "rows 0 (M2 23), xpose and stack read 0 (X6, X7b, X7 with into(a b)), joinby keeps "
            "one copy per marked row (X4 19 of 72), bsample keeps it on the drawn originals (X2 10 "
            "of 61, Stata's rows from `set seed 20261013`); ungrouped collapse is named "
            "r(198); restore brings the 23 back. Main: count 0, r(198), stack into(a b) refused "
            "or the copies' rows counted. Got P0 " + show(p0) + ", restore " + show(back) +
                (fails.empty() ? std::string() : ", fails:" + fails));

    // (e) the e(sample) predicate is LastModel's esample state, not the macro
    // layer's absent-e() test: pca installs a model with no result JSON and no
    // mask, so every layer refuses by name (never 0 at rc 0, which the
    // `!lastModelResult` predicate would have answered).
    (void)runLine("quietly pca x1 x2", session);
    const CountRes pc = countIf("e(sample)", session);
    const LineResult pd = runLine("display e(sample)", session);
    const LineResult pl = runLine("local s = e(sample)", session);
    w2check(pc.rc == "198" && pc.msg.find("pca") != std::string::npos && pd.rc == "198" &&
                pd.text.empty() && pl.rc == "198",
            "W2 esample T2 (e) pca: after `pca x1 x2` (a model with no e() result and no mask) "
            "`count if e(sample)`, `display e(sample)` and `local s = e(sample)` are r(198) naming "
            "pca, never 0 at rc 0. Got count " + show(pc) + ", display rc " + pd.rc + " `" +
                pd.text + "`, local rc " + pl.rc);
  }

  // ---- merge 1:m copies; the install's frame reuse; setcol --ones-as ------------
  // (review 2026-10-13 of this task's candidate fbd27863.)
  {
    const std::string wd = (root / "t2_merge").u8string();
    fs::create_directories(wd, ec);
    piet::SessionState session(wd);
    const std::string u2 = (fs::path(wd) / "u2.dta").u8string();
    const std::string u1 = (fs::path(wd) / "u1.dta").u8string();
    for (const char* s0 : {"clear", "set obs 22", "generate int id = ceil(_n/2)",
                           "generate int k = mod(_n,2)"})
      (void)runLine(s0, session);
    (void)runLine("save \"" + u2 + "\", replace", session);
    for (const char* s0 : {"clear", "set obs 11", "generate int id = _n", "generate int k = 10*_n"})
      (void)runLine(s0, session);
    (void)runLine("save \"" + u1 + "\", replace", session);
    for (const char* s0 : kEsampleFrame) (void)runLine(s0, session);
    (void)runLine("keep if t==1", session);
    (void)runLine("quietly regress y x1 x2", session);
    const auto nFit = readNumber("e(N)", session);
    // Two using rows per id: each master row is copied. FU-496: StataBE keeps
    // the marker on ONE copy, the first in merge's output order, and e(N) is
    // unchanged (w2_esample_copies.log:596 M1
    // `0|9|0|9|22|matched=9|not_first_of_run=0|k0=0|k1=9|`): 9 marked rows, none
    // of them a run's second copy, all on the k==1 copy (the using file's first
    // row of each id). Before: a candidate counted 18 at rc 0, then the branch
    // withdrew the marker r(198).
    (void)runLine("preserve", session);
    const LineResult m2 = runLine("merge 1:m id using \"" + u2 + "\"", session);
    const CountRes c2 = countIf("e(sample)", session);
    (void)runLine("generate int obs1 = id", session);
    const CountRes c2fo = countIf("e(sample) & obs1[_n-1]==obs1", session);
    const CountRes c2k1 = countIf("e(sample) & k==1", session);
    const CountRes c2m3 = countIf("e(sample) & _merge==3", session);
    (void)runLine("restore", session);
    // Control: a unique using file copies no master row; the mask follows.
    (void)runLine("preserve", session);
    const LineResult m1 = runLine("merge 1:m id using \"" + u1 + "\"", session);
    const CountRes c1 = countIf("e(sample)", session);
    (void)runLine("restore", session);
    w2check(near(nFit, 9.0) && m2.rc == "0" && c2.rc == "0" && near(c2.n, 9.0) &&
                near(c2fo.n, 0.0) && near(c2k1.n, 9.0) && near(c2m3.n, 9.0) && m1.rc == "0" &&
                c1.rc == "0" && near(c1.n, 9.0),
            "W2 esample T2 merge 1:m (FU-496, M1): after `regress y x1 x2` on one row per id (e(N) "
            "9), a `merge 1:m` whose using file has two rows per id keeps the marker on the first "
            "copy of each master row: 9 (matched 9, not_first_of_run 0, k1 9); with a unique "
            "using file the 9 follow. Main: r(198) (the marker withdrawn). Got e(N) " +
                num(nFit) + ", merge rc " + m2.rc + " `" + m2.msg + "` " + show(c2) + " fo " +
                show(c2fo) + " k1 " + show(c2k1) + " m3 " + show(c2m3) + ", control rc " +
                m1.rc + " " + show(c1));

    // The install's frame reuse: a second fit on the unmoved install frame
    // names the first install's on-disk base as its frame (no full-frame write
    // per fit), and a refit replaying that base reads the resident head.
    for (const char* s0 : kEsampleFrame) (void)runLine(s0, session);
    (void)runLine("quietly regress y x1 x2", session);
    const std::string base1 = session.lastModel() ? session.lastModel()->dataPath : "";
    (void)runLine("quietly regress y x1 x2 if id<=5", session);
    const std::string base2 = session.lastModel() ? session.lastModel()->dataPath : "";
    std::error_code fec;
    const bool base2OnDisk = !base2.empty() && fs::exists(base2, fec);  // the live model's frame
    const CountRes r2 = countIf("e(sample)", session);
    const auto b2 = readNumber("_b[x1]", session);
    const LineResult l2 = runLine("lincom x1", session);
    const auto l2e = readNumber("r(estimate)", session);
    const std::string inst2 = session.lastModel() ? session.lastModel()->esampleInstallPath : "";
    // The same sample again (another vce()): byte-identical admitted rows on the
    // unmoved head, so nothing is installed -- the head stays the install frame.
    (void)runLine("quietly regress y x1 x2 if id<=5, vce(robust)", session);
    const std::string inst2b = session.lastModel() ? session.lastModel()->esampleInstallPath : "";
    const std::string head2b = session.currentHbin().value_or("");
    const CountRes r2b = countIf("e(sample)", session);
    (void)runLine("generate double gmove = 1", session);  // the head moves: no reuse
    (void)runLine("quietly regress y x1 x2 if id<=7", session);
    const std::string base3 = session.lastModel() ? session.lastModel()->dataPath : "";
    const auto n3 = readNumber("e(N)", session);
    const CountRes r3 = countIf("e(sample)", session);
    w2check(!base1.empty() && base2 == base1 && base2OnDisk && near(r2.n, 28.0) &&
                l2.rc == "0" && b2.has_value() && near(l2e, *b2) && !inst2.empty() &&
                inst2b == inst2 && head2b == inst2 && near(r2b.n, 28.0) && !base3.empty() &&
                base3 != base1 && fs::exists(base3, fec) && n3.has_value() && near(r3.n, *n3),
            "W2 esample T2 install reuse: the second of two fits on the unmoved install frame "
            "records the first fit's on-disk frame (the candidate wrote the whole frame to disk "
            "per fit: 1M rows, 56 -> 240 ms), counts its own 28 (w2_esample.log:168 B1) and "
            "`lincom x1` is _b[x1]; the same sample under vce(robust) installs nothing (the head "
            "stays that install frame, 28); after `generate` the next fit records a new frame and "
            "counts e(N). Got base1 `" + base1 + "` base2 `" + base2 + "` " + show(r2) +
                " lincom rc " + l2.rc + " " + num(l2e) + "/" + num(b2) + ", robust install `" +
                inst2b + "` vs `" + inst2 + "` head `" + head2b + "` " + show(r2b) + ", base3 `" +
                base3 + "` e(N) " +
                num(n3) + " " + show(r3));

    // setcol --ones-as (the install's 1 -> serial map in the engine): a values
    // file that is not 0/1 is refused, never stored.
    session.ensureMaterializedHead("selftest-onesas");
    const std::string head = session.currentHbin().value_or("");
    const std::string vals = (fs::path(wd) / "onesas.txt").u8string();
    const std::string outp = (fs::path(wd) / "onesas_out.hbin").u8string();
    auto writeVals = [&](int bad) {
      std::FILE* f = std::fopen(vals.c_str(), "w");
      if (f == nullptr) return;
      for (int i = 1; i <= 61; ++i) std::fprintf(f, "%d\n", i == 5 ? bad : (i % 2));
      std::fclose(f);
    };
    writeVals(2);
    const piet::CobraRunResult sb =
        piet::runCobra({"setcol", "--data", head, "--var", "__piet_esample__", "--values", vals,
                        "--dtype", "f64", "--new", "--ones-as", "7", "--out", outp});
    writeVals(1);
    const piet::CobraRunResult sg =
        piet::runCobra({"setcol", "--data", head, "--var", "__piet_esample__", "--values", vals,
                        "--dtype", "f64", "--new", "--ones-as", "7", "--out", outp});
    w2check(!head.empty() && !sb.ok && sb.stderr_text.find("not 0 or 1") != std::string::npos &&
                sg.ok,
            "W2 esample T2 setcol --ones-as: a values file with a cell 2 is refused (`not 0 or "
            "1`), a 0/1 file is written. Got bad ok=" + std::string(sb.ok ? "1" : "0") + " `" +
                sb.stderr_text + "`, good ok=" + std::string(sg.ok ? "1" : "0") + " `" +
                sg.stderr_text + "`");
    fs::remove(outp, fec);
    fs::remove(vals, fec);
  }

  // ---- a --keepif fit whose `if` names the mask (review 2026-10-14) -----------
  // xtreg,fe and a TS regress replay their `if` as --keepif. Under `if e(sample)`
  // that expression reads the PREVIOUS fit's mask cells, which the new install
  // overwrites, so every replay must read the fit's own frame: the candidate
  // aliased the mask-less base (lincom/test r(111) `unknown variable
  // __piet_esample__`) and predict replayed on the head (`[no_obs]`, _rc 1).
  // Stata serves all of them; the reference is the same fit under a generated
  // copy of the marker (`if es`).
  {
    const std::string wd = (root / "t2_keepifmask").u8string();
    fs::create_directories(wd, ec);
    piet::SessionState session(wd);
    for (const char* s0 : kEsampleFrame) (void)runLine(s0, session);
    (void)runLine("xtset id t", session);
    (void)runLine("quietly regress y x1 x2 z1 if id<=7", session);
    const LineResult xf = runLine("quietly xtreg y x1 x2 if e(sample), fe", session);
    const auto bX = readNumber("_b[x1]", session);
    const CountRes cX = countIf("e(sample)", session);
    const auto nX = readNumber("e(N)", session);
    const LineResult xl = runLine("lincom x1", session);
    const auto xlE = readNumber("r(estimate)", session);
    const LineResult xt = runLine("test x2", session);
    const auto xtF = readNumber("r(F)", session);
    (void)runLine("quietly regress y x1 x2 z1 if id<=7", session);
    (void)runLine("generate byte es = e(sample)", session);
    (void)runLine("quietly xtreg y x1 x2 if es, fe", session);
    const auto bRef = readNumber("_b[x1]", session);
    (void)runLine("quietly test x2", session);
    const auto fRef = readNumber("r(F)", session);
    auto close9 = [](std::optional<double> a, std::optional<double> b) {
      return a.has_value() && b.has_value() &&
             std::fabs(*a - *b) <= 1e-9 * std::max(1.0, std::fabs(*b));
    };
    w2check(xf.rc == "0" && nX.has_value() && near(cX.n, *nX) && close9(bX, bRef) &&
                xl.rc == "0" && close9(xlE, bX) && xt.rc == "0" && close9(xtF, fRef),
            "W2 esample T2 `xtreg y x1 x2 if e(sample), fe` after `regress y x1 x2 z1 if "
            "id<=7`: count == e(N), _b[x1] equals the `if es` reference, `lincom x1` is _b[x1] "
            "and `test x2` is the reference F. The candidate: lincom/test r(111) `unknown "
            "variable __piet_esample__`. Got fit rc " + xf.rc + " e(N) " + num(nX) + " " +
                show(cX) + " _b " + num(bX) + "/" + num(bRef) + ", lincom " + xl.rc + " `" +
                xl.msg + "` " + num(xlE) + ", test " + xt.rc + " `" + xt.msg + "` " + num(xtF) +
                "/" + num(fRef));

    (void)runLine("quietly regress y x1 x2 if id<=7", session);
    const LineResult tf = runLine("quietly regress y L.x1 x2 if e(sample)", session);
    const auto bT = readNumber("_b[L.x1]", session);
    const auto nT = readNumber("e(N)", session);
    const CountRes cT = countIf("e(sample)", session);
    const LineResult tl = runLine("lincom L.x1", session);
    const auto tlE = readNumber("r(estimate)", session);
    const LineResult tp = runLine("capture noisily predict double p1", session);
    const auto tpRc = readNumber("_rc", session);
    const CountRes tpN = countIf("!missing(p1)", session);
    (void)runLine("quietly summarize p1 if e(sample)", session);
    const auto pMean = readNumber("r(mean)", session);
    (void)runLine("quietly summarize y if e(sample)", session);
    const auto yMean = readNumber("r(mean)", session);
    w2check(tf.rc == "0" && nT.has_value() && near(cT.n, *nT) && tl.rc == "0" &&
                close9(tlE, bT) && tp.rc == "0" && near(tpRc, 0.0) && near(tpN.n, 49.0) &&
                close9(pMean, yMean),
            "W2 esample T2 `regress y L.x1 x2 if e(sample)` after `regress y x1 x2 if id<=7`: "
            "count == e(N), `lincom L.x1` is _b[L.x1], and `predict double p1` is served at _rc "
            "0, filling the 49 rows whose L.x1 and x2 exist (as C6b's moved-head predict), with "
            "the sample mean of y over e(sample) (least squares with a constant). The candidate: "
            "lincom r(111), predict `[no_obs]` with _rc 1. Got fit rc " + tf.rc + " e(N) " +
                num(nT) + " " + show(cT) + ", lincom " + tl.rc + " `" + tl.msg + "` " +
                num(tlE) + "/" + num(bT) + ", predict " + tp.rc + " `" + tp.msg + "` _rc " +
                num(tpRc) + " n " + num(tpN.n) + " mean " + num(pMean) + " vs y " + num(yMean));
  }

  // ---- kUnavailable: a fit with no engine mask is a named r(198) -------------
  // PIET-specific refusal BY DESIGN (SPEC §5.5/§7): Stata answers these; this
  // build's engine-path qreg (vce(robust); the default iid qreg is fitted natively
  // with its mask since w2j_e3_qreg2) has no mask (logit was the unwired fit here until T4
  // wired it, w2_esample.log:1692 H1 -- see testW2EsampleT3), and the cardinal
  // rule is no silent 0/1 and no stale mask from the previous fit -- which IS
  // still on the rows here.
  {
    const std::string wd = (root / "t2_unavail").u8string();
    fs::create_directories(wd, ec);
    piet::SessionState session(wd);
    for (const char* s0 : kEsampleFrame) (void)runLine(s0, session);
    (void)runLine("quietly regress y x1 x2 if id<=5", session);
    const CountRes before = countIf("e(sample)", session);
    (void)runLine("quietly qreg y x1 x2, vce(robust)", session);
    const CountRes c = countIf("e(sample)", session);
    const LineResult d = runLine("display e(sample)", session);
    const LineResult l = runLine("local s = e(sample)", session);
    const LineResult g = runLine("generate byte q = e(sample)", session);
    const LineResult gq = runLine("confirm variable q", session);
    const std::string blk = cmdIfBlock(session);
    const bool named = c.msg.find("e(sample)") != std::string::npos &&
                       c.msg.find("qreg") != std::string::npos;
    w2check(near(before.n, 28.0) && c.rc == "198" && named && d.rc == "198" &&
                d.text.empty() && l.rc == "198" && g.rc == "198" && gq.rc == "111" &&
                blk.rfind("198|", 0) == 0,
            "W2 esample T2 kUnavailable (PIET named refusal by design): after `qreg` (no engine "
            "mask wired) every e(sample) read is r(198) naming the command -- `count if`, "
            "`display` (never `.` at rc 0), `local =`, `generate` (writes nothing), and `if "
            "e(sample) {` is rc 198, not 1 -- while the previous regress's mask (28) is still on "
            "the rows. Main: 198 for count/local/gen but `.` at rc 0 for display. Got before " +
                show(before) + ", count " + show(c) + ", display rc " + d.rc + " `" + d.text +
                "`, local " + l.rc + ", gen " + g.rc + " confirm " + gq.rc + ", if-block " + blk);
    // T3 serves a restored model's mask from `_est_NAME` (w2_esample.log:1414
    // G2, testW2EsampleT3); a model stored WITHOUT one stays refused by name
    // after the restore, never the last fit's rows.
    (void)runLine("quietly qreg y x1 x2 if id<=5, vce(robust)", session);
    (void)runLine("estimates store Q", session);
    const LineResult qv = runLine("confirm variable _est_Q", session);
    (void)runLine("quietly regress y x1 if id>=6", session);
    (void)runLine("estimates restore Q", session);
    const CountRes r = countIf("e(sample)", session);
    w2check(qv.rc == "111" && r.rc == "198" &&
                r.msg.find("estimates restore") != std::string::npos,
            "W2 esample T2/T3 kUnavailable (PIET named refusal by design): results stored from a "
            "fit with no mask write no _est_Q and, after `estimates restore Q`, e(sample) is a "
            "named r(198), never the 31-row mask the last fit left on the data. Got confirm " +
                qv.rc + ", " + show(r));
  }
}

// ============================================================================
// FU-602 -- the braceless branch pair and c_local. Oracle: the golden
// tests/eval/golden/aer_w2/w2_esample_copies.do:35-41, helper program pcnt:
//     capture quietly count if `cond'
//     if _rc c_local `name' = .
//     else c_local `name' = r(N)
// StataBE runs it (22 PREC records, w2_esample_copies.log; P0_fit
// `0|23|23|23|61|` reads two pcnt results). bin/main stopped the do-file at the
// `else` line (unsupported_loop, "`else` is a next-milestone feature") and had
// no `c_local` command, so every arm below fails there.
// ============================================================================
static void testW2EsampleIfElse(const fs::path& root) {
  std::error_code ec;
  const std::string wd = (root / "ifelse").u8string();
  fs::create_directories(wd, ec);
  piet::SessionState session(wd);
  auto runDo = [&](const std::string& src) {
    try {
      const auto res = piet::doexec::executeDoSource(src, session, piet::dispatch);
      return res.stoppedAtLine.has_value() ? std::string("stopped: ") + res.stoppedAtReason.value_or("")
                                           : std::string();
    } catch (const std::exception& e) {
      return std::string("threw: ") + e.what();
    }
  };
  auto loc = [&](const char* n) { return session.eval.locals.get(n, "<unset>"); };

  // (1) top level, both branches.
  std::string st = runDo(
      "local a = 1\n"
      "if `a' == 1 local t1 then\n"
      "else local t1 else\n"
      "if `a' == 2 local t2 then\n"
      "else local t2 else\n");
  w2check(st.empty() && loc("t1") == "then" && loc("t2") == "else",
          "W2 FU-602 top level: `if exp cmd` / `else cmd` runs the if body when true (t1 then) "
          "and the else body when false (t2 else). Main stopped at the else line. Got `" + st +
              "` t1=" + loc("t1") + " t2=" + loc("t2"));

  // (2) inside a foreach body.
  st = runDo(
      "foreach v in 1 2 3 {\n"
      "  if `v' == 2 local r`v' T\n"
      "  else local r`v' E\n"
      "}\n");
  w2check(st.empty() && loc("r1") == "E" && loc("r2") == "T" && loc("r3") == "E",
          "W2 FU-602 foreach body: the pair runs per iteration (r1 E, r2 T, r3 E). Got `" + st +
              "` " + loc("r1") + loc("r2") + loc("r3"));

  // (3) the oracle's pcnt program, both branches, through c_local.
  st = runDo(
      "clear\n"
      "set obs 5\n"
      "generate x = _n\n"
      "capture program drop pcnt\n"
      "program define pcnt\n"
      "  args name cond\n"
      "  capture quietly count if `cond'\n"
      "  if _rc c_local `name' = .\n"
      "  else c_local `name' = r(N)\n"
      "end\n"
      "pcnt good \"x >= 3\"\n"
      "pcnt bad \"nosuchvar == 1\"\n");
  w2check(st.empty() && loc("good") == "3" && loc("bad") == "." && loc("name") == "<unset>",
          "W2 FU-602 program pcnt (w2_esample_copies.do:35-41): `pcnt good \"x >= 3\"` sets the "
          "caller's good = 3 (else branch, c_local = r(N)); `pcnt bad \"nosuchvar == 1\"` sets "
          "bad = . (if branch: count failed, _rc != 0); the program's own args stay private. "
          "Got `" + st + "` good=" + loc("good") + " bad=" + loc("bad") + " name=" + loc("name"));

  // (4) else-if chains and the mixed braced/braceless forms.
  st = runDo(
      "local a = 1\n"
      "if `a' == 2 local c1 first\n"
      "else if `a' == 1 local c1 second\n"
      "else local c1 third\n"
      "if `a' == 3 local c2 first\n"
      "else if `a' == 4 local c2 second\n"
      "else local c2 third\n"
      "if `a' == 0 {\n"
      "  local c3 then\n"
      "}\n"
      "else local c3 else\n"
      "if `a' == 0 local c4 then\n"
      "else {\n"
      "  local c4 block\n"
      "}\n"
      "if `a' == 1 local c5 alone\n");
  w2check(st.empty() && loc("c1") == "second" && loc("c2") == "third" && loc("c3") == "else" &&
              loc("c4") == "block" && loc("c5") == "alone",
          "W2 FU-602 chains: braceless else-if picks the true arm (second) or falls to the last "
          "else (third); a braced if takes a braceless else; a one-line if takes a braced else; "
          "an unpaired one-line if still runs. Got `" + st + "` c1=" + loc("c1") + " c2=" +
              loc("c2") + " c3=" + loc("c3") + " c4=" + loc("c4") + " c5=" + loc("c5"));

  // (5) c_local writes the caller only; outside a program it is a named r(198).
  st = runDo(
      "local q = 7\n"
      "capture program drop setq\n"
      "program define setq\n"
      "  local q = 99\n"
      "  c_local out = `q' + 1\n"
      "  local inner `q'\n"
      "  c_local inner `inner'\n"
      "end\n"
      "setq\n");
  const LineResult top = runLine("capture noisily c_local zz = 5", session);
  const LineResult topRc = runLine("display _rc", session);
  const LineResult stray = runLine("capture noisily else display \"stray\"", session);
  const LineResult strayRc = runLine("display _rc", session);
  w2check(st.empty() && loc("q") == "7" && loc("out") == "100" && loc("inner") == "99" &&
              topRc.text == "198" && top.msg.find("c_local") != std::string::npos &&
              loc("zz") == "<unset>" && strayRc.text == "198" &&
              stray.msg.find("else") != std::string::npos,
          "W2 FU-602 c_local / stray else: inside a program `c_local out = `q' + 1` evaluates in "
          "the program's scope (q 99) and sets the caller's out = 100, leaving the caller's q 7; "
          "at the top level c_local is a named r(198) and sets nothing; an `else` with no `if` "
          "before it is a named r(198), never rc 1. Got `" + st + "` q=" + loc("q") + " out=" +
              loc("out") + " inner=" + loc("inner") + ", top-level rc " + topRc.text + " `" +
              top.msg + "`, stray rc " + strayRc.text + " `" + stray.msg + "`");
}

// ============================================================================
// T3/T4 -- e(sample) through the estimates lifecycle, the ML fits and the
// by-prefix `if` (internal note esample/TASKS.md T3/T4, corrected
// toward the records). Oracle: tests/eval/golden/aer_w2/w2_esample.log section G
// and H1/H2/H7/I1 (StataBE 19.5), and the behaviour of Stata's estimates store/restore,
// which those records show. Main (measured on the frozen main binary,
// 2026-10-15, replaying w2_esample.do): G0b `111|.|`, G0d 18, no `_est_A` in
// ds, G2-G4b r(N) 31 (the count refused, r(N) stale), G5b `1|111|0|`, G6 `.`,
// G7 199, G8 `.`, G9 `0|111|.|`, G12 `.|.|`, H1/H2/H7 count refused, I1 198;
// `by id: count if x1 > scalar(s)` rc 1 and `... > _b[x1]` r(111). So every arm
// below fails there.
// ============================================================================
static void testW2EsampleT3(const fs::path& root) {
  std::printf("[selftest] --- W2 esample T3/T4: _est_<name>, restore/use/esample:/drop/clear, "
              "logit/probit/poisson, by-prefix if (w2_esample.log G, H1/H2/H7, I1) ---\n");
  std::error_code ec;
  const std::string wd = (root / "t3").u8string();
  fs::create_directories(wd, ec);
  piet::SessionState session(wd);
  for (const char* s0 : kEsampleFrame) (void)runLine(s0, session);
  (void)runLine("xtset id t", session);

  // ---- G0-G0e: `estimates store A` writes _est_A ---------------------------
  (void)runLine("quietly describe", session);
  const auto kBefore = readNumber("r(k)", session);
  (void)runLine("quietly regress y x1 x2 if id<=5", session);
  const LineResult g0 = runLine("estimates store A", session);
  const LineResult g0b = runLine("confirm variable _est_A", session);
  const CountRes g0n = countIf("_est_A", session);
  const LineResult g0c = runLine("describe _est_A", session);
  (void)runLine("quietly describe", session);
  const auto kAfter = readNumber("r(k)", session);
  const std::string vl = dsVarlist(session);
  (void)runLine("local labA : variable label _est_A", session);
  (void)runLine("local fmtA : format _est_A", session);
  const std::string lab = session.eval.locals.get("labA", "<unset>") + "|" +
                          session.eval.locals.get("fmtA", "<unset>");
  const bool dsLast = vl.size() >= 6 && vl.compare(vl.size() - 6, 6, "_est_A") == 0 &&
                      vl.find(piet::kEsampleColumn) == std::string::npos;
  w2check(g0.rc == "0" && g0b.rc == "0" && g0n.rc == "0" && near(g0n.n, 28.0) && g0c.rc == "0" &&
              kBefore.has_value() && kAfter.has_value() && *kAfter == *kBefore + 1 && dsLast &&
              lab == "esample() from estimates store|%8.0g",
          "W2 esample T3 G0-G0e: `estimates store A` writes the variable _est_A (28 rows nonzero, "
          "label \"esample() from estimates store\", format %8.0g, last in ds, r(k) +1; "
          "stored under varname _est_A; w2_esample.log:1361 G0b |0|28|, :1375 G0c |0|, "
          ":1382 G0d 19, :1393 G0e `... zb27 _est_A|`). Main: confirm r(111). Got store " +
              g0.rc + ", confirm " + g0b.rc + ", count " + show(g0n) + ", describe " + g0c.rc +
              ", r(k) " + num(kBefore) + "->" + num(kAfter) + ", label `" + lab + "`, ds `" + vl +
              "`");

  // ---- G1/G2: a second fit, then restore A ---------------------------------
  (void)runLine("quietly regress y x1 if id>=6", session);
  const CountRes g1 = countIf("e(sample)", session);
  const LineResult g2r = runLine("estimates restore A", session);
  const CountRes g2 = countIf("e(sample)", session);
  const auto nG2 = readNumber("e(N)", session);
  w2check(near(g1.n, 31.0) && g2r.rc == "0" && g2.rc == "0" && near(g2.n, 28.0) &&
              near(nG2, 28.0),
          "W2 esample T3 G1/G2: after a second fit (31) `estimates restore A` brings A's own "
          "e(sample) back from _est_A: count 28, e(N) 28 (w2_esample.log:1402 G1 |31|31|, :1414 "
          "G2 |0|28|28|...). Main: count refused r(198). Got G1 " + show(g1) + ", restore " +
              g2r.rc + ", G2 " + show(g2) + " e(N) " + num(nG2));

  // ---- G3/G4/G4b: restore of the ACTIVE stored name is a no-op --------------
  (void)runLine("preserve", session);
  (void)runLine("sort y", session);
  (void)runLine("estimates restore A", session);
  const CountRes g3 = countIf("e(sample) & id<=5", session);
  (void)runLine("restore", session);
  (void)runLine("preserve", session);
  (void)runLine("drop if id==1", session);
  (void)runLine("estimates restore A", session);
  const CountRes g4 = countIf("e(sample)", session);
  (void)runLine("restore", session);
  (void)runLine("preserve", session);
  (void)runLine("expand 2 if id==3", session);
  (void)runLine("estimates restore A", session);
  const CountRes g4b = countIf("e(sample)", session);
  (void)runLine("restore", session);
  w2check(g3.rc == "0" && near(g3.n, 28.0) && g4.rc == "0" && near(g4.n, 22.0) &&
              g4b.rc == "0" && near(g4b.n, 28.0),
          "W2 esample T3 G3/G4/G4b: restoring the results that are already active does nothing "
          "(restoring the already-active results is a no-op), so e(sample) "
          "follows the rows: 28 after `sort y`, 22 after `drop if id==1`, 28 after `expand 2 if "
          "id==3` (w2_esample.log:1430 G3 |0|28|, :1449 G4 |0|22|28|, :1468 G4b |0|28|). Main: "
          "refused. Got G3 " + show(g3) + ", G4 " + show(g4) + ", G4b " + show(g4b));

  // ---- G5/G5b: ereturn post, then store (FU-580) ----------------------------
  (void)runLine("matrix bG5 = (1, 2)", session);
  (void)runLine("matrix colnames bG5 = x1 _cons", session);
  const LineResult g5p = runLine("ereturn post bG5", session);
  const CountRes g5 = countIf("e(sample)", session);
  const LineResult g5s = runLine("estimates store P", session);
  (void)runLine("estimates restore A", session);
  const LineResult g5r = runLine("estimates restore P", session);
  const CountRes g5b = countIf("e(sample)", session);
  w2check(g5p.rc == "0" && near(g5.n, 0.0) && g5s.rc == "301" &&
              g5s.msg == "last estimation results not found, nothing to store" &&
              g5r.rc == "111" && g5b.rc == "0" && near(g5b.n, 28.0),
          "W2 esample T3 G5/G5b (FU-580): after `ereturn post` e(sample) is 0, `estimates store "
          "P` is r(301) \"last estimation results not found, nothing to store\", "
          "`estimates restore A` brings 28 back and `estimates restore P` is r(111) "
          "(w2_esample.log:1487 G5 |0|0|.|, :1507 G5b |301|111|28|). Main: store rc 1. Got post " +
              g5p.rc + ", G5 " + show(g5) + ", store " + g5s.rc + " `" + g5s.msg +
              "`, restore P " + g5r.rc + ", G5b " + show(g5b));

  // ---- G6: estimates save / use carries no e(sample) ------------------------
  {
    const std::string ster = (root / "t3_g6").u8string();
    (void)runLine("estimates restore A", session);
    const LineResult sv = runLine("estimates save \"" + ster + "\", replace", session);
    (void)runLine("quietly regress y x1 if id>=6", session);
    const LineResult us = runLine("estimates use \"" + ster + "\"", session);
    const CountRes g6 = countIf("e(sample)", session);
    const auto nG6 = readNumber("e(N)", session);
    w2check(sv.rc == "0" && us.rc == "0" && g6.rc == "0" && near(g6.n, 0.0) && near(nG6, 28.0),
            "W2 esample T3 G6: results brought back by `estimates use` carry no e(sample): count "
            "0 at rc 0, e(N) 28 (w2_esample.log:1531 G6 |0|0|0|28|). Main: count refused. Got "
            "save " + sv.rc + ", use " + us.rc + ", " + show(g6) + ", e(N) " + num(nG6));
  }

  // ---- G7: estimates esample: ----------------------------------------------
  {
    const LineResult g7 = runLine("estimates esample: y x1 x2 if id<=5", session);
    const CountRes c7 = countIf("e(sample)", session);
    const LineResult again = runLine("estimates esample: y x1 if id<=4", session);
    (void)runLine("estimates esample", session);
    const LineResult who = runLine("display \"`r(who)'|`e(_estimates_sample)'\"", session);
    const LineResult rep = runLine("estimates esample: y x1 if id<=4, replace", session);
    const CountRes c7r = countIf("e(sample)", session);
    w2check(g7.rc == "0" && c7.rc == "0" && near(c7.n, 28.0) && again.rc == "322" &&
                who.text == "user|user" && rep.rc == "0" && near(c7r.n, 22.0),
            "W2 esample T3 G7: `estimates esample: y x1 x2 if id<=5` sets e(sample) to the if "
            "rows with y x1 x2 nonmissing, 28 (w2_esample.log:1542 G7 |0|28|); a second one "
            "without replace is r(322) \"no; e(sample) already set\", `estimates esample` "
            "reports r(who) user, and `, replace` resets it. Main: r(199). Got " + g7.rc + " `" + g7.msg + "`, " + show(c7) +
                ", again " + again.rc + ", who `" + who.text + "`, replace " + rep.rc + " " +
                show(c7r));
  }

  // ---- G8/G8b: drop keeps the active e(sample), takes _est_A ----------------
  (void)runLine("estimates restore A", session);
  const LineResult g8d = runLine("estimates drop A", session);
  const CountRes g8 = countIf("e(sample)", session);
  const LineResult g8b = runLine("confirm variable _est_A", session);
  w2check(g8d.rc == "0" && g8.rc == "0" && near(g8.n, 28.0) && g8b.rc == "111",
          "W2 esample T3 G8/G8b: `estimates drop A` leaves the active e(sample) (28) and drops "
          "_est_A (w2_esample.log:1556 G8 |0|28|, :1564 G8b |111|). Main: count refused. Got "
          "drop " + g8d.rc + ", " + show(g8) + ", confirm " + g8b.rc);

  // ---- G9/G9b: eststo NAME: writes _est_NAME, eststo clear drops it ---------
  const LineResult g9 = runLine("eststo E1: regress y x1 x2 if id<=5", session);
  const LineResult g9v = runLine("confirm variable _est_E1", session);
  const CountRes g9n = countIf("_est_E1", session);
  (void)runLine("eststo clear", session);
  const LineResult g9b = runLine("confirm variable _est_E1", session);
  w2check(g9.rc == "0" && g9v.rc == "0" && near(g9n.n, 28.0) && g9b.rc == "111",
          "W2 esample T3 G9/G9b: `eststo E1: regress ...` writes _est_E1 (28) and `eststo "
          "clear` drops it (w2_esample.log:1596 G9 |0|0|28|, :1606 G9b |111|). Main: confirm "
          "r(111). Got " + g9.rc + " `" + g9.msg + "`, confirm " + g9v.rc + ", " + show(g9n) +
              ", after clear " + g9b.rc);

  // ---- G10: estimates clear keeps the active e(sample) ----------------------
  (void)runLine("quietly regress y x1 x2 if id<=5", session);
  (void)runLine("estimates store B", session);
  const LineResult g10c = runLine("estimates clear", session);
  const CountRes g10 = countIf("e(sample)", session);
  const LineResult g10v = runLine("confirm variable _est_B", session);
  w2check(g10c.rc == "0" && near(g10.n, 28.0) && g10v.rc == "111",
          "W2 esample T3 G10: `estimates clear` keeps the active e(sample) (28) and drops _est_B "
          "(w2_esample.log:1621 G10 |0|28|28|). Got " +
              g10c.rc + ", " + show(g10) + ", confirm _est_B " + g10v.rc);

  // ---- G12 + I1: two stored masks; predict after a restore ------------------
  (void)runLine("quietly regress y x1 x2 if id<=5", session);
  (void)runLine("estimates store C", session);
  (void)runLine("quietly regress y x1 if id>=6", session);
  (void)runLine("estimates store D", session);
  (void)runLine("estimates restore C", session);
  const CountRes nC = countIf("e(sample)", session);
  (void)runLine("estimates restore D", session);
  const CountRes nD = countIf("e(sample)", session);
  (void)runLine("estimates restore C", session);
  const LineResult i1 = runLine("predict double pI1 if e(sample), xb", session);
  const CountRes i1n = countIf("pI1<.", session);
  w2check(near(nC.n, 28.0) && near(nD.n, 31.0) && i1.rc == "0" && near(i1n.n, 28.0),
          "W2 esample T3 G12/I1: two stored results restore their own masks, 28 then 31, and "
          "`predict ... if e(sample)` after a restore fills the restored rows, 28 "
          "(w2_esample.log:1659 G12 |28|31|, :2005 I1 |0|28|). Main: `.|.`, predict r(198). Got " +
              show(nC) + " / " + show(nD) + ", predict " + i1.rc + " `" + i1.msg + "` " +
              show(i1n));

  // ---- stale: restore when _est_NAME left the data --------------------------
  (void)runLine("drop _est_C", session);
  (void)runLine("quietly regress y x1", session);
  (void)runLine("estimates restore C", session);
  const CountRes st = countIf("e(sample)", session);
  w2check(st.rc == "198" && st.msg.find("_est_C") != std::string::npos,
          "W2 esample T3 (PIET named refusal by design -- no StataBE arm drops _est_NAME before a "
          "restore): with _est_C gone, e(sample) after `estimates restore C` is a named r(198) "
          "naming _est_C, never the previous fit's rows. Got " + show(st));

  // ---- T4: logit/probit/poisson set e(sample) --------------------------------
  {
    const LineResult h1 = runLine("logit d x1 x2", session);
    const CountRes c1 = countIf("e(sample)", session);
    const auto n1 = readNumber("e(N)", session);
    const LineResult h2 = runLine("probit d x1 x2 if id<=8", session);
    const CountRes c2 = countIf("e(sample)", session);
    const auto n2 = readNumber("e(N)", session);
    const LineResult h7 = runLine("poisson d x1 x2", session);
    const CountRes c7 = countIf("e(sample)", session);
    const auto n7 = readNumber("e(N)", session);
    w2check(h1.rc == "0" && near(c1.n, 60.0) && near(n1, 60.0) && h2.rc == "0" &&
                near(c2.n, 47.0) && near(n2, 47.0) && h7.rc == "0" && near(c7.n, 60.0) &&
                near(n7, 60.0),
            "W2 esample T4 H1/H2/H7: logit, probit with `if` and poisson set e(sample): count == "
            "e(N) = 60, 47, 60 (w2_esample.log:1692 H1 |0|60|60|, :1725 H2 |0|47|47|, :1928 H7 "
            "|0|60|60|). Main: count refused r(198). Got logit " + h1.rc + " " + show(c1) + " " +
                num(n1) + ", probit " + h2.rc + " " + show(c2) + " " + num(n2) + ", poisson " +
                h7.rc + " " + show(c7) + " " + num(n7));
  }

  // ---- T4: the by-prefix `if` takes the row rewrite (class: scalar()/_b[]) ---
  {
    (void)runLine("quietly regress y x1 x2", session);
    (void)runLine("scalar s6 = 6", session);
    (void)runLine("quietly count if x1 > scalar(s6) & id==11", session);
    const auto wantS = readNumber("r(N)", session);
    const LineResult bs = runLine("by id: count if x1 > scalar(s6)", session);
    const auto gotS = readNumber("r(N)", session);
    (void)runLine("quietly count if x1 > _b[x1]*3 & id==11", session);
    const auto wantB = readNumber("r(N)", session);
    const LineResult bb = runLine("by id: count if x1 > _b[x1]*3", session);
    const auto gotB = readNumber("r(N)", session);
    const LineResult br = runLine("by id: regress y x1 if x1 > _b[x1]", session);
    w2check(bs.rc == "0" && wantS.has_value() && near(gotS, *wantS) && bb.rc == "0" &&
                wantB.has_value() && near(gotB, *wantB) && br.rc == "198",
            "W2 esample T4 by-prefix: `by id: count if x1 > scalar(s6)` and `... if x1 > "
            "_b[x1]*3` resolve scalar()/_b[] like every other `if` (r(N) = the last group's, "
            "id 11), and a by-prefixed regress whose `if` reads e()/_b[] is a named r(198). Main: "
            "rc 1 `function scalar() is not supported` / r(111) `unknown variable _b`. Got " +
                bs.rc + " `" + bs.msg + "` " + num(gotS) + " (want " + num(wantS) + "), " + bb.rc +
                " `" + bb.msg + "` " + num(gotB) + " (want " + num(wantB) + "), regress " + br.rc);
  }

  // ---- T4: the by-prefix generate/replace route takes the same row rewrite --
  // (review 2026-10-15: the class fix above moved only the summarize/count
  // driver; `by g: gen ... if x > scalar(s)` stayed rc 1 and `... > _b[x]`
  // r(111) while the line without `by` ran.) Each `by id:` line is compared with
  // its own un-prefixed twin: generate/replace do not read e(), so the groups
  // change nothing. `gzs` is also a stored scalar: the target name is not
  // rewritten (Stata creates the variable; the scalar is untouched).
  {
    (void)runLine("quietly regress y x1 x2", session);
    (void)runLine("scalar s6 = 6", session);
    (void)runLine("scalar gzs = 3", session);
    (void)runLine("quietly generate gp0 = 1 if x1 > scalar(s6)", session);
    const LineResult g1 = runLine("by id: generate gp1 = 1 if x1 > scalar(s6)", session);
    const CountRes g1d = countIf("gp1 != gp0", session);
    const CountRes g1n = countIf("gp1 == 1", session);
    (void)runLine("quietly generate gp2 = x1 - _b[x1] if x1 > _b[x1]*3", session);
    const LineResult g2 = runLine("by id: generate gp3 = x1 - _b[x1] if x1 > _b[x1]*3", session);
    const CountRes g2d = countIf("gp3 != gp2", session);
    const CountRes g2n = countIf("gp3 < .", session);
    (void)runLine("quietly replace gp0 = scalar(s6) if x1 > _b[x1]*3", session);
    const LineResult r1 = runLine("by id: replace gp1 = scalar(s6) if x1 > _b[x1]*3", session);
    const CountRes r1d = countIf("gp1 != gp0", session);
    const LineResult zs = runLine("by id: generate gzs = scalar(s6)", session);
    const CountRes zsn = countIf("gzs == 6", session);
    const auto zsv = readNumber("scalar(gzs)", session);
    (void)runLine("quietly count", session);
    const auto nAll = readNumber("r(N)", session);
    w2check(g1.rc == "0" && g1d.rc == "0" && near(g1d.n, 0.0) && g1n.n.value_or(0) > 0 &&
                g2.rc == "0" && g2d.rc == "0" && near(g2d.n, 0.0) && g2n.n.value_or(0) > 0 &&
                r1.rc == "0" && r1d.rc == "0" && near(r1d.n, 0.0) && zs.rc == "0" &&
                nAll.has_value() && near(zsn.n, *nAll) && near(zsv, 3.0),
            "W2 esample T4 by-prefix generate/replace: `by id: generate ... if x1 > scalar(s6)`, "
            "`... = x1 - _b[x1] if x1 > _b[x1]*3` and `by id: replace ... = scalar(s6) if ...` "
            "equal their un-prefixed twins row for row, and `by id: generate gzs = ...` with a "
            "scalar gzs creates the variable (scalar kept 3). Branch before this round: rc 1 "
            "`function scalar() is not supported` / r(111) `unknown variable _b`. Got gen " +
                g1.rc + " `" + g1.msg + "` diff " + show(g1d) + " n " + show(g1n) + "; gen _b " +
                g2.rc + " `" + g2.msg + "` diff " + show(g2d) + " n " + show(g2n) + "; replace " +
                r1.rc + " `" + r1.msg + "` diff " + show(r1d) + "; gzs " + zs.rc + " `" + zs.msg +
                "` " + show(zsn) + " of " + num(nAll) + ", scalar " + num(zsv));
  }

  // ---- T4: no ML install from a Σw row count (fweight) -----------------------
  // (review 2026-10-15, probe rv2/fw2.do.) The ML mask is the feols admission
  // set, installed only when the ML fit's own ROW count equals it. Under
  // fweight logit's n_obs is Σw and the reply has no n_rows: here x1==1 only
  // with d==1 (2 rows, w=1), the screen drops them, and Σw over the 6 rows
  // left (1+1+2+2+1+1 = 8) equals the feols set's 8 rows, so the superset was
  // installed and `count if e(sample)` said 8. Stata: 6 rows. PIET answers
  // the named r(198) (the main binary's answer for every ML fit) or 6, never 8.
  {
    const std::string wd2 = (root / "t4fw").u8string();
    fs::create_directories(wd2, ec);
    piet::SessionState s2(wd2);
    (void)piet::doexec::executeDoSource(
        "input d x1 x2 w cl\n"
        "1 1 0.3 1 1\n"
        "1 1 1.2 1 2\n"
        "0 0 0.5 1 1\n"
        "1 0 2.1 1 2\n"
        "0 0 1.7 2 3\n"
        "1 0 0.2 2 3\n"
        "0 0 0.9 1 4\n"
        "1 0 1.4 1 4\n"
        "end\n",
        s2, piet::dispatch);
    const LineResult fl = runLine("logit d x1 x2 [fweight=w], vce(cluster cl)", s2);
    const auto fN = readNumber("e(N)", s2);
    const CountRes fc = countIf("e(sample)", s2);
    w2check(fl.rc == "0" && near(fN, 8.0) &&
                (fc.rc == "198" || (fc.rc == "0" && near(fc.n, 6.0))),
            "W2 esample T4 ML fweight: after `logit d x1 x2 [fweight=w], vce(cluster cl)` with "
            "a perfect predictor screened out (e(N) = Σw = 8 over 6 rows), e(sample) is the "
            "named r(198) or the 6 rows, never the 8-row feols superset. Branch before this "
            "round: count 8. Got logit " + fl.rc + " `" + fl.msg + "` e(N) " + num(fN) + ", " +
                show(fc));
  }
}

void testW2Esample() {
  std::printf("[selftest] --- W2 esample T0: estimates restore (w2_cmdtriage.log C, "
              "w2_esample.log G2/G12, BUG-636) ---\n");
  std::error_code ec;
  const fs::path root = fs::temp_directory_path() / "piet_native_selftest_w2_esample";
  fs::remove_all(root, ec);
  fs::create_directories(root, ec);

  // ---- w2_cmdtriage.log section C, the four restore arms ------------------
  {
    const std::string wd = (root / "triage").u8string();
    fs::create_directories(wd, ec);
    piet::SessionState session(wd);
    for (const char* s0 : kTriageFrame) (void)runLine(s0, session);
    (void)runLine("quietly regress y a", session);
    LineResult s1 = runLine("estimates sto m1", session);
    (void)runLine("quietly regress y a b", session);
    LineResult s2 = runLine("est sto m2", session);
    w2check(s1.rc == "0" && s2.rc == "0",
            "W2 esample T0: `estimates sto m1` / `est sto m2` are rc 0 (w2_cmdtriage.log "
            "estimates_sto/est_sto). Got " + s1.rc + " `" + s1.msg + "` / " + s2.rc + " `" +
                s2.msg + "`");
    const auto dfBefore = readNumber("e(df_m)", session);
    const auto bBefore = readNumber("_b[a]", session);
    w2check(near(dfBefore, 2.0) && near(bBefore, .4875776397515528),
            "W2 esample T0: before the restore e(df_m)=2, _b[a]=.4875776397515528 "
            "(w2_cmdtriage.log before_restore). Got " + num(dfBefore) + " " + num(bBefore));
    LineResult rs = runLine("estimates restore m1", session);
    w2check(rs.rc == "0" && rs.text == "(results m1 are active now)",
            "W2 esample T0: `estimates restore m1` is rc 0 and prints exactly \"(results m1 are "
            "active now)\" (w2_cmdtriage.log estimates_restore). Got rc " + rs.rc + " `" +
                rs.msg + "` text `" + rs.text + "`");
    const auto dfAfter = readNumber("e(df_m)", session);
    const auto bAfter = readNumber("_b[a]", session);
    w2check(near(dfAfter, 1.0) && near(bAfter, .4848484848484848),
            "W2 esample T0: after `estimates restore m1` e(df_m)=1, _b[a]=.4848484848484848 -- "
            "m1's, not the last fit's (w2_cmdtriage.log after_restore). Got " + num(dfAfter) +
                " " + num(bAfter));
    LineResult bare = runLine("capture noisily est restore", session);
    LineResult bareRc = runLine("display _rc", session);
    w2check(bare.msg == "estimates __000000 not found" && bareRc.text == "111",
            "W2 esample T0: bare `est restore` is r(111) \"estimates __000000 not found\" "
            "(w2_cmdtriage.log est_restore_bare). Got _rc " + bareRc.text + " `" + bare.msg + "`");
    LineResult miss = runLine("capture noisily estimates restore nosuch", session);
    LineResult missRc = runLine("display _rc", session);
    w2check(miss.msg == "estimation result nosuch not found" && missRc.text == "111",
            "W2 esample T0: `estimates restore nosuch` is r(111) \"estimation result nosuch not "
            "found\" (w2_cmdtriage.log estimates_restore_missing). Got _rc " + missRc.text +
                " `" + miss.msg + "`");
  }

  // ---- w2_esample.log G2 and G12 (the e(N)/_b half; counts are T3) --------
  {
    const std::string wd = (root / "esample").u8string();
    fs::create_directories(wd, ec);
    piet::SessionState session(wd);
    for (const char* s0 : kEsampleFrame) (void)runLine(s0, session);
    (void)runLine("quietly regress y x1 x2 if id<=5", session);
    (void)runLine("estimates store A", session);
    (void)runLine("quietly regress y x1 if id>=6", session);
    const auto nG1 = readNumber("e(N)", session);
    LineResult g2 = runLine("estimates restore A", session);
    const auto nG2 = readNumber("e(N)", session);
    const auto bG2 = readNumber("_b[x1]", session);
    w2check(near(nG1, 31.0) && g2.rc == "0" && g2.text == "(results A are active now)" &&
                near(nG2, 28.0) && near(bG2, 2.081516065514025),
            "W2 esample T0: G1 e(N)=31, then `estimates restore A` is rc 0 with e(N)=28 and "
            "_b[x1]=2.081516065514025 (w2_esample.log G1_second_fit_before_restore, G2_restore). "
            "Got G1 " + num(nG1) + ", rc " + g2.rc + " `" + g2.msg + g2.text + "`, e(N) " +
                num(nG2) + ", _b[x1] " + num(bG2));

    (void)runLine("quietly regress y x1 x2 if id<=5", session);
    (void)runLine("estimates store C", session);
    (void)runLine("quietly regress y x1 if id>=6", session);
    (void)runLine("estimates store D", session);

    // Stored-table outputs, taken BEFORE any restore of C/D: a restore must not
    // rewrite what `estimates dir` / `esttab` / `estimates table` render.
    LineResult dir0 = runLine("estimates dir", session);
    LineResult tab0 = runLine("estimates table C D", session);
    LineResult est0 = runLine("esttab C D", session);
    w2check(dir0.rc == "0" && dir0.text == "Stored estimation results:\n  A  (y)\n  C  (y)\n  D  (y)",
            "W2 esample T0: `estimates dir` lists A C D in store order with their depvar. Got rc " +
                dir0.rc + " `" + dir0.msg + "` text `" + dir0.text + "`");

    LineResult rc1 = runLine("estimates restore C", session);
    const auto nC = readNumber("e(N)", session);
    LineResult rd1 = runLine("estimates restore D", session);
    const auto nD = readNumber("e(N)", session);
    w2check(rc1.text == "(results C are active now)" && rd1.text == "(results D are active now)" &&
                near(nC, 28.0) && near(nD, 31.0),
            "W2 esample T0: two stored models each restore their OWN results: e(N) 28 after "
            "`estimates restore C`, 31 after `estimates restore D` (w2_esample.log "
            "G12_two_stored_masks records e(sample) counts 28|31; e(N) is the T0 half). Got " +
                num(nC) + " / " + num(nD) + " `" + rc1.text + "` `" + rd1.text + "`");

    LineResult dir1 = runLine("estimates dir", session);
    LineResult tab1 = runLine("estimates table C D", session);
    LineResult est1 = runLine("esttab C D", session);
    w2check(tab0.rc == "0" && est0.rc == "0" && dir1.dump == dir0.dump &&
                tab1.dump == tab0.dump && est1.dump == est0.dump,
            "W2 esample T0: `estimates dir`, `estimates table C D` and `esttab C D` are "
            "byte-identical before and after restoring C and D. Got rc " + tab0.rc + "/" +
                est0.rc + ", dir " + std::to_string(dir1.dump == dir0.dump) + " table " +
                std::to_string(tab1.dump == tab0.dump) + " esttab " +
                std::to_string(est1.dump == est0.dump));

    // Control for BUG-636's guard: an in-session restore keeps its (pinned)
    // fit data, so the re-fitting verbs still answer.
    (void)runLine("estimates restore C", session);
    LineResult t = runLine("test x1", session);
    LineResult l = runLine("lincom x1", session);
    w2check(t.rc == "0" && l.rc == "0",
            "W2 esample T0 (BUG-636 control): after an in-session `estimates restore C`, `test "
            "x1` and `lincom x1` still answer rc 0 (the guard must not fire on pinned data). "
            "Got " + t.rc + " `" + t.msg + "` / " + l.rc + " `" + l.msg + "`");
  }

  // ---- BUG-636: results whose working frame is gone ------------------------
  // `estimates use` restores the data path the SAVING process recorded; that
  // process's scratch is gone by the time another session reads the file.
  {
    const std::string wd1 = (root / "saver").u8string();
    const std::string wd2 = (root / "reader").u8string();
    const std::string ster = (root / "m1").u8string();
    fs::create_directories(wd1, ec);
    fs::create_directories(wd2, ec);
    {
      piet::SessionState saver(wd1);
      for (const char* s0 : kTriageFrame) (void)runLine(s0, saver);
      (void)runLine("generate double k = 1", saver);  // the fit's head is scratch, not a user file
      (void)runLine("quietly regress y a", saver);
      (void)runLine("estimates save \"" + ster + "\", replace", saver);
    }
    fs::remove_all(wd1, ec);  // the saving session's scratch, reclaimed

    piet::SessionState session(wd2);
    for (const char* s0 : kTriageFrame) (void)runLine(s0, session);
    (void)runLine("quietly regress y a b", session);
    LineResult use = runLine("estimates use \"" + ster + "\"", session);
    const auto bUse = readNumber("_b[a]", session);
    w2check(use.rc == "0" && near(bUse, .4848484848484848),
            "W2 esample T0 (BUG-636): `estimates use` of the saved m1 is rc 0 and _b[a] is m1's "
            ".4848484848484848. Got rc " + use.rc + " `" + use.msg + "`, _b[a] " + num(bUse));

    auto refusedByName = [](const LineResult& r) {
      return r.rc == "198" && r.msg.find("no longer available") != std::string::npos &&
             r.msg.find("[data_io]") == std::string::npos;
    };
    for (const char* cmd : {"test a", "testparm a", "lincom a", "nlcom _b[a]", "predict double p1"}) {
      LineResult r = runLine(cmd, session);
      w2check(refusedByName(r),
              std::string("W2 esample T0 (BUG-636, PIET named refusal by design -- no Stata "
              "arm: Stata keeps the data in memory): `") + cmd + "` after `estimates use` of a "
              "file whose fit data is gone is a named r(198) refusal, never `[data_io] cannot "
              "open ...` at rc 1. Got rc " + r.rc + " `" + r.msg + "`");
    }

    // The same results stored and restored carry the same dead path.
    (void)runLine("estimates store u1", session);
    (void)runLine("quietly regress y b", session);
    LineResult rs = runLine("estimates restore u1", session);
    LineResult t = runLine("test a", session);
    LineResult p = runLine("predict double p2", session);
    w2check(rs.rc == "0" && refusedByName(t) && refusedByName(p),
            "W2 esample T0 (BUG-636, PIET named refusal by design -- no Stata arm): after "
            "`estimates store u1` + a new fit + `estimates restore u1`, `test a` and `predict` "
            "refuse by name (r(198)). Got restore " + rs.rc +
                ", test " + t.rc + " `" + t.msg + "`, predict " + p.rc + " `" + p.msg + "`");

    // A fresh fit clears it: the live model's data is the live head.
    (void)runLine("quietly regress y a", session);
    LineResult t2 = runLine("test a", session);
    w2check(t2.rc == "0",
            "W2 esample T0 (BUG-636 control): after an ordinary in-session `regress y a`, `test "
            "a` is rc 0 -- the guard must not fire on the live head (w2_cmdtriage.log section "
            "C fits and tests in one session). Got " + t2.rc + " `" + t2.msg + "`");
  }

  // ---- BUG-636 control: `estimates use` whose fit data STILL exists --------
  // The saving session stays alive, so the path the .ster recorded is on disk
  // but is not the reader's head: the guard's exists() branch must let every
  // re-fitting verb through (BUG-470's round trip: e() restored, verbs answer).
  {
    const std::string wd1 = (root / "saver2").u8string();
    const std::string wd2 = (root / "reader2").u8string();
    const std::string ster = (root / "m2").u8string();
    fs::create_directories(wd1, ec);
    fs::create_directories(wd2, ec);
    piet::SessionState saver(wd1);
    for (const char* s0 : kTriageFrame) (void)runLine(s0, saver);
    (void)runLine("generate double k = 1", saver);
    (void)runLine("quietly regress y a", saver);
    (void)runLine("estimates save \"" + ster + "\", replace", saver);

    piet::SessionState session(wd2);
    for (const char* s0 : kTriageFrame) (void)runLine(s0, session);
    (void)runLine("quietly regress y a b", session);
    LineResult use = runLine("estimates use \"" + ster + "\"", session);
    LineResult l = runLine("lincom a", session);
    const auto est = readNumber("r(estimate)", session);
    LineResult t = runLine("test a", session);
    w2check(use.rc == "0" && l.rc == "0" && near(est, .4848484848484848) && t.rc == "0",
            "W2 esample T0 (BUG-636 control): `lincom a` and `test a` after `estimates use` of a "
            ".ster whose fit data still exists are rc 0, and lincom's r(estimate) is the used "
            "model's _b[a] .4848484848484848 (w2_cmdtriage.log after_restore, same frame and "
            "`regress y a`); the guard fires only when nothing is at the path. Got use " +
                use.rc + " `" + use.msg + "`, lincom " + l.rc + " `" + l.msg + "` r(estimate) " +
                num(est) + ", test " + t.rc + " `" + t.msg + "`");
  }
  testW2EsampleT2(root);
  testW2EsampleBaseOwnership(root);
  testW2EsampleIfElse(root);
  testW2EsampleT3(root);
  fs::remove_all(root, ec);
}
