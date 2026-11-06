// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// R2 (giant-file split, 2026-09-01): continuation TU of handlers_data_verbs.cpp
// — a MOVE-ONLY cut along pre-existing anonymous-namespace boundaries. The
// preamble is the original file head verbatim; shared helpers come from
// handlers_data_verbs_internal.h.
// M3b: MI-066 data verbs (merge/reshape/append/duplicates/gsort/order/
// recode) via cobra 0.18.0's native CLI verbs. C++ ports of the
// corresponding piet/backend/piet/{merge_op,reshape,append,duplicates,
// gsort,order,recode}.py oracle modules — bug-for-bug where cobra 0.18.0
// and the Python backend agree; where they differ, the FROZEN 1.16.0
// corpus wins (this file pre-validates the oracle's own error conditions
// in C++ BEFORE shelling out to cobra, so error text matches byte-for-byte
// even where cobra's native error text differs — see each handler's notes).
#include "piet/handlers.h"
#include "piet/column_fetch.h"  // BUG-394: reshape long j-values via §38
#include "piet/translate.h"
#include "piet/stata_format.h"  // BUG-918: padMissingLeftJustify (tostring format())
#include "io/columnar.h"  // cecon::hbin_level_unescape: encode's escaped levels (BUG-1372)

#include <sys/stat.h>
#include <unistd.h>
#if defined(__APPLE__)
#include <sys/clonefile.h>  // BUG-138/L5-IOSAVE: O(1) same-volume save (runSaveData)
#endif
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>  // MoveFileExW / GetLastError (runSaveData's atomic replace)
#endif

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <cerrno>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <charconv>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <regex>
#include <set>
#include <sstream>
#include <system_error>
#include <unordered_set>
#include <vector>

#include "piet/cobra_runner.h"
#include "piet/counters.h"
#include "piet/eval/errors.h"
#include "piet/eval/exprfns.h"  // gFormat/applyStataFmt (BS-003 tostring default + usedisplayformat rendering)
#include "piet/eval/pyvalue.h"
#include "piet/inrange_filter.h"
#include "piet/mutation_result.h"
#include "piet/strutil.h"
#include "data/missing.h"
#include "xlsx.h"   // Stata `.` / `.a`-`.z` kinds — CSV writes the extended code
#include "piet/scratch_seq.h"
#include "piet/storage_type.h"  // stataDefaultFormat (tostring usedisplayformat)
#include "io/win_compat.h"  // cecon::native_exists / to_native_path (BUG-050 wide open)

// ---------------------------------------------------------------------------
// BUG-233 / FIX:OPTNET-ABBREV-SWEEP — this file's option vocabularies.
//
// Every set below used to be a function-local `static const` sitting next to its
// reject net, which meant nothing upstream of the handler could see it — and the
// handler's own `k == "fullname"` comparisons therefore ran against RAW keys
// while eval::rejectUnknownOptions had already ACCEPTED an abbreviation of them.
// That is BUG-231's mechanism, and it was live in nets across six files.
//
// Registering here (static init, before any dispatch) lets dispatcher.cpp's single
// resolution point canonicalize the keys BEFORE routing. The sets did not get
// copied: each net now binds a reference back to its registered entry, so there is
// still exactly one place each spelling is written.
#include "handlers_data_verbs_internal.h"

