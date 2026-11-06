// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// handlers_e3_suest.cpp -- `suest`: one coefficient vector and one sandwich
// covariance over several stored regress fits, per [R] suest, Methods and formulas.
//
// A stored regress has no ancillary parameter, so each fit contributes its own
// e(b) columns (equation `mean`) plus ln(sigma^2) (equation `lnvar`). The bread is
// block diagonal and comes from the stored fit; the meat is the outer product of
// the stacked scores, evaluated from the data in memory, summed per observation or
// per cluster. Scores are not centred, and the only finite-sample factor is
// N/(N-1) or G/(G-1). Fits, weights and terms this build does not cover are named
// refusals, never an approximate answer.
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <memory>
#include <numeric>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "handlers_e3_common.h"
#include "piet/datastore.h"
#include "piet/eval/errors.h"
#include "piet/eval/macros.h"
#include "piet/handlers.h"
#include "piet/session_state.h"
#include "piet/strutil.h"
#include "vendor/nlohmann/json.hpp"

namespace piet {
namespace handlers {

namespace {

using ojson = nlohmann::ordered_json;
using StoredModel = SessionState::StoredModel;

ojson wrapBlock(ojson block) {
  ojson blocks = ojson::array();
  blocks.push_back(std::move(block));
  return blocks;
}

// "Valid Stata, not built here" in the shape every suest scope refusal shares.
ojson outOfScope(const std::string& sentence) {
  return e3::rcError(sentence, 198, "unsupported_option");
}

std::vector<std::string> blankSplit(const std::string& text) {
  std::vector<std::string> words;
  std::istringstream in(text);
  for (std::string w; in >> w;) words.push_back(std::move(w));
  return words;
}

bool abbreviates(const std::string& typed, const std::string& full, size_t shortest) {
  return typed.size() >= shortest && typed.size() <= full.size() &&
         full.compare(0, typed.size(), typed) == 0;
}

std::optional<double> wholeNumber(const std::string& text) {
  const std::string t = strutil::trim(text);
  if (t.empty()) return std::nullopt;
  char* stop = nullptr;
  const double v = std::strtod(t.c_str(), &stop);
  if (stop == t.c_str() || *stop != '\0') return std::nullopt;
  return v;
}

// ---- the option line ----------------------------------------------------------

struct Request {
  std::string clusterVar;
  bool mlVariance = false;
};

// Reads vce(...) into `word` ("robust"/"cluster", empty when vce() is empty) and
// `var`. Returns the syntax error, if any.
std::optional<ojson> readVceText(const std::string& raw, std::string& word, std::string& var) {
  const size_t comma = raw.find(',');
  const std::vector<std::string> head = blankSplit(raw.substr(0, comma));
  const std::vector<std::string> tail =
      comma == std::string::npos ? std::vector<std::string>{} : blankSplit(raw.substr(comma + 1));
  if (head.empty()) {
    if (tail.empty()) return std::nullopt;
    return e3::rcError("option vce() incorrectly specified", 198);
  }
  if (abbreviates(head[0], "robust", 1)) {
    if (head.size() > 1) return e3::rcError("option vce() incorrectly specified", 198);
    word = "robust";
  } else if (abbreviates(head[0], "cluster", 2)) {
    if (head.size() != 2)
      return e3::rcError("option vce(cluster clustvar) incorrectly specified", 198);
    word = "cluster";
    var = head[1];
  } else {
    return e3::rcError("vcetype " + head[0] + " not allowed", 198);
  }
  if (!tail.empty()) return e3::rcError("option " + tail[0] + " not allowed", 198);
  return std::nullopt;
}

std::optional<ojson> readOptionLine(const eval::ParsedCommand& parsed, Request& req) {
  enum Slot { kVce, kCluster, kRobust, kRegressMl, kLevel, kDir, kEform, kSvy, kMinus };
  static const std::vector<e3::OptSpec> kLine = {
      {"vce", 3, true},    {"cluster", 2, true}, {"robust", 1, false},
      {"regressml", 9, false}, {"level", 1, true},   {"dir", 1, false},
      {"eform", 2, true},  {"svy", 3, false},    {"minus", 5, true},
  };
  std::string vceWord, vceVar, clusterOpt;
  std::string notBuilt;
  for (const auto& [typed, value] : parsed.opts) {
    const bool hasParens = !value.isBool;
    const int slot = e3::matchOpt(kLine, typed);
    if (slot < 0)
      return e3::rcError("option " + typed + (hasParens ? "()" : "") + " not allowed", 198);
    const e3::OptSpec& spec = kLine[static_cast<size_t>(slot)];
    if (spec.takesArg && !hasParens)
      return e3::rcError("option " + spec.full + "() required", 198);
    if (!spec.takesArg && hasParens)
      return e3::rcError("option " + typed + "() not allowed", 198);
    switch (slot) {
      case kVce:
        if (auto bad = readVceText(value.strValue, vceWord, vceVar)) return bad;
        break;
      case kCluster:
        clusterOpt = strutil::trim(value.strValue);
        break;
      case kRegressMl:
        req.mlVariance = true;
        break;
      case kLevel: {
        const auto lv = wholeNumber(value.strValue);
        if (!lv || !(*lv >= 10.0 && *lv <= 99.99))
          return e3::rcError("level() must be between 10 and 99.99 inclusive", 198);
        break;
      }
      case kSvy:
        if (notBuilt.empty()) notBuilt = "svy";
        break;
      case kMinus:
        if (notBuilt.empty()) notBuilt = "minus()";
        break;
      default:  // robust, dir, eform: accepted, no effect on the posting
        break;
    }
  }
  if (!clusterOpt.empty() && vceWord == "robust")
    return e3::rcError("only one of options cluster() or vce(robust) is allowed", 198);
  if (!clusterOpt.empty() && vceWord == "cluster" && clusterOpt != vceVar)
    return e3::rcError("options cluster(clustvar) and vce(cluster clustvar) are in conflict", 100);
  req.clusterVar = !clusterOpt.empty() ? clusterOpt : vceVar;
  if (!notBuilt.empty())
    return outOfScope("suest, " + notBuilt + ": not implemented in this version of PIET; nothing was estimated");
  return std::nullopt;
}

// ---- one stored fit ------------------------------------------------------------

// How a column of the stored e(b) reads the data.
struct DesignTerm {
  enum class Reads { kNothing, kConstant, kValue, kLevelOf } reads = Reads::kNothing;
  const double* cells = nullptr;
  double level = 0.0;
  double coef = 0.0;
};

struct FitEquations {
  std::string name;
  std::vector<std::string> columns;  // the stored e(b) stripe
  std::vector<DesignTerm> terms;     // index-aligned with columns
  std::vector<std::vector<double>> meanBread;
  const double* depvarCells = nullptr;
  const double* sampleCells = nullptr;
  double sigma2 = 0.0;
  double observations = 0.0;
  size_t firstSlot = 0;  // where this fit's mean columns start in the stacked vector

