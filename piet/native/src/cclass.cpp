// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// cclass.cpp — see piet/cclass.h for the full design note (BETA Phase A4,
// BS-091/BS-101).
#include "piet/cclass.h"

#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <map>
#include <regex>

#include "io/win_compat.h"  // cecon::env_utf8 (BUG-051 wide %USERNAME%)
#include "piet/eval/macros.h"  // kVerbatimTick (substituteCRefs skips `c(name)')
#include "piet/eval/pyvalue.h"  // pythonFloatRepr (a numeric key's text)
#include "piet/bundle_paths.h"  // bundleResourcesDir (c(sysdir_sample))
#include "piet/handlers.h"  // runTempfile (c(tmpdir)), runSysdir/resolveUserPath (c(sysdir_*))

namespace piet {

namespace {

// c(sysdir_sample): the directory holding the bundled sample datasets.
// Resolution order mirrors `sysuse` exactly (dispatcher.cpp's path helpers,
// themselves mirroring main.cpp's runUiSmoke): bundle Resources/sample first,
// dev-tree piet/packaging/sample otherwise (piet::devTreePath, shared since
// BUG-1939).
std::string sampleDataDir() {
  if (auto res = bundleResourcesDir()) {
    std::string dir = *res + "/sample";
    std::error_code ec;
    if (std::filesystem::is_directory(cecon::to_native_path(dir), ec)) return dir;
  }
  return devTreePath("/piet/packaging/sample");  // BUG-1939: shared, executable-relative anchor
}

std::string detectOs() {
#if defined(__APPLE__)
  return "MacOSX";
#elif defined(_WIN32)
  return "Windows";
#else
  return "Unix";
#endif
}

// Coarse, compile-time-detected description — NOT byte-identical to real
// Stata's own machine-specific string (see cclass.h's "known simplification"
// note); good enough for a c(machine_type) that reads sensibly.
std::string detectMachineType() {
#if defined(__APPLE__)
#if defined(__aarch64__) || defined(__arm64__)
  return "Mac (Apple Silicon)";
#else
  return "Mac (Intel 64-bit)";
#endif
#elif defined(_WIN32)
  return "PC (Windows 64-bit)";
#elif defined(__aarch64__) || defined(__arm64__)
  return "PC (ARM 64-bit)";
#else
  return "PC (Unix 64-bit)";
#endif
}

std::string detectUsername() {
#ifdef _WIN32
  // A Korean %USERNAME% comes back from narrow getenv() as CP949 bytes, which
  // would land in c(username) as mojibake (and in any path built from it).
  // env_utf8 reads it wide and hands back UTF-8 like the rest of piet.
  return cecon::env_utf8("USERNAME");
#else
  const char* u = std::getenv("USER");
  if (!u || !*u) u = std::getenv("LOGNAME");
  return (u && *u) ? std::string(u) : std::string();
#endif
}

// current_date / current_time (BUG-091) — folded in verbatim from the
// pre-A4 display_hardening.cpp resolver (no behavior change to these two
// keys' formatting, only relocation into the single shared source).
std::string currentDateOrTime(const std::string& name) {
  std::time_t t = std::time(nullptr);
  std::tm tmv{};
#ifdef _WIN32
  // MinGW-w64/MSVC have no localtime_r; localtime_s is the same thread-safe
  // fill (argument order differs) and keeps the formatting below untouched.
  localtime_s(&tmv, &t);
#else
  localtime_r(&t, &tmv);
#endif
  char buf[32];
  if (name == "current_date") {
    static const char* const kMon[12] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                          "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    int mon = (tmv.tm_mon >= 0 && tmv.tm_mon < 12) ? tmv.tm_mon : 0;
    std::snprintf(buf, sizeof(buf), "%2d %s %04d", tmv.tm_mday, kMon[mon], tmv.tm_year + 1900);
  } else {  // current_time
    std::snprintf(buf, sizeof(buf), "%02d:%02d:%02d", tmv.tm_hour, tmv.tm_min, tmv.tm_sec);
  }
  return std::string(buf);
}

// A numeric key's text, ready to splice into an expression: an integer as its
// digits, anything else as the shortest text that reads back to the same double
// (c(maxdouble) 8.98846567431158e+307, every bit of it; the delimited form's
// macro text is SessionState::macroCValue's job).
std::string numText(double v) {
  if (std::fabs(v) < 1e15 && v == std::floor(v)) return std::to_string(static_cast<long long>(v));
  return eval::pythonFloatRepr(v);
}

// BUG-1330: the numeric constants of StataBE's c() (StataNow 19.5 BE, `creturn
// list` and %21x of each, 2026-11-03): the storage-type limits, which are
// PIET's own (data/missing.h, the .dta writer), and pi. c(stata_version) is the
// version this build reproduces.
std::optional<double> constantCValue(const std::string& name) {
  static const std::map<std::string, double> kConst = {
      {"pi", M_PI},                                 // +1.921fb54442d18X+001
      {"maxdouble", 0x1.fffffffffffffp+1022},       // +1.fffffffffffffX+3fe
      {"mindouble", -0x1.fffffffffffffp+1022},
      {"epsdouble", 0x1p-52},                       // +1.0000000000000X-034
      {"smallestdouble", 0x1p-1022},                // +1.0000000000000X-3fe
      {"maxfloat", 0x1.fffffep+126},                // +1.fffffe0000000X+07e
      {"minfloat", -0x1.fffffep+126},
      {"epsfloat", 0x1p-23},                        // +1.0000000000000X-017
      {"maxlong", 2147483620.0},     {"minlong", -2147483647.0},
      {"maxint", 32740.0},           {"minint", -32767.0},
      {"maxbyte", 100.0},            {"minbyte", -127.0},
      {"maxstrvarlen", 2045.0},      {"maxstrlvarlen", 2000000000.0},
      {"maxvlabellen", 32000.0},     {"stata_version", 19.5},
  };
  auto it = kConst.find(name);
  if (it == kConst.end()) return std::nullopt;
  return it->second;
}

// Settings PIET accepts and ignores (dispatcher.cpp's generic `set` arm): StataBE
// 19.5's own default, which is what PIET runs with, until a `set NAME` of the
// same name has run. After that the value Stata would report is one PIET did not
// keep, so the key is refused by name (kSetMarkerPrefix) rather than answered
// with a default the do-file just changed.
std::optional<CValue> ignoredSettingCValue(const std::string& name, SessionState& session) {
  static const std::map<std::string, CValue> kDefaults = {
      {"scheme", {true, "stcolor"}},   {"rmsg", {true, "off"}},
      {"trace", {true, "off"}},        {"varabbrev", {true, "on"}},
      {"fredkey", {true, ""}},         {"matsize", {false, "400"}},
      {"maxvar", {false, "2048"}},     {"segmentsize", {false, "33554432"}},
      {"max_memory", {false, "."}},
  };
  auto it = kDefaults.find(name);
  if (it == kDefaults.end()) return std::nullopt;
  if (const std::string* set = session.eval.globals.find(std::string(kSetMarkerPrefix) + name)) {
    // `set matsize #` keeps its checked number and `set rmsg|trace|varabbrev`
    // their checked on/off (dispatcher.cpp), which is what StataBE reports
    // although PIET ignores the setting: 800 after `set matsize 800`, on after
    // `set rm on`.
    if (name == "matsize") return CValue{false, *set};
    if (name == "rmsg" || name == "trace" || name == "varabbrev") return CValue{true, *set};
    return std::nullopt;
  }
  return it->second;
}

// The fixed word lists (`foreach y in `c(ALPHA)'`, corpus 100).
std::optional<std::string> wordListCValue(const std::string& name) {
  static const std::map<std::string, std::string> kLists = {
      {"alpha", "a b c d e f g h i j k l m n o p q r s t u v w x y z"},
      {"ALPHA", "A B C D E F G H I J K L M N O P Q R S T U V W X Y Z"},
      {"Mons", "Jan Feb Mar Apr May Jun Jul Aug Sep Oct Nov Dec"},
      {"Months", "January February March April May June July August September October "
                 "November December"},
      {"Wdays", "Sun Mon Tue Wed Thu Fri Sat"},
      {"Weekdays", "Sunday Monday Tuesday Wednesday Thursday Friday Saturday"},
  };
  auto it = kLists.find(name);
  if (it == kLists.end()) return std::nullopt;
  return it->second;
}

}  // namespace

std::optional<CValue> resolveCValue(const std::string& name, SessionState& session) {
  // -- numeric keys ----------------------------------------------------
  if (name == "N" || name == "obs") {
    // c(obs) is accepted as a harmless synonym of c(N) — see cclass.h.
    Datastore* ds = session.datastore();
    return CValue{false, std::to_string(ds ? ds->nRows() : 0)};
  }
  if (name == "k") {
    Datastore* ds = session.datastore();
    return CValue{false, std::to_string(ds ? static_cast<long long>(ds->names().size()) : 0)};
  }
  if (name == "rc") {
    // Same value _rc resolves to (macros.cpp's evalArith bare-_rc rule) —
    // read straight from the same store so the two never disagree.
    return CValue{false, session.eval.locals.get("_rc", "0")};
  }
  if (name == "level") {
    return CValue{false, std::to_string(session.level)};
  }
  if (name == "seed") {
    // Under mt64 c(seed) is the generator's current state, the same text as
    // c(rngstate), not the number `set seed` was given (StataBE 19.5: after
    // `set seed 12345` it is `XAA0000000000003039...`, 5011 characters, and it
    // moves with every draw; BUG-1051). `set seed` reads it back.
    return CValue{true, session.seed.stream().state_string()};
  }
  if (name == "rngstate") {
    // The session stream's state in Stata's c(rngstate) layout (core/rng_mt64.h
    // state_string; `set rngstate` reads it back). 5011 characters under mt64.
    return CValue{true, session.seed.stream().state_string()};
  }
  if (name == "rng_current") {
    // The generator runiform() actually uses: mt64 under `set rng default`.
    return CValue{true, "mt64"};
  }
  if (name == "rng") {
    // The `set rng` setting (dispatcher.cpp), "default" until one is made.
    return CValue{true, session.rngSetting};
  }
  if (name == "changed") {
    return CValue{false, session.datasetChanged ? "1" : "0"};
  }
  // BUG-1463: the edition PIET answers as is Stata/BE, the one its results are
  // measured against: one processor (c(processors_mach) is `.` there), no MP, no
  // SE -- so `set processors `=min(4, c(processors_max))'' reads 1 before it is
  // refused as BE refuses it (dispatcher.cpp), and `if c(MP)` skips the setting.
  if (name == "processors" || name == "processors_lic" || name == "processors_max") {
    return CValue{false, "1"};
  }
  if (name == "processors_mach") return CValue{false, "."};
  if (name == "MP" || name == "SE") return CValue{false, "0"};
  if (name == "version" || name == "userversion") {
    // BUG-1330: the `version #` in force (kVersionLocal, kUserVersionLocal),
    // else this build's own 19.5. It was the constant 18 whatever version ran.
    const std::string* v =
        session.eval.locals.find(name == "version" ? kVersionLocal : kUserVersionLocal);
    return CValue{false, v != nullptr ? *v : std::string("19.5")};
  }
  if (auto k = constantCValue(name)) return CValue{false, numText(*k)};
  if (name == "obs_t") {
    // The smallest storage type that holds _N (StataBE: byte at 0..100, int to
    // 32740, long from 32741).
    Datastore* ds = session.datastore();
    const double n = ds ? static_cast<double>(ds->nRows()) : 0.0;
    return CValue{true, n <= 100 ? "byte" : n <= 32740 ? "int" : n <= 2147483620 ? "long" : "double"};
  }
  if (auto w = wordListCValue(name)) return CValue{true, *w};
  if (name == "dirsep") return CValue{true, "/"};  // "/" under Windows too ([P] creturn)
  if (name == "tmpdir") {
    // The directory `tempfile` names its files in, with Stata's trailing
    // separator; minted here when no tempfile has asked for it yet.
    nlohmann::ordered_json made = handlers::runTempfile({}, session);
    if (!session.runTempdir.has_value() || made.is_object()) return std::nullopt;
    return CValue{true, *session.runTempdir + "/"};
  }
  // PIET's own key, ahead of the sysdir_* arm below: `sysdir` has no SAMPLE entry.
  if (name == "sysdir_sample") return CValue{true, sampleDataDir()};
  if (name.rfind("sysdir_", 0) == 0) {
    // What PIET's own `sysdir` records (handlers_aer_env.cpp seedSysdir, then
    // `sysdir set`), `~` read as the home directory the way findfile reads it.
    (void)handlers::runSysdir("sysdir", {}, session);  // seeds the defaults
    std::string key = name.substr(7);
    for (char& ch : key) ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
    auto it = session.sysdir.find(key);
    if (it == session.sysdir.end() || it->second.empty()) return std::nullopt;
    return CValue{true, resolveUserPath(session, it->second)};
  }
  if (name == "graphics") return CValue{true, session.graphicsOff ? "off" : "on"};  // BS-047
  if (auto v = ignoredSettingCValue(name, session)) return v;

  // -- string keys -------------------------------------------------------
  if (name == "os") return CValue{true, detectOs()};
  if (name == "machine_type") return CValue{true, detectMachineType()};
  if (name == "username") return CValue{true, detectUsername()};
  if (name == "pwd") {
    return CValue{true, session.cwd.has_value() && !session.cwd->empty()
                             ? *session.cwd
                             : std::filesystem::current_path().u8string()};
  }
  if (name == "filename") return CValue{true, session.currentFile()};
  if (name == "edition" || name == "edition_real") return CValue{true, "BE"};  // BUG-1463
  // BUG-471 T2: c(type), the default storage type `set type` writes
  // (w2e_float_default.log settype_ctype_default :1236 "float",
  // settype_double_ctype :1262 "double").
  if (name == "type") return CValue{true, session.defaultStorageType};
  if (name == "more") return CValue{true, session.moreOn ? "on" : "off"};  // EM1
  if (name == "current_date" || name == "current_time") {
    return CValue{true, currentDateOrTime(name)};
  }

  return std::nullopt;  // outside this build's spine — honest gap.
}

std::string substituteCRefs(const std::string& exprIn, SessionState& session) {
  if (exprIn.find("c(") == std::string::npos) return exprIn;  // cheap common-case skip
  // BUG-1420: text inside a string literal is data. `local b = "c(N)" + "x"` is
  // c(N)x and `count if s == "c(N)"` matches the text in StataBE 19.5; spliced,
  // they read 3x and s == "3".
  std::vector<std::string> spans;
  const std::string expr = eval::protectLiterals(exprIn, &spans);
  static const std::regex reC(R"(\bc\(\s*([A-Za-z_]\w*)\s*\))");
  auto begin = std::sregex_iterator(expr.begin(), expr.end(), reC);
  auto end = std::sregex_iterator();
  if (begin == end) return exprIn;

  std::string out;
  out.reserve(expr.size());
  size_t last = 0;
  bool any = false;
  size_t scanned = 0;    // how far the quote state below has been read
  bool inQuote = false;  // BUG-1330: inside "..." c(name) is text, as in Stata
  for (auto it = begin; it != end; ++it) {
    const std::smatch& m = *it;
    for (; scanned < static_cast<size_t>(m.position(0)); ++scanned)
      if (expr[scanned] == '"') inQuote = !inQuote;
    if (inQuote) continue;
    // `c(name)' is a delimited MACRO reference (macros.cpp reCRefDelim, the
    // cValue hook): its value is spliced as text. Quoting it here as an
    // expression literal turned `local s "`c(rngstate)'"` into the compound
    // literal `"XAA..."', so `"`s'"' then carried the quotes (strlen 5011+6,
    // and L_cv_s1_rng's equality with e(rngstate) answered 0).
    const size_t at = static_cast<size_t>(m.position(0));
    const size_t after = at + static_cast<size_t>(m.length(0));
    if (at > 0 && (expr[at - 1] == '`' || expr[at - 1] == eval::kVerbatimTick) &&
        after < expr.size() && expr[after] == '\'')
      continue;
    auto resolved = resolveCValue(m.str(1), session);
    if (!resolved.has_value()) continue;  // leave THIS occurrence untouched
    any = true;
    out.append(expr, last, static_cast<size_t>(m.position(0)) - last);
    if (resolved->isString) {
      out += '"';
      out += resolved->text;
      out += '"';
    } else if (!resolved->text.empty() && resolved->text[0] == '-') {
      // A negative constant is one operand: c(minint)^2 is (-32767)^2.
      out += "(" + resolved->text + ")";
    } else {
      out += resolved->text;
    }
    last = static_cast<size_t>(m.position(0)) + static_cast<size_t>(m.length(0));
  }
  if (!any) return exprIn;
  out.append(expr, last, expr.size() - last);
  return eval::restoreLiterals(out, spans);
}

std::optional<std::string> unservedCRefError(const std::string& expr, std::string* rc) {
  if (expr.find("c(") == std::string::npos) return std::nullopt;
  static const std::regex reC(R"(\bc\(\s*([A-Za-z_]\w*)\s*\))");
  size_t i = 0;
  while (i < expr.size()) {
    const size_t q = expr.find('"', i);
    const std::string plain = expr.substr(i, q == std::string::npos ? std::string::npos : q - i);
    std::smatch m;
    if (std::regex_search(plain, m, reC)) {
      if (rc != nullptr) *rc = eval::cClassRefusalRc(m.str(1));
      return eval::cClassRefusalMessage(m.str(1));
    }
    if (q == std::string::npos) break;
    const size_t close = expr.find('"', q + 1);
    if (close == std::string::npos) break;
    i = close + 1;
  }
  return std::nullopt;
}

}  // namespace piet
