// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// BUG-471 T2: the storage-type resolver. See piet/storage_type.h for the rules
// and the oracle arms each one rests on.
#include "piet/storage_type.h"

#include <atomic>
#include <cctype>
#include <cmath>
#include <limits>
#include <memory>
#include <regex>

#include "piet/cobra_runner.h"
#include "piet/column_fetch.h"
#include "piet/datastore.h"
#include "piet/eval/errors.h"
#include "piet/session_state.h"
#include "piet/strutil.h"
#include "piet/translate.h"

namespace piet::storage {

using ojson = nlohmann::ordered_json;

namespace {

std::atomic<int64_t> gUnrecordedReplaceFallbacks{0};

// The probe's second scratch column: 1 where the stored value is not an integer.
const char* const kReplaceProbeFrac = "__piet_rp_frac__";

// Stata's integer ranges (data_types.sthlp; the engine's kStore*Lo/Hi in
// src/data/frame_internal.h, which is not in PIET's include path).
constexpr double kByteLo = -127.0, kByteHi = 100.0;
constexpr double kIntLo = -32767.0, kIntHi = 32740.0;
constexpr double kLongLo = -2147483647.0, kLongHi = 2147483620.0;

double jsonNumberOr(const ojson& j, const char* key, double fallback) {
  if (!j.is_object() || !j.contains(key) || !j[key].is_number()) return fallback;
  return j[key].get<double>();
}

}  // namespace

const char* const kReplaceProbeVar = "__piet_rp_val__";

bool isNumericType(const std::string& t) {
  return t == "byte" || t == "int" || t == "long" || t == "float" || t == "double";
}

bool isIntegerType(const std::string& t) { return t == "byte" || t == "int" || t == "long"; }

std::string resolveCreateType(const SessionState& session, const std::string& declaredToken) {
  if (!declaredToken.empty()) return isNumericType(declaredToken) ? declaredToken : std::string();
  return session.defaultStorageType;
}

void recordCreateType(SessionState& session, const std::string& varname,
                      const std::string& declaredToken, const std::string& stored) {
  if (varname.empty() || stored.empty()) return;
  if (declaredToken.empty() && stored == "float") {
    session.implicitFloatVars.insert(varname);
    return;
  }
  session.implicitFloatVars.erase(varname);  // an explicit declaration wins
  session.varMeta[varname].stataType = stored;
}

std::string resolveReplaceType(SessionState& session, const std::string& varname) {
  auto it = session.varMeta.find(varname);
  if (it != session.varMeta.end() && it->second.stataType.has_value()) {
    const std::string& t = *it->second.stataType;
    if (isNumericType(t)) return t;
    return std::string();  // a string type: the numeric store has no say
  }
  if (session.implicitFloatVars.count(varname)) return "float";
  ++session.unrecordedReplaceFallbacks;
  ++gUnrecordedReplaceFallbacks;
  return "double";
}

void carryTypeClaim(SessionState& session, const std::string& from, const std::string& to,
                    bool move) {
  if (from.empty() || to.empty() || from == to) return;
  std::optional<std::string> declared;
  auto it = session.varMeta.find(from);
  if (it != session.varMeta.end() && it->second.stataType.has_value())
    declared = *it->second.stataType;
  const bool implicitFloat = session.implicitFloatVars.count(from) != 0;
  if (declared.has_value()) {
    session.varMeta[to].stataType = *declared;
    session.implicitFloatVars.erase(to);
  } else {
    auto toIt = session.varMeta.find(to);
    if (toIt != session.varMeta.end()) toIt->second.stataType.reset();
    if (implicitFloat) session.implicitFloatVars.insert(to);
    else session.implicitFloatVars.erase(to);
  }
  if (move) session.implicitFloatVars.erase(from);
}

int64_t unrecordedReplaceFallbackTotal() { return gUnrecordedReplaceFallbacks.load(); }

void appendStoreTypeArg(std::vector<std::string>& args, const std::string& type) {
  if (type.empty() || type == "double") return;
  args.push_back("--store-type");
  args.push_back(type);
}

std::string promotedType(const std::string& current, bool anyFraction, double lo, double hi) {
  if (!isIntegerType(current)) return current;
  // A fraction: byte/int -> float (:810 :843, and still float under `set type
  // double` :912 — the target is not c(type)); long -> double (:859).
  if (anyFraction) return current == "long" ? "double" : "float";
  auto fits = [&](double a, double b) { return lo >= a && hi <= b; };
  if (current == "byte" && fits(kByteLo, kByteHi)) return "byte";
  // Integer overflow widens to the next integer type that holds it: byte -> int
  // (:876), int -> long (:893).
  if (current != "long" && fits(kIntLo, kIntHi)) return "int";
  if (fits(kLongLo, kLongHi)) return "long";
  // DESIGN, unpinned by the capture: an integer past long's range. The help file
  // names only int and long as integer targets; double is the type that holds
  // the value exactly, so nothing is lost while the arm is uncaptured.
  return "double";
}

std::optional<double> numericLiteral(const std::string& rhsIn) {
  const std::string rhs = strutil::trim(rhsIn);
  static const std::regex reNum(R"(^[+-]?(\d+\.?\d*|\.\d+)([eE][+-]?\d+)?$)");
  static const std::regex reMiss(R"(^\.[a-z]?$)");
  if (std::regex_match(rhs, reMiss)) return std::numeric_limits<double>::quiet_NaN();
  if (!std::regex_match(rhs, reNum)) return std::nullopt;
  try {
    return std::stod(rhs);
  } catch (...) {
    return std::nullopt;  // out of double's range: let the engine answer
  }
}

ReplaceTypeDecision decideReplaceStoreType(SessionState& session, const std::string& resolved,
                                           bool noPromote, const std::string& varname,
                                           const std::string& rhs, bool qualified,
                                           const std::string& probeFlag,
                                           const std::string& probeArg,
                                           const std::vector<std::string>& colExprs,
                                           bool drawContext) {
  ReplaceTypeDecision d;
  d.storeType = resolved;
  if (!isIntegerType(resolved) || noPromote) return d;  // nopromote truncates in place (:827)

  // A literal: every touched row receives this one value. A value the type holds
  // decides without data, qualified or not (`replace d = 1 if x > 5` into a
  // byte, the commonest replace there is); one that widens it does too when
  // every row is touched. Only a widening literal under if/in reads the data,
  // because a replace that touches no row changes nothing.
  if (std::optional<double> lit = numericLiteral(rhs)) {
    if (std::isnan(*lit)) return d;  // missing fits every type
    const bool frac = std::trunc(*lit) != *lit;
    const std::string widened = promotedType(resolved, frac, *lit, *lit);
    if (widened == resolved || !qualified) {
      d.storeType = widened;
      return d;
    }
  }

  // The data pass. A projection of the columns the probe reads when the head is
  // resident; otherwise the head file itself (materialized by datastore()).
  std::string dataPath;
  std::unique_ptr<Datastore> proj;
  {
    std::vector<const std::string*> ex;
    for (const auto& s : colExprs) ex.push_back(&s);
    if (auto cols = referencedColumns(session, ex, varname)) {
      std::string projPath;
      proj = fetchColumns(session, *cols, session.workdir(), &projPath);
      ++d.engineCalls;
      if (proj) dataPath = projPath;
    }
  }
  if (!proj) {
    Datastore* ds = session.datastore();
    if (ds == nullptr) {
      d.error = ojson(eval::noDataLoaded());
      return d;
    }
    dataPath = ds->path();
  }
  const std::string pv = kReplaceProbeVar;
  const std::string pf = kReplaceProbeFrac;
  std::vector<std::string> args = {"eval",      "--data", dataPath,
                                   probeFlag,   probeArg, "--gen",
                                   pf + "=(" + pv + "!=int(" + pv + "))",
                                   "--summarize", pv + "," + pf};
  args = translate::injectTsContextForExpr(args, probeArg, session);
  if (drawContext) {
    // The store draws from the stream the probe would consume: hand the probe
    // that stream and give it back, so both see the same draws.
    const long long savedStream = session.rngStream;
    args = translate::injectRngContextForExpr(args, probeArg, session);
    session.rngStream = savedStream;
  }
  ojson out;
  {
    // No --out: a read-only call. Stateless, so the resident frame never stages
    // the probe's scratch columns and a projection never evicts the head.
    SessionState::ResidentSuspend suspend(session);
    SessionState::StatelessRead statelessRead(session);
    try {
      out = runCobraJson(args);
    } catch (const CobraError& ce) {
      d.error = ojson(eval::engineError(cobraErrorMessage(ce), ce.code));
      return d;
    }
  }
  ++d.engineCalls;
  const ojson summ = out.value("summarize", ojson::object());
  const ojson sv = summ.value(pv, ojson::object());
  const ojson sf = summ.value(pf, ojson::object());
  const double n = jsonNumberOr(sv, "N", 0.0);
  if (!(n > 0)) return d;  // nothing non-missing is stored: no widening
  const double lo = jsonNumberOr(sv, "min", 0.0);
  const double hi = jsonNumberOr(sv, "max", 0.0);
  const bool frac = jsonNumberOr(sf, "max", 0.0) > 0.0;
  d.storeType = promotedType(resolved, frac, lo, hi);
  return d;
}

std::string stataDefaultFormat(const std::optional<std::string>& t) {
  if (t == std::string("double")) return "%10.0g";
  if (t == std::string("long")) return "%12.0g";
  if (t == std::string("byte") || t == std::string("int")) return "%8.0g";
  return "%9.0g";
}

void recordReplaceType(SessionState& session, const std::string& varname,
                       const std::string& resolved, const std::string& stored) {
  if (varname.empty() || stored.empty() || stored == resolved) return;
  session.implicitFloatVars.erase(varname);
  VarMeta& vm = session.varMeta[varname];
  // The widened variable keeps the display format it had (byte -> float keeps
  // %8.0g, exprfns_contract_oracle.log:646/736; int -> long keeps %8.0g,
  // :655/737). Only a recorded pre-widening type says what that was, and an
  // explicit `format` stays as it is.
  if (vm.stataType.has_value() && *vm.stataType == resolved &&
      (!vm.format.has_value() || vm.format->empty())) {
    vm.format = stataDefaultFormat(vm.stataType);
  }
  vm.stataType = stored;
}

std::string egenFunctionName(const std::string& rhs) {
  static const std::regex reFn(R"(^\s*(\w+)\s*\()");
  std::smatch m;
  if (!std::regex_search(rhs, m, reFn)) return std::string();
  return strutil::lower(m.str(1));
}

bool egenIgnoresStoreType(const std::string& fn) {
  return fn == "tag" || fn == "seq" || fn == "mode";
}

bool recordEgenFixedType(SessionState& session, const std::string& newvar,
                         const std::string& rhs, const std::string& fixedType) {
  const std::string fn = egenFunctionName(rhs);
  if (newvar.empty()) return false;
  if (!egenIgnoresStoreType(fn)) {
    // cut(), anyvalue(), anycount(), anymatch() and concat() with punct()/format()
    // (egen.cpp runEgenAdoFunction) recorded the result's type themselves: cut's
    // declared byte widened to float by a .5 edge, anycount's byte whatever was
    // declared, concat's str#.
    if (fixedType.empty()) return false;
    session.implicitFloatVars.erase(newvar);
    session.varMeta[newvar].stataType = fixedType;
    return true;
  }
  if (fn == "tag" || fn == "seq") {
    session.implicitFloatVars.erase(newvar);
    // seq: `gen byte` widened by its replace (egen seq()) to
    // int/long once a value leaves byte's range (the promotion lattice).
    session.varMeta[newvar].stataType =
        fn == "seq" && isIntegerType(fixedType) ? fixedType : std::string("byte");
    return true;
  }
  // mode: `gen `type' g = varlist[_N]` with type = the source's own type
  // (egen mode()), compressed for a string source unless the function left
  // early (runEgen hands back the str# then, BUG-1109).
  session.implicitFloatVars.erase(newvar);
  if (!fixedType.empty()) {
    session.varMeta[newvar].stataType = fixedType;
    return true;
  }
  // The source is the one variable inside mode(...).
  static const std::regex reSrc(R"(^\s*\w+\s*\(\s*([A-Za-z_]\w*)\s*\))");
  std::smatch m;
  std::string src;
  if (std::regex_search(rhs, m, reSrc)) src = m.str(1);
  // The source's claim, copied (carryTypeClaim, FU-243's one carry rule); a
  // source whose type was never recorded leaves the copy with none.
  if (!src.empty()) carryTypeClaim(session, src, newvar, /*move=*/false);
  return true;
}

}  // namespace piet::storage
