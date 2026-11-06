// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// W2E float default (BUG-471 T2, FU-103, FU-136) -- PIET stores an untyped
// generate/egen/bysort-gen as Stata's default float, an integer-typed column as
// its integer, `set type` moves the default and c(type) reads it, replace keeps
// the variable's type and widens an integer type by Stata's lattice, and float()
// exists in the macro layer.
//
// Oracle: tests/eval/golden/aer_w2/w2e_float_default.log (StataBE 19.5). The
// table below is the do-file's own command stream for families (1) generate,
// (2) replace, (3) egen, (4) set type, (7) float()/round() and (10) propagation,
// extracted mechanically with the log's PREC record beside each arm (generator:
// autonomy/scratch/float-default-t2-piet-default-float/gen/mkarms.py). Families
// (5)/(6)/(8)/(9) are T3's and are not replayed; of predict_*, predict_default and
// predict_eq_count are (BUG-924: predict stores the float it labels).
//
// Tiers (tests/eval/TOLERANCE.md): rc, `: type`, counts and missing flags are
// T0 (exact text). A `%21x VAR[i]` field is the STORED value parsed from the
// log's hex: exact for a float or integer column (the rounding IS the claim),
// rtol 1e-9 for a double column. `%21x _b[]/_se[]` is T2 (rtol 1e-9) read from
// the estimation block's coeftable, never from PIET's %21x text (BUG-444).
// Seven arms print a SCALAR through `display %21x`. Three of them compare the
// stored scalar (FU-1325, BUG-710); the four float() arms are still SKIPPED by
// name (FU-141 / FU-225's float() half), not counted.
#include "selftest_w2.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <set>
#include <string>
#include <vector>

#include "piet/cobra_runner.h"
#include "piet/datastore.h"
#include "piet/dispatcher.h"
#include "piet/doexec.h"
#include "piet/eval/errors.h"
#include "piet/session_state.h"
#include "piet/storage_type.h"
#include "vendor/nlohmann/json.hpp"

