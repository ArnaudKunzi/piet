// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// handlers_aer_estim.cpp — AER scope-1 bundle H (ESTIM).
//
// Two kinds of command live here and they are worth telling apart:
//
//   E1  aliases onto estimators PIET already has. `areg` IS regress with an
//       absorb(); `ivreg2`/`xtivreg2` are ivregress under community spellings.
//       These normalise options and delegate -- no new mathematics.
//
//   E2  closed-form statistics computed HERE, with no engine call at all:
//       spearman, sktest, signrank, centile. Each reads its columns through
//       the section-38 projection and does its own arithmetic on top of
//       core/distributions.h.
//
// The E2 four are why this bundle was specified before it was written. Their
// formulas were checked against the StataNow 19.5 manuals and independently
// recomputed BEFORE implementation, because H's tests would otherwise have been
// written from the same transcription as its code -- a transcription error
// would have been PINNED as the answer instead of caught. Six of the eight
// specs came back wrong. Every correction that survived that review is carried
// at its own site below, with the measurement that forced it, so that a later
// reader cannot quietly "simplify" it back. The record is
// internal note ERRATA-AER-H-SPECS-20260904.md.
//
// The engine-backed half of E2 (tsfilter hp, lpoly, lowess, pca) refuses
// honestly here until the cobra subcommands land: the handler ASKS the engine
// what it supports rather than assuming, so a PIET built against an older cobra
// says so instead of producing a wrong number or a crash.

#include "piet/handlers.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <fstream>
#include <iomanip>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <regex>
#include <set>
#include <sstream>
#include <string>
#include <unordered_set>
#include <vector>

#include "core/distributions.h"  // betai / betaitail / z_critical (shared with cobra)
#include "core/exact_sum.h"  // ExactSum: summarize's accumulation (centile's mean)
#include "piet/eval/exprfns.h"  // kStataMissingSentinel
#include "piet/cobra_runner.h"
#include "piet/column_fetch.h"
#include "piet/display_hardening.h"
#include "piet/eval/errors.h"
#include "piet/inrange_filter.h"
#include "piet/mutation_result.h"
#include "piet/scratch_seq.h"
#include "io/win_compat.h"
#include "piet/loops.h"
#include "piet/session_state.h"
#include "piet/strutil.h"
#include "piet/twoway_opts.h"
#include "estimation_m4_internal.h"
#include "estimators/postest_names.h"  // factor_vars_of / parse_level_ref (the engine's one resolver)
#include "registry.h"  // resolveCanonical: the command word as Stata names it

namespace piet::handlers {

using ojson = nlohmann::ordered_json;

namespace {

// Fixed-width table cells count CHARACTERS, not bytes: StataBE pads and
// abbreviates a Unicode variable name by characters (scratch-r150/uni-v/p4.do),
// so a multi-byte name stays aligned and is never cut mid-sequence.
// fitLeft: the first `w` characters, right-aligned in `w`; fitRight: left-aligned.
std::string fitLeft(const std::string& s, size_t w) {
  const std::string c = cecon::ident::truncateCodePoints(s, w);
  const size_t n = cecon::ident::codePointCount(c);
  return n >= w ? c : std::string(w - n, ' ') + c;
}
std::string fitRight(const std::string& s, size_t w) {
  const std::string c = cecon::ident::truncateCodePoints(s, w);
  const size_t n = cecon::ident::codePointCount(c);
  return n >= w ? c : c + std::string(w - n, ' ');
}

// Stata's abbrev(name, w): a name past `w` characters keeps its first w-2, `~`
// and its last character (StataBE centile: `قيمةطويلةج~ا`).
std::string abbrevCp(const std::string& s, size_t w) {
  if (cecon::ident::codePointCount(s) <= w || w < 3) return s;
  size_t last = s.size();
  do {
    --last;
  } while (last > 0 && (static_cast<unsigned char>(s[last]) & 0xC0) == 0x80);
  return cecon::ident::truncateCodePoints(s, w - 2) + "~" + s.substr(last);
}

ojson aerError(const std::string& message, const std::string& code = "engine_error") {
  return ojson(eval::engineError(message, code));
}

// r(198) "invalid syntax". The rc is derived from the error CODE by the BS-100
// taxonomy, not from a reason string -- `engine_error` lands in the generic
// bucket and reads back as rc 1, which is what this wrapper exists to avoid.
ojson aerSyntaxError(const std::string& message) {
  return ojson(eval::engineError(message, "syntax_error"));
}

// A refusal whose Stata rc is known but which the coarse message rules do not
// recognise. `reason` is the only channel carrying an exact rc, and it outranks
// the coarse mapping -- without it these come back as the generic rc 1.
ojson aerErrorReason(const std::string& message, int rc) {
  return ojson(eval::errorBlock("engine_error", message, std::nullopt, std::nullopt, std::nullopt,
                                "r(" + std::to_string(rc) + ")"));
}

ojson displayBlock(const std::string& text) {
  ojson b;
  b["type"] = "display";
  b["text"] = text;
  return b;
}

// ---------------------------------------------------------------------------
// Reading

// Open the columns this command needs. Mirrors handlers_aer_util.cpp's
// ReadScope: try the section-38 projection first, fall back to the resident
// head, and never half-succeed -- a projection that is missing any requested
// column is discarded rather than silently answering about fewer columns.
class ColumnScope {
 public:
  ColumnScope(SessionState& session, const eval::ParsedCommand& parsed,
              const std::vector<std::string>& cols, const std::string& verb) {
    if (!session.hasData() || !session.currentHbin().has_value()) {
      err_ = ojson(eval::noDataLoaded());
      return;
    }
    const bool wantFilter = (parsed.ifExpr.has_value() && !parsed.ifExpr->empty()) ||
                            (parsed.inRange.has_value() && !parsed.inRange->empty());
    try {
      if (wantFilter) {
        InRangeFilterResult f =
            applyIfAndInFilter(session, *session.currentHbin(), parsed.ifExpr, parsed.inRange);
        if (!f.ok) {
          err_ = inRangeRejectError(f, parsed.inRange.value_or(std::string()), verb);
          return;
        }
        if (f.dataPath != *session.currentHbin()) {
          session.noteProjectionScratch(f.dataPath);
          owned_ = std::make_unique<Datastore>(f.dataPath);
          ds_ = owned_.get();
          return;
        }
        ds_ = session.datastore();
        return;
      }
      std::vector<const std::string*> exprs;
      exprs.reserve(cols.size());
      for (const auto& c : cols) exprs.push_back(&c);
      if (auto want = referencedColumns(session, exprs, "")) {
        std::string projPath;
        std::unique_ptr<Datastore> proj =
            fetchColumns(session, *want, session.workdir(), &projPath);
        if (proj) {
          bool complete = true;
          for (const auto& c : cols) {
            if (!proj->has(c)) {
              complete = false;
              break;
            }
          }
          if (complete) owned_ = std::move(proj);
        }
      }
      ds_ = owned_ ? owned_.get() : session.datastore();
    } catch (const std::exception& e) {
      err_ = aerError(verb + ": could not read the data -- " + std::string(e.what()), "data_io");
    }
  }

  Datastore* get() const { return ds_; }
  const ojson& error() const { return err_; }
  bool failed() const { return !err_.is_null() || ds_ == nullptr; }

 private:
  std::unique_ptr<Datastore> owned_;
  Datastore* ds_ = nullptr;
  ojson err_;
};

// Pull one numeric column as doubles, NaN for missing. Returns false when the
// column is absent or is a string/factor the statistic cannot use.
bool numericColumn(const Datastore& ds, const std::string& name, std::vector<double>* out,
                   std::string* err) {
  if (!ds.has(name)) {
    *err = "variable " + name + " not found";
    return false;
  }
  const cecon::Column& c = ds.col(name);
  const size_t n = ds.nRows();
  out->assign(n, std::numeric_limits<double>::quiet_NaN());
  // DType carries exactly two shapes at this layer: F64 and Factor. A factor's
  // code IS its numeric value for these statistics (the same reading `egen rank`
  // takes), and a negative code is the missing sentinel.
  if (c.dtype == cecon::DType::Factor) {
    const int32_t* codes = c.codes();
    if (codes == nullptr) {
      *err = name + ": column has no readable payload";
      return false;
    }
    for (size_t i = 0; i < n; ++i)
      (*out)[i] = (codes[i] < 0) ? std::numeric_limits<double>::quiet_NaN()
                                 : static_cast<double>(codes[i]);
    return true;
  }
  const double* d = c.f64();
  if (d == nullptr) {
    *err = name + ": column has no readable payload";
    return false;
  }
  for (size_t i = 0; i < n; ++i) (*out)[i] = d[i];
  return true;
}

// ---------------------------------------------------------------------------
// Shared arithmetic

// Average ("mid") ranks, the rule `egen rank` already uses (frame.cpp:1805):
// tied observations all receive the mean of the positions they occupy.
std::vector<double> midranks(const std::vector<double>& v) {
  const size_t n = v.size();
  std::vector<size_t> idx(n);
  for (size_t i = 0; i < n; ++i) idx[i] = i;
  std::sort(idx.begin(), idx.end(), [&v](size_t a, size_t b) { return v[a] < v[b]; });
  std::vector<double> r(n, 0.0);
  size_t i = 0;
  while (i < n) {
    size_t j = i;
    while (j + 1 < n && v[idx[j + 1]] == v[idx[i]]) ++j;
    const double m = (static_cast<double>(i + 1) + static_cast<double>(j + 1)) / 2.0;
    for (size_t k = i; k <= j; ++k) r[idx[k]] = m;
    i = j + 1;
  }
  return r;
}

// Standard normal CDF via erfc. Used ONLY in the complement direction -- see
// each call site for why `1 - Phi(+x)` is banned here.
double normalCdf(double z) { return 0.5 * std::erfc(-z / std::sqrt(2.0)); }

// Phi^-1(p) for p in (0,1). core/distributions.h's z_critical is the TWO-SIDED
// critical value -- it computes Phi^-1(1 - alpha/2), so it cannot reach a lower
// tail below 0.5 at all, and sktest's Royston block needs exactly that
// (Zc = -Phi^-1(exp(-K^2/2)), and exp(-K^2/2) < 0.5 as soon as K^2 > 1.386).
// Same Acklam rational approximation plus the same single Halley refinement, so
// the two agree wherever their domains overlap; kept local because widening
// z_critical's contract would touch every cobra caller it already has.
double invNormal(double p) {
  if (!(p > 0.0) || !(p < 1.0)) return std::numeric_limits<double>::quiet_NaN();
  static const double a[] = {-3.969683028665376e+01, 2.209460984245205e+02,
                             -2.759285104469687e+02, 1.383577518672690e+02,
                             -3.066479806614716e+01, 2.506628277459239e+00};
  static const double b[] = {-5.447609879822406e+01, 1.615858368580409e+02,
                             -1.556989798598866e+02, 6.680131188771972e+01,
                             -1.328068155288572e+01};
  static const double c[] = {-7.784894002430293e-03, -3.223964580411365e-01,
                             -2.400758277161838e+00, -2.549732539343734e+00,
                             4.374664141464968e+00, 2.938163982698783e+00};
  static const double d[] = {7.784695709041462e-03, 3.224671290700398e-01, 2.445134137142996e+00,
                             3.754408661907416e+00};
  const double plow = 0.02425, phigh = 1 - plow;
  double q, r, x;
  if (p < plow) {
    q = std::sqrt(-2 * std::log(p));
    x = (((((c[0] * q + c[1]) * q + c[2]) * q + c[3]) * q + c[4]) * q + c[5]) /
        ((((d[0] * q + d[1]) * q + d[2]) * q + d[3]) * q + 1);
  } else if (p <= phigh) {
    q = p - 0.5;
    r = q * q;
    x = (((((a[0] * r + a[1]) * r + a[2]) * r + a[3]) * r + a[4]) * r + a[5]) * q /
        (((((b[0] * r + b[1]) * r + b[2]) * r + b[3]) * r + b[4]) * r + 1);
  } else {
    q = std::sqrt(-2 * std::log(1 - p));
    x = -(((((c[0] * q + c[1]) * q + c[2]) * q + c[3]) * q + c[4]) * q + c[5]) /
        ((((d[0] * q + d[1]) * q + d[2]) * q + d[3]) * q + 1);
  }
  const double e = 0.5 * std::erfc(-x * 0.7071067811865476) - p;
  const double u = e * 2.5066282746310002 * std::exp(0.5 * x * x);
  return x - u / (1 + 0.5 * x * u);
}

// Does the engine expose this subcommand? Asked, not assumed: a PIET built
// against a cobra without the bundle-H verbs must say so rather than fail
// somewhere less legible.
bool engineHasSubcommand(const std::string& sub) {
  try {
    nlohmann::json opts = runCobraJson({"options", "--json"});
    if (opts.is_object() && opts.contains(sub)) return true;
    if (opts.is_object() && opts.contains("commands") && opts["commands"].is_object())
      return opts["commands"].contains(sub);
    if (opts.is_array()) {
      for (const auto& e : opts)
        if (e.is_string() && e.get<std::string>() == sub) return true;
    }
  } catch (const std::exception&) {
    return false;
  }
  return false;
}

ojson engineTooOld(const std::string& stataCmd, const std::string& sub) {
  return aerError(stataCmd + " requires a cobra build that provides the `" + sub +
                      "` subcommand; this engine does not. Rebuild the engine from a tree that "
                      "includes AER bundle H1, or point $COBRA_BIN at one.",
                  "unsupported_command");
}

}  // namespace

// ===========================================================================
// E1 — aliases

// areg IS regress with an absorb(), so it routes to the same handler with a
// different stataCmd. Everything that makes areg areg (the e(df_a) absorbed
// degrees of freedom, the "Linear regression, absorbing indicators" header)
// belongs to runRegress's areg arm, not to a second copy of the estimator.
// areg's clustered dof (it charges an FE nested in the cluster, where reghdfe
// and xtreg,fe exempt it) is runRegress's areg arm too (BUG-568); that is what
// retired the BUG-684 stop-gap which used to refuse weighted areg + cluster here.
ojson runAreg(const eval::ParsedCommand& parsed, SessionState& session) {
  // BUG-569: areg's option abbreviations. w2b_estim.log measures `a(id)`,
  // `ab(id)`, `abs(id)` (areg_a(id) .. areg_abs(id)), `cl(id)` (areg_a_cl) and
  // `r` (areg_a_r) all at rc 0, but areg has no row in the generated minimum
  // table the dispatcher resolves abbreviations from, and runRegress only reads
  // the full names -- so each of them was refused "option `a` not allowed".
  // Canonicalized here to the names runRegress reads, at the measured floors
  // (absorb 1, cluster 2, robust 1); anything shorter stays as typed and is
  // refused by runRegress's unknown-option net.
  eval::ParsedCommand canon = parsed;
  auto isAbbrev = [](const std::string& key, const std::string& full, size_t floor) {
    return key.size() >= floor && key.size() <= full.size() &&
           full.compare(0, key.size(), key) == 0;
  };
  bool sawAbsorb = false;
  for (auto& [k, v] : canon.opts) {
    (void)v;
    if (isAbbrev(k, "absorb", 1)) {
      k = "absorb";
      sawAbsorb = true;
    } else if (isAbbrev(k, "cluster", 2)) {
      k = "cluster";
    } else if (isAbbrev(k, "robust", 1)) {
      k = "robust";
    }
  }
  // Stata: areg without absorb() is r(198), not a silent plain regression.
  if (!sawAbsorb) return aerSyntaxError("option absorb() required");

  return runRegress(canon, session, "areg");
}

// ivreg2 / xtivreg2 -> runIvRegress. The spellings differ from ivregress in
// their option vocabulary, not in their estimator, so this normalises and
// delegates.
ojson runIvreg2(const eval::ParsedCommand& parsed, SessionState& session,
                const std::string& stataCmd) {
  eval::ParsedCommand p = parsed;

  bool robust = false, gmm2s = false;
  std::string cluster;
  bool fe = false, fd = false, small = false;
  const bool xtTyped = stataCmd == "xtivreg2" && strutil::lower(parsed.cmd) == "xtivreg2";
  std::vector<std::pair<std::string, eval::OptValue>> kept;
  for (const auto& kv : p.opts) {
    const std::string& k = kv.first;
    // BUG-569 (review round): the value shape, as runRegress checks it. A
    // valued `r(x)` used to count as robust, and a bare `cl` or an empty
    // `cl()` set no cluster, so the fit ran iid at rc 0.
    if (k == "r" || k == "robust") {
      if (!kv.second.isBool) return aerSyntaxError("option `robust` takes no argument");
      robust = true;
    } else if (k == "cl" || k == "cluster") {
      if (kv.second.isBool || strutil::trim(kv.second.strValue).empty()) {
        return aerSyntaxError("option `cluster` requires a variable: cluster(varname)");
      }
      cluster = kv.second.strValue;
    } else if (k == "gmm2s") {
      gmm2s = true;
    } else if (k == "fe") {
      fe = true;
    } else if (k == "fd" && xtTyped) {
      fd = true;
    } else if (k == "small") {
      small = true;
    } else {
      kept.push_back(kv);
    }
  }

  // `gmm2s` alone is not GMM in the sense ivregress means: without a robust or
  // clustered weight matrix the two-step estimator collapses to 2SLS, so the
  // honest translation is 2sls plus a note rather than a silent method switch.
  const bool weighted = robust || !cluster.empty();
  const std::string method = (gmm2s && weighted) ? "gmm" : "2sls";
  // xtivreg2: exactly one of fe and fd, else
  // "error - must specify either fe or fd option" and error 198
  // (w2d_uncovered_xtivreg2.log:2866 xtivreg2_nofe_ctl).
  if (xtTyped && fe == fd) {
    return ojson(eval::errorBlock("syntax_error", "error - must specify either fe or fd option",
                                  std::nullopt, std::nullopt, std::nullopt, "r(198)"));
  }
  if (xtTyped && fd) {
    return aerError(
        "Option `fd` is a valid xtivreg2 option that this version of PIET does not implement "
        "(the first-differences estimator); `fe` is implemented.",
        "unsupported_option");
  }

  // B4 P0-C: `eval::OptValue{}` here was a WRONG NUMBER, not a style nit. A
  // default-constructed OptValue has isBool == false (tokenizer.h:33), and the
  // only reader of this key — vcovFromOpts (estimation_m4.cpp:509) — matches
  // `k == "robust" && v.isBool && v.boolValue`. So the flag this loop had just
  // parsed was re-emitted in a shape its consumer could not see, `--vcov hc1`
  // was never appended, and `ivreg2 ..., robust` returned the IID standard
  // error while the census certified the option as supported. Measured on
  // auto.csv: se[mpg] 65.3314331 for bare AND robust AND small; StataBE gives
  // 65.33143 / 70.08472 / 66.23260 (oracle_p.log:250, :297, :341).
  if (robust) kept.emplace_back("robust", eval::OptValue::ofBool(true));
  if (!cluster.empty()) {
    eval::OptValue cv;
    cv.strValue = cluster;
    kept.emplace_back("cluster", cv);
  }
  if (fe) {
    if (!session.panelvar.has_value()) {
      return aerError(stataCmd + ": option fe requires a panel setting -- run xtset first",
                      "syntax_error");
    }
    eval::OptValue av;
    av.strValue = *session.panelvar;
    kept.emplace_back("absorb", av);
  }
  p.opts = kept;

  // ivreg2's default inference is large-sample (z); `small` asks for the
  // t/F small-sample correction. ivregress's absorb path forces --small
  // unconditionally, which is right for `ivreghdfe` and wrong here, so the
  // flag is threaded (see the forceSmall argument below) rather than inherited.
  // B4 P0-C, the OTHER half — and it is not the same defect as `robust` above.
  // This line used to read `(void)small;` at the bottom of the function: the
  // flag was parsed, kept out of `kept` so it could not be rejected, and then
  // explicitly DISCARDED. The comment above it claimed "the flag is threaded
  // rather than inherited", which described a wiring that did not exist —
  // `--small` reached cobra only through `allowAbsorb`, i.e. only for
  // `ivreg2 ..., fe`. So `ivreg2 ..., small` certified an option it dropped.
  // Threading it is what the comment already said should happen.
  // `smallDefault=fe` reproduces exactly what `allowAbsorb` used to imply here: ivreg2 reports
  // the small-sample correction when it is absorbing, and not otherwise. It is spelled out now
  // because the two meanings were one bit until 2026-09-09 (BUG-474's split).
  // xtivreg2 calls ivreg2 on the within-transformed data with dofminus(N_g) and no
  // `small` of its own (xtivreg2): large-sample z statistics unless
  // `small` is typed (w2d_uncovered_xtivreg2.log:2647 `z`), so its default is not
  // the absorbing ivreg2 spelling's.
  ojson out = runIvRegress(method, p, /*allowAbsorb=*/fe, stataCmd, session,
                           /*smallDefault=*/fe && !xtTyped, /*forceSmall=*/small);
  if (gmm2s && !weighted && out.is_object() && out.value("type", std::string()) != "error") {
    out["notes"].push_back(
        "gmm2s without robust or cluster() is 2SLS: the two-step weight matrix "
        "equals the 2SLS one under homoskedasticity.");
  }
  // e(model) (ivreg2): gmm2s under gmm2s, with or without robust/cluster()
  // (StataBE 19.5, BUG-1223: `ivreg2 y (x = z z2), gmm2s` posts gmm2s, with _b equal to
  // 2SLS's up to rounding, and boottest then prints "GMM no longer supported"); ols when
  // there is no endogenous regressor (BUG-438); iv otherwise.
  const bool ivreg2Typed = stataCmd == "ivreg2" && strutil::lower(parsed.cmd) == "ivreg2";
  if (ivreg2Typed && out.is_object() && out.value("type", std::string()) != "error" &&
      session.lastModel().has_value()) {
    const std::string model =
        gmm2s ? "gmm2s" : (session.lastModel()->ivEndog.empty() ? "ols" : "iv");
    session.eval.setEString("model", model);
    session.noteEOnFit("model");
  }
  return out;
}

// ===========================================================================
// E2 — computed here

// spearman (StataNow 19.5): rank correlation, p from a
// four-parameter beta fit to the first four permutation moments of
// T = sum x_i y_i on centred midranks ([R] spearman, Methods and formulas).
namespace {
struct SpearmanPair {
  size_t n = 0;
  double rho = std::numeric_limits<double>::quiet_NaN();
  double p = std::numeric_limits<double>::quiet_NaN(), p_l = p, p_u = p;
};

// One pair over the rows where both are present (Spearman2: the ranks are taken
// within that sample).
SpearmanPair spearmanPair(const std::vector<double>& xs, const std::vector<double>& ys) {
  SpearmanPair out;
  std::vector<double> x, y;
  for (size_t i = 0; i < xs.size(); ++i) {
    if (std::isnan(xs[i]) || std::isnan(ys[i])) continue;
    x.push_back(xs[i]);
    y.push_back(ys[i]);
  }
  const size_t n = x.size();
  out.n = n;
  if (n < 3) return out;
  const std::vector<double> rx = midranks(x), ry = midranks(y);
  const double nn = static_cast<double>(n);
  double mx = 0.0, my = 0.0;
  for (size_t i = 0; i < n; ++i) {
    mx += rx[i];
    my += ry[i];
  }
  mx /= nn;
  my /= nn;

  std::vector<double> X(n), Y(n);
  for (size_t i = 0; i < n; ++i) {
    X[i] = rx[i] - mx;
    Y[i] = ry[i] - my;
  }
  auto powsum = [&](const std::vector<double>& v, int k) {
    double s = 0.0;
    for (double a : v) s += std::pow(a, k);
    return s;
  };
  const double S2 = powsum(X, 2), S3 = powsum(X, 3), S4 = powsum(X, 4);
  const double T2 = powsum(Y, 2), T3 = powsum(Y, 3), T4 = powsum(Y, 4);
  if (S2 == 0.0 || T2 == 0.0) return out;  // a constant variable: no correlation

  double Tobs = 0.0;
  for (size_t i = 0; i < n; ++i) Tobs += X[i] * Y[i];
  const double rho = Tobs / std::sqrt(S2 * T2);

  const double ET2 = S2 * T2 / (nn - 1.0);
  const double e3x = S3 / 3.0, e3y = T3 / 3.0;
  const double ET3 = S3 * T3 / nn + 3.0 * S3 * T3 / (nn * (nn - 1.0)) +
                     36.0 * e3x * e3y / (nn * (nn - 1.0) * (nn - 2.0));
  const double C2x = (S2 * S2 - S4) / 2.0, C2y = (T2 * T2 - T4) / 2.0;
  const double Dx = S4 - S2 * S2 / 2.0, Dy = T4 - T2 * T2 / 2.0;
  const double e4x = (S2 * S2 - 2.0 * S4) / 8.0, e4y = (T2 * T2 - 2.0 * T4) / 8.0;
  const double ET4 = S4 * T4 / nn + 4.0 * S4 * T4 / (nn * (nn - 1.0)) +
                     12.0 * C2x * C2y / (nn * (nn - 1.0)) +
                     24.0 * Dx * Dy / (nn * (nn - 1.0) * (nn - 2.0)) +
                     576.0 * e4x * e4y / (nn * (nn - 1.0) * (nn - 2.0) * (nn - 3.0));

  const double v = ET2;
  const double g3 = ET3 / std::pow(v, 1.5);
  const double g4 = ET4 / (v * v);
  const double rr = 6.0 * (g4 - g3 * g3 - 1.0) / (6.0 + 3.0 * g3 * g3 - 2.0 * g4);
  const double d = 1.0 - 24.0 * (rr + 1.0) /
                             ((rr + 2.0) * (rr + 3.0) * g4 - 3.0 * (rr - 6.0) * (rr + 1.0));

  // CLAMP before the root. When both variables are tie-free the centred ranks
  // are symmetric, so S3 = T3 = 0 exactly, g3 = 0, and the true d is 0 -- but in
  // double precision it lands on the wrong side of zero about half the time.
  // Measured over n = 5..500 with tie-free ranks: d < 0 for 239 of 496 sample
  // sizes, first at n = 13 (-6.66e-16). std::sqrt of that is NaN, and the NaN
  // propagates through alpha, beta, l, u, s and p, so the command would return a
  // MISSING p-value on ordinary continuous data. At d = 0 the clamp correctly
  // yields alpha = beta = r/2, the symmetric beta.
  const double dc = (d > 0.0) ? d : 0.0;
  const double a = rr * (1.0 - std::sqrt(dc)) / 2.0;
  const double b = rr * (1.0 + std::sqrt(dc)) / 2.0;
  const double alpha = (g3 > 0.0) ? a : b;
  const double beta = (g3 > 0.0) ? b : a;

  double p = std::numeric_limits<double>::quiet_NaN();
  double p_l = p, p_u = p;
  if (alpha > 0.0 && beta > 0.0) {
    const double sc = std::sqrt(v * (alpha + beta + 1.0) / (alpha * beta));
    const double l = -alpha * sc, u = beta * sc;
    if (u > l) {
      const double s = (Tobs - l) / (u - l);
      // TWO functions, not one algebraic identity. The manual calls ibeta and
      // ibetatail separately and the difference is not cosmetic: for strongly
      // positive rho, `1 - p_l` annihilates the answer. Measured at n = 74 with
      // tie-free ranks and rho = 0.9999, the subtraction returns exactly 0 where
      // the upper tail is 6.8136e-32. Both display as 0.0000, so a check value
      // cannot see it -- but r(p_u) is a published return and a golden at
      // rtol 1e-9 diverges once the tail drops below about 1e-7.
      p_l = cecon::betai(alpha, beta, s);
      p_u = cecon::betaitail(alpha, beta, s);
      const double pm = std::min(p_l, p_u);
      p = std::min(1.0, 2.0 * pm);
    }
  }

  out.rho = rho;
  out.p = p;
  out.p_l = p_l;
  out.p_u = p_u;
  return out;
}
}  // namespace

// BUG-1014 (w2as_summary_stats_family sx_spearman_*): more than two variables are
// served as spearman.ado serves them -- the `matrix` form: one listwise sample,
// every pair i < j through the pair kernel, r(Rho) (1 on the diagonal), r(P)
// (missing on the diagonal), r(Nobs), and r(N) r(rho) r(p) r(p_l) r(p_u) of the
// LAST pair (`return add` after the loop; sx_spearman_three). Two variables post
// no matrix (rowsof(r(Rho)) r(109) in Stata, sx_spearman_two). A string variable
// is ignored; fewer than two variables named is r(102) (varlist(min=2),
// sx_spearman_ref_one); an option Stata does not have is r(198)
// (sx_spearman_ref_opt; it was accepted and ignored).
ojson runSpearman(const eval::ParsedCommand& parsed, SessionState& session) {
  // stats()/print()/star() choose what the table prints; matrix forces the
  // matrix form. exact, pw, bonferroni and sidak change the numbers and are not
  // served here: named refusals.
  static const std::unordered_set<std::string> kKnown = {"stats", "print", "star", "matrix"};
  static const std::unordered_set<std::string> kUnimpl = {"exact", "pw", "bonferroni", "sidak"};
  if (auto e = eval::rejectUnknownOptions(parsed.opts, kKnown, kUnimpl)) return ojson(*e);
  bool matrixForm = false;
  for (const auto& [k, v] : parsed.opts) {
    if (k == "matrix") matrixForm = true;
    if (k == "stats" && !v.isBool) {
      std::istringstream iss(v.strValue);
      std::string w;
      while (iss >> w)
        if (w != "rho" && w != "obs" && w != "p")
          return ojson(eval::errorBlock("syntax_error",
                                        "statistic " + w + " not allowed in option stats()",
                                        std::nullopt, std::nullopt, std::nullopt,
                                        std::string("r(198)")));
    }
  }
  if (parsed.varlist.size() < 2) {
    return ojson(eval::errorBlock("syntax_error", "too few variables specified", std::nullopt,
                                  std::nullopt, std::nullopt, std::string("r(102)")));
  }
  ColumnScope scope(session, parsed, parsed.varlist, "spearman");
  if (scope.failed()) return scope.error();
  Datastore& ds = *scope.get();

  std::vector<std::string> vars;
  for (const auto& v : parsed.varlist) {
    if (!ds.has(v)) return aerError("spearman: variable " + v + " not found");
    if (ds.dtype(v) == "factor") continue;  // "note: v ignored because string variable."
    vars.push_back(v);
  }
  const size_t k = vars.size();
  if (k < 2) {
    return ojson(eval::errorBlock("syntax_error", "too few variables specified", std::nullopt,
                                  std::nullopt, std::nullopt, std::string("r(102)")));
  }
  std::vector<std::vector<double>> cols(k);
  for (size_t j = 0; j < k; ++j) {
    std::string err;
    if (!numericColumn(ds, vars[j], &cols[j], &err)) return aerError("spearman: " + err);
  }
  if (k > 2) matrixForm = true;
  if (matrixForm) {
    // marksample: the listwise sample over every variable.
    const size_t nr = cols[0].size();
    for (size_t i = 0; i < nr; ++i) {
      bool miss = false;
      for (size_t j = 0; j < k; ++j) miss = miss || std::isnan(cols[j][i]);
      if (miss)
        for (size_t j = 0; j < k; ++j) cols[j][i] = std::numeric_limits<double>::quiet_NaN();
    }
  }
  const double nanv = std::numeric_limits<double>::quiet_NaN();
  eval::EvalMatrix Rho, P, Nobs;
  for (auto* m : {&Rho, &P, &Nobs}) {
    m->rownames = vars;
    m->colnames = vars;
    m->data.assign(k, std::vector<double>(k, nanv));
  }
  SpearmanPair last;
  for (size_t i = 0; i < k; ++i) {
    size_t cnt = 0;
    for (double v : cols[i]) cnt += std::isnan(v) ? 0 : 1;
    Nobs.data[i][i] = static_cast<double>(cnt);
    Rho.data[i][i] = 1.0;
  }
  for (size_t i = 0; i + 1 < k; ++i) {
    for (size_t j = i + 1; j < k; ++j) {
      last = spearmanPair(cols[i], cols[j]);
      Nobs.data[i][j] = Nobs.data[j][i] = static_cast<double>(last.n);
      Rho.data[i][j] = Rho.data[j][i] = last.rho;
      P.data[i][j] = P.data[j][i] = last.p;
    }
  }
  if (!matrixForm) {
    if (last.n == 0) {
      return ojson(eval::errorBlock("engine_error", "no observations", std::nullopt, std::nullopt,
                                    std::nullopt, std::string("r(2000)")));
    }
    if (last.n == 1) {
      return ojson(eval::errorBlock("engine_error", "insufficient observations", std::nullopt,
                                    std::nullopt, std::nullopt, std::string("r(2001)")));
    }
    if (last.n < 3) return aerError("spearman: too few observations", "engine_error");
    if (std::isnan(last.rho))
      return aerError("spearman: a variable is constant over the sample", "engine_error");
  }

  eval::replaceRClass(session.eval);
  std::vector<std::string> posted = {"N", "rho", "p", "p_l", "p_u"};
  session.eval.rValues.set("N", static_cast<double>(last.n));
  session.eval.rValues.set("rho", last.rho);
  session.eval.rValues.set("p", last.p);
  session.eval.rValues.set("p_l", last.p_l);
  session.eval.rValues.set("p_u", last.p_u);
  if (matrixForm) {
    session.eval.rMatrices.emplace_back("P", P);
    session.eval.rMatrices.emplace_back("Rho", Rho);
    session.eval.rMatrices.emplace_back("Nobs", Nobs);
    posted.insert(posted.end(), {"P", "Rho", "Nobs"});
  }
  eval::declareRPosted(session.eval, "spearman", std::move(posted));

  char buf[256];
  std::ostringstream out;
  if (!matrixForm) {
    std::snprintf(buf, sizeof(buf), "Number of observations = %6d", static_cast<int>(last.n));
    out << "\n" << buf << "\n";
    std::snprintf(buf, sizeof(buf), "        Spearman's rho = %.4f", last.rho);
    out << buf << "\n\n";
    out << "Test of H0: " << vars[0] << " and " << vars[1] << " are independent\n";
    std::snprintf(buf, sizeof(buf), "                  Prob = %.4f", last.p);
    out << buf << "\n";
  } else {
    out << "\n";
    std::snprintf(buf, sizeof(buf), "%12s |", "");
    out << buf;
    for (size_t j = 0; j < k; ++j) {
      std::snprintf(buf, sizeof(buf), " %s", fitLeft(abbrevCp(vars[j], 8), 8).c_str());
      out << buf;
    }
    out << "\n-------------+" << std::string(9 * k, '-') << "\n";
    for (size_t i = 0; i < k; ++i) {
      std::snprintf(buf, sizeof(buf), "%s |", fitLeft(abbrevCp(vars[i], 12), 12).c_str());
      out << buf;
      for (size_t j = 0; j <= i; ++j) {
        std::snprintf(buf, sizeof(buf), " %8.4f", Rho.data[i][j]);
        out << buf;
      }
      out << "\n";
    }
  }
  return displayBlock(out.str());
}

// sktest: D'Agostino-Belanger-D'Agostino (1990) skewness/kurtosis tests with
// Royston's (1991c) adjustment ([R] sktest, Methods and formulas).
ojson runSktest(const eval::ParsedCommand& parsed, SessionState& session) {
  if (parsed.varlist.empty()) return aerError("sktest: no variables specified", "syntax_error");
  static const std::unordered_set<std::string> kKnown = {"noadjust"};
  if (auto e = eval::rejectUnknownOptions(parsed.opts, kKnown)) return ojson(*e);
  bool noadjust = false;
  for (const auto& [k, v] : parsed.opts) {
    (void)v;
    if (k == "noadjust") noadjust = true;
  }

  ColumnScope scope(session, parsed, parsed.varlist, "sktest");
  if (scope.failed()) return scope.error();
  Datastore& ds = *scope.get();

  struct Row {
    std::string name;
    double N = 0, pSkew = 0, pKurt = 0, chi2 = 0, pChi2 = 0;
    bool ok = false;
  };
  std::vector<Row> rows;

  for (const auto& vn : parsed.varlist) {
    Row row;
    row.name = vn;
    std::vector<double> raw;
    std::string err;
    if (!numericColumn(ds, vn, &raw, &err)) return aerError("sktest: " + err);
    std::vector<double> z;
    for (double d : raw)
      if (!std::isnan(d)) z.push_back(d);
    const double nn = static_cast<double>(z.size());
    row.N = nn;
    const double nanv = std::numeric_limits<double>::quiet_NaN();
    row.pSkew = row.pKurt = row.chi2 = row.pChi2 = nanv;

    // n < 8 is NOT a refusal. Live StataBE 19.5: at n = 7, 2, 1, 0 nonmissing,
    // with zero rows selected by an if, and on a constant variable, sktest
    // returns _rc == 0, prints the row with "." for all four statistics, and
    // CONTINUES to the next variable. The manual sentence about "a minimum of
    // eight observations" is Description prose, not an emitted error -- turning
    // it into one would abort whole commands that Stata completes.
    if (z.size() >= 8) {
      double m = 0.0;
      for (double d : z) m += d;
      m /= nn;
      double m2 = 0, m3 = 0, m4 = 0;
      for (double d : z) {
        const double e = d - m;
        const double e2 = e * e;
        m2 += e2;
        m3 += e2 * e;
        m4 += e2 * e2;
      }
      m2 /= nn;
      m3 /= nn;
      m4 /= nn;
      if (m2 > 0.0) {
        const double g1 = m3 / std::pow(m2, 1.5);
        const double b2 = m4 / (m2 * m2);

        const double Y = g1 * std::sqrt((nn + 1.0) * (nn + 3.0) / (6.0 * (nn - 2.0)));
        const double beta2 = 3.0 * (nn * nn + 27.0 * nn - 70.0) * (nn + 1.0) * (nn + 3.0) /
                             ((nn - 2.0) * (nn + 5.0) * (nn + 7.0) * (nn + 9.0));
        const double W2 = -1.0 + std::sqrt(2.0 * (beta2 - 1.0));
        const double alph = std::sqrt(2.0 / (W2 - 1.0));
        const double Z1 = (1.0 / std::sqrt(std::log(std::sqrt(W2)))) *
                          std::log(Y / alph + std::sqrt((Y / alph) * (Y / alph) + 1.0));

        const double Eb2 = 3.0 * (nn - 1.0) / (nn + 1.0);
        const double varb2 =
            24.0 * nn * (nn - 2.0) * (nn - 3.0) / ((nn + 1.0) * (nn + 1.0) * (nn + 3.0) * (nn + 5.0));
        const double Xk = (b2 - Eb2) / std::sqrt(varb2);
        const double sqb1 = 6.0 * (nn * nn - 5.0 * nn + 2.0) / ((nn + 7.0) * (nn + 9.0)) *
                            std::sqrt(6.0 * (nn + 3.0) * (nn + 5.0) / (nn * (nn - 2.0) * (nn - 3.0)));
        const double A = 6.0 + 8.0 / sqb1 * (2.0 / sqb1 + std::sqrt(1.0 + 4.0 / (sqb1 * sqb1)));
        const double Z2 = ((1.0 - 2.0 / (9.0 * A)) -
                           std::cbrt((1.0 - 2.0 / A) / (1.0 + Xk * std::sqrt(2.0 / (A - 4.0))))) /
                          std::sqrt(2.0 / (9.0 * A));

        // COMPLEMENT form, never 1 - Phi(+|Z|). The literal spelling loses the
        // tail to cancellation: measured on auto price (Z1 = 4.7544) it is 1.3e-11
        // out where 2*Phi(-|Z|) is 1.1e-14, and beyond |Z| ~ 8.3 the subtraction
        // underflows to exactly 0, making -2 ln P infinite.
        row.pSkew = 2.0 * normalCdf(-std::fabs(Z1));
        row.pKurt = 2.0 * normalCdf(-std::fabs(Z2));
        const double K2 = Z1 * Z1 + Z2 * Z2;

        if (noadjust) {
          row.chi2 = K2;
          row.pChi2 = std::exp(-K2 / 2.0);  // exact chi2(2) upper tail
        } else {
          const double lnn = std::log(nn);
          const double pk = std::exp(-K2 / 2.0);
          // The '.' outcome the manual's Technical note anticipates is a MISSING
          // Royston block, not a generic "unrepresentable": when exp(-K2/2)
          // underflows to 0, Phi^-1 is undefined and Zc/Z/P all go missing, so
          // BOTH the adjusted chi2 and its p print as ".".
          if (pk > 0.0) {
            const double Zc = -invNormal(pk);
            const double Zt = 0.55 * std::pow(nn, 0.2) - 0.21;
            const double a1 = (-5.0 + 3.46 * lnn) * std::exp(-1.37 * lnn);
            const double b1 = 1.0 + (0.854 - 0.148 * lnn) * std::exp(-0.55 * lnn);
            const double a2 = a1 - (2.13 / (1.0 - 2.37 * lnn)) * Zt;
            const double b2p = 2.13 / (1.0 - 2.37 * lnn) + b1;
            double Z;
            if (Zc < -1.0) {
              Z = Zc;
            } else if (Zc < Zt) {
              Z = a1 + b1 * Zc;
            } else {
              Z = a2 + b2p * Zc;
            }
            const double P = normalCdf(-Z);  // never 1 - Phi(Z)
            if (P > 0.0) {
              row.chi2 = -2.0 * std::log(P);
              row.pChi2 = P;
            }
          }
        }
        row.ok = true;
      }
    }
    rows.push_back(row);
  }

  // r() carries the LAST variable's values, missing included.
  const Row& last = rows.back();
  session.eval.rValues.set("N", last.N);
  session.eval.rValues.set("p_skew", last.pSkew);
  session.eval.rValues.set("p_kurt", last.pKurt);
  session.eval.rValues.set("chi2", last.chi2);
  session.eval.rValues.set("p_chi2", last.pChi2);

  // r(table): one row per variable, in varlist order. Declared in the manual's
  // Stored results and previously unreachable in PIET -- there was no r-class
  // matrix store at all until this bundle added one.
  eval::EvalMatrix tbl;
  tbl.colnames = {"N", "p_skew", "p_kurt", "chi2", "p_chi2"};
  for (const auto& r : rows) {
    tbl.rownames.push_back(r.name);
    tbl.data.push_back({r.N, r.pSkew, r.pKurt, r.chi2, r.pChi2});
  }
  bool replaced = false;
  for (auto& kv : session.eval.rMatrices) {
    if (kv.first == "table") {
      kv.second = tbl;
      replaced = true;
      break;
    }
  }
  if (!replaced) session.eval.rMatrices.emplace_back("table", std::move(tbl));

  std::ostringstream out;
  char buf[256];
  // The name column is as wide as the longest name (in CHARACTERS), at least 12
  // (StataBE 19.5: a longest name of 18 characters gives an 18-wide column;
  // scratch-r150/uni-v/p5.do).
  size_t stubW = 12;
  for (const auto& r : rows) stubW = std::max(stubW, cecon::ident::codePointCount(r.name));
  out << "\nSkewness and kurtosis tests for normality\n";
  out << std::string(45 + stubW, ' ') << "----- Joint test -----\n";
  out << fitLeft("Variable", stubW) << " |       Obs   Pr(skewness)   Pr(kurtosis)   Adj chi2(2)  Prob>chi2\n";
  out << std::string(stubW + 1, '-')
      << "+-----------------------------------------------------------------\n";
  auto f4 = [&buf](double v, int w) {
    if (std::isnan(v)) {
      std::snprintf(buf, sizeof(buf), "%*s", w, ".");
    } else {
      std::snprintf(buf, sizeof(buf), "%*.4f", w, v);
    }
    return std::string(buf);
  };
  auto f2 = [&buf](double v, int w) {
    if (std::isnan(v)) {
      std::snprintf(buf, sizeof(buf), "%*s", w, ".");
    } else {
      std::snprintf(buf, sizeof(buf), "%*.2f", w, v);
    }
    return std::string(buf);
  };
  for (const auto& r : rows) {
    std::snprintf(buf, sizeof(buf), "%s |%10d   ", fitLeft(r.name, stubW).c_str(),
                  static_cast<int>(r.N));
    out << buf << f4(r.pSkew, 12) << "   " << f4(r.pKurt, 12) << "   " << f2(r.chi2, 11) << "   "
        << f4(r.pChi2, 8) << "\n";
  }
  return displayBlock(out.str());
}

// signrank: Wilcoxon matched-pairs signed-rank test ([R] signrank).
// BUG-1014 (w2as_summary_stats_family sx_signrank_*), as signrank runs:
//  - `= exp` may be a number (`signrank x = 0`, sx_signrank_zero): it was read
//    as a variable name, r(111);
//  - a string variable is r(109) (the difference is a type mismatch,
//    sx_signrank_ref_str), and an option other than exact/noexact r(198)
//    (sx_signrank_ref_opt; it was accepted and ignored);
//  - the exact p-values r(p_exact) r(p_l_exact) r(p_u_exact) are posted when
//    N <= 200 (or with `exact`, N <= 2000; r(459) above) unless `noexact`: the
//    permutation distribution of T = sum of the signed ranks over the 2^n sign
//    assignments, the ranks doubled when a tie makes them half-integers (the
//    ado's mata signrank_exact); p_exact = P(|T| >= |t|).
ojson runSignrank(const eval::ParsedCommand& parsed, SessionState& session) {
  static const std::unordered_set<std::string> kKnown = {"exact", "noexact"};
  if (auto e = eval::rejectUnknownOptions(parsed.opts, kKnown)) return ojson(*e);
  bool wantExact = false, noExact = false;
  for (const auto& [k, v] : parsed.opts) {
    (void)v;
    if (k == "exact") wantExact = true;
    if (k == "noexact") noExact = true;
  }
  if (wantExact && noExact) {
    return ojson(eval::errorBlock("syntax_error", "options exact and noexact may not be combined",
                                  std::nullopt, std::nullopt, std::nullopt, std::string("r(198)")));
  }
  // `signrank var = exp` arrives as a varlist whose middle token is "=".
  std::string lhs, rhs;
  {
    std::string joined;
    for (const auto& t : parsed.varlist) joined += t + " ";
    const size_t eq = joined.find('=');
    if (eq == std::string::npos) {
      return aerError("signrank: syntax is `signrank varname = exp`", "syntax_error");
    }
    auto trim = [](std::string s) {
      while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) s.erase(s.begin());
      while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.pop_back();
      return s;
    };
    lhs = trim(joined.substr(0, eq));
    rhs = trim(joined.substr(eq + 1));
  }
  if (lhs.empty() || rhs.empty()) {
    return aerError("signrank: syntax is `signrank varname = exp`", "syntax_error");
  }

