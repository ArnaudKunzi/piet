// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// W2 lane `esttabnames` -- selftest arms (BUG-552, BUG-553).
//
// Oracles (StataBE 19.5, estout 2.1.4):
//   tests/eval/golden/aer_w2/w2_smalls.log section G -- the stored-estimates
//     NAMELIST: `esttab A_*` / `*` / `A_? B_1` / `.` render the models the log
//     types out, in that column order; `esttab Z_*` is r(111) "estimation
//     result Z_* not found".
//   tests/eval/golden/aer_w2/w2_cmdtriage.log section C -- `estimates sto` and
//     `est sto` are rc 0; after `estimates restore m1` e(df_m) and _b[a] are
//     m1's; bare `est restore` and `estimates restore nosuch` are r(111).
// Every arm fails on the frozen main binary: there `esttab A_*` was rc 1
// "model(s) not found", `est sto`/`estimates sto`/`estimates restore` were
// r(199) "subcommand not supported", and e() stayed on the LAST fit.
#include "selftest_w2.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <system_error>
#include <vector>

#include "piet/dispatcher.h"
#include "piet/eval/errors.h"
#include "piet/session_state.h"
#include "vendor/nlohmann/json.hpp"

namespace {

using ojson = nlohmann::ordered_json;
namespace fs = std::filesystem;

// w2_smalls.do's frame (the columns section G reads), verbatim.
const char* const kSmallsFrame[] = {
    "clear",
    "set obs 12",
    "generate id = ceil(_n/3)",
    "generate t  = mod(_n-1,3)+1",
    "generate x  = mod(_n*7,11)",
    "generate x2 = mod(_n*3,5)",
    "generate y  = 2*x - x2 + mod(_n,4)",
};

// w2_cmdtriage.do's frame (the columns section C reads), verbatim.
const char* const kTriageFrame[] = {
    "clear",
    "set obs 12",
    "generate double a = _n*1.5",
    "generate double b = mod(_n*5, 7)",
    "generate double y = 1 + 0.5*a - 0.25*b + mod(_n, 4)",
};

struct LineResult {
  std::string rc = "0";
  std::string msg;
  std::vector<std::string> names;  // the estimates_table block's model names
  bool sawTable = false;
  std::string text;  // every display block's text, newline-joined
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
      if (t == "estimates_table" && b.contains("names") && b["names"].is_array()) {
        r.sawTable = true;
        for (const auto& n : b["names"]) {
          if (n.is_string()) r.names.push_back(n.get<std::string>());
        }
      }
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

std::string joinNames(const std::vector<std::string>& v) {
  std::string s;
  for (const auto& n : v) s += (s.empty() ? "" : " ") + n;
  return s;
}

std::optional<double> displayedNumber(const LineResult& r) {
  try {
    size_t pos = 0;
    std::string t = r.text;
    while (!t.empty() && t.front() == ' ') t.erase(t.begin());
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

std::string readFile(const std::string& p) {
  std::ifstream f(p, std::ios::binary);
  std::ostringstream ss;
  ss << f.rdbuf();
  return ss.str();
}

}  // namespace

void testW2EsttabNames() {
  std::printf("[selftest] --- W2 esttabnames: stored-estimates namelists + estimates restore "
              "(w2_smalls.log G, w2_cmdtriage.log C) ---\n");
  std::error_code ec;
  const std::string workdir =
      (fs::temp_directory_path() / "piet_native_selftest_w2_esttabnames").u8string();
  fs::remove_all(workdir, ec);
  fs::create_directories(workdir, ec);

  // ---- BUG-552: namelist expansion (w2_smalls.log section G) ---------------
  {
    piet::SessionState session(workdir);
    for (const char* s0 : kSmallsFrame) (void)runLine(s0, session);
    for (const char* s0 : {"estimates clear", "quietly regress y x2", "estimates store A_1",
                           "quietly regress y x2 t", "estimates store A_2",
                           "quietly regress y t", "estimates store B_1"}) {
      (void)runLine(s0, session);
    }

    struct Arm {
      const char* cmd;
      const char* want;
      const char* oracle;
    };
    // The log's tables name the models by their COLUMNS: esttab_glob has x2|x2,t
    // (A_1 A_2), esttab_star and esttab_qmark have x2|x2,t|t (A_1 A_2 B_1),
    // esttab_dot has t alone (B_1, the active results, stored last).
    const Arm arms[] = {
        {"esttab A_*, b(3) se(3) nostar", "A_1 A_2", "esttab_glob"},
        {"esttab *, b(3) se(3) nostar", "A_1 A_2 B_1", "esttab_star"},
        {"esttab A_? B_1, b(3) se(3) nostar", "A_1 A_2 B_1", "esttab_qmark"},
        {"esttab ., b(3) se(3) nostar", "B_1", "esttab_dot"},
    };
    for (const auto& a : arms) {
      LineResult r = runLine(a.cmd, session);
      w2check(r.rc == "0" && r.sawTable && joinNames(r.names) == a.want,
              std::string("W2 esttabnames (BUG-552): `") + a.cmd + "` is rc 0 with columns `" +
                  a.want + "` in store order (w2_smalls.log " + a.oracle + "); main answered rc 1 "
                  "\"model(s) not found in estimates store\". Got rc " + r.rc + " columns `" +
                  joinNames(r.names) + "` `" + r.msg + "`");
    }

    // The using-file half renders the same columns: A_? B_1 puts A_2's t
    // (2.725) left of B_1's t (2.500) on the `t` row, exactly as the log types.
    {
      const std::string out = workdir + "/w2s_q.txt";
      LineResult r = runLine("esttab A_? B_1 using \"" + out + "\", replace b(3) se(3) nostar",
                             session);
      const std::string body = readFile(out);
      const size_t p1 = body.find("2.725"), p2 = body.find("2.500"), p0 = body.find("0.045");
      w2check(r.rc == "0" && p0 != std::string::npos && p1 != std::string::npos &&
                  p2 != std::string::npos && p0 < p1 && p1 < p2 &&
                  body.find("(3)") != std::string::npos,
              "W2 esttabnames (BUG-552): `esttab A_? B_1 using f` writes three columns, A_1 "
              "(x2 0.045) then A_2 (t 2.725) then B_1 (t 2.500) (w2_smalls.log esttab_qmark "
              "table). Got rc " + r.rc + " `" + r.msg + "`, file:\n" + body);
    }

    // No match: r(111), Stata's own message, on EVERY namelist consumer.
    for (const char* cmd : {"esttab Z_*", "estout Z_*", "estimates table Z_*",
                            "estimates dir Z_*", "estimates drop Z_*",
                            "estimates restore Z_*"}) {
      LineResult r = runLine(cmd, session);
      w2check(r.rc == "111" && r.msg == "estimation result Z_* not found",
              std::string("W2 esttabnames (BUG-552): `") + cmd + "` is r(111) \"estimation result "
                  "Z_* not found\" (w2_smalls.log esttab_glob_nomatch; the one shared expansion "
                  "serves every consumer). Got rc " + r.rc + " `" + r.msg + "`");
    }

    // estimates table / dir take the namelist too.
    {
      LineResult r = runLine("estimates table A_*", session);
      w2check(r.rc == "0",
              "W2 esttabnames (BUG-552): `estimates table A_*` expands like esttab (rc 0). Got rc " +
                  r.rc + " `" + r.msg + "`");
      LineResult d = runLine("estimates dir A_?", session);
      w2check(d.rc == "0" && d.text.find("A_1") != std::string::npos &&
                  d.text.find("A_2") != std::string::npos &&
                  d.text.find("B_1") == std::string::npos,
              "W2 esttabnames (BUG-552): `estimates dir A_?` lists A_1 and A_2 only. Got rc " +
                  d.rc + " `" + d.text + "`");
    }

    // A refit replaces e(): `.` is then the unstored active results, not B_1.
    {
      (void)runLine("quietly regress y x2", session);
      LineResult r = runLine("esttab .", session);
      w2check(r.rc == "0" && joinNames(r.names) == ".",
              "W2 esttabnames (BUG-552): after a new fit `esttab .` is the ACTIVE unstored "
              "results (e(_estimates_name) goes with e()), not the stale B_1. Got rc " + r.rc +
                  " columns `" + joinNames(r.names) + "`");
    }

    // drop takes the namelist: A_* goes, B_1 stays.
    {
      LineResult r = runLine("estimates drop A_*", session);
      w2check(r.rc == "0" && session.estimatesGet("A_1") == nullptr &&
                  session.estimatesGet("A_2") == nullptr && session.estimatesGet("B_1") != nullptr,
              "W2 esttabnames (BUG-552): `estimates drop A_*` drops A_1 and A_2 and keeps B_1 "
              "(drop takes a namelist). Got rc " + r.rc + " `" + r.msg + "`");
    }
  }

  // ---- BUG-553: estimates sto / restore (w2_cmdtriage.log section C) -------
  {
    piet::SessionState session(workdir);
    for (const char* s0 : kTriageFrame) (void)runLine(s0, session);
    (void)runLine("quietly regress y a", session);
    LineResult sto1 = runLine("estimates sto m1", session);
    w2check(sto1.rc == "0" && session.estimatesGet("m1") != nullptr,
            "W2 esttabnames (BUG-553): `estimates sto m1` is rc 0 and stores m1 "
            "(w2_cmdtriage.log estimates_sto; main r(199)). Got rc " + sto1.rc + " `" + sto1.msg +
                "`");
    (void)runLine("quietly regress y a b", session);
    LineResult sto2 = runLine("est sto m2", session);
    w2check(sto2.rc == "0" && session.estimatesGet("m2") != nullptr,
            "W2 esttabnames (BUG-553): `est sto m2` is rc 0 (w2_cmdtriage.log est_sto; main "
            "r(199)). Got rc " + sto2.rc + " `" + sto2.msg + "`");

    // before_restore: e(df_m)=2, _b[a]=.4875776397515528 (the live m2 fit).
    {
      LineResult d = runLine("display e(df_m)", session);
      // Read _b[a] through a scalar rather than the oracle's literal `display %18.0g
      // _b[a]`. The scalar route was chosen while display_hardening.cpp resolveBRef
      // still substituted `_b[]` as 9-digit `%.9g` text (BUG-554); it now
      // substitutes at full precision, so both spellings read the same value. This
      // check is about WHICH fit is active; the display-precision half of the arm
      // is pinned by W2c(postest) and probe BUG-554.do.
      (void)runLine("scalar __w2esttab_ba = _b[a]", session);
      LineResult b = runLine("display %18.0g __w2esttab_ba", session);
      w2check(near(displayedNumber(d), 2.0) && near(displayedNumber(b), .4875776397515528),
              "W2 esttabnames (BUG-553): before the restore e(df_m)=2 and _b[a]=.4875776397515528 "
              "(w2_cmdtriage.log before_restore). Got `" + d.text + "` `" + b.text + "`");
    }
    LineResult rs = runLine("estimates restore m1", session);
    w2check(rs.rc == "0" && rs.text.find("(results m1 are active now)") != std::string::npos,
            "W2 esttabnames (BUG-553): `estimates restore m1` is rc 0, \"(results m1 are active "
            "now)\" (w2_cmdtriage.log estimates_restore; main r(199)). Got rc " + rs.rc + " `" +
                rs.msg + rs.text + "`");
    // after_restore: e(df_m)=1, _b[a]=.4848484848484848 -- m1's, not m2's.
    {
      LineResult d = runLine("display e(df_m)", session);
      // Read _b[a] through a scalar rather than the oracle's literal `display %18.0g
      // _b[a]`. The scalar route was chosen while display_hardening.cpp resolveBRef
      // still substituted `_b[]` as 9-digit `%.9g` text (BUG-554); it now
      // substitutes at full precision, so both spellings read the same value. This
      // check is about WHICH fit is active; the display-precision half of the arm
      // is pinned by W2c(postest) and probe BUG-554.do.
      (void)runLine("scalar __w2esttab_ba = _b[a]", session);
      LineResult b = runLine("display %18.0g __w2esttab_ba", session);
      w2check(near(displayedNumber(d), 1.0) && near(displayedNumber(b), .4848484848484848),
              "W2 esttabnames (BUG-553): after `estimates restore m1` e(df_m)=1 and "
              "_b[a]=.4848484848484848 (w2_cmdtriage.log after_restore) -- main kept answering "
              "the LAST fit's 2 / .4875776397515528, a silent wrong e(). Got `" + d.text + "` `" +
                  b.text + "`");
    }
    // The restored results carry their store name: `esttab .` is m1.
    {
      LineResult r = runLine("esttab .", session);
      w2check(r.rc == "0" && joinNames(r.names) == "m1",
              "W2 esttabnames (BUG-553): after `estimates restore m1`, `.` expands to m1 "
              "(the active results keep their store name). Got rc " + r.rc + " `" + joinNames(r.names) +
                  "`");
    }
    {
      LineResult r = runLine("est restore", session);
      w2check(r.rc == "111" && r.msg == "estimates __000000 not found",
              "W2 esttabnames (BUG-553): bare `est restore` is r(111) \"estimates __000000 not "
              "found\" (w2_cmdtriage.log est_restore_bare; main r(199)). Got rc " + r.rc + " `" +
                  r.msg + "`");
    }
    {
      LineResult r = runLine("estimates restore nosuch", session);
      w2check(r.rc == "111" && r.msg == "estimation result nosuch not found",
              "W2 esttabnames (BUG-553): `estimates restore nosuch` is r(111) \"estimation result "
              "nosuch not found\" (w2_cmdtriage.log estimates_restore_missing; main r(199)). Got "
              "rc " + r.rc + " `" + r.msg + "`");
    }
    // restore takes exactly one name: a pattern matching two stored names is r(198).
    {
      LineResult r = runLine("estimates restore m*", session);
      w2check(r.rc == "198" && r.msg == "too many names specified",
              "W2 esttabnames (BUG-553): `estimates restore m*` over m1 m2 is r(198) \"too many "
              "names specified\" (restore takes exactly one name). Got rc " + r.rc +
                  " `" + r.msg + "`");
    }
  }

  // ---- BUG-553 (review fix): the active store name is IDENTITY, not a fingerprint
  // Two postings with the same depvar|estimator|N|k (two `estpost summarize`
  // over different variables, or a csdid-style direct e() replacement) used to
  // look like the stored one: `esttab .` rendered the OLD table and
  // `estimates restore` was skipped while printing success. Stata's
  // `ereturn post` drops e(_estimates_name), so `.` is the live posting.
  {
    piet::SessionState session(workdir);
    for (const char* s0 : kTriageFrame) (void)runLine(s0, session);
    (void)runLine("estpost summarize a b", session);
    (void)runLine("eststo S1", session);
    (void)runLine("estpost summarize y b", session);
    LineResult r = runLine("esttab .", session);
    w2check(r.rc == "0" && joinNames(r.names) == ".",
            "W2 esttabnames (BUG-553): after `estpost summarize a b; eststo S1; estpost summarize "
            "y b`, `esttab .` is the live posting (ereturn post clears e(_estimates_name)), not "
            "S1's a/b table. Got rc " + r.rc + " columns `" + joinNames(r.names) + "` `" + r.msg +
                "`");
  }
  {
    piet::SessionState session(workdir);
    for (const char* s0 : kTriageFrame) (void)runLine(s0, session);
    (void)runLine("quietly regress y a", session);
    (void)runLine("estimates store m1", session);
    const std::optional<ojson> stored = session.lastEstBlock;
    // A csdid-shaped replacement: e() block rewritten directly, same
    // depvar/estimator/N/k (the old fingerprint), different coefficient.
    bool mutated = false;
    if (session.lastEstBlock.has_value() && session.lastEstBlock->contains("coeftable") &&
        (*session.lastEstBlock)["coeftable"].is_array() &&
        !(*session.lastEstBlock)["coeftable"].empty() &&
        (*session.lastEstBlock)["coeftable"][0].is_object()) {
      (*session.lastEstBlock)["coeftable"][0]["b"] = 123.25;
      mutated = true;
    }
    LineResult dot = runLine("esttab .", session);
    w2check(mutated && dot.rc == "0" && joinNames(dot.names) == ".",
            "W2 esttabnames (BUG-553): once e() is replaced underneath a stored name (same "
            "fingerprint), `esttab .` is the live results, not m1. Got rc " + dot.rc +
                " columns `" + joinNames(dot.names) + "`");
    LineResult rs = runLine("estimates restore m1", session);
    w2check(rs.rc == "0" && session.lastEstBlock == stored,
            "W2 esttabnames (BUG-553): `estimates restore m1` after a same-fingerprint e() "
            "replacement really restores m1's block (it used to be skipped as a no-op while "
            "printing \"(results m1 are active now)\"). Got rc " + rs.rc + " `" + rs.msg + "`");
  }

  // ---- BUG-553 (review fix): restore brings back the macro-layer e() + pins
  {
    piet::SessionState session(workdir);
    for (const char* s0 : kTriageFrame) (void)runLine(s0, session);
    for (const char* s0 :
         {"quietly regress y a", "estadd local fe \"No\"", "estadd scalar ymean = 7",
          "est sto m1", "quietly regress y a b", "estadd local fe \"Yes\"",
          "estadd scalar ymean = 9", "est sto m2", "estadd local tag \"late\" : m1",
          "est restore m1"}) {
      (void)runLine(s0, session);
    }
    LineResult fe = runLine("display \"`e(fe)'\"", session);
    LineResult ym = runLine("display e(ymean)", session);
    LineResult tag = runLine("display \"`e(tag)'\"", session);
    LineResult df = runLine("display e(df_m)", session);
    w2check(fe.text == "No" && near(displayedNumber(ym), 7.0) && tag.text == "late" &&
                near(displayedNumber(df), 1.0),
            "W2 esttabnames (BUG-553): after `est restore m1`, e(fe)=No and e(ymean)=7 are m1's "
            "estadd postings (not m2's Yes/9, which leaked through the e-stores), e(tag)=late "
            "from the post-store `estadd ...: m1`, e(df_m)=1. Got fe `" + fe.text + "` ymean `" +
                ym.text + "` tag `" + tag.text + "` df_m `" + df.text + "`");

    // The stored model's fit data is pinned against the scratch GC for as long
    // as the model is stored, and a scoped ScratchPin cannot release it.
    const auto it = session.estimatesModels.find("m1");
    std::string dp;
    if (it != session.estimatesModels.end() && it->second && it->second->lastModel.has_value()) {
      dp = it->second->lastModel->dataPath;
    }
    const bool pinnedStored = !dp.empty() && session.scratchPins.count(dp) == 1;
    { piet::SessionState::ScratchPin scoped(session, {dp}); }
    const bool survivesScoped = !dp.empty() && session.scratchPins.count(dp) == 1;
    LineResult pr = runLine("predict double w2p, xb", session);
    (void)runLine("estimates clear", session);
    const bool releasedOnClear = session.scratchPins.count(dp) == 0;
    w2check(pinnedStored && survivesScoped && pr.rc == "0" && releasedOnClear,
            "W2 esttabnames (BUG-553): a stored model's fit data (" + dp + ") is pinned while "
            "stored (" + std::to_string(pinnedStored) + "), survives a scoped ScratchPin's "
            "release (" + std::to_string(survivesScoped) + "), `predict` after the restore is rc "
            "0 (got " + pr.rc + " `" + pr.msg + "`), and `estimates clear` releases it (" +
                std::to_string(releasedOnClear) + ")");
  }
  fs::remove_all(workdir, ec);
}
