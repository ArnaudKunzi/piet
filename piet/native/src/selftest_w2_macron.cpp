// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// W2 lane `macron` -- `_N` / `_n` reach a macro, a scalar and an inline expansion
// (BUG-512), and `local NAME : display ...` exists (BUG-513).
//
// Oracle: tests/eval/golden/aer_w2/w2_smalls.log, section A (StataBE 19.5). The
// fixture below is that file's OWN synthetic data (`set obs 12` + the same
// closed-form generates and the two `replace x = . in`), so every number
// asserted here is a number the log prints, or a composition the label spells
// out. Every arm says what main answered before the fix; the one guard that main
// already passed is folded into a discriminating check rather than counted alone.
#include "selftest_w2.h"

#include <cmath>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

#include "piet/dispatcher.h"
#include "piet/doexec.h"
#include "piet/eval/errors.h"
#include "piet/session_state.h"
#include "vendor/nlohmann/json.hpp"

namespace {

using ojson = nlohmann::ordered_json;

// The oracle's data block, verbatim (w2_smalls.do lines 8-15).
const char* const kW2SmallsData[] = {
    "clear",
    "set obs 12",
    "generate id = ceil(_n/3)",
    "generate t  = mod(_n-1,3)+1",
    "generate x  = mod(_n*7,11)",
    "generate x2 = mod(_n*3,5)",
    "generate y  = 2*x - x2 + mod(_n,4)",
    "replace x = . in 5",
    "replace x = . in 11",
};

struct Outcome {
  std::string rc = "0";   // coarseRc of the LAST error block, "0" when none
  std::string message;    // that block's message
  std::vector<std::string> displays;
};

// Any shape other than "array of objects" is simply not collected, so a wrong
// shape is a FAILED check downstream, never a json type_error out of this TU.
void collect(const ojson& blocks, Outcome* out) {
  if (!blocks.is_array()) return;
  for (const auto& b : blocks) {
    if (!b.is_object()) continue;
    const std::string type = b.value("type", std::string());
    if (type == "error") {
      // An error a `capture` already swallowed is not this line's outcome.
      if (b.contains("captured") && b.at("captured").is_boolean() && b.at("captured").get<bool>())
        continue;
      try {
        out->rc = piet::eval::coarseRc(b);
      } catch (...) {
        out->rc = "?";
      }
      out->message = b.value("message", std::string());
    } else if (type == "display" && b.contains("text") && b.at("text").is_string()) {
      out->displays.push_back(b.at("text").get<std::string>());
    }
  }
}

Outcome run(piet::SessionState& session, const std::string& cmd) {
  Outcome out;
  try {
    collect(piet::dispatch(cmd, session), &out);
  } catch (const std::exception& e) {
    out.rc = "threw";
    out.message = e.what();
  }
  return out;
}

Outcome runSource(piet::SessionState& session, const std::string& src) {
  Outcome out;
  try {
    collect(piet::doexec::executeDoSource(src, session, piet::dispatch).blocks, &out);
  } catch (const std::exception& e) {
    out.rc = "threw";
    out.message = e.what();
  }
  return out;
}

void loadOracleData(piet::SessionState& session) {
  for (const char* line : kW2SmallsData) (void)run(session, line);
}

std::string lastDisplay(const Outcome& o) {
  return o.displays.empty() ? std::string("<no display block>") : o.displays.back();
}

}  // namespace