  bool inSample(int64_t row) const {
    const double m = sampleCells[row];
    return !std::isnan(m) && m != 0.0;
  }
  size_t slotCount() const { return columns.size() + 1; }
  size_t lnvarSlot() const { return firstSlot + columns.size(); }
};

// What one coefficient name asks of the data. False when the form is outside the
// supported set: interactions, c./co./bn. forms and time-series operators.
bool readCoefName(const std::string& coefName, std::string& variable, DesignTerm& term) {
  if (coefName == "_cons") {
    term.reads = DesignTerm::Reads::kConstant;
    return true;
  }
  if (coefName.find('#') != std::string::npos) return false;
  const size_t dot = coefName.find('.');
  if (dot == std::string::npos) {
    variable = coefName;
    term.reads = DesignTerm::Reads::kValue;
    return true;
  }
  const std::string marker = coefName.substr(0, dot);
  if (coefName.find('.', dot + 1) != std::string::npos || dot + 1 == coefName.size()) return false;
  variable = coefName.substr(dot + 1);
  size_t digits = 0;
  while (digits < marker.size() && std::isdigit(static_cast<unsigned char>(marker[digits]))) ++digits;
  const std::string suffix = marker.substr(digits);
  if (suffix == "o" || (suffix == "b" && digits > 0)) {
    term.reads = DesignTerm::Reads::kNothing;
    variable.clear();
    return true;
  }
  if (suffix.empty() && digits > 0) {
    term.reads = DesignTerm::Reads::kLevelOf;
    term.level = std::strtod(marker.c_str(), nullptr);
    return true;
  }
  return false;
}

double fitScalar(const eval::EvalSession& fitView, const char* name) {
  const std::string reference = std::string("e(") + name + ")";
  std::string text = strutil::trim(eval::resolveEstScalarRefs(reference, fitView, nullptr));
  if (text.size() >= 2 && text.front() == '(' && text.back() == ')')
    text = strutil::trim(text.substr(1, text.size() - 2));
  return wholeNumber(text).value_or(std::numeric_limits<double>::quiet_NaN());
}

// The fit-level refusals, in the order Stata reports them when several apply.
// `first` is the first model's weighting (null while checking that model).
std::optional<ojson> screenFit(const std::string& name, const StoredModel& stored,
                               const StoredModel* first) {
  const auto word = [&stored](const char* key) { return stored.eStrings.get(key); };
  if (!word("clustvar").empty())
    return e3::rcError(name + " was estimated with cluster(" + word("clustvar") +
                           "). re-estimate without the cluster() option, and specify the "
                           "cluster() option with suest.",
                       322);
  if (word("wtype") == "pweight")
    return e3::rcError("model " + name +
                           " was estimated with pweights, you should re-estimate using iweights",
                       322);
  if (word("cmd") != "regress")
    return outOfScope("suest of a stored `" + word("cmd") +
                      "` fit is not implemented in this version of PIET (only regress is); "
                      "nothing was estimated");
  const std::string vce = word("vce");
  if (!vce.empty() && vce != "ols")
    return e3::rcError(name + " was estimated with a nonstandard vce (" + vce + ")", 322);
  if (first != nullptr) {
    if (word("wtype") != first->eStrings.get("wtype"))
      return e3::rcError("inconsistent weighting types", 322);
    if (word("wexp") != first->eStrings.get("wexp"))
      return e3::rcError("weighting expression differs between models", 322);
  }
  const std::string wtype = word("wtype");
  if (!wtype.empty() && wtype != "aweight")
    return outOfScope("suest of a " + wtype +
                      "ed regress is not implemented in this version of PIET; nothing was estimated");
  return std::nullopt;
}

// Fills `fit` from the stored stripe, e(V) and scalars.
std::optional<ojson> loadFit(const std::string& name, const StoredModel& stored, Datastore& frame,
                             bool mlVariance, FitEquations& fit) {
  const std::string sampleVar = "_est_" + name;
  if (!stored.lastModel.has_value() || !stored.esampleVar || !frame.has(sampleVar))
    return e3::rcError("estimation sample of the model saved under " + name +
                           " could not be restored",
                       198);
  fit.name = name;
  fit.sampleCells = frame.col(sampleVar).f64();

  const auto numericCells = [&frame](const std::string& var) -> const double* {
    if (var.empty() || !frame.has(var) || frame.dtype(var) != "f64") return nullptr;
    return frame.col(var).f64();
  };
  const auto termRefusal = [](const std::string& term) {
    return outOfScope("suest of a regress with the term `" + term +
                      "` is not implemented in this version of PIET; nothing was estimated");
  };

  eval::EvalSession fitView;
  // The estimation block carries the fit's e(b) stripe (base and `o.` columns).
  fitView.lastModelResult = stored.estBlock.has_value() ? stored.estBlock : stored.lastModelResult;
  fitView.eScalars = stored.eScalars;
  fitView.eStrings = stored.eStrings;
  fitView.eMatrices = stored.eMatrices;
  using Reader = eval::EvalSession::EReader;
  auto storedB = fitView.resolveEMatrixFor("b", Reader::kNames);
  auto storedV = fitView.resolveEMatrixFor("V", Reader::kNames);
  if (!storedB.matrix.has_value() || !storedV.matrix.has_value())
    return outOfScope("suest: the stored e(b)/e(V) of " + name +
                      " cannot be read in this version of PIET (" +
                      (!storedB.matrix.has_value() ? storedB.why : storedV.why) + ")");
  const eval::EvalMatrix& bMat = *storedB.matrix;
  const eval::EvalMatrix& vMat = *storedV.matrix;
  const size_t columnCount = bMat.colnames.size();
  if (bMat.data.size() != 1 || bMat.data[0].size() != columnCount ||
      vMat.colnames != bMat.colnames || vMat.data.size() != columnCount)
    return outOfScope("suest: the stored e(b)/e(V) of " + name +
                      " do not line up in this version of PIET");

  fit.columns = bMat.colnames;
  fit.terms.resize(columnCount);
  for (size_t j = 0; j < columnCount; ++j) {
    DesignTerm& term = fit.terms[j];
    std::string variable;
    if (!readCoefName(fit.columns[j], variable, term)) return termRefusal(fit.columns[j]);
    if (term.reads == DesignTerm::Reads::kValue || term.reads == DesignTerm::Reads::kLevelOf) {
      term.cells = numericCells(variable);
      if (term.cells == nullptr) return termRefusal(fit.columns[j]);
    }
    term.coef = term.reads == DesignTerm::Reads::kNothing ? 0.0 : bMat.data[0][j];
  }
  const std::string depvar = stored.eStrings.get("depvar");
  fit.depvarCells = numericCells(depvar);
  if (fit.depvarCells == nullptr) return termRefusal(depvar);

  fit.observations = fitScalar(fitView, "N");
  const double rmse = fitScalar(fitView, "rmse");
  const double conventional = rmse * rmse;
  fit.sigma2 = mlVariance ? fitScalar(fitView, "rss") / fit.observations : conventional;
  if (!(fit.sigma2 > 0.0) || !std::isfinite(fit.sigma2) || !(conventional > 0.0) ||
      !(fit.observations > 0.0))
    return outOfScope("suest: the stored fit " + name +
                      " has no positive residual variance; not implemented in this version of PIET");

  const double breadScale = mlVariance ? fit.sigma2 / conventional : 1.0;
  fit.meanBread = vMat.data;
  for (auto& row : fit.meanBread)
    for (double& cell : row) cell *= breadScale;
  // A structural zero column has no variance, whatever the stored cell spells.
  for (size_t j = 0; j < columnCount; ++j) {
    if (fit.terms[j].reads != DesignTerm::Reads::kNothing) continue;
    for (size_t i = 0; i < columnCount; ++i) fit.meanBread[j][i] = fit.meanBread[i][j] = 0.0;
  }
  return std::nullopt;
}

// ---- the stacked scores --------------------------------------------------------

// Adds observation `row`'s score contribution for `fit` (scaled by the row's
// weight) into `sum`. False when the data no longer give a number for that row.
bool addScores(const FitEquations& fit, int64_t row, double rowWeight, long double* sum) {
  double fitted = 0.0;
  std::vector<double> x(fit.terms.size(), 0.0);
  for (size_t j = 0; j < fit.terms.size(); ++j) {
    const DesignTerm& term = fit.terms[j];
    switch (term.reads) {
      case DesignTerm::Reads::kNothing:
        continue;
      case DesignTerm::Reads::kConstant:
        x[j] = 1.0;
        break;
      case DesignTerm::Reads::kValue:
        x[j] = term.cells[row];
        break;
      case DesignTerm::Reads::kLevelOf: {
        const double v = term.cells[row];
        if (std::isnan(v)) return false;
        x[j] = v == term.level ? 1.0 : 0.0;
        break;
      }
    }
    fitted += term.coef * x[j];
  }
  const double residual = fit.depvarCells[row] - fitted;
  if (std::isnan(residual)) return false;
  const double scaled = residual / fit.sigma2;
  for (size_t j = 0; j < fit.terms.size(); ++j)
    sum[fit.firstSlot + j] += static_cast<long double>(rowWeight) * (scaled * x[j]);
  sum[fit.lnvarSlot()] +=
      static_cast<long double>(rowWeight) * (0.5 * (residual * scaled - 1.0));
  return true;
}

// Rows of the union in the order their scores are summed, and where each sum (a
// cluster, or a single row) ends.
struct SumGroups {
  std::vector<int64_t> rows;
  std::vector<size_t> ends;
};

SumGroups groupRows(const std::vector<int64_t>& unionRows, const double* clusterCells) {
  SumGroups groups;
  groups.rows = unionRows;
  if (clusterCells == nullptr) {
    groups.ends.resize(unionRows.size());
    std::iota(groups.ends.begin(), groups.ends.end(), size_t{1});
    return groups;
  }
  std::stable_sort(groups.rows.begin(), groups.rows.end(), [clusterCells](int64_t a, int64_t b) {
    return clusterCells[a] < clusterCells[b];
  });
  for (size_t i = 1; i <= groups.rows.size(); ++i)
    if (i == groups.rows.size() || clusterCells[groups.rows[i]] != clusterCells[groups.rows[i - 1]])
      groups.ends.push_back(i);
  return groups;
}

using Grid = std::vector<std::vector<long double>>;

// Bread * meat * bread, the bread being one block per fit.
std::vector<std::vector<double>> sandwich(const std::vector<FitEquations>& fits, const Grid& meat,
                                          double factor) {
  const size_t dim = meat.size();
  Grid bread(dim, std::vector<long double>(dim, 0.0L));
  for (const FitEquations& fit : fits) {
    const size_t k = fit.columns.size();
    for (size_t i = 0; i < k; ++i)
      for (size_t j = 0; j < k; ++j) bread[fit.firstSlot + i][fit.firstSlot + j] = fit.meanBread[i][j];
    bread[fit.lnvarSlot()][fit.lnvarSlot()] = 2.0L / static_cast<long double>(fit.observations);
  }
  Grid breadMeat(dim, std::vector<long double>(dim, 0.0L));
  for (size_t i = 0; i < dim; ++i)
    for (size_t l = 0; l < dim; ++l) {
      if (bread[i][l] == 0.0L) continue;
      for (size_t j = 0; j < dim; ++j) breadMeat[i][j] += bread[i][l] * meat[l][j];
    }
  std::vector<std::vector<double>> out(dim, std::vector<double>(dim, 0.0));
  for (size_t i = 0; i < dim; ++i)
    for (size_t j = i; j < dim; ++j) {
      long double above = 0.0L, below = 0.0L;
      for (size_t l = 0; l < dim; ++l) {
        if (bread[l][j] != 0.0L) above += breadMeat[i][l] * bread[l][j];
        if (bread[l][i] != 0.0L) below += breadMeat[j][l] * bread[l][i];
      }
      const double cell = static_cast<double>(static_cast<long double>(factor) * (above + below) / 2.0L);
      out[i][j] = cell;
      out[j][i] = cell;
    }
  return out;
}

}  // namespace

nlohmann::ordered_json runSuest(const eval::ParsedCommand& parsed, SessionState& session) {
  Request req;
  if (auto bad = readOptionLine(parsed, req)) return wrapBlock(*bad);

  // ---- which fits ----
  std::vector<std::string> names;
  {
    std::set<std::string> seen;
    for (const std::string& word : parsed.varlist)
      if (seen.insert(word).second) names.push_back(word);
  }
  if (names.empty())
    return wrapBlock(outOfScope(
        "suest without a model name (the active results `.`) is not implemented in this version "
        "of PIET; store the fit and name it"));
  for (const std::string& n : names)
    if (n == "." || n == "_all" || n.find_first_of("*?") != std::string::npos)
      return wrapBlock(outOfScope("suest of `" + n +
                                  "` (the active results or a pattern) is not implemented in "
                                  "this version of PIET; name each stored fit"));
  std::vector<std::shared_ptr<const StoredModel>> storedFits;
  for (const std::string& n : names) {
    const auto hit = session.estimatesModels.find(n);
    if (hit == session.estimatesModels.end())
      return wrapBlock(e3::rcError("estimation result " + n + " not found", 111));
    storedFits.push_back(hit->second);
  }
  Datastore* frame = session.datastore();
  if (frame == nullptr) return wrapBlock(ojson(eval::noDataLoaded()));

  // ---- one check pass per fit, stopping at the first failure ----
  std::vector<FitEquations> fits(names.size());
  size_t nextSlot = 0;
  for (size_t m = 0; m < names.size(); ++m) {
    const StoredModel* leader = m == 0 ? nullptr : storedFits[0].get();
    if (auto bad = screenFit(names[m], *storedFits[m], leader)) return wrapBlock(*bad);
    if (auto bad = loadFit(names[m], *storedFits[m], *frame, req.mlVariance, fits[m]))
      return wrapBlock(*bad);
    fits[m].firstSlot = nextSlot;
    nextSlot += fits[m].slotCount();
  }
  const size_t dim = nextSlot;

  // ---- the union sample, its weights, its clusters ----
  const int64_t frameRows = frame->nRows();
  std::vector<char> inUnion(static_cast<size_t>(frameRows), 0);
  std::vector<int64_t> unionRows;
  for (int64_t row = 0; row < frameRows; ++row) {
    const bool any = std::any_of(fits.begin(), fits.end(),
                                 [row](const FitEquations& f) { return f.inSample(row); });
    if (!any) continue;
    inUnion[static_cast<size_t>(row)] = 1;
    unionRows.push_back(row);
  }
  const double unionCount = static_cast<double>(unionRows.size());

  const std::string wtype = storedFits[0]->eStrings.get("wtype");
  const std::string wexp = storedFits[0]->eStrings.get("wexp");
  const double* weightCells = nullptr;
  double weightScale = 1.0;
  if (!wtype.empty()) {
    std::string var = strutil::trim(wexp);
    if (!var.empty() && var.front() == '=') var = strutil::trim(var.substr(1));
    const bool plainName = !var.empty() && frame->has(var) && frame->dtype(var) == "f64";
    if (!plainName)
      return wrapBlock(outOfScope("suest: the weight expression `" + wexp +
                                  "` is not a variable; not implemented in this version of PIET"));
    weightCells = frame->col(var).f64();
    long double total = 0.0L;
    for (int64_t row : unionRows) total += weightCells[row];
    weightScale = static_cast<double>(static_cast<long double>(unionCount) / total);
  }

  const double* clusterCells = nullptr;
  if (!req.clusterVar.empty()) {
    if (!frame->has(req.clusterVar))
      return wrapBlock(e3::rcError("variable " + req.clusterVar + " not found", 111));
    if (frame->dtype(req.clusterVar) != "f64")
      return wrapBlock(
          outOfScope("suest: a string cluster() variable is not implemented in this version of PIET"));
    clusterCells = frame->col(req.clusterVar).f64();
    for (int64_t row : unionRows)
      if (std::isnan(clusterCells[row]))
        return wrapBlock(e3::rcError("cluster variable " + req.clusterVar +
                                         " is not allowed to be missing within the estimation sample",
                                     459));
  }

  const SumGroups groups = groupRows(unionRows, clusterCells);
  const double groupCount = static_cast<double>(groups.ends.size());
  if (groupCount < 2.0)
    return wrapBlock(e3::rcError("insufficient observations", 2001, "engine_error"));

  // ---- the meat ----
  Grid meat(dim, std::vector<long double>(dim, 0.0L));
  std::vector<long double> stacked(dim, 0.0L);
  size_t cursor = 0;
  for (size_t endAt : groups.ends) {
    std::fill(stacked.begin(), stacked.end(), 0.0L);
    for (; cursor < endAt; ++cursor) {
      const int64_t row = groups.rows[cursor];
      const double rowWeight = weightCells == nullptr ? 1.0 : weightCells[row] * weightScale;
      for (const FitEquations& fit : fits) {
        if (!fit.inSample(row)) continue;
        if (!addScores(fit, row, rowWeight, stacked.data()))
          return wrapBlock(e3::rcError("suest: a variable of " + fit.name +
                                           " is missing inside its stored estimation sample",
                                       198));
      }
    }
    for (size_t i = 0; i < dim; ++i) {
      if (stacked[i] == 0.0L) continue;
      for (size_t j = i; j < dim; ++j) meat[i][j] += stacked[i] * stacked[j];
    }
  }
  for (size_t i = 0; i < dim; ++i)
    for (size_t j = 0; j < i; ++j) meat[i][j] = meat[j][i];

  const std::vector<std::vector<double>> stackedV =
      sandwich(fits, meat, groupCount / (groupCount - 1.0));

  // ---- the posting ----
  const bool severalFits = fits.size() > 1;
  std::vector<std::string> stackedNames;
  std::vector<double> stackedB;
  double rank = 0.0;
  for (const FitEquations& fit : fits) {
    const std::string lead = severalFits ? fit.name + "_" : std::string();
    for (size_t j = 0; j < fit.columns.size(); ++j) {
      stackedNames.push_back(lead + "mean:" + fit.columns[j]);
      stackedB.push_back(fit.terms[j].coef);
      if (fit.terms[j].reads != DesignTerm::Reads::kNothing) rank += 1.0;
    }
    stackedNames.push_back(lead + "lnvar:_cons");
    stackedB.push_back(std::log(fit.sigma2));
    rank += 1.0;
  }

  ojson block;
  block["type"] = "estimation";
  block["estimator"] = "suest";
  block["depvar"] = "";
  block["n_obs"] = static_cast<long long>(unionRows.size());
  block["coeftable"] = e3::coeftableFromPosting(stackedNames, stackedB, stackedV,
                                                std::numeric_limits<double>::quiet_NaN());

  LastModel posted;
  posted.cobraCmd = "suest";
  posted.stataCmd = "suest";
  posted.depvar = "";
  posted.vcov = "";
  posted.result = block;
  (void)e3::installEsampleMask(session, posted, inUnion);
  session.setLastModel(posted);
  session.lastEstBlock = block;
  e3::postBV(session, stackedNames, stackedB, stackedV);

  e3::setEScalarOnFit(session, "N", unionCount);
  e3::setEScalarOnFit(session, "rank", rank);
  e3::setEStringOnFit(session, "cmd", "suest");
  e3::setEStringOnFit(session, "vcetype", "Robust");
  e3::setEStringOnFit(session, "properties", "b V");
  if (clusterCells != nullptr) {
    e3::setEScalarOnFit(session, "N_clust", groupCount);
    e3::setEStringOnFit(session, "vce", "cluster");
    e3::setEStringOnFit(session, "clustvar", req.clusterVar);
  } else {
    e3::setEStringOnFit(session, "vce", "robust");
  }
  std::string joined;
  for (const std::string& n : names) joined += (joined.empty() ? "" : " ") + n;
  e3::setEStringOnFit(session, "names", joined);
  for (size_t m = 0; m < names.size(); ++m)
    e3::setEStringOnFit(session, "eqnames" + std::to_string(m + 1), "mean lnvar");
  if (!wtype.empty()) {
    e3::setEStringOnFit(session, "wtype", wtype);
    e3::setEStringOnFit(session, "wexp", wexp);
  }
  return wrapBlock(std::move(block));
}

}  // namespace handlers
}  // namespace piet