  // A numeric constant on the right (`= 0`); anything else names a variable.
  std::optional<double> rhsConst;
  try {
    size_t pos = 0;
    const double v = std::stod(rhs, &pos);
    if (pos == rhs.size()) rhsConst = v;
  } catch (...) {
  }
  ColumnScope scope(session, parsed,
                    rhsConst ? std::vector<std::string>{lhs} : std::vector<std::string>{lhs, rhs},
                    "signrank");
  if (scope.failed()) return scope.error();
  Datastore& ds = *scope.get();

  for (const auto& v : rhsConst ? std::vector<std::string>{lhs} : std::vector<std::string>{lhs, rhs})
    if (ds.has(v) && ds.dtype(v) == "factor")
      return ojson(eval::errorBlock("syntax_error", "type mismatch", std::nullopt, std::nullopt,
                                    std::nullopt, std::string("r(109)")));
  std::vector<double> a, b;
  std::string err;
  if (!numericColumn(ds, lhs, &a, &err)) return aerError("signrank: " + err);
  if (rhsConst) {
    b.assign(a.size(), *rhsConst);
  } else if (!numericColumn(ds, rhs, &b, &err)) {
    return aerError("signrank: " + err);
  }

  std::vector<double> d;
  for (size_t i = 0; i < a.size(); ++i) {
    if (std::isnan(a[i]) || std::isnan(b[i])) continue;
    d.push_back(a[i] - b[i]);
  }
  const size_t n = d.size();
  if (n == 0) {
    return ojson(eval::errorBlock("engine_error", "no observations", std::nullopt, std::nullopt,
                                  std::nullopt, std::string("r(2000)")));
  }
  const bool exact = (n <= 200 && !noExact) || wantExact;
  if (exact && n > 2000) {
    return ojson(eval::errorBlock("syntax_error",
                                  "number of observations must be <= 2,000 for option exact",
                                  std::nullopt, std::nullopt, std::nullopt, std::string("r(459)")));
  }

  std::vector<double> absd(n);
  for (size_t i = 0; i < n; ++i) absd[i] = std::fabs(d[i]);
  const std::vector<double> r = midranks(absd);

  double sumPos = 0.0, sumNeg = 0.0, sumZero = 0.0;
  size_t nPos = 0, nNeg = 0, nZero = 0;
  for (size_t i = 0; i < n; ++i) {
    if (d[i] > 0.0) {
      sumPos += r[i];
      ++nPos;
    } else if (d[i] < 0.0) {
      sumNeg += r[i];
      ++nNeg;
    } else {
      sumZero += r[i];
      ++nZero;
    }
  }
  const double nn = static_cast<double>(n), n0 = static_cast<double>(nZero);

  // E(T+) carries the zero correction. The manual prints only n(n+1)/4, which
  // for its own fuel example gives 39 and contradicts its own printed output of
  // 38.5; the corrected form follows from the manual's E(T) = 0 together with
  // T+ = (T + sum|r_j|)/2. Transcribing the printed formula gives z = -2.012026
  // instead of -1.973, and a golden captured from that same transcription could
  // not catch it.
  const double ET = nn * (nn + 1.0) / 4.0 - n0 * (n0 + 1.0) / 4.0;
  const double varUnadj = nn * (nn + 1.0) * (2.0 * nn + 1.0) / 24.0;
  const double dZero = -n0 * (n0 + 1.0) * (2.0 * n0 + 1.0) / 24.0;
  double varAdj = 0.0;
  for (size_t i = 0; i < n; ++i)
    if (d[i] != 0.0) varAdj += r[i] * r[i];
  varAdj /= 4.0;
  const double dTies = varAdj - varUnadj - dZero;

  const double z = (varAdj > 0.0) ? (sumPos - ET) / std::sqrt(varAdj)
                                  : std::numeric_limits<double>::quiet_NaN();
  // Complement form again, and for the same reason: the literal 2(1 - Phi|z|)
  // underflows to exactly 0 for large |z|, which an absolute-tolerance
  // comparator would happily accept as correct.
  const double p_l = normalCdf(z);
  const double p_u = normalCdf(-z);
  const double p = 2.0 * std::min(p_l, p_u);

  // The exact permutation distribution (signrank_exact): integer signed ranks,
  // doubled when a tie made any rank a half-integer; each nonzero rank's sign is
  // + or - with probability 1/2.
  double pExact = std::numeric_limits<double>::quiet_NaN(), pLExact = pExact, pUExact = pExact;
  if (exact) {
    bool integral = true;
    for (size_t i = 0; i < n; ++i) integral = integral && r[i] == std::floor(r[i]);
    std::vector<long long> xs(n);
    long long total = 0, tObs = 0;
    for (size_t i = 0; i < n; ++i) {
      const long long rk = integral ? static_cast<long long>(r[i])
                                    : static_cast<long long>(std::llround(2.0 * r[i]));
      xs[i] = d[i] == 0.0 ? 0 : rk;
      total += xs[i];
      tObs += d[i] > 0.0 ? xs[i] : (d[i] < 0.0 ? -xs[i] : 0);
    }
    // prob[s + total] = P(T = s), T in [-total, total].
    std::vector<double> prob(static_cast<size_t>(2 * total + 1), 0.0), next;
    prob[static_cast<size_t>(total)] = 1.0;
    long long reach = 0;
    for (long long xv : xs) {
      if (xv == 0) continue;
      next.assign(prob.size(), 0.0);
      for (long long sv = -reach; sv <= reach; ++sv) {
        const double pr = prob[static_cast<size_t>(sv + total)];
        if (pr == 0.0) continue;
        next[static_cast<size_t>(sv + xv + total)] += 0.5 * pr;
        next[static_cast<size_t>(sv - xv + total)] += 0.5 * pr;
      }
      reach += xv;
      prob.swap(next);
    }
    double lo = 0.0, hi = 0.0, two = 0.0;
    const long long at = tObs < 0 ? -tObs : tObs;
    for (long long sv = -total; sv <= total; ++sv) {
      const double pr = prob[static_cast<size_t>(sv + total)];
      if (sv <= tObs) lo += pr;
      if (sv >= tObs) hi += pr;
      if (sv >= at || sv <= -at) two += pr;
    }
    pExact = std::min(two, 1.0);
    pLExact = std::min(lo, 1.0);
    pUExact = std::min(hi, 1.0);
  }

  eval::replaceRClass(session.eval);
  std::vector<std::string> posted = {"N", "N_pos", "N_neg", "N_tie", "sum_pos", "sum_neg",
                                     "Var_a", "z", "p", "p_l", "p_u"};
  if (exact) {
    session.eval.rValues.set("p_exact", pExact);
    session.eval.rValues.set("p_l_exact", pLExact);
    session.eval.rValues.set("p_u_exact", pUExact);
    posted.insert(posted.end(), {"p_exact", "p_l_exact", "p_u_exact"});
  }
  session.eval.rValues.set("N", nn);
  session.eval.rValues.set("N_pos", static_cast<double>(nPos));
  session.eval.rValues.set("N_neg", static_cast<double>(nNeg));
  session.eval.rValues.set("N_tie", static_cast<double>(nZero));
  session.eval.rValues.set("sum_pos", sumPos);
  session.eval.rValues.set("sum_neg", sumNeg);
  session.eval.rValues.set("Var_a", varAdj);
  session.eval.rValues.set("z", z);
  session.eval.rValues.set("p", p);
  session.eval.rValues.set("p_l", p_l);
  session.eval.rValues.set("p_u", p_u);
  eval::declareRPosted(session.eval, "signrank", std::move(posted));

  std::ostringstream out;
  char buf[256];
  const double expected = (sumPos + sumNeg + sumZero) / 2.0;
  out << "\nWilcoxon signed-rank test\n\n";
  out << "        Sign |      Obs   Sum ranks    Expected\n";
  out << "-------------+---------------------------------\n";
  std::snprintf(buf, sizeof(buf), "    Positive |%9d %11.10g %11.10g\n", static_cast<int>(nPos),
                sumPos, ET);
  out << buf;
  std::snprintf(buf, sizeof(buf), "    Negative |%9d %11.10g %11.10g\n", static_cast<int>(nNeg),
                sumNeg, ET);
  out << buf;
  std::snprintf(buf, sizeof(buf), "        Zero |%9d %11.10g %11.10g\n", static_cast<int>(nZero),
                sumZero, sumZero);
  out << buf;
  out << "-------------+---------------------------------\n";
  std::snprintf(buf, sizeof(buf), "         All |%9d %11.10g %11.10g\n", static_cast<int>(n),
                sumPos + sumNeg + sumZero, expected * 2.0 / 2.0 * 2.0 / 2.0);
  out << buf;
  std::snprintf(buf, sizeof(buf), "\nUnadjusted variance %11.2f\n", varUnadj);
  out << buf;
  std::snprintf(buf, sizeof(buf), "Adjustment for ties %11.2f\n", dTies);
  out << buf;
  std::snprintf(buf, sizeof(buf), "Adjustment for zeros %10.2f\n", dZero);
  out << buf;
  out << "                     ----------\n";
  std::snprintf(buf, sizeof(buf), "Adjusted variance   %11.2f\n", varAdj);
  out << buf;
  out << "\nH0: " << lhs << " = " << rhs << "\n";
  std::snprintf(buf, sizeof(buf), "         z = %6.3f\n", z);
  out << buf;
  std::snprintf(buf, sizeof(buf), "Prob > |z| = %.4f\n", p);
  out << buf;
  if (exact) {
    std::snprintf(buf, sizeof(buf), "Exact prob = %.4f\n", pExact);
    out << buf;
  }
  return displayBlock(out.str());
}

// centile: the centile and its confidence interval, [R] centile (Methods and
// formulas: the default binomial-interpolated interval, the exact binomial
// interval `cci`, and the two normal-theory forms `normal` and `meansd`). The
// behaviour is as measured on StataBE 19.5.
namespace {

enum class CentileMethod { Interpolated, Exact, Normal, MeanSd };

struct CentileRequest {
  std::vector<double> percents;  // as typed, duplicates and order kept
  double level = 95.0;
  CentileMethod method = CentileMethod::Interpolated;
};

struct CentileRow {
  double centile = std::numeric_limits<double>::quiet_NaN();
  double lower = std::numeric_limits<double>::quiet_NaN();
  double upper = std::numeric_limits<double>::quiet_NaN();
  bool held = false;  // a limit was held at the sample minimum or maximum
};

// A number in Stata's numlist spelling: `5`, `.25`, `2.50`, `1e1`, `-3`.
bool centileNumber(const std::string& tok, double* v) {
  static const std::regex kNum(R"(^[+-]?(\d+\.?\d*|\.\d+)([eE][+-]?\d+)?$)");
  if (!std::regex_match(tok, kNum)) return false;
  *v = std::strtod(tok.c_str(), nullptr);
  return std::isfinite(*v);
}

bool centileMissingLiteral(const std::string& tok) {
  return tok == "." || (tok.size() == 2 && tok[0] == '.' && tok[1] >= 'a' && tok[1] <= 'z');
}

// A plain decimal of up to 15 significant digits with no exponent, no leading
// zero before the point and no trailing zeros (`.25`, `33.3333`, `.00001`): the
// text r(centiles) and the header's level carry.
std::string centilePlainText(double v) {
  if (v == 0.0) return "0";
  char buf[64];
  std::snprintf(buf, sizeof(buf), "%.14e", std::fabs(v));
  const std::string s(buf);
  const size_t e = s.find('e');
  const int exp10 = std::atoi(s.c_str() + e + 1);
  std::string digits = s.substr(0, 1) + s.substr(2, e - 2);
  while (digits.size() > 1 && digits.back() == '0') digits.pop_back();
  const long point = static_cast<long>(exp10) + 1;  // digits before the point
  std::string out;
  if (point <= 0) {
    out = "." + std::string(static_cast<size_t>(-point), '0') + digits;
  } else if (static_cast<size_t>(point) >= digits.size()) {
    out = digits + std::string(static_cast<size_t>(point) - digits.size(), '0');
  } else {
    out = digits.substr(0, static_cast<size_t>(point)) + "." +
          digits.substr(static_cast<size_t>(point));
  }
  return v < 0 ? "-" + out : out;
}

// The centile() numlist: numbers separated by blanks or commas, and the range
// forms a(s)b, a[s]b, a/b and `x y to z` (step y - x). Each element must lie in
// [0, 100]. Refusals carry Stata's rc: 121 malformed, 127 missing, 125 range.
std::optional<ojson> parseCentileList(const std::string& text, std::vector<double>* out) {
  std::string spaced = text;
  for (char& ch : spaced)
    if (ch == ',') ch = ' ';
  std::vector<std::string> toks;
  {
    std::istringstream iss(spaced);
    std::string t;
    while (iss >> t) toks.push_back(t);
  }
  const auto invalid = [] {
    return aerErrorReason("centile() invalid -- invalid numlist", 121);
  };
  std::vector<double> vals;
  bool sawMissing = false;
  int plainRun = 0;  // single numbers seen since the last range
  for (size_t i = 0; i < toks.size(); ++i) {
    const std::string& t = toks[i];
    if (t == "to" || t == ":") {
      double end = 0.0;
      if (plainRun < 2 || i + 1 >= toks.size() || !centileNumber(toks[i + 1], &end))
        return invalid();
      const double from = vals[vals.size() - 1];
      const double step = from - vals[vals.size() - 2];
      if (step == 0.0) return invalid();
      char spec[128];
      std::snprintf(spec, sizeof(spec), "%.17g(%.17g)%s", from, step, toks[i + 1].c_str());
      std::vector<std::string> els;
      if (!loops::expandNumlist(spec, &els) || els.empty()) return invalid();
      for (size_t k = 1; k < els.size(); ++k) vals.push_back(std::strtod(els[k].c_str(), nullptr));
      ++i;
      plainRun = 0;
      continue;
    }
    if (t.find_first_of("([/") != std::string::npos) {
      std::string r = t;
      for (char& ch : r) {
        if (ch == '[') ch = '(';
        else if (ch == ']') ch = ')';
      }
      std::vector<std::string> els;
      if (!loops::expandNumlist(r, &els) || els.empty()) return invalid();
      for (const auto& el : els) vals.push_back(std::strtod(el.c_str(), nullptr));
      plainRun = 0;
      continue;
    }
    if (centileMissingLiteral(t)) {
      sawMissing = true;
      vals.push_back(std::numeric_limits<double>::quiet_NaN());
      plainRun = 0;
      continue;
    }
    double v = 0.0;
    if (!centileNumber(t, &v)) return invalid();
    vals.push_back(v);
    ++plainRun;
  }
  if (sawMissing)
    return aerErrorReason("centile() invalid -- invalid numlist has missing values", 127);
  for (double v : vals) {
    if (!(v >= 0.0 && v <= 100.0))
      return aerErrorReason(
          "centile() invalid -- invalid numlist has elements outside of allowed range", 125);
  }
  *out = std::move(vals);
  return std::nullopt;
}

// Option spellings, shortest first: Centile() CCi Normal Meansd Level() and the
// default's own name NOCCI. A bare `c` is centile(); cci needs `cc`.
std::string centileOptionName(const std::string& key) {
  const auto abbrevOf = [&key](const char* full, size_t minLen) {
    const std::string f(full);
    return key.size() >= minLen && key.size() <= f.size() && f.compare(0, key.size(), key) == 0;
  };
  if (abbrevOf("centile", 1)) return "centile";
  if (abbrevOf("cci", 2)) return "cci";
  if (key == "nocci") return "nocci";
  if (abbrevOf("normal", 1)) return "normal";
  if (abbrevOf("meansd", 1)) return "meansd";
  if (abbrevOf("level", 1)) return "level";
  return std::string();
}

std::optional<ojson> parseCentileOptions(const eval::ParsedCommand& parsed,
                                         const SessionState& session, CentileRequest* req) {
  std::vector<std::pair<std::string, eval::OptValue>> canon;
  canon.reserve(parsed.opts.size());
  for (const auto& [key, val] : parsed.opts) {
    const std::string name = centileOptionName(key);
    eval::OptValue v = val;
    if (name.empty()) v.exactOnly = true;
    canon.emplace_back(name.empty() ? key : name, v);
  }
  static const std::unordered_set<std::string> kKnown = {"centile", "cci",   "nocci",
                                                         "normal",  "meansd", "level"};
  if (auto e = eval::rejectUnknownOptions(canon, kKnown)) return ojson(*e);

  bool wantExact = false, wantNormal = false, wantMeanSd = false;
  std::optional<std::string> centileText, levelText;
  for (size_t i = 0; i < canon.size(); ++i) {
    const std::string& name = canon[i].first;
    const eval::OptValue& v = canon[i].second;
    const std::string& typed = parsed.opts[i].first;
    const bool valued = name == "centile" || name == "level";
    if (valued && v.isBool)
      return aerErrorReason("option " + typed + "() incorrectly specified", 198);
    if (!valued && !v.isBool)
      return aerErrorReason("option " + typed + "() incorrectly specified", 198);
    if (name == "centile") {
      if (centileText) return aerErrorReason("option " + typed + "() not allowed", 198);
      centileText = v.strValue;
    } else if (name == "level") {
      if (levelText) return aerErrorReason("option " + typed + "() not allowed", 198);
      levelText = v.strValue;
    } else if (name == "cci") {
      wantExact = true;
    } else if (name == "normal") {
      wantNormal = true;
    } else if (name == "meansd") {
      wantMeanSd = true;
    }
  }
  // meansd outranks normal, which outranks cci.
  req->method = wantMeanSd   ? CentileMethod::MeanSd
                : wantNormal ? CentileMethod::Normal
                : wantExact  ? CentileMethod::Exact
                             : CentileMethod::Interpolated;

  req->percents.clear();
  if (centileText) {
    if (auto e = parseCentileList(*centileText, &req->percents)) return e;
  }
  if (req->percents.empty()) req->percents.push_back(50.0);

  req->level = static_cast<double>(session.level);
  if (levelText) {
    std::string lt = *levelText;
    while (!lt.empty() && std::isspace(static_cast<unsigned char>(lt.back()))) lt.pop_back();
    while (!lt.empty() && std::isspace(static_cast<unsigned char>(lt.front()))) lt.erase(0, 1);
    double lv = 0.0;
    if (!centileNumber(lt, &lv)) {
      return aerErrorReason(
          (lt.empty() ? std::string("nothing") : lt) + " found where number expected", 198);
    }
    if (!(lv >= 10.0 && lv <= 99.99))
      return aerErrorReason("level() must be between 10 and 99.99 inclusive", 198);
    req->level = lv;
  }
  return std::nullopt;
}

// Cumulative binomial probabilities for S ~ Binomial(n, p):
//   below(i) = Pr(S <= i)       (F_i in [R] centile; F_-1 = 0, F_n = 1)
//   above(i) = Pr(S >= i + 1)   (1 - F_i, formed without the subtraction)
// For n up to kDirect the probability masses are summed from each end, which is
// exact for the small cases whose comparisons with alpha are ties (n = 3, p = .5,
// alpha = .125); beyond it the regularized incomplete beta gives each tail.
class BinomialTails {
 public:
  BinomialTails(long long n, double p) : n_(n), p_(p) {
    if (n_ > kDirect) return;
    const double q = 1.0 - p_;
    std::vector<double> mass(static_cast<size_t>(n_) + 1);
    double coef = 1.0;  // C(n, k)
    for (long long k = 0; k <= n_; ++k) {
      if (k > 0) coef = coef * static_cast<double>(n_ - k + 1) / static_cast<double>(k);
      mass[static_cast<size_t>(k)] = coef * std::pow(p_, static_cast<double>(k)) *
                                     std::pow(q, static_cast<double>(n_ - k));
    }
    below_.assign(static_cast<size_t>(n_) + 1, 0.0);
    above_.assign(static_cast<size_t>(n_) + 1, 0.0);
    double run = 0.0;
    for (long long k = 0; k <= n_; ++k) {
      run += mass[static_cast<size_t>(k)];
      below_[static_cast<size_t>(k)] = run;
    }
    run = 0.0;
    for (long long k = n_; k >= 1; --k) {
      run += mass[static_cast<size_t>(k)];
      above_[static_cast<size_t>(k - 1)] = run;  // Pr(S >= k) = above(k - 1)
    }
  }

