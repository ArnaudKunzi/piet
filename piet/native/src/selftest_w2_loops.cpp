// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// W2 lane `loops` -- selftest arms (BUG-544..547).
//
// Oracles: tests/eval/golden/aer_w2/w2_openbugs.log (arms fe_ctl_known,
// fe_unknown, fe_mixed, fe_of_var, fe_abbrev -- a unique prefix resolves) and
// tests/eval/golden/aer_w2/w2_cmdtriage.log (section A, arms for_*). Every
// arm names the oracle arm it rests on and says what main answered before
// the fix, so each arm is a discriminator.
//
// Numeric arms use a DELTA check (`gen double diff = observed - expected`,
// then `summarize diff` and read r(sum)) rather than parsing a `display`ed
// number against a precomputed string: the same arithmetic evaluated the
// same way on both sides is exactly zero regardless of this build's numeric
// formatting quirks (BUG-406 class), so the check is robust to that without
// needing to replicate PIET's display formatting by hand.
#include "selftest_w2.h"

#include <cstdio>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

#include "piet/dispatcher.h"
#include "piet/doexec.h"
#include "piet/eval/errors.h"
#include "piet/session_state.h"
#include "vendor/nlohmann/json.hpp"

namespace {

using ojson = nlohmann::ordered_json;

// The rc of the first UNCAPTURED error block in `blocks`, "0" when there is
// none. A wrong shape (not an array, a non-object block) reads as "shape" so
// the arm FAILS instead of the binary aborting (lane brief, defect class 1).
std::pair<std::string, std::string> rcOf(const ojson& blocks) {
  if (!blocks.is_array()) return {"shape", "dispatch did not return an array"};
  for (const auto& b : blocks) {
    if (!b.is_object()) return {"shape", "a block is not an object"};
    try {
      if (b.value("type", std::string()) == "error" && !b.value("captured", false)) {
        return {piet::eval::coarseRc(b), b.value("message", std::string())};
      }
    } catch (const std::exception& e) {
      return {"shape", e.what()};
    }
  }
  return {"0", ""};
}

std::string joinedText(const ojson& blocks) {
  std::string all;
  if (!blocks.is_array()) return all;
  for (const auto& b : blocks) {
    if (!b.is_object()) continue;
    const std::string t = b.value("type", std::string());
    if (t == "display" || t == "error") {
      all += b.value("text", b.value("message", std::string()));
      all += "\n";
    }
  }
  return all;
}

bool has(const std::string& hay, const std::string& needle) {
  return hay.find(needle) != std::string::npos;
}

// The r(sum)/r(N) text of the LAST display block (trimmed) -- same shape the
// existing W2 lanes read a scalar result through.
std::string trimmed(const std::string& s) {
  const auto b = s.find_first_not_of(" \t");
  const auto e = s.find_last_not_of(" \t");
  if (b == std::string::npos) return "";
  return s.substr(b, e - b + 1);
}

std::string lastDisplayText(const ojson& blocks) {
  std::string out;
  if (!blocks.is_array()) return out;
  for (const auto& b : blocks) {
    if (b.is_object() && b.value("type", std::string()) == "display") {
      out = b.value("text", std::string());
    }
  }
  return trimmed(out);
}

// `summarize VAR` then `display r(sum)`/`display r(N)`, trimmed. Reused by
// every numeric delta check below.
std::string rOf(piet::SessionState& session, const std::string& summarizeVar,
                const std::string& rname) {
  (void)piet::dispatch("summarize " + summarizeVar, session);
  return lastDisplayText(piet::dispatch("display r(" + rname + ")", session));
}

// The BUG-509/544/545/546 fixture: w2_openbugs.log's own data
// (w2_openbugs.do, BUG-509 section) -- 3 rows, two closed-form columns.
void seedOpenBugsData(piet::SessionState& session) {
  for (const char* s : {"clear", "set obs 3", "generate double alpha = _n",
                        "generate double beta = 2 * _n"}) {
    (void)piet::dispatch(s, session);
  }
}

// The BUG-547 fixture: w2_cmdtriage.log's own data (w2_cmdtriage.do lines
// 4-12), verbatim.
void seedCmdTriageData(piet::SessionState& session) {
  for (const char* s :
       {"clear", "set obs 12", "generate double id = _n", "generate double g = mod(_n, 3)",
        "generate double a = _n*1.5", "generate double b = mod(_n*5, 7)",
        "generate double c = mod(_n*7, 5)",
        "generate str4 s = cond(mod(_n, 2)==0, \"ev\", \"od\")",
        "generate byte f = mod(_n, 2)", "generate double a2 = a", "generate double b2 = b"}) {
    (void)piet::dispatch(s, session);
  }
  (void)piet::dispatch("global XC \"b\"", session);
}

}  // namespace

