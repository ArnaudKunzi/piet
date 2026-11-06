// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// W2 lane `labelglued` -- selftest arms (2026-09-19).
//
// Oracle: tests/eval/golden/aer_w2/w2_cmdtriage.log (StataBE 19.5), sections
//   B  label subcommand abbreviations (BUG-524) and `label language NAME,
//      new` (BUG-527),
//   D  a capture/quietly prefix delivered through a macro (BUG-526),
//   E  a command word glued to `(` or `"`, and the parenthesised varlist that
//      the glued form delivers (BUG-525).
// Every label names the oracle arm it rests on and says what main answered
// before the fix, so each arm is a discriminator. Numbers asserted here are
// computed on the oracle's own fixture (w2_cmdtriage.do lines 4-12) so they are
// the oracle's numbers, not this build's. Floors that come from `help label`
// rather than an arm say so.
#include "selftest_w2.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <utility>
#include <vector>

#include "piet/dispatcher.h"
#include "piet/eval/errors.h"
#include "piet/session_state.h"
#include "vendor/nlohmann/json.hpp"

namespace {

using ojson = nlohmann::ordered_json;

// The oracle's fixture: 12 rows, closed-form columns, no randomness.
void seedOracleData(piet::SessionState& session) {
  for (const char* s : {"clear", "set obs 12", "generate double id = _n",
                        "generate double g = mod(_n, 3)", "generate double a = _n*1.5",
                        "generate double b = mod(_n*5, 7)", "generate double c = mod(_n*7, 5)",
                        "generate byte f = mod(_n, 2)"}) {
    (void)piet::dispatch(s, session);
  }
}

// The rc of ONE dispatched line as a do-file would see it: the coarse rc of the
// first UNCAPTURED error block, "0" when there is none. A wrong shape (not an
// array, a non-object block, a field of the wrong type) comes back as "shape"
// so the arm FAILS instead of the binary aborting (lane brief, defect class 1).
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

// Text of the first display block; "" when there is none or the shape is wrong.
std::string displayTextOf(const ojson& blocks) {
  if (!blocks.is_array()) return "";
  for (const auto& b : blocks) {
    if (!b.is_object()) continue;
    try {
      if (b.value("type", std::string()) == "display") return b.value("text", std::string());
    } catch (const std::exception&) {
      return "";
    }
  }
  return "";
}

// True iff every block is an echo: what a `quietly`-prefixed success looks like.
bool onlyEchoes(const ojson& blocks) {
  if (!blocks.is_array()) return false;
  for (const auto& b : blocks) {
    if (!b.is_object()) return false;
    try {
      if (b.value("type", std::string()) != "echo") return false;
    } catch (const std::exception&) {
      return false;
    }
  }
  return true;
}

std::string trimmed(const std::string& s) {
  const auto b = s.find_first_not_of(" \t");
  const auto e = s.find_last_not_of(" \t");
  if (b == std::string::npos) return "";
  return s.substr(b, e - b + 1);
}

}  // namespace