namespace {

using ojson = nlohmann::ordered_json;

struct Reply {
  std::string rc = "0";
  std::string msg;
  std::string text;  // every display block's text, concatenated
  ojson est;         // the last estimation block (null when none)
};

Reply run(piet::SessionState& session, const std::string& cmd) {
  Reply r;
  try {
    for (const auto& b : piet::dispatch(cmd, session)) {
      if (!b.is_object()) continue;
      const std::string t = b.value("type", std::string());
      if (t == "error") {
        r.rc = piet::eval::coarseRc(b);
        r.msg += b.value("message", std::string());
      } else if (t == "estimation") {
        r.est = b;
      } else if (t == "display") {
        r.text += b.value("text", std::string());
      }
    }
  } catch (const std::exception& e) {
    r.rc = "EXC";
    r.msg = e.what();
  }
  return r;
}

std::string trimmed(const std::string& s) {
  const size_t a = s.find_first_not_of(" \n");
  if (a == std::string::npos) return std::string();
  const size_t z = s.find_last_not_of(" \n");
  return s.substr(a, z - a + 1);
}

// The log's `%21x` text ("+1.5555560000000X-002") as a double. Stata prints
// system missing as +1.0000000000000X+3ff; that is returned as NaN, the store's
// own missing. `ok` is false for text that is not a %21x value.
double fromHex21x(const std::string& h, bool* ok) {
  *ok = false;
  const size_t x = h.find('X');
  if (h.size() < 6 || x == std::string::npos || x + 2 > h.size()) return 0.0;
  const std::string mant = h.substr(0, x);   // "+1.5555560000000"
  const std::string expo = h.substr(x + 1);  // "-002" (hexadecimal)
  if (mant == "+1.0000000000000" && expo == "+3ff") {
    *ok = true;
    return std::nan("");
  }
  char* end = nullptr;
  const long e = std::strtol(expo.c_str(), &end, 16);
  if (end == expo.c_str()) return 0.0;
  const char sign = mant[0];
  const std::string c = std::string(sign == '-' ? "-" : "") + "0x" + mant.substr(1) + "p" +
                        std::to_string(e);
  const double v = std::strtod(c.c_str(), &end);
  if (end == c.c_str()) return 0.0;
  *ok = true;
  return v;
}

std::string fmt(double v) {
  if (std::isnan(v)) return ".";
  char buf[64];
  std::snprintf(buf, sizeof(buf), "%.17g", v);
  return buf;
}

// The stored value of VAR[i] (1-based), straight off the column.
bool storedValue(piet::SessionState& session, const std::string& var, long row, double* out) {
  piet::Datastore* ds = session.datastore();
  if (ds == nullptr || !ds->has(var) || ds->dtype(var) != "f64") return false;
  const cecon::Column& c = ds->col(var);
  const long n = static_cast<long>(c.nbytes / static_cast<int64_t>(sizeof(double)));
  if (row < 1 || row > n) return false;
  *out = c.f64()[row - 1];
  return true;
}

bool coefOf(const ojson& est, const std::string& name, const char* key, double* out) {
  if (!est.is_object() || !est.contains("coeftable") || !est["coeftable"].is_array()) return false;
  for (const auto& row : est["coeftable"]) {
    if (row.is_object() && row.value("name", std::string()) == name && row.contains(key) &&
        row[key].is_number()) {
      *out = row[key].get<double>();
      return true;
    }
  }
  return false;
}

// An e() value at full precision: through a scalar, then `%18.0g`.
double eNum(piet::SessionState& session, const std::string& expr) {
  (void)run(session, "scalar __w2ef_v = " + expr);
  const std::string text = trimmed(run(session, "display %18.0g __w2ef_v").text);
  if (text.empty() || text == ".") return std::nan("");
  char* end = nullptr;
  const double v = std::strtod(text.c_str(), &end);
  return end == text.c_str() ? std::nan("") : v;
}

bool relClose(double got, double want) {  // TOLERANCE.md T1/T2: rtol 1e-9
  return std::isfinite(got) && std::isfinite(want) &&
         std::fabs(got - want) <= 1e-9 * std::fmax(1.0, std::fabs(want));
}

std::vector<std::string> splitBar(const std::string& s) {
  std::vector<std::string> out;
  std::string cur;
  for (char ch : s) {
    if (ch == '|') {
      out.push_back(cur);
      cur.clear();
    } else {
      cur += ch;
    }
  }
  return out;  // the record ends in '|', so nothing trails
}

struct Step {
  bool rcBearing;       // the do-file reads `_rc` right after this command
  const char* cmd;      // nullptr = an arm record
  const char* arm;
  int logLine;
  std::vector<const char*> fields;  // the PREC display's items after the arm id
  const char* want;                  // the log's fields after the arm id
};

// FU-141: `display %21x <scalar>` prints the scalar's defining text.
// FU-1325 (varsub-assign-from-variable-bug710-v2, BUG-710): fn_round_lit_double,
// fn_expr_reads_float and fn_expr_reads_double left this list -- `scalar =`
// stores the value now, and checkField reads the STORED scalar, not display's
// rendering of it. The four float() arms stay FU-225's float() half.
const std::set<std::string>& owedToFu141() {
  static const std::set<std::string> s = {"fn_float_13", "fn_float_1e300", "fn_float_big",
                                          "fn_float_neg"};
  return s;
}

// One field of a PREC record, compared at its tier. Returns "" when it matches,
// else what was seen.
std::string checkField(piet::SessionState& session, const std::string& field,
                       const std::string& want, const std::string& rc, const Reply& last,
                       bool doubleColumn) {
  if (field == "`__rc'") return rc == want ? "" : "rc " + rc;
  if (field == "\"\"") return want.empty() ? "" : "(empty)";
  if (field == "\"`__t0'\"" || field == "\"`__ct'\"") {
    const std::string name = field == "\"`__t0'\"" ? "__t0" : "__ct";
    const std::string got = session.eval.locals.get(name, "");
    return got == want ? "" : name + "=" + got;
  }
  if (field.rfind("%21x ", 0) == 0) {
    const std::string ex = field.substr(5);
    bool ok = false;
    const double w = fromHex21x(want, &ok);
    if (!ok) return "unparsed oracle " + want;
    if (ex.rfind("_b[", 0) == 0 || ex.rfind("_se[", 0) == 0) {
      const bool isB = ex[1] == 'b';
      const std::string name = ex.substr(isB ? 3 : 4, ex.size() - (isB ? 4 : 5));
      double got = 0.0;
      if (!coefOf(last.est, name, isB ? "b" : "se", &got)) return "no coefficient " + name;
      return relClose(got, w) ? "" : ex + "=" + fmt(got);
    }
    if (ex.rfind("e(", 0) == 0) {
      const double got = eNum(session, ex);
      return relClose(got, w) ? "" : ex + "=" + fmt(got);
    }
    // `%21x NAME` of a scalar (fn_round_lit_double, fn_expr_reads_*): the STORED
    // value, which must be a number -- main stored the defining text `d13[1]*3`
    // and only display's re-evaluation of it printed the right bits (FU-1324).
    if (const std::string* sv = session.eval.scalars.find(ex)) {
      char* end = nullptr;
      const double got = std::strtod(sv->c_str(), &end);
      if (sv->empty() || end != sv->c_str() + sv->size()) return "scalar " + ex + " holds `" + *sv + "`";
      return relClose(got, w) ? "" : ex + "=" + fmt(got);
    }
    const size_t lb = ex.find('[');
    if (lb == std::string::npos) return "not a column read: " + ex;
    const std::string var = ex.substr(0, lb);
    const long row = std::strtol(ex.c_str() + lb + 1, nullptr, 10);
    double got = 0.0;
    if (!storedValue(session, var, row, &got)) return "no stored " + ex;
    if (std::isnan(w)) return std::isnan(got) ? "" : ex + "=" + fmt(got);
    if (doubleColumn) return relClose(got, w) ? "" : ex + "=" + fmt(got);
    return got == w ? "" : ex + "=" + fmt(got) + " want " + fmt(w);  // the float itself
  }
  // missing(VAR[i]): the flag of the STORED value. `display missing(v[i])` is
  // r(198) in PIET's display layer on main as well (a subscript inside a
  // function call; filed as a follow-up), and the claim here is the storage.
  if (field.rfind("missing(", 0) == 0 && field.find('[') != std::string::npos) {
    const size_t lb = field.find('[');
    const std::string var = field.substr(8, lb - 8);
    const long row = std::strtol(field.c_str() + lb + 1, nullptr, 10);
    double got = 0.0;
    if (!storedValue(session, var, row, &got)) return "no stored " + field;
    const std::string flag = std::isnan(got) ? "1" : "0";
    return flag == want ? "" : field + "=" + flag;
  }
  // r(N), e(N), missing(scalar), (sfm == .a): the value `display` prints.
  const std::string got = trimmed(run(session, "display " + field).text);
  return got == want ? "" : field + "=" + got;
}

// Replays a Step table in one session: commands run in order, each arm record
// is compared field by field at its tier. Returns the number of arms compared.
int replaySteps(piet::SessionState& s, const std::vector<Step>& steps) {
  std::string rc = "0";
  Reply last;
  int armsRun = 0;
  for (const Step& st : steps) {
    if (st.cmd != nullptr) {
      Reply r = run(s, st.cmd);
      if (st.rcBearing) rc = r.rc;
      if (r.est.is_object()) last = r;
      continue;
    }
    const std::string arm = st.arm;
    const std::string where = std::string("w2e_float_default.log:") + std::to_string(st.logLine);
    if (owedToFu141().count(arm)) {
      w2skip("W2E float " + arm + " (" + where + ")",
             "FU-141: `display %21x <scalar>` prints the scalar's text, not its value");
      continue;
    }
    ++armsRun;
    const std::vector<std::string> want = splitBar(st.want);
    const bool doubleColumn = std::string(st.want).find("|double|") != std::string::npos;
    std::string seen;
    if (want.size() != st.fields.size()) {
      seen = "field count " + std::to_string(st.fields.size()) + " vs " +
             std::to_string(want.size());
    } else {
      for (size_t i = 0; i < want.size(); ++i) {
        const std::string bad = checkField(s, st.fields[i], want[i], rc, last, doubleColumn);
        if (!bad.empty()) seen += (seen.empty() ? "" : "; ") + bad;
      }
    }
    w2check(seen.empty(), "W2E float " + arm + " (" + where + "): " + st.want +
                              (seen.empty() ? std::string() : " -- got " + seen));
  }
  return armsRun;
}

// Family (5), collapse result types (BUG-960, BUG-961): w2e_float_default.do:696-781
// with the log's records (:1370-:1523). mkfix (do:34-41) is inlined.
#define W2EF_MKFIX                                                                        \
  {false, "clear", nullptr, 0, {}, nullptr},                                              \
      {false, "set obs 12", nullptr, 0, {}, nullptr},                                     \
      {false, "generate byte g = mod(_n, 4)", nullptr, 0, {}, nullptr},                   \
      {false, "generate double d13 = 1/3", nullptr, 0, {}, nullptr},                      \
      {false, "generate double src = _n/7", nullptr, 0, {}, nullptr},                     \
      {false, "generate double src2 = _n/11", nullptr, 0, {}, nullptr}
const std::vector<Step>& collapseSteps() {
  static const std::vector<Step> k = {
    {false, "set type float", nullptr, 0, {}, nullptr},
    W2EF_MKFIX,
    {false, "generate src_u = _n/7", nullptr, 0, {}, nullptr},
    {false, "generate byte bb = mod(_n, 3)", nullptr, 0, {}, nullptr},
    {true, "collapse (mean) src src_u bb, by(g)", nullptr, 0, {}, nullptr},
    {false, nullptr, "collapse_mean_rc", 1370, {"`__rc'", "\"\""}, "0||"},
    {false, "local __t0 : type src", nullptr, 0, {}, nullptr},
    {false, nullptr, "collapse_mean_double_src", 1380, {"`__rc'", "%21x src[1]", "\"`__t0'\""}, "0|+1.2492492492492X+000|double|"},
    {false, "local __t0 : type src_u", nullptr, 0, {}, nullptr},
    {false, nullptr, "collapse_mean_float_src", 1390, {"`__rc'", "%21x src_u[1]", "\"`__t0'\""}, "0|+1.24924a0000000X+000|float|"},
    {false, "local __t0 : type bb", nullptr, 0, {}, nullptr},
    {false, nullptr, "collapse_mean_byte_src", 1400, {"`__rc'", "%21x bb[1]", "\"`__t0'\""}, "0|+1.0000000000000X+000|float|"},
    {false, "local __t0 : type g", nullptr, 0, {}, nullptr},
    {false, nullptr, "collapse_bykey_type", 1410, {"`__rc'", "%21x g[1]", "\"`__t0'\""}, "0|+0.0000000000000X-3ff|byte|"},
    W2EF_MKFIX,
    {false, "generate byte bb = mod(_n, 3)", nullptr, 0, {}, nullptr},
    {true, "collapse (sum) s2 = src2 bsum = bb, by(g)", nullptr, 0, {}, nullptr},
    {false, nullptr, "collapse_sum_rc", 1425, {"`__rc'", "\"\""}, "0||"},
    {false, "local __t0 : type s2", nullptr, 0, {}, nullptr},
    {false, nullptr, "collapse_sum_double_src", 1435, {"`__rc'", "%21x s2[1]", "\"`__t0'\""}, "0|+1.1745d1745d174X+001|double|"},
    {false, "local __t0 : type bsum", nullptr, 0, {}, nullptr},
    {false, nullptr, "collapse_sum_byte_src", 1445, {"`__rc'", "%21x bsum[1]", "\"`__t0'\""}, "0|+1.8000000000000X+001|double|"},
    W2EF_MKFIX,
    {true, "collapse (count) c = src, by(g)", nullptr, 0, {}, nullptr},
    {false, nullptr, "collapse_count_rc", 1458, {"`__rc'", "\"\""}, "0||"},
    {false, "local __t0 : type c", nullptr, 0, {}, nullptr},
    {false, nullptr, "collapse_count_type", 1468, {"`__rc'", "%21x c[1]", "\"`__t0'\""}, "0|+1.8000000000000X+001|long|"},
    W2EF_MKFIX,
    {false, "generate byte bb = mod(_n, 3)", nullptr, 0, {}, nullptr},
    {true, "collapse (max) mx = src bmx = bb, by(g)", nullptr, 0, {}, nullptr},
    {false, nullptr, "collapse_max_rc", 1483, {"`__rc'", "\"\""}, "0||"},
    {false, "local __t0 : type mx", nullptr, 0, {}, nullptr},
    {false, nullptr, "collapse_max_double_src", 1493, {"`__rc'", "%21x mx[1]", "\"`__t0'\""}, "0|+1.b6db6db6db6dbX+000|double|"},
    {false, "local __t0 : type bmx", nullptr, 0, {}, nullptr},
    {false, nullptr, "collapse_max_byte_src", 1503, {"`__rc'", "%21x bmx[1]", "\"`__t0'\""}, "0|+1.0000000000000X+001|byte|"},
    W2EF_MKFIX,
    {false, "set type double", nullptr, 0, {}, nullptr},
    {true, "collapse (mean) src, by(g)", nullptr, 0, {}, nullptr},
    {false, "local __t0 : type src", nullptr, 0, {}, nullptr},
    {false, nullptr, "collapse_mean_settype_double", 1523, {"`__rc'", "%21x src[1]", "\"`__t0'\""}, "0|+1.2492492492491X+000|double|"},
    {false, "set type float", nullptr, 0, {}, nullptr},
  };
  return k;
}
#undef W2EF_MKFIX

// One collapse on a fresh fixture: rc, message, _N, and every listed variable's
// `: type` plus its stored values as exact hex ("absent" for a missing one).
struct CollapseRun {
  std::string rc;
  std::string msg;
  long long n = -1;
  std::string cells;
};

const std::vector<const char*>& collapseFixture() {
  static const std::vector<const char*> k = {
      "clear", "set type float", "set obs 12", "generate byte g = mod(_n, 4)",
      "generate double src = _n/7", "generate double src2 = _n/11", "generate src_u = _n/7",
      "generate byte bb = mod(_n, 3)", "generate byte w = mod(_n, 3) + 1",
      "replace src_u = . in 6", "replace src2 = . in 3"};
  return k;
}

CollapseRun collapseOn(const std::string& workdir, const std::string& cmd,
                       const std::vector<std::string>& vars) {
  piet::SessionState s(workdir);
  for (const char* c : collapseFixture()) (void)run(s, c);
  const Reply r = run(s, cmd);
  CollapseRun out;
  out.rc = r.rc;
  out.msg = r.msg;
  piet::Datastore* ds = s.datastore();
  out.n = ds != nullptr ? static_cast<long long>(ds->nRows()) : -1;
  for (const std::string& v : vars) {
    (void)run(s, "local __w2efc_t \"absent\"");
    (void)run(s, "capture local __w2efc_t : type " + v);
    out.cells += v + ":" + s.eval.locals.get("__w2efc_t", "") + "[";
    if (ds != nullptr && ds->has(v) && ds->dtype(v) == "f64") {
      const cecon::Column& c = ds->col(v);
      const long n = static_cast<long>(c.nbytes / static_cast<int64_t>(sizeof(double)));
      for (long i = 0; i < n; ++i) {
        char buf[64];
        std::snprintf(buf, sizeof(buf), "%a ", c.f64()[i]);
        out.cells += buf;
      }
    }
    out.cells += "] ";
  }
  return out;
}

// BUG-960/961/962: the target spellings, the float re-store, `cw`, the option
// comma and the named refusals. Every arm fails on bin/main 29df46e9 (measured
// with this file over the base handler: 26 FAIL).
void testW2eFloatCollapse(const std::string& workdir) {
  // Every spelling of collapse.ado's `name[ ]=[ ]<varname>` gives the unspaced
  // form's values and types (w2e_float_default.log:1425-1503 records the spaced
  // form at rc 0), with several targets in one block, targets beside plain
  // variables, every statistic family, by(), [fw=], if, in, cw and no by().
  struct Pair {
    const char* spaced;
    const char* unspaced;
    std::vector<std::string> vars;
  };
  const std::vector<Pair> pairs = {
      {"collapse (mean) mu = src_u (sum) s= src2 sb =bb (max) mx = src bb (count) c = src, by(g)",
       "collapse (mean) mu=src_u (sum) s=src2 sb=bb (max) mx=src bb (count) c=src, by(g)",
       {"g", "mu", "s", "sb", "mx", "bb", "c"}},
      {"collapse (mean) src_u (sum) s = src2 (count) c = src [fw=w], by(g)",
       "collapse (mean) src_u (sum) s=src2 (count) c=src [fw=w], by(g)", {"g", "src_u", "s", "c"}},
      {"collapse (p50) m = src_u if g < 3, by(g)", "collapse (p50) m=src_u if g < 3, by(g)",
       {"g", "m"}},
      {"collapse (sd) sd = src_u (first) f = src2 in 2/11, by(g)",
       "collapse (sd) sd=src_u (first) f=src2 in 2/11, by(g)", {"g", "sd", "f"}},
      {"collapse (mean) src_u (last) l = src2, by(g) cw",
       "collapse (mean) src_u (last) l=src2, by(g) cw", {"g", "src_u", "l"}},
      {"collapse (min) lo = src_u (max) hi =src_u (median) md= src",
       "collapse (min) lo=src_u (max) hi=src_u (median) md=src", {"lo", "hi", "md"}},
  };
  for (const Pair& p : pairs) {
    const CollapseRun a = collapseOn(workdir, p.spaced, p.vars);
    const CollapseRun b = collapseOn(workdir, p.unspaced, p.vars);
    w2check(a.rc == "0" && b.rc == "0" && a.n == b.n && a.cells == b.cells &&
                a.cells.find("absent") == std::string::npos,
            std::string("W2E float collapse target spelling (BUG-960; collapse.ado grammar, "
                        "w2e_float_default.log:1425): `") + p.spaced +
                "' == its unspaced form -- got rc " + a.rc + " " + a.msg + " / rc " + b.rc + " " +
                b.msg + (a.cells == b.cells ? std::string() : "; " + a.cells + " vs " + b.cells));
  }

  // The float re-store (BUG-961; collapse_mean_float_src
  // :1390): the mean of a float source is float(mean of the stored floats), not
  // the engine's double. g==0 holds rows 4, 8 and 12.
  {
    piet::SessionState s(workdir);
    for (const char* c : {"clear", "set type float", "set obs 12", "generate byte g = mod(_n, 4)",
                          "generate src_u = _n/7"})
      (void)run(s, c);
    const Reply r = run(s, "collapse (mean) m = src_u, by(g)");
    const double dmean = (static_cast<double>(static_cast<float>(4.0 / 7.0)) +
                          static_cast<double>(static_cast<float>(8.0 / 7.0)) +
                          static_cast<double>(static_cast<float>(12.0 / 7.0))) / 3.0;
    const double want = static_cast<double>(static_cast<float>(dmean));
    double got = 0.0;
    const bool have = storedValue(s, "m", 1, &got);
    (void)run(s, "local __t0 : type m");
    w2check(r.rc == "0" && have && got == want && want != dmean &&
                s.eval.locals.get("__t0", "") == "float",
            "W2E float collapse float re-store (BUG-961, collapse_mean_float_src :1390's rule): "
            "`(mean) m = src_u` stores float(" + fmt(dmean) + ") = " + fmt(want) + ", type float "
            "-- got rc " + r.rc + " " + fmt(have ? got : std::nan("")) + " " +
                s.eval.locals.get("__t0", ""));
  }

  // `cw` (BUG-962; collapse drops rows with any missing source, UNPINNED by a
  // capture): g==3 holds rows 3, 7, 11 and src2 is missing in row 3, so the
  // src_u mean runs over rows 7 and 11 only (9/7; all three rows give 1).
  {
    const CollapseRun c = collapseOn(workdir, "collapse (mean) src_u src2, by(g) cw", {"src_u"});
    piet::SessionState s(workdir);
    for (const char* cmd : collapseFixture()) (void)run(s, cmd);
    (void)run(s, "collapse (mean) src_u src2, by(g) cw");
    const double want = static_cast<double>(static_cast<float>(
        (static_cast<double>(static_cast<float>(7.0 / 7.0)) +
         static_cast<double>(static_cast<float>(11.0 / 7.0))) / 2.0));
    double got = 0.0;
    const bool have = storedValue(s, "src_u", 4, &got);
    w2check(c.rc == "0" && have && got == want,
            "W2E float collapse cw (BUG-962, UNPINNED): src_u[4] is the "
            "mean over the rows where every source is non-missing, " + fmt(want) + " -- got rc " +
                c.rc + " " + fmt(have ? got : std::nan("")));
  }

  // The option comma (BUG-962): `, cw` without by() and `, fast by(g)` no longer
  // leak into the stat-varlist ("no column: x,", rc 1 on main).
  {
    const CollapseRun a = collapseOn(workdir, "collapse (mean) m = src_u, cw", {"m"});
    const CollapseRun b = collapseOn(workdir, "collapse (mean) src, fast by(g)", {"src"});
    w2check(a.rc == "0" && a.n == 1 && b.rc == "0" && b.n == 4,
            "W2E float collapse option comma (BUG-962): `, cw` -> 1 row, `, fast by(g)` -> 4 rows "
            "-- got rc " + a.rc + " " + a.msg + " N " + std::to_string(a.n) + ", rc " + b.rc + " " +
                b.msg + " N " + std::to_string(b.n));
  }

  // A plain wildcard token is expanded before the engine (BUG-960; "no column:
  // src*", rc 1 on main): `src*` is src src2 src_u in dataset order.
  {
    const std::vector<std::string> v = {"g", "src", "src2", "src_u"};
    const CollapseRun a = collapseOn(workdir, "collapse (mean) src*, by(g)", v);
    const CollapseRun b = collapseOn(workdir, "collapse (mean) src src2 src_u, by(g)", v);
    w2check(a.rc == "0" && a.cells == b.cells,
            "W2E float collapse wildcard (BUG-960): `(mean) src*` == `(mean) src src2 src_u` -- "
            "got rc " + a.rc + " " + a.msg);
  }

  // Malformed targets are named refusals at r(198), never rc 1 and never the
  // engine's text; the data stay as they were. Stata: a duplicate
  // result is "name conflict" exit 198, a by() variable as a result
  // exit 198. `= old` and `new =` are UNPINNED by a capture (r(198)
  // by the task's rule).
  for (const char* bad : {"collapse (mean) = src, by(g)", "collapse (mean) a =, by(g)",
                          "collapse (mean) a = src a = src2, by(g)",
                          "collapse (mean) src (sum) src, by(g)", "collapse (mean) g = src, by(g)",
                          "collapse (mean) a = (sum) src, by(g)"}) {
    const CollapseRun r = collapseOn(workdir, bad, {});
    w2check(r.rc == "198" && r.n == 12 && r.msg.find("no column") == std::string::npos &&
                r.msg.rfind("collapse: ", 0) == 0,
            std::string("W2E float collapse refusal (BUG-960): `") + bad +
                "' is a named r(198), data kept -- got rc " + r.rc + " N " + std::to_string(r.n) +
                " `" + r.msg + "'");
  }

  // No observation left after the qualifiers is "no observations" r(2000) with
  // the data unchanged (UNPINNED by a capture). The
  // float re-store must never see the empty result.
  for (const char* none : {"collapse (mean) m = src_u if g > 10, by(g)",
                           "collapse (mean) m=src_u if g > 10, by(g)",
                           "collapse (mean) src_u src2 in 3/3, by(g) cw"}) {
    const CollapseRun r = collapseOn(workdir, none, {});
    w2check(r.rc == "2000" && r.n == 12,
            std::string("W2E float collapse no observations (BUG-962): `") +
                none + "' is r(2000), data kept -- got rc " + r.rc + " N " + std::to_string(r.n) +
                " `" + r.msg + "'");
  }

  // Under `preserve` a collapse with float results and new targets keeps a card
  // that names the collapsed variables (af_collapse, w2af_format_lifecycle.log:
  // the float re-store's card must fold onto collapse's card, not the
  // pre-collapse one), and restore brings the 12 rows back.
  {
    piet::SessionState s(workdir);
    for (const char* c : collapseFixture()) (void)run(s, c);
    (void)run(s, "preserve");
    const Reply r = run(s, "collapse (mean) src_u (sum) sm = bb (count) cn = bb, by(g)");
    std::string types;
    for (const char* v : {"src_u", "sm", "cn"}) {
      (void)run(s, "local __w2efc_t \"absent\"");
      (void)run(s, std::string("capture local __w2efc_t : type ") + v);
      types += s.eval.locals.get("__w2efc_t", "") + "|";
    }
    (void)run(s, "restore");
    piet::Datastore* ds = s.datastore();
    const long long n = ds != nullptr ? static_cast<long long>(ds->nRows()) : -1;
    w2check(r.rc == "0" && types == "float|double|long|" && n == 12,
            "W2E float collapse under preserve (af_collapse, BUG-961 card fold): types float|double|long|, "
            "restore -> 12 rows -- got rc " + r.rc + " " + types + " N " + std::to_string(n));
  }
}

}  // namespace

