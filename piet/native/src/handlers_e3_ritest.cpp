// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// Portions derived from ritest 1.1.7 (GPL-3.0).
// See THIRD_PARTY_NOTICES.
//
// handlers_e3_ritest.cpp -- `ritest` (Simon Hess, SSC; ritest.ado 1.1.7, as identified below):
// randomization inference, derived from the ado. `ritest resampvar exp_list [if] [in] [, options] :
// command` runs COMMAND on the data, evaluates exp_list, then re-runs it reps()
// times with RESAMPVAR permuted (within strata, at the level of (strata, cluster)
// cells) and counts how often the permuted statistic reaches the observed one.
//
// Spec: internal note ritest/IDENTITY.md (method notes on
// the ado); oracle tests/eval/golden/aer_w2/w2j_e3_ritest.log. The steps, in
// the ado's order (each check at its own point, so the rc of a refused call is
// Stata's):
//   ado   confirm the resampvar (r(111));
//   ado      `:` required (r(198) "'' found where ':' expected");
//   _prefix_command: the inner command must exist (r(111)), `if`/`in` on one side
//               only (r(198)), the command is rebuilt with the condition;
//   ado the option syntax (an option ritest does not have is r(198));
//   ado alternative sampling methods combined (r(198));
//   ado seed() is `set seed` (a global side effect, as in Stata);
//   ado     the command on the data as is (its output is shown);
//   ado e() and r() cleared, the command again, `estimates store`
//               (r(301) when it leaves no e(cmd));
//   ado the statistics on the full data (r(322) when one is missing);
//   ado     reps() >= 1 (r(198));
//   ado strata and (strata, cluster) cells among the if/in rows, the
//               resampvar constant within a cell (r(9999));
//   ado eps() >= 0, strict needs eps(0) (r(198));
//   ado the replicates (permute_simple, ado);
//   ado     left and right together (r(198), after the replicates);
//   ado the original estimation restored, r() set.
// Draws: PIET's own Philox stream keyed on `set seed` (EvalSession::rngSeed /
// rngDraws, the counter runiform() advances), one uniform per row per replicate
// as permute_simple draws them; draw identity with Stata is out of scope (1-S),
// and every PREC field of the oracle is draw-independent.
// Not ported (refused by name): weights (force), samplingsourcefile/
// samplingprogram and their randomization* synonyms, null(), fixlevels(),
// reject(), saveresampling(), kdensityplot, the `_b`/`_se` exp_list shortcuts.
// Not written: the saving() dataset's characteristics (ado; PIET has no
// `char` command), so a later `: char _dta[...]` reads empty.
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "core/distributions.h"
#include "core/rng.h"
#include "handlers_e3_common.h"
#include "piet/datastore.h"
#include "piet/eval/errors.h"
#include "piet/eval/macros.h"
#include "piet/handlers.h"
#include "piet/program.h"
#include "piet/session_state.h"
#include "piet/strutil.h"
#include "registry.h"

