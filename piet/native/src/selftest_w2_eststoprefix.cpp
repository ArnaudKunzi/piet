// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// W2 lane `eststoprefix` -- selftest arms (BUG-520, BUG-521).
//
// Oracle: tests/eval/golden/aer_w2/w2_eststo_prefix.log (StataBE 19.5 + SSC
// estout), Part A. Every arm below is primed the way the oracle primes its own:
// `quietly regress y x` first, so a prefix that never runs its command is
// visible -- the model that gets stored then has an `x` row and no `w` row.
//
// The first session REPLAYS Part A line for line (minus the `capture noisily`
// wrappers and the PREC/VAL displays), on the oracle's own synthetic frame. That
// is what lets the auto-name arms assert the oracle's own names -- est2, zz9,
// est10, est1 -- rather than names derived from a rule: the number is "how many
// sets eststo has stored so far, plus one", so it only equals the oracle's when
// the same stores happened before it.
#include "selftest_w2.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>  // getenv/setenv: the WS-B gate leg isolates the settings store
#include <exception>
#include <filesystem>
#include <optional>
#include <string>
#include <system_error>
#include <vector>

#include "piet/dispatcher.h"
#include "piet/doexec.h"
#include "piet/eval/errors.h"
#include "piet/session_state.h"
#include "piet/settings.h"
#include "vendor/nlohmann/json.hpp"