void testW2LabelGlued() {
  std::printf("[selftest] --- W2 labelglued: label abbreviations, macro-led prefixes, glued heads "
              "(w2_cmdtriage.log) ---\n");
  namespace fs = std::filesystem;
  const std::string workdir =
      (fs::temp_directory_path() / "piet_native_selftest_w2labelglued").u8string();
  std::error_code ec;
  fs::remove_all(workdir, ec);
  fs::create_directories(workdir, ec);

  // ── B. label subcommand abbreviations (BUG-524) ─────────────────────────
  {
    piet::SessionState session(workdir);
    seedOracleData(session);
    auto valueLabelOf = [&](const std::string& var) {
      (void)piet::dispatch("local vl : value label " + var, session);
      return session.eval.locals.get("vl");
    };

    // The full spelling is each abbreviation's control (label_define_full).
    auto r = rcOf(piet::dispatch("label define yn 0 \"no\" 1 \"yes\"", session));
    w2check(r.first == "0", "W2 labelglued/B (label_define_full): `label define` is rc 0. Got rc " +
                                r.first + " `" + r.second + "`");

    // label_def / la_de: rc 0. Main answered r(199) "Command `label def` is not
    // supported ... this subcommand is not" for both.
    r = rcOf(piet::dispatch("label def yn2 0 \"no\" 1 \"yes\"", session));
    w2check(r.first == "0" && session.labelDefs.count("yn2") == 1 &&
                session.labelDefs["yn2"].size() == 2,
            "W2 labelglued/B (label_def): `label def yn2 0 \"no\" 1 \"yes\"` is rc 0 AND defines "
            "the two-entry label -- the accept has to reach runLabelDefine, whose raw anchor is "
            "`^label\\s+define\\s+`. Main: r(199) unknown subcommand. Got rc " +
                r.first + " `" + r.second + "` defs=" + std::to_string(session.labelDefs.count("yn2")));
    r = rcOf(piet::dispatch("la de yn3 0 \"no\" 1 \"yes\"", session));
    w2check(r.first == "0" && session.labelDefs.count("yn3") == 1,
            "W2 labelglued/B (la_de): `la de yn3 ...` -- the command word at ITS floor (`la`, the "
            "registry row) and the subcommand at its floor (`de`) together -- is rc 0 and defines "
            "yn3. Main: r(199). Got rc " + r.first + " `" + r.second + "`");

    // label_d_too_short: ONE character is below define's floor -> r(198)
    // "invalid syntax", and nothing is defined. Main: r(199) (wrong code, wrong
    // diagnosis: the subcommand IS `define`, typed too short).
    r = rcOf(piet::dispatch("label d yn4 0 \"no\"", session));
    w2check(r.first == "198" && session.labelDefs.count("yn4") == 0,
            "W2 labelglued/B (label_d_too_short): `label d yn4 0 \"no\"` is r(198) invalid syntax "
            "-- `d` is below define's 2-character floor (and is also a prefix of data/dir/drop) "
            "-- and yn4 is NOT defined. Main: r(199). Got rc " + r.first + " `" + r.second +
                "` defs=" + std::to_string(session.labelDefs.count("yn4")));

    // label_values_full: control.
    r = rcOf(piet::dispatch("label values f yn", session));
    w2check(r.first == "0" && valueLabelOf("f") == "yn",
            "W2 labelglued/B (label_values_full): `label values f yn` attaches yn (`: value label "
            "f` reads yn). Got rc " + r.first + " vl=" + valueLabelOf("f"));
    // label_val: the 3-character floor. Main: r(199) and f kept yn.
    r = rcOf(piet::dispatch("label val f yn2", session));
    w2check(r.first == "0" && valueLabelOf("f") == "yn2",
            "W2 labelglued/B (label_val): `label val f yn2` is rc 0 and f now carries yn2 -- 519 "
            "corpus statements spell it this way. Main: r(199), f still yn. Got rc " + r.first +
                " vl=" + valueLabelOf("f"));
    // label_value: any prefix at or above the floor, up to the full word.
    r = rcOf(piet::dispatch("label value f yn3", session));
    w2check(r.first == "0" && valueLabelOf("f") == "yn3",
            "W2 labelglued/B (label_value): `label value f yn3` is rc 0 and f carries yn3. Main: "
            "r(199). Got rc " + r.first + " vl=" + valueLabelOf("f"));
    // label_va_too_short: r(198) and f KEEPS yn3 (the oracle reads `: value
    // label f` after the failed line and gets yn3, not yn).
    r = rcOf(piet::dispatch("label va f yn", session));
    w2check(r.first == "198" && valueLabelOf("f") == "yn3",
            "W2 labelglued/B (label_va_too_short): `label va f yn` is r(198) invalid syntax and f "
            "still carries yn3 -- two characters are below values' floor (and `va` is also a "
            "prefix of `variable`). Main: r(199). Got rc " + r.first + " vl=" + valueLabelOf("f"));

    // Floors the ORACLE does not carry, taken from `help label`'s syntax block
    // (help label marks the minimum abbreviations l:ist, di:r and da:ta). Said
    // plainly: these three rest on the published definition, not on an arm.
    // Main: r(199) for each.
    r = rcOf(piet::dispatch("lab li yn", session));
    w2check(r.first == "0",
            "W2 labelglued/B (help label `{opt l:ist}`, not an oracle arm): `lab li yn` is rc 0. "
            "Main: r(199). Got rc " + r.first + " `" + r.second + "`");
    r = rcOf(piet::dispatch("label di", session));
    w2check(r.first == "0",
            "W2 labelglued/B (help label `{opt di:r}`, not an oracle arm): `label di` is rc 0. "
            "Main: r(199). Got rc " + r.first + " `" + r.second + "`");
    r = rcOf(piet::dispatch("la data \"Oracle fixture\"", session));
    w2check(r.first == "0" && session.dataLabel == "Oracle fixture",
            "W2 labelglued/B (help label `{opt da:ta}`, not an oracle arm): `la data \"Oracle "
            "fixture\"` is rc 0 and session.dataLabel -- the field the .dta writer already carries "
            "-- holds the text. Main: r(199), no mutator at all. Got rc " + r.first +
                " label=`" + session.dataLabel + "`");
    r = rcOf(piet::dispatch("label data", session));
    w2check(r.first == "0" && session.dataLabel.empty(),
            "W2 labelglued/B (help label: \"If no label is specified, any existing label is "
            "removed\"): bare `label data` clears it. Got rc " + r.first + " label=`" +
                session.dataLabel + "`");

    // lang_bare / lang_default / lang_undefined / lang_new. Main: r(199) for all.
    ojson lb = piet::dispatch("label language", session);
    r = rcOf(lb);
    w2check(r.first == "0" && session.eval.rStrings.get("language") == "default" &&
                session.eval.rStrings.get("languages") == "default",
            "W2 labelglued/B (lang_bare): bare `label language` is rc 0 with r(language)=default "
            "and r(languages)=default. Main: r(199). Got rc " + r.first + " r(language)=`" +
                session.eval.rStrings.get("language") + "`");
    r = rcOf(piet::dispatch("label language default", session));
    w2check(r.first == "0",
            "W2 labelglued/B (lang_default): `label language default` is rc 0 (\"default already "
            "current language\"). Main: r(199). Got rc " + r.first);
    r = rcOf(piet::dispatch("label language english", session));
    w2check(r.first == "111" && r.second.find("language english not defined") != std::string::npos,
            "W2 labelglued/B (lang_undefined): `label language english` is r(111) \"language "
            "english not defined\". Main: r(199). Got rc " + r.first + " `" + r.second + "`");
    ojson lnBlk = piet::dispatch("label language english, new", session);
    r = rcOf(lnBlk);
    w2check(r.first == "0" &&
                displayTextOf(lnBlk) == "(language english now current language)",
            "W2 labelglued/B (lang_new, BUG-527): `label language english, new` is rc 0 in the "
            "oracle (\"(language english now current language)\"). Main: r(199). Got rc " +
                r.first + " display=`" + displayTextOf(lnBlk) + "`");

    // The unknown-subcommand refusal survives the canonicaliser: not a prefix
    // of anything documented -> still the BUG-105 arm-3 wording, rc 199.
    r = rcOf(piet::dispatch("label nosuchsub f", session));
    w2check(r.first == "199" && r.second.find("this subcommand is not") != std::string::npos,
            "W2 labelglued/B: `label nosuchsub f` is still the unknown-subcommand refusal (BUG-105 "
            "arm 3 pins its wording). Got rc " + r.first + " `" + r.second + "`");
  }

  // ── D. a prefix delivered through a macro (BUG-526) ─────────────────────
  {
    piet::SessionState session(workdir);
    seedOracleData(session);
    // macro_quietly: `local q quietly` then `` `q' summarize a `` is rc 0, r(N)
    // is 12, and the output is quiet. Main: r(199) "Command `quietly` is not
    // supported" -- the expanded head was looked up as a command -- and r(N)
    // stayed unset.
    (void)piet::dispatch("local q quietly", session);
    ojson mq = piet::dispatch("`q' summarize a", session);
    auto r = rcOf(mq);
    const std::string rN = trimmed(displayTextOf(piet::dispatch("display r(N)", session)));
    w2check(r.first == "0" && rN == "12",
            "W2 labelglued/D (macro_quietly): `` `q' summarize a `` with q=quietly is rc 0 and "
            "leaves r(N)=12, so the summarize really ran under the prefix. Main: r(199) `quietly` "
            "as a command, r(N) unset. Got rc " + r.first + " `" + r.second + "` r(N)=`" + rN + "`");
    w2check(onlyEchoes(mq),
            "W2 labelglued/D (macro_quietly): ...and the output is QUIET -- nothing but the typed "
            "echo comes back, exactly as the literal `quietly summarize a` returns. The inner "
            "dispatch's own echo of the expanded line is dropped (Stata logs the line as typed).");

    // global_qui: paper 073's 2,981-line idiom. Main: r(199) `qui` as a command.
    (void)piet::dispatch("global qui \"qui\"", session);
    r = rcOf(piet::dispatch("${qui} generate double mq = 1", session));
    (void)piet::dispatch("count if mq == 1", session);
    const std::string nMq = trimmed(displayTextOf(piet::dispatch("display r(N)", session)));
    w2check(r.first == "0" && nMq == "12",
            "W2 labelglued/D (global_qui): `${qui} generate double mq = 1` with $qui=qui is rc 0 "
            "and creates mq (12 rows equal 1). Main: r(199). Got rc " + r.first + " `" + r.second +
                "` count=`" + nMq + "`");

    // ctl_drop_missing / macro_capture: the bare drop is r(111); through
    // `local cp capture` the same line is rc 0 and _rc holds the 111 the
    // capture swallowed. Main: r(199) `capture` as a command.
    r = rcOf(piet::dispatch("drop nosuchvar", session));
    w2check(r.first == "111",
            "W2 labelglued/D (ctl_drop_missing): `drop nosuchvar` is r(111) -- the control the "
            "macro-capture arm is measured against. Got rc " + r.first);
    (void)piet::dispatch("local cp capture", session);
    r = rcOf(piet::dispatch("`cp' drop nosuchvar", session));
    const std::string rcLocal = session.eval.locals.get("_rc");
    w2check(r.first == "0" && rcLocal == "111",
            "W2 labelglued/D (macro_capture): `` `cp' drop nosuchvar `` with cp=capture is rc 0 "
            "(no uncaptured error) and _rc==111 -- the prefix did its job on the same 111 the "
            "control shows. Main: r(199). Got rc " + r.first + " `" + r.second + "` _rc=`" +
                rcLocal + "`");
  }

  // ── E. glued heads and the parenthesised varlist (BUG-525) ──────────────
  {
    piet::SessionState session(workdir);
    seedOracleData(session);
    // di_glued: prints `glued`, rc 0. Main: r(199) "Command `di(\"glued\")`".
    ojson dg = piet::dispatch("di(\"glued\")", session);
    auto r = rcOf(dg);
    w2check(r.first == "0" && trimmed(displayTextOf(dg)) == "glued",
            "W2 labelglued/E (di_glued): `di(\"glued\")` prints glued, rc 0 -- the command word "
            "ends at `(`. Main: r(199). Got rc " + r.first + " text=`" + displayTextOf(dg) + "`");

    // ctl_corr / corr_glued: the same r(rho) both ways, .0171895075182102 on the
    // oracle's rows (%18.0g). Main: r(199) "Command `corr(a`" -- the word was
    // cut at whitespace, and even spaced, `(a` reached the engine as a name.
    r = rcOf(piet::dispatch("corr a c", session));
    const std::string rhoCtl =
        trimmed(displayTextOf(piet::dispatch("display %18.0g r(rho)", session)));
    w2check(r.first == "0" && rhoCtl == ".0171895075182102",
            "W2 labelglued/E (ctl_corr): `corr a c` on the oracle's rows gives r(rho) "
            ".0171895075182102 -- the control. Got rc " + r.first + " rho=`" + rhoCtl + "`");
    session.eval.rValues = piet::eval::OrderedFloatDict();  // r(rho) below is corr(a c)'s own
    r = rcOf(piet::dispatch("corr(a c)", session));
    const std::string rhoGlued =
        trimmed(displayTextOf(piet::dispatch("display %18.0g r(rho)", session)));
    w2check(r.first == "0" && rhoGlued == ".0171895075182102",
            "W2 labelglued/E (corr_glued): `corr(a c)` is rc 0 with the SAME r(rho) -- the head "
            "is split AND the one paren pair around the plain varlist is unwrapped at the "
            "varlist site. Main: r(199) `corr(a`. Got rc " + r.first + " `" + r.second +
                "` rho=`" + rhoGlued + "`");

    // levelsof_glued: `0 1 2` into the local. Main: r(199) `levelsof(g)`.
    r = rcOf(piet::dispatch("levelsof(g), local(lv)", session));
    const std::string lv = session.eval.locals.get("lv");
    w2check(r.first == "0" && lv == "0 1 2",
            "W2 labelglued/E (levelsof_glued): `levelsof(g), local(lv)` is rc 0 and lv is `0 1 2`. "
            "Main: r(199). Got rc " + r.first + " `" + r.second + "` lv=`" + lv + "`");

    // Review fix round (finding 2): the paren-varlist unwrap at the shared
    // varlist site is gated to corr/levelsof ONLY (kParenVarlistUnwrap in
    // dispatcher.cpp), because those are the only two rows the oracle has an
    // arm for. `summarize` is also a plain-varlist row (kPlainVarlist in
    // registry.cpp) but has no oracle arm for a parenthesised varlist, so
    // `summarize (a c)` must keep answering r(111) "variable (a not found" --
    // measured on the frozen main binary -- rather than silently being
    // unwrapped into a passing summarize the same fix would give corr.
    r = rcOf(piet::dispatch("summarize (a c)", session));
    w2check(r.first == "111" && r.second.find("(a not found") != std::string::npos,
            "W2 labelglued/E (summarize_paren_not_unwrapped): `summarize (a c)` stays r(111) "
            "\"variable (a not found\" -- the corr/levelsof paren-unwrap must not leak onto other "
            "plain-varlist rows with no oracle arm. Got rc " + r.first + " `" + r.second + "`");

    // erase_glued: the quote glued to the word. The oracle erases the file it
    // just saved; PIET's `save` writes .hbin, so the file is made here and its
    // absolute path is glued the same way. Main: r(199), file left in place.
    const std::string victim = (fs::path(workdir) / "ct_tmp.txt").u8string();
    {
      std::ofstream os(victim, std::ios::binary);
      os << "erase me\n";
    }
    r = rcOf(piet::dispatch("erase\"" + victim + "\"", session));
    w2check(r.first == "0" && !fs::exists(victim),
            "W2 labelglued/E (erase_glued): `erase\"<file>\"` is rc 0 and the file is gone -- the "
            "command word ends at `\"` too. Main: r(199), file still there. Got rc " + r.first +
                " `" + r.second + "` exists=" + (fs::exists(victim) ? "yes" : "no"));
  }

  // w2ao_label_family lb_rt_lang / lb_rt_lang_switch / lb_rt_nolabel.
  // Use a new session for DTA to prove the language stash is in the file,
  // rather than surviving accidentally in a save-path cache.
  {
    const std::string path = (fs::path(workdir) / "labels,roundtrip.dta").u8string();
    piet::SessionState source(workdir);
    bool setup = true;
    for (const char* cmd : {"clear", "set obs 3", "generate byte rt = _n",
                            "label define rtl 1 one 2 two 3 three",
                            "label values rt rtl", "label variable rt \"Round trip var\"",
                            "label data \"Round trip data\"", "label language english, new copy",
                            "label variable rt \"English label\"", "label data \"English data\"",
                            "label define engonly 1 first 2 second 3 third",
                            "label values rt engonly", "label language default"})
      setup = rcOf(piet::dispatch(cmd, source)).first == "0" && setup;
    const auto saved = rcOf(piet::dispatch("save \"" + path + "\", replace", source));
    piet::SessionState loaded(workdir);
    const auto used = rcOf(piet::dispatch("use \"" + path + "\", clear", loaded));
    (void)piet::dispatch("label language", loaded);
    (void)piet::dispatch("local __label : variable label rt", loaded);
    w2check(setup && saved.first == "0" && used.first == "0" &&
                loaded.labelLang == "default" && loaded.labelLangs == std::vector<std::string>{"default", "english"} &&
                loaded.eval.locals.get("__label") == "Round trip var" && loaded.dataLabel == "Round trip data",
            "W2 labels: DTA round trip preserves current language, language list and active labels");
    const auto switched = rcOf(piet::dispatch("label language english", loaded));
    (void)piet::dispatch("local __label : variable label rt", loaded);
    w2check(switched.first == "0" && loaded.labelLang == "english" &&
                loaded.eval.locals.get("__label") == "English label" && loaded.dataLabel == "English data" &&
                loaded.varLblname["rt"] == "engonly" && loaded.labelDefs.count("engonly") &&
                loaded.labelDefs["engonly"]["2"] == "second",
            "W2 labels: switching after DTA use restores inactive labels and an alternate-only value-label set");
    // Re-save with English current: stale imported _lang_* characteristics
    // must not override the new current language on the next load.
    const auto savedAgain = rcOf(piet::dispatch("save \"" + path + "\", replace", loaded));
    const auto usedAgain = rcOf(piet::dispatch("use \"" + path + "\", clear", loaded));
    const auto back = rcOf(piet::dispatch("label language default", loaded));
    (void)piet::dispatch("local __label : variable label rt", loaded);
    w2check(savedAgain.first == "0" && usedAgain.first == "0" && back.first == "0" &&
                loaded.eval.locals.get("__label") == "Round trip var" && loaded.varLblname["rt"] == "rtl",
            "W2 labels: switching then saving rebuilds language characteristics from current state");
    const std::string defaultPath = (fs::path(workdir) / "labels-default.dta").u8string();
    (void)piet::dispatch("save \"" + defaultPath + "\", replace", loaded);
    const auto noLabels = rcOf(piet::dispatch("use \"" + defaultPath + "\", clear nolabel", loaded));
    (void)piet::dispatch("local __label : variable label rt", loaded);
    (void)piet::dispatch("local __fallback : label rtl 2", loaded);
    w2check(noLabels.first == "0" && loaded.eval.locals.get("__label") == "Round trip var" &&
                loaded.varLblname["rt"] == "rtl" && loaded.eval.locals.get("__fallback") == "2" &&
                loaded.labelDefs.empty() && loaded.valueLabels.empty(),
            "W2 labels: use clear nolabel keeps variable labels and attachments, omits all definitions");
    // Native checkpoints carry the same language state in their session record.
    const std::string native = (fs::path(workdir) / "labels-native.hbin").u8string();
    (void)piet::dispatch("use \"" + defaultPath + "\", clear", loaded);
    const auto nativeSaved = rcOf(piet::dispatch("save \"" + native + "\", replace", loaded));
    (void)piet::dispatch("clear", loaded);
    const auto nativeUsed = rcOf(piet::dispatch("use \"" + native + "\", clear", loaded));
    const auto nativeSwitch = rcOf(piet::dispatch("label language english", loaded));
    (void)piet::dispatch("local __label : variable label rt", loaded);
    w2check(nativeSaved.first == "0" && nativeUsed.first == "0" && nativeSwitch.first == "0" &&
                loaded.eval.locals.get("__label") == "English label" && loaded.dataLabel == "English data",
            "W2 labels: native checkpoint restores its label-language state after clear");
  }

  fs::remove_all(workdir, ec);
}
