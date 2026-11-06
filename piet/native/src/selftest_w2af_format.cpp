// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// format-display-default-lifecycle (2026-10-20) -- one display-format resolver.
//
// Every reader of a variable's display format (`: format`, describe, codebook,
// list/browse, the varlist cards) now asks SessionState::displayFormat: the
// session's own format, else the file's, else Stata's default for the DECLARED
// storage type (byte/int %8.0g, long %12.0g, float %9.0g, double %10.0g, str#
// %#s with a minimum width of 9, strL %9s). Before, each reader kept its own
// fallback and all of them answered %9.0g for a byte, a long or a str12. Every
// arm cites the oracle record it rests on and fails on frozen main 62c376ca
// unless it is marked CONTROL.
#include "selftest_w2.h"

#include <cstdio>
#include <filesystem>
#include <string>

#include "piet/dispatcher.h"
#include "piet/doexec.h"
#include "piet/eval/errors.h"
#include "piet/session_state.h"
#include "vendor/nlohmann/json.hpp"

namespace {

int gAfChecks = 0;
void afCheck(bool cond, const std::string& label) {
  ++gAfChecks;
  w2check(cond, label);
}

using ojson = nlohmann::ordered_json;

ojson run(piet::SessionState& session, const std::string& src) {
  return piet::doexec::executeDoSource(src, session, piet::dispatch).blocks;
}

std::string L(piet::SessionState& session, const std::string& k) {
  return session.eval.locals.get(k, "<unset>");
}

// `: format v` and `: type v` through the macro layer (the projection
// syncEvalVarLabels writes), "absent" when the function fails.
std::string fmtOf(piet::SessionState& session, const std::string& v) {
  run(session, "local __f absent\ncapture local __f : format " + v + "\n");
  return L(session, "__f");
}
std::string typeOf(piet::SessionState& session, const std::string& v) {
  run(session, "local __t absent\ncapture local __t : type " + v + "\n");
  return L(session, "__t");
}

// The `format` field the last block of `type` reports for variable v ("" if none).
std::string blockFormat(const ojson& blocks, const std::string& type, const std::string& listKey,
                        const std::string& v) {
  std::string out;
  for (const auto& b : blocks) {
    if (!b.is_object() || b.value("type", std::string()) != type || !b.contains(listKey)) continue;
    for (const auto& e : b[listKey]) {
      if (e.is_object() && e.value("name", std::string()) == v)
        out = e.value("format", std::string("<no format key>"));
    }
  }
  return out;
}

// The coarse rc of the last error block a line produced ("0" if none).
std::string rcOf(piet::SessionState& session, const std::string& cmd) {
  std::string rc = "0";
  for (const auto& b : piet::dispatch(cmd, session)) {
    if (b.is_object() && b.value("type", std::string()) == "error") rc = piet::eval::coarseRc(b);
  }
  return rc;
}

const char* kCreate =
    "clear\n"
    "quietly set obs 6\n"
    "generate x1 = _n + 1\n"
    "generate byte e_byte = x1/3\n"
    "generate int e_int = x1/3\n"
    "generate long e_long = x1/3\n"
    "generate float e_float = x1/3\n"
    "generate double e_double = x1/3\n"
    "generate e_default = x1/3\n"
    "generate str5 e_str5 = \"ab\" + string(_n)\n"
    "generate str12 s12 = \"x\" + string(_n)\n"
    "generate strL sL = \"big\"\n";

void testDefaultsByType(const std::string& workdir) {
  piet::SessionState session(workdir);
  run(session, kCreate);
  afCheck(fmtOf(session, "e_byte") == "%8.0g" && fmtOf(session, "e_int") == "%8.0g",
          "W2AF defaults: byte and int answer %8.0g (w2p_estim_ev.log:1725 E_byte, :1742 E_int) "
          "-- main %9.0g. Got " + fmtOf(session, "e_byte") + "/" + fmtOf(session, "e_int"));
  afCheck(fmtOf(session, "e_long") == "%12.0g",
          "W2AF defaults: long answers %12.0g (w2p_estim_ev.log:1759 E_long) -- main %9.0g. Got " +
              fmtOf(session, "e_long"));
  afCheck(fmtOf(session, "e_double") == "%10.0g",
          "W2AF defaults: double answers %10.0g (w2p_estim_ev.log:1793 E_double) -- main %9.0g. "
          "Got " + fmtOf(session, "e_double"));
  afCheck(fmtOf(session, "e_float") == "%9.0g" && fmtOf(session, "e_default") == "%9.0g",
          "W2AF defaults CONTROL: float and an untyped generate stay %9.0g (w2p_estim_ev.log:1776 "
          "E_float, :1810 E_default)");
  afCheck(fmtOf(session, "e_str5") == "%9s",
          "W2AF defaults: str5 answers %9s (w2p_estim_ev.log:1827 E_str5) -- main %9.0g. Got " +
              fmtOf(session, "e_str5"));
  afCheck(fmtOf(session, "s12") == "%12s",
          "W2AF defaults: str12 answers %12s ([D] format: str# is %#s; str10 %10s and str20 %20s in "
          "the captured describe blocks, aer_b4/egen-six.log:449) -- main %9.0g. Got " +
              fmtOf(session, "s12"));
  afCheck(fmtOf(session, "sL") == "%9s" && typeOf(session, "sL") == "strL",
          "W2AF defaults: strL answers type strL and format %9s ([D] format; aer_w2 describe blocks "
          "`strL %9s`) -- main strl/%9.0g. Got " + typeOf(session, "sL") + "/" +
              fmtOf(session, "sL"));

  // describe, codebook and list read the same resolver.
  const ojson desc = run(session, "describe e_byte e_long e_double e_str5 s12\n");
  afCheck(blockFormat(desc, "describe", "variables", "e_byte") == "%8.0g" &&
              blockFormat(desc, "describe", "variables", "e_long") == "%12.0g" &&
              blockFormat(desc, "describe", "variables", "e_double") == "%10.0g" &&
              blockFormat(desc, "describe", "variables", "e_str5") == "%9s" &&
              blockFormat(desc, "describe", "variables", "s12") == "%12s",
          "W2AF readers: describe reports %8.0g/%12.0g/%10.0g/%9s/%12s, the same answers as "
          "`: format` (w2p_estim_ev.log E_*) -- main %9.0g x3, %9s, %9s");
  const ojson cb = run(session, "codebook e_long s12, compact\n");
  afCheck(blockFormat(cb, "describe", "variables", "e_long") == "%12.0g" &&
              blockFormat(cb, "describe", "variables", "s12") == "%12s",
          "W2AF readers: codebook reports %12.0g and %12s (w2p_estim_ev.log:1759 E_long) -- main "
          "%9.0g and %9s. Got " + blockFormat(cb, "describe", "variables", "e_long") + "/" +
              blockFormat(cb, "describe", "variables", "s12"));
  const ojson lst = run(session, "list e_double e_long e_byte s12 in 1/2\n");
  afCheck(blockFormat(lst, "browse", "columns", "e_double") == "%10.0g" &&
              blockFormat(lst, "browse", "columns", "e_long") == "%12.0g" &&
              blockFormat(lst, "browse", "columns", "e_byte") == "%8.0g" &&
              blockFormat(lst, "browse", "columns", "s12") == "%12s",
          "W2AF readers: list carries each column's display format (w2p_estim_ev.log E_*) -- main "
          "emitted no format key for an unformatted column. Got " +
              blockFormat(lst, "browse", "columns", "e_double"));
  const ojson mut = run(session, "generate double e_d2 = 1\n");
  afCheck(blockFormat(mut, "data_mutation", "varlist_after", "e_d2") == "%10.0g" &&
              blockFormat(mut, "data_mutation", "varlist_after", "s12") == "%12s",
          "W2AF readers: the varlist card states %10.0g/%12s, not VarInfo's %9.0g member default "
          "(w2p_estim_ev.log:1793 E_double) -- main %9.0g for both");
}

void testPersistence(const std::string& workdir) {
  piet::SessionState session(workdir);
  run(session, kCreate);
  run(session, "format e_float %6.2f\nformat e_double %9.0g\n");
  // BUG-198 law: a column with only a derived default states NO format in the
  // saved header; a declared one (even the float default spelled out) does.
  std::vector<piet::VarInfo> card;
  for (const char* n : {"e_byte", "e_double", "e_float", "s12"}) {
    piet::VarInfo v;
    v.name = n;
    v.dtype = std::string(n) == "s12" ? "factor" : "f64";
    card.push_back(v);
  }
  bool byteKey = false, s12Key = false;
  std::string dblFmt, fltFmt;
  for (const auto& p : session.hbinMetaPatch(card)) {
    if (p.name == "e_byte" && p.format.has_value()) byteKey = true;
    if (p.name == "s12" && p.format.has_value()) s12Key = true;
    if (p.name == "e_double" && p.format.has_value()) dblFmt = *p.format;
    if (p.name == "e_float" && p.format.has_value()) fltFmt = *p.format;
  }
  afCheck(!byteKey && !s12Key && dblFmt == "%9.0g" && fltFmt == "%6.2f",
          "W2AF BUG-198 law CONTROL: hbinMetaPatch writes only declared formats (byte/str12 "
          "defaults are derived from the type, never written as if declared)");
  run(session,
      "tempfile tfE\nquietly save `tfE'\nclear\nquietly set obs 1\ngenerate z = 1\n"
      "quietly use `tfE', clear\n");
  afCheck(fmtOf(session, "e_byte") == "%8.0g" && fmtOf(session, "e_long") == "%12.0g" &&
              fmtOf(session, "s12") == "%12s",
          "W2AF persistence: defaults come back after a tempfile save/use, derived from the carried "
          "type (w2p_estim_ev.log:1866 E_byte_reload) -- main %9.0g. Got " +
              fmtOf(session, "e_byte") + "/" + fmtOf(session, "e_long") + "/" + fmtOf(session, "s12"));
  afCheck(fmtOf(session, "e_double") == "%9.0g" && fmtOf(session, "e_float") == "%6.2f" &&
              fmtOf(session, "e_default") == "%9.0g",
          "W2AF persistence CONTROL: declared formats survive and win over the type default "
          "(w2_prog_controlflow.log:1354 pc_tf_use_formats; w2p_estim_ev.log:1883 E_default_reload)");
  run(session, "preserve\nformat e_byte %5.1f\nrestore\n");
  afCheck(fmtOf(session, "e_byte") == "%8.0g",
          "W2AF persistence: restore brings the byte default back (w2p_estim_ev.log:1725) -- main "
          "%9.0g. Got " + fmtOf(session, "e_byte"));
}

void testFormatCommand(const std::string& workdir) {
  piet::SessionState session(workdir);
  run(session, kCreate);
  run(session, "generate double dt = mdy(1, _n, 2020)\nformat %td dt\n");
  afCheck(fmtOf(session, "dt") == "%td",
          "W2AF format: the reversed `format %td dt` sets %td ([D] format: the varlist and %fmt may come in "
          "either order; paper 069 `format %td c_dob`) -- main r(1). "
          "Got " + fmtOf(session, "dt"));
  run(session, "generate double pa = 1\ngenerate double pb = 2\nformat %9.2f p*\n");
  afCheck(fmtOf(session, "pa") == "%9.2f" && fmtOf(session, "pb") == "%9.2f",
          "W2AF format: a reversed wildcard (paper 044 `format %9.2f percent*`) -- main r(1)");
  run(session, "format e_long %tmMon_CCYY\nformat e_int %12.3fc\n");
  afCheck(fmtOf(session, "e_long") == "%tmMon_CCYY" && fmtOf(session, "e_int") == "%12.3fc",
          "W2AF format CONTROL: corpus masks and comma formats are kept as typed "
          "(w2_prog_controlflow.log:162 pc_fix_meta)");
  afCheck(rcOf(session, "format e_double %zz") == "120" && fmtOf(session, "e_double") == "%10.0g",
          "W2AF format CONTROL: an invalid %fmt is r(120) and leaves the format unchanged "
          "(StataBE 19.5 cap.log A_zz_rc)");
  afCheck(rcOf(session, "format e_double %9.12f") == "120",
          "W2AF format: %9.12f is not a format ([D] format: `d must be less than w`) -- main rc 0");
  afCheck(rcOf(session, "format e_double %12s") == "120" &&
              rcOf(session, "format s12 %9.2f") == "120" && fmtOf(session, "s12") == "%12s",
          "W2AF format: a string format on a numeric variable and a numeric one on a string "
          "variable are refused r(120) (w2af_format_lifecycle.log af_fmt_str_on_num, "
          "af_fmt_num_on_str) -- main accepted both");
  afCheck(rcOf(session, "format nosuchvar %9.2f") == "111",
          "W2AF format: a missing variable is r(111) -- main rc 1");
}

void testCarriers(const std::string& workdir) {
  piet::SessionState session(workdir);
  run(session, "clear\nquietly set obs 3\ngenerate float y = _n/3\nrecast double y\n"
               "generate byte b1 = 1\nreplace b1 = 1.5 in 1\n");
  afCheck(typeOf(session, "y") == "double" && fmtOf(session, "y") == "%9.0g",
          "W2AF carriers: `recast double` of a float keeps %9.0g "
          "(internal capture exprfns_contract_oracle.log:735) -- the resolver alone "
          "would derive %10.0g from the new type. Got " + fmtOf(session, "y"));
  afCheck(typeOf(session, "b1") == "float" && fmtOf(session, "b1") == "%8.0g",
          "W2AF carriers CONTROL: a widening replace keeps the byte's %8.0g "
          "(exprfns_contract_oracle.log:736)");
  run(session, "generate a = _n*1.5\ntostring a, generate(a_s) usedisplayformat\n");
  afCheck(fmtOf(session, "a_s") == "%9s",
          "W2AF FU-947: tostring's str# result answers %9s through `: format`, as describe does "
          "-- main %9.0g. Got " + fmtOf(session, "a_s"));
}

// The creators the capture grades (internal capture
// w2af_format_lifecycle.log, StataNow 19.5).
void testCreators(const std::string& workdir) {
  piet::SessionState session(workdir);
  run(session,
      "clear\nquietly set obs 3\ngenerate byte key = _n\ngenerate double uval = _n/7\n"
      "format uval %7.3f\ntempfile tu\nquietly save `tu'\n"
      "clear\nquietly set obs 6\ngenerate byte grp = mod(_n, 2)\ngenerate byte key = mod(_n, 4)\n"
      "generate str12 s12 = \"x\" + string(_n)\ngenerate long v_l = 100000*_n\n"
      "generate byte v_b = _n\n");
  run(session, "generate su2 = \"y\" + \"x\"\ngenerate su14 = \"abcdefghijklm\" + string(_n)\n");
  afCheck(typeOf(session, "su2") == "str2" && fmtOf(session, "su2") == "%9s" &&
              typeOf(session, "su14") == "str14" && fmtOf(session, "su14") == "%14s",
          "W2AF FU-936: an untyped string generate is str<longest> and its default format follows "
          "(af_gen_str_untyped2 `str2|%9s`, af_gen_str_untyped14 `str14|%14s`) -- main r(101)/%9s. "
          "Got " + typeOf(session, "su2") + "/" + typeOf(session, "su14") + "/" +
              fmtOf(session, "su14"));
  run(session, "tostring v_b, generate(ts_i)\ndestring ts_i, generate(ds_i)\n"
               "encode s12, generate(en12)\n");
  afCheck(typeOf(session, "ds_i") == "byte" && fmtOf(session, "ds_i") == "%10.0g",
          "W2AF destring: gen double + compress keeps %10.0g (af_destring_gen `int|%10.0g`, "
          "[D] destring: created as double, then compressed) -- main %8.0g from the compressed type. Got " +
              typeOf(session, "ds_i") + "/" + fmtOf(session, "ds_i"));
  afCheck(typeOf(session, "en12") == "long" && fmtOf(session, "en12") == "%8.0g",
          "W2AF encode: long displayed %8.0g (af_encode `long|%8.0g`) -- main float/%9.0g. Got " +
              typeOf(session, "en12") + "/" + fmtOf(session, "en12"));
  run(session, "generate str4 rc_s = \"ab\"\nrecast str20 rc_s\n");
  afCheck(typeOf(session, "rc_s") == "str20" && fmtOf(session, "rc_s") == "%20s",
          "W2AF recast: a string's default format follows its new width (af_recast_str4_str20 "
          "`str20|%20s`) -- the numeric pin kept %9s. Got " + fmtOf(session, "rc_s"));
  const ojson cp = run(session,
                       "generate float cp_f = _n\ngenerate double cp_d = 1000*_n\n"
                       "generate long cp_l = _n\ngenerate str20 cp_s = \"ab\"\n"
                       "compress cp_f cp_d cp_l cp_s\n");
  afCheck(typeOf(session, "cp_f") == "byte" && fmtOf(session, "cp_f") == "%9.0g" &&
              typeOf(session, "cp_d") == "int" && fmtOf(session, "cp_d") == "%10.0g" &&
              typeOf(session, "cp_l") == "byte" && fmtOf(session, "cp_l") == "%12.0g" &&
              typeOf(session, "cp_s") == "str2" && fmtOf(session, "cp_s") == "%9s",
          "W2AF compress: demotes the storage type, numerics keep their format and a string's "
          "default follows (af_compress `byte|%9.0g|int|%10.0g|byte|%12.0g|str2|%9s`) -- main "
          "no-op. Got " + typeOf(session, "cp_f") + "/" + typeOf(session, "cp_d") + "/" +
              typeOf(session, "cp_s") + "/" + fmtOf(session, "cp_s"));
  bool savedLine = false;
  for (const auto& b : cp)
    if (b.is_object() && b.value("text", std::string()) == "  (180 bytes saved)") savedLine = true;
  afCheck(savedLine,
          "W2AF compress: Stata's summary line `  (180 bytes saved)` (w2af_format_lifecycle.log:767) "
          "-- main printed nothing");
  // The messages are command output: `capture noisily` and `noisily` print them
  // (w2af_format_lifecycle.log:762-767, w2_geo_strl.log:228-232), `quietly` and
  // `capture` do not, and the demotion happens either way. As "echo" blocks the
  // prefix filter inverted all four.
  auto savedLines = [](const ojson& blocks) {
    int n = 0;
    for (const auto& b : blocks)
      if (b.is_object() && b.value("type", std::string()) != "echo" &&
          b.value("text", std::string()) == "  (42 bytes saved)")
        ++n;
    return n;
  };
  run(session, "generate double cq_cn = _n\ngenerate double cq_n = _n\n"
               "generate double cq_q = _n\ngenerate double cq_c = _n\n");
  const int sCapNoi = savedLines(run(session, "capture noisily compress cq_cn\n"));
  const int sNoi = savedLines(run(session, "noisily compress cq_n\n"));
  const int sQui = savedLines(run(session, "quietly compress cq_q\n"));
  const int sCap = savedLines(run(session, "capture compress cq_c\n"));
  afCheck(sCapNoi == 1 && sNoi == 1,
          "W2AF compress: `capture noisily compress` and `noisily compress` print Stata's summary "
          "line (w2af_format_lifecycle.log:762-767) -- the echo-typed block was dropped. Got " +
              std::to_string(sCapNoi) + "/" + std::to_string(sNoi));
  afCheck(sQui == 0 && sCap == 0,
          "W2AF compress: `quietly compress` and `capture compress` print nothing -- the echo-typed "
          "block was kept. Got " + std::to_string(sQui) + "/" + std::to_string(sCap));
  afCheck(typeOf(session, "cq_cn") == "byte" && typeOf(session, "cq_n") == "byte" &&
              typeOf(session, "cq_q") == "byte" && typeOf(session, "cq_c") == "byte",
          "W2AF compress: the demotion happens under every prefix (af_compress) -- Got " +
              typeOf(session, "cq_q") + "/" + typeOf(session, "cq_c"));
  run(session, "preserve\nmerge m:1 key using `tu', keep(master match)\n");
  const std::string mType = typeOf(session, "_merge"), mFmt = fmtOf(session, "_merge"),
                    uFmt = fmtOf(session, "uval");
  run(session, "restore\n");
  afCheck(mType == "byte" && mFmt == "%23.0g" && uFmt == "%7.3f",
          "W2AF merge: _merge is byte %23.0g and a merged-in variable keeps the using file's format "
          "(af_merge_merge `byte|%23.0g|double|%7.3f`) -- main float/%9.0g and %10.0g. Got " +
              mType + "/" + mFmt + "/" + uFmt);
  run(session, "preserve\ngenerate double dt = mdy(1, _n, 2020)\nformat dt %td\n"
               "collapse (mean) dt v_b (sum) sm=v_b (count) cn=v_b (max) mx=v_l, by(grp)\n");
  const std::string c1 = typeOf(session, "v_b") + "|" + fmtOf(session, "v_b") + "|" +
                         typeOf(session, "sm") + "|" + fmtOf(session, "sm") + "|" +
                         typeOf(session, "cn") + "|" + fmtOf(session, "cn") + "|" +
                         typeOf(session, "mx") + "|" + fmtOf(session, "mx") + "|" +
                         fmtOf(session, "dt");
  run(session, "restore\n");
  afCheck(c1 == "float|%8.0g|double|%8.0g|long|%8.0g|long|%12.0g|%td",
          "W2AF collapse: types per statistic, formats from the source (af_collapse) "
          "-- main kept v_b byte and gave the rest float %9.0g. Got " +
              c1);
}

}  // namespace

void testW2afFormat() {
  std::printf("[selftest] --- W2AF format lifecycle: one display-format resolver ---\n");
  namespace fs = std::filesystem;
  const std::string workdir =
      (fs::temp_directory_path() / "piet_native_selftest_w2afformat").u8string();
  std::error_code ec;
  fs::remove_all(workdir, ec);
  fs::create_directories(workdir, ec);

  testDefaultsByType(workdir);
  testPersistence(workdir);
  testFormatCommand(workdir);
  testCarriers(workdir);
  testCreators(workdir);

  fs::remove_all(workdir, ec);
  std::printf("[selftest] W2AF format lifecycle: %d checks\n", gAfChecks);
}