namespace {

using ojson = nlohmann::ordered_json;
namespace fs = std::filesystem;

// w2_eststo_prefix.do's own frame, verbatim: closed-form, no runiform.
const char* const kOracleFrame[] = {
    "clear all",
    "set obs 400",
    "generate id = ceil(_n/10)",
    "generate t = mod(_n-1,10)+1",
    "generate double z = sin(_n*0.37) + cos(id*1.3)",
    "generate double x = 0.6*z + sin(_n*1.7)*0.8 + id/40",
    "generate double w = cos(_n*0.91)",
    "generate double y = 1 + 0.5*x + 0.3*w + sin(_n*2.3) + mod(id,7)/7",
};

// VAL|A1..A6|b_w in the oracle: `regress y w` on that frame.
constexpr double kOracleBw = .3084090989041905;

struct LineResult {
  std::string rc = "0";
  std::string msg;
  bool ranEstimation = false;  // an `estimation` block came back: the inner command RAN
};

// Every JSON read is guarded: a wrong shape is a FAIL with a name, never an
// abort (type_error.306 took the whole selftest binary down once already).
LineResult scanBlocks(const ojson& blocks) {
  LineResult r;
  try {
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
      if (t == "estimation") r.ranEstimation = true;
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

LineResult runLine(const std::string& cmd, piet::SessionState& session) {
  try {
    return scanBlocks(piet::dispatch(cmd, session));
  } catch (const std::exception& e) {
    LineResult r;
    r.rc = "threw";
    r.msg = e.what();
    return r;
  }
}

void loadFrame(piet::SessionState& session) {
  for (const char* s0 : kOracleFrame) (void)runLine(s0, session);
}

// One number out of a stored snapshot's coeftable, or nullopt when the model,
// the row or the field is not there.
std::optional<double> storedCoef(const piet::SessionState& session, const std::string& model,
                                 const std::string& term, const char* field = "b") {
  try {
    const ojson* snap = session.estimatesGet(model);
    if (snap == nullptr || !snap->is_object() || !snap->contains("coeftable")) return std::nullopt;
    const ojson& ct = (*snap)["coeftable"];
    if (!ct.is_array()) return std::nullopt;
    for (const auto& row : ct) {
      if (!row.is_object() || !row.contains("name") || !row["name"].is_string()) continue;
      if (row["name"].get<std::string>() != term) continue;
      if (!row.contains(field) || !row[field].is_number()) return std::nullopt;
      return row[field].get<double>();
    }
  } catch (const std::exception&) {
  }
  return std::nullopt;
}

std::optional<double> storedN(const piet::SessionState& session, const std::string& model) {
  try {
    const ojson* snap = session.estimatesGet(model);
    if (snap == nullptr || !snap->is_object() || !snap->contains("n_obs")) return std::nullopt;
    if (!(*snap)["n_obs"].is_number()) return std::nullopt;
    return (*snap)["n_obs"].get<double>();
  } catch (const std::exception&) {
  }
  return std::nullopt;
}

std::optional<std::string> storedTitle(const piet::SessionState& session,
                                       const std::string& model) {
  try {
    const ojson* snap = session.estimatesGet(model);
    if (snap == nullptr || !snap->is_object() || !snap->contains("title")) return std::nullopt;
    if (!(*snap)["title"].is_string()) return std::nullopt;
    return (*snap)["title"].get<std::string>();
  } catch (const std::exception&) {
  }
  return std::nullopt;
}

bool isStored(const piet::SessionState& session, const std::string& model) {
  return session.estimatesGet(model) != nullptr;
}

// rtol 1e-9: the parity ladder's own rung (tests/eval/TOLERANCE.md).
bool near(const std::optional<double>& got, double want) {
  return got.has_value() && std::fabs(*got - want) <= 1e-9 * std::fabs(want);
}

// "the FRESH `regress y w`, not the primed `regress y x`": has the oracle's
// b[w] and has no x row at all.
bool isFreshYonW(const piet::SessionState& session, const std::string& model) {
  return near(storedCoef(session, model, "w"), kOracleBw) &&
         !storedCoef(session, model, "x").has_value();
}

std::string namesOf(const piet::SessionState& session) {
  std::string s;
  for (const auto& n : session.estimatesNames()) s += (s.empty() ? "" : " ") + n;
  return "[" + s + "]";
}

}  // namespace

void testW2EststoPrefix() {
  std::printf("[selftest] --- W2 eststoprefix: the `eststo` PREFIX (w2_eststo_prefix.log) ---\n");
  const std::string base =
      (fs::temp_directory_path() / "piet_native_selftest_w2_eststoprefix").u8string();
  std::error_code ec;
  fs::remove_all(base, ec);
  const std::string kLog = "w2_eststo_prefix.log ";

  // ======================= the oracle's Part A, replayed ======================
  {
    const std::string wd = base + "/seq";
    fs::create_directories(wd, ec);
    piet::SessionState session(wd);
    loadFrame(session);
    (void)runLine("eststo clear", session);

    // A1 (the oracle's CONTROL, and this replay's first store).
    (void)runLine("quietly regress y x", session);
    (void)runLine("eststo lit1: regress y w", session);

    // ---- A2: bare `eststo:` ----
    // main today: rc 0, NO estimation block, "(stored as regress)" -- the primed
    // y~x model stored under the inner command's first word.
    (void)runLine("quietly regress y x", session);
    {
      const LineResult r = runLine("eststo: regress y w", session);
      w2check(r.rc == "0" && r.ranEstimation,
              kLog + "A2_bare_colon: bare `eststo: regress y w` is rc 0 and RUNS the inner "
              "command. Got rc " + r.rc + " `" + r.msg + "`, ran=" +
                  (r.ranEstimation ? "yes" : "no"));
      w2check(isStored(session, "est2") && isFreshYonW(session, "est2"),
              kLog + "A2_bare_colon + VAL|A2|b_w: the set it stores is the FRESH y~w model "
              "(b[w] = .3084090989041905, no x row) under the auto-name est2 -- \"(est2 stored)\", "
              "the second set eststo stored. Stored: " + namesOf(session));
      w2check(!isStored(session, "est1") && !isStored(session, "m1") &&
                  !isStored(session, "regress"),
              kLog + "A2_autoname_is_est1 = 111, A2_autoname_is_m1_CONTROL = 111: the named "
              "store before it took number 1, so there is no est1; there is no m1 (the old "
              "auto-name); and nothing is keyed by the inner command's first word. Stored: " +
                  namesOf(session));
    }

    // ---- A3: a LOCAL in the name ----
    // main today: rc 0, no inner run, "(stored as mac3:)" -- colon glued on.
    (void)runLine("quietly regress y x", session);
    (void)runLine("local i = 3", session);
    {
      const LineResult r = runLine("eststo mac`i': regress y w", session);
      w2check(r.rc == "0" && isStored(session, "mac3") && isFreshYonW(session, "mac3") &&
                  !isStored(session, "mac3:"),
              kLog + "A3_macro_in_name / A3_stored_as_mac3: `eststo mac`i': regress y w` stores "
              "the fresh model as mac3 (the macro is expanded in the HEAD; the key has no "
              "colon). Got rc " + r.rc + " `" + r.msg + "`; stored: " + namesOf(session));
    }

    // ---- A4: macro name in a LOOP, inner command with options ----
    // main today: r(198) "option `vce` not allowed" -- the flat eststo arm's net
    // judging the INNER command's option -- and the loop stops on its first pass.
    {
      LineResult r;
      try {
        const auto res = piet::doexec::executeDoSource(
            "forvalues r = 1/2 {\n"
            "  quietly regress y x\n"
            "  eststo lp`r': quietly regress y w if t > `r', vce(cluster id)\n"
            "}\n",
            session, piet::dispatch);
        r = scanBlocks(res.blocks);
      } catch (const std::exception& e) {
        r.rc = "threw";
        r.msg = e.what();
      }
      w2check(r.rc == "0" && isStored(session, "lp1") && isStored(session, "lp2"),
              kLog + "A4_loop_macro_name_opts_1/_2 + A4_stored_lp1_lp2: both passes are rc 0 and "
              "store lp1 and lp2. Got rc " + r.rc + " `" + r.msg + "`; stored: " +
                  namesOf(session));
      // `r' sits on BOTH sides of the colon. The head is expanded by the prefix
      // arm and the remainder by dispatch, once each: N and se[w] differ between
      // the passes only if the remainder's `t > `r'` saw its own pass's value.
      w2check(near(storedN(session, "lp1"), 360) &&
                  near(storedCoef(session, "lp1", "w", "se"), .026437843030034) &&
                  near(storedN(session, "lp2"), 320) &&
                  near(storedCoef(session, "lp2", "w", "se"), .0275242884482439),
              kLog + "VAL|A4_1 (se_w .026437843030034, N 360) and VAL|A4_2 (se_w "
              ".0275242884482439, N 320): each stored set is ITS pass's clustered fit -- the "
              "inner `if t > `r'` and `vce(cluster id)` reached the inner command intact.");
    }

    // ---- A5: a GLOBAL in the name ----
    // main today: rc 0, no inner run, "(stored as g7_a:)".
    (void)runLine("global ptag \"g7\"", session);
    (void)runLine("quietly regress y x", session);
    {
      const LineResult r = runLine("eststo ${ptag}_a: regress y w", session);
      w2check(r.rc == "0" && isStored(session, "g7_a") && isFreshYonW(session, "g7_a") &&
                  !isStored(session, "g7_a:"),
              kLog + "A5_global_in_name / A5_stored_as_g7_a. Got rc " + r.rc + " `" + r.msg +
                  "`; stored: " + namesOf(session));
    }

    // ---- A6: options BEFORE the colon, and a QUOTED colon among them ----
    // main today: r(198) "option `:` not allowed" for both.
    (void)runLine("quietly regress y x", session);
    {
      const LineResult r = runLine("eststo t0, title(\"plain\"): regress y w", session);
      w2check(r.rc == "0" && isFreshYonW(session, "t0") &&
                  storedTitle(session, "t0") == std::optional<std::string>("plain"),
              kLog + "A6_opts_before_colon_CONTROL: `eststo t0, title(\"plain\"): regress y w` is "
              "rc 0, stores the fresh model as t0, and the title lands on the set. Got rc " +
                  r.rc + " `" + r.msg + "`");
    }
    (void)runLine("quietly regress y x", session);
    {
      const LineResult r = runLine("eststo t1, title(\"a: b\"): regress y w", session);
      w2check(r.rc == "0" && isFreshYonW(session, "t1") &&
                  storedTitle(session, "t1") == std::optional<std::string>("a: b"),
              kLog + "A6_quoted_colon_in_title / A6_stored_t0_t1 / VAL|A6|b_w: the colon INSIDE "
              "the quotes does not end the head -- the line splits at the one after `)`, the "
              "title keeps its colon, and the fresh model is stored as t1. Got rc " + r.rc +
                  " `" + r.msg + "`; stored: " + namesOf(session));
    }

    // ---- A7: prefix() swaps the auto-name's stem and SHARES the counter ----
    // main today: r(198) "option `prefix` not allowed".
    (void)runLine("quietly regress y x", session);
    {
      const LineResult r = runLine("eststo, prefix(zz): regress y w", session);
      w2check(r.rc == "0" && isStored(session, "zz9") && isFreshYonW(session, "zz9") &&
                  !isStored(session, "zz1") && !isStored(session, "zz2"),
              kLog + "A7_bare_with_prefix_opt (\"(zz9 stored)\"), A7_name_zz1 = 111, A7_name_zz2 = "
              "111: eight sets were stored before it, so it is zz9 -- prefix() has no counter of "
              "its own. Got rc " + r.rc + " `" + r.msg + "`; stored: " + namesOf(session));
    }

    // ---- A8: a failing inner command stores nothing, returns ITS rc, and
    //          consumes no auto-name number ----
    // main today: the NAMED form already behaves; the BARE form is rc 0 and
    // stores the stale model as "regress".
    {
      const LineResult alone = runLine("regress y nosuchvar", session);
      const size_t before = session.estimatesNames().size();
      const LineResult named = runLine("eststo bad1: regress y nosuchvar", session);
      const LineResult bare = runLine("eststo: regress y nosuchvar", session);
      // The oracle's rc is 111. It is asserted as "the inner command's own rc"
      // and not as the literal, because `regress <missing var>` alone answers 1
      // on this build and selftest.cpp pins that (BS-100, C1/N3 guards) -- see
      // this lane's pinned_old_behaviour report. What the PREFIX owes the oracle
      // is that it adds nothing and hides nothing.
      w2check(alone.rc != "0" && named.rc == alone.rc && bare.rc == alone.rc &&
                  !isStored(session, "bad1") && session.estimatesNames().size() == before,
              kLog + "A8_named_inner_fails, A8_bad1_not_stored, A8_bare_inner_fails: the line's "
              "rc is the inner command's own (alone: " + alone.rc + ", named: " + named.rc +
                  ", bare: " + bare.rc + ") and nothing is stored. Stored: " + namesOf(session));
      const LineResult after = runLine("eststo: regress y x w", session);
      w2check(after.rc == "0" && isStored(session, "est10") &&
                  storedCoef(session, "est10", "x").has_value() &&
                  storedCoef(session, "est10", "w").has_value() && !isStored(session, "est3") &&
                  !isStored(session, "est4"),
              kLog + "A8_bare_after_failure (\"(est10 stored)\"), A8_counter_est3_... = 111, "
              "A8_counter_est4_... = 111: the two failures took no number -- nine sets stored, so "
              "the next bare store is est10. Got rc " + after.rc + " `" + after.msg +
                  "`; stored: " + namesOf(session));
    }

    // A9 (quietly outside / inside) already behaves on main; replayed only so
    // the store count in front of A11 is the oracle's.
    (void)runLine("quietly eststo q1: regress y w", session);
    (void)runLine("eststo q2: quietly regress y w", session);

    // ---- A11: `eststo clear` restarts the counter ----
    // main today: rc 0, "(stored as regress)".
    (void)runLine("eststo clear", session);
    {
      const LineResult r = runLine("eststo: regress y w", session);
      w2check(r.rc == "0" && isStored(session, "est1") && isFreshYonW(session, "est1") &&
                  !isStored(session, "lit1") && session.estimatesNames().size() == 1,
              kLog + "A11_bare_after_clear (\"(est1 stored)\"), A11_counter_reset_to_est1 = 0, "
              "A11_lit1_gone_after_clear = 111. Got rc " + r.rc + " `" + r.msg + "`; stored: " +
                  namesOf(session));
    }
  }

  // ============ the head's option net (class: OPTION VOCABULARY) ==============
  {
    const std::string wd = base + "/headopts";
    fs::create_directories(wd, ec);
    piet::SessionState session(wd);
    loadFrame(session);
    (void)runLine("quietly regress y x", session);
    // An ABBREVIATED head option must reach the reader under its canonical name:
    // the head never passes the tokenizer's resolution point, so the prefix arm
    // canonicalizes it itself. eststo.ado declares `Title(passthru)`, so `ti()`
    // is Stata. main today: r(198) "option `:` not allowed".
    {
      const LineResult r = runLine("eststo h1, ti(\"x: y\"): regress y w", session);
      w2check(r.rc == "0" && isFreshYonW(session, "h1") &&
                  storedTitle(session, "h1") == std::optional<std::string>("x: y"),
              kLog + "A6_quoted_colon_in_title, abbreviated: `ti(\"x: y\")` before the colon is "
              "READ as title(), not accepted-and-dropped. Got rc " + r.rc + " `" + r.msg + "`");
    }
    // A real eststo option this build does not implement is refused BY NAME as
    // such, before the inner command runs. main today: "option `refresh` not
    // allowed" -- the wording for a typo.
    (void)runLine("quietly regress y x", session);
    {
      const LineResult r = runLine("eststo h2, refresh: regress y w", session);
      w2check(r.rc == "198" && r.msg.find("refresh") != std::string::npos &&
                  r.msg.find("valid Stata option") != std::string::npos && !r.ranEstimation &&
                  !isStored(session, "h2"),
              kLog + "A7 (options before the colon are eststo's own): `refresh` is in "
              "eststo.ado's syntax line and unimplemented here, so it is an honest r(198) naming "
              "it as valid Stata -- and the head is judged BEFORE the command runs, so nothing "
              "ran and nothing was stored. Got rc " + r.rc + " `" + r.msg + "`");
    }
  }

  // ===== the flat arm never stores under a colon shape (BUG-520, 2nd half) =====
  {
    const std::string wd = base + "/flatguard";
    fs::create_directories(wd, ec);
    piet::SessionState session(wd);
    loadFrame(session);
    (void)runLine("quietly regress y x", session);
    (void)runLine("local e \"eststo\"", session);
    // The command word arrives through a macro, so Pre-0b (which matches the RAW
    // line) cannot see the prefix. main today: rc 0, inner never run, the primed
    // y~x model "(stored as viaMac:)" -- the same silent-wrong as A3, by the one
    // door the structural split leaves open. Stata runs this line; this build
    // refuses it by name, which is the honest half of that gap.
    const LineResult r = runLine("`e' viaMac: regress y w", session);
    w2check(r.rc == "198" && !r.ranEstimation && session.estimatesNames().empty(),
            kLog + "A3_macro_in_name's silent-wrong, flat-arm door: a colon-shaped `eststo` line "
            "that Pre-0b did not recognise is an honest r(198) that stores NOTHING -- never the "
            "stale model under `NAME:`. Got rc " + r.rc + " `" + r.msg + "`; stored: " +
                namesOf(session));
    // ...and the flat form with a quoted colon stays flat: no colon outside the
    // quotes means no prefix, so this stores the ACTIVE (primed y~x) model.
    const LineResult f = runLine("eststo m9, title(\"c: d\")", session);
    // Same for a colon behind a trailing comment, which tokenize() strips for a
    // line that reaches it raw (the console path): still the flat store.
    const LineResult c = runLine("eststo m10 // note: keep", session);
    w2check(f.rc == "0" && storedCoef(session, "m9", "x").has_value() &&
                storedTitle(session, "m9") == std::optional<std::string>("c: d") &&
                c.rc == "0" && storedCoef(session, "m10", "x").has_value() &&
                session.estimatesNames().size() == 2,
            kLog + "A6_quoted_colon_in_title, the other side of it: with the refusal above in "
            "place, flat `eststo m9, title(\"c: d\")` is still the FLAT store (rc 0, the active "
            "model, title intact) and so is `eststo m10 // note: keep` -- the quote- and "
            "comment-aware split is what keeps the two forms apart. Got rc " + f.rc + " `" +
                f.msg + "` / rc " + c.rc + " `" + c.msg + "`; stored: " + namesOf(session));
  }

  // ====== the prefix arm answers the community-name gate (WS-B) like `eststo` ======
  // No oracle arm: `set extcompat` is PIET's own switch, not Stata's. It is here
  // because the prefix arm returns BEFORE the gate's place in dispatch, and the
  // A2 fix would otherwise have widened an old hole from `eststo NAME: cmd` to
  // every bare `eststo:` line. Isolation pattern read from selftest.cpp's
  // GTOOLS/extcompat leg: own settings store for the duration, previous value
  // RESTORED (the harness pins PIET_SETTINGS_FILE on some runs), then the cache
  // invalidated and the default forced, in that order. POSIX-only for the same
  // reason that leg is (setenv/unsetenv); the flat form's gate is covered on
  // every platform by testExtcompatGate.
#ifndef _WIN32
  {
    const std::string wd = base + "/gate";
    fs::create_directories(wd, ec);
    const char* prevSettings = std::getenv("PIET_SETTINGS_FILE");
    const std::string savedSettings = prevSettings ? prevSettings : "";
    const std::string ownSettings = (fs::path(wd) / "settings.json").u8string();
    setenv("PIET_SETTINGS_FILE", ownSettings.c_str(), 1);

    piet::SessionState session(wd);
    loadFrame(session);
    (void)runLine("quietly regress y x", session);
    (void)runLine("set extcompat off", session);
    // main today: rc 0, runs, "(stored as gate1)" -- while flat `eststo gate1`
    // on the same session is gated r(199).
    const LineResult r = runLine("eststo gate1: regress y w", session);
    w2check(r.rc == "199" && !r.ranEstimation && !isStored(session, "gate1"),
            "W2 eststoprefix / WS-B gate (no oracle arm -- PIET's own `set extcompat off`): the "
            "prefix form of a community name is gated exactly as its flat form is; nothing runs "
            "and nothing is stored. Got rc " + r.rc + " `" + r.msg + "`");

    if (!savedSettings.empty()) {
      setenv("PIET_SETTINGS_FILE", savedSettings.c_str(), 1);
    } else {
      unsetenv("PIET_SETTINGS_FILE");
    }
    piet::settings::invalidateExtcompatCache();
    piet::settings::setExtcompatEnabled(true);
    // Verified by DISPATCHING again, not by reading the flag back: the cases
    // after this one inherit whatever this leaves behind.
    const LineResult again = runLine("eststo gate2: regress y w", session);
    w2check(again.rc == "0" && isFreshYonW(session, "gate2"),
            "W2 eststoprefix / WS-B gate: ...and with the default restored the same line answers "
            "again (w2_eststo_prefix.log A1_literal_name_CONTROL), so the cases after this one "
            "get the state they were written against. Got rc " + again.rc + " `" + again.msg + "`");
  }
#endif

  fs::remove_all(base, ec);
}