  double below(long long i) const {
    if (i < 0) return 0.0;
    if (i >= n_) return 1.0;
    if (n_ <= kDirect) return below_[static_cast<size_t>(i)];
    return cecon::betaitail(static_cast<double>(i + 1), static_cast<double>(n_ - i), p_);
  }
  double above(long long i) const {
    if (i < 0) return 1.0;
    if (i >= n_) return 0.0;
    if (n_ <= kDirect) return above_[static_cast<size_t>(i)];
    return cecon::betai(static_cast<double>(i + 1), static_cast<double>(n_ - i), p_);
  }

 private:
  static constexpr long long kDirect = 300;
  long long n_;
  double p_;
  std::vector<double> below_, above_;
};

// x_i of the sorted sample, 1-based, with x_0 = x_1 and x_{n+1} = x_n: a limit
// never leaves [min, max].
double orderStat(const std::vector<double>& x, long long i) {
  if (i < 1) return x.front();
  if (i > static_cast<long long>(x.size())) return x.back();
  return x[static_cast<size_t>(i - 1)];
}

// [R] centile, Methods and formulas, "Default case": R = (n+1)p, the centile
// interpolates between x_r and x_{r+1} at the fraction f = R - int(R).
double interpolatedCentile(const std::vector<double>& x, double p) {
  const double R = static_cast<double>(x.size() + 1) * p;
  const double r = std::floor(R);
  const double f = R - r;
  const long long ri = static_cast<long long>(r);
  const double lo = orderStat(x, ri), hi = orderStat(x, ri + 1);
  return lo + f * (hi - lo);
}

CentileRow centileFor(const std::vector<double>& x, double mean, double sd, double pct,
                      const CentileRequest& req) {
  CentileRow row;
  const long long n = static_cast<long long>(x.size());
  const double p = pct / 100.0;
  const double alpha = (100.0 - req.level) / 200.0;
  const double nan = std::numeric_limits<double>::quiet_NaN();

  if (req.method == CentileMethod::MeanSd) {
    // c = mean + z_p * s; se = s * sqrt(1/n + z_p^2 / (2n - 2)).
    const double zp = cecon::stata_invnormal(p);
    if (n < 2 || std::isnan(zp) || std::isnan(sd)) return row;
    const double zc = cecon::stata_invnormal(1.0 - alpha);
    const double nd = static_cast<double>(n);
    row.centile = mean + zp * sd;
    const double se = sd * std::sqrt(1.0 / nd + zp * zp / (2.0 * nd - 2.0));
    row.lower = row.centile - zc * se;
    row.upper = row.centile + zc * se;
    return row;
  }

  row.centile = interpolatedCentile(x, p);

  if (req.method == CentileMethod::Normal) {
    // Kendall and Stuart: se = sqrt(p(1-p)/n) / f(c), f the normal density with
    // the sample mean and standard deviation.
    if (n < 2 || std::isnan(sd) || !(sd > 0.0)) {
      row.lower = row.upper = nan;
      return row;
    }
    const double dens = cecon::stata_normalden(row.centile, mean, sd);
    const double se = std::sqrt(p * (1.0 - p) / static_cast<double>(n)) / dens;
    const double zc = cecon::stata_invnormal(1.0 - alpha);
    row.lower = row.centile - zc * se;
    row.upper = row.centile + zc * se;
    if (!std::isfinite(row.lower) || !std::isfinite(row.upper)) row.lower = row.upper = nan;
    return row;
  }

  // Binomial limits. t: F_t <= alpha < F_{t+1}, t in -1..n-1.
  //                  u: 1 - F_u <= alpha < 1 - F_{u-1}, u in 0..n.
  const BinomialTails tails(n, p);
  long long lo = -1, hi = n;  // below(lo) <= alpha < below(hi)
  while (hi - lo > 1) {
    const long long mid = lo + (hi - lo) / 2;
    if (tails.below(mid) <= alpha) lo = mid;
    else hi = mid;
  }
  const long long t = lo;
  long long ulo = -1, uhi = n;  // above(ulo) > alpha >= above(uhi)
  while (uhi - ulo > 1) {
    const long long mid = ulo + (uhi - ulo) / 2;
    if (tails.above(mid) <= alpha) uhi = mid;
    else ulo = mid;
  }
  const long long u = uhi;

  if (req.method == CentileMethod::Exact) {
    row.lower = orderStat(x, t + 1);
    row.upper = orderStat(x, u + 1);
  } else {
    const double Ft = tails.below(t), Ft1 = tails.below(t + 1);
    const double g = (alpha - Ft) / (Ft1 - Ft);
    const double a1 = orderStat(x, t + 1), a2 = orderStat(x, t + 2);
    row.lower = a1 + g * (a2 - a1);
    const double Tu = tails.above(u), Tu1 = tails.above(u - 1);
    const double h = (alpha - Tu) / (Tu1 - Tu);
    const double b1 = orderStat(x, u + 1), b0 = orderStat(x, u);
    row.upper = b1 - h * (b1 - b0);
  }
  row.held = (t == -1) || (u == n);
  return row;
}

// A value cell: %.7g right-aligned in `width`, `.` for a missing value.
std::string centileCell(double v, int width) {
  char buf[64];
  if (std::isnan(v)) {
    std::snprintf(buf, sizeof(buf), "%*s", width, ".");
  } else {
    std::snprintf(buf, sizeof(buf), "%*.7g", width, v);
  }
  return buf;
}

}  // namespace

ojson runCentile(const eval::ParsedCommand& parsed, SessionState& session) {
  CentileRequest req;
  if (auto e = parseCentileOptions(parsed, session, &req)) return *e;

  // The variables: the varlist as typed (wildcards, ranges, abbreviations), or
  // every variable in dataset order.
  std::vector<std::string> vars;
  if (!parsed.varlist.empty()) {
    std::string spec;
    for (const auto& v : parsed.varlist) spec += (spec.empty() ? "" : " ") + v;
    loops::VarlistExpandResult ex = loops::expandVarlist(spec, session);
    if (ex.isError) return ex.error;
    vars = std::move(ex.tokens);
    if (!session.hasData()) {
      return aerErrorReason("variable " + (vars.empty() ? spec : vars.front()) + " not found",
                            111);
    }
    if (auto e = loops::checkKnownVarlist(vars, session)) return *e;
  } else if (session.hasData()) {
    if (const auto* card = session.residentReadCard()) {
      for (const auto& vi : *card) vars.push_back(vi.name);
    } else if (Datastore* ds = session.datastore()) {
      vars = ds->names();
    }
  }

  struct Printed {
    std::string name;
    long long n = 0;
    std::vector<CentileRow> rows;
  };
  std::vector<Printed> results;
  if (!vars.empty()) {
    ColumnScope scope(session, parsed, vars, "centile");
    if (scope.failed()) return scope.error();
    Datastore& ds = *scope.get();
    for (const auto& name : vars) {
      if (!ds.has(name)) return aerErrorReason("variable " + name + " not found", 111);
      if (ds.dtype(name) == "factor") continue;  // a string variable is skipped
      std::vector<double> col;
      std::string err;
      if (!numericColumn(ds, name, &col, &err)) return aerError("centile: " + err);
      std::vector<double> x;
      x.reserve(col.size());
      for (double v : col)
        if (std::fabs(v) < eval::kStataMissingSentinel) x.push_back(v);
      std::sort(x.begin(), x.end());
      Printed pr;
      pr.name = name;
      pr.n = static_cast<long long>(x.size());
      if (!x.empty()) {
        cecon::ExactSum sum;
        for (double v : x) sum.add(v);
        const double mean = sum.value() / static_cast<double>(x.size());
        double sd = std::numeric_limits<double>::quiet_NaN();
        if (x.size() > 1) {
          cecon::ExactSqDev dev(mean);
          for (double v : x) dev.add(v);
          sd = std::sqrt(dev.value() / static_cast<double>(x.size() - 1));
        }
        for (double pct : req.percents) pr.rows.push_back(centileFor(x, mean, sd, pct, req));
      }
      results.push_back(std::move(pr));
    }
  }

  // r(): replaced wholesale. Posting order makes `return list` read n_cent, N,
  // then ub_k lb_k c_k ... ub_1 lb_1 c_1.
  eval::replaceRClass(session.eval);
  const size_t k = req.percents.size();
  std::vector<std::string> posted = {"N", "n_cent", "centiles"};
  const auto rval = [](double v) { return std::isnan(v) ? eval::kStataMissingSentinel : v; };
  if (!results.empty()) {
    const Printed& last = results.back();
    for (size_t j = 0; j < k; ++j) {
      const std::string s = std::to_string(j + 1);
      const CentileRow row = j < last.rows.size() ? last.rows[j] : CentileRow();
      session.eval.rValues.set("c_" + s, rval(row.centile));
      session.eval.rValues.set("lb_" + s, rval(row.lower));
      session.eval.rValues.set("ub_" + s, rval(row.upper));
      posted.insert(posted.end(), {"c_" + s, "lb_" + s, "ub_" + s});
    }
  }
  session.eval.rValues.set("N", results.empty() ? 0.0 : static_cast<double>(results.back().n));
  session.eval.rValues.set("n_cent", static_cast<double>(k));
  {
    std::string list;
    for (double pct : req.percents) list += (list.empty() ? "" : " ") + centilePlainText(pct);
    session.eval.rStrings.set("centiles", list);
  }
  eval::declareRPosted(session.eval, "centile", std::move(posted));

  // The table.
  std::ostringstream out;
  out << "\n";
  switch (req.method) {
    case CentileMethod::Interpolated:
      out << std::string(58, ' ') << "Binom. interp.   \n";
      break;
    case CentileMethod::Exact:
      out << std::string(58, ' ') << "Binomial exact   \n";
      break;
    case CentileMethod::Normal:
      out << std::string(35, ' ') << "-- Normal, based on observed centiles --\n";
      break;
    case CentileMethod::MeanSd:
      out << std::string(35, ' ') << "-- Normal, based on mean and std. dev.--\n";
      break;
  }
  const std::string head = "    Variable |       Obs  Percentile    Centile";
  const std::string ci = "[" + centilePlainText(req.level) + "% conf. interval]";
  out << head << std::string(ci.size() < 28 ? 28 - ci.size() : 1, ' ') << ci << "\n";
  out << std::string(13, '-') << "+" << std::string(61, '-') << "\n";

  bool anyHeld = false, lastHeld = false;
  char buf[64];
  for (const Printed& pr : results) {
    const std::string stub = fitLeft(abbrevCp(pr.name, 12), 12);
    if (pr.n == 0) {
      std::snprintf(buf, sizeof(buf), "%8lld", pr.n);
      out << stub << " |" << buf << "\n";
      lastHeld = false;
      continue;
    }
    for (size_t j = 0; j < pr.rows.size(); ++j) {
      const CentileRow& row = pr.rows[j];
      if (j == 0) {
        std::snprintf(buf, sizeof(buf), "%10lld", pr.n);
        out << stub << " |" << buf;
      } else {
        out << std::string(12, ' ') << " |" << std::string(10, ' ');
      }
      out << " " << centileCell(req.percents[j], 10) << " " << centileCell(row.centile, 11) << " "
          << centileCell(row.lower, 15) << " " << centileCell(row.upper, 11);
      const bool held = row.held &&
                        (req.method == CentileMethod::Interpolated ||
                         req.method == CentileMethod::Exact);
      if (held) out << "*";
      out << "\n";
      anyHeld = anyHeld || held;
      lastHeld = held;
    }
  }
  if (anyHeld) {
    out << "\n"
        << (lastHeld ? "*" : "")
        << " Lower (upper) confidence limit held at minimum (maximum) of sample\n";
  }
  return displayBlock(out.str());
}

// ===========================================================================
// E2 engine-backed — H1 has landed, so these now compute.
//
// Each still ASKS the engine whether its subcommand exists before using it: a
// PIET built against an older cobra says so by name rather than failing
// somewhere less legible.

namespace {

// mintScratchPath always names a ".hbin"; a setcol payload is a CSV. Same body
// and same reason as handlers_aer_results.cpp's copy -- registering the minted
// .hbin and then writing a .csv beside it would leave the file that actually
// exists unregistered, which is the orphan class the registry exists to close.
std::string estimMintCsv(const std::string& workdir, const std::string& prefix) {
  const std::string hbin = mintScratchPath(workdir, prefix);
  forgetScratchMint(hbin);
  const std::string csv = hbin.substr(0, hbin.size() - 5) + ".csv";
  registerScratchMint(csv);
  return csv;
}

// The headerless one-value-per-row payload `cobra setcol` reads. An EMPTY line
// is how a missing value is spelled there; "nan" would land a literal token in
// a numeric column.
bool estimWriteCsv(const std::string& path, const std::vector<double>& v) {
  std::ofstream f(cecon::to_native_path(path));
  if (!f) return false;
  f.setf(std::ios::fmtflags(0), std::ios::floatfield);
  f.precision(17);
  for (double d : v) {
    if (!std::isnan(d)) f << d;
    f << "\n";
  }
  return static_cast<bool>(f);
}

// Write one f64 column into the session frame.
bool estimWriteColumn(SessionState& session, const std::string& name,
                      const std::vector<double>& vals, ojson* err) {
  const std::string csv = estimMintCsv(session.workdir(), "aerestim");
  if (!estimWriteCsv(csv, vals)) {
    *err = aerError("could not write the values payload", "data_io");
    return false;
  }
  const std::string out = mintScratchPath(session.workdir(), "aerestim");
  const CobraRunResult r = runCobra({"setcol", "--data", *session.currentHbin(), "--var", name,
                                     "--values", csv, "--dtype", "f64", "--new", "--out", out});
  if (!r.ok) {
    *err = aerError("could not create " + name, "data_io");
    return false;
  }
  // The engine's writer creates --out before it can detect a write failure, and
  // a full scratch volume has reached PIET as a ZERO exit code twice. A
  // session.load on a path that is not there records the head anyway and hands
  // the next open a missing mapping -- a crash rather than a diagnostic.
  if (!cecon::native_exists(out)) {
    *err = aerError("failed writing output dataset (disk full or I/O error)", "data_io");
    return false;
  }
  session.load(out);
  return true;
}

std::vector<double> jsonDoubles(const nlohmann::json& j) {
  std::vector<double> v;
  if (!j.is_array()) return v;
  v.reserve(j.size());
  for (const auto& e : j)
    v.push_back(e.is_number() ? e.get<double>() : std::numeric_limits<double>::quiet_NaN());
  return v;
}

}  // namespace

// `tsfilter hp NEW = Y [, smooth(#) trend(NEW2)]`
ojson runTsfilterHp(const eval::ParsedCommand& parsed, SessionState& session) {
  if (!engineHasSubcommand("hpfilter")) return engineTooOld("tsfilter hp", "hpfilter");
  if (!session.hasData() || !session.currentHbin().has_value()) return ojson(eval::noDataLoaded());

  // varlist is `hp NEW = Y`; the filter word was already checked by the arm.
  std::string cycleName, yname;
  {
    std::string joined;
    for (std::size_t i = 1; i < parsed.varlist.size(); ++i) joined += parsed.varlist[i] + " ";
    const std::size_t eq = joined.find('=');
    if (eq == std::string::npos)
      return aerSyntaxError("tsfilter hp: syntax is `tsfilter hp newvar = varname`");
    cycleName = strutil::trim(joined.substr(0, eq));
    yname = strutil::trim(joined.substr(eq + 1));
  }
  if (cycleName.empty() || yname.empty())
    return aerSyntaxError("tsfilter hp: syntax is `tsfilter hp newvar = varname`");

  std::string trendName, smooth;
  for (const auto& [k, v] : parsed.opts) {
    if (k == "trend") trendName = v.strValue;
    else if (k == "smooth") smooth = v.strValue;
  }
  // tsset/xtset is required: Stata's tsfilter is r(111) without one.
  if (!session.timevar.has_value())
    return aerErrorReason("tsfilter hp: you must tsset your data first", 111);

  std::vector<std::string> args = {"hpfilter", "--data", *session.currentHbin(), "--y", yname};
  if (!smooth.empty()) { args.push_back("--lambda"); args.push_back(smooth); }
  else {
    // The Ravn-Uhlig default is a function of the TIME-UNIT FORMAT, not a
    // constant 1600 -- %tw is 33,177,600 and %td is 110,930,628,906.25.
    // The display format is carried on the varLabels plane under a reserved
    // prefix, not on a named accessor.
    const std::string fmt = session.eval.varLabels.get(
        std::string(SessionState::kEvalVarFormatPrefix) + *session.timevar, "");
    if (!fmt.empty()) { args.push_back("--tsfmt"); args.push_back(fmt); }
  }
  if (session.panelvar.has_value()) { args.push_back("--panel"); args.push_back(*session.panelvar); }

  nlohmann::json raw;
  try {
    raw = runCobraJson(args);
  } catch (const CobraError& ce) {
    return aerError("tsfilter hp: " + cobraErrorMessage(ce), ce.code);
  }

  ojson err;
  if (!estimWriteColumn(session, cycleName, jsonDoubles(raw["cycle"]), &err)) return err;
  if (!trendName.empty() && !estimWriteColumn(session, trendName, jsonDoubles(raw["trend"]), &err))
    return err;

  session.eval.rValues.set("smooth", raw.value("lambda", 1600.0));
  session.eval.rStrings.set("method", "Hodrick-Prescott");
  session.eval.rStrings.set("varlist", yname);
  session.eval.rStrings.set("filterlist", cycleName);
  if (!trendName.empty()) session.eval.rStrings.set("trendlist", trendName);

  // tsfilter is otherwise SILENT; a short panel is the one exception, and it is
  // a note rather than a refusal (measured: rc 0, that panel left missing).
  std::string noteText;
  if (raw.contains("notes") && raw["notes"].is_array())
    for (const auto& n : raw["notes"]) noteText += "note: " + n.get<std::string>() + "\n";
  ojson mut = mutationResult(session, "tsfilter",
                              trendName.empty() ? std::vector<std::string>{cycleName}
                                                : std::vector<std::string>{cycleName, trendName},
                              {});
  if (noteText.empty()) return mut;
  ojson arr = ojson::array();
  arr.push_back(displayBlock(noteText));
  arr.push_back(mut);
  return arr;
}

// ---------------------------------------------------------------------------
// lpoly / lowess (goal gfx-lpoly-lowess, oracle aer_w2:w2ae_lpoly_lowess)
//
// The numbers come from the engine; what lives here is the command layer: option
// validation with Stata's return codes, the if/in/weight sample handed to the
// engine as a MASK (rows keep their positions), generate() written back by row,
// r(), and the graph block when the graph is drawn.

namespace {

// A title-like option's text: `"a" "b", size(small)` -> "a b". The part after
// the first top-level comma is suboptions (size, color, ...), which the block
// cannot express; several quoted strings are Stata's multi-line form.
std::string smoothUnquote(std::string s) {
  s = strutil::trim(s);
  std::string head;
  int depth = 0;
  bool inQ = false;
  for (char c : s) {
    if (c == '"') inQ = !inQ;
    else if (!inQ && c == '(') ++depth;
    else if (!inQ && c == ')') --depth;
    else if (!inQ && depth == 0 && c == ',') break;
    head += c;
  }
  head = strutil::trim(head);
  if (head.find('"') == std::string::npos) return head;
  std::string out;
  std::size_t i = 0;
  while ((i = head.find('"', i)) != std::string::npos) {
    const std::size_t j = head.find('"', i + 1);
    if (j == std::string::npos) break;
    if (!out.empty()) out += " ";
    out += head.substr(i + 1, j - i - 1);
    i = j + 1;
  }
  return out;
}

// `confirm new var NAME`: r(110) when it exists, r(198) when it is not a name.
std::optional<ojson> smoothConfirmNew(SessionState& session, const std::string& name) {
  if (!strutil::isValidStataIdentifier(name)) return aerErrorReason(name + " invalid name", 198);
  // BUG-1120 review: `lowess y x, gen(_cons)', `lpoly y x, gen(int g)' are "NAME
  // invalid varname" r(198) (StataBE 19.5).
  if (auto r = strutil::newVarNameRefusal(name)) return aerErrorReason(r->message, r->rc);
  Datastore* ds = session.datastore();
  if (ds != nullptr && ds->has(name)) return aerErrorReason("variable " + name + " already defined", 110);
  return std::nullopt;
}

bool smoothIsNumeric(SessionState& session, const std::string& name) {
  Datastore* ds = session.datastore();
  if (ds == nullptr || !ds->has(name)) return false;
  const std::string t = ds->dtype(name);
  return t != "factor" && t.rfind("str", 0) != 0;
}

// The if/in qualifiers as engine flags. `in` is resolved here to literal rows.
std::optional<ojson> smoothSampleArgs(const eval::ParsedCommand& parsed, SessionState& session,
                                      const std::string& cmd, std::vector<std::string>* args) {
  if (parsed.ifExpr.has_value() && !parsed.ifExpr->empty()) {
    args->push_back("--keepif");
    args->push_back(*parsed.ifExpr);
  }
  if (parsed.inRange.has_value() && !parsed.inRange->empty()) {
    Datastore* ds = session.datastore();
    InRangeBounds rr = parseInRangeBounds(*parsed.inRange, ds != nullptr ? ds->nRows() : 0);
    if (!rr.ok) {
      InRangeFilterResult bad;
      bad.ok = false;
      bad.negative = rr.negative;
      bad.refusal = rr.refusal;
      return inRangeRejectError(bad, *parsed.inRange, cmd);
    }
    args->push_back("--inrange");
    args->push_back(std::to_string(rr.a) + "/" + std::to_string(rr.b));
  }
  return std::nullopt;
}

// An engine refusal carrying one of the return codes Stata's lpoly gives.
ojson smoothEngineError(const std::string& cmd, const CobraError& ce) {
  const std::string msg = cobraErrorMessage(ce);
  if (msg.find("insufficient observations") != std::string::npos)
    return aerErrorReason("insufficient observations", 2001);
  if (msg.find("no observations") != std::string::npos) return aerErrorReason("no observations", 2000);
  if (msg.find("negative weights encountered") != std::string::npos)
    return aerErrorReason("negative weights encountered", 402);
  return aerError(cmd + ": " + msg, ce.code);
}

// Write a generated column as Stata's lpoly/lowess create it: `gen double`,
// with its variable label (empty = none).
bool smoothWriteColumn(SessionState& session, const std::string& name, const std::vector<double>& v,
                       const std::string& label, ojson* err) {
  if (!estimWriteColumn(session, name, v, err)) return false;
  session.implicitFloatVars.erase(name);
  session.varMeta[name].stataType = "double";
  if (!label.empty()) session.setVarLabel(name, label);
  return true;
}

std::string smoothVarTitle(SessionState& session, const std::string& v) {
  session.syncEvalVarLabels();
  std::string l = session.eval.varLabels.get(v, std::string());
  return l.empty() ? v : l;
}

// The RIGHTMOST occurrence: a repeated option is Stata's "last one wins".
const eval::OptValue* smoothOpt(const eval::ParsedCommand& parsed, const std::string& key) {
  const eval::OptValue* found = nullptr;
  for (const auto& kv : parsed.opts)
    if (kv.first == key) found = &kv.second;
  return found;
}

// Stata's string(round(x, 0.01)), the form of lpoly's graph note.
std::string smoothRound2(double v) {
  char buf[64];
  std::snprintf(buf, sizeof buf, "%.2f", std::round(v * 100.0) / 100.0);
  std::string s = buf;
  while (!s.empty() && s.back() == '0') s.pop_back();
  if (!s.empty() && s.back() == '.') s.pop_back();
  if (s.rfind("0.", 0) == 0) s = s.substr(1);
  if (s.rfind("-0.", 0) == 0) s = "-" + s.substr(2);
  return s;
}

// The drawn graph: the scatter of the sample (unless noscatter), the CI band
// (lpoly ci) and the smooth line, with Stata's default title/axis titles/
// note, each replaced by the user's option. Every option this does not read is
// named by the dispatcher's graphAcceptedNotApplied note.
ojson smoothGraphBlock(const eval::ParsedCommand& parsed, SessionState& session,
                       const std::string& defTitle, const std::string& defNote,
                       const std::string& yname, const std::string& xname,
                       const std::vector<std::pair<double, double>>& scatterPts,
                       const std::vector<std::array<double, 3>>& band,
                       const std::vector<std::pair<double, double>>& linePts) {
  static const std::vector<std::pair<std::string, eval::OptValue>> kNoLayerOpts;
  ojson layers = ojson::array();
  double xMin = std::numeric_limits<double>::infinity(), xMax = -xMin;
  double yMin = xMin, yMax = -xMin;
  auto see = [&](double x, double y) {
    xMin = std::min(xMin, x);
    xMax = std::max(xMax, x);
    yMin = std::min(yMin, y);
    yMax = std::max(yMax, y);
  };
  if (!band.empty()) {
    ojson b = ojson::array();
    for (const auto& r : band) {
      b.push_back(ojson::array({r[0], r[1], r[2]}));
      see(r[0], r[1]);
      see(r[0], r[2]);
    }
    ojson lyr;
    lyr["kind"] = "rarea";
    lyr["band"] = b;
    lyr["style"] = twoway::layerStyle(kNoLayerOpts, static_cast<int>(layers.size()), "rarea");
    lyr["n"] = static_cast<int64_t>(band.size());
    layers.push_back(lyr);
  }
  if (!scatterPts.empty()) {
    ojson p = ojson::array();
    for (const auto& [x, y] : scatterPts) {
      p.push_back(ojson::array({x, y}));
      see(x, y);
    }
    ojson lyr;
    lyr["kind"] = "scatter";
    lyr["points"] = p;
    lyr["style"] = twoway::layerStyle(kNoLayerOpts, static_cast<int>(layers.size()), "scatter");
    lyr["n"] = static_cast<int64_t>(scatterPts.size());
    layers.push_back(lyr);
  }
  {
    ojson p = ojson::array();
    for (const auto& [x, y] : linePts) {
      p.push_back(ojson::array({x, y}));
      see(x, y);
    }
    ojson lyr;
    lyr["kind"] = "line";
    lyr["points"] = p;
    lyr["style"] = twoway::layerStyle(kNoLayerOpts, static_cast<int>(layers.size()), "line");
    lyr["n"] = static_cast<int64_t>(linePts.size());
    layers.push_back(lyr);
  }
  if (!std::isfinite(xMin)) {
    xMin = 0.0;
    xMax = 1.0;
    yMin = 0.0;
    yMax = 1.0;
  }
  auto optText = [&](const std::string& key, const std::string& dflt) {
    const eval::OptValue* v = smoothOpt(parsed, key);
    if (v == nullptr || v->isBool) return dflt;
    return smoothUnquote(v->strValue);
  };
  ojson block;
  block["type"] = "graph";
  block["kind"] = "twoway";
  block["title"] = optText("title", defTitle);
  block["subtitle"] = std::string();
  block["x_label"] = optText("xtitle", smoothVarTitle(session, xname));
  block["y_label"] = optText("ytitle", smoothVarTitle(session, yname));
  block["note"] = optText("note", defNote);
  block["x_min"] = xMin;
  block["x_max"] = xMax;
  block["y_min"] = yMin;
  block["y_max"] = yMax;
  block["layers"] = layers;
  ojson axes;
  axes["xline"] = ojson::array();
  axes["yline"] = ojson::array();
  axes["xscale_noline"] = false;
  axes["yscale_noline"] = false;
  axes["xlabel_none"] = false;
  axes["ylabel_none"] = false;
  block["axes"] = axes;
  ojson legend;
  legend["order"] = ojson::array();
  legend["cols"] = nullptr;
  legend["size"] = nullptr;
  legend["off"] = true;  // Stata draws no legend unless ci or legend() (noted, not applied)
  block["legend"] = legend;
  block["name"] = nullptr;  // filled by the dispatcher's name() capture (BS-011)
  block["n_obs"] = static_cast<int64_t>(scatterPts.size());
  block["suppressed"] = session.graphicsOff;
  return block;
}

}  // namespace