namespace piet {
namespace handlers {

namespace {

using e3::ojson;
using e3::rcError;

std::string lowerS(std::string s) {
  for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return s;
}

// The index of the first top-level occurrence of `ch` (outside "...", (), []),
// or npos.
size_t findTopLevel(const std::string& s, char ch, size_t from = 0) {
  int paren = 0, brack = 0;
  bool inq = false;
  for (size_t i = from; i < s.size(); ++i) {
    const char c = s[i];
    if (c == '"') {
      inq = !inq;
      continue;
    }
    if (inq) continue;
    if (c == '(') ++paren;
    else if (c == ')') --paren;
    else if (c == '[') ++brack;
    else if (c == ']') --brack;
    else if (c == ch && paren == 0 && brack == 0) return i;
  }
  return std::string::npos;
}

// Top-level whitespace-separated words, (), [] and "..." kept whole.
std::vector<std::string> topWords(const std::string& s) {
  std::vector<std::string> out;
  std::string cur;
  int paren = 0, brack = 0;
  bool inq = false;
  for (char c : s) {
    if (c == '"') inq = !inq;
    if (!inq) {
      if (c == '(') ++paren;
      else if (c == ')') --paren;
      else if (c == '[') ++brack;
      else if (c == ']') --brack;
    }
    if (!inq && paren == 0 && brack == 0 && std::isspace(static_cast<unsigned char>(c))) {
      if (!cur.empty()) out.push_back(cur);
      cur.clear();
      continue;
    }
    cur += c;
  }
  if (!cur.empty()) out.push_back(cur);
  return out;
}

// Split `text` at its first top-level whole word `if` / `in` and a top-level
// `[weight]` (a `[` that starts a word): {lead, if-expression, in-range, weight}.
struct Quals {
  std::string lead, ifExp, inRange, weight;
  bool hasIf = false, hasIn = false, hasWeight = false;
};
Quals splitQuals(const std::string& text) {
  Quals q;
  std::string* dst = &q.lead;
  int paren = 0, brack = 0;
  bool inq = false;
  for (size_t i = 0; i < text.size(); ++i) {
    const char c = text[i];
    const bool wordStart = i == 0 || std::isspace(static_cast<unsigned char>(text[i - 1]));
    if (!inq && paren == 0 && brack == 0 && wordStart) {
      auto kw = [&](const char* w, size_t n) {
        return text.compare(i, n, w) == 0 &&
               (i + n == text.size() || std::isspace(static_cast<unsigned char>(text[i + n])));
      };
      if (!q.hasIf && dst == &q.lead && kw("if", 2)) {
        q.hasIf = true;
        dst = &q.ifExp;
        ++i;
        continue;
      }
      if (!q.hasIn && kw("in", 2)) {
        q.hasIn = true;
        dst = &q.inRange;
        ++i;
        continue;
      }
      if (c == '[' && dst != &q.ifExp) {
        const size_t close = text.find(']', i);
        q.hasWeight = true;
        q.weight = text.substr(i + 1, (close == std::string::npos ? text.size() : close) - i - 1);
        i = close == std::string::npos ? text.size() : close;
        continue;
      }
    }
    if (c == '"') inq = !inq;
    if (!inq) {
      if (c == '(') ++paren;
      else if (c == ')') --paren;
      else if (c == '[') ++brack;
      else if (c == ']') --brack;
    }
    *dst += c;
  }
  q.lead = strutil::trim(q.lead);
  q.ifExp = strutil::trim(q.ifExp);
  q.inRange = strutil::trim(q.inRange);
  q.weight = strutil::trim(q.weight);
  return q;
}

// Macro expansion of text that may hold real macro references (`x', $g) but
// whose bare e()/r() references are the statistic's, evaluated per replicate:
// those are masked from the expander's e()/r() layers and put back.
bool expandKeepingClassRefs(const std::string& in, SessionState& session, std::string* out,
                            ojson* err) {
  if (in.find('`') == std::string::npos && in.find('$') == std::string::npos) {
    *out = in;
    return true;
  }
  std::string masked;
  for (size_t i = 0; i < in.size(); ++i) {
    const char c = in[i];
    const bool boundary =
        i == 0 || !cecon::ident::isIdentByte(in[i - 1]);  // bytes >= 0x80 continue a Unicode name
    if (boundary && (c == 'e' || c == 'r') && i + 1 < in.size() && in[i + 1] == '(') {
      masked += c == 'e' ? '\x02' : '\x03';
      continue;
    }
    masked += c;
  }
  eval::ExpandResult r = eval::expandMacros(masked, session.eval);
  if (r.isError) {
    *err = r.error;
    return false;
  }
  std::string s = r.line;
  for (auto& c : s) {
    if (c == '\x02') c = 'e';
    else if (c == '\x03') c = 'r';
  }
  *out = s;
  return true;
}

bool expandText(const std::string& in, SessionState& session, std::string* out, ojson* err) {
  if (in.empty() || !eval::hasMacroRefs(in)) {
    *out = in;
    return true;
  }
  eval::ExpandResult r = eval::expandMacros(in, session.eval);
  if (r.isError) {
    *err = r.error;
    return false;
  }
  *out = r.line;
  return true;
}

// One statistic after a run: the number, NaN for Stata's missing; nullopt with
// *rc/*msg when the expression itself fails.
std::optional<double> evalStatistic(const std::string& expr, SessionState& session, int* rc,
                                    std::string* msg) {
  std::string text = expr;
  if (eval::hasMacroRefs(text)) {
    eval::ExpandResult r = eval::expandMacros(text, session.eval);
    if (r.isError) {
      *rc = 198;
      *msg = r.error.value("message", std::string("invalid expression"));
      const std::string reason = r.error.value("reason", std::string());
      if (reason.rfind("r(", 0) == 0) *rc = std::atoi(reason.c_str() + 2);
      return std::nullopt;
    }
    text = r.line;
  }
  std::string raised;
  bool isStr = false;
  std::optional<std::string> v = eval::evalArith(text, &session.eval, &raised, &isStr);
  if (!v.has_value() || isStr) {
    *rc = isStr ? 109 : 198;
    *msg = raised.empty() ? (isStr ? std::string("type mismatch") : "invalid expression " + expr)
                          : raised;
    if (raised.rfind("r(", 0) == 0) *rc = std::atoi(raised.c_str() + 2);
    return std::nullopt;
  }
  const std::string t = strutil::trim(*v);
  if (t.empty() || t[0] == '.') return std::nan("");
  char* end = nullptr;
  const double d = std::strtod(t.c_str(), &end);
  if (end == nullptr || *end != '\0') {
    *rc = 198;
    *msg = "invalid expression " + expr;
    return std::nullopt;
  }
  return d;
}

// The uniform draws permute_simple's `gen rorder = runiform()` makes: PIET's
// session stream counter (the one runiform() advances; exprfns.cpp
// macroRngDraw64), so a `set seed` restarts it and consecutive calls continue.
double drawUniform(SessionState& session) {
  eval::EvalSession& ev = session.eval;
  eval::EvalSession::RngSeedReading reading;
  if (ev.rngSeed) reading = ev.rngSeed();
  if (reading.epoch != ev.rngEpochSeen) {
    ev.rngEpochSeen = reading.epoch;
    ev.rngDraws = 0;
  }
  const uint64_t seed = static_cast<uint64_t>(reading.seed.value_or(123456789LL));
  constexpr uint64_t kRitestStream = 0x5249544553545052ull;  // "RITESTPR"
  const uint64_t g = ev.rngDraws++;
  const uint64_t hi = cecon::Philox::draw32(seed, kRitestStream, 2 * g);
  const uint64_t lo = cecon::Philox::draw32(seed, kRitestStream, 2 * g + 1);
  const uint64_t bits = ((hi << 32) | lo) >> 11;
  return (static_cast<double>(bits) + 0.5) * 0x1.0p-53;
}

double betaInv(double target, double a, double b) {
  if (a <= 0) return 0.0;
  if (b <= 0) return 1.0;
  double lo = 0.0, hi = 1.0;
  for (int i = 0; i < 200; ++i) {
    const double mid = 0.5 * (lo + hi);
    if (cecon::betai(a, b, mid) < target) lo = mid;
    else hi = mid;
  }
  return 0.5 * (lo + hi);
}

// `cii n c` (ado): the exact (Clopper-Pearson) interval at level(), two-sided.
std::pair<double, double> clopperPearson(double c, double n, double alpha) {
  const double lo = c <= 0 ? 0.0 : betaInv(alpha / 2.0, c, n - c + 1);
  const double hi = c >= n ? 1.0 : betaInv(1.0 - alpha / 2.0, c + 1, n - c);
  return {lo, hi};
}

// The live e() as it stands -- what `estimates store`/`restore` round-trip.
struct EState {
  std::optional<LastModel> lm;
  std::optional<ojson> block;
  std::optional<ojson> lmResult;
  eval::OrderedFloatDict scalars;
  eval::OrderedDict strings;
  std::vector<std::pair<std::string, eval::EvalMatrix>> matrices;
  bool posted = false;
  unsigned long long postedMask = 0;
  std::vector<std::string> onFit;
  std::string activeName;
  ojson estAdded;
  std::string estAddedFp;
};

EState snapshotE(SessionState& session) {
  EState s;
  s.lm = session.lastModel();
  s.block = session.lastEstBlock;
  s.lmResult = session.eval.lastModelResult;
  s.scalars = session.eval.eScalars;
  s.strings = session.eval.eStrings;
  s.matrices = session.eval.eMatrices;
  s.posted = session.ePostedLive;
  s.postedMask = session.ePostedEsampleSerial;
  s.onFit = session.eOnFitNames;
  s.activeName = session.activeEstimatesName;
  s.estAdded = session.estAdded;
  s.estAddedFp = session.estAddedFp;
  return s;
}

void restoreE(SessionState& session, const EState& s) {
  if (s.lm.has_value()) {
    session.setLastModel(*s.lm);
  } else {
    session.lastModel_reset();
  }
  session.lastEstBlock = s.block;
  session.eval.lastModelResult = s.lmResult;
  session.eval.eScalars = s.scalars;
  session.eval.eStrings = s.strings;
  session.eval.eMatrices = s.matrices;
  session.ePostedLive = s.posted;
  session.ePostedEsampleSerial = s.postedMask;
  session.eOnFitNames = s.onFit;
  session.activeEstimatesName = s.activeName;
  session.estAdded = s.estAdded;
  session.estAddedFp = s.estAddedFp;
}

// A row key over one or more variables (factor codes for strings); missing
// sorts last, as Stata's `egen group(...), missing` orders it.
std::vector<std::vector<double>> keyColumns(Datastore* ds, const std::vector<std::string>& vars,
                                            size_t n) {
  std::vector<std::vector<double>> cols;
  for (const auto& v : vars) {
    const cecon::Column& c = ds->col(v);
    std::vector<double> col(n, HUGE_VAL);
    if (c.dtype == cecon::DType::Factor) {
      const int32_t* codes = c.codes();
      for (size_t i = 0; i < n; ++i) {
        const bool missing = codes == nullptr || codes[i] < 0 ||
                             (static_cast<size_t>(codes[i]) < c.levels.size() &&
                              c.levels[static_cast<size_t>(codes[i])].empty());
        col[i] = missing ? HUGE_VAL : static_cast<double>(codes[i]);
      }
    } else {
      const double* p = c.f64();
      for (size_t i = 0; i < n; ++i) col[i] = (p == nullptr || std::isnan(p[i])) ? HUGE_VAL : p[i];
    }
    cols.push_back(std::move(col));
  }
  return cols;
}

struct ExpItem {
  std::string name;
  std::string expr;
};

std::string joinWords(const std::vector<std::string>& v) {
  std::string s;
  for (const auto& w : v) s += (s.empty() ? "" : " ") + w;
  return s;
}

}  // namespace

nlohmann::ordered_json runRitestLine(const std::string& restIn, SessionState& session) {
  ojson blocks = ojson::array();
  auto fail = [&blocks](ojson e) {
    blocks.push_back(std::move(e));
    return blocks;
  };
  if (!session.hasData()) return fail(ojson(eval::noDataLoaded()));
  ojson err;

  // ---- ado the resampvar --------------------------------------------------
  std::string rest = strutil::trim(restIn);
  size_t cut = 0;
  while (cut < rest.size() && !std::isspace(static_cast<unsigned char>(rest[cut])) &&
         rest[cut] != ',' && rest[cut] != ':')
    ++cut;
  std::string resampTyped = rest.substr(0, cut);
  rest = rest.substr(cut);
  if (!expandText(resampTyped, session, &resampTyped, &err)) return fail(err);
  if (resampTyped.empty()) return fail(rcError("varlist required", 100));
  std::string resampvar;
  {
    std::vector<std::string> v;
    if (!e3::expandVars(resampTyped, session, &v, &err)) return fail(err);
    if (v.size() != 1) return fail(rcError("too many variables specified", 103));
    resampvar = v[0];
  }

  // ---- ado <my_stuff> : <command> -----------------------------------------------
  const size_t colon = findTopLevel(rest, ':');
  if (colon == std::string::npos) return fail(rcError("'' found where ':' expected", 198));
  const std::string myStuff = strutil::trim(rest.substr(0, colon));
  std::string command = strutil::trim(rest.substr(colon + 1));
  if (!expandText(command, session, &command, &err)) return fail(err);

  const size_t comma = findTopLevel(myStuff, ',');
  const std::string head = comma == std::string::npos ? myStuff : myStuff.substr(0, comma);
  std::string optText = comma == std::string::npos ? std::string() : myStuff.substr(comma + 1);
  if (!expandText(optText, session, &optText, &err)) return fail(err);
  Quals outer = splitQuals(head);
  std::string expListText;
  if (!expandKeepingClassRefs(outer.lead, session, &expListText, &err)) return fail(err);
  if (!expandText(outer.ifExp, session, &outer.ifExp, &err)) return fail(err);
  if (!expandText(outer.inRange, session, &outer.inRange, &err)) return fail(err);
  if (strutil::trim(expListText).empty()) return fail(rcError("expression list required", 100));

  // ---- _prefix_command: the inner command -------------------------------------------
  std::string cmdname;
  std::string cmdArgs;
  std::string cmdOpts;
  Quals inner;
  {
    size_t w = 0;
    while (w < command.size() && !std::isspace(static_cast<unsigned char>(command[w])) &&
           command[w] != ',')
      ++w;
    cmdname = command.substr(0, w);
    const std::string afterName = command.substr(w);
    if (cmdname.empty()) return fail(rcError("command required", 198));
    const std::string lc = lowerS(cmdname);
    if (lc == "xi:" || lc == "xi")
      return fail(rcError("xi is allowed only as a prefix to ritest", 199));
    if (lc == "by" || lc == "bysort" || lc == "by:" || lc == "bysort:")
      return fail(rcError(cmdname + " may not be used as the command of ritest", 199));
    const registry::CommandSpec* spec = registry::findByName(lc);
    const std::optional<std::string> prog = program::findProgram(session, cmdname);
    if (spec == nullptr && !prog.has_value())
      return fail(rcError(cmdname + " command not found", 111, "unsupported_command"));
    // _prefix_command's unabbreviation: a typed ABBREVIATION of a command
    // (`reg`) reads as the command (`regress`); a distinct command PIET serves
    // through another row (`reghdfe` on feols') keeps its own name.
    if (prog.has_value()) cmdname = *prog;
    else if (spec->canonical.compare(0, lc.size(), lc) == 0) cmdname = spec->canonical;
    const size_t ic = findTopLevel(afterName, ',');
    const std::string ihead = ic == std::string::npos ? afterName : afterName.substr(0, ic);
    cmdOpts = ic == std::string::npos ? std::string() : strutil::trim(afterName.substr(ic + 1));
    inner = splitQuals(ihead);
    cmdArgs = inner.lead;
  }
  if (outer.hasIf && inner.hasIf)
    return fail(rcError("ritest and " + cmdname + " may not both specify an if condition", 198));
  if (outer.hasIn && inner.hasIn)
    return fail(rcError("ritest and " + cmdname + " may not both specify an in range", 198));
  if (outer.hasWeight || inner.hasWeight)
    return fail(rcError("ritest: weights are not implemented in this version of PIET (ritest "
                        "refuses them without force); nothing was run",
                        101, "unsupported_option"));
  const std::string ifExp = outer.hasIf ? outer.ifExp : inner.ifExp;
  const std::string inRange = outer.hasIn ? outer.inRange : inner.inRange;
  std::string runCmd = cmdname;
  if (!cmdArgs.empty()) runCmd += " " + cmdArgs;
  if (!ifExp.empty()) runCmd += " if " + ifExp;
  if (!inRange.empty()) runCmd += " in " + inRange;
  if (!cmdOpts.empty()) runCmd += ", " + cmdOpts;

  // ---- ado the options --------------------------------------------------------
  static const std::vector<e3::OptSpec> kSpecs = {
      {"nodots", 6, false},
      {"lessdots", 8, false},
      {"reps", 1, true},
      {"saving", 2, true},
      {"saveresampling", 5, true},
      {"saverandomization", 6, true},
      {"double", 4, false},
      {"strata", 3, true},
      {"cluster", 3, true},
      {"fixlevels", 3, true},
      {"seed", 4, true},
      {"eps", 3, true},
      {"samplingsourcefile", 9, true},
      {"randomizationsourcefile", 14, true},
      {"samplingmatchvar", 9, true},
      {"randomizationmatchvar", 14, true},
      {"samplingprogram", 9, true},
      {"randomizationprogram", 14, true},
      {"samplingprogramoptions", 16, true},
      {"randomizationprogramoptions", 21, true},
      {"null", 4, true},
      {"reject", 6, true},
      {"noisily", 3, false},
      {"left", 2, false},
      {"right", 2, false},
      {"strict", 3, false},
      {"noheader", 3, false},
      {"nolegend", 4, false},
      {"noanalytics", 9, false},
      {"collapse", 5, false},
      {"kdensityplot", 5, false},
      {"kdensityoptions", 9, true},
      {"level", 1, true},
      {"force", 5, false},
      // _get_diopts (ado): the display options it admits
      {"noomitted", 6, false},
      {"vsquish", 3, false},
      {"noemptycells", 7, false},
      {"baselevels", 8, false},
      {"allbaselevels", 13, false},
      {"nofvlabel", 9, false},
      {"cformat", 7, true},
      {"pformat", 7, true},
      {"sformat", 7, true},
      {"nolstretch", 10, false}};
  long long reps = 100;
  double eps = 1e-7;
  double level = static_cast<double>(session.level);
  std::string savingText, seedText;
  std::vector<std::string> strataVars, clusterVars;
  bool left = false, right = false, strict = false, dbl = false;
  bool altFile = false, altProgram = false;
  std::vector<std::string> refusedOpts;
  for (const auto& w : topWords(optText)) {
    const size_t lp = w.find('(');
    const std::string key = lowerS(lp == std::string::npos ? w : w.substr(0, lp));
    std::string arg;
    if (lp != std::string::npos) {
      const size_t rp = w.rfind(')');
      arg = strutil::trim(w.substr(lp + 1, (rp == std::string::npos ? w.size() : rp) - lp - 1));
    }
    const int idx = e3::matchOpt(kSpecs, key);
    if (idx < 0)
      return fail(
          rcError("option " + key + (lp != std::string::npos ? "()" : "") + " not allowed", 198));
    const e3::OptSpec& s = kSpecs[static_cast<size_t>(idx)];
    if (s.takesArg && lp == std::string::npos)
      return fail(rcError("option " + s.full + "() required", 198));
    if (!s.takesArg && lp != std::string::npos)
      return fail(rcError("option " + s.full + "() not allowed", 198));
    const std::string& o = s.full;
    if (o == "reps") {
      char* end = nullptr;
      const double d = std::strtod(arg.c_str(), &end);
      if (arg.empty() || *end != '\0' || d != std::floor(d))
        return fail(rcError("option reps() incorrectly specified", 198));
      reps = static_cast<long long>(d);
    } else if (o == "eps") {
      char* end = nullptr;
      const double d = std::strtod(arg.c_str(), &end);
      if (arg.empty() || *end != '\0')
        return fail(rcError("option eps() incorrectly specified", 198));
      eps = d;
    } else if (o == "level") {
      char* end = nullptr;
      const double d = std::strtod(arg.c_str(), &end);
      if (arg.empty() || *end != '\0' || d < 10 || d > 99.99)
        return fail(rcError("level() must be between 10 and 99.99 inclusive", 198));
      level = d;
    } else if (o == "strata" || o == "cluster") {
      std::vector<std::string> v;
      if (!e3::expandVars(arg, session, &v, &err)) {
        blocks.push_back(e3::displayText("(error in option " + o + "())"));
        return fail(err);
      }
      (o == "strata" ? strataVars : clusterVars) = v;
    } else if (o == "seed") {
      seedText = arg;
    } else if (o == "saving") {
      savingText = arg;
    } else if (o == "double") {
      dbl = true;
    } else if (o == "left") {
      left = true;
    } else if (o == "right") {
      right = true;
    } else if (o == "strict") {
      strict = true;
    } else if (o == "samplingsourcefile" || o == "randomizationsourcefile" ||
               o == "samplingmatchvar" || o == "randomizationmatchvar") {
      altFile = true;
      refusedOpts.push_back(o);
    } else if (o == "samplingprogram" || o == "randomizationprogram" ||
               o == "samplingprogramoptions" || o == "randomizationprogramoptions") {
      altProgram = true;
      refusedOpts.push_back(o);
    } else if (o == "null" || o == "fixlevels" || o == "reject" || o == "saveresampling" ||
               o == "saverandomization" || o == "kdensityplot" || o == "kdensityoptions" ||
               o == "collapse" || o == "force") {
      refusedOpts.push_back(o);
    }
  }
  // ado
  if (((!strataVars.empty() || !clusterVars.empty()) ? 1 : 0) + (altFile ? 1 : 0) +
          (altProgram ? 1 : 0) >
      1)
    return fail(rcError("Alternative sampling methods may not be combined.", 198));
  if (!refusedOpts.empty())
    return fail(rcError("ritest, " + refusedOpts.front() +
                            "(): not implemented in this version of PIET; nothing was run",
                        198, "unsupported_option"));

  // ---- ado seed() is `set seed` --------------------------------------------
  if (!seedText.empty() && !e3::runLine("set seed " + seedText, session, &err)) return fail(err);

  // ---- _prefix_explist ------------------------------------------------------------------
  std::vector<ExpItem> exps;
  for (std::string w : topWords(expListText)) {
    std::string name;
    const size_t eq = findTopLevel(w, '=');
    if (eq != std::string::npos && eq > 0 && (eq + 1 >= w.size() || w[eq + 1] != '=') &&
        strutil::isValidStataIdentifier(w.substr(0, eq))) {
      name = w.substr(0, eq);
      w = w.substr(eq + 1);
    }
    if (w.size() >= 2 && w.front() == '(' && w.back() == ')')
      w = strutil::trim(w.substr(1, w.size() - 2));
    if (w == "_b" || w == "_se")
      return fail(rcError("ritest: the `" + w +
                              "` shortcut (every coefficient) is not implemented in this version "
                              "of PIET; name the coefficients",
                          198, "unsupported_option"));
    if (w.empty()) return fail(rcError("invalid expression list", 198));
    exps.push_back({name, w});
  }
  const size_t K = exps.size();
  for (size_t j = 0; j < K; ++j)
    if (exps[j].name.empty()) exps[j].name = "_pm_" + std::to_string(j + 1);

  // ---- ado the command on the data as is (its output shown) ----------------------
  try {
    (void)session.datastore();
  } catch (const std::exception& e) {
    return fail(rcError(std::string("ritest: the data could not be read: ") + e.what(), 198));
  }
  {
    ojson out = dispatch(runCmd, session);
    for (const auto& b : out) blocks.push_back(b);
    if (e3::firstError(out)) return blocks;
  }

  // ---- ado mark touse (the if/in rows) ------------------------------------------
  Datastore* ds = session.datastore();
  const size_t N = static_cast<size_t>(ds->nRows());
  std::vector<char> touse(N, 1);
  if (!ifExp.empty() || !inRange.empty()) {
    const std::string tv = "__ritest_touse";
    std::string q;
    if (!ifExp.empty()) q += " if " + ifExp;
    if (!inRange.empty()) q += " in " + inRange;
    if (!e3::runLine("quietly generate double " + tv + " = 1" + q, session, &err)) return fail(err);
    ds = session.datastore();
    const double* p = ds->col(tv).f64();
    for (size_t i = 0; i < N; ++i) touse[i] = p != nullptr && p[i] == 1.0;
    {
      SessionState::TempDropScope td(session);
      (void)e3::runLine("capture drop " + tv, session, nullptr);
    }
    ds = session.datastore();
  }

  // ---- ado e() and r() cleared, the command again, `estimates store` ---------
  (void)e3::runLine("ereturn clear", session, nullptr);
  eval::replaceRClass(session.eval);
  {
    ojson out = dispatch(runCmd, session);
    if (auto e = e3::firstError(out)) return fail(*e);
  }
  {
    const std::string* ecmd = session.eval.eStrings.find("cmd");
    const bool haveCmd = (ecmd != nullptr && !ecmd->empty()) ||
                         (session.lastModel().has_value() &&
                          !(session.lastModel()->stataCmd.empty() &&
                            session.lastModel()->cobraCmd.empty()));
    if (!haveCmd) return fail(rcError("last estimation results not found, nothing to store", 301));
  }
  const EState original = snapshotE(session);
  try {
    (void)session.datastore();
  } catch (const std::exception& e) {
    return fail(rcError(std::string("ritest: the data could not be read: ") + e.what(), 198));
  }
  const std::string origHbin = *session.currentHbin();
  SessionState::ScratchPin pinOrig(session, {origHbin});
  // Every exit from here on puts the data and e() back as the observed run left
  // them (ritest's `preserve`, ado, and `est restore`, ado).
  auto finish = [&](std::optional<ojson> e) {
    session.load(origHbin);
    restoreE(session, original);
    if (e.has_value()) blocks.push_back(*e);
    return blocks;
  };

  // ---- ado the statistics on the full data -----------------------------------
  std::vector<double> bObs(K);
  for (size_t j = 0; j < K; ++j) {
    int rc = 0;
    std::string msg;
    std::optional<double> v = evalStatistic(exps[j].expr, session, &rc, &msg);
    if (!v.has_value()) return finish(rcError(msg, rc));
    if (std::isnan(*v))
      return finish(rcError("'" + exps[j].expr + "' evaluated to missing in full sample", 322));
    bObs[j] = *v;
  }

  // ---- ado reps(), saving() -----------------------------------------------------
  if (reps < 1) return finish(rcError("reps() must be a positive integer", 198));
  std::string savingFile;
  bool savingReplace = false;
  if (!savingText.empty()) {
    const size_t sc = findTopLevel(savingText, ',');
    savingFile = strutil::trim(sc == std::string::npos ? savingText : savingText.substr(0, sc));
    if (savingFile.size() >= 2 && savingFile.front() == '"' && savingFile.back() == '"')
      savingFile = savingFile.substr(1, savingFile.size() - 2);
    if (sc != std::string::npos) {
      for (const auto& w : topWords(savingText.substr(sc + 1))) {
        const std::string lw = lowerS(w);
        if (lw == "replace") {
          savingReplace = true;
        } else if (lw == "double") {
          dbl = true;
        } else if (lw.rfind("every(", 0) != 0) {
          return finish(rcError("option " + lw + " not allowed", 198));
        }
      }
    }
    if (savingFile.empty()) return finish(rcError("saving(): filename required", 198));
  }

  // ---- ado strata and (strata, cluster) cells -----------------------------------
  ds = session.datastore();
  for (const auto& v : strataVars)
    if (v == resampvar)
      return finish(
          rcError("permutation variable may not be specified in strata() option", 198));
  std::vector<long long> stratum(N, 0), cell(N, 0);
  long long nStrata = 0, nClust = 0;
  {
    // strata group id (egen group(strata), missing): every row has one.
    std::vector<long long> sg(N, 1);
    if (!strataVars.empty()) {
      const auto cols = keyColumns(ds, strataVars, N);
      std::map<std::vector<double>, long long> ids;
      for (size_t i = 0; i < N; ++i) {
        std::vector<double> k;
        for (const auto& c : cols) k.push_back(c[i]);
        ids.emplace(k, 0);
      }
      long long g = 0;
      for (auto& kv : ids) kv.second = ++g;
      for (size_t i = 0; i < N; ++i) {
        std::vector<double> k;
        for (const auto& c : cols) k.push_back(c[i]);
        sg[i] = ids[k];
      }
    }
    // markout touse cluster, strok: a missing cluster value leaves the sample.
    std::vector<std::vector<double>> ccols;
    if (!clusterVars.empty()) {
      ccols = keyColumns(ds, clusterVars, N);
      for (size_t i = 0; i < N; ++i)
        for (const auto& c : ccols)
          if (c[i] == HUGE_VAL) touse[i] = 0;
    }
    // strata cells among the sample, numbered in strata order (sflag).
    std::map<long long, long long> sNum;
    for (size_t i = 0; i < N; ++i)
      if (touse[i]) sNum.emplace(sg[i], 0);
    for (auto& kv : sNum) kv.second = ++nStrata;
    for (size_t i = 0; i < N; ++i) stratum[i] = touse[i] ? sNum[sg[i]] : 0;
    // (strata, cluster) cells (cflag); without cluster() every row is a cell.
    std::map<std::pair<long long, std::vector<double>>, long long> cNum;
    auto cellKey = [&](size_t i) {
      std::vector<double> k;
      if (ccols.empty()) k.push_back(static_cast<double>(i));
      else
        for (const auto& c : ccols) k.push_back(c[i]);
      return std::make_pair(stratum[i], k);
    };
    for (size_t i = 0; i < N; ++i)
      if (touse[i]) cNum.emplace(cellKey(i), 0);
    for (auto& kv : cNum) kv.second = ++nClust;
    for (size_t i = 0; i < N; ++i)
      if (touse[i]) cell[i] = cNum[cellKey(i)];
  }
  std::vector<double> resampOrig(N);
  {
    const cecon::Column& c = ds->col(resampvar);
    if (c.dtype != cecon::DType::F64)
      return finish(rcError("ritest: a string resampvar is not implemented in this version of "
                            "PIET",
                            198, "unsupported_option"));
    const double* p = c.f64();
    for (size_t i = 0; i < N; ++i) resampOrig[i] = p[i];
  }
  // ado constant within a cell (loneway's within sd).
  {
    std::map<long long, double> first;
    for (size_t i = 0; i < N; ++i) {
      if (!touse[i] || std::isnan(resampOrig[i])) continue;
      auto it = first.find(cell[i]);
      if (it == first.end()) {
        first.emplace(cell[i], resampOrig[i]);
      } else if (it->second != resampOrig[i]) {
        return finish(rcError(resampvar + " does not seem to be constant within clusters", 9999));
      }
    }
  }
  // ado
  if (eps < 0) return finish(rcError("eps() must be greater than or equal to zero", 198));
  if (strict && eps > 0)
    return finish(rcError(
        "Warning: if the strict option is specified, eps must be set to 0 using -eps(0)", 198));

  // ---- ado the replicates ------------------------------------------------------
  // permute_simple's order: rows by (stratum, cell), stable; a cell's first row
  // carries its value; the first rows of a stratum are re-ordered by their
  // uniforms, and cell j takes the value of the cell at sorted position j's
  // original position (`newt = resampvar[nn]` within `by strata ind`).
  std::vector<size_t> order(N);
  for (size_t i = 0; i < N; ++i) order[i] = i;
  std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b) {
    if (stratum[a] != stratum[b]) return stratum[a] < stratum[b];
    return cell[a] < cell[b];
  });
  struct CellRef {
    size_t firstRow;
    long long stratum;
  };
  std::vector<CellRef> cells;  // in (stratum, cell) order
  std::vector<size_t> cellOfRow(N);
  for (size_t k = 0; k < N; ++k) {
    const size_t r = order[k];
    if (k == 0 || stratum[r] != stratum[order[k - 1]] || cell[r] != cell[order[k - 1]])
      cells.push_back({r, stratum[r]});
    cellOfRow[r] = cells.size() - 1;
  }
  const std::string valsBase = mintScratchPath(session.workdir(), "ritest_vals");
  forgetScratchMint(valsBase);
  const std::string valsPath = valsBase.substr(0, valsBase.size() - 5) + ".csv";
  registerScratchMint(valsPath);
  std::vector<std::vector<double>> repVals(static_cast<size_t>(reps), std::vector<double>(K));
  std::vector<double> u(N), cellVal(cells.size()), newCell(cells.size());
  for (long long rep = 0; rep < reps; ++rep) {
    for (size_t i = 0; i < N; ++i) u[i] = drawUniform(session);
    for (size_t c = 0; c < cells.size(); ++c) cellVal[c] = resampOrig[cells[c].firstRow];
    for (size_t lo = 0; lo < cells.size();) {
      size_t hi = lo;
      while (hi < cells.size() && cells[hi].stratum == cells[lo].stratum) ++hi;
      std::vector<size_t> idx;
      for (size_t c = lo; c < hi; ++c) idx.push_back(c);
      std::stable_sort(idx.begin(), idx.end(), [&](size_t a, size_t b) {
        return u[cells[a].firstRow] < u[cells[b].firstRow];
      });
      // pi(k) = idx[k] - lo; newt(c_j) = v(c_{pi(j)}).
      for (size_t j = 0; j < idx.size(); ++j) newCell[lo + j] = cellVal[idx[j]];
      lo = hi;
    }
    {
      std::ofstream f(cecon::to_native_path(valsPath), std::ios::binary | std::ios::trunc);
      f.setf(std::ios::fmtflags(0), std::ios::floatfield);
      f.precision(17);
      for (size_t i = 0; i < N; ++i) {
        const double v = e3::toFloat(newCell[cellOfRow[i]]);  // `gen newt`: float storage
        if (!std::isnan(v)) f << v;
        f << "\n";
      }
    }
    const std::string repPath = mintScratchPath(session.workdir(), "ritest_rep");
    const CobraRunResult cr =
        runCobra({"setcol", "--data", origHbin, "--var", resampvar, "--values", valsPath, "--dtype",
                  "f64", "--store-type", "float", "--out", repPath});
    if (!cr.ok || !cecon::native_exists(repPath)) {
      cecon::native_unlink(valsPath);
      return finish(rcError("ritest: the permuted variable could not be written", 198));
    }
    session.load(repPath);
    ojson out = dispatch(runCmd, session);
    const bool bad = e3::firstError(out).has_value();
    for (size_t j = 0; j < K; ++j) {
      double v = std::nan("");
      if (!bad) {
        int rc = 0;
        std::string msg;
        std::optional<double> s = evalStatistic(exps[j].expr, session, &rc, &msg);
        if (s.has_value()) v = *s;
      }
      repVals[static_cast<size_t>(rep)][j] = v;
    }
  }
  cecon::native_unlink(valsPath);

