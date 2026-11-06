// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// W2 lane `bykeep` (2026-09-23) -- `by/bysort g [(s)]: keep if` / `drop if`
// (BUG-532), the global `drop if` complement under a MISSING condition
// (BUG-533), the if+in composition under a MISSING condition (BUG-534), and
// the by-prefix's own refusals/spellings (BUG-535), and a missing OPERAND of
// `!`, `&`, `|` (BUG-620, fixed in the engine and count's CiParser; PIET does
// not respell those operators).
// Oracles: tests/eval/golden/aer_w2/w2_bykeep.log (PREC/PCELL arms, cited by
// name on every check), w2_smalls.log section B and w2c_functions.log section
// operators_missing. Every by-arm below was r(199) "not supported yet" on
// main; ctl_drop_misstrue was N=9 (oracle 8). FU-811 (the precedenceArms()
// section): how `-`, `!` and `^` group in every data-expression layer, from the
// w2ac_expr_precedence capture.
//
// The record per arm is the oracle's own: rc | _N | sum(g*_n) | sum(x) |
// sum(x*_n). Arms tagged _ties have no unique within-group order, and the
// oracle header says only rc, _N and sum(g*_n) are comparable there -- so only
// those are asserted. JSON access goes through is_object()/value() inside a
// try so a wrong shape is a FAIL, never an abort.
#include "selftest_w2.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <vector>

#include "piet/datastore.h"
#include "piet/dispatcher.h"
#include "piet/doexec.h"
#include "piet/eval/errors.h"
#include "piet/handlers.h"
#include "piet/session_state.h"
#include "vendor/nlohmann/json.hpp"

namespace {

// Every check of this section goes through here so the section can print how
// many arms it executed.
int gByKeepChecks = 0;
void bkCheck(bool cond, const std::string& label) {
  ++gByKeepChecks;
  w2check(cond, label);
}

// rc of one dispatched line (the last error block's coarse rc; "0" if none).
std::string rcOf(piet::SessionState& session, const std::string& cmd) {
  std::string rc = "0";
  try {
    for (const auto& b : piet::dispatch(cmd, session)) {
      if (!b.is_object()) continue;
      if (b.value("type", std::string()) == "error") rc = piet::eval::coarseRc(b);
    }
  } catch (const std::exception& e) {
    rc = std::string("EXC:") + e.what();
  }
  return rc;
}

// Text of `display EXP` (empty when the line errored or displayed nothing).
std::string shown(piet::SessionState& session, const std::string& exp) {
  std::string text;
  try {
    for (const auto& b : piet::dispatch("display " + exp, session)) {
      if (b.is_object() && b.value("type", std::string()) == "display")
        text += b.value("text", std::string());
    }
  } catch (const std::exception&) {
  }
  return text;
}

// w2_bykeep.do's `mkfix`: g sizes g0=1 g1=2 g2=3 g3=3, rows arrive unsorted by g,
// s's first-seen order (lo, Hi) is the reverse of its sort order.
void mkfix(piet::SessionState& session) {
  for (const char* s0 :
       {"clear", "set obs 12", "generate g = mod(_n*7, 4)", "generate b = mod(_n*5, 12)",
        "generate x = _n", "generate h = mod(_n, 2)", "generate z = 0",
        "generate str2 s = cond(g < 2, \"Hi\", \"lo\")", "drop if x == 4 | x == 8 | x == 11"}) {
    (void)rcOf(session, s0);
  }
}

struct Rec {
  std::string rc, n, sg, sx, shx, rcby;
};

// The oracle's `arm` program: fixture, pre, the command, then the record.
Rec armOf(const std::string& workdir, const std::string& pre, const std::string& cmd) {
  piet::SessionState session(workdir);
  mkfix(session);
  if (!pre.empty()) (void)rcOf(session, pre);
  Rec r;
  r.rc = rcOf(session, cmd);
  r.rcby = rcOf(session, "by g: generate byte q1 = 1");
  (void)rcOf(session, "generate double qg = sum(g*_n)");
  (void)rcOf(session, "generate double qx = sum(x)");
  (void)rcOf(session, "generate double qhx = sum(x*_n)");
  r.n = shown(session, "_N");
  r.sg = shown(session, "qg[_N]");
  r.sx = shown(session, "qx[_N]");
  r.shx = shown(session, "qhx[_N]");
  return r;
}

// w2m_bymask.do's fixtures. F6: gg = mod(_n,2), x = _n*10, sorted by gg x
// (log :52/:57). F12: 12 rows, three groups, two missing x, stored unsorted
// (:549/:554).
void mkF6(piet::SessionState& session) {
  for (const char* s0 :
       {"clear", "set obs 6", "generate gg = mod(_n,2)", "generate x = _n*10", "sort gg x"})
    (void)rcOf(session, s0);
}
void mkF12(piet::SessionState& session) {
  static const char* kG[] = {"2", "1", "3", "1", "2", "3", "1", "2", "1", "3", "1", "2"};
  static const char* kX[] = {"7", "30", "12", "10", ".", "12", "25", "3", ".", "40", "18", "9"};
  for (const char* s0 : {"clear", "set obs 12", "generate g = 0", "generate x = 0"})
    (void)rcOf(session, s0);
  for (int i = 0; i < 12; ++i) {
    const std::string in = " in " + std::to_string(i + 1);
    (void)rcOf(session, std::string("replace g = ") + kG[i] + in);
    (void)rcOf(session, std::string("replace x = ") + kX[i] + in);
  }
}

// The record's values, `v[1]|...|v[n]`, as `display` prints them.
std::string valuesOf(piet::SessionState& session, const std::string& v, int n) {
  std::string out;
  for (int i = 1; i <= n; ++i)
    out += (i > 1 ? "|" : "") + shown(session, v + "[" + std::to_string(i) + "]");
  return out;
}

struct Arm {
  const char* tag;
  const char* pre;
  const char* cmd;
  const char* rc;
  const char* n;
  const char* sg;
  const char* sx;   // nullptr = not comparable (ties)
  const char* shx;  // nullptr = not comparable (ties / order)
};

// ---- FU-811 / w2ac_expr_precedence: how `-`, `!` and `^` group --------------
// Oracle: internal capture
// w2ac_expr_precedence.log (StataBE 19.5; the eval-lane
// golden copies it). Each row is one arm: the do-file's own statements before the
// record (verbatim), the record's display fields ("#v" = %21x v[1] .. v[10]) and
// the oracle's fields at the cited log line (rc first). StataBE reads negation and
// `!` looser than `^`, `^` left to right, and a sign right of `^` as covering the
// next operand only. On main the engine Parser (gen/replace/keep/drop/egen/regress
// if/count if ... & _n > 0), count's CiParser fast path (count_/assert_), the
// nlcom/nl evaluator and the `if` command (rc 1 on every `^`; `!` rewritten to
// Python's `not`; the macro layer read 2^3^2 as 512) grouped them otherwise: 117 of
// these 200 arms fail there (the rest are controls). count_<s> (fast path) and countn_<s> (`& _n > 0`, the engine)
// carry the same oracle count, so the two layers must agree.
struct PrecArm {
  const char* arm;
  int line;
  const char* pre;     // statements before the record, '\n'-separated
  const char* fields;  // '\t'-separated display fields, or "#v"
  const char* want;    // oracle fields after the arm id, '|'-separated
};

const PrecArm kPrecArms[] = {
    {"fx_a", 205,
     "version 19\nclear all\nset more off\nset linesize 255\nset obs 10\ngenerate double a = .\nreplace a = 3 in 1\nreplace a = -2 in 2\nreplace a = 0 in 3\nreplace a = 1.5 in 5\nreplace a = -0.5 in 6\nreplace a = 2 in 7\nreplace a = -3 in 8\nreplace a = 1 in 9\nreplace a = 0.25 in 10\ngenerate double b = .\nreplace b = 2 in 1\nreplace b = -1 in 2\nreplace b = 4 in 3\nreplace b = 1 in 4\nreplace b = 3 in 6\nreplace b = 0 in 7\nreplace b = 2 in 8\nreplace b = -2 in 9\nreplace b = 1 in 10\ngenerate double c = .\nreplace c = 1 in 1\nreplace c = 0 in 2\nreplace c = 1 in 3\nreplace c = 0 in 4\nreplace c = 1 in 5\nreplace c = 0 in 6\nreplace c = 1 in 7\nreplace c = 0 in 8\nreplace c = 1 in 9\nreplace c = 0 in 10\ngenerate double x = .\nreplace x = 1 in 1\nreplace x = 2 in 2\nreplace x = 3 in 3\nreplace x = 4 in 4\nreplace x = 5 in 5\nreplace x = 6 in 6\nreplace x = 7 in 7\nreplace x = 8 in 8\nreplace x = 9 in 9\nreplace x = 10 in 10\ngenerate double y = .\nreplace y = 4.5 in 1\nreplace y = 4.25 in 2\nreplace y = 3.25 in 3\nreplace y = 3.0 in 4\nreplace y = 2.75 in 5\nreplace y = 1.75 in 6\nreplace y = 1.5 in 7\nreplace y = 1.25 in 8\nreplace y = 0.25 in 9\nreplace y = 0.0 in 10\nlocal __rc = 0",
     "#a",
     "0|+1.8000000000000X+001|-1.0000000000000X+001|+0.0000000000000X-3ff|+1.0000000000000X+3ff|+1.8000000000000X+000|-1.0000000000000X-001|+1.0000000000000X+001|-1.8000000000000X+001|+1.0000000000000X+000|+1.0000000000000X-002"},
    {"fx_b", 210,
     "",
     "#b",
     "0|+1.0000000000000X+001|-1.0000000000000X+000|+1.0000000000000X+002|+1.0000000000000X+000|+1.0000000000000X+3ff|+1.8000000000000X+001|+0.0000000000000X-3ff|+1.0000000000000X+001|-1.0000000000000X+001|+1.0000000000000X+000"},
    {"fx_c", 215,
     "",
     "#c",
     "0|+1.0000000000000X+000|+0.0000000000000X-3ff|+1.0000000000000X+000|+0.0000000000000X-3ff|+1.0000000000000X+000|+0.0000000000000X-3ff|+1.0000000000000X+000|+0.0000000000000X-3ff|+1.0000000000000X+000|+0.0000000000000X-3ff"},
    {"fx_x", 220,
     "",
     "#x",
     "0|+1.0000000000000X+000|+1.0000000000000X+001|+1.8000000000000X+001|+1.0000000000000X+002|+1.4000000000000X+002|+1.8000000000000X+002|+1.c000000000000X+002|+1.0000000000000X+003|+1.2000000000000X+003|+1.4000000000000X+003"},
    {"fx_y", 225,
     "",
     "#y",
     "0|+1.2000000000000X+002|+1.1000000000000X+002|+1.a000000000000X+001|+1.8000000000000X+001|+1.6000000000000X+001|+1.c000000000000X+000|+1.8000000000000X+000|+1.4000000000000X+000|+1.0000000000000X-002|+0.0000000000000X-3ff"},
    {"gen_neg_pow", 238,
     "capture noisily generate double g_neg_pow = -a^2\nlocal __rc = _rc",
     "#g_neg_pow",
     "0|-1.2000000000000X+003|-1.0000000000000X+002|+0.0000000000000X-3ff|+1.0000000000000X+3ff|-1.2000000000000X+001|-1.0000000000000X-002|-1.0000000000000X+002|-1.2000000000000X+003|-1.0000000000000X+000|-1.0000000000000X-004"},
    {"gen_neg_pow_ws", 249,
     "capture noisily generate double g_neg_pow_ws = - a ^ 2\nlocal __rc = _rc",
     "#g_neg_pow_ws",
     "0|-1.2000000000000X+003|-1.0000000000000X+002|+0.0000000000000X-3ff|+1.0000000000000X+3ff|-1.2000000000000X+001|-1.0000000000000X-002|-1.0000000000000X+002|-1.2000000000000X+003|-1.0000000000000X+000|-1.0000000000000X-004"},
    {"gen_negpar_pow", 260,
     "capture noisily generate double g_negpar_pow = -(a)^2\nlocal __rc = _rc",
     "#g_negpar_pow",
     "0|-1.2000000000000X+003|-1.0000000000000X+002|+0.0000000000000X-3ff|+1.0000000000000X+3ff|-1.2000000000000X+001|-1.0000000000000X-002|-1.0000000000000X+002|-1.2000000000000X+003|-1.0000000000000X+000|-1.0000000000000X-004"},
    {"gen_h7", 270,
     "capture noisily generate double g_h7 = -(1-a)^2*b\nlocal __rc = _rc",
     "#g_h7",
     "0|-1.0000000000000X+003|+1.2000000000000X+003|-1.0000000000000X+002|+1.0000000000000X+3ff|+1.0000000000000X+3ff|-1.b000000000000X+002|+0.0000000000000X-3ff|-1.0000000000000X+005|+0.0000000000000X-3ff|-1.2000000000000X-001"},
    {"gen_exp_negpow", 281,
     "capture noisily generate double g_exp_negpow = exp(-a^2/2)\nlocal __rc = _rc",
     "#g_exp_negpow",
     "0|+1.6c0504695c417X-007|+1.152aaa3bf81ccX-003|+1.0000000000000X+000|+1.0000000000000X+3ff|+1.4c71b2477ab20X-002|+1.c3d6a24ed8222X-001|+1.152aaa3bf81ccX-003|+1.6c0504695c417X-007|+1.368b2fc6f960aX-001|+1.f03f56a88b5d8X-001"},
    {"gen_sub_pow", 292,
     "capture noisily generate double g_sub_pow = 1-a^2\nlocal __rc = _rc",
     "#g_sub_pow",
     "0|-1.0000000000000X+003|-1.8000000000000X+001|+1.0000000000000X+000|+1.0000000000000X+3ff|-1.4000000000000X+000|+1.8000000000000X-001|-1.8000000000000X+001|-1.0000000000000X+003|+0.0000000000000X-3ff|+1.e000000000000X-001"},
    {"gen_chain_const", 302,
     "capture noisily generate double g_chain_const = 2^3^2\nlocal __rc = _rc",
     "#g_chain_const",
     "0|+1.0000000000000X+006|+1.0000000000000X+006|+1.0000000000000X+006|+1.0000000000000X+006|+1.0000000000000X+006|+1.0000000000000X+006|+1.0000000000000X+006|+1.0000000000000X+006|+1.0000000000000X+006|+1.0000000000000X+006"},
    {"gen_chain_data", 313,
     "capture noisily generate double g_chain_data = a^2^3\nlocal __rc = _rc",
     "#g_chain_data",
     "0|+1.6c80000000000X+009|+1.0000000000000X+006|+0.0000000000000X-3ff|+1.0000000000000X+3ff|+1.6c80000000000X+003|+1.0000000000000X-006|+1.0000000000000X+006|+1.6c80000000000X+009|+1.0000000000000X+000|+1.0000000000000X-00c"},
    {"gen_pow_negexp", 324,
     "capture noisily generate double g_pow_negexp = 2^-a\nlocal __rc = _rc",
     "#g_pow_negexp",
     "0|+1.0000000000000X-003|+1.0000000000000X+002|+1.0000000000000X+000|+1.0000000000000X+3ff|+1.6a09e667f3bcdX-002|+1.6a09e667f3bccX+000|+1.0000000000000X-002|+1.0000000000000X+003|+1.0000000000000X-001|+1.ae89f995ad3adX-001"},
    {"gen_neg_pow_negexp", 335,
     "capture noisily generate double g_neg_pow_negexp = -2^-a\nlocal __rc = _rc",
     "#g_neg_pow_negexp",
     "0|-1.0000000000000X-003|-1.0000000000000X+002|-1.0000000000000X+000|+1.0000000000000X+3ff|-1.6a09e667f3bcdX-002|-1.6a09e667f3bccX+000|-1.0000000000000X-002|-1.0000000000000X+003|-1.0000000000000X-001|-1.ae89f995ad3adX-001"},
    {"gen_pow_neg_pow", 346,
     "capture noisily generate double g_pow_neg_pow = 2^-a^2\nlocal __rc = _rc",
     "#g_pow_neg_pow",
     "0|+1.0000000000000X-006|+1.0000000000000X+004|+1.0000000000000X+000|+1.0000000000000X+3ff|+1.0000000000001X-003|+1.ffffffffffffeX+000|+1.0000000000000X-004|+1.0000000000000X+006|+1.0000000000000X-002|+1.6a09e667f3bccX-001"},
    {"gen_not_pow0", 356,
     "capture noisily generate double g_not_pow0 = !a^0\nlocal __rc = _rc",
     "#g_not_pow0",
     "0|+0.0000000000000X-3ff|+0.0000000000000X-3ff|+0.0000000000000X-3ff|+0.0000000000000X-3ff|+0.0000000000000X-3ff|+0.0000000000000X-3ff|+0.0000000000000X-3ff|+0.0000000000000X-3ff|+0.0000000000000X-3ff|+0.0000000000000X-3ff"},
    {"gen_not_pow2", 366,
     "capture noisily generate double g_not_pow2 = !a^2\nlocal __rc = _rc",
     "#g_not_pow2",
     "0|+0.0000000000000X-3ff|+0.0000000000000X-3ff|+1.0000000000000X+000|+0.0000000000000X-3ff|+0.0000000000000X-3ff|+0.0000000000000X-3ff|+0.0000000000000X-3ff|+0.0000000000000X-3ff|+0.0000000000000X-3ff|+0.0000000000000X-3ff"},
    {"gen_neg_neg", 377,
     "capture noisily generate double g_neg_neg = - -a\nlocal __rc = _rc",
     "#g_neg_neg",
     "0|+1.8000000000000X+001|-1.0000000000000X+001|+0.0000000000000X-3ff|+1.0000000000000X+3ff|+1.8000000000000X+000|-1.0000000000000X-001|+1.0000000000000X+001|-1.8000000000000X+001|+1.0000000000000X+000|+1.0000000000000X-002"},
    {"gen_not_eq", 387,
     "capture noisily generate double g_not_eq = !a==b\nlocal __rc = _rc",
     "#g_not_eq",
     "0|+0.0000000000000X-3ff|+0.0000000000000X-3ff|+0.0000000000000X-3ff|+0.0000000000000X-3ff|+0.0000000000000X-3ff|+0.0000000000000X-3ff|+1.0000000000000X+000|+0.0000000000000X-3ff|+0.0000000000000X-3ff|+0.0000000000000X-3ff"},
    {"gen_lt_eq", 396,
     "capture noisily generate double g_lt_eq = a<b==c\nlocal __rc = _rc",
     "#g_lt_eq",
     "0|+0.0000000000000X-3ff|+0.0000000000000X-3ff|+1.0000000000000X+000|+1.0000000000000X+000|+1.0000000000000X+000|+0.0000000000000X-3ff|+0.0000000000000X-3ff|+0.0000000000000X-3ff|+0.0000000000000X-3ff|+0.0000000000000X-3ff"},
    {"gen_eq_gt", 405,
     "capture noisily generate double g_eq_gt = b==c>a\nlocal __rc = _rc",
     "#g_eq_gt",
     "0|+0.0000000000000X-3ff|+0.0000000000000X-3ff|+0.0000000000000X-3ff|+0.0000000000000X-3ff|+0.0000000000000X-3ff|+0.0000000000000X-3ff|+1.0000000000000X+000|+0.0000000000000X-3ff|+0.0000000000000X-3ff|+0.0000000000000X-3ff"},
    {"genabbr_neg_pow", 416,
     "capture noisily gen double ga_neg_pow = -a^2\nlocal __rc = _rc",
     "#ga_neg_pow",
     "0|-1.2000000000000X+003|-1.0000000000000X+002|+0.0000000000000X-3ff|+1.0000000000000X+3ff|-1.2000000000000X+001|-1.0000000000000X-002|-1.0000000000000X+002|-1.2000000000000X+003|-1.0000000000000X+000|-1.0000000000000X-004"},
    {"genabbr_h7", 426,
     "capture noisily gen double ga_h7 = -(1-a)^2*b\nlocal __rc = _rc",
     "#ga_h7",
     "0|-1.0000000000000X+003|+1.2000000000000X+003|-1.0000000000000X+002|+1.0000000000000X+3ff|+1.0000000000000X+3ff|-1.b000000000000X+002|+0.0000000000000X-3ff|-1.0000000000000X+005|+0.0000000000000X-3ff|-1.2000000000000X-001"},
    {"genabbr_chain_data", 437,
     "capture noisily gen double ga_chain_data = a^2^3\nlocal __rc = _rc",
     "#ga_chain_data",
     "0|+1.6c80000000000X+009|+1.0000000000000X+006|+0.0000000000000X-3ff|+1.0000000000000X+3ff|+1.6c80000000000X+003|+1.0000000000000X-006|+1.0000000000000X+006|+1.6c80000000000X+009|+1.0000000000000X+000|+1.0000000000000X-00c"},
    {"genabbr_not_pow0", 447,
     "capture noisily gen double ga_not_pow0 = !a^0\nlocal __rc = _rc",
     "#ga_not_pow0",
     "0|+0.0000000000000X-3ff|+0.0000000000000X-3ff|+0.0000000000000X-3ff|+0.0000000000000X-3ff|+0.0000000000000X-3ff|+0.0000000000000X-3ff|+0.0000000000000X-3ff|+0.0000000000000X-3ff|+0.0000000000000X-3ff|+0.0000000000000X-3ff"},
    {"genabbr_pow_neg_pow", 458,
     "capture noisily gen double ga_pow_neg_pow = 2^-a^2\nlocal __rc = _rc",
     "#ga_pow_neg_pow",
     "0|+1.0000000000000X-006|+1.0000000000000X+004|+1.0000000000000X+000|+1.0000000000000X+3ff|+1.0000000000001X-003|+1.ffffffffffffeX+000|+1.0000000000000X-004|+1.0000000000000X+006|+1.0000000000000X-002|+1.6a09e667f3bccX-001"},
    {"replace_neg_pow", 471,
     "generate double r_neg_pow = 0\ncapture noisily replace r_neg_pow = -a^2\nlocal __rc = _rc",
     "#r_neg_pow",
     "0|-1.2000000000000X+003|-1.0000000000000X+002|+0.0000000000000X-3ff|+1.0000000000000X+3ff|-1.2000000000000X+001|-1.0000000000000X-002|-1.0000000000000X+002|-1.2000000000000X+003|-1.0000000000000X+000|-1.0000000000000X-004"},
    {"replace_neg_pow_ws", 484,
     "generate double r_neg_pow_ws = 0\ncapture noisily replace r_neg_pow_ws = - a ^ 2\nlocal __rc = _rc",
     "#r_neg_pow_ws",
     "0|-1.2000000000000X+003|-1.0000000000000X+002|+0.0000000000000X-3ff|+1.0000000000000X+3ff|-1.2000000000000X+001|-1.0000000000000X-002|-1.0000000000000X+002|-1.2000000000000X+003|-1.0000000000000X+000|-1.0000000000000X-004"},
    {"replace_negpar_pow", 497,
     "generate double r_negpar_pow = 0\ncapture noisily replace r_negpar_pow = -(a)^2\nlocal __rc = _rc",
     "#r_negpar_pow",
     "0|-1.2000000000000X+003|-1.0000000000000X+002|+0.0000000000000X-3ff|+1.0000000000000X+3ff|-1.2000000000000X+001|-1.0000000000000X-002|-1.0000000000000X+002|-1.2000000000000X+003|-1.0000000000000X+000|-1.0000000000000X-004"},
    {"replace_h7", 509,
     "generate double r_h7 = 0\ncapture noisily replace r_h7 = -(1-a)^2*b\nlocal __rc = _rc",
     "#r_h7",
     "0|-1.0000000000000X+003|+1.2000000000000X+003|-1.0000000000000X+002|+1.0000000000000X+3ff|+1.0000000000000X+3ff|-1.b000000000000X+002|+0.0000000000000X-3ff|-1.0000000000000X+005|+0.0000000000000X-3ff|-1.2000000000000X-001"},
    {"replace_exp_negpow", 522,
     "generate double r_exp_negpow = 0\ncapture noisily replace r_exp_negpow = exp(-a^2/2)\nlocal __rc = _rc",
     "#r_exp_negpow",
     "0|+1.6c0504695c417X-007|+1.152aaa3bf81ccX-003|+1.0000000000000X+000|+1.0000000000000X+3ff|+1.4c71b2477ab20X-002|+1.c3d6a24ed8222X-001|+1.152aaa3bf81ccX-003|+1.6c0504695c417X-007|+1.368b2fc6f960aX-001|+1.f03f56a88b5d8X-001"},
    {"replace_sub_pow", 535,
     "generate double r_sub_pow = 0\ncapture noisily replace r_sub_pow = 1-a^2\nlocal __rc = _rc",
     "#r_sub_pow",
     "0|-1.0000000000000X+003|-1.8000000000000X+001|+1.0000000000000X+000|+1.0000000000000X+3ff|-1.4000000000000X+000|+1.8000000000000X-001|-1.8000000000000X+001|-1.0000000000000X+003|+0.0000000000000X-3ff|+1.e000000000000X-001"},
    {"replace_chain_const", 548,
     "generate double r_chain_const = 0\ncapture noisily replace r_chain_const = 2^3^2\nlocal __rc = _rc",
     "#r_chain_const",
     "0|+1.0000000000000X+006|+1.0000000000000X+006|+1.0000000000000X+006|+1.0000000000000X+006|+1.0000000000000X+006|+1.0000000000000X+006|+1.0000000000000X+006|+1.0000000000000X+006|+1.0000000000000X+006|+1.0000000000000X+006"},
    {"replace_chain_data", 561,
     "generate double r_chain_data = 0\ncapture noisily replace r_chain_data = a^2^3\nlocal __rc = _rc",
     "#r_chain_data",
     "0|+1.6c80000000000X+009|+1.0000000000000X+006|+0.0000000000000X-3ff|+1.0000000000000X+3ff|+1.6c80000000000X+003|+1.0000000000000X-006|+1.0000000000000X+006|+1.6c80000000000X+009|+1.0000000000000X+000|+1.0000000000000X-00c"},
    {"replace_pow_negexp", 574,
     "generate double r_pow_negexp = 0\ncapture noisily replace r_pow_negexp = 2^-a\nlocal __rc = _rc",
     "#r_pow_negexp",
     "0|+1.0000000000000X-003|+1.0000000000000X+002|+1.0000000000000X+000|+1.0000000000000X+3ff|+1.6a09e667f3bcdX-002|+1.6a09e667f3bccX+000|+1.0000000000000X-002|+1.0000000000000X+003|+1.0000000000000X-001|+1.ae89f995ad3adX-001"},
    {"replace_neg_pow_negexp", 587,
     "generate double r_neg_pow_negexp = 0\ncapture noisily replace r_neg_pow_negexp = -2^-a\nlocal __rc = _rc",
     "#r_neg_pow_negexp",
     "0|-1.0000000000000X-003|-1.0000000000000X+002|-1.0000000000000X+000|+1.0000000000000X+3ff|-1.6a09e667f3bcdX-002|-1.6a09e667f3bccX+000|-1.0000000000000X-002|-1.0000000000000X+003|-1.0000000000000X-001|-1.ae89f995ad3adX-001"},
    {"replace_pow_neg_pow", 600,
     "generate double r_pow_neg_pow = 0\ncapture noisily replace r_pow_neg_pow = 2^-a^2\nlocal __rc = _rc",
     "#r_pow_neg_pow",
     "0|+1.0000000000000X-006|+1.0000000000000X+004|+1.0000000000000X+000|+1.0000000000000X+3ff|+1.0000000000001X-003|+1.ffffffffffffeX+000|+1.0000000000000X-004|+1.0000000000000X+006|+1.0000000000000X-002|+1.6a09e667f3bccX-001"},
    {"replace_not_pow0", 613,
     "generate double r_not_pow0 = 0\ncapture noisily replace r_not_pow0 = !a^0\nlocal __rc = _rc",
     "#r_not_pow0",
     "0|+0.0000000000000X-3ff|+0.0000000000000X-3ff|+0.0000000000000X-3ff|+0.0000000000000X-3ff|+0.0000000000000X-3ff|+0.0000000000000X-3ff|+0.0000000000000X-3ff|+0.0000000000000X-3ff|+0.0000000000000X-3ff|+0.0000000000000X-3ff"},
    {"replace_not_pow2", 626,
     "generate double r_not_pow2 = 0\ncapture noisily replace r_not_pow2 = !a^2\nlocal __rc = _rc",
     "#r_not_pow2",
     "0|+0.0000000000000X-3ff|+0.0000000000000X-3ff|+1.0000000000000X+000|+0.0000000000000X-3ff|+0.0000000000000X-3ff|+0.0000000000000X-3ff|+0.0000000000000X-3ff|+0.0000000000000X-3ff|+0.0000000000000X-3ff|+0.0000000000000X-3ff"},
    {"replace_neg_neg", 639,
     "generate double r_neg_neg = 0\ncapture noisily replace r_neg_neg = - -a\nlocal __rc = _rc",
     "#r_neg_neg",
     "0|+1.8000000000000X+001|-1.0000000000000X+001|+0.0000000000000X-3ff|+1.0000000000000X+3ff|+1.8000000000000X+000|-1.0000000000000X-001|+1.0000000000000X+001|-1.8000000000000X+001|+1.0000000000000X+000|+1.0000000000000X-002"},
    {"replace_not_eq", 652,
     "generate double r_not_eq = 0\ncapture noisily replace r_not_eq = !a==b\nlocal __rc = _rc",
     "#r_not_eq",
     "0|+0.0000000000000X-3ff|+0.0000000000000X-3ff|+0.0000000000000X-3ff|+0.0000000000000X-3ff|+0.0000000000000X-3ff|+0.0000000000000X-3ff|+1.0000000000000X+000|+0.0000000000000X-3ff|+0.0000000000000X-3ff|+0.0000000000000X-3ff"},
    {"replace_lt_eq", 665,
     "generate double r_lt_eq = 0\ncapture noisily replace r_lt_eq = a<b==c\nlocal __rc = _rc",
     "#r_lt_eq",
     "0|+0.0000000000000X-3ff|+0.0000000000000X-3ff|+1.0000000000000X+000|+1.0000000000000X+000|+1.0000000000000X+000|+0.0000000000000X-3ff|+0.0000000000000X-3ff|+0.0000000000000X-3ff|+0.0000000000000X-3ff|+0.0000000000000X-3ff"},
    {"replace_eq_gt", 678,
     "generate double r_eq_gt = 0\ncapture noisily replace r_eq_gt = b==c>a\nlocal __rc = _rc",
     "#r_eq_gt",
     "0|+0.0000000000000X-3ff|+0.0000000000000X-3ff|+0.0000000000000X-3ff|+0.0000000000000X-3ff|+0.0000000000000X-3ff|+0.0000000000000X-3ff|+1.0000000000000X+000|+0.0000000000000X-3ff|+0.0000000000000X-3ff|+0.0000000000000X-3ff"},
    {"count_neg_pow", 690,
     "capture noisily count if -a^2 < 0\nlocal __rc = _rc",
     "%21x r(N)",
     "0|+1.0000000000000X+003"},
    {"countn_neg_pow", 700,
     "capture noisily count if -a^2 < 0 & _n > 0\nlocal __rc = _rc",
     "%21x r(N)",
     "0|+1.0000000000000X+003"},
    {"count_neg_pow_ws", 710,
     "capture noisily count if - a ^ 2 < 0\nlocal __rc = _rc",
     "%21x r(N)",
     "0|+1.0000000000000X+003"},
    {"countn_neg_pow_ws", 720,
     "capture noisily count if - a ^ 2 < 0 & _n > 0\nlocal __rc = _rc",
     "%21x r(N)",
     "0|+1.0000000000000X+003"},
    {"count_negpar_pow", 730,
     "capture noisily count if -(a)^2 < 0\nlocal __rc = _rc",
     "%21x r(N)",
     "0|+1.0000000000000X+003"},
    {"countn_negpar_pow", 740,
     "capture noisily count if -(a)^2 < 0 & _n > 0\nlocal __rc = _rc",
     "%21x r(N)",
     "0|+1.0000000000000X+003"},
    {"count_h7", 750,
     "capture noisily count if -(1-a)^2*b < 0\nlocal __rc = _rc",
     "%21x r(N)",
     "0|+1.4000000000000X+002"},
    {"countn_h7", 760,
     "capture noisily count if -(1-a)^2*b < 0 & _n > 0\nlocal __rc = _rc",
     "%21x r(N)",
     "0|+1.4000000000000X+002"},
    {"count_exp_negpow", 770,
     "capture noisily count if exp(-a^2/2) < 1\nlocal __rc = _rc",
     "%21x r(N)",
     "0|+1.0000000000000X+003"},
    {"countn_exp_negpow", 780,
     "capture noisily count if exp(-a^2/2) < 1 & _n > 0\nlocal __rc = _rc",
     "%21x r(N)",
     "0|+1.0000000000000X+003"},
    {"count_sub_pow", 790,
     "capture noisily count if 1-a^2 < 0\nlocal __rc = _rc",
     "%21x r(N)",
     "0|+1.4000000000000X+002"},
    {"countn_sub_pow", 800,
     "capture noisily count if 1-a^2 < 0 & _n > 0\nlocal __rc = _rc",
     "%21x r(N)",
     "0|+1.4000000000000X+002"},
    {"count_chain_const", 810,
     "capture noisily count if 2^3^2 > 100\nlocal __rc = _rc",
     "%21x r(N)",
     "0|+0.0000000000000X-3ff"},
    {"countn_chain_const", 820,
     "capture noisily count if 2^3^2 > 100 & _n > 0\nlocal __rc = _rc",
     "%21x r(N)",
     "0|+0.0000000000000X-3ff"},
    {"count_chain_data", 830,
     "capture noisily count if a^2^3 > 20\nlocal __rc = _rc",
     "%21x r(N)",
     "0|+1.4000000000000X+002"},
    {"countn_chain_data", 840,
     "capture noisily count if a^2^3 > 20 & _n > 0\nlocal __rc = _rc",
     "%21x r(N)",
     "0|+1.4000000000000X+002"},
    {"count_pow_negexp", 850,
     "capture noisily count if 2^-a < 1\nlocal __rc = _rc",
     "%21x r(N)",
     "0|+1.4000000000000X+002"},
    {"countn_pow_negexp", 860,
     "capture noisily count if 2^-a < 1 & _n > 0\nlocal __rc = _rc",
     "%21x r(N)",
     "0|+1.4000000000000X+002"},
    {"count_neg_pow_negexp", 870,
     "capture noisily count if -2^-a < 0\nlocal __rc = _rc",
     "%21x r(N)",
     "0|+1.2000000000000X+003"},
    {"countn_neg_pow_negexp", 880,
     "capture noisily count if -2^-a < 0 & _n > 0\nlocal __rc = _rc",
     "%21x r(N)",
     "0|+1.2000000000000X+003"},
    {"count_pow_neg_pow", 890,
     "capture noisily count if 2^-a^2 < .1\nlocal __rc = _rc",
     "%21x r(N)",
     "0|+1.0000000000000X+001"},
    {"countn_pow_neg_pow", 900,
     "capture noisily count if 2^-a^2 < .1 & _n > 0\nlocal __rc = _rc",
     "%21x r(N)",
     "0|+1.0000000000000X+001"},
    {"count_not_pow0", 910,
     "capture noisily count if !a^0\nlocal __rc = _rc",
     "%21x r(N)",
     "0|+0.0000000000000X-3ff"},
    {"countn_not_pow0", 920,
     "capture noisily count if !a^0 & _n > 0\nlocal __rc = _rc",
     "%21x r(N)",
     "0|+0.0000000000000X-3ff"},
    {"count_not_pow2", 930,
     "capture noisily count if !a^2\nlocal __rc = _rc",
     "%21x r(N)",
     "0|+1.0000000000000X+000"},
    {"countn_not_pow2", 940,
     "capture noisily count if !a^2 & _n > 0\nlocal __rc = _rc",
     "%21x r(N)",
     "0|+1.0000000000000X+000"},
    {"count_neg_neg", 950,
     "capture noisily count if - -a < 0\nlocal __rc = _rc",
     "%21x r(N)",
     "0|+1.8000000000000X+001"},
    {"countn_neg_neg", 960,
     "capture noisily count if - -a < 0 & _n > 0\nlocal __rc = _rc",
     "%21x r(N)",
     "0|+1.8000000000000X+001"},
    {"count_not_eq", 970,
     "capture noisily count if !a==b\nlocal __rc = _rc",
     "%21x r(N)",
     "0|+1.0000000000000X+000"},
    {"countn_not_eq", 980,
     "capture noisily count if !a==b & _n > 0\nlocal __rc = _rc",
     "%21x r(N)",
     "0|+1.0000000000000X+000"},
    {"count_lt_eq", 990,
     "capture noisily count if a<b==c\nlocal __rc = _rc",
     "%21x r(N)",
     "0|+1.8000000000000X+001"},
    {"countn_lt_eq", 1000,
     "capture noisily count if a<b==c & _n > 0\nlocal __rc = _rc",
     "%21x r(N)",
     "0|+1.8000000000000X+001"},
    {"count_eq_gt", 1010,
     "capture noisily count if b==c>a\nlocal __rc = _rc",
     "%21x r(N)",
     "0|+1.0000000000000X+000"},
    {"countn_eq_gt", 1020,
     "capture noisily count if b==c>a & _n > 0\nlocal __rc = _rc",
     "%21x r(N)",
     "0|+1.0000000000000X+000"},
    {"keep_neg_pow", 1036,
     "preserve\ncapture noisily keep if -a^2 < 0\nlocal __rc = _rc\nquietly count",
     "%21x r(N)",
     "0|+1.0000000000000X+003"},
    {"drop_neg_pow", 1052,
     "restore\npreserve\ncapture noisily drop if -a^2 < 0\nlocal __rc = _rc\nquietly count",
     "%21x r(N)",
     "0|+1.0000000000000X+001"},
    {"keep_neg_pow_ws", 1068,
     "restore\npreserve\ncapture noisily keep if - a ^ 2 < 0\nlocal __rc = _rc\nquietly count",
     "%21x r(N)",
     "0|+1.0000000000000X+003"},
    {"drop_neg_pow_ws", 1084,
     "restore\npreserve\ncapture noisily drop if - a ^ 2 < 0\nlocal __rc = _rc\nquietly count",
     "%21x r(N)",
     "0|+1.0000000000000X+001"},
    {"keep_negpar_pow", 1100,
     "restore\npreserve\ncapture noisily keep if -(a)^2 < 0\nlocal __rc = _rc\nquietly count",
     "%21x r(N)",
     "0|+1.0000000000000X+003"},
    {"drop_negpar_pow", 1116,
     "restore\npreserve\ncapture noisily drop if -(a)^2 < 0\nlocal __rc = _rc\nquietly count",
     "%21x r(N)",
     "0|+1.0000000000000X+001"},
    {"keep_h7", 1132,
     "restore\npreserve\ncapture noisily keep if -(1-a)^2*b < 0\nlocal __rc = _rc\nquietly count",
     "%21x r(N)",
     "0|+1.4000000000000X+002"},
    {"drop_h7", 1148,
     "restore\npreserve\ncapture noisily drop if -(1-a)^2*b < 0\nlocal __rc = _rc\nquietly count",
     "%21x r(N)",
     "0|+1.4000000000000X+002"},
    {"keep_exp_negpow", 1164,
     "restore\npreserve\ncapture noisily keep if exp(-a^2/2) < 1\nlocal __rc = _rc\nquietly count",
     "%21x r(N)",
     "0|+1.0000000000000X+003"},
    {"drop_exp_negpow", 1180,
     "restore\npreserve\ncapture noisily drop if exp(-a^2/2) < 1\nlocal __rc = _rc\nquietly count",
     "%21x r(N)",
     "0|+1.0000000000000X+001"},
    {"keep_sub_pow", 1196,
     "restore\npreserve\ncapture noisily keep if 1-a^2 < 0\nlocal __rc = _rc\nquietly count",
     "%21x r(N)",
     "0|+1.4000000000000X+002"},
    {"drop_sub_pow", 1212,
     "restore\npreserve\ncapture noisily drop if 1-a^2 < 0\nlocal __rc = _rc\nquietly count",
     "%21x r(N)",
     "0|+1.4000000000000X+002"},
    {"keep_chain_const", 1228,
     "restore\npreserve\ncapture noisily keep if 2^3^2 > 100\nlocal __rc = _rc\nquietly count",
     "%21x r(N)",
     "0|+0.0000000000000X-3ff"},
    {"drop_chain_const", 1244,
     "restore\npreserve\ncapture noisily drop if 2^3^2 > 100\nlocal __rc = _rc\nquietly count",
     "%21x r(N)",
     "0|+1.4000000000000X+003"},
    {"keep_chain_data", 1260,
     "restore\npreserve\ncapture noisily keep if a^2^3 > 20\nlocal __rc = _rc\nquietly count",
     "%21x r(N)",
     "0|+1.4000000000000X+002"},
    {"drop_chain_data", 1276,
     "restore\npreserve\ncapture noisily drop if a^2^3 > 20\nlocal __rc = _rc\nquietly count",
     "%21x r(N)",
     "0|+1.4000000000000X+002"},
    {"keep_pow_negexp", 1292,
     "restore\npreserve\ncapture noisily keep if 2^-a < 1\nlocal __rc = _rc\nquietly count",
     "%21x r(N)",
     "0|+1.4000000000000X+002"},
    {"drop_pow_negexp", 1308,
     "restore\npreserve\ncapture noisily drop if 2^-a < 1\nlocal __rc = _rc\nquietly count",
     "%21x r(N)",
     "0|+1.4000000000000X+002"},
    {"keep_neg_pow_negexp", 1324,
     "restore\npreserve\ncapture noisily keep if -2^-a < 0\nlocal __rc = _rc\nquietly count",
     "%21x r(N)",
     "0|+1.2000000000000X+003"},
    {"drop_neg_pow_negexp", 1340,
     "restore\npreserve\ncapture noisily drop if -2^-a < 0\nlocal __rc = _rc\nquietly count",
     "%21x r(N)",
     "0|+1.0000000000000X+000"},
    {"keep_pow_neg_pow", 1356,
     "restore\npreserve\ncapture noisily keep if 2^-a^2 < .1\nlocal __rc = _rc\nquietly count",
     "%21x r(N)",
     "0|+1.0000000000000X+001"},
    {"drop_pow_neg_pow", 1372,
     "restore\npreserve\ncapture noisily drop if 2^-a^2 < .1\nlocal __rc = _rc\nquietly count",
     "%21x r(N)",
     "0|+1.0000000000000X+003"},
    {"keep_not_pow0", 1388,
     "restore\npreserve\ncapture noisily keep if !a^0\nlocal __rc = _rc\nquietly count",
     "%21x r(N)",
     "0|+0.0000000000000X-3ff"},
    {"drop_not_pow0", 1404,
     "restore\npreserve\ncapture noisily drop if !a^0\nlocal __rc = _rc\nquietly count",
     "%21x r(N)",
     "0|+1.4000000000000X+003"},
    {"keep_not_pow2", 1420,
     "restore\npreserve\ncapture noisily keep if !a^2\nlocal __rc = _rc\nquietly count",
     "%21x r(N)",
     "0|+1.0000000000000X+000"},
    {"drop_not_pow2", 1436,
     "restore\npreserve\ncapture noisily drop if !a^2\nlocal __rc = _rc\nquietly count",
     "%21x r(N)",
     "0|+1.2000000000000X+003"},
    {"keep_neg_neg", 1452,
     "restore\npreserve\ncapture noisily keep if - -a < 0\nlocal __rc = _rc\nquietly count",
     "%21x r(N)",
     "0|+1.8000000000000X+001"},
    {"drop_neg_neg", 1468,
     "restore\npreserve\ncapture noisily drop if - -a < 0\nlocal __rc = _rc\nquietly count",
     "%21x r(N)",
     "0|+1.c000000000000X+002"},
    {"keep_not_eq", 1484,
     "restore\npreserve\ncapture noisily keep if !a==b\nlocal __rc = _rc\nquietly count",
     "%21x r(N)",
     "0|+1.0000000000000X+000"},
    {"drop_not_eq", 1500,
     "restore\npreserve\ncapture noisily drop if !a==b\nlocal __rc = _rc\nquietly count",
     "%21x r(N)",
     "0|+1.2000000000000X+003"},
    {"keep_lt_eq", 1516,
     "restore\npreserve\ncapture noisily keep if a<b==c\nlocal __rc = _rc\nquietly count",
     "%21x r(N)",
     "0|+1.8000000000000X+001"},
    {"drop_lt_eq", 1532,
     "restore\npreserve\ncapture noisily drop if a<b==c\nlocal __rc = _rc\nquietly count",
     "%21x r(N)",
     "0|+1.c000000000000X+002"},
    {"keep_eq_gt", 1548,
     "restore\npreserve\ncapture noisily keep if b==c>a\nlocal __rc = _rc\nquietly count",
     "%21x r(N)",
     "0|+1.0000000000000X+000"},
    {"drop_eq_gt", 1564,
     "restore\npreserve\ncapture noisily drop if b==c>a\nlocal __rc = _rc\nquietly count",
     "%21x r(N)",
     "0|+1.2000000000000X+003"},
    {"egen_neg_pow", 1577,
     "restore\ncapture noisily egen double m_neg_pow = mean(-a^2)\nlocal __rc = _rc",
     "%21x m_neg_pow[1]",
     "0|-1.a471c71c71c72X+001"},
    {"egen_neg_pow_ws", 1586,
     "capture noisily egen double m_neg_pow_ws = mean(- a ^ 2)\nlocal __rc = _rc",
     "%21x m_neg_pow_ws[1]",
     "0|-1.a471c71c71c72X+001"},
    {"egen_negpar_pow", 1595,
     "capture noisily egen double m_negpar_pow = mean(-(a)^2)\nlocal __rc = _rc",
     "%21x m_negpar_pow[1]",
     "0|-1.a471c71c71c72X+001"},
    {"egen_h7", 1604,
     "capture noisily egen double m_h7 = mean(-(1-a)^2*b)\nlocal __rc = _rc",
     "%21x m_h7[1]",
     "0|-1.5280000000000X+002"},
    {"egen_exp_negpow", 1613,
     "capture noisily egen double m_exp_negpow = mean(exp(-a^2/2))\nlocal __rc = _rc",
     "%21x m_exp_negpow[1]",
     "0|+1.cfbc55244e7b0X-002"},
    {"egen_sub_pow", 1622,
     "capture noisily egen double m_sub_pow = mean(1-a^2)\nlocal __rc = _rc",
     "%21x m_sub_pow[1]",
     "0|-1.2471c71c71c72X+001"},
    {"egen_chain_const", 1631,
     "capture noisily egen double m_chain_const = mean(2^3^2)\nlocal __rc = _rc",
     "%21x m_chain_const[1]",
     "0|+1.0000000000000X+006"},
    {"egen_chain_data", 1640,
     "capture noisily egen double m_chain_data = mean(a^2^3)\nlocal __rc = _rc",
     "%21x m_chain_data[1]",
     "0|+1.633391c71c71cX+007"},
    {"egen_pow_negexp", 1649,
     "capture noisily egen double m_pow_negexp = mean(2^-a)\nlocal __rc = _rc",
     "%21x m_pow_negexp[1]",
     "0|+1.d4de5fa4f9b2cX+000"},
    {"egen_neg_pow_negexp", 1658,
     "capture noisily egen double m_neg_pow_negexp = mean(-2^-a)\nlocal __rc = _rc",
     "%21x m_neg_pow_negexp[1]",
     "0|-1.d4de5fa4f9b2cX+000"},
    {"egen_pow_neg_pow", 1667,
     "capture noisily egen double m_pow_neg_pow = mean(2^-a^2)\nlocal __rc = _rc",
     "%21x m_pow_neg_pow[1]",
     "0|+1.2b3c8360b8cdcX+003"},
    {"egen_not_pow0", 1676,
     "capture noisily egen double m_not_pow0 = mean(!a^0)\nlocal __rc = _rc",
     "%21x m_not_pow0[1]",
     "0|+0.0000000000000X-3ff"},
    {"egen_not_pow2", 1685,
     "capture noisily egen double m_not_pow2 = mean(!a^2)\nlocal __rc = _rc",
     "%21x m_not_pow2[1]",
     "0|+1.999999999999aX-004"},
    {"egen_neg_neg", 1694,
     "capture noisily egen double m_neg_neg = mean(- -a)\nlocal __rc = _rc",
     "%21x m_neg_neg[1]",
     "0|+1.0000000000000X-002"},
    {"egen_not_eq", 1703,
     "capture noisily egen double m_not_eq = mean(!a==b)\nlocal __rc = _rc",
     "%21x m_not_eq[1]",
     "0|+1.999999999999aX-004"},
    {"egen_lt_eq", 1712,
     "capture noisily egen double m_lt_eq = mean(a<b==c)\nlocal __rc = _rc",
     "%21x m_lt_eq[1]",
     "0|+1.3333333333333X-002"},
    {"egen_eq_gt", 1721,
     "capture noisily egen double m_eq_gt = mean(b==c>a)\nlocal __rc = _rc",
     "%21x m_eq_gt[1]",
     "0|+1.999999999999aX-004"},
    {"reg_neg_pow", 1749,
     "capture noisily regress y x if -a^2 < 0 | x > 7\nlocal __rc = _rc\nlocal __n = cond(`__rc' == 0, e(N), .)",
     "%21x `__n'",
     "0|+1.0000000000000X+003"},
    {"reg_neg_pow_ws", 1776,
     "ereturn clear\ncapture noisily regress y x if - a ^ 2 < 0 | x > 7\nlocal __rc = _rc\nlocal __n = cond(`__rc' == 0, e(N), .)",
     "%21x `__n'",
     "0|+1.0000000000000X+003"},
    {"reg_negpar_pow", 1803,
     "ereturn clear\ncapture noisily regress y x if -(a)^2 < 0 | x > 7\nlocal __rc = _rc\nlocal __n = cond(`__rc' == 0, e(N), .)",
     "%21x `__n'",
     "0|+1.0000000000000X+003"},
    {"reg_h7", 1830,
     "ereturn clear\ncapture noisily regress y x if -(1-a)^2*b < 0 | x > 7\nlocal __rc = _rc\nlocal __n = cond(`__rc' == 0, e(N), .)",
     "%21x `__n'",
     "0|+1.8000000000000X+002"},
    {"reg_exp_negpow", 1857,
     "ereturn clear\ncapture noisily regress y x if exp(-a^2/2) < 1 | x > 7\nlocal __rc = _rc\nlocal __n = cond(`__rc' == 0, e(N), .)",
     "%21x `__n'",
     "0|+1.0000000000000X+003"},
    {"reg_sub_pow", 1884,
     "ereturn clear\ncapture noisily regress y x if 1-a^2 < 0 | x > 7\nlocal __rc = _rc\nlocal __n = cond(`__rc' == 0, e(N), .)",
     "%21x `__n'",
     "0|+1.c000000000000X+002"},
    {"reg_chain_const", 1911,
     "ereturn clear\ncapture noisily regress y x if 2^3^2 > 100 | x > 7\nlocal __rc = _rc\nlocal __n = cond(`__rc' == 0, e(N), .)",
     "%21x `__n'",
     "0|+1.8000000000000X+001"},
    {"reg_chain_data", 1938,
     "ereturn clear\ncapture noisily regress y x if a^2^3 > 20 | x > 7\nlocal __rc = _rc\nlocal __n = cond(`__rc' == 0, e(N), .)",
     "%21x `__n'",
     "0|+1.c000000000000X+002"},
    {"reg_pow_negexp", 1965,
     "ereturn clear\ncapture noisily regress y x if 2^-a < 1 | x > 7\nlocal __rc = _rc\nlocal __n = cond(`__rc' == 0, e(N), .)",
     "%21x `__n'",
     "0|+1.8000000000000X+002"},
    {"reg_neg_pow_negexp", 1992,
     "ereturn clear\ncapture noisily regress y x if -2^-a < 0 | x > 7\nlocal __rc = _rc\nlocal __n = cond(`__rc' == 0, e(N), .)",
     "%21x `__n'",
     "0|+1.2000000000000X+003"},
    {"reg_pow_neg_pow", 2019,
     "ereturn clear\ncapture noisily regress y x if 2^-a^2 < .1 | x > 7\nlocal __rc = _rc\nlocal __n = cond(`__rc' == 0, e(N), .)",
     "%21x `__n'",
     "0|+1.4000000000000X+002"},
    {"reg_not_pow0", 2046,
     "ereturn clear\ncapture noisily regress y x if !a^0 | x > 7\nlocal __rc = _rc\nlocal __n = cond(`__rc' == 0, e(N), .)",
     "%21x `__n'",
     "0|+1.8000000000000X+001"},
    {"reg_not_pow2", 2073,
     "ereturn clear\ncapture noisily regress y x if !a^2 | x > 7\nlocal __rc = _rc\nlocal __n = cond(`__rc' == 0, e(N), .)",
     "%21x `__n'",
     "0|+1.0000000000000X+002"},
    {"reg_neg_neg", 2100,
     "ereturn clear\ncapture noisily regress y x if - -a < 0 | x > 7\nlocal __rc = _rc\nlocal __n = cond(`__rc' == 0, e(N), .)",
     "%21x `__n'",
     "0|+1.4000000000000X+002"},
    {"reg_not_eq", 2127,
     "ereturn clear\ncapture noisily regress y x if !a==b | x > 7\nlocal __rc = _rc\nlocal __n = cond(`__rc' == 0, e(N), .)",
     "%21x `__n'",
     "0|+1.0000000000000X+002"},
    {"reg_lt_eq", 2154,
     "ereturn clear\ncapture noisily regress y x if a<b==c | x > 7\nlocal __rc = _rc\nlocal __n = cond(`__rc' == 0, e(N), .)",
     "%21x `__n'",
     "0|+1.8000000000000X+002"},
    {"reg_eq_gt", 2181,
     "ereturn clear\ncapture noisily regress y x if b==c>a | x > 7\nlocal __rc = _rc\nlocal __n = cond(`__rc' == 0, e(N), .)",
     "%21x `__n'",
     "0|+1.0000000000000X+002"},
    {"assert_neg_pow", 2194,
     "ereturn clear\ncapture noisily assert -a^2 <= 0 if a < .\nlocal __rc = _rc",
     "",
     "0"},
    {"assert_h7", 2203,
     "capture noisily assert -(1-a)^2*b <= 0 if b >= 0 & b < . & a < .\nlocal __rc = _rc",
     "",
     "0"},
    {"assert_exp_negpow", 2212,
     "capture noisily assert exp(-a^2/2) <= 1 if a < .\nlocal __rc = _rc",
     "",
     "0"},
    {"assert_chain_const", 2221,
     "capture noisily assert 2^3^2 < 100\nlocal __rc = _rc",
     "",
     "0"},
    {"assert_neg_pow_negexp", 2230,
     "capture noisily assert -2^-a < 0 if a < .\nlocal __rc = _rc",
     "",
     "0"},
    {"assert_pow_neg_pow", 2241,
     "capture noisily assert 2^-a^2 <= 1 if a < .\nlocal __rc = _rc",
     "",
     "9"},
    {"assert_not_pow0", 2252,
     "capture noisily assert !a^0\nlocal __rc = _rc",
     "",
     "9"},
    {"assert_not_eq", 2262,
     "capture noisily assert !a==b if _n == 1\nlocal __rc = _rc",
     "",
     "9"},
    {"ifcmd_neg_pow", 2279,
     "local __v = 0\ncapture noisily {\n    if -3^2 < 0 {\n        local __v = 1\n    }\n}\nlocal __rc = _rc",
     "`__v'",
     "0|1"},
    {"ifcmd_h7", 2294,
     "local __v = 0\ncapture noisily {\n    if -(1-3)^2*2 < 0 {\n        local __v = 1\n    }\n}\nlocal __rc = _rc",
     "`__v'",
     "0|1"},
    {"ifcmd_chain_const", 2309,
     "local __v = 0\ncapture noisily {\n    if 2^3^2 > 100 {\n        local __v = 1\n    }\n}\nlocal __rc = _rc",
     "`__v'",
     "0|0"},
    {"ifcmd_pow_neg_pow", 2324,
     "local __v = 0\ncapture noisily {\n    if 2^-3^2 < .01 {\n        local __v = 1\n    }\n}\nlocal __rc = _rc",
     "`__v'",
     "0|0"},
    {"ifcmd_not_pow0", 2339,
     "local __v = 0\ncapture noisily {\n    if !3^0 {\n        local __v = 1\n    }\n}\nlocal __rc = _rc",
     "`__v'",
     "0|0"},
    {"ifcmd_neg_pow_negexp", 2354,
     "local __v = 0\ncapture noisily {\n    if -2^2 < 0 {\n        local __v = 1\n    }\n}\nlocal __rc = _rc",
     "`__v'",
     "0|1"},
    {"ifcmd_not_eq", 2369,
     "local __v = 0\ncapture noisily {\n    if !3==2 {\n        local __v = 1\n    }\n}\nlocal __rc = _rc",
     "`__v'",
     "0|0"},
    {"ifcmd_ctl_mul", 2384,
     "local __v = 0\ncapture noisily {\n    if 2*2 > 3 {\n        local __v = 1\n    }\n}\nlocal __rc = _rc",
     "`__v'",
     "0|1"},
    {"disp_neg_pow", 2393,
     "local __rc = 0",
     "%21x (-3^2)",
     "0|-1.2000000000000X+003"},
    {"scalar_neg_pow", 2402,
     "capture noisily scalar s_neg_pow = -3^2\nlocal __rc = _rc",
     "%21x s_neg_pow",
     "0|-1.2000000000000X+003"},
    {"local_neg_pow", 2411,
     "capture noisily local l_neg_pow = -3^2\nlocal __rc = _rc",
     "`l_neg_pow'",
     "0|-9"},
    {"disp_h7", 2418,
     "local __rc = 0",
     "%21x (-(1-3)^2*2)",
     "0|-1.0000000000000X+003"},
    {"scalar_h7", 2427,
     "capture noisily scalar s_h7 = -(1-3)^2*2\nlocal __rc = _rc",
     "%21x s_h7",
     "0|-1.0000000000000X+003"},
    {"local_h7", 2436,
     "capture noisily local l_h7 = -(1-3)^2*2\nlocal __rc = _rc",
     "`l_h7'",
     "0|-8"},
    {"disp_exp_negpow", 2443,
     "local __rc = 0",
     "%21x (exp(-3^2/2))",
     "0|+1.6c0504695c417X-007"},
    {"scalar_exp_negpow", 2452,
     "capture noisily scalar s_exp_negpow = exp(-3^2/2)\nlocal __rc = _rc",
     "%21x s_exp_negpow",
     "0|+1.6c0504695c417X-007"},
    {"local_exp_negpow", 2461,
     "capture noisily local l_exp_negpow = exp(-3^2/2)\nlocal __rc = _rc",
     "`l_exp_negpow'",
     "0|.011109"},
    {"disp_chain_const", 2468,
     "local __rc = 0",
     "%21x (2^3^2)",
     "0|+1.0000000000000X+006"},
    {"scalar_chain_const", 2477,
     "capture noisily scalar s_chain_const = 2^3^2\nlocal __rc = _rc",
     "%21x s_chain_const",
     "0|+1.0000000000000X+006"},
    {"local_chain_const", 2486,
     "capture noisily local l_chain_const = 2^3^2\nlocal __rc = _rc",
     "`l_chain_const'",
     "0|64"},
    {"disp_pow_negexp", 2493,
     "local __rc = 0",
     "%21x (2^-2)",
     "0|+1.0000000000000X-002"},
    {"scalar_pow_negexp", 2502,
     "capture noisily scalar s_pow_negexp = 2^-2\nlocal __rc = _rc",
     "%21x s_pow_negexp",
     "0|+1.0000000000000X-002"},
    {"local_pow_negexp", 2511,
     "capture noisily local l_pow_negexp = 2^-2\nlocal __rc = _rc",
     "`l_pow_negexp'",
     "0|.25"},
    {"disp_neg_pow_negexp", 2518,
     "local __rc = 0",
     "%21x (-2^-2)",
     "0|-1.0000000000000X-002"},
    {"scalar_neg_pow_negexp", 2527,
     "capture noisily scalar s_neg_pow_negexp = -2^-2\nlocal __rc = _rc",
     "%21x s_neg_pow_negexp",
     "0|-1.0000000000000X-002"},
    {"local_neg_pow_negexp", 2536,
     "capture noisily local l_neg_pow_negexp = -2^-2\nlocal __rc = _rc",
     "`l_neg_pow_negexp'",
     "0|-.25"},
    {"disp_pow_neg_pow", 2543,
     "local __rc = 0",
     "%21x (2^-3^2)",
     "0|+1.0000000000000X-006"},
    {"scalar_pow_neg_pow", 2552,
     "capture noisily scalar s_pow_neg_pow = 2^-3^2\nlocal __rc = _rc",
     "%21x s_pow_neg_pow",
     "0|+1.0000000000000X-006"},
    {"local_pow_neg_pow", 2561,
     "capture noisily local l_pow_neg_pow = 2^-3^2\nlocal __rc = _rc",
     "`l_pow_neg_pow'",
     "0|.015625"},
    {"disp_not_pow0", 2568,
     "local __rc = 0",
     "%21x (!3^0)",
     "0|+0.0000000000000X-3ff"},
    {"scalar_not_pow0", 2577,
     "capture noisily scalar s_not_pow0 = !3^0\nlocal __rc = _rc",
     "%21x s_not_pow0",
     "0|+0.0000000000000X-3ff"},
    {"local_not_pow0", 2586,
     "capture noisily local l_not_pow0 = !3^0\nlocal __rc = _rc",
     "`l_not_pow0'",
     "0|0"},
    {"disp_not_pow0z", 2593,
     "local __rc = 0",
     "%21x (!0^0)",
     "0|+0.0000000000000X-3ff"},
    {"scalar_not_pow0z", 2602,
     "capture noisily scalar s_not_pow0z = !0^0\nlocal __rc = _rc",
     "%21x s_not_pow0z",
     "0|+0.0000000000000X-3ff"},
    {"local_not_pow0z", 2611,
     "capture noisily local l_not_pow0z = !0^0\nlocal __rc = _rc",
     "`l_not_pow0z'",
     "0|0"},
    {"disp_neg_neg", 2618,
     "local __rc = 0",
     "%21x (- -3)",
     "0|+1.8000000000000X+001"},
    {"scalar_neg_neg", 2627,
     "capture noisily scalar s_neg_neg = - -3\nlocal __rc = _rc",
     "%21x s_neg_neg",
     "0|+1.8000000000000X+001"},
    {"local_neg_neg", 2636,
     "capture noisily local l_neg_neg = - -3\nlocal __rc = _rc",
     "`l_neg_neg'",
     "0|3"},
    {"nlcom_bx", 2647,
     "quietly regress y x\nlocal __rc = 0",
     "%21x _b[x]",
     "0|-1.04a7904a7904bX-001"},
    {"nlcom_negpow", 2664,
     "capture noisily nlcom -_b[x]^2\nlocal __rc = _rc",
     "%21x r(b)[1,1]\t%21x r(V)[1,1]",
     "0|-1.0964cac4d84f4X-002|+1.2f34f381ee72eX-00b"},
    {"nlcom_chain", 2681,
     "capture noisily nlcom _b[x]^2^3\nlocal __rc = _rc",
     "%21x r(b)[1,1]\t%21x r(V)[1,1]",
     "0|+1.1d3a570cbc7bcX-006|+1.89ff896aa4d0cX-010"},
    {"nlcom_negexp", 2698,
     "capture noisily nlcom -2^-_b[x]\nlocal __rc = _rc",
     "%21x r(b)[1,1]\t%21x r(V)[1,1]",
     "0|-1.6c53c390be569X+000|+1.1c9ae0a241dd9X-00b"},
    {"nlcom_const", 2715,
     "capture noisily nlcom -2^2 + _b[x]\nlocal __rc = _rc",
     "%21x r(b)[1,1]\t%21x r(V)[1,1]",
     "0|-1.2094f2094f209X+002|+1.247986a4bc171X-00b"},
    {"nlcom_ctl", 2732,
     "capture noisily nlcom -(_b[x]^2)\nlocal __rc = _rc",
     "%21x r(b)[1,1]\t%21x r(V)[1,1]",
     "0|-1.0964cac4d84f4X-002|+1.2f34f381ee72eX-00b"},
    {"nl_negpow", 2766,
     "ereturn clear\ncapture noisily nl (y = -{b1=1}^2*x + {b0=0})\nlocal __rc = _rc",
     "%21x round(e(rss), 1e-6)\t%21x e(N)",
     "0|+1.7904b3c3e74b0X-002|+1.4000000000000X+003"},
    {"nl_ctl", 2798,
     "ereturn clear\ncapture noisily nl (y = {b0=0} - {b1=1}^2*x)\nlocal __rc = _rc",
     "%21x round(e(rss), 1e-6)\t%21x e(N)",
     "0|+1.7904b3c3e74b0X-002|+1.4000000000000X+003"},
};

// FU-844: the six comparison operators and `!` against each other, from the
// supplementary oracle internal capture
// w2ac_expr_relational.log (StataBE 19.5, 27 rows u v w over
// {0,1,2}^3). Each comparison operator is its own left-associative level, tightest
// first != > < <= >= ==, in generate (engine Parser), count (CiParser) and the macro
// layer (display / local / scalar / the `if` command). On main all six shared one
// left-to-right level in the engine and the CiParser, the macro layer chained them
// Python's way (`0==0<0` = 0, Stata 1), and `~` / `~=` were refused at rc 1: 163 of
// these 239 arms fail there, and gen_not_neg (`!-t`, r(198)) was refused at rc 1. The
// capture stopped at ifcmd_not_gt, so later arms have no record.
const PrecArm kRelArms[] = {
    {"fx_u", 54,
     "version 19\nclear all\nset more off\nset linesize 255\nset obs 27\ngenerate double u = floor((_n - 1) / 9)\ngenerate double v = mod(floor((_n - 1) / 3), 3)\ngenerate double w = mod(_n - 1, 3)\ngenerate double t = u - 1\nlocal __rc = 0",
     "%21x u[1]\t%21x u[2]\t%21x u[3]\t%21x u[4]\t%21x u[5]\t%21x u[6]\t%21x u[7]\t%21x u[8]\t%21x u[9]\t%21x u[10]\t%21x u[11]\t%21x u[12]\t%21x u[13]\t%21x u[14]\t%21x u[15]\t%21x u[16]\t%21x u[17]\t%21x u[18]\t%21x u[19]\t%21x u[20]\t%21x u[21]\t%21x u[22]\t%21x u[23]\t%21x u[24]\t%21x u[25]\t%21x u[26]\t%21x u[27]",
     "0|+0.0000000000000X-3ff|+0.0000000000000X-3ff|+0.0000000000000X-3ff|+0.0000000000000X-3ff|+0.0000000000000X-3ff|+0.0000000000000X-3ff|+0.0000000000000X-3ff|+0.0000000000000X-3ff|+0.0000000000000X-3ff|+1.0000000000000X+000|+1.0000000000000X+000|+1.0000000000000X+000|+1.0000000000000X+000|+1.0000000000000X+000|+1.0000000000000X+000|+1.0000000000000X+000|+1.0000000000000X+000|+1.0000000000000X+000|+1.0000000000000X+001|+1.0000000000000X+001|+1.0000000000000X+001|+1.0000000000000X+001|+1.0000000000000X+001|+1.0000000000000X+001|+1.0000000000000X+001|+1.0000000000000X+001|+1.0000000000000X+001"},
    {"fx_v", 62,
     "",
     "%21x v[1]\t%21x v[2]\t%21x v[3]\t%21x v[4]\t%21x v[5]\t%21x v[6]\t%21x v[7]\t%21x v[8]\t%21x v[9]\t%21x v[10]\t%21x v[11]\t%21x v[12]\t%21x v[13]\t%21x v[14]\t%21x v[15]\t%21x v[16]\t%21x v[17]\t%21x v[18]\t%21x v[19]\t%21x v[20]\t%21x v[21]\t%21x v[22]\t%21x v[23]\t%21x v[24]\t%21x v[25]\t%21x v[26]\t%21x v[27]",
     "0|+0.0000000000000X-3ff|+0.0000000000000X-3ff|+0.0000000000000X-3ff|+1.0000000000000X+000|+1.0000000000000X+000|+1.0000000000000X+000|+1.0000000000000X+001|+1.0000000000000X+001|+1.0000000000000X+001|+0.0000000000000X-3ff|+0.0000000000000X-3ff|+0.0000000000000X-3ff|+1.0000000000000X+000|+1.0000000000000X+000|+1.0000000000000X+000|+1.0000000000000X+001|+1.0000000000000X+001|+1.0000000000000X+001|+0.0000000000000X-3ff|+0.0000000000000X-3ff|+0.0000000000000X-3ff|+1.0000000000000X+000|+1.0000000000000X+000|+1.0000000000000X+000|+1.0000000000000X+001|+1.0000000000000X+001|+1.0000000000000X+001"},
    {"fx_w", 70,
     "",
     "%21x w[1]\t%21x w[2]\t%21x w[3]\t%21x w[4]\t%21x w[5]\t%21x w[6]\t%21x w[7]\t%21x w[8]\t%21x w[9]\t%21x w[10]\t%21x w[11]\t%21x w[12]\t%21x w[13]\t%21x w[14]\t%21x w[15]\t%21x w[16]\t%21x w[17]\t%21x w[18]\t%21x w[19]\t%21x w[20]\t%21x w[21]\t%21x w[22]\t%21x w[23]\t%21x w[24]\t%21x w[25]\t%21x w[26]\t%21x w[27]",
     "0|+0.0000000000000X-3ff|+1.0000000000000X+000|+1.0000000000000X+001|+0.0000000000000X-3ff|+1.0000000000000X+000|+1.0000000000000X+001|+0.0000000000000X-3ff|+1.0000000000000X+000|+1.0000000000000X+001|+0.0000000000000X-3ff|+1.0000000000000X+000|+1.0000000000000X+001|+0.0000000000000X-3ff|+1.0000000000000X+000|+1.0000000000000X+001|+0.0000000000000X-3ff|+1.0000000000000X+000|+1.0000000000000X+001|+0.0000000000000X-3ff|+1.0000000000000X+000|+1.0000000000000X+001|+0.0000000000000X-3ff|+1.0000000000000X+000|+1.0000000000000X+001|+0.0000000000000X-3ff|+1.0000000000000X+000|+1.0000000000000X+001"},
    {"fx_t", 78,
     "",
     "%21x t[1]\t%21x t[2]\t%21x t[3]\t%21x t[4]\t%21x t[5]\t%21x t[6]\t%21x t[7]\t%21x t[8]\t%21x t[9]\t%21x t[10]\t%21x t[11]\t%21x t[12]\t%21x t[13]\t%21x t[14]\t%21x t[15]\t%21x t[16]\t%21x t[17]\t%21x t[18]\t%21x t[19]\t%21x t[20]\t%21x t[21]\t%21x t[22]\t%21x t[23]\t%21x t[24]\t%21x t[25]\t%21x t[26]\t%21x t[27]",
     "0|-1.0000000000000X+000|-1.0000000000000X+000|-1.0000000000000X+000|-1.0000000000000X+000|-1.0000000000000X+000|-1.0000000000000X+000|-1.0000000000000X+000|-1.0000000000000X+000|-1.0000000000000X+000|+0.0000000000000X-3ff|+0.0000000000000X-3ff|+0.0000000000000X-3ff|+0.0000000000000X-3ff|+0.0000000000000X-3ff|+0.0000000000000X-3ff|+0.0000000000000X-3ff|+0.0000000000000X-3ff|+0.0000000000000X-3ff|+1.0000000000000X+000|+1.0000000000000X+000|+1.0000000000000X+000|+1.0000000000000X+000|+1.0000000000000X+000|+1.0000000000000X+000|+1.0000000000000X+000|+1.0000000000000X+000|+1.0000000000000X+000"},
    {"gen_eq_eq", 93,
     "capture noisily generate double g_eq_eq = u == v == w\nlocal __rc = _rc",
     "%21x g_eq_eq[1]\t%21x g_eq_eq[2]\t%21x g_eq_eq[3]\t%21x g_eq_eq[4]\t%21x g_eq_eq[5]\t%21x g_eq_eq[6]\t%21x g_eq_eq[7]\t%21x g_eq_eq[8]\t%21x g_eq_eq[9]\t%21x g_eq_eq[10]\t%21x g_eq_eq[11]\t%21x g_eq_eq[12]\t%21x g_eq_eq[13]\t%21x g_eq_eq[14]\t%21x g_eq_eq[15]\t%21x g_eq_eq[16]\t%21x g_eq_eq[17]\t%21x g_eq_eq[18]\t%21x g_eq_eq[19]\t%21x g_eq_eq[20]\t%21x g_eq_eq[21]\t%21x g_eq_eq[22]\t%21x g_eq_eq[23]\t%21x g_eq_eq[24]\t%21x g_eq_eq[25]\t%21x g_eq_eq[26]\t%21x g_eq_eq[27]",
     "0|+0.0000000000000X-3ff|+1.0000000000000X+000|+0.0000000000000X-3ff|+1.0000000000000X+000|+0.0000000000000X-3ff|+0.0000000000000X-3ff|+1.0000000000000X+000|+0.0000000000000X-3ff|+0.0000000000000X-3ff|+1.0000000000000X+000|+0.0000000000000X-3ff|+0.0000000000000X-3ff|+0.0000000000000X-3ff|+1.0000000000000X+000|+0.0000000000000X-3ff|+1.0000000000000X+000|+0.0000000000000X-3ff|+0.0000000000000X-3ff|+1.0000000000000X+000|+0.0000000000000X-3ff|+0.0000000000000X-3ff|+1.0000000000000X+000|+0.0000000000000X-3ff|+0.0000000000000X-3ff|+0.0000000000000X-3ff|+1.0000000000000X+000|+0.0000000000000X-3ff"},
    {"gen_eq_ne", 106,
     "capture noisily generate double g_eq_ne = u == v != w\nlocal __rc = _rc",
     "%21x g_eq_ne[1]\t%21x g_eq_ne[2]\t%21x g_eq_ne[3]\t%21x g_eq_ne[4]\t%21x g_eq_ne[5]\t%21x g_eq_ne[6]\t%21x g_eq_ne[7]\t%21x g_eq_ne[8]\t%21x g_eq_ne[9]\t%21x g_eq_ne[10]\t%21x g_eq_ne[11]\t%21x g_eq_ne[12]\t%21x g_eq_ne[13]\t%21x g_eq_ne[14]\t%21x g_eq_ne[15]\t%21x g_eq_ne[16]\t%21x g_eq_ne[17]\t%21x g_eq_ne[18]\t%21x g_eq_ne[19]\t%21x g_eq_ne[20]\t%21x g_eq_ne[21]\t%21x g_eq_ne[22]\t%21x g_eq_ne[23]\t%21x g_eq_ne[24]\t%21x g_eq_ne[25]\t%21x g_eq_ne[26]\t%21x g_eq_ne[27]",
     "0|+1.0000000000000X+000|+0.0000000000000X-3ff|+0.0000000000000X-3ff|+0.0000000000000X-3ff|+1.0000000000000X+000|+0.0000000000000X-3ff|+0.0000000000000X-3ff|+0.0000000000000X-3ff|+1.0000000000000X+000|+0.0000000000000X-3ff|+1.0000000000000X+000|+1.0000000000000X+000|+1.0000000000000X+000|+0.0000000000000X-3ff|+1.0000000000000X+000|+1.0000000000000X+000|+1.0000000000000X+000|+0.0000000000000X-3ff|+0.0000000000000X-3ff|+0.0000000000000X-3ff|+0.0000000000000X-3ff|+0.0000000000000X-3ff|+0.0000000000000X-3ff|+0.0000000000000X-3ff|+0.0000000000000X-3ff|+0.0000000000000X-3ff|+0.0000000000000X-3ff"},
    {"gen_eq_gt", 119,
     "capture noisily generate double g_eq_gt = u == v > w\nlocal __rc = _rc",
     "%21x g_eq_gt[1]\t%21x g_eq_gt[2]\t%21x g_eq_gt[3]\t%21x g_eq_gt[4]\t%21x g_eq_gt[5]\t%21x g_eq_gt[6]\t%21x g_eq_gt[7]\t%21x g_eq_gt[8]\t%21x g_eq_gt[9]\t%21x g_eq_gt[10]\t%21x g_eq_gt[11]\t%21x g_eq_gt[12]\t%21x g_eq_gt[13]\t%21x g_eq_gt[14]\t%21x g_eq_gt[15]\t%21x g_eq_gt[16]\t%21x g_eq_gt[17]\t%21x g_eq_gt[18]\t%21x g_eq_gt[19]\t%21x g_eq_gt[20]\t%21x g_eq_gt[21]\t%21x g_eq_gt[22]\t%21x g_eq_gt[23]\t%21x g_eq_gt[24]\t%21x g_eq_gt[25]\t%21x g_eq_gt[26]\t%21x g_eq_gt[27]",
     "0|+1.0000000000000X+000|+1.0000000000000X+000|+1.0000000000000X+000|+0.0000000000000X-3ff|+1.0000000000000X+000|+1.0000000000000X+000|+0.0000000000000X-3ff|+0.0000000000000X-3ff|+1.0000000000000X+000|+0.0000000000000X-3ff|+0.0000000000000X-3ff|+0.0000000000000X-3ff|+1.0000000000000X+000|+0.0000000000000X-3ff|+0.0000000000000X-3ff|+1.0000000000000X+000|+1.0000000000000X+000|+0.0000000000000X-3ff|+0.0000000000000X-3ff|+0.0000000000000X-3ff|+0.0000000000000X-3ff|+0.0000000000000X-3ff|+0.0000000000000X-3ff|+0.0000000000000X-3ff|+0.0000000000000X-3ff|+0.0000000000000X-3ff|+0.0000000000000X-3ff"},
    {"gen_eq_lt", 132,
     "capture noisily generate double g_eq_lt = u == v < w\nlocal __rc = _rc",
     "%21x g_eq_lt[1]\t%21x g_eq_lt[2]\t%21x g_eq_lt[3]\t%21x g_eq_lt[4]\t%21x g_eq_lt[5]\t%21x g_eq_lt[6]\t%21x g_eq_lt[7]\t%21x g_eq_lt[8]\t%21x g_eq_lt[9]\t%21x g_eq_lt[10]\t%21x g_eq_lt[11]\t%21x g_eq_lt[12]\t%21x g_eq_lt[13]\t%21x g_eq_lt[14]\t%21x g_eq_lt[15]\t%21x g_eq_lt[16]\t%21x g_eq_lt[17]\t%21x g_eq_lt[18]\t%21x g_eq_lt[19]\t%21x g_eq_lt[20]\t%21x g_eq_lt[21]\t%21x g_eq_lt[22]\t%21x g_eq_lt[23]\t%21x g_eq_lt[24]\t%21x g_eq_lt[25]\t%21x g_eq_lt[26]\t%21x g_eq_lt[27]",
     "0|+1.0000000000000X+000|+0.0000000000000X-3ff|+0.0000000000000X-3ff|+1.0000000000000X+000|+1.0000000000000X+000|+0.0000000000000X-3ff|+1.0000000000000X+000|+1.0000000000000X+000|+1.0000000000000X+000|+0.0000000000000X-3ff|+1.0000000000000X+000|+1.0000000000000X+000|+0.0000000000000X-3ff|+0.0000000000000X-3ff|+1.0000000000000X+000|+0.0000000000000X-3ff|+0.0000000000000X-3ff|+0.0000000000000X-3ff|+0.0000000000000X-3ff|+0.0000000000000X-3ff|+0.0000000000000X-3ff|+0.0000000000000X-3ff|+0.0000000000000X-3ff|+0.0000000000000X-3ff|+0.0000000000000X-3ff|+0.0000000000000X-3ff|+0.0000000000000X-3ff"},
    {"gen_eq_ge", 145,
     "capture noisily generate double g_eq_ge = u == v >= w\nlocal __rc = _rc",
     "%21x g_eq_ge[1]\t%21x g_eq_ge[2]\t%21x g_eq_ge[3]\t%21x g_eq_ge[4]\t%21x g_eq_ge[5]\t%21x g_eq_ge[6]\t%21x g_eq_ge[7]\t%21x g_eq_ge[8]\t%21x g_eq_ge[9]\t%21x g_eq_ge[10]\t%21x g_eq_ge[11]\t%21x g_eq_ge[12]\t%21x g_eq_ge[13]\t%21x g_eq_ge[14]\t%21x g_eq_ge[15]\t%21x g_eq_ge[16]\t%21x g_eq_ge[17]\t%21x g_eq_ge[18]\t%21x g_eq_ge[19]\t%21x g_eq_ge[20]\t%21x g_eq_ge[21]\t%21x g_eq_ge[22]\t%21x g_eq_ge[23]\t%21x g_eq_ge[24]\t%21x g_eq_ge[25]\t%21x g_eq_ge[26]\t%21x g_eq_ge[27]",
     "0|+0.0000000000000X-3ff|+1.0000000000000X+000|+1.0000000000000X+000|+0.0000000000000X-3ff|+0.0000000000000X-3ff|+1.0000000000000X+000|+0.0000000000000X-3ff|+0.0000000000000X-3ff|+0.0000000000000X-3ff|+1.0000000000000X+000|+0.0000000000000X-3ff|+0.0000000000000X-3ff|+1.0000000000000X+000|+1.0000000000000X+000|+0.0000000000000X-3ff|+1.0000000000000X+000|+1.0000000000000X+000|+1.0000000000000X+000|+0.0000000000000X-3ff|+0.0000000000000X-3ff|+0.0000000000000X-3ff|+0.0000000000000X-3ff|+0.0000000000000X-3ff|+0.0000000000000X-3ff|+0.0000000000000X-3ff|+0.0000000000000X-3ff|+0.0000000000000X-3ff"},
    {"gen_eq_le", 158,
     "capture noisily generate double g_eq_le = u == v <= w\nlocal __rc = _rc",
     "%21x g_eq_le[1]\t%21x g_eq_le[2]\t%21x g_eq_le[3]\t%21x g_eq_le[4]\t%21x g_eq_le[5]\t%21x g_eq_le[6]\t%21x g_eq_le[7]\t%21x g_eq_le[8]\t%21x g_eq_le[9]\t%21x g_eq_le[10]\t%21x g_eq_le[11]\t%21x g_eq_le[12]\t%21x g_eq_le[13]\t%21x g_eq_le[14]\t%21x g_eq_le[15]\t%21x g_eq_le[16]\t%21x g_eq_le[17]\t%21x g_eq_le[18]\t%21x g_eq_le[19]\t%21x g_eq_le[20]\t%21x g_eq_le[21]\t%21x g_eq_le[22]\t%21x g_eq_le[23]\t%21x g_eq_le[24]\t%21x g_eq_le[25]\t%21x g_eq_le[26]\t%21x g_eq_le[27]",
     "0|+0.0000000000000X-3ff|+0.0000000000000X-3ff|+0.0000000000000X-3ff|+1.0000000000000X+000|+0.0000000000000X-3ff|+0.0000000000000X-3ff|+1.0000000000000X+000|+1.0000000000000X+000|+0.0000000000000X-3ff|+1.0000000000000X+000|+1.0000000000000X+000|+1.0000000000000X+000|+0.0000000000000X-3ff|+1.0000000000000X+000|+1.0000000000000X+000|+0.0000000000000X-3ff|+0.0000000000000X-3ff|+1.0000000000000X+000|+0.0000000000000X-3ff|+0.0000000000000X-3ff|+0.0000000000000X-3ff|+0.0000000000000X-3ff|+0.0000000000000X-3ff|+0.0000000000000X-3ff|+0.0000000000000X-3ff|+0.0000000000000X-3ff|+0.0000000000000X-3ff"},
    {"gen_ne_eq", 171,
     "capture noisily generate double g_ne_eq = u != v == w\nlocal __rc = _rc",
     "%21x g_ne_eq[1]\t%21x g_ne_eq[2]\t%21x g_ne_eq[3]\t%21x g_ne_eq[4]\t%21x g_ne_eq[5]\t%21x g_ne_eq[6]\t%21x g_ne_eq[7]\t%21x g_ne_eq[8]\t%21x g_ne_eq[9]\t%21x g_ne_eq[10]\t%21x g_ne_eq[11]\t%21x g_ne_eq[12]\t%21x g_ne_eq[13]\t%21x g_ne_eq[14]\t%21x g_ne_eq[15]\t%21x g_ne_eq[16]\t%21x g_ne_eq[17]\t%21x g_ne_eq[18]\t%21x g_ne_eq[19]\t%21x g_ne_eq[20]\t%21x g_ne_eq[21]\t%21x g_ne_eq[22]\t%21x g_ne_eq[23]\t%21x g_ne_eq[24]\t%21x g_ne_eq[25]\t%21x g_ne_eq[26]\t%21x g_ne_eq[27]",
     "0|+1.0000000000000X+000|+0.0000000000000X-3ff|+0.0000000000000X-3ff|+0.0000000000000X-3ff|+1.0000000000000X+000|+0.0000000000000X-3ff|+0.0000000000000X-3ff|+1.0000000000000X+000|+0.0000000000000X-3ff|+0.0000000000000X-3ff|+1.0000000000000X+000|+0.0000000000000X-3ff|+1.0000000000000X+000|+0.0000000000000X-3ff|+0.0000000000000X-3ff|+0.0000000000000X-3ff|+1.0000000000000X+000|+0.0000000000000X-3ff|+0.0000000000000X-3ff|+1.0000000000000X+000|+0.0000000000000X-3ff|+0.0000000000000X-3ff|+1.0000000000000X+000|+0.0000000000000X-3ff|+1.0000000000000X+000|+0.0000000000000X-3ff|+0.0000000000000X-3ff"},
    {"gen_ne_ne", 184,
     "capture noisily generate double g_ne_ne = u != v != w\nlocal __rc = _rc",
     "%21x g_ne_ne[1]\t%21x g_ne_ne[2]\t%21x g_ne_ne[3]\t%21x g_ne_ne[4]\t%21x g_ne_ne[5]\t%21x g_ne_ne[6]\t%21x g_ne_ne[7]\t%21x g_ne_ne[8]\t%21x g_ne_ne[9]\t%21x g_ne_ne[10]\t%21x g_ne_ne[11]\t%21x g_ne_ne[12]\t%21x g_ne_ne[13]\t%21x g_ne_ne[14]\t%21x g_ne_ne[15]\t%21x g_ne_ne[16]\t%21x g_ne_ne[17]\t%21x g_ne_ne[18]\t%21x g_ne_ne[19]\t%21x g_ne_ne[20]\t%21x g_ne_ne[21]\t%21x g_ne_ne[22]\t%21x g_ne_ne[23]\t%21x g_ne_ne[24]\t%21x g_ne_ne[25]\t%21x g_ne_ne[26]\t%21x g_ne_ne[27]",
     "0|+0.0000000000000X-3ff|+1.0000000000000X+000|+1.0000000000000X+000|+1.0000000000000X+000|+0.0000000000000X-3ff|+1.0000000000000X+000|+1.0000000000000X+000|+0.0000000000000X-3ff|+1.0000000000000X+000|+1.0000000000000X+000|+0.0000000000000X-3ff|+1.0000000000000X+000|+0.0000000000000X-3ff|+1.0000000000000X+000|+1.0000000000000X+000|+1.0000000000000X+000|+0.0000000000000X-3ff|+1.0000000000000X+000|+1.0000000000000X+000|+0.0000000000000X-3ff|+1.0000000000000X+000|+1.0000000000000X+000|+0.0000000000000X-3ff|+1.0000000000000X+000|+0.0000000000000X-3ff|+1.0000000000000X+000|+1.0000000000000X+000"},
    {"gen_ne_gt", 197,
     "capture noisily generate double g_ne_gt = u != v > w\nlocal __rc = _rc",
     "%21x g_ne_gt[1]\t%21x g_ne_gt[2]\t%21x g_ne_gt[3]\t%21x g_ne_gt[4]\t%21x g_ne_gt[5]\t%21x g_ne_gt[6]\t%21x g_ne_gt[7]\t%21x g_ne_gt[8]\t%21x g_ne_gt[9]\t%21x g_ne_gt[10]\t%21x g_ne_gt[11]\t%21x g_ne_gt[12]\t%21x g_ne_gt[13]\t%21x g_ne_gt[14]\t%21x g_ne_gt[15]\t%21x g_ne_gt[16]\t%21x g_ne_gt[17]\t%21x g_ne_gt[18]\t%21x g_ne_gt[19]\t%21x g_ne_gt[20]\t%21x g_ne_gt[21]\t%21x g_ne_gt[22]\t%21x g_ne_gt[23]\t%21x g_ne_gt[24]\t%21x g_ne_gt[25]\t%21x g_ne_gt[26]\t%21x g_ne_gt[27]",
     "0|+0.0000000000000X-3ff|+0.0000000000000X-3ff|+0.0000000000000X-3ff|+1.0000000000000X+000|+0.0000000000000X-3ff|+0.0000000000000X-3ff|+1.0000000000000X+000|+0.0000000000000X-3ff|+0.0000000000000X-3ff|+1.0000000000000X+000|+0.0000000000000X-3ff|+0.0000000000000X-3ff|+0.0000000000000X-3ff|+0.0000000000000X-3ff|+0.0000000000000X-3ff|+1.0000000000000X+000|+0.0000000000000X-3ff|+0.0000000000000X-3ff|+1.0000000000000X+000|+0.0000000000000X-3ff|+0.0000000000000X-3ff|+1.0000000000000X+000|+0.0000000000000X-3ff|+0.0000000000000X-3ff|+0.0000000000000X-3ff|+0.0000000000000X-3ff|+0.0000000000000X-3ff"},
    {"gen_ne_lt", 210,
     "capture noisily generate double g_ne_lt = u != v < w\nlocal __rc = _rc",
     "%21x g_ne_lt[1]\t%21x g_ne_lt[2]\t%21x g_ne_lt[3]\t%21x g_ne_lt[4]\t%21x g_ne_lt[5]\t%21x g_ne_lt[6]\t%21x g_ne_lt[7]\t%21x g_ne_lt[8]\t%21x g_ne_lt[9]\t%21x g_ne_lt[10]\t%21x g_ne_lt[11]\t%21x g_ne_lt[12]\t%21x g_ne_lt[13]\t%21x g_ne_lt[14]\t%21x g_ne_lt[15]\t%21x g_ne_lt[16]\t%21x g_ne_lt[17]\t%21x g_ne_lt[18]\t%21x g_ne_lt[19]\t%21x g_ne_lt[20]\t%21x g_ne_lt[21]\t%21x g_ne_lt[22]\t%21x g_ne_lt[23]\t%21x g_ne_lt[24]\t%21x g_ne_lt[25]\t%21x g_ne_lt[26]\t%21x g_ne_lt[27]",
     "0|+0.0000000000000X-3ff|+1.0000000000000X+000|+1.0000000000000X+000|+0.0000000000000X-3ff|+0.0000000000000X-3ff|+1.0000000000000X+000|+0.0000000000000X-3ff|+0.0000000000000X-3ff|+1.0000000000000X+000|+0.0000000000000X-3ff|+0.0000000000000X-3ff|+1.0000000000000X+000|+0.0000000000000X-3ff|+1.0000000000000X+000|+1.0000000000000X+000|+0.0000000000000X-3ff|+0.0000000000000X-3ff|+1.0000000000000X+000|+0.0000000000000X-3ff|+0.0000000000000X-3ff|+1.0000000000000X+000|+0.0000000000000X-3ff|+0.0000000000000X-3ff|+1.0000000000000X+000|+0.0000000000000X-3ff|+1.0000000000000X+000|+1.0000000000000X+000"},
    {"gen_ne_ge", 223,
     "capture noisily generate double g_ne_ge = u != v >= w\nlocal __rc = _rc",
     "%21x g_ne_ge[1]\t%21x g_ne_ge[2]\t%21x g_ne_ge[3]\t%21x g_ne_ge[4]\t%21x g_ne_ge[5]\t%21x g_ne_ge[6]\t%21x g_ne_ge[7]\t%21x g_ne_ge[8]\t%21x g_ne_ge[9]\t%21x g_ne_ge[10]\t%21x g_ne_ge[11]\t%21x g_ne_ge[12]\t%21x g_ne_ge[13]\t%21x g_ne_ge[14]\t%21x g_ne_ge[15]\t%21x g_ne_ge[16]\t%21x g_ne_ge[17]\t%21x g_ne_ge[18]\t%21x g_ne_ge[19]\t%21x g_ne_ge[20]\t%21x g_ne_ge[21]\t%21x g_ne_ge[22]\t%21x g_ne_ge[23]\t%21x g_ne_ge[24]\t%21x g_ne_ge[25]\t%21x g_ne_ge[26]\t%21x g_ne_ge[27]",
     "0|+1.0000000000000X+000|+0.0000000000000X-3ff|+0.0000000000000X-3ff|+1.0000000000000X+000|+1.0000000000000X+000|+0.0000000000000X-3ff|+1.0000000000000X+000|+1.0000000000000X+000|+0.0000000000000X-3ff|+1.0000000000000X+000|+1.0000000000000X+000|+0.0000000000000X-3ff|+1.0000000000000X+000|+0.0000000000000X-3ff|+0.0000000000000X-3ff|+1.0000000000000X+000|+1.0000000000000X+000|+0.0000000000000X-3ff|+1.0000000000000X+000|+1.0000000000000X+000|+0.0000000000000X-3ff|+1.0000000000000X+000|+1.0000000000000X+000|+0.0000000000000X-3ff|+1.0000000000000X+000|+0.0000000000000X-3ff|+0.0000000000000X-3ff"},
    {"gen_ne_le", 236,
     "capture noisily generate double g_ne_le = u != v <= w\nlocal __rc = _rc",
     "%21x g_ne_le[1]\t%21x g_ne_le[2]\t%21x g_ne_le[3]\t%21x g_ne_le[4]\t%21x g_ne_le[5]\t%21x g_ne_le[6]\t%21x g_ne_le[7]\t%21x g_ne_le[8]\t%21x g_ne_le[9]\t%21x g_ne_le[10]\t%21x g_ne_le[11]\t%21x g_ne_le[12]\t%21x g_ne_le[13]\t%21x g_ne_le[14]\t%21x g_ne_le[15]\t%21x g_ne_le[16]\t%21x g_ne_le[17]\t%21x g_ne_le[18]\t%21x g_ne_le[19]\t%21x g_ne_le[20]\t%21x g_ne_le[21]\t%21x g_ne_le[22]\t%21x g_ne_le[23]\t%21x g_ne_le[24]\t%21x g_ne_le[25]\t%21x g_ne_le[26]\t%21x g_ne_le[27]",
     "0|+1.0000000000000X+000|+1.0000000000000X+000|+1.0000000000000X+000|+0.0000000000000X-3ff|+1.0000000000000X+000|+1.0000000000000X+000|+0.0000000000000X-3ff|+1.0000000000000X+000|+1.0000000000000X+000|+0.0000000000000X-3ff|+1.0000000000000X+000|+1.0000000000000X+000|+1.0000000000000X+000|+1.0000000000000X+000|+1.0000000000000X+000|+0.0000000000000X-3ff|+1.0000000000000X+000|+1.0000000000000X+000|+0.0000000000000X-3ff|+1.0000000000000X+000|+1.0000000000000X+000|+0.0000000000000X-3ff|+1.0000000000000X+000|+1.0000000000000X+000|+1.0000000000000X+000|+1.0000000000000X+000|+1.0000000000000X+000"},
    {"gen_gt_eq", 249,
     "capture noisily generate double g_gt_eq = u > v == w\nlocal __rc = _rc",
     "%21x g_gt_eq[1]\t%21x g_gt_eq[2]\t%21x g_gt_eq[3]\t%21x g_gt_eq[4]\t%21x g_gt_eq[5]\t%21x g_gt_eq[6]\t%21x g_gt_eq[7]\t%21x g_gt_eq[8]\t%21x g_gt_eq[9]\t%21x g_gt_eq[10]\t%21x g_gt_eq[11]\t%21x g_gt_eq[12]\t%21x g_gt_eq[13]\t%21x g_gt_eq[14]\t%21x g_gt_eq[15]\t%21x g_gt_eq[16]\t%21x g_gt_eq[17]\t%21x g_gt_eq[18]\t%21x g_gt_eq[19]\t%21x g_gt_eq[20]\t%21x g_gt_eq[21]\t%21x g_gt_eq[22]\t%21x g_gt_eq[23]\t%21x g_gt_eq[24]\t%21x g_gt_eq[25]\t%21x g_gt_eq[26]\t%21x g_gt_eq[27]",
     "0|+1.0000000000000X+000|+0.0000000000000X-3ff|+0.0000000000000X-3ff|+1.0000000000000X+000|+0.0000000000000X-3ff|+0.0000000000000X-3ff|+1.0000000000000X+000|+0.0000000000000X-3ff|+0.0000000000000X-3ff|+0.0000000000000X-3ff|+1.0000000000000X+000|+0.0000000000000X-3ff|+1.0000000000000X+000|+0.0000000000000X-3ff|+0.0000000000000X-3ff|+1.0000000000000X+000|+0.0000000000000X-3ff|+0.0000000000000X-3ff|+0.0000000000000X-3ff|+1.0000000000000X+000|+0.0000000000000X-3ff|+0.0000000000000X-3ff|+1.0000000000000X+000|+0.0000000000000X-3ff|+1.0000000000000X+000|+0.0000000000000X-3ff|+0.0000000000000X-3ff"},
    {"gen_gt_ne", 262,
     "capture noisily generate double g_gt_ne = u > v != w\nlocal __rc = _rc",
     "%21x g_gt_ne[1]\t%21x g_gt_ne[2]\t%21x g_gt_ne[3]\t%21x g_gt_ne[4]\t%21x g_gt_ne[5]\t%21x g_gt_ne[6]\t%21x g_gt_ne[7]\t%21x g_gt_ne[8]\t%21x g_gt_ne[9]\t%21x g_gt_ne[10]\t%21x g_gt_ne[11]\t%21x g_gt_ne[12]\t%21x g_gt_ne[13]\t%21x g_gt_ne[14]\t%21x g_gt_ne[15]\t%21x g_gt_ne[16]\t%21x g_gt_ne[17]\t%21x g_gt_ne[18]\t%21x g_gt_ne[19]\t%21x g_gt_ne[20]\t%21x g_gt_ne[21]\t%21x g_gt_ne[22]\t%21x g_gt_ne[23]\t%21x g_gt_ne[24]\t%21x g_gt_ne[25]\t%21x g_gt_ne[26]\t%21x g_gt_ne[27]",
     "0|+0.0000000000000X-3ff|+0.0000000000000X-3ff|+0.0000000000000X-3ff|+0.0000000000000X-3ff|+0.0000000000000X-3ff|+0.0000000000000X-3ff|+0.0000000000000X-3ff|+0.0000000000000X-3ff|+0.0000000000000X-3ff|+1.0000000000000X+000|+0.0000000000000X-3ff|+0.0000000000000X-3ff|+0.0000000000000X-3ff|+1.0000000000000X+000|+0.0000000000000X-3ff|+0.0000000000000X-3ff|+0.0000000000000X-3ff|+1.0000000000000X+000|+1.0000000000000X+000|+1.0000000000000X+000|+1.0000000000000X+000|+1.0000000000000X+000|+1.0000000000000X+000|+1.0000000000000X+000|+1.0000000000000X+000|+1.0000000000000X+000|+1.0000000000000X+000"},
    {"gen_gt_gt", 275,
     "capture noisily generate double g_gt_gt = u > v > w\nlocal __rc = _rc",
     "%21x g_gt_gt[1]\t%21x g_gt_gt[2]\t%21x g_gt_gt[3]\t%21x g_gt_gt[4]\t%21x g_gt_gt[5]\t%21x g_gt_gt[6]\t%21x g_gt_gt[7]\t%21x g_gt_gt[8]\t%21x g_gt_gt[9]\t%21x g_gt_gt[10]\t%21x g_gt_gt[11]\t%21x g_gt_gt[12]\t%21x g_gt_gt[13]\t%21x g_gt_gt[14]\t%21x g_gt_gt[15]\t%21x g_gt_gt[16]\t%21x g_gt_gt[17]\t%21x g_gt_gt[18]\t%21x g_gt_gt[19]\t%21x g_gt_gt[20]\t%21x g_gt_gt[21]\t%21x g_gt_gt[22]\t%21x g_gt_gt[23]\t%21x g_gt_gt[24]\t%21x g_gt_gt[25]\t%21x g_gt_gt[26]\t%21x g_gt_gt[27]",
     "0|+0.0000000000000X-3ff|+0.0000000000000X-3ff|+0.0000000000000X-3ff|+0.0000000000000X-3ff|+0.0000000000000X-3ff|+0.0000000000000X-3ff|+0.0000000000000X-3ff|+0.0000000000000X-3ff|+0.0000000000000X-3ff|+1.0000000000000X+000|+0.0000000000000X-3ff|+0.0000000000000X-3ff|+0.0000000000000X-3ff|+0.0000000000000X-3ff|+0.0000000000000X-3ff|+0.0000000000000X-3ff|+0.0000000000000X-3ff|+0.0000000000000X-3ff|+1.0000000000000X+000|+0.0000000000000X-3ff|+0.0000000000000X-3ff|+1.0000000000000X+000|+0.0000000000000X-3ff|+0.0000000000000X-3ff|+0.0000000000000X-3ff|+0.0000000000000X-3ff|+0.0000000000000X-3ff"},
    {"gen_gt_lt", 288,
     "capture noisily generate double g_gt_lt = u > v < w\nlocal __rc = _rc",
     "%21x g_gt_lt[1]\t%21x g_gt_lt[2]\t%21x g_gt_lt[3]\t%21x g_gt_lt[4]\t%21x g_gt_lt[5]\t%21x g_gt_lt[6]\t%21x g_gt_lt[7]\t%21x g_gt_lt[8]\t%21x g_gt_lt[9]\t%21x g_gt_lt[10]\t%21x g_gt_lt[11]\t%21x g_gt_lt[12]\t%21x g_gt_lt[13]\t%21x g_gt_lt[14]\t%21x g_gt_lt[15]\t%21x g_gt_lt[16]\t%21x g_gt_lt[17]\t%21x g_gt_lt[18]\t%21x g_gt_lt[19]\t%21x g_gt_lt[20]\t%21x g_gt_lt[21]\t%21x g_gt_lt[22]\t%21x g_gt_lt[23]\t%21x g_gt_lt[24]\t%21x g_gt_lt[25]\t%21x g_gt_lt[26]\t%21x g_gt_lt[27]",
     "0|+0.0000000000000X-3ff|+1.0000000000000X+000|+1.0000000000000X+000|+0.0000000000000X-3ff|+1.0000000000000X+000|+1.0000000000000X+000|+0.0000000000000X-3ff|+1.0000000000000X+000|+1.0000000000000X+000|+0.0000000000000X-3ff|+0.0000000000000X-3ff|+1.0000000000000X+000|+0.0000000000000X-3ff|+1.0000000000000X+000|+1.0000000000000X+000|+0.0000000000000X-3ff|+1.0000000000000X+000|+1.0000000000000X+000|+0.0000000000000X-3ff|+0.0000000000000X-3ff|+1.0000000000000X+000|+0.0000000000000X-3ff|+0.0000000000000X-3ff|+1.0000000000000X+000|+0.0000000000000X-3ff|+1.0000000000000X+000|+1.0000000000000X+000"},
    {"gen_gt_ge", 301,
     "capture noisily generate double g_gt_ge = u > v >= w\nlocal __rc = _rc",
     "%21x g_gt_ge[1]\t%21x g_gt_ge[2]\t%21x g_gt_ge[3]\t%21x g_gt_ge[4]\t%21x g_gt_ge[5]\t%21x g_gt_ge[6]\t%21x g_gt_ge[7]\t%21x g_gt_ge[8]\t%21x g_gt_ge[9]\t%21x g_gt_ge[10]\t%21x g_gt_ge[11]\t%21x g_gt_ge[12]\t%21x g_gt_ge[13]\t%21x g_gt_ge[14]\t%21x g_gt_ge[15]\t%21x g_gt_ge[16]\t%21x g_gt_ge[17]\t%21x g_gt_ge[18]\t%21x g_gt_ge[19]\t%21x g_gt_ge[20]\t%21x g_gt_ge[21]\t%21x g_gt_ge[22]\t%21x g_gt_ge[23]\t%21x g_gt_ge[24]\t%21x g_gt_ge[25]\t%21x g_gt_ge[26]\t%21x g_gt_ge[27]",
     "0|+1.0000000000000X+000|+0.0000000000000X-3ff|+0.0000000000000X-3ff|+1.0000000000000X+000|+0.0000000000000X-3ff|+0.0000000000000X-3ff|+1.0000000000000X+000|+0.0000000000000X-3ff|+0.0000000000000X-3ff|+1.0000000000000X+000|+1.0000000000000X+000|+0.0000000000000X-3ff|+1.0000000000000X+000|+0.0000000000000X-3ff|+0.0000000000000X-3ff|+1.0000000000000X+000|+0.0000000000000X-3ff|+0.0000000000000X-3ff|+1.0000000000000X+000|+1.0000000000000X+000|+0.0000000000000X-3ff|+1.0000000000000X+000|+1.0000000000000X+000|+0.0000000000000X-3ff|+1.0000000000000X+000|+0.0000000000000X-3ff|+0.0000000000000X-3ff"},
    {"gen_gt_le", 314,
     "capture noisily generate double g_gt_le = u > v <= w\nlocal __rc = _rc",
     "%21x g_gt_le[1]\t%21x g_gt_le[2]\t%21x g_gt_le[3]\t%21x g_gt_le[4]\t%21x g_gt_le[5]\t%21x g_gt_le[6]\t%21x g_gt_le[7]\t%21x g_gt_le[8]\t%21x g_gt_le[9]\t%21x g_gt_le[10]\t%21x g_gt_le[11]\t%21x g_gt_le[12]\t%21x g_gt_le[13]\t%21x g_gt_le[14]\t%21x g_gt_le[15]\t%21x g_gt_le[16]\t%21x g_gt_le[17]\t%21x g_gt_le[18]\t%21x g_gt_le[19]\t%21x g_gt_le[20]\t%21x g_gt_le[21]\t%21x g_gt_le[22]\t%21x g_gt_le[23]\t%21x g_gt_le[24]\t%21x g_gt_le[25]\t%21x g_gt_le[26]\t%21x g_gt_le[27]",
     "0|+1.0000000000000X+000|+1.0000000000000X+000|+1.0000000000000X+000|+1.0000000000000X+000|+1.0000000000000X+000|+1.0000000000000X+000|+1.0000000000000X+000|+1.0000000000000X+000|+1.0000000000000X+000|+0.0000000000000X-3ff|+1.0000000000000X+000|+1.0000000000000X+000|+1.0000000000000X+000|+1.0000000000000X+000|+1.0000000000000X+000|+1.0000000000000X+000|+1.0000000000000X+000|+1.0000000000000X+000|+0.0000000000000X-3ff|+1.0000000000000X+000|+1.0000000000000X+000|+0.0000000000000X-3ff|+1.0000000000000X+000|+1.0000000000000X+000|+1.0000000000000X+000|+1.0000000000000X+000|+1.0000000000000X+000"},
    {"gen_lt_eq", 327,
     "capture noisily generate double g_lt_eq = u < v == w\nlocal __rc = _rc",
     "%21x g_lt_eq[1]\t%21x g_lt_eq[2]\t%21x g_lt_eq[3]\t%21x g_lt_eq[4]\t%21x g_lt_eq[5]\t%21x g_lt_eq[6]\t%21x g_lt_eq[7]\t%21x g_lt_eq[8]\t%21x g_lt_eq[9]\t%21x g_lt_eq[10]\t%21x g_lt_eq[11]\t%21x g_lt_eq[12]\t%21x g_lt_eq[13]\t%21x g_lt_eq[14]\t%21x g_lt_eq[15]\t%21x g_lt_eq[16]\t%21x g_lt_eq[17]\t%21x g_lt_eq[18]\t%21x g_lt_eq[19]\t%21x g_lt_eq[20]\t%21x g_lt_eq[21]\t%21x g_lt_eq[22]\t%21x g_lt_eq[23]\t%21x g_lt_eq[24]\t%21x g_lt_eq[25]\t%21x g_lt_eq[26]\t%21x g_lt_eq[27]",
     "0|+1.0000000000000X+000|+0.0000000000000X-3ff|+0.0000000000000X-3ff|+0.0000000000000X-3ff|+1.0000000000000X+000|+0.0000000000000X-3ff|+0.0000000000000X-3ff|+1.0000000000000X+000|+0.0000000000000X-3ff|+1.0000000000000X+000|+0.0000000000000X-3ff|+0.0000000000000X-3ff|+1.0000000000000X+000|+0.0000000000000X-3ff|+0.0000000000000X-3ff|+0.0000000000000X-3ff|+1.0000000000000X+000|+0.0000000000000X-3ff|+1.0000000000000X+000|+0.0000000000000X-3ff|+0.0000000000000X-3ff|+1.0000000000000X+000|+0.0000000000000X-3ff|+0.0000000000000X-3ff|+1.0000000000000X+000|+0.0000000000000X-3ff|+0.0000000000000X-3ff"},
    {"gen_lt_ne", 340,
     "capture noisily generate double g_lt_ne = u < v != w\nlocal __rc = _rc",
     "%21x g_lt_ne[1]\t%21x g_lt_ne[2]\t%21x g_lt_ne[3]\t%21x g_lt_ne[4]\t%21x g_lt_ne[5]\t%21x g_lt_ne[6]\t%21x g_lt_ne[7]\t%21x g_lt_ne[8]\t%21x g_lt_ne[9]\t%21x g_lt_ne[10]\t%21x g_lt_ne[11]\t%21x g_lt_ne[12]\t%21x g_lt_ne[13]\t%21x g_lt_ne[14]\t%21x g_lt_ne[15]\t%21x g_lt_ne[16]\t%21x g_lt_ne[17]\t%21x g_lt_ne[18]\t%21x g_lt_ne[19]\t%21x g_lt_ne[20]\t%21x g_lt_ne[21]\t%21x g_lt_ne[22]\t%21x g_lt_ne[23]\t%21x g_lt_ne[24]\t%21x g_lt_ne[25]\t%21x g_lt_ne[26]\t%21x g_lt_ne[27]",
     "0|+0.0000000000000X-3ff|+1.0000000000000X+000|+1.0000000000000X+000|+1.0000000000000X+000|+0.0000000000000X-3ff|+1.0000000000000X+000|+1.0000000000000X+000|+1.0000000000000X+000|+0.0000000000000X-3ff|+0.0000000000000X-3ff|+0.0000000000000X-3ff|+0.0000000000000X-3ff|+0.0000000000000X-3ff|+0.0000000000000X-3ff|+0.0000000000000X-3ff|+0.0000000000000X-3ff|+0.0000000000000X-3ff|+0.0000000000000X-3ff|+0.0000000000000X-3ff|+0.0000000000000X-3ff|+0.0000000000000X-3ff|+0.0000000000000X-3ff|+0.0000000000000X-3ff|+0.0000000000000X-3ff|+0.0000000000000X-3ff|+0.0000000000000X-3ff|+0.0000000000000X-3ff"},
    {"gen_lt_gt", 353,
     "capture noisily generate double g_lt_gt = u < v > w\nlocal __rc = _rc",
     "%21x g_lt_gt[1]\t%21x g_lt_gt[2]\t%21x g_lt_gt[3]\t%21x g_lt_gt[4]\t%21x g_lt_gt[5]\t%21x g_lt_gt[6]\t%21x g_lt_gt[7]\t%21x g_lt_gt[8]\t%21x g_lt_gt[9]\t%21x g_lt_gt[10]\t%21x g_lt_gt[11]\t%21x g_lt_gt[12]\t%21x g_lt_gt[13]\t%21x g_lt_gt[14]\t%21x g_lt_gt[15]\t%21x g_lt_gt[16]\t%21x g_lt_gt[17]\t%21x g_lt_gt[18]\t%21x g_lt_gt[19]\t%21x g_lt_gt[20]\t%21x g_lt_gt[21]\t%21x g_lt_gt[22]\t%21x g_lt_gt[23]\t%21x g_lt_gt[24]\t%21x g_lt_gt[25]\t%21x g_lt_gt[26]\t%21x g_lt_gt[27]",
     "0|+0.0000000000000X-3ff|+0.0000000000000X-3ff|+0.0000000000000X-3ff|+1.0000000000000X+000|+0.0000000000000X-3ff|+0.0000000000000X-3ff|+1.0000000000000X+000|+1.0000000000000X+000|+0.0000000000000X-3ff|+0.0000000000000X-3ff|+0.0000000000000X-3ff|+0.0000000000000X-3ff|+0.0000000000000X-3ff|+0.0000000000000X-3ff|+0.0000000000000X-3ff|+0.0000000000000X-3ff|+0.0000000000000X-3ff|+0.0000000000000X-3ff|+0.0000000000000X-3ff|+0.0000000000000X-3ff|+0.0000000000000X-3ff|+0.0000000000000X-3ff|+0.0000000000000X-3ff|+0.0000000000000X-3ff|+0.0000000000000X-3ff|+0.0000000000000X-3ff|+0.0000000000000X-3ff"},
    {"gen_lt_lt", 366,
     "capture noisily generate double g_lt_lt = u < v < w\nlocal __rc = _rc",
     "%21x g_lt_lt[1]\t%21x g_lt_lt[2]\t%21x g_lt_lt[3]\t%21x g_lt_lt[4]\t%21x g_lt_lt[5]\t%21x g_lt_lt[6]\t%21x g_lt_lt[7]\t%21x g_lt_lt[8]\t%21x g_lt_lt[9]\t%21x g_lt_lt[10]\t%21x g_lt_lt[11]\t%21x g_lt_lt[12]\t%21x g_lt_lt[13]\t%21x g_lt_lt[14]\t%21x g_lt_lt[15]\t%21x g_lt_lt[16]\t%21x g_lt_lt[17]\t%21x g_lt_lt[18]\t%21x g_lt_lt[19]\t%21x g_lt_lt[20]\t%21x g_lt_lt[21]\t%21x g_lt_lt[22]\t%21x g_lt_lt[23]\t%21x g_lt_lt[24]\t%21x g_lt_lt[25]\t%21x g_lt_lt[26]\t%21x g_lt_lt[27]",
     "0|+0.0000000000000X-3ff|+1.0000000000000X+000|+1.0000000000000X+000|+0.0000000000000X-3ff|+0.0000000000000X-3ff|+1.0000000000000X+000|+0.0000000000000X-3ff|+0.0000000000000X-3ff|+1.0000000000000X+000|+0.0000000000000X-3ff|+1.0000000000000X+000|+1.0000000000000X+000|+0.0000000000000X-3ff|+1.0000000000000X+000|+1.0000000000000X+000|+0.0000000000000X-3ff|+0.0000000000000X-3ff|+1.0000000000000X+000|+0.0000000000000X-3ff|+1.0000000000000X+000|+1.0000000000000X+000|+0.0000000000000X-3ff|+1.0000000000000X+000|+1.0000000000000X+000|+0.0000000000000X-3ff|+1.0000000000000X+000|+1.0000000000000X+000"},
    {"gen_lt_ge", 379,
     "capture noisily generate double g_lt_ge = u < v >= w\nlocal __rc = _rc",
     "%21x g_lt_ge[1]\t%21x g_lt_ge[2]\t%21x g_lt_ge[3]\t%21x g_lt_ge[4]\t%21x g_lt_ge[5]\t%21x g_lt_ge[6]\t%21x g_lt_ge[7]\t%21x g_lt_ge[8]\t%21x g_lt_ge[9]\t%21x g_lt_ge[10]\t%21x g_lt_ge[11]\t%21x g_lt_ge[12]\t%21x g_lt_ge[13]\t%21x g_lt_ge[14]\t%21x g_lt_ge[15]\t%21x g_lt_ge[16]\t%21x g_lt_ge[17]\t%21x g_lt_ge[18]\t%21x g_lt_ge[19]\t%21x g_lt_ge[20]\t%21x g_lt_ge[21]\t%21x g_lt_ge[22]\t%21x g_lt_ge[23]\t%21x g_lt_ge[24]\t%21x g_lt_ge[25]\t%21x g_lt_ge[26]\t%21x g_lt_ge[27]",
     "0|+1.0000000000000X+000|+0.0000000000000X-3ff|+0.0000000000000X-3ff|+1.0000000000000X+000|+1.0000000000000X+000|+0.0000000000000X-3ff|+1.0000000000000X+000|+1.0000000000000X+000|+0.0000000000000X-3ff|+1.0000000000000X+000|+0.0000000000000X-3ff|+0.0000000000000X-3ff|+1.0000000000000X+000|+0.0000000000000X-3ff|+0.0000000000000X-3ff|+1.0000000000000X+000|+1.0000000000000X+000|+0.0000000000000X-3ff|+1.0000000000000X+000|+0.0000000000000X-3ff|+0.0000000000000X-3ff|+1.0000000000000X+000|+0.0000000000000X-3ff|+0.0000000000000X-3ff|+1.0000000000000X+000|+0.0000000000000X-3ff|+0.0000000000000X-3ff"},
    {"gen_lt_le", 392,
     "capture noisily generate double g_lt_le = u < v <= w\nlocal __rc = _rc",
     "%21x g_lt_le[1]\t%21x g_lt_le[2]\t%21x g_lt_le[3]\t%21x g_lt_le[4]\t%21x g_lt_le[5]\t%21x g_lt_le[6]\t%21x g_lt_le[7]\t%21x g_lt_le[8]\t%21x g_lt_le[9]\t%21x g_lt_le[10]\t%21x g_lt_le[11]\t%21x g_lt_le[12]\t%21x g_lt_le[13]\t%21x g_lt_le[14]\t%21x g_lt_le[15]\t%21x g_lt_le[16]\t%21x g_lt_le[17]\t%21x g_lt_le[18]\t%21x g_lt_le[19]\t%21x g_lt_le[20]\t%21x g_lt_le[21]\t%21x g_lt_le[22]\t%21x g_lt_le[23]\t%21x g_lt_le[24]\t%21x g_lt_le[25]\t%21x g_lt_le[26]\t%21x g_lt_le[27]",
     "0|+1.0000000000000X+000|+1.0000000000000X+000|+1.0000000000000X+000|+0.0000000000000X-3ff|+1.0000000000000X+000|+1.0000000000000X+000|+0.0000000000000X-3ff|+1.0000000000000X+000|+1.0000000000000X+000|+1.0000000000000X+000|+1.0000000000000X+000|+1.0000000000000X+000|+1.0000000000000X+000|+1.0000000000000X+000|+1.0000000000000X+000|+0.0000000000000X-3ff|+1.0000000000000X+000|+1.0000000000000X+000|+1.0000000000000X+000|+1.0000000000000X+000|+1.0000000000000X+000|+1.0000000000000X+000|+1.0000000000000X+000|+1.0000000000000X+000|+1.0000000000000X+000|+1.0000000000000X+000|+1.0000000000000X+000"},
    {"gen_ge_eq", 405,
     "capture noisily generate double g_ge_eq = u >= v == w\nlocal __rc = _rc",
     "%21x g_ge_eq[1]\t%21x g_ge_eq[2]\t%21x g_ge_eq[3]\t%21x g_ge_eq[4]\t%21x g_ge_eq[5]\t%21x g_ge_eq[6]\t%21x g_ge_eq[7]\t%21x g_ge_eq[8]\t%21x g_ge_eq[9]\t%21x g_ge_eq[10]\t%21x g_ge_eq[11]\t%21x g_ge_eq[12]\t%21x g_ge_eq[13]\t%21x g_ge_eq[14]\t%21x g_ge_eq[15]\t%21x g_ge_eq[16]\t%21x g_ge_eq[17]\t%21x g_ge_eq[18]\t%21x g_ge_eq[19]\t%21x g_ge_eq[20]\t%21x g_ge_eq[21]\t%21x g_ge_eq[22]\t%21x g_ge_eq[23]\t%21x g_ge_eq[24]\t%21x g_ge_eq[25]\t%21x g_ge_eq[26]\t%21x g_ge_eq[27]",
     "0|+0.0000000000000X-3ff|+1.0000000000000X+000|+0.0000000000000X-3ff|+1.0000000000000X+000|+0.0000000000000X-3ff|+0.0000000000000X-3ff|+1.0000000000000X+000|+0.0000000000000X-3ff|+0.0000000000000X-3ff|+0.0000000000000X-3ff|+1.0000000000000X+000|+0.0000000000000X-3ff|+0.0000000000000X-3ff|+1.0000000000000X+000|+0.0000000000000X-3ff|+1.0000000000000X+000|+0.0000000000000X-3ff|+0.0000000000000X-3ff|+0.0000000000000X-3ff|+1.0000000000000X+000|+0.0000000000000X-3ff|+0.0000000000000X-3ff|+1.0000000000000X+000|+0.0000000000000X-3ff|+0.0000000000000X-3ff|+1.0000000000000X+000|+0.0000000000000X-3ff"},
    {"gen_ge_ne", 418,
     "capture noisily generate double g_ge_ne = u >= v != w\nlocal __rc = _rc",
     "%21x g_ge_ne[1]\t%21x g_ge_ne[2]\t%21x g_ge_ne[3]\t%21x g_ge_ne[4]\t%21x g_ge_ne[5]\t%21x g_ge_ne[6]\t%21x g_ge_ne[7]\t%21x g_ge_ne[8]\t%21x g_ge_ne[9]\t%21x g_ge_ne[10]\t%21x g_ge_ne[11]\t%21x g_ge_ne[12]\t%21x g_ge_ne[13]\t%21x g_ge_ne[14]\t%21x g_ge_ne[15]\t%21x g_ge_ne[16]\t%21x g_ge_ne[17]\t%21x g_ge_ne[18]\t%21x g_ge_ne[19]\t%21x g_ge_ne[20]\t%21x g_ge_ne[21]\t%21x g_ge_ne[22]\t%21x g_ge_ne[23]\t%21x g_ge_ne[24]\t%21x g_ge_ne[25]\t%21x g_ge_ne[26]\t%21x g_ge_ne[27]",
     "0|+1.0000000000000X+000|+0.0000000000000X-3ff|+0.0000000000000X-3ff|+0.0000000000000X-3ff|+1.0000000000000X+000|+0.0000000000000X-3ff|+0.0000000000000X-3ff|+0.0000000000000X-3ff|+1.0000000000000X+000|+1.0000000000000X+000|+1.0000000000000X+000|+1.0000000000000X+000|+1.0000000000000X+000|+1.0000000000000X+000|+1.0000000000000X+000|+1.0000000000000X+000|+1.0000000000000X+000|+1.0000000000000X+000|+1.0000000000000X+000|+1.0000000000000X+000|+1.0000000000000X+000|+1.0000000000000X+000|+1.0000000000000X+000|+1.0000000000000X+000|+1.0000000000000X+000|+1.0000000000000X+000|+1.0000000000000X+000"},
    {"gen_ge_gt", 431,
     "capture noisily generate double g_ge_gt = u >= v > w\nlocal __rc = _rc",
     "%21x g_ge_gt[1]\t%21x g_ge_gt[2]\t%21x g_ge_gt[3]\t%21x g_ge_gt[4]\t%21x g_ge_gt[5]\t%21x g_ge_gt[6]\t%21x g_ge_gt[7]\t%21x g_ge_gt[8]\t%21x g_ge_gt[9]\t%21x g_ge_gt[10]\t%21x g_ge_gt[11]\t%21x g_ge_gt[12]\t%21x g_ge_gt[13]\t%21x g_ge_gt[14]\t%21x g_ge_gt[15]\t%21x g_ge_gt[16]\t%21x g_ge_gt[17]\t%21x g_ge_gt[18]\t%21x g_ge_gt[19]\t%21x g_ge_gt[20]\t%21x g_ge_gt[21]\t%21x g_ge_gt[22]\t%21x g_ge_gt[23]\t%21x g_ge_gt[24]\t%21x g_ge_gt[25]\t%21x g_ge_gt[26]\t%21x g_ge_gt[27]",
     "0|+1.0000000000000X+000|+1.0000000000000X+000|+1.0000000000000X+000|+0.0000000000000X-3ff|+1.0000000000000X+000|+1.0000000000000X+000|+0.0000000000000X-3ff|+0.0000000000000X-3ff|+1.0000000000000X+000|+1.0000000000000X+000|+1.0000000000000X+000|+1.0000000000000X+000|+1.0000000000000X+000|+1.0000000000000X+000|+1.0000000000000X+000|+1.0000000000000X+000|+1.0000000000000X+000|+1.0000000000000X+000|+1.0000000000000X+000|+1.0000000000000X+000|+1.0000000000000X+000|+1.0000000000000X+000|+1.0000000000000X+000|+1.0000000000000X+000|+1.0000000000000X+000|+1.0000000000000X+000|+1.0000000000000X+000"},
    {"gen_ge_lt", 444,
     "capture noisily generate double g_ge_lt = u >= v < w\nlocal __rc = _rc",
     "%21x g_ge_lt[1]\t%21x g_ge_lt[2]\t%21x g_ge_lt[3]\t%21x g_ge_lt[4]\t%21x g_ge_lt[5]\t%21x g_ge_lt[6]\t%21x g_ge_lt[7]\t%21x g_ge_lt[8]\t%21x g_ge_lt[9]\t%21x g_ge_lt[10]\t%21x g_ge_lt[11]\t%21x g_ge_lt[12]\t%21x g_ge_lt[13]\t%21x g_ge_lt[14]\t%21x g_ge_lt[15]\t%21x g_ge_lt[16]\t%21x g_ge_lt[17]\t%21x g_ge_lt[18]\t%21x g_ge_lt[19]\t%21x g_ge_lt[20]\t%21x g_ge_lt[21]\t%21x g_ge_lt[22]\t%21x g_ge_lt[23]\t%21x g_ge_lt[24]\t%21x g_ge_lt[25]\t%21x g_ge_lt[26]\t%21x g_ge_lt[27]",
     "0|+1.0000000000000X+000|+0.0000000000000X-3ff|+0.0000000000000X-3ff|+1.0000000000000X+000|+1.0000000000000X+000|+0.0000000000000X-3ff|+1.0000000000000X+000|+1.0000000000000X+000|+1.0000000000000X+000|+1.0000000000000X+000|+1.0000000000000X+000|+1.0000000000000X+000|+1.0000000000000X+000|+1.0000000000000X+000|+1.0000000000000X+000|+1.0000000000000X+000|+1.0000000000000X+000|+1.0000000000000X+000|+1.0000000000000X+000|+1.0000000000000X+000|+1.0000000000000X+000|+1.0000000000000X+000|+1.0000000000000X+000|+1.0000000000000X+000|+1.0000000000000X+000|+1.0000000000000X+000|+1.0000000000000X+000"},
    {"gen_ge_ge", 457,
     "capture noisily generate double g_ge_ge = u >= v >= w\nlocal __rc = _rc",
     "%21x g_ge_ge[1]\t%21x g_ge_ge[2]\t%21x g_ge_ge[3]\t%21x g_ge_ge[4]\t%21x g_ge_ge[5]\t%21x g_ge_ge[6]\t%21x g_ge_ge[7]\t%21x g_ge_ge[8]\t%21x g_ge_ge[9]\t%21x g_ge_ge[10]\t%21x g_ge_ge[11]\t%21x g_ge_ge[12]\t%21x g_ge_ge[13]\t%21x g_ge_ge[14]\t%21x g_ge_ge[15]\t%21x g_ge_ge[16]\t%21x g_ge_ge[17]\t%21x g_ge_ge[18]\t%21x g_ge_ge[19]\t%21x g_ge_ge[20]\t%21x g_ge_ge[21]\t%21x g_ge_ge[22]\t%21x g_ge_ge[23]\t%21x g_ge_ge[24]\t%21x g_ge_ge[25]\t%21x g_ge_ge[26]\t%21x g_ge_ge[27]",
     "0|+1.0000000000000X+000|+1.0000000000000X+000|+0.0000000000000X-3ff|+1.0000000000000X+000|+0.0000000000000X-3ff|+0.0000000000000X-3ff|+1.0000000000000X+000|+0.0000000000000X-3ff|+0.0000000000000X-3ff|+1.0000000000000X+000|+1.0000000000000X+000|+0.0000000000000X-3ff|+1.0000000000000X+000|+1.0000000000000X+000|+0.0000000000000X-3ff|+1.0000000000000X+000|+0.0000000000000X-3ff|+0.0000000000000X-3ff|+1.0000000000000X+000|+1.0000000000000X+000|+0.0000000000000X-3ff|+1.0000000000000X+000|+1.0000000000000X+000|+0.0000000000000X-3ff|+1.0000000000000X+000|+1.0000000000000X+000|+0.0000000000000X-3ff"},
    {"gen_ge_le", 470,
     "capture noisily generate double g_ge_le = u >= v <= w\nlocal __rc = _rc",
     "%21x g_ge_le[1]\t%21x g_ge_le[2]\t%21x g_ge_le[3]\t%21x g_ge_le[4]\t%21x g_ge_le[5]\t%21x g_ge_le[6]\t%21x g_ge_le[7]\t%21x g_ge_le[8]\t%21x g_ge_le[9]\t%21x g_ge_le[10]\t%21x g_ge_le[11]\t%21x g_ge_le[12]\t%21x g_ge_le[13]\t%21x g_ge_le[14]\t%21x g_ge_le[15]\t%21x g_ge_le[16]\t%21x g_ge_le[17]\t%21x g_ge_le[18]\t%21x g_ge_le[19]\t%21x g_ge_le[20]\t%21x g_ge_le[21]\t%21x g_ge_le[22]\t%21x g_ge_le[23]\t%21x g_ge_le[24]\t%21x g_ge_le[25]\t%21x g_ge_le[26]\t%21x g_ge_le[27]",
     "0|+0.0000000000000X-3ff|+0.0000000000000X-3ff|+0.0000000000000X-3ff|+1.0000000000000X+000|+0.0000000000000X-3ff|+0.0000000000000X-3ff|+1.0000000000000X+000|+1.0000000000000X+000|+0.0000000000000X-3ff|+1.0000000000000X+000|+1.0000000000000X+000|+1.0000000000000X+000|+1.0000000000000X+000|+1.0000000000000X+000|+1.0000000000000X+000|+1.0000000000000X+000|+1.0000000000000X+000|+1.0000000000000X+000|+1.0000000000000X+000|+1.0000000000000X+000|+1.0000000000000X+000|+1.0000000000000X+000|+1.0000000000000X+000|+1.0000000000000X+000|+1.0000000000000X+000|+1.0000000000000X+000|+1.0000000000000X+000"},
    {"gen_le_eq", 483,
     "capture noisily generate double g_le_eq = u <= v == w\nlocal __rc = _rc",
     "%21x g_le_eq[1]\t%21x g_le_eq[2]\t%21x g_le_eq[3]\t%21x g_le_eq[4]\t%21x g_le_eq[5]\t%21x g_le_eq[6]\t%21x g_le_eq[7]\t%21x g_le_eq[8]\t%21x g_le_eq[9]\t%21x g_le_eq[10]\t%21x g_le_eq[11]\t%21x g_le_eq[12]\t%21x g_le_eq[13]\t%21x g_le_eq[14]\t%21x g_le_eq[15]\t%21x g_le_eq[16]\t%21x g_le_eq[17]\t%21x g_le_eq[18]\t%21x g_le_eq[19]\t%21x g_le_eq[20]\t%21x g_le_eq[21]\t%21x g_le_eq[22]\t%21x g_le_eq[23]\t%21x g_le_eq[24]\t%21x g_le_eq[25]\t%21x g_le_eq[26]\t%21x g_le_eq[27]",
     "0|+0.0000000000000X-3ff|+1.0000000000000X+000|+0.0000000000000X-3ff|+0.0000000000000X-3ff|+1.0000000000000X+000|+0.0000000000000X-3ff|+0.0000000000000X-3ff|+1.0000000000000X+000|+0.0000000000000X-3ff|+1.0000000000000X+000|+0.0000000000000X-3ff|+0.0000000000000X-3ff|+0.0000000000000X-3ff|+1.0000000000000X+000|+0.0000000000000X-3ff|+0.0000000000000X-3ff|+1.0000000000000X+000|+0.0000000000000X-3ff|+1.0000000000000X+000|+0.0000000000000X-3ff|+0.0000000000000X-3ff|+1.0000000000000X+000|+0.0000000000000X-3ff|+0.0000000000000X-3ff|+0.0000000000000X-3ff|+1.0000000000000X+000|+0.0000000000000X-3ff"},
    {"gen_le_ne", 496,
     "capture noisily generate double g_le_ne = u <= v != w\nlocal __rc = _rc",
     "%21x g_le_ne[1]\t%21x g_le_ne[2]\t%21x g_le_ne[3]\t%21x g_le_ne[4]\t%21x g_le_ne[5]\t%21x g_le_ne[6]\t%21x g_le_ne[7]\t%21x g_le_ne[8]\t%21x g_le_ne[9]\t%21x g_le_ne[10]\t%21x g_le_ne[11]\t%21x g_le_ne[12]\t%21x g_le_ne[13]\t%21x g_le_ne[14]\t%21x g_le_ne[15]\t%21x g_le_ne[16]\t%21x g_le_ne[17]\t%21x g_le_ne[18]\t%21x g_le_ne[19]\t%21x g_le_ne[20]\t%21x g_le_ne[21]\t%21x g_le_ne[22]\t%21x g_le_ne[23]\t%21x g_le_ne[24]\t%21x g_le_ne[25]\t%21x g_le_ne[26]\t%21x g_le_ne[27]",
     "0|+1.0000000000000X+000|+1.0000000000000X+000|+1.0000000000000X+000|+1.0000000000000X+000|+1.0000000000000X+000|+1.0000000000000X+000|+1.0000000000000X+000|+1.0000000000000X+000|+1.0000000000000X+000|+0.0000000000000X-3ff|+1.0000000000000X+000|+1.0000000000000X+000|+1.0000000000000X+000|+0.0000000000000X-3ff|+1.0000000000000X+000|+1.0000000000000X+000|+1.0000000000000X+000|+0.0000000000000X-3ff|+0.0000000000000X-3ff|+0.0000000000000X-3ff|+0.0000000000000X-3ff|+0.0000000000000X-3ff|+0.0000000000000X-3ff|+0.0000000000000X-3ff|+0.0000000000000X-3ff|+0.0000000000000X-3ff|+0.0000000000000X-3ff"},
    {"gen_le_gt", 509,
     "capture noisily generate double g_le_gt = u <= v > w\nlocal __rc = _rc",
     "%21x g_le_gt[1]\t%21x g_le_gt[2]\t%21x g_le_gt[3]\t%21x g_le_gt[4]\t%21x g_le_gt[5]\t%21x g_le_gt[6]\t%21x g_le_gt[7]\t%21x g_le_gt[8]\t%21x g_le_gt[9]\t%21x g_le_gt[10]\t%21x g_le_gt[11]\t%21x g_le_gt[12]\t%21x g_le_gt[13]\t%21x g_le_gt[14]\t%21x g_le_gt[15]\t%21x g_le_gt[16]\t%21x g_le_gt[17]\t%21x g_le_gt[18]\t%21x g_le_gt[19]\t%21x g_le_gt[20]\t%21x g_le_gt[21]\t%21x g_le_gt[22]\t%21x g_le_gt[23]\t%21x g_le_gt[24]\t%21x g_le_gt[25]\t%21x g_le_gt[26]\t%21x g_le_gt[27]",
     "0|+1.0000000000000X+000|+1.0000000000000X+000|+1.0000000000000X+000|+1.0000000000000X+000|+1.0000000000000X+000|+1.0000000000000X+000|+1.0000000000000X+000|+1.0000000000000X+000|+1.0000000000000X+000|+0.0000000000000X-3ff|+0.0000000000000X-3ff|+0.0000000000000X-3ff|+1.0000000000000X+000|+0.0000000000000X-3ff|+0.0000000000000X-3ff|+1.0000000000000X+000|+1.0000000000000X+000|+0.0000000000000X-3ff|+0.0000000000000X-3ff|+0.0000000000000X-3ff|+0.0000000000000X-3ff|+0.0000000000000X-3ff|+0.0000000000000X-3ff|+0.0000000000000X-3ff|+0.0000000000000X-3ff|+0.0000000000000X-3ff|+0.0000000000000X-3ff"},
    {"gen_le_lt", 522,
     "capture noisily generate double g_le_lt = u <= v < w\nlocal __rc = _rc",
     "%21x g_le_lt[1]\t%21x g_le_lt[2]\t%21x g_le_lt[3]\t%21x g_le_lt[4]\t%21x g_le_lt[5]\t%21x g_le_lt[6]\t%21x g_le_lt[7]\t%21x g_le_lt[8]\t%21x g_le_lt[9]\t%21x g_le_lt[10]\t%21x g_le_lt[11]\t%21x g_le_lt[12]\t%21x g_le_lt[13]\t%21x g_le_lt[14]\t%21x g_le_lt[15]\t%21x g_le_lt[16]\t%21x g_le_lt[17]\t%21x g_le_lt[18]\t%21x g_le_lt[19]\t%21x g_le_lt[20]\t%21x g_le_lt[21]\t%21x g_le_lt[22]\t%21x g_le_lt[23]\t%21x g_le_lt[24]\t%21x g_le_lt[25]\t%21x g_le_lt[26]\t%21x g_le_lt[27]",
     "0|+1.0000000000000X+000|+1.0000000000000X+000|+1.0000000000000X+000|+1.0000000000000X+000|+1.0000000000000X+000|+1.0000000000000X+000|+1.0000000000000X+000|+1.0000000000000X+000|+1.0000000000000X+000|+0.0000000000000X-3ff|+1.0000000000000X+000|+1.0000000000000X+000|+0.0000000000000X-3ff|+0.0000000000000X-3ff|+1.0000000000000X+000|+0.0000000000000X-3ff|+0.0000000000000X-3ff|+0.0000000000000X-3ff|+0.0000000000000X-3ff|+0.0000000000000X-3ff|+0.0000000000000X-3ff|+0.0000000000000X-3ff|+0.0000000000000X-3ff|+0.0000000000000X-3ff|+0.0000000000000X-3ff|+0.0000000000000X-3ff|+0.0000000000000X-3ff"},
    {"gen_le_ge", 535,
     "capture noisily generate double g_le_ge = u <= v >= w\nlocal __rc = _rc",
     "%21x g_le_ge[1]\t%21x g_le_ge[2]\t%21x g_le_ge[3]\t%21x g_le_ge[4]\t%21x g_le_ge[5]\t%21x g_le_ge[6]\t%21x g_le_ge[7]\t%21x g_le_ge[8]\t%21x g_le_ge[9]\t%21x g_le_ge[10]\t%21x g_le_ge[11]\t%21x g_le_ge[12]\t%21x g_le_ge[13]\t%21x g_le_ge[14]\t%21x g_le_ge[15]\t%21x g_le_ge[16]\t%21x g_le_ge[17]\t%21x g_le_ge[18]\t%21x g_le_ge[19]\t%21x g_le_ge[20]\t%21x g_le_ge[21]\t%21x g_le_ge[22]\t%21x g_le_ge[23]\t%21x g_le_ge[24]\t%21x g_le_ge[25]\t%21x g_le_ge[26]\t%21x g_le_ge[27]",
     "0|+1.0000000000000X+000|+1.0000000000000X+000|+0.0000000000000X-3ff|+1.0000000000000X+000|+1.0000000000000X+000|+0.0000000000000X-3ff|+1.0000000000000X+000|+1.0000000000000X+000|+0.0000000000000X-3ff|+1.0000000000000X+000|+0.0000000000000X-3ff|+0.0000000000000X-3ff|+1.0000000000000X+000|+1.0000000000000X+000|+0.0000000000000X-3ff|+1.0000000000000X+000|+1.0000000000000X+000|+0.0000000000000X-3ff|+1.0000000000000X+000|+0.0000000000000X-3ff|+0.0000000000000X-3ff|+1.0000000000000X+000|+0.0000000000000X-3ff|+0.0000000000000X-3ff|+1.0000000000000X+000|+1.0000000000000X+000|+0.0000000000000X-3ff"},
    {"gen_le_le", 548,
     "capture noisily generate double g_le_le = u <= v <= w\nlocal __rc = _rc",
     "%21x g_le_le[1]\t%21x g_le_le[2]\t%21x g_le_le[3]\t%21x g_le_le[4]\t%21x g_le_le[5]\t%21x g_le_le[6]\t%21x g_le_le[7]\t%21x g_le_le[8]\t%21x g_le_le[9]\t%21x g_le_le[10]\t%21x g_le_le[11]\t%21x g_le_le[12]\t%21x g_le_le[13]\t%21x g_le_le[14]\t%21x g_le_le[15]\t%21x g_le_le[16]\t%21x g_le_le[17]\t%21x g_le_le[18]\t%21x g_le_le[19]\t%21x g_le_le[20]\t%21x g_le_le[21]\t%21x g_le_le[22]\t%21x g_le_le[23]\t%21x g_le_le[24]\t%21x g_le_le[25]\t%21x g_le_le[26]\t%21x g_le_le[27]",
     "0|+0.0000000000000X-3ff|+1.0000000000000X+000|+1.0000000000000X+000|+0.0000000000000X-3ff|+1.0000000000000X+000|+1.0000000000000X+000|+0.0000000000000X-3ff|+1.0000000000000X+000|+1.0000000000000X+000|+1.0000000000000X+000|+1.0000000000000X+000|+1.0000000000000X+000|+0.0000000000000X-3ff|+1.0000000000000X+000|+1.0000000000000X+000|+0.0000000000000X-3ff|+1.0000000000000X+000|+1.0000000000000X+000|+1.0000000000000X+000|+1.0000000000000X+000|+1.0000000000000X+000|+1.0000000000000X+000|+1.0000000000000X+000|+1.0000000000000X+000|+0.0000000000000X-3ff|+1.0000000000000X+000|+1.0000000000000X+000"},
    {"count_eq_eq", 562,
     "capture noisily count if u == v == w\nlocal __rc = _rc",
     "%21x r(N)",
     "0|+1.2000000000000X+003"},
    {"count_eq_ne", 572,
     "capture noisily count if u == v != w\nlocal __rc = _rc",
     "%21x r(N)",
     "0|+1.2000000000000X+003"},
    {"count_eq_gt", 582,
     "capture noisily count if u == v > w\nlocal __rc = _rc",
     "%21x r(N)",
     "0|+1.2000000000000X+003"},
    {"count_eq_lt", 592,
     "capture noisily count if u == v < w\nlocal __rc = _rc",
     "%21x r(N)",
     "0|+1.2000000000000X+003"},
    {"count_eq_ge", 602,
     "capture noisily count if u == v >= w\nlocal __rc = _rc",
     "%21x r(N)",
     "0|+1.2000000000000X+003"},
    {"count_eq_le", 612,
     "capture noisily count if u == v <= w\nlocal __rc = _rc",
     "%21x r(N)",
     "0|+1.2000000000000X+003"},
    {"count_ne_eq", 622,
     "capture noisily count if u != v == w\nlocal __rc = _rc",
     "%21x r(N)",
     "0|+1.2000000000000X+003"},
    {"count_ne_ne", 632,
     "capture noisily count if u != v != w\nlocal __rc = _rc",
     "%21x r(N)",
     "0|+1.2000000000000X+004"},
    {"count_ne_gt", 642,
     "capture noisily count if u != v > w\nlocal __rc = _rc",
     "%21x r(N)",
     "0|+1.8000000000000X+002"},
    {"count_ne_lt", 652,
     "capture noisily count if u != v < w\nlocal __rc = _rc",
     "%21x r(N)",
     "0|+1.8000000000000X+003"},
    {"count_ne_ge", 662,
     "capture noisily count if u != v >= w\nlocal __rc = _rc",
     "%21x r(N)",
     "0|+1.e000000000000X+003"},
    {"count_ne_le", 672,
     "capture noisily count if u != v <= w\nlocal __rc = _rc",
     "%21x r(N)",
     "0|+1.5000000000000X+004"},
    {"count_gt_eq", 682,
     "capture noisily count if u > v == w\nlocal __rc = _rc",
     "%21x r(N)",
     "0|+1.2000000000000X+003"},
    {"count_gt_ne", 692,
     "capture noisily count if u > v != w\nlocal __rc = _rc",
     "%21x r(N)",
     "0|+1.8000000000000X+003"},
    {"count_gt_gt", 702,
     "capture noisily count if u > v > w\nlocal __rc = _rc",
     "%21x r(N)",
     "0|+1.8000000000000X+001"},
    {"count_gt_lt", 712,
     "capture noisily count if u > v < w\nlocal __rc = _rc",
     "%21x r(N)",
     "0|+1.e000000000000X+003"},
    {"count_gt_ge", 722,
     "capture noisily count if u > v >= w\nlocal __rc = _rc",
     "%21x r(N)",
     "0|+1.8000000000000X+003"},
    {"count_gt_le", 732,
     "capture noisily count if u > v <= w\nlocal __rc = _rc",
     "%21x r(N)",
     "0|+1.8000000000000X+004"},
    {"count_lt_eq", 742,
     "capture noisily count if u < v == w\nlocal __rc = _rc",
     "%21x r(N)",
     "0|+1.2000000000000X+003"},
    {"count_lt_ne", 752,
     "capture noisily count if u < v != w\nlocal __rc = _rc",
     "%21x r(N)",
     "0|+1.8000000000000X+002"},
    {"count_lt_gt", 762,
     "capture noisily count if u < v > w\nlocal __rc = _rc",
     "%21x r(N)",
     "0|+1.8000000000000X+001"},
    {"count_lt_lt", 772,
     "capture noisily count if u < v < w\nlocal __rc = _rc",
     "%21x r(N)",
     "0|+1.e000000000000X+003"},
    {"count_lt_ge", 782,
     "capture noisily count if u < v >= w\nlocal __rc = _rc",
     "%21x r(N)",
     "0|+1.8000000000000X+003"},
    {"count_lt_le", 792,
     "capture noisily count if u < v <= w\nlocal __rc = _rc",
     "%21x r(N)",
     "0|+1.8000000000000X+004"},
    {"count_ge_eq", 802,
     "capture noisily count if u >= v == w\nlocal __rc = _rc",
     "%21x r(N)",
     "0|+1.2000000000000X+003"},
    {"count_ge_ne", 812,
     "capture noisily count if u >= v != w\nlocal __rc = _rc",
     "%21x r(N)",
     "0|+1.5000000000000X+004"},
    {"count_ge_gt", 822,
     "capture noisily count if u >= v > w\nlocal __rc = _rc",
     "%21x r(N)",
     "0|+1.8000000000000X+004"},
    {"count_ge_lt", 832,
     "capture noisily count if u >= v < w\nlocal __rc = _rc",
     "%21x r(N)",
     "0|+1.8000000000000X+004"},
    {"count_ge_ge", 842,
     "capture noisily count if u >= v >= w\nlocal __rc = _rc",
     "%21x r(N)",
     "0|+1.e000000000000X+003"},
    {"count_ge_le", 852,
     "capture noisily count if u >= v <= w\nlocal __rc = _rc",
     "%21x r(N)",
     "0|+1.5000000000000X+004"},
    {"count_le_eq", 862,
     "capture noisily count if u <= v == w\nlocal __rc = _rc",
     "%21x r(N)",
     "0|+1.2000000000000X+003"},
    {"count_le_ne", 872,
     "capture noisily count if u <= v != w\nlocal __rc = _rc",
     "%21x r(N)",
     "0|+1.e000000000000X+003"},
    {"count_le_gt", 882,
     "capture noisily count if u <= v > w\nlocal __rc = _rc",
     "%21x r(N)",
     "0|+1.8000000000000X+003"},
    {"count_le_lt", 892,
     "capture noisily count if u <= v < w\nlocal __rc = _rc",
     "%21x r(N)",
     "0|+1.8000000000000X+003"},
    {"count_le_ge", 902,
     "capture noisily count if u <= v >= w\nlocal __rc = _rc",
     "%21x r(N)",
     "0|+1.e000000000000X+003"},
    {"count_le_le", 912,
     "capture noisily count if u <= v <= w\nlocal __rc = _rc",
     "%21x r(N)",
     "0|+1.5000000000000X+004"},
    {"gen_not_mul", 925,
     "capture noisily generate double n_not_mul = !t*2\nlocal __rc = _rc",
     "%21x n_not_mul[1]\t%21x n_not_mul[2]\t%21x n_not_mul[3]\t%21x n_not_mul[4]\t%21x n_not_mul[5]\t%21x n_not_mul[6]\t%21x n_not_mul[7]\t%21x n_not_mul[8]\t%21x n_not_mul[9]\t%21x n_not_mul[10]\t%21x n_not_mul[11]\t%21x n_not_mul[12]\t%21x n_not_mul[13]\t%21x n_not_mul[14]\t%21x n_not_mul[15]\t%21x n_not_mul[16]\t%21x n_not_mul[17]\t%21x n_not_mul[18]\t%21x n_not_mul[19]\t%21x n_not_mul[20]\t%21x n_not_mul[21]\t%21x n_not_mul[22]\t%21x n_not_mul[23]\t%21x n_not_mul[24]\t%21x n_not_mul[25]\t%21x n_not_mul[26]\t%21x n_not_mul[27]",
     "0|+0.0000000000000X-3ff|+0.0000000000000X-3ff|+0.0000000000000X-3ff|+0.0000000000000X-3ff|+0.0000000000000X-3ff|+0.0000000000000X-3ff|+0.0000000000000X-3ff|+0.0000000000000X-3ff|+0.0000000000000X-3ff|+1.0000000000000X+001|+1.0000000000000X+001|+1.0000000000000X+001|+1.0000000000000X+001|+1.0000000000000X+001|+1.0000000000000X+001|+1.0000000000000X+001|+1.0000000000000X+001|+1.0000000000000X+001|+0.0000000000000X-3ff|+0.0000000000000X-3ff|+0.0000000000000X-3ff|+0.0000000000000X-3ff|+0.0000000000000X-3ff|+0.0000000000000X-3ff|+0.0000000000000X-3ff|+0.0000000000000X-3ff|+0.0000000000000X-3ff"},
    {"gen_not_div", 938,
     "capture noisily generate double n_not_div = !t/2\nlocal __rc = _rc",
     "%21x n_not_div[1]\t%21x n_not_div[2]\t%21x n_not_div[3]\t%21x n_not_div[4]\t%21x n_not_div[5]\t%21x n_not_div[6]\t%21x n_not_div[7]\t%21x n_not_div[8]\t%21x n_not_div[9]\t%21x n_not_div[10]\t%21x n_not_div[11]\t%21x n_not_div[12]\t%21x n_not_div[13]\t%21x n_not_div[14]\t%21x n_not_div[15]\t%21x n_not_div[16]\t%21x n_not_div[17]\t%21x n_not_div[18]\t%21x n_not_div[19]\t%21x n_not_div[20]\t%21x n_not_div[21]\t%21x n_not_div[22]\t%21x n_not_div[23]\t%21x n_not_div[24]\t%21x n_not_div[25]\t%21x n_not_div[26]\t%21x n_not_div[27]",
     "0|+0.0000000000000X-3ff|+0.0000000000000X-3ff|+0.0000000000000X-3ff|+0.0000000000000X-3ff|+0.0000000000000X-3ff|+0.0000000000000X-3ff|+0.0000000000000X-3ff|+0.0000000000000X-3ff|+0.0000000000000X-3ff|+1.0000000000000X-001|+1.0000000000000X-001|+1.0000000000000X-001|+1.0000000000000X-001|+1.0000000000000X-001|+1.0000000000000X-001|+1.0000000000000X-001|+1.0000000000000X-001|+1.0000000000000X-001|+0.0000000000000X-3ff|+0.0000000000000X-3ff|+0.0000000000000X-3ff|+0.0000000000000X-3ff|+0.0000000000000X-3ff|+0.0000000000000X-3ff|+0.0000000000000X-3ff|+0.0000000000000X-3ff|+0.0000000000000X-3ff"},
    {"gen_not_add", 951,
     "capture noisily generate double n_not_add = !t+1\nlocal __rc = _rc",
     "%21x n_not_add[1]\t%21x n_not_add[2]\t%21x n_not_add[3]\t%21x n_not_add[4]\t%21x n_not_add[5]\t%21x n_not_add[6]\t%21x n_not_add[7]\t%21x n_not_add[8]\t%21x n_not_add[9]\t%21x n_not_add[10]\t%21x n_not_add[11]\t%21x n_not_add[12]\t%21x n_not_add[13]\t%21x n_not_add[14]\t%21x n_not_add[15]\t%21x n_not_add[16]\t%21x n_not_add[17]\t%21x n_not_add[18]\t%21x n_not_add[19]\t%21x n_not_add[20]\t%21x n_not_add[21]\t%21x n_not_add[22]\t%21x n_not_add[23]\t%21x n_not_add[24]\t%21x n_not_add[25]\t%21x n_not_add[26]\t%21x n_not_add[27]",
     "0|+1.0000000000000X+000|+1.0000000000000X+000|+1.0000000000000X+000|+1.0000000000000X+000|+1.0000000000000X+000|+1.0000000000000X+000|+1.0000000000000X+000|+1.0000000000000X+000|+1.0000000000000X+000|+1.0000000000000X+001|+1.0000000000000X+001|+1.0000000000000X+001|+1.0000000000000X+001|+1.0000000000000X+001|+1.0000000000000X+001|+1.0000000000000X+001|+1.0000000000000X+001|+1.0000000000000X+001|+1.0000000000000X+000|+1.0000000000000X+000|+1.0000000000000X+000|+1.0000000000000X+000|+1.0000000000000X+000|+1.0000000000000X+000|+1.0000000000000X+000|+1.0000000000000X+000|+1.0000000000000X+000"},
    {"gen_not_sub", 964,
     "capture noisily generate double n_not_sub = !t-1\nlocal __rc = _rc",
     "%21x n_not_sub[1]\t%21x n_not_sub[2]\t%21x n_not_sub[3]\t%21x n_not_sub[4]\t%21x n_not_sub[5]\t%21x n_not_sub[6]\t%21x n_not_sub[7]\t%21x n_not_sub[8]\t%21x n_not_sub[9]\t%21x n_not_sub[10]\t%21x n_not_sub[11]\t%21x n_not_sub[12]\t%21x n_not_sub[13]\t%21x n_not_sub[14]\t%21x n_not_sub[15]\t%21x n_not_sub[16]\t%21x n_not_sub[17]\t%21x n_not_sub[18]\t%21x n_not_sub[19]\t%21x n_not_sub[20]\t%21x n_not_sub[21]\t%21x n_not_sub[22]\t%21x n_not_sub[23]\t%21x n_not_sub[24]\t%21x n_not_sub[25]\t%21x n_not_sub[26]\t%21x n_not_sub[27]",
     "0|-1.0000000000000X+000|-1.0000000000000X+000|-1.0000000000000X+000|-1.0000000000000X+000|-1.0000000000000X+000|-1.0000000000000X+000|-1.0000000000000X+000|-1.0000000000000X+000|-1.0000000000000X+000|+0.0000000000000X-3ff|+0.0000000000000X-3ff|+0.0000000000000X-3ff|+0.0000000000000X-3ff|+0.0000000000000X-3ff|+0.0000000000000X-3ff|+0.0000000000000X-3ff|+0.0000000000000X-3ff|+0.0000000000000X-3ff|-1.0000000000000X+000|-1.0000000000000X+000|-1.0000000000000X+000|-1.0000000000000X+000|-1.0000000000000X+000|-1.0000000000000X+000|-1.0000000000000X+000|-1.0000000000000X+000|-1.0000000000000X+000"},
    {"gen_not_gt", 977,
     "capture noisily generate double n_not_gt = !t>0\nlocal __rc = _rc",
     "%21x n_not_gt[1]\t%21x n_not_gt[2]\t%21x n_not_gt[3]\t%21x n_not_gt[4]\t%21x n_not_gt[5]\t%21x n_not_gt[6]\t%21x n_not_gt[7]\t%21x n_not_gt[8]\t%21x n_not_gt[9]\t%21x n_not_gt[10]\t%21x n_not_gt[11]\t%21x n_not_gt[12]\t%21x n_not_gt[13]\t%21x n_not_gt[14]\t%21x n_not_gt[15]\t%21x n_not_gt[16]\t%21x n_not_gt[17]\t%21x n_not_gt[18]\t%21x n_not_gt[19]\t%21x n_not_gt[20]\t%21x n_not_gt[21]\t%21x n_not_gt[22]\t%21x n_not_gt[23]\t%21x n_not_gt[24]\t%21x n_not_gt[25]\t%21x n_not_gt[26]\t%21x n_not_gt[27]",
     "0|+0.0000000000000X-3ff|+0.0000000000000X-3ff|+0.0000000000000X-3ff|+0.0000000000000X-3ff|+0.0000000000000X-3ff|+0.0000000000000X-3ff|+0.0000000000000X-3ff|+0.0000000000000X-3ff|+0.0000000000000X-3ff|+1.0000000000000X+000|+1.0000000000000X+000|+1.0000000000000X+000|+1.0000000000000X+000|+1.0000000000000X+000|+1.0000000000000X+000|+1.0000000000000X+000|+1.0000000000000X+000|+1.0000000000000X+000|+0.0000000000000X-3ff|+0.0000000000000X-3ff|+0.0000000000000X-3ff|+0.0000000000000X-3ff|+0.0000000000000X-3ff|+0.0000000000000X-3ff|+0.0000000000000X-3ff|+0.0000000000000X-3ff|+0.0000000000000X-3ff"},
    {"gen_not_and", 990,
     "capture noisily generate double n_not_and = !t&v\nlocal __rc = _rc",
     "%21x n_not_and[1]\t%21x n_not_and[2]\t%21x n_not_and[3]\t%21x n_not_and[4]\t%21x n_not_and[5]\t%21x n_not_and[6]\t%21x n_not_and[7]\t%21x n_not_and[8]\t%21x n_not_and[9]\t%21x n_not_and[10]\t%21x n_not_and[11]\t%21x n_not_and[12]\t%21x n_not_and[13]\t%21x n_not_and[14]\t%21x n_not_and[15]\t%21x n_not_and[16]\t%21x n_not_and[17]\t%21x n_not_and[18]\t%21x n_not_and[19]\t%21x n_not_and[20]\t%21x n_not_and[21]\t%21x n_not_and[22]\t%21x n_not_and[23]\t%21x n_not_and[24]\t%21x n_not_and[25]\t%21x n_not_and[26]\t%21x n_not_and[27]",
     "0|+0.0000000000000X-3ff|+0.0000000000000X-3ff|+0.0000000000000X-3ff|+0.0000000000000X-3ff|+0.0000000000000X-3ff|+0.0000000000000X-3ff|+0.0000000000000X-3ff|+0.0000000000000X-3ff|+0.0000000000000X-3ff|+0.0000000000000X-3ff|+0.0000000000000X-3ff|+0.0000000000000X-3ff|+1.0000000000000X+000|+1.0000000000000X+000|+1.0000000000000X+000|+1.0000000000000X+000|+1.0000000000000X+000|+1.0000000000000X+000|+0.0000000000000X-3ff|+0.0000000000000X-3ff|+0.0000000000000X-3ff|+0.0000000000000X-3ff|+0.0000000000000X-3ff|+0.0000000000000X-3ff|+0.0000000000000X-3ff|+0.0000000000000X-3ff|+0.0000000000000X-3ff"},
    {"gen_not_or", 1003,
     "capture noisily generate double n_not_or = !t|v\nlocal __rc = _rc",
     "%21x n_not_or[1]\t%21x n_not_or[2]\t%21x n_not_or[3]\t%21x n_not_or[4]\t%21x n_not_or[5]\t%21x n_not_or[6]\t%21x n_not_or[7]\t%21x n_not_or[8]\t%21x n_not_or[9]\t%21x n_not_or[10]\t%21x n_not_or[11]\t%21x n_not_or[12]\t%21x n_not_or[13]\t%21x n_not_or[14]\t%21x n_not_or[15]\t%21x n_not_or[16]\t%21x n_not_or[17]\t%21x n_not_or[18]\t%21x n_not_or[19]\t%21x n_not_or[20]\t%21x n_not_or[21]\t%21x n_not_or[22]\t%21x n_not_or[23]\t%21x n_not_or[24]\t%21x n_not_or[25]\t%21x n_not_or[26]\t%21x n_not_or[27]",
     "0|+0.0000000000000X-3ff|+0.0000000000000X-3ff|+0.0000000000000X-3ff|+1.0000000000000X+000|+1.0000000000000X+000|+1.0000000000000X+000|+1.0000000000000X+000|+1.0000000000000X+000|+1.0000000000000X+000|+1.0000000000000X+000|+1.0000000000000X+000|+1.0000000000000X+000|+1.0000000000000X+000|+1.0000000000000X+000|+1.0000000000000X+000|+1.0000000000000X+000|+1.0000000000000X+000|+1.0000000000000X+000|+0.0000000000000X-3ff|+0.0000000000000X-3ff|+0.0000000000000X-3ff|+1.0000000000000X+000|+1.0000000000000X+000|+1.0000000000000X+000|+1.0000000000000X+000|+1.0000000000000X+000|+1.0000000000000X+000"},
    {"gen_tilde_pow0", 1016,
     "capture noisily generate double n_tilde_pow0 = ~t^0\nlocal __rc = _rc",
     "%21x n_tilde_pow0[1]\t%21x n_tilde_pow0[2]\t%21x n_tilde_pow0[3]\t%21x n_tilde_pow0[4]\t%21x n_tilde_pow0[5]\t%21x n_tilde_pow0[6]\t%21x n_tilde_pow0[7]\t%21x n_tilde_pow0[8]\t%21x n_tilde_pow0[9]\t%21x n_tilde_pow0[10]\t%21x n_tilde_pow0[11]\t%21x n_tilde_pow0[12]\t%21x n_tilde_pow0[13]\t%21x n_tilde_pow0[14]\t%21x n_tilde_pow0[15]\t%21x n_tilde_pow0[16]\t%21x n_tilde_pow0[17]\t%21x n_tilde_pow0[18]\t%21x n_tilde_pow0[19]\t%21x n_tilde_pow0[20]\t%21x n_tilde_pow0[21]\t%21x n_tilde_pow0[22]\t%21x n_tilde_pow0[23]\t%21x n_tilde_pow0[24]\t%21x n_tilde_pow0[25]\t%21x n_tilde_pow0[26]\t%21x n_tilde_pow0[27]",
     "0|+0.0000000000000X-3ff|+0.0000000000000X-3ff|+0.0000000000000X-3ff|+0.0000000000000X-3ff|+0.0000000000000X-3ff|+0.0000000000000X-3ff|+0.0000000000000X-3ff|+0.0000000000000X-3ff|+0.0000000000000X-3ff|+0.0000000000000X-3ff|+0.0000000000000X-3ff|+0.0000000000000X-3ff|+0.0000000000000X-3ff|+0.0000000000000X-3ff|+0.0000000000000X-3ff|+0.0000000000000X-3ff|+0.0000000000000X-3ff|+0.0000000000000X-3ff|+0.0000000000000X-3ff|+0.0000000000000X-3ff|+0.0000000000000X-3ff|+0.0000000000000X-3ff|+0.0000000000000X-3ff|+0.0000000000000X-3ff|+0.0000000000000X-3ff|+0.0000000000000X-3ff|+0.0000000000000X-3ff"},
    {"gen_not_not", 1029,
     "capture noisily generate double n_not_not = !!t\nlocal __rc = _rc",
     "%21x n_not_not[1]\t%21x n_not_not[2]\t%21x n_not_not[3]\t%21x n_not_not[4]\t%21x n_not_not[5]\t%21x n_not_not[6]\t%21x n_not_not[7]\t%21x n_not_not[8]\t%21x n_not_not[9]\t%21x n_not_not[10]\t%21x n_not_not[11]\t%21x n_not_not[12]\t%21x n_not_not[13]\t%21x n_not_not[14]\t%21x n_not_not[15]\t%21x n_not_not[16]\t%21x n_not_not[17]\t%21x n_not_not[18]\t%21x n_not_not[19]\t%21x n_not_not[20]\t%21x n_not_not[21]\t%21x n_not_not[22]\t%21x n_not_not[23]\t%21x n_not_not[24]\t%21x n_not_not[25]\t%21x n_not_not[26]\t%21x n_not_not[27]",
     "0|+1.0000000000000X+000|+1.0000000000000X+000|+1.0000000000000X+000|+1.0000000000000X+000|+1.0000000000000X+000|+1.0000000000000X+000|+1.0000000000000X+000|+1.0000000000000X+000|+1.0000000000000X+000|+0.0000000000000X-3ff|+0.0000000000000X-3ff|+0.0000000000000X-3ff|+0.0000000000000X-3ff|+0.0000000000000X-3ff|+0.0000000000000X-3ff|+0.0000000000000X-3ff|+0.0000000000000X-3ff|+0.0000000000000X-3ff|+1.0000000000000X+000|+1.0000000000000X+000|+1.0000000000000X+000|+1.0000000000000X+000|+1.0000000000000X+000|+1.0000000000000X+000|+1.0000000000000X+000|+1.0000000000000X+000|+1.0000000000000X+000"},
    {"gen_neg_not", 1042,
     "capture noisily generate double n_neg_not = -!t\nlocal __rc = _rc",
     "%21x n_neg_not[1]\t%21x n_neg_not[2]\t%21x n_neg_not[3]\t%21x n_neg_not[4]\t%21x n_neg_not[5]\t%21x n_neg_not[6]\t%21x n_neg_not[7]\t%21x n_neg_not[8]\t%21x n_neg_not[9]\t%21x n_neg_not[10]\t%21x n_neg_not[11]\t%21x n_neg_not[12]\t%21x n_neg_not[13]\t%21x n_neg_not[14]\t%21x n_neg_not[15]\t%21x n_neg_not[16]\t%21x n_neg_not[17]\t%21x n_neg_not[18]\t%21x n_neg_not[19]\t%21x n_neg_not[20]\t%21x n_neg_not[21]\t%21x n_neg_not[22]\t%21x n_neg_not[23]\t%21x n_neg_not[24]\t%21x n_neg_not[25]\t%21x n_neg_not[26]\t%21x n_neg_not[27]",
     "0|+0.0000000000000X-3ff|+0.0000000000000X-3ff|+0.0000000000000X-3ff|+0.0000000000000X-3ff|+0.0000000000000X-3ff|+0.0000000000000X-3ff|+0.0000000000000X-3ff|+0.0000000000000X-3ff|+0.0000000000000X-3ff|-1.0000000000000X+000|-1.0000000000000X+000|-1.0000000000000X+000|-1.0000000000000X+000|-1.0000000000000X+000|-1.0000000000000X+000|-1.0000000000000X+000|-1.0000000000000X+000|-1.0000000000000X+000|+0.0000000000000X-3ff|+0.0000000000000X-3ff|+0.0000000000000X-3ff|+0.0000000000000X-3ff|+0.0000000000000X-3ff|+0.0000000000000X-3ff|+0.0000000000000X-3ff|+0.0000000000000X-3ff|+0.0000000000000X-3ff"},
    {"gen_not_neg", 1056,
     "capture noisily generate double n_not_neg = !-t\nlocal __rc = _rc",
     "%21x n_not_neg[1]\t%21x n_not_neg[2]\t%21x n_not_neg[3]\t%21x n_not_neg[4]\t%21x n_not_neg[5]\t%21x n_not_neg[6]\t%21x n_not_neg[7]\t%21x n_not_neg[8]\t%21x n_not_neg[9]\t%21x n_not_neg[10]\t%21x n_not_neg[11]\t%21x n_not_neg[12]\t%21x n_not_neg[13]\t%21x n_not_neg[14]\t%21x n_not_neg[15]\t%21x n_not_neg[16]\t%21x n_not_neg[17]\t%21x n_not_neg[18]\t%21x n_not_neg[19]\t%21x n_not_neg[20]\t%21x n_not_neg[21]\t%21x n_not_neg[22]\t%21x n_not_neg[23]\t%21x n_not_neg[24]\t%21x n_not_neg[25]\t%21x n_not_neg[26]\t%21x n_not_neg[27]",
     "198"},
    {"gen_pow_not", 1068,
     "capture noisily generate double n_pow_not = 2^!t\nlocal __rc = _rc",
     "%21x n_pow_not[1]\t%21x n_pow_not[2]\t%21x n_pow_not[3]\t%21x n_pow_not[4]\t%21x n_pow_not[5]\t%21x n_pow_not[6]\t%21x n_pow_not[7]\t%21x n_pow_not[8]\t%21x n_pow_not[9]\t%21x n_pow_not[10]\t%21x n_pow_not[11]\t%21x n_pow_not[12]\t%21x n_pow_not[13]\t%21x n_pow_not[14]\t%21x n_pow_not[15]\t%21x n_pow_not[16]\t%21x n_pow_not[17]\t%21x n_pow_not[18]\t%21x n_pow_not[19]\t%21x n_pow_not[20]\t%21x n_pow_not[21]\t%21x n_pow_not[22]\t%21x n_pow_not[23]\t%21x n_pow_not[24]\t%21x n_pow_not[25]\t%21x n_pow_not[26]\t%21x n_pow_not[27]",
     "0|+1.0000000000000X+000|+1.0000000000000X+000|+1.0000000000000X+000|+1.0000000000000X+000|+1.0000000000000X+000|+1.0000000000000X+000|+1.0000000000000X+000|+1.0000000000000X+000|+1.0000000000000X+000|+1.0000000000000X+001|+1.0000000000000X+001|+1.0000000000000X+001|+1.0000000000000X+001|+1.0000000000000X+001|+1.0000000000000X+001|+1.0000000000000X+001|+1.0000000000000X+001|+1.0000000000000X+001|+1.0000000000000X+000|+1.0000000000000X+000|+1.0000000000000X+000|+1.0000000000000X+000|+1.0000000000000X+000|+1.0000000000000X+000|+1.0000000000000X+000|+1.0000000000000X+000|+1.0000000000000X+000"},
    {"gen_pow_negneg", 1081,
     "capture noisily generate double n_pow_negneg = 2^- -t\nlocal __rc = _rc",
     "%21x n_pow_negneg[1]\t%21x n_pow_negneg[2]\t%21x n_pow_negneg[3]\t%21x n_pow_negneg[4]\t%21x n_pow_negneg[5]\t%21x n_pow_negneg[6]\t%21x n_pow_negneg[7]\t%21x n_pow_negneg[8]\t%21x n_pow_negneg[9]\t%21x n_pow_negneg[10]\t%21x n_pow_negneg[11]\t%21x n_pow_negneg[12]\t%21x n_pow_negneg[13]\t%21x n_pow_negneg[14]\t%21x n_pow_negneg[15]\t%21x n_pow_negneg[16]\t%21x n_pow_negneg[17]\t%21x n_pow_negneg[18]\t%21x n_pow_negneg[19]\t%21x n_pow_negneg[20]\t%21x n_pow_negneg[21]\t%21x n_pow_negneg[22]\t%21x n_pow_negneg[23]\t%21x n_pow_negneg[24]\t%21x n_pow_negneg[25]\t%21x n_pow_negneg[26]\t%21x n_pow_negneg[27]",
     "0|+1.0000000000000X-001|+1.0000000000000X-001|+1.0000000000000X-001|+1.0000000000000X-001|+1.0000000000000X-001|+1.0000000000000X-001|+1.0000000000000X-001|+1.0000000000000X-001|+1.0000000000000X-001|+1.0000000000000X+000|+1.0000000000000X+000|+1.0000000000000X+000|+1.0000000000000X+000|+1.0000000000000X+000|+1.0000000000000X+000|+1.0000000000000X+000|+1.0000000000000X+000|+1.0000000000000X+000|+1.0000000000000X+001|+1.0000000000000X+001|+1.0000000000000X+001|+1.0000000000000X+001|+1.0000000000000X+001|+1.0000000000000X+001|+1.0000000000000X+001|+1.0000000000000X+001|+1.0000000000000X+001"},
    {"gen_and_or", 1094,
     "capture noisily generate double n_and_or = u|v&w\nlocal __rc = _rc",
     "%21x n_and_or[1]\t%21x n_and_or[2]\t%21x n_and_or[3]\t%21x n_and_or[4]\t%21x n_and_or[5]\t%21x n_and_or[6]\t%21x n_and_or[7]\t%21x n_and_or[8]\t%21x n_and_or[9]\t%21x n_and_or[10]\t%21x n_and_or[11]\t%21x n_and_or[12]\t%21x n_and_or[13]\t%21x n_and_or[14]\t%21x n_and_or[15]\t%21x n_and_or[16]\t%21x n_and_or[17]\t%21x n_and_or[18]\t%21x n_and_or[19]\t%21x n_and_or[20]\t%21x n_and_or[21]\t%21x n_and_or[22]\t%21x n_and_or[23]\t%21x n_and_or[24]\t%21x n_and_or[25]\t%21x n_and_or[26]\t%21x n_and_or[27]",
     "0|+0.0000000000000X-3ff|+0.0000000000000X-3ff|+0.0000000000000X-3ff|+0.0000000000000X-3ff|+1.0000000000000X+000|+1.0000000000000X+000|+0.0000000000000X-3ff|+1.0000000000000X+000|+1.0000000000000X+000|+1.0000000000000X+000|+1.0000000000000X+000|+1.0000000000000X+000|+1.0000000000000X+000|+1.0000000000000X+000|+1.0000000000000X+000|+1.0000000000000X+000|+1.0000000000000X+000|+1.0000000000000X+000|+1.0000000000000X+000|+1.0000000000000X+000|+1.0000000000000X+000|+1.0000000000000X+000|+1.0000000000000X+000|+1.0000000000000X+000|+1.0000000000000X+000|+1.0000000000000X+000|+1.0000000000000X+000"},
    {"gen_tilde_ne_gt", 1108,
     "capture noisily generate double n_tilde_ne_gt = u~=v>w\nlocal __rc = _rc",
     "%21x n_tilde_ne_gt[1]\t%21x n_tilde_ne_gt[2]\t%21x n_tilde_ne_gt[3]\t%21x n_tilde_ne_gt[4]\t%21x n_tilde_ne_gt[5]\t%21x n_tilde_ne_gt[6]\t%21x n_tilde_ne_gt[7]\t%21x n_tilde_ne_gt[8]\t%21x n_tilde_ne_gt[9]\t%21x n_tilde_ne_gt[10]\t%21x n_tilde_ne_gt[11]\t%21x n_tilde_ne_gt[12]\t%21x n_tilde_ne_gt[13]\t%21x n_tilde_ne_gt[14]\t%21x n_tilde_ne_gt[15]\t%21x n_tilde_ne_gt[16]\t%21x n_tilde_ne_gt[17]\t%21x n_tilde_ne_gt[18]\t%21x n_tilde_ne_gt[19]\t%21x n_tilde_ne_gt[20]\t%21x n_tilde_ne_gt[21]\t%21x n_tilde_ne_gt[22]\t%21x n_tilde_ne_gt[23]\t%21x n_tilde_ne_gt[24]\t%21x n_tilde_ne_gt[25]\t%21x n_tilde_ne_gt[26]\t%21x n_tilde_ne_gt[27]",
     "0|+0.0000000000000X-3ff|+0.0000000000000X-3ff|+0.0000000000000X-3ff|+1.0000000000000X+000|+0.0000000000000X-3ff|+0.0000000000000X-3ff|+1.0000000000000X+000|+0.0000000000000X-3ff|+0.0000000000000X-3ff|+1.0000000000000X+000|+0.0000000000000X-3ff|+0.0000000000000X-3ff|+0.0000000000000X-3ff|+0.0000000000000X-3ff|+0.0000000000000X-3ff|+1.0000000000000X+000|+0.0000000000000X-3ff|+0.0000000000000X-3ff|+1.0000000000000X+000|+0.0000000000000X-3ff|+0.0000000000000X-3ff|+1.0000000000000X+000|+0.0000000000000X-3ff|+0.0000000000000X-3ff|+0.0000000000000X-3ff|+0.0000000000000X-3ff|+0.0000000000000X-3ff"},
    {"disp_eq_eq", 1119,
     "local __rc = 0",
     "%21x (0==0==0)",
     "0|+0.0000000000000X-3ff"},
    {"local_eq_eq", 1128,
     "capture noisily local l_eq_eq = 0==0==0\nlocal __rc = _rc",
     "`l_eq_eq'",
     "0|0"},
    {"scalar_eq_eq", 1137,
     "capture noisily scalar s_eq_eq = 0==0==0\nlocal __rc = _rc",
     "%21x s_eq_eq",
     "0|+0.0000000000000X-3ff"},
    {"ifcmd_eq_eq", 1152,
     "local __v = 0\ncapture noisily {\n    if 0==0==0 {\n        local __v = 1\n    }\n}\nlocal __rc = _rc",
     "`__v'",
     "0|0"},
    {"disp_eq_ne", 1159,
     "local __rc = 0",
     "%21x (0==0!=0)",
     "0|+1.0000000000000X+000"},
    {"local_eq_ne", 1168,
     "capture noisily local l_eq_ne = 0==0!=0\nlocal __rc = _rc",
     "`l_eq_ne'",
     "0|1"},
    {"scalar_eq_ne", 1177,
     "capture noisily scalar s_eq_ne = 0==0!=0\nlocal __rc = _rc",
     "%21x s_eq_ne",
     "0|+1.0000000000000X+000"},
    {"ifcmd_eq_ne", 1192,
     "local __v = 0\ncapture noisily {\n    if 0==0!=0 {\n        local __v = 1\n    }\n}\nlocal __rc = _rc",
     "`__v'",
     "0|1"},
    {"disp_eq_gt", 1199,
     "local __rc = 0",
     "%21x (0==0>0)",
     "0|+1.0000000000000X+000"},
    {"local_eq_gt", 1208,
     "capture noisily local l_eq_gt = 0==0>0\nlocal __rc = _rc",
     "`l_eq_gt'",
     "0|1"},
    {"scalar_eq_gt", 1217,
     "capture noisily scalar s_eq_gt = 0==0>0\nlocal __rc = _rc",
     "%21x s_eq_gt",
     "0|+1.0000000000000X+000"},
    {"ifcmd_eq_gt", 1232,
     "local __v = 0\ncapture noisily {\n    if 0==0>0 {\n        local __v = 1\n    }\n}\nlocal __rc = _rc",
     "`__v'",
     "0|1"},
    {"disp_eq_lt", 1239,
     "local __rc = 0",
     "%21x (0==0<0)",
     "0|+1.0000000000000X+000"},
    {"local_eq_lt", 1248,
     "capture noisily local l_eq_lt = 0==0<0\nlocal __rc = _rc",
     "`l_eq_lt'",
     "0|1"},
    {"scalar_eq_lt", 1257,
     "capture noisily scalar s_eq_lt = 0==0<0\nlocal __rc = _rc",
     "%21x s_eq_lt",
     "0|+1.0000000000000X+000"},
    {"ifcmd_eq_lt", 1272,
     "local __v = 0\ncapture noisily {\n    if 0==0<0 {\n        local __v = 1\n    }\n}\nlocal __rc = _rc",
     "`__v'",
     "0|1"},
    {"disp_eq_ge", 1279,
     "local __rc = 0",
     "%21x (0==0>=0)",
     "0|+0.0000000000000X-3ff"},
    {"local_eq_ge", 1288,
     "capture noisily local l_eq_ge = 0==0>=0\nlocal __rc = _rc",
     "`l_eq_ge'",
     "0|0"},
    {"scalar_eq_ge", 1297,
     "capture noisily scalar s_eq_ge = 0==0>=0\nlocal __rc = _rc",
     "%21x s_eq_ge",
     "0|+0.0000000000000X-3ff"},
    {"ifcmd_eq_ge", 1312,
     "local __v = 0\ncapture noisily {\n    if 0==0>=0 {\n        local __v = 1\n    }\n}\nlocal __rc = _rc",
     "`__v'",
     "0|0"},
    {"disp_eq_le", 1319,
     "local __rc = 0",
     "%21x (0==0<=0)",
     "0|+0.0000000000000X-3ff"},
    {"local_eq_le", 1328,
     "capture noisily local l_eq_le = 0==0<=0\nlocal __rc = _rc",
     "`l_eq_le'",
     "0|0"},
    {"scalar_eq_le", 1337,
     "capture noisily scalar s_eq_le = 0==0<=0\nlocal __rc = _rc",
     "%21x s_eq_le",
     "0|+0.0000000000000X-3ff"},
    {"ifcmd_eq_le", 1352,
     "local __v = 0\ncapture noisily {\n    if 0==0<=0 {\n        local __v = 1\n    }\n}\nlocal __rc = _rc",
     "`__v'",
     "0|0"},
    {"disp_ne_eq", 1359,
     "local __rc = 0",
     "%21x (0!=0==0)",
     "0|+1.0000000000000X+000"},
    {"local_ne_eq", 1368,
     "capture noisily local l_ne_eq = 0!=0==0\nlocal __rc = _rc",
     "`l_ne_eq'",
     "0|1"},
    {"scalar_ne_eq", 1377,
     "capture noisily scalar s_ne_eq = 0!=0==0\nlocal __rc = _rc",
     "%21x s_ne_eq",
     "0|+1.0000000000000X+000"},
    {"ifcmd_ne_eq", 1392,
     "local __v = 0\ncapture noisily {\n    if 0!=0==0 {\n        local __v = 1\n    }\n}\nlocal __rc = _rc",
     "`__v'",
     "0|1"},
    {"disp_ne_ne", 1399,
     "local __rc = 0",
     "%21x (0!=0!=1)",
     "0|+1.0000000000000X+000"},
    {"local_ne_ne", 1408,
     "capture noisily local l_ne_ne = 0!=0!=1\nlocal __rc = _rc",
     "`l_ne_ne'",
     "0|1"},
    {"scalar_ne_ne", 1417,
     "capture noisily scalar s_ne_ne = 0!=0!=1\nlocal __rc = _rc",
     "%21x s_ne_ne",
     "0|+1.0000000000000X+000"},
    {"ifcmd_ne_ne", 1432,
     "local __v = 0\ncapture noisily {\n    if 0!=0!=1 {\n        local __v = 1\n    }\n}\nlocal __rc = _rc",
     "`__v'",
     "0|1"},
    {"disp_ne_gt", 1439,
     "local __rc = 0",
     "%21x (0!=2>1)",
     "0|+0.0000000000000X-3ff"},
    {"local_ne_gt", 1448,
     "capture noisily local l_ne_gt = 0!=2>1\nlocal __rc = _rc",
     "`l_ne_gt'",
     "0|0"},
    {"scalar_ne_gt", 1457,
     "capture noisily scalar s_ne_gt = 0!=2>1\nlocal __rc = _rc",
     "%21x s_ne_gt",
     "0|+0.0000000000000X-3ff"},
    {"ifcmd_ne_gt", 1472,
     "local __v = 0\ncapture noisily {\n    if 0!=2>1 {\n        local __v = 1\n    }\n}\nlocal __rc = _rc",
     "`__v'",
     "0|0"},
    {"disp_ne_lt", 1479,
     "local __rc = 0",
     "%21x (0!=0<1)",
     "0|+1.0000000000000X+000"},
    {"local_ne_lt", 1488,
     "capture noisily local l_ne_lt = 0!=0<1\nlocal __rc = _rc",
     "`l_ne_lt'",
     "0|1"},
    {"scalar_ne_lt", 1497,
     "capture noisily scalar s_ne_lt = 0!=0<1\nlocal __rc = _rc",
     "%21x s_ne_lt",
     "0|+1.0000000000000X+000"},
    {"ifcmd_ne_lt", 1512,
     "local __v = 0\ncapture noisily {\n    if 0!=0<1 {\n        local __v = 1\n    }\n}\nlocal __rc = _rc",
     "`__v'",
     "0|1"},
    {"disp_ne_ge", 1519,
     "local __rc = 0",
     "%21x (0!=0>=0)",
     "0|+1.0000000000000X+000"},
    {"local_ne_ge", 1528,
     "capture noisily local l_ne_ge = 0!=0>=0\nlocal __rc = _rc",
     "`l_ne_ge'",
     "0|1"},
    {"scalar_ne_ge", 1537,
     "capture noisily scalar s_ne_ge = 0!=0>=0\nlocal __rc = _rc",
     "%21x s_ne_ge",
     "0|+1.0000000000000X+000"},
    {"ifcmd_ne_ge", 1552,
     "local __v = 0\ncapture noisily {\n    if 0!=0>=0 {\n        local __v = 1\n    }\n}\nlocal __rc = _rc",
     "`__v'",
     "0|1"},
    {"disp_ne_le", 1559,
     "local __rc = 0",
     "%21x (0!=0<=0)",
     "0|+1.0000000000000X+000"},
    {"local_ne_le", 1568,
     "capture noisily local l_ne_le = 0!=0<=0\nlocal __rc = _rc",
     "`l_ne_le'",
     "0|1"},
    {"scalar_ne_le", 1577,
     "capture noisily scalar s_ne_le = 0!=0<=0\nlocal __rc = _rc",
     "%21x s_ne_le",
     "0|+1.0000000000000X+000"},
    {"ifcmd_ne_le", 1592,
     "local __v = 0\ncapture noisily {\n    if 0!=0<=0 {\n        local __v = 1\n    }\n}\nlocal __rc = _rc",
     "`__v'",
     "0|1"},
    {"disp_gt_eq", 1599,
     "local __rc = 0",
     "%21x (0>0==0)",
     "0|+1.0000000000000X+000"},
    {"local_gt_eq", 1608,
     "capture noisily local l_gt_eq = 0>0==0\nlocal __rc = _rc",
     "`l_gt_eq'",
     "0|1"},
    {"scalar_gt_eq", 1617,
     "capture noisily scalar s_gt_eq = 0>0==0\nlocal __rc = _rc",
     "%21x s_gt_eq",
     "0|+1.0000000000000X+000"},
    {"ifcmd_gt_eq", 1632,
     "local __v = 0\ncapture noisily {\n    if 0>0==0 {\n        local __v = 1\n    }\n}\nlocal __rc = _rc",
     "`__v'",
     "0|1"},
    {"disp_gt_ne", 1639,
     "local __rc = 0",
     "%21x (0>0!=1)",
     "0|+0.0000000000000X-3ff"},
    {"local_gt_ne", 1648,
     "capture noisily local l_gt_ne = 0>0!=1\nlocal __rc = _rc",
     "`l_gt_ne'",
     "0|0"},
    {"scalar_gt_ne", 1657,
     "capture noisily scalar s_gt_ne = 0>0!=1\nlocal __rc = _rc",
     "%21x s_gt_ne",
     "0|+0.0000000000000X-3ff"},
    {"ifcmd_gt_ne", 1672,
     "local __v = 0\ncapture noisily {\n    if 0>0!=1 {\n        local __v = 1\n    }\n}\nlocal __rc = _rc",
     "`__v'",
     "0|0"},
    {"disp_gt_gt", 1679,
     "local __rc = 0",
     "%21x (1>0>0)",
     "0|+1.0000000000000X+000"},
    {"local_gt_gt", 1688,
     "capture noisily local l_gt_gt = 1>0>0\nlocal __rc = _rc",
     "`l_gt_gt'",
     "0|1"},
    {"scalar_gt_gt", 1697,
     "capture noisily scalar s_gt_gt = 1>0>0\nlocal __rc = _rc",
     "%21x s_gt_gt",
     "0|+1.0000000000000X+000"},
    {"ifcmd_gt_gt", 1712,
     "local __v = 0\ncapture noisily {\n    if 1>0>0 {\n        local __v = 1\n    }\n}\nlocal __rc = _rc",
     "`__v'",
     "0|1"},
    {"disp_gt_lt", 1719,
     "local __rc = 0",
     "%21x (0>0<1)",
     "0|+1.0000000000000X+000"},
    {"local_gt_lt", 1728,
     "capture noisily local l_gt_lt = 0>0<1\nlocal __rc = _rc",
     "`l_gt_lt'",
     "0|1"},
    {"scalar_gt_lt", 1737,
     "capture noisily scalar s_gt_lt = 0>0<1\nlocal __rc = _rc",
     "%21x s_gt_lt",
     "0|+1.0000000000000X+000"},
    {"ifcmd_gt_lt", 1752,
     "local __v = 0\ncapture noisily {\n    if 0>0<1 {\n        local __v = 1\n    }\n}\nlocal __rc = _rc",
     "`__v'",
     "0|1"},
    {"disp_gt_ge", 1759,
     "local __rc = 0",
     "%21x (0>0>=0)",
     "0|+1.0000000000000X+000"},
    {"local_gt_ge", 1768,
     "capture noisily local l_gt_ge = 0>0>=0\nlocal __rc = _rc",
     "`l_gt_ge'",
     "0|1"},
    {"scalar_gt_ge", 1777,
     "capture noisily scalar s_gt_ge = 0>0>=0\nlocal __rc = _rc",
     "%21x s_gt_ge",
     "0|+1.0000000000000X+000"},
    {"ifcmd_gt_ge", 1792,
     "local __v = 0\ncapture noisily {\n    if 0>0>=0 {\n        local __v = 1\n    }\n}\nlocal __rc = _rc",
     "`__v'",
     "0|1"},
    {"disp_gt_le", 1799,
     "local __rc = 0",
     "%21x (0>0<=0)",
     "0|+1.0000000000000X+000"},
    {"local_gt_le", 1808,
     "capture noisily local l_gt_le = 0>0<=0\nlocal __rc = _rc",
     "`l_gt_le'",
     "0|1"},
    {"scalar_gt_le", 1817,
     "capture noisily scalar s_gt_le = 0>0<=0\nlocal __rc = _rc",
     "%21x s_gt_le",
     "0|+1.0000000000000X+000"},
    {"ifcmd_gt_le", 1832,
     "local __v = 0\ncapture noisily {\n    if 0>0<=0 {\n        local __v = 1\n    }\n}\nlocal __rc = _rc",
     "`__v'",
     "0|1"},
    {"disp_lt_eq", 1839,
     "local __rc = 0",
     "%21x (0<0==0)",
     "0|+1.0000000000000X+000"},
    {"local_lt_eq", 1848,
     "capture noisily local l_lt_eq = 0<0==0\nlocal __rc = _rc",
     "`l_lt_eq'",
     "0|1"},
    {"scalar_lt_eq", 1857,
     "capture noisily scalar s_lt_eq = 0<0==0\nlocal __rc = _rc",
     "%21x s_lt_eq",
     "0|+1.0000000000000X+000"},
    {"ifcmd_lt_eq", 1872,
     "local __v = 0\ncapture noisily {\n    if 0<0==0 {\n        local __v = 1\n    }\n}\nlocal __rc = _rc",
     "`__v'",
     "0|1"},
    {"disp_lt_ne", 1879,
     "local __rc = 0",
     "%21x (0<0!=1)",
     "0|+1.0000000000000X+000"},
    {"local_lt_ne", 1888,
     "capture noisily local l_lt_ne = 0<0!=1\nlocal __rc = _rc",
     "`l_lt_ne'",
     "0|1"},
    {"scalar_lt_ne", 1897,
     "capture noisily scalar s_lt_ne = 0<0!=1\nlocal __rc = _rc",
     "%21x s_lt_ne",
     "0|+1.0000000000000X+000"},
    {"ifcmd_lt_ne", 1912,
     "local __v = 0\ncapture noisily {\n    if 0<0!=1 {\n        local __v = 1\n    }\n}\nlocal __rc = _rc",
     "`__v'",
     "0|1"},
    {"disp_lt_gt", 1919,
     "local __rc = 0",
     "%21x (0<2>1)",
     "0|+1.0000000000000X+000"},
    {"local_lt_gt", 1928,
     "capture noisily local l_lt_gt = 0<2>1\nlocal __rc = _rc",
     "`l_lt_gt'",
     "0|1"},
    {"scalar_lt_gt", 1937,
     "capture noisily scalar s_lt_gt = 0<2>1\nlocal __rc = _rc",
     "%21x s_lt_gt",
     "0|+1.0000000000000X+000"},
    {"ifcmd_lt_gt", 1952,
     "local __v = 0\ncapture noisily {\n    if 0<2>1 {\n        local __v = 1\n    }\n}\nlocal __rc = _rc",
     "`__v'",
     "0|1"},
    {"disp_lt_lt", 1959,
     "local __rc = 0",
     "%21x (0<0<1)",
     "0|+1.0000000000000X+000"},
    {"local_lt_lt", 1968,
     "capture noisily local l_lt_lt = 0<0<1\nlocal __rc = _rc",
     "`l_lt_lt'",
     "0|1"},
    {"scalar_lt_lt", 1977,
     "capture noisily scalar s_lt_lt = 0<0<1\nlocal __rc = _rc",
     "%21x s_lt_lt",
     "0|+1.0000000000000X+000"},
    {"ifcmd_lt_lt", 1992,
     "local __v = 0\ncapture noisily {\n    if 0<0<1 {\n        local __v = 1\n    }\n}\nlocal __rc = _rc",
     "`__v'",
     "0|1"},
    {"disp_lt_ge", 1999,
     "local __rc = 0",
     "%21x (0<0>=0)",
     "0|+1.0000000000000X+000"},
    {"local_lt_ge", 2008,
     "capture noisily local l_lt_ge = 0<0>=0\nlocal __rc = _rc",
     "`l_lt_ge'",
     "0|1"},
    {"scalar_lt_ge", 2017,
     "capture noisily scalar s_lt_ge = 0<0>=0\nlocal __rc = _rc",
     "%21x s_lt_ge",
     "0|+1.0000000000000X+000"},
    {"ifcmd_lt_ge", 2032,
     "local __v = 0\ncapture noisily {\n    if 0<0>=0 {\n        local __v = 1\n    }\n}\nlocal __rc = _rc",
     "`__v'",
     "0|1"},
    {"disp_lt_le", 2039,
     "local __rc = 0",
     "%21x (0<0<=0)",
     "0|+1.0000000000000X+000"},
    {"local_lt_le", 2048,
     "capture noisily local l_lt_le = 0<0<=0\nlocal __rc = _rc",
     "`l_lt_le'",
     "0|1"},
    {"scalar_lt_le", 2057,
     "capture noisily scalar s_lt_le = 0<0<=0\nlocal __rc = _rc",
     "%21x s_lt_le",
     "0|+1.0000000000000X+000"},
    {"ifcmd_lt_le", 2072,
     "local __v = 0\ncapture noisily {\n    if 0<0<=0 {\n        local __v = 1\n    }\n}\nlocal __rc = _rc",
     "`__v'",
     "0|1"},
    {"disp_ge_eq", 2079,
     "local __rc = 0",
     "%21x (0>=0==0)",
     "0|+0.0000000000000X-3ff"},
    {"local_ge_eq", 2088,
     "capture noisily local l_ge_eq = 0>=0==0\nlocal __rc = _rc",
     "`l_ge_eq'",
     "0|0"},
    {"scalar_ge_eq", 2097,
     "capture noisily scalar s_ge_eq = 0>=0==0\nlocal __rc = _rc",
     "%21x s_ge_eq",
     "0|+0.0000000000000X-3ff"},
    {"ifcmd_ge_eq", 2112,
     "local __v = 0\ncapture noisily {\n    if 0>=0==0 {\n        local __v = 1\n    }\n}\nlocal __rc = _rc",
     "`__v'",
     "0|0"},
    {"disp_ge_ne", 2119,
     "local __rc = 0",
     "%21x (0>=0!=0)",
     "0|+1.0000000000000X+000"},
    {"local_ge_ne", 2128,
     "capture noisily local l_ge_ne = 0>=0!=0\nlocal __rc = _rc",
     "`l_ge_ne'",
     "0|1"},
    {"scalar_ge_ne", 2137,
     "capture noisily scalar s_ge_ne = 0>=0!=0\nlocal __rc = _rc",
     "%21x s_ge_ne",
     "0|+1.0000000000000X+000"},
    {"ifcmd_ge_ne", 2152,
     "local __v = 0\ncapture noisily {\n    if 0>=0!=0 {\n        local __v = 1\n    }\n}\nlocal __rc = _rc",
     "`__v'",
     "0|1"},
    {"disp_ge_gt", 2159,
     "local __rc = 0",
     "%21x (0>=0>0)",
     "0|+1.0000000000000X+000"},
    {"local_ge_gt", 2168,
     "capture noisily local l_ge_gt = 0>=0>0\nlocal __rc = _rc",
     "`l_ge_gt'",
     "0|1"},
    {"scalar_ge_gt", 2177,
     "capture noisily scalar s_ge_gt = 0>=0>0\nlocal __rc = _rc",
     "%21x s_ge_gt",
     "0|+1.0000000000000X+000"},
    {"ifcmd_ge_gt", 2192,
     "local __v = 0\ncapture noisily {\n    if 0>=0>0 {\n        local __v = 1\n    }\n}\nlocal __rc = _rc",
     "`__v'",
     "0|1"},
    {"disp_ge_lt", 2199,
     "local __rc = 0",
     "%21x (0>=0<0)",
     "0|+1.0000000000000X+000"},
    {"local_ge_lt", 2208,
     "capture noisily local l_ge_lt = 0>=0<0\nlocal __rc = _rc",
     "`l_ge_lt'",
     "0|1"},
    {"scalar_ge_lt", 2217,
     "capture noisily scalar s_ge_lt = 0>=0<0\nlocal __rc = _rc",
     "%21x s_ge_lt",
     "0|+1.0000000000000X+000"},
    {"ifcmd_ge_lt", 2232,
     "local __v = 0\ncapture noisily {\n    if 0>=0<0 {\n        local __v = 1\n    }\n}\nlocal __rc = _rc",
     "`__v'",
     "0|1"},
    {"disp_ge_ge", 2239,
     "local __rc = 0",
     "%21x (0>=0>=0)",
     "0|+1.0000000000000X+000"},
    {"local_ge_ge", 2248,
     "capture noisily local l_ge_ge = 0>=0>=0\nlocal __rc = _rc",
     "`l_ge_ge'",
     "0|1"},
    {"scalar_ge_ge", 2257,
     "capture noisily scalar s_ge_ge = 0>=0>=0\nlocal __rc = _rc",
     "%21x s_ge_ge",
     "0|+1.0000000000000X+000"},
    {"ifcmd_ge_ge", 2272,
     "local __v = 0\ncapture noisily {\n    if 0>=0>=0 {\n        local __v = 1\n    }\n}\nlocal __rc = _rc",
     "`__v'",
     "0|1"},
    {"disp_ge_le", 2279,
     "local __rc = 0",
     "%21x (0>=0<=0)",
     "0|+0.0000000000000X-3ff"},
    {"local_ge_le", 2288,
     "capture noisily local l_ge_le = 0>=0<=0\nlocal __rc = _rc",
     "`l_ge_le'",
     "0|0"},
    {"scalar_ge_le", 2297,
     "capture noisily scalar s_ge_le = 0>=0<=0\nlocal __rc = _rc",
     "%21x s_ge_le",
     "0|+0.0000000000000X-3ff"},
    {"ifcmd_ge_le", 2312,
     "local __v = 0\ncapture noisily {\n    if 0>=0<=0 {\n        local __v = 1\n    }\n}\nlocal __rc = _rc",
     "`__v'",
     "0|0"},
    {"disp_le_eq", 2319,
     "local __rc = 0",
     "%21x (0<=0==0)",
     "0|+0.0000000000000X-3ff"},
    {"local_le_eq", 2328,
     "capture noisily local l_le_eq = 0<=0==0\nlocal __rc = _rc",
     "`l_le_eq'",
     "0|0"},
    {"scalar_le_eq", 2337,
     "capture noisily scalar s_le_eq = 0<=0==0\nlocal __rc = _rc",
     "%21x s_le_eq",
     "0|+0.0000000000000X-3ff"},
    {"ifcmd_le_eq", 2352,
     "local __v = 0\ncapture noisily {\n    if 0<=0==0 {\n        local __v = 1\n    }\n}\nlocal __rc = _rc",
     "`__v'",
     "0|0"},
    {"disp_le_ne", 2359,
     "local __rc = 0",
     "%21x (0<=0!=0)",
     "0|+1.0000000000000X+000"},
    {"local_le_ne", 2368,
     "capture noisily local l_le_ne = 0<=0!=0\nlocal __rc = _rc",
     "`l_le_ne'",
     "0|1"},
    {"scalar_le_ne", 2377,
     "capture noisily scalar s_le_ne = 0<=0!=0\nlocal __rc = _rc",
     "%21x s_le_ne",
     "0|+1.0000000000000X+000"},
    {"ifcmd_le_ne", 2392,
     "local __v = 0\ncapture noisily {\n    if 0<=0!=0 {\n        local __v = 1\n    }\n}\nlocal __rc = _rc",
     "`__v'",
     "0|1"},
    {"disp_le_gt", 2399,
     "local __rc = 0",
     "%21x (0<=0>0)",
     "0|+1.0000000000000X+000"},
    {"local_le_gt", 2408,
     "capture noisily local l_le_gt = 0<=0>0\nlocal __rc = _rc",
     "`l_le_gt'",
     "0|1"},
    {"scalar_le_gt", 2417,
     "capture noisily scalar s_le_gt = 0<=0>0\nlocal __rc = _rc",
     "%21x s_le_gt",
     "0|+1.0000000000000X+000"},
    {"ifcmd_le_gt", 2432,
     "local __v = 0\ncapture noisily {\n    if 0<=0>0 {\n        local __v = 1\n    }\n}\nlocal __rc = _rc",
     "`__v'",
     "0|1"},
    {"disp_le_lt", 2439,
     "local __rc = 0",
     "%21x (0<=0<0)",
     "0|+1.0000000000000X+000"},
    {"local_le_lt", 2448,
     "capture noisily local l_le_lt = 0<=0<0\nlocal __rc = _rc",
     "`l_le_lt'",
     "0|1"},
    {"scalar_le_lt", 2457,
     "capture noisily scalar s_le_lt = 0<=0<0\nlocal __rc = _rc",
     "%21x s_le_lt",
     "0|+1.0000000000000X+000"},
    {"ifcmd_le_lt", 2472,
     "local __v = 0\ncapture noisily {\n    if 0<=0<0 {\n        local __v = 1\n    }\n}\nlocal __rc = _rc",
     "`__v'",
     "0|1"},
    {"disp_le_ge", 2479,
     "local __rc = 0",
     "%21x (0<=0>=1)",
     "0|+1.0000000000000X+000"},
    {"local_le_ge", 2488,
     "capture noisily local l_le_ge = 0<=0>=1\nlocal __rc = _rc",
     "`l_le_ge'",
     "0|1"},
    {"scalar_le_ge", 2497,
     "capture noisily scalar s_le_ge = 0<=0>=1\nlocal __rc = _rc",
     "%21x s_le_ge",
     "0|+1.0000000000000X+000"},
    {"ifcmd_le_ge", 2512,
     "local __v = 0\ncapture noisily {\n    if 0<=0>=1 {\n        local __v = 1\n    }\n}\nlocal __rc = _rc",
     "`__v'",
     "0|1"},
    {"disp_le_le", 2519,
     "local __rc = 0",
     "%21x (0<=0<=0)",
     "0|+0.0000000000000X-3ff"},
    {"local_le_le", 2528,
     "capture noisily local l_le_le = 0<=0<=0\nlocal __rc = _rc",
     "`l_le_le'",
     "0|0"},
    {"scalar_le_le", 2537,
     "capture noisily scalar s_le_le = 0<=0<=0\nlocal __rc = _rc",
     "%21x s_le_le",
     "0|+0.0000000000000X-3ff"},
    {"ifcmd_le_le", 2552,
     "local __v = 0\ncapture noisily {\n    if 0<=0<=0 {\n        local __v = 1\n    }\n}\nlocal __rc = _rc",
     "`__v'",
     "0|0"},
    {"disp_not_mul", 2559,
     "local __rc = 0",
     "%21x (!0*2)",
     "0|+1.0000000000000X+001"},
    {"ifcmd_not_mul", 2574,
     "local __v = 0\ncapture noisily {\n    if !0*2 {\n        local __v = 1\n    }\n}\nlocal __rc = _rc",
     "`__v'",
     "0|1"},
    {"disp_not_add", 2581,
     "local __rc = 0",
     "%21x (!0+1)",
     "0|+1.0000000000000X+001"},
    {"ifcmd_not_add", 2596,
     "local __v = 0\ncapture noisily {\n    if !0+1 {\n        local __v = 1\n    }\n}\nlocal __rc = _rc",
     "`__v'",
     "0|1"},
    {"disp_not_gt", 2603,
     "local __rc = 0",
     "%21x (!-1>0)",
     "0"},
};

// FU-842 and the `!`-sign rule, from the supplementary oracle
// internal capture w2ac_expr_valueref.log (StataBE
// 19.5, the same 27 rows). A resolved VALUE (scalar, scalar(), _b[], matrix element,
// r()) is one operand: with zneg = -2, zneg^2 is 4 and -zneg^2 is -4, where pasting the
// text `-2` made them -4 and 4; a local macro stays text (gen/disp_vr_local: -9). A sign
// after `!` or `~`, spaces or not, is r(198) in every layer. On main 48 of these 93 arms
// fail, and the engine-layer `!`-sign arms (gen_not_neg_sp, gen_not_plus,
// gen_tilde_neg, gen_not_not_neg, gen_neg_not_neg, gen_pow_not_neg, count_not_neg,
// count_not_neg_sp) were refused at rc 1, not 198 (FU-862: coarseRc had no rule for
// the engine's "expr: invalid syntax"); test_fn_w2c pins the engine refusal. "@..." fields are the record's display tail verbatim (the local arms).
const PrecArm kVrArms[] = {
    {"fx_u", 53,
     "version 19\nclear all\nset more off\nset linesize 255\nset obs 27\ngenerate double u = floor((_n - 1) / 9)\ngenerate double v = mod(floor((_n - 1) / 3), 3)\ngenerate double w = mod(_n - 1, 3)\ngenerate double t = u - 1\nlocal __rc = 0",
     "%21x u[1]\t%21x u[2]\t%21x u[3]\t%21x u[4]\t%21x u[5]\t%21x u[6]\t%21x u[7]\t%21x u[8]\t%21x u[9]\t%21x u[10]\t%21x u[11]\t%21x u[12]\t%21x u[13]\t%21x u[14]\t%21x u[15]\t%21x u[16]\t%21x u[17]\t%21x u[18]\t%21x u[19]\t%21x u[20]\t%21x u[21]\t%21x u[22]\t%21x u[23]\t%21x u[24]\t%21x u[25]\t%21x u[26]\t%21x u[27]",
     "0|+0.0000000000000X-3ff|+0.0000000000000X-3ff|+0.0000000000000X-3ff|+0.0000000000000X-3ff|+0.0000000000000X-3ff|+0.0000000000000X-3ff|+0.0000000000000X-3ff|+0.0000000000000X-3ff|+0.0000000000000X-3ff|+1.0000000000000X+000|+1.0000000000000X+000|+1.0000000000000X+000|+1.0000000000000X+000|+1.0000000000000X+000|+1.0000000000000X+000|+1.0000000000000X+000|+1.0000000000000X+000|+1.0000000000000X+000|+1.0000000000000X+001|+1.0000000000000X+001|+1.0000000000000X+001|+1.0000000000000X+001|+1.0000000000000X+001|+1.0000000000000X+001|+1.0000000000000X+001|+1.0000000000000X+001|+1.0000000000000X+001"},
    {"fx_v", 61,
     "",
     "%21x v[1]\t%21x v[2]\t%21x v[3]\t%21x v[4]\t%21x v[5]\t%21x v[6]\t%21x v[7]\t%21x v[8]\t%21x v[9]\t%21x v[10]\t%21x v[11]\t%21x v[12]\t%21x v[13]\t%21x v[14]\t%21x v[15]\t%21x v[16]\t%21x v[17]\t%21x v[18]\t%21x v[19]\t%21x v[20]\t%21x v[21]\t%21x v[22]\t%21x v[23]\t%21x v[24]\t%21x v[25]\t%21x v[26]\t%21x v[27]",
     "0|+0.0000000000000X-3ff|+0.0000000000000X-3ff|+0.0000000000000X-3ff|+1.0000000000000X+000|+1.0000000000000X+000|+1.0000000000000X+000|+1.0000000000000X+001|+1.0000000000000X+001|+1.0000000000000X+001|+0.0000000000000X-3ff|+0.0000000000000X-3ff|+0.0000000000000X-3ff|+1.0000000000000X+000|+1.0000000000000X+000|+1.0000000000000X+000|+1.0000000000000X+001|+1.0000000000000X+001|+1.0000000000000X+001|+0.0000000000000X-3ff|+0.0000000000000X-3ff|+0.0000000000000X-3ff|+1.0000000000000X+000|+1.0000000000000X+000|+1.0000000000000X+000|+1.0000000000000X+001|+1.0000000000000X+001|+1.0000000000000X+001"},
    {"fx_w", 69,
     "",
     "%21x w[1]\t%21x w[2]\t%21x w[3]\t%21x w[4]\t%21x w[5]\t%21x w[6]\t%21x w[7]\t%21x w[8]\t%21x w[9]\t%21x w[10]\t%21x w[11]\t%21x w[12]\t%21x w[13]\t%21x w[14]\t%21x w[15]\t%21x w[16]\t%21x w[17]\t%21x w[18]\t%21x w[19]\t%21x w[20]\t%21x w[21]\t%21x w[22]\t%21x w[23]\t%21x w[24]\t%21x w[25]\t%21x w[26]\t%21x w[27]",
     "0|+0.0000000000000X-3ff|+1.0000000000000X+000|+1.0000000000000X+001|+0.0000000000000X-3ff|+1.0000000000000X+000|+1.0000000000000X+001|+0.0000000000000X-3ff|+1.0000000000000X+000|+1.0000000000000X+001|+0.0000000000000X-3ff|+1.0000000000000X+000|+1.0000000000000X+001|+0.0000000000000X-3ff|+1.0000000000000X+000|+1.0000000000000X+001|+0.0000000000000X-3ff|+1.0000000000000X+000|+1.0000000000000X+001|+0.0000000000000X-3ff|+1.0000000000000X+000|+1.0000000000000X+001|+0.0000000000000X-3ff|+1.0000000000000X+000|+1.0000000000000X+001|+0.0000000000000X-3ff|+1.0000000000000X+000|+1.0000000000000X+001"},
    {"fx_t", 77,
     "",
     "%21x t[1]\t%21x t[2]\t%21x t[3]\t%21x t[4]\t%21x t[5]\t%21x t[6]\t%21x t[7]\t%21x t[8]\t%21x t[9]\t%21x t[10]\t%21x t[11]\t%21x t[12]\t%21x t[13]\t%21x t[14]\t%21x t[15]\t%21x t[16]\t%21x t[17]\t%21x t[18]\t%21x t[19]\t%21x t[20]\t%21x t[21]\t%21x t[22]\t%21x t[23]\t%21x t[24]\t%21x t[25]\t%21x t[26]\t%21x t[27]",
     "0|-1.0000000000000X+000|-1.0000000000000X+000|-1.0000000000000X+000|-1.0000000000000X+000|-1.0000000000000X+000|-1.0000000000000X+000|-1.0000000000000X+000|-1.0000000000000X+000|-1.0000000000000X+000|+0.0000000000000X-3ff|+0.0000000000000X-3ff|+0.0000000000000X-3ff|+0.0000000000000X-3ff|+0.0000000000000X-3ff|+0.0000000000000X-3ff|+0.0000000000000X-3ff|+0.0000000000000X-3ff|+0.0000000000000X-3ff|+1.0000000000000X+000|+1.0000000000000X+000|+1.0000000000000X+000|+1.0000000000000X+000|+1.0000000000000X+000|+1.0000000000000X+000|+1.0000000000000X+000|+1.0000000000000X+000|+1.0000000000000X+000"},
    {"disp_not_and", 88,
     "local __rc = 0",
     "%21x (!0&2)",
     "0|+1.0000000000000X+000"},
    {"ifcmd_not_and", 103,
     "local __v = 0\ncapture noisily {\n    if !0&2 {\n        local __v = 1\n    }\n}\nlocal __rc = _rc",
     "`__v'",
     "0|1"},
    {"disp_tilde_pow0", 110,
     "local __rc = 0",
     "%21x (~3^0)",
     "0|+0.0000000000000X-3ff"},
    {"ifcmd_tilde_pow0", 125,
     "local __v = 0\ncapture noisily {\n    if ~3^0 {\n        local __v = 1\n    }\n}\nlocal __rc = _rc",
     "`__v'",
     "0|0"},
    {"disp_pow_not", 132,
     "local __rc = 0",
     "%21x (2^!0)",
     "0|+1.0000000000000X+001"},
    {"ifcmd_pow_not", 147,
     "local __v = 0\ncapture noisily {\n    if 2^!0 {\n        local __v = 1\n    }\n}\nlocal __rc = _rc",
     "`__v'",
     "0|1"},
    {"disp_vr_scalar", 168,
     "generate double yy = -u + mod(_n, 2) / 4\nscalar zneg = -2\nmatrix MN = (-3)\nquietly regress yy u\nlocal m = -3\nlocal __rc = 0",
     "%21x (zneg^2)",
     "0|+1.0000000000000X+002"},
    {"scalar_vr_scalar", 177,
     "capture noisily scalar s_vr_scalar = zneg^2\nlocal __rc = _rc",
     "%21x s_vr_scalar",
     "0|+1.0000000000000X+002"},
    {"gen_vr_scalar", 186,
     "capture noisily generate double v_vr_scalar = zneg^2\nlocal __rc = _rc",
     "%21x v_vr_scalar[1]",
     "0|+1.0000000000000X+002"},
    {"count_vr_scalar", 196,
     "capture noisily count if zneg^2 > 3\nlocal __rc = _rc",
     "%21x r(N)",
     "0|+1.b000000000000X+004"},
    {"ifcmd_vr_scalar", 211,
     "local __v = 0\ncapture noisily {\n    if zneg^2 > 3 {\n        local __v = 1\n    }\n}\nlocal __rc = _rc",
     "`__v'",
     "0|1"},
    {"disp_vr_scalarfn", 218,
     "local __rc = 0",
     "%21x (scalar(zneg)^2)",
     "0|+1.0000000000000X+002"},
    {"scalar_vr_scalarfn", 227,
     "capture noisily scalar s_vr_scalarfn = scalar(zneg)^2\nlocal __rc = _rc",
     "%21x s_vr_scalarfn",
     "0|+1.0000000000000X+002"},
    {"gen_vr_scalarfn", 236,
     "capture noisily generate double v_vr_scalarfn = scalar(zneg)^2\nlocal __rc = _rc",
     "%21x v_vr_scalarfn[1]",
     "0|+1.0000000000000X+002"},
    {"count_vr_scalarfn", 246,
     "capture noisily count if scalar(zneg)^2 > 3\nlocal __rc = _rc",
     "%21x r(N)",
     "0|+1.b000000000000X+004"},
    {"ifcmd_vr_scalarfn", 261,
     "local __v = 0\ncapture noisily {\n    if scalar(zneg)^2 > 3 {\n        local __v = 1\n    }\n}\nlocal __rc = _rc",
     "`__v'",
     "0|1"},
    {"disp_vr_b", 268,
     "local __rc = 0",
     "%21x (_b[u]^2)",
     "0|+1.0000000000002X+000"},
    {"scalar_vr_b", 277,
     "capture noisily scalar s_vr_b = _b[u]^2\nlocal __rc = _rc",
     "%21x s_vr_b",
     "0|+1.0000000000002X+000"},
    {"gen_vr_b", 286,
     "capture noisily generate double v_vr_b = _b[u]^2\nlocal __rc = _rc",
     "%21x v_vr_b[1]",
     "0|+1.0000000000002X+000"},
    {"count_vr_b", 296,
     "capture noisily count if _b[u]^2 > 3\nlocal __rc = _rc",
     "%21x r(N)",
     "0|+0.0000000000000X-3ff"},
    {"ifcmd_vr_b", 311,
     "local __v = 0\ncapture noisily {\n    if _b[u]^2 > 3 {\n        local __v = 1\n    }\n}\nlocal __rc = _rc",
     "`__v'",
     "0|0"},
    {"disp_vr_matrix", 318,
     "local __rc = 0",
     "%21x (MN[1,1]^2)",
     "0|+1.2000000000000X+003"},
    {"scalar_vr_matrix", 327,
     "capture noisily scalar s_vr_matrix = MN[1,1]^2\nlocal __rc = _rc",
     "%21x s_vr_matrix",
     "0|+1.2000000000000X+003"},
    {"gen_vr_matrix", 336,
     "capture noisily generate double v_vr_matrix = MN[1,1]^2\nlocal __rc = _rc",
     "%21x v_vr_matrix[1]",
     "0|+1.2000000000000X+003"},
    {"count_vr_matrix", 346,
     "capture noisily count if MN[1,1]^2 > 3\nlocal __rc = _rc",
     "%21x r(N)",
     "0|+1.b000000000000X+004"},
    {"ifcmd_vr_matrix", 361,
     "local __v = 0\ncapture noisily {\n    if MN[1,1]^2 > 3 {\n        local __v = 1\n    }\n}\nlocal __rc = _rc",
     "`__v'",
     "0|1"},
    {"disp_vr_neg_scalar", 368,
     "local __rc = 0",
     "%21x (-zneg^2)",
     "0|-1.0000000000000X+002"},
    {"scalar_vr_neg_scalar", 377,
     "capture noisily scalar s_vr_neg_scalar = -zneg^2\nlocal __rc = _rc",
     "%21x s_vr_neg_scalar",
     "0|-1.0000000000000X+002"},
    {"gen_vr_neg_scalar", 386,
     "capture noisily generate double v_vr_neg_scalar = -zneg^2\nlocal __rc = _rc",
     "%21x v_vr_neg_scalar[1]",
     "0|-1.0000000000000X+002"},
    {"count_vr_neg_scalar", 396,
     "capture noisily count if -zneg^2 > 3\nlocal __rc = _rc",
     "%21x r(N)",
     "0|+0.0000000000000X-3ff"},
    {"ifcmd_vr_neg_scalar", 411,
     "local __v = 0\ncapture noisily {\n    if -zneg^2 > 3 {\n        local __v = 1\n    }\n}\nlocal __rc = _rc",
     "`__v'",
     "0|0"},
    {"disp_vr_local", 418,
     "local __rc = 0",
     "%21x (`m'^2)",
     "0|-1.2000000000000X+003"},
    {"scalar_vr_local", 427,
     "capture noisily scalar s_vr_local = `m'^2\nlocal __rc = _rc",
     "%21x s_vr_local",
     "0|-1.2000000000000X+003"},
    {"gen_vr_local", 436,
     "capture noisily generate double v_vr_local = `m'^2\nlocal __rc = _rc",
     "%21x v_vr_local[1]",
     "0|-1.2000000000000X+003"},
    {"count_vr_local", 446,
     "capture noisily count if `m'^2 > 3\nlocal __rc = _rc",
     "%21x r(N)",
     "0|+0.0000000000000X-3ff"},
    {"ifcmd_vr_local", 461,
     "local __v = 0\ncapture noisily {\n    if `m'^2 > 3 {\n        local __v = 1\n    }\n}\nlocal __rc = _rc",
     "`__v'",
     "0|0"},
    {"disp_vr_rmean", 470,
     "quietly summarize yy\nlocal __rc = 0",
     "%21x (r(mean)^2)",
     "0|+1.83dce226a0d58X-001"},
    {"gen_vr_rmean", 479,
     "capture noisily generate double v_vr_rmean = r(mean)^2\nlocal __rc = _rc",
     "%21x v_vr_rmean[1]",
     "0|+1.83dce226a0d58X-001"},
    {"gen_not_neg_sp", 491,
     "capture noisily generate double g_not_neg_sp = ! -t\nlocal __rc = _rc",
     "%21x g_not_neg_sp[1]\t%21x g_not_neg_sp[10]\t%21x g_not_neg_sp[19]",
     "198"},
    {"gen_not_plus", 502,
     "capture noisily generate double g_not_plus = !+t\nlocal __rc = _rc",
     "%21x g_not_plus[1]\t%21x g_not_plus[10]\t%21x g_not_plus[19]",
     "198"},
    {"gen_not_paren_neg", 512,
     "capture noisily generate double g_not_paren_neg = !(-t)\nlocal __rc = _rc",
     "%21x g_not_paren_neg[1]\t%21x g_not_paren_neg[10]\t%21x g_not_paren_neg[19]",
     "0|+0.0000000000000X-3ff|+1.0000000000000X+000|+0.0000000000000X-3ff"},
    {"gen_tilde_neg", 522,
     "capture noisily generate double g_tilde_neg = ~-t\nlocal __rc = _rc",
     "%21x g_tilde_neg[1]\t%21x g_tilde_neg[10]\t%21x g_tilde_neg[19]",
     "198"},
    {"gen_not_not_neg", 533,
     "capture noisily generate double g_not_not_neg = !!-t\nlocal __rc = _rc",
     "%21x g_not_not_neg[1]\t%21x g_not_not_neg[10]\t%21x g_not_not_neg[19]",
     "198"},
    {"gen_neg_not_neg", 544,
     "capture noisily generate double g_neg_not_neg = -!-t\nlocal __rc = _rc",
     "%21x g_neg_not_neg[1]\t%21x g_neg_not_neg[10]\t%21x g_neg_not_neg[19]",
     "198"},
    {"gen_pow_not_neg", 555,
     "capture noisily generate double g_pow_not_neg = 2^!-t\nlocal __rc = _rc",
     "%21x g_pow_not_neg[1]\t%21x g_pow_not_neg[10]\t%21x g_pow_not_neg[19]",
     "198"},
    {"count_not_neg", 566,
     "capture noisily count if !-t\nlocal __rc = _rc",
     "%21x r(N)",
     "198|+1.b000000000000X+004"},
    {"count_not_neg_sp", 576,
     "capture noisily count if ! -t\nlocal __rc = _rc",
     "%21x r(N)",
     "198|+1.b000000000000X+004"},
    {"disp_not_neg", 586,
     "capture noisily display !-1\nlocal __rc = _rc",
     "",
     "198"},
    {"local_not_neg", 598,
     "local __l\ncapture noisily local __l = !-1\nlocal __rc = _rc",
     "@ \"|`__l'|\"",
     "198|"},
    {"scalar_not_neg", 610,
     "capture scalar drop s_not_neg\ncapture noisily scalar s_not_neg = !-1\nlocal __rc = _rc",
     "%21x scalar(s_not_neg)",
     "198"},
    {"disp_not_neg_sp", 621,
     "capture noisily display ! -1\nlocal __rc = _rc",
     "",
     "198"},
    {"local_not_neg_sp", 633,
     "local __l\ncapture noisily local __l = ! -1\nlocal __rc = _rc",
     "@ \"|`__l'|\"",
     "198|"},
    {"scalar_not_neg_sp", 645,
     "capture scalar drop s_not_neg_sp\ncapture noisily scalar s_not_neg_sp = ! -1\nlocal __rc = _rc",
     "%21x scalar(s_not_neg_sp)",
     "198"},
    {"disp_not_plus", 656,
     "capture noisily display !+1\nlocal __rc = _rc",
     "",
     "198"},
    {"local_not_plus", 668,
     "local __l\ncapture noisily local __l = !+1\nlocal __rc = _rc",
     "@ \"|`__l'|\"",
     "198|"},
    {"scalar_not_plus", 680,
     "capture scalar drop s_not_plus\ncapture noisily scalar s_not_plus = !+1\nlocal __rc = _rc",
     "%21x scalar(s_not_plus)",
     "198"},
    {"disp_not_paren_neg", 691,
     "capture noisily display !(-1)\nlocal __rc = _rc",
     "",
     "0"},
    {"local_not_paren_neg", 702,
     "local __l\ncapture noisily local __l = !(-1)\nlocal __rc = _rc",
     "@ \"|`__l'|\"",
     "0|0"},
    {"scalar_not_paren_neg", 713,
     "capture scalar drop s_not_paren_neg\ncapture noisily scalar s_not_paren_neg = !(-1)\nlocal __rc = _rc",
     "%21x scalar(s_not_paren_neg)",
     "0|+0.0000000000000X-3ff"},
    {"disp_tilde_neg", 723,
     "capture noisily display ~-1\nlocal __rc = _rc",
     "",
     "198"},
    {"local_tilde_neg", 735,
     "local __l\ncapture noisily local __l = ~-1\nlocal __rc = _rc",
     "@ \"|`__l'|\"",
     "198|"},
    {"scalar_tilde_neg", 747,
     "capture scalar drop s_tilde_neg\ncapture noisily scalar s_tilde_neg = ~-1\nlocal __rc = _rc",
     "%21x scalar(s_tilde_neg)",
     "198"},
    {"disp_tilde_ne", 758,
     "capture noisily display (1~=2)+(~0)\nlocal __rc = _rc",
     "",
     "0"},
    {"local_tilde_ne", 769,
     "local __l\ncapture noisily local __l = (1~=2)+(~0)\nlocal __rc = _rc",
     "@ \"|`__l'|\"",
     "0|2"},
    {"scalar_tilde_ne", 780,
     "capture scalar drop s_tilde_ne\ncapture noisily scalar s_tilde_ne = (1~=2)+(~0)\nlocal __rc = _rc",
     "%21x scalar(s_tilde_ne)",
     "0|+1.0000000000000X+001"},
    {"gen_vr_not_scalar", 791,
     "capture noisily generate double g_vr_not_scalar = !zneg\nlocal __rc = _rc",
     "%21x g_vr_not_scalar[1]\t%21x g_vr_not_scalar[10]\t%21x g_vr_not_scalar[19]",
     "0|+0.0000000000000X-3ff|+0.0000000000000X-3ff|+0.0000000000000X-3ff"},
    {"disp_vr_not_scalar", 801,
     "capture noisily display !zneg\nlocal __rc = _rc",
     "",
     "0"},
    {"local_vr_not_scalar", 812,
     "local __l\ncapture noisily local __l = !zneg\nlocal __rc = _rc",
     "@ \"|`__l'|\"",
     "0|0"},
    {"scalar_vr_not_scalar", 823,
     "capture scalar drop s_vr_not_scalar\ncapture noisily scalar s_vr_not_scalar = !zneg\nlocal __rc = _rc",
     "%21x scalar(s_vr_not_scalar)",
     "0|+0.0000000000000X-3ff"},
    {"gen_vr_neg_scalar1", 832,
     "capture noisily generate double g_vr_neg_scalar1 = -zneg\nlocal __rc = _rc",
     "%21x g_vr_neg_scalar1[1]\t%21x g_vr_neg_scalar1[10]\t%21x g_vr_neg_scalar1[19]",
     "0|+1.0000000000000X+001|+1.0000000000000X+001|+1.0000000000000X+001"},
    {"disp_vr_neg_scalar1", 842,
     "capture noisily display -zneg\nlocal __rc = _rc",
     "",
     "0"},
    {"local_vr_neg_scalar1", 853,
     "local __l\ncapture noisily local __l = -zneg\nlocal __rc = _rc",
     "@ \"|`__l'|\"",
     "0|2"},
    {"scalar_vr_neg_scalar1", 864,
     "capture scalar drop s_vr_neg_scalar1\ncapture noisily scalar s_vr_neg_scalar1 = -zneg\nlocal __rc = _rc",
     "%21x scalar(s_vr_neg_scalar1)",
     "0|+1.0000000000000X+001"},
    {"gen_vr_pow_scalar", 873,
     "capture noisily generate double g_vr_pow_scalar = 2^zneg\nlocal __rc = _rc",
     "%21x g_vr_pow_scalar[1]\t%21x g_vr_pow_scalar[10]\t%21x g_vr_pow_scalar[19]",
     "0|+1.0000000000000X-002|+1.0000000000000X-002|+1.0000000000000X-002"},
    {"disp_vr_pow_scalar", 883,
     "capture noisily display 2^zneg\nlocal __rc = _rc",
     "",
     "0"},
    {"local_vr_pow_scalar", 894,
     "local __l\ncapture noisily local __l = 2^zneg\nlocal __rc = _rc",
     "@ \"|`__l'|\"",
     "0|.25"},
    {"scalar_vr_pow_scalar", 905,
     "capture scalar drop s_vr_pow_scalar\ncapture noisily scalar s_vr_pow_scalar = 2^zneg\nlocal __rc = _rc",
     "%21x scalar(s_vr_pow_scalar)",
     "0|+1.0000000000000X-002"},
    {"gen_vr_not_matrix", 914,
     "capture noisily generate double g_vr_not_matrix = !MN[1,1]\nlocal __rc = _rc",
     "%21x g_vr_not_matrix[1]\t%21x g_vr_not_matrix[10]\t%21x g_vr_not_matrix[19]",
     "0|+0.0000000000000X-3ff|+0.0000000000000X-3ff|+0.0000000000000X-3ff"},
    {"disp_vr_not_matrix", 924,
     "capture noisily display !MN[1,1]\nlocal __rc = _rc",
     "",
     "0"},
    {"local_vr_not_matrix", 935,
     "local __l\ncapture noisily local __l = !MN[1,1]\nlocal __rc = _rc",
     "@ \"|`__l'|\"",
     "0|0"},
    {"scalar_vr_not_matrix", 946,
     "capture scalar drop s_vr_not_matrix\ncapture noisily scalar s_vr_not_matrix = !MN[1,1]\nlocal __rc = _rc",
     "%21x scalar(s_vr_not_matrix)",
     "0|+0.0000000000000X-3ff"},
    {"gen_vr_negneg_matrix", 955,
     "capture noisily generate double g_vr_negneg_matrix = -MN[1,1]^2\nlocal __rc = _rc",
     "%21x g_vr_negneg_matrix[1]\t%21x g_vr_negneg_matrix[10]\t%21x g_vr_negneg_matrix[19]",
     "0|-1.2000000000000X+003|-1.2000000000000X+003|-1.2000000000000X+003"},
    {"disp_vr_negneg_matrix", 965,
     "capture noisily display -MN[1,1]^2\nlocal __rc = _rc",
     "",
     "0"},
    {"local_vr_negneg_matrix", 976,
     "local __l\ncapture noisily local __l = -MN[1,1]^2\nlocal __rc = _rc",
     "@ \"|`__l'|\"",
     "0|-9"},
    {"scalar_vr_negneg_matrix", 987,
     "capture scalar drop s_vr_negneg_matrix\ncapture noisily scalar s_vr_negneg_matrix = -MN[1,1]^2\nlocal __rc = _rc",
     "%21x scalar(s_vr_negneg_matrix)",
     "0|-1.2000000000000X+003"},
    {"gen_vr_b_neg", 996,
     "capture noisily generate double g_vr_b_neg = -_b[u]^2\nlocal __rc = _rc",
     "%21x g_vr_b_neg[1]\t%21x g_vr_b_neg[10]\t%21x g_vr_b_neg[19]",
     "0|-1.0000000000002X+000|-1.0000000000002X+000|-1.0000000000002X+000"},
    {"disp_vr_b_neg", 1006,
     "capture noisily display -_b[u]^2\nlocal __rc = _rc",
     "",
     "0"},
    {"local_vr_b_neg", 1017,
     "local __l\ncapture noisily local __l = -_b[u]^2\nlocal __rc = _rc",
     "@ \"|`__l'|\"",
     "0|-1"},
    {"scalar_vr_b_neg", 1028,
     "capture scalar drop s_vr_b_neg\ncapture noisily scalar s_vr_b_neg = -_b[u]^2\nlocal __rc = _rc",
     "%21x scalar(s_vr_b_neg)",
     "0|-1.0000000000002X+000"},
    {"gen_vr_rmin", 1039,
     "quietly summarize yy\ncapture noisily generate double g_vr_rmin = r(min)^2\nlocal __rc = _rc",
     "%21x g_vr_rmin[1]\t%21x g_vr_rmin[10]\t%21x g_vr_rmin[19]",
     "0|+1.0000000000000X+002|+1.0000000000000X+002|+1.0000000000000X+002"},
    {"disp_vr_rmin", 1049,
     "capture noisily display r(min)^2\nlocal __rc = _rc",
     "",
     "0"},
    {"local_vr_rmin", 1060,
     "local __l\ncapture noisily local __l = r(min)^2\nlocal __rc = _rc",
     "@ \"|`__l'|\"",
     "0|4"},
    {"scalar_vr_rmin", 1071,
     "capture scalar drop s_vr_rmin\ncapture noisily scalar s_vr_rmin = r(min)^2\nlocal __rc = _rc",
     "%21x scalar(s_vr_rmin)",
     "0|+1.0000000000000X+002"},
    {"gen_vr_neg_rmin", 1080,
     "capture noisily generate double g_vr_neg_rmin = -r(min)^2\nlocal __rc = _rc",
     "%21x g_vr_neg_rmin[1]\t%21x g_vr_neg_rmin[10]\t%21x g_vr_neg_rmin[19]",
     "0|-1.0000000000000X+002|-1.0000000000000X+002|-1.0000000000000X+002"},
    {"disp_vr_neg_rmin", 1090,
     "capture noisily display -r(min)^2\nlocal __rc = _rc",
     "",
     "0"},
    {"local_vr_neg_rmin", 1101,
     "local __l\ncapture noisily local __l = -r(min)^2\nlocal __rc = _rc",
     "@ \"|`__l'|\"",
     "0|-4"},
    {"scalar_vr_neg_rmin", 1112,
     "capture scalar drop s_vr_neg_rmin\ncapture noisily scalar s_vr_neg_rmin = -r(min)^2\nlocal __rc = _rc",
     "%21x scalar(s_vr_neg_rmin)",
     "0|-1.0000000000000X+002"},
};

// ---- FU-854 / w2ac_expr_syntaxrc: the rc of a malformed data expression ------
// Oracle: internal capture
// w2ac_expr_syntaxrc.log (StataBE 19.5). A dangling operator,
// two adjacent operands, an empty (), an unclosed function call or subscript, a
// malformed number in generate/replace/count (fast path and `& _n > 0`)/keep/drop/
// assert/egen/regress if. Stata answers r(132) when the parentheses or brackets do
// not balance, r(111) for `max(a b)`, r(198) otherwise; the gen arms' `__cv' = 111
// (nothing created) and keep/drop's _N = 10 (data untouched). On main all 29 arms
// came back rc 1: coarseRc (eval/errors.cpp) had no rule for the engine Parser's
// "expr: <what> in '<expression>'". Table emitted by emit_selftest.py, which moves
// the keep/drop arms' `restore` after the PRECFAIL line (the record is the same).
const PrecArm kSynArms[] = {
    {"fx_a", 80,
     "version 19\nclear all\nset more off\nset linesize 255\nset obs 10\ngenerate double a = .\nreplace a = 3 in 1\nreplace a = -2 in 2\nreplace a = 0 in 3\nreplace a = 1.5 in 5\nreplace a = -0.5 in 6\nreplace a = 2 in 7\nreplace a = -3 in 8\nreplace a = 1 in 9\nreplace a = 0.25 in 10\ngenerate double x = _n\ngenerate double y = 5 - 0.5*_n\nlocal __rc = 0",
     "#a",
     "0|+1.8000000000000X+001|-1.0000000000000X+001|+0.0000000000000X-3ff|+1.0000000000000X+3ff|+1.8000000000000X+000|-1.0000000000000X-001|+1.0000000000000X+001|-1.8000000000000X+001|+1.0000000000000X+000|+1.0000000000000X-002"},
    {"gen_trail_plus", 95,
     "capture noisily generate double g01 = a +\nlocal __rc = _rc\ncapture confirm variable g01\nlocal __cv = _rc",
     "`__cv'",
     "198|111"},
    {"gen_trail_star", 110,
     "capture noisily generate double g02 = a *\nlocal __rc = _rc\ncapture confirm variable g02\nlocal __cv = _rc",
     "`__cv'",
     "198|111"},
    {"gen_trail_gt", 125,
     "capture noisily generate double g03 = a >\nlocal __rc = _rc\ncapture confirm variable g03\nlocal __cv = _rc",
     "`__cv'",
     "198|111"},
    {"gen_trail_and", 140,
     "capture noisily generate double g04 = a &\nlocal __rc = _rc\ncapture confirm variable g04\nlocal __cv = _rc",
     "`__cv'",
     "198|111"},
    {"gen_lead_star", 155,
     "capture noisily generate double g05 = * a\nlocal __rc = _rc\ncapture confirm variable g05\nlocal __cv = _rc",
     "`__cv'",
     "198|111"},
    {"gen_two_ops", 170,
     "capture noisily generate double g06 = a + * b\nlocal __rc = _rc\ncapture confirm variable g06\nlocal __cv = _rc",
     "`__cv'",
     "198|111"},
    {"gen_juxta", 185,
     "capture noisily generate double g07 = a b\nlocal __rc = _rc\ncapture confirm variable g07\nlocal __cv = _rc",
     "`__cv'",
     "198|111"},
    {"gen_juxta_num", 200,
     "capture noisily generate double g08 = a 2\nlocal __rc = _rc\ncapture confirm variable g08\nlocal __cv = _rc",
     "`__cv'",
     "198|111"},
    {"gen_open_paren", 215,
     "capture noisily generate double g09 = (a + 1\nlocal __rc = _rc\ncapture confirm variable g09\nlocal __cv = _rc",
     "`__cv'",
     "132|111"},
    {"gen_close_paren", 230,
     "capture noisily generate double g10 = a + 1)\nlocal __rc = _rc\ncapture confirm variable g10\nlocal __cv = _rc",
     "`__cv'",
     "132|111"},
    {"gen_empty_paren", 245,
     "capture noisily generate double g11 = ()\nlocal __rc = _rc\ncapture confirm variable g11\nlocal __cv = _rc",
     "`__cv'",
     "198|111"},
    {"gen_fn_open", 260,
     "capture noisily generate double g12 = abs(a\nlocal __rc = _rc\ncapture confirm variable g12\nlocal __cv = _rc",
     "`__cv'",
     "132|111"},
    {"gen_fn_nosep", 275,
     "capture noisily generate double g13 = max(a b)\nlocal __rc = _rc\ncapture confirm variable g13\nlocal __cv = _rc",
     "`__cv'",
     "111|111"},
    {"gen_subscr_open", 290,
     "capture noisily generate double g14 = a[1\nlocal __rc = _rc\ncapture confirm variable g14\nlocal __cv = _rc",
     "`__cv'",
     "132|111"},
    {"gen_badnum", 305,
     "capture noisily generate double g15 = 1.2.3\nlocal __rc = _rc\ncapture confirm variable g15\nlocal __cv = _rc",
     "`__cv'",
     "198|111"},
    {"gen_trail_pow", 320,
     "capture noisily generate double g16 = a^\nlocal __rc = _rc\ncapture confirm variable g16\nlocal __cv = _rc",
     "`__cv'",
     "198|111"},
    {"rep_trail_minus", 331,
     "capture noisily replace a = a -\nlocal __rc = _rc",
     "",
     "198"},
    {"rep_open_paren", 342,
     "capture noisily replace a = (a\nlocal __rc = _rc",
     "",
     "132"},
    {"count_trail_plus", 353,
     "capture noisily count if a +\nlocal __rc = _rc",
     "",
     "198"},
    {"count_trail_gt", 364,
     "capture noisily count if a >\nlocal __rc = _rc",
     "",
     "198"},
    {"count_juxta", 375,
     "capture noisily count if a b\nlocal __rc = _rc",
     "",
     "198"},
    {"count_open_paren", 386,
     "capture noisily count if (a > 0\nlocal __rc = _rc",
     "",
     "132"},
    {"count_close_paren", 397,
     "capture noisily count if a > 0)\nlocal __rc = _rc",
     "",
     "132"},
    {"count_eng_trail", 408,
     "capture noisily count if a + & _n > 0\nlocal __rc = _rc",
     "",
     "198"},
    {"keep_trail_or", 421,
     "preserve\ncapture noisily keep if a |\nlocal __rc = _rc",
     "%21x _N",
     "198|+1.4000000000000X+003"},
    {"drop_open_paren", 436,
     "restore\npreserve\ncapture noisily drop if (a < 0\nlocal __rc = _rc",
     "%21x _N",
     "132|+1.4000000000000X+003"},
    {"assert_trail", 449,
     "restore\ncapture noisily assert a +\nlocal __rc = _rc",
     "",
     "198"},
    {"egen_trail", 464,
     "capture noisily egen double e01 = mean(a +)\nlocal __rc = _rc\ncapture confirm variable e01\nlocal __cv = _rc",
     "`__cv'",
     "198|111"},
    {"reg_if_trail", 475,
     "capture noisily regress y x if a +\nlocal __rc = _rc",
     "",
     "198"},
};

std::vector<std::string> precSplit(const std::string& s, char sep) {
  std::vector<std::string> out;
  size_t a = 0;
  for (size_t b; (b = s.find(sep, a)) != std::string::npos; a = b + 1) out.push_back(s.substr(a, b - a));
  out.push_back(s.substr(a));
  return out;
}

// A %21x field (the X exponent is hex) or a plain decimal; false if neither.
bool precNumber(const std::string& t, double& out) {
  if (t.size() == 21 && (t[0] == '+' || t[0] == '-') && t[2] == '.' && t[16] == 'X') {
    char* end = nullptr;
    const double mant = std::strtod(("0x" + t.substr(1, 15)).c_str(), &end);
    const long ex = std::strtol(t.substr(18, 3).c_str(), nullptr, 16);
    out = (t[0] == '-' ? -1.0 : 1.0) * std::ldexp(mant, t[17] == '-' ? -ex : ex);
    return true;
  }
  if (t.empty()) return false;
  char* end = nullptr;
  out = std::strtod(t.c_str(), &end);
  return end != nullptr && *end == '\0' && std::isfinite(out);
}

// The eval grader's rule (tools/autonomy/scoreboard.py grade()): the rc field is
// text; a value field matches as text or within rtol 1e-9 of max(1, |x|, |y|).
bool precFieldsMatch(const std::vector<std::string>& want, const std::vector<std::string>& got) {
  if (want.size() != got.size() || want.empty() || want[0] != got[0]) return false;
  for (size_t i = 1; i < want.size(); ++i) {
    if (want[i] == got[i]) continue;
    double x = 0, y = 0;
    if (!precNumber(want[i], x) || !precNumber(got[i], y)) return false;
    if (std::fabs(x - y) > 1e-9 * std::max({1.0, std::fabs(x), std::fabs(y)})) return false;
  }
  return true;
}

std::string precDoSource(const PrecArm* arms, size_t n) {
  std::string src;
  for (const PrecArm* pa = arms; pa != arms + n; ++pa) {
    const PrecArm& a = *pa;
    if (*a.pre) src += std::string(a.pre) + "\n";
    std::vector<std::string> f;
    if (a.fields[0] == '@') {
      src += std::string("capture noisily display \"PREC|") + a.arm + "|\" `__rc'" + (a.fields + 1);
      src += "\nif _rc != 0 display \"PRECFAIL|" + std::string(a.arm) + "|rc=`__rc'|display_rc=\" _rc \"|\"\n";
      continue;
    }
    if (a.fields[0] == '#') {
      for (int i = 1; i <= 10; ++i) f.push_back("%21x " + std::string(a.fields + 1) + "[" + std::to_string(i) + "]");
    } else if (*a.fields) {
      f = precSplit(a.fields, '\t');
    }
    src += std::string("capture noisily display \"PREC|") + a.arm + "|\" `__rc' \"|\"";
    for (const auto& x : f) src += " " + x + " \"|\"";
    src += "\nif _rc != 0 display \"PRECFAIL|" + std::string(a.arm) + "|rc=`__rc'|display_rc=\" _rc \"|\"\n";
  }
  return src;
}

void precedenceArms(const std::string& workdir, const PrecArm* arms, size_t n, const char* logName) {
  piet::SessionState session(workdir);
  std::map<std::string, std::vector<std::string>> got;
  try {
    const auto res = piet::doexec::executeDoSource(precDoSource(arms, n), session, piet::dispatch);
    for (const auto& b : res.blocks) {
      if (!b.is_object() || b.value("type", std::string()) != "display") continue;
      std::string t = b.value("text", std::string());
      while (!t.empty() && (t.back() == '\n' || t.back() == ' ')) t.pop_back();
      // A record whose display failed part-way (`%21x scalar(nosuch)`) ends in the
      // error text, not `|`; as in the log, the fields stop at the last `|`.
      const size_t last = t.rfind('|');
      if (t.rfind("PREC|", 0) != 0 || last == std::string::npos || last < 5) continue;
      std::vector<std::string> f = precSplit(t.substr(5, last - 5), '|');
      const std::string arm = f.front();
      f.erase(f.begin());
      got.emplace(arm, f);
    }
  } catch (const std::exception& e) {
    bkCheck(false, std::string("W2 precedence: the do-source threw: ") + e.what());
    return;
  }
  for (const PrecArm* pa = arms; pa != arms + n; ++pa) {
    const PrecArm& a = *pa;
    const auto it = got.find(a.arm);
    const std::string have = it == got.end() ? std::string("(no record)") : [&] {
      std::string s;
      for (const auto& x : it->second) s += (s.empty() ? "" : "|") + x;
      return s;
    }();
    bkCheck(it != got.end() && precFieldsMatch(precSplit(a.want, '|'), it->second),
            std::string("W2 precedence (") + logName + ".log:" + std::to_string(a.line) + " PREC|" +
                a.arm + "|): want " + a.want + " got " + have);
  }
}

}  // namespace

void testW2ByKeep() {
  std::printf("[selftest] --- W2 bykeep: by/bysort: keep if / drop if (w2_bykeep.log) ---\n");
  namespace fs = std::filesystem;
  const std::string workdir = (fs::temp_directory_path() / "piet_native_selftest_w2bykeep").u8string();
  std::error_code ec;
  fs::remove_all(workdir, ec);
  fs::create_directories(workdir, ec);

  // Every row is a PREC line of w2_bykeep.log, values copied verbatim.
  static const Arm kArms[] = {
      // BUG-533: the control that discriminates the global complement. main: N=9.
      {"ctl_drop_misstrue", "sort g b", "drop if z[_n-1]", "0", "8", "90", "43", "190"},
      {"ctl_keep_misstrue", "sort g b", "keep if z[_n-1]", "0", "1", "0", "12", "12"},
      {"ctl_drop_ngt1", "sort g b", "drop if _n>1", "0", "1", "0", "12", "12"},
      {"ctl_keep_n1", "sort g b", "keep if _n==1", "0", "1", "0", "12", "12"},
      {"ctl_keep_nN", "sort g b", "keep if _n==_N", "0", "1", "3", "9", "9"},
      {"ctl_keep_Ngt1", "sort g b", "keep if _N>1", "0", "9", "107", "55", "245"},
      // BUG-532: bysort with a unique (b) key -- every column comparable.
      {"bys_p_n1", "sort x", "bysort g (b): keep if _n==1", "0", "4", "20", "30", "68"},
      {"bys_p_nN", "sort x", "bysort g (b): keep if _n==_N", "0", "4", "20", "30", "68"},
      {"bys_p_Ngt1", "sort x", "bysort g (b): keep if _N>1", "0", "8", "90", "43", "190"},
      {"bys_p_Neq3", "sort x", "bysort g (b): keep if _N==3", "0", "6", "57", "33", "107"},
      {"bys_p_Nge2", "sort x", "bysort g (b): keep if _N >= 2", "0", "8", "90", "43", "190"},
      {"bys_p_xeqx1", "sort x", "bysort g (b): keep if x==x[1]", "0", "4", "20", "30", "68"},
      {"bys_p_xeqxN", "sort x", "bysort g (b): keep if x==x[_N]", "0", "4", "20", "30", "68"},
      {"bys_p_inrange", "sort x", "bysort g (b): keep if inrange(_n,2,3)", "0", "5", "38", "25",
       "74"},
      {"bys_p_lag", "sort x", "bysort g (b): keep if x>x[_n-1]", "0", "2", "7", "16", "25"},
      {"bys_p_runsum", "sort x", "bysort g (b): keep if sum(x)>6", "0", "6", "44", "46", "144"},
      {"bys_p_or", "sort x", "bysort g (b): keep if _n==_N | x>=9", "0", "5", "31", "40", "109"},
      {"bys_p_misstrue", "sort x", "bysort g (b): keep if z[_n-1]", "0", "4", "20", "30", "68"},
      {"bys_p_two", "sort x", "bysort g h (b): keep if _n==1", "0", "4", "20", "30", "68"},
      {"bys_p_twokeys", "sort x", "bysort g (h b): keep if _n==_N", "0", "4", "20", "30", "68"},
      {"bys_p_var", "bysort g: generate nobs = _N", "bysort g (b): keep if _n == nobs", "0", "4",
       "20", "30", "68"},
      {"bys_abbrev", "sort x", "bys g (b): keep if _n==1", "0", "4", "20", "30", "68"},
      {"bys_quiet", "sort x", "quietly bysort g (b): keep if _n==1", "0", "4", "20", "30", "68"},
      // No within-group key: only rc, _N, sum(g*_n) are comparable (oracle header).
      {"bys_n1_ties", "sort x", "bysort g: keep if _n==1", "0", "4", "20", nullptr, nullptr},
      {"bys_Neq3_ties", "sort x", "bysort g: keep if _N==3", "0", "6", "57", nullptr, nullptr},
      {"bys_presorted", "sort g b", "bysort g: keep if _n==1", "0", "4", "20", "30", "68"},
      // Plain by: sortedness required (r(5), data untouched), no re-sort.
      {"by_unsorted", "sort x", "by g: keep if _n==1", "5", "9", "75", "55", "357"},
      {"by_sorted_n1", "sort g b", "by g: keep if _n==1", "0", "4", "20", "30", "68"},
      {"by_sorted_nN", "sort g b", "by g: keep if _n==_N", "0", "4", "20", "30", "68"},
      {"by_sorted_Neq3", "sort g x", "by g: keep if _N == 3", "0", "6", "57", "33", "127"},
      {"by_p_sorted", "sort g b", "by g (b): keep if _n==_N", "0", "4", "20", "30", "68"},
      {"by_p_wrongkey", "sort g x", "by g (b): keep if _n==_N", "5", "9", "107", "55", "265"},
      {"by_two_sorted", "sort g h b", "by g h: keep if _n==1", "0", "4", "20", "30", "68"},
      // After `sort g b` the rows are grouped by (g h) too, but Stata's sortedby
      // marker is (g b), which (g h) does not lead: r(5). PIET reads the engine
      // file's own claim (BUG-272). Previous round: rc 0, _N 4.
      {"by_two_halfsorted", "sort g b", "by g h: keep if _n==1", "5", "9", "107", "55", "245"},
      // `bysort g (b): generate` leaves Stata's data sorted by g b, and PIET's
      // now too (runBysortGen sorts in place), so the plain `by` reads the
      // engine claim g b and runs. Before BUG-532: r(5), _N 9.
      {"by_p_after_bysgen", "bysort g (b): generate nn = _N", "by g (b): keep if _n==_N", "0",
       "4", "20", "30", "68"},
      // A STRING by-key: Stata sorts s by its bytes (Hi before lo), the engine's
      // code order is first-seen (lo before Hi). Previous round: sum(g*_n) 4,
      // sum(x*_n) 16 -- the right rows in the wrong order.
      {"bys_p_str", "sort x", "bysort s (b): keep if _n==_N", "0", "2", "5", "9", "11"},
      {"by_sortopt", "sort x", "by g (b), sort: keep if _n==1", "0", "4", "20", "30", "68"},
      // The drop sibling: complement is mask==0, so a missing mask drops.
      {"bys_p_drop_ngt1", "sort x", "bysort g (b): drop if _n>1", "0", "4", "20", "30", "68"},
      {"bys_p_drop_Ngt1", "sort x", "bysort g (b): drop if _N>1", "0", "1", "0", "12", "12"},
      {"bys_p_drop_dup", "sort x", "bysort g (b): drop if h==h[_n-1]", "0", "4", "20", "30", "68"},
      {"bys_p_drop_miss", "sort x", "bysort g (b): drop if z[_n-1]", "0", "5", "38", "25", "74"},
      {"bys_p_drop_lastN", "sort x", "bysort g (b): drop if _n==_N & _N>1", "0", "6", "49", "37",
       "103"},
      {"by_drop_unsorted", "sort x", "by g: drop if _n==1", "5", "9", "75", "55", "357"},
      {"by_drop_sorted", "sort g b", "by g: drop if _n==1", "0", "5", "38", "25", "74"},
      // Refused forms: Stata's rc, data untouched.
      {"by_keep_in", "sort g b", "by g: keep in 1/2", "190", "9", "107", "55", "245"},
      {"by_keep_if_in", "sort g b", "by g: keep if _n==1 in 1/5", "190", "9", "107", "55", "245"},
      {"by_keep_varlist", "sort g b", "by g: keep g x b h z s", "198", "9", "107", "55", "245"},
      {"by_drop_varlist", "sort g b", "by g: drop h", "198", "9", "107", "55", "245"},
      {"bys_nosuchvar", "sort x", "bysort nosuch: keep if _n==1", "111", "9", "75", "55", "357"},
      {"bys_p_nosuchkey", "sort x", "bysort g (nosuch): keep if _n==1", "111", "9", "75", "55",
       "357"},
  };

  for (const Arm& a : kArms) {
    const Rec r = armOf(workdir, a.pre, a.cmd);
    const std::string got = "rc " + r.rc + " _N " + r.n + " sum(g*_n) " + r.sg + " sum(x) " +
                            r.sx + " sum(x*_n) " + r.shx;
    bool ok = r.rc == a.rc && r.n == a.n && r.sg == a.sg;
    if (a.sx != nullptr) ok = ok && r.sx == a.sx;
    if (a.shx != nullptr) ok = ok && r.shx == a.shx;
    bkCheck(ok, std::string("W2 bykeep: `") + a.cmd + "` after `" + a.pre +
                    "` matches w2_bykeep.log PREC|" + a.tag + "| (rc " + a.rc + " _N " + a.n +
                    " sum(g*_n) " + a.sg + (a.sx ? std::string(" sum(x) ") + a.sx : "") +
                    (a.shx ? std::string(" sum(x*_n) ") + a.shx : "") + "). Got " + got);
    const std::string tag = a.tag;
    if (tag == "bys_p_str" || tag == "by_unsorted" || tag == "by_drop_unsorted" ||
        tag == "bys_nosuchvar" || tag == "bys_p_nosuchkey") {
      bkCheck(r.rcby == "5", "W2 bykeep rcby: " + tag +
                  " later plain by g:generate is r(5) (w2_bykeep.log:203/246/337/374/380). Got " +
                  r.rcby);
    }
  }

  // w2c_functions.log:1817: bysort avoids sorting an already sorted key
  // prefix; the more-specific g b marker survives the non-key generate.
  {
    piet::SessionState session(workdir);
    for (const char* cmd : {"clear", "set obs 4", "generate g = ceil(_n/2)",
                            "generate b = cond(mod(_n,2)==1,2,1)", "sort g b",
                            "bysort g: generate n1 = _n"})
      (void)rcOf(session, cmd);
    const std::string rc = rcOf(session, "by g (b): keep if _n==1");
    bkCheck(rc == "0" && shown(session, "_N") == "2",
            "W2 sort marker: bysort g:generate retains sorted-by g b (w2c_functions:1817)");
  }

  // w2c_functions.log:1626/1635/1678/1688: direct sort/gsort use level
  // bytes and extended missing kinds, independent of dictionary insertion order.
  {
    piet::SessionState session(workdir);
    for (const char* cmd : {"clear", "set obs 6", "generate str2 s = \"\"",
                            "replace s = \"lo\" in 1", "replace s = \"Hi\" in 2",
                            "replace s = \"a\" in 4", "replace s = \"A\" in 5",
                            "replace s = \"b\" in 6", "sort s"})
      (void)rcOf(session, cmd);
    bkCheck(valuesOf(session, "s", 6) == "|A|Hi|a|b|lo",
            "W2 sort strings: ascending lexical order (w2c_functions:1626)");
    (void)rcOf(session, "gsort -s");
    bkCheck(valuesOf(session, "s", 6) == "lo|b|a|Hi|A|",
            "W2 gsort strings: descending lexical order (w2c_functions:1635)");
    for (const char* cmd : {"clear", "set obs 7", "generate v = .", "replace v = 3 in 1",
                            "replace v = .a in 3", "replace v = 1 in 4", "replace v = .z in 5",
                            "replace v = .b in 6", "replace v = 2 in 7", "sort v"})
      (void)rcOf(session, cmd);
    bkCheck(valuesOf(session, "v", 7) == "1|2|3|.|.a|.b|.z",
            "W2 sort missing kinds: . then .a to .z (w2c_functions:1678)");
    (void)rcOf(session, "gsort -v");
    bkCheck(valuesOf(session, "v", 7) == "3|2|1|.z|.b|.a|.",
            "W2 gsort missing kinds: reversed classes after finite values (w2c_functions:1688)");
  }

  // PCELL|bys_p_nN|: the surviving rows, row by row -- and the data stays
  // physically sorted by g b after `bysort` (Stata's in-place sort).
  {
    piet::SessionState session(workdir);
    mkfix(session);
    const std::string rc = rcOf(session, "bysort g (b): keep if _n==_N");
    static const char* kCells[][3] = {{"0", "0", "12"}, {"1", "11", "7"}, {"2", "10", "2"},
                                      {"3", "9", "9"}};
    bool ok = rc == "0" && shown(session, "_N") == "4";
    std::string got;
    for (int i = 0; i < 4; ++i) {
      const std::string k = std::to_string(i + 1);
      const std::string g = shown(session, "g[" + k + "]"), b = shown(session, "b[" + k + "]"),
                        x = shown(session, "x[" + k + "]");
      got += " (" + g + "," + b + "," + x + ")";
      ok = ok && g == kCells[i][0] && b == kCells[i][1] && x == kCells[i][2];
    }
    bkCheck(ok, "W2 bykeep: PCELL|bys_p_nN| rows (g,b,x) = (0,0,12) (1,11,7) (2,10,2) (3,9,9) in "
                "that order. Got rc " + rc + got);
  }

  // PCELL|bys_p_str_n1|: a STRING by-key, row by row -- the groups come out in
  // Stata's (byte) order, Hi before lo, though lo is seen first.
  {
    piet::SessionState session(workdir);
    mkfix(session);
    const std::string rc = rcOf(session, "bysort s (b): keep if _n==1");
    static const char* kCells[][3] = {{"Hi", "0", "12"}, {"lo", "1", "5"}};
    bool ok = rc == "0" && shown(session, "_N") == "2";
    std::string got;
    for (int i = 0; i < 2; ++i) {
      const std::string k = std::to_string(i + 1);
      const std::string sv = shown(session, "s[" + k + "]"), b = shown(session, "b[" + k + "]"),
                        x = shown(session, "x[" + k + "]");
      got += " (" + sv + "," + b + "," + x + ")";
      ok = ok && sv == kCells[i][0] && b == kCells[i][1] && x == kCells[i][2];
    }
    bkCheck(ok, "W2 bykeep: PCELL|bys_p_str_n1| rows (s,b,x) = (Hi,0,12) (lo,1,5) in that "
                "order. Got rc " + rc + got);
  }

  // w2_smalls.log section B: global drop/keep with MISSING x (missing is +inf,
  // and true). drop_gt / drop_not_gt run through the rewritten complement.
  {
    struct SmallArm {
      const char* tag;
      const char* cmd;
      const char* n;
    };
    static const SmallArm kSmall[] = {{"drop_gt", "drop if x > 5", "4"},
                                      {"keep_lt", "keep if x < 5", "3"},
                                      {"drop_not_gt", "drop if !(x > 5)", "8"},
                                      {"keep_truthy", "keep if x", "12"}};
    for (const SmallArm& a : kSmall) {
      piet::SessionState session(workdir);
      for (const char* s0 : {"clear", "set obs 12", "generate x = mod(_n*7,11)",
                             "replace x = . in 5", "replace x = . in 11"}) {
        (void)rcOf(session, s0);
      }
      const std::string rc = rcOf(session, a.cmd);
      const std::string n = shown(session, "_N");
      bkCheck(rc == "0" && n == a.n, std::string("W2 bykeep: `") + a.cmd +
                                         "` leaves _N=" + a.n + " (w2_smalls.log PREC|" + a.tag +
                                         "|). Got rc " + rc + " _N " + n);
    }
  }

  // BUG-534: a missing condition is TRUE under `in` too (w2_smalls.log
  // keep_truthy, composed with `in`). Fixture x = . 0 3 . ; every row in
  // range. main: count if x in 1/4 -> 1, keep if x in 1/4 -> 1, drop if x in
  // 1/4 -> 3 (the engine's `&` read the missing x as false).
  {
    struct InArm {
      const char* cmd;
      const char* want;  // _N after a mutation, the displayed count for `count`
      bool isCount;
    };
    static const InArm kIn[] = {
        {"count if x in 1/4", "3", true},
        {"keep if x in 1/4", "3", false},
        {"drop if x in 1/4", "1", false},
    };
    for (const InArm& a : kIn) {
      piet::SessionState session(workdir);
      for (const char* s0 : {"clear", "set obs 4", "generate g = 1", "generate x = .",
                             "replace x = 0 in 2", "replace x = 3 in 3"}) {
        (void)rcOf(session, s0);
      }
      std::string got, rc;
      if (a.isCount) {
        try {
          rc = "0";
          for (const auto& b : piet::dispatch(a.cmd, session)) {
            if (!b.is_object()) continue;
            if (b.value("type", std::string()) == "error") rc = piet::eval::coarseRc(b);
            if (b.value("type", std::string()) == "display") got += b.value("text", std::string());
          }
        } catch (const std::exception& e) {
          rc = std::string("EXC:") + e.what();
        }
      } else {
        rc = rcOf(session, a.cmd);
        got = shown(session, "_N");
      }
      bkCheck(rc == "0" && got == a.want,
              std::string("W2 bykeep: `") + a.cmd + "` over x = . 0 3 . gives " + a.want +
                  " (a missing condition is true under `in`, w2_smalls.log keep_truthy). Got rc " +
                  rc + " -> " + got);
    }
  }

  // A STRING key in the parentheses picks the row: Stata orders it by its
  // bytes, the engine by first appearance. `bysort id (name): keep if _n==1`
  // over names b a c keeps `a` (x 2) and leaves the data in name order; code
  // order would keep `b`. The ordering rule is the one PREC|bys_p_str| and
  // PCELL|bys_p_str_n1| pin for a string BY-key (no StataBE arm has a string
  // in the parentheses). Previous round: refused r(199), data untouched.
  {
    auto strFix = [&](piet::SessionState& session, const char* n1, const char* n2,
                      const char* n3) {
      for (const std::string s0 :
           {std::string("clear"), std::string("set obs 3"), std::string("generate id = 1"),
            std::string("generate x = _n"), std::string("generate str1 name = \"") + n1 + "\"",
            std::string("replace name = \"") + n2 + "\" in 2",
            std::string("replace name = \"") + n3 + "\" in 3"}) {
        (void)rcOf(session, s0);
      }
    };
    {
      piet::SessionState session(workdir);
      strFix(session, "b", "a", "c");
      const std::string rc = rcOf(session, "bysort id (name): keep if _n==1");
      const std::string n = shown(session, "_N");
      const std::string x = shown(session, "x[1]");
      bkCheck(rc == "0" && n == "1" && x == "2",
              "W2 bykeep: `bysort id (name): keep if _n==1` over first-seen names b a c keeps "
              "name a (x 2), as Stata's byte order does. Got rc " + rc + " _N " + n + " x[1] " + x);
    }
    {
      piet::SessionState session(workdir);
      strFix(session, "b", "a", "c");
      const std::string rc = rcOf(session, "bysort id (name): drop if _n==_N");
      const std::string n = shown(session, "_N");
      const std::string x1 = shown(session, "x[1]"), x2 = shown(session, "x[2]");
      bkCheck(rc == "0" && n == "2" && x1 == "2" && x2 == "1",
              "W2 bykeep: `bysort id (name): drop if _n==_N` over names b a c drops c and leaves "
              "a (x 2), b (x 1) in that order. Got rc " + rc + " _N " + n + " x " + x1 + "," + x2);
    }
    {
      piet::SessionState session(workdir);
      strFix(session, "a", "b", "c");
      (void)rcOf(session, "replace x = 4 - x");
      const std::string rc = rcOf(session, "bysort id (name): keep if _n==_N");
      const std::string n = shown(session, "_N");
      const std::string x = shown(session, "x[1]");
      bkCheck(rc == "0" && n == "1" && x == "1",
              "W2 bykeep: `bysort id (name): keep if _n==_N` over byte-ordered names a b c runs "
              "and keeps name c (x 1). Got rc " + rc + " _N " + n + " x[1] " + x);
    }
  }

  // BUG-532: a string compared with the blank literal "". A blank read from
  // CSV or .dta is dictionary code -1 with no "" level, and the engine folds
  // `s==""` to a constant 0 then (`s!=""` to 1). Stata: blank == "" is true
  // (the dta_reader note: `count if grp == ""` -> 1). Fixture (CSV, file order
  // grouped by g): g 1 1 2 2 2 3, s a . . b . c (blank = .), x 1..6. Previous
  // round: `bysort g: drop if s==""` dropped nothing (_N 6).
  {
    const std::string csv = workdir + "/bk_blank.csv";
    {
      std::ofstream out(csv, std::ios::binary);
      out << "g,s,x\n1,a,1\n1,,2\n2,,3\n2,b,4\n2,,5\n3,c,6\n";
    }
    auto load = [&](piet::SessionState& session) {
      return rcOf(session, "import delimited using \"" + csv + "\", clear");
    };
    {
      piet::SessionState session(workdir);
      const std::string rc = load(session);
      bool minusOne = false;
      try {
        piet::Datastore* ds = session.datastore();
        if (ds != nullptr && ds->has("s") && ds->dtype("s") != "f64") {
          const int32_t* codes = ds->col("s").codes();
          for (int64_t r = 0; r < ds->nRows(); ++r) minusOne = minusOne || codes[r] < 0;
        }
      } catch (const std::exception&) {
      }
      std::string cnt;
      try {
        for (const auto& b : piet::dispatch("count if s==\"\"", session))
          if (b.is_object() && b.value("type", std::string()) == "display")
            cnt += b.value("text", std::string());
      } catch (const std::exception&) {
      }
      bkCheck(rc == "0" && minusOne && cnt == "3",
              "W2 bykeep: fixture -- the CSV's blank s cells load as code -1 and the global "
              "`count if s==\"\"` (strmutate path) counts 3. Got rc " + rc + " code-1 " +
                  (minusOne ? "yes" : "no") + " count " + cnt);
    }
    struct BlankArm {
      const char* pre;  // nullptr = none
      const char* cmd;
      const char* rc;
      const char* n;
      const char* sx;
    };
    // The plain `by` arm sorts first: a freshly imported file carries no
    // sortedby marker, and Stata answers a plain `by` there r(5) even though
    // these rows happen to be grouped (by_two_halfsorted: `by` reads the
    // marker, not the rows). The arm is about the blank compare.
    static const BlankArm kBlank[] = {
        {nullptr, "bysort g: drop if s==\"\"", "0", "3", "11"},
        {"sort g", "by g: keep if s!=\"\"", "0", "3", "11"},
        {nullptr, "bysort g: keep if s==\"\" | _n==1", "0", "5", "17"},
        {nullptr, "bysort g: drop if \"\"==s & _n>1", "0", "4", "14"},
        // Not a whole operand (a subscripted string): refused by name, data
        // untouched. (`s=="" == 1` no longer reaches this branch: the BUG-456
        // constant-compare fold in substituteRowExprRefs answers it r(109)
        // first.)
        {nullptr, "bysort g: keep if s[_n]==\"\"", "199", "6", "21"},
    };
    for (const BlankArm& a : kBlank) {
      piet::SessionState session(workdir);
      (void)load(session);
      if (a.pre != nullptr) (void)rcOf(session, a.pre);
      const std::string rc = rcOf(session, a.cmd);
      (void)rcOf(session, "generate double qx = sum(x)");
      const std::string n = shown(session, "_N"), sx = shown(session, "qx[_N]");
      bkCheck(rc == a.rc && n == a.n && sx == a.sx,
              std::string("W2 bykeep: `") + a.cmd + "` over s = a . . b . c (blank = code -1) -> rc " +
                  a.rc + " _N " + a.n + " sum(x) " + a.sx + ". Got rc " + rc + " _N " + n +
                  " sum(x) " + sx);
    }
  }

  // A draw under `by:` follows `set seed` and advances the session's stream,
  // as `bysort: generate` does (runBysortGen's injectRngContextForExpr). No
  // StataBE arm (a draw's values are this engine's own stream, not Stata's);
  // the checks are reproducibility and the stream advancing. Previous round:
  // no RNG context on the call, the stream did not move.
  {
    struct Draw {
      std::string rc, n, xs;
      long long streamBefore = 0, streamAfter = 0;
    };
    auto drawRun = [&]() {
      piet::SessionState session(workdir);
      mkfix(session);
      (void)rcOf(session, "set seed 20260925");
      Draw d;
      d.streamBefore = session.rngStream;
      d.rc = rcOf(session, "bysort g (b): keep if runiform() < .5");
      d.streamAfter = session.rngStream;
      d.n = shown(session, "_N");
      int n = 0;
      try {
        n = std::stoi(d.n);
      } catch (const std::exception&) {
      }
      for (int i = 1; i <= n; ++i) d.xs += shown(session, "x[" + std::to_string(i) + "]") + " ";
      return d;
    };
    const Draw d1 = drawRun(), d2 = drawRun();
    bkCheck(d1.rc == "0" && d2.rc == "0" && d1.n == d2.n && d1.xs == d2.xs &&
                d1.streamAfter == d1.streamBefore + 1,
            "W2 bykeep: `set seed` + `bysort g (b): keep if runiform() < .5` twice gives the "
            "same rows, and the draw advances the session's RNG stream by 1. Got rc " + d1.rc +
                "/" + d2.rc + " _N " + d1.n + "/" + d2.n + " x [" + d1.xs + "]/[" + d2.xs +
                "] stream " + std::to_string(d1.streamBefore) + "->" +
                std::to_string(d1.streamAfter));
  }

  // `by KEYS, sort:` is re-tokenized in place, not re-dispatched (which would
  // expand the already-expanded line a second time). A macro in the condition
  // is expanded once and the line runs as `bysort g (b):` (arm by_sortopt).
  // x > 6 keeps x 7 (g1), 9 (g3), 10 (g2), 12 (g0); rows then sorted g b.
  {
    piet::SessionState session(workdir);
    mkfix(session);
    (void)rcOf(session, "sort x");
    (void)rcOf(session, "local thr 6");
    const std::string rc = rcOf(session, "by g (b), sort: keep if x > `thr'");
    (void)rcOf(session, "generate double qg = sum(g*_n)");
    (void)rcOf(session, "generate double qx = sum(x)");
    const std::string n = shown(session, "_N"), sg = shown(session, "qg[_N]"),
                      sx = shown(session, "qx[_N]");
    bkCheck(rc == "0" && n == "4" && sg == "20" && sx == "38",
            "W2 bykeep: `local thr 6` + `by g (b), sort: keep if x > `thr'` keeps 4 rows "
            "(sum(g*_n) 20, sum(x) 38). Got rc " + rc + " _N " + n + " sum(g*_n) " + sg +
                " sum(x) " + sx);
  }

  // (h) A global holding a literal `$gs` (gs = Hi). The review of d82bdfa6
  // asked for an arm that tells a single expansion of the rewritten line from a
  // double one. None can exist here: Stata expands embedded references
  // recursively ([U] 18.3.10 item 6: a global whose text names another global
  // expands through to that one's contents), and so does PIET
  // (eval/macros.cpp expandMacros loops until no reference remains), so a
  // second expansion of an expanded line is a no-op; the one construct that
  // would survive one pass, the `\$` escape, is not implemented in PIET's
  // macro layer, and `++i` is refused. What is checked: `$thr` reaches `Hi`
  // (the documented recursive answer, 3 Hi rows kept), and the `, sort`
  // spelling answers exactly as `bysort` does -- the rewrite adds nothing of
  // its own. Derived, no captured arm.
  {
    auto run = [&](const char* cmd, std::string* lenOut) {
      piet::SessionState session(workdir);
      mkfix(session);
      (void)rcOf(session, "sort x");
      (void)rcOf(session, "global gs Hi");
      (void)rcOf(session, "global thr = char(36) + \"gs\"");
      if (lenOut != nullptr) *lenOut = shown(session, "length(\"$thr\")");
      const std::string rc = rcOf(session, cmd);
      (void)rcOf(session, "generate double qx = sum(x)");
      return rc + " " + shown(session, "_N") + " " + shown(session, "qx[_N]");
    };
    std::string len;
    const std::string viaSortOpt = run("by g (b), sort: keep if s == \"$thr\"", &len);
    const std::string viaBysort = run("bysort g (b): keep if s == \"$thr\"", nullptr);
    bkCheck(len == "2" && viaSortOpt == "0 3 22" && viaBysort == viaSortOpt,
            "W2 bykeep: (h) global thr = `$gs` (gs = Hi): `$thr` expands recursively to Hi "
            "([U] 18.3.10 item 6), and `by g (b), sort: keep if s == \"$thr\"` answers as the "
            "`bysort g (b):` spelling does (rc 0 _N 3 sum(x) 22). Got length " + len + ", sort "
            "option [" + viaSortOpt + "], bysort [" + viaBysort + "]");
  }

  // Under `by:`, the engine evaluates the condition and gets only a
  // whole-operand `VAR ==/!= "text"` right on a string variable. Every other
  // string form is refused by name, r(199), data untouched (the record is
  // PREC|by_unsorted|'s untouched `sort x` data). Previous round: the engine's
  // generic error, rc 1 (measured on main with `bysort g: generate t =
  // inlist(s,"Hi")`: "string/factor variable used where a number is
  // required"; strpos()/substr(): "function ... is not supported";
  // s==s[_n-1]: "string/factor comparison must be ...").
  {
    static const Arm kStr[] = {
        {"str_inlist", "sort x", "bysort g: keep if inlist(s, \"Hi\")", "199", "9", "75", "55",
         "357"},
        {"str_strpos", "sort x", "bysort g: drop if strpos(s, \"H\")", "199", "9", "75", "55",
         "357"},
        {"str_substr", "sort x", "bysort g (b): keep if substr(s,1,1)==\"H\"", "199", "9", "75",
         "55", "357"},
        {"str_lag", "sort x", "bysort g (b): drop if s==s[_n-1]", "199", "9", "75", "55", "357"},
        // Whole-operand compares with a literal keep running.
        {"str_eq", "sort x", "bysort g (b): keep if s == \"Hi\" & _n==1", "0", "2", "2", "15",
         "18"},
        {"str_ne", "sort x", "bysort g (b): keep if (s != \"Hi\") | _n==_N", "0", "8", "89", "52",
         "199"},
    };
    for (const Arm& a : kStr) {
      const Rec r = armOf(workdir, a.pre, a.cmd);
      const bool ok = r.rc == a.rc && r.n == a.n && r.sg == a.sg && r.sx == a.sx && r.shx == a.shx;
      bkCheck(ok, std::string("W2 bykeep: `") + a.cmd + "` (" + a.tag + ") -> rc " + a.rc +
                      " _N " + a.n + " sum(g*_n) " + a.sg + " sum(x) " + a.sx + " sum(x*_n) " +
                      a.shx + ". Got rc " + r.rc + " _N " + r.n + " sum(g*_n) " + r.sg +
                      " sum(x) " + r.sx + " sum(x*_n) " + r.shx);
    }
  }

  // A numeric key holding an extended missing value: Stata orders `. < .a <
  // ... < .z` and groups each apart, the engine's sort and by() treat every
  // missing kind as one. Refused by name, r(199), data untouched. Previous
  // round: ran, grouping .a with . (no StataBE arm; [U] 12.2.1 orders them).
  {
    struct ExtArm {
      const char* cmd;
    };
    static const ExtArm kExt[] = {
        {"bysort g (b): keep if _n==_N"},
        {"bysort b: drop if _n>1"},
        {"by g (b): keep if _n==1"},
    };
    for (const ExtArm& a : kExt) {
      piet::SessionState session(workdir);
      mkfix(session);
      (void)rcOf(session, "replace b = .a in 2");
      (void)rcOf(session, "replace b = . in 5");
      (void)rcOf(session, "sort g b");
      const std::string rc = rcOf(session, a.cmd);
      const std::string n = shown(session, "_N");
      bkCheck(rc == "199" && n == "9",
              std::string("W2 bykeep: `") + a.cmd +
                  "` with an extended missing (.a) in the key b is refused r(199), data "
                  "untouched. Got rc " + rc + " _N " + n);
    }
  }

  // w2e_probes.log bo_gsort_marker captures a leading ascending marker even
  // when the next key descends. Negative-first keys still have no marker.
  {
    piet::SessionState session(workdir);
    mkfix(session);
    const std::string rc = rcOf(session, "gsort g -x");
    (void)rcOf(session, "local __marker : sortedby");
    bkCheck(rc == "0" && session.eval.locals.get("__marker", "") == "g",
            "W2 bykeep: gsort g -x posts ascending prefix g (bo_gsort_marker)");
    (void)rcOf(session, "gsort -g x");
    (void)rcOf(session, "local __marker : sortedby");
    bkCheck(session.eval.locals.get("__marker", "") == "",
            "W2 bykeep: descending-first gsort has no ascending marker");
  }

  // The scratch-column path (a string sort key whose dictionary is not in
  // Stata's order, arm bys_p_str) leaves none of its intermediate files
  // behind: the setcol payload, the column-added copy and the sorted copy are
  // unlinked once read. Previous round: two whole-frame copies per line.
  {
    piet::SessionState session(workdir);
    mkfix(session);
    (void)rcOf(session, "sort x");
    const std::string rc = rcOf(session, "bysort s (b): keep if _n==_N");
    std::string left;
    std::error_code dec;
    for (const auto& e : fs::directory_iterator(session.workdir(), dec)) {
      const std::string nm = e.path().filename().u8string();
      if (nm.rfind("bykeepcol", 0) == 0 || nm.rfind("bykeepsort", 0) == 0 ||
          nm.rfind("bykeepvals", 0) == 0)
        left += nm + " ";
    }
    bkCheck(rc == "0" && shown(session, "_N") == "2" && left.empty(),
            "W2 bykeep: `bysort s (b): keep if _n==_N` (scratch-column path) leaves no "
            "bykeepcol/bykeepsort/bykeepvals file in the session workdir. Got rc " + rc +
                " left: " + left);
  }

  // Row order after a `bysort`. Stata's `bysort g (x):` sorts the data in place
  // by g x and leaves them marked sorted by g x (PREC|by_p_after_bysgen: a plain
  // `by` then runs on that order), and plain `by` reads the marker, not the rows
  // (PREC|by_two_halfsorted, by_p_wrongkey). PIET now sorts its rows in place
  // too (runBysortGen / sortInStataOrder), so the engine file's own claim IS the
  // marker. On mkfix the min-x row of each g is x 12, 3, 2, 1 (sum(x) 18), and
  // the min-b row is x 12, 3, 10, 5 (sum(x) 30). The sequences were derived
  // from [D] by / [D] sort; w2e_probes.log has since captured five of them with
  // the same answers: (a) bo_bysgen_then_by 0|4|18, (b) bo_macro_gsort and
  // bo_literal_gsort_ctl 0|4|18, (c) bo_fresh_use 0|4|30, (d)
  // bo_keep_then_gen_memo 0|4|30, (e) bo_by_h_r5 5|9|55. The others stay derived. Round 4 (d82bdfa6) kept a side ledger of row
  // orders instead; its review measured (b) rc 0 sum(x) 30, (c) r(199), (d)
  // r(199), (e) r(199) and (f) r(111).
  {
    struct SeqArm {
      const char* what;
      std::vector<std::string> pre;
      const char* cmd;
      const char* rc;
      const char* n;
      const char* sx;
    };
    const std::string memoFile = workdir + "/bykeep_memo.dta";
    const std::string memoSave = "save \"" + memoFile + "\", replace";
    const std::string memoUse = "use \"" + memoFile + "\", clear";
    const std::string otherFile = workdir + "/bykeep_other2.dta";
    const std::vector<std::string> kMk = {
        "clear", "set obs 12", "generate g = mod(_n*7, 4)", "generate b = mod(_n*5, 12)",
        "generate x = _n", "generate h = mod(_n, 2)", "generate z = 0",
        "drop if x == 4 | x == 8 | x == 11"};
    auto withMk = [&](std::vector<std::string> head, const std::vector<std::string>& tail) {
      head.insert(head.end(), kMk.begin(), kMk.end());
      head.insert(head.end(), tail.begin(), tail.end());
      return head;
    };
    const SeqArm kSeq[] = {
        // (a) the plain `by` runs on the (g x) rows the bysort left.
        {"(a) sort g b / bysort g (x): generate",
         {"sort g b", "bysort g (x): generate r = _n"},
         "by g: keep if _n==1", "0", "4", "18"},
        {"an intervening generate",
         {"sort g b", "bysort g (x): generate r = _n", "generate y = 1"},
         "by g: keep if _n==1", "0", "4", "18"},
        {"an intervening replace and drop-if",
         {"sort g b", "bysort g (x): generate r = _n", "replace z = 1", "drop if x == 99"},
         "by g: drop if _n>1", "0", "4", "18"},
        {"bysort g (x): egen",
         {"sort g b", "bysort g (x): egen m = mean(x)"},
         "by g: keep if _n==1", "0", "4", "18"},
        {"bysort g (x): replace",
         {"sort g b", "bysort g (x): replace z = _n"},
         "by g: keep if _n==1", "0", "4", "18"},
        // (b) gsort through a macro and typed literally: gsort g is a stable
        // sort of rows already in (g x) order.
        {"(b) local k g / gsort `k'",
         {"sort g b", "bysort g (x): generate r = _n", "local k g", "gsort `k'"},
         "by g: keep if _n==1", "0", "4", "18"},
        {"(b) literal gsort g",
         {"sort g b", "bysort g (x): generate r = _n", "gsort g"},
         "by g: keep if _n==1", "0", "4", "18"},
        // (c) a fresh `use` takes the file's own marker (g b).
        {"(c) fresh use of a file saved sorted by g b",
         withMk({"sort g b", "save \"" + otherFile + "\", replace"},
                {"sort g b", "bysort g (x): generate r = _n", "use \"" + otherFile + "\", clear"}),
         "by g: keep if _n==1", "0", "4", "30"},
        {"(c) fresh use / drop b / sort g",
         withMk({"sort g b", "save \"" + otherFile + "\", replace"},
                {"sort g b", "bysort g (x): generate r = _n", "use \"" + otherFile + "\", clear",
                 "drop b", "sort g"}),
         "by g: keep if _n==1", "0", "4", "30"},
        // (d) the same file used again, with a bysort keep before the generate.
        {"(d) use / bysort keep / bysort generate / the same use",
         {"sort g b", memoSave, memoUse, "bysort g (b): keep if _n<=100",
          "bysort g (x): generate r = _n", memoUse},
         "by g: keep if _n==1", "0", "4", "30"},
        {"use / bysort generate / the same use",
         {"sort g b", memoSave, memoUse, "bysort g (x): generate r = _n", memoUse},
         "by g: keep if _n==1", "0", "4", "30"},
        // (e) keys that do not lead the marker (g x): r(5), data untouched.
        {"(e) by h: after the bysort",
         {"sort g b", "bysort g (x): generate r = _n", "generate y = 1"},
         "by h: keep if _n==1", "5", "9", "55"},
        {"preserve / sort b / restore",
         {"sort g b", "bysort g (x): generate r = _n", "preserve", "sort b", "restore"},
         "by g: keep if _n==1", "0", "4", "18"},
        {"bysort g (x): summarize",
         {"sort g b", "bysort g (x): summarize z"},
         "by g: keep if _n==1", "0", "4", "18"},
        {"preserve / bysort g (x): summarize / restore",
         {"sort g b", "preserve", "bysort g (x): summarize z", "restore"},
         "by g: keep if _n==1", "0", "4", "30"},
        {"use / bysort g (x): summarize / the same use",
         {"sort g b", memoSave, memoUse, "bysort g (x): summarize z", memoUse},
         "by g: keep if _n==1", "0", "4", "30"},
        {"xtset g b",
         {"sort x", "bysort g (x): generate r = _n", "xtset g b"},
         "by g: keep if _n==1", "0", "4", "30"},
        {"preserve / restore, preserve / sort b / restore",
         {"sort g b", "bysort g (x): generate r = _n", "generate y = 1", "preserve",
          "restore, preserve", "sort b", "restore"},
         "by g: keep if _n==1", "0", "4", "18"},
        {"a user file rewritten after the bysort",
         {"bysort g (b): generate first = _n", "sort g b", "tempfile u", "save `u'",
          "use `u', clear", "bysort g (x): generate r = _n", "save `u', replace", "use `u', clear"},
         "by g: keep if _n==1", "0", "4", "18"},
        // `sort g` on data sorted by g x moves no row and sets the marker to g
        // (w2e_probes.log bo_sort_prefix_noop "g"; this first arm is
        // bo_sort_prefix_then_by as captured: rc 0, _N 4, sum(x) 18), so a
        // later `by g x:` is r(5), data untouched (derived from the marker
        // "g" and by_two_halfsorted; e19291ad answered rc 0).
        {"a later sort g, then by g (bo_sort_prefix_then_by)",
         {"sort g b", "bysort g (x): generate r = _n", "sort g"},
         "by g: keep if _n==1", "0", "4", "18"},
        {"a later sort g, then by g x",
         {"sort g b", "bysort g (x): generate r = _n", "sort g"},
         "by g x: keep if _n==1", "5", "9", "55"},
        {"a later sort g x",
         {"sort g b", "bysort g (x): generate r = _n", "generate y = 1", "sort g x"},
         "by g: keep if _n==1", "0", "4", "18"},
        {"bysort g (x): carryforward",
         {"sort g b", "bysort g (x): generate r = _n", "bysort g (x): carryforward z, gen(zf)"},
         "by g: keep if _n==1", "0", "4", "18"},
    };
    for (const SeqArm& a : kSeq) {
      piet::SessionState session(workdir);
      mkfix(session);
      std::string preRc;
      for (const auto& p : a.pre) preRc += rcOf(session, p) + " ";
      const std::string rc = rcOf(session, a.cmd);
      (void)rcOf(session, "generate double qx = sum(x)");
      const std::string n = shown(session, "_N"), sx = shown(session, "qx[_N]");
      bkCheck(rc == a.rc && n == a.n && sx == a.sx,
              std::string("W2 bykeep: [") + a.what + "] then `" + a.cmd + "` -> rc " + a.rc +
                  " _N " + a.n + " sum(x) " + a.sx +
                  " (derived from [D] by / [D] sort; analogues by_p_after_bysgen, "
                  "by_two_halfsorted). Got pre rc [" + preRc + "] rc " + rc + " _N " + n +
                  " sum(x) " + sx);
    }
  }

  // (f) Abbreviated keys (review of d82bdfa6, finding F): `xv` stands for
  // `xval`, the one variable it abbreviates ([U] 11.2); main ran the summarize
  // at rc 0 but ignored the key, d82bdfa6 answered r(111). The bysort sorts by
  // g xval, so the plain `by g:` keeps the min-x rows (sum 18). Two candidates
  // is r(111) "ambiguous abbreviation". Derived, no captured arm.
  {
    struct AbArm {
      std::vector<std::string> pre;
      const char* cmd;
      const char* rc;
      const char* then;  // nullptr = none
      const char* sx;    // sum(xval) after `then`, nullptr = not checked
    };
    const AbArm kAb[] = {
        {{"sort g b", "rename x xval"}, "bysort g (xv): summarize z", "0", "by g: keep if _n==1",
         "18"},
        {{"sort g b", "rename x xval"}, "bysort g (xv): generate r = _n", "0",
         "by g: keep if _n==1", "18"},
        {{"sort g b", "rename x xval"}, "bysort g (xv): keep if _n==1", "0", nullptr, "18"},
        {{"sort x", "rename g grp", "rename x xval"}, "bys gr (xv): egen m = mean(z)", "0",
         "by grp: keep if _n==1", "18"},
        {{"sort g b", "rename x xval", "generate xv2 = 1"}, "bysort g (xv): summarize z", "111",
         nullptr, nullptr},
    };
    for (const AbArm& a : kAb) {
      piet::SessionState session(workdir);
      mkfix(session);
      for (const auto& p : a.pre) (void)rcOf(session, p);
      const std::string rc = rcOf(session, a.cmd);
      std::string thenRc = "-", sx = "-";
      if (a.then != nullptr) thenRc = rcOf(session, a.then);
      if (a.sx != nullptr) {
        (void)rcOf(session, "generate double qx = sum(xval)");
        sx = shown(session, "qx[_N]");
      }
      const bool ok = rc == a.rc && (a.then == nullptr || thenRc == "0") &&
                      (a.sx == nullptr || sx == a.sx);
      bkCheck(ok, std::string("W2 bykeep: (f) `") + a.cmd + "` resolves the abbreviated key -> rc " +
                      a.rc + (a.sx ? std::string(", then sum(xval) ") + a.sx : "") +
                      ". Got rc " + rc + " then-rc " + thenRc + " sum(xval) " + sx);
    }
  }

  // (g) Every row-order read after `bysort g (x): generate` sees Stata's (g x)
  // order: x 12 3 7 2 6 10 1 5 9. Derived from [D] by, oracle pending
  // w2e_probes bo_list_after_bysgen / bo_lag_after_bysgen. From `sort x`,
  // d82bdfa6 left the rows 1 2 3 5 6 7 9 10 12 (list 1 2 3, lag[2] 1).
  {
    piet::SessionState session(workdir);
    mkfix(session);
    (void)rcOf(session, "sort x");
    const std::string rc = rcOf(session, "bysort g (x): generate r = _n");
    std::string listed;
    try {
      for (const auto& b : piet::dispatch("list x in 1/3", session))
        if (b.is_object() && b.value("type", std::string()) != "echo") listed += b.dump();
    } catch (const std::exception& e) {
      listed = std::string("EXC:") + e.what();
    }
    // The list block's rows, in order: "12", "3", "7" (the old order gives 1, 2, 3).
    const size_t p12 = listed.find("12"), p3 = listed.find("3", p12 == std::string::npos ? 0 : p12 + 2),
                 p7 = listed.find("7", p3 == std::string::npos ? 0 : p3 + 1);
    const bool listOk = p12 != std::string::npos && p3 != std::string::npos &&
                        p7 != std::string::npos && listed.find("\"1\"") == std::string::npos;
    const std::string lrc = rcOf(session, "generate lag = x[_n-1]");
    (void)rcOf(session, "generate double ql = sum(lag)");
    const std::string lag2 = shown(session, "lag[2]"), lag5 = shown(session, "lag[5]"),
                      sl = shown(session, "ql[_N]"), x1 = shown(session, "x[1]"),
                      x9 = shown(session, "x[9]");
    bkCheck(rc == "0" && listOk && lrc == "0" && lag2 == "12" && lag5 == "2" && sl == "46" &&
                x1 == "12" && x9 == "9",
            "W2 bykeep: (g) after `bysort g (x): generate` from `sort x`, `list x in 1/3` shows "
            "12 3 7 and `generate lag = x[_n-1]` gives lag[2] 12, lag[5] 2, sum 46 (Stata's "
            "(g x) order; derived from [D] by, oracle pending w2e_probes "
            "bo_list_after_bysgen / bo_lag_after_bysgen). Got rc " + rc + " list " + listed +
                " lag rc " + lrc + " lag[2] " + lag2 + " lag[5] " + lag5 + " sum " + sl +
                " x[1] " + x1 + " x[9] " + x9);
  }

  // The marker after a coarser sort (review of e19291ad). Stata's `sort g` on
  // data sorted by g x moves no row and sets `: sortedby` to "g"
  // (w2e_probes.log PREC|bo_sort_prefix_noop|0|g|), so a plain `by g b x:`
  // afterwards is r(5), data untouched: `by` reads the marker, not the rows
  // (by_two_halfsorted, by_p_wrongkey). e19291ad's runSort kept the longer
  // marker and answered rc 0 on the first two arms; that rule is gone, and the
  // engine's gsort (which skips the radix pass on a prefix of the claim) sets
  // the marker to the requested keys. The `bysort g: drop if s == ""` arms take
  // runByKeepIf's scratch-column path (a PIET-built blank column, then gsort g
  // file to file); `bysort g:` is `sort g`, so the marker is "g" there too
  // (review finding 4 assumed the longer marker should survive; the oracle
  // says it does not). On mkfix b is unique, so each (g b x) group is one row
  // and the min-b rows are x 12 3 10 5.
  {
    struct PrefArm {
      std::vector<std::string> pre;
      const char* cmd;
      const char* rc;
      const char* n;
      const char* sx;
    };
    const PrefArm kPref[] = {
        {{"sort g b x", "sort g"}, "by g b x: keep if _n==1", "5", "9", "55"},
        {{"sort g b x", "sort g b"}, "by g b x: keep if _n==1", "5", "9", "55"},
        {{"sort g b x", "sort g"}, "by g: keep if _n==1", "0", "4", "30"},
        {{"sort g b x", "sort g b x"}, "by g b x: keep if _n==1", "0", "9", "55"},
        {{"replace s = \"\" if x == 12", "sort g b x", "bysort g: drop if s == \"\""},
         "by g b x: keep if _n==1", "5", "8", "43"},
        {{"replace s = \"\" if x == 12", "sort g b x", "bysort g: drop if s == \"\""},
         "by g: keep if _n==1", "0", "3", "18"},
    };
    for (const PrefArm& a : kPref) {
      piet::SessionState session(workdir);
      mkfix(session);
      std::string preRc;
      for (const auto& p : a.pre) preRc += rcOf(session, p) + " ";
      const std::string rc = rcOf(session, a.cmd);
      (void)rcOf(session, "generate double qx = sum(x)");
      const std::string n = shown(session, "_N"), sx = shown(session, "qx[_N]");
      std::string pres;
      for (const auto& p : a.pre) pres += p + " / ";
      bkCheck(rc == a.rc && n == a.n && sx == a.sx,
              std::string("W2 bykeep: [") + pres + "] then `" + a.cmd + "` -> rc " + a.rc +
                  " _N " + a.n + " sum(x) " + a.sx +
                  " (the marker is the last sort's keys; w2e_probes bo_sort_prefix_noop). Got "
                  "pre rc [" + preRc + "] rc " + rc + " _N " + n + " sum(x) " + sx);
    }
  }

  // w2e_probes.log PREC|bo_list_after_bysgen|0|12|3|7|1|1|2| and
  // PREC|bo_lag_after_bysgen|0|46|12|, replayed as captured: mkfix / sort g b
  // / bysort g (x): generate r=_n, then generate lag = x[_n-1].
  {
    piet::SessionState session(workdir);
    mkfix(session);
    (void)rcOf(session, "sort g b");
    const std::string rc = rcOf(session, "bysort g (x): generate r=_n");
    const std::string got = shown(session, "x[1]") + "|" + shown(session, "x[2]") + "|" +
                            shown(session, "x[3]") + "|" + shown(session, "r[1]") + "|" +
                            shown(session, "r[2]") + "|" + shown(session, "r[3]");
    const std::string lrc = rcOf(session, "generate lag = x[_n-1]");
    (void)rcOf(session, "generate double ql = sum(lag)");
    const std::string lag = shown(session, "ql[_N]") + "|" + shown(session, "lag[2]");
    bkCheck(rc == "0" && got == "12|3|7|1|1|2" && lrc == "0" && lag == "46|12",
            "W2 bykeep: bo_list_after_bysgen rc 0 x[1..3] r[1..3] = 12|3|7|1|1|2 and "
            "bo_lag_after_bysgen rc 0 sum(lag) lag[2] = 46|12 (w2e_probes.log). Got rc " + rc +
                " " + got + " lag rc " + lrc + " " + lag);
  }

  // REFUSAL rc (review of e19291ad, finding 2): a by-key or a parenthesised
  // sort key written as a range with an endpoint that is no variable is
  // Stata's r(111) "variable zz not found" (the arms bys_nosuchvar /
  // bys_p_nosuchkey give r(111) for a plain missing name, data untouched).
  // e19291ad answered rc 1 (loops::expandVarlist's range block carries no rc).
  {
    const char* kRange[] = {"bysort g (zz-yy): generate r = _n", "bysort zz-yy: keep if _n==1",
                            "bysort g (b-zz): summarize x", "by g (zz-yy): keep if _n==1"};
    for (const char* cmd : kRange) {
      piet::SessionState session(workdir);
      mkfix(session);
      (void)rcOf(session, "sort x");
      (void)rcOf(session, std::string("capture ") + cmd);
      const std::string rc = shown(session, "_rc");
      const std::string n = shown(session, "_N"), x1 = shown(session, "x[1]");
      bkCheck(rc == "111" && n == "9" && x1 == "1",
              std::string("W2 bykeep: `capture ") + cmd +
                  "` / `display _rc` -> 111, data untouched (_N 9, x[1] 1). Got _rc " + rc +
                  " _N " + n + " x[1] " + x1);
    }
  }

  // BUG-623: `bysort id (t): carryforward` fills in t order. t = 4 3 2 1 and y
  // is set only on the t == 1 row: sorted by id t, that row comes first and the
  // fill reaches all 4 rows. main sorted by id alone: f filled 1 row, the data
  // stayed t = 4 3 2 1.
  {
    piet::SessionState session(workdir);
    for (const char* s0 : {"clear", "set obs 4", "generate id = 1", "generate t = 5 - _n",
                           "generate y = .", "replace y = 10 in 4"}) {
      (void)rcOf(session, s0);
    }
    const std::string rc = rcOf(session, "bysort id (t): carryforward y, gen(f)");
    std::string filled;
    try {
      for (const auto& b : piet::dispatch("count if f == 10", session))
        if (b.is_object() && b.value("type", std::string()) == "display")
          filled += b.value("text", std::string());
    } catch (const std::exception&) {
    }
    const std::string t1 = shown(session, "t[1]");
    bkCheck(rc == "0" && filled == "4" && t1 == "1",
            "W2 bykeep: `bysort id (t): carryforward y, gen(f)` sorts by id t (t[1] 1) and "
            "fills all 4 rows. Got rc " + rc + " filled " + filled + " t[1] " + t1);
  }

  // BUG-620: `!`, `&`, `|` read a missing operand as TRUE, and the result is
  // always 0/1. Oracle: w2c_functions.log section operators_missing, fixture
  // x = . 0 3 .a 5, y = 1 . 0 2 . (values copied from its PREC lines). main:
  // `!x` was . on the missing rows, a missing `&`/`|` operand read as false
  // (count if x & 1 -> 2, keep if x & 1 -> _N 2, drop if x | 0 -> _N 3), and
  // count's CiParser fast path and its engine fallback disagreed on `!x` (3
  // vs 1). The display/local/scalar arms are the macro layer's, not here.
  {
    auto opFix = [&](piet::SessionState& session) {
      for (const char* s0 :
           {"clear", "set obs 5", "generate x = .", "replace x = 0 in 2", "replace x = 3 in 3",
            "replace x = .a in 4", "replace x = 5 in 5", "generate y = 1", "replace y = . in 2",
            "replace y = 0 in 3", "replace y = 2 in 4", "replace y = . in 5"}) {
        (void)rcOf(session, s0);
      }
    };
    auto countOf = [&](piet::SessionState& session, const std::string& cmd) {
      std::string got;
      try {
        for (const auto& b : piet::dispatch(cmd, session)) {
          if (!b.is_object()) continue;
          if (b.value("type", std::string()) == "error") return "rc " + piet::eval::coarseRc(b);
          if (b.value("type", std::string()) == "display") got += b.value("text", std::string());
        }
      } catch (const std::exception& e) {
        return std::string("EXC:") + e.what();
      }
      return got;
    };
    struct GenArm {
      const char* tag;
      const char* exp;
      const char* want;  // v[1]|..|v[5]
    };
    static const GenArm kGen[] = {{"op_not_x", "!x", "0|1|0|0|0"},
                                  {"op_x_and_1", "x & 1", "1|0|1|1|1"},
                                  {"op_x_or_0", "x | 0", "1|0|1|1|1"},
                                  {"op_x_and_y", "x & y", "1|0|0|1|1"},
                                  {"op_x_or_y", "x | y", "1|1|1|1|1"}};
    for (const GenArm& a : kGen) {
      piet::SessionState session(workdir);
      opFix(session);
      const std::string rc = rcOf(session, std::string("generate v = ") + a.exp);
      std::string got;
      for (int i = 1; i <= 5; ++i)
        got += (i > 1 ? "|" : "") + shown(session, "v[" + std::to_string(i) + "]");
      bkCheck(rc == "0" && got == a.want,
              std::string("W2 bykeep: `generate v = ") + a.exp + "` gives " + a.want +
                  " (w2c_functions.log PREC|" + a.tag + "|). Got rc " + rc + " " + got);
    }
    // count: the plain predicate takes the CiParser fast path; `& _n>0` (true
    // on every row) makes it bail, so the engine fallback answers the same
    // question. Both must give the oracle's r(N).
    struct CountArm {
      const char* tag;
      const char* exp;
      const char* want;
    };
    static const CountArm kCount[] = {{"op_count_not_x", "!x", "1"},
                                      {"op_count_x_and_1", "x & 1", "4"},
                                      {"op_count_x_or_0", "x | 0", "4"},
                                      {"op_count_x_and_y", "x & y", "3"}};
    for (const CountArm& a : kCount) {
      piet::SessionState session(workdir);
      opFix(session);
      const std::string fast = countOf(session, std::string("count if ") + a.exp);
      const std::string slow = countOf(session, std::string("count if (") + a.exp + ") & _n>0");
      bkCheck(fast == a.want && slow == a.want,
              std::string("W2 bykeep: `count if ") + a.exp + "` gives " + a.want +
                  " on the fast path and the engine fallback (w2c_functions.log PREC|" + a.tag +
                  "|). Got fast " + fast + " fallback " + slow);
    }
    struct KeepArm {
      const char* tag;
      const char* cmd;
      const char* n;
    };
    // keep_not_x has no PREC line of its own: op_not_x is 1 on one row (sum 1).
    static const KeepArm kKeep[] = {{"op_keep_x_and_1", "keep if x & 1", "4"},
                                    {"op_drop_x_or_0", "drop if x | 0", "1"},
                                    {"op_not_x (sum 1)", "keep if !x", "1"}};
    for (const KeepArm& a : kKeep) {
      piet::SessionState session(workdir);
      opFix(session);
      const std::string rc = rcOf(session, a.cmd);
      const std::string n = shown(session, "_N");
      bkCheck(rc == "0" && n == a.n, std::string("W2 bykeep: `") + a.cmd + "` leaves _N " + a.n +
                                         " (w2c_functions.log PREC|" + a.tag + "|). Got rc " +
                                         rc + " _N " + n);
    }
  }

  // ---- w2m_bymask (tests/eval/golden/aer_w2/w2m_bymask.log): the by-mask class
  // through PIET's routing. Each record is the log's PREC line, rc then values.
  // (A) BUG-840: egen's `if` under by() / a by-prefix is group-relative (main:
  // E01 . 100 100 90 90 90). (B) BUG-841: a running-sum replace sums only the
  // selected rows and keeps the others (main: C09 -1 60 120 130 160 210). (C)
  // BUG-842: group() under `in` numbers within the range, an `if` does not (main:
  // G01 r(1), G03 left the old values).
  {
    struct MaskArm {
      const char* tag;
      bool f12;
      const char* pre;  // nullptr = none
      const char* cmd;
      const char* var;
      const char* want;  // rc|v1|...|vN
    };
    static const MaskArm kMask[] = {
        {"B09 :265", false, "generate double b09 = -1", "by gg: replace b09 = sum(x) if _n > 1",
         "b09", "0|-1|40|100|-1|30|80"},
        {"C09 :361", false, "generate double c09 = -1", "replace c09 = sum(x) if _n > 1", "c09",
         "0|-1|40|100|110|140|190"},
        {"E01 :373", false, nullptr, "by gg: egen e01 = total(x) if _n > 1", "e01",
         "0|.|100|100|.|80|80"},
        {"E02 :383", false, nullptr, "egen e02 = total(x) if _n > 1, by(gg)", "e02",
         "0|.|100|100|.|80|80"},
        {"E05 :411", false, nullptr, "by gg: egen e05 = max(x) if _n < _N", "e05",
         "0|40|40|.|30|30|."},
        {"G01 :472", false, nullptr, "generate g01 = group(2) in 2/4", "g01", "0|.|1|1|2|.|."},
        {"G02 :482", false, nullptr, "generate g02 = group(2) if _n>=2 & _n<=4", "g02",
         "0|.|1|1|2|.|."},
        {"G03 :494", false, "generate double g03 = x", "replace g03 = group(2) in 2/4", "g03",
         "0|20|1|1|2|30|50"},
        {"G04 :504", false, nullptr, "generate g04 = sum(group(2)) in 2/4", "g04",
         "0|.|1|2|4|.|."},
        {"G05 :514", false, nullptr, "generate g05 = group(2) in 3/6", "g05", "0|.|.|1|1|2|2"},
        {"D09 :712", true, nullptr, "bysort g (x): egen d09 = total(x) if _n > 1", "d09",
         "0|.|73|73|73|73|.|16|16|16|.|52|52"},
        {"D11 :728", true, "generate double d11 = -1",
         "bysort g (x): replace d11 = sum(x) if _n > 1", "d11",
         "0|-1|18|43|73|73|-1|7|16|16|-1|12|52"},
        // Controls (pass on main): GRPIF, an `if` does not renumber (test_fn_w2c
        // StataBE pin, `group(2) if _n > 2` on N = 6); E03/E04 without `if`.
        {"GRPIF", false, nullptr, "generate z = group(2) if _n > 2", "z", "0|.|.|1|2|2|2"},
        {"E03 :392", false, nullptr, "by gg: egen e03 = total(x)", "e03",
         "0|120|120|120|90|90|90"},
        // runReplace's route (no _n, no `in`) folded the same running sum: `x != 20`
        // selects the rows `_n > 1` selects on F6, so the record is C09's.
        {"C09 :361 (if x != 20)", false, "generate double c = -1", "replace c = sum(x) if x != 20",
         "c", "0|-1|40|100|110|140|190"},
        // A string by-variable naming F6's groups in the same order: E01's record.
        {"E01 :373 (string by)", false, "generate str1 s = cond(gg == 0, \"a\", \"b\")",
         "bysort s: egen es = total(x) if _n > 1", "es", "0|.|100|100|.|80|80"},
    };
    for (const MaskArm& a : kMask) {
      piet::SessionState session(workdir);
      if (a.f12) mkF12(session);
      else mkF6(session);
      if (a.pre != nullptr) (void)rcOf(session, a.pre);
      const std::string rc = rcOf(session, a.cmd);
      const std::string got = rc + "|" + valuesOf(session, a.var, a.f12 ? 12 : 6);
      bkCheck(got == a.want, std::string("W2 bykeep: w2m_bymask ") + a.tag + " `" + a.cmd +
                                 "` gives " + a.want + ". Got " + got);
    }
    // The store type of a masked running-sum replace is read from the masked
    // values: x = -100, 150 and `replace b = sum(x) if _n > 1` stores 150 (C09
    // :361's rule: the excluded row adds nothing), which overflows byte, so b
    // becomes int (w2e_float_default.log replace_byte_overflow :876). The folded
    // probe read -100 + 150 = 50, kept byte, and the store wrote `.`.
    for (const char* cmd : {"replace b = sum(x) if _n > 1", "replace b = sum(x) if x > 0"}) {
      piet::SessionState session(workdir);
      for (const char* s0 : {"clear", "set obs 2", "generate x = cond(_n == 1, -100, 150)",
                             "generate byte b = 0", "local __t \"\""})
        (void)rcOf(session, s0);
      const std::string rc = rcOf(session, cmd);
      (void)rcOf(session, "local __t : type b");
      const std::string got = rc + "|" + valuesOf(session, "b", 2) + "|" +
                              shown(session, "\"`__t'\"");
      bkCheck(got == "0|0|150|int",
              std::string("W2 bykeep: `") + cmd + "` into a byte stores 150 as int " +
                  "(w2m_bymask C09 :361 masked sum; replace_byte_overflow :876). Got " + got);
    }
    // A masked running sum whose `if` carries a time-series operator: the `if`
    // rides as --gen-mask, outside the sum expression, and still needs the tsset
    // metadata (the branch's first cut sent none and refused at r(111) with "no
    // time variable is set"; main folded the `if` and summed row 4). C09 :361's
    // rule on tsset data: only the selected rows accumulate, an unselected row
    // keeps its value (generate: missing), and L.z missing on row 1 selects it.
    {
      struct TsArm {
        const char* pre;
        const char* cmd;
        const char* var;
        const char* want;
      };
      static const TsArm kTs[] = {
          {"generate double y = -1", "replace y = sum(x) if L.z > 0", "y",
           "0|10|30|60|-1|110|170|double"},
          {"generate double y = -1", "bysort g: replace y = sum(x) if L.z > 0", "y",
           "0|10|30|60|-1|50|110|double"},
          {"generate byte y = 0", "replace y = sum(x) if L.z > 0", "y",
           "0|10|30|60|0|110|170|int"},
          {nullptr, "generate y = sum(x) if L.z > 0", "y", "0|10|30|60|.|110|170|float"},
      };
      for (const TsArm& a : kTs) {
        piet::SessionState session(workdir);
        for (const char* s0 : {"clear", "set obs 6", "generate t = _n", "generate x = _n * 10",
                               "generate z = cond(_n == 3, -1, 1)", "generate g = _n > 3",
                               "tsset t", "local __t \"\""})
          (void)rcOf(session, s0);
        if (a.pre != nullptr) (void)rcOf(session, a.pre);
        const std::string rc = rcOf(session, a.cmd);
        (void)rcOf(session, std::string("local __t : type ") + a.var);
        const std::string got = rc + "|" + valuesOf(session, a.var, 6) + "|" +
                                shown(session, "\"`__t'\"");
        bkCheck(got == a.want, std::string("W2 bykeep: `") + a.cmd + "` on tsset data gives " +
                                   a.want + " (w2m_bymask C09 :361 masked sum). Got " + got);
      }
    }
    // No refusal on these paths is rc 1: `capture noisily generate z = group(2)
    // in 3/6` / `display _rc` prints 0 (G05).
    {
      piet::SessionState session(workdir);
      mkF6(session);
      (void)rcOf(session, "capture noisily generate z = group(2) in 3/6");
      const std::string rc = shown(session, "_rc");
      bkCheck(rc == "0", "W2 bykeep: w2m_bymask G05 :514 under capture: _rc 0. Got " + rc);
    }
  }

  // ---- FU-163 / FU-36: by-prefixed `count`, and r() after a by-prefixed summarize ----
  // Oracle: tests/eval/golden/aer_w2/w2n_followups.log section E (E01-E04), fixture
  // lines w2n_followups.do:300-313 (60 rows, c = mod(_n,4), last group c==3 has 15
  // rows). All of these were rc 199 / the previous command's r() on main.
  {
    auto trim = [](std::string t) {
      const size_t a = t.find_first_not_of(' ');
      return a == std::string::npos ? std::string() : t.substr(a);
    };
    auto mkE = [](piet::SessionState& session) {
      for (const char* s0 : {"clear", "set obs 60", "generate x1 = mod(_n*7,11) + mod(_n,5)/10",
                              "generate byte c = mod(_n,4)"})
        (void)rcOf(session, s0);
    };
    // E04 (log :920): rc 0, r(N) = 15 after `quietly summarize c` left r(N) = 60.
    {
      piet::SessionState session(workdir);
      mkE(session);
      (void)rcOf(session, "quietly summarize c");
      const std::string rc = rcOf(session, "capture quietly bysort c: count");
      const std::string crc = shown(session, "_rc");
      bkCheck(crc == "0" && shown(session, "r(N)") == "15",
              "W2 bykeep: `capture quietly bysort c: count` is rc 0 with r(N) 15, the last "
              "group's count (w2n_followups.log:920 PREC|E04|0|15|). Got _rc " + crc + " r(N) " +
                  shown(session, "r(N)") + " (line rc " + rc + ")");
    }
    // E02 / E03 (log :894, :908): r(N) 15 after by-prefixed summarize; r(mean) is the
    // last group's, equal to the plain summarize on that group (E01, log :882).
    for (const bool sortFirst : {true, false}) {
      piet::SessionState session(workdir);
      mkE(session);
      if (!sortFirst) (void)rcOf(session, "sort c");
      (void)rcOf(session, "quietly summarize c");
      const std::string crc =
          rcOf(session, std::string("capture quietly ") + (sortFirst ? "bysort" : "by") +
                            " c: summarize x1");
      const std::string rcShown = shown(session, "_rc");
      const std::string n = shown(session, "r(N)");
      const std::string mean = trim(shown(session, "%18.0g r(mean)"));
      (void)rcOf(session, "quietly summarize x1 if c == 3");
      const std::string ctl = trim(shown(session, "%18.0g r(mean)"));
      bkCheck(rcShown == "0" && n == "15" && mean == ctl && mean != "1.5",
              std::string("W2 bykeep: `") + (sortFirst ? "bysort" : "by") +
                  " c: summarize x1` leaves the LAST group's r(N)=15 and r(mean) "
                  "(w2n_followups.log:" + (sortFirst ? "894" : "908") + " PREC|" +
                  (sortFirst ? "E02" : "E03") + "|0|15|5.266666681071123|; control E01 :882). Got rc " +
                  rcShown + " r(N) " + n + " r(mean) " + mean + " control " + ctl + " (line rc " +
                  crc + ")");
    }
    // The last group's own statistics, not group 0's and not the whole sample's.
    {
      piet::SessionState session(workdir);
      mkE(session);
      (void)rcOf(session, "quietly summarize x1 if c == 3");
      const std::string wantMin = shown(session, "r(min)"), wantMax = shown(session, "r(max)"),
                        wantSd = shown(session, "r(sd)");
      (void)rcOf(session, "quietly summarize c");
      (void)rcOf(session, "quietly bysort c: summarize x1");
      bkCheck(shown(session, "r(min)") == wantMin && shown(session, "r(max)") == wantMax &&
                  shown(session, "r(sd)") == wantSd,
              "W2 bykeep: by-prefixed summarize leaves the last group's r(min)/r(max)/r(sd) "
              "(w2n_followups.log:894 rule). Got min " + shown(session, "r(min)") + " max " +
                  shown(session, "r(max)") + " sd " + shown(session, "r(sd)") + " want " + wantMin +
                  " " + wantMax + " " + wantSd);
    }
    // meanonly keeps the plain arm's BUG-476 rule (r(sd) is Stata's `.`) for the last group.
    {
      piet::SessionState session(workdir);
      mkE(session);
      (void)rcOf(session, "quietly summarize x1 if c == 3");
      const std::string wantMean = shown(session, "r(mean)");
      (void)rcOf(session, "quietly summarize c");
      const std::string rc = rcOf(session, "quietly bysort c: summarize x1, meanonly");
      bkCheck(rc == "0" && shown(session, "r(mean)") == wantMean && shown(session, "r(N)") == "15" &&
                  shown(session, "missing(r(sd))") == "1",
              "W2 bykeep: `bysort c: summarize x1, meanonly` keeps BUG-476 for the last group "
              "(oracle_r.log:71). Got rc " + rc + " r(mean) " + shown(session, "r(mean)") +
                  " r(N) " + shown(session, "r(N)") + " r(sd) " + shown(session, "r(sd)"));
    }
    // `if` applies within each group; r(N) is the last group's hand count.
    {
      piet::SessionState session(workdir);
      mkE(session);
      (void)rcOf(session, "quietly count if c == 3 & x1 > 5");
      const std::string want = shown(session, "r(N)");
      (void)rcOf(session, "quietly summarize c");
      const std::string rc = rcOf(session, "quietly bysort c: count if x1 > 5");
      bkCheck(rc == "0" && shown(session, "r(N)") == want && want != "60",
              "W2 bykeep: `bysort c: count if x1 > 5` leaves the last group's count (plain "
              "`count if c == 3 & x1 > 5` = " + want + "; w2n_followups.log:920 rule). Got rc " + rc +
                  " r(N) " + shown(session, "r(N)"));
      (void)rcOf(session, "sort c");
      (void)rcOf(session, "quietly summarize c");
      const std::string rc2 = rcOf(session, "quietly by c: count if x1 > 5");
      bkCheck(rc2 == "0" && shown(session, "r(N)") == want,
              "W2 bykeep: `by c: count if x1 > 5` on sorted data gives the same. Got rc " + rc2 +
                  " r(N) " + shown(session, "r(N)"));
    }
    // Refusals: count takes no varlist and no options; both are r(198), never r(1).
    // `in` under a by prefix is r(190) for every command (the existing guard).
    {
      piet::SessionState session(workdir);
      mkE(session);
      const std::string rcVar = rcOf(session, "bysort c: count x1");
      const std::string rcOpt = rcOf(session, "bysort c: count, foo");
      const std::string rcIn = rcOf(session, "bysort c: count in 1/40");
      bkCheck(rcVar == "198", "W2 bykeep: `bysort c: count x1` is a named refusal, rc 198. Got " + rcVar);
      bkCheck(rcOpt == "198", "W2 bykeep: `bysort c: count, foo` is a named refusal, rc 198. Got " + rcOpt);
      bkCheck(rcIn == "190", "W2 bykeep: `bysort c: count in 1/40` keeps the by-prefix `in` refusal, "
                             "rc 190. Got " + rcIn);
    }
    // Noisy form: UNPINNED (no captured oracle). A `-> c = k` header and that group's
    // count per group, four groups of 15.
    {
      piet::SessionState session(workdir);
      mkE(session);
      std::string text;
      for (const auto& b : piet::dispatch("bysort c: count", session))
        if (b.is_object() && b.value("type", std::string()) == "display")
          text += b.value("text", std::string()) + "\n";
      bkCheck(text == "-> c = 0\n15\n-> c = 1\n15\n-> c = 2\n15\n-> c = 3\n15\n",
              "W2 bykeep (UNPINNED, no oracle): noisy `bysort c: count` prints a header and the "
              "count per group. Got: " + text);
    }
  }

  precedenceArms(workdir, kPrecArms, std::size(kPrecArms), "w2ac_expr_precedence");
  precedenceArms(workdir, kRelArms, std::size(kRelArms), "w2ac_expr_relational");
  precedenceArms(workdir, kVrArms, std::size(kVrArms), "w2ac_expr_valueref");
  precedenceArms(workdir, kSynArms, std::size(kSynArms), "w2ac_expr_syntaxrc");

  std::printf("[selftest] W2 bykeep: %d arms executed\n", gByKeepChecks);
  fs::remove_all(workdir, ec);
}
