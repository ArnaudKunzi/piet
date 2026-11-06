// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// R2 (giant-file split, 2026-09-01): cross-TU declarations for the shared
// helpers of the former 7,859-line estimation_m4.cpp. Definitions stay in
// estimation_m4.cpp (namespace est_detail); the two continuation TUs include
// this and `using namespace est_detail;`. Default arguments live HERE (the
// declaration) and are stripped from the definitions, per the one-default rule.
#pragma once
#include <map>
#include <optional>
#include <string>
#include <vector>
#include "nlohmann/json.hpp"
#include "piet/datastore.h"  // EsampleLostBy
#include "piet/session_state.h"
#include "piet/eval/errors.h"  // eval::ParsedCommand / OptValue

namespace piet::handlers {
using ojson = nlohmann::ordered_json;
namespace est_detail {
struct EstimationArgvResult {
  std::vector<std::string> argv;
  bool errored = false;
  ojson errorBlock;
};

void captureFitFilterM4(LastModel& lm, const std::vector<std::string>& argv);
std::string cobraErrorStrM4(const CobraError& ce);
ojson cobraToEstimationM4(const ojson& raw, const std::string& cobraCmd,
                           const SessionState* session = nullptr);
bool computeYMinMaxM4(SessionState& session, const std::string& yName,
                       const std::optional<std::string>& ifExpr, double& outMin, double& outMax,
                       ojson& errBlockOut);
ojson engineErrorBlockM4(const std::string& message, const std::string& code = "engine_error");
std::string joinCommaM4(const std::vector<std::string>& v);
std::string lowerM4(std::string s);
std::string nextPathM4(const std::string& workdir, const std::string& prefix);
double normalPvalue(double z);
std::string trimM4(const std::string& s);
EstimationArgvResult buildEstimationArgvM4(const std::string& cobraCmd,
                                            const eval::ParsedCommand& parsed, SessionState& session,
                                            const std::optional<std::string>& extraFe = std::nullopt);
nlohmann::json runCobraJsonM4(std::vector<std::string> args);
void applyFactorValueLabelsM4(ojson& coeftable, const SessionState& session);
struct VcovResult {
  std::vector<std::string> argv;
  bool errored = false;
  std::string errorMsg;
  // Stata's own rc for a refusal that is not the generic r(198) (ppmlhdfe's
  // r(3498)/r(9)); 0 = the unsupported_option r(198). vcovErrorBlockM4 builds it.
  int rc = 0;
};
ojson vcovErrorBlockM4(const VcovResult& vr);
// Whose option parser reads vce() and cluster(): Stata's _vce_parse (logit,
// probit, xtreg, poisson, nbreg), ivregress's own, or ppmlhdfe's.
enum class ClusterOptParserM4 { kVceParse, kIvregress, kPpmlhdfe };
// What that parser refuses about the cluster variables -- one that is not in the
// data, a second one where the fit takes one, vce() paired with cluster() --
// before any engine call, with Stata's rc and wording (definition:
// estimation_m4.cpp).
std::optional<ojson> clusterSpecRefusalM4(const eval::ParsedCommand& parsed, SessionState& session,
                                          ClusterOptParserM4 parser, bool multiwayVceRefused,
                                          bool legacyOneVar = true);
inline constexpr double kZ975 = 1.959963984540054;  // qnorm(0.975)
VcovResult vcovFromOpts(const eval::ParsedCommand& parsed, bool* sawRobustFlag = nullptr);
void pushLevelIfGivenM4(const eval::ParsedCommand& parsed, std::vector<std::string>& argv);
ojson runP7Mle(const std::string& cobraCmd, std::vector<std::string> cobraArgs,
               SessionState& session, ojson* rawOut = nullptr);
// BUG-1196 (estimation_m4.cpp, beside runP7Mle): e(b)/e(V) posted for the fits
// whose Stata stripe the coefficient table does not carry.
struct AncillaryEbColM4 {
  std::string stata;   // Stata's column name
  std::string engine;  // the engine's `e_V` name; "" = a structural 0 column
  double b = 0;
};
bool depvarEqColsM4(const std::string& depvar, const ojson& coefs, const char* bKey,
                    std::vector<AncillaryEbColM4>& out);
bool postAncillaryEbM4(SessionState& session, const ojson& raw,
                       const std::vector<AncillaryEbColM4>& cols);
std::vector<std::string> splitTokensM4(const std::string& s);
std::vector<std::string> parseAtNumlistM4(const std::string& at);
void fillCoeftableStatsM4(ojson& block);
std::optional<std::string> applyIfFilterM4(SessionState& session, const std::string& hbinPath,
                                           const std::optional<std::string>& ifExpr,
                                           ojson& errBlockOut,
                                           const std::optional<std::string>& inRange = std::nullopt);
// The working-frame rows of `lm`'s estimation sample: those whose hidden e(sample) cell
// (kEsampleColumn) holds the fit's install serial. nullopt when the mask is not on the
// working frame (LastModel::esample not kColumn, no data, no mask column).
std::optional<std::vector<int64_t>> esampleRowsM4(SessionState& session, const LastModel& lm);
// The working frame's f64 column `name`, nullptr when there is none.
const double* f64ColumnM4(SessionState& session, const std::string& name);
}  // namespace est_detail
// Cross-TU (defined in estimation_m4b.cpp, consumed by estimation_m4c.cpp):
ojson csdidToEstimationM4(const ojson& raw, const std::string& subcmd);
// csdid-ematrix-post: one {names, b, V} posting of the csdid engine reply
// (`attgt`, `agg_simple|event|group|calendar`, or estat_event's {e_b, e_V}),
// the coefficient table Stata prints for it (one row per posted column, the
// full `eq:name` as the row name, normal-based z/p/CI from sqrt(V_ii)), and
// the e(b)/e(V) install. Defined in estimation_m4b.cpp.
struct CsdidPostingM4 {
  std::vector<std::string> names;
  std::vector<double> b;
  std::vector<double> V;  // row-major K x K; NaN = missing
};
std::optional<CsdidPostingM4> csdidPostingM4(const ojson& raw, const std::string& key);
ojson csdidPostedCoeftableM4(const CsdidPostingM4& P);
void postCsdidPostingM4(SessionState& session, const CsdidPostingM4& P,
                        const std::string& bName = "b", const std::string& vName = "V");
// e(sample) T2 (defined in handlers.cpp beside runRegress, the first wired fit;
// piet/handlers.h is not this change's to edit): the fit handlers' mask
// install, shared by the iv (estimation_m4b.cpp), xtreg,fe (estimation_m4.cpp),
// newey and prais (estimation_m4c.cpp) routes.
std::string esampleFitArgs(SessionState& session, std::vector<std::string>& argv,
                           const std::string& indexVar, int64_t headRows);
bool esampleInstallOnHead(SessionState& session, const std::string& valuesPath, ojson& raw,
                          LastModel& lm);
// The column write alone, onto `basePath`'s frame (runRegress's residuals() frame,
// runPoisson's ppmlhdfe d frame); returns the new frame's path or "".
std::string esampleInstallColumn(SessionState& session, const std::string& valuesPath,
                                 ojson& raw, const std::string& basePath,
                                 bool residentEligible, unsigned long long* serialOut);
// e(sample) T2, the data verbs that build observations out of others (handlers.cpp,
// beside esampleInstallOnHead, where the StataBE rules are cited): expand, cross,
// collapse, contract, joinby, bsample. Each is a no-op when the working frame
// carries no mask.
std::optional<std::vector<double>> esampleHeadMask(SessionState& session);
bool esampleRewriteHeadMask(SessionState& session, const std::vector<double>& cells);
bool esampleMarkHeadLost(SessionState& session, EsampleLostBy by);
// merge 1:m (blockLen null) and joinby (blockLen = esampleUsingRunLengths): the
// marker stays on the first copy of each master row (handlers.cpp cites the rule).
bool esampleKeepFirstCopy(SessionState& session, const std::vector<std::string>& keys,
                          const std::map<std::string, int64_t>* blockLen);
std::map<std::string, int64_t> esampleUsingRunLengths(const Datastore& usingDs,
                                                      const std::vector<std::string>& keys);
// collapse/contract: read the groups' marks BEFORE the verb (`restricted`: an
// if/in, weight or casewise rule may leave some of a group's rows out), write
// them onto the result AFTER it.
struct EsampleGroupMarks {
  bool active = false;  // the frame carried a mask and the verb has by-groups
  bool lost = false;    // Stata's answer depends on its tie order: EsampleLostBy
  std::map<std::string, double> byKey;  // by-values -> the group's first row's cell
};
EsampleGroupMarks esampleGroupMarksBefore(SessionState& session,
                                          const std::vector<std::string>& byVars,
                                          bool restricted);
bool esampleGroupMarksAfter(SessionState& session, const EsampleGroupMarks& g,
                            const std::vector<std::string>& byVars, EsampleLostBy by);
// `expand N [if] [in]` (handlers_data_verbs3.cpp; runExpand, declared in
// piet/handlers.h, is its unqualified form).
ojson runExpandIf(const std::string& countArg, const std::optional<std::string>& genVar,
                  const std::optional<std::string>& ifExpr,
                  const std::optional<std::string>& inRange, SessionState& session);
// FU-475 (handlers.cpp, beside runRegress): the named r(198) for a fit whose
// regressors carry a time-series operator and whose restriction the engine's
// --keepif cannot apply after those operators (`in`, `_n`/`_N`, an operator in
// the `if` itself); nullopt otherwise.
std::optional<ojson> tsFitRestrictionRefusal(const eval::ParsedCommand& parsed,
                                             const std::string& stataCmd);
// e(df_a) after areg / reghdfe from the engine's FE reply (handlers.cpp, beside
// runRegress, where the counting rules are cited); ivreghdfe's iv reply reads it as
// "reghdfe" (runIvRegress). nullopt = not derivable, nothing posted.
std::optional<double> absorbedDofA(const ojson& raw, const std::string& stataCmd);
// FU-475's refit half (estimation_m4c.cpp): the named refusal for a
// post-estimation re-fit of a TS-operator fit whose `if` the engine applied
// (lm.fitKeepif); nullopt otherwise.
std::optional<ojson> postestTsKeepifRefusalM4(const std::string& verb, const LastModel& lm);
// Cross-TU (defined in estimation_m4c.cpp, consumed by runBoottest in estimation_m4b.cpp):
bool boottestRefitMatchesFitM4(const LastModel& lm, ojson& reject);
bool boottestFitLacksConstantM4(const LastModel& lm);
}  // namespace piet::handlers