  // ---- ado the event -----------------------------------------------------
  if (left && right) return finish(rcError("only one of left or right can be specified", 198));
  const std::string ge = strict ? ">" : ">=";
  const std::string le = strict ? "<" : "<=";
  const std::string event = left    ? "T " + le + " T(obs)"
                            : right ? "T " + ge + " T(obs)"
                                    : "|T| " + ge + " |T(obs)|";

  // ---- ado the saved results (float unless double) and rit_GetResults ---------
  auto stored = [dbl](double v) { return dbl ? v : e3::toFloat(v); };
  std::vector<double> bR(K), cR(K), nR(K), pR(K), seR(K), loR(K), hiR(K);
  bool anyMissing = false;
  const double alpha = 1.0 - level / 100.0;
  for (size_t j = 0; j < K; ++j) {
    const double T = stored(bObs[j]);
    bR[j] = T;
    double c = 0, n = 0;
    for (long long r = 0; r < reps; ++r) {
      const double t = stored(repVals[static_cast<size_t>(r)][j]);
      if (std::isnan(t)) {
        anyMissing = true;
        continue;
      }
      bool hit;
      if (left) hit = strict ? (t < T + eps) : (t <= T + eps);
      else if (right) hit = strict ? (t > T - eps) : (t >= T - eps);
      else hit = strict ? (std::fabs(t) > std::fabs(T) - eps) : (std::fabs(t) >= std::fabs(T) - eps);
      n += 1;
      if (hit) c += 1;
    }
    cR[j] = c;
    nR[j] = n;
    pR[j] = n > 0 ? c / n : std::nan("");
    seR[j] = n > 0 ? std::sqrt(pR[j] * (1 - pR[j]) / n) : std::nan("");
    if (n > 0) {
      const auto ci = clopperPearson(c, n, alpha);
      loR[j] = ci.first;
      hiR[j] = ci.second;
    } else {
      loR[j] = hiR[j] = std::nan("");
    }
  }

