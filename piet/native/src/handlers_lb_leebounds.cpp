// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// handlers_lb_leebounds.cpp -- `leebounds` (SSC, Harald Tauchmann, leebounds.ado
// "version 1.5 2013-07-17"): Lee (2009) treatment-effect bounds under sample
// selection. Implements the simple (no `tight()`) bounds per the public ado's
// behaviour; identified against StataBE probes (StataNow 19.5 BE, leebounds 1.5):
// scratch-r150/lee/oracle/lo.{do,golden} and scratch-r150/lee/lb/p1-p4.{do,log}.
//
// What is computed, in the ado's order:
//   * the sample: rows in if/in; selection s = (y nonmissing) by default, or the
//     0/1 values of select(); the treatment indicator is the egen-group of its
//     two distinct values (any two values: 2/7 posts the same bounds as 0/1);
//     e(sample) = rows with s and the group nonmissing, and with y nonmissing
//     unless s == 0.
//   * the group with the LOWER selection share is the reference group, the other
//     one is trimmed (e(trimmed) "treatment" or "control"); the trimming share
//     q = (p1 - p0)/p1.
//   * upper/lower bound = the mean of the trimmed group's selected outcomes above
//     the q-quantile / below the (1-q)-quantile, minus the reference group's
//     selected mean; a quantile that equals a data value splits the tied mass
//     (the share correction for ties), and the analytic variance is Lee's
//     (2009) formula with its four terms.
//
// Numerical conventions that change answers, each measured on StataBE:
//   * every intermediate the ado keeps in a `local` is the %18.0g macro text of
//     the value (eval::stataMacroRealText), so b, V and the tie test `y == q`
//     see that rounded value (p1.log K/K2/K3: `local x = 1/3` is
//     .3333333333333333);
//   * the quantile is `_pctile` with pweights, whose "lands exactly on a rank"
//     test has a tolerance of 1e-7 of the total weight (p2/p3: N = 9..9400; at
//     N = 94 a target 1e-6 past rank 11 still averages ranks 11 and 12, 1e-5
//     past it does not; the threshold scales with N, not with the rank). In a
//     balanced design the target is an exact integer in exact arithmetic, so
//     this tolerance, not the 1-ulp error of the product, decides it (oracle
//     `plain`: the unweighted rule would give a 1% different V);
//   * the squared outcome in the tie branch is a float variable (`gen` without a
//     type).
//
// Refused by name (rc 199): tight(), cieffect, vce(bootstrap) and any other
// vce() but vce(analytic), and weights (pw/fw/iw; aw is Stata's r(101)).
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <map>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

#include "core/distributions.h"
#include "handlers_e3_common.h"
#include "piet/datastore.h"
#include "piet/display_hardening.h"
#include "piet/eval/errors.h"
#include "piet/eval/macros.h"
#include "piet/handlers.h"
#include "piet/session_state.h"
#include "piet/strutil.h"
#include "vendor/nlohmann/json.hpp"

namespace piet::eval {
// eval/macros.cpp: the text a `local v = <real>` stores (%18.0g). Declared here
// as handlers_qe.cpp and loops.cpp declare it (macros.h does not export it).
std::string stataMacroRealText(double d);
}  // namespace piet::eval