namespace piet::handlers {
using ojson = nlohmann::ordered_json;
using namespace dv_detail;

namespace {
// R2: re-binding of the option vocabularies this TU consumes. Registering the
// same token twice UNIONS the sets (documented, deliberate — errors.h), and a
// set unioned with itself is itself, so the single-registry law (BUG-233)
// holds with three TUs exactly as it did with one.
// reshape.ado's syntax line: I() J() ATwl() String favor(); `string` is read
// by runReshape (s/str/string, and j(t, string)).
const piet::eval::OptVocab& kVocabReshape = piet::eval::registerOptVocab(
    {"reshape"}, {{"i", "j", "string"},
     {"atwl", "favor"}});
// B4-W3b: `generate`, `update` and `replace` graduate from UNIMPLEMENTED to
// KNOWN — the engine grew all three on main 3a5d80e2 ("merge --update and the
// egen riders, against an oracle captured first") and this is the PIET side of
// that landing. The rest stay unimplemented and keep saying so.
// merge's syntax line: force, sorted, noreport and nonotes are read by
// runMerge (nonotes: merge appends no using notes in this build either way).
const piet::eval::OptVocab& kVocabMerge = piet::eval::registerOptVocab(
    {"merge"}, {{"keepusing", "nogenerate", "keep", "assert", "generate", "update", "replace",
                 "force", "sorted", "noreport", "nonotes", "nolabel"},
     {}});
}  // namespace

namespace {

// _parse_ivar_jvar: opts["i"]/["j"] are space-separated token strings
// (tokenizer stores paren-option values as raw text: `i(id year)` ->
// {"i": "id year"}).
std::pair<std::vector<std::string>, std::optional<std::string>> parseIvarJvar(
    const std::vector<std::pair<std::string, eval::OptValue>>& opts) {
  std::optional<std::string> iRaw, jRaw;
  bool iIsBool = false, jIsBool = false;
  for (const auto& [k, v] : opts) {
    if (k == "i") {
      if (v.isBool) iIsBool = true;
      else iRaw = v.strValue;
    }
    if (k == "j") {
      if (v.isBool) jIsBool = true;
      else jRaw = v.strValue;
    }
  }
  if (iIsBool || !iRaw.has_value() || iRaw->empty()) return {{}, std::nullopt};
  std::vector<std::string> iVars;
  {
    std::istringstream iss(*iRaw);
    std::string tok;
    while (iss >> tok) iVars.push_back(tok);
  }
  if (jIsBool || !jRaw.has_value() || jRaw->empty()) return {iVars, std::nullopt};
  std::istringstream jss(*jRaw);
  std::string jVar;
  jss >> jVar;
  return {iVars, jVar};
}

}  // namespace

namespace {
// Defined below (the merge/append helpers); runReshape types its results with them.
std::optional<std::string> appendPromotedType(const std::optional<std::string>& m,
                                              const std::optional<std::string>& u);
int strWidthOf(const std::string& t);
}  // namespace

ojson runReshape(const std::vector<std::string>& stubsIn,
                 const std::vector<std::pair<std::string, eval::OptValue>>& opts,
                 SessionState& session) {
  if (stubsIn.empty()) {
    return engineErrorBlock("reshape: specify direction: `reshape long ...' or `reshape wide ...'");
  }
  std::string direction = lower(stubsIn[0]);
  std::vector<std::string> stubs(stubsIn.begin() + 1, stubsIn.end());

  // BUG-105 (옵션망): reshape accepts only i()/j() — any other residual
  // option token is a named reject, not a silent no-op.
  // BUG-233: the one copy of this vocabulary now lives in the file-scope
  // registration block (see top of file); this binding keeps every existing
  // use — including the reject net below — reading the same set.
  const std::unordered_set<std::string>& kKnownReshapeOpts = kVocabReshape.known;
  // `String` abbreviates to s (reshape.ado's syntax line), below the net's
  // 2-character prefix floor: s / st / str ... string all name it.
  std::vector<std::pair<std::string, eval::OptValue>> optsC(opts);
  bool jString = false;
  for (auto& kv : optsC) {
    if (!kv.first.empty() && kv.first.size() <= 6 && std::string("string").compare(0, kv.first.size(), kv.first) == 0) {
      kv.first = "string";
      jString = true;
    }
  }
  if (auto err = eval::rejectUnknownOptions(optsC, kKnownReshapeOpts, kVocabReshape.unimplemented)) {
    return *err;
  }
  // j(t, string): the J() option's own `string` (reshape).
  for (auto& kv : optsC) {
    if (kv.first != "j" || kv.second.isBool) continue;
    const size_t comma = kv.second.strValue.find(',');
    if (comma == std::string::npos) continue;
    const std::string tail = lower(trim(kv.second.strValue.substr(comma + 1)));
    if (!tail.empty() && tail.size() <= 6 && std::string("string").compare(0, tail.size(), tail) == 0)
      jString = true;
    kv.second.strValue = trim(kv.second.strValue.substr(0, comma));
  }

  auto [iVars, jVar] = parseIvarJvar(optsC);
  if (iVars.empty()) {
    return engineErrorBlock(
        "reshape: i() option required — e.g. `reshape long inc, i(id) j(year)'");
  }
  if (!jVar.has_value()) {
    return engineErrorBlock(
        "reshape: j() option required — e.g. `reshape long inc, i(id) j(year)'");
  }
  if (direction != "long" && direction != "wide") {
    return engineErrorBlock("reshape: unknown direction `" + direction +
                            "'. Use `reshape long ...' or `reshape wide ...'.");
  }

  Datastore* ds = session.datastore();
  if (ds == nullptr) return engineErrorBlock("No dataset loaded.");
  const std::vector<std::string> allNames = ds->names();  // a copy: the head is replaced below
  auto hasName = [&](const std::string& n) {
    return std::find(allNames.begin(), allNames.end(), n) != allNames.end();
  };

  if (direction == "long") {
    for (const auto& v : iVars) {
      if (!hasName(v)) return engineErrorBlock("reshape long: id variable `" + v + "' not found");
    }
    if (stubs.empty()) return engineErrorBlock("reshape long: no stub variables specified");
  } else {
    if (stubs.empty()) return engineErrorBlock("reshape wide: no stub variables specified");
    for (const auto& v : iVars) {
      if (!hasName(v)) return engineErrorBlock("reshape wide: id variable `" + v + "' not found");
    }
    if (!hasName(*jVar)) {
      return engineErrorBlock("reshape wide: j variable `" + *jVar + "' not found");
    }
    for (const auto& s : stubs) {
      if (!hasName(s)) return engineErrorBlock("reshape wide: stub variable `" + s + "' not found");
    }
  }

  std::string joinedStubs, joinedI;
  for (size_t i = 0; i < stubs.size(); ++i) {
    if (i) joinedStubs += ",";
    joinedStubs += stubs[i];
  }
  for (size_t i = 0; i < iVars.size(); ++i) {
    if (i) joinedI += ",";
    joinedI += iVars[i];
  }

  int64_t nBeforeRows = ds->nRows();
  // What the result variables take from the source ones (reshape.ado
  // mkrtmpST/Recast :1425-1464 and the wide labels): long — a stub is typed
  // by the widest of its components, with no label and its default format
  // (dc_r_long_v3 p `float|%9.0g|[]`, v4 q int); wide — each new variable
  // keeps the long stub's type and format and is labelled "<j> <stub>"
  // (dc_r_wide_v2 p1 `float|%6.2f|[1 p]`).
  const std::vector<VarInfo> cardBefore = session.varlistWithMeta(ds);
  auto typeOf = [&](const std::string& nm) -> std::optional<std::string> {
    auto it = session.varMeta.find(nm);
    if (it != session.varMeta.end() && it->second.stataType.has_value() && !it->second.stataType->empty())
      return it->second.stataType;
    if (session.implicitFloatVars.count(nm)) return std::string("float");
    return std::nullopt;
  };
  std::map<std::string, std::vector<std::string>> stubComponents;  // long: stub -> its columns
  if (direction == "long") {
    for (const auto& nm : allNames) {
      size_t best = stubs.size();
      for (size_t si = 0; si < stubs.size(); ++si)
        if (nm.size() > stubs[si].size() && nm.compare(0, stubs[si].size(), stubs[si]) == 0 &&
            (best == stubs.size() || stubs[si].size() > stubs[best].size()))
          best = si;
      if (best != stubs.size()) stubComponents[stubs[best]].push_back(nm);
    }
  }
  struct WideStubMeta {
    std::optional<std::string> type;
    std::string format;
  };
  std::map<std::string, WideStubMeta> wideStubMeta;
  if (direction == "wide") {
    for (const auto& st : stubs) {
      WideStubMeta wm;
      wm.type = typeOf(st);
      for (const VarInfo& v : cardBefore)
        if (v.name == st) wm.format = session.displayFormat(v);
      wideStubMeta[st] = wm;
    }
  }
  // (cobra reshape --stata drops PIET's hidden `__piet_*` columns itself.)
  const std::string reshapeData = ds->path();
  std::string outPath = nextPath(session.workdir(), "reshape_" + direction);
  std::vector<std::string> args = {"reshape", direction,  "--data", reshapeData, "--stubs",
                                    joinedStubs, "--i",    joinedI,  "--j",      *jVar,
                                    "--stata", "--out",   outPath};
  if (jString) args.push_back("--string");
  CobraRunResult cr;
  {
    // BUG-394 (2026-09-02): onto the resident plane. The one stateless dependency
    // below — reading the post-reshape j column for `long` — now goes through the
    // §38 column fetch (one column, not the whole frame) with the materialized
    // head as the fallback, instead of opening the --out file that no longer exists.
    SessionState::ResidentEligible residentReshape(session);
    cr = runCobra(args);
  }
  if (!cr.ok) {
    CobraError ce = errorFromRaw(cr);
    // reshape.ado's refusals leave the data as they were (dc_r_long_nostr
    // r(498), dc_r_long_idup r(9), dc_r_wide_jmiss r(498), dc_r_wide_jdup
    // r(9), dc_r_wide_notconst r(9), dc_r_wide_nostr r(109)).
    for (const char* rc : {"498", "9", "109"}) {
      if (ce.code != std::string("reshape_") + rc) continue;
      // A wide r(9) leaves the data sorted the way reshape.ado's check sorted
      // them (the engine names that sort in the error's detail).
      if (direction == "wide" && ce.code == "reshape_9" && !ce.detail.empty()) {
        std::vector<std::string> sortKeys;
        std::istringstream in(ce.detail);
        for (std::string w; in >> w;) sortKeys.push_back(w);
        ojson sorted = runSort(sortKeys, session);
        if (sorted.is_object() && sorted.value("type", std::string()) == "error") return sorted;
      }
      return ojson(eval::errorBlock("engine_error", ce.what(), std::nullopt, std::nullopt,
                                    std::nullopt, std::string("r(") + rc + ")"));
    }
    if (ce.code == "missing_column")
      return ojson(eval::errorBlock("engine_error", ce.what(), std::nullopt, std::nullopt,
                                    std::nullopt, std::string("r(111)")));
    return engineErrorBlock(cobraErrorStr(ce), ce.code);
  }

  // j_values: reshape.py's all_suffixes / sorted unique j-values, formatted
  // as strings. The output hbin's j_var column HOLDS exactly these values
  // (long: the melted j column; wide: unaffected, so read from the ORIGINAL
  // dataset instead) — read back rather than re-deriving stub-suffix
  // discovery logic, avoiding a second bug-for-bug reimplementation.
  std::vector<std::string> jValues;
  {
    Datastore* srcForJ = (direction == "long") ? nullptr : ds;  // wide: j_var lives pre-reshape
    std::unique_ptr<Datastore> outDsHolder;
    Datastore* jDs = srcForJ;
    if (direction == "long") {
      session.load(outPath);  // adopt the result first (resident: a virtual head; stateless: the file)
      std::string jFetchPath;
      outDsHolder = fetchColumns(session, {*jVar}, session.workdir(), &jFetchPath);
      jDs = outDsHolder ? outDsHolder.get() : session.datastore();
    }
    if (jDs != nullptr && jDs->has(*jVar) && jDs->dtype(*jVar) == "factor") {
      // A string j: its distinct texts in byte order are the suffixes.
      std::set<std::string> texts;
      for (const auto& lv : jDs->varinfo(*jVar).levels)
        if (!lv.empty()) texts.insert(lv);
      jValues.assign(texts.begin(), texts.end());
    } else if (jDs != nullptr && jDs->has(*jVar) && jDs->dtype(*jVar) == "f64") {
      const double* d = jDs->col(*jVar).f64();
      int64_t count = jDs->col(*jVar).nbytes / static_cast<int64_t>(sizeof(double));
      std::vector<double> uniq;
      for (int64_t i = 0; i < count; ++i) {
        if (!std::isnan(d[i])) uniq.push_back(d[i]);
      }
      std::sort(uniq.begin(), uniq.end());
      uniq.erase(std::unique(uniq.begin(), uniq.end()), uniq.end());
      for (double v : uniq) {
        // reshape.py's j-value formatting: integer-valued -> plain int
        // string (matches _j_suffix's convention, str(int(v)) when
        // float(v)==int(float(v))).
        if (v == static_cast<double>(static_cast<long long>(v))) {
          jValues.push_back(std::to_string(static_cast<long long>(v)));
        } else {
          std::ostringstream oss;
          oss << v;
          jValues.push_back(oss.str());
        }
      }
    }
  }

  if (direction != "long") session.load(outPath);  // BUG-394: `long` adopted above
  Datastore* outDs = session.datastore();
  int64_t nAfterRows = outDs ? outDs->nRows() : 0;
  int64_t nWide = (direction == "long") ? nBeforeRows : nAfterRows;
  int64_t nLong = (direction == "long") ? nAfterRows : nBeforeRows;
  size_t k = jValues.size();

  // The result's storage types, formats and labels (see wideStubMeta above).
  {
    auto widen = [](const std::optional<std::string>& a, const std::optional<std::string>& b)
        -> std::optional<std::string> {
      if (!a.has_value()) return b;
      if (!b.has_value()) return a;
      const int wa = strWidthOf(*a), wb = strWidthOf(*b);
      if (wa >= 0 || wb >= 0) return wa >= wb ? a : b;
      if (*a == "strL" || *b == "strL") return std::string("strL");
      return appendPromotedType(a, b);
    };
    if (direction == "long") {
      for (const auto& st : stubs) {
        std::optional<std::string> t;
        for (const auto& comp : stubComponents[st]) {
          t = widen(t, typeOf(comp));
          session.forgetVarPlanes(comp);
        }
        session.forgetVarPlanes(st);
        if (t.has_value()) session.varMeta[st].stataType = *t;
      }
      // j: the smallest type that holds it (byte 1/2, int for years), or the
      // widest suffix's str# (dc_r_long_v2 t byte, dc_r_long_year_v2 year int,
      // dc_r_long_string t str2).
      session.forgetVarPlanes(*jVar);
      if (jString) {
        // (stubComponents holds copies: the pre-reshape Datastore is gone.)
        size_t w = 1;
        for (const auto& [st, comps] : stubComponents)
          for (const auto& nm : comps) w = std::max(w, nm.size() - st.size());
        session.varMeta[*jVar].stataType = "str" + std::to_string(w);
      } else if (!jValues.empty()) {
        double lo = 0, hi = 0;
        bool integral = true, first = true;
        for (const auto& jv : jValues) {
          double v = 0;
          try { v = std::stod(jv); } catch (...) { integral = false; break; }
          if (v != std::floor(v)) integral = false;
          lo = first ? v : std::min(lo, v);
          hi = first ? v : std::max(hi, v);
          first = false;
        }
        std::string t = "float";
        if (integral) {
          if (lo >= -127 && hi <= 100) t = "byte";
          else if (lo >= -32767 && hi <= 32740) t = "int";
          else t = "long";
        }
        session.varMeta[*jVar].stataType = t;
      }
    } else {
      for (const auto& st : stubs) {
        const WideStubMeta& wm = wideStubMeta[st];
        for (const auto& jv : jValues) {
          const std::string nm = st + jv;
          session.forgetVarPlanes(nm);
          VarMeta& vm = session.varMeta[nm];
          if (wm.type.has_value()) vm.stataType = *wm.type;
          if (!wm.format.empty()) vm.format = wm.format;
          vm.label = jv + " " + st;
        }
        session.forgetVarPlanes(st);
      }
      session.forgetVarPlanes(*jVar);
    }
  }
  // reshape leaves r(N) = the result's _N (dc_r_long_r 6, dc_r_wide_r 3).
  session.eval.rValues.set("N", static_cast<double>(nAfterRows));
  ojson result = mutationResult(session, "reshape");
  result["direction"] = direction;
  result["n_wide"] = nWide;
  result["n_long"] = nLong;
  ojson jArr = ojson::array();
  for (const auto& j : jValues) jArr.push_back(j);
  result["j_values"] = jArr;
  std::ostringstream note;
  if (direction == "long") {
    note << "(" << nWide << " obs × " << k << " waves = " << nLong << " obs in long format)";
  } else {
    note << "(" << nLong << " obs → " << nWide << " obs × " << k
        << " waves in wide format)";
  }
  result["note"] = note.str();
  return result;
}

// ---------------------------------------------------------------------------
// BUG-548 / BUG-549: the keep-list of `merge ..., keepusing(varlist)` and
// `append using F, keep(varlist)` is a VARLIST parsed against the USING data's
// names, not a list of literal names. Both verbs ask this one function, so they
// cannot disagree about what `mother*` means. Oracles (StataBE 19.5):
//   tests/eval/golden/aer_w2/w2_smalls.log section E --
//     keepusing(mother*)            rc 0, k 8 -> 10  (glob)
//     keepusing(mother_a-father_a)  rc 0, k 8 -> 11  (range, using order)
//     keepusing(zzz)                r(111) "variable zzz not found"
//     keepusing(moth)               r(111) "moth ambiguous abbreviation"
//   tests/eval/golden/aer_w2/w2_data_io.log append arms --
//     keep(z s id)  rc 0, columns land in the USING file's order (s before z)
//     keep(id zz) / keep(id m)  r(111), data untouched
//     keep(i)       rc 0 -- a UNIQUE abbreviation is accepted (-> id)
//     keep(i* z) / keep(id-s)   glob and range, rc 0
// Grammar: `*` and `?` through strutil::globMatch (no second matcher); `~` is
// the unique-match wildcard (zero or several matches = r(111), the shape the
// unab measurement in loops.cpp records for `~e` on auto); `a-b` ranges in the
// using file's column order, endpoints resolved like names; `_all`; otherwise
// an exact name, then a unique abbreviation. Result: the using file's column
// order, deduplicated. On error `out` is unspecified and the block carries the
// exact rc in `reason`.
namespace {
std::optional<ojson> expandUsingKeepList(const std::string& spec,
                                         const std::vector<std::string>& usingNames,
                                         std::vector<std::string>& out) {
  out.clear();
  auto fail = [](const std::string& head, const std::string& rc) {
    // Stata's message is the parse error plus a two-line explanation naming
    // keep(varlist) -- for merge's keepusing() too (w2_smalls.log:204-206).
    return ojson(eval::errorBlock(
        "engine_error",
        head + "\n    You specified the keep(varlist) option.  The above error was the result "
               "of parsing the list of variables you supplied against the using data.",
        std::nullopt, std::nullopt, std::nullopt, "r(" + rc + ")"));
  };
  // A name or unique abbreviation -> its index in usingNames.
  auto resolveName = [&](const std::string& tok, size_t& idx) -> std::optional<ojson> {
    for (size_t i = 0; i < usingNames.size(); ++i) {
      if (usingNames[i] == tok) { idx = i; return std::nullopt; }
    }
    size_t hits = 0;
    for (size_t i = 0; i < usingNames.size(); ++i) {
      if (usingNames[i].compare(0, tok.size(), tok) == 0) { idx = i; ++hits; }
    }
    if (hits == 1) return std::nullopt;
    if (hits > 1) return fail(tok + " ambiguous abbreviation", "111");
    return fail("variable " + tok + " not found", "111");
  };

  // Commas separate like spaces; the range operator may carry whitespace on
  // either side (same collapse as loops.cpp's expandVarlistImpl -- a variable
  // name cannot contain `-`, so in a varlist it is always the range operator).
  std::string joined;
  joined.reserve(spec.size());
  for (size_t i = 0; i < spec.size(); ++i) {
    char c = spec[i] == ',' ? ' ' : spec[i];
    if (c == '-') {
      while (!joined.empty() && std::isspace(static_cast<unsigned char>(joined.back())))
        joined.pop_back();
      joined.push_back('-');
      while (i + 1 < spec.size() && std::isspace(static_cast<unsigned char>(spec[i + 1]))) ++i;
      continue;
    }
    joined.push_back(c);
  }

  std::vector<char> keep(usingNames.size(), 0);
  std::istringstream iss(joined);
  std::string tok;
  bool anyTok = false;
  while (iss >> tok) {
    anyTok = true;
    if (tok == "_all") {
      std::fill(keep.begin(), keep.end(), 1);
      continue;
    }
    if (tok.find('~') != std::string::npos) {
      std::string pat = tok;
      std::replace(pat.begin(), pat.end(), '~', '*');
      size_t hits = 0, at = 0;
      for (size_t i = 0; i < usingNames.size(); ++i) {
        if (strutil::globMatch(pat, usingNames[i])) { at = i; ++hits; }
      }
      if (hits == 1) { keep[at] = 1; continue; }
      if (hits > 1) return fail(tok + " ambiguous abbreviation", "111");
      return fail("variable " + tok + " not found", "111");
    }
    if (tok.find('*') != std::string::npos || tok.find('?') != std::string::npos) {
      bool any = false;
      for (size_t i = 0; i < usingNames.size(); ++i) {
        if (strutil::globMatch(tok, usingNames[i])) { keep[i] = 1; any = true; }
      }
      if (!any) return fail("variable " + tok + " not found", "111");
      continue;
    }
    size_t dash = tok.find('-');
    if (dash != std::string::npos) {
      std::string v1 = tok.substr(0, dash), v2 = tok.substr(dash + 1);
      if (v1.empty() || v2.empty()) return fail("- invalid name", "198");
      size_t i1 = 0, i2 = 0;
      if (auto e = resolveName(v1, i1)) return e;
      if (auto e = resolveName(v2, i2)) return e;
      // DESIGN DECISION (no oracle arm): a reversed range is refused as a
      // syntax error rather than silently read backwards.
      if (i1 > i2) return fail("variable range " + tok + " is out of order", "198");
      for (size_t k = i1; k <= i2; ++k) keep[k] = 1;
      continue;
    }
    size_t idx = 0;
    if (auto e = resolveName(tok, idx)) return e;
    keep[idx] = 1;
  }
  // DESIGN DECISION (no oracle arm): an empty keep list is Stata's generic
  // "varlist required" r(100), never "keep nothing" or "keep everything".
  if (!anyTok) {
    return ojson(eval::errorBlock("engine_error", "varlist required", std::nullopt, std::nullopt,
                                  std::nullopt, "r(100)"));
  }
  for (size_t i = 0; i < usingNames.size(); ++i)
    if (keep[i]) out.push_back(usingNames[i]);
  return std::nullopt;
}

bool isNumericStataType(const std::string& t) {
  return t == "byte" || t == "int" || t == "long" || t == "float" || t == "double";
}

// BUG-666: the storage type of every variable a using .dta declares, read from
// the import-dta sidecar (the one `use` reads, handlers.cpp applyDtaMeta). An
// unreadable sidecar gives an empty map, so its columns keep no type claim.
std::map<std::string, std::string> sidecarStataTypes(const std::string& metaPath) {
  std::map<std::string, std::string> out;
  if (metaPath.empty()) return out;
  try {
    std::ifstream mf(cecon::to_native_path(metaPath), std::ios::binary);
    std::ostringstream ms;
    ms << mf.rdbuf();
    nlohmann::json meta = nlohmann::json::parse(ms.str());
    if (!meta.contains("variables") || !meta["variables"].is_array()) return out;
    for (const auto& v : meta["variables"]) {
      if (!v.is_object()) continue;
      const std::string name = v.value("name", std::string());
      const std::string t = v.value("stata_type", std::string());
      if (!name.empty() && !t.empty()) out[name] = t;
    }
  } catch (const std::exception&) {
    out.clear();
  }
  return out;
}

// The numeric storage type a column claims: a declaration, else gen/egen's
// implicit float (BUG-295), else none.
std::optional<std::string> numericTypeClaim(const SessionState& session, const std::string& name) {
  auto it = session.varMeta.find(name);
  if (it != session.varMeta.end() && it->second.stataType.has_value())
    return *it->second.stataType;
  if (session.implicitFloatVars.count(name) != 0) return std::string("float");
  return std::nullopt;
}

// Replace a column's storage-type claim on both planes (nullopt = no claim).
// describe, `: type`, the .dta export and tostring's float/double reading all
// read it.
void setNumericTypeClaim(SessionState& session, const std::string& name,
                         const std::optional<std::string>& t) {
  session.implicitFloatVars.erase(name);
  if (t.has_value()) {
    session.varMeta[name].stataType = *t;
  } else {
    auto it = session.varMeta.find(name);
    if (it != session.varMeta.end()) it->second.stataType.reset();
  }
}

// The current dataset's (name, dtype) pairs: the resident card when it is
// current (mutationResult has just folded the verb into it), else the
// Datastore. Reading the card avoids writing a virtual head to disk.
std::vector<std::pair<std::string, std::string>> currentNameDtypes(SessionState& session) {
  std::vector<std::pair<std::string, std::string>> out;
  if (const std::vector<VarInfo>* card = session.residentReadCard()) {
    for (const auto& vi : *card) out.emplace_back(vi.name, vi.dtype);
    return out;
  }
  Datastore* ds = session.datastore();
  if (ds == nullptr) return out;
  for (const auto& nm : ds->names()) out.emplace_back(nm, ds->dtype(nm));
  return out;
}

// BUG-666: the type append leaves on a numeric variable both datasets hold.
// StataBE 19.5 (measured 2026-09-27): float+double -> double, long+double ->
// double, long+float -> double (either way round), byte+int -> int, each with
// "(variable x was float, now double to accommodate using data's values)".
// Within byte<int<long or byte<int<float the wider one wins. A side with no
// recorded type gives no claim unless the other is double; runAppend then keeps
// that double only when the master rows are float-exact (below).
std::optional<std::string> appendPromotedType(const std::optional<std::string>& m,
                                              const std::optional<std::string>& u) {
  if (m == u) return m;
  if (m == std::string("double") || u == std::string("double")) return std::string("double");
  if (!m.has_value() || !u.has_value()) return std::nullopt;
  if ((*m == "long" && *u == "float") || (*m == "float" && *u == "long"))
    return std::string("double");
  auto rank = [](const std::string& t) {
    return t == "byte" ? 0 : t == "int" ? 1 : 2;  // long and float both rank 2
  };
  return rank(*m) >= rank(*u) ? m : u;
}

// The type -> default display format map is shared with replace's widening
// (storage_type.h), so there is one table.
using storage::stataDefaultFormat;
}  // namespace

namespace {

// merge.ado result(): a result word in keep()/assert() is a digit
// 1-5 or a prefix of masters (3+ letters), usings (2+), matches / matched
// (3+), match_updates (8+) or match_conflicts (8+). Anything else is
// `xyz:  invalid resulttype` r(198) (w2aw_data_combine_family.log dc_mk_bad,
// dc_ma_bad); `keep(mas mat)` is keep(1 3) (dc_mk_mas_mat).
std::optional<int> mergeResultWord(const std::string& w) {
  if (w.size() == 1 && w[0] >= '1' && w[0] <= '5') return w[0] - '0';
  auto prefixOf = [&](const char* full, size_t minLen) {
    const std::string f(full);
    return w.size() >= minLen && w.size() <= f.size() && f.compare(0, w.size(), w) == 0;
  };
  if (prefixOf("masters", 3)) return 1;
  if (prefixOf("usings", 2)) return 2;
  if (prefixOf("matches", 3) || prefixOf("matched", 3)) return 3;
  if (prefixOf("match_updates", 8)) return 4;
  if (prefixOf("match_conflicts", 8)) return 5;
  return std::nullopt;
}

std::optional<ojson> parseMergeResults(const std::string& spec, std::vector<int>& out) {
  out.clear();
  std::istringstream iss(spec);
  for (std::string w; iss >> w;) {
    const std::optional<int> c = mergeResultWord(w);
    if (!c.has_value()) {
      return ojson(eval::errorBlock(
          "syntax_error",
          w + ":  invalid resulttype\n    results, specified in options assert() and keep(), are "
              "the integers 1 through 5, or master (equivalent to 1), using (2), match (3), "
              "match_update (4), or match_conflict (5).",
          std::nullopt, std::nullopt, std::nullopt, std::string("r(198)")));
    }
    if (std::find(out.begin(), out.end(), *c) == out.end()) out.push_back(*c);
  }
  std::sort(out.begin(), out.end());
  return std::nullopt;
}

// str# width, or -1 (strL / not a str# spelling).
int strWidthOf(const std::string& t) {
  if (t.size() <= 3 || t.compare(0, 3, "str") != 0) return -1;
  const std::string d = t.substr(3);
  if (d.empty() || !std::all_of(d.begin(), d.end(), [](unsigned char c) { return std::isdigit(c); }))
    return -1;
  return std::stoi(d);
}

}  // namespace

namespace {
// merge/append bring the using file's notes along: each variable's (and
// _dta's) using notes are numbered on after the master's, and note0 counts
// them all (w2aw_data_combine_family.log dc_ml_notes_n / dc_a_notes_n:
// `[2]|[master note]|[u note]|...|[2]|[x master note]|[x u note]|`). The using
// char table is the .dta sidecar's, or the one a native checkpoint this
// session saved recorded beside its path. A variable the result does not have
// takes none.
void appendUsingNotes(SessionState& session, const std::string& usingPath,
                      const std::string& usingMetaPath) {
  std::vector<DtaCharacteristic> uChars;
  if (!usingMetaPath.empty()) {
    try {
      std::ifstream mf(cecon::to_native_path(usingMetaPath), std::ios::binary);
      std::ostringstream ms;
      ms << mf.rdbuf();
      nlohmann::json meta = nlohmann::json::parse(ms.str());
      if (meta.contains("characteristics") && meta["characteristics"].is_array()) {
        for (const auto& ch : meta["characteristics"]) {
          if (!ch.is_object() || !ch.contains("value") || !ch["value"].is_string()) continue;
          uChars.push_back(DtaCharacteristic{ch.value("varname", std::string()),
                                             ch.value("charname", std::string()),
                                             ch["value"].get<std::string>()});
        }
      }
    } catch (const std::exception&) {
      return;
    }
  } else if (const SessionState::CheckpointTsDecl* ck = session.checkpointTsDecl(usingPath)) {
    uChars = ck->characteristics;
  }
  if (uChars.empty()) return;
  const auto charOf = [](const std::vector<DtaCharacteristic>& tbl, const std::string& v,
                         const std::string& c) -> const std::string* {
    for (const auto& x : tbl)
      if (x.varname == v && x.charname == c) return &x.value;
    return nullptr;
  };
  std::set<std::string> resultVars;
  for (const auto& [nm, dt] : currentNameDtypes(session)) {
    (void)dt;
    resultVars.insert(nm);
  }
  std::vector<std::string> owners;
  for (const auto& c : uChars)
    if (c.charname == "note0" && std::find(owners.begin(), owners.end(), c.varname) == owners.end())
      owners.push_back(c.varname);
  for (const auto& v : owners) {
    if (v != "_dta" && resultVars.count(v) == 0) continue;
    int nu = 0;
    try {
      nu = std::stoi(*charOf(uChars, v, "note0"));
    } catch (const std::exception&) {
      continue;
    }
    if (nu <= 0) continue;
    int nm = 0;
    if (const std::string* m0 = charOf(session.characteristics, v, "note0")) {
      try {
        nm = std::stoi(*m0);
      } catch (const std::exception&) {
        nm = 0;
      }
    }
    const auto setChar = [&session](const std::string& var, const std::string& name,
                                    const std::string& value) {
      for (auto& x : session.characteristics)
        if (x.varname == var && x.charname == name) {
          x.value = value;
          return;
        }
      session.characteristics.push_back(DtaCharacteristic{var, name, value});
    };
    int added = 0;
    for (int k = 1; k <= nu; ++k) {
      const std::string* t = charOf(uChars, v, "note" + std::to_string(k));
      if (t == nullptr) continue;
      ++added;
      setChar(v, "note" + std::to_string(nm + added), *t);
    }
    if (added > 0) setChar(v, "note0", std::to_string(nm + added));
  }
}
}  // namespace

std::string dv_detail::resolveUsingDataset(SessionState& session, const std::string& rawPath,
                                           std::string* shown) {
  std::string path = resolveUserPath(session, rawPath);
  session.materializeVirtualSave(path);  // BUG-125: a save_virtual'd tempfile becomes real on demand
  if (extOf(path).empty() && !fileExists(path)) {
    path += ".dta";
    if (shown != nullptr) *shown += ".dta";
    session.materializeVirtualSave(path);
  }
  return path;
}

void dv_detail::adoptUsingValueLabels(SessionState& session, const std::string& usingPath,
                                      const std::string& usingMetaPath,
                                      const std::vector<std::string>& brought,
                                      bool copyDefinitions) {
  // The using side's attachments and definitions: a .dta's import-dta sidecar,
  // or the record a native checkpoint this session saved keeps beside its path.
  std::map<std::string, std::string> uAttach;
  std::map<std::string, std::map<std::string, std::string>> uDefs;
  if (!usingMetaPath.empty()) {
    nlohmann::json meta;
    try {
      std::ifstream mf(cecon::to_native_path(usingMetaPath), std::ios::binary);
      std::ostringstream ms;
      ms << mf.rdbuf();
      meta = nlohmann::json::parse(ms.str());
    } catch (const std::exception&) {
      meta = nlohmann::json();
    }
    if (meta.contains("variables") && meta["variables"].is_array()) {
      for (const auto& v : meta["variables"]) {
        if (!v.is_object()) continue;
        const std::string vl = v.value("value_label_name", std::string());
        if (!vl.empty()) uAttach[v.value("name", std::string())] = vl;
      }
    }
    if (meta.contains("value_labels") && meta["value_labels"].is_object()) {
      for (auto it = meta["value_labels"].begin(); it != meta["value_labels"].end(); ++it) {
        if (!it.value().is_object()) continue;
        std::map<std::string, std::string>& codeMap = uDefs[it.key()];
        for (auto cit = it.value().begin(); cit != it.value().end(); ++cit)
          if (cit.value().is_string()) codeMap[cit.key()] = cit.value().get<std::string>();
      }
    }
  } else if (const SessionState::CheckpointTsDecl* ck = session.checkpointTsDecl(usingPath)) {
    uAttach = ck->varLblname;
    uDefs = ck->labelDefs;
  } else {
    return;
  }
  for (const auto& nm : brought) {
    auto a = uAttach.find(nm);
    if (a != uAttach.end()) session.varLblname[nm] = a->second;
  }
  if (copyDefinitions) {
    for (const auto& [name, codeMap] : uDefs)
      if (!session.labelDefs.count(name)) session.labelDefs[name] = codeMap;
  }
  // Every attachment now reads through its (possibly just copied) set.
  for (const auto& [varname, lname] : session.varLblname) {
    auto d = session.labelDefs.find(lname);
    if (d == session.labelDefs.end()) continue;
    std::map<int, std::string> numericMap;
    for (const auto& [code, text] : d->second) {
      try {
        numericMap[std::stoi(code)] = text;
      } catch (const std::exception&) {
      }
    }
    session.valueLabels[varname] = numericMap;
  }
}

// What a using file states about one of its variables (dv_detail::UsingVarMeta):
// a .dta states all three in the import-dta sidecar; a native checkpoint
// (tempfile) states the type beside its path (BUG-922) and the label/format
// either beside its path (a virtual save) or in its header.
std::map<std::string, UsingVarMeta> dv_detail::usingVarMetaOf(SessionState& session, const std::string& usingPath,
                                                   const std::string& usingMetaPath,
                                                   Datastore& usingDs) {
  std::map<std::string, UsingVarMeta> out;
  if (!usingMetaPath.empty()) {
    try {
      std::ifstream mf(cecon::to_native_path(usingMetaPath), std::ios::binary);
      std::ostringstream ms;
      ms << mf.rdbuf();
      nlohmann::json meta = nlohmann::json::parse(ms.str());
      if (meta.contains("variables") && meta["variables"].is_array()) {
        for (const auto& v : meta["variables"]) {
          if (!v.is_object()) continue;
          const std::string name = v.value("name", std::string());
          if (name.empty()) continue;
          UsingVarMeta& u = out[name];
          u.label = v.value("label", std::string());
          u.format = v.value("format", std::string());
          const std::string t = v.value("stata_type", std::string());
          if (!t.empty()) u.stataType = t;
        }
      }
    } catch (const std::exception&) {
      out.clear();
    }
    return out;
  }
  for (const auto& nm : usingDs.names()) {
    UsingVarMeta& u = out[nm];
    const cecon::Column& c = usingDs.col(nm);
    u.label = c.label;
    u.format = c.format;
  }
  if (const auto* vm = session.virtualSaveMetaOf(usingPath)) {
    for (const cecon::ColMetaPatch& p : *vm) {
      auto it = out.find(p.name);
      if (it == out.end()) continue;
      if (p.label.has_value()) it->second.label = *p.label;
      if (p.format.has_value() && !p.format->empty()) it->second.format = *p.format;
    }
  }
  for (const auto& [nm, t] : session.checkpointStataTypes(usingPath)) {
    auto it = out.find(nm);
    if (it != out.end() && !t.empty()) it->second.stataType = t;
  }
  return out;
}


// ---------------------------------------------------------------------------
// append using FILE — C++ port of append.py's run_append. cobra 0.18.0's
// native `append` verb accepts .hbin/.dta/.csv directly on --using (no
// manual pre-conversion needed, verified empirically) and already
// implements the f64/factor dtype-promotion + missing-column padding the
// oracle documents (contract §22) — this only resolves the path against
// session.cwd and pre-checks existence for the oracle's exact error text.
ojson runAppend(const std::string& filepathArg, SessionState& session,
                 const std::optional<std::string>& genVar,
                 const std::optional<std::string>& keepSpec, bool force, bool nolabel,
                 bool nonotes, std::vector<std::string>* typeNotes) {
  // Resolve the `using` file against session.cwd (native-paths stabilization
  // item 2) — session.workdir() is the internal hbin-chain scratch dir, not
  // where a user's own dataset files live.
  std::string shown = filepathArg;
  std::string filepath = resolveUsingDataset(session, filepathArg, &shown);
  if (!fileExists(filepath)) {
    return engineErrorBlock("append: file not found: " + filepath);
  }

  Datastore* ds = session.datastore();
  if (ds == nullptr) return eval::noDataLoaded();
  int64_t nMaster = ds->nRows();
  const std::vector<std::string> masterNames = ds->names();
  // append.ado picks generate()'s label name before the append reads anything, so
  // a using file's own `_append` definition cannot change it (BUG-551, below).
  const std::string genLbl = session.labelDefs.count("_append") ? "__append" : "_append";

  // B5-D10: generate(NAME) validated BEFORE the append runs (same ordering
  // destring/tostring use for their gen() targets), so a bad name never
  // leaves a half-appended dataset.
  if (genVar.has_value()) {
    // append.ado `confirm new var` (after every file was described): r(110)
    // (w2aw_data_combine_family.log dc_a_gen_exists).
    if (ds->has(*genVar)) {
      return ojson(eval::errorBlock("engine_error", "variable " + *genVar + " already defined",
                                    std::nullopt, std::nullopt, std::nullopt, "r(110)"));
    }
    if (!strutil::isValidStataIdentifier(*genVar)) {
      return engineErrorBlock("append: invalid variable name `" + *genVar + "' — " +
                                  strutil::stataIdentifierRuleText(),
                              "syntax_error");
    }
  }

  // BUG-549: keep(varlist) -- expand against the USING file's names, then hand the
  // engine the exact list (`--keepusing`, cmd_append projects the using frame
  // before the concat). The names come from the hbin header when the using file is
  // one; a .dta/.csv is converted ONCE (the same import-dta/convert merge uses for
  // its pre-checks) and the engine then appends from that converted file, so the
  // using data is still read once, not once for names and again for rows.
  // BUG-551: a .dta using is always converted here (the engine would convert it
  // anyway, so it is still read once), with the same --out-meta sidecar
  // `use` reads (it also names the using side's storage types for BUG-550's r(106)
  // message). A .csv is converted here too, so a file the converter refuses
  // (BUG-414) is answered with append's own code; a .hbin goes to the engine as is.
  std::string engineUsing = filepath;
  std::string keepCsv;
  std::string usingMetaPath;
  std::optional<std::set<std::string>> usingKept;  // keep()'s using names (BUG-666)
  const std::string ext = extOf(filepath);
  if (ext == ".dta" || usingFileIsDta(filepath)) {
    std::string conv = nextPath(session.workdir(), "append_using_dta");
    usingMetaPath = conv + ".meta.json";
    registerScratchMint(usingMetaPath);  // BUG-389
    CobraRunResult ccr = runCobra({"import-dta", filepath, conv, "--out-meta", usingMetaPath});
    if (!ccr.ok) {
      CobraError ce = errorFromRaw(ccr);
      return engineErrorBlock("append using: cannot convert '" + filepath + "': " + ce.what());
    }
    engineUsing = conv;
  } else if (ext == ".csv" || ext == ".tsv") {
    std::string conv = nextPath(session.workdir(), "append_using_csv");
    char delim = (ext == ".tsv") ? '\t' : ',';
    CobraRunResult ccr = runCobra(
        {"convert", "--data", filepath, "--out", conv, "--delim", std::string(1, delim)});
    if (!ccr.ok) return delimitedUsingRefused(shown, 601);
    engineUsing = conv;
  }
  if (keepSpec.has_value()) {
    std::vector<std::string> usingNames;
    try {
      Datastore usingDs(engineUsing);  // header parse only -- the names, no column payload
      usingNames = usingDs.names();
    } catch (const std::exception& e) {
      return engineErrorBlock("append using: cannot read '" + filepath + "': " + e.what());
    }
    std::vector<std::string> kept;
    if (auto err = expandUsingKeepList(*keepSpec, usingNames, kept)) return *err;
    for (size_t i = 0; i < kept.size(); ++i) {
      if (i) keepCsv += ",";
      keepCsv += kept[i];
    }
    usingKept.emplace(kept.begin(), kept.end());
  }

  // BUG-666: the master rows of a shared numeric column decide the storage
  // claim it keeps after the append (below). They are read here, from the
  // pre-append Datastore `ds` (already a file on disk: its path is what the
  // engine is handed), and not from the post-append head: on the resident
  // plane session.datastore() would first materialize that head, writing a
  // snapshot the append otherwise avoids. Only the columns whose claim can
  // depend on their values are scanned: in both files, numeric in the using
  // file's sidecar, and without a master double/long claim.
  struct MasterRows {
    bool hasFraction = false, floatExact = true;
  };
  // BUG-922: a native checkpoint (tempfile) names its types beside its path.
  const std::map<std::string, std::string> uTypes =
      usingMetaPath.empty() ? session.checkpointStataTypes(filepath)
                            : sidecarStataTypes(usingMetaPath);
  std::map<std::string, MasterRows> masterRows;
  for (const auto& nm : masterNames) {
    auto uit = uTypes.find(nm);
    if (uit == uTypes.end() || !isNumericStataType(uit->second)) continue;
    if (usingKept && usingKept->count(nm) == 0) continue;
    if (ds->dtype(nm) != "f64") continue;
    const std::optional<std::string> m = numericTypeClaim(session, nm);
    if (m == std::string("double") || m == std::string("long")) continue;
    if (m.has_value() && !isNumericStataType(*m)) continue;
    MasterRows out;
    const double* v = ds->col(nm).f64();
    if (v == nullptr) {
      out.hasFraction = true;  // unreadable: no row is known to be exact
      out.floatExact = false;
    } else {
      for (int64_t r = 0; r < nMaster; ++r) {
        if (std::isnan(v[r])) continue;
        if (v[r] != std::trunc(v[r])) out.hasFraction = true;
        if (std::fabs(v[r]) > std::numeric_limits<float>::max() ||
            static_cast<double>(static_cast<float>(v[r])) != v[r])
          out.floatExact = false;
      }
    }
    masterRows.emplace(nm, out);
  }

  // BUG-666: append's float->double widening. A master column Stata holds as
  // float (an undeclared gen/egen, a declared float, or a byte/int a `replace`
  // made float) that the using file's type promotes to double keeps its float
  // values: StataBE widens float(v), it does not recompute v. PIET keeps gen
  // values unrounded, so those master rows are rounded to float here, before
  // the engine appends, and the column then takes an ordinary double claim
  // below. tests/eval/golden/aer_w2/w2f_residuals.log:1824 f_append_types_ctl:
  // master `generate x = 0.07*100` / `generate y = 123456789`, using double x
  // 0.1 / long y 123456789 -> x and y double, x[1] exactly 7, y[1] 123456792,
  // y[3] 123456789; :1856/:1898 tostring then stores "7"/".1" and
  // "123456792"/"123456789". The rounding is a new --data for the append, so a
  // refused append leaves the master as it was. A column the setcol payload
  // cannot carry (an extended missing .a-.z, which it would collapse to `.`, or
  // a value beyond float's range) is left unrounded and keeps the old rule
  // below: no claim unless its rows are float-exact.
  std::string masterData = ds->path();
  for (auto& [nm, rows] : masterRows) {
    if (rows.floatExact) continue;
    const std::optional<std::string> m = numericTypeClaim(session, nm);
    std::optional<std::string> mEff = m;
    if ((m == std::string("byte") || m == std::string("int")) && rows.hasFraction)
      mEff = std::string("float");
    if (mEff != std::string("float")) continue;
    if (appendPromotedType(mEff, uTypes.at(nm)) != std::string("double")) continue;
    const double* v = ds->col(nm).f64();
    if (v == nullptr) continue;
    bool carriable = true;
    for (int64_t r = 0; r < nMaster && carriable; ++r) {
      if (std::isnan(v[r]))
        carriable = cecon::missing_kind(v[r]) == cecon::kMissSys;
      else if (std::fabs(v[r]) > std::numeric_limits<float>::max())
        carriable = false;
    }
    if (!carriable) continue;
    std::string valuesPath = nextPath(session.workdir(), "append_float") + ".csv";
    {
      std::ofstream csv(cecon::to_native_path(valuesPath), std::ios::binary);
      if (!csv) return engineErrorBlock("append: could not open scratch CSV for write");
      for (int64_t r = 0; r < nMaster; ++r) {
        if (!std::isnan(v[r]))
          csv << eval::pythonFloatRepr(static_cast<double>(static_cast<float>(v[r])));
        csv << "\n";  // missing -> empty line (system missing only, checked above)
      }
      // BUG-124 sibling: flush + check — silent short payload = silent row loss.
      csv.flush();
      if (!csv) {
        cecon::native_unlink(valuesPath);
        return engineErrorBlock("append: failed writing scratch CSV (disk full or I/O error)");
      }
    }
    // The format and label the column carries in the file: setcol replaces the
    // column whole, so they are kept on the session plane first.
    const cecon::Column& col = ds->col(nm);
    VarMeta& vm = session.varMeta[nm];
    if (!col.format.empty() && !vm.format.has_value()) vm.format = col.format;
    if (!col.label.empty() && !vm.label.has_value()) vm.label = col.label;
    const std::string roundedPath = nextPath(session.workdir(), "append_float");
    CobraRunResult rcr = runCobra({"setcol", "--data", masterData, "--var", nm, "--values",
                                   valuesPath, "--dtype", "f64", "--out", roundedPath});
    cecon::native_unlink(valuesPath);
    if (!rcr.ok) {
      CobraError ce = errorFromRaw(rcr);
      return engineErrorBlock(cobraErrorStr(ce), ce.code);
    }
    masterData = roundedPath;
    rows.floatExact = true;
  }

  // What the using file states about its variables (label, format, str#),
  // read while it is a file on disk (DC5: dc_a_base_v7 onlyu `str3|[only in u]`).
  std::map<std::string, UsingVarMeta> uMeta;
  std::vector<std::string> usingOrder;  // the using file's variable order (BUG-1092)
  if (!usingMetaPath.empty() || extOf(engineUsing) != ".csv") {
    try {
      Datastore usingDs(engineUsing);
      uMeta = usingVarMetaOf(session, filepath, usingMetaPath, usingDs);
      usingOrder = usingDs.names();
    } catch (const std::exception&) {
      uMeta.clear();  // the engine's own read below answers for the file
    }
  }

  std::string outPath = nextPath(session.workdir(), "append");
  std::vector<std::string> args = {"append", "--data", masterData, "--using", engineUsing, "--out",
                                    outPath};
  if (keepSpec.has_value()) {
    args.push_back("--keepusing");
    args.push_back(keepCsv);
  }
  // BUG-550: Stata's type rule, not the engine's promote-both-to-f64 default (which
  // turned the using side's string codes into numbers at rc 0). w2_data_io.log
  // app15: numeric s in master, str3 s in using -> r(106); app16/app17 under force:
  // s = . on the using rows.
  args.push_back("--type-clash");
  args.push_back(force ? "force" : "error");
  CobraRunResult cr;
  {
    // BUG-394 (2026-09-02): onto the resident plane — append was the largest of the
    // eight verbs still writing a whole --out file per call. Its only stateless
    // dependency was the stderr notice PIET parsed for n_using; on the resident leg
    // the count is exact arithmetic (append's result has nm+nu rows), so it is read
    // off the resident row count instead. The stderr parse stays as the stateless
    // fallback, exactly like merge's counts (854fcbed).
    SessionState::ResidentEligible residentAppend(session);
    cr = runCobra(args);
  }
  if (!cr.ok) {
    CobraError ce = errorFromRaw(cr);
    if (ce.detail.rfind("append_type_clash:", 0) == 0) {
      // Stata's two-line explanation (app15: "variable s is float in master but str3
      // in using data"). The engine carries no Stata storage type, so it says
      // numeric/string; the storage types are filled in here where they are known:
      // the master's from varMeta (else `float`, describe's fallback for an
      // undeclared numeric), the using side's from the .dta sidecar.
      const std::string nm = ce.detail.substr(std::string("append_type_clash:").size());
      const bool masterStr =
          std::string(ce.what()).find(" is string in master") != std::string::npos;
      std::string mType = masterStr ? "string" : "float";
      auto mIt = session.varMeta.find(nm);
      if (mIt != session.varMeta.end() && mIt->second.stataType.has_value())
        mType = *mIt->second.stataType;
      std::string uType = masterStr ? "numeric" : "string";
      if (!usingMetaPath.empty()) {
        try {
          std::ifstream mf(cecon::to_native_path(usingMetaPath), std::ios::binary);
          std::ostringstream ms;
          ms << mf.rdbuf();
          nlohmann::json meta = nlohmann::json::parse(ms.str());
          if (meta.contains("variables") && meta["variables"].is_array())
            for (const auto& v : meta["variables"])
              if (v.is_object() && v.value("name", std::string()) == nm &&
                  !v.value("stata_type", std::string()).empty())
                uType = v.value("stata_type", std::string());
        } catch (const std::exception&) {
          // keep the generic word: the refusal itself does not depend on the sidecar
        }
      }
      std::string msg = "variable " + nm + " is " + mType + " in master but " + uType +
                        " in using data";
      if (!masterStr)
        msg += "\n    You could specify append's force option to ignore this numeric/string "
               "mismatch.  The using variable would then be treated as if\n    it contained "
               "numeric missing value.";
      return ojson(eval::errorBlock("engine_error", msg, std::nullopt, std::nullopt, std::nullopt,
                                    "r(106)"));
    }
    return engineErrorBlock(cobraErrorStr(ce), ce.code);
  }
  session.load(outPath);
  ojson result = mutationResult(session, "append");
  // BUG-666: storage types, which tostring's float/double reading, describe,
  // `: type` and the .dta export all read. A variable only the using file has
  // takes the using file's type (before, it had none, and a stale claim left
  // under its name by a renamed or dropped column could stand in). One both
  // hold takes append's promotion of the two. A using file with no .dta sidecar
  // names no types: its new columns get no claim and shared ones keep the
  // master's.
  {
    const std::set<std::string> inMaster(masterNames.begin(), masterNames.end());
    // The master rows of a shared column, scanned before the append (above).
    // A column missing from that scan was not a candidate; it is read as not
    // float-exact, the safe side (no double claim).
    auto scanMasterRows = [&](const std::string& nm) {
      auto it = masterRows.find(nm);
      if (it != masterRows.end()) return it->second;
      MasterRows out;
      out.hasFraction = true;
      out.floatExact = false;
      return out;
    };
    // BUG-1092 (FU-421): StataBE names each master variable the using data
    // widen (measured 2026-11-03: float+double, float+long and byte+double give
    // double, byte+int gives int, str2+str5 gives str5; a double master taking a
    // float using is silent), in the using file's variable order (using y s x
    // onto master x s y prints y, s, x). "was" is the type `: type` answered
    // before.
    std::vector<std::pair<std::string, std::string>> widened;
    auto noteWidening = [&](const std::string& nm, const std::string& was,
                            const std::string& now) {
      if (typeNotes != nullptr && was != now)
        widened.emplace_back(nm, "(variable " + nm + " was " + was + ", now " + now +
                                     " to accommodate using data's values)");
    };
    for (const auto& [nm, dtype] : currentNameDtypes(session)) {
      auto uit = uTypes.find(nm);
      const bool usingHas = uit != uTypes.end() && (!usingKept || usingKept->count(nm) != 0);
      std::optional<std::string> u;
      if (usingHas && isNumericStataType(uit->second)) u = uit->second;
      const std::optional<std::string> m = numericTypeClaim(session, nm);
      auto umIt = uMeta.find(nm);
      const UsingVarMeta* um = umIt == uMeta.end() ? nullptr : &umIt->second;
      if (!inMaster.count(nm)) {
        // format-display-default-lifecycle: and the format the using file
        // states for it (merge's rule, af_merge).
        session.adoptVirtualSaveFormats(filepath, {nm});
        if (dtype == "f64") {
          setNumericTypeClaim(session, nm, u);
        } else {
          if (m.has_value() && isNumericStataType(*m))
            setNumericTypeClaim(session, nm, std::nullopt);  // stale numeric claim on a string
          if (um != nullptr && um->stataType.has_value() && !isNumericStataType(*um->stataType))
            session.varMeta[nm].stataType = *um->stataType;
        }
        // The using file's variable label and format come along (dc_a_base_v7).
        if (um != nullptr && !um->label.empty()) session.setVarLabel(nm, um->label);
        if (um != nullptr && !um->format.empty()) {
          VarMeta& vm = session.varMeta[nm];
          if (!vm.format.has_value() || vm.format->empty()) vm.format = um->format;
        }
        continue;
      }
      // A string both files hold widens to the wider str# (dc_a_base_v3 s str5
      // from str2 + str5).
      if (dtype == "factor" && usingHas && um != nullptr && um->stataType.has_value() &&
          m.has_value()) {
        const int mw = strWidthOf(*m), uw = strWidthOf(*um->stataType);
        if (*um->stataType == "strL" && *m != "strL")
          session.varMeta[nm].stataType = std::string("strL");
        else if (mw >= 0 && uw > mw)
          session.varMeta[nm].stataType = *um->stataType;
        noteWidening(nm, *m, session.varMeta[nm].stataType.value_or(*m));
        continue;
      }
      if (!u.has_value() || dtype != "f64") continue;
      if (m.has_value() && !isNumericStataType(*m)) continue;
      // A master byte/int holding a fraction was made float by a `replace`
      // PIET does not record (runTostring reads it as float the same way).
      // StataBE then promotes that float like any other.
      const MasterRows rows = scanMasterRows(nm);
      std::optional<std::string> mEff = m;
      if ((m == std::string("byte") || m == std::string("int")) && rows.hasFraction)
        mEff = std::string("float");
      std::optional<std::string> promoted = appendPromotedType(mEff, u);
      // Promotion to double turns a float master row into double(float(v)).
      // A float master's rows were rounded to float before the append (above),
      // so they are float-exact now. A master with NO claim may be float in
      // Stata too (import delimited, collapse outputs, clonevar, a renamed
      // gen): its rows are not rounded, since its Stata type is not known. It
      // takes the double claim only when its rows are float-exact, and read
      // the same either way; otherwise it keeps no claim, and tostring reads it
      // both ways and refuses by name when they disagree. The same holds for a
      // float master the rounding could not carry (extended missing). A master
      // long (or a long holding a fraction, i.e. a double) is exact.
      if (promoted == std::string("double") && mEff != std::string("double") &&
          mEff != std::string("long")) {
        if (!rows.floatExact) promoted = std::nullopt;
      }
      if (promoted != m) {
        // The variable keeps the display format it was created with (see
        // stataDefaultFormat), which tostring's usedisplayformat reads: record
        // the master's creation-time default before the claim changes. The
        // declared type, not mEff: a byte a `replace` made float keeps %8.0g.
        VarMeta& vm = session.varMeta[nm];
        if (!vm.format.has_value() || vm.format->empty()) vm.format = stataDefaultFormat(m);
        setNumericTypeClaim(session, nm, promoted);
        noteWidening(nm, mEff.value_or("float"), promoted.value_or("float"));
      }
    }
    auto usingPos = [&usingOrder](const std::string& nm) {
      return std::find(usingOrder.begin(), usingOrder.end(), nm) - usingOrder.begin();
    };
    std::stable_sort(widened.begin(), widened.end(), [&](const auto& a, const auto& b) {
      return usingPos(a.first) < usingPos(b.first);
    });
    for (auto& w : widened) typeNotes->push_back(std::move(w.second));
  }
  int64_t nUsing = 0;
  {
    // Stateless leg: the one-shot stderr notice. Resident leg: no notice — the
    // count is exact arithmetic off the row count the card build just published
    // (append's result has nm+nu rows), read from the same block the user sees.
    static const std::regex reCounts(R"(\((\d+)\s+master\s*\+\s*(\d+)\s+using\s*=)");
    std::smatch m;
    if (std::regex_search(cr.stderr_text, m, reCounts)) {
      try {
        nUsing = std::stoll(m.str(2));
      } catch (...) {
      }
    } else if (result.contains("n_rows_after") && result["n_rows_after"].is_number_integer()) {
      const int64_t after = result["n_rows_after"].get<int64_t>();
      if (after >= nMaster) nUsing = after - nMaster;
    }
  }
  result["n_master"] = nMaster;
  result["n_using"] = nUsing;

  // BUG-551: copy the using file's value-label DEFINITIONS; a definition already
  // in memory is never replaced (w2_data_io.log app1..app10/app13: alab, defined
  // only in the using file, is defined after the append -- also under keep() that
  // drops its variable; app11/app12: not under nolabel). A shared variable's
  // attachment is not copied: the master's `a` stays unlabelled (app0 lists 10,
  // not "ten").
  if (!nolabel && !usingMetaPath.empty()) {
    nlohmann::json meta;
    try {
      std::ifstream mf(cecon::to_native_path(usingMetaPath), std::ios::binary);
      std::ostringstream ms;
      ms << mf.rdbuf();
      meta = nlohmann::json::parse(ms.str());
    } catch (const std::exception& e) {
      return engineErrorBlock("append using: cannot read the value labels of '" + filepath +
                              "': " + e.what());
    }
    if (meta.contains("value_labels") && meta["value_labels"].is_object()) {
      for (auto it = meta["value_labels"].begin(); it != meta["value_labels"].end(); ++it) {
        if (session.labelDefs.count(it.key()) || !it.value().is_object()) continue;
        if (genVar.has_value() && it.key() == genLbl) continue;  // defined "before" the append
        std::map<std::string, std::string> codeMap;
        std::map<int, std::string> numericMap;
        for (auto cit = it.value().begin(); cit != it.value().end(); ++cit) {
          if (!cit.value().is_string()) continue;
          codeMap[cit.key()] = cit.value().get<std::string>();
          try {
            numericMap[std::stoi(cit.key())] = cit.value().get<std::string>();
          } catch (const std::exception&) {
          }
        }
        session.labelDefs[it.key()] = codeMap;
        // A master variable already pointing at this (so far undefined) name now
        // displays through it, as `label values` before `label define` does.
        for (const auto& [varname, lname] : session.varLblname)
          if (lname == it.key()) session.valueLabels[varname] = numericMap;
      }
    }
  }
  // ...while a variable only the using file has keeps the set it is attached to
  // there (BUG-1157: `append using` of a file whose u carries ul lists u
  // through ul, StataBE, as merge's brought-in variables do). A native
  // checkpoint states its definitions beside its path, so they come here too.
  {
    const std::set<std::string> inMaster(masterNames.begin(), masterNames.end());
    std::vector<std::string> brought;
    for (const auto& [nm, dtype] : currentNameDtypes(session)) {
      (void)dtype;
      if (!inMaster.count(nm)) brought.push_back(nm);
    }
    adoptUsingValueLabels(session, filepath, usingMetaPath, brought,
                          !nolabel && usingMetaPath.empty());
  }

  if (!nonotes) appendUsingNotes(session, filepath, usingMetaPath);

  if (genVar.has_value()) {
    // B5-D10 (measured B4REC|append|generate|0|, tests/eval/golden/aer_b4/
    // pins_oracle_r.log:96-97): src marks which side each row came from —
    // 0 for the nMaster original master rows, 1 for the nUsing rows appended
    // from `using`. Same setcol --new mechanism winsor uses for its genName
    // column (handlers_aer_abbrev.cpp), applied to the head the append above
    // just produced.
    std::string valuesPath = nextPath(session.workdir(), "append_gen") + ".csv";
    {
      std::ofstream csv(cecon::to_native_path(valuesPath), std::ios::binary);
      if (!csv) return engineErrorBlock("append: could not open scratch CSV for write");
      for (int64_t r = 0; r < nMaster; ++r) csv << "0\n";
      for (int64_t r = 0; r < nUsing; ++r) csv << "1\n";
      // BUG-124 sibling: flush + check — silent short payload = silent row loss.
      csv.flush();
      if (!csv) {
        cecon::native_unlink(valuesPath);
        return engineErrorBlock("append: failed writing scratch CSV (disk full or I/O error)");
      }
    }
    std::string genOutPath = nextPath(session.workdir(), "append_gen");
    CobraRunResult gcr = runCobra({"setcol", "--data", *session.currentHbin(), "--var", *genVar,
                                    "--values", valuesPath, "--dtype", "f64", "--new", "--out",
                                    genOutPath});
    if (!gcr.ok) {
      CobraError ce = errorFromRaw(gcr);
      return engineErrorBlock(cobraErrorStr(ce), ce.code);
    }
    session.load(genOutPath);
    // BUG-551: Stata's append.ado creates the marker BEFORE the append (`gen byte
    // NAME = 0`), so it sits right after the master's variables (w2_data_io.log
    // app8: id a m src s z), is a byte labelled "Dataset source", and carries the
    // value label `_append` (or `__append` when `_append` is taken) with 0
    // "Master" and 1 "Appended dataset 1" (app8/app9/app10 list those texts).
    Datastore* after = session.datastore();
    if (after != nullptr && !masterNames.empty() &&
        after->names().size() > masterNames.size() + 1) {
      std::string ordOut = nextPath(session.workdir(), "append_gen_order");
      CobraRunResult ocr = runCobra({"order", "--data", *session.currentHbin(), "--vars", *genVar,
                                     "--after", masterNames.back(), "--out", ordOut});
      if (!ocr.ok) {
        CobraError ce = errorFromRaw(ocr);
        return engineErrorBlock(cobraErrorStr(ce), ce.code);
      }
      session.load(ordOut);
    }
    session.labelDefs[genLbl] = {{"0", "Master"}, {"1", "Appended dataset 1"}};
    session.varLblname[*genVar] = genLbl;
    session.valueLabels[*genVar] = {{0, "Master"}, {1, "Appended dataset 1"}};
    session.varMeta[*genVar].stataType = "byte";
    // Attaching the value label widens the display to its longest text,
    // "Appended dataset 1" (dc_a_gen_v7 `byte|%18.0g`).
    session.varMeta[*genVar].format = std::string("%18.0g");
    session.setVarLabel(*genVar, "Dataset source");
    result = mutationResult(session, "append");
    result["n_master"] = nMaster;
    result["n_using"] = nUsing;
  }
  return result;
}

// ---------------------------------------------------------------------------
// append using FILE [FILE ...] — append.ado's own loop over runAppend: every
// file is described first (`file X not found` r(601) before anything moves;
// the last description leaves r(N) = that file's _N, dc_a_base_r 2), then
// generate() is created, then each file is appended in turn with the marker
// set to its number k and labelled "Appended dataset k" (dc_a_two,
// dc_a_gen_two). Appending into an empty dataset (after `clear`) is the using
// data itself (dc_a_empty; with generate() the marker is the first variable).
ojson runAppendFiles(const std::vector<std::string>& files, SessionState& session,
                     const std::optional<std::string>& genVar,
                     const std::optional<std::string>& keepSpec, bool force, bool nolabel,
                     bool nonotes, std::vector<std::string>* typeNotes) {
  auto rcError = [](const std::string& msg, const std::string& rc) {
    return ojson(eval::errorBlock("engine_error", msg, std::nullopt, std::nullopt, std::nullopt,
                                  "r(" + rc + ")"));
  };
  if (files.empty()) return rcError("invalid file specification", "198");
  // An empty master (after `clear`: no variables) takes the first file whole.
  // Decided before any using file is materialized, which can move the plane.
  bool emptyMaster = !session.currentHbin().has_value();
  if (!emptyMaster) {
    if (const std::vector<VarInfo>* card = session.residentReadCard()) {
      emptyMaster = card->empty();
    } else if (Datastore* ds = session.datastore(); ds == nullptr || ds->names().empty()) {
      emptyMaster = true;
    }
  }
  std::optional<int64_t> lastRows;
  for (const auto& f : files) {
    // append.ado names the file as typed when it is missing (`file nof not
    // found`, not `nof.dta`), while the stem still reads NAME.dta.
    const std::string path = resolveUsingDataset(session, f);
    if (!fileExists(path)) return rcError("file " + f + " not found", "601");
    lastRows.reset();
    if (extOf(path) == ".dta" || usingFileIsDta(path)) {
      lastRows = usingDtaRows(path);
    } else if (extOf(path) != ".csv") {
      try {
        lastRows = Datastore(path).nRows();
      } catch (const std::exception&) {
        lastRows.reset();
      }
    }
  }
  if (lastRows.has_value()) session.eval.rValues.set("N", static_cast<double>(*lastRows));

  if (genVar.has_value() && !emptyMaster) {
    Datastore* ds = session.datastore();
    if (ds != nullptr && ds->has(*genVar))
      return rcError("variable " + *genVar + " already defined", "110");
  }
  if (files.size() == 1 && !emptyMaster)
    return runAppend(files[0], session, genVar, keepSpec, force, nolabel, nonotes, typeNotes);

  // The marker's code per row, filled as the files land (multi-file / empty master).
  std::vector<int> codes;
  ojson result;
  size_t first = 0;
  if (emptyMaster) {
    result = runUse(files[0], session);
    if (result.is_object() && result.value("type", std::string()) == "error") {
      // A .csv `use` refuses at r(610) "not Stata format"; append's answer for
      // it is "not found" r(601) (delimitedUsingRefused).
      if (result.value("reason", std::string()) == "r(610)") {
        result["reason"] = "r(601)";
        std::string msg = result.value("message", std::string());
        const size_t at = msg.find(" not Stata format");
        if (at != std::string::npos) msg.replace(at, 17, " not found");
        result["message"] = msg;
      }
      return result;
    }
    int64_t n1 = -1;
    if (result.is_object() && result.contains("n_rows_after") &&
        result["n_rows_after"].is_number_integer())
      n1 = result["n_rows_after"].get<int64_t>();
    if (n1 < 0) {
      Datastore* ds = session.datastore();
      if (ds == nullptr) return eval::noDataLoaded();
      n1 = ds->nRows();
    }
    codes.assign(static_cast<size_t>(n1), 1);
    first = 1;
  } else {
    Datastore* ds = session.datastore();
    if (ds == nullptr) return eval::noDataLoaded();
    const int64_t n0 = ds->nRows();
    result = runAppend(files[0], session, genVar, keepSpec, force, nolabel, nonotes, typeNotes);
    if (result.is_object() && result.value("type", std::string()) == "error") return result;
    codes.assign(static_cast<size_t>(n0), 0);
    const int64_t nu = result.value("n_using", int64_t{0});
    codes.insert(codes.end(), static_cast<size_t>(nu), 1);
    first = 1;
  }
  for (size_t k = first; k < files.size(); ++k) {
    result = runAppend(files[k], session, std::nullopt, keepSpec, force, nolabel, nonotes, typeNotes);
    if (result.is_object() && result.value("type", std::string()) == "error") return result;
    const int64_t nu = result.value("n_using", int64_t{0});
    codes.insert(codes.end(), static_cast<size_t>(nu), static_cast<int>(k + 1));
  }
  if (!genVar.has_value()) return result;

  // Write the marker's codes over the result (a new first column after an
  // empty master, else the column the first append created).
  {
    (void)mutationResult(session, "append");
    std::string valuesPath = nextPath(session.workdir(), "append_gen") + ".csv";
    {
      std::ofstream csv(cecon::to_native_path(valuesPath), std::ios::binary);
      if (!csv) return engineErrorBlock("append: could not open scratch CSV for write");
      for (int c : codes) csv << c << "\n";
      csv.flush();
      if (!csv) {
        cecon::native_unlink(valuesPath);
        return engineErrorBlock("append: failed writing scratch CSV (disk full or I/O error)");
      }
    }
    std::vector<std::string> sargs = {"setcol", "--data", *session.currentHbin(), "--var", *genVar,
                                      "--values", valuesPath, "--dtype", "f64"};
    if (emptyMaster) sargs.push_back("--new");
    const std::string genOut = nextPath(session.workdir(), "append_gen");
    sargs.push_back("--out");
    sargs.push_back(genOut);
    CobraRunResult gcr = runCobra(sargs);
    cecon::native_unlink(valuesPath);
    if (!gcr.ok) {
      CobraError ce = errorFromRaw(gcr);
      return engineErrorBlock(cobraErrorStr(ce), ce.code);
    }
    session.load(genOut);
    if (emptyMaster) {
      const std::string ordOut = nextPath(session.workdir(), "append_gen_order");
      CobraRunResult ocr = runCobra({"order", "--data", *session.currentHbin(), "--vars", *genVar,
                                     "--first", "--out", ordOut});
      if (!ocr.ok) {
        CobraError ce = errorFromRaw(ocr);
        return engineErrorBlock(cobraErrorStr(ce), ce.code);
      }
      session.load(ordOut);
    }
  }
  // append.ado's label: `_append` (or `__append` when `_append` is taken), 0
  // "Master", k "Appended dataset k" (dc_a_gen_two_gl); a byte "Dataset source"
  // displayed as wide as its longest text.
  const std::string genLbl =
      (emptyMaster || !session.labelDefs.count("_append") ||
       session.varLblname.count(*genVar))
          ? (session.varLblname.count(*genVar) ? session.varLblname[*genVar] : std::string("_append"))
          : std::string("__append");
  std::map<std::string, std::string> defs = {{"0", "Master"}};
  std::map<int, std::string> numeric = {{0, "Master"}};
  size_t widest = 6;
  for (size_t k = 1; k <= files.size(); ++k) {
    const std::string text = "Appended dataset " + std::to_string(k);
    defs[std::to_string(k)] = text;
    numeric[static_cast<int>(k)] = text;
    widest = std::max(widest, text.size());
  }
  session.labelDefs[genLbl] = defs;
  session.varLblname[*genVar] = genLbl;
  session.valueLabels[*genVar] = numeric;
  setNumericTypeClaim(session, *genVar, std::string("byte"));
  session.varMeta[*genVar].format = "%" + std::to_string(widest) + ".0g";
  session.setVarLabel(*genVar, "Dataset source");
  return mutationResult(session, "append");
}

// ---------------------------------------------------------------------------
// merge 1:1|m:1|1:m KEYLIST using FILE [, keepusing() nogenerate keep()
// assert()] — C++ port of merge_op.py's run_merge. cobra 0.18.0's native
// `merge` verb performs the actual join (factor-key label-join, outer join,
// keep()/assert()/keepusing()/nogenerate all natively supported per
// CONTRACT-engine §22) but its error TEXT diverges from the oracle's
// (verified empirically: cobra's cardinality error omits the duplicate-key
// value and its column-conflict message is shorter) — pre-validate the
// oracle's own checks (key existence, column conflict, cardinality) here so
// error cells match byte-for-byte; success path + option semantics delegate
// to cobra entirely.

namespace {

// _check_cardinality's _find_dup: first duplicated key (by first appearance
// order, mirroring pandas' duplicated(keep=False).idxmax() — the FIRST row
// index where a dup exists) — read raw column values directly (f64 raw
// value or factor level string) to format `{k=v}` like the oracle's
// DataFrame cell repr.
struct DupKeyResult {
  bool found = false;
  std::vector<std::string> keyValueStrs;  // formatted per keylist entry
};

DupKeyResult findDupKey(Datastore& ds, const std::vector<std::string>& keylist) {
  int64_t n = ds.nRows();
  // Build per-row composite key as a joined string (safe delimiter unlikely
  // to collide with real data — good enough for O(n) duplicate detection).
  std::vector<std::vector<double>> f64Cols;
  std::vector<std::vector<int32_t>> facCols;
  std::vector<bool> isFactor;
  for (const auto& k : keylist) {
    if (ds.dtype(k) == "f64") {
      const double* d = ds.col(k).f64();
      isFactor.push_back(false);
      f64Cols.emplace_back(d, d + n);
      facCols.emplace_back();
    } else {
      const int32_t* c = ds.col(k).codes();
      isFactor.push_back(true);
      facCols.emplace_back(c, c + n);
      f64Cols.emplace_back();
    }
  }
  std::map<std::string, int64_t> seen;  // composite key -> first row index
  for (int64_t row = 0; row < n; ++row) {
    std::ostringstream keyOss;
    for (size_t ki = 0; ki < keylist.size(); ++ki) {
      if (ki) keyOss << "\x1f";
      if (isFactor[ki]) {
        keyOss << "f:" << facCols[ki][static_cast<size_t>(row)];
      } else {
        double v = f64Cols[ki][static_cast<size_t>(row)];
        if (std::isnan(v)) keyOss << "n:nan";
        // eval::pythonFloatRepr, not lossy default ostream<<double (~6 sig
        // digits) — distinct high-precision keys (e.g. 1005938 vs 1005942)
        // would otherwise collide into the same composite-key string.
        else keyOss << "n:" << eval::pythonFloatRepr(v);
      }
    }
    std::string composite = keyOss.str();
    auto it = seen.find(composite);
    if (it != seen.end()) {
      DupKeyResult r;
      r.found = true;
      int64_t dupRow = row;
      for (size_t ki = 0; ki < keylist.size(); ++ki) {
        std::ostringstream vs;
        if (isFactor[ki]) {
          const auto& levels = ds.varinfo(keylist[ki]).levels;
          int32_t code = facCols[ki][static_cast<size_t>(dupRow)];
          vs << ((code >= 0 && static_cast<size_t>(code) < levels.size()) ? levels[static_cast<size_t>(code)]
                                                                            : std::to_string(code));
        } else {
          double v = f64Cols[ki][static_cast<size_t>(dupRow)];
          // Python's DataFrame cell repr for a float key: "1.0" not "1" —
          // f64 columns are always floats in pandas, formatted via str().
          if (v == static_cast<double>(static_cast<long long>(v))) {
            vs << static_cast<long long>(v) << ".0";
          } else {
            vs << v;
          }
        }
        r.keyValueStrs.push_back(vs.str());
      }
      return r;
    }
    seen[composite] = row;
  }
  return DupKeyResult{};
}

}  // namespace


ojson runMerge(const std::string& mergeType, const std::vector<std::string>& keylist,
               const std::string& usingPathArg,
               const std::vector<std::pair<std::string, eval::OptValue>>& opts,
               SessionState& session, bool sequential, bool oldSyntax, bool oldSort) {
  if (!session.currentHbin().has_value()) {
    return engineErrorBlock("No dataset loaded. Load a dataset before merge.");
  }
  auto rcError = [](const std::string& code, const std::string& msg, const std::string& rc) {
    return ojson(eval::errorBlock(code, msg, std::nullopt, std::nullopt, std::nullopt,
                                  "r(" + rc + ")"));
  };

  // ---- parsing (merge.ado): no data are touched before it passes ----
  std::optional<std::string> keepusingOpt;
  bool nogenerate = false;
  std::optional<std::string> keepOpt, assertOpt;
  std::optional<std::string> generateOpt;
  bool updateOpt = false, replaceOpt = false, forceOpt = false, sortedOpt = false;
  bool noreportOpt = false;
  bool nonotesOpt = false;
  // BUG-148: resolve Stata option abbreviations to canonical spellings BEFORE reading them.
  static const std::unordered_set<std::string> kMergeCanonical = {
      "keepusing", "nogenerate", "keep",   "assert",   "generate", "update",
      "replace",   "force",      "sorted", "noreport", "nonotes", "nolabel"};
  std::vector<std::pair<std::string, eval::OptValue>> optsC(opts);  // `opts` is const&
  eval::canonicalizeOptionKeys(optsC, kMergeCanonical);
  bool nolabelOpt = false;
  for (const auto& [k, v] : optsC) {
    if (k == "keepusing" && !v.isBool) keepusingOpt = v.strValue;
    if (k == "nogenerate" && v.isBool && v.boolValue) nogenerate = true;
    if (k == "keep" && !v.isBool) keepOpt = v.strValue;
    if (k == "assert" && !v.isBool) assertOpt = v.strValue;
    if (k == "generate" && !v.isBool) generateOpt = trim(v.strValue);
    if (k == "update" && (!v.isBool || v.boolValue)) updateOpt = true;
    if (k == "replace" && (!v.isBool || v.boolValue)) replaceOpt = true;
    if (k == "force" && v.isBool && v.boolValue) forceOpt = true;
    if (k == "sorted" && v.isBool && v.boolValue) sortedOpt = true;
    if (k == "noreport" && v.isBool && v.boolValue) noreportOpt = true;
    if (k == "nonotes" && v.isBool && v.boolValue) nonotesOpt = true;
    if (k == "nolabel" && v.isBool && v.boolValue) nolabelOpt = true;
  }
  // BUG-105 (옵션망): any other residual option token is a named reject.
  if (auto err = eval::rejectUnknownOptions(optsC, kVocabMerge.known, kVocabMerge.unimplemented)) {
    return *err;
  }
  // resultlist("assert") / resultlist("keep") (merge).
  std::vector<int> keepCodes, assertCodes;
  if (assertOpt.has_value())
    if (auto err = parseMergeResults(*assertOpt, assertCodes)) return *err;
  if (keepOpt.has_value())
    if (auto err = parseMergeResults(*keepOpt, keepCodes)) return *err;
  // Stata: `generate()` RENAMES the marker; `nogenerate` suppresses it.
  if (generateOpt.has_value() && nogenerate) {
    return engineErrorBlock(
        "merge: `generate()` and `nogenerate` are alternatives — one names the marker variable, "
        "the other suppresses it.",
        "bad_input");
  }
  if (generateOpt.has_value() &&
      (generateOpt->empty() || !strutil::isValidStataIdentifier(*generateOpt))) {
    return rcError("syntax_error", "merge: generate() requires a variable name — got `" +
                                       generateOpt.value_or("") + "'",
                   "198");
  }
  // BUG-1120 review: a reserved marker name is refused by the new-variable rule
  // with _se and the r(table) row names ("_cons invalid varname", r(198)); alias
  // and _skip are "NAME not allowed" r(101) (StataBE 19.5).
  if (generateOpt.has_value()) {
    if (auto r = strutil::newVarNameRefusal(*generateOpt, /*resultRows=*/true))
      return rcError("syntax_error", r->message, std::to_string(r->rc));
    if (*generateOpt == "alias" || *generateOpt == "_skip")
      return rcError("syntax_error", *generateOpt + " not allowed", "101");
  }
  // `confirm new var` (merge): parse time, the data are not sorted yet
  // (dc_mg_gen_exists r(110), k=6 N=6 unchanged).
  auto hasVar = [&](const std::string& nm) {
    for (const auto& [n, dt] : currentNameDtypes(session))
      if (n == nm) return true;
    return false;
  };
  if (generateOpt.has_value() && hasVar(*generateOpt))
    return rcError("engine_error", "variable " + *generateOpt + " already defined", "110");
  // `replace` (merge): requires `update`.
  if (replaceOpt && !updateOpt) {
    return rcError("syntax_error",
                   "option replace not allowed\n    option replace requires you also specify "
                   "option update, thus demonstrating your understanding that an update will be "
                   "performed",
                   "198");
  }

  // Resolve the `using` file against session.cwd (native-paths stabilization
  // item 2) — session.workdir() is the internal hbin-chain scratch dir, not
  // where a user's own dataset files live.
  // [D] merge: "If filename is specified without an extension, .dta is
  // assumed" (w2d_uncovered.log merge_noforce_ctl: `using "w2d_mu"` reads
  // w2d_mu.dta -> r(106) on its str3 w).
  std::string usingShown = usingPathArg;
  std::string usingPath = resolveUsingDataset(session, usingPathArg, &usingShown);
  if (!fileExists(usingPath)) {
    // merge.ado sorts the master before `describe using` finds no
    // file, so the r(601) leaves the data sorted (dc_mx_nofile_d).
    if (!sortedOpt && !sequential && !oldSyntax) {
      for (const auto& k : keylist)
        if (!hasVar(k)) return rcError("engine_error", "variable " + k + " not found", "111");
      ojson sorted = runSort(keylist, session);
      if (sorted.is_object() && sorted.value("type", std::string()) == "error") return sorted;
    }
    return rcError("engine_error", "file " + usingShown + " not found", "601");
  }

  Datastore* master = session.datastore();
  if (master == nullptr) return eval::noDataLoaded();

  // The using side may be .dta/.csv — pre-convert to hbin so we can
  // pre-validate keys/cardinality on it directly.
  std::string usingHbinPath = usingPath;
  std::string ext = lower(usingPath.substr(usingPath.find_last_of('.') == std::string::npos
                                               ? usingPath.size()
                                               : usingPath.find_last_of('.')));
  std::string usingMetaPath;  // xtset-file-persistence: the using side's char table
  if (ext == ".dta" || usingFileIsDta(usingPath)) {
    std::string conv = nextPath(session.workdir(), "merge_using_dta");
    usingMetaPath = conv + ".meta.json";
    registerScratchMint(usingMetaPath);  // BUG-389 (same sidecar shape as append's)
    CobraRunResult cr = runCobra({"import-dta", usingPath, conv, "--out-meta", usingMetaPath});
    if (!cr.ok) {
      CobraError ce = errorFromRaw(cr);
      return engineErrorBlock("merge using: cannot convert '" + usingPath + "': " + ce.what());
    }
    usingHbinPath = conv;
  } else if (ext == ".csv" || ext == ".tsv") {
    std::string conv = nextPath(session.workdir(), "merge_using_csv");
    char delim = (ext == ".tsv") ? '\t' : ',';
    CobraRunResult cr =
        runCobra({"convert", "--data", usingPath, "--out", conv, "--delim", std::string(1, delim)});
    if (!cr.ok) return delimitedUsingRefused(usingShown, 610);
    usingHbinPath = conv;
  }

  Datastore usingDs(usingHbinPath);

  for (const auto& k : keylist) {
    if (!master->has(k))
      return rcError("engine_error", "variable " + k + " not found", "111");
    if (!usingDs.has(k))
      return rcError("engine_error", "variable " + k + " not found in using data", "111");
  }

  // `sorted` (merge skips its sort): the built-in _merge then needs both
  // files marked sorted by the keys, else r(5) with the data as they were
  // (dc_ms_unsorted_master, dc_ms_unsorted_using).
  if ((sortedOpt && !sequential) || (oldSyntax && !oldSort)) {
    auto leads = [&](const std::string& claim) {
      std::istringstream in(claim);
      std::vector<std::string> c;
      for (std::string w; in >> w;) c.push_back(w);
      if (keylist.size() > c.size()) return false;
      for (size_t i = 0; i < keylist.size(); ++i)
        if (c[i] != keylist[i]) return false;
      return true;
    };
    if (!leads(master->sortedBy())) return rcError("engine_error", "master data not sorted", "5");
    if (!leads(usingDs.sortedBy())) return rcError("engine_error", "using data not sorted", "5");
  }

  // BUG-548: keepusing() is a varlist parsed against the USING names.
  std::vector<std::string> usingNonKeyKept;
  std::string keepusingCsv;
  std::set<std::string> keptUsing;  // the using names the merge brings (keys included)
  if (keepusingOpt.has_value()) {
    std::vector<std::string> expanded;
    if (auto err = expandUsingKeepList(*keepusingOpt, usingDs.names(), expanded)) return *err;
    for (size_t i = 0; i < expanded.size(); ++i) {
      if (i) keepusingCsv += ",";
      keepusingCsv += expanded[i];
    }
    std::set<std::string> full(keylist.begin(), keylist.end());
    full.insert(expanded.begin(), expanded.end());
    for (const auto& n : usingDs.names()) {
      if (full.count(n)) usingNonKeyKept.push_back(n);
    }
  } else {
    usingNonKeyKept = usingDs.names();
  }
  keptUsing.insert(usingNonKeyKept.begin(), usingNonKeyKept.end());

  const std::vector<std::string> masterNamesBefore = master->names();
  const std::set<std::string> keySet(keylist.begin(), keylist.end());
  // Shared non-key variables the using brings (keepusing permitting).
  std::vector<std::string> sharedVars;
  for (const auto& n : masterNamesBefore)
    if (!keySet.count(n) && keptUsing.count(n)) sharedVars.push_back(n);

  // ---- the checks the built-in _merge makes after merge.ado sorted the
  // master (:98-104): their refusals leave the master SORTED by the keys ----
  const std::string marker = nogenerate ? std::string() : generateOpt.value_or("_merge");
  std::optional<ojson> lateRefusal;
  if (!marker.empty() && hasVar(marker)) {
    lateRefusal = rcError("engine_error", "variable " + marker + " already defined", "110");
  }
  if (!lateRefusal) {
    for (const auto& k : keylist) {
      if (master->dtype(k) != usingDs.dtype(k)) {
        const bool mStr = master->dtype(k) == "factor";
        lateRefusal = rcError("engine_error",
                              "key variable " + k + " is " + (mStr ? "str" : "numeric") +
                                  " in master but " + (mStr ? "numeric" : "str") +
                                  " in using data",
                              "106");
        break;
      }
    }
  }
  std::vector<std::string> forcedVars;  // string/numeric clashes `force` lets through
  if (!lateRefusal) {
    for (const auto& n : sharedVars) {
      if (master->dtype(n) == usingDs.dtype(n)) continue;
      if (forceOpt) {
        forcedVars.push_back(n);
        continue;
      }
      const bool mStr = master->dtype(n) == "factor";
      lateRefusal = rcError(
          "engine_error",
          "variable " + n + " is " + (mStr ? "str" : "numeric") + " in master but " +
              (mStr ? "numeric" : "str") +
              " in using data\n    You could specify merge's force option to ignore this "
              "numeric/string mismatch.  The using variable would then be treated as if it "
              "contained \"\" or numeric missing value.",
          "106");
      break;
    }
  }
  if (!lateRefusal && !sequential) {
    auto dupRefusal = [&](Datastore& ds, const char* side) -> std::optional<ojson> {
      DupKeyResult dup = findDupKey(ds, keylist);
      if (!dup.found) return std::nullopt;
      std::string vars;
      for (size_t i = 0; i < keylist.size(); ++i) vars += (i ? " " : "") + keylist[i];
      return rcError("engine_error",
                     std::string(keylist.size() == 1 ? "variable " : "variables ") + vars +
                         (keylist.size() == 1 ? " does" : " do") +
                         " not uniquely identify observations in the " + side + " data",
                     "459");
    };
    if (mergeType == "1:1" || mergeType == "1:m") lateRefusal = dupRefusal(*master, "master");
    if (!lateRefusal && (mergeType == "1:1" || mergeType == "m:1")) {
      lateRefusal = dupRefusal(usingDs, "using");
      // The using side is checked after the master's keys were promoted to hold
      // the using's values (dc_mx_11_dupusing_d_v1 id int, from byte).
      if (lateRefusal) {
        const std::map<std::string, UsingVarMeta> um =
            usingVarMetaOf(session, usingPath, usingMetaPath, usingDs);
        for (const auto& k : keylist) {
          auto it = um.find(k);
          if (it == um.end() || !it->second.stataType.has_value() || master->dtype(k) != "f64" ||
              !isNumericStataType(*it->second.stataType))
            continue;
          const std::optional<std::string> m = numericTypeClaim(session, k);
          if (m.has_value() && !isNumericStataType(*m)) continue;
          const std::optional<std::string> promoted = appendPromotedType(m, it->second.stataType);
          if (promoted.has_value() && promoted != m) {
            VarMeta& vm = session.varMeta[k];
            if (!vm.format.has_value() || vm.format->empty()) vm.format = stataDefaultFormat(m);
            setNumericTypeClaim(session, k, promoted);
          }
        }
      }
    }
  }
  if (lateRefusal) {
    if (!sortedOpt && !sequential && !oldSyntax) {
      ojson sorted = runSort(keylist, session);
      if (sorted.is_object() && sorted.value("type", std::string()) == "error") return sorted;
    }
    return *lateRefusal;
  }

  // ---- the merge: cobra's built-in _merge plan (MergeOpts::stata_plan) ----
  std::string joinedKeys;
  for (size_t i = 0; i < keylist.size(); ++i) {
    if (i) joinedKeys += ",";
    joinedKeys += keylist[i];
  }
  // nogenerate with update still needs the marker for r(N) (count of 4):
  // it is generated under a scratch name and dropped below.
  const bool scratchMarker = nogenerate && updateOpt && !noreportOpt;
  const std::string engineMarker = scratchMarker ? std::string("__000merge") : marker;
  auto mergeArgs = [&](bool withResults, const std::string& outPath) {
    std::vector<std::string> args = {"merge",     "--data", master->path(), "--using",
                                      usingHbinPath, "--type", mergeType,     "--stata-plan"};
    if (sequential) {
      args.push_back("--sequential");
    } else {
      args.push_back("--key");
      args.push_back(joinedKeys);
    }
    if (sortedOpt) args.push_back("--sorted");
    if (keepusingOpt.has_value()) {
      args.push_back("--keepusing");
      args.push_back(keepusingCsv);
    }
    if (engineMarker.empty()) {
      args.push_back("--nogenerate");
    } else if (engineMarker != "_merge") {
      args.push_back("--generate");
      args.push_back(engineMarker);
    }
    if (updateOpt) {
      args.push_back("--update");
      if (replaceOpt) args.push_back("--replace");
    }
    auto codesCsv = [](const std::vector<int>& c) {
      std::string s;
      for (int x : c) s += (s.empty() ? "" : " ") + std::to_string(x);
      return s;
    };
    if (withResults && !keepCodes.empty()) {
      args.push_back("--keep");
      args.push_back(codesCsv(keepCodes));
    }
    if (withResults && !assertCodes.empty()) {
      args.push_back("--assert");
      args.push_back(codesCsv(assertCodes));
    }
    args.push_back("--out");
    args.push_back(outPath);
    return args;
  };
  std::string outPath = nextPath(session.workdir(), "merge");
  CobraRunResult cr;
  {
    SessionState::ResidentEligible residentMerge(session);
    cr = runCobra(mergeArgs(true, outPath));
  }
  bool assertFailed = false;
  if (!cr.ok) {
    CobraError ce = errorFromRaw(cr);
    if (ce.code != "merge_assert") return engineErrorBlock(cobraErrorStr(ce), ce.code);
    // merge.ado checks assert() before keep() and on failure exits
    // r(9) with the merged result -- labelled, keep() not applied -- left in
    // memory (dc_ma_fail_3_d k=9 N=8, dc_ma_fail_keep_d N=8).
    assertFailed = true;
    outPath = nextPath(session.workdir(), "merge");
    SessionState::ResidentEligible residentMerge(session);
    cr = runCobra(mergeArgs(false, outPath));
    if (!cr.ok) {
      CobraError ce2 = errorFromRaw(cr);
      return engineErrorBlock(cobraErrorStr(ce2), ce2.code);
    }
  }

  // What the using file states about its variables, read while it is open.
  std::map<std::string, UsingVarMeta> uMeta = usingVarMetaOf(session, usingPath, usingMetaPath, usingDs);

  session.load(outPath);
  // xtset-file-persistence: the result keeps the MASTER's characteristics; the
  // using side's ts declaration fills in only when the master states none.
  // Oracle: w2_eststo_prefix.log:541 (B3, master un-xtset, using xtset -> xtreg
  // rc 0) and :582 (B3 CONTROL, master xtset, using not -> rc 0). Scoped to the
  // ts entries — no oracle here pins how merge folds the using side's other chars.
  const bool masterUndeclared = !(session.panelvar.has_value() && !session.panelvar->empty()) &&
                                !(session.timevar.has_value() && !session.timevar->empty());
  // BUG-659: a native checkpoint (`.hbin`/`.tmp`) this session saved states its
  // declaration beside its path rather than in a char table — same fill rule.
  const SessionState::CheckpointTsDecl* usingCk =
      (masterUndeclared && usingMetaPath.empty()) ? session.checkpointTsDecl(usingPath) : nullptr;
  if (usingCk != nullptr &&
      ((usingCk->panelvar.has_value() && !usingCk->panelvar->empty()) ||
       (usingCk->timevar.has_value() && !usingCk->timevar->empty()))) {
    session.panelvar = usingCk->panelvar;
    session.timevar = usingCk->timevar;
    session.delta = usingCk->delta;
    session.writeTsChars();
  }
  if (!usingMetaPath.empty() && masterUndeclared) {
    try {
      std::ifstream mf(cecon::to_native_path(usingMetaPath), std::ios::binary);
      std::ostringstream ms;
      ms << mf.rdbuf();
      nlohmann::json meta = nlohmann::json::parse(ms.str());
      if (meta.contains("characteristics") && meta["characteristics"].is_array()) {
        bool adopted = false;
        for (const auto& ch : meta["characteristics"]) {
          if (!ch.is_object()) continue;
          const std::string vn = ch.value("varname", std::string());
          const std::string cn = ch.value("charname", std::string());
          if (vn != "_dta" || !SessionState::isTsCharName(cn) ||
              !ch.contains("value") || !ch["value"].is_string())
            continue;
          if (session.dtaChar(cn) != nullptr) continue;
          session.characteristics.push_back(
              DtaCharacteristic{vn, cn, ch["value"].get<std::string>()});
          adopted = true;
        }
        if (adopted) session.adoptTsFromChars();
      }
    } catch (const std::exception&) {
      // An unreadable sidecar leaves the master's (empty) declaration as it is —
      // the merge itself already succeeded on the engine's own read of the file.
    }
  }
  ojson result = mutationResult(session, "merge");
  // After mutationResult, which leaves the card current: appendUsingNotes reads
  // the result's variable names from it. Before, no card existed yet and the
  // names came from a Datastore, which wrote the whole merged frame to disk
  // (BUG-1089). The notes touch only the char table, which the result block
  // does not carry.
  if (!nonotesOpt) appendUsingNotes(session, usingPath, usingMetaPath);
  // Storage types, labels and formats (BUG-666, DC4, DC5).
  {
    const std::set<std::string> inMaster(masterNamesBefore.begin(), masterNamesBefore.end());
    const std::set<std::string> forced(forcedVars.begin(), forcedVars.end());
    std::vector<std::string> brought;
    for (const auto& [nm, dtype] : currentNameDtypes(session)) {
      auto uit = uMeta.find(nm);
      const UsingVarMeta* u = uit == uMeta.end() ? nullptr : &uit->second;
      if (!inMaster.count(nm)) {
        // A variable the merge brings in keeps the using file's storage type,
        // label and format (dc_m11_v6 yval `double|%10.3f|[y of u]`, dc_m11_v7
        // wtxt str4).
        brought.push_back(nm);
        if (u == nullptr) continue;
        if (dtype == "f64") {
          std::optional<std::string> t;
          if (u->stataType.has_value() && isNumericStataType(*u->stataType)) t = u->stataType;
          setNumericTypeClaim(session, nm, t);
        } else {
          if (const auto m = numericTypeClaim(session, nm); m && isNumericStataType(*m))
            setNumericTypeClaim(session, nm, std::nullopt);  // stale numeric claim on a string
          if (u->stataType.has_value() && !isNumericStataType(*u->stataType))
            session.varMeta[nm].stataType = *u->stataType;
        }
        if (!u->label.empty()) session.setVarLabel(nm, u->label);
        if (!u->format.empty()) {
          VarMeta& vm = session.varMeta[nm];
          if (!vm.format.has_value() || vm.format->empty()) vm.format = u->format;
        }
        continue;
      }
      // A key or a shared variable the using brings: merge promotes the
      // master's storage to hold the using's values (`(variable id was byte,
      // now int to accommodate using data's values)`, dc_m11_v1 id int, v int,
      // t str3). `force` keeps a clashing variable's master type.
      if (u == nullptr || (!keySet.count(nm) && !keptUsing.count(nm)) || forced.count(nm)) continue;
      if (!u->stataType.has_value()) continue;
      const std::optional<std::string> m = numericTypeClaim(session, nm);
      if (dtype == "f64") {
        if (!isNumericStataType(*u->stataType)) continue;
        if (m.has_value() && !isNumericStataType(*m)) continue;
        const std::optional<std::string> promoted = appendPromotedType(m, u->stataType);
        if (promoted.has_value() && promoted != m) {
          VarMeta& vm = session.varMeta[nm];
          if (!vm.format.has_value() || vm.format->empty()) vm.format = stataDefaultFormat(m);
          setNumericTypeClaim(session, nm, promoted);
        }
      } else if (m.has_value()) {
        const int mw = strWidthOf(*m), uw = strWidthOf(*u->stataType);
        if (*u->stataType == "strL" && *m != "strL") {
          session.varMeta[nm].stataType = std::string("strL");
        } else if (mw >= 0 && uw > mw) {
          VarMeta& vm = session.varMeta[nm];
          vm.stataType = *u->stataType;
        }
      }
      // Under update, a shared variable unlabelled in the master takes the
      // using's label (dc_mu_m1_update_replace v `[v of u]`; without update it
      // stays unlabelled, dc_mm1_v3 `[]`).
      if (updateOpt && !keySet.count(nm) && !u->label.empty()) {
        auto vit = session.varMeta.find(nm);
        const bool masterLabelled = vit != session.varMeta.end() && vit->second.label.has_value() &&
                                    !vit->second.label->empty();
        if (!masterLabelled) session.setVarLabel(nm, u->label);
      }
    }
    session.adoptVirtualSaveFormats(usingPath, brought);
    // Value labels (StataBE uni-oracle/vm.do; AER 007's Combined_waves1and2_final
    // lost all of Wave 2's 43 labelled variables): a variable the merge brings in
    // keeps its attachment; a shared one keeps the master's, even none; the
    // using's definitions are copied unless the master already defines the name
    // (`both` stays the master's), and `nolabel` copies no definition while the
    // attachments still come.
    adoptUsingValueLabels(session, usingPath, usingMetaPath, brought, !nolabelOpt);
    // The marker (merge.ado): byte %23.0g, value label `_merge`
    // (defined unless a `_merge` label already is), variable label "Matching
    // result from merge" (dc_m11_g `[_merge]`, dc_m11_gl, dc_m11_v9).
    if (!marker.empty() && oldSyntax) {
      // merge_10: a byte %8.0g marker with no labels (dc_mold_g `[]`, dc_mold_v9).
      setNumericTypeClaim(session, marker, std::string("byte"));
      session.varMeta[marker].format = std::string("%8.0g");
    } else if (!marker.empty()) {
      setNumericTypeClaim(session, marker, std::string("byte"));
      session.varMeta[marker].format = std::string("%23.0g");
      if (!session.labelDefs.count("_merge")) {
        session.labelDefs["_merge"] = {{"1", "Master only (1)"},
                                       {"2", "Using only (2)"},
                                       {"3", "Matched (3)"},
                                       {"4", "Missing updated (4)"},
                                       {"5", "Nonmissing conflict (5)"}};
      }
      std::map<int, std::string> numericMap;
      for (const auto& [code, text] : session.labelDefs["_merge"]) {
        try {
          numericMap[std::stoi(code)] = text;
        } catch (const std::exception&) {
        }
      }
      session.varLblname[marker] = "_merge";
      session.valueLabels[marker] = numericMap;
      session.setVarLabel(marker, "Matching result from merge");
    }
  }

  // Counts: the resident leg publishes them on the frame response, the
  // stateless leg in its stderr notice. They are the plan's, before keep().
  std::optional<int64_t> usingOnly;
  if (const CobraFrameMeta* mm = session.lastResidentMergeCounts()) {
    ojson counts;
    counts["master_only"] = mm->mergeMasterOnly;
    counts["using_only"] = mm->mergeUsingOnly;
    counts["matched"] = mm->mergeMatched;
    usingOnly = mm->mergeUsingOnly;
    if (!marker.empty()) result["merge_counts"] = counts;
  } else {
    static const std::regex reCounts(R"(master_only=(\d+)\s+using_only=(\d+)\s+matched=(\d+))");
    std::smatch m;
    if (std::regex_search(cr.stderr_text, m, reCounts)) {
      ojson counts;
      counts["master_only"] = std::stoll(m.str(1));
      counts["using_only"] = std::stoll(m.str(2));
      counts["matched"] = std::stoll(m.str(3));
      usingOnly = std::stoll(m.str(2));
      if (!marker.empty()) result["merge_counts"] = counts;
    }
  }

  // r(N) (merge's result table): `count if marker==2` after keep(),
  // or `count if marker==4` under update (dc_m11_r 2, dc_mk_master_r 0,
  // dc_mu_update_r 1). With noreport only a 1:1 merge counts (==2).
  // A failed assert() exits before both, leaving r() as it was (dc_ma_fail_3_r).
  if (!assertFailed && !oldSyntax && (!noreportOpt || mergeType == "1:1")) {
    const bool countFour = updateOpt && !noreportOpt;
    if (!countFour) {
      if (usingOnly.has_value()) {
        const bool kept2 = keepCodes.empty() ||
                           std::find(keepCodes.begin(), keepCodes.end(), 2) != keepCodes.end();
        session.eval.rValues.set("N", static_cast<double>(kept2 ? *usingOnly : 0));
      }
    } else if (!engineMarker.empty()) {
      // The update split (code 4) is not in the published counts: read the marker.
      if (Datastore* ds = session.datastore(); ds != nullptr && ds->has(engineMarker)) {
        const double* v = ds->col(engineMarker).f64();
        const int64_t n = ds->nRows();
        int64_t c = 0;
        for (int64_t r = 0; v != nullptr && r < n; ++r) c += v[r] == 4.0 ? 1 : 0;
        session.eval.rValues.set("N", static_cast<double>(c));
      }
    }
  }
  if (scratchMarker) {
    (void)mutationResult(session, "merge");
    ojson dropped = runDrop({engineMarker}, session);
    if (dropped.is_object() && dropped.value("type", std::string()) == "error") return dropped;
    session.forgetVarPlanes(engineMarker);
    result = mutationResult(session, "merge");
  }
  if (assertFailed) {
    std::string text;
    for (size_t i = 0; i < assertCodes.size(); ++i) {
      static const char* kText[] = {"", "from master", "from using", "matched",
                                    "matched and updated", "matched and conflicting"};
      const int c = assertCodes[i];
      const std::string t = (c >= 1 && c <= 5) ? kText[c] : "of unknown type";
      if (i) text += (i + 1 == assertCodes.size()) ? (assertCodes.size() > 2 ? ", or " : " or ") : ", ";
      text += t;
    }
    return rcError("engine_error",
                   "after merge, not all observations " + text + "\n(merged result left in memory)",
                   "9");
  }
  return result;
}

ojson runDuplicates(const std::string& sub, const std::optional<std::vector<std::string>>& varlist,
                    const std::optional<std::string>& genvar, SessionState& session,
                    const std::optional<std::string>& ifExpr,
                    const std::optional<std::string>& inRange) {
  std::string subLower = lower(sub);
  if (subLower == "report") return runDuplicatesReport(varlist, session, ifExpr, inRange);
  if (subLower == "drop") return runDuplicatesDrop(varlist, session, ifExpr, inRange);
  if (subLower == "tag") {
    if (!genvar.has_value() || genvar->empty()) {
      return engineErrorBlock("duplicates tag: ,gen(NEWVAR) required");
    }
    return runDuplicatesTag(varlist, *genvar, session, ifExpr, inRange);
  }
  if (subLower == "list") return runDuplicatesList(varlist, session, ifExpr, inRange);
  if (subLower == "examples") return runDuplicatesExamples(varlist, session, ifExpr, inRange);
  return engineErrorBlock(
      "duplicates " + sub +
      ": unknown subcommand. Use: duplicates report|examples|drop|tag|list [VARLIST]");
}

// ---------------------------------------------------------------------------
// encode STRVAR, gen(NEWVAR) [label(LBLNAME)] — C++ port of encode_op.py's
// run_encode. Folded into M4a (ROADMAP M4a, blocking the debug_all.do depth
// boundary at line 157) even though it predates this campaign's explicit
// item list.
//
// L1/M3 (CSV-shim retirement): this calls cobra's native `encode` verb
// (`--var SRC --gen NEW`), which appends NEW as an f64 column (codes in
// alphabetical level order, 1-based, missing preserved) and byte-preserves
// the source and every other column — no whole-frame CSV dump, no
// re-factorization. NEW lands f64, not factor, exactly the oracle's contract
// (encode's output is a plain numeric column with a value-label ATTACHMENT).
// (History: this used to dump the whole dataset to CSV with the extra
// alphabetically-ranked column and `cobra convert` it back; superseded.)
ojson runEncode(const std::string& strvar, const std::string& newvar,
                const std::optional<std::string>& lblname, SessionState& session,
                const std::optional<std::string>& ifExpr, bool noExtend) {
  if (!session.currentHbin().has_value()) return eval::noDataLoaded();
  // fix/verb-overhead-sweep S4: encode's entry validation reads a NAME, a
  // DTYPE and another NAME — no column data — so the resident shadow card
  // answers all three, and the datastore open (cecon::Dataset::open: mmap +
  // 192-column header parse) is skipped. Same mechanism as runGsort/runOrder
  // above; the dtype half is the S4a treatment (strmutate.cpp's factorColumns),
  // where the A12 argument for trusting the card's dtype is written out in
  // full: the card's vocabulary is exactly Datastore::dtype()'s ("factor" |
  // "f64"), it is the BASE type on both sides, and any other spelling is
  // refused rather than guessed. Refusal here means falling into the else
  // branch, which is the untouched prior behavior.
  //
  // SCOPE, STATED HONESTLY. This is NOT where encode's measured ~0.24 s/call
  // goes. That is dominated by mutationResult's card rebuild rescanning all 192
  // columns for n_missing, because "encode" is missing from
  // SessionState::seedNMissForPureAdd's kPureAdd set (session_state.cpp) even
  // though encode is a textbook pure add — a one-token fix in a file this stint
  // does not touch. Removing the wasted open here does not substitute for it.
  //
  // The three facts the checks below need, resolved from the card when it can
  // supply them and from the datastore otherwise. Resolution is separated from
  // checking on purpose: the three rejections must keep firing in their
  // ORIGINAL order (not-found, then not-a-string, then already-defined), since
  // an input that trips two of them must keep returning the same message it
  // always did. Collapsing the lookups into the checks is what would silently
  // reorder them.
  bool haveFacts = false;
  bool srcExists = false;
  std::string srcDtype;
  bool newExists = false;
  if (const auto* card = session.shadowVarlistIfCurrent()) {
    const VarInfo* src = cardFind(*card, strvar);
    // An unrecognized dtype spelling is refused rather than guessed (S4a's
    // rule) — haveFacts stays false and the datastore branch re-answers all
    // three, exactly as before this fix.
    if (src == nullptr || src->dtype == "factor" || src->dtype == "f64") {
      srcExists = (src != nullptr);
      if (src != nullptr) srcDtype = src->dtype;
      newExists = (cardFind(*card, newvar) != nullptr);
      haveFacts = true;
    }
  }
  if (!haveFacts) {
    Datastore* ds = session.datastore();
    if (ds == nullptr) return eval::noDataLoaded();
    srcExists = ds->has(strvar);
    if (srcExists) srcDtype = ds->dtype(strvar);
    newExists = ds->has(newvar);
  }

  // Stata's rc for each refusal (w2ah_data_verbs.log ah_encode_numeric: 107 --
  // main answered rc 7; ah_encode_exist: 110).
  if (!srcExists) {
    return rcErrorBlock("variable " + strvar + " not found", 111);
  }
  if (srcDtype != "factor") {
    return rcErrorBlock(strvar + ": not possible with numeric variable", 107);
  }
  // BUG-1096: noextend reads the label() set as it stands, so it needs one
  // (StataBE 19.5 order: the source checks, then this, then r(110)).
  if (noExtend && (!lblname.has_value() || lblname->empty())) {
    return rcErrorBlock("noextend option not allowed without label() option", 198);
  }
  // BUG-1120: after the source checks, a reserved generate() name is refused by
  // the new-variable rule with _se and the r(table) row names: "_cons invalid
  // varname", "long invalid varname", "str5 invalid variable name", r(198); _skip
  // and _all are legal here (StataBE 19.5).
  if (auto r = strutil::newVarNameRefusal(newvar, /*resultRows=*/true)) {
    return rcErrorBlock(r->message, r->rc);
  }
  if (newExists) {
    return rcErrorBlock("variable " + newvar + " already defined", 110);
  }

  bool haveIf = ifExpr.has_value() && !ifExpr->empty();

  // BUG-1096: under noextend every string the sample holds must already be a
  // text of the label() set (a set that does not exist has none); otherwise
  // nothing is created and the strangers are named, in sorted order, r(459):
  // "Unexpected values encountered:" / "ccc" (StataBE 19.5; "" is missing, never
  // a stranger, and rows outside `if` are not looked at).
  if (noExtend) {
    std::set<std::string> texts;
    if (auto set = session.labelDefs.find(*lblname); set != session.labelDefs.end())
      for (const auto& [code, text] : set->second) texts.insert(text);
    std::optional<Datastore> sampleFile;
    Datastore* sample = nullptr;
    if (haveIf) {
      try {
        InRangeFilterResult fr =
            applyIfAndInFilter(session, *session.currentHbin(), ifExpr, std::nullopt);
        sampleFile.emplace(fr.dataPath);
      } catch (const CobraError& ce) {
        return engineErrorBlock(cobraErrorStr(ce), ce.code);
      } catch (const std::exception& e) {
        return engineErrorBlock(std::string("encode: ") + e.what());
      }
      sample = &*sampleFile;
    } else {
      sample = session.datastore();
    }
    if (sample == nullptr || !sample->has(strvar)) return eval::noDataLoaded();
    const cecon::Column& c = sample->col(strvar);
    std::set<std::string> strangers;
    for (int64_t i = 0; i < sample->nRows(); ++i) {
      const int32_t code = c.codes()[i];
      if (code < 0 || static_cast<size_t>(code) >= c.levels.size()) continue;
      std::string text(c.levels[static_cast<size_t>(code)]);
      if (!text.empty() && texts.count(text) == 0) strangers.insert(std::move(text));
    }
    if (!strangers.empty()) {
      std::string msg = "Unexpected values encountered:";
      for (const auto& s : strangers) msg += "\n" + s;
      return rcErrorBlock(msg, 459);
    }
  }

  // L1/M3 (CSV-shim retirement): cobra's native `encode` produces NEWVAR as
  // f64 with codes in ALPHABETICAL level order (1-based; missing stays
  // missing), byte-preserving the source column and every other column (no
  // whole-frame CSV dump, no re-factorization). Its stdout JSON carries the
  // alphabetical "levels" array, which drives the value-label attachment
  // below — codes and labels thus come from the SAME engine sort and always
  // agree. `--gen` lands f64, not factor: exactly encode's contract.
  //
  // BUG-042 (`encode STRVAR if COND, gen(NEWVAR)`): Stata restricts which
  // observations get a real code — excluded rows come back MISSING on NEWVAR,
  // while the level MAPPING still spans the WHOLE variable (encode's
  // value-label set is dataset-wide, not tied to one invocation's sample). So
  // encode runs on the full data (all rows -> whole-variable levels) into a
  // temp column, then a cobra `cond()` mask nulls the out-of-sample rows —
  // the identical `if`-folding runClonevar's numeric fast path uses.
  std::string encGen = haveIf ? ("__encode_full_" + scratchToken()) : newvar;
  std::string encOut = nextPath(session.workdir(), "encode");
  // *session.currentHbin() rather than ds->path() — same substitution runGsort
  // and runRename make, and the reason `ds` no longer needs to exist here.
  std::vector<std::string> encArgs = {"encode", "--data", *session.currentHbin(), "--out",
                                      encOut,   "--var",  strvar,                 "--gen", encGen};
  // C4-L4: cobra's cmd_encode rides the same §26 acquire_data/commit_frame
  // bridge as setcol (apps/cmds/cmds_data.cpp:693/:698 — read cross-lane, no
  // cobra change), so the SAME resident opt-in routes it through the frame
  // plane: virtual head, vars_delta fold, no whole-Datastore reconstruct —
  // and on a dirty virtual head no EMH materialization at the stateless
  // --data site (the a2 encode window class). The `if` mask's eval below
  // rides the same scope. Escapes as at the destring site; the stateless
  // fallback (plane down ⇒ no virtual head) is byte-identical to before.
  const bool residentEncode = std::getenv("PIET_NO_RESIDENT_SETCOL") == nullptr &&
                              !residentReadDisabled();
  std::optional<SessionState::ResidentEligible> encodeResident;
  if (residentEncode) encodeResident.emplace(session);
  nlohmann::json encJson;
  try {
    encJson = runCobraJson(encArgs);
  } catch (const CobraError& ce) {
    return engineErrorBlock(cobraErrorStr(ce), ce.code);
  }

  // Alphabetical levels straight from the engine's own encode output, so the
  // value labels below describe exactly the codes it assigned. A level that is
  // not UTF-8 arrives escaped (BUG-1372; cmd_encode) and is turned back here.
  std::vector<std::string> sortedLevels;
  if (encJson.contains("levels") && encJson.at("levels").is_array()) {
    for (const auto& lv : encJson.at("levels")) {
      sortedLevels.push_back(lv.get<std::string>());
      cecon::hbin_level_unescape(sortedLevels.back());
    }
  }

  if (haveIf) {
    // Mask out-of-sample rows to missing (in-sample rows keep encGen, itself
    // already missing where the source was missing), then drop the temp — the
    // same one-shot `--gen COND-cond() --drop TMP` shape collapse's proxy
    // step used.
    std::string finalOut = nextPath(session.workdir(), "encode");
    // BUG-369 follow-up: the mask cond() embeds the user's if, which can carry
    // a TS operator (`encode s if L.x>0, gen(v)`).
    std::string maskExpr = newvar + "=cond((" + *ifExpr + ")," + encGen + ",.)";
    std::vector<std::string> maskArgs = translate::injectTsContextForExpr(
        {"eval", "--data", encOut, "--gen", maskExpr, "--drop", encGen, "--out", finalOut},
        maskExpr, session);
    CobraRunResult cr = runCobra(maskArgs);
    if (!cr.ok) {
      CobraError ce = errorFromRaw(cr);
      return engineErrorBlock(cobraErrorStr(ce), ce.code);
    }
    session.load(finalOut);
  } else {
    session.load(encOut);
  }

  // The value-label set encode fills and attaches. Its name is label() when
  // given, else the NEW variable's (w2ah_data_verbs.log ah_encode_gen: `: value
  // label e_es' is e_es; main named it after the source, es). When the set
  // already exists its codes are reused and each new string is appended at the
  // set's largest code + 1, in sorted order (ah_encode_label_exist: ordl = {1
  // pear, 5 kiwi} gives pear 1, Pear 6, apple 7, fig 8; ah_encode_label_again
  // reuses intervention's codes). The engine numbered the sorted levels 1..K; a
  // set that does not agree with that numbering is applied by one in-place
  // engine recode of the codes.
  std::string lbl = (lblname.has_value() && !lblname->empty()) ? *lblname : newvar;
  std::map<std::string, std::string> codeMap;  // string-keyed (labelDefs)
  auto existingSet = session.labelDefs.find(lbl);
  if (existingSet != session.labelDefs.end()) codeMap = existingSet->second;
  std::map<std::string, long long> codeOfText;
  long long maxCode = 0;
  for (const auto& [codeStr, text] : codeMap) {
    long long c = 0;
    try {
      c = std::stoll(codeStr);
    } catch (const std::exception&) {
      continue;
    }
    maxCode = std::max(maxCode, c);
    auto ins = codeOfText.emplace(text, c);
    if (!ins.second && c < ins.first->second) ins.first->second = c;
  }
  std::string remapRules;
  for (size_t i = 0; i < sortedLevels.size(); ++i) {
    const long long engineCode = static_cast<long long>(i) + 1;
    long long code = 0;
    auto known = codeOfText.find(sortedLevels[i]);
    if (known != codeOfText.end()) {
      code = known->second;
    } else if (noExtend) {
      // Only rows outside the sample hold this string (checked above), and they
      // are missing: the set is not extended (StataBE 19.5: `label list` after
      // `encode s if x < 4, ... noextend` still has only the set's own codes).
      continue;
    } else {
      code = ++maxCode;
      codeMap[std::to_string(code)] = sortedLevels[i];
    }
    if (code != engineCode)
      remapRules += "(" + std::to_string(engineCode) + "=" + std::to_string(code) + ")";
  }
  if (!remapRules.empty()) {
    std::string remapOut = nextPath(session.workdir(), "encode_codes");
    std::vector<std::string> args = {"recode", "--data", *session.currentHbin(), "--var", newvar,
                                     "--rules", remapRules, "--out", remapOut};
    std::optional<SessionState::ResidentEligible> remapResident;
    if (residentEncode) remapResident.emplace(session);
    CobraRunResult cr = runCobra(args);
    if (!cr.ok) {
      CobraError ce = errorFromRaw(cr);
      return engineErrorBlock(cobraErrorStr(ce), ce.code);
    }
    session.load(remapOut);
  }
  if (!codeMap.empty() || existingSet != session.labelDefs.end()) session.labelDefs[lbl] = codeMap;
  session.varLblname[newvar] = lbl;
  // Every variable carrying this set sees the appended codes.
  for (const auto& [var, lname] : session.varLblname) {
    if (lname != lbl) continue;
    std::map<int, std::string> numericMap;  // int-keyed (valueLabels)
    for (const auto& [codeStr, text] : codeMap) {
      try {
        numericMap[std::stoi(codeStr)] = text;
      } catch (const std::exception&) {
      }
    }
    if (numericMap.empty()) session.valueLabels.erase(var);
    else session.valueLabels[var] = numericMap;
  }
  // encode keeps the source's variable label (ah_encode_gen: [Fruit name]).
  {
    auto srcMeta = session.varMeta.find(strvar);
    if (srcMeta != session.varMeta.end() && srcMeta->second.label.has_value() &&
        !srcMeta->second.label->empty())
      session.varMeta[newvar].label = srcMeta->second.label;
  }
  // format-display-default-lifecycle: Stata's encode creates a long that
  // displays %8.0g, not long's own %12.0g (w2af_format_lifecycle.log
  // af_encode: `long|%8.0g`, and the same pair after a save/use,
  // af_rt_created). The format is the variable's own, so it is held.
  {
    VarMeta& vm = session.varMeta[newvar];
    vm.stataType = std::string("long");
    vm.format = std::string("%8.0g");
    session.implicitFloatVars.erase(newvar);
  }

  return mutationResult(session, "encode", {newvar});
}

namespace {
// BUG-664/665: the informational line Stata's destring and tostring print
// when a variable is skipped ("... no replace"). A display block, rc 0 -- the
// command did not fail, it declined one variable and moved on.
ojson destringNoteBlock(const std::string& text) {
  ojson out;
  out["type"] = "display";
  out["text"] = text;
  return out;
}

// BUG-928 (FU-750): generate() places the new variable right after its own
// source, as Stata does (w2ab_strconv_order oracle) -- so `tostring a b, gen(a_s
// b_s)` on a b c leaves a a_s b b_s c (w2ab_strconv_order so_t3_gen_two).
// setcol --new appends the column last; one engine `order --after` (the
// append generate() pattern, BUG-551) moves it, skipped when the source was
// already the last column. `mutated` is the setcol's own mutationResult; its
// varlist_after says where the new column landed, and on a move it is
// replaced by the post-order card (same action and new_vars, final order).
// The order step rides the plane the setcol used: resident when the caller
// held ResidentEligible for setcol (the card fold takes order's S1b permutation
// plan), else the stateless one-shot over the real file setcol wrote. Returns
// an error block on failure, nullopt on success.
std::optional<ojson> placeGeneratedAfterSource(SessionState& session, const std::string& verb,
                                               const std::string& source,
                                               const std::string& target, bool resident,
                                               ojson& mutated) {
  if (mutated.is_object() && mutated.contains("varlist_after") &&
      mutated["varlist_after"].is_array()) {
    const ojson& vl = mutated["varlist_after"];
    for (size_t i = 0; i + 1 < vl.size(); ++i) {
      if (vl[i].value("name", std::string()) == source &&
          vl[i + 1].value("name", std::string()) == target)
        return std::nullopt;  // already right after its source
    }
  }
  std::string ordOut = nextPath(session.workdir(), verb + "_gen_order");
  std::vector<std::string> args = {"order", "--data", *session.currentHbin(), "--vars",
                                   target,  "--after", source, "--out", ordOut};
  {
    std::optional<SessionState::ResidentEligible> orderResident;
    if (resident) orderResident.emplace(session);
    CobraRunResult ocr = runCobra(args);
    if (!ocr.ok) {
      CobraError ce = errorFromRaw(ocr);
      return engineErrorBlock(cobraErrorStr(ce), ce.code);
    }
  }
  if (!resident && !cecon::native_exists(ordOut))
    return engineErrorBlock(verb + ": failed writing output dataset (disk full or I/O error)");
  session.load(ordOut);
  mutated = mutationResult(session, verb, {target});
  return std::nullopt;
}
}  // namespace

// ---------------------------------------------------------------------------
// destring VAR, {replace | gen(NEWVAR)} [ignore("chars")] [force] [float]
// [percent] [dpcomma] -- see handlers.h. Behaviour per [D] destring (and the
// StataBE 19.5 measurements).

namespace {

// Reads a number the way Stata's real() does: an optional sign, digits with an
// optional decimal point, and an optional exponent introduced by e/E/d/D, or the
// %21x hexadecimal form. Overflow, and any magnitude not below 2^1023 (beyond
// Stata's largest double), is unreadable; underflow reads as 0.
std::optional<double> destringReadNumber(const std::string& text) {
  if (text.empty()) return std::nullopt;
  size_t pos = 0;
  bool negative = false;
  if (text[pos] == '+' || text[pos] == '-') {
    negative = text[pos] == '-';
    ++pos;
  }
  auto isDec = [](char c) { return c >= '0' && c <= '9'; };
  auto hexVal = [](char c) -> int {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
  };
  constexpr double kLimit = 8.98846567431158e307;  // 2^1023
  auto finish = [&](double magnitude) -> std::optional<double> {
    if (!std::isfinite(magnitude) || magnitude >= kLimit) return std::nullopt;
    if (magnitude == 0.0) return 0.0;  // -0 reads as 0
    return negative ? -magnitude : magnitude;
  };

  // Hexadecimal form, recognised by its x/X marker: `1.` (or `0.` with all-zero
  // digits for zero, as %21x prints it), one to thirteen hexadecimal digits, the
  // marker, a required sign and one to three hexadecimal exponent digits; the
  // exponent runs from -3ff (which is zero) to +3fe. StataBE,
  // oracle/cr_destring_probe3 and cr_destring_probe4.
  const size_t xAt = text.find_first_of("xX", pos);
  if (xAt != std::string::npos) {
    if (xAt < pos + 3 || xAt > pos + 15 || text[pos + 1] != '.') return std::nullopt;
    const char lead = text[pos];
    if (lead != '0' && lead != '1') return std::nullopt;
    double fraction = 0.0, scale = 1.0;
    for (size_t i = pos + 2; i < xAt; ++i) {
      const int h = hexVal(text[i]);
      if (h < 0) return std::nullopt;
      scale /= 16.0;
      fraction += h * scale;
    }
    size_t e = xAt + 1;
    if (e >= text.size() || (text[e] != '+' && text[e] != '-')) return std::nullopt;
    const bool expNeg = text[e] == '-';
    ++e;
    if (e >= text.size() || text.size() - e > 3) return std::nullopt;
    int expo = 0;
    for (; e < text.size(); ++e) {
      const int h = hexVal(text[e]);
      if (h < 0) return std::nullopt;
      expo = expo * 16 + h;
    }
    if (expNeg) expo = -expo;
    if (expo > 0x3fe || expo < -0x3ff) return std::nullopt;
    if (lead == '0') {
      if (fraction != 0.0 || expo != -0x3ff) return std::nullopt;
      return 0.0;
    }
    if (expo == -0x3ff) return 0.0;
    return finish(std::ldexp(1.0 + fraction, expo));
  }

  // Decimal form; validated here, converted by strtod in the C locale.
  std::string cleaned;
  cleaned.reserve(text.size() - pos);
  bool mantDigit = false, sawPoint = false;
  size_t i = pos;
  for (; i < text.size(); ++i) {
    const char c = text[i];
    if (isDec(c)) {
      mantDigit = true;
    } else if (c == '.' && !sawPoint) {
      sawPoint = true;
    } else {
      break;
    }
    cleaned.push_back(c);
  }
  if (!mantDigit) return std::nullopt;
  if (i < text.size()) {
    const char c = text[i];
    if (c != 'e' && c != 'E' && c != 'd' && c != 'D') return std::nullopt;
    cleaned.push_back('e');
    ++i;
    if (i < text.size() && (text[i] == '+' || text[i] == '-')) cleaned.push_back(text[i++]);
    bool expDigit = false;
    for (; i < text.size(); ++i) {
      if (!isDec(text[i])) return std::nullopt;
      expDigit = true;
      cleaned.push_back(text[i]);
    }
    if (!expDigit) return std::nullopt;
  }
  char* end = nullptr;
  const double magnitude = std::strtod(cleaned.c_str(), &end);
  if (end == nullptr || *end != '\0') return std::nullopt;
  return finish(magnitude);  // overflow gives inf (refused); underflow gives 0
}

// `.`, `.a`-`.z` (and the empty string) are missing values, never unreadable.
bool destringIsMissingToken(const std::string& t) {
  if (t.empty() || t == ".") return true;
  return t.size() == 2 && t[0] == '.' && t[1] >= 'a' && t[1] <= 'z';
}

// dpcomma: a period anywhere makes the value unreadable; the first comma is
// the decimal point, unless the value is a lone comma or some comma is
// directly followed by a lowercase letter (then nothing is converted and the
// value stays unreadable).
std::optional<std::string> destringApplyDpcomma(const std::string& t) {
  if (t.find('.') != std::string::npos) return std::nullopt;
  if (t == ",") return t;
  for (size_t i = 0; i + 1 < t.size(); ++i) {
    if (t[i] == ',' && t[i + 1] >= 'a' && t[i + 1] <= 'z') return t;
  }
  std::string out = t;
  const size_t comma = out.find(',');
  if (comma != std::string::npos) out[comma] = '.';
  return out;
}

enum class DestringRead { Number, Missing, Unreadable };

// One observation's text -> number / missing / unreadable, after ignore(),
// trimming, the missing tokens and dpcomma ([D] destring, Options and
// Description). `sawPercent` is set when a `%` was removed.
DestringRead destringReadCell(const std::string& raw, const std::array<bool, 256>& ignoreSet,
                              bool anyIgnore, bool dpcomma, double& value, bool& sawPercent) {
  std::string t;
  if (anyIgnore) {
    t.reserve(raw.size());
    for (char c : raw) {
      const unsigned char u = static_cast<unsigned char>(c);
      if (ignoreSet[u]) {
        if (c == '%') sawPercent = true;
        continue;
      }
      t.push_back(c);
    }
  } else {
    t = raw;
  }
  size_t a = 0, b = t.size();
  while (a < b && (t[a] == ' ' || t[a] == '\t')) ++a;
  while (b > a && (t[b - 1] == ' ' || t[b - 1] == '\t')) --b;
  t = t.substr(a, b - a);
  if (destringIsMissingToken(t)) return DestringRead::Missing;
  if (dpcomma) {
    std::optional<std::string> conv = destringApplyDpcomma(t);
    if (!conv.has_value()) return DestringRead::Unreadable;
    t = std::move(*conv);
  }
  std::optional<double> v = destringReadNumber(t);
  if (!v.has_value()) return DestringRead::Unreadable;
  value = *v;
  return DestringRead::Number;
}

// float(): the nearest single; nullopt from 2^127 up, where Stata's float range
// ends (StataBE: under `float`, 1.7e38 reads, while 1.8e38 and 2e38% are
// unreadable, or missing under `force`).
std::optional<double> toSingle(double v) {
  if (std::fabs(v) >= 0x1p127) return std::nullopt;
  const double f = static_cast<double>(static_cast<float>(v));
  if (std::fabs(f) >= 0x1p127) return std::nullopt;
  return f;
}

// [D] destring creates double (float under `float`) and then compresses;
// [D] compress demotes to the smallest integer type that holds every value
// (ranges per [D] Data types), never double->float and never float->long.
std::string destringStorageType(const std::vector<double>& vals, bool asFloat) {
  const std::string created = asFloat ? "float" : "double";
  if (vals.empty()) return created;  // an empty variable is not compressed
  bool any = false, integral = true;
  double lo = 0, hi = 0;
  for (double v : vals) {
    if (std::isnan(v)) continue;
    if (v != std::trunc(v)) integral = false;
    if (!any) {
      lo = hi = v;
      any = true;
    } else {
      lo = std::min(lo, v);
      hi = std::max(hi, v);
    }
  }
  if (!any) return "byte";
  if (!integral) return created;
  if (lo >= -127 && hi <= 100) return "byte";
  if (lo >= -32767 && hi <= 32740) return "int";
  if (asFloat) return "float";
  if (lo >= -2147483647.0 && hi <= 2147483620.0) return "long";
  return "double";
}

}  // namespace

ojson runDestring(const std::string& varname, bool doReplace,
                  const std::optional<std::string>& genVar,
                  const std::optional<std::string>& ignoreChars, bool force,
                  SessionState& session, const DestringOpts& opts) {
  if (!session.hasData()) return eval::noDataLoaded();

  // Names and types come from the resident card when one is current (it is
  // authoritative for absence as well); the datastore is opened only when
  // there is no card, or when the row strings are not cached.
  Datastore* ds = nullptr;
  auto openDs = [&]() -> Datastore* {
    if (ds == nullptr) ds = session.datastore();
    return ds;
  };
  const std::vector<VarInfo>* card = session.residentReadCard();
  auto dtypeOf = [&](const std::string& name) -> std::optional<std::string> {
    if (card != nullptr) {
      for (const auto& vi : *card)
        if (vi.name == name) return vi.dtype;
      return std::nullopt;
    }
    Datastore* d = openDs();
    if (d == nullptr || !d->has(name)) return std::nullopt;
    return d->dtype(name);
  };

  const std::optional<std::string> srcType = dtypeOf(varname);
  if (!srcType.has_value())
    return engineErrorBlock("destring: variable '" + varname + "' not found");
  if (doReplace == genVar.has_value())
    return engineErrorBlock("destring: specify exactly one of `replace` or `generate(newvar)`",
                            "syntax_error");

  const std::string target = doReplace ? varname : trim(*genVar);
  if (!doReplace) {
    const bool reserved = strutil::isReservedVarName(target);
    if (reserved || target == "_skip" || target == "_all")
      return rcErrorBlock("generate() contains existing variable(s) and/or illegal variable name(s)",
                          reserved ? 198 : 101);
    if (dtypeOf(target).has_value())
      return engineErrorBlock("destring: variable '" + target + "' already defined");
    if (!strutil::isValidStataIdentifier(target))
      return engineErrorBlock("destring: invalid variable name `" + target + "' — " +
                                  strutil::stataIdentifierRuleText(),
                              "syntax_error");
  }
  const std::string op = doReplace ? "replace" : "generate";
  if (*srcType == "f64") return destringNoteBlock(varname + " already numeric; no " + op);

  // The effective ignore set: ignore()'s bytes, plus `%` under percent.
  std::array<bool, 256> ignoreSet{};
  bool anyIgnore = false;
  if (ignoreChars.has_value()) {
    for (char c : *ignoreChars) {
      ignoreSet[static_cast<unsigned char>(c)] = true;
      anyIgnore = true;
    }
  }
  if (opts.percent) {
    ignoreSet[static_cast<unsigned char>('%')] = true;
    anyIgnore = true;
  }

  // The source's row strings: the cached committed column when it is current,
  // else the datastore's codes and level table.
  int64_t n = 0;
  const std::vector<std::string>* cachedStrs = nullptr;
  const int32_t* codes = nullptr;
  const std::vector<std::string>* levels = nullptr;
  if (const auto* cc = session.residentComputedCol(varname); cc != nullptr && cc->dtype == "factor") {
    n = cc->nRows;
    cachedStrs = &cc->strs;
  } else {
    Datastore* d = openDs();
    if (d == nullptr) return eval::noDataLoaded();
    n = d->nRows();
    const cecon::Column& col = d->col(varname);
    codes = col.codes();
    levels = &col.levels;
  }
  static const std::string kEmpty;
  auto rowText = [&](int64_t r) -> const std::string& {
    if (cachedStrs != nullptr) return (*cachedStrs)[static_cast<size_t>(r)];
    const int32_t code = codes[r];
    if (code < 0 || static_cast<size_t>(code) >= levels->size()) return kEmpty;
    return (*levels)[static_cast<size_t>(code)];
  };

  const double kMissing = std::numeric_limits<double>::quiet_NaN();
  std::vector<double> values(static_cast<size_t>(n), kMissing);
  bool sawPercent = false;
  for (int64_t r = 0; r < n; ++r) {
    double v = 0;
    DestringRead got =
        destringReadCell(rowText(r), ignoreSet, anyIgnore, opts.dpcomma, v, sawPercent);
    if (got == DestringRead::Number && opts.asFloat) {
      const std::optional<double> single = toSingle(v);
      if (single.has_value()) v = *single;
      else got = DestringRead::Unreadable;
    }
    if (got == DestringRead::Number) {
      values[static_cast<size_t>(r)] = v;
    } else if (got == DestringRead::Unreadable && !force) {
      return destringNoteBlock(varname +
                               (anyIgnore ? ": contains characters not specified in ignore(); no "
                                          : ": contains nonnumeric characters; no ") +
                               op);
    }
  }
  if (opts.percent && sawPercent) {
    for (double& v : values) {
      if (std::isnan(v)) continue;
      v = v / 100.0;
      if (opts.asFloat) v = toSingle(v).value_or(kMissing);
    }
  }
  const std::string storage = destringStorageType(values, opts.asFloat);

  // The engine's single-column write: one line per row, shortest round-trip
  // text, an empty line for missing.
  std::string payloadPath = nextPath(session.workdir(), "destring") + ".csv";
  {
    std::ofstream payload(cecon::to_native_path(payloadPath), std::ios::binary);
    if (!payload) return engineErrorBlock("destring: could not open scratch CSV for write");
    for (double v : values) {
      if (!std::isnan(v)) payload << eval::pythonFloatRepr(v);
      payload << '\n';
    }
    payload.flush();
    if (!payload) {
      cecon::native_unlink(payloadPath);
      return engineErrorBlock("destring: failed writing scratch CSV (disk full or I/O error)");
    }
  }
  {
    std::error_code ec;
    const auto bytes = std::filesystem::file_size(cecon::to_native_path(payloadPath), ec);
    counters::addIpcBytes(ec ? 0 : static_cast<int64_t>(bytes), /*setcolShaped=*/true);
  }
  const bool resident =
      std::getenv("PIET_NO_RESIDENT_SETCOL") == nullptr && !residentReadDisabled();
  std::string dataPath;
  if (resident) {
    dataPath = *session.currentHbin();
  } else {
    Datastore* d = openDs();
    if (d == nullptr) {
      cecon::native_unlink(payloadPath);
      return eval::noDataLoaded();
    }
    dataPath = d->path();
  }
  const std::string outPath = nextPath(session.workdir(), "destring");
  std::vector<std::string> args = {"setcol",   "--data",    dataPath, "--var",  target,
                                   "--values", payloadPath, "--dtype", "f64",   "--out",
                                   outPath};
  if (!doReplace) args.push_back("--new");
  CobraRunResult cr;
  {
    std::optional<SessionState::ResidentEligible> eligible;
    if (resident) eligible.emplace(session);
    cr = runCobra(args);
  }
  cecon::native_unlink(payloadPath);
  if (!cr.ok) {
    CobraError ce = errorFromRaw(cr);
    return engineErrorBlock(cobraErrorStr(ce), ce.code);
  }
  if (!resident && !cecon::native_exists(outPath))
    return engineErrorBlock("destring: failed writing output dataset (disk full or I/O error)");
  session.load(outPath);

  {
    // The recorded type is the compressed one; the display format stays the
    // created type's default (double %10.0g, float %9.0g).
    std::optional<std::string> sourceLabel;
    if (!doReplace) {
      auto it = session.varMeta.find(varname);
      if (it != session.varMeta.end() && it->second.label.has_value() &&
          !it->second.label->empty())
        sourceLabel = it->second.label;
    }
    VarMeta& vm = session.varMeta[target];
    vm.stataType = storage;
    vm.format = stataDefaultFormat(std::string(opts.asFloat ? "float" : "double"));
    if (!doReplace) {
      if (sourceLabel.has_value()) vm.label = sourceLabel;
      else vm.label.reset();
    }
  }
  session.implicitFloatVars.erase(target);

  ojson result = doReplace ? mutationResult(session, "destring", /*newVars=*/{},
                                            /*droppedVars=*/{}, /*rewrittenVars=*/{target})
                           : mutationResult(session, "destring", {target});
  if (!doReplace) {
    if (auto err = placeGeneratedAfterSource(session, "destring", varname, target, resident, result))
      return *err;
  }
  if (resident && session.residentHeadDirty()) {
    SessionState::ResidentComputedCol memo;
    memo.dtype = "f64";
    memo.nRows = n;
    memo.f64 = std::move(values);
    session.residentComputedColPut(target, std::move(memo));
  }
  return result;
}

// ---------------------------------------------------------------------------
// tostring VAR, {gen(NEWVAR) | replace} [usedisplayformat] — BS-003/D4 (BETA
// B-data). PIET-NATIVE numeric->string(factor) conversion, the mirror of
// runDestring above.

namespace {
// Default (non-usedisplayformat) numeric->string rendering — mirrors
// strmutate.cpp's file-local coerceStr() convention (integer -> plain
// digits via eval::pythonIntStrFromDouble, non-integer -> eval::gFormat);
// kept as its own copy per this codebase's established per-TU convention
// (see errorFromRaw's own header note above) rather than exporting
// strmutate.cpp's file-local helper. Missing is handled by the caller
// (tostringMissingRender) before this is reached.
std::string tostringDefaultRender(double v) {
  if (eval::pyIsInteger(v) && std::fabs(v) < 1e15) {
    return eval::pythonIntStrFromDouble(v);
  }
  return eval::gFormat(v);
}

// BUG-207: what `tostring` writes for a MISSING numeric.
//
// This used to be `""` on both branches, with an in-tree comment asserting that
// was Stata-faithful. It is not, and the assertion is exactly how the defect
// survived: `fmt_g_oracle.log`'s section C only ever spoke to the
// `usedisplayformat` branch, and one branch's evidence was generalised to the
// other. Measured on BOTH branches, StataNow 19.5, marker-judged
// (`internal capture tostring_missing_oracle.log`, whole-line marker
// `TOSTRING-MISSING-CAPTURE-COMPLETE`), with `strlen` reported per cell so `""`
// and `"."` cannot be confused by eye:
//
//     branch                      42        .           .a
//     plain tostring              [42] 2    [.] 1       [.a] 2
//     , usedisplayformat          [42] 2    [.] 1       [.a] 2
//     , usedisplayformat %12.4g   [42] 2    [.] 1       [.a] 2
//
// So Stata writes the missing TOKEN, and it writes the extended CODE — a
// `tostring`-ed column keeps the 27-way distinction the numeric column had. The
// field width is NOT kept (`%12.4g` still yields `42`, not a padded field), which
// is why the width row is in the capture at all.
//
// The kind is available here: PIET carries the extended missings as NaN payload
// bits (`data/missing.h`, already included by this TU), and this engine really
// does preserve them through `replace v = .a` — live-probed this stint,
// `count if v == .a` -> 1 and `count if v == .` -> 1 on the same three-row
// column. So rendering only system missing as `.` and collapsing `.a`-`.z` onto
// it would be trading one wrong byte for a different wrong byte at a site being
// edited anyway, with the oracle for the right answer sitting in the same log.
// SCOPE NOTE: BUG-207 named the extended half as a separate claim. It is
// implemented here because it is three lines off a store this file already reads
// and an oracle row already captured — not because the entry authorised it.
//
// `missing_kind` returns -1 for anything finite or infinite, so a caller that
// reaches this for a non-missing gets the honest empty string rather than a
// bogus `.`; the one call site guards with isnan() first regardless.
std::string tostringMissingRender(double v) {
  const int kind = cecon::missing_kind(v);
  if (kind < cecon::kMissSys) return std::string();  // not missing at all
  if (kind == cecon::kMissSys) return ".";           // system missing -> `.`
  return std::string(".") + static_cast<char>('a' + (kind - cecon::kMissA));
}
}  // namespace

// BUG-371: `decode NUMVAR, generate(NEWVAR)` — the inverse of `encode`, and the
// asymmetry was the finding: a user who encoded a string could not get it back.
//
// What `encode` actually leaves behind (runEncode above) is an f64 column of
// 1-based codes PLUS a session-side value-label map — not a factor column. So
// decode is not a dtype relabel: it reads session.valueLabels[var], maps each
// row's code to its label, and writes a NEW factor column. A code with no label
// becomes missing, which is what Stata does.
ojson runDecode(const std::string& varname, const std::optional<std::string>& genVar,
                SessionState& session, size_t maxLength) {
  if (!session.hasData()) return eval::noDataLoaded();
  if (!genVar.has_value() || genVar->empty())
    return engineErrorBlock("decode: generate(newvar) is required", "syntax_error");
  const std::string target = trim(*genVar);
  if (!strutil::isValidStataIdentifier(target))
    return engineErrorBlock(
        "decode: invalid variable name `" + target + "' — " + strutil::stataIdentifierRuleText(),
        "syntax_error");

  Datastore* ds = session.datastore();
  if (ds == nullptr) return eval::noDataLoaded();
  if (!ds->has(varname))
    return engineErrorBlock("variable " + varname + " not found", "unknown_variable");
  if (ds->has(target))
    return engineErrorBlock("decode: variable " + target + " already defined", "bad_input");
  // Stata's rc for both refusals (w2ah_data_verbs.log ah_decode_string: 108,
  // ah_decode_nolab: 182; main answered rc 1 to each).
  if (ds->dtype(varname) != "f64")
    return rcErrorBlock(varname + ": not possible with string variable", 108);

  // "Not labeled" is about the attachment: a variable carrying a value-label
  // name decodes (an unmapped code to ""), one carrying none is r(182).
  if (session.varLblname.find(varname) == session.varLblname.end())
    return rcErrorBlock(varname + " not labeled", 182);
  // An attached set that is not defined (dropped, or never defined) is
  // r(111) "value label X not found" (w2ao_label_family lb_val_undef, lb_drop
  // `dec=111`).
  {
    const std::string& lbl = session.varLblname.at(varname);
    if (session.labelDefs.find(lbl) == session.labelDefs.end())
      return rcErrorBlock("value label " + lbl + " not found", 111);
  }
  // BUG-1120: only then the generate() name, by encode's rule: "_cons invalid
  // varname", "int invalid varname", r(198) (`decode s, gen(_cons)' with s string
  // is still r(108), `decode id, gen(_rc)' with id unlabeled r(182); StataBE 19.5).
  if (auto r = strutil::newVarNameRefusal(target, /*resultRows=*/true))
    return rcErrorBlock(r->message, r->rc);
  static const std::map<int, std::string> kNoLabels;
  auto lblIt = session.valueLabels.find(varname);
  const std::map<int, std::string>& labels =
      lblIt == session.valueLabels.end() ? kNoLabels : lblIt->second;

  const int64_t n = ds->nRows();
  const double* src = ds->col(varname).f64();
  std::vector<std::string> out(static_cast<size_t>(n));
  for (int64_t r = 0; r < n; ++r) {
    const double v = src[r];
    if (std::isnan(v) || v != std::floor(v)) continue;  // stays "" == factor-missing
    auto it = labels.find(static_cast<int>(v));
    if (it == labels.end()) continue;  // an unlabeled code decodes to missing, matching Stata
    // BUG-1096: maxlength(#) counts bytes but never cuts inside a character; it
    // keeps the whole characters that fit (StataBE 19.5, labels "héllo" and
    // "日本語です": maxlength(4) gives "hél" and "日", maxlength(2) "h" and "").
    // A cut mid-sequence wrote invalid UTF-8 the snapshot writer then refused.
    std::string text = it->second;
    if (text.size() > maxLength) {
      size_t cut = maxLength;
      while (cut > 0 && (static_cast<unsigned char>(text[cut]) & 0xC0) == 0x80) --cut;
      text.resize(cut);
    }
    out[static_cast<size_t>(r)] = std::move(text);
  }

  // Same one-column setcol shape runTostring uses — see its comments for the
  // resident-plane reasoning and the BUG-124 flush guard.
  std::string valuesPath = nextPath(session.workdir(), "decode") + ".csv";
  {
    std::ofstream csv(cecon::to_native_path(valuesPath), std::ios::binary);
    if (!csv) return engineErrorBlock("decode: could not open scratch CSV for write");
    for (int64_t r = 0; r < n; ++r) csv << csvEscape(out[static_cast<size_t>(r)]) << "\n";
    csv.flush();
    if (!csv) {
      cecon::native_unlink(valuesPath);
      return engineErrorBlock("decode: failed writing scratch CSV (disk full or I/O error)");
    }
  }
  const bool residentSetcol = std::getenv("PIET_NO_RESIDENT_SETCOL") == nullptr &&
                              !residentReadDisabled();
  std::string outPath = nextPath(session.workdir(), "decode");
  std::string dataPath = residentSetcol ? *session.currentHbin() : ds->path();
  std::vector<std::string> setArgs = {"setcol",   "--data",   dataPath, "--var",   target,
                                      "--values", valuesPath, "--dtype", "factor", "--new",
                                      "--out",    outPath};
  std::optional<SessionState::ResidentEligible> setcolResident;
  if (residentSetcol) setcolResident.emplace(session);
  CobraRunResult cr = runCobra(setArgs);
  cecon::native_unlink(valuesPath);
  if (!cr.ok) {
    CobraError ce = errorFromRaw(cr);
    return engineErrorBlock(cobraErrorStr(ce), ce.code);
  }
  if (!residentSetcol && !cecon::native_exists(outPath))
    return engineErrorBlock("decode: failed writing output dataset (disk full or I/O error)");
  session.load(outPath);
  // decode's new variable is a str# as wide as the longest label of the attached
  // set in bytes, used or not, and at most maxlength() (at least 1), with that
  // type's default format, and it keeps the source's variable label
  // (w2ah_data_verbs.log ah_decode_gen: str17 %17s [Region code]; ah_decode_short:
  // str2 %9s). Main recorded no type, so `: type' failed and the format read %9s.
  // StataBE 19.5, set {"ab", "c", unused "a very long unused label"}: str24, and
  // str10 under maxlength(10); "héllo"/"日本語です" under maxlength(2): str2 though
  // the longest kept text is "h". Past 2045 bytes it is strL, as every str# is: a
  // 2048-byte label (used or not) makes strL, and so does maxlength(2046) over a
  // longer one, while maxlength(2045) makes str2045.
  size_t width = 1;
  for (const auto& [code, text] : labels) width = std::max(width, text.size());
  width = std::min(width, std::max<size_t>(maxLength, 1));
  {
    VarMeta& vm = session.varMeta[target];
    vm.stataType = width > 2045 ? std::string("strL") : "str" + std::to_string(width);
    auto srcMeta = session.varMeta.find(varname);
    if (srcMeta != session.varMeta.end() && srcMeta->second.label.has_value() &&
        !srcMeta->second.label->empty())
      vm.label = srcMeta->second.label;
  }
  session.implicitFloatVars.erase(target);
  return mutationResult(session, "decode", {target});
}

// BUG-371: `inspect VARNAME` — Stata's quick "what is in this column" readout:
// how many values are negative / zero / positive, how many are integers vs not,
// how many are missing, and how many distinct values there are. It is pure
// counting over one mmap'd column, so it is PIET-native for the same reason
// `isid` is (shelling out would cost a scratch .hbin to answer an arithmetic
// question). Emitted as a `display` text block: the frontend has no inspect
// renderer, and a text block renders TODAY rather than degrading to a JSON echo.
namespace {
// Stata's %9.0g rendering: 7 significant digits, trailing zeros trimmed, and the
// leading zero dropped for |v| < 1 (Stata writes .4780935, never 0.4780935).
// Checked against eight values in the live StataBE capture
// (internal capture conventions.log): .4780935, 1.674907, 5.263916,
// -.4077221, .424569, 3.912023, .29266, 4.78367 — all exact.
// BUG-1126: the shared %9.0g rule (stata_format.h general), not printf's %.7g,
// whose own small and large sides are not Stata's: xtsum printed 1e-06, 2e-05,
// 1.234568e+08 and 8.582929e-06 where StataBE prints 1.00e-06, .00002,
// 1.23e+08 and 8.58e-06 (and .0000115, -1.69e-21) in the same cells.
std::string stataG(double v) {
  if (std::isnan(v)) return ".";
  return piet::fmt::general(v, 9, 0);
}

}  // namespace

ojson runInspect(const std::vector<std::string>& varlist, SessionState& session) {
  if (!session.currentHbin().has_value()) return eval::noDataLoaded();
  Datastore* ds = session.datastore();
  if (ds == nullptr) return eval::noDataLoaded();
  if (varlist.size() != 1)
    return engineErrorBlock("inspect: one variable at a time in this build (`inspect varname`)",
                            "syntax_error");
  const std::string& name = varlist[0];
  if (!ds->has(name))
    return engineErrorBlock("variable " + name + " not found", "unknown_variable");
  if (ds->dtype(name) != "f64")
    return engineErrorBlock(
        "inspect: " + name + " is a string variable; inspect reports the numeric composition of "
        "a numeric variable.",
        "bad_input");

  const int64_t n = ds->nRows();
  const double* x = ds->col(name).f64();
  int64_t neg = 0, zero = 0, pos = 0, ints = 0, nonints = 0, miss = 0;
  int64_t negint = 0, posint = 0;
  std::set<double> uniq;
  bool uniqCapped = false;
  for (int64_t i = 0; i < n; ++i) {
    const double v = x[i];
    if (std::isnan(v)) { ++miss; continue; }
    if (v < 0) ++neg; else if (v == 0) ++zero; else ++pos;
    if (v == std::floor(v)) {
      ++ints;
      // BUG-373: Stata splits the integer count by SIGN too (r(N_negint) /
      // r(N_posint)), so track them here rather than deriving them later.
      if (v < 0) ++negint; else if (v > 0) ++posint;
    } else {
      ++nonints;
    }
    // Same 65536 cap `codebook` uses for its unique count — past that the number
    // stops being informative and starts being a memory bill.
    if (!uniqCapped) {
      uniq.insert(v);
      if (uniq.size() > 65536) uniqCapped = true;
    }
  }
  const int64_t total = n - miss;

  // BUG-377: this used to print a single count column with PERCENTAGES. Real
  // Stata prints three COUNT columns — Total / Integers / Nonintegers — with "-"
  // for a zero cell and no percentages anywhere. Captured live
  // (internal capture conventions.log, section Q3) on `inspect mpg`:
  //
  //   mpg:  Mileage (mpg)                             Number of observations
  //   -------------------                    ---------------------------------------
  //                                                Total      Integers   Nonintegers
  //   |      #                     Negative            -             -             -
  //   ...
  //     (21 unique values)
  //
  // The ASCII histogram in Stata's left panel is deliberately NOT reproduced: its
  // binning rule is not derivable from one capture, and a plausible-looking wrong
  // histogram is worse than an absent one. The numbers — which is what a reader
  // acts on — are exact.
  auto cell = [](int64_t k) { return k == 0 ? std::string("-") : std::to_string(k); };
  auto row3 = [&cell](const char* label, int64_t tot, int64_t ints_, int64_t non) {
    char buf[160];
    std::snprintf(buf, sizeof(buf), "  %-24s %11s   %11s   %11s", label, cell(tot).c_str(),
                  cell(ints_).c_str(), cell(non).c_str());
    return std::string(buf);
  };
  // Split each sign bucket by integer-ness so the three columns add up.
  const int64_t negNon = neg - negint, posNon = pos - posint;
  const int64_t zeroInt = zero, zeroNon = 0;  // 0 is an integer
  std::vector<std::string> lines;
  {
    char buf[200];
    std::snprintf(buf, sizeof(buf), "%s:%*sNumber of observations", name.c_str(),
                  static_cast<int>(name.size() < 34 ? 36 - name.size() : 2), "");
    lines.push_back(buf);
    lines.push_back("  ----------------------------------------------------------------------");
    std::snprintf(buf, sizeof(buf), "  %-24s %11s   %11s   %11s", "", "Total", "Integers",
                  "Nonintegers");
    lines.push_back(buf);
  }
  lines.push_back(row3("Negative", neg, negint, negNon));
  lines.push_back(row3("Zero", zero, zeroInt, zeroNon));
  lines.push_back(row3("Positive", pos, posint, posNon));
  lines.push_back("                           -----------   -----------   -----------");
  lines.push_back(row3("Total", total, ints, nonints));
  {
    char buf[160];
    std::snprintf(buf, sizeof(buf), "  %-24s %11s", "Missing", cell(miss).c_str());
    lines.push_back(std::string(buf));
    lines.push_back("                           -----------");
    std::snprintf(buf, sizeof(buf), "  %-24s %11lld", "", static_cast<long long>(n));
    lines.push_back(std::string(buf));
    std::snprintf(buf, sizeof(buf), "  (%s unique values)",
                  uniqCapped ? ">65536" : std::to_string(uniq.size()).c_str());
    lines.push_back(std::string(buf));
  }
  std::string text;
  for (size_t i = 0; i < lines.size(); ++i) {
    if (i) text += "\n";
    text += lines[i];
  }
  ojson out;
  out["type"] = "display";
  out["text"] = text;
  // BUG-373: the counts ride alongside the rendered text so the dispatcher can
  // publish them as r(). Stata's inspect is r-class and this one published
  // nothing — measured by the declared-returns sweep the day AFTER it shipped.
  ojson rcls = ojson::object();
  rcls["N"] = static_cast<double>(total);
  rcls["N_neg"] = static_cast<double>(neg);
  rcls["N_0"] = static_cast<double>(zero);
  rcls["N_pos"] = static_cast<double>(pos);
  rcls["N_negint"] = static_cast<double>(negint);
  rcls["N_posint"] = static_cast<double>(posint);
  // BUG-1453 (StataBE 19.5, scratch-laneA/lead/insp/i.do, i2.do): r(N_unique) is
  // the distinct count up to 99 and missing from 100 on (99 -> 99, 100 -> .);
  // r(N_undoc), only for a variable with a value label attached, counts the
  // nonmissing observations whose value the label does not name (a noninteger
  // included: 72 of 117), and is missing otherwise.
  const double nan = std::numeric_limits<double>::quiet_NaN();
  rcls["N_unique"] = (!uniqCapped && uniq.size() <= 99) ? static_cast<double>(uniq.size()) : nan;
  double undoc = nan;
  if (session.varLblname.count(name) != 0) {
    const auto lit = session.valueLabels.find(name);
    int64_t k = 0;
    for (int64_t i = 0; i < n; ++i) {
      const double v = x[i];
      if (std::isnan(v)) continue;
      const bool labelled = v == std::floor(v) && std::fabs(v) < 2147483648.0 &&
                            lit != session.valueLabels.end() &&
                            lit->second.count(static_cast<int>(v)) != 0;
      if (!labelled) ++k;
    }
    undoc = static_cast<double>(k);
  }
  rcls["N_undoc"] = undoc;
  out["inspect"] = rcls;
  return out;
}

// BUG-371: `xtsum` and `xttab` — the two panel summaries the idiom probe named.
//
// Both are PIET-native for the same reason `isid` and `inspect` are: they are
// arithmetic over mmap'd columns, and shelling out would cost a scratch .hbin to
// answer a question the data in hand already answers. Neither exists engine-side.
//
// The decompositions are the textbook ones, and they are what makes xtsum worth
// having at all — an overall SD tells a panel researcher nothing about whether
// the variation is across units or across time:
//   overall  x_it                      N = non-missing rows,   n/a
//   between  x̄_i                       n = units with any obs
//   within   x_it - x̄_i + x̄            same N as overall
// T-bar is N/n, the average number of observations per unit.
namespace {

struct PanelCols {
  const double* id = nullptr;
  const double* x = nullptr;
  int64_t n = 0;
};

// Sample SD (ddof=1), matching Stata's summarize. Fewer than two values -> missing.
double sampleSdDV(const std::vector<double>& v) {
  if (v.size() < 2) return std::numeric_limits<double>::quiet_NaN();
  double sum = 0;
  for (double q : v) sum += q;
  const double mean = sum / static_cast<double>(v.size());
  double ss = 0;
  for (double q : v) ss += (q - mean) * (q - mean);
  return std::sqrt(ss / static_cast<double>(v.size() - 1));
}

}  // namespace

ojson runXtsum(const std::vector<std::string>& varlist, SessionState& session) {
  if (!session.currentHbin().has_value()) return eval::noDataLoaded();
  Datastore* ds = session.datastore();
  if (ds == nullptr) return eval::noDataLoaded();
  // BUG-657: xtsum.ado calls `_xt, i()`: "must specify panelvar; use xtset" r(459)
  // (was unsupported_option = r(198)).
  if (!session.panelvar.has_value() || session.panelvar->empty())
    return ojson(eval::errorBlock("engine_error", "must specify panelvar; use xtset", std::nullopt,
                                  std::nullopt, std::nullopt, "r(459)"));
  const std::string& idvar = *session.panelvar;
  if (!ds->has(idvar))
    return engineErrorBlock("variable " + idvar + " not found", "unknown_variable");
  if (ds->dtype(idvar) != "f64")
    return engineErrorBlock("xtsum: panel variable " + idvar + " must be numeric", "bad_input");
  if (varlist.empty())
    return engineErrorBlock("xtsum: variable list required", "syntax_error");
  for (const auto& v : varlist) {
    if (!ds->has(v)) return engineErrorBlock("variable " + v + " not found", "unknown_variable");
    if (ds->dtype(v) != "f64")
      return engineErrorBlock("xtsum: " + v + " is a string variable", "bad_input");
  }

  const int64_t n = ds->nRows();
  const double* id = ds->col(idvar).f64();

  // BUG-377: Stata's real layout, captured live on nlswork
  // (internal capture conventions.log, section Q4):
  //
  //   Variable         |      Mean   Std. dev.       Min        Max |    Observations
  //   -----------------+--------------------------------------------+----------------
  //   ln_wage  overall |  1.674907   .4780935          0   5.263916 |     N =   28534
  //            between |              .424569          0   3.912023 |     n =    4711
  //            within  |               .29266  -.4077221    4.78367 | T-bar = 6.05689
  //
  // The `|` rules and the N=/n=/T-bar= labels in the Observations column are the
  // shape; the previous rendering used a plain Obs column and no rules.
  std::vector<std::string> lines;
  {
    char buf[200];
    std::snprintf(buf, sizeof(buf), "%-16s | %9s %10s %10s %10s | %15s", "Variable", "Mean",
                  "Std. dev.", "Min", "Max", "Observations");
    lines.push_back(buf);
    lines.push_back("-----------------+--------------------------------------------+----------------");
  }
  ojson stats = ojson::object();

  for (const auto& name : varlist) {
    const double* x = ds->col(name).f64();
    // Unit means, in first-appearance order; a unit with no non-missing value of
    // THIS variable contributes nothing (Stata's n counts units that have one).
    std::map<double, std::pair<double, int64_t>> acc;  // id -> (sum, count)
    std::vector<double> all;
    for (int64_t i = 0; i < n; ++i) {
      if (std::isnan(x[i]) || std::isnan(id[i])) continue;
      all.push_back(x[i]);
      auto& a = acc[id[i]];
      a.first += x[i];
      a.second += 1;
    }
    const int64_t N = static_cast<int64_t>(all.size());
    const int64_t units = static_cast<int64_t>(acc.size());
    const double NA = std::numeric_limits<double>::quiet_NaN();
    double gmean = NA, omin = NA, omax = NA;
    if (N > 0) {
      double sum = 0;
      omin = all[0];
      omax = all[0];
      for (double q : all) { sum += q; omin = std::min(omin, q); omax = std::max(omax, q); }
      gmean = sum / static_cast<double>(N);
    }
    std::vector<double> means;
    means.reserve(acc.size());
    for (const auto& kv : acc) means.push_back(kv.second.first / static_cast<double>(kv.second.second));
    double bmin = NA, bmax = NA;
    if (!means.empty()) {
      bmin = *std::min_element(means.begin(), means.end());
      bmax = *std::max_element(means.begin(), means.end());
    }
    // Within: x_it - x̄_i + x̄. Stata reports the min/max of THAT quantity, which
    // is why they can look odd next to the raw range — they are deviations
    // re-centred on the grand mean, not observations.
    std::vector<double> within;
    within.reserve(all.size());
    for (int64_t i = 0; i < n; ++i) {
      if (std::isnan(x[i]) || std::isnan(id[i])) continue;
      const auto& a = acc[id[i]];
      within.push_back(x[i] - a.first / static_cast<double>(a.second) + gmean);
    }
    double wmin = NA, wmax = NA;
    if (!within.empty()) {
      wmin = *std::min_element(within.begin(), within.end());
      wmax = *std::max_element(within.begin(), within.end());
    }
    const double osd = sampleSdDV(all), bsd = sampleSdDV(means), wsd = sampleSdDV(within);
    const double tbar = units > 0 ? static_cast<double>(N) / static_cast<double>(units) : NA;

    auto num = [](double v) { return stataG(v); };
    char buf[240];
    // Stata puts the variable name and the "overall" tag in one 16-wide stub.
    std::string stub = name + "  overall";
    // Widths count CHARACTERS (StataBE, scratch-r150/uni-v/p4.do), not bytes.
    stub = cecon::ident::truncateCodePoints(stub, 16);
    stub += std::string(16 - cecon::ident::codePointCount(stub), ' ');
    std::snprintf(buf, sizeof(buf), "%s | %9s %10s %10s %10s |   N = %9lld", stub.c_str(),
                  num(gmean).c_str(), num(osd).c_str(), num(omin).c_str(), num(omax).c_str(),
                  static_cast<long long>(N));
    lines.push_back(buf);
    std::snprintf(buf, sizeof(buf), "%-16s | %9s %10s %10s %10s |   n = %9lld", "         between",
                  "", num(bsd).c_str(), num(bmin).c_str(), num(bmax).c_str(),
                  static_cast<long long>(units));
    lines.push_back(buf);
    std::snprintf(buf, sizeof(buf), "%-16s | %9s %10s %10s %10s | T-bar = %7s", "         within ",
                  "", num(wsd).c_str(), num(wmin).c_str(), num(wmax).c_str(), num(tbar).c_str());
    lines.push_back(buf);

    ojson e;
    e["N"] = N;
    e["n_units"] = units;
    e["T_bar"] = tbar;
    e["mean"] = gmean;
    e["sd_overall"] = osd;
    e["min_overall"] = omin;
    e["max_overall"] = omax;
    e["sd_between"] = bsd;
    e["min_between"] = bmin;
    e["max_between"] = bmax;
    e["sd_within"] = wsd;
    e["min_within"] = wmin;
    e["max_within"] = wmax;
    stats[name] = e;
  }

  std::string text;
  for (size_t i = 0; i < lines.size(); ++i) { if (i) text += "\n"; text += lines[i]; }
  ojson out;
  out["type"] = "display";
  out["text"] = text;
  // The numbers ride along beside the rendered text so a consumer (and the
  // selftest lock) can assert on values rather than on formatting.
  out["xtsum"] = stats;
  return out;
}

ojson runXttab(const std::vector<std::string>& varlist, SessionState& session) {
  if (!session.currentHbin().has_value()) return eval::noDataLoaded();
  Datastore* ds = session.datastore();
  if (ds == nullptr) return eval::noDataLoaded();
  // BUG-657: xttab.ado calls `_xt, i()`: "must specify panelvar; use xtset" r(459)
  // (was unsupported_option = r(198)).
  if (!session.panelvar.has_value() || session.panelvar->empty())
    return ojson(eval::errorBlock("engine_error", "must specify panelvar; use xtset", std::nullopt,
                                  std::nullopt, std::nullopt, "r(459)"));
  if (varlist.size() != 1)
    return engineErrorBlock("xttab: one variable at a time (`xttab varname`)", "syntax_error");
  const std::string& name = varlist[0];
  const std::string& idvar = *session.panelvar;
  if (!ds->has(name)) return engineErrorBlock("variable " + name + " not found", "unknown_variable");
  if (!ds->has(idvar)) return engineErrorBlock("variable " + idvar + " not found", "unknown_variable");
  if (ds->dtype(name) != "f64")
    return engineErrorBlock("xttab: " + name + " is a string variable", "bad_input");

  const int64_t n = ds->nRows();
  const double* x = ds->col(name).f64();
  const double* id = ds->col(idvar).f64();

  std::map<double, int64_t> overall;                      // value -> rows
  std::map<double, std::set<double>> everUnits;           // value -> units taking it at least once
  std::map<double, std::map<double, int64_t>> perUnitHits;  // value -> unit -> rows with it
  std::map<double, int64_t> unitRows;                     // unit -> its non-missing rows
  std::set<double> allUnits;
  int64_t N = 0;
  for (int64_t i = 0; i < n; ++i) {
    if (std::isnan(x[i]) || std::isnan(id[i])) continue;
    ++N;
    ++overall[x[i]];
    everUnits[x[i]].insert(id[i]);
    ++perUnitHits[x[i]][id[i]];
    ++unitRows[id[i]];
    allUnits.insert(id[i]);
  }
  const int64_t nUnits = static_cast<int64_t>(allUnits.size());

  // BUG-377: Stata's real layout, captured live on nlswork (section Q4):
  //
  //                     Overall             Between            Within
  //        race |    Freq.  Percent      Freq.  Percent        Percent
  //   ----------+-----------------------------------------------------
  //       White |   20180     70.72      3329     70.66         100.00
  //   ----------+-----------------------------------------------------
  //       Total |   28534    100.00      4711    100.00         100.00
  //                                 (n = 4711)
  //
  // Note the BETWEEN column is headed "Freq." like the overall one — it is a
  // count of UNITS, and the previous rendering called it "Units", which reads as
  // a different quantity.
  std::vector<std::string> lines;
  {
    char buf[200];
    std::snprintf(buf, sizeof(buf), "%*s%-19s%-19s%s", 12, "", "Overall", "Between", "Within");
    lines.push_back(buf);
    std::snprintf(buf, sizeof(buf), "%10s | %8s %8s  %9s %8s %14s", name.c_str(), "Freq.",
                  "Percent", "Freq.", "Percent", "Percent");
    lines.push_back(buf);
    lines.push_back("-----------+-----------------------------------------------------");
  }
  ojson rows = ojson::array();
  for (const auto& kv : overall) {
    const double val = kv.first;
    const int64_t freq = kv.second;
    const int64_t units = static_cast<int64_t>(everUnits[val].size());
    // Within% is the AVERAGE over the units that ever take this value of the
    // share of that unit's observations holding it — not freq/N restricted, which
    // is a different (and wrong) number when units have unequal T.
    double withinPct = std::numeric_limits<double>::quiet_NaN();
    if (units > 0) {
      double acc = 0;
      for (const auto& uk : perUnitHits[val])
        acc += static_cast<double>(uk.second) / static_cast<double>(unitRows[uk.first]);
      withinPct = 100.0 * acc / static_cast<double>(units);
    }
    const double ovPct = N > 0 ? 100.0 * static_cast<double>(freq) / static_cast<double>(N) : 0.0;
    const double btPct =
        nUnits > 0 ? 100.0 * static_cast<double>(units) / static_cast<double>(nUnits) : 0.0;
    char buf[220];
    char valbuf[32];
    std::snprintf(valbuf, sizeof(valbuf), "%g", val);
    std::snprintf(buf, sizeof(buf), "%10s | %8lld %8.2f  %9lld %8.2f %14.2f", valbuf,
                  static_cast<long long>(freq), ovPct, static_cast<long long>(units), btPct,
                  withinPct);
    lines.push_back(buf);
    ojson r;
    r["value"] = val;
    r["freq"] = freq;
    r["percent"] = ovPct;
    r["units"] = units;
    r["units_percent"] = btPct;
    r["within_percent"] = withinPct;
    rows.push_back(r);
  }
  {
    char buf[220];
    lines.push_back("-----------+-----------------------------------------------------");
    std::snprintf(buf, sizeof(buf), "%10s | %8lld %8.2f  %9lld %8.2f", "Total",
                  static_cast<long long>(N), 100.0, static_cast<long long>(nUnits), 100.0);
    lines.push_back(buf);
    std::snprintf(buf, sizeof(buf), "%*s(n = %lld)", 30, "", static_cast<long long>(nUnits));
    lines.push_back(buf);
  }
  std::string text;
  for (size_t i = 0; i < lines.size(); ++i) { if (i) text += "\n"; text += lines[i]; }
  ojson out;
  out["type"] = "display";
  out["text"] = text;
  out["xttab"] = rows;
  out["n_obs"] = N;
  out["n_units"] = nUnits;
  return out;
}

ojson runTostring(const std::string& varname, bool doReplace,
                  const std::optional<std::string>& genVar, bool useDisplayFormat,
                  SessionState& session, const TostringOpts& opts, bool* forcedLoss) {
  if (!session.hasData()) return eval::noDataLoaded();
  // C4-L4: same lazy-datastore + card-first validation as runDestring above
  // (see the destring site for the full rationale — this is its mirror).
  Datastore* ds = nullptr;
  auto ensureDs = [&]() -> Datastore* {
    if (ds == nullptr) ds = session.datastore();
    return ds;
  };
  const std::vector<VarInfo>* card = session.residentReadCard();
  auto lookupDtype = [&](const std::string& name) -> std::optional<std::string> {
    if (card != nullptr) {
      for (const auto& vi : *card) {
        if (vi.name == name) return vi.dtype;
      }
      return std::nullopt;  // a current card is authoritative for absence too
    }
    Datastore* d = ensureDs();
    if (d == nullptr || !d->has(name)) return std::nullopt;
    return d->dtype(name);
  };
  std::optional<std::string> srcDtype = lookupDtype(varname);
  if (!srcDtype.has_value()) {
    return engineErrorBlock("tostring: variable '" + varname + "' not found");
  }
  if (doReplace == genVar.has_value()) {
    return engineErrorBlock("tostring: specify exactly one of `replace` or `gen(newvar)`");
  }
  std::string target = doReplace ? varname : *genVar;
  if (!doReplace) {
    // BUG-1120: as destring's generate(): a reserved name or _skip/_all is the
    // ado's "generate(newvarlist) invalid", r(198) and r(101), present or not
    // (StataBE 19.5).
    if (strutil::isReservedVarName(target) || target == "_skip" || target == "_all") {
      return rcErrorBlock("generate(newvarlist) invalid",
                          strutil::isReservedVarName(target) ? 198 : 101);
    }
    if (lookupDtype(target).has_value()) {
      return engineErrorBlock("tostring: variable '" + target + "' already defined");
    }
    if (!strutil::isValidStataIdentifier(target)) {
      return engineErrorBlock("tostring: invalid variable name `" + target + "' — " +
                                  strutil::stataIdentifierRuleText(),
                              "syntax_error");
    }
  }
  const std::string genrep = doReplace ? "replace" : "generate";
  if (*srcDtype != "f64") {
    // BUG-665: tostring.ado answers "VAR already string; no replace|generate"
    // and moves on (rc 0); the error stays on the legacy (non-gated) caller.
    if (opts.stataGate) return destringNoteBlock(varname + " already string; no " + genrep);
    return engineErrorBlock("tostring: '" + varname + "' is already a string variable");
  }
  if (opts.stataGate && doReplace && !opts.force) {
    auto lblIt = session.varLblname.find(varname);
    if (lblIt != session.varLblname.end() && !lblIt->second.empty())
      return destringNoteBlock(varname + " has value label; no replace");
  }
  const bool residentSetcol = std::getenv("PIET_NO_RESIDENT_SETCOL") == nullptr &&
                              !residentReadDisabled();

  // Source values: the computed-column cache first (the destring→tostring
  // chain shape), else the datastore (honest flush on a dirty head).
  const SessionState::ResidentComputedCol* srcCc = session.residentComputedCol(varname);
  if (srcCc != nullptr && srcCc->dtype != "f64") srcCc = nullptr;
  int64_t n = 0;
  const double* src = nullptr;
  if (srcCc != nullptr) {
    n = srcCc->nRows;
    src = srcCc->f64.data();
  } else {
    Datastore* d = ensureDs();
    if (d == nullptr) return eval::noDataLoaded();
    n = d->nRows();
    src = d->col(varname).f64();
  }

  // usedisplayformat (D6): render via the variable's OWN session display
  // format (BS-005's session.varMeta[...].format — "%9.0g" default when
  // unset, matching Datastore/VarInfo's own declared default). Reuses
  // piet::eval::applyStataFmt — note that helper only recognizes the
  // numeric %{f,g,e} families (falls back to its own generic-number
  // rendering for anything else, e.g. a date/time %fmt); a pre-existing
  // limitation of that shared helper, not newly introduced here.
  //
  // format-display-default-lifecycle: the format is the one resolver's
  // (SessionState::displayFormat): the session's own, else the one the card's
  // file states; with neither, the storage-type default below (the resolver's
  // third step, the same stataDefaultFormat table), which also covers a column
  // with no known type by rendering both readings.
  std::string fmt = "%9.0g";
  bool fmtRecorded = false;
  if (useDisplayFormat) {
    std::optional<VarInfo> vi;
    if (card != nullptr) {
      for (const auto& c : *card)
        if (c.name == varname) {
          vi = c;
          break;
        }
    } else if (Datastore* d = ensureDs()) {
      std::vector<VarInfo> fc = d->formatCard(std::vector<std::string>{varname});
      if (!fc.empty()) vi = fc.front();
    }
    auto it = session.varMeta.find(varname);
    const bool held = it != session.varMeta.end() && it->second.format.has_value() &&
                      !it->second.format->empty();
    if (held || (vi.has_value() && vi->formatStated)) {
      fmt = vi.has_value() ? session.displayFormat(*vi) : *it->second.format;
      fmtRecorded = true;
    }
  }
  // BUG-665: tostring.ado's format choice -- format() when given, the display
  // format under usedisplayformat (above), else `%12.0g` (the ado's
  // `local fmt "%12.0g"`). The legacy caller keeps its renderer.
  const bool adoFormat = opts.stataGate && !useDisplayFormat;
  if (adoFormat) fmt = opts.format.value_or("%12.0g");
  // The ado renders the STORED value: `string(v, fmt)` on a float variable
  // sees float(v). PIET holds numerics as f64, so the storage type decides
  // what is rendered. A declared VarMeta::stataType is trusted ("float" is
  // rounded, byte/int/long/double are not), and so is an undeclared
  // gen/egen's implicit float (implicitFloatVars, Stata's `generate`
  // default). merge/append record a .dta using file's types (BUG-666; append
  // rounds a float master's rows to float before widening it to double, as
  // Stata does, and drops the claim of an unclaimed master whose rows are not
  // float-exact). A
  // column with none of these (collapse outputs under a new name, columns from
  // a .csv/.hbin using file, import delimited, an undeclared gen after rename
  // or clonevar) has no known storage type: StataBE gives such shapes float,
  // double or long depending on the source.
  // For those both readings are rendered, and the command goes ahead only
  // when they agree (every value float-exact, or both skip with some row
  // irreversible under both, so that no mixed float/double truth converts
  // either). When they
  // disagree it is refused by name below rather than guessed, since either
  // guess can convert where Stata skips or skip where Stata converts.
  // A declared byte/int/long that now holds a fraction was promoted by a
  // later `replace` that left the claim unrecorded: StataBE makes byte/int
  // float and long double ("variable b was byte now float", "variable l was
  // long now double"), so that is what is read here. (BUG-471 T2's replace
  // records the widened type itself, storage::recordReplaceType.)
  enum class Storage { Float, NotFloat, Unknown };
  Storage storage = Storage::NotFloat;
  // The storage type the variable was created with (its declaration, or
  // gen's implicit float), which is what its default display format follows.
  std::optional<std::string> createdType;
  if (opts.stataGate) {
    auto it = session.varMeta.find(varname);
    if (it != session.varMeta.end() && it->second.stataType.has_value()) {
      const std::string& t = *it->second.stataType;
      createdType = t;
      storage = t == "float" ? Storage::Float : Storage::NotFloat;
      if (t == "byte" || t == "int") {
        for (int64_t r = 0; r < n; ++r) {
          if (!std::isnan(src[r]) && src[r] != std::trunc(src[r])) {
            storage = Storage::Float;
            break;
          }
        }
      }
    } else if (session.implicitFloatVars.count(varname) != 0) {
      storage = Storage::Float;
      createdType = std::string("float");
    } else {
      storage = Storage::Unknown;
    }
  }

  // usedisplayformat with no recorded display format: the default `generate`
  // gives the type the variable was CREATED with (stataDefaultFormat), so `set
  // type double` / `gen id = 12345678` / `tostring id, replace
  // usedisplayformat` renders "12345678" (%10.0g) and converts, where %9.0g
  // would render "1.23e+07" and skip. Not the current type: StataBE keeps the
  // creation format when a `replace` promotes the variable
  // (exprfns_contract_oracle.log:646/736 byte -> float keeps %8.0g, :655/737
  // int -> long keeps %8.0g), so a byte holding a fraction renders %8.0g and a
  // long holding one %12.0g. append and a widening replace
  // (storage::recordReplaceType) record the creation format themselves when
  // they change the claim. recast writes no format and cannot be
  // told apart here (an OPEN divergence: `recast double` of a float renders
  // %10.0g here, %9.0g in Stata, :735). A column with no known storage type
  // is rendered with each reading's own default (below). The legacy caller
  // keeps %9.0g.
  const bool typeDefaultFmt = opts.stataGate && useDisplayFormat && !fmtRecorded;
  if (typeDefaultFmt && storage != Storage::Unknown) fmt = stataDefaultFormat(createdType);

  struct RenderPass {
    std::vector<std::string> strings;
    bool irreversible = false;
    std::vector<char> rowBad;  // per row, filled only when asked for (trackRows)
    size_t maxLen = 1;
  };
  auto renderAll = [&](bool asFloat, const std::string& rfmt, bool trackRows) {
    RenderPass p;
    p.strings.resize(static_cast<size_t>(n));
    if (trackRows) p.rowBad.assign(static_cast<size_t>(n), 0);
    for (int64_t r = 0; r < n; ++r) {
      double v = src[r];
      if (std::isnan(v)) {
        // BUG-207: `.` / `.a`-`.z`, NOT "" — see tostringMissingRender's oracle.
        // Deliberately BEFORE the branch fork, so plain and `usedisplayformat`
        // cannot diverge here: the capture says both write the same token, and
        // one shared site is what makes that structural rather than a
        // coincidence.
        p.strings[static_cast<size_t>(r)] = tostringMissingRender(v);
        // BUG-918: the ado stores `string(v, fmt)`, so a left-justified
        // format() pads the missing token too (w2z_tostring tz_fmt_left:
        // `[.        ]`, str9). BUG-1159 (FU-753, FU-761): so does a
        // left-justified display format under usedisplayformat, which the ado
        // renders the same way (StataBE 19.5, measured 2026-11-03: `format z
        // %-9.0g` / `tostring z, replace usedisplayformat` stores ".        ",
        // str9; %-9.2f gives "3.00     " and ".        ").
        if (opts.stataGate) {
          p.strings[static_cast<size_t>(r)] =
              piet::fmt::padMissingLeftJustify(p.strings[static_cast<size_t>(r)], rfmt);
        }
        p.maxLen = std::max(p.maxLen, p.strings[static_cast<size_t>(r)].size());
        continue;
      }
      if (asFloat) v = static_cast<double>(static_cast<float>(v));
      std::string rendered;
      if (opts.stataGate) {
        // The ado stores `string(v, fmt)` as is: a left-justified format keeps
        // its trailing pad (WSR|292), which applyStataFmt already produces.
        rendered = eval::applyStataFmt(v, rfmt);
        // The ado's reversibility test: `count if v != real(temp)` (real()
        // ignores the pad).
        if (!p.irreversible || trackRows) {
          const std::string bare = trim(rendered);
          std::optional<double> back;
          if (bare.find('_') == std::string::npos) back = eval::pythonFloat(bare);
          if (!back.has_value() || *back != v) {
            p.irreversible = true;
            if (trackRows) p.rowBad[static_cast<size_t>(r)] = 1;
          }
        }
      } else {
        rendered = useDisplayFormat ? eval::applyStataFmt(v, rfmt) : tostringDefaultRender(v);
      }
      p.maxLen = std::max(p.maxLen, rendered.size());
      p.strings[static_cast<size_t>(r)] = std::move(rendered);
    }
    return p;
  };
  const bool unknown = storage == Storage::Unknown;
  RenderPass pass = renderAll(storage == Storage::Float,
                              unknown && typeDefaultFmt ? stataDefaultFormat("double") : fmt,
                              unknown && !opts.force);
  if (unknown) {
    RenderPass asFloat =
        renderAll(true, typeDefaultFmt ? stataDefaultFormat("float") : fmt, !opts.force);
    const bool convertsAsDouble = opts.force || !pass.irreversible;
    const bool convertsAsFloat = opts.force || !asFloat.irreversible;
    const bool agree = convertsAsDouble == convertsAsFloat &&
                       (!convertsAsDouble || pass.strings == asFloat.strings);
    // Both whole-column readings skip. That settles it only when some row is
    // irreversible under both, since then every truth skips. A column whose
    // rows are each reversible under one reading or the other may be held
    // mixed in Stata, the way an append that widens a float master to double
    // holds it (w2f_residuals.log:1856: master `gen x = 0.07*100` is 7, the
    // using double 0.1 stays 0.1, and tostring converts). A float master is
    // rounded at append time and claimed double, so it no longer reaches this
    // branch; only a column with no recorded type does (import delimited,
    // collapse outputs, clonevar, a renamed gen, or an unclaimed master
    // append widened). Its Stata truth is not known, and a skip here could be
    // a silent wrong answer, so it is refused by name instead (STEP 5). The
    // price: such a column whose Stata storage is uniform, and whose rows each
    // convert under exactly one reading, is refused where StataBE skips with
    // the note at rc 0.
    if (agree && !convertsAsDouble) {
      bool someRowSkipsBoth = false;
      for (int64_t r = 0; r < n && !someRowSkipsBoth; ++r) {
        const size_t i = static_cast<size_t>(r);
        if (pass.rowBad[i] && asFloat.rowBad[i]) someRowSkipsBoth = true;
      }
      if (!someRowSkipsBoth) {
        return engineErrorBlock(
            "tostring: " + varname +
                " has no recorded storage type; as float and as double it cannot be converted "
                "reversibly, but each of its values can under one of the two, so a column that "
                "holds some rows as float and others as double converts; declare it first "
                "(`recast double " + varname + "' or `recast float " + varname + "')",
            "syntax_error");
      }
    }
    if (!agree) {
      return engineErrorBlock(
          "tostring: " + varname +
              " has no recorded storage type, and as float and as double it converts "
              "differently; declare it first (`recast double " + varname + "' or `recast float " +
              varname + "')",
          "syntax_error");
    }
  }
  std::vector<std::string>& outStrings = pass.strings;
  const size_t maxLen = pass.maxLen;
  if (pass.irreversible && !opts.force) {
    // w2b_dataio.log tostring_nofmt_ctl / tostring_gen_fmt / _gen_nofmt_ctl:
    // "big cannot be converted reversibly; no replace", rc 0, big unchanged.
    return destringNoteBlock(varname + " cannot be converted reversibly; no " + genrep);
  }
  if (forcedLoss != nullptr) *forcedLoss = pass.irreversible;

  // Hand the engine's single-column `cobra setcol` verb just the rendered
  // string column instead of a whole-frame CSV + `cobra convert` round-trip
  // (which re-factorizes every untouched factor column). The rendering above
  // is unchanged; every other column/dictionary byte is preserved.
  //
  // BUG-207 changed what a missing row puts on this wire, so the transport note
  // here changed with it. It used to read: a numeric missing renders "" and
  // setcol maps an empty payload line to missing (code -1), so missing stays
  // missing. It now renders `.` / `.a`-`.z`, which setcol carries as an ORDINARY
  // FACTOR LEVEL — `Frame::read_csv_column` treats only an empty cell as missing
  // and says so in its own comment ("empty -> missing; '.' etc. stay literal
  // levels", src/data/frame.cpp), so no new transport support was needed.
  //
  // The resulting card change is the CORRECT one, not a side effect to absorb:
  // the product of `tostring` is a STRING variable, and a string variable's
  // missing is `""`, not `.`. So a Stata-faithful `tostring` of a column with
  // missings yields a string column with ZERO missing cells and a `.` level —
  // which is what n_missing/n_levels now report, and what Stata's own
  // `describe` reports for the same operation.
  bool targetPresent =
      lookupDtype(target).has_value();  // true only for `replace` (target==varname)
  std::string valuesPath = nextPath(session.workdir(), "tostring") + ".csv";
  {
    std::ofstream csv(cecon::to_native_path(valuesPath), std::ios::binary);
    if (!csv) return engineErrorBlock("tostring: could not open scratch CSV for write");
    for (int64_t r = 0; r < n; ++r) {
      csv << csvEscape(outStrings[static_cast<size_t>(r)]) << "\n";
    }
    // BUG-124 sibling: flush + check — silent short payload = silent row loss.
    csv.flush();
    if (!csv) {
      cecon::native_unlink(valuesPath);
      return engineErrorBlock("tostring: failed writing scratch CSV (disk full or I/O error)");
    }
  }
  // FLOOR (1) site class IPC, `semantic_zero` class (counters.h). setcol's own
  // semantic payload is metadata, so it leaves the ratio metric and is asserted
  // as an ABSOLUTE byte budget instead (A1 1.3). What that budget catches is
  // exactly this: the scratch CSV is a full rows-scale re-serialisation handed
  // to a subprocess for what is nominally a one-column edit. Only the bytes
  // THIS process wrote are billed here -- the `--out` hbin is read back through
  // Datastore, which bills it once at the COLUMNAR site (no double count).
  {
    std::error_code cec;
    auto csvSz = std::filesystem::file_size(cecon::to_native_path(valuesPath), cec);
    counters::addIpcBytes(cec ? 0 : static_cast<int64_t>(csvSz), /*setcolShaped=*/true);
  }
  std::string outPath = nextPath(session.workdir(), "tostring");
  // C2-SETCOL-RESIDENT (mirror of runDestring): route this setcol through the
  // resident frame plane so session.load(outPath) takes a VIRTUAL head instead
  // of reconstructing the whole Datastore. For an in-place f64->factor change the
  // engine reports card_stable=true with a vars_delta carrying the new
  // type/n_missing AND n_levels (de-risk brief §8.2); the vars_delta fold applies
  // it, SETTING nLevels (the opposite direction of destring — the two together
  // are the A/B parity gate's symmetry check). Stateless fallback + escapes
  // behave exactly as documented at the destring site (residentSetcol resolved
  // at entry, C2-specific + C4-family escapes both honored).
  std::string dataPath;
  if (residentSetcol) {
    dataPath = *session.currentHbin();  // frame-run channel strips --data
  } else {
    Datastore* d = ensureDs();  // pre-C4 verbatim: materialize + real file
    if (d == nullptr) return eval::noDataLoaded();
    dataPath = d->path();
  }
  std::vector<std::string> setArgs = {"setcol",   "--data",    dataPath, "--var",  target,
                                      "--values", valuesPath,  "--dtype", "factor", "--out",
                                      outPath};
  if (!targetPresent) setArgs.push_back("--new");
  std::optional<SessionState::ResidentEligible> setcolResident;
  if (residentSetcol) setcolResident.emplace(session);
  CobraRunResult cr = runCobra(setArgs);
  cecon::native_unlink(valuesPath);
  if (!cr.ok) {
    CobraError ce = errorFromRaw(cr);
    return engineErrorBlock(cobraErrorStr(ce), ce.code);
  }
  // Virtual (resident) head has no on-disk file; guard the torn-write stat to the
  // forced-stateless path only (see the destring site's note).
  if (!residentSetcol && !cecon::native_exists(outPath))
    return engineErrorBlock("tostring: failed writing output dataset (disk full or I/O error)");
  session.load(outPath);
  // C1-CARD-REBUILD-REDUCE A11 sibling: mirror of destring — `replace`
  // (targetPresent) rewrites `target` IN PLACE via single-column `cobra setcol`,
  // seed via rewrittenVars={target} (branch 2); `gen` appends `target`, pass it
  // in newVars (branch 1). Byte-identical vs PIET_NO_NMISS_CACHE=1.
  // BUG-665: the target is now a str# of the longest value (tostring_fmt_replace:
  // "big was double now str11"); a replaced column must not keep claiming its
  // numeric type or numeric display format.
  if (opts.stataGate) {
    // BUG-919: generate() labels the new variable the ado's way,
    // `_crcslbl NEW v` (_crcslbl.ado): the source's variable label, or the
    // source's NAME when it has none (w2z_tostring tz_gen_varlabel
    // `[Byte source]`). replace keeps the label on the replaced variable itself.
    std::optional<std::string> srcLabel;
    if (!doReplace) {
      srcLabel = varname;
      auto lit = session.varMeta.find(varname);
      if (lit != session.varMeta.end() && lit->second.label.has_value() &&
          !lit->second.label->empty())
        srcLabel = *lit->second.label;
    }
    VarMeta& vm = session.varMeta[target];
    vm.stataType = "str" + std::to_string(maxLen);
    if (doReplace) vm.format.reset();
    if (srcLabel.has_value()) vm.label = *srcLabel;
  }
  ojson tostringResult =
      targetPresent ? mutationResult(session, "tostring", /*newVars=*/{}, /*droppedVars=*/{},
                                     /*rewrittenVars=*/{target})
                    : mutationResult(session, "tostring", {target});
  if (!targetPresent) {
    if (auto err = placeGeneratedAfterSource(session, "tostring", varname, target,
                                             residentSetcol, tostringResult))
      return *err;
  }
  // C4-L4: memoize the committed row strings (UNESCAPED — csvEscape is
  // transport-only; the engine stores the unescaped value) for the chained
  // consumer. Put AFTER mutationResult (the fold erases the name first) and
  // after the generate() placement (see the destring site).
  if (residentSetcol && session.residentHeadDirty()) {
    SessionState::ResidentComputedCol put;
    put.dtype = "factor";
    put.nRows = n;
    put.strs = std::move(outStrings);
    session.residentComputedColPut(target, std::move(put));
  }
  return tostringResult;
}

// ---------------------------------------------------------------------------
// joinby key1 [key2 ...] using FILE (bare form only) — BS-004/D5 (BETA
// B-data). See handlers.h's runJoinby doc comment for the design summary.
// L1b: the join itself is now ONE engine call (`cobra joinby --data --using
// --keys --out`) — the old in-C++ cross-product + whole-frame CSV + `cobra
// convert` round-trip is retired. PIET keeps the using-file resolution and
// all pre-validation below, so the established error messages still fire
// before the engine is reached.


}  // namespace piet::handlers
