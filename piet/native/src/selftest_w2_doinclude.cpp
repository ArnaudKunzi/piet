// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// W2 lane `doinclude` -- `include` exists and shares the caller's locals
// (BUG-528), the do/run/include path is macro-expanded and defaults to `.do`
// (BUG-529), do/run get a PRIVATE local scope (BUG-530), and `exit N` inside a
// nested file reaches the caller as r(N) (BUG-531).
//
// Oracle: tests/eval/golden/aer_w2/w2_include_scope.log (StataBE 19.5). The
// helper files below are that do-file's own helpers, byte for byte in content,
// written into a temp dir; each arm names the oracle line it rests on. Every arm
// fails on main (74daf54f): `include` was refused (no_data_loaded, rc 111, or
// unsupported_command), do/run leaked locals both ways and clobbered the
// caller's `1', `do "$D/x.do"` and `do x` answered r(601), and `exit 5` inside a
// nested do-file gave the caller rc 0.
#include "selftest_w2.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "io/win_compat.h"
#include "piet/dispatcher.h"
#include "piet/doexec.h"
#include "piet/eval/errors.h"
#include "piet/session_state.h"
#include "vendor/nlohmann/json.hpp"

namespace {

using ojson = nlohmann::ordered_json;

struct Ran {
  std::string rc = "0";           // the session's _rc after the line (the oracle's PREC)
  std::string msg;                // the last error block's message
  std::vector<std::string> shown; // display texts, in order
};

Ran collect(const ojson& blocks) {
  Ran r;
  for (const auto& b : blocks) {
    if (!b.is_object()) continue;
    const std::string type = b.value("type", std::string());
    if (type == "error") {
      r.msg = b.value("message", std::string());
    } else if (type == "display") {
      r.shown.push_back(b.value("text", std::string()));
    }
  }
  return r;
}

bool showed(const Ran& r, const std::string& text) {
  for (const auto& s : r.shown) {
    if (s == text) return true;
  }
  return false;
}

std::string joined(const Ran& r) {
  std::string s;
  for (const auto& t : r.shown) s += "[" + t + "]";
  return s;
}

void writeFile(const std::string& path, const std::string& body) {
  std::ofstream f(cecon::to_native_path(path), std::ios::binary);
  f << body;
}

}  // namespace