namespace piet {
namespace handlers {

namespace {

using e3::ojson;
using e3::rcError;
constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();
const char* const kTouse = "__lb_touse";
const char* const kTouseIf = "__lb_touseif";

// A value as the ado's `local name = <exp>` keeps it: its macro text, read back.
double M(double v) {
  if (std::isnan(v)) return kNaN;
  const std::string t = eval::stataMacroRealText(v);
  if (t == ".") return kNaN;
  return std::strtod(t.c_str(), nullptr);
}

// A named refusal: `reason` carries refused:leebounds_<cause>.
ojson refused(const std::string& cause) {
  return ojson(eval::refusedCommand("leebounds", "leebounds_" + cause));
}

std::string padLeft(const std::string& s, size_t w) {
  return s.size() >= w ? s : std::string(w - s.size(), ' ') + s;
}
std::string padRight(const std::string& s, size_t w) {
  return s.size() >= w ? s : s + std::string(w - s.size(), ' ');
}
// The coefficient-table name column: 12 characters, a longer name abbreviated
// to its first 10, `~` and its last character (Stata's abbrev(name, 12)).
std::string abbrev12(const std::string& s) {
  if (s.size() <= 12) return s;
  return s.substr(0, 10) + "~" + s.substr(s.size() - 1);
}
// Stata's coefficient-table cell: %9.0g with one character reserved for the
// sign (handlers_qe.cpp / handlers_resample.cpp g9cell, the same rule).
std::string g9cell(double v) {
  if (std::isnan(v)) return ".";
  std::string s = fmtStataDefaultG(v);
  const size_t sign = (!s.empty() && s[0] == '-') ? 1u : 0u;
  if (s.size() - sign <= 8) return s;
  for (int prec = 7; prec >= 1; --prec) {
    char buf[64];
    std::snprintf(buf, sizeof buf, "%.*g", prec, v);
    std::string t(buf);
    const size_t sg = (!t.empty() && t[0] == '-') ? 1u : 0u;
    if (t.size() > sg + 1 && t.compare(sg, 2, "0.") == 0) t.erase(sg, 1);
    if (t.size() - sg <= 8) return t;
  }
  return s;
}
std::string fixedN(double v, int n) {
  if (std::isnan(v)) return ".";
  char buf[64];
  std::snprintf(buf, sizeof buf, "%.*f", n, v);
  return std::string(buf);
}

// Mean and sample variance (n - 1) of a set of values, the way `summarize`
// reports them; NaN variance for fewer than two values.
struct Moments {
  double n = 0, sum = 0, mean = kNaN, var = kNaN;
};
// A sum carried beyond double precision (Neumaier's compensation), so the
// summation order of the data cannot move the result.
double compensatedSum(const std::vector<double>& x) {
  double s = 0, c = 0;
  for (double v : x) {
    const double t = s + v;
    c += std::fabs(s) >= std::fabs(v) ? (s - t) + v : (v - t) + s;
    s = t;
  }
  return s + c;
}
Moments moments(const std::vector<double>& x) {
  Moments m;
  m.n = static_cast<double>(x.size());
  m.sum = compensatedSum(x);
  if (x.empty()) return m;
  m.mean = m.sum / m.n;
  if (x.size() >= 2) {
    std::vector<double> d2;
    d2.reserve(x.size());
    for (double v : x) d2.push_back((v - m.mean) * (v - m.mean));
    m.var = compensatedSum(d2) / (m.n - 1);
  }
  return m;
}

// `_pctile x [pweight = 1], percentiles(p)` over ascending values: target
// P = W p / 100 over the running weight; a running weight within 1e-7 W of P
// averages that value with the next one (StataBE p2/p3, file header), the first
// running weight past it otherwise picks that value.
double pctilePw(const std::vector<double>& sorted, double p) {
  const size_t n = sorted.size();
  if (n == 0) return kNaN;
  const double W = static_cast<double>(n);
  const double P = W * p / 100.0;
  const double tol = 1e-7 * W;
  double cum = 0;
  for (size_t i = 0; i < n; ++i) {
    cum += 1.0;
    if (std::fabs(cum - P) <= tol) return i + 1 < n ? (sorted[i] + sorted[i + 1]) / 2.0 : sorted[i];
    if (cum > P) return sorted[i];
  }
  return sorted[n - 1];
}

// One side of the bounds (the ado's leesbound, upper or lower half): the trimmed
// mean of the selected trimmed-group outcomes and its analytic variance.
struct Side {
  double treat = kNaN;  // trimmed mean minus the reference mean
  double var = kNaN;
};
struct Cells {
  double est, esnt, et, oddsc, oddst, nall;
};
Side boundSide(bool upper, const std::vector<double>& y1sorted, double pct, double qtrim,
               const Cells& c, double vp, double vc, double mean0) {
  Side out;
  const double th = M(pctilePw(y1sorted, pct));
  double neth = 0;
  for (double v : y1sorted) neth += (v == th) ? 1.0 : 0.0;
  double tb = kNaN, vb1 = kNaN;
  if (neth == 0) {
    std::vector<double> sel;
    for (double v : y1sorted)
      if (upper ? v >= th : v <= th) sel.push_back(v);
    const Moments m = moments(sel);
    tb = M(m.mean);
    vb1 = M(m.var / m.n);
  } else {
    neth = M(neth);
    std::vector<double> beyond, beyondSq;
    for (double v : y1sorted) {
      if (upper ? v > th : v < th) {
        beyond.push_back(v);
        beyondSq.push_back(e3::toFloat(v * v));  // `gen __ysq = (y)^2`: a float variable
      }
    }
    const double nb = M(static_cast<double>(beyond.size()));
    const double sb = M(compensatedSum(beyond));
    const double sq = compensatedSum(beyondSq);
    const double ntall = M(static_cast<double>(y1sorted.size()));
    const double stie = M((ntall * (1 - qtrim) - nb) / neth);
    tb = M((sb + stie * neth * th) / (nb + stie * neth));
    vb1 = M(((sq + stie * neth * std::pow(th, 2)) / (nb + stie * neth) - std::pow(tb, 2)) /
            (nb + stie * neth));
  }
  const double vb2 = M(std::pow(th - tb, 2) * qtrim * std::pow(c.est * (1 - qtrim), -1));
  const double vb3 = M(std::pow((th - tb) / (1 - qtrim), 2) * vp);
  out.var = M(vb1 + (vb2 + vb3 + vc) / c.nall);
  out.treat = M(tb - mean0);
  return out;
}

// The posted results the display reads (fresh or replayed).
struct Posted {
  std::string depvar, treatment;
  double N = kNaN, Nsel = kNaN, trim = kNaN;
  std::vector<double> b;
  std::vector<std::vector<double>> V;
};

void appendDisplay(ojson& blocks, const Posted& p, double level) {
  auto say = [&blocks](const std::string& t) { blocks.push_back(e3::displayText(t)); };
  say("");
  say("Lee (2009) treatment effect bounds");
  say("");
  say("Number of obs." + std::string(20, ' ') + " =   " + eval::stataMacroRealText(p.N));
  say("Number of selected obs." + std::string(11, ' ') + " =   " +
      eval::stataMacroRealText(p.Nsel));
  // `%-06.4f`: four decimals, left-justified in six.
  say("Trimming porportion" + std::string(15, ' ') + " =   " + padRight(fixedN(p.trim, 4), 6));
  say("");
  const std::string rule(78, '-');
  say(rule);
  const std::string left = padLeft(abbrev12(p.depvar), 12) +
                           " | Coefficient  Std. err.      z    P>|z|";
  const std::string lw = fmtStataDefaultG(level);
  say(left + padLeft("[" + lw + "% conf. interval]", 78 - left.size()));
  say("-------------+" + std::string(64, '-'));
  say(padRight(abbrev12(p.treatment), 12) + " |");
  const double crit = cecon::normal_icdf(1.0 - (100.0 - level) / 200.0);
  const char* names[2] = {"lower", "upper"};
  for (size_t j = 0; j < 2; ++j) {
    const double bj = p.b[j];
    const double vjj = p.V[j][j];
    // A zero variance (the "cannot compute analytic variance" J(2,2,0)) prints
    // `.` in every inference column, as StataBE does (p4 TINY).
    const double se = (std::isfinite(vjj) && vjj > 0) ? std::sqrt(vjj) : kNaN;
    const double z = bj / se;
    const double pv = std::isnan(z) ? kNaN : std::erfc(std::fabs(z) / std::sqrt(2.0));
    say(padLeft(names[j], 12) + " |" + padLeft(g9cell(bj), 11) + padLeft(g9cell(se), 11) +
        padLeft(fixedN(z, 2), 9) + padLeft(fixedN(pv, 3), 8) +
        padLeft(g9cell(bj - crit * se), 13) + padLeft(g9cell(bj + crit * se), 12));
  }
  say(rule);
}

// level() as the ado treats it: a value in [50, 100) is rounded to .01 and
// becomes the level (r(198) past 99.99, from `set level`); anything else falls
// back to c(level) (p1 H30: level(30) posts 95).
bool resolveLevel(const std::optional<double>& given, SessionState& session, double* level,
                  ojson* err) {
  *level = static_cast<double>(session.level);
  if (!given.has_value() || !(*given >= 50 && *given < 100)) return true;
  char buf[64];
  std::snprintf(buf, sizeof buf, "round(%.17g,0.01)", *given);
  std::string raised;
  const std::optional<std::string> r = eval::evalArith(buf, &session.eval, &raised);
  const double v = r.has_value() ? std::strtod(r->c_str(), nullptr) : kNaN;
  if (!(v >= 10 && v <= 99.99)) {
    *err = rcError("level() must be between 10 and 99.99 inclusive", 198);
    return false;
  }
  *level = v;
  return true;
}

std::optional<double> realOpt(const std::string& text) {
  const std::string t = strutil::trim(text);
  if (t.empty()) return std::nullopt;
  char* end = nullptr;
  const double v = std::strtod(t.c_str(), &end);
  if (end == t.c_str() || *end != '\0') return std::nullopt;
  return v;
}

// The ado's cmdline: `leebounds `*'`, its arguments as blank-separated tokens
// (p4 S1: `leebounds   y    treat ,   level(90)` -> "leebounds y treat , level(90)").
std::string cmdlineOf(const std::string& raw) {
  std::istringstream is(raw);
  std::string out, w;
  is >> w;  // the command word as typed
  out = "leebounds";
  while (is >> w) out += " " + w;
  return out;
}

}  // namespace

nlohmann::ordered_json runLeebounds(const eval::ParsedCommand& parsed, SessionState& session) {
  ojson blocks = ojson::array();
  auto fail = [&blocks](ojson e) {
    blocks.push_back(std::move(e));
    return blocks;
  };
  const std::string argText = strutil::trim(parsed.argText);
  // replay(): nothing before the comma -- a weight is not nothing.
  const bool replay = argText.empty() && !parsed.ifExpr.has_value() &&
                      !parsed.inRange.has_value() && !parsed.weightType.has_value();

  // ---- options (ado syntax; replay ado takes level() only) ------------------
  static const std::vector<e3::OptSpec> kSpecs = {{"select", 3, true},
                                                  {"tight", 3, true},
                                                  {"cieffect", 3, false},
                                                  {"vce", 3, true},
                                                  {"level", 3, true}};
  std::string selectVar;
  std::optional<double> levelGiven;
  ojson err;
  for (const auto& [k, v] : parsed.opts) {
    const int idx = e3::matchOpt(kSpecs, k);
    const std::string full = idx < 0 ? k : kSpecs[static_cast<size_t>(idx)].full;
    if (idx < 0 || (replay && full != "level"))
      return fail(rcError("option " + full + (idx >= 0 && kSpecs[idx].takesArg ? "()" : "") +
                              " not allowed",
                          198));
    const e3::OptSpec& s = kSpecs[static_cast<size_t>(idx)];
    if (s.takesArg && v.isBool) return fail(rcError("option " + s.full + "() required", 198));
    if (!s.takesArg && !v.isBool) return fail(rcError("option " + s.full + " not allowed", 198));
    const std::string arg = strutil::trim(v.strValue);
    if (full == "level") {
      levelGiven = realOpt(arg);
      if (!levelGiven.has_value())
        return fail(rcError("option level() incorrectly specified", 198));
    } else if (full == "select") {
      std::vector<std::string> sv;
      if (!e3::expandVars(arg, session, &sv, &err)) return fail(err);
      if (sv.size() != 1) return fail(rcError("too many variables specified", 103));
      selectVar = sv[0];
    } else if (full == "tight") {
      return fail(refused("tight_not_implemented"));
    } else if (full == "cieffect") {
      return fail(refused("cieffect_not_implemented"));
    } else if (full == "vce") {
      // ado: vce(analytic) and its abbreviations down to `ana` are the
      // default (p4 VA/VA2); vce(bootstrap...) is the ado's own weighted
      // bootstrap, not implemented here.
      std::string a = strutil::lower(arg);
      const std::string kAnalytic = "analytic";
      if (!(a.size() >= 3 && a.size() <= kAnalytic.size() && kAnalytic.compare(0, a.size(), a) == 0))
        return fail(refused("vce_" + (a.compare(0, 4, "boot") == 0 ? std::string("bootstrap")
                                                                   : std::string("other")) +
                            "_not_implemented"));
    }
  }

  // ---- replay (ado) -------------------------------------------
  if (replay) {
    const std::string* cmd = session.eval.eStrings.find("cmd");
    if (cmd == nullptr || *cmd != "leebounds") return fail(rcError("last estimates not found", 301));
    Posted p;
    const std::string* dv = session.eval.eStrings.find("depvar");
    const std::string* tr = session.eval.eStrings.find("treatment");
    p.depvar = dv != nullptr ? *dv : std::string();
    p.treatment = tr != nullptr ? *tr : std::string();
    auto sc = [&session](const char* k) {
      const double* d = session.eval.eScalars.find(k);
      return d != nullptr ? *d : kNaN;
    };
    p.N = sc("N");
    p.Nsel = sc("Nsel");
    p.trim = sc("trim");
    const eval::EvalMatrix* eb = nullptr;
    const eval::EvalMatrix* eV = nullptr;
    for (const auto& [k, m] : session.eval.eMatrices) {
      if (k == "b") eb = &m;
      if (k == "V") eV = &m;
    }
    if (eb == nullptr || eV == nullptr || eb->data.size() != 1 || eb->data[0].size() != 2 ||
        eV->data.size() != 2 || eV->data[0].size() != 2 || eV->data[1].size() != 2)
      return fail(rcError("last estimates not found", 301));
    p.b = eb->data[0];
    p.V = eV->data;
    // `syntax, [LEVel(real `e(level)')]` then `ereturn display, level()`: the
    // header prints before the level is checked (p4 RL5).
    const double eLevel = sc("level");
    const double level = levelGiven.has_value() ? *levelGiven : eLevel;
    if (!(level >= 10 && level <= 99.99)) {
      ojson hdr = ojson::array();
      appendDisplay(hdr, p, 95);
      for (size_t i = 0; i < 7 && i < hdr.size(); ++i) blocks.push_back(hdr[i]);
      return fail(rcError("level() must be between 10 and 99.99 inclusive", 198));
    }
    appendDisplay(blocks, p, level);
    return blocks;
  }

  if (!session.hasData()) return fail(ojson(eval::noDataLoaded()));
  // ---- varlist(min=2 max=2) (p4 V0/V1/V3: 100/102/103) -------------------------
  std::vector<std::string> vars;
  if (argText.empty()) return fail(rcError("varlist required", 100));
  // Weights: pweight/fweight/iweight are the ado's (refused here); aweight is
  // not in its syntax (p4 AW: "aweights not allowed", r(101)).
  if (parsed.weightType.has_value()) {
    const std::string w = strutil::lower(*parsed.weightType);
    if (w.rfind("a", 0) == 0) return fail(rcError("aweights not allowed", 101));
    return fail(refused("weights_not_implemented"));
  }
  if (!e3::expandVars(argText, session, &vars, &err)) return fail(err);
  if (vars.size() < 2) return fail(rcError("too few variables specified", 102));
  if (vars.size() > 2) return fail(rcError("too many variables specified", 103));
  const std::string yv = vars[0], tv = vars[1];
  Datastore* ds = session.datastore();
  // `gen tempsel = y < .` on a string y is a type mismatch (p4 SY).
  if (ds->dtype(yv) != "f64") return fail(rcError("type mismatch", 109));
  if (ds->dtype(tv) != "f64") return fail(refused("string_treatment_not_implemented"));
  if (!selectVar.empty() && ds->dtype(selectVar) != "f64")
    return fail(refused("string_select_not_implemented"));

  // ---- level (ado) ---------------------------------------------------------
  double level = 95;
  if (!resolveLevel(levelGiven, session, &level, &err)) return fail(err);

  // ---- the if/in rows (and, for select(), the if-only rows: ado reads `if2`) --
  struct TempGuard {
    SessionState& s;
    std::vector<std::string> cols;
    ~TempGuard() {
      for (const auto& c : cols) {
        try {
          SessionState::TempDropScope td(s);
          (void)piet::dispatch("capture drop " + c, s);
        } catch (...) {
        }
      }
    }
  } guard{session, {}};
  const size_t N = static_cast<size_t>(ds->nRows());
  std::vector<char> inQual(N, 1), inIf(N, 1);
  // The guard owns a column only once OUR generate made it: a failed generate
  // (say the user already has a variable of that name, r(110)) must never let the
  // guard drop the user's column.
  auto qualMask = [&](const char* col, const std::string& qual, std::vector<char>* out) -> bool {
    if (!e3::runLine(std::string("quietly generate double ") + col + " = 1" + qual, session, &err))
      return false;
    guard.cols.push_back(col);
    Datastore* d = session.datastore();
    const double* t = d->col(col).f64();
    for (size_t i = 0; i < N; ++i) (*out)[i] = t[i] == 1.0;
    SessionState::TempDropScope td(session);
    if (!e3::runLine(std::string("quietly drop ") + col, session, &err)) return false;
    guard.cols.pop_back();
    return true;
  };
  {
    std::string qual;
    if (parsed.ifExpr.has_value()) qual += " if " + *parsed.ifExpr;
    if (parsed.inRange.has_value()) qual += " in " + *parsed.inRange;
    if (!qual.empty() && !qualMask(kTouse, qual, &inQual)) return fail(err);
    if (!selectVar.empty() && parsed.ifExpr.has_value() &&
        !qualMask(kTouseIf, " if " + *parsed.ifExpr, &inIf))
      return fail(err);
  }
  ds = session.datastore();
  auto column = [&ds, N](const std::string& name) {
    const double* p = ds->col(name).f64();
    return std::vector<double>(p, p + N);
  };
  const std::vector<double> y = column(yv);
  const std::vector<double> trv = column(tv);

  // ---- the selection indicator (ado) ---------------------------------------
  std::vector<double> ss(N, kNaN);
  if (!selectVar.empty()) {
    const std::vector<double> sel = column(selectVar);
    double cssel = 0;
    for (size_t i = 0; i < N; ++i)
      if (inIf[i] && !std::isnan(y[i]) && !std::isnan(sel[i])) cssel += sel[i];
    std::vector<double> cats;
    for (size_t i = 0; i < N; ++i)
      if (inQual[i] && !std::isnan(sel[i])) cats.push_back(sel[i]);
    std::sort(cats.begin(), cats.end());
    cats.erase(std::unique(cats.begin(), cats.end()), cats.end());
    if (cats.empty()) return fail(rcError("no observations", 2000));
    if (cats.size() == 1)
      return fail(rcError("no variation in selection indicator " + selectVar, 109));
    if (cats.size() > 2 || cats[0] != 0 || cats[1] != 1 || cssel == 0)
      return fail(rcError("selection indicator " + selectVar + " incorrectly specified", 109));
    for (size_t i = 0; i < N; ++i)
      if (inQual[i] && (sel[i] == 0 || sel[i] == 1)) ss[i] = sel[i];
  } else {
    // ado `sum tempsel in if`: every if/in row counts here, a row with a
    // missing treatment value included (it leaves the sample only later).
    double n = 0, s = 0;
    for (size_t i = 0; i < N; ++i) {
      if (!inQual[i]) continue;
      ss[i] = std::isnan(y[i]) ? 0.0 : 1.0;
      n += 1;
      s += ss[i];
    }
    if (n > 0 && s / n == 1) return fail(rcError("all observations selected", 109));
    if (n > 0 && s / n == 0) return fail(rcError("no observations selected", 2000));
  }

  // ---- the treatment indicator: egen group() of its values (ado) ------------
  std::vector<double> tvals;
  for (size_t i = 0; i < N; ++i)
    if (inQual[i] && !std::isnan(trv[i])) tvals.push_back(trv[i]);
  std::sort(tvals.begin(), tvals.end());
  tvals.erase(std::unique(tvals.begin(), tvals.end()), tvals.end());
  if (tvals.empty()) return fail(rcError("no observations", 2000));
  if (tvals.size() != 2) return fail(rcError("treatment indicator " + tv + " not binary", 149));
  std::vector<double> tt(N, kNaN);
  for (size_t i = 0; i < N; ++i)
    if (inQual[i] && !std::isnan(trv[i])) tt[i] = trv[i] == tvals[0] ? 0.0 : 1.0;

  // ---- e(sample) (ado) -------------------------------------------------------
  std::vector<char> esamp(N, 0);
  for (size_t i = 0; i < N; ++i) {
    const bool sOk = ss[i] == 0 || ss[i] == 1;
    esamp[i] = inQual[i] && sOk && !std::isnan(tt[i]) && (!std::isnan(y[i]) || ss[i] == 0);
  }
  double nObs = 0, nSel = 0, n0 = 0, n1 = 0, s0 = 0, s1 = 0;
  for (size_t i = 0; i < N; ++i) {
    if (!esamp[i]) continue;
    nObs += 1;
    nSel += ss[i];
    if (tt[i] == 0) {
      n0 += 1;
      s0 += ss[i];
    } else {
      n1 += 1;
      s1 += ss[i];
    }
  }
  if (n0 == 0 || n1 == 0) return fail(rcError("no observations", 2000));
  // `mean s, over(t)` (ado): the selection shares.
  const double p0 = s0 / n0, p1 = s1 / n1;
  if (p0 == 0) return fail(rcError("no information on " + yv + " for control group", 2000));
  if (p1 == 0) return fail(rcError("no information on " + yv + " for treatment group", 2000));
  if (p0 == p1)
    return fail(rcError("selection proportion exactly equal for both groups, no bounds computed",
                        498, "engine_error"));
  const bool trimControl = p0 > p1;
  if (trimControl)
    for (size_t i = 0; i < N; ++i)
      if (!std::isnan(tt[i])) tt[i] = (tt[i] - 1) * (tt[i] - 1);

  // ---- the bounds (the ado's leesbound, ado) ---------------------------------
  double c00 = 0, c01 = 0, c10 = 0, c11 = 0;  // (selection, group) counts
  std::vector<double> y1, y0;
  for (size_t i = 0; i < N; ++i) {
    if (!esamp[i]) continue;
    const bool sel = ss[i] == 1, trimmed = tt[i] == 1;
    (sel ? (trimmed ? c11 : c10) : (trimmed ? c01 : c00)) += 1;
    if (sel && trimmed) y1.push_back(y[i]);
    if (sel && !trimmed) y0.push_back(y[i]);
  }
  Cells c;
  const double nall = nObs;
  c.nall = M(std::round(nall));
  c.est = M(c11 / nall);
  c.esnt = M(c10 / nall);
  c.et = M(c01 / nall + c11 / nall);
  c.oddsc = M((c00 / nall) / (c10 / nall));
  c.oddst = M((c01 / nall) / (c11 / nall));
  // `mean s [pw], over(t)` again after the swap: the trimmed group's share and
  // the reference group's, i.e. p0/p1 above relabelled.
  const double sh1 = trimControl ? p0 : p1, sh0 = trimControl ? p1 : p0;
  const double trim = M(100 * (sh1 - sh0) / sh1);
  const double qtrim = M(trim / 100);
  std::sort(y1.begin(), y1.end());
  const Moments m0 = moments(y0);
  const double mean0 = m0.mean;
  const double vp = M(std::pow(1 - qtrim, 2) * (c.oddst / c.et + c.oddsc / (1 - c.et)));
  const double vc = M(m0.var / c.esnt);
  const Side up = boundSide(true, y1, trim, qtrim, c, vp, vc, mean0);
  const double itrim = M(100 - trim);
  const Side lo = boundSide(false, y1, itrim, qtrim, c, vp, vc, mean0);

  std::vector<double> b(2);
  std::vector<std::vector<double>> V(2, std::vector<double>(2, 0.0));
  if (!trimControl) {
    b = {lo.treat, up.treat};
    V[0][0] = lo.var;
    V[1][1] = up.var;
  } else {
    b = {-1 * up.treat, -1 * lo.treat};
    V[0][0] = up.var;
    V[1][1] = lo.var;
  }
  if (std::isnan(b[0]) || std::isnan(b[1])) return fail(rcError("cannot compute bounds", 2000));
  if (std::isnan(V[0][0]) || std::isnan(V[1][1])) {
    V = {{0.0, 0.0}, {0.0, 0.0}};
    blocks.push_back(e3::displayText("warning: cannot compute analytic variance"));
  }

  // ---- post (ado) -------------------------------------------------------------
  const std::vector<std::string> names = {tv + ":lower", tv + ":upper"};
  ojson block;
  block["type"] = "estimation";
  block["estimator"] = "leebounds";
  block["depvar"] = yv;
  block["n_obs"] = static_cast<long long>(nObs);
  block["coeftable"] = e3::coeftableFromPosting(names, b, V, kNaN);
  LastModel lm;
  lm.cobraCmd = "leebounds";
  lm.stataCmd = "leebounds";
  // lm.y stays EMPTY on purpose: a resampling prefix reads lm.y/lm.x as "rows
  // with these missing are outside the fit" (handlers_resample.cpp), and here
  // the rows with a missing outcome ARE in e(sample) -- they are the
  // non-selected observations the bounds are made of. Filtering them out
  // leaves every replicate fully selected (r(109)), so no replicate completes.
  lm.depvar = yv;
  if (parsed.ifExpr.has_value()) lm.fitKeepif = *parsed.ifExpr;
  lm.vcov = "";
  lm.result = block;
  (void)e3::installEsampleMask(session, lm, esamp);
  session.setLastModel(lm);
  session.lastEstBlock = block;
  e3::postBV(session, names, b, V);
  e3::setEScalarOnFit(session, "N", M(std::round(nObs)));
  e3::setEScalarOnFit(session, "Nsel", M(std::round(nSel)));
  e3::setEScalarOnFit(session, "trim", qtrim);
  e3::setEScalarOnFit(session, "level", level);
  e3::setEStringOnFit(session, "trimmed", trimControl ? "control" : "treatment");
  e3::setEStringOnFit(session, "treatment", tv);
  if (!selectVar.empty()) e3::setEStringOnFit(session, "select", selectVar);
  e3::setEStringOnFit(session, "vce", "analytic");
  e3::setEStringOnFit(session, "title", "Lee (2009) treatment effect bounds");
  e3::setEStringOnFit(session, "cmdline", cmdlineOf(parsed.raw));
  e3::setEStringOnFit(session, "cmd", "leebounds");
  e3::setEStringOnFit(session, "properties", "b V");
  e3::setEStringOnFit(session, "depvar", yv);

  // The structured block lives in session.lastEstBlock (where estimates store,
  // eststo, esttab and _b[]/_se[] read it) and is NOT pushed to the reply: the
  // console carries the ado's own `ereturn display` text below, with its
  // equation header row and level() interval, which the frontend's coeftable
  // renderer (one 95% table, no equation rows) would print a second time.
  Posted p;
  p.depvar = yv;
  p.treatment = tv;
  p.N = M(std::round(nObs));
  p.Nsel = M(std::round(nSel));
  p.trim = qtrim;
  p.b = b;
  p.V = V;
  appendDisplay(blocks, p, level);
  return blocks;
}

}  // namespace handlers
}  // namespace piet