namespace {

bool smoothCellMissing(double v) { return std::isnan(v) || std::fabs(v) >= eval::kStataMissingSentinel; }

// The option spelled as at least `atLeast` leading characters of `full`; a repeated
// option answers with its FIRST occurrence (what both commands' computations use,
// measured: `lowess y x, bw(0.5) bw(0.4)` smooths with 0.5).
const eval::OptValue* smoothFirstOpt(const eval::ParsedCommand& parsed, const std::string& full, size_t atLeast) {
  for (const auto& [key, value] : parsed.opts)
    if (key.size() >= atLeast && key.size() <= full.size() && full.compare(0, key.size(), key) == 0) return &value;
  return nullptr;
}

std::string smoothOptText(const eval::OptValue* option) {
  return option == nullptr || option->isBool ? std::string() : strutil::trim(option->strValue);
}

std::vector<std::string> smoothBlankWords(const std::string& text) {
  std::istringstream in(text);
  std::vector<std::string> words;
  std::string word;
  while (in >> word) words.push_back(word);
  return words;
}

// A number written as a number: digits, sign, point and exponent only, so `abc`,
// `.`, `0.5+0.1`, `inf` and hex forms are not numbers here.
bool smoothPlainNumber(const std::string& text, double* value) {
  if (text.empty() || text.find_first_not_of("0123456789+-.eE") != std::string::npos) return false;
  char* stop = nullptr;
  *value = std::strtod(text.c_str(), &stop);
  return stop != text.c_str() && *stop == '\0';
}

bool smoothHasOperator(const std::string& token) { return token.find('.') != std::string::npos; }

bool smoothColumnExists(SessionState& session, const std::string& name) {
  Datastore* ds = session.datastore();
  return ds != nullptr && ds->has(name);
}

std::optional<ojson> smoothBindYorX(SessionState& session, const std::string& token, std::string* bound) {
  bool ambiguous = false;
  const std::optional<std::string> full = resolveVarName(token, session, &ambiguous);
  if (ambiguous) return aerErrorReason(token + " ambiguous abbreviation", 111);
  if (!full) return aerErrorReason("variable " + token + " not found", 111);
  if (!smoothIsNumeric(session, *full))
    return aerErrorReason("string variables not allowed in varlist;\n" + *full + " is a string variable", 109);
  *bound = *full;
  return std::nullopt;
}

std::optional<ojson> smoothDrawnNet(const eval::ParsedCommand& parsed, const std::string& cmd) {
  const eval::OptVocab* vocab = eval::findOptVocab(cmd);
  if (vocab == nullptr) return std::nullopt;
  if (auto bad = eval::rejectUnknownOptions(parsed.opts, vocab->known, vocab->unimplemented)) return ojson(*bad);
  return std::nullopt;
}

// N, sum_w, sum, mean, min, max: what `summarize, meanonly` leaves, posted over the
// rows `keep` admits (an empty mask admits every row).
std::vector<std::string> smoothPostSummary(SessionState& session, const std::vector<double>& values,
                                           const std::vector<int>& keep) {
  const double absent = std::numeric_limits<double>::quiet_NaN();
  cecon::ExactSum total;
  double count = 0.0;
  double lowest = absent, highest = absent;
  for (size_t r = 0; r < values.size(); ++r) {
    if ((!keep.empty() && keep[r] != 1) || smoothCellMissing(values[r])) continue;
    total.add(values[r]);
    lowest = count == 0.0 ? values[r] : std::min(lowest, values[r]);
    highest = count == 0.0 ? values[r] : std::max(highest, values[r]);
    count += 1.0;
  }
  auto& out = session.eval.rValues;
  out.set("N", count);
  out.set("sum_w", count);
  out.set("sum", total.value());
  out.set("mean", count > 0.0 ? total.value() / count : absent);
  out.set("min", lowest);
  out.set("max", highest);
  return {"N", "sum_w", "sum", "mean", "min", "max"};
}

std::vector<int> smoothSampleMask(const nlohmann::json& reply) {
  std::vector<int> mask;
  if (reply.contains("touse") && reply["touse"].is_array())
    for (const auto& cell : reply["touse"]) mask.push_back(cell.is_number() ? cell.get<int>() : 0);
  return mask;
}

std::vector<std::pair<double, double>> smoothPairsSortedByX(std::vector<std::pair<double, double>> pairs) {
  std::stable_sort(pairs.begin(), pairs.end(),
                   [](const auto& left, const auto& right) { return left.first < right.first; });
  return pairs;
}

// The sample rows as (x, y) points.
bool smoothSamplePoints(SessionState& session, const std::string& yVar, const std::string& xVar,
                        const std::vector<int>& mask, std::vector<std::pair<double, double>>* points, ojson* failure) {
  Datastore* ds = session.datastore();
  std::vector<double> ys, xs;
  std::string why;
  if (ds == nullptr || !numericColumn(*ds, yVar, &ys, &why) || !numericColumn(*ds, xVar, &xs, &why)) {
    *failure = aerError(why.empty() ? std::string("no dataset") : why);
    return false;
  }
  for (size_t r = 0; r < ys.size() && r < mask.size(); ++r)
    if (mask[r] == 1) points->emplace_back(xs[r], ys[r]);
  return true;
}

// generate() as lowess reads it. One word must be new and a name; a reserved word
// gets past this and fails once the smooth is done. More than one word is refused
// here: on the first existing word, else on the second.
std::optional<ojson> lowessGenerateRefusal(SessionState& session, const std::vector<std::string>& words) {
  if (words.size() == 1) {
    if (!strutil::isValidStataIdentifier(words[0])) return aerErrorReason(words[0] + " invalid name", 198);
    if (smoothColumnExists(session, words[0])) return aerErrorReason("variable " + words[0] + " already defined", 110);
    return std::nullopt;
  }
  for (const std::string& word : words)
    if (smoothColumnExists(session, word)) return aerErrorReason("variable " + word + " already defined", 110);
  if (words.size() >= 2) return aerErrorReason(words[1] + " not allowed", 101);
  return std::nullopt;
}

// A smooth with no neighbours is left missing: [R] lowess, Methods and formulas, has
// the half-window k = int((N * bwidth - 0.5) / 2), and Stata returns all missing below 1.
void lowessBlankThinWindow(double rows, double bandwidth, std::vector<double>* smooth) {
  if (std::floor((rows * bandwidth - 0.5) / 2.0) < 1.0)
    std::fill(smooth->begin(), smooth->end(), std::numeric_limits<double>::quiet_NaN());
}

// `adjust` rescales to the mean of every nonmissing y in the dataset (if/in play no
// part), then `logit` moves the extreme proportions to 1/N and 1-1/N before taking the
// log odds.
void lowessRescale(const std::vector<double>& yAll, double rows, bool adjust, bool logit,
                   std::vector<double>* smooth) {
  if (adjust) {
    cecon::ExactSum yTotal, smoothTotal;
    double yCount = 0.0, smoothCount = 0.0;
    for (double v : yAll)
      if (!smoothCellMissing(v)) {
        yTotal.add(v);
        yCount += 1.0;
      }
    for (double v : *smooth)
      if (!smoothCellMissing(v)) {
        smoothTotal.add(v);
        smoothCount += 1.0;
      }
    const double ratio = (yTotal.value() / yCount) / (smoothTotal.value() / smoothCount);
    for (double& v : *smooth)
      if (!smoothCellMissing(v)) v *= ratio;
  }
  if (logit) {
    for (double& v : *smooth) {
      if (smoothCellMissing(v)) continue;
      const double p = v < 0.0001 ? 1.0 / rows : (v > 0.9999 ? 1.0 - 1.0 / rows : v);
      v = std::log(p / (1.0 - p));
    }
  }
}

// One entry of the lpoly kernel table: how few leading characters name it.
struct LpolyKernelSpelling {
  const char* full;
  size_t atLeast;
};
const LpolyKernelSpelling kLpolyKernelTable[] = {{"epanechnikov", 2}, {"epan2", 5},  {"biweight", 2},
                                                 {"cosine", 3},       {"gaussian", 3}, {"parzen", 3},
                                                 {"rectangle", 3},    {"triangle", 3}};

std::string lpolyKernelSpelled(const std::string& word) {
  for (const LpolyKernelSpelling& entry : kLpolyKernelTable) {
    const std::string full = entry.full;
    if (word.size() >= entry.atLeast && word.size() <= full.size() && full.compare(0, word.size(), word) == 0)
      return full;
  }
  return std::string();
}

// kernel(name) and the older bare-word spelling, which may not be combined.
std::optional<ojson> lpolyChooseKernel(const eval::ParsedCommand& parsed, std::string* kernel) {
  std::vector<std::string> bareWords;
  for (const auto& [key, value] : parsed.opts) {
    if (!value.isBool) continue;
    const std::string named = lpolyKernelSpelled(key);
    if (!named.empty()) bareWords.push_back(named);
  }
  const std::string typed = smoothOptText(smoothFirstOpt(parsed, "kernel", 1));
  if (bareWords.size() > 1) return aerErrorReason("only one kernel may be specified", 198);
  if (bareWords.size() == 1 && !typed.empty())
    return aerErrorReason("kernel(): old syntax may not be combined with new syntax", 198);
  *kernel = bareWords.empty() ? std::string("epanechnikov") : bareWords[0];
  if (typed.empty()) return std::nullopt;
  if (typed.find_first_of(" \t") != std::string::npos)
    return aerErrorReason("only one kernel may be specified", 198);
  *kernel = lpolyKernelSpelled(typed);
  if (kernel->empty()) return aerErrorReason("invalid kernel function", 198);
  return std::nullopt;
}

std::optional<ojson> lpolyNewNameRefusal(SessionState& session, const std::string& name) {
  if (!strutil::isValidStataIdentifier(name)) return aerErrorReason(name + " invalid name", 198);
  if (smoothColumnExists(session, name)) return aerErrorReason("variable " + name + " already defined", 110);
  return std::nullopt;
}

// A bwidth() that is not a number may only name a numeric at()-companion variable,
// and that form itself is still PIET's named gap (BUG-1313).
std::optional<ojson> lpolyBandwidthRefusal(SessionState& session, const std::string& text, bool hasAt) {
  if (text.empty()) return std::nullopt;
  double width = 0.0;
  if (smoothPlainNumber(text, &width)) {
    if (width <= 0.0) return aerErrorReason("bwidth() must be a positive number", 198);
    return std::nullopt;
  }
  bool ambiguous = false;
  const std::optional<std::string> variable = resolveVarName(text, session, &ambiguous);
  if (!variable) return aerErrorReason("variable " + text + " not found", 111);
  if (!hasAt) return aerErrorReason("variable name in bwidth() is allowed only with at() option", 198);
  if (!smoothIsNumeric(session, *variable))
    return aerErrorReason("bwidth(): " + text + " is not a numeric variable", 198);
  return aerErrorReason("lpoly: a variable in bwidth() is not implemented in this build", 198);
}

// A column lpoly will add, once the fit is in hand.
struct LpolyOutput {
  std::string name;
  std::string label;
  std::vector<double> values;
  int addOrder;
};

std::vector<double> lpolyRowLength(std::vector<double> values, size_t rows) {
  values.resize(rows, std::numeric_limits<double>::quiet_NaN());
  return values;
}

}  // namespace

// `lowess yvar xvar [if] [in] [, options]`. The checks, the bandwidth reading, the r()
// that remains and the graph text were measured on StataBE 19.5; the fit is the
// engine's.
ojson runLowess(const eval::ParsedCommand& parsed, SessionState& session) {
  if (!engineHasSubcommand("lowess")) return engineTooOld("lowess", "lowess");
  if (parsed.weightType.has_value()) return aerErrorReason("weights not allowed", 101);
  if (parsed.varlist.size() < 2) return aerErrorReason("too few variables specified", 102);
  if (parsed.varlist.size() > 2) return aerErrorReason("too many variables specified", 103);
  if (!session.hasData() || !session.currentHbin().has_value()) return ojson(eval::noDataLoaded());

  std::string bound[2];
  for (int i = 0; i < 2; ++i)
    if (!smoothHasOperator(parsed.varlist[static_cast<size_t>(i)]))
      if (auto bad = smoothBindYorX(session, parsed.varlist[static_cast<size_t>(i)], &bound[i])) return *bad;
  if (bound[0].empty() || bound[1].empty()) {
    if (!session.timevar.has_value() || session.timevar->empty())
      return aerErrorReason("time variable not set", 111);
    return aerErrorReason("lowess: time-series operators are not implemented in this build", 198);
  }
  const std::string& yVar = bound[0];
  const std::string& xVar = bound[1];

  if (smoothOpt(parsed, "by") != nullptr)
    return aerErrorReason("lowess: option by() is not implemented in this build", 198);
  const bool drawn = smoothFirstOpt(parsed, "nograph", 3) == nullptr;
  if (drawn) {
    for (const char* graphOnly : {"plot", "addplot"})
      if (smoothOpt(parsed, graphOnly) != nullptr)
        return aerErrorReason(std::string("lowess: option ") + graphOnly + "() is not implemented in this build", 198);
    if (auto bad = smoothDrawnNet(parsed, "lowess")) return *bad;
  }
  const std::vector<std::string> outWords = smoothBlankWords(smoothOptText(smoothFirstOpt(parsed, "generate", 3)));
  if (auto bad = lowessGenerateRefusal(session, outWords)) return *bad;

  const bool wantMean = smoothFirstOpt(parsed, "mean", 1) != nullptr;
  const bool wantLogit = smoothFirstOpt(parsed, "logit", 2) != nullptr;
  const bool wantAdjust = smoothFirstOpt(parsed, "adjust", 1) != nullptr;

  std::vector<std::string> args = {"lowess", "--data", *session.currentHbin(), "--y", yVar, "--x", xVar, "--emit-touse"};
  // Anything but a plain number means the default; the engine itself replaces a
  // number outside (0,1) with 0.8.
  const std::string widthText = smoothOptText(smoothFirstOpt(parsed, "bwidth", 2));
  double ignored = 0.0;
  if (smoothPlainNumber(widthText, &ignored)) {
    args.push_back("--bw");
    args.push_back(widthText);
  }
  if (wantMean) args.push_back("--mean");
  if (smoothFirstOpt(parsed, "noweight", 3) != nullptr) args.push_back("--noweight");
  if (auto bad = smoothSampleArgs(parsed, session, "lowess", &args)) return *bad;

  nlohmann::json reply;
  try {
    reply = runCobraJson(args);
  } catch (const CobraError& ce) {
    return smoothEngineError("lowess", ce);
  }
  const double sampleRows = static_cast<double>(reply.value("n_obs", 0));
  const double bandwidth = reply.value("bwidth", 0.8);
  const std::vector<int> mask = smoothSampleMask(reply);
  std::vector<double> smooth = jsonDoubles(reply["smooth"]);
  lowessBlankThinWindow(sampleRows, bandwidth, &smooth);

  std::vector<double> xColumn, yColumn;
  {
    Datastore* ds = session.datastore();
    std::string why;
    if (ds == nullptr) return ojson(eval::noDataLoaded());
    if (!numericColumn(*ds, xVar, &xColumn, &why) || !numericColumn(*ds, yVar, &yColumn, &why)) return aerError(why);
  }
  if (sampleRows > 0.0) lowessRescale(yColumn, sampleRows, wantAdjust, wantLogit, &smooth);

  // lowess is no r-class command, but what survives is the summary its last internal
  // step leaves: nothing when drawn; else the sample size alone after logit, y's
  // summary after adjust, x's over the sample otherwise.
  eval::replaceRClass(session.eval);
  std::vector<std::string> posted;
  if (!drawn) {
    if (sampleRows > 0.0 && wantLogit) {
      session.eval.rValues.set("N", sampleRows);
      posted = {"N"};
    } else if (sampleRows > 0.0 && wantAdjust) {
      posted = smoothPostSummary(session, yColumn, {});
    } else {
      posted = smoothPostSummary(session, xColumn, mask);
    }
  }
  eval::declareRPosted(session.eval, "lowess", posted);

  const std::string outName = outWords.empty() ? std::string() : outWords[0];
  if (!outName.empty())
    if (const auto reserved = strutil::newVarNameRefusal(outName)) return aerErrorReason(reserved->message, reserved->rc);

  ojson blocks = ojson::array();
  if (drawn) {
    std::vector<std::pair<double, double>> scatter;
    std::vector<std::pair<double, double>> line;
    for (size_t r = 0; r < smooth.size() && r < xColumn.size(); ++r)
      if (!smoothCellMissing(smooth[r])) line.emplace_back(xColumn[r], smooth[r]);
    for (size_t r = 0; !wantLogit && r < mask.size() && r < xColumn.size(); ++r)
      if (mask[r] == 1) scatter.emplace_back(xColumn[r], yColumn[r]);
    ojson graph = smoothGraphBlock(parsed, session, wantMean ? "Running mean smoother" : "Lowess smoother",
                                   "bandwidth = " + strutil::trim(eval::applyStataFmt(bandwidth, "%9.0g")), yVar, xVar,
                                   scatter, {}, smoothPairsSortedByX(std::move(line)));
    std::string subtitle = wantLogit ? "Logit transformed smooth" : "";
    if (wantAdjust) subtitle += (subtitle.empty() ? "" : " ") + std::string("Mean adjusted smooth");
    graph["subtitle"] = subtitle;
    graph["n_obs"] = static_cast<int64_t>(sampleRows);
    blocks.push_back(std::move(graph));
  }
  if (!outName.empty()) {
    ojson failure;
    const std::string label = drawn ? "lowess: " + smoothVarTitle(session, yVar) : std::string();
    if (!smoothWriteColumn(session, outName, smooth, label, &failure)) return failure;
    blocks.push_back(mutationResult(session, "lowess", {outName}, {}));
  }
  return blocks;
}

// `lpoly yvar xvar [if] [in] [aw] [, options]`. Same arrangement as lowess: the command
// layer was measured on StataBE 19.5, the local fits are the engine's.
ojson runLpoly(const eval::ParsedCommand& parsed, SessionState& session) {
  if (!engineHasSubcommand("lpoly")) return engineTooOld("lpoly", "lpoly");
  // lpoly is r-class: a refusal before the fit leaves r() known to be empty, so
  // reading a result then gives missing. A fit declares its names below.
  eval::replaceRClass(session.eval);
  eval::declareRPosted(session.eval, "lpoly", {});

  const std::string weightKind = parsed.weightType.value_or(std::string()).substr(0, 1);
  if (weightKind == "p") return aerErrorReason("pweights not allowed", 101);
  if (weightKind == "i") return aerErrorReason("iweights not allowed", 101);
  if (weightKind == "f")
    return aerErrorReason(
        "lpoly: fweights are not implemented in this build (how Stata counts r(N) and the rule-of-thumb n under "
        "fweights is unmeasured); [aw=] is",
        198);
  if (parsed.varlist.size() < 2) return aerErrorReason("too few variables specified", 102);
  if (parsed.varlist.size() > 2) return aerErrorReason("too many variables specified", 103);
  if (!session.hasData() || !session.currentHbin().has_value()) return ojson(eval::noDataLoaded());
  if (smoothHasOperator(parsed.varlist[0]) || smoothHasOperator(parsed.varlist[1]))
    return aerErrorReason("factor-variable and time-series operators not allowed", 101);
  std::string yVar, xVar;
  if (auto bad = smoothBindYorX(session, parsed.varlist[0], &yVar)) return *bad;
  if (auto bad = smoothBindYorX(session, parsed.varlist[1], &xVar)) return *bad;

  const std::string atText = smoothOptText(smoothFirstOpt(parsed, "at", 2));
  std::string atVar;
  if (!atText.empty()) {
    const std::optional<std::string> found = resolveVarName(atText, session);
    if (!found) return aerErrorReason("variable " + atText + " not found\n(error in option at())", 111);
    atVar = *found;
  }

  double confidence = 95.0;
  const std::string levelText = smoothOptText(smoothFirstOpt(parsed, "level", 1));
  if (!levelText.empty()) {
    if (!smoothPlainNumber(levelText, &confidence)) return aerErrorReason(levelText + " found where number expected", 198);
    if (confidence < 10.0 || confidence > 99.99)
      return aerErrorReason("level() must be between 10 and 99.99 inclusive", 198);
  }

  if (smoothOpt(parsed, "by") != nullptr) return aerErrorReason("option by() not allowed", 191);
  const std::vector<std::pair<std::string, size_t>> namedGaps = {{"var", 1}, {"generatevar", 9}, {"generateci", 9}};
  for (const auto& [gap, atLeast] : namedGaps)
    if (smoothFirstOpt(parsed, gap, atLeast) != nullptr)
      return aerErrorReason("lpoly: option " + gap + "() is not implemented in this build", 198);

  const std::string pilotText = smoothOptText(smoothFirstOpt(parsed, "pwidth", 2));
  const bool pilotGiven = smoothFirstOpt(parsed, "pwidth", 2) != nullptr;
  double numeric = 0.0;
  if (pilotGiven && (!smoothPlainNumber(pilotText, &numeric) || numeric <= 0.0))
    return aerErrorReason("pwidth() must be a positive number", 198);

  // degree() and n() read loosely: a non-integer or non-number means the default
  // (degree 0, the default grid), and n(1) too. Only a nonpositive number refuses.
  std::string degreeArg;
  const std::string degreeText = smoothOptText(smoothFirstOpt(parsed, "degree", 3));
  if (!degreeText.empty()) {
    degreeArg = "0";
    if (smoothPlainNumber(degreeText, &numeric)) {
      if (numeric < 0.0) return aerErrorReason("degree() must be a nonnegative number", 198);
      if (numeric == std::floor(numeric)) degreeArg = std::to_string(static_cast<long long>(numeric));
    }
  }
  std::string gridArg;
  const std::string gridText = smoothOptText(smoothFirstOpt(parsed, "n", 1));
  if (smoothPlainNumber(gridText, &numeric)) {
    if (numeric <= 0.0) return aerErrorReason("n() must be a positive number", 198);
    if (numeric == std::floor(numeric) && numeric >= 2.0) gridArg = std::to_string(static_cast<long long>(numeric));
  }

  std::string kernel;
  if (auto bad = lpolyChooseKernel(parsed, &kernel)) return *bad;

  if (!atVar.empty() && !smoothIsNumeric(session, atVar))
    return aerErrorReason("at(): " + atVar + " is not a numeric variable", 198);

  const std::vector<std::string> outWords = smoothBlankWords(smoothOptText(smoothFirstOpt(parsed, "generate", 3)));
  if (outWords.size() == 1 && atVar.empty())
    return aerErrorReason("at() must be specified for generate() to work with one variable", 198);
  if (outWords.size() > 2) return aerErrorReason("generate(): too many variables specified", 198);
  for (const std::string& word : outWords)
    if (auto bad = lpolyNewNameRefusal(session, word)) return *bad;

  const std::string seName = smoothOptText(smoothFirstOpt(parsed, "se", 2));
  if (!seName.empty()) {
    if (atVar.empty() && outWords.empty())
      return aerErrorReason("se() requires the specification of at() or generate()", 198);
    if (auto bad = lpolyNewNameRefusal(session, seName)) return *bad;
  }

  const std::string widthText = smoothOptText(smoothFirstOpt(parsed, "bwidth", 2));
  if (auto bad = lpolyBandwidthRefusal(session, widthText, !atVar.empty())) return *bad;

  const bool drawn = smoothFirstOpt(parsed, "nograph", 4) == nullptr;
  if (drawn)
    if (auto bad = smoothDrawnNet(parsed, "lpoly")) return *bad;

  const bool bandsInForce = smoothFirstOpt(parsed, "ci", 2) != nullptr || pilotGiven;
  const bool wantScatter = drawn && smoothFirstOpt(parsed, "noscatter", 4) == nullptr;
  std::vector<std::string> args = {"lpoly", "--data", *session.currentHbin(), "--y", yVar, "--x", xVar, "--kernel", kernel};
  auto addFlag = [&args](const char* flag, const std::string& value) {
    if (value.empty()) return;
    args.push_back(flag);
    args.push_back(value);
  };
  addFlag("--bw", widthText);
  addFlag("--degree", degreeArg);
  addFlag("--pwidth", pilotText);
  addFlag("--at", atVar);
  if (atVar.empty()) addFlag("--n", gridArg);
  if (bandsInForce || !seName.empty()) args.push_back("--se");
  if (wantScatter) args.push_back("--emit-touse");
  if (weightKind == "a") {
    addFlag("--weight", parsed.weightVar.value_or(std::string()));
    addFlag("--wtype", "aweight");
  }
  if (auto bad = smoothSampleArgs(parsed, session, "lpoly", &args)) return *bad;

  nlohmann::json reply;
  try {
    reply = runCobraJson(args);
  } catch (const CobraError& ce) {
    return smoothEngineError("lpoly", ce);
  }

  const double absent = std::numeric_limits<double>::quiet_NaN();
  const double chosenWidth = reply.value("bwidth", absent);
  const double chosenPilot = reply.value("pwidth", absent);
  const int chosenDegree = reply.value("degree", 0);
  const std::string chosenKernel = reply.value("kernel", kernel);
  session.eval.rValues.set("N", static_cast<double>(reply.value("n_obs", 0)));
  session.eval.rValues.set("bwidth", chosenWidth);
  session.eval.rValues.set("pwidth", (bandsInForce || !seName.empty()) ? chosenPilot : absent);
  session.eval.rValues.set("ngrid", static_cast<double>(reply.value("ngrid", 0)));
  session.eval.rValues.set("degree", static_cast<double>(chosenDegree));
  session.eval.rStrings.set("kernel", chosenKernel);
  eval::declareRPosted(session.eval, "lpoly", {"N", "bwidth", "pwidth", "ngrid", "degree", "kernel"});

  const std::vector<double> grid = jsonDoubles(reply["grid"]);
  const std::vector<double> fitted = jsonDoubles(reply["smooth"]);
  const std::vector<double> fittedSe = reply.contains("se") ? jsonDoubles(reply["se"]) : std::vector<double>();

  ojson blocks = ojson::array();
  const bool capped = reply.contains("n_capped") && reply["n_capped"].is_boolean() ? reply["n_capped"].get<bool>()
                      : reply.value("n_capped", 0) != 0;
  if (capped && atVar.empty()) blocks.push_back(displayBlock("(n() set to " + std::to_string(grid.size()) + ")\n"));

  // Stata makes the columns smooth, grid, se after r() is in place and stops at the
  // first that cannot be made, keeping the earlier ones. What survives is added in the
  // order grid, smooth, se (`varlist_after` in the freeze row pins this).
  const size_t rows = static_cast<size_t>(session.datastore()->nRows());
  std::vector<LpolyOutput> outputs;
  if (!outWords.empty())
    outputs.push_back({outWords.back(), "lpoly smooth: " + smoothVarTitle(session, yVar), lpolyRowLength(fitted, rows), 1});
  if (outWords.size() == 2) outputs.push_back({outWords[0], "lpoly smoothing grid", lpolyRowLength(grid, rows), 0});
  if (!seName.empty()) outputs.push_back({seName, "lpoly SEs", lpolyRowLength(fittedSe, rows), 2});
  std::optional<ojson> lateRefusal;
  size_t creatable = 0;
  for (; creatable < outputs.size(); ++creatable) {
    const std::string& name = outputs[creatable].name;
    if (const auto reserved = strutil::newVarNameRefusal(name)) {
      lateRefusal = aerErrorReason(reserved->message, reserved->rc);
      break;
    }
    bool repeated = false;
    for (size_t earlier = 0; earlier < creatable; ++earlier) repeated = repeated || outputs[earlier].name == name;
    if (repeated) {
      lateRefusal = aerErrorReason("variable " + name + " already defined", 110);
      break;
    }
  }
  outputs.resize(creatable);

  if (drawn && !lateRefusal) {
    std::vector<std::pair<double, double>> scatter;
    if (wantScatter) {
      ojson failure;
      if (!smoothSamplePoints(session, yVar, xVar, smoothSampleMask(reply), &scatter, &failure)) return failure;
    }
    std::vector<std::array<double, 3>> band;
    std::vector<std::pair<double, double>> line;
    const double zScore = cecon::z_critical(1.0 - confidence / 100.0);
    for (size_t g = 0; g < grid.size() && g < fitted.size(); ++g) {
      if (smoothCellMissing(grid[g]) || smoothCellMissing(fitted[g])) continue;
      line.emplace_back(grid[g], fitted[g]);
      if (bandsInForce && g < fittedSe.size() && !smoothCellMissing(fittedSe[g]))
        band.push_back({grid[g], fitted[g] - zScore * fittedSe[g], fitted[g] + zScore * fittedSe[g]});
    }
    std::stable_sort(band.begin(), band.end(), [](const auto& l, const auto& r) { return l[0] < r[0]; });
    std::string note = "kernel = " + chosenKernel + ", degree = " + std::to_string(chosenDegree) +
                       ", bandwidth = " + smoothRound2(chosenWidth);
    if (bandsInForce) note += ", pwidth = " + smoothRound2(chosenPilot);
    blocks.push_back(smoothGraphBlock(parsed, session, "Local polynomial smooth", note, yVar, xVar, scatter, band,
                                      smoothPairsSortedByX(std::move(line))));
  }

  std::stable_sort(outputs.begin(), outputs.end(),
                   [](const LpolyOutput& l, const LpolyOutput& r) { return l.addOrder < r.addOrder; });
  std::vector<std::string> added;
  for (const LpolyOutput& output : outputs) {
    ojson failure;
    if (!smoothWriteColumn(session, output.name, output.values, output.label, &failure)) return failure;
    added.push_back(output.name);
  }
  if (!added.empty()) blocks.push_back(mutationResult(session, "lpoly", added, {}));
  if (lateRefusal) blocks.push_back(*lateRefusal);
  return blocks;
}

// `pca varlist [, components() mineigen() covariance correlation]`
ojson runPca(const eval::ParsedCommand& parsed, SessionState& session) {
  if (!engineHasSubcommand("pca")) return engineTooOld("pca", "pca");
  if (parsed.varlist.size() < 2) return aerErrorReason("pca: too few variables", 102);
  if (!session.hasData() || !session.currentHbin().has_value()) return ojson(eval::noDataLoaded());

  bool cov = false, corr = false;
  std::string comp, mineigen;
  for (const auto& [k, v] : parsed.opts) {
    if (k == "covariance") cov = true;
    else if (k == "correlation") corr = true;
    else if (k == "components" || k == "comp" || k == "factors") comp = v.strValue;
    else if (k == "mineigen") mineigen = v.strValue;
  }
  if (cov && corr) return aerErrorReason("pca: options correlation and covariance are exclusive", 198);

  std::string vars;
  for (std::size_t i = 0; i < parsed.varlist.size(); ++i)
    vars += (i ? "," : "") + parsed.varlist[i];
  std::vector<std::string> args = {"pca", "--data", *session.currentHbin(), "--x", vars};
  if (cov) args.push_back("--covariance");
  if (!comp.empty()) { args.push_back("--comp"); args.push_back(comp); }
  if (!mineigen.empty()) { args.push_back("--mineigen"); args.push_back(mineigen); }

  nlohmann::json raw;
  try {
    raw = runCobraJson(args);
  } catch (const CobraError& ce) {
    return aerError("pca: " + cobraErrorMessage(ce), ce.code);
  }

  const int p = raw.value("p", 0);
  const int f = raw.value("f", 0);
  const std::vector<double> ev = jsonDoubles(raw["eigenvalues"]);
  const std::string ctype = raw.value("ctype", std::string("correlation"));

  // Install the fit before publishing its e(): setLastModel supersedes any
  // prior explicit posting, and must not erase the PCA matrices just written.
  LastModel lm;
  lm.cobraCmd = "pca";
  lm.vcov = "none";
  session.setLastModel(lm);
  session.eval.eScalars.clear();
  session.eval.eStrings.clear();
  session.eval.eMatrices.clear();

  session.eval.setEScalar("N", static_cast<double>(raw.value("n_obs", 0)));
  session.eval.setEScalar("f", static_cast<double>(f));
  session.eval.setEScalar("rho", raw.value("rho", 0.0));
  if (raw.contains("trace") && raw["trace"].is_number())
    session.eval.setEScalar("trace", raw["trace"].get<double>());
  // A null lndet/cond is Stata's MISSING, which is what the collinear case
  // produces once the relative-1e-8 zeroing has run.
  if (raw.contains("lndet") && raw["lndet"].is_number())
    session.eval.setEScalar("lndet", raw["lndet"].get<double>());
  if (raw.contains("cond") && raw["cond"].is_number())
    session.eval.setEScalar("cond", raw["cond"].get<double>());

  session.eval.setEString("cmd", "pca");
  session.eval.setEString("Ctype", ctype);
  session.eval.setEString("title", "Principal components");
  session.eval.setEString("properties", "nob noV eigen");
  session.eval.setEString("predict", "pca_p");
  session.eval.setEString("marginsnotok", "_ALL");

  auto postM = [&session](const std::string& nm, eval::EvalMatrix m) {
    for (auto& kv : session.eval.eMatrices)
      if (kv.first == nm) { kv.second = std::move(m); return; }
    session.eval.eMatrices.emplace_back(nm, std::move(m));
    session.noteEOnFit(nm);
  };
  auto rowvec = [&](const std::vector<double>& v, std::vector<std::string> cn) {
    eval::EvalMatrix m;
    m.data.push_back(v);
    m.rownames = {"r1"};
    m.colnames = std::move(cn);
    return m;
  };
  std::vector<std::string> compNames;
  for (int j = 0; j < p; ++j) compNames.push_back("Comp" + std::to_string(j + 1));
  postM("Ev", rowvec(ev, compNames));

  if (f > 0 && raw.contains("loadings")) {
    const std::vector<double> L = jsonDoubles(raw["loadings"]);
    eval::EvalMatrix m;
    for (int i = 0; i < p; ++i) {
      std::vector<double> row;
      for (int j = 0; j < f; ++j) row.push_back(L[static_cast<std::size_t>(i * f + j)]);
      m.data.push_back(row);
      m.rownames.push_back(parsed.varlist[static_cast<std::size_t>(i)]);
    }
    for (int j = 0; j < f; ++j) m.colnames.push_back("Comp" + std::to_string(j + 1));
    postM("L", std::move(m));
  }
  if (raw.contains("psi"))
    postM("Psi", rowvec(jsonDoubles(raw["psi"]), parsed.varlist));
  if (raw.contains("means"))
    postM("means", rowvec(jsonDoubles(raw["means"]), parsed.varlist));
  // e(sds) exists ONLY for correlation PCA -- after a covariance run,
  // `confirm matrix e(sds)` is rc 111.
  if (raw.contains("sds"))
    postM("sds", rowvec(jsonDoubles(raw["sds"]), parsed.varlist));
  if (raw.contains("c")) {
    const std::vector<double> C = jsonDoubles(raw["c"]);
    eval::EvalMatrix m;
    for (int i = 0; i < p; ++i) {
      std::vector<double> row;
      for (int j = 0; j < p; ++j) row.push_back(C[static_cast<std::size_t>(i * p + j)]);
      m.data.push_back(row);
      m.rownames.push_back(parsed.varlist[static_cast<std::size_t>(i)]);
    }
    m.colnames = parsed.varlist;
    postM("C", std::move(m));
  }
  if (raw.contains("components")) {
    const std::vector<double> T4 = jsonDoubles(raw["components"]);
    eval::EvalMatrix m;
    for (int i = 0; i < p; ++i) {
      std::vector<double> row;
      for (int j = 0; j < 4; ++j) row.push_back(T4[static_cast<std::size_t>(i * 4 + j)]);
      m.data.push_back(row);
      m.rownames.push_back("Comp" + std::to_string(i + 1));
    }
    m.colnames = {"Eigenvalue", "Difference", "Proportion", "Cumulative"};
    postM("components", std::move(m));
  }

  // The complete actual matrix namespace is listed in Stata publication
  // order, not in engine-JSON key order (w2v_estpost_ematrix ep15c).
  const std::vector<std::string> matrixOrder{"sds","means","components","C","Psi","Ev","L"};
  std::stable_sort(session.eval.eMatrices.begin(),session.eval.eMatrices.end(),
      [&](const auto& a, const auto& b) {
        return std::find(matrixOrder.begin(),matrixOrder.end(),a.first) <
               std::find(matrixOrder.begin(),matrixOrder.end(),b.first);
      });
  for (const auto& v : session.eval.eScalars.items()) session.noteEOnFit(v.first);
  for (const auto& v : session.eval.eStrings.items()) session.noteEOnFit(v.first);

  std::ostringstream out;
  char buf[256];
  out << "\nPrincipal components/" << ctype << "                 Number of obs    = "
      << std::setw(10) << raw.value("n_obs", 0) << "\n";
  std::snprintf(buf, sizeof(buf), "                                                 Number of comp.  = %10d\n", f);
  out << buf;
  if (raw.contains("trace") && raw["trace"].is_number()) {
    std::snprintf(buf, sizeof(buf), "                                                 Trace            = %10.10g\n",
                  raw["trace"].get<double>());
    out << buf;
  }
  std::snprintf(buf, sizeof(buf), "    Rotation: (unrotated = principal)            Rho              = %10.4f\n",
                raw.value("rho", 0.0));
  out << buf;
  out << "    --------------------------------------------------------------------------\n";
  out << "       Component |   Eigenvalue   Difference         Proportion   Cumulative\n";
  out << "    -------------+------------------------------------------------------------\n";
  if (raw.contains("components")) {
    const std::vector<double> T4 = jsonDoubles(raw["components"]);
    for (int i = 0; i < p; ++i) {
      const double diff = T4[static_cast<std::size_t>(i * 4 + 1)];
      std::snprintf(buf, sizeof(buf), "%16s | %12.6g %12.6g %18.4f %12.4f\n", ("Comp" + std::to_string(i + 1)).c_str(),
                    T4[static_cast<std::size_t>(i * 4 + 0)], diff,
                    T4[static_cast<std::size_t>(i * 4 + 2)], T4[static_cast<std::size_t>(i * 4 + 3)]);
      if (std::isnan(diff))
        std::snprintf(buf, sizeof(buf), "%16s | %12.6g %12s %18.4f %12.4f\n", ("Comp" + std::to_string(i + 1)).c_str(),
                      T4[static_cast<std::size_t>(i * 4 + 0)], ".",
                      T4[static_cast<std::size_t>(i * 4 + 2)], T4[static_cast<std::size_t>(i * 4 + 3)]);
      out << buf;
    }
  }
  out << "    --------------------------------------------------------------------------\n";
  if (raw.contains("dropped") && raw["dropped"].is_array())
    for (const auto& dv : raw["dropped"])
      out << "(" << dv.get<std::string>() << " dropped because of zero variance)\n";
  return displayBlock(out.str());
}