void testW2MacroN() {
  std::printf("[selftest] --- W2 macron: _N/_n in a macro, a scalar, an inline expansion "
              "(w2_smalls.log section A) ---\n");
  namespace fs = std::filesystem;
  const std::string workdir =
      (fs::temp_directory_path() / "piet_native_selftest_w2macron").u8string();
  std::error_code ec;
  fs::remove_all(workdir, ec);
  fs::create_directories(workdir, ec);

  {
    piet::SessionState session(workdir);
    loadOracleData(session);

    // --- the seven arms of section A, in the log's order -------------------
    // main: stored the two characters `_N` at rc 0.
    Outcome o = run(session, "local n = _N");
    w2check(o.rc == "0" && session.eval.locals.get("n", "<unset>") == "12",
            "W2 macron [w2_smalls.log A local_N]: `local n = _N` stores 12, the observation "
            "count, not its own source text. Got `" + session.eval.locals.get("n", "<unset>") +
                "` rc " + o.rc);

    // main: stored `_n`.
    o = run(session, "local i = _n");
    w2check(o.rc == "0" && session.eval.locals.get("i", "<unset>") == "1",
            "W2 macron [w2_smalls.log A local_n]: `local i = _n` stores 1 -- a macro-context "
            "expression is evaluated on observation 1. Got `" +
                session.eval.locals.get("i", "<unset>") + "` rc " + o.rc);

    // main: stored `_N*2 + 1`.
    o = run(session, "local a = _N*2 + 1");
    w2check(o.rc == "0" && session.eval.locals.get("a", "<unset>") == "25",
            "W2 macron [w2_smalls.log A local_N_expr]: `local a = _N*2 + 1` stores 25 -- `_N` is "
            "an operand like any other. Got `" + session.eval.locals.get("a", "<unset>") +
                "` rc " + o.rc);

    // main: the scalar held the text `_N`, and display printed it back.
    (void)run(session, "scalar s = _N");
    o = run(session, "display \"PREC|scalar_N|\" s \"|\"");
    w2check(session.eval.scalars.get("s", "<unset>") == "12" &&
                lastDisplay(o) == "PREC|scalar_N|12|",
            "W2 macron [w2_smalls.log A scalar_N]: `scalar s = _N` is 12 and displays as the "
            "oracle's own record. Got scalar `" + session.eval.scalars.get("s", "<unset>") +
                "` display `" + lastDisplay(o) + "`");

    // main: `Macro expansion did not converge after 16 passes`.
    o = run(session, "display \"PREC|inline_N|`=_N'|`=_N-1'|\"");
    w2check(o.rc == "0" && lastDisplay(o) == "PREC|inline_N|12|11|",
            "W2 macron [w2_smalls.log A inline_N]: `` `=_N' `` and `` `=_N-1' `` expand to 12 and "
            "11 inside a quoted display. Got rc " + o.rc + " `" + lastDisplay(o) + "` " +
                o.message);

    // main: r(101) `display not allowed`, `m` left unset (BUG-513).
    o = run(session, "local m : display _N");
    w2check(o.rc == "0" && session.eval.locals.get("m", "<unset>") == "12",
            "W2 macron [w2_smalls.log A ext_display_N]: `local m : display _N` stores what display "
            "would have printed, 12. Got `" + session.eval.locals.get("m", "<unset>") + "` rc " +
                o.rc + " " + o.message);

    // main: `$G` was `_N`.
    o = run(session, "global G = _N");
    w2check(o.rc == "0" && session.eval.globals.get("G", "<unset>") == "12",
            "W2 macron [w2_smalls.log A global_N]: `global G = _N` stores 12 -- the global twin "
            "of local_N goes through the same evaluator. Got `" +
                session.eval.globals.get("G", "<unset>") + "` rc " + o.rc);

    // --- a quoted literal is DATA ------------------------------------------
    // One check, two halves: `"_N"` alone stays text (main already did that --
    // it is the guard), and beside a bare `_N` only the bare one is resolved
    // (main: stored the whole expression as text). A quote-blind substitution
    // would turn the literal into "12", strpos would miss, and the sum would be
    // 12 instead of 2 + 12.
    (void)run(session, "local lit = \"_N\"");
    o = run(session, "local mix = strpos(\"_N\", \"N\") + _N");
    w2check(session.eval.locals.get("lit", "<unset>") == "_N" &&
                session.eval.locals.get("mix", "<unset>") == "14",
            "W2 macron [w2_smalls.log A local_N + the lane brief's literal rule]: `\"_N\"` inside "
            "a string literal stays text -- `local lit = \"_N\"` is `_N`, and `strpos(\"_N\",\"N\") "
            "+ _N` is 2 + 12 = 14. Got lit `" + session.eval.locals.get("lit", "<unset>") +
                "` mix `" + session.eval.locals.get("mix", "<unset>") + "`");

    // --- the same evaluator's other consumers ------------------------------
    // main: echoed the source text `_N*2 + 1` at rc 0.
    o = run(session, "display _N*2 + 1");
    w2check(o.rc == "0" && lastDisplay(o) == "25",
            "W2 macron [w2_smalls.log A local_N_expr, on display]: `display _N*2 + 1` prints 25. "
            "Only the WHOLE token `_N` resolved before; any expression around it was echoed back. "
            "Got rc " + o.rc + " `" + lastDisplay(o) + "`");

    // main: `matrix x not found` -- the subscript arm only took an integer
    // literal. x[12] = mod(12*7,11) = 7 on the oracle's own data.
    o = run(session, "display x[_N]");
    w2check(o.rc == "0" && lastDisplay(o) == "7",
            "W2 macron [w2_smalls.log A local_N + its `generate x = mod(_n*7,11)`]: `display "
            "x[_N]` is the last observation, mod(84,11) = 7. Got rc " + o.rc + " `" +
                lastDisplay(o) + "` " + o.message);

    // main: the header died on the same non-converging expansion as inline_N.
    o = runSource(session, "local cnt = 0\nforvalues k = 1/`=_N' {\n  local cnt = `cnt' + 1\n}\n");
    w2check(o.rc == "0" && session.eval.locals.get("cnt", "<unset>") == "12",
            "W2 macron [w2_smalls.log A inline_N, as a loop bound]: `forvalues k = 1/`=_N'` runs "
            "its body 12 times. Got count `" + session.eval.locals.get("cnt", "<unset>") +
                "` rc " + o.rc + " " + o.message);

    // main: r(N) was posted as MISSING at rc 0.
    o = runSource(session,
                  "program define w2macron_n, rclass\n"
                  "  return scalar N = _N\n"
                  "end\n"
                  "w2macron_n\n");
    {
      const double* rn = session.eval.rValues.find("N");
      w2check(rn != nullptr && *rn == 12.0,
              "W2 macron [w2_smalls.log A scalar_N, through `return scalar`]: an rclass program's "
              "`return scalar N = _N` posts r(N) = 12, not missing. Got " +
                  (rn == nullptr ? std::string("<unset>")
                                 : (std::isnan(*rn) ? std::string("nan") : std::to_string(*rn))) +
                  " rc " + o.rc + " " + o.message);
    }
  }

  // --- `_N` follows the LIVE frame, and the hook is never stale -------------
  {
    piet::SessionState session(workdir);
    // main: `local e = _N` stored `_N`; `display _N` printed `.`.
    (void)run(session, "clear");
    Outcome o = run(session, "local e = _N");
    const Outcome d = run(session, "display _N");
    w2check(o.rc == "0" && session.eval.locals.get("e", "<unset>") == "0" && lastDisplay(d) == "0",
            "W2 macron [w2_smalls.log `set obs 12`: \"Number of observations (_N) was 0\"]: with "
            "no data in memory `_N` is 0 on both surfaces -- `local e = _N` and `display _N`. Got "
            "local `" + session.eval.locals.get("e", "<unset>") + "` display `" + lastDisplay(d) +
                "`");

    loadOracleData(session);
    (void)run(session, "local before = _N");
    (void)run(session, "keep if x < 5");
    o = run(session, "local after = _N");
    const Outcome inl = run(session, "display \"`=_N'\"");
    // main: both locals held the text `_N`.
    w2check(session.eval.locals.get("before", "<unset>") == "12" &&
                session.eval.locals.get("after", "<unset>") == "3" && lastDisplay(inl) == "3",
            "W2 macron [w2_smalls.log A local_N + B keep_lt N=3]: `_N` is read at the moment of "
            "use -- 12 before `keep if x < 5`, 3 after, on the local and the inline form alike. "
            "Got before `" + session.eval.locals.get("before", "<unset>") + "` after `" +
                session.eval.locals.get("after", "<unset>") + "` inline `" + lastDisplay(inl) +
                "`");
  }

  // --- the third evaluator: `post` ------------------------------------------
  {
    piet::SessionState session(workdir);
    loadOracleData(session);
    // main: both cells were posted as `.` at rc 0 -- postfile keeps its own
    // namespace and answers a raised name with missing.
    const Outcome o = runSource(session,
                                "tempname w2h\n"
                                "tempfile w2pf\n"
                                "postfile `w2h' a b using \"`w2pf'\", replace\n"
                                "post `w2h' (_N) (_N*2+1)\n"
                                "postclose `w2h'\n"
                                "use \"`w2pf'\", clear\n"
                                "display \"POST|\" a[1] \"|\" b[1] \"|\"\n");
    w2check(o.rc == "0" && lastDisplay(o) == "POST|12|25|",
            "W2 macron [w2_smalls.log A scalar_N / local_N_expr, through `post`]: `post h (_N) "
            "(_N*2+1)` writes 12 and 25, not two missings. Got rc " + o.rc + " `" +
                lastDisplay(o) + "` " + o.message);
  }

  fs::remove_all(workdir, ec);
}