void testW2DoInclude() {
  std::printf("[selftest] --- W2 doinclude: include / do / run scope, paths, exit (w2_include_scope.log) ---\n");
  namespace fs = std::filesystem;
  const std::string wd = (fs::temp_directory_path() / "piet_native_selftest_w2_doinclude").u8string();
  std::error_code ec;
  fs::remove_all(wd, ec);
  fs::create_directories(wd + "/incsub", ec);

  // The oracle's helpers (w2_include_scope.do, `file write` block).
  writeFile(wd + "/inc_h1.do",
            "display \"H1|sees=[`outer']|\"\nlocal inner = 42\nlocal outer \"changed\"\n");
  writeFile(wd + "/incsub/inc_h2.do", "display \"H2|ran|\"\n");
  writeFile(wd + "/inc_h3.do", "display \"H3|1=[`1']|0=[`0']|\"\n");
  writeFile(wd + "/inc_h3w.do",
            "do inc_h3.do extra\ndisplay \"H3W|after_do|1=[`1']|\"\ninclude inc_h3.do\n");
  writeFile(wd + "/inc_h4err.do", "local e_before = 1\nerror 7\nlocal e_after = 1\n");
  writeFile(wd + "/inc_h4exit.do", "local x_before = 1\nexit\nlocal x_after = 1\n");
  writeFile(wd + "/inc_h4exit5.do", "local x5_before = 1\nexit 5\nlocal x5_after = 1\n");
  writeFile(wd + "/inc_h5.do",
            "local mid \"midval\"\ninclude inc_h6.do\ndisplay \"H5|deep=[`deep']|\"\n");
  writeFile(wd + "/inc_h6.do",
            "display \"H6|mid=[`mid']|outer=[`outer']|\"\nlocal deep \"deepval\"\n");
  writeFile(wd + "/incsub/inc_h7.do",
            "capture noisily include inc_h2.do\ndisplay \"H7|sibling_rc=\" _rc \"|\"\n");
  writeFile(wd + "/inc_h8.do", "local acc \"`acc' `k'\"\n");

  // One fresh session per arm, cd'd into the helpers' dir, no dataset loaded
  // (the oracle runs after `clear`: include must not hit the no-data guard).
  auto fresh = [&](piet::SessionState& s) { (void)piet::dispatch("cd \"" + wd + "\"", s); };
  // Every oracle arm is `capture noisily <verb> ...` followed by `display _rc`,
  // so the rc asserted is the session's _rc (a nested, already-captured error
  // in the stream must not count -- G's inner r(601) is one).
  auto run = [](piet::SessionState& s, const std::string& cmd) {
    Ran r = collect(piet::dispatch(cmd, s));
    r.rc = s.eval.locals.get("_rc", "0");
    return r;
  };
  auto loc = [](piet::SessionState& s, const std::string& name) {
    return s.eval.locals.get(name);
  };

  // ---- A. scope ----
  for (const char* verb : {"do", "run"}) {
    piet::SessionState s(wd);
    fresh(s);
    (void)piet::dispatch("local outer \"orig\"", s);
    Ran r = run(s, std::string("capture noisily ") + verb + " inc_h1.do");
    const std::string arm = std::string(verb) + "_basic";
    w2check(r.rc == "0" && showed(r, "H1|sees=[]|"),
            "W2 doinclude(" + arm + "): the file does NOT see the caller's `outer' -- oracle "
            "`H1|sees=[]|`, PREC 0. Got rc " + r.rc + " " + joined(r));
    w2check(loc(s, "inner").empty() && loc(s, "outer") == "orig",
            "W2 doinclude(" + arm + "): nothing leaks back -- oracle `VAL|" + arm +
                "|inner=[]|outer=[orig]|`. Got inner=[" + loc(s, "inner") + "] outer=[" +
                loc(s, "outer") + "]");
  }
  {
    piet::SessionState s(wd);
    fresh(s);
    (void)piet::dispatch("local outer \"orig\"", s);
    Ran r = run(s, "capture noisily include inc_h1.do");
    w2check(r.rc == "0" && showed(r, "H1|sees=[orig]|"),
            "W2 doinclude(include_basic): include sees the caller's `outer' -- oracle "
            "`H1|sees=[orig]|`, PREC 0 (main: rc 111 no_data_loaded). Got rc " + r.rc + " " +
                joined(r) + " " + r.msg);
    w2check(loc(s, "inner") == "42" && loc(s, "outer") == "changed",
            "W2 doinclude(include_basic): the file's locals survive in the caller -- oracle "
            "`VAL|include_basic|inner=[42]|outer=[changed]|`. Got inner=[" + loc(s, "inner") +
                "] outer=[" + loc(s, "outer") + "]");
  }

  // ---- B. default extension ----
  for (const char* verb : {"do", "include"}) {
    piet::SessionState s(wd);
    fresh(s);
    Ran r = run(s, std::string("capture noisily ") + verb + " inc_h3");
    w2check(r.rc == "0" && showed(r, "H3|1=[]|0=[]|"),
            std::string("W2 doinclude(") + verb + "_noext): `" + verb +
                " inc_h3` assumes `.do` -- oracle PREC 0 and `H3|1=[]|0=[]|` (main: r(601)). Got "
                "rc " + r.rc + " " + joined(r) + " " + r.msg);
  }

  // ---- C. macro-built paths, backslash ----
  {
    piet::SessionState s(wd);
    fresh(s);
    (void)piet::dispatch("global INCDIR \"incsub\"", s);
    (void)piet::dispatch("local incdir \"incsub\"", s);
    const std::pair<const char*, const char*> arms[] = {
        {"do_gpath", "capture noisily do \"$INCDIR/inc_h2.do\""},
        {"include_gpath", "capture noisily include \"$INCDIR/inc_h2.do\""},
        {"include_gbrace_noext", "capture noisily include \"${INCDIR}/inc_h2\""},
        {"include_lpath", "capture noisily include \"`incdir'/inc_h2.do\""},
    };
    for (const auto& [arm, cmd] : arms) {
      Ran r = run(s, cmd);
      w2check(r.rc == "0" && showed(r, "H2|ran|"),
              std::string("W2 doinclude(") + arm + "): `" + cmd +
                  "` expands the macro in the path and runs the file -- oracle PREC 0 + `H2|ran|` "
                  "(main: `file not found: $INCDIR/...`, r(601)). Got rc " + r.rc + " " + r.msg);
    }
#ifndef _WIN32
    Ran r = run(s, "capture noisily include \"${INCDIR}\\inc_h2.do\"");
    w2check(r.rc == "601",
            "W2 doinclude(include_backslash): a backslash is NOT a separator on macOS -- oracle "
            "`file incsub\\inc_h2.do not found`, PREC 601. Got rc " + r.rc);
#endif
  }

  // ---- D. arguments / extra tokens / options ----
  {
    piet::SessionState s(wd);
    fresh(s);
    s.eval.locals.set("1", "callerone");  // the top caller's own positional
    Ran r = run(s, "capture noisily do inc_h3w.do wrapperarg");
    w2check(r.rc == "0" && showed(r, "H3|1=[extra]|0=[extra]|") &&
                showed(r, "H3W|after_do|1=[wrapperarg]|") &&
                showed(r, "H3|1=[wrapperarg]|0=[wrapperarg]|"),
            "W2 doinclude(do_args_private): the nested `do inc_h3.do extra` seeds `1' in ITS "
            "scope only, the wrapper's `1' is still `wrapperarg` after it, and the include then "
            "sees the wrapper's `1'/`0' -- oracle H3|1=[extra] / H3W|after_do|1=[wrapperarg] / "
            "H3|1=[wrapperarg]|0=[wrapperarg]|, PREC 0 (main: after_do showed `extra`). Got rc " +
                r.rc + " " + joined(r) + " " + r.msg);
    w2check(loc(s, "1") == "callerone" && loc(s, "0").empty(),
            "W2 doinclude(do_args_private): the TOP caller's `0'/`1' are untouched by "
            "`do inc_h3w.do wrapperarg`. Got 1=[" + loc(s, "1") + "] 0=[" + loc(s, "0") + "]");
  }
  {
    piet::SessionState s(wd);
    fresh(s);
    Ran r = run(s, "capture noisily include inc_h3.do extra");
    w2check(r.rc == "198" && r.shown.empty(),
            "W2 doinclude(include_extra_token): `include` takes no arguments -- oracle "
            "`invalid syntax`, PREC 198, file not run. Got rc " + r.rc + " " + joined(r));
    r = run(s, "capture noisily include inc_h1.do, nostop");
    w2check(r.rc == "198" && r.shown.empty(),
            "W2 doinclude(include_opt_nostop): `nostop` is a do/run option, not include's -- "
            "oracle `option nostop not allowed`, PREC 198. Got rc " + r.rc + " " + r.msg);
  }

  // ---- E. error / exit / exit N ----
  struct ExitArm {
    const char* arm;
    const char* verb;
    const char* file;
    const char* rc;
    const char* beforeName;
    const char* before;  // expected caller value of the "before" local
    const char* afterName;
  };
  const ExitArm exitArms[] = {
      {"do_err", "do", "inc_h4err.do", "7", "e_before", "", "e_after"},
      {"include_err", "include", "inc_h4err.do", "7", "e_before", "1", "e_after"},
      {"do_exit", "do", "inc_h4exit.do", "0", "x_before", "", "x_after"},
      {"include_exit", "include", "inc_h4exit.do", "0", "x_before", "1", "x_after"},
      {"do_exit5", "do", "inc_h4exit5.do", "5", "x5_before", "", "x5_after"},
      {"include_exit5", "include", "inc_h4exit5.do", "5", "x5_before", "1", "x5_after"},
  };
  for (const auto& a : exitArms) {
    piet::SessionState s(wd);
    fresh(s);
    Ran r = run(s, std::string("capture noisily ") + a.verb + " " + a.file);
    w2check(r.rc == a.rc,
            std::string("W2 doinclude(") + a.arm + "): PREC " + a.rc +
                " in the oracle (exit 5 inside a nested file reaches the caller as r(5); main "
                "answered 0 for do_exit5). Got rc " + r.rc + " " + r.msg);
    w2check(loc(s, a.beforeName) == a.before && loc(s, a.afterName).empty(),
            std::string("W2 doinclude(") + a.arm + "): oracle VAL before=[" + a.before +
                "] after=[] -- only include keeps the file's locals, and nothing after the stop "
                "ran. Got before=[" + loc(s, a.beforeName) + "] after=[" + loc(s, a.afterName) +
                "]");
  }
  {
    // exit 5 is an ERROR for the caller: uncaptured, the caller stops there.
    piet::SessionState s(wd);
    fresh(s);
    auto res = piet::doexec::executeDoSource(
        "do inc_h4exit5.do\ndisplay \"W2DI-MUST-NOT-PRINT\"", s, piet::dispatch);
    Ran r = collect(res.blocks);
    w2check(!showed(r, "W2DI-MUST-NOT-PRINT") && res.stoppedAtLine.has_value(),
            "W2 doinclude(do_exit5): uncaptured, `exit 5` in the nested file stops the caller "
            "(the oracle prints r(5); after `end of do-file`). Got " + joined(r));
  }
  {
    piet::SessionState s(wd);
    fresh(s);
    Ran r = run(s, "capture noisily include inc_nosuch.do");
    w2check(r.rc == "601",
            "W2 doinclude(include_missing): oracle `file inc_nosuch.do not found`, PREC 601. Got "
            "rc " + r.rc + " " + r.msg);
  }

  // ---- F. nesting ----
  {
    piet::SessionState s(wd);
    fresh(s);
    (void)piet::dispatch("local outer \"orig\"", s);
    Ran r = run(s, "capture noisily do inc_h5.do");
    w2check(r.rc == "0" && showed(r, "H6|mid=[midval]|outer=[]|") &&
                showed(r, "H5|deep=[deepval]|"),
            "W2 doinclude(do_then_include): an include inside a do-file shares only the "
            "do-file's scope -- oracle `H6|mid=[midval]|outer=[]|` and `H5|deep=[deepval]|`. "
            "Got rc " + r.rc + " " + joined(r));
    w2check(loc(s, "mid").empty() && loc(s, "deep").empty(),
            "W2 doinclude(do_then_include): oracle `VAL|do_then_include|mid=[]|deep=[]|`. Got "
            "mid=[" + loc(s, "mid") + "] deep=[" + loc(s, "deep") + "]");
    r = run(s, "capture noisily include inc_h5.do");
    w2check(r.rc == "0" && showed(r, "H6|mid=[midval]|outer=[orig]|") &&
                loc(s, "mid") == "midval" && loc(s, "deep") == "deepval",
            "W2 doinclude(include_nested): include>include shares the top scope -- oracle "
            "`H6|mid=[midval]|outer=[orig]|` and `VAL|include_nested|mid=[midval]|deep=[deepval]|`"
            ". Got rc " + r.rc + " " + joined(r) + " mid=[" + loc(s, "mid") + "] deep=[" +
                loc(s, "deep") + "]");
  }

  // ---- G. a relative path resolves against the cwd ----
  {
    piet::SessionState s(wd);
    fresh(s);
    Ran r = run(s, "capture noisily include incsub/inc_h7.do");
    w2check(r.rc == "0" && showed(r, "H7|sibling_rc=601|"),
            "W2 doinclude(include_sibling_relpath): inside incsub/inc_h7.do, `include inc_h2.do` "
            "looks in the cwd, not beside the including file -- oracle `H7|sibling_rc=601|`, "
            "PREC 0. Got rc " + r.rc + " " + joined(r));
  }

  // ---- H. in a loop, in a program ----
  {
    piet::SessionState s(wd);
    fresh(s);
    auto res = piet::doexec::executeDoSource(
        "local acc\nforeach k in a b c {\n    capture noisily include inc_h8.do\n}\n", s,
        piet::dispatch);
    w2check(loc(s, "acc") == " a b c",
            "W2 doinclude(include_in_loop): the loop macro is visible and the accumulator is "
            "written back -- oracle `VAL|include_in_loop|acc=[ a b c]|`. Got acc=[" +
                loc(s, "acc") + "]");
    (void)res;
  }
  {
    piet::SessionState s(wd);
    fresh(s);
    auto res = piet::doexec::executeDoSource(
        "program define incprog\n"
        "    local outer \"fromprog\"\n"
        "    include inc_h1.do\n"
        "    display \"VAL|include_in_program|inner=[`inner']|outer=[`outer']|\"\n"
        "end\n"
        "local outer \"orig\"\n"
        "capture noisily incprog\n",
        s, piet::dispatch);
    Ran r = collect(res.blocks);
    r.rc = s.eval.locals.get("_rc", "0");
    w2check(showed(r, "H1|sees=[fromprog]|") &&
                showed(r, "VAL|include_in_program|inner=[42]|outer=[changed]|"),
            "W2 doinclude(include_in_program): include inside a program shares the PROGRAM's "
            "scope -- oracle `H1|sees=[fromprog]|` and `VAL|include_in_program|inner=[42]|outer="
            "[changed]|`. Got " + joined(r) + " " + r.msg);
    w2check(loc(s, "outer") == "orig" && r.rc == "0",
            "W2 doinclude(include_in_program): the program's caller keeps `outer'=orig, PREC 0 -- "
            "oracle `VAL|include_in_program_caller|outer=[orig]|`. Got outer=[" +
                loc(s, "outer") + "] rc " + r.rc);
  }

  // ---- nesting limit (NOT oracle-covered: [R] limits says 64; the rc is PIET's) ----
  {
    piet::SessionState s(wd);
    fresh(s);
    s.doExecDepth = piet::doexec::kMaxDoNesting;
    Ran r = run(s, "capture noisily do inc_h3.do");
    s.doExecDepth = 0;
    w2check(r.rc != "0" && r.msg.find("nesting") != std::string::npos && r.shown.empty(),
            "W2 doinclude(nesting guard): at 64 nested files the next do/include is refused "
            "instead of recursing (main recursed without bound: a self-including file overflowed "
            "the stack). Got rc " + r.rc + " " + r.msg);
    // ...and every private scope a refused level pushed was popped again.
    w2check(s.scopeStack.empty(),
            "W2 doinclude(nesting guard): the refused do's private scope was restored");
  }

  fs::remove_all(wd, ec);
}