  // saving(): the replicate dataset (_pm_1.._pm_K, float unless double), its data
  // label and variable labels (ado), reps() rows.
  if (!savingFile.empty()) {
    const std::string csvBase = mintScratchPath(session.workdir(), "ritest_sav");
    forgetScratchMint(csvBase);
    const std::string csv = csvBase.substr(0, csvBase.size() - 5) + ".csv";
    registerScratchMint(csv);
    {
      std::ofstream f(cecon::to_native_path(csv), std::ios::binary | std::ios::trunc);
      f.setf(std::ios::fmtflags(0), std::ios::floatfield);
      f.precision(17);
      for (size_t j = 0; j < K; ++j) f << (j ? "," : "") << exps[j].name;
      f << "\n";
      for (long long r = 0; r < reps; ++r) {
        for (size_t j = 0; j < K; ++j) {
          if (j) f << ",";
          const double v = stored(repVals[static_cast<size_t>(r)][j]);
          if (!std::isnan(v)) f << v;
        }
        f << "\n";
      }
    }
    const std::string hb = mintScratchPath(session.workdir(), "ritest_sav");
    const CobraRunResult cr = runCobra({"convert", "--data", csv, "--out", hb, "--delim", ","});
    cecon::native_unlink(csv);
    if (!cr.ok || !cecon::native_exists(hb))
      return finish(rcError("ritest: the saving() dataset could not be built", 198));
    session.load(hb);
    if (!dbl)
      for (size_t j = 0; j < K; ++j)
        (void)e3::runLine("quietly recast float " + exps[j].name, session, nullptr);
    (void)e3::runLine("label data \"ritest " + resampvar + " : " + cmdname + "\"", session, nullptr);
    for (size_t j = 0; j < K; ++j) {
      const std::string lab = exps[j].expr.substr(0, 80);
      if (lab.find('"') == std::string::npos)
        (void)e3::runLine("label variable " + exps[j].name + " \"" + lab + "\"", session, nullptr);
    }
    if (!e3::runLine("quietly save \"" + savingFile + "\"" + (savingReplace ? ", replace" : ""),
                     session, &err))
      return finish(err);
  }