// ===========================================================================
// xi -- indicator expansion, command form and prefix form
//
// Matches Stata's xi; the option arrangement, the letter sequence and the omit and
// listing-width rules were identified from its ado, the rest measured on StataBE 19.5
// (the help file is the only documentation left; xi has no manual entry since Stata
// 19). Two choices are PIET's own: generated
// columns are f64 with format %9.0g rather than byte, and a name stem cut in
// bytes backs off to a UTF-8 character boundary rather than splitting one.

namespace {

// Dataset characteristics that remember, across runs and save/use, which columns
// an earlier run generated and under which prefix. A new run removes only those,
// so a user's own `_Ifoo` survives.
const char kXiMadeChar[] = "__xi__Vars__To__Drop__";
const char kXiStubChar[] = "__xi__Vars__Prefix__";

bool xiCellMissing(double v) { return std::isnan(v) || std::fabs(v) >= eval::kStataMissingSentinel; }

bool xiHasFactorTag(const std::string& word) {
  return word.size() >= 2 && (word[0] == 'i' || word[0] == 'I') && word[1] == '.';
}

// Blank-separated words; blanks inside parentheses do not separate (`prefix( _S )`).
std::vector<std::string> xiWords(const std::string& text) {
  std::vector<std::string> words;
  std::string word;
  int nesting = 0;
  for (char ch : text) {
    if (ch == '(') ++nesting;
    if (ch == ')') --nesting;
    if (nesting <= 0 && std::isspace(static_cast<unsigned char>(ch))) {
      if (!word.empty()) words.push_back(std::move(word));
      word.clear();
      continue;
    }
    word += ch;
  }
  if (!word.empty()) words.push_back(std::move(word));
  return words;
}

// The leading `bytes` bytes of a name, shortened further if the cut would land
// inside a multibyte character.
std::string xiStem(const std::string& name, size_t bytes) {
  if (name.size() <= bytes) return name;
  while (bytes > 0 && (static_cast<unsigned char>(name[bytes]) & 0xC0) == 0x80) --bytes;
  return name.substr(0, bytes);
}

int xiChars(const std::string& s) { return static_cast<int>(cecon::ident::codePointCount(s)); }

// Stata's abbrev(): widths under 5 act as 5; a trailing digit run stays whole when
// it fits beside at least one leading character, otherwise only the last character
// survives after the tilde. The dataset-aware variant (avoiding an abbreviation two
// variables share) is not reproduced.
std::string xiShorten(const std::string& name, int width) {
  const int room = std::max(width, 5);
  if (xiChars(name) <= room) return name;
  size_t digitStart = name.size();
  while (digitStart > 0 && std::isdigit(static_cast<unsigned char>(name[digitStart - 1])) != 0) --digitStart;
  const int digitCount = static_cast<int>(name.size() - digitStart);
  if (digitCount > 0 && digitCount + 2 <= room)
    return cecon::ident::truncateCodePoints(name, static_cast<size_t>(room - 1 - digitCount)) + "~" +
           name.substr(digitStart);
  size_t lastChar = name.size() - 1;
  while (lastChar > 0 && (static_cast<unsigned char>(name[lastChar]) & 0xC0) == 0x80) --lastChar;
  return cecon::ident::truncateCodePoints(name, static_cast<size_t>(room - 2)) + "~" + name.substr(lastChar);
}

// A listing row: the term from column 1, the name range from column 19, the
// coding note from column 39. An overlong middle field just pushes the note along.
std::string xiListingRow(const std::string& term, const std::string& names, const std::string& note) {
  auto padded = [](std::string s, int cols) {
    const int have = xiChars(s);
    if (have < cols) s.append(static_cast<size_t>(cols - have), ' ');
    return s;
  };
  return padded(term, 18) + padded(names, 20) + note + "\n";
}

// How the listing writes an interaction term. The width formulas reproduce
// StataBE's output over a grid of name lengths (oracle cr_xi_c, section C2).
std::string xiFactorCrossText(const std::string& leftTag, const std::string& left, const std::string& rightTag,
                              const std::string& right) {
  const std::string shownLeft = xiShorten(left, std::min(xiChars(left), std::max(5, 8 - xiChars(right))));
  const std::string shownRight = xiShorten(right, std::min(xiChars(right), 10 - xiChars(shownLeft)));
  return leftTag + shownLeft + "*" + rightTag + shownRight;
}
std::string xiContinuousText(const std::string& leftTag, const std::string& left, char joint,
                             const std::string& right) {
  if (xiChars(left) + xiChars(right) <= 12) return leftTag + left + joint + right;
  const std::string shownLeft = xiShorten(left, std::min(xiChars(left), std::max(6, 12 - xiChars(right))));
  const std::string shownRight = xiShorten(right, std::min(xiChars(right), 12 - xiChars(shownLeft)));
  return leftTag + shownLeft + joint + shownRight;
}

// One distinct non-missing value of a factor.
struct XiLevel {
  double number = 0.0;
  std::string text;
  long long code = 0;
  size_t rows = 0;
  std::string shown;  // the value as a label prints it
};

// A factor as this run sees it. `rowLevel` indexes `levels`, -1 = missing.
struct XiFactor {
  std::string var;
  bool textual = false;
  bool natural = false;
  std::vector<XiLevel> levels;
  std::vector<int> rowLevel;
  int omitted = -1;
  std::vector<int> retained;
  std::string root;  // `_Ia_`, or a collision letter form such as `_Iaa`
  std::string wildcard() const { return root + "*"; }
  bool hasIndicators() const { return !retained.empty(); }
};

// `i.a*i.b` and friends, taken apart without looking at the data.
struct XiTermShape {
  std::string leftTag;
  std::string left;
  char joint = '\0';  // '\0', '*' or '|'
  bool rightIsFactor = false;
  std::string rightTag;
  std::string right;
};

std::optional<ojson> xiReadShape(const std::string& token, XiTermShape* out) {
  const std::string body = token.substr(2);
  out->leftTag = token.substr(0, 2);
  const size_t jointAt = body.find_first_of("*|");
  out->left = body.substr(0, jointAt);
  if (jointAt != std::string::npos) {
    out->joint = body[jointAt];
    std::string after = body.substr(jointAt + 1);
    if (xiHasFactorTag(after)) {
      if (out->joint == '|') return aerErrorReason("I.xxx|I.yyy not allowed", 198);
      out->rightIsFactor = true;
      out->rightTag = after.substr(0, 2);
      after.erase(0, 2);
    }
    out->right = after;
    if (after.empty()) return aerErrorReason("varlist required", 100);
  }
  if (out->left.empty()) return aerErrorReason("varlist required", 100);
  return std::nullopt;
}

// One run of xi over one line. Columns reach the dataset as soon as they are
// computed: a later term's refusal leaves the earlier terms' columns standing, as
// in Stata, and the collision test simply looks at the current dataset.
class XiExpansion {
 public:
  XiExpansion(SessionState& session, std::string stub, bool keepEveryLevel)
      : session_(session), stub_(std::move(stub)), keepEveryLevel_(keepEveryLevel) {}

  std::optional<ojson> run(const std::vector<std::string>& tokens) {
    if (auto bad = retirePreviousRun()) return bad;
    std::optional<ojson> refusal;
    for (const std::string& token : tokens) {
      refusal = expandTerm(token);
      if (refusal) break;
    }
    rememberThisRun();
    return refusal;
  }

  const std::string& listing() const { return listing_; }
  const std::vector<std::string>& made() const { return made_; }
  const std::vector<std::string>& substitutions() const { return substitutions_; }
  bool changedData() const { return !made_.empty() || !retired_.empty(); }
  // Columns the run removed and did not make again.
  std::vector<std::string> gone() const {
    std::vector<std::string> names;
    for (const auto& n : retired_)
      if (std::find(made_.begin(), made_.end(), n) == made_.end()) names.push_back(n);
    return names;
  }

 private:
  std::string characteristic(const std::string& owner, const std::string& key) const {
    for (const auto& entry : session_.characteristics)
      if (entry.varname == owner && entry.charname == key) return entry.value;
    return std::string();
  }

  void setDatasetCharacteristic(const std::string& key, const std::string& value) {
    auto& table = session_.characteristics;
    table.erase(std::remove_if(table.begin(), table.end(),
                               [&](const DtaCharacteristic& e) { return e.varname == "_dta" && e.charname == key; }),
                table.end());
    if (!value.empty()) table.push_back(DtaCharacteristic{"_dta", key, value});
  }

  std::optional<ojson> retirePreviousRun() {
    const std::vector<std::string> names = xiWords(characteristic("_dta", kXiMadeChar));
    const std::vector<std::string> stubs = xiWords(characteristic("_dta", kXiStubChar));
    Datastore* ds = session_.datastore();
    for (size_t i = 0; i < names.size() && i < stubs.size(); ++i) {
      if (stubs[i] != stub_) {
        foreignNames_.push_back(names[i]);
        foreignStubs_.push_back(stubs[i]);
      } else if (ds != nullptr && ds->has(names[i])) {
        retired_.push_back(names[i]);
      }
    }
    if (retired_.empty()) return std::nullopt;
    const std::string target = mintScratchPath(session_.workdir(), "xidrop");
    std::vector<std::string> args = {"eval", "--data", *session_.currentHbin(), "--out", target};
    for (const auto& name : retired_) {
      args.push_back("--drop");
      args.push_back(name);
    }
    try {
      runCobraJson(args);
    } catch (const CobraError& ce) {
      retired_.clear();
      return aerError(cobraErrorMessage(ce), ce.code);
    }
    session_.load(target);
    for (const auto& name : retired_) session_.varMeta.erase(name);
    return std::nullopt;
  }

  void rememberThisRun() {
    std::string names, stubs;
    auto add = [&](const std::string& name, const std::string& stub) {
      names += (names.empty() ? "" : " ") + name;
      stubs += (stubs.empty() ? "" : " ") + stub;
    };
    for (size_t i = 0; i < foreignNames_.size(); ++i) add(foreignNames_[i], foreignStubs_[i]);
    for (const auto& name : made_) add(name, stub_);
    setDatasetCharacteristic(kXiMadeChar, names);
    setDatasetCharacteristic(kXiStubChar, stubs);
  }

  bool rootInUse(const std::string& root) const {
    Datastore* ds = session_.datastore();
    if (ds == nullptr) return false;
    for (const auto& name : ds->names())
      if (name.compare(0, root.size(), root) == 0) return true;
    return false;
  }

  // A root some variable already begins gives way to its letter forms.
  std::string claimRoot(std::string root) const {
    if (!rootInUse(root)) return root;
    static const std::string kLetters = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ";
    for (char letter : kLetters) {
      root.back() = letter;
      if (!rootInUse(root)) break;
    }
    return root;
  }

  std::optional<ojson> bindVariable(const std::string& typed, std::string* full) {
    bool ambiguous = false;
    std::optional<std::string> bound = resolveVarName(typed, session_, &ambiguous);
    if (ambiguous) return aerErrorReason(typed + " ambiguous abbreviation", 111);
    if (!bound) return aerErrorReason("variable " + typed + " not found", 111);
    *full = *bound;
    return std::nullopt;
  }

  bool writeColumn(const std::string& name, const std::string& label, const std::vector<double>& values,
                   ojson* failure) {
    if (!estimWriteColumn(session_, name, values, failure)) return false;
    session_.setVarLabel(name, label);
    made_.push_back(name);
    return true;
  }

  // The distinct values with their counts, ascending, whichever way they are keyed,
  // and each row's position among them.
  template <typename Key>
  static std::map<Key, int> tallyLevels(const std::map<Key, size_t>& counts, std::vector<XiLevel>* levels) {
    std::map<Key, int> position;
    for (const auto& [key, count] : counts) {
      position[key] = static_cast<int>(levels->size());
      XiLevel level;
      level.rows = count;
      levels->push_back(std::move(level));
    }
    return position;
  }

  std::optional<ojson> readLevels(XiFactor* factor) {
    Datastore* ds = session_.datastore();
    if (ds == nullptr) return ojson(eval::noDataLoaded());
    const cecon::Column& column = ds->col(factor->var);
    const size_t rows = static_cast<size_t>(ds->nRows());
    factor->rowLevel.assign(rows, -1);
    if (column.dtype == cecon::DType::Factor) {
      factor->textual = true;
      const int32_t* cells = column.codes();
      auto textAt = [&](size_t r) -> const std::string* {
        if (cells == nullptr || cells[r] < 0) return nullptr;
        const std::string& s = column.levels[static_cast<size_t>(cells[r])];
        return s.empty() ? nullptr : &s;
      };
      std::map<std::string, size_t> counts;
      for (size_t r = 0; r < rows; ++r)
        if (const std::string* s = textAt(r)) ++counts[*s];
      std::map<std::string, int> position = tallyLevels(counts, &factor->levels);
      for (const auto& [value, index] : position) {
        factor->levels[static_cast<size_t>(index)].text = value;
        factor->levels[static_cast<size_t>(index)].shown = value;
      }
      for (size_t r = 0; r < rows; ++r)
        if (const std::string* s = textAt(r)) factor->rowLevel[r] = position[*s];
    } else {
      std::vector<double> cells;
      std::string why;
      if (!numericColumn(*ds, factor->var, &cells, &why)) return aerError(why);
      std::map<double, size_t> counts;
      for (double v : cells)
        if (!xiCellMissing(v)) ++counts[v];
      std::map<double, int> position = tallyLevels(counts, &factor->levels);
      for (const auto& [value, index] : position) {
        factor->levels[static_cast<size_t>(index)].number = value;
        factor->levels[static_cast<size_t>(index)].shown = strutil::trim(eval::applyStataFmt(value, "%18.0g"));
      }
      for (size_t r = 0; r < rows; ++r)
        if (!xiCellMissing(cells[r])) factor->rowLevel[r] = position[cells[r]];
    }
    factor->natural = !factor->textual;
    for (const XiLevel& level : factor->levels)
      if (level.number != std::floor(level.number) || level.number < 0.0 || level.number > 999999999.0)
        factor->natural = false;
    long long ordinal = 0;
    for (XiLevel& level : factor->levels)
      level.code = factor->natural ? static_cast<long long>(level.number) : ++ordinal;
    return std::nullopt;
  }

  // Which level the indicators leave out: noomit (none) beats char v[omit], which
  // beats any text in char _dta[omit] (the most frequent level, `prevalent` being only
  // the documented spelling), which beats the first level. A number in char v[omit]
  // is compared at float precision, so `0.1` finds a float column's 0.1.
  std::optional<ojson> chooseOmitted(XiFactor* factor) {
    if (keepEveryLevel_) return std::nullopt;
    const std::string wanted = strutil::trim(characteristic(factor->var, "omit"));
    if (!wanted.empty()) {
      float numericTarget = 0.0F;
      if (!factor->textual) {
        char* stop = nullptr;
        numericTarget = static_cast<float>(std::strtod(wanted.c_str(), &stop));
        if (stop == wanted.c_str() || *stop != '\0')
          return aerErrorReason(" characteristic " + factor->var + "[omit] (" + wanted + ") invalid;\nvariable " +
                                    factor->var + " is numeric",
                                198);
      }
      // Several double levels can share one float value; the highest of them is left out.
      for (size_t i = 0; i < factor->levels.size(); ++i) {
        const XiLevel& level = factor->levels[i];
        if (factor->textual ? level.text == wanted : static_cast<float>(level.number) == numericTarget)
          factor->omitted = static_cast<int>(i);
      }
      if (factor->omitted >= 0) return std::nullopt;
      listing_ += "(characteristic " + factor->var + "[omit]: " + wanted + "\nyet variable " +
                  xiShorten(factor->var, 22) + " never equals " + wanted + "; characteristic ignored)\n";
    }
    factor->omitted = 0;
    if (!characteristic("_dta", "omit").empty())
      for (size_t i = 1; i < factor->levels.size(); ++i)
        if (factor->levels[i].rows > factor->levels[static_cast<size_t>(factor->omitted)].rows)
          factor->omitted = static_cast<int>(i);
    return std::nullopt;
  }

  void listMainEffects(const XiFactor& factor, const std::string& tag) {
    if (keepEveryLevel_) return;
    const std::string range = factor.root + std::to_string(factor.levels.front().code) + "-" +
                              std::to_string(factor.levels.back().code);
    const XiLevel& out = factor.levels[static_cast<size_t>(factor.omitted)];
    const std::string outName = factor.root + std::to_string(out.code);
    // The variable's name gets what is left of a 41-byte note.
    const int nameRoom = 24 - static_cast<int>(outName.size() + out.shown.size());
    const std::string note = factor.natural ? "(naturally coded; " + outName + " omitted)"
                                            : "(" + outName + " for " + xiShorten(factor.var, nameRoom) + "==" +
                                                  out.shown + " omitted)";
    listing_ += xiListingRow(tag + xiShorten(factor.var, 13), range, note);
  }

  // The factor's main effects, made once per run by whichever term first needs them.
  std::optional<ojson> ensureFactor(const std::string& var, const std::string& tag, XiFactor** out) {
    const auto seen = factors_.find(var);
    if (seen != factors_.end()) {
      *out = &seen->second;
      return std::nullopt;
    }
    XiFactor factor;
    factor.var = var;
    if (auto bad = readLevels(&factor)) return bad;
    if (factor.levels.empty()) return aerSyntaxError("invalid syntax");  // measured: an all-missing factor
    if (auto bad = chooseOmitted(&factor)) return bad;
    const std::string plainRoot = stub_ + xiStem(var, 11 - stub_.size()) + "_";
    const std::string firstName = plainRoot + std::to_string(factor.levels.front().code);
    if (!strutil::isValidStataIdentifier(firstName)) return aerErrorReason(firstName + " invalid name", 198);
    factor.root = claimRoot(plainRoot);
    for (size_t i = 0; i < factor.levels.size(); ++i)
      if (static_cast<int>(i) != factor.omitted) factor.retained.push_back(static_cast<int>(i));
    listMainEffects(factor, tag);
    const size_t rows = factor.rowLevel.size();
    for (int level : factor.retained) {
      std::vector<double> values(rows, std::numeric_limits<double>::quiet_NaN());
      for (size_t r = 0; r < rows; ++r)
        if (factor.rowLevel[r] >= 0) values[r] = factor.rowLevel[r] == level ? 1.0 : 0.0;
      const XiLevel& info = factor.levels[static_cast<size_t>(level)];
      ojson failure;
      if (!writeColumn(factor.root + std::to_string(info.code), var + "==" + info.shown, values, &failure))
        return failure;
    }
    *out = &factors_.emplace(var, std::move(factor)).first->second;
    return std::nullopt;
  }

  std::optional<ojson> expandFactorCross(const XiTermShape& shape, const std::string& leftVar) {
    XiFactor* left = nullptr;
    if (auto bad = ensureFactor(leftVar, shape.leftTag, &left)) return bad;
    if (shape.right.find('*') != std::string::npos)
      return aerErrorReason("factor-variable and time-series operators not allowed", 101);
    if (shape.right.find('|') != std::string::npos) return aerErrorReason("| invalid name", 198);
    std::string rightVar;
    if (auto bad = bindVariable(shape.right, &rightVar)) return bad;
    XiFactor* right = nullptr;
    if (auto bad = ensureFactor(rightVar, shape.rightTag, &right)) return bad;
    const std::string caption = xiFactorCrossText(shape.leftTag, leftVar, shape.rightTag, rightVar);
    if (!left->hasIndicators() || !right->hasIndicators()) {
      listing_ += xiListingRow(caption, "", "(requires no interaction terms)");
      substitutions_.push_back(left->hasIndicators() ? left->wildcard() : leftVar);
      return std::nullopt;
    }
    const std::string root = claimRoot(stub_ + xiStem(leftVar, 3) + "X" + xiStem(rightVar, 3) + "_");
    listing_ += xiListingRow(caption, root + "#_#", "(coded as above)");
    const size_t rows = left->rowLevel.size();
    for (int a : left->retained) {
      const XiLevel& aInfo = left->levels[static_cast<size_t>(a)];
      for (int b : right->retained) {
        const XiLevel& bInfo = right->levels[static_cast<size_t>(b)];
        std::vector<double> values(rows, std::numeric_limits<double>::quiet_NaN());
        for (size_t r = 0; r < rows; ++r)
          if (left->rowLevel[r] >= 0 && right->rowLevel[r] >= 0)
            values[r] = (left->rowLevel[r] == a && right->rowLevel[r] == b) ? 1.0 : 0.0;
        ojson failure;
        if (!writeColumn(root + std::to_string(aInfo.code) + "_" + std::to_string(bInfo.code),
                         leftVar + "==" + aInfo.shown + " & " + rightVar + "==" + bInfo.shown, values, &failure))
          return failure;
      }
    }
    substitutions_.push_back(left->wildcard() + " " + right->wildcard() + " " + root + "*");
    return std::nullopt;
  }

  std::optional<ojson> expandContinuousCross(const XiTermShape& shape, const std::string& leftVar) {
    std::string rightVar;
    if (auto bad = bindVariable(shape.right, &rightVar)) return bad;
    if (!smoothIsNumeric(session_, rightVar)) return aerErrorReason("string variables not allowed", 109);
    std::vector<double> multiplier;
    {
      Datastore* ds = session_.datastore();
      std::string why;
      if (ds == nullptr) return ojson(eval::noDataLoaded());
      if (!numericColumn(*ds, rightVar, &multiplier, &why)) return aerError(why);
    }
    XiFactor* left = nullptr;
    if (auto bad = ensureFactor(leftVar, shape.leftTag, &left)) return bad;
    const std::string caption = xiContinuousText(shape.leftTag, leftVar, shape.joint, rightVar);
    // The command names c itself only the first time this run enters it.
    const std::string entry = enteredContinuous_.insert(rightVar).second ? rightVar + " " : std::string();
    if (!left->hasIndicators()) {
      listing_ += xiListingRow(caption, "", "(requires no interaction terms)");
      substitutions_.push_back(entry);
      return std::nullopt;
    }
    const size_t codeDigits = std::to_string(left->levels[static_cast<size_t>(left->retained.back())].code).size();
    const size_t rightBytes = codeDigits >= 3 ? 3 : 6 - codeDigits;
    const std::string root = claimRoot(stub_ + xiStem(leftVar, 3) + "X" + xiStem(rightVar, rightBytes) + "_");
    listing_ += xiListingRow(caption, root + "#", "(coded as above)");
    const size_t rows = left->rowLevel.size();
    for (int a : left->retained) {
      const XiLevel& aInfo = left->levels[static_cast<size_t>(a)];
      std::vector<double> values(rows, std::numeric_limits<double>::quiet_NaN());
      for (size_t r = 0; r < rows; ++r)
        if (left->rowLevel[r] >= 0 && !xiCellMissing(multiplier[r]))
          values[r] = left->rowLevel[r] == a ? multiplier[r] : 0.0;
      ojson failure;
      if (!writeColumn(root + std::to_string(aInfo.code), "(" + leftVar + "==" + aInfo.shown + ")*" + rightVar, values,
                       &failure))
        return failure;
    }
    // `i.a|c` leaves the main effects out of the command; `i.a*c` keeps them.
    substitutions_.push_back((shape.joint == '*' ? left->wildcard() + " " : std::string()) + entry + root + "*");
    return std::nullopt;
  }

  std::optional<ojson> expandTerm(const std::string& token) {
    XiTermShape shape;
    if (auto bad = xiReadShape(token, &shape)) return bad;
    std::string leftVar;
    if (auto bad = bindVariable(shape.left, &leftVar)) return bad;
    if (shape.joint == '\0') {
      XiFactor* factor = nullptr;
      if (auto bad = ensureFactor(leftVar, shape.leftTag, &factor)) return bad;
      substitutions_.push_back(factor->hasIndicators() ? factor->wildcard() : std::string());
      return std::nullopt;
    }
    return shape.rightIsFactor ? expandFactorCross(shape, leftVar) : expandContinuousCross(shape, leftVar);
  }

  SessionState& session_;
  std::string stub_;
  bool keepEveryLevel_;
  std::string listing_;
  std::vector<std::string> made_;
  std::vector<std::string> retired_;
  std::vector<std::string> foreignNames_;
  std::vector<std::string> foreignStubs_;
  std::vector<std::string> substitutions_;
  std::map<std::string, XiFactor> factors_;
  std::set<std::string> enteredContinuous_;
};

struct XiCommandLine {
  std::vector<std::string> tokens;
  std::string stub;
  bool keepEveryLevel = false;
};

// `prefix(x)`, spelled with at least `pre`.
bool xiTakePrefixOption(const std::string& word, std::string* stub) {
  const size_t paren = word.find('(');
  if (paren == std::string::npos || paren < 3 || word.back() != ')') return false;
  if (std::string("prefix").compare(0, paren, word, 0, paren) != 0) return false;
  *stub = strutil::trim(word.substr(paren + 1, word.size() - paren - 2));
  return true;
}

std::string xiOptionWordName(const std::string& word) {
  size_t end = 0;
  while (end < word.size() && (std::isalnum(static_cast<unsigned char>(word[end])) != 0 || word[end] == '_')) ++end;
  return end > 0 ? word.substr(0, end) : word;
}

// The line is read raw: xi's two arrangements, identified from its ado, are not ones
// the tokenizer knows.
//   `xi terms [, options]`: the part after the comma is options and nothing else.
//   `xi, options terms`: one or two leading option words, then an ordinary term
//   line. Words without a factor tag on a term line are passed over, so
//   `xi i.a b` and `xi, prefix(_I) b i.a` expand only `i.a`.
std::optional<ojson> xiReadCommandLine(const std::string& raw, XiCommandLine* out) {
  size_t cursor = raw.find_first_not_of(" \t");
  while (cursor != std::string::npos && cursor < raw.size() && std::isalpha(static_cast<unsigned char>(raw[cursor])) != 0)
    ++cursor;
  const std::string line = cursor == std::string::npos ? std::string() : raw.substr(cursor);
  const size_t comma = line.find(',');
  auto collectTerms = [out](const std::vector<std::string>& words, size_t from) {
    for (size_t i = from; i < words.size(); ++i)
      if (xiHasFactorTag(words[i])) out->tokens.push_back(words[i]);
  };
  auto refused = [](const std::string& word) { return aerErrorReason("option " + xiOptionWordName(word) + " not allowed", 198); };
  if (comma == std::string::npos) {
    collectTerms(xiWords(line), 0);
    return std::nullopt;
  }
  const std::vector<std::string> tail = xiWords(line.substr(comma + 1));
  if (line.find_first_not_of(" \t") != comma) {
    collectTerms(xiWords(line.substr(0, comma)), 0);
    for (const std::string& word : tail) {
      if (word == "noomit") out->keepEveryLevel = true;
      else if (!xiTakePrefixOption(word, &out->stub)) return refused(word);
    }
    return std::nullopt;
  }
  size_t used = 0;
  if (!tail.empty()) {
    if (tail[0] == "noomit") {
      out->keepEveryLevel = true;
      used = 1;
      if (tail.size() > 1 && xiTakePrefixOption(tail[1], &out->stub)) used = 2;
    } else {
      if (!xiTakePrefixOption(tail[0], &out->stub)) return refused(tail[0]);
      used = 1;
      if (tail.size() > 1 && tail[1] == "noomit") {
        out->keepEveryLevel = true;
        used = 2;
      }
    }
  }
  collectTerms(tail, used);
  return std::nullopt;
}

std::optional<ojson> xiStubRefusal(const std::string& stub) {
  if (stub.size() > 4) return aerSyntaxError("prefix() invalid; stub name too long ( >4 )");
  return std::nullopt;
}

// Each term token becomes its text in place; every other byte of the line stays,
// including runs of blanks and a comma glued to the term.
std::string xiRewriteLine(const std::string& line, const std::vector<std::string>& tokens,
                          const std::vector<std::string>& texts) {
  static const char kBlanks[] = " \t";
  std::string result;
  size_t cursor = 0;
  size_t next = 0;
  while (next < tokens.size() && next < texts.size()) {
    const size_t start = line.find_first_not_of(kBlanks, cursor);
    if (start == std::string::npos) break;
    size_t stop = line.find_first_of(kBlanks, start);
    if (stop == std::string::npos) stop = line.size();
    const std::string word = line.substr(start, stop - start);
    const size_t comma = word.find(',');
    result.append(line, cursor, start - cursor);
    if (word.compare(0, comma, tokens[next]) == 0) {
      result += texts[next++];
      if (comma != std::string::npos) result += word.substr(comma);
    } else {
      result += word;
    }
    cursor = stop;
  }
  result.append(line, cursor, std::string::npos);
  return result;
}

}  // namespace

