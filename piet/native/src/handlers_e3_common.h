// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// handlers_e3_common.h -- the small toolkit the E3 handler TUs share
// (handlers_e3_{swindex,qreg2,lasso,suest,ritest,acreg}.cpp): Stata's `syntax`
// option-abbreviation rule, rc-carrying error blocks, nested dispatch with error
// propagation, and the one f64 column writer (`cobra setcol --new` +
// session.load, the established write path; handlers_aer_estim.cpp
// estimWriteColumn is the same shape). Header-only so no build file changes.
#pragma once

// FIRST: the PIET copy of nlohmann (v3.12, piet/native/vendor). "nlohmann/json.hpp"
// alone resolves to the repo-root vendor copy (v3.11) through -I vendor, and a TU
// that sees that one first declares every ojson signature in another ABI
// namespace (an undefined esampleInstallOnHead at link time).
#include "vendor/nlohmann/json.hpp"

#include <cmath>
#include <fstream>
#include <optional>
#include <string>
#include <vector>

#include <sstream>

#include "core/distributions.h"
#include "estimation_m4_internal.h"  // esampleInstallOnHead
#include "io/win_compat.h"
#include "piet/cobra_runner.h"
#include "piet/datastore.h"
#include "piet/loops.h"
#include "piet/dispatcher.h"
#include "piet/eval/errors.h"
#include "piet/scratch_seq.h"
#include "piet/session_state.h"
#include "vendor/nlohmann/json.hpp"