  // ---- ado the original estimation back, r() ------------------------------------
  session.load(origHbin);
  restoreE(session, original);

  {
    char buf[256];
    std::string t;
    t += "\n      command:  " + runCmd + "\n";
    for (size_t j = 0; j < K; ++j) {
      std::snprintf(buf, sizeof buf, "%13s:  %s\n", exps[j].name.c_str(), exps[j].expr.c_str());
      t += buf;
    }
    t += "  res. var(s):  " + resampvar + "\n";
    t += "   Resampling:  Permuting " + resampvar + "\n";
    if (!clusterVars.empty()) {
      t += "Clust. var(s):  " + joinWords(clusterVars) + "\n";
      t += "     Clusters:  " + std::to_string(nClust) + "\n";
    }
    if (!strataVars.empty()) {
      t += "Strata var(s):  " + joinWords(strataVars) + "\n";
      t += "       Strata:  " + std::to_string(nStrata) + "\n";
    }
    t += "\n------------------------------------------------------------------------------\n";
    std::snprintf(buf, sizeof buf, "%-12s |%12s%8s%8s%8s%8s  [%g%% conf. interval]\n", "T",
                  "T(obs)", "c", "n", "p=c/n", "SE(p)", level);
    t += buf;
    t += "-------------+----------------------------------------------------------------\n";
    for (size_t j = 0; j < K; ++j) {
      // 12 CHARACTERS wide, right-aligned (StataBE pads by characters, not bytes).
      std::string stub = cecon::ident::truncateCodePoints(exps[j].name, 12);
      stub.insert(0, 12 - cecon::ident::codePointCount(stub), ' ');
      std::snprintf(buf, sizeof buf, "%s | %11.7g%8.0f%8.0f%8.4f%8.4f  %9.7g  %9.7g\n",
                    stub.c_str(), bR[j], cR[j], nR[j], pR[j], seR[j],
                    loR[j], hiR[j]);
      t += buf;
    }
    t += "------------------------------------------------------------------------------\n";
    t += K == 1 ? "Note: Confidence interval is with respect to p=c/n.\n"
                : "Note: Confidence intervals are with respect to p=c/n.\n";
    t += "Note: c = #{" + event + "}";
    if (anyMissing) t += "\nNote: Missing values observed in permutation replicates.";
    blocks.push_back(e3::displayText(t));
  }