ojson runXi(const eval::ParsedCommand& parsed, SessionState& session) {
  if (parsed.ifExpr.has_value()) return aerErrorReason("if not allowed", 101);
  if (parsed.inRange.has_value()) return aerErrorReason("in range not allowed", 101);
  if (parsed.weightType.has_value()) return aerErrorReason("weights not allowed", 101);

  XiCommandLine line;
  if (auto bad = xiReadCommandLine(parsed.raw, &line)) return *bad;
  if (auto bad = xiStubRefusal(line.stub)) return *bad;
  if (!session.hasData() || !session.currentHbin().has_value()) {
    if (line.tokens.empty()) return ojson::array();
    return ojson(eval::noDataLoaded());
  }

  // A line with no term still retires the previous run's columns (measured: `xi i.a`
  // then `xi b` leaves no `_Ia_*`).
  XiExpansion expansion(session, line.stub.empty() ? "_I" : line.stub, line.keepEveryLevel);
  const std::optional<ojson> refusal = expansion.run(line.tokens);
  ojson blocks = ojson::array();
  if (!expansion.listing().empty()) blocks.push_back(displayBlock(expansion.listing()));
  if (expansion.changedData()) blocks.push_back(mutationResult(session, "xi", expansion.made(), expansion.gone()));
  if (refusal) blocks.push_back(*refusal);
  return blocks;
}

ojson runXiPrefix(const std::vector<std::string>& terms,
                  const std::vector<std::pair<std::string, eval::OptValue>>& opts,
                  const std::string& wrapped, SessionState& session, std::string* rewritten,
                  std::string* displayText) {
  std::string stub;
  bool keepEveryLevel = false;
  for (const auto& [key, value] : opts) {
    if (key == "prefix") stub = value.strValue;
    if (key == "noomit") keepEveryLevel = true;
  }
  if (auto bad = xiStubRefusal(stub)) return *bad;
  if (!session.hasData() || !session.currentHbin().has_value()) return ojson(eval::noDataLoaded());

  XiExpansion expansion(session, stub.empty() ? "_I" : stub, keepEveryLevel);
  if (auto refusal = expansion.run(terms)) return *refusal;
  *displayText += expansion.listing();
  *rewritten = xiRewriteLine(wrapped, terms, expansion.substitutions());
  return ojson();
}

// ===========================================================================
// AER scope-2 bundle B2 -- `oaxaca` (Ben Jann, SSC 4.1.1)
//
// `oaxaca depvar indepvars, by(groupvar) pooled [relax noisily level(#)]`
//
// ONLY the twofold `pooled` decomposition is computed, because that is the form
// the twelve-paper corpus writes (034: `... sch2-sch531, by(parentedex) relax
// noisily pooled`) and the only form whose inference was VERIFIED before this
// handler existed: internal note oaxaca-se-20260908/REPORT.md reproduces all
// 120 SEs and every e(V)/e(V0) entry of 12 live-Stata fixtures to 3.8e-14, and
// 48 deliberately wrong constructions fail. The threefold DEFAULT (no `pooled`)
// is a NAMED refusal, not a silent fall-through to the twofold numbers: Stata
// answers a different table there, and an rc 0 with the wrong table is the
// cardinal defect class.
//
// What the verification pinned, and this handler must not lose:
//   * `pooled` IMPLIES robust `suest` (oaxaca.hlp: "suest is implied by
//     pooled"), so e(vce)=robust and e(suest)=suest even though nobody typed
//     vce(robust). The engine owns that construction (src/estimators/oaxaca.cpp).
//   * `relax` leaves a dropped coefficient as 0 with SE 0 (NOT missing), and
//     Stata drops the FIRST of two duplicates, keeping the later one
//     (rank_oracle.txt CASE 13) -- the opposite of regress's convention.
//   * Group 1 is the LOWER group value; e(N_1)/e(N_2) are per-group counts.
//
// e(b) is one row whose column names are Stata's `eq:name` spellings
// (`overall:difference`, `explained:x1`, `unexplained:_cons`), so `_b[...]` and
// `_se[...]` resolve by exact match. NAMED DIVERGENCE: PIET's matrix store has
// no separate row/column EQUATION metadata, so `: colnames e(b)` answers the
// joined spelling where Stata answers the bare name plus `: coleq`.
namespace {
const eval::OptVocab& kVocabOaxaca = eval::registerOptVocab(
    {"oaxaca"},
    {{"by", "pooled", "relax", "noisily", "level", "linear", "nolegend"},
     {"swap", "logit", "probit", "nodetail", "adjust", "threefold", "weight", "omega",
      "reference", "split", "svy", "vce", "cluster", "fixed", "suest", "nosuest", "nose",
      "model1", "model2", "x1", "x2", "xb", "eform", "categorical", "detail", "normalize"},
     // oaxaca.ado's syntax line declares one-letter stems the 2-char prefix
     // rule cannot reach: p:ooled l:evel a:djust w:eight o:mega.
     {{"p", "pooled"}, {"l", "level"}, {"a", "adjust"}, {"w", "weight"}, {"o", "omega"}}});

// An engine refusal carrying an exact Stata rc in `detail` ("r(499)") outranks
// the coarse code mapping; `no_obs` is Stata's r(2000); a bad_input with no
// rc is the r(198) class. Never the default engine_error, which reads as rc 1
// and silently breaks `_rc` assertions (the RC defect class this campaign has
// shipped before).
ojson oaxacaEngineError(const CobraError& ce) {
  const std::string msg = "oaxaca: " + cobraErrorMessage(ce);
  static const std::regex reRc(R"(r\((\d+)\))");
  std::smatch m;
  if (std::regex_search(ce.detail, m, reRc)) return aerErrorReason(msg, std::stoi(m.str(1)));
  if (ce.code == "no_obs") return aerErrorReason(msg, 2000);
  if (ce.code == "bad_input" || ce.code == "collinear") return aerErrorReason(msg, 198);
  return aerError(msg, ce.code);
}
}  // namespace

ojson runOaxaca(const eval::ParsedCommand& parsed, SessionState& session) {
  if (!engineHasSubcommand("oaxaca")) return engineTooOld("oaxaca", "oaxaca");
  if (!session.hasData() || !session.currentHbin().has_value()) return ojson(eval::noDataLoaded());
  if (auto err = eval::rejectUnknownOptions(parsed.opts, kVocabOaxaca.known,
                                            kVocabOaxaca.unimplemented)) {
    return ojson(*err);
  }
  std::string by, level;
  bool pooled = false, relax = false, noisily = false;
  for (const auto& [k, v] : parsed.opts) {
    if (k == "by") by = strutil::trim(v.strValue);
    else if (k == "pooled") pooled = true;
    else if (k == "relax") relax = true;
    else if (k == "noisily") noisily = true;
    else if (k == "level") level = strutil::trim(v.strValue);
    // `linear` is the default model and `nolegend` suppresses a legend this
    // build never prints: both are exact no-ops, accepted without a note.
  }
  if (by.empty()) return aerErrorReason("oaxaca: option by() required", 198);
  if (!pooled) {
    // The threefold default (and every other twofold weighting) is a different
    // table with its own, unverified inference. Say so by name.
    return ojson(eval::refusedCommand("oaxaca", "oaxaca_threefold_default"));
  }
  if (parsed.varlist.empty()) return aerSyntaxError("oaxaca: syntax is `oaxaca depvar indepvars, by(groupvar) pooled`");

  // The corpus form is `... sch2-sch531`: a RANGE inside an estimation varlist.
  // Estimation rows do not opt into the dispatcher's plain-varlist expansion
  // (they legitimately carry `i.x`/`L.x`), so the expansion is asked for HERE,
  // through the one expander PIET has (BUG-371/BUG-450), on the whole list.
  std::vector<std::string> vars = parsed.varlist;
  bool patterned = false;
  for (const auto& t : vars) {
    if (t.find('*') != std::string::npos || t.find('?') != std::string::npos ||
        t.find('-') != std::string::npos || strutil::lower(t) == "_all") {
      patterned = true;
      break;
    }
  }
  if (patterned) {
    std::string spec;
    for (std::size_t i = 0; i < vars.size(); ++i) spec += (i ? " " : "") + vars[i];
    loops::VarlistExpandResult ex = loops::expandVarlist(spec, session);
    if (ex.isError) return ex.error;
    vars = ex.tokens;
  }
  for (const auto& t : vars) {
    // Factor-variable and time-series operators have no engine path here.
    if (t.find('.') != std::string::npos)
      return ojson(eval::refusedCommand("oaxaca", "factor_variable_operators"));
  }
  if (vars.size() < 2)
    return aerErrorReason("oaxaca: at least one regressor is required in this build", 198);
  const std::string y = vars[0];
  std::vector<std::string> xs(vars.begin() + 1, vars.end());

  std::vector<std::string> args = {"oaxaca", "--data", *session.currentHbin(), "--y", y,
                                   "--x", est_detail::joinCommaM4(xs), "--by", by, "--pooled"};
  if (relax) args.push_back("--relax");
  if (!level.empty()) { args.push_back("--level"); args.push_back(level); }
  if (parsed.ifExpr.has_value() && !parsed.ifExpr->empty()) {
    args.push_back("--keepif");
    args.push_back(*parsed.ifExpr);
  }

  nlohmann::json raw;
  try {
    raw = runCobraJson(args);
  } catch (const CobraError& ce) {
    return oaxacaEngineError(ce);
  }

  // e(): measured on StataBE (tests/validate/b2_oracle.json ox_* and the
  // oaxaca-se-20260908 captures): e(cmd) e(model) e(N) e(N_1) e(N_2) e(vce)
  // e(suest) e(depvar), plus e(b)/e(V).
  const std::vector<std::string> names = raw["e_b"]["names"].get<std::vector<std::string>>();
  const std::vector<double> beta = jsonDoubles(raw["e_b"]["values"]);
  session.eval.setEScalar("N", static_cast<double>(raw.value("n_obs", 0)));
  session.eval.setEScalar("N_1", static_cast<double>(raw.value("N_1", 0)));
  session.eval.setEScalar("N_2", static_cast<double>(raw.value("N_2", 0)));
  session.eval.setEScalar("level", raw.value("level", 95.0));
  session.eval.setEString("cmd", "oaxaca");
  session.eval.setEString("depvar", y);
  session.eval.setEString("model", "linear");
  session.eval.setEString("vce", "robust");
  session.eval.setEString("suest", "suest");
  session.eval.setEString("by", by);
  session.eval.setEString("title", "Blinder-Oaxaca decomposition");
  session.eval.setEString("properties", "b V");
  auto postM = [&session](const std::string& nm, eval::EvalMatrix m) {
    for (auto& kv : session.eval.eMatrices)
      if (kv.first == nm) { kv.second = std::move(m); return; }
    session.eval.eMatrices.emplace_back(nm, std::move(m));
  };

  // The LastModel is what `_b[]`/`_se[]`/`ereturn list`/outreg2 read; the fit
  // filter is carried so a post-estimation refit reconstructs the same sample.
  LastModel lm;
  lm.cobraCmd = "oaxaca";
  lm.y = y;
  lm.depvar = y;
  lm.x = xs;
  lm.vcov = "robust";
  lm.dataPath = *session.currentHbin();
  est_detail::captureFitFilterM4(lm, args);
  lm.result = ojson(raw);
  session.setLastModel(lm);
  // eb-order T-P3 part 0: a fresh setLastModel clears the previous fit's block
  // and posted e(b)/e(V), so this fit posts its own AFTER it -- the e(b)/e(V)
  // pair, and the estimation block `_b[]`, eststo and esttab read (the same
  // block this handler returns; its coeftable names are the raw eq:name ones,
  // so `_b[overall:difference]` is unchanged, aer_estim2_oracle.log:242).
  {
    eval::EvalMatrix b;
    b.data.push_back(beta);
    b.rownames = {"y1"};
    b.colnames = names;
    postM("b", std::move(b));
    eval::EvalMatrix V;
    for (const auto& row : raw["e_V"]["matrix"]) V.data.push_back(jsonDoubles(row));
    V.rownames = names;
    V.colnames = names;
    postM("V", std::move(V));
  }
  const ojson estBlock = est_detail::cobraToEstimationM4(ojson(raw), "oaxaca", &session);
  session.lastEstBlock = estBlock;

  ojson blocks = ojson::array();
  if (noisily) {
    // `noisily` shows the two group regressions and the pooled one in Stata.
    // Those fits are not surfaced by this build; the decomposition is the same.
    blocks.push_back(displayBlock(
        "(oaxaca noisily: the underlying group and pooled regressions are not displayed "
        "in PIET -- decomposition unaffected)"));
  }
  if (raw.contains("warnings") && raw["warnings"].is_array())
    for (const auto& w : raw["warnings"]) blocks.push_back(displayBlock(w.get<std::string>()));
  blocks.push_back(estBlock);
  return blocks;
}

// ===========================================================================
// AER scope-2 B2-4 — `teffects nnmatch` (StataCorp [CAUSAL])
// ===========================================================================
//
// Paper 076 (`code/clean/clean_15_matching.do:366`) is the reason this exists:
//
//   sort PNR
//   teffects nnmatch (outcome i.earnlevel_0 i.ageg ... i.mother_ageg) (treatment), gen(match_id)
//
// `outcome` is a constant 0 there, so the ESTIMATE is not what the paper
// consumes — `match_id1` of the TREATED rows is (lines 382-413 join it to
// control row numbers). That makes two things load-bearing that a coefficient
// gate would never see: the ids are 1-based `_n` of the CURRENT data (MEASURED
// under `if`: an odd-rows-only subsample comes back with ODD ids, so the row
// numbers are the FULL frame's — INFO|nn_edge|ifodd_* in
// tests/eval/golden/aer_estim2_oracle.log), and their ORDER inside a tie is
// Stata's, not ours. Gate ① still wants e(b)/e(V) exact, and it gets them.
//
// Method identity: internal note teffects-nnmatch-20260908/REPORT.md as
// CORRECTED by REFUTE-0.md. The kernel comment in src/estimators/nnmatch.cpp
// carries the H2 rule and its named divergence; this handler owns only the
// surface: parsing, option honesty, e() posting, and gen().
//
// NAMED DIVERGENCES, all of them deliberate:
//   * `e(mscale)`/`e(mstats)` are HIDDEN matrices in Stata and are not posted
//     here (the engine computes them; nothing in PIET reads them back).
//   * `predict te|po|distance` after this command is not wired in this landing;
//     the engine already returns te/po0/po1/dist in the LastModel's result, so
//     it is a wiring step, not a method question. Refused by name until then.
//   * a treatment variable that is not 0/1 is ACCEPTED with control = the lower
//     level (Stata's `ATE:r2vs1.t12` colname is reproduced), but only the
//     colname of that case was measured.
namespace {
const eval::OptVocab& kVocabTeffects = eval::registerOptVocab(
    {"teffects"},
    {{"generate", "gen", "nneighbor", "nn", "ate", "atet", "metric", "vce", "level", "dtolerance"},
     // Known to Stata, absent here: each is a DIFFERENT estimator or a rule
     // this investigation never measured. Named, never silently ignored.
     // dtolerance (BUG-468) is served: the engine's --dtolerance.
     {"biasadj", "ematch", "caliper", "osample", "control", "tlevel",
      "dmvariables", "pstolerance", "iterate", "aequations", "tolerance", "nolog",
      "coeflegend"},
     // _teffects_nnmatch.ado's syntax line: GENerate(), NNeighbor(), ATET,
     // METric() -- stems the 2-character prefix rule cannot reach on its own.
     {{"gen", "generate"}, {"nn", "nneighbor"}}});

// The two parenthesised groups of `teffects <sub> (…) (…)`. Scanned off the RAW
// text because the tokenizer has no notion of them; only the first two
// top-level groups are read, and everything before the first `(` must be the
// command and sub-command words.
bool teffectsGroups(const std::string& raw, std::vector<std::string>* g1,
                    std::vector<std::string>* g2, std::string* why) {
  std::vector<std::string> groups;
  std::size_t i = 0;
  while (groups.size() < 2 && i < raw.size()) {
    if (raw[i] != '(') { ++i; continue; }
    int depth = 0;
    std::size_t j = i;
    for (; j < raw.size(); ++j) {
      if (raw[j] == '(') ++depth;
      else if (raw[j] == ')') { if (--depth == 0) break; }
    }
    if (j >= raw.size()) { *why = "unbalanced parentheses"; return false; }
    groups.push_back(raw.substr(i + 1, j - i - 1));
    i = j + 1;
  }
  if (groups.size() < 2) { *why = "two parenthesised groups are required"; return false; }
  auto split = [](const std::string& s) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : s) {
      if (std::isspace(static_cast<unsigned char>(c)) || c == ',') {
        if (!cur.empty()) { out.push_back(cur); cur.clear(); }
        if (c == ',') out.push_back(",");
      } else cur += c;
    }
    if (!cur.empty()) out.push_back(cur);
    return out;
  };
  *g1 = split(groups[0]);
  *g2 = split(groups[1]);
  return true;
}

// `vce(robust, nn(3))` / `vce(robust)` / `vce(iid)`. Returns false with a
// message when the spelling is one this build does not answer for.
bool parseTeffectsVce(const std::string& v, int* nn, std::string* why) {
  std::string s = strutil::trim(v);
  std::string head = s, tail;
  const auto comma = s.find(',');
  if (comma != std::string::npos) { head = strutil::trim(s.substr(0, comma));
                                    tail = strutil::trim(s.substr(comma + 1)); }
  if (strutil::lower(head) != "robust") { *why = strutil::lower(head); return false; }
  if (tail.empty()) return true;
  static const std::regex reNn(R"(^nn\s*\(\s*([0-9]+)\s*\)$)");
  std::smatch m;
  if (!std::regex_match(tail, m, reNn)) { *why = tail; return false; }
  *nn = std::stoi(m.str(1));
  return true;
}
}  // namespace

ojson runTeffects(const std::string& sub, const eval::ParsedCommand& parsed,
                  SessionState& session) {
  // Every other teffects estimator is a DIFFERENT method with its own
  // inference; each keeps its own named refusal rather than borrowing this one.
  if (sub != "nnmatch")
    return ojson(eval::refusedCommand("teffects", "teffects_" + sub + "_unimplemented"));
  if (!engineHasSubcommand("nnmatch")) return engineTooOld("teffects nnmatch", "nnmatch");
  if (!session.hasData() || !session.currentHbin().has_value()) return ojson(eval::noDataLoaded());
  if (auto err = eval::rejectUnknownOptions(parsed.opts, kVocabTeffects.known,
                                            kVocabTeffects.unimplemented)) {
    return ojson(*err);
  }

  std::string genStub, metric, level, dtolerance;
  int nneighbor = 1, vceNn = 2;
  bool atet = false, sawAte = false, sawVce = false;
  for (const auto& [k, v] : parsed.opts) {
    if (k == "generate" || k == "gen") genStub = strutil::trim(v.strValue);
    else if (k == "dtolerance") dtolerance = strutil::trim(v.strValue);
    else if (k == "nneighbor" || k == "nn") {
      const std::string t = strutil::trim(v.strValue);
      if (t.empty() || t.find_first_not_of("0123456789") != std::string::npos)
        return aerErrorReason("teffects nnmatch: nneighbor(#) must be an integer", 198);
      nneighbor = std::stoi(t);
    } else if (k == "atet") atet = true;
    else if (k == "ate") sawAte = true;
    else if (k == "metric") {
      // `metric(matrix M)` names a user matrix; only the metric WORD decides
      // the refusal, and the reason string must stay a single token.
      metric = strutil::lower(strutil::trim(v.strValue));
      const auto sp = metric.find_first_of(" \t");
      if (sp != std::string::npos) metric = metric.substr(0, sp);
    }
    else if (k == "level") level = strutil::trim(v.strValue);
    else if (k == "vce") {
      sawVce = true;
      std::string why;
      if (!parseTeffectsVce(v.strValue, &vceNn, &why)) {
        // `vce(iid)` was captured once for the record and never verified, and
        // an unknown vcetype is a wrong ANSWER, not a wrong option name.
        for (char& c : why)
          if (!std::isalnum(static_cast<unsigned char>(c))) c = '_';
        return ojson(eval::refusedCommand("teffects", "teffects_nnmatch_vce_" + why));
      }
    }
  }
  (void)sawVce;
  if (sawAte && atet)
    return aerSyntaxError("teffects nnmatch: options ate and atet are mutually exclusive");
  if (!metric.empty() && metric != "mahalanobis" && metric != "ivariance" &&
      metric != "euclidean")
    return ojson(eval::refusedCommand("teffects", "teffects_nnmatch_metric_" + metric));
  // `level(#)`: Stata's band, MEASURED at four points on this very command
  // (aer_estim2_oracle.log INFO|nn_level|l0_rc=198, |l9_rc=198, |l9999_rc=0,
  // |l101_rc=198, |labc_rc=198) -- 10.00 <= # <= 99.99, r(198) outside it and
  // r(198) for a non-number. The value is FORWARDED to the engine below; it is
  // not an accept-note, because it moves the reported interval.
  if (!level.empty()) {
    char* endp = nullptr;
    const double lv = std::strtod(level.c_str(), &endp);
    while (endp != nullptr && *endp != '\0' && std::isspace(static_cast<unsigned char>(*endp))) ++endp;
    if (endp == level.c_str() || (endp != nullptr && *endp != '\0') || !std::isfinite(lv) ||
        lv < 10.0 || lv > 99.99)
      return aerErrorReason("teffects nnmatch: level() must be between 10 and 99.99", 198);
  }

  std::vector<std::string> g1, g2;
  std::string why;
  if (!teffectsGroups(parsed.raw, &g1, &g2, &why))
    return aerSyntaxError("teffects nnmatch: syntax is `teffects nnmatch (ovar mvarlist) (tvar) "
                          "[if] [in] [, options]' -- " + why);
  if (g1.size() < 2)
    return aerSyntaxError("teffects nnmatch: the first group needs an outcome and at least one "
                          "matching variable");
  for (const auto& tok : g1) if (tok == ",")
    return ojson(eval::refusedCommand("teffects", "teffects_nnmatch_omvarlist_options"));
  // `(tvar , tlevel(1))` and the treatment-model varlist of ipw/aipw are other
  // commands' grammar; here the second group is exactly one variable.
  if (g2.size() != 1)
    return ojson(eval::refusedCommand("teffects", "teffects_nnmatch_tvar_group_options"));

  const std::string y = g1[0], tvar = g2[0];
  // `mvars` is e(mvarlist): the matching variables in TYPED order with the
  // factor/continuous OPERATOR STRIPPED. Measured (aer_estim2_oracle.log
  // INFO|nn_level|mixed_mvarlist and |cdot_mvarlist, plus the 14-fixture
  // oracle.txt): `(y x1 i.a i.b)` -> `x1 a b`, `(y c.x1 i.a)` -> `x1 a`,
  // `(y i.a i.b)` -> `a b`. xs/fvs alone cannot rebuild that order, which is
  // why the list is accumulated here and not from the two split vectors.
  std::vector<std::string> xs, fvs, mvars;
  for (std::size_t i = 1; i < g1.size(); ++i) {
    std::string tok = g1[i];
    if (tok.find('#') != std::string::npos)
      return ojson(eval::refusedCommand("teffects", "teffects_nnmatch_interaction_terms"));
    const std::string low = strutil::lower(tok);
    if (low.rfind("i.", 0) == 0) { fvs.push_back(tok.substr(2)); mvars.push_back(tok.substr(2)); }
    else if (low.rfind("c.", 0) == 0) { xs.push_back(tok.substr(2)); mvars.push_back(tok.substr(2)); }
    else if (tok.find('.') != std::string::npos)
      // `L.x`, `ib2.a`, `o.x` -- an operator whose meaning this build has not
      // measured for the matching metric. Named, not quietly dropped.
      return ojson(eval::refusedCommand("teffects", "teffects_nnmatch_fv_operator"));
    else { xs.push_back(tok); mvars.push_back(tok); }
  }
  if (xs.empty() && fvs.empty())
    return aerSyntaxError("teffects nnmatch: at least one matching variable is required");

  // gen(stub) mints stub1..stubK where K = e(k_nnmax). Stata refuses r(110) on a
  // collision, but it does NOT scan the whole stub* family and it does not do it
  // all at once -- MEASURED, StataBE 19.5 2026-09-09 on the 200-row fixture
  // (default nneighbor(1), k_nnmax 2):
  //
  //   mm1 exists, nneighbor(1)  -> r(110) "variable mm1 already defined /
  //                                        in option generate(mm)"
  //   mm2 exists, nneighbor(1)  -> r(110) "maximum number of nearest neighbors
  //                                        is 2, but variable mm2 already exists"
  //   mm3 exists, nneighbor(1)  -> rc 0, mm1/mm2 created  (mm3 is NOT touched)
  //   mm3 exists, nneighbor(3)  -> r(110) "variable mm3 already defined ..."
  //
  // i.e. the stubs Stata can name BEFORE it estimates are stub1..stub(nneighbor)
  // -- that many are guaranteed -- and it carries a DIFFERENT text for the ones
  // only the fitted K reveals. The earlier 1..64 pre-scan refused the third case
  // too, which is an honest error where Stata answers: a divergence created by a
  // fix. Both halves below are the measured rule.
  if (!genStub.empty()) {
    if (!strutil::isValidStataIdentifier(genStub + "1"))
      return aerSyntaxError("teffects nnmatch: invalid generate() stub `" + genStub + "'");
    Datastore* ds = nullptr;
    try { ds = session.datastore(); } catch (const std::exception&) { ds = nullptr; }
    if (ds != nullptr)
      for (int k = 1; k <= nneighbor; ++k) {
        const std::string nm = genStub + std::to_string(k);
        if (ds->has(nm))
          return aerErrorReason("teffects nnmatch: variable " + nm + " already defined", 110);
      }
  }

  std::vector<std::string> args = {"nnmatch", "--data", *session.currentHbin(), "--y", y,
                                   "--t", tvar};
  if (!xs.empty()) { args.push_back("--x"); args.push_back(est_detail::joinCommaM4(xs)); }
  if (!fvs.empty()) { args.push_back("--fv"); args.push_back(est_detail::joinCommaM4(fvs)); }
  args.push_back("--nn"); args.push_back(std::to_string(nneighbor));
  args.push_back("--vce-nn"); args.push_back(std::to_string(vceNn));
  if (!metric.empty()) { args.push_back("--metric"); args.push_back(metric); }
  if (atet) { args.push_back("--stat"); args.push_back("atet"); }
  if (!level.empty()) { args.push_back("--level"); args.push_back(level); }
  // BUG-468: `dtolerance(#)`, the exact-match tolerance (default sqrt(c(epsdouble)),
  // e(dtolerance); StataBE 19.5 p-margins/s8.do: dtolerance(1e-7) and (0.05) give
  // the engine's ATE/SE/k_nnmax, dtolerance(0) is r(198) with this message).
  if (!dtolerance.empty()) {
    char* endp = nullptr;
    const double dt = std::strtod(dtolerance.c_str(), &endp);
    if (endp == dtolerance.c_str() || endp == nullptr || *endp != '\0' || !(dt > 0))
      return aerErrorReason("dtolerance(#) must be a real greater than zero", 198);
    args.push_back("--dtolerance");
    args.push_back(dtolerance);
  }
  // `if` restricts the ESTIMATION SAMPLE without renumbering the frame: the
  // engine takes it as a MASK over the original rows, so the ids it returns
  // stay frame `_n` -- which is what Stata does (INFO|nn_edge|ifodd_*) and what
  // paper 076's `merge 1:m match_id1` depends on. BUG-468: `in a/b` joins the
  // same mask as `_n` bounds, so the ids stay frame `_n` there too.
  std::string keepif;
  if (parsed.ifExpr.has_value() && !parsed.ifExpr->empty()) keepif = "(" + *parsed.ifExpr + ")";
  if (parsed.inRange.has_value() && !parsed.inRange->empty()) {
    Datastore* ds = nullptr;
    try { ds = session.datastore(); } catch (const std::exception&) { ds = nullptr; }
    const InRangeBounds rr = parseInRangeBounds(*parsed.inRange, ds != nullptr ? ds->nRows() : 0);
    if (!rr.ok) {
      InRangeFilterResult bad;
      bad.ok = false;
      bad.negative = rr.negative;
      bad.refusal = rr.refusal;
      return inRangeRejectError(bad, *parsed.inRange, "teffects");
    }
    keepif += (keepif.empty() ? "" : " & ") + std::string("(_n>=") + std::to_string(rr.a) +
              " & _n<=" + std::to_string(rr.b) + ")";
  }
  if (!keepif.empty()) {
    args.push_back("--keepif");
    args.push_back(keepif);
  }

  nlohmann::json raw;
  try {
    raw = runCobraJson(args);
  } catch (const CobraError& ce) {
    // Same rc discipline as oaxacaEngineError: an engine refusal carrying an
    // exact Stata rc outranks the coarse mapping, and the default engine_error
    // (which reads as rc 1) is never what comes back.
    const std::string msg = "teffects nnmatch: " + cobraErrorMessage(ce);
    static const std::regex reRc(R"(r\((\d+)\))");
    std::smatch mrc;
    if (std::regex_search(ce.detail, mrc, reRc))
      return aerErrorReason(msg, std::stoi(mrc.str(1)));
    if (ce.code == "no_obs") return aerErrorReason(msg, 2000);
    if (ce.code == "bad_input" || ce.code == "collinear") return aerErrorReason(msg, 198);
    return aerError(msg, ce.code);
  }

  // ---- gen(stub1..stubK) --------------------------------------------------
  // Ragged ids -> padded long columns, ORDER preserved, `.` beyond |Omega(i)|
  // and on every row the estimation sample excluded.
  if (!genStub.empty()) {
    const int kmax = raw.value("k_nnmax", 0);
    const auto& ids = raw["matches"]["ids"];
    // The second half of the measured collision rule (see the pre-estimation
    // guard above): stub(nneighbor+1)..stubK are the ones only the fitted K
    // names, and Stata's text for them is the one that quotes K. Checked for
    // the WHOLE range before a single column is written, so a collision at
    // stub2 does not leave stub1 minted behind an error.
    {
      Datastore* ds2 = nullptr;
      try { ds2 = session.datastore(); } catch (const std::exception&) { ds2 = nullptr; }
      if (ds2 != nullptr)
        for (int k = nneighbor + 1; k <= kmax; ++k) {
          const std::string nm = genStub + std::to_string(k);
          if (ds2->has(nm))
            return aerErrorReason("teffects nnmatch: maximum number of nearest neighbors is " +
                                      std::to_string(kmax) + ", but variable " + nm +
                                      " already exists",
                                  110);
        }
    }
    for (int k = 0; k < kmax; ++k) {
      std::vector<double> col;
      col.reserve(ids.size());
      for (const auto& row : ids)
        col.push_back(static_cast<int>(row.size()) > k
                          ? static_cast<double>(row[k].get<long long>())
                          : std::numeric_limits<double>::quiet_NaN());
      const std::string nm = genStub + std::to_string(k + 1);
      ojson err;
      if (!estimWriteColumn(session, nm, col, &err)) return err;
    }
  }

  // ---- e() ----------------------------------------------------------------
  const std::vector<std::string> names = raw["e_b"]["names"].get<std::vector<std::string>>();
  const std::vector<double> beta = jsonDoubles(raw["e_b"]["values"]);
  session.eval.setEScalar("N", static_cast<double>(raw.value("n_obs", 0)));
  session.eval.setEScalar("n0", static_cast<double>(raw.value("n0", 0)));
  session.eval.setEScalar("n1", static_cast<double>(raw.value("n1", 0)));
  session.eval.setEScalar("treated", raw.value("treated_level", 1.0));
  session.eval.setEScalar("control", raw.value("control_level", 0.0));
  session.eval.setEScalar("k_levels", 2.0);
  session.eval.setEScalar("k_nneighbor", static_cast<double>(raw.value("k_nneighbor", 1)));
  session.eval.setEScalar("k_nnmin", static_cast<double>(raw.value("k_nnmin", 0)));
  session.eval.setEScalar("k_nnmax", static_cast<double>(raw.value("k_nnmax", 0)));
  session.eval.setEScalar("k_robust", static_cast<double>(raw.value("k_robust", 2)));
  session.eval.setEScalar("dtolerance", raw.value("dtolerance", 1.4901161193847656e-8));
  // e(level) is deliberately NOT posted: the full `ereturn list` capture of
  // all 14 oracle fixtures (internal capture
  // oracle.txt SCALARS) has no `level`, and `display e(level)` after
  // `teffects nnmatch ..., level(90)` prints `.`
  // (aer_estim2_oracle.log INFO|nn_level|l90_e_level). Posting it was an
  // invention; the option's ONLY measured effect is on ci_lo/ci_hi.
  session.eval.setEString("cmd", "teffects");
  session.eval.setEString("subcmd", "nnmatch");
  session.eval.setEString("depvar", y);
  session.eval.setEString("tvar", tvar);
  {
    std::string mv;
    for (std::size_t i = 0; i < mvars.size(); ++i) mv += (i > 0 ? " " : "") + mvars[i];
    session.eval.setEString("mvarlist", mv);
  }
  session.eval.setEString("metric", raw.value("metric", std::string("mahalanobis")));
  session.eval.setEString("stat", atet ? "atet" : "ate");
  session.eval.setEString("vce", "robust");
  session.eval.setEString("vcetype", "AI robust");
  session.eval.setEString("title", "Treatment-effects estimation");
  {
    // NOT the literal "0 1": measured on a 1/2-coded treatment
    // (aer_estim2_oracle.log INFO|nn_level|t12_tlevels) Stata answers `1 2`,
    // which is also what e(control)/e(treated)=1/2 and the colname
    // `ATE:r2vs1.t12` in the SAME e() set already said. The levels are
    // integers by construction (the engine refuses a non-integer treatment).
    auto lvl = [](double v) {
      char buf[32];
      std::snprintf(buf, sizeof(buf), "%lld", static_cast<long long>(std::llround(v)));
      return std::string(buf);
    };
    session.eval.setEString("tlevels", lvl(raw.value("control_level", 0.0)) + " " +
                                           lvl(raw.value("treated_level", 1.0)));
  }
  session.eval.setEString("properties", "b V");
  if (!genStub.empty()) {
    std::string iv;
    for (int k = 1; k <= raw.value("k_nnmax", 0); ++k)
      iv += (k > 1 ? " " : "") + genStub + std::to_string(k);
    session.eval.setEString("indexvar", iv);
  }
  auto postM = [&session](const std::string& nm, eval::EvalMatrix m) {
    for (auto& kv : session.eval.eMatrices)
      if (kv.first == nm) { kv.second = std::move(m); return; }
    session.eval.eMatrices.emplace_back(nm, std::move(m));
  };

  LastModel lm;
  lm.cobraCmd = "nnmatch";
  // BUG-458 publishes e(cmd)/e(depvar)/e(vce) out of the LastModel, and it
  // prefers stataCmd -- without this line e(cmd) would come back `nnmatch`,
  // which is the ENGINE verb, where Stata answers `teffects` (with `nnmatch`
  // in e(subcmd), posted separately above).
  lm.stataCmd = "teffects";
  lm.y = y;
  lm.depvar = y;
  lm.x = xs;
  for (const auto& f : fvs) lm.x.push_back("i." + f);
  lm.vcov = "robust";
  lm.dataPath = *session.currentHbin();
  est_detail::captureFitFilterM4(lm, args);
  lm.result = ojson(raw);
  session.setLastModel(lm);
  // eb-order T-P3 part 0: posted after the fresh setLastModel, as oaxaca's
  // (the block's one row is `ATE:r<t>vs<c>.<tvar>`, the raw name, so
  // `_b[ATE:r1vs0.t]` is unchanged, aer_estim2_oracle.log:832).
  {
    eval::EvalMatrix b;
    b.data.push_back(beta);
    b.rownames = {"y1"};
    b.colnames = names;
    postM("b", std::move(b));
    eval::EvalMatrix V;
    for (const auto& row : raw["e_V"]["matrix"]) V.data.push_back(jsonDoubles(row));
    V.rownames = names;
    V.colnames = names;
    postM("V", std::move(V));
  }
  const ojson estBlock = est_detail::cobraToEstimationM4(ojson(raw), "nnmatch", &session);
  session.lastEstBlock = estBlock;

  ojson blocks = ojson::array();
  if (raw.contains("warnings") && raw["warnings"].is_array())
    for (const auto& w : raw["warnings"]) blocks.push_back(displayBlock(w.get<std::string>()));
  blocks.push_back(estBlock);
  // The gen() columns are a DATASET MUTATION and mutation_result.h's contract
  // ("every handler TU that mutates the loaded dataset returns" this envelope)
  // binds here exactly as it does for the estimWriteColumn callers above
  // (runTsfilter / runLowess / runLpoly). Without it routes.cpp's
  // `varlist_after` comes back null and the console's Variables pane never
  // learns that stub1..stubK exist (app.js:1007 guards on it).
  if (!genStub.empty()) {
    std::vector<std::string> minted;
    for (int k = 1; k <= raw.value("k_nnmax", 0); ++k) minted.push_back(genStub + std::to_string(k));
    if (!minted.empty()) blocks.push_back(mutationResult(session, "teffects", minted, {}));
  }
  return blocks;
}


