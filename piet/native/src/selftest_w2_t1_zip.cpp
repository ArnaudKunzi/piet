// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// AER tier-1 (2026-10-28), lane ZIP (unzipfile zipfile zipuse zipsave): checks against
// tests/eval/golden/aer_w2/w2ax_t1_official.log (arms zip_*) / w2ax_t1_community.log (arms
// zipsave_*/zipuse_*), StataBE 19.5, and the lane's own StataBE probes zp1/zp2 (scratch
// lane-zipmat/zp/zp*.log, 2026-10-28). Every check names the arm whose record it mirrors; the
// fixtures repeat the oracle do-files' set-up lines in a fresh directory that is the session's
// working directory. Archives are read back through zip_io.h, files through the filesystem,
// data through `display`, and rc through eval::coarseRc -- what a do-file's `_rc` sees.
#include "selftest_w2.h"

#include <cstdio>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <system_error>
#include <vector>

#include "piet/dispatcher.h"
#include "piet/eval/errors.h"
#include "piet/session_state.h"
#include "vendor/nlohmann/json.hpp"
#include "zip_io.h"

namespace {

namespace fs = std::filesystem;

int gT1ZipChecks = 0;
void zipCheck(bool cond, const std::string& label) {
  ++gT1ZipChecks;
  w2check(cond, "T1 Zip " + label);
}

// rc and display text of ONE dispatch of a line.
struct Out {
  std::string rc = "0";
  std::string text;
};
Out outOf(piet::SessionState& session, const std::string& cmd) {
  Out o;
  try {
    for (const auto& b : piet::dispatch(cmd, session)) {
      if (!b.is_object()) continue;
      const std::string type = b.value("type", std::string());
      if (type == "error") o.rc = piet::eval::coarseRc(b);
      if (type == "display") o.text += b.value("text", std::string());
    }
  } catch (const std::exception& e) {
    o.rc = std::string("EXC:") + e.what();
  }
  return o;
}
std::string rcOf(piet::SessionState& s, const std::string& cmd) { return outOf(s, cmd).rc; }

// The message of the last error block a dispatched line produced ("" when none).
std::string errOf(piet::SessionState& s, const std::string& cmd) {
  std::string msg;
  try {
    for (const auto& b : piet::dispatch(cmd, s))
      if (b.is_object() && b.value("type", std::string()) == "error")
        msg = b.value("message", std::string());
  } catch (const std::exception& e) {
    msg = std::string("EXC:") + e.what();
  }
  return msg;
}

// `display EXP`, without the trailing newline.
std::string shown(piet::SessionState& s, const std::string& exp) {
  std::string t = outOf(s, "display " + exp).text;
  while (!t.empty() && (t.back() == '\n' || t.back() == '\r')) t.pop_back();
  return t;
}

void run(piet::SessionState& s, std::initializer_list<const char*> lines) {
  for (const char* l : lines) {
    try {
      piet::dispatch(l, s);
    } catch (const std::exception&) {
    }
  }
}

void writeFile(const std::string& path, const std::string& bytes) {
  std::ofstream f(path, std::ios::binary | std::ios::trunc);
  f << bytes;
}
std::string readFile(const std::string& path) {
  std::ifstream f(path, std::ios::binary);
  if (!f) return "<absent>";
  return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}
bool there(const std::string& path) {
  std::error_code ec;
  return fs::exists(path, ec);
}

std::string entryNames(const std::string& zip) {
  std::string out;
  try {
    for (const auto& e : piet::zipio::readEntries(zip)) out += (out.empty() ? "" : " ") + e.name;
  } catch (const std::exception& e) {
    return std::string("EXC:") + e.what();
  }
  return out;
}

// t1deflate.txt as `zip -9 -X` (Info-ZIP, macOS) archived it on 2026-10-28: one entry,
// method 8 (DEFLATE), 280 bytes in 42 (`unzip -lv`: Defl:X 85%, CRC 0f486268).
const unsigned char kDeflateZip[] = {
    0x50, 0x4b, 0x03, 0x04, 0x14, 0x00, 0x02, 0x00, 0x08, 0x00, 0x3d, 0x3d,
    0x5c, 0x5d, 0x68, 0x62, 0x48, 0x0f, 0x2a, 0x00, 0x00, 0x00, 0x18, 0x01,
    0x00, 0x00, 0x0d, 0x00, 0x00, 0x00, 0x74, 0x31, 0x64, 0x65, 0x66, 0x6c,
    0x61, 0x74, 0x65, 0x2e, 0x74, 0x78, 0x74, 0x4b, 0x49, 0x4d, 0xcb, 0x49,
    0x2c, 0x49, 0x55, 0xc8, 0x4d, 0x55, 0x48, 0xc1, 0xca, 0x34, 0xe4, 0x4a,
    0x21, 0xa4, 0xc4, 0x88, 0xb0, 0x12, 0x63, 0xc2, 0x4a, 0x4c, 0x08, 0x2b,
    0x31, 0x25, 0xac, 0xc4, 0x8c, 0xb0, 0x12, 0x73, 0xc2, 0x4a, 0x2c, 0xb8,
    0x00, 0x50, 0x4b, 0x01, 0x02, 0x1e, 0x03, 0x14, 0x00, 0x02, 0x00, 0x08,
    0x00, 0x3d, 0x3d, 0x5c, 0x5d, 0x68, 0x62, 0x48, 0x0f, 0x2a, 0x00, 0x00,
    0x00, 0x18, 0x01, 0x00, 0x00, 0x0d, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x01, 0x00, 0x00, 0x00, 0xa4, 0x81, 0x00, 0x00, 0x00, 0x00, 0x74,
    0x31, 0x64, 0x65, 0x66, 0x6c, 0x61, 0x74, 0x65, 0x2e, 0x74, 0x78, 0x74,
    0x50, 0x4b, 0x05, 0x06, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x01, 0x00,
    0x3b, 0x00, 0x00, 0x00, 0x55, 0x00, 0x00, 0x00, 0x00, 0x00,
};

std::string deflateSource() {
  std::string s;
  for (int i = 1; i <= 8; ++i) s += "deflate me deflate me deflate me " + std::to_string(i) + "\n";
  return s;
}

// No scratch file of the dataset verbs outlives the command (the probe's ORPHAN 0).
bool noZipScratch(const piet::SessionState& s) {
  std::error_code ec;
  for (fs::directory_iterator it(s.workdir(), ec), end; !ec && it != end; it.increment(ec)) {
    const std::string nm = it->path().filename().string();
    if (nm.rfind("zipuse_", 0) == 0 || nm.rfind("zipsave_", 0) == 0) return false;
  }
  return true;
}

void zipfileUnzipArms(const std::string& workdir, const std::string& dir) {
  piet::SessionState s(workdir);
  s.cwd = dir;
  writeFile(dir + "/t1z1.txt", "alpha\n");
  writeFile(dir + "/t1z2.txt", "beta beta\n");
  // zip_zipfile_basic: PREC|zip_zipfile_basic|0|0| -- and the entries are the two files'
  // exact bytes, in the order named (round trip through the reader).
  {
    const Out o = outOf(s, "zipfile t1z1.txt t1z2.txt, saving(t1zz)");
    bool bytesOk = false;
    try {
      const auto es = piet::zipio::readEntries(dir + "/t1zz.zip");
      bytesOk = es.size() == 2 && es[0].name == "t1z1.txt" && es[0].data == "alpha\n" &&
                es[1].name == "t1z2.txt" && es[1].data == "beta beta\n";
    } catch (const std::exception&) {
    }
    zipCheck(o.rc == "0" && bytesOk &&
                 o.text.find("    adding: t1z1.txt (stored 0%)") != std::string::npos &&
                 o.text.find("       archived:  2") != std::string::npos,
             "zip_zipfile_basic (rc 0, t1zz.zip holds both files byte-exact, adding: lines)");
  }
  // zip_zipfile_noreplace 130 / zip_zipfile_replace 0 / zip_zipfile_nosaving 198 /
  // zip_zipfile_missingfile 198.
  zipCheck(rcOf(s, "zipfile t1z1.txt t1z2.txt, saving(t1zz)") == "130" &&
               rcOf(s, "zipfile t1z1.txt t1z2.txt, saving(t1zz, replace)") == "0" &&
               rcOf(s, "zipfile t1z1.txt") == "198" &&
               rcOf(s, "zipfile t1nosuch.txt, saving(t1zn, replace)") == "198" &&
               !there(dir + "/t1zn.zip"),
           "zip_zipfile_noreplace 130 / replace 0 / nosaving 198 / missingfile 198");
  // zip_unzip_basic + zip_unzip_content: erase both, unzip, both back with their bytes.
  {
    std::error_code ec;
    fs::remove(dir + "/t1z1.txt", ec);
    fs::remove(dir + "/t1z2.txt", ec);
    const Out o = outOf(s, "unzipfile t1zz.zip");
    zipCheck(o.rc == "0" && readFile(dir + "/t1z1.txt") == "alpha\n" &&
                 readFile(dir + "/t1z2.txt") == "beta beta\n" &&
                 o.text.find("    inflating: t1z2.txt") != std::string::npos &&
                 o.text.find("successfully unzipped t1zz.zip to current directory") !=
                     std::string::npos,
             "zip_unzip_basic / zip_unzip_content (beta beta)");
  }
  // zip_unzip_noreplace: rc 0, each file skipped with Stata's line, the file left alone.
  {
    writeFile(dir + "/t1z2.txt", "changed\n");
    const Out o = outOf(s, "unzipfile t1zz.zip");
    zipCheck(o.rc == "0" && readFile(dir + "/t1z2.txt") == "changed\n" &&
                 o.text.find("    can't overwrite \"t1z2.txt\"; skipping") != std::string::npos &&
                 o.text.find("        skipped:  2") != std::string::npos,
             "zip_unzip_noreplace (rc 0, can't overwrite ...; skipping, file untouched)");
  }
  // zip_unzip_replace / zip_unzip_noext / zip_unzip_quoted (all rc 0), zip_unzip_missing 601.
  {
    const std::string r1 = rcOf(s, "unzipfile t1zz.zip, replace");
    const bool restored = readFile(dir + "/t1z2.txt") == "beta beta\n";
    zipCheck(r1 == "0" && restored && rcOf(s, "unzipfile t1zz, replace") == "0" &&
                 rcOf(s, "unzipfile \"t1zz.zip\", replace") == "0" &&
                 rcOf(s, "unzipfile t1nosuch.zip") == "601",
             "zip_unzip_replace / noext / quoted rc 0, zip_unzip_missing r(601)");
  }
  // zp1 evil/abs: StataBE writes them; this build refuses the archive by name and writes
  // nothing at all (not even the archive's harmless first entry).
  {
    writeFile(dir + "/evil.zip",
              piet::zipio::build({{"ok.txt", "fine\n"}, {"../t1evil.txt", "x\n"}}));
    writeFile(dir + "/abs.zip", piet::zipio::build({{"/tmp/t1abs_piet.txt", "x\n"}}));
    const std::string e1 = rcOf(s, "unzipfile evil");
    const std::string e2 = rcOf(s, "unzipfile abs");
    zipCheck(e1 == "198" && e2 == "198" && !there(dir + "/ok.txt") &&
                 !there(fs::path(dir).parent_path().string() + "/t1evil.txt"),
             "zp1 evil/abs: a .. or absolute entry is refused, nothing extracted");
  }
  // zp1 dirzip / nodir: a directory is zipped as its files (t1d/f.txt), and unzipping creates
  // the directories an entry needs.
  {
    std::error_code ec;
    fs::create_directories(dir + "/t1d", ec);
    writeFile(dir + "/t1d/f.txt", "inner\n");
    const std::string r = rcOf(s, "zipfile t1d, saving(t1dz, replace)");
    const std::string names = entryNames(dir + "/t1dz.zip");
    fs::remove_all(dir + "/t1d", ec);
    const std::string u = rcOf(s, "unzipfile t1dz");
    zipCheck(r == "0" && names == "t1d/f.txt" && u == "0" &&
                 readFile(dir + "/t1d/f.txt") == "inner\n",
             "zp1 dirzip/dirunzip (entry t1d/f.txt, directory recreated)");
  }
  // zp1 dup r(692), zp1 bad r(603), zp1 suffix (saving(foo.bar) keeps its name).
  {
    writeFile(dir + "/bad.zip", "not a zip\n");
    zipCheck(rcOf(s, "zipfile t1z1.txt t1z1.txt, saving(t1dup, replace)") == "692" &&
                 rcOf(s, "unzipfile bad") == "603" &&
                 rcOf(s, "zipfile t1z1.txt, saving(foo.bar, replace)") == "0" &&
                 there(dir + "/foo.bar") && !there(dir + "/foo.bar.zip"),
             "zp1 dup r(692) / bad r(603) / suffix kept");
  }
  // DEFLATE: an Info-ZIP `zip -9` archive (method 8) unzips to the original bytes.
  {
    writeFile(dir + "/d.zip",
              std::string(reinterpret_cast<const char*>(kDeflateZip), sizeof(kDeflateZip)));
    const Out o = outOf(s, "unzipfile d, replace");
    zipCheck(o.rc == "0" && readFile(dir + "/t1deflate.txt") == deflateSource(),
             "DEFLATE archive (zip -9, 280 bytes in 42) inflates byte-exact");
  }
  // Review st_zf dotdot / hidden / emptyd / nodir: `sub/../f` is stored as `f`; `*.txt`
  // takes dot-files too; an empty directory is rc 0 with 0 archived; a target in a missing
  // directory is r(601).
  {
    std::error_code ec;
    fs::create_directories(dir + "/t1sub", ec);
    fs::create_directories(dir + "/t1empty", ec);
    writeFile(dir + "/.t1hidden.txt", "h\n");
    const std::string dd = rcOf(s, "zipfile t1sub/../t1z1.txt, saving(t1dd, replace)");
    const std::string ddNames = entryNames(dir + "/t1dd.zip");
    const std::string hid = rcOf(s, "zipfile *.txt, saving(t1hid, replace)");
    const std::string hidNames = entryNames(dir + "/t1hid.zip");
    const Out em = outOf(s, "zipfile t1empty, saving(t1emz, replace)");
    zipCheck(dd == "0" && ddNames == "t1z1.txt" && hid == "0" &&
                 hidNames.find(".t1hidden.txt") != std::string::npos && em.rc == "0" &&
                 em.text.find("       archived:  0") != std::string::npos &&
                 entryNames(dir + "/t1emz.zip").empty() &&
                 rcOf(s, "zipfile t1z1.txt, saving(t1nodir/x, replace)") == "601",
             "review st_zf dotdot (f.txt) / hidden / emptyd 0 archived / nodir r(601)");
  }
  // Review st_zf cl05 / cl50 rc 0, cl15 r(126), clx r(121), cl12 r(123): complevel() is a
  // one-element integer numlist.
  zipCheck(rcOf(s, "zipfile t1z1.txt, saving(t1cl, replace) complevel(05)") == "0" &&
               rcOf(s, "zipfile t1z1.txt, saving(t1cl, replace) complevel(5.0)") == "0" &&
               rcOf(s, "zipfile t1z1.txt, saving(t1cl, replace) complevel(1.5)") == "126" &&
               rcOf(s, "zipfile t1z1.txt, saving(t1cl, replace) complevel(x)") == "121" &&
               rcOf(s, "zipfile t1z1.txt, saving(t1cl, replace) complevel(1 2)") == "123",
           "review st_zf complevel() numlist codes (0 0 126 121 123)");
  // Review: a damaged archive is refused by the reader, r(603), with the reader's message
  // (st_zf drop: StataBE "invalid LOC header", 603). The local header of the first entry is
  // broken here, so the central directory points at garbage.
  {
    std::string bytes = readFile(dir + "/t1zz.zip");
    if (!bytes.empty()) bytes[0] = 'X';
    writeFile(dir + "/t1bad2.zip", bytes);
    std::error_code ec;
    fs::remove(dir + "/t1z1.txt", ec);
    const std::string msg = errOf(s, "unzipfile t1bad2.zip, replace");
    zipCheck(rcOf(s, "unzipfile t1bad2.zip, replace") == "603" &&
                 msg.find("invalid ZIP local header") != std::string::npos &&
                 !there(dir + "/t1z1.txt"),
             "damaged local header: r(603), the reader's message, nothing extracted");
  }
#ifndef _WIN32
  // Review st_lp: a directory link is listed and skipped ("error reading"), never followed --
  // a `self -> .` link no longer recurses. Review st_zf colon: on POSIX `a:b.txt` is a plain
  // file name and extracts.
  {
    std::error_code ec;
    fs::create_directories(dir + "/t1loop", ec);
    writeFile(dir + "/t1loop/a.txt", "a\n");
    fs::create_directory_symlink(".", dir + "/t1loop/self", ec);
    const Out o = outOf(s, "zipfile t1loop, saving(t1lp, replace)");
    writeFile(dir + "/t1colon.zip", piet::zipio::build({{"a:b.txt", "colon\n"}}));
    const std::string c = rcOf(s, "unzipfile t1colon, replace");
    zipCheck(o.rc == "0" && entryNames(dir + "/t1lp.zip") == "t1loop/a.txt" &&
                 o.text.find("    error reading t1loop/self; skipping") != std::string::npos &&
                 o.text.find("        skipped:  1") != std::string::npos && c == "0" &&
                 readFile(dir + "/a:b.txt") == "colon\n",
             "review st_lp symlink skipped (no recursion) / st_zf colon extracts on POSIX");
  }
#endif
}

void zipsaveZipuseArms(const std::string& workdir, const std::string& dir) {
  piet::SessionState s(workdir);
  s.cwd = dir;
  run(s, {"clear", "set obs 6", "generate a = _n * 2", "generate b = _n + 100",
          "generate str3 s = \"s\" + string(_n)", "label variable a \"the a\""});
  // zipsave_basic 0 / zipsave_name (t1zs.zip 601, t1zs.dta.zip 0) / zipsave_noreplace 602 /
  // zipsave_replace 0.
  {
    const Out o = outOf(s, "zipsave t1zs");
    const bool named = !there(dir + "/t1zs.zip") && there(dir + "/t1zs.dta.zip");
    zipCheck(o.rc == "0" && named &&
                 o.text == "data compressed with zip and saved in file t1zs.dta.zip" &&
                 rcOf(s, "zipsave t1zs") == "602" && rcOf(s, "zipsave t1zs, replace") == "0" &&
                 entryNames(dir + "/t1zs.dta.zip") == "t1zs.dta",
             "zipsave_basic / zipsave_name / zipsave_noreplace 602 / zipsave_replace");
  }
  // zipuse_basic: PREC|zipuse_basic|0|6|3|12|s2|the a|
  {
    run(s, {"clear"});
    const std::string rc = rcOf(s, "zipuse t1zs, clear");
    run(s, {"local t1vl : variable label a"});
    zipCheck(rc == "0" && shown(s, "_N") == "6" && shown(s, "c(k)") == "3" &&
                 shown(s, "a[6]") == "12" && shown(s, "s[2]") == "s2" &&
                 s.eval.locals.get("t1vl") == "the a",
             "zipuse_basic (6 obs, 3 vars, a[6] 12, s[2] s2, label `the a')");
  }
  // zipuse_varlist: PREC|zipuse_varlist|0|6|2|101|
  zipCheck(rcOf(s, "zipuse b s using t1zs, clear") == "0" && shown(s, "_N") == "6" &&
               shown(s, "c(k)") == "2" && shown(s, "b[1]") == "101",
           "zipuse_varlist (b s using: 2 vars, b[1] 101)");
  // zipuse_ext 601 (t1zs.zip is looked for as is), zipuse_noclear 0 (data unchanged since the
  // load), zipuse_missing 601.
  zipCheck(rcOf(s, "zipuse t1zs.zip, clear") == "601" && rcOf(s, "zipuse t1zs") == "0" &&
               rcOf(s, "zipuse t1nosuch, clear") == "601",
           "zipuse_ext 601 / zipuse_noclear 0 / zipuse_missing 601");
  // zp1 zu_changed: changed data without clear is r(4); zp2 zu_in: `b in 2/3 using` keeps
  // 2 obs, b[1] 102; zp2 use_badvar_left: a failed subset leaves no data (k 0, N 0).
  {
    run(s, {"replace a = 9 in 1"});
    const std::string ch = rcOf(s, "zipuse t1zs");
    const std::string in = rcOf(s, "zipuse b in 2/3 using t1zs, clear");
    const bool inOk = shown(s, "_N") == "2" && shown(s, "b[1]") == "102";
    const std::string bad = rcOf(s, "zipuse nosuchvar using t1zs, clear");
    zipCheck(ch == "4" && in == "0" && inOk && bad == "111" && shown(s, "c(k)") == "0",
             "zp1 zu_changed r(4) / zp2 zu_in / zp2 badvar r(111) leaves no data");
  }
  // Review st_zu zs1 / zs_noname: after zipsave the archive is the current file -- c(changed)
  // 0, c(filename) the archive -- and a name-less zipsave writes THERE, never over the .dta
  // the data came from.
  {
    run(s, {"clear", "set obs 3", "generate a = _n", "save t1base, replace", "use t1base, clear"});
    const std::string z1 = rcOf(s, "zipsave t1zsx");
    const std::string fn = shown(s, "c(filename)");
    const std::string changed = shown(s, "c(changed)");
    const std::string z2 = rcOf(s, "zipsave, replace");
    const bool baseIsDta = readFile(dir + "/t1base.dta").compare(0, 11, "<stata_dta>") == 0;
    zipCheck(z1 == "0" && fn.size() >= 13 && fn.compare(fn.size() - 13, 13, "t1zsx.dta.zip") == 0 &&
                 changed == "0" && z2 == "0" && baseIsDta &&
                 entryNames(dir + "/t1zsx.dta.zip") == "t1zsx.dta",
             "review st_zu zs1/zs_noname (c(filename) = archive, t1base.dta untouched)");
  }
  // Review st_zu finder: without dtafile() the FIRST file entry is the dataset (a Finder
  // archive's `__MACOSX/._x.dta` after it is ignored); st_zf notdta: an entry that is no
  // Stata dataset is r(610). No `use` memo survives keyed on the deleted scratch file.
  {
    std::string dta;
    try {
      for (const auto& e : piet::zipio::readEntries(dir + "/t1zs.dta.zip")) dta = e.data;
    } catch (const std::exception&) {
    }
    writeFile(dir + "/t1finder.zip",
              piet::zipio::build({{"t1x.dta", dta}, {"__MACOSX/._t1x.dta", "junk"}}));
    writeFile(dir + "/t1notes.zip", piet::zipio::build({{"notes.txt", "hello\n"}}));
    const std::string f = rcOf(s, "zipuse t1finder.zip, clear");
    const std::string n = shown(s, "_N");
    const bool noMemo = s.useCacheEntryCount() == 0;
    zipCheck(!dta.empty() && f == "0" && n == "6" && noMemo &&
                 rcOf(s, "zipuse t1notes.zip, clear") == "610",
             "review st_zu finder (first file entry, 6 obs) / no use memo / st_zf notdta 610");
  }
  // The dataset scratch files are gone (zipsave's .dta, zipuse's extracted .dta).
  zipCheck(noZipScratch(s), "zipsave/zipuse leave no scratch file in the workdir");
}

}  // namespace

void testW2T1Zip() {
  std::printf(
      "[selftest] --- T1 Zip: zipfile unzipfile zipsave zipuse (w2ax_t1_*.log, zp1/zp2) ---\n");
  const std::string base = (fs::temp_directory_path() / "piet_native_selftest_t1zip").u8string();
  std::error_code ec;
  fs::remove_all(base, ec);
  fs::create_directories(base + "/wd", ec);
  fs::create_directories(base + "/cwd/inner", ec);
  const std::string dir = base + "/cwd/inner";
  zipfileUnzipArms(base + "/wd", dir);
  zipsaveZipuseArms(base + "/wd", dir);
  fs::remove_all(base, ec);
  std::printf("[selftest] T1 Zip: %d checks\n", gT1ZipChecks);
}