namespace piet::handlers::e3 {

using ojson = nlohmann::ordered_json;

// An error block carrying Stata's own rc (coarseRc reads the `r(NNN)` reason).
inline ojson rcError(const std::string& message, int rc,
                     const std::string& code = "syntax_error") {
  return ojson(eval::errorBlock(code, message, std::nullopt, std::nullopt, std::nullopt,
                                "r(" + std::to_string(rc) + ")"));
}

inline ojson displayText(const std::string& text) {
  ojson b;
  b["type"] = "display";
  b["text"] = text;
  return b;
}

// One option of a `syntax` line: its full lower-case name, the length of its
// capitalised minimal abbreviation, and whether it takes an argument.
struct OptSpec {
  std::string full;
  size_t minLen;
  bool takesArg;
};

// Stata's abbreviation rule: a typed key names an option when it is a prefix of
// the full name at least as long as the minimal abbreviation. -1 when it names
// none (Stata: "option KEY not allowed", r(198)).
inline int matchOpt(const std::vector<OptSpec>& specs, const std::string& keyIn) {
  std::string key;
  for (char c : keyIn) key += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  for (size_t i = 0; i < specs.size(); ++i) {
    const OptSpec& s = specs[i];
    if (key.size() >= s.minLen && key.size() <= s.full.size() &&
        s.full.compare(0, key.size(), key) == 0)
      return static_cast<int>(i);
  }
  return -1;
}

// The first error block of a dispatch() reply, if any.
inline std::optional<ojson> firstError(const ojson& blocks) {
  if (blocks.is_object()) {
    if (blocks.value("type", std::string()) == "error") return blocks;
    return std::nullopt;
  }
  if (!blocks.is_array()) return std::nullopt;
  for (const auto& b : blocks)
    if (b.is_object() && b.value("type", std::string()) == "error") return b;
  return std::nullopt;
}

// dispatch() a helper line; an error block comes back in *err.
inline bool runLine(const std::string& line, SessionState& session, ojson* err) {
  ojson out = piet::dispatch(line, session);
  if (auto e = firstError(out)) {
    if (err != nullptr) *err = *e;
    return false;
  }
  return true;
}

// Write one f64 column (NaN = missing) into the session frame as a NEW variable.
inline bool writeColumn(SessionState& session, const std::string& name,
                        const std::vector<double>& vals, ojson* err) {
  const std::string hbin = mintScratchPath(session.workdir(), "e3col");
  forgetScratchMint(hbin);
  const std::string csv = hbin.substr(0, hbin.size() - 5) + ".csv";
  registerScratchMint(csv);
  {
    std::ofstream f(cecon::to_native_path(csv));
    if (!f) {
      if (err != nullptr) *err = rcError("could not write the values payload", 603, "data_io");
      return false;
    }
    f.setf(std::ios::fmtflags(0), std::ios::floatfield);
    f.precision(17);
    for (double d : vals) {
      if (!std::isnan(d)) f << d;
      f << "\n";
    }
  }
  const std::string out = mintScratchPath(session.workdir(), "e3col");
  const CobraRunResult r = runCobra({"setcol", "--data", *session.currentHbin(), "--var", name,
                                     "--values", csv, "--dtype", "f64", "--new", "--out", out});
  cecon::native_unlink(csv);
  if (!r.ok || !cecon::native_exists(out)) {
    if (err != nullptr) *err = rcError("could not create " + name, 603, "data_io");
    return false;
  }
  session.load(out);
  return true;
}

// A varlist the way `syntax` reads it: wildcards and ranges expand
// (loops::expandVarlist, the one expander), and a name that is not a variable is
// Stata's r(111) "variable X not found" (the expander's own error is rc 1).
inline bool expandVars(const std::string& spec, SessionState& session,
                       std::vector<std::string>* out, ojson* err) {
  std::istringstream is(spec);
  Datastore* ds = session.datastore();
  for (std::string t; is >> t;) {
    if (t.find_first_of("*?-~") != std::string::npos) continue;
    if (ds == nullptr || !ds->has(t)) {
      *err = rcError("variable " + t + " not found", 111);
      return false;
    }
  }
  loops::VarlistExpandResult ex = loops::expandVarlist(spec, session);
  if (ex.isError) {
    const std::string msg = ex.error.value("message", std::string());
    *err = msg.find("not found") != std::string::npos ? rcError(msg, 111) : ex.error;
    return false;
  }
  *out = ex.tokens;
  return true;
}

// Mata's invsym() on a symmetric matrix: the sweep in pivot order; a pivot that
// is not positive relative to its original diagonal drops its row and column
// (set to 0) -- the generalised inverse mf_invsym documents.
inline std::vector<std::vector<double>> invsym(std::vector<std::vector<double>> A) {
  const size_t k = A.size();
  std::vector<double> d0(k);
  for (size_t i = 0; i < k; ++i) d0[i] = std::fabs(A[i][i]);
  std::vector<bool> dropped(k, false);
  for (size_t p = 0; p < k; ++p) {
    const double D = A[p][p];
    if (!(D > 0) || !(D > 1e-13 * d0[p])) {
      dropped[p] = true;
      for (size_t j = 0; j < k; ++j) A[p][j] = A[j][p] = 0;
      continue;
    }
    for (size_t j = 0; j < k; ++j) A[p][j] /= D;
    for (size_t i = 0; i < k; ++i) {
      if (i == p) continue;
      const double B = A[i][p];
      for (size_t j = 0; j < k; ++j) A[i][j] -= B * A[p][j];
      A[i][p] = -B / D;
    }
    A[p][p] = 1.0 / D;
  }
  for (size_t p = 0; p < k; ++p)
    if (dropped[p])
      for (size_t j = 0; j < k; ++j) A[p][j] = A[j][p] = 0;
  return A;
}

// The coefficient table of a posted b/V the way `ereturn display` prints it: z
// statistics without a residual dof (dfr NaN), t(dfr) with one; se missing when
// V_ii is missing or negative.
inline ojson coeftableFromPosting(const std::vector<std::string>& names,
                                  const std::vector<double>& b,
                                  const std::vector<std::vector<double>>& V, double dfr) {
  const bool tRef = std::isfinite(dfr) && dfr > 0;
  const double crit = tRef ? cecon::t_critical(0.05, dfr) : cecon::z_critical(0.05);
  ojson rows = ojson::array();
  for (size_t j = 0; j < names.size(); ++j) {
    ojson r;
    r["name"] = names[j];
    const double bj = b[j];
    const double v = V[j][j];
    const double se = (std::isfinite(v) && v >= 0) ? std::sqrt(v) : std::nan("");
    r["b"] = std::isfinite(bj) ? ojson(bj) : ojson(nullptr);
    if (std::isfinite(se) && se > 0 && std::isfinite(bj)) {
      const double t = bj / se;
      r["se"] = se;
      r["t"] = t;
      r["p"] = tRef ? cecon::t_twosided_pvalue(t, dfr) : std::erfc(std::fabs(t) / std::sqrt(2.0));
      r["ci_lo"] = bj - crit * se;
      r["ci_hi"] = bj + crit * se;
    } else {
      r["se"] = std::isfinite(se) ? ojson(se) : ojson(nullptr);
      r["t"] = nullptr;
      r["p"] = nullptr;
      r["ci_lo"] = nullptr;
      r["ci_hi"] = nullptr;
    }
    rows.push_back(r);
  }
  return rows;
}

// e(b) (1 x k, row y1) and e(V) (k x k) into the live e() stores.
inline void postBV(SessionState& session, const std::vector<std::string>& names,
                   const std::vector<double>& b, const std::vector<std::vector<double>>& V) {
  auto setE = [&session](const std::string& nm, eval::EvalMatrix m) {
    for (auto& kv : session.eval.eMatrices) {
      if (kv.first == nm) {
        kv.second = std::move(m);
        return;
      }
    }
    session.eval.eMatrices.emplace_back(nm, std::move(m));
  };
  eval::EvalMatrix mb;
  mb.data.push_back(b);
  mb.rownames = {"y1"};
  mb.colnames = names;
  eval::EvalMatrix mv;
  mv.data = V;
  mv.rownames = names;
  mv.colnames = names;
  setE("b", std::move(mb));
  setE("V", std::move(mv));
}

// An e() scalar written on top of an installed fit: recorded so the next model
// install drops it (SessionState::noteEOnFit).
inline void setEScalarOnFit(SessionState& session, const std::string& name, double v) {
  session.eval.setEScalar(name, v);
  session.noteEOnFit(name);
}
inline void setEStringOnFit(SessionState& session, const std::string& name, const std::string& v) {
  session.eval.setEString(name, v);
  session.noteEOnFit(name);
}

// e(sample) for a fit computed in PIET: `mask` (one cell per row of the working
// frame) becomes the hidden e(sample) column through the fit handlers' one install
// (handlers.cpp esampleInstallOnHead), lm.dataPath set to the head first. False
// leaves e(sample) the named r(198) (kUnavailable). Call before setLastModel.
inline bool installEsampleMask(SessionState& session, LastModel& lm,
                               const std::vector<char>& mask) {
  if (!session.currentHbin().has_value()) return false;
  const std::string hb = mintScratchPath(session.workdir(), "esample");
  forgetScratchMint(hb);
  const std::string path = hb.substr(0, hb.size() - 5) + ".txt";
  registerScratchMint(path);
  long long ones = 0;
  {
    std::ofstream o(cecon::to_native_path(path), std::ios::binary | std::ios::trunc);
    if (!o) return false;
    for (char m : mask) {
      o << (m ? "1\n" : "0\n");
      ones += m ? 1 : 0;
    }
  }
  ojson raw;
  raw["esample_n"] = ones;
  raw["n_rows"] = ones;
  lm.dataPath = *session.currentHbin();
  return esampleInstallOnHead(session, path, raw, lm);
}

// Stata's float storage of a double (an untyped `generate` under `set type
// float`): round to nearest float; missing stays missing.
inline double toFloat(double v) {
  if (std::isnan(v)) return v;
  return static_cast<double>(static_cast<float>(v));
}

}  // namespace piet::handlers::e3