// ===========================================================================
// r(table) after estimation — THE posting site (w2c_postest family 1)
//
// Stata posts r(table) from every e-class estimation's coefficient table: a
// 9 x k matrix with rows `b se t pvalue ll ul df crit eform` (`z` instead of `t`
// for a normal-referenced fit) and one column per e(b) column, in e(b) order —
// INCLUDING the base level of a factor, which is a column of its own
// (`x1 x2 0b.c 1.c 2.c 3.c _cons`, arm rtable_regress_colnames) holding b 0 (1 in
// an eform table) and missing se/t/p/ll/ul. ~25 papers read it straight after the
// estimation command.
//
// Design (one site, not per-command copies): dispatcher.cpp's dispatch() captures
// the identity of the live e() block on entry (liveEstimationIdentity) and calls
// rTableAfterCommand() on exit. "e() changed" is decided by that identity, never by
// comparing the returned block with session.lastEstBlock: a dispatcher arm may
// transform the returned block after the handler stored e() (`logistic` / `logit,
// or` exponentiate it), and a value comparison then misses a real new estimation and
// serves the PREVIOUS model's table.
//   * LEAF command (no other dispatch() ran inside it) that changed e(): the old
//     table is retired, and a new one is built from the estimation block the command
//     returned — the transformed one, so an odds-ratio table is posted as Stata posts
//     it (eform 1). No usable block = no table (an honest "not found").
//   * CONTAINER (quietly, capture, a do-file, a bootstrap-style prefix) that changed
//     e(): the table survives only if it was posted for the e() now live — so a
//     prefix that refits and replaces e() after its inner leaves ran never leaves a
//     replicate's table behind.
//   * `margins` is r-class and posts its own table in Stata; PIET's margins posts
//     none, so the estimation's table is retired rather than served as margins'.
//     Likewise the replicating prefixes (bootstrap, jackknife, permute, rolling):
//     their last replicate's leaf posted a table, and Stata's is the resampling one.
//
// It posts ONLY what it can reproduce exactly; otherwise nothing, and a later read
// is the honest "r(table) not found" instead of a matrix whose cell is not Stata's:
//   * every design row must map onto a varlist term, in term order;
//   * a factor term's base column comes from the estimation sample's levels: the
//     one sample level with no design row IS the base (the base is always such a
//     level, whatever ib#./ib(first) rule chose it). Zero missing levels is `ibn.`;
//     two or more (a collinear-omitted level, or a level outside the fitted sample)
//     cannot be placed with certainty, so nothing is posted;
//   * interactions (`#`), equation-striped tables and omitted continuous regressors
//     are not reproduced (named limitation: no oracle arm pins their column names);
//   * the confidence level is DECLARED — the command's own level(#), else `set
//     level` — never inferred from the block; the reference distribution is t(e(df_r))
//     when the block carries df_r and the normal otherwise; and every row's p-value
//     and interval must agree with that level and reference. A block that disagrees
//     (tobit prints t intervals without publishing df_r; `set level` does not reach
//     regress's engine interval) posts nothing. The two exceptions are the ones
//     whose Stata definition is fixed and whose block is known to use another
//     reference: nlcom (always normal in Stata) and poisson/ppmlhdfe (normal in
//     Stata; the engine prints t(dof_resid) intervals) — their p/ll/ul are rebuilt
//     from b and se under the normal.
//
// Which commands replace it: in Stata EVERY r-class command replaces all of r(),
// r(table) included, and an e-class fit replaces r() with its own table
// (w2n_followups.log:968, E07). PIET's r-class commands write r() from dozens of
// handlers, most of which MERGE their keys into what the previous command left, so
// the replacement is done generically, at one site (DIAGNOSIS.md
// postest-rtable F, design W2): dispatch() takes an r() mark at entry
// (eval::RStoreMark); when the command returns, if it wrote r() at all (a store
// generation moved, see eval/session.h nextStoreGeneration), replaceStaleRClass
// erases every r() scalar, user macro and matrix last written BEFORE the mark --
// the stale r(table) among them (arms rtable_survives_summarize,
// rtable_replaced_by_test). A container (a program, a prefix) is judged against
// the mark taken after its last inner command, which is where an rclass program's
// `return` set lands. A command that writes nothing leaves r() alone (a failing
// `summarize nosuchvar`: w2n_followups.log:988/:1000). The bookkeeping that says
// which r(table) is the estimation machinery's (EvalSession::rTableGen/Eid/Model)
// and the refusal note (rTableRefusal) live OUTSIDE every r() store, so no
// wholesale wipe of r() can delete them while the table lives on (the 10-11
// review: `label language` deleted a stamp kept in rStrings and the table was
// never retired again).
namespace {

double rtNan() { return std::numeric_limits<double>::quiet_NaN(); }

double rowNum(const ojson& row, const char* key) {
  if (!row.contains(key) || !row.at(key).is_number()) return rtNan();
  return row.at(key).get<double>();
}

double critFor(double level, bool zRef, double df) {
  const double alpha = (level == 95.0) ? 0.05 : 1.0 - level / 100.0;
  return zRef ? cecon::z_critical(alpha) : cecon::t_critical(alpha, df);
}

// Levels each factor variable takes over the rows the fit could have used (every
// model column that is a dataset column non-missing). A superset of the sample at
// worst — which only ever turns a post into a (safe) non-post, never into a wrong
// base, because a superset can only ADD levels without design rows.
bool sampleFactorLevels(SessionState& session, const LastModel& lm,
                        const std::vector<std::string>& fvars,
                        std::map<std::string, std::vector<double>>& out,
                        const std::vector<std::string>& maskVars = {}) {
  std::vector<std::string> cols;
  // Plain column names only: a `L.x`/`i.c`/`a#b` token is not a dataset column.
  auto want = [&cols](const std::string& c) {
    if (c.empty() || c.find_first_of(".#()") != std::string::npos) return;
    if (std::find(cols.begin(), cols.end(), c) == cols.end()) cols.push_back(c);
  };
  want(lm.y);
  for (const auto& x : lm.x) want(x);
  for (const auto& v : fvars) want(v);
  for (const auto& v : maskVars) want(v);  // an interaction's continuous atoms
  for (const auto& f : lm.fe) want(f);

  std::unique_ptr<Datastore> owned;
  const Datastore* ds = nullptr;
  std::optional<ColumnScope> scope;
  try {
    if (!lm.fitDataPath.empty()) {
      owned = std::make_unique<Datastore>(lm.fitDataPath);
      ds = owned.get();
    } else {
      eval::ParsedCommand none;
      scope.emplace(session, none, cols, "r(table)");
      if (scope->failed()) return false;
      ds = scope->get();
    }
  } catch (const std::exception&) {
    return false;
  }
  if (ds == nullptr) return false;
  const size_t n = ds->nRows();
  std::vector<char> keep(n, 1);
  std::string err;
  std::vector<double> buf;
  for (const auto& c : cols) {
    if (!ds->has(c)) continue;
    if (!numericColumn(*ds, c, &buf, &err)) continue;
    for (size_t i = 0; i < n; ++i)
      if (std::isnan(buf[i])) keep[i] = 0;
  }
  for (const auto& v : fvars) {
    if (!ds->has(v) || !numericColumn(*ds, v, &buf, &err)) return false;
    std::vector<double> lv;
    for (size_t i = 0; i < n; ++i)
      if (keep[i]) lv.push_back(buf[i]);
    std::sort(lv.begin(), lv.end());
    lv.erase(std::unique(lv.begin(), lv.end()), lv.end());
    out[v] = std::move(lv);
  }
  return true;
}

struct RtCol {
  std::string name;
  const ojson* row = nullptr;  // null: a base-level column (b 0, the rest missing)
};

// An omitted continuous regressor is reproducible as Stata's `o.x` column only when
// the omission cannot depend on drop order: the variable is constant within every
// absorbed group (constant overall when nothing is absorbed), so Stata's rmcoll
// drops it whatever else is in the model. A collinear PAIR (z = 2x) is not: which
// member goes depends on the command (reghdfe/areg/xtreg,fe keep x and omit z,
// regress the reverse; StataBE scratch-r150/rtab/r1.do, BUG-1067), and that case
// keeps the named refusal here. The rows are the
// superset sampleFactorLevels uses, which only errs toward refusing.
bool omittedTermIsUnambiguous(SessionState& session, const LastModel& lm, const std::string& term) {
  if (term.empty() || term.find_first_of(".#()") != std::string::npos) return false;
  std::vector<std::string> cols;
  auto want = [&cols](const std::string& c) {
    if (c.empty() || c.find_first_of(".#()") != std::string::npos) return;
    if (std::find(cols.begin(), cols.end(), c) == cols.end()) cols.push_back(c);
  };
  want(lm.y);
  for (const auto& x : lm.x) want(x);
  for (const auto& f : lm.fe) want(f);
  want(term);
  std::unique_ptr<Datastore> owned;
  const Datastore* ds = nullptr;
  std::optional<ColumnScope> scope;
  try {
    if (!lm.fitDataPath.empty()) {
      owned = std::make_unique<Datastore>(lm.fitDataPath);
      ds = owned.get();
    } else {
      eval::ParsedCommand none;
      scope.emplace(session, none, cols, "r(table)");
      if (scope->failed()) return false;
      ds = scope->get();
    }
  } catch (const std::exception&) {
    return false;
  }
  if (ds == nullptr || !ds->has(term)) return false;
  const size_t n = ds->nRows();
  std::vector<char> keep(n, 1);
  std::string err;
  std::vector<double> buf;
  for (const auto& c : cols) {
    if (!ds->has(c) || !numericColumn(*ds, c, &buf, &err)) continue;
    for (size_t i = 0; i < n; ++i)
      if (std::isnan(buf[i])) keep[i] = 0;
  }
  std::vector<double> xv;
  if (!numericColumn(*ds, term, &xv, &err)) return false;
  std::vector<std::vector<double>> fe;
  for (const auto& f : lm.fe) {
    if (f.find_first_of(".#()") != std::string::npos || !ds->has(f)) return false;
    fe.emplace_back();
    if (!numericColumn(*ds, f, &fe.back(), &err)) return false;
  }
  std::map<std::vector<double>, double> firstByGroup;
  bool any = false;
  for (size_t i = 0; i < n; ++i) {
    if (!keep[i]) continue;
    any = true;
    std::vector<double> key;
    key.reserve(fe.size());
    for (const auto& col : fe) key.push_back(col[i]);
    auto [it, inserted] = firstByGroup.emplace(std::move(key), xv[i]);
    if (!inserted && it->second != xv[i]) return false;
  }
  return any;
}

// Stata's spelling of an omitted column: `o.x` for a plain variable, and inside a
// factor-variable name an `o` on every atom off its base level: `2o.g`,
// `0b.a#1o.b`, `0b.g#co.x` (StataBE 19.5 `regress y i.a##i.b`, `c.x##i.g`).
std::string omittedColumnName(const std::string& name) {
  if (name.find('.') == std::string::npos) return "o." + name;
  static const std::regex atom(R"((^|#)([0-9]+|c)\.)");
  return std::regex_replace(name, atom, "$1$2o.");
}

// One atom of a factor-variable interaction: `i.g`, `ib2.g`, `ib(first).g`,
// `ib(last).g`, a bare `g` (inside an interaction Stata and the engine read it as
// `i.g`: StataBE `regress y a##g` names `0b.a ... 2.a#1.g`) -- a factor, with
// the rule for its base level -- or `c.x` (continuous). False = a spelling these
// columns do not reproduce: `ibn.`, `ib(freq).`, `i2.`, a time-series operator.
struct FvAtom {
  std::string spelling;  // as typed
  std::string var;
  bool factor = false;
  int baseRule = 0;  // 0 the lowest level, 1 `ib#.`'s level, 2 the highest level
  double base = 0;
};

bool parseFvAtom(const std::string& s, FvAtom& a) {
  a.spelling = s;
  const size_t dot = s.find('.');
  if (dot == std::string::npos) {
    if (s.empty() || s.find_first_of("#()") != std::string::npos) return false;
    a.var = s;
    a.factor = true;
    return true;
  }
  if (dot == 0 || dot + 1 >= s.size()) return false;
  const std::string op = s.substr(0, dot);
  a.var = s.substr(dot + 1);
  if (a.var.find_first_of(".#()") != std::string::npos) return false;
  if (op == "c") return true;
  a.factor = true;
  if (op == "i" || op == "ib(first)") return true;
  if (op == "ib(last)") {
    a.baseRule = 2;
    return true;
  }
  if (op.size() < 3 || op.compare(0, 2, "ib") != 0) return false;
  for (size_t i = 2; i < op.size(); ++i)
    if (!std::isdigit(static_cast<unsigned char>(op[i]))) return false;
  a.baseRule = 1;
  a.base = std::strtod(op.c_str() + 2, nullptr);
  return true;
}

// `A##B##C` is the factorial: every sub-interaction, in fvexpand's order A, B,
// A#B, C, A#C, B#C, A#B#C (StataBE `regress y i.a##c.x##i.g`); `A#B#C` is the
// one interaction. A term mixing `#` and `##` is refused.
bool expandFvTerm(const std::string& term, std::vector<std::vector<FvAtom>>& subs) {
  const bool factorial = term.find("##") != std::string::npos;
  std::vector<FvAtom> atoms;
  size_t start = 0;
  for (;;) {
    const size_t h = term.find('#', start);
    FvAtom a;
    if (!parseFvAtom(term.substr(start, h == std::string::npos ? std::string::npos : h - start), a))
      return false;
    atoms.push_back(std::move(a));
    if (h == std::string::npos) break;
    if (factorial) {
      if (h + 1 >= term.size() || term[h + 1] != '#') return false;
      start = h + 2;
    } else {
      start = h + 1;
    }
  }
  if (atoms.size() < 2 || atoms.size() > 8) return false;
  if (!factorial) {
    subs.push_back(std::move(atoms));
    return true;
  }
  // A repeated subterm appears once (`c.x##c.x` is `x c.x#c.x`, StataBE).
  std::set<std::vector<std::string>> seen;
  for (unsigned mask = 1; mask < (1u << atoms.size()); ++mask) {
    std::vector<FvAtom> s;
    std::vector<std::string> id;
    for (size_t i = 0; i < atoms.size(); ++i)
      if (mask & (1u << i)) {
        s.push_back(atoms[i]);
        id.push_back(atoms[i].spelling);
      }
    if (seen.insert(id).second) subs.push_back(std::move(s));
  }
  return true;
}

// Map the design rows onto Stata's e(b) column order. False = cannot reproduce.
bool stataColumns(SessionState& session, const LastModel& lm, const ojson& ct,
                  std::vector<RtCol>& cols) {
  std::vector<bool> used(ct.size(), false);
  int icpt = -1;
  auto rowName = [&ct](size_t i) { return ct[i].value("name", std::string()); };
  for (size_t i = 0; i < ct.size(); ++i) {
    const std::string nm = rowName(i);
    if (nm.find(':') != std::string::npos || ct[i].contains("eq") || ct[i].contains("equation"))
      return false;
    if (nm == "(intercept)" || nm == "_cons") icpt = static_cast<int>(i);
  }
  // A plain term (a variable, a factor's main effect) or an interaction's atoms.
  struct Item {
    std::string term;
    std::vector<FvAtom> atoms;
    bool repeat = false;  // a continuous term already in the varlist
  };
  std::vector<Item> items;
  std::vector<std::string> fvars, cvars;
  // A term's identity: its atoms as `i.v`/`c.v`, sorted (`a#g` is `g#a`).
  const auto termKey = [](const std::vector<FvAtom>& s) {
    std::vector<std::string> id;
    for (const auto& a : s) id.push_back((a.factor ? "i." : "c.") + a.var);
    std::sort(id.begin(), id.end());
    std::string key;
    for (const auto& p : id) key += p + "#";
    return key;
  };
  std::set<std::string> modelKeys;
  // A factor-variable term repeated in the varlist is one term, at its first
  // position (StataBE `i.g i.g` is `0b.g 1.g`; `a##g b##g` has one i.g); a
  // repeated continuous term is not (`x x` is `x o.x`).
  const auto first = [&](const std::vector<FvAtom>& s) {
    const bool anyFactor =
        std::any_of(s.begin(), s.end(), [](const FvAtom& a) { return a.factor; });
    return modelKeys.insert(termKey(s)).second || !anyFactor;
  };
  for (const auto& term : lm.x) {
    if (term.find('#') == std::string::npos) {
      FvAtom fa;
      // `c.x` alone is the column `x`
      const bool cont = term.compare(0, 2, "c.") == 0 && term.find('.', 2) == std::string::npos;
      const std::string col = cont ? term.substr(2) : term;
      const std::vector<std::string> fv = cecon::postest::factor_vars_of({term});
      if (term.find('.') != std::string::npos && parseFvAtom(term, fa) && fa.factor) {
        if (!first({fa})) continue;
        items.push_back({col, {}});
      } else {
        bool seen = false;
        if (fv.empty()) {
          fa.var = col;
          fa.factor = false;
          seen = modelKeys.count(termKey({fa})) != 0;
          first({fa});
        }
        items.push_back({col, {}, seen});
      }
      for (const auto& v : fv) fvars.push_back(v);
      continue;
    }
    std::vector<std::vector<FvAtom>> subs;
    if (!expandFvTerm(term, subs)) return false;
    for (auto& s : subs) {
      if (s.size() == 1 && !s[0].factor) {
        // a factorial's continuous main effect
        const bool seen = modelKeys.count(termKey(s)) != 0;
        first(s);
        cvars.push_back(s[0].var);
        items.push_back({s[0].var, {}, seen});
        continue;
      }
      if (!first(s)) continue;
      for (const auto& a : s) (a.factor ? fvars : cvars).push_back(a.var);
      if (s.size() == 1)
        items.push_back(
            {s[0].spelling.find('.') == std::string::npos ? "i." + s[0].var : s[0].spelling, {}});
      else
        items.push_back({std::string(), std::move(s)});
    }
  }
  std::map<std::string, std::vector<double>> levels;
  if (!fvars.empty() && !sampleFactorLevels(session, lm, fvars, levels, cvars)) return false;

  for (const auto& item : items) {
    if (!item.atoms.empty()) {
      // The interaction's cells in Stata's order (the first factor slowest), each
      // named factor atoms first, then the continuous ones (`1.g#c.x` for
      // `c.x#i.g`); the engine's row is the cell's atoms in any order
      // (interaction_key: it names that cell `x#1.g`).
      std::vector<const FvAtom*> fac, con;
      for (const auto& a : item.atoms) (a.factor ? fac : con).push_back(&a);
      std::vector<const std::vector<double>*> lv;
      std::vector<double> base;
      for (const FvAtom* a : fac) {
        const auto f = levels.find(a->var);
        if (f == levels.end() || f->second.empty()) return false;
        for (double v : f->second)
          if (v < 0 || v != std::floor(v)) return false;  // not a legal factor level
        const double b = a->baseRule == 0 ? f->second.front()
                         : a->baseRule == 2 ? f->second.back()
                                            : a->base;
        if (!std::binary_search(f->second.begin(), f->second.end(), b)) return false;
        lv.push_back(&f->second);
        base.push_back(b);
      }
      std::vector<size_t> pos(fac.size(), 0);
      for (bool more = true; more;) {
        std::string name, engine;
        std::vector<FvAtom> rest;  // the cell's atoms off their base level
        bool anyBase = false;
        for (size_t k = 0; k < fac.size(); ++k) {
          const double v = (*lv[k])[pos[k]];
          std::string at = cecon::postest::level_name(v, fac[k]->var);
          engine += (engine.empty() ? "" : "#") + at;
          if (v == base[k]) {
            at.insert(at.find('.'), "b");
            anyBase = true;
          } else {
            rest.push_back(*fac[k]);
          }
          name += (name.empty() ? "" : "#") + at;
        }
        for (const FvAtom* a : con) {
          engine += (engine.empty() ? "" : "#") + a->var;
          name += (name.empty() ? "" : "#") + ("c." + a->var);
          rest.push_back(*a);
        }
        const std::string key = cecon::postest::interaction_key(engine);
        const ojson* row = nullptr;
        for (size_t i = 0; i < ct.size(); ++i) {
          if (used[i] || cecon::postest::interaction_key(rowName(i)) != key) continue;
          row = &ct[i];
          used[i] = true;
          break;
        }
        // Stata's fvexpand: a cell with an atom at its base level is the
        // interaction's base when nothing else is left (`0b.a#0b.b`), omitted when
        // the term left without those atoms is in the model, wherever it stands
        // (`c.x##i.g` and `c.x#i.g c.x` both omit `0b.g#co.x`; `i.a i.a#i.g` omits
        // `1o.a#0b.g` and keeps `0b.a#1.g`: StataBE 19.5), and estimable
        // otherwise (`i.a#i.b` alone keeps `0b.a#1.b`). The engine must have built
        // the same design: a column it estimated that Stata omits is another
        // parameterization, and the stripe is refused.
        const bool isBase = anyBase && rest.empty();
        const bool isOmitted = anyBase && !rest.empty() && modelKeys.count(termKey(rest)) != 0;
        if ((isBase || isOmitted) && row != nullptr &&
            !(rowNum(*row, "b") == 0.0 &&
              (rowNum(*row, "se") == 0.0 || std::isnan(rowNum(*row, "se")))))
          return false;
        // A cell with no design column is omitted unless it is the base: an `o` on
        // each atom off its base (`0b.a#1o.b`, `0b.g#co.x`, an empty cell `2o.a#1o.b`).
        if (row == nullptr && !isBase) name = omittedColumnName(name);
        cols.push_back({name, row});
        more = false;
        for (size_t k = fac.size(); k-- > 0;) {
          if (++pos[k] < lv[k]->size()) {
            more = true;
            break;
          }
          pos[k] = 0;
        }
      }
      continue;
    }
    const std::string& term = item.term;
    const std::vector<std::string> fv = cecon::postest::factor_vars_of({term});
    if (fv.empty()) {
      bool found = false;
      for (size_t i = 0; i < ct.size(); ++i) {
        if (used[i] || rowName(i) != term) continue;
        used[i] = true;
        cols.push_back({term, &ct[i]});
        found = true;
        break;
      }
      if (!found) {
        // An omitted continuous regressor with no row: Stata's `o.x` column
        // (b 0, the rest missing; StataBE 007 FA3 `o.miss_age_PAP`), posted only
        // when the omission is order-independent (see omittedTermIsUnambiguous)
        // or the term repeats one earlier in the varlist (`x x` is `x o.x`).
        if (!item.repeat && !omittedTermIsUnambiguous(session, lm, term)) return false;
        cols.push_back({"o." + term, nullptr});
      }
      continue;
    }
    const std::string& v = fv.front();
    if (term.compare(0, 4, "ibn.") == 0) return false;
    std::map<double, size_t> rowsByLevel;
    for (size_t i = 0; i < ct.size(); ++i) {
      if (used[i]) continue;
      double lv = 0;
      std::string var;
      if (cecon::postest::parse_level_ref(rowName(i), lv, var) && var == v) rowsByLevel[lv] = i;
    }
    auto it = levels.find(v);
    if (it == levels.end()) return false;
    std::vector<double> missing;
    for (double lv : it->second) {
      if (lv < 0 || lv != std::floor(lv)) return false;  // not a legal factor level
      if (!rowsByLevel.count(lv)) missing.push_back(lv);
    }
    if (missing.size() != 1 || rowsByLevel.empty()) return false;
    std::vector<double> all = it->second;
    for (const auto& kv : rowsByLevel)
      if (!std::binary_search(all.begin(), all.end(), kv.first)) return false;
    for (double lv : all) {
      if (lv == missing.front()) {
        std::string nm = cecon::postest::level_name(lv, v);
        nm.insert(nm.find('.'), "b");
        cols.push_back({nm, nullptr});
      } else {
        const size_t i = rowsByLevel[lv];
        used[i] = true;
        cols.push_back({rowName(i), &ct[i]});
      }
    }
  }
  for (size_t i = 0; i < ct.size(); ++i)
    if (!used[i] && static_cast<int>(i) != icpt) return false;  // a row no term explains
  if (icpt >= 0) cols.push_back({"_cons", &ct[static_cast<size_t>(icpt)]});
  return !cols.empty();
}

// eb-order T-E2 (ME08/C7/EB-BASE): record Stata's e(b) stripe on the live
// estimation block, once, at the fit -- the columns stataColumns places for
// r(table) (the same base-level rule: the one sample level without a design
// row), plus the `o.` spelling of a column the binary perfect-prediction screen
// dropped (the engine's reply lists it in `omitted`; Stata posts `x1 o.dz0
// _cons`, w2u_binary_ml_edges.log:215). Every e(b) reader takes it from here
// (eval::ebStripeOf / stataEbColumns / ebStripeCoef): `matrix X = e(b)`,
// `: colnames e(b)`, el()/colsof(), `_b[0b.g]`, e(V), parmest and regsave. A
// fit whose columns stataColumns cannot place (interactions, equations, an
// ambiguous base) records nothing and keeps the readers' named refusals.
void annotateEbStripe(SessionState& session) {
  if (!session.lastEstBlock.has_value() || !session.lastEstBlock->is_object()) return;
  ojson& blk = *session.lastEstBlock;
  blk.erase("eb_stripe");
  const auto& lmOpt = session.lastModel();
  if (!lmOpt.has_value() || !blk.contains("coeftable") || !blk["coeftable"].is_array() ||
      blk["coeftable"].empty())
    return;
  std::vector<std::string> omitted;
  if ((lmOpt->cobraCmd == "logit" || lmOpt->cobraCmd == "probit") && lmOpt->result.has_value() &&
      lmOpt->result->is_object() && lmOpt->result->contains("omitted") &&
      (*lmOpt->result)["omitted"].is_array()) {
    for (const auto& o : (*lmOpt->result)["omitted"])
      if (o.is_string()) omitted.push_back(o.get<std::string>());
  }
  std::vector<RtCol> cols;
  try {
    if (!stataColumns(session, *lmOpt, blk["coeftable"], cols)) return;
  } catch (const std::exception&) {
    return;
  }
  bool special = false;
  ojson stripe = ojson::array();
  for (const auto& c : cols) {
    ojson e;
    if (c.row == nullptr) {
      e["name"] = c.name;
      e["src"] = "";
      special = true;
    } else {
      const bool zero = rowNum(*c.row, "b") == 0.0 &&
                        (rowNum(*c.row, "se") == 0.0 || std::isnan(rowNum(*c.row, "se")));
      const bool om = std::find(omitted.begin(), omitted.end(), c.name) != omitted.end() || zero;
      std::string name = om ? omittedColumnName(c.name) : c.name;
      e["name"] = name;
      std::string src = c.row->value("name", std::string());
      if (src == "(intercept)") src = "_cons";
      e["src"] = src;
      // a column Stata names otherwise than the engine (`1.g#c.x` for `x#1.g`)
      special = special || om || name != src;
    }
    stripe.push_back(e);
  }
  if (!special) {
    // Stata orders the columns otherwise than the engine (`i.a##c.x##i.g`: the
    // engine puts i.g before i.a#c.x): the readers' default is the engine's
    // order with `_cons` last.
    std::vector<std::string> engine, mine;
    for (const auto& r : blk["coeftable"]) {
      const std::string n = r.value("name", std::string());
      if (n != "(intercept)" && n != "_cons") engine.push_back(n);
    }
    if (engine.size() < blk["coeftable"].size()) engine.push_back("_cons");
    for (const auto& e : stripe) mine.push_back(e.value("src", std::string()));
    special = engine != mine;
  }
  if (special) {
    // The engine's covariance names only the independent columns. Omitted
    // columns are exact zero rows/columns, so retain them on the same fitted
    // matrix before every e(V) consumer applies the stripe.
    if (blk.contains("e_V") && blk["e_V"].is_object()) {
      auto& v = blk["e_V"];
      if (v.contains("names") && v["names"].is_array() &&
          v.contains("matrix") && v["matrix"].is_array()) {
        for (const auto& c : cols) {
          if (!c.row || rowNum(*c.row, "b") != 0.0 || rowNum(*c.row, "se") != 0.0) continue;
          const std::string src = c.row->value("name", std::string());
          if (std::find(v["names"].begin(), v["names"].end(), ojson(src)) != v["names"].end()) continue;
          for (auto& row : v["matrix"]) row.push_back(0.0);
          v["names"].push_back(src);
          v["matrix"].push_back(std::vector<double>(v["names"].size(), 0.0));
        }
      }
    }
    blk["eb_stripe"] = std::move(stripe);
  }
}

// What the live e() estimated: estimator, depvar, N and the coefficient table. An e()
// replaced by a copy that estimates the same thing (`estadd` re-assigns e() with a
// scalar added) keeps its table; any other replacement retires it. Computed only
// when a table is posted or e() was replaced — never on the per-command path.
std::string modelPrint(const SessionState& session) {
  if (!session.lastEstBlock.has_value() || !session.lastEstBlock->is_object()) return std::string();
  const ojson& b = *session.lastEstBlock;
  std::string s = b.value("estimator", std::string()) + "\x04" + b.value("depvar", std::string()) + "\x04";
  if (b.contains("n_obs")) s += b["n_obs"].dump();
  s += "\x04";
  if (b.contains("coeftable")) s += b["coeftable"].dump();
  return std::to_string(std::hash<std::string>{}(s));
}

// Post `tbl` as r(table) and record it as the estimation machinery's own, for the
// e() that is live now (EvalSession::rTableGen/Eid/Model).
void setRTable(SessionState& session, eval::EvalMatrix tbl) {
  auto& ev = session.eval;
  tbl.gen = eval::nextStoreGeneration();  // posted now, whenever it was built
  ev.rTableGen = tbl.gen;
  ev.rTableEid = liveEstimationIdentity(session);
  ev.rTableModel = modelPrint(session);
  for (auto& kv : ev.rMatrices) {
    if (kv.first == "table") {
      kv.second = std::move(tbl);
      return;
    }
  }
  ev.rMatrices.emplace_back("table", std::move(tbl));
}

void forgetRTableRefusal(eval::EvalSession& ev) {
  ev.rTableRefusal.clear();
  ev.rTableRefusalEid = 0;
  ev.rTableRefusalGen = 0;
}

void retireRTable(SessionState& session) {
  auto& ev = session.eval;
  for (auto it = ev.rMatrices.begin(); it != ev.rMatrices.end();) {
    if (it->first == "table") it = ev.rMatrices.erase(it);
    else ++it;
  }
  ev.rTableGen = 0;
  ev.rTableEid = 0;
  ev.rTableModel.clear();
  forgetRTableRefusal(ev);
}

// Is the r(table) now in r() the one the estimation machinery posted? A table
// posted by anyone else (sktest's) has another gen.
bool ownsRTable(const eval::EvalSession& ev) {
  const eval::EvalMatrix* t = ev.findClassMatrix('r', "table");
  return t != nullptr && ev.rTableGen != 0 && t->gen == ev.rTableGen;
}

// e() was replaced but by a copy of the same model (see modelPrint): re-bind the
// table to the new identity and keep it. False = the table must go.
bool rebindIfSameModel(SessionState& session, std::uintptr_t eIdNow) {
  auto& ev = session.eval;
  if (!ownsRTable(ev)) return false;
  if (ev.rTableEid == eIdNow) return true;
  if (ev.rTableModel.empty() || ev.rTableModel != modelPrint(session)) return false;
  ev.rTableEid = eIdNow;
  return true;
}

// The confidence level a command declared: its own level(#), else `set level`.
// NaN when a level() is present but not a number this code can read (an unexpanded
// macro, say) — the caller then posts nothing rather than guess.
double declaredLevel(const std::string& rawCmd, const SessionState& session) {
  const long long comma = eval::findUnquotedComma(rawCmd);
  if (comma >= 0) {
    std::vector<std::pair<std::string, eval::OptValue>> opts;
    try {
      opts = eval::parseOpts(rawCmd.substr(static_cast<size_t>(comma) + 1));
    } catch (const std::exception&) {
      return rtNan();
    }
    for (const auto& [k, v] : opts) {
      if (strutil::lower(k) != "level") continue;
      if (v.isBool) return rtNan();
      const std::string s = strutil::trim(v.strValue);
      char* end = nullptr;
      const double lv = std::strtod(s.c_str(), &end);
      if (s.empty() || end == nullptr || *end != '\0' || !(lv >= 10.0 && lv <= 99.99)) return rtNan();
      return lv;
    }
  }
  return static_cast<double>(session.level);
}

// eval/macros.cpp turns a later read of r(table) into a NAMED r(111) while this
// note lives: Stata posts a table there, this build does not.
void refuseRTable(SessionState& session, const std::string& why) {
  auto& ev = session.eval;
  ev.rTableRefusal = why;
  ev.rTableRefusalEid = liveEstimationIdentity(session);
  ev.rTableRefusalGen = eval::nextStoreGeneration();
}

// True when the refusal note was left for the e() that is live now.
bool refusedFor(const SessionState& session, std::uintptr_t eIdNow) {
  return !session.eval.rTableRefusal.empty() && session.eval.rTableRefusalEid == eIdNow;
}

std::string estimationRefusal(const SessionState& session) {
  const auto& lm = session.lastModel();
  if (lm.has_value())
    for (const auto& term : lm->x)
      if (term.find('#') != std::string::npos)
        return "r(table) is not posted after a model with interaction (#) terms in this "
               "version of PIET: Stata's column names for interaction cells are not reproduced. "
               "Read the coefficients with _b[] or copy e(b) instead.";
  return "r(table) is not posted after this estimation in this version of PIET: its "
         "coefficient table (omitted or equation-striped coefficients, or a confidence level "
         "or reference distribution this build cannot reproduce exactly) is not Stata's. "
         "Read the coefficients with _b[]/_se[] instead.";
}

// First word of a command line, lower-cased.
std::string commandWord(const std::string& rawCmd) {
  size_t i = 0;
  while (i < rawCmd.size() && std::isspace(static_cast<unsigned char>(rawCmd[i]))) ++i;
  size_t j = i;
  while (j < rawCmd.size() && !std::isspace(static_cast<unsigned char>(rawCmd[j])) &&
         rawCmd[j] != ',')
    ++j;
  return strutil::lower(rawCmd.substr(i, j - i));
}

}  // namespace