void testW2Loops() {
  std::printf("[selftest] --- W2 loops: foreach-varlist guard, `of var` abbreviation, captured "
              "loop/block rc, legacy `for` (BUG-544..547) ---\n");
  namespace fs = std::filesystem;
  const std::string workdir = (fs::temp_directory_path() / "piet_native_selftest_w2loops").u8string();
  std::error_code ec;
  fs::remove_all(workdir, ec);
  fs::create_directories(workdir, ec);

  // ── BUG-509 (corrected)/BUG-544: foreach-of-varlist existence guard ──────
  // Oracle: w2_openbugs.log fe_unknown/fe_mixed. The 2026-09-18 capture's
  // r(199) pin was an artifact of ITS `capture noisily foreach ... {`
  // spelling (the 199 was the orphaned `}`, not the foreach's own rc); the
  // real rc, measured here inside a `capture noisily { }` BLOCK exactly like
  // the oracle, is r(111) -- BEFORE any iteration, even for a list whose
  // FIRST name exists (fe_mixed: `alpha nosuchcol` never iterates `alpha`).
  // Main (pre-fix): rc 0, and the body ran once printing `saw nosuchcol`.
  {
    piet::SessionState session(workdir);
    seedOpenBugsData(session);
    auto run = [&](const std::string& src) {
      return piet::doexec::executeDoSource(src, session, piet::dispatch);
    };

    auto r1 = run("capture noisily {\n"
                  "    foreach v of varlist nosuchcol {\n"
                  "        display \"saw `v'\"\n"
                  "    }\n"
                  "}\n");
    w2check(session.eval.locals.get("_rc", "") == "111" && !has(joinedText(r1.blocks), "saw"),
            "W2 loops/BUG-544 (fe_unknown): `foreach v of varlist nosuchcol` inside `capture "
            "noisily { }` is r(111) and the body NEVER runs. Main: rc 0, body ran once with "
            "`saw nosuchcol`. Got _rc=" + session.eval.locals.get("_rc", "<unset>") + " text=[" +
                joinedText(r1.blocks) + "]");

    auto r2 = run("capture noisily {\n"
                  "    foreach v of varlist alpha nosuchcol {\n"
                  "        display \"saw `v'\"\n"
                  "    }\n"
                  "}\n");
    w2check(session.eval.locals.get("_rc", "") == "111" && !has(joinedText(r2.blocks), "saw"),
            "W2 loops/BUG-544 (fe_mixed): a MIXED list `alpha nosuchcol` is ALSO r(111) with NO "
            "iteration at all -- not even the leading known name `alpha` runs, because the whole "
            "varlist is validated before the loop starts. Main: rc 0, iterated both. Got _rc=" +
                session.eval.locals.get("_rc", "<unset>") + " text=[" + joinedText(r2.blocks) + "]");

    // Control: the known-varlist arm is untouched by the guard (fe_ctl_known).
    auto r3 = run("capture noisily {\n"
                  "    foreach v of varlist alpha beta {\n"
                  "        display \"saw `v'\"\n"
                  "    }\n"
                  "}\n");
    w2check(session.eval.locals.get("_rc", "") == "0" && has(joinedText(r3.blocks), "saw alpha") &&
                has(joinedText(r3.blocks), "saw beta"),
            "W2 loops/BUG-544 control (fe_ctl_known): `foreach v of varlist alpha beta` is "
            "unaffected -- rc 0, iterates both. Got _rc=" + session.eval.locals.get("_rc", "<unset>"));

    // fe_abbrev: Stata resolves the unique abbreviation `alph` -> `alpha`
    // (w2_openbugs.log:190-194: rc 0, iterates alpha).
    auto r4 = run("capture noisily {\n"
                  "    foreach v of varlist alph {\n"
                  "        display \"saw `v'\"\n"
                  "    }\n"
                  "}\n");
    w2check(session.eval.locals.get("_rc", "") == "0" && has(joinedText(r4.blocks), "saw alpha"),
            "W2 loops/BUG-544 fe_abbrev: `foreach v of varlist alph` iterates `alpha` at rc 0 "
            "(w2_openbugs.log:190-194). Got _rc=" + session.eval.locals.get("_rc", "<unset>") +
                " text=[" + joinedText(r4.blocks) + "]");
  }

  // ── BUG-545: `foreach ... of var[list]` abbreviation ─────────────────────
  // Oracle: w2_openbugs.log fe_of_var (`of var`, rc 0) and open-bugs.json's
  // corpus measurement (`of var` 19 lines + `of varl` 1 line, 8 papers).
  // Main: "loops: unrecognised loop header" -- the regex required the
  // literal word `varlist`.
  {
    piet::SessionState session(workdir);
    seedOpenBugsData(session);
    auto r = piet::dispatch("foreach v of var alpha beta {\ndisplay \"saw `v'\"\n}\n", session);
    auto rc = rcOf(r);
    w2check(rc.first == "0" && has(joinedText(r), "saw alpha") && has(joinedText(r), "saw beta"),
            "W2 loops/BUG-545 (fe_of_var): `foreach v of var alpha beta` is rc 0, iterates both. "
            "Main: `loops: unrecognised loop header`. Got rc " + rc.first + " `" + rc.second + "`");

    // Corpus-measured second spelling: `of varl` (1 line, open-bugs.json).
    auto r2 = piet::dispatch("foreach v of varl alpha beta {\ndisplay \"saw `v'\"\n}\n", session);
    auto rc2 = rcOf(r2);
    w2check(rc2.first == "0" && has(joinedText(r2), "saw alpha") && has(joinedText(r2), "saw beta"),
            "W2 loops/BUG-545 (of varl, corpus-measured 1 line): `foreach v of varl alpha beta` "
            "is rc 0, iterates both. Got rc " + rc2.first + " `" + rc2.second + "`");
  }

  // ── BUG-546: captured loop / capture BLOCK carries the EXACT rc ──────────
  // Oracle: the same fe_unknown/fe_mixed shape, but the discriminator here is
  // the CODE, not the iteration count. Main hardcoded _rc=1 for BOTH shapes
  // below regardless of the real code.
  {
    piet::SessionState session(workdir);
    seedOpenBugsData(session);

    // (a) capture BLOCK around a plain command (runCaptureBlock, ~1138-1140
    // pre-fix). Main: _rc=1.
    auto r1 = piet::doexec::executeDoSource(
        "capture noisily {\n    summarize nosuchcol\n}\n", session, piet::dispatch);
    w2check(session.eval.locals.get("_rc", "") == "111",
            "W2 loops/BUG-546a (capture BLOCK): `capture noisily { summarize nosuchcol }` carries "
            "the exact _rc=111 (eval::coarseRc), not the coarse 1 the single-line "
            "`capture noisily summarize nosuchcol` never had to begin with. Main: _rc=1. Got _rc=" +
                session.eval.locals.get("_rc", "<unset>"));

    // (b) capture as a LOOP-HEADER PREFIX (executeLoop's capture-prefix
    // filter, ~1693 pre-fix) -- combines with BUG-544's guard: the loop
    // never iterates AND the prefix carries the real code.
    auto r2 = piet::dispatch(
        "capture noisily foreach v of varlist nosuchcol {\n    display \"saw\"\n}\n", session);
    w2check(session.eval.locals.get("_rc", "") == "111" && !has(joinedText(r2), "saw"),
            "W2 loops/BUG-546b (captured loop prefix): `capture noisily foreach v of varlist "
            "nosuchcol { display \"saw\" }` is _rc=111 with NO iteration. Main: _rc=1 (after "
            "iterating once with `v` unset). Got _rc=" + session.eval.locals.get("_rc", "<unset>") +
                " text=[" + joinedText(r2) + "]");
  }

  // ── BUG-547: the legacy `for` command ─────────────────────────────────────
  // Oracle: tests/eval/golden/aer_w2/w2_cmdtriage.log section A (for_*).
  // Main: `for` was not recognized at all (an unsupported-command refusal).
  {
    piet::SessionState session(workdir);
    seedCmdTriageData(session);

    // for_num: `for num 2 3 4: gen double pX = a^X` -- numlist, default
    // placeholder X, single command, rc 0. Also checks the header echo
    // format (`->  <cmd>`, two spaces, dispatched command's own echo
    // dropped).
    {
      auto r = piet::dispatch("for num 2 3 4: gen double pX = a^X", session);
      auto rc = rcOf(r);
      w2check(rc.first == "0",
              "W2 loops/BUG-547 (for_num): `for num 2 3 4: gen double pX = a^X` is rc 0. Main: "
              "unsupported command. Got rc " + rc.first + " `" + rc.second + "`");
      w2check(has(joinedText(r), "->  gen double p2 = a^2") &&
                  !has(joinedText(r), ".  gen double p2 = a^2"),
              "W2 loops/BUG-547 (for_num header): each substituted command is echoed as "
              "`->  <cmd>` (Stata's `for`-specific echo), and the DISPATCHED command's own "
              "normal echo is dropped (Stata's `for` does not double-echo). Got: [" +
                  joinedText(r) + "]");
      (void)piet::dispatch("gen double d547a = p2 - a^2", session);
      (void)piet::dispatch("gen double d547b = p3 - a^3", session);
      (void)piet::dispatch("gen double d547c = p4 - a^4", session);
      w2check(rOf(session, "d547a", "sum") == "0" && rOf(session, "d547b", "sum") == "0" &&
                  rOf(session, "d547c", "sum") == "0",
              "W2 loops/BUG-547 (for_num values): p2/p3/p4 equal a^2/a^3/a^4 exactly (delta sums "
              "are 0) -- every element of the numlist iterated with the right substitution.");
    }

    // for_any_multi: TWO commands in the body, separated by `\`, both run
    // per iteration. Oracle for_any_multi_val: b2_post has r(N)=11 (one row
    // missing after `replace ... if id==3`).
    {
      auto r = piet::dispatch(
          "for any a2 b2: replace X = . if id==3 \\ gen double X_post = X*2", session);
      auto rc = rcOf(r);
      w2check(rc.first == "0",
              "W2 loops/BUG-547 (for_any_multi): rc 0. Got rc " + rc.first + " `" + rc.second + "`");
      w2check(rOf(session, "b2_post", "N") == "11",
              "W2 loops/BUG-547 (for_any_multi_val): b2_post has r(N)=11 (oracle value) -- both "
              "body commands ran, in order, each iteration. Got r(N)=" +
                  rOf(session, "b2_post", "N"));
    }

    // for_var_range: TYPE `var` (the 3-char floor) with an `a-c` RANGE list,
    // reusing loops::expandVarlist the same way `foreach ... of varlist`
    // does. Oracle for_var_range_val: zb has r(N)=12.
    {
      auto r = piet::dispatch("for var a-c: gen double zX = X + 1", session);
      auto rc = rcOf(r);
      w2check(rc.first == "0",
              "W2 loops/BUG-547 (for_var_range): rc 0. Got rc " + rc.first + " `" + rc.second + "`");
      (void)piet::dispatch("gen double d547d = zb - (b + 1)", session);
      w2check(rOf(session, "zb", "N") == "12" && rOf(session, "d547d", "sum") == "0",
              "W2 loops/BUG-547 (for_var_range_val): zb has r(N)=12 (oracle value) and equals "
              "b+1 exactly on every row.");
    }

    // for_two_lists: two `\`-separated lists in the HEADER, each `NAME in
    // TYPE elements`, run in lockstep (Q=1..3, W=a,b,c). Oracle
    // for_two_lists_val: sum(w3)=78 -- checked here as w3 == c*3 exactly.
    {
      auto r = piet::dispatch(
          "for Q in num 1/3 \\ W in any a b c: gen double wQ = W*Q", session);
      auto rc = rcOf(r);
      w2check(rc.first == "0",
              "W2 loops/BUG-547 (for_two_lists): rc 0. Got rc " + rc.first + " `" + rc.second + "`");
      (void)piet::dispatch("gen double d547e = w3 - c*3", session);
      w2check(rOf(session, "d547e", "sum") == "0",
              "W2 loops/BUG-547 (for_two_lists_val): TWO parallel lists substitute in lockstep -- "
              "w3 equals c*3 exactly (the THIRD element of each list together).");
    }

    // for_unequal: the two lists have different lengths (3 vs 2) -> r(198),
    // validation case 1 of 4 (design brief).
    {
      auto r = piet::dispatch(
          "for Q in num 1/3 \\ W in any a b: gen double vQ = W", session);
      auto rc = rcOf(r);
      w2check(rc.first == "198",
              "W2 loops/BUG-547 (for_unequal): unequal list lengths (3 vs 2) is r(198). Got rc " +
                  rc.first + " `" + rc.second + "`");
    }

    // for_no_placeholder: the body never mentions the placeholder X ->
    // r(198), validation case 2 of 4.
    {
      auto r = piet::dispatch("for num 1/2: gen double noph = 1", session);
      auto rc = rcOf(r);
      w2check(rc.first == "198",
              "W2 loops/BUG-547 (for_no_placeholder): the placeholder never occurring in the "
              "command is r(198). Got rc " + rc.first + " `" + rc.second + "`");
    }

    // Design-required, not an oracle arm (cmd-triage.json validation cases 3
    // and 4): a missing colon, and an unrecognised list type.
    {
      auto r = piet::dispatch("for num 1 2 gen double zzz9 = X", session);
      auto rc = rcOf(r);
      w2check(rc.first == "198",
              "W2 loops/BUG-547 (design, not an oracle arm: missing ':'): `for num 1 2 gen "
              "double zzz9 = X` with no colon at all is r(198). Got rc " + rc.first);
    }
    {
      auto r = piet::dispatch("for foo 1 2: gen double zzz8 = X", session);
      auto rc = rcOf(r);
      w2check(rc.first == "198",
              "W2 loops/BUG-547 (design, not an oracle arm: bad list type): TYPE `foo` matches "
              "none of varlist/numlist/newlist/any -> r(198). Got rc " + rc.first);
    }

    // Design-required, not an oracle arm (OPTION VOCABULARY class): an
    // option that abbreviates none of dryrun/noheader/nostop/pause is refused by name with
    // r(198) and nothing iterates. Pre-fix: the typo was silently dropped
    // and the loop ran (rc 0, header echoes emitted).
    {
      auto r = piet::dispatch("for X in num 1/2, noheder: display X", session);
      auto rc = rcOf(r);
      w2check(rc.first == "198" && !has(joinedText(r), "->") && has(rc.second, "noheder"),
              "W2 loops/BUG-547 (design, not an oracle arm: unknown option): `for X in num "
              "1/2, noheder: display X` is r(198) naming the option, with no iteration. Got rc " +
                  rc.first + " `" + rc.second + "` text=[" + joinedText(r) + "]");
    }

    // for_in_quotes: BLIND substitution reaches inside a quoted string.
    // Oracle for_in_quotes_val: sum(is_ev)=6.
    {
      auto r = piet::dispatch("for any ev od: gen byte is_X = s == \"X\"", session);
      auto rc = rcOf(r);
      w2check(rc.first == "0",
              "W2 loops/BUG-547 (for_in_quotes): rc 0. Got rc " + rc.first + " `" + rc.second + "`");
      w2check(rOf(session, "is_ev", "sum") == "6",
              "W2 loops/BUG-547 (for_in_quotes_val): is_ev sums to 6 (oracle value) -- the "
              "placeholder substituted INSIDE the quoted string `\"X\"`, not just outside it. "
              "Got sum=" + rOf(session, "is_ev", "sum"));
    }

    // for_r_per_iteration: r() is refreshed PER ITERATION, before the SAME
    // iteration's next command runs (not frozen at the pre-loop value).
    // mean(b) is carried in a SCALAR (exact double). A local would store
    // Stata's %18.0g text 3.333333333333333, one ULP below mean(b) = 10/3
    // (tests/eval/golden/aer_w2/w2f_residuals.log:120 lt_03; w2i_varsub.log:294
    // a_10 vs :524 b_10), and the delta would sum to -5.3e-15 in Stata too.
    // `quietly summarize c` moves the pre-loop r(mean) to mean(c) = 2.1666...,
    // so an r() frozen before the loop fails (delta sum 14), as does one
    // frozen at iteration 1's mean(a) (sum -77).
    {
      (void)piet::dispatch("quietly summarize b", session);
      (void)piet::dispatch("scalar mb547 = r(mean)", session);
      (void)piet::dispatch("quietly summarize c", session);
      auto r = piet::dispatch(
          "for var a b: quietly summarize X \\ gen double cX = X - r(mean)", session);
      auto rc = rcOf(r);
      w2check(rc.first == "0",
              "W2 loops/BUG-547 (for_r_per_iteration): rc 0. Got rc " + rc.first + " `" +
                  rc.second + "`");
      (void)piet::dispatch("gen double d547f = cb - (b - scalar(mb547))", session);
      w2check(rOf(session, "d547f", "sum") == "0",
              "W2 loops/BUG-547 (for_r_per_iteration_val): cb equals b - mean(b) exactly -- "
              "r(mean) was refreshed by THIS iteration's `quietly summarize X`, not left at some "
              "earlier value (a whole-line-upfront macro expansion would have frozen it).");
      // Oracle record, tests/eval/golden/aer_w2/w2_cmdtriage.log:203-204,
      // replayed in its own spelling.
      (void)piet::dispatch("capture noisily summarize cb", session);
      const std::string rec = lastDisplayText(piet::dispatch(
          "capture noisily display \"PREC|for_r_per_iteration_val|\" _rc \"|\" %18.0g r(max)",
          session));
      w2check(rec == "PREC|for_r_per_iteration_val|0| 2.666666666666667",
              "W2 loops/BUG-547 (for_r_per_iteration_val_record): byte-equal to "
              "w2_cmdtriage.log:204 `PREC|for_r_per_iteration_val|0| 2.666666666666667`. Got `" +
                  rec + "`");
    }

    // for_macro_name_has_X: the placeholder substitution must NOT corrupt a
    // macro REFERENCE's name -- `$XC` stays `$XC` (global XC="b" from the
    // fixture), never becomes `$aC`.
    {
      auto r = piet::dispatch("for any a: gen double gX = $XC + X", session);
      auto rc = rcOf(r);
      w2check(rc.first == "0",
              "W2 loops/BUG-547 (for_macro_name_has_X): rc 0. Got rc " + rc.first + " `" +
                  rc.second + "`");
      (void)piet::dispatch("gen double d547g = ga - (b + a)", session);
      w2check(rOf(session, "d547g", "sum") == "0",
              "W2 loops/BUG-547 (for_macro_name_has_X_val): ga equals b+a exactly -- `$XC` was "
              "NOT touched by the blind substitution of placeholder `X` even though the macro "
              "NAME `XC` contains an `X`. A corrupted `$aC` would have left ga == a (undefined "
              "global expands empty), which this delta check would have caught.");
    }

    // for_stop_on_error / for_nostop: default STOPS the loop at the first
    // error (the next element's variable is never created); `nostop` tags
    // the error captured and continues (all elements' variables exist, rc 0).
    //
    // NOTE: this build's `gen` does not yet map an undefined-identifier
    // expression error to Stata's own r(111) (a pre-existing gap in the
    // expression evaluator's message text, outside this lane's manifest --
    // `confirm variable` already does, verified below) -- so only the STOP
    // vs CONTINUE behaviour and the nostop _rc=0 are asserted here, not the
    // erroring iteration's own exact code.
    {
      auto r = piet::dispatch("for any a nosuch b: gen double eX = X", session);
      auto rc = rcOf(r);
      w2check(rc.first != "0",
              "W2 loops/BUG-547 (for_stop_on_error): the loop halts at the `nosuch` element's "
              "error (a nonzero, uncaptured rc). Got rc " + rc.first);
      auto confirmEb = rcOf(piet::dispatch("confirm variable eb", session));
      w2check(confirmEb.first == "111",
              "W2 loops/BUG-547 (for_stop_on_error_eb): `eb` was NEVER created -- the loop "
              "stopped before the third element ran. `confirm variable eb` is r(111). Got rc " +
                  confirmEb.first);
    }
    {
      auto r = piet::dispatch("for any a nosuch b, nostop: gen double hX = X", session);
      auto rc = rcOf(r);
      w2check(rc.first == "0",
              "W2 loops/BUG-547 (for_nostop): with `nostop` the middle element's error is "
              "captured and the loop continues -- rc 0 overall (oracle value). Got rc " +
                  rc.first + " `" + rc.second + "`");
      w2check(rcOf(piet::dispatch("confirm variable ha", session)).first == "0" &&
                  rcOf(piet::dispatch("confirm variable hb", session)).first == "0",
              "W2 loops/BUG-547 (for_nostop_hb): BOTH `ha` and `hb` exist -- every element ran "
              "despite the middle one erroring.");
    }

    // for_by_colon_noheader: the header/body split is on the FIRST unquoted
    // ':' only -- the body itself contains one (`bysort g: egen ...`) -- and
    // `noheader` suppresses the `->  ` echo lines.
    {
      auto r = piet::dispatch(
          "for num 1 2, noheader: bysort g: egen double mX = max(a*X)", session);
      auto rc = rcOf(r);
      w2check(rc.first == "0",
              "W2 loops/BUG-547 (for_by_colon_noheader): a colon INSIDE the body does not end "
              "the header early -- rc 0. Got rc " + rc.first + " `" + rc.second + "`");
      w2check(!has(joinedText(r), "->"),
              "W2 loops/BUG-547 (for_by_colon_noheader, noheader): the `->  ` echo is suppressed "
              "under `noheader`. Got: [" + joinedText(r) + "]");
      (void)piet::dispatch("bysort g: egen double m2ctl = max(a*2)", session);
      (void)piet::dispatch("gen double d547h = m2 - m2ctl", session);
      w2check(rOf(session, "d547h", "sum") == "0",
              "W2 loops/BUG-547 (for_by_colon_val): m2 equals the SAME `bysort g: egen ... max("
              "a*2)` run directly -- the whole colon-bearing body text forwarded unchanged apart "
              "from the placeholder substitution.");
    }

    // Option ABBREVIATIONS (OPTION VOCABULARY class). StataBE 19.5: the
    // shortest accepted spellings are `d` (dryrun), `noh` (noheader), `nos`
    // (nostop) and `p` (pause), case-sensitive. Pre-fix: each was refused
    // with r(198) (a closed list of full names only).
    {
      auto r = piet::dispatch("for num 1 2, nohead: gen double nh547X = X", session);
      auto rc = rcOf(r);
      w2check(rc.first == "0" && !has(joinedText(r), "->"),
              "W2 loops/BUG-547 (noheader abbrev): `, nohead` is `noheader` -- rc 0 "
              "and no `->` echo. Got rc " + rc.first + " `" + rc.second + "` text=[" +
                  joinedText(r) + "]");
    }
    {
      auto r = piet::dispatch("for any a nosuch b, nos: gen double ns547X = X", session);
      auto rc = rcOf(r);
      w2check(rc.first == "0" &&
                  rcOf(piet::dispatch("confirm variable ns547b", session)).first == "0",
              "W2 loops/BUG-547 (nostop abbrev): `, nos` is `nostop` -- the loop "
              "continues past the `nosuch` error, rc 0, ns547b exists. Got rc " + rc.first +
                  " `" + rc.second + "`");
    }
    {
      auto r = piet::dispatch("for num 1 2, d p: gen double dr547X = X", session);
      auto rc = rcOf(r);
      w2check(rc.first == "0" &&
                  rcOf(piet::dispatch("confirm variable dr5471", session)).first == "111",
              "W2 loops/BUG-547 (dryrun/pause abbrev): `, d p` is `dryrun pause` -- "
              "rc 0 and nothing executes (dr5471 never created). Got rc " + rc.first + " `" +
                  rc.second + "`");
    }
  }

  // ── BUG-1338: a failed inline `: fn'/`=exp' under capture and quietly ─────
  // The message of an inline extended function or `=exp' that failed prints
  // under quietly, from any depth, and under capture only for the capture
  // line's own expansion (StataBE 19.5, 2026-11-03): `capture myq` (myq's body
  // expands `=nosuchZ'), `capture { }` and `capture foreach` print nothing;
  // `capture display`, `capture noisily { }`, `quietly { }` and `quietly
  // foreach` print the message. The plain-capture leg leaked the body's
  // message once capture kept these blocks.
  {
    piet::SessionState session(workdir);
    auto r = piet::doexec::executeDoSource(
        "program define st1338q\n"
        "display \"Q|`=nosuchZ1338'|\"\n"
        "end\n"
        "capture st1338q\n"
        "capture {\n"
        "display \"`: type nosuchB1338'\"\n"
        "}\n"
        "capture foreach v in a {\n"
        "display \"`: type nosuchE1338'\"\n"
        "}\n"
        "capture display \"`: type nosuchI1338'\"\n"
        "capture noisily {\n"
        "display \"`: type nosuchH1338'\"\n"
        "}\n"
        "quietly {\n"
        "display \"`: type nosuchC1338'\"\n"
        "}\n"
        "quietly foreach v in a {\n"
        "display \"`: type nosuchD1338'\"\n"
        "}\n",
        session, piet::dispatch);
    const std::string t = joinedText(r.blocks);
    w2check(!has(t, "nosuchZ1338") && !has(t, "nosuchB1338") && !has(t, "nosuchE1338") &&
                has(t, "variable nosuchI1338 not found") &&
                has(t, "variable nosuchH1338 not found") &&
                has(t, "variable nosuchC1338 not found") &&
                has(t, "variable nosuchD1338 not found"),
            "W2 loops/BUG-1338: inline-expansion messages are silent inside a captured program, "
            "block or loop and shown for the capture line itself, under capture noisily and "
            "under quietly. Got text=[" + t + "]");
  }

  fs::remove_all(workdir, ec);
}