  // r() (rit_GetResults, ado, and N_reps, ado)
  eval::replaceRClass(session.eval);
  std::vector<std::string> colnames;
  for (const auto& e : exps) colnames.push_back(e.name);
  auto rowMat = [&colnames](const std::vector<double>& v) {
    eval::EvalMatrix m;
    m.data.push_back(v);
    m.rownames = {"r1"};
    m.colnames = colnames;
    return m;
  };
  eval::EvalMatrix mb = rowMat(bR);
  mb.rownames = {"y1"};
  eval::EvalMatrix mci;
  mci.data = {loR, hiR};
  mci.rownames = {"ll", "ul"};
  mci.colnames = colnames;
  session.eval.rMatrices.emplace_back("reps", rowMat(nR));
  session.eval.rMatrices.emplace_back("c", rowMat(cR));
  session.eval.rMatrices.emplace_back("b", mb);
  session.eval.rMatrices.emplace_back("p", rowMat(pR));
  session.eval.rMatrices.emplace_back("se", rowMat(seR));
  session.eval.rMatrices.emplace_back("ci", mci);
  session.eval.rValues.set("N", static_cast<double>(N));
  session.eval.rValues.set("level", level);
  session.eval.rValues.set("k_eexp", 0.0);
  session.eval.rValues.set("k_exp", static_cast<double>(K));
  session.eval.rValues.set("N_strata", static_cast<double>(nStrata));
  session.eval.rValues.set("N_clust", static_cast<double>(nClust));
  session.eval.rValues.set("N_reps", static_cast<double>(reps));
  session.eval.rStrings.set("rngstate", std::to_string(session.seed.value_or(0)));
  session.eval.rStrings.set("missing", anyMissing ? "missing" : "");
  session.eval.rStrings.set("resampvar", resampvar);
  session.eval.rStrings.set("strata_orignal_varnames",
                            strataVars.empty() ? std::string("none") : joinWords(strataVars));
  // r(strata)/r(clustvar) name ritest's tempvars (`__000xxx`) when it made them.
  session.eval.rStrings.set("strata", "__000001");
  session.eval.rStrings.set("clustvar",
                            clusterVars.empty() ? std::string("__000000") : joinWords(clusterVars));
  session.eval.rStrings.set("event", event);
  session.eval.rStrings.set("left", left ? "left" : "");
  session.eval.rStrings.set("right", right ? "right" : "");
  session.eval.rStrings.set("strict", strict ? "strict" : "");
  for (size_t j = 0; j < K; ++j)
    session.eval.rStrings.set("exp" + std::to_string(j + 1), exps[j].expr);
  session.eval.rStrings.set("title", "Monte Carlo results");
  session.eval.rStrings.set("command", runCmd);
  session.eval.rStrings.set("sampling_method", "permute");
  session.eval.rStrings.set("cmd", "ritest");
  {
    std::vector<std::string> names = {"N",        "level",    "k_eexp",   "k_exp",
                                      "N_strata", "N_clust",  "N_reps",   "reps",
                                      "c",        "b",        "p",        "se",
                                      "ci",       "rngstate", "missing",  "resampvar",
                                      "strata_orignal_varnames", "strata", "clustvar",
                                      "event",    "left",     "right",    "strict",
                                      "title",    "command",  "sampling_method", "cmd"};
    for (size_t j = 0; j < K; ++j) names.push_back("exp" + std::to_string(j + 1));
    eval::declareRPosted(session.eval, "ritest", names);
  }
  return blocks;
}

// The registry route (`ritest` reaching the E3 arm without the raw-line
// intercept): the same driver on the typed text after the command word.
nlohmann::ordered_json runRitest(const eval::ParsedCommand& parsed, SessionState& session) {
  const std::string raw = strutil::trim(parsed.raw);
  const size_t sp = raw.find_first_of(" \t");
  return runRitestLine(sp == std::string::npos ? std::string() : raw.substr(sp), session);
}

}  // namespace handlers
}  // namespace piet