std::uintptr_t liveEstimationIdentity(const SessionState& session) {
  if (!session.lastEstBlock.has_value()) return 0;
  // An e() block is replaced by assignment (`session.lastEstBlock = block`), and
  // nlohmann's assignment builds the new value before it releases the old one, so
  // the address of the object's storage is a new one for every new estimation. It
  // is read in O(1) — dispatch() calls this for every command, loops included.
  if (const auto* obj = session.lastEstBlock->get_ptr<const ojson::object_t*>())
    return reinterpret_cast<std::uintptr_t>(obj);
  return static_cast<std::uintptr_t>(std::hash<std::string>{}(session.lastEstBlock->dump())) | 1u;
}

bool buildRTableFromBlock(SessionState& session, const ojson& block, double level,
                          eval::EvalMatrix& out) {
  if (!block.is_object() || !block.contains("coeftable") || !block["coeftable"].is_array() ||
      block["coeftable"].empty())
    return false;
  if (!(level >= 10.0 && level <= 99.99)) return false;
  const ojson& ct = block["coeftable"];
  const std::string estimator = block.value("estimator", std::string());
  // bootstrap's table (handlers_resample.cpp runBootstrap) is normal-based too,
  // named by its own e(b) stripe: the same shape as nlcom's.
  const bool isNlcom = estimator == "nlcom" || estimator == "bootstrap";
  // runMargins' own table (estimation_m4c.cpp postMarginsRClassM4): its columns
  // are margins' r(b) names (`_cons`, `1.t`, `x1`), not the model's e(b); the
  // reference is t(df_r) when the block carries one, the normal otherwise.
  const bool isMargins = estimator == "margins_rtable";

  std::vector<RtCol> cols;
  if (isNlcom || isMargins) {
    for (const auto& row : ct) cols.push_back({row.value("name", std::string()), &row});
  } else {
    const auto& lmOpt = session.lastModel();
    if (!lmOpt.has_value() || !stataColumns(session, *lmOpt, ct, cols)) return false;
  }

  // Reference distribution: t(e(df_r)) when the fit publishes a residual df
  // (G-1 under clustering), the normal otherwise (logit, ivregress's default, ...:
  // `z` row, df missing). nlcom is ALWAYS normal in Stata, whatever the model
  // (w2c_postest.log prints `z` after `regress`).
  const double dfR = (!isNlcom && block.contains("df_r") && block["df_r"].is_number())
                         ? block["df_r"].get<double>() : rtNan();
  const bool zRef = isNlcom || !(dfR > 0);
  const double crit = critFor(level, zRef, dfR);
  if (!(crit > 0)) return false;
  // Blocks whose Stata definition is normal-referenced but whose printed p/CI use
  // another reference: rebuilt from b and se (see the header).
  const bool rebuild = isNlcom || estimator == "poisson" || estimator == "ppmlhdfe" ||
                       estimator == "fepois";
  // An eform block (`logistic`, `logit, or`, stcox's hazard ratios) carries exp(b),
  // exp(b)*se and exp() of the interval; the checks run on the coefficient metric.
  const bool eform = block.contains("coef_metric") && block["coef_metric"].is_string();

  eval::EvalMatrix m;
  m.rownames = {"b", "se", zRef ? "z" : "t", "pvalue", "ll", "ul", "df", "crit", "eform"};
  m.data.assign(9, std::vector<double>());
  for (const auto& c : cols) {
    // An engine-omitted column (b 0 and se 0, kept in the table) is Stata's
    // `o.` column: b 0, se/t/p/ll/ul missing -- the same zero rule
    // annotateEbStripe applies to e(b), so r(table) and e(b) name it alike
    // (StataBE r1.do: an omitted column is `o.z`, se `.`).
    if (c.row != nullptr && !eform && rowNum(*c.row, "b") == 0.0 && rowNum(*c.row, "se") == 0.0) {
      m.colnames.push_back(omittedColumnName(c.name));
      const double col[9] = {0.0, rtNan(), rtNan(), rtNan(), rtNan(), rtNan(), zRef ? rtNan() : dfR, crit, 0.0};
      for (int r = 0; r < 9; ++r) m.data[static_cast<size_t>(r)].push_back(col[r]);
      continue;
    }
    m.colnames.push_back(c.name);
    double b = 0, se = rtNan(), t = rtNan(), p = rtNan(), ll = rtNan(), ul = rtNan();
    if (c.row != nullptr) {
      b = rowNum(*c.row, "b");
      se = rowNum(*c.row, "se");
      if (eform) {
        if (!(b > 0)) return false;
        se = se / b;
        b = std::log(b);
      }
      if (se > 0) {
        if (rebuild) {
          t = b / se;
          p = std::erfc(std::fabs(t) / std::sqrt(2.0));
          ll = b - crit * se;
          ul = b + crit * se;
        } else {
          t = rowNum(*c.row, "t");
          if (std::isnan(t)) t = rowNum(*c.row, "z");
          p = rowNum(*c.row, "p");
          ll = rowNum(*c.row, "ci_lo");
          ul = rowNum(*c.row, "ci_hi");
          if (eform) {
            ll = std::log(ll);
            ul = std::log(ul);
          }
          // The block must agree with the declared level and reference; a block
          // that does not is not Stata's table (header), so nothing is posted.
          if (!std::isfinite(t) || !std::isfinite(p) || !std::isfinite(ll) || !std::isfinite(ul))
            return false;
          // A half-width within ~1000 ulps of b cannot be checked: ul - ll is
          // quantized by b's spacing (BUG-1056: b 2, se 1.6e-16 gives a measured
          // half of 2.7 against crit 2.0106). The p check below still pins the
          // reference distribution.
          const double ulpB = std::nextafter(std::fabs(b), HUGE_VAL) - std::fabs(b);
          const double half = (ul - ll) / (2.0 * se);
          if (crit * se > 1e3 * ulpB && std::fabs(half - crit) > 1e-6 * crit) return false;
          const double pRef = zRef ? std::erfc(std::fabs(t) / std::sqrt(2.0))
                                   : cecon::t_twosided_pvalue(t, dfR);
          if (!(std::fabs(p - pRef) <= 1e-8 + 1e-6 * pRef)) return false;
        }
      } else if (!std::isnan(se) && se != 0) {
        return false;  // a negative/invalid se: not a table this code reproduces
      } else if (se == 0 && c.row->contains("t") && c.row->at("t").is_null() &&
                 c.row->contains("p") && c.row->at("p").is_null()) {
        // The engine's exact `regress` fit (V = 0, t and p posted missing):
        // Stata's r(table) prints the se as `.` too (StataBE `regress yx x` on
        // an exact line: se, t, pvalue, ll, ul all missing), while _se[] is 0.
        se = rtNan();
      }
    }
    if (eform) {
      // exp() back to the reported metric; the base column's 0 becomes 1.
      const double eb = std::exp(b);
      b = eb;
      if (!std::isnan(se)) se = eb * se;
      if (!std::isnan(ll)) ll = std::exp(ll);
      if (!std::isnan(ul)) ul = std::exp(ul);
    }
    const double col[9] = {b, se, t, p, ll, ul, zRef ? rtNan() : dfR, crit, eform ? 1.0 : 0.0};
    for (int r = 0; r < 9; ++r) m.data[static_cast<size_t>(r)].push_back(col[r]);
  }
  out = std::move(m);
  return true;
}

bool postRTableFromBlock(SessionState& session, const ojson& block, double level) {
  eval::EvalMatrix tbl;
  if (!buildRTableFromBlock(session, block, level, tbl)) return false;
  setRTable(session, std::move(tbl));
  // r(level) beside r(table) (w2o_rnames.log:400 N24 `regress` r(level) 95,
  // :410 N25 level(90) -> 90).
  session.eval.rValues.set("level", level);
  return true;
}

bool replaceStaleRClass(eval::EvalSession& ev, const eval::RStoreMark& mark) {
  size_t leftOver = 0;
  bool postedMatrix = false;
  for (const auto& kv : ev.rMatrices) {
    if (kv.second.gen <= mark.seq) ++leftOver;
    else postedMatrix = true;
  }
  const bool wroteR = ev.rValues.generation() > mark.seq || ev.rStrings.generation() > mark.seq ||
                      postedMatrix || leftOver < mark.rMatrixCount;
  if (!wroteR) return false;
  ev.rValues.eraseWrittenAtOrBefore(mark.seq);
  ev.rStrings.eraseWrittenAtOrBefore(mark.seq);
  for (auto it = ev.rMatrices.begin(); it != ev.rMatrices.end();) {
    if (it->second.gen <= mark.seq) it = ev.rMatrices.erase(it);
    else ++it;
  }
  if (!ownsRTable(ev)) {
    ev.rTableGen = 0;
    ev.rTableEid = 0;
    ev.rTableModel.clear();
  }
  if (!ev.rTableRefusal.empty() && ev.rTableRefusalGen <= mark.seq) forgetRTableRefusal(ev);
  return true;
}

namespace {

// What a command did to r(), for the identity of the live r-class command
// (EvalSession::rClassCmd; RULING.md C.3): nothing, wrote r() (replaced it),
// a fit (r() is the fit's: {table, level}), or a write whose Stata set this
// build does not know (margins, a resampling prefix).
enum class RAfter { kNone, kWrote, kFit, kUnknown };

// e(cmdline) of a fit whose handler posted none: every estimator posts the ado's
// `"<cmd> `0'"' -- the command word as Stata names it, then the rest as typed
// (StataBE 19.5, 26 estimators in scratch-laneA/lead/cmdline/cl.do: `reg income
// beertax` posts "regress income beertax", `xtlogit hi beertax, fe` "xtlogit hi
// beertax, fe"). A handler's own posting (regress's ordered e() lists, leebounds,
// bootstrap) is kept.
void postDefaultCmdline(SessionState& session) {
  if (session.eval.eStrings.find("cmdline") != nullptr) return;
  // the leaf's own line after macro expansion (empty: none recorded, post none)
  const std::string& rawCmd = session.leafCmdExpanded;
  const std::string word = commandWord(rawCmd);
  if (word.empty()) return;
  size_t k = rawCmd.find_first_not_of(" \t");
  k = rawCmd.find_first_of(" \t,", k);
  std::string rest;
  if (k != std::string::npos) {
    k = rawCmd.find_first_not_of(" \t", k);
    if (k != std::string::npos) rest = rawCmd.substr(k);  // trailing blanks stay (BUG-1048)
  }
  const std::string canon = registry::resolveCanonical(word);
  session.eval.setEString("cmdline", rest.empty() ? canon : canon + " " + rest);
  session.noteEOnFit("cmdline");  // the next install drops it
}

// e(title), e(predict), e(estat_cmd) and e(marginsok) of a fit whose handler posted
// none. Each ado posts the same words for every variant PIET fits (StataBE 19.5,
// BUG-1492/BUG-1493, scratch-laneA2/agents/epost/s1.do: default, robust and cluster VCEs,
// weights, noconstant, or, baseoutcome(), quantile(), ul()/ll(), families and links); ""
// = Stata posts none (xtreg and qreg have no e(title), areg no e(estat_cmd)). Keyed by
// e(cmd) and, where one word names several of Stata's estimators, by the fit: xtreg's fe
// (the engine's feols) and re ("xtreg_re"), clogit and xtpoisson only as the fe fits
// (`xtlogit, fe` posts e(cmd) clogit), ivregress by method, ivreghdfe as 2SLS. A prefixed
// fit (e(prefix) svy) posts its own and is left alone.
void postDefaultEMacros(SessionState& session) {
  const auto& lmo = session.lastModel();
  if (!lmo.has_value() || session.ePostedLive || !session.eval.eStrings.get("prefix").empty())
    return;
  const std::string cmd = session.eval.eStrings.get("cmd");
  const std::string& engine = lmo->cobraCmd;
  const std::string est = lmo->result.has_value() && lmo->result->is_object()
                              ? lmo->result->value("estimator", std::string())
                              : std::string();
  struct Macros {
    const char *title, *predict, *estatCmd, *marginsok;
  };
  std::optional<Macros> m;
  if (cmd == "regress") {
    m = Macros{"Linear regression", "regres_p", "regress_estat", "XB default"};
  } else if (cmd == "areg") {
    m = Macros{"Linear regression, absorbing indicators", "areg_p", "", ""};
  } else if (cmd == "reghdfe") {
    m = Macros{"HDFE Linear regression", "reghdfe_p", "reghdfe_estat", ""};
  } else if (cmd == "logit") {
    m = Macros{"Logistic regression", "logit_p", "logit_estat", "default Pr"};
  } else if (cmd == "logistic") {
    m = Macros{"Logistic regression", "logistic_p", "logit_estat", "default Pr"};
  } else if (cmd == "probit") {
    m = Macros{"Probit regression", "probit_p", "probit_estat", "default Pr"};
  } else if (cmd == "clogit" && est == "xtlogit_fe") {
    m = Macros{"Conditional fixed-effects logistic regression", "xtlogit_fe_p", "", "PU0 XB"};
  } else if (cmd == "xtpoisson" && est == "xtpoisson_fe") {
    m = Macros{"Conditional fixed-effects Poisson regression", "xtpoisson_fe_p", "", ""};
  } else if (cmd == "xtreg" && engine == "feols") {
    m = Macros{"", "xtrefe_p", "xtreg_estat", ""};
  } else if (cmd == "xtreg" && est == "xtreg_re") {
    m = Macros{"", "xtrere_p", "xtreg_estat", ""};
  } else if (cmd == "poisson") {
    m = Macros{"Poisson regression", "poisso_p", "poisson_estat", ""};
  } else if (cmd == "ppmlhdfe") {
    m = Macros{"HDFE PPML regression", "ppmlhdfe_p", "reghdfe5_estat", "default"};
  } else if (cmd == "ivregress") {
    const char* title = nullptr;
    if (engine == "liml")
      title = "Instrumental-variables LIML regression";
    else if (est == "ivgmm" || est == "ivgmm_multi")
      title = "Instrumental-variables GMM regression";
    else if (est == "iv2sls" || est == "iv2sls_multi")
      title = "Instrumental-variables 2SLS regression";
    if (title != nullptr) m = Macros{title, "ivregress_p", "ivregress_estat", "XB default"};
  } else if (cmd == "ivreghdfe" && engine == "iv" && (est == "iv2sls" || est == "iv2sls_multi")) {
    m = Macros{"IV (2SLS) estimation", "reghdfe_p", "", "XB default"};
  } else if (cmd == "tobit") {
    m = Macros{"Tobit regression", "tobit_p", "",
               "default XB Pr(passthru) E(passthru) YStar(passthru)"};
  } else if (cmd == "nbreg") {
    m = Macros{"Negative binomial regression", "nbreg_p", "", ""};
  } else if (cmd == "qreg") {
    m = Macros{"", "qreg_p", "", ""};
  } else if (cmd == "glm") {
    m = Macros{"Generalized linear models", "glim_p", "", "default"};
  } else if (cmd == "ologit") {
    m = Macros{"Ordered logistic regression", "ologit_p", "ologit_estat", ""};
  } else if (cmd == "mlogit") {
    m = Macros{"Multinomial logistic regression", "mlogit_p", "", ""};
  }
  if (!m.has_value()) return;
  const auto fill = [&session](const char* name, const char* v) {
    if (*v == '\0' || session.eval.eStrings.find(name) != nullptr) return;
    session.eval.setEString(name, v);
    session.noteEOnFit(name);  // the next install drops it
  };
  fill("title", m->title);
  fill("predict", m->predict);
  fill("estat_cmd", m->estatCmd);
  fill("marginsok", m->marginsok);
}

// The leaf half of rTableAfterCommand: an estimation replaces r() and posts its
// table, a replay re-posts, a restore of another model retires, margins replaces
// r() and refuses its table.
RAfter leafPostsOrRetires(SessionState& session, const ojson& result, const std::string& rawCmd,
                          bool eChanged, std::uintptr_t eIdNow) {
  // tabstat.ado is an r-class program: however it ends, r() is what it
  // returned, and a refused tabstat returned nothing (BUG-1151, StataBE 19.5:
  // after `capture tabstat x, s(nosuch) save`, `capture tabstat x [pw=w], save`
  // or `capture tabstat x in 10/12, save`, `matrix list r(StatTotal)` is r(111),
  // and an earlier summarize's r(N) reads `.` after any failed tabstat). Judged
  // here, after the line, so the refusals raised before the tabstat arm runs
  // (the tokenizer's weight and `in` checks, a weight variable that does not
  // exist) empty r() too, and an `if` read the caller's r() while it ran.
  if (commandWord(rawCmd) == "tabstat" && result.is_array()) {
    for (const auto& b : result) {
      if (!b.is_object() || b.value("type", std::string()) != "error") continue;
      eval::replaceRClass(session.eval);
      eval::declareRPosted(session.eval, "tabstat", {});
      return RAfter::kWrote;
    }
  }
  // FU-276: a margins form runMargins served posted its own r() (r(b), r(V),
  // r(table), r(N), ...) and declared Stata's set for the invocation
  // (declareRPosted): r() is margins', `post` included -- live StataBE 19.5
  // (2026-10-13) still lists r(b)/r(V)/r(table)/r(N) after `margins t, post` --
  // so neither the fit branch below nor the refusal branch may replace it.
  if (commandWord(rawCmd) == "margins" && session.eval.rDeclaredNames.has_value() &&
      session.eval.rDeclaredCmd == "margins")
    return RAfter::kWrote;
  // csdid-estat-estore-post: `estat simple|event` after csdid returns its own
  // r(b)/r(V)/r(table)/r(agg) (csdid_estat.ado, rclass) and, under `post`, installs
  // the posting as a new e(); its r() is the estat's, never a fit's table
  // (w2ag log:1564 ag_simple_nopost_r, :1777 ag_evwin_rtable).
  if (commandWord(rawCmd) == "estat" && session.eval.rDeclaredNames.has_value() &&
      session.eval.rDeclaredCmd == "estat")
    return RAfter::kWrote;
  // nlcom posts its own r(table) (dispatcher's nlcom arm), `post` or not.
  const ojson* est = nullptr;
  if (result.is_array()) {
    for (const auto& b : result) {
      if (!b.is_object() || b.value("type", std::string()) != "estimation") continue;
      if (b.value("estimator", std::string()) == "nlcom") return RAfter::kNone;
      est = &b;
    }
  }
  // A new estimation REPLACES r() (w2n_followups.log:968, E07: r(N)/r(mean) of an
  // earlier summarize read `.` after `regress`) and posts its own table.
  if (eChanged && est != nullptr) {
    eval::replaceRClass(session.eval);
    if (session.lastEstBlock.has_value() &&
        !postRTableFromBlock(session, *est, declaredLevel(rawCmd, session)))
      refuseRTable(session, estimationRefusal(session));
    annotateEbStripe(session);
    postDefaultCmdline(session);
    postDefaultEMacros(session);
    return RAfter::kFit;
  }
  // e() replaced by a copy of the same model (`estadd`) keeps the table it has; any
  // other replacement with no estimation output (`estimates restore` of another
  // model) leaves no table behind.
  if (eChanged) {
    if (!rebindIfSameModel(session, eIdNow)) retireRTable(session);
    return RAfter::kNone;
  }
  // A replay (`regress` with no arguments) returns the live block unchanged: it
  // re-displays the fit, and r() is the fit's again.
  if (est != nullptr && session.lastEstBlock.has_value() && *est == *session.lastEstBlock) {
    eval::replaceRClass(session.eval);
    if (!postRTableFromBlock(session, *est, declaredLevel(rawCmd, session)))
      refuseRTable(session, estimationRefusal(session));
    return RAfter::kFit;
  }
  // margins: Stata's r() is margins' own (margins's help). Every refused
  // margins (and runGlmMargins, which declares nothing) replaces r()
  // (replaceRClass also retires the table) and every later read of it refuses
  // (RULING.md D.2.3): when PIET refuses a margins that Stata runs, Stata's r() is
  // still margins'. Unconditional for those -- the served forms returned above.
  if (commandWord(rawCmd) == "margins") {
    eval::replaceRClass(session.eval);
    refuseRTable(session, "r(table) after margins is margins' own table, which this version "
                          "of PIET does not build.");
    return RAfter::kUnknown;
  }
  return RAfter::kNone;
}

RAfter afterCommand(SessionState& session, const ojson& result, const std::string& rawCmd,
                    std::uintptr_t eIdAtEntry, bool leaf, const eval::RStoreMark& markAtEntry) {
  const std::uintptr_t eIdNow = liveEstimationIdentity(session);
  const bool eChanged = eIdNow != eIdAtEntry;
  // The replicating prefixes (dispatcher.cpp's reResamplePrefix) run their inner
  // command once per replicate through dispatch(), so the LAST replicate's leaf
  // posted a table. Stata's table there is the resampling table, which PIET does
  // not build: none is served.
  {
    const std::string word = commandWord(rawCmd);
    // bootstrap posts its own normal-based table for the e() it installs
    // (runBootstrap); that one stays, and the container path below keeps it.
    // Only bootstrap's own posting qualifies: on a failure path the live e()
    // and table are the full-sample fit's or a replicate's.
    const bool bootstrapOwn = word == "bootstrap" && ownsRTable(session.eval) &&
                              session.eval.rTableEid == eIdNow && eIdNow != 0 &&
                              session.lastModel().has_value() &&
                              session.lastModel()->cobraCmd == "bootstrap";
    if (!bootstrapOwn &&
        (word == "bootstrap" || word == "jackknife" || word == "permute" || word == "rolling")) {
      retireRTable(session);
      refuseRTable(session, "r(table) after " + word +
                                " is the resampling table, which this version of PIET does not "
                                "build.");
      return RAfter::kUnknown;
    }
  }
  // Whatever this command wrote into r() replaced all of it. A leaf is judged
  // against r() at its entry; a container against r() after its last inner
  // command, so only its OWN writes count (an rclass program's `return` set is
  // committed to r() when the program ends, after every inner leaf).
  //
  // The one measured exception: `timer list` ADDS r(tN)/r(ntN) to r() and keeps
  // every other r() result (StataNow BE 19.5 probe of BUG-151: `count`'s r(N) 74
  // was still present after `timer list 1`; selftest "C4/BS-124 BUG-151"). It is
  // keyed on the command word because the timer handler maintains its own keys.
  bool wrote = false;
  if (!(leaf && commandWord(rawCmd) == "timer"))
    wrote = replaceStaleRClass(session.eval, leaf ? markAtEntry : session.eval.rMarkAfterLastCommand);
  if (!leaf) {
    // A container that replaced e(): keep the table only if a leaf inside posted it
    // for the e() that is live now.
    if (eChanged && !rebindIfSameModel(session, eIdNow) && !refusedFor(session, eIdNow))
      retireRTable(session);
    return wrote ? RAfter::kWrote : RAfter::kNone;
  }
  const RAfter leafWhat = leafPostsOrRetires(session, result, rawCmd, eChanged, eIdNow);
  if (leafWhat != RAfter::kNone) return leafWhat;
  return wrote ? RAfter::kWrote : RAfter::kNone;
}

// Record the identity of the r-class command whose r() is now live (RULING.md
// C.3). A command that did not replace r() records nothing: the previous
// identity stays, exactly as r() does. "present" = what r() holds now.
void setRClassIdentity(eval::EvalSession& ev, std::string cmd,
                       const std::vector<std::string>& stataNames) {
  ev.rClassCmd = std::move(cmd);
  ev.rClassSetUnknown = false;
  ev.rUnposted.clear();
  for (const auto& n : stataNames) {
    const bool present = ev.rValues.find(n) != nullptr ||
                         (!eval::isReservedStoreKey(n) && ev.rStrings.find(n) != nullptr) ||
                         ev.findClassMatrix('r', n) != nullptr;
    if (!present) ev.rUnposted.push_back(n);
  }
}

void recordRClassIdentity(eval::EvalSession& ev, RAfter what, bool leaf, const std::string& rawCmd) {
  switch (what) {
    case RAfter::kNone:
      break;
    case RAfter::kFit:
      // r(level) after a fit is UNPINNED (RULING.md D.2.4, FU-o): kept in the set,
      // so it refuses until a capture settles it.
      setRClassIdentity(ev, commandWord(rawCmd), {"table", "level"});
      break;
    case RAfter::kUnknown:
      ev.rClassCmd = commandWord(rawCmd);
      ev.rUnposted.clear();
      ev.rClassSetUnknown = true;
      break;
    case RAfter::kWrote:
      if (ev.rDeclaredNames.has_value()) {
        setRClassIdentity(ev, ev.rDeclaredCmd.empty() ? commandWord(rawCmd) : ev.rDeclaredCmd,
                          *ev.rDeclaredNames);
      } else if (!leaf) {
        // A container's own write after its last inner command: an rclass
        // program's return set IS r() (class X).
        ev.rClassCmd = commandWord(rawCmd);
        ev.rUnposted.clear();
        ev.rClassSetUnknown = false;
      } else {
        ev.rClassCmd = commandWord(rawCmd);
        ev.rUnposted.clear();
        ev.rClassSetUnknown = true;
      }
      break;
  }
  ev.rDeclaredNames.reset();
  ev.rDeclaredCmd.clear();
}

}  // namespace

void rTableAfterCommand(SessionState& session, const ojson& result, const std::string& rawCmd,
                        std::uintptr_t eIdAtEntry, bool leaf, const eval::RStoreMark& markAtEntry) {
  const RAfter what = afterCommand(session, result, rawCmd, eIdAtEntry, leaf, markAtEntry);
  recordRClassIdentity(session.eval, what, leaf, rawCmd);
  session.eval.rMarkAfterLastCommand = session.eval.rMark();
}

}  // namespace piet::handlers