void testW2eFloat() {
  std::printf("[selftest] --- W2E float default: storage types (w2e_float_default.log) ---\n");
  std::printf("[selftest]        resolved cobra bin: %s\n", piet::resolveCobraBin().c_str());
  namespace fs = std::filesystem;
  const std::string workdir =
      (fs::temp_directory_path() / "piet_native_selftest_w2efloat").u8string();
  std::error_code ec;
  fs::remove_all(workdir, ec);
  fs::create_directories(workdir, ec);

  // OLDER-BINARY GUARD (DIAGNOSIS E.1 item 7): `cobra eval` ignores a flag it
  // does not know, so a PIET paired with a pre-T1 cobra would store the full
  // double here and every float arm below would fail for a reason that is not
  // PIET's. This arm names that reason first. gen_untyped_13 (:96).
  {
    piet::SessionState s(workdir);
    (void)run(s, "clear");
    (void)run(s, "set obs 1");
    (void)run(s, "generate g = 1/3");
    double got = 0.0;
    const bool have = storedValue(s, "g", 1, &got);
    w2check(have && got == static_cast<double>(static_cast<float>(1.0 / 3.0)),
            "W2E float GUARD (w2e_float_default.log:96): `generate g = 1/3` stores float(1/3) "
            "through the resolved cobra (a cobra that ignores --store-type stores the double) -- got " +
                fmt(have ? got : std::nan("")));
  }

  // The replay. One session, the do-file's order; each family opens with its
  // own `clear` (or mkfix's), so the six families run back to back.
  static const std::vector<Step> kSteps = {
    {false, "clear", nullptr, 0, {}, nullptr},
    {false, "set obs 5", nullptr, 0, {}, nullptr},
    {true, "generate g_u13 = 1/3", nullptr, 0, {}, nullptr},
    {false, "local __t0 : type g_u13", nullptr, 0, {}, nullptr},
    {false, nullptr, "gen_untyped_13", 96, {"`__rc'", "%21x g_u13[1]", "\"`__t0'\""}, "0|+1.5555560000000X-002|float|"},
    {true, "generate float g_f13 = 1/3", nullptr, 0, {}, nullptr},
    {false, "local __t0 : type g_f13", nullptr, 0, {}, nullptr},
    {false, nullptr, "gen_float_13", 110, {"`__rc'", "%21x g_f13[1]", "\"`__t0'\""}, "0|+1.5555560000000X-002|float|"},
    {true, "generate double g_d13 = 1/3", nullptr, 0, {}, nullptr},
    {false, "local __t0 : type g_d13", nullptr, 0, {}, nullptr},
    {false, nullptr, "gen_double_13", 124, {"`__rc'", "%21x g_d13[1]", "\"`__t0'\""}, "0|+1.5555555555555X-002|double|"},
    {true, "generate byte g_b13 = 1/3", nullptr, 0, {}, nullptr},
    {false, "local __t0 : type g_b13", nullptr, 0, {}, nullptr},
    {false, nullptr, "gen_byte_13", 138, {"`__rc'", "%21x g_b13[1]", "\"`__t0'\""}, "0|+0.0000000000000X-3ff|byte|"},
    {true, "generate int g_i13 = 1/3", nullptr, 0, {}, nullptr},
    {false, "local __t0 : type g_i13", nullptr, 0, {}, nullptr},
    {false, nullptr, "gen_int_13", 152, {"`__rc'", "%21x g_i13[1]", "\"`__t0'\""}, "0|+0.0000000000000X-3ff|int|"},
    {true, "generate long g_l13 = 1/3", nullptr, 0, {}, nullptr},
    {false, "local __t0 : type g_l13", nullptr, 0, {}, nullptr},
    {false, nullptr, "gen_long_13", 166, {"`__rc'", "%21x g_l13[1]", "\"`__t0'\""}, "0|+0.0000000000000X-3ff|long|"},
    {true, "generate g_u27 = 2.7", nullptr, 0, {}, nullptr},
    {false, "local __t0 : type g_u27", nullptr, 0, {}, nullptr},
    {false, nullptr, "gen_untyped_27", 180, {"`__rc'", "%21x g_u27[1]", "\"`__t0'\""}, "0|+1.59999a0000000X+001|float|"},
    {true, "generate float g_f27 = 2.7", nullptr, 0, {}, nullptr},
    {false, "local __t0 : type g_f27", nullptr, 0, {}, nullptr},
    {false, nullptr, "gen_float_27", 194, {"`__rc'", "%21x g_f27[1]", "\"`__t0'\""}, "0|+1.59999a0000000X+001|float|"},
    {true, "generate double g_d27 = 2.7", nullptr, 0, {}, nullptr},
    {false, "local __t0 : type g_d27", nullptr, 0, {}, nullptr},
    {false, nullptr, "gen_double_27", 208, {"`__rc'", "%21x g_d27[1]", "\"`__t0'\""}, "0|+1.599999999999aX+001|double|"},
    {true, "generate byte g_b27 = 2.7", nullptr, 0, {}, nullptr},
    {false, "local __t0 : type g_b27", nullptr, 0, {}, nullptr},
    {false, nullptr, "gen_byte_27", 222, {"`__rc'", "%21x g_b27[1]", "\"`__t0'\""}, "0|+1.0000000000000X+001|byte|"},
    {true, "generate int g_i27 = 2.7", nullptr, 0, {}, nullptr},
    {false, "local __t0 : type g_i27", nullptr, 0, {}, nullptr},
    {false, nullptr, "gen_int_27", 236, {"`__rc'", "%21x g_i27[1]", "\"`__t0'\""}, "0|+1.0000000000000X+001|int|"},
    {true, "generate long g_l27 = 2.7", nullptr, 0, {}, nullptr},
    {false, "local __t0 : type g_l27", nullptr, 0, {}, nullptr},
    {false, nullptr, "gen_long_27", 250, {"`__rc'", "%21x g_l27[1]", "\"`__t0'\""}, "0|+1.0000000000000X+001|long|"},
    {true, "generate g_um25 = -2.5", nullptr, 0, {}, nullptr},
    {false, "local __t0 : type g_um25", nullptr, 0, {}, nullptr},
    {false, nullptr, "gen_untyped_m25", 264, {"`__rc'", "%21x g_um25[1]", "\"`__t0'\""}, "0|-1.4000000000000X+001|float|"},
    {true, "generate float g_fm25 = -2.5", nullptr, 0, {}, nullptr},
    {false, "local __t0 : type g_fm25", nullptr, 0, {}, nullptr},
    {false, nullptr, "gen_float_m25", 278, {"`__rc'", "%21x g_fm25[1]", "\"`__t0'\""}, "0|-1.4000000000000X+001|float|"},
    {true, "generate double g_dm25 = -2.5", nullptr, 0, {}, nullptr},
    {false, "local __t0 : type g_dm25", nullptr, 0, {}, nullptr},
    {false, nullptr, "gen_double_m25", 292, {"`__rc'", "%21x g_dm25[1]", "\"`__t0'\""}, "0|-1.4000000000000X+001|double|"},
    {true, "generate byte g_bm25 = -2.5", nullptr, 0, {}, nullptr},
    {false, "local __t0 : type g_bm25", nullptr, 0, {}, nullptr},
    {false, nullptr, "gen_byte_m25", 306, {"`__rc'", "%21x g_bm25[1]", "\"`__t0'\""}, "0|-1.0000000000000X+001|byte|"},
    {true, "generate int g_im25 = -2.5", nullptr, 0, {}, nullptr},
    {false, "local __t0 : type g_im25", nullptr, 0, {}, nullptr},
    {false, nullptr, "gen_int_m25", 320, {"`__rc'", "%21x g_im25[1]", "\"`__t0'\""}, "0|-1.0000000000000X+001|int|"},
    {true, "generate long g_lm25 = -2.5", nullptr, 0, {}, nullptr},
    {false, "local __t0 : type g_lm25", nullptr, 0, {}, nullptr},
    {false, nullptr, "gen_long_m25", 334, {"`__rc'", "%21x g_lm25[1]", "\"`__t0'\""}, "0|-1.0000000000000X+001|long|"},
    {true, "generate g_u1e10 = 1e10", nullptr, 0, {}, nullptr},
    {false, "local __t0 : type g_u1e10", nullptr, 0, {}, nullptr},
    {false, nullptr, "gen_untyped_1e10", 348, {"`__rc'", "%21x g_u1e10[1]", "\"`__t0'\""}, "0|+1.2a05f20000000X+021|float|"},
    {true, "generate float g_f1e10 = 1e10", nullptr, 0, {}, nullptr},
    {false, "local __t0 : type g_f1e10", nullptr, 0, {}, nullptr},
    {false, nullptr, "gen_float_1e10", 362, {"`__rc'", "%21x g_f1e10[1]", "\"`__t0'\""}, "0|+1.2a05f20000000X+021|float|"},
    {true, "generate double g_d1e10 = 1e10", nullptr, 0, {}, nullptr},
    {false, "local __t0 : type g_d1e10", nullptr, 0, {}, nullptr},
    {false, nullptr, "gen_double_1e10", 376, {"`__rc'", "%21x g_d1e10[1]", "\"`__t0'\""}, "0|+1.2a05f20000000X+021|double|"},
    {true, "generate byte g_b1e10 = 1e10", nullptr, 0, {}, nullptr},
    {false, "local __t0 : type g_b1e10", nullptr, 0, {}, nullptr},
    {false, nullptr, "gen_byte_1e10", 390, {"`__rc'", "%21x g_b1e10[1]", "\"`__t0'\""}, "0|+1.0000000000000X+3ff|byte|"},
    {true, "generate int g_i1e10 = 1e10", nullptr, 0, {}, nullptr},
    {false, "local __t0 : type g_i1e10", nullptr, 0, {}, nullptr},
    {false, nullptr, "gen_int_1e10", 404, {"`__rc'", "%21x g_i1e10[1]", "\"`__t0'\""}, "0|+1.0000000000000X+3ff|int|"},
    {true, "generate long g_l1e10 = 1e10", nullptr, 0, {}, nullptr},
    {false, "local __t0 : type g_l1e10", nullptr, 0, {}, nullptr},
    {false, nullptr, "gen_long_1e10", 418, {"`__rc'", "%21x g_l1e10[1]", "\"`__t0'\""}, "0|+1.0000000000000X+3ff|long|"},
    {true, "generate g_u123456789 = 123456789", nullptr, 0, {}, nullptr},
    {false, "local __t0 : type g_u123456789", nullptr, 0, {}, nullptr},
    {false, nullptr, "gen_untyped_123456789", 432, {"`__rc'", "%21x g_u123456789[1]", "\"`__t0'\""}, "0|+1.d6f3460000000X+01a|float|"},
    {true, "generate float g_f123456789 = 123456789", nullptr, 0, {}, nullptr},
    {false, "local __t0 : type g_f123456789", nullptr, 0, {}, nullptr},
    {false, nullptr, "gen_float_123456789", 446, {"`__rc'", "%21x g_f123456789[1]", "\"`__t0'\""}, "0|+1.d6f3460000000X+01a|float|"},
    {true, "generate double g_d123456789 = 123456789", nullptr, 0, {}, nullptr},
    {false, "local __t0 : type g_d123456789", nullptr, 0, {}, nullptr},
    {false, nullptr, "gen_double_123456789", 460, {"`__rc'", "%21x g_d123456789[1]", "\"`__t0'\""}, "0|+1.d6f3454000000X+01a|double|"},
    {true, "generate byte g_b123456789 = 123456789", nullptr, 0, {}, nullptr},
    {false, "local __t0 : type g_b123456789", nullptr, 0, {}, nullptr},
    {false, nullptr, "gen_byte_123456789", 474, {"`__rc'", "%21x g_b123456789[1]", "\"`__t0'\""}, "0|+1.0000000000000X+3ff|byte|"},
    {true, "generate int g_i123456789 = 123456789", nullptr, 0, {}, nullptr},
    {false, "local __t0 : type g_i123456789", nullptr, 0, {}, nullptr},
    {false, nullptr, "gen_int_123456789", 488, {"`__rc'", "%21x g_i123456789[1]", "\"`__t0'\""}, "0|+1.0000000000000X+3ff|int|"},
    {true, "generate long g_l123456789 = 123456789", nullptr, 0, {}, nullptr},
    {false, "local __t0 : type g_l123456789", nullptr, 0, {}, nullptr},
    {false, nullptr, "gen_long_123456789", 502, {"`__rc'", "%21x g_l123456789[1]", "\"`__t0'\""}, "0|+1.d6f3454000000X+01a|long|"},
    {true, "generate g_u1e300 = 1e300", nullptr, 0, {}, nullptr},
    {false, "local __t0 : type g_u1e300", nullptr, 0, {}, nullptr},
    {false, nullptr, "gen_untyped_1e300", 516, {"`__rc'", "%21x g_u1e300[1]", "missing(g_u1e300[1])", "\"`__t0'\""}, "0|+1.0000000000000X+3ff|1|float|"},
    {true, "generate float g_f1e300 = 1e300", nullptr, 0, {}, nullptr},
    {false, "local __t0 : type g_f1e300", nullptr, 0, {}, nullptr},
    {false, nullptr, "gen_float_1e300", 530, {"`__rc'", "%21x g_f1e300[1]", "missing(g_f1e300[1])", "\"`__t0'\""}, "0|+1.0000000000000X+3ff|1|float|"},
    {true, "generate double g_d1e300 = 1e300", nullptr, 0, {}, nullptr},
    {false, "local __t0 : type g_d1e300", nullptr, 0, {}, nullptr},
    {false, nullptr, "gen_double_1e300", 544, {"`__rc'", "%21x g_d1e300[1]", "missing(g_d1e300[1])", "\"`__t0'\""}, "0|+1.7e43c8800759cX+3e4|0|double|"},
    {true, "generate g_up24 = 16777217", nullptr, 0, {}, nullptr},
    {false, "local __t0 : type g_up24", nullptr, 0, {}, nullptr},
    {false, nullptr, "gen_untyped_16777217", 558, {"`__rc'", "%21x g_up24[1]", "\"`__t0'\""}, "0|+1.0000000000000X+018|float|"},
    {true, "generate double g_dp24 = 16777217", nullptr, 0, {}, nullptr},
    {false, "local __t0 : type g_dp24", nullptr, 0, {}, nullptr},
    {false, nullptr, "gen_double_16777217", 572, {"`__rc'", "%21x g_dp24[1]", "\"`__t0'\""}, "0|+1.0000010000000X+018|double|"},
    {true, "generate long g_lp24 = 16777217", nullptr, 0, {}, nullptr},
    {false, "local __t0 : type g_lp24", nullptr, 0, {}, nullptr},
    {false, nullptr, "gen_long_16777217", 586, {"`__rc'", "%21x g_lp24[1]", "\"`__t0'\""}, "0|+1.0000010000000X+018|long|"},
    {true, "generate byte g_b300 = 300", nullptr, 0, {}, nullptr},
    {false, "local __t0 : type g_b300", nullptr, 0, {}, nullptr},
    {false, nullptr, "gen_byte_300", 601, {"`__rc'", "%21x g_b300[1]", "missing(g_b300[1])", "\"`__t0'\""}, "0|+1.0000000000000X+3ff|1|byte|"},
    {true, "generate int g_i40000 = 40000", nullptr, 0, {}, nullptr},
    {false, "local __t0 : type g_i40000", nullptr, 0, {}, nullptr},
    {false, nullptr, "gen_int_40000", 615, {"`__rc'", "%21x g_i40000[1]", "missing(g_i40000[1])", "\"`__t0'\""}, "0|+1.0000000000000X+3ff|1|int|"},
    {true, "generate long g_l3e9 = 3e9", nullptr, 0, {}, nullptr},
    {false, "local __t0 : type g_l3e9", nullptr, 0, {}, nullptr},
    {false, nullptr, "gen_long_3e9", 629, {"`__rc'", "%21x g_l3e9[1]", "missing(g_l3e9[1])", "\"`__t0'\""}, "0|+1.0000000000000X+3ff|1|long|"},
    {true, "count if g_u13 == g_f13", nullptr, 0, {}, nullptr},
    {false, nullptr, "gen_untyped_eq_float", 641, {"`__rc'", "r(N)"}, "0|5|"},
    {true, "count if g_u13 == g_d13", nullptr, 0, {}, nullptr},
    {false, nullptr, "gen_untyped_eq_double", 653, {"`__rc'", "r(N)"}, "0|0|"},
    {true, "generate g_uif = 1/3 if _n <= 2", nullptr, 0, {}, nullptr},
    {false, "local __t0 : type g_uif", nullptr, 0, {}, nullptr},
    {false, nullptr, "gen_untyped_if", 669, {"`__rc'", "%21x g_uif[1]", "missing(g_uif[5])", "\"`__t0'\""}, "0|+1.5555560000000X-002|1|float|"},
    {false, "generate byte grp = mod(_n, 2)", nullptr, 0, {}, nullptr},
    {true, "bysort grp: generate bg_u = _n/3", nullptr, 0, {}, nullptr},
    {false, "local __t0 : type bg_u", nullptr, 0, {}, nullptr},
    {false, nullptr, "bysort_gen_untyped", 686, {"`__rc'", "%21x bg_u[1]", "\"`__t0'\""}, "0|+1.5555560000000X-002|float|"},
    {true, "bysort grp: generate double bg_d = _n/3", nullptr, 0, {}, nullptr},
    {false, "local __t0 : type bg_d", nullptr, 0, {}, nullptr},
    {false, nullptr, "bysort_gen_double", 700, {"`__rc'", "%21x bg_d[1]", "\"`__t0'\""}, "0|+1.5555555555555X-002|double|"},
    {true, "bysort grp: generate bg_u2 = _n/3", nullptr, 0, {}, nullptr},
    {false, "local __t0 : type bg_u2", nullptr, 0, {}, nullptr},
    {false, nullptr, "bysort_gen_untyped_grp", 714, {"`__rc'", "%21x bg_u2[1]", "%21x bg_u2[2]", "\"`__t0'\""}, "0|+1.5555560000000X-002|+1.5555560000000X-001|float|"},
    {false, "clear", nullptr, 0, {}, nullptr},
    {false, "set obs 5", nullptr, 0, {}, nullptr},
    {false, "generate float rf = 1/3", nullptr, 0, {}, nullptr},
    {false, "generate double rd = 1/3", nullptr, 0, {}, nullptr},
    {false, "generate ru = 1/3", nullptr, 0, {}, nullptr},
    {false, "generate byte rb = 1", nullptr, 0, {}, nullptr},
    {false, "generate byte rb2 = 1", nullptr, 0, {}, nullptr},
    {false, "generate byte rb3 = 1", nullptr, 0, {}, nullptr},
    {false, "generate int ri = 1", nullptr, 0, {}, nullptr},
    {false, "generate long rl = 1", nullptr, 0, {}, nullptr},
    {false, "generate byte rbo = 1", nullptr, 0, {}, nullptr},
    {false, "generate int rio = 1", nullptr, 0, {}, nullptr},
    {true, "replace rf = 2/3", nullptr, 0, {}, nullptr},
    {false, "local __t0 : type rf", nullptr, 0, {}, nullptr},
    {false, nullptr, "replace_float_23", 761, {"`__rc'", "%21x rf[1]", "\"`__t0'\""}, "0|+1.5555560000000X-001|float|"},
    {true, "replace rd = 2/3", nullptr, 0, {}, nullptr},
    {false, "local __t0 : type rd", nullptr, 0, {}, nullptr},
    {false, nullptr, "replace_double_23", 777, {"`__rc'", "%21x rd[1]", "\"`__t0'\""}, "0|+1.5555555555555X-001|double|"},
    {true, "replace ru = 2/3", nullptr, 0, {}, nullptr},
    {false, "local __t0 : type ru", nullptr, 0, {}, nullptr},
    {false, nullptr, "replace_untyped_23", 793, {"`__rc'", "%21x ru[1]", "\"`__t0'\""}, "0|+1.5555560000000X-001|float|"},
    {true, "replace rb = 2.7", nullptr, 0, {}, nullptr},
    {false, "local __t0 : type rb", nullptr, 0, {}, nullptr},
    {false, nullptr, "replace_byte_promote", 810, {"`__rc'", "%21x rb[1]", "\"`__t0'\""}, "0|+1.59999a0000000X+001|float|"},
    {true, "replace rb2 = 2.7, nopromote", nullptr, 0, {}, nullptr},
    {false, "local __t0 : type rb2", nullptr, 0, {}, nullptr},
    {false, nullptr, "replace_byte_nopromote", 827, {"`__rc'", "%21x rb2[1]", "\"`__t0'\""}, "0|+1.0000000000000X+001|byte|"},
    {true, "replace ri = 1/3", nullptr, 0, {}, nullptr},
    {false, "local __t0 : type ri", nullptr, 0, {}, nullptr},
    {false, nullptr, "replace_int_frac", 843, {"`__rc'", "%21x ri[1]", "\"`__t0'\""}, "0|+1.5555560000000X-002|float|"},
    {true, "replace rl = 1/3", nullptr, 0, {}, nullptr},
    {false, "local __t0 : type rl", nullptr, 0, {}, nullptr},
    {false, nullptr, "replace_long_frac", 859, {"`__rc'", "%21x rl[1]", "\"`__t0'\""}, "0|+1.5555555555555X-002|double|"},
    {true, "replace rbo = 200", nullptr, 0, {}, nullptr},
    {false, "local __t0 : type rbo", nullptr, 0, {}, nullptr},
    {false, nullptr, "replace_byte_overflow", 876, {"`__rc'", "%21x rbo[1]", "\"`__t0'\""}, "0|+1.9000000000000X+007|int|"},
    {true, "replace rio = 40000", nullptr, 0, {}, nullptr},
    {false, "local __t0 : type rio", nullptr, 0, {}, nullptr},
    {false, nullptr, "replace_int_overflow", 893, {"`__rc'", "%21x rio[1]", "\"`__t0'\""}, "0|+1.3880000000000X+00f|long|"},
    {false, "set type double", nullptr, 0, {}, nullptr},
    {true, "replace rb3 = 2.7", nullptr, 0, {}, nullptr},
    {false, "local __t0 : type rb3", nullptr, 0, {}, nullptr},
    {false, nullptr, "replace_byte_promote_settype_double", 912, {"`__rc'", "%21x rb3[1]", "\"`__t0'\""}, "0|+1.59999a0000000X+001|float|"},
    {false, "set type float", nullptr, 0, {}, nullptr},
    {true, "replace rf = 1/7 if _n == 1", nullptr, 0, {}, nullptr},
    {false, "local __t0 : type rf", nullptr, 0, {}, nullptr},
    {false, nullptr, "replace_float_if", 930, {"`__rc'", "%21x rf[1]", "%21x rf[2]", "\"`__t0'\""}, "0|+1.24924a0000000X-003|+1.5555560000000X-001|float|"},
    {false, "clear", nullptr, 0, {}, nullptr},
    {false, "set obs 12", nullptr, 0, {}, nullptr},
    {false, "generate byte g = mod(_n, 4)", nullptr, 0, {}, nullptr},
    {false, "generate double d13 = 1/3", nullptr, 0, {}, nullptr},
    {false, "generate double src = _n/7", nullptr, 0, {}, nullptr},
    {false, "generate double src2 = _n/11", nullptr, 0, {}, nullptr},
    {true, "egen em_u = mean(src)", nullptr, 0, {}, nullptr},
    {false, "local __t0 : type em_u", nullptr, 0, {}, nullptr},
    {false, nullptr, "egen_mean_untyped", 953, {"`__rc'", "%21x em_u[1]", "\"`__t0'\""}, "0|+1.db6db60000000X-001|float|"},
    {true, "egen double em_d = mean(src)", nullptr, 0, {}, nullptr},
    {false, "local __t0 : type em_d", nullptr, 0, {}, nullptr},
    {false, nullptr, "egen_mean_double", 968, {"`__rc'", "%21x em_d[1]", "\"`__t0'\""}, "0|+1.db6db6db6db6dX-001|double|"},
    {true, "egen emb_u = mean(src), by(g)", nullptr, 0, {}, nullptr},
    {false, "local __t0 : type emb_u", nullptr, 0, {}, nullptr},
    {false, nullptr, "egen_mean_by_untyped", 982, {"`__rc'", "%21x emb_u[1]", "%21x emb_u[2]", "\"`__t0'\""}, "0|+1.6db6dc0000000X-001|+1.b6db6e0000000X-001|float|"},
    {true, "egen double emb_d = mean(src), by(g)", nullptr, 0, {}, nullptr},
    {false, "local __t0 : type emb_d", nullptr, 0, {}, nullptr},
    {false, nullptr, "egen_mean_by_double", 996, {"`__rc'", "%21x emb_d[1]", "%21x emb_d[2]", "\"`__t0'\""}, "0|+1.6db6db6db6db8X-001|+1.b6db6db6db6dbX-001|double|"},
    {true, "egen et_u = total(src)", nullptr, 0, {}, nullptr},
    {false, "local __t0 : type et_u", nullptr, 0, {}, nullptr},
    {false, nullptr, "egen_total_untyped", 1010, {"`__rc'", "%21x et_u[1]", "\"`__t0'\""}, "0|+1.64924a0000000X+003|float|"},
    {true, "egen double et_d = total(src)", nullptr, 0, {}, nullptr},
    {false, "local __t0 : type et_d", nullptr, 0, {}, nullptr},
    {false, nullptr, "egen_total_double", 1024, {"`__rc'", "%21x et_d[1]", "\"`__t0'\""}, "0|+1.6492492492492X+003|double|"},
    {true, "egen ert_u = rowtotal(src src2)", nullptr, 0, {}, nullptr},
    {false, "local __t0 : type ert_u", nullptr, 0, {}, nullptr},
    {false, nullptr, "egen_rowtotal_untyped", 1038, {"`__rc'", "%21x ert_u[1]", "\"`__t0'\""}, "0|+1.dec0d60000000X-003|float|"},
    {true, "egen double ert_d = rowtotal(src src2)", nullptr, 0, {}, nullptr},
    {false, "local __t0 : type ert_d", nullptr, 0, {}, nullptr},
    {false, nullptr, "egen_rowtotal_double", 1052, {"`__rc'", "%21x ert_d[1]", "\"`__t0'\""}, "0|+1.dec0d4c77b035X-003|double|"},
    {true, "egen erm_u = rowmean(src src2)", nullptr, 0, {}, nullptr},
    {false, "local __t0 : type erm_u", nullptr, 0, {}, nullptr},
    {false, nullptr, "egen_rowmean_untyped", 1066, {"`__rc'", "%21x erm_u[1]", "\"`__t0'\""}, "0|+1.dec0d40000000X-004|float|"},
    {true, "egen esd_u = sd(src)", nullptr, 0, {}, nullptr},
    {false, "local __t0 : type esd_u", nullptr, 0, {}, nullptr},
    {false, nullptr, "egen_sd_untyped", 1080, {"`__rc'", "%21x esd_u[1]", "\"`__t0'\""}, "0|+1.07b8680000000X-001|float|"},
    {true, "egen emx_u = max(src)", nullptr, 0, {}, nullptr},
    {false, "local __t0 : type emx_u", nullptr, 0, {}, nullptr},
    {false, nullptr, "egen_max_untyped", 1095, {"`__rc'", "%21x emx_u[1]", "\"`__t0'\""}, "0|+1.b6db6e0000000X+000|float|"},
    {true, "egen ec = count(src)", nullptr, 0, {}, nullptr},
    {false, "local __t0 : type ec", nullptr, 0, {}, nullptr},
    {false, nullptr, "egen_count_type", 1109, {"`__rc'", "%21x ec[1]", "\"`__t0'\""}, "0|+1.8000000000000X+003|float|"},
    {true, "egen etg = tag(g)", nullptr, 0, {}, nullptr},
    {false, "local __t0 : type etg", nullptr, 0, {}, nullptr},
    {false, nullptr, "egen_tag_type", 1123, {"`__rc'", "%21x etg[1]", "\"`__t0'\""}, "0|+1.0000000000000X+000|byte|"},
    {true, "egen esq = seq()", nullptr, 0, {}, nullptr},
    {false, "local __t0 : type esq", nullptr, 0, {}, nullptr},
    {false, nullptr, "egen_seq_type", 1137, {"`__rc'", "%21x esq[12]", "\"`__t0'\""}, "0|+1.8000000000000X+003|byte|"},
    {true, "egen erk = rank(src)", nullptr, 0, {}, nullptr},
    {false, "local __t0 : type erk", nullptr, 0, {}, nullptr},
    {false, nullptr, "egen_rank_type", 1151, {"`__rc'", "%21x erk[1]", "\"`__t0'\""}, "0|+1.0000000000000X+000|float|"},
    {true, "egen egr = group(g)", nullptr, 0, {}, nullptr},
    {false, "local __t0 : type egr", nullptr, 0, {}, nullptr},
    {false, nullptr, "egen_group_type", 1165, {"`__rc'", "%21x egr[1]", "\"`__t0'\""}, "0|+1.0000000000000X+001|float|"},
    {true, "egen ern = rownonmiss(src src2)", nullptr, 0, {}, nullptr},
    {false, "local __t0 : type ern", nullptr, 0, {}, nullptr},
    {false, nullptr, "egen_rownonmiss_type", 1179, {"`__rc'", "%21x ern[1]", "\"`__t0'\""}, "0|+1.0000000000000X+001|float|"},
    {false, "set type double", nullptr, 0, {}, nullptr},
    {true, "egen ems = mean(src)", nullptr, 0, {}, nullptr},
    {false, "local __t0 : type ems", nullptr, 0, {}, nullptr},
    {false, nullptr, "egen_mean_settype_double", 1196, {"`__rc'", "%21x ems[1]", "\"`__t0'\""}, "0|+1.db6db6db6db6dX-001|double|"},
    {false, "set type float", nullptr, 0, {}, nullptr},
    {false, "generate srcf = _n/7", nullptr, 0, {}, nullptr},
    {true, "egen emf_u = mean(srcf)", nullptr, 0, {}, nullptr},
    {false, "local __t0 : type emf_u", nullptr, 0, {}, nullptr},
    {false, nullptr, "egen_mean_float_src", 1215, {"`__rc'", "%21x emf_u[1]", "%21x srcf[1]", "\"`__t0'\""}, "0|+1.db6db80000000X-001|+1.24924a0000000X-003|float|"},
    {false, "clear", nullptr, 0, {}, nullptr},
    {false, "set obs 3", nullptr, 0, {}, nullptr},
    {true, "local __ct = c(type)", nullptr, 0, {}, nullptr},
    {false, nullptr, "settype_ctype_default", 1236, {"`__rc'", "\"`__ct'\""}, "0|float|"},
    {false, "set type double", nullptr, 0, {}, nullptr},
    {true, "generate st_d13 = 1/3", nullptr, 0, {}, nullptr},
    {false, "local __t0 : type st_d13", nullptr, 0, {}, nullptr},
    {false, nullptr, "settype_double_gen", 1252, {"`__rc'", "%21x st_d13[1]", "\"`__t0'\""}, "0|+1.5555555555555X-002|double|"},
    {true, "local __ct = c(type)", nullptr, 0, {}, nullptr},
    {false, nullptr, "settype_double_ctype", 1262, {"`__rc'", "\"`__ct'\""}, "0|double|"},
    {true, "generate st_dbig = 123456789", nullptr, 0, {}, nullptr},
    {false, "local __t0 : type st_dbig", nullptr, 0, {}, nullptr},
    {false, nullptr, "settype_double_gen_big", 1276, {"`__rc'", "%21x st_dbig[1]", "\"`__t0'\""}, "0|+1.d6f3454000000X+01a|double|"},
    {false, "set type float", nullptr, 0, {}, nullptr},
    {true, "generate st_f13 = 1/3", nullptr, 0, {}, nullptr},
    {false, "local __t0 : type st_f13", nullptr, 0, {}, nullptr},
    {false, nullptr, "settype_float_restore_gen", 1292, {"`__rc'", "%21x st_f13[1]", "\"`__t0'\""}, "0|+1.5555560000000X-002|float|"},
    {true, "local __ct = c(type)", nullptr, 0, {}, nullptr},
    {false, nullptr, "settype_float_ctype", 1302, {"`__rc'", "\"`__ct'\""}, "0|float|"},
    {true, "set type int", nullptr, 0, {}, nullptr},
    {false, nullptr, "settype_bad_rc", 1314, {"`__rc'", "\"\""}, "198||"},
    {true, "local __ct = c(type)", nullptr, 0, {}, nullptr},
    {false, nullptr, "settype_bad_ctype_unchanged", 1324, {"`__rc'", "\"`__ct'\""}, "0|float|"},
    {true, "set ty double", nullptr, 0, {}, nullptr},
    {false, nullptr, "settype_abbrev_ty", 1335, {"`__rc'", "\"\""}, "0||"},
    {true, "local __ct = c(type)", nullptr, 0, {}, nullptr},
    {false, nullptr, "settype_abbrev_ctype", 1345, {"`__rc'", "\"`__ct'\""}, "0|double|"},
    {false, "set type float", nullptr, 0, {}, nullptr},
    {false, "clear", nullptr, 0, {}, nullptr},
    {false, "set obs 5", nullptr, 0, {}, nullptr},
    {false, "generate u13 = 1/3", nullptr, 0, {}, nullptr},
    {false, "generate double d13 = 1/3", nullptr, 0, {}, nullptr},
    {true, "scalar sf13 = float(1/3)", nullptr, 0, {}, nullptr},
    {false, nullptr, "fn_float_13", 1773, {"`__rc'", "%21x sf13"}, "0|+1.5555560000000X-002|"},
    {true, "count if u13 == float(1/3)", nullptr, 0, {}, nullptr},
    {false, nullptr, "fn_float_eq_count", 1784, {"`__rc'", "r(N)"}, "0|5|"},
    {true, "count if u13 == 1/3", nullptr, 0, {}, nullptr},
    {false, nullptr, "fn_lit_eq_count", 1796, {"`__rc'", "r(N)"}, "0|0|"},
    {true, "count if d13 == 1/3", nullptr, 0, {}, nullptr},
    {false, nullptr, "fn_double_lit_eq_count", 1807, {"`__rc'", "r(N)"}, "0|5|"},
    {true, "scalar sfm = float(.a)", nullptr, 0, {}, nullptr},
    {false, nullptr, "fn_float_missing", 1818, {"`__rc'", "(sfm == .a)", "missing(sfm)"}, "0|1|1|"},
    {true, "scalar sfs = float(.)", nullptr, 0, {}, nullptr},
    {false, nullptr, "fn_float_sysmiss", 1828, {"`__rc'", "missing(sfs)"}, "0|1|"},
    {true, "scalar sfo = float(1e300)", nullptr, 0, {}, nullptr},
    {false, nullptr, "fn_float_1e300", 1839, {"`__rc'", "%21x sfo", "missing(sfo)"}, "0|+1.0000000000000X+3ff|1|"},
    {true, "scalar sfb = float(123456789)", nullptr, 0, {}, nullptr},
    {false, nullptr, "fn_float_big", 1849, {"`__rc'", "%21x sfb"}, "0|+1.d6f3460000000X+01a|"},
    {true, "scalar sfn = float(-2.7)", nullptr, 0, {}, nullptr},
    {false, nullptr, "fn_float_neg", 1859, {"`__rc'", "%21x sfn"}, "0|-1.59999a0000000X+001|"},
    {true, "generate r_u = round(u13, .01)", nullptr, 0, {}, nullptr},
    {false, "local __t0 : type r_u", nullptr, 0, {}, nullptr},
    {false, nullptr, "fn_round_untyped", 1874, {"`__rc'", "%21x r_u[1]", "\"`__t0'\""}, "0|+1.51eb860000000X-002|float|"},
    {true, "generate double r_d = round(d13, .01)", nullptr, 0, {}, nullptr},
    {false, "local __t0 : type r_d", nullptr, 0, {}, nullptr},
    {false, nullptr, "fn_round_double", 1889, {"`__rc'", "%21x r_d[1]", "\"`__t0'\""}, "0|+1.51eb851eb851fX-002|double|"},
    {true, "scalar sr = round(1/3, .01)", nullptr, 0, {}, nullptr},
    {false, nullptr, "fn_round_lit_double", 1899, {"`__rc'", "%21x sr"}, "0|+1.51eb851eb851fX-002|"},
    {true, "scalar se_u = u13[1]*3", nullptr, 0, {}, nullptr},
    {false, nullptr, "fn_expr_reads_float", 1910, {"`__rc'", "%21x se_u"}, "0|+1.0000008000000X+000|"},
    {true, "scalar se_d = d13[1]*3", nullptr, 0, {}, nullptr},
    {false, nullptr, "fn_expr_reads_double", 1921, {"`__rc'", "%21x se_d"}, "0|+1.0000000000000X+000|"},
    {true, "generate double fd = float(1/3)", nullptr, 0, {}, nullptr},
    {false, "local __t0 : type fd", nullptr, 0, {}, nullptr},
    {false, nullptr, "fn_float_then_double_gen", 1936, {"`__rc'", "%21x fd[1]", "\"`__t0'\""}, "0|+1.5555560000000X-002|double|"},
    {false, "clear", nullptr, 0, {}, nullptr},
    {false, "set obs 200", nullptr, 0, {}, nullptr},
    {false, "generate double y = mod(_n*7, 13) + _n/17", nullptr, 0, {}, nullptr},
    {false, "generate double z = 3 + 0.5*y + mod(_n*11, 5)/3", nullptr, 0, {}, nullptr},
    {false, "generate byte g = mod(_n, 4)", nullptr, 0, {}, nullptr},
    {false, "generate x = y/3", nullptr, 0, {}, nullptr},
    {false, "generate double xd = y/3", nullptr, 0, {}, nullptr},
    {true, "count if x == xd", nullptr, 0, {}, nullptr},
    {false, nullptr, "prop_untyped_differs", 2387, {"`__rc'", "r(N)"}, "0|6|"},
    {true, "regress z x", nullptr, 0, {}, nullptr},
    {false, nullptr, "prop_untyped", 2411, {"`__rc'", "%21x _b[x]", "%21x _se[x]", "e(N)", "%21x e(rmse)"}, "0|+1.7f585135be640X+000|+1.46aff8c051d22X-006|200|+1.e52140d64eaebX-002|"},
    {true, "regress z xd", nullptr, 0, {}, nullptr},
    {false, nullptr, "prop_double", 2436, {"`__rc'", "%21x _b[xd]", "%21x _se[xd]", "e(N)", "%21x e(rmse)"}, "0|+1.7f58511beea0eX+000|+1.46aff8fc47832X-006|200|+1.e521414be2775X-002|"},
    {false, "set type double", nullptr, 0, {}, nullptr},
    {false, "generate x2 = y/3", nullptr, 0, {}, nullptr},
    {true, "regress z x2", nullptr, 0, {}, nullptr},
    {false, nullptr, "prop_settype_double", 2465, {"`__rc'", "%21x _b[x2]", "%21x _se[x2]", "e(N)"}, "0|+1.7f58511beea0eX+000|+1.46aff8fc47832X-006|200|"},
    {false, "set type float", nullptr, 0, {}, nullptr},
    {false, "generate float xr = 0", nullptr, 0, {}, nullptr},
    {false, "replace xr = y/3", nullptr, 0, {}, nullptr},
    {true, "regress z xr", nullptr, 0, {}, nullptr},
    {false, nullptr, "prop_replace_into_float", 2497, {"`__rc'", "%21x _b[xr]", "%21x _se[xr]", "e(N)"}, "0|+1.7f585135be640X+000|+1.46aff8c051d22X-006|200|"},
    {false, "egen xm = mean(y), by(g)", nullptr, 0, {}, nullptr},
    {true, "regress z xm", nullptr, 0, {}, nullptr},
    {false, nullptr, "prop_egen_untyped", 2523, {"`__rc'", "%21x _b[xm]", "%21x _se[xm]", "e(N)"}, "0|+1.fffff1493ef36X-002|+1.642c642f9a21eX+000|200|"},
    {false, "egen double xmd = mean(y), by(g)", nullptr, 0, {}, nullptr},
    {true, "regress z xmd", nullptr, 0, {}, nullptr},
    {false, nullptr, "prop_egen_double", 2550, {"`__rc'", "%21x _b[xmd]", "%21x _se[xmd]", "e(N)"}, "0|+1.0000000000003X-001|+1.642c6e6bf04fdX+000|200|"},
    {true, "count if xm == xmd", nullptr, 0, {}, nullptr},
    {false, nullptr, "prop_egen_differs", 2561, {"`__rc'", "r(N)"}, "0|0|"},
    {false, "quietly regress z xd", nullptr, 0, {}, nullptr},
    {true, "predict p", nullptr, 0, {}, nullptr},
    {false, "local __t0 : type p", nullptr, 0, {}, nullptr},
    // BUG-924: the predicted VALUES are float, not only the label.
    {false, nullptr, "predict_default", 2579, {"`__rc'", "%21x p[1]", "\"`__t0'\""}, "0|+1.ccd0280000000X+002|float|"},
    {true, "predict double pd", nullptr, 0, {}, nullptr},
    {false, "local __t0 : type pd", nullptr, 0, {}, nullptr},
    {true, "count if p == pd", nullptr, 0, {}, nullptr},
    {false, nullptr, "predict_eq_count", 2606, {"`__rc'", "r(N)"}, "0|0|"},
    {true, "regress y p", nullptr, 0, {}, nullptr},
    {true, "regress y pd", nullptr, 0, {}, nullptr},
  };
  {
    piet::SessionState s(workdir);
    const int armsRun = replaySteps(s, kSteps);
    w2check(armsRun == 106, "W2E float: 106 oracle arms replayed (110 less the 4 FU-141 owes) -- ran " +
                                std::to_string(armsRun));
  }

  // Family (5), collapse (BUG-960, BUG-961): 14 records, the spaced target form
  // and the float re-store among them.
  {
    piet::SessionState s(workdir);
    const int armsRun = replaySteps(s, collapseSteps());
    w2check(armsRun == 14, "W2E float: 14 collapse oracle arms replayed (:1370-:1523) -- ran " +
                               std::to_string(armsRun));
  }
  testW2eFloatCollapse(workdir);

  // --- Siblings the log's fields cannot see, each resting on a log rule. ---

  // r() survives a promoting replace (TASKS.md T2 item 5 invariant): the
  // decision reads the touched values through a read-only engine eval, never
  // through dispatch() of count/summarize. A non-literal RHS forces that path.
  {
    piet::SessionState s(workdir);
    for (const char* c : {"clear", "set obs 5", "generate double h = _n/2", "generate byte b = 1"})
      (void)run(s, c);
    (void)run(s, "count if h > 1");
    const std::string before = trimmed(run(s, "display r(N)").text);
    const Reply rep = run(s, "replace b = h");
    const std::string after = trimmed(run(s, "display r(N)").text);
    (void)run(s, "local __t0 : type b");
    double b1 = 0.0;
    const bool have = storedValue(s, "b", 1, &b1);
    w2check(rep.rc == "0" && before == "3" && after == "3",
            "W2E float replace invariant: r(N) 3 before and after `replace b = h` (a promoting "
            "replace reads no r()) -- got rc " + rep.rc + ", " + before + " -> " + after);
    w2check(s.eval.locals.get("__t0", "") == "float" && have && b1 == 0.5,
            "W2E float replace promote (data pass; rule :810): byte b receiving h = _n/2 -> "
            "float, b[1] .5 -- got " + s.eval.locals.get("__t0", "") + " " + fmt(have ? b1 : std::nan("")));
    // An `if` keeps the untouched rows and decides from the touched ones only:
    // integral values into a byte stay byte (no widening, rule :827's type).
    (void)run(s, "generate byte c = 1");
    (void)run(s, "replace c = h if mod(_n, 2) == 0");
    (void)run(s, "local __t0 : type c");
    w2check(s.eval.locals.get("__t0", "") == "byte",
            "W2E float replace qualified: `replace c = h if mod(_n,2)==0` stores 1 and 2 -> byte "
            "stays byte -- got " + s.eval.locals.get("__t0", ""));
    // A widening literal under `if` reads the touched rows (the one literal shape
    // that is not decided from the literal alone): 2.5 into a byte -> float (:810).
    (void)run(s, "generate byte q = 1");
    (void)run(s, "replace q = 2.5 if h > 2");
    (void)run(s, "local __t0 : type q");
    double q5 = 0.0;
    const bool haveQ = storedValue(s, "q", 5, &q5);
    w2check(s.eval.locals.get("__t0", "") == "float" && haveQ && q5 == 2.5,
            "W2E float replace qualified literal (rule :810): `replace q = 2.5 if h > 2` into a "
            "byte -> float, q[5] 2.5 -- got " + s.eval.locals.get("__t0", "") + " " +
                fmt(haveQ ? q5 : std::nan("")));
    // An overflow through the by-prefix path widens like the plain one (:876).
    (void)run(s, "generate byte grp = mod(_n, 2)");
    (void)run(s, "generate byte o = 1");
    (void)run(s, "bysort grp: replace o = 100*_n + 100");
    (void)run(s, "local __t0 : type o");
    double o1 = 0.0;
    const bool haveO = storedValue(s, "o", 1, &o1);
    w2check(s.eval.locals.get("__t0", "") == "int" && haveO && o1 == 200.0,
            "W2E float bysort replace overflow (rule :876): byte receiving 200..400 -> int, "
            "values exact -- got " + s.eval.locals.get("__t0", "") + " " + fmt(haveO ? o1 : std::nan("")));
  }

  // Vocabulary (defect class 3): `nopromote` is replace's, `permanently` is set
  // type's; an unknown option is r(198), not ignored.
  {
    piet::SessionState s(workdir);
    for (const char* c : {"clear", "set obs 2", "generate byte b = 1"}) (void)run(s, c);
    const Reply bad = run(s, "replace b = 2, nosuchopt");
    w2check(bad.rc == "198", "W2E float replace vocabulary: an unknown option is r(198) -- got " +
                                 bad.rc);
    const Reply perm = run(s, "set type double, permanently");
    const std::string ct = trimmed(run(s, "display c(type)").text);
    w2check(perm.rc == "0" && ct == "double" && s.defaultStorageType == "double",
            "W2E float set type: `, permanently` is accepted and session-scoped -- got rc " +
                perm.rc + ", c(type) " + ct);
    (void)run(s, "clear");
    w2check(s.defaultStorageType == "double",
            "W2E float set type: `clear` keeps c(type) (it is a setting, not data) -- got " +
                s.defaultStorageType);
  }

  // c(type) in a `local =` (the macro/arith layer; settype_ctype_default :1236
  // reads it that way) and float() in the same layer (fn_float_eq_count :1784's
  // value, computed without the engine): float(1/3) is the float nearest 1/3.
  {
    piet::SessionState s(workdir);
    (void)run(s, "local ct = c(type)");
    (void)run(s, "local ok = (float(1/3) == 0.3333333432674408)");
    (void)run(s, "local okm = missing(float(.))");
    (void)run(s, "local okb = missing(float(1e300))");
    w2check(s.eval.locals.get("ct", "") == "float",
            "W2E float c(type) in `local =`: float -- got " + s.eval.locals.get("ct", ""));
    w2check(s.eval.locals.get("ok", "") == "1" && s.eval.locals.get("okm", "") == "1" &&
                s.eval.locals.get("okb", "") == "1",
            "W2E float float() macro layer: float(1/3) == 0.3333333432674408, missing in -> "
            "missing out, 1e300 -> . (fn_float_1e300's rule) -- got " + s.eval.locals.get("ok", "") +
                s.eval.locals.get("okm", "") + s.eval.locals.get("okb", ""));
  }

  // egen labels the log's arms do not print (DIAGNOSIS section B): mode takes
  // its SOURCE's type and value; a declared type does not reach
  // it. rownonmiss untyped is float (:1179, in the table above).
  {
    piet::SessionState s(workdir);
    for (const char* c : {"clear", "set obs 3", "generate double md = 1/3"}) (void)run(s, c);
    (void)run(s, "egen mo = mode(md)");
    (void)run(s, "local __t0 : type mo");
    double m1 = 0.0;
    const bool have = storedValue(s, "mo", 1, &m1);
    w2check(s.eval.locals.get("__t0", "") == "double" && have && m1 == 1.0 / 3.0,
            "W2E float egen mode: a double source gives a double mode, value kept -- got " +
                s.eval.locals.get("__t0", "") + " " + fmt(have ? m1 : std::nan("")));
    (void)run(s, "generate fs = 1/3");
    (void)run(s, "egen double mf = mode(fs)");
    (void)run(s, "local __t0 : type mf");
    w2check(s.eval.locals.get("__t0", "") == "float",
            "W2E float egen mode: `egen double mf = mode(fs)` with an untyped source is float "
            "(the source's type wins) -- got " + s.eval.locals.get("__t0", ""));
    // A replace-built egen under an integer type is refused by name, r(198),
    // never rc 1 and never retried without the type (defect class 2).
    const Reply rt = run(s, "egen byte rt = rowtotal(md fs)");
    w2check(rt.rc == "198",
            "W2E float egen rowtotal under byte: named refusal r(198) -- got " + rt.rc + " " + rt.msg);
  }

  // F2 (fix-up): float() of a finite NEGATIVE value past float's range is
  // missing, like the positive one (fn_float_1e300 :1839's rule). The sentinel
  // test is signed: only x >= the sentinel passes through as a missing value.
  // UNPINNED by a golden until the w2e_float_followups capture (section H of
  // that draft); the rule is the positive arm's, mirrored.
  // The overflow answer is the layer's own `.`, so it also COMPARES as missing:
  // `float(1e300) >= .` is 1 (fn_float_1e300 :1839 prints missing(sfo) 1); a
  // NaN printed `.` but compared false, and both displays answered 0.
  {
    piet::SessionState s(workdir);
    const Reply r = run(s, "display float(-1e308) >= .");
    w2check(r.rc == "0" && trimmed(r.text) == "1",
            "W2E float float() macro layer, UNPINNED (w2e_float_followups section H owed): "
            "`display float(-1e308) >= .` prints 1 -- got rc " + r.rc + " '" + trimmed(r.text) + "'");
    const Reply p = run(s, "display float(1e300) >= .");
    w2check(p.rc == "0" && trimmed(p.text) == "1",
            "W2E float float() macro layer (fn_float_1e300 :1839's rule): `display float(1e300) "
            ">= .` prints 1 -- got rc " + p.rc + " '" + trimmed(p.text) + "'");
  }

  // F3 (fix-up): egen seq() starts as byte and widens as the values grow
  // (as measured), so its label follows the values stored: byte up to
  // 100 (egen_seq_type :1137, in the table above), int past it, long past
  // 32740 (the replace lattice of :876/:893). The min/max come from the
  // computing call's own summary, so r() is untouched.
  {
    piet::SessionState s(workdir);
    for (const char* c : {"clear", "set obs 150", "generate byte g = mod(_n, 4)"}) (void)run(s, c);
    (void)run(s, "count if g == 1");
    const std::string before = trimmed(run(s, "display r(N)").text);
    (void)run(s, "egen sq = seq()");
    const std::string after = trimmed(run(s, "display r(N)").text);
    (void)run(s, "local __t0 : type sq");
    double sq150 = 0.0;
    const bool have = storedValue(s, "sq", 150, &sq150);
    w2check(s.eval.locals.get("__t0", "") == "int" && have && sq150 == 150.0,
            "W2E float egen seq widening (lattice :876): 150 rows -> int, "
            "sq[150] 150 -- got " + s.eval.locals.get("__t0", "") + " " +
                fmt(have ? sq150 : std::nan("")));
    w2check(before == "38" && after == "38",
            "W2E float egen seq: r(N) unchanged across the egen (no dispatch) -- got " + before +
                " -> " + after);
    (void)run(s, "egen sqg = seq(), by(g)");
    (void)run(s, "local __t0 : type sqg");
    w2check(s.eval.locals.get("__t0", "") == "byte",
            "W2E float egen seq by(g): groups of <= 38 rows stay byte (the stored max decides, "
            "not _N) -- got " + s.eval.locals.get("__t0", ""));
    (void)run(s, "egen sqn = seq(), from(-200) to(-1)");
    (void)run(s, "local __t0 : type sqn");
    w2check(s.eval.locals.get("__t0", "") == "int",
            "W2E float egen seq from(-200): values below byte's -127 widen to int -- got " +
                s.eval.locals.get("__t0", ""));
    for (const char* c : {"clear", "set obs 40000"}) (void)run(s, c);
    (void)run(s, "egen sql = seq()");
    (void)run(s, "local __t0 : type sql");
    w2check(s.eval.locals.get("__t0", "") == "long",
            "W2E float egen seq widening (lattice :893): 40000 rows -> long -- got " +
                s.eval.locals.get("__t0", ""));
  }

  // F1 (fix-up): c(type) reaches a parallel worker. A worker is a fresh
  // piet_native process (parallel.cpp runParallelWorker) that inherits the
  // parent's `set` state through the task spec; before the fix-up the spec
  // carried graphics/level/seed only, so after `set type double` a worker's
  // untyped generate stored float where the serial run stores double
  // (settype_double_gen :1252). The task is a program call (programs travel),
  // so the gen and its reads run in the same worker; the generated column
  // never comes back to the parent, which is how the arm knows it fanned out.
  // Both arms share buildTaskSpec/runParallelWorker with auto-approved regions
  // (doexec.cpp's runTasksInWorkers call).
  for (const bool asDouble : {true, false}) {
    piet::SessionState s(workdir);
    const std::string tag = asDouble ? "w2ef_pd" : "w2ef_pf";
    std::string src =
        "clear\n"
        "set obs 3\n"
        "generate byte k = _n\n"
        "program define " + tag + "\n"
        "generate v = 1/3\n"
        "quietly count if v == 1/3\n"
        "local a = r(N)\n"
        "quietly count if v == float(1/3)\n"
        "local t : type v\n"
        "display \"" + tag + " t=`t' a=`a' f=\" r(N)\n"
        "end\n" +
        std::string(asDouble ? "set type double\n" : "") +
        "parallel, workers(2) {\n" + tag + "\n"
        "display \"" + tag + "_other\"\n"
        "}\n";
    auto r = piet::doexec::executeDoSource(src, s, piet::dispatch);
    std::string line;
    for (const auto& b : r.blocks) {
      if (!b.is_object() || b.value("type", std::string()) != "display") continue;
      const std::string t = b.value("text", std::string());
      if (t.rfind(tag + " t=", 0) == 0) line = trimmed(t);
    }
    piet::Datastore* ds = s.datastore();
    const bool fannedOut = ds != nullptr && !ds->has("v");
    const std::string label = std::string("W2E float parallel c(type) hand-off (") +
                              (asDouble ? "set type double; settype_double_gen :1252" :
                                          "default float; gen_untyped_13 :96") + ")";
    if (!fannedOut) {
      w2skip(label, "the block ran serially (budget 1 or no sibling piet_native), so no worker "
                    "was exercised");
      continue;
    }
    const std::string want = asDouble ? tag + " t=double a=3 f=0" : tag + " t=float a=0 f=3";
    w2check(!r.stoppedAtReason.has_value() && line == want,
            label + ": a worker's untyped `generate v = 1/3` -> '" + want + "' -- got '" + line +
                "'" + (r.stoppedAtReason ? " stop=" + *r.stoppedAtReason : std::string()));
  }

  // Actual recast stores: w2e_float_default.log:1565/1580/1612/1637.
  {
    piet::SessionState s(workdir);
    for (const char* c : {"clear", "set obs 5", "generate double d = 1/3",
                         "generate float f = 2.7", "generate double exact = 5"}) (void)run(s, c);
    const std::string head = *s.currentHbin();
    const Reply refused = run(s, "recast float d");
    (void)run(s, "local __t0 : type d");
    double value = 0;
    w2check(refused.rc == "0" && s.eval.locals.get("__t0", "") == "double" &&
                storedValue(s, "d", 1, &value) && value == 1.0/3.0 && *s.currentHbin() == head,
            "W2E recast_float_noforce: lossy narrowing keeps type, bits and head at rc 0");
    const Reply forced = run(s, "recast float d, force");
    (void)run(s, "local __t0 : type d");
    w2check(forced.rc == "0" && s.eval.locals.get("__t0", "") == "float" &&
                storedValue(s, "d", 1, &value) && value == static_cast<double>(static_cast<float>(1.0/3.0)),
            "W2E recast_float_force: the stored value rounds to float with its type claim");
    const Reply fractional = run(s, "recast byte f");
    (void)run(s, "local __t0 : type f");
    w2check(fractional.rc == "0" && s.eval.locals.get("__t0", "") == "float" &&
                storedValue(s, "f", 1, &value) && value == static_cast<double>(2.7f),
            "W2E recast_byte_noforce: a fraction keeps float and its stored value");
    const Reply truncated = run(s, "recast byte f, force");
    (void)run(s, "local __t0 : type f");
    w2check(truncated.rc == "0" && s.eval.locals.get("__t0", "") == "byte" &&
                storedValue(s, "f", 1, &value) && value == 2,
            "W2E recast_byte_force: a fraction truncates toward zero in actual storage");
    const std::string exactHead = *s.currentHbin();
    const Reply narrowed = run(s, "recast byte exact");
    (void)run(s, "local __t0 : type exact");
    w2check(narrowed.rc == "0" && s.eval.locals.get("__t0", "") == "byte" &&
                *s.currentHbin() == exactHead && storedValue(s, "exact", 1, &value) && value == 5,
            "W2E recast: exact narrowing changes only metadata and keeps the current head");
    (void)run(s, "generate double over = 300");
    (void)run(s, "generate double ext = .a");
    (void)run(s, "recast byte over ext, force");
    double missing = 0, extended = 0;
    w2check(storedValue(s, "over", 1, &missing) && std::isnan(missing) &&
                storedValue(s, "ext", 1, &extended) && std::isnan(extended) &&
                run(s, "count if ext == .a").rc == "0" && s.eval.rValues.find("N") && *s.eval.rValues.find("N") == 5,
            "W2E recast: forced integer overflow becomes missing; extended missing payload survives");
  }

  // The unrecorded-creator fallback (SPEC.md 3.4): a replace into a column no
  // creator recorded is stored double (no rounding) and counted.
  std::printf("[selftest]        unrecorded-creator replace fallbacks so far (process): %lld\n",
              static_cast<long long>(piet::storage::unrecordedReplaceFallbackTotal()));
}
