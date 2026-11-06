// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
#pragma once

// Shared surface for the apps/cmds/*.cpp command-handler translation units.
// This file was factored out of the former monolithic apps/cecon_cli.cpp
// (pure code motion — internal Step-1 W0 / WP-00): the top-of-file includes, the
// json alias, and declarations for the ~35 helpers (plus a handful of
// cross-file estimator loaders) that used to live in one big anonymous
// namespace. Splitting the command bodies across multiple .cpp files means
// these need EXTERNAL linkage (declared here, defined in common.cpp) instead
// of the anonymous namespace's internal linkage.

#include <chrono>
#include <algorithm>
#include <cctype>
#include <functional>
#include <cmath>
#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <memory>
#include <unordered_map>
#include <cstdlib>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include "io/win_compat.h"  // native_open/to_native_path: wide open for non-ASCII paths (BUG-050)
#include "core/error.h"
#include "core/linalg.h"
#include "core/model.h"
#include "core/threads.h"
#include "core/rng.h"
#include "data/frame.h"
#include "data/ts_operators.h"
#include <map>
#include "io/columnar.h"
#include "estimators/posthest.h"  // betai/t_critical/t_twosided_pvalue/chi2_cdf/chi2_pvalue/f_pvalue + wald/hausman/margins — shared stats helpers (WP-00: posthest.h was in the monolith's includes)
#include "nlohmann/json.hpp"

// BUG-100: use ordered_json so estimator JSON objects (coef{}, se{}, tstat{}, …)
// iterate in INSERTION order (= estimation order: typed varlist order for plain
// vars, ascending numeric level for factor terms) rather than the lexicographic
// key order of plain nlohmann::json's std::map. tidy_coeftable() builds its rows
// by iterating the coef object, so the coeftable row order (and the raw dicts)
// now match Stata. Same reason piet/native uses `using ojson = ordered_json;`.
using json = nlohmann::ordered_json;
using namespace cecon;

#ifndef COBRA_VERSION
#define COBRA_VERSION "dev"
#endif

// Boundary A contract version (plain semver, User-decided scheme). Owned by CoBrA Op as
// pen-holder — bumps when CONTRACT-engine.md changes, independently of the engine release
// version (COBRA_VERSION). Source of truth mirrored in internal note CONTRACT-engine.md header.
#ifndef CONTRACT_VERSION
#define CONTRACT_VERSION "1.26.0"
#endif

namespace cecon {
namespace cli {

// --- provenance plumbing (set in main, consumed by emit) -----------------------
extern std::vector<std::string> g_argv;  // full invocation (cobra + args)
extern std::string g_data_path;          // value of --data (original, pre-keepif)
extern bool g_provenance;                // attach provenance{} to estimator JSON

// FNV-1a 64-bit over the raw file bytes — a cheap content fingerprint so a run's
// provenance pins the EXACT input (irrecoverable after the fact). Cached by path.
std::string hash_file_hex(const std::string& path);

// Drop hash_file_hex's path→hash memo. serve mode only (contract_A §25, per-request
// reset — a path rewritten between requests must re-hash); one-shot never calls it.
void hash_cache_clear();

// Seed the hash_file_hex memo with a known content hash for `path`. serve resident-run
// mode (§26): the root file was hashed ONCE at `use`; seeding avoids re-hashing the
// (possibly large) root on every frame-run's estimator provenance.
void hash_cache_put(const std::string& path, const std::string& hex);

// FNV-1a-64 over a byte range, rendered %016llx — the ONE implementation of the
// content hash whose output goldens pin (hash_file_hex delegates to it).
std::string hash_bytes_hex(const void* data, size_t len);

// ENG-ADOPTION-AVOIDANCE (§26 bridge, DEFINED in apps/cecon_cli.cpp): the resident
// session's root hash, computed LAZILY from the root's own adoption mmap the first
// time anything actually needs it, then cached on the session. Empty when there is
// no serve session, when `path` is not that session's root, or when the root is a
// non-owning view — every such case falls through to hash_file_hex's file read.
//
// WHY LAZY. `frame use` used to hash the root eagerly on EVERY adoption, and a
// re-adoption happens on every chain break (measured: 2,241 adoptions in one
// analysis.do run, ~270 ms of whole-file FNV each = ~10 of the engine's 19 minutes).
// The hash's ONLY consumer is build_provenance, which runs solely for results
// carrying an "estimator" key — a small minority of requests. Deferring it removes
// the work from every adoption that never needed it, and changes no emitted value.
std::string serve_root_hash(const std::string& path);

// ---- contract_A §26: resident-frame serve session bridge -----------------------
// A frame-run request holds ONE working dataset in RAM across requests. These three
// hooks (DEFINED in apps/cecon_cli.cpp, where the session lives) let the shared data
// plane below consult it WITHOUT any command knowing about serve. Outside a frame-run
// (one-shot, stateless §25) serve_resident_active() is false and the other two are
// never called, so every existing path is byte-untouched.
bool serve_resident_active();            // true only DURING a frame-run "run" dispatch
const Dataset& serve_resident_view();    // the session's cached zero-copy view of the resident frame
void serve_stage_pending(Frame&& fr);    // stage a mutator's result as the pending new resident
// GA sweep 2026-08-22. `merge` reports its match counts on STDERR, and PIET parses
// them there to publish `merge_counts` in the user-visible result. That notice is
// emitted only on the one-shot leg, so merge could never be routed onto the resident
// plane without silently dropping a number the user sees — the exact class this sweep
// exists to remove. A staged merge publishes them here instead, and the frame response
// carries them out of band (contract_A: additive key, absent unless a merge staged).
void serve_publish_merge_counts(int64_t master_only, int64_t using_only, int64_t matched);

// ---- contract_A §31: session estimation store (post-estimation by handle) ---------
// The STORABLE projection of a fitted linear model: exactly the fields lincom/nlcom/
// test read to answer a combination, and nothing else. Every member is copied out of
// FeolsResult, which is already a plain value struct — so nothing in src/ has to change
// to make a fit storable.
//
// BS-098 / BS-013 (the reason this shape is the safe one): `vcov_matrix` travels with
// the model AS FITTED (hc1 / cluster / multiway / iid). A post-estimation call that
// addresses a model by handle passes NO --vcov at all, so there is no flag to forget
// and no path by which the SE can silently revert to cobra's `iid` default — the only
// covariance in scope is the fit's own. `n_clusters` + `dof_resid` are BOTH stored
// because the dof branch (`n_clusters > 0 ? G-1 : dof_resid`) must be reproduced
// literally, not re-derived.
struct PostestModel {
    std::vector<std::string> names;
    std::vector<double> beta;
    std::vector<double> vcov_matrix;  // k*k, column-major — AS FITTED, never re-derived
    std::string vcov_type;            // echoed into the result JSON verbatim
    int n_clusters = 0;
    int64_t dof_resid = 0;
    int64_t n = 0;                    // n_obs
    // BUG-203: true when the fit is a maximum-likelihood one (`--model
    // logit|probit|poisson`), i.e. its covariance is an information matrix and
    // there is NO residual degrees-of-freedom. Stata references post-estimation
    // off such a fit to the NORMAL (lincom/nlcom print z, not t) and to chi2(J)
    // (`test` prints chi2, not F); a t(dof_resid) reference is simply not
    // defined for it. Travelling on the model — rather than being re-derived at
    // each call site — is what makes the §31 handle path answer identically to
    // the fitting path by construction, the same argument `vcov_matrix` above
    // makes for BS-098/BS-013.
    bool asymptotic = false;
    // FU-439: lincom's reference dof after `regress ..., vce(cluster a b ...)` is the
    // residual dof N - k (w2s_multiway_class.log:158 r(df) 177), not e(df_r) = G-1
    // (:116 df_r 6), while test keeps G-1 (:174 r(df_r) 6). areg and xtreg,fe post
    // through ereturn and lincom reads e(df_r) there (:372 r(df) 6). Set by the
    // least-squares refit for a multiway fit under --multiway-subset-factors with no
    // absorbed effect; read only by lincom (postest_lincom_dof).
    bool lincom_resid_dof = false;
    // BUG-1231: s^2 when the covariance is the classic V = s^2 (X'X)^-1 of a
    // least-squares fit (regress/areg/xtreg,fe), whose `test` judges a constraint
    // negligible on R V R' / s^2; -1 for every other covariance (R V R' itself).
    // Set by the least-squares refit; read only by test (cmds_postest.cpp).
    double ls_scale = -1.0;
    // BUG-1276: the least-squares refit of a `regress` exact fit (V = 0) carries the
    // bread inv(X'X) (FeolsResult::exact_fit_bread), under which Stata's `test`
    // still drops the constraints that depend on the others. Empty otherwise.
    std::vector<double> exact_fit_bread;
};

// Non-null ONLY during a §31 `postest` frame dispatch: the stored model that request
// addressed by handle. Null everywhere else (one-shot, stateless §25, frame-run), so
// every existing caller fits from its own --data/--y/--x specification exactly as before.
const PostestModel* serve_active_model();

// Publish a just-fitted model so the serve loop can mint a handle for it. A NO-OP
// outside serve frame-mode — one-shot and stateless §25 requests keep zero session
// footprint, so their behavior and output bytes are untouched.
void serve_publish_model(const PostestModel& m);

// Open the PRIMARY dataset for a command. Resident-run mode + empty `data` → the
// session's resident view (zero copy, no re-read); otherwise Dataset::open(data)
// exactly as before (the returned ref is backed by a process-static holder, valid
// until the next acquire_data call — single-threaded, one primary open per command).
// `data` is the site's ALREADY-RESOLVED path (post --keepif), never re-derived from a.
const Dataset& acquire_data(const std::string& data);

// ENG-SETCOL-DEDUP. The shared handle behind the most recent PATH-based acquire_data
// (null while the resident §26 view is what was acquired), and the Frame builder that
// uses it. WHY: Frame::from_dataset re-OPENS the file to own an independent mmap for
// the Frame's lifetime — a second full header parse of a file this process just parsed.
// On the campaign corpus that header is 2.4 MB (99% factor level strings; one ID column
// alone carries 108,384 levels), so the re-open cost ~16 ms and a single one-column
// `setcol` paid the header three times over. frame_from_acquired shares the handle
// instead, and falls back to from_dataset for any dataset it cannot identify.
std::shared_ptr<const Dataset> acquired_handle();
Frame frame_from_acquired(const Dataset& ds);

// WS-D2 / S06 ARC-MRG. The `using` (second) input of merge/append/joinby/cross, with
// exactly ONE physical open — the counterpart of acquire_data/frame_from_acquired for
// the second file. Accepts .hbin / .dta / .csv / .tsv, chosen by extension, so all
// four verbs agree on what a `using` file may be. The old per-site idiom
// (`Dataset uds = Dataset::open(p); frame_from_acquired(uds);`) opened the .hbin
// TWICE — see common.cpp for the measured backtrace. Prefer this at any new site.
Frame open_using_frame(const std::string& path);

// True when a command's own `--data`/`--out` emptiness guard should be SUPPRESSED
// because the resident session supplies the data / receives the result (§26). In
// one-shot/stateless mode these are just `s.empty()`, so guards behave as before.
inline bool data_missing(const std::string& s) { return s.empty() && !serve_resident_active(); }
inline bool out_missing(const std::string& s) { return s.empty() && !serve_resident_active(); }

// Commit a mutator's result frame. Resident-run mode → stage `fr` as the session's
// pending new resident (applied after a clean dispatch); `out` ignored; NO disk write,
// NO notice; returns true (caller then SKIPS its site-specific "wrote ..." notice).
// One-shot/stateless → write `fr` to `out` via write_hbin when `out` is non-empty
// (matches the callers that guard on out), returns false (caller emits its notice).
bool commit_frame(Frame&& fr, const std::string& out);

std::vector<std::string> split_csv(const std::string& s);

// ---- E6: unknown-flag rejection net --------------------------------------------
// opt()/has()/opt_all() are the sanctioned way a cmd_XXX() reads its flags out of
// `a` (the per-invocation argv slice; a[0] is the subcommand name). g_argv_consumed
// records, per invocation, WHICH INDICES of `a` a real match actually touched.
extern std::vector<char> g_argv_consumed;

void mark_consumed(const std::vector<std::string>& a, size_t i);

std::string opt(const std::vector<std::string>& a, const std::string& flag,
                const std::string& def = "");
bool has(const std::vector<std::string>& a, const std::string& flag);

// All values for a repeatable flag (e.g. multiple --gen).
std::vector<std::string> opt_all(const std::vector<std::string>& a, const std::string& flag);

// After a command has run to completion, reject any argv token shaped like a flag
// ("--xxx") that no opt()/has()/opt_all() call ever matched. Under buffered dispatch
// (cecon_cli.cpp) this runs BEFORE the buffered result is flushed, so an unrecognized
// flag never emits a (silently-wrong) result on stdout — it errors instead (BUG-110).
// COBRA_OPTIONS_STRICT (set in ctest) additionally asserts every CONSUMED flag is
// declared in options_table() — the drift detector that keeps `cobra options` honest
// (that half is check_options_table_drift below, which this calls).
void check_unconsumed_flags(const std::vector<std::string>& a);

// The COBRA_OPTIONS_STRICT drift half on its own — no-op unless that env var is set.
// Separate because it must be reachable on the paths the unrecognized-option net above
// deliberately skips: a command that returned rc != 0, and (BUG-182) a handler that
// THREW. Refuse-by-name flags always throw, so without this they were structurally
// unverifiable against their options_table() declaration. Idempotent; safe to call from
// a catch block. See common.cpp for the full rationale.
void check_options_table_drift(const std::vector<std::string>& a);

// B.0.3 option-forwarding contract (Boundary A §option-forwarding). One accepted flag
// of one subcommand; takes_value distinguishes opt/opt_all (a value follows) from a
// boolean has() flag. Serialized by `cobra options --json` for PIET's pre-forward
// check; the authoritative reject remains the consumed-marks net above.
struct OptSpec { const char* flag; bool takes_value; };
// argv[0]-keyed table (dispatch names incl. aliases: tab, pwcorr, iv-diag, import-dta).
const std::map<std::string, std::vector<OptSpec>>& options_table();

// Parse time-series metadata flags for the L./F./D./S. varlist operators.
cecon::TsMeta parse_tsmeta(const std::vector<std::string>& a);

// --format tidy: add a uniform coeftable [{name,b,se,t,p,ci_lo,ci_hi}] alongside
// the native fields, so a driver can parse ONE schema regardless of whether the
// estimator emits a coef{} dict or a coefs[] array.
extern bool g_format_tidy;

json tidy_coeftable(const json& o);

// Attach tidy coeftables in place, descending into multi-outcome / multi-equation
// containers (feols_multi, iv2sls_multi, reg3).
void apply_tidy(json& o);

// provenance{} — what the engine alone can attest to: the exact build, the exact
// invocation, the exact input (path + content hash), and the sample/convergence
// accounting at read time.
json build_provenance(const json& out);

void emit(const json& out, const std::string& outpath);

double tstat(double b, double se);

// Two-sided normal (z) p-value, 2*(1-Phi(|z|)), computed WITHOUT the
// catastrophic cancellation of that literal spelling.
//
// FULL FUNCTIONAL CHECK item (2026-08-05), measured: `2*(1-0.5*erfc(-|z|/sqrt2))`
// evaluates the upper tail by subtracting a number near 1 from 1, so it loses
// the answer's leading digits as the tail shrinks — 4e-5 relative error already
// at |z|=7, and from |z|~8.3 it returns EXACTLY 0.0 where the true value is
// ~1e-16 and still falling to ~1e-299. A printed table hides it (Stata prints
// 0.000 too) but `e(p)` and anything that READS the value get a hard zero.
// erfc(|z|/sqrt2) IS the two-sided tail directly, with no cancellation.
inline double z_twosided_pvalue(double z) {
  return std::erfc(std::fabs(z) * 0.7071067811865476);
}
// Lower-tail F CDF P(F_{d1,d2} <= f) — betai directly so both tails stay accurate.
double f_cdf(double f, double d1, double d2);
// Student-t CDF P(T_dof <= t).
double t_cdf(double t, double dof);
// Non-missing values of a numeric column.
std::vector<double> nonmissing(const double* x, int64_t n);
// mean + sample (n-1) sd of a vector.
void mean_sd(const std::vector<double>& v, double& mean, double& sd);

// Attach two-sided p-values + level% CI (dicts keyed by coef name) to an estimator
// JSON. asymptotic=true uses z/normal inference (Stata `ivregress` default);
// false (the default) keeps Student-t(dof) exactly as every existing caller had.
// level drives the CI quantile on the t branch only (iv doesn't expose --level).
void add_inference(json& out, const std::vector<std::string>& names,
                   const std::vector<double>& beta, const std::vector<double>& se,
                   double dof, bool asymptotic = false, double level = 95.0);

// Append Stata-style omitted (collinear) terms to a coef object. `full_names`
// (optional) is the pre-drop design-matrix column order; when given (and it
// accounts for every dropped name), the inference maps are rebuilt into that
// natural position instead of leaving omitted terms tail-appended.
void add_omitted(json& out, const std::vector<std::string>& omitted,
                 const std::vector<std::string>& full_names = {});

// BUG-112(8): append non-fatal model-spec notes (ModelData/IvModelData::warnings)
// as a top-level "warnings" array. The key is emitted ONLY when there is something
// to say, so every clean response stays byte-identical to before — no golden in the
// 127-case corpus re-captures. Stata's counterpart is its `note:` output line; the
// StataBE 19.5 oracle (2026-07-31) confirmed warn-and-proceed, not reject.
void add_warnings(json& out, const std::vector<std::string>& warnings);

// Opt-in (`--emit-v`) full fitted covariance as out["e_V"] = {names, matrix}, the shape
// oaxaca / nnmatch / csdid already emit. `V` is the k*k column-major matrix AS FITTED
// (never re-derived); `names` is its row/column order. `order` (optional) is a
// permutation of 0..k-1 giving the emitted order, so the matrix can follow the reply's
// coefficient order when that differs from the fit's (iv display order). matrix[i][j]
// is V[order[i] + order[j]*k]. A size mismatch is an engine bug and throws "internal".
void add_e_v(json& out, const std::vector<std::string>& names, const std::vector<double>& V,
             const std::vector<size_t>* order = nullptr);

// Emit the model-level overall-F and its p-value (Prob>F).
// df2 = the F reference residual dof. A double, not an integer: under [iw=] Stata's
// e(df_r) is fractional (Sum(w)-k), and rounding it before the p-value moves Prob>F.
// Every integral caller converts implicitly and its output is unchanged.
void add_model_f(json& out, double f, int df1, double df2);

// Emit MLE binary-model fit scalars (Stata logit/probit header).
void add_mle_fit(json& out, const std::vector<double>& p, const std::vector<double>& y,
                 int64_t n, int df);

std::string trimx(const std::string& s);
// split on commas AND whitespace (for by(h_pid year) style varlists)
std::vector<std::string> split_vars(const std::string& s);
// index of the assignment '=' (a lone '=', not part of == <= >= !=)
size_t find_assign(const std::string& s);
// contents of key(...) inside s, paren-matched ("" if absent)
std::string paren_arg(const std::string& s, const std::string& key);

// --- Shared loaders for the estimator commands (csdid / xt* / nbreg) ---------
// Validate a column exists AND is numeric (f64).
const double* require_num(const Dataset& ds, const char* cmd, const std::string& name);

// Row indices where every listed column is non-missing (listwise deletion).
std::vector<int64_t> listwise_keep(const std::vector<const double*>& cols, int64_t n);

// Compact a column to the kept rows.
std::vector<double> gather_rows(const double* src, const std::vector<int64_t>& keep);

// Indices of covariates (cols of col-major X, n*k) that vary WITHIN at least one
// panel.
std::vector<int> within_varying_cols(const std::vector<double>& X, const double* ivar,
                                     int64_t n, int k);

// Scratch .hbin files created by maybe_keepif this process.
extern std::vector<std::string> g_keepif_scratch;

void keepif_cleanup();

// Resolve the scratch directory honoring $TMPDIR -> $TMP -> /tmp.
std::string keepif_scratch_dir();

// If keepif is non-empty, load the data into a Frame, apply the row filter, and
// write a temp .hbin whose path is returned.
std::string maybe_keepif(const std::string& data, const std::string& keepif);

// Evaluate `keepif` against the FULL dataset WITHOUT removing any rows, returning a
// length-n_rows() mask (element 1 = row satisfies the `if`).
std::vector<char> eval_keepif_mask(const std::string& data_path, const std::string& keepif);

// True if any token in `names` carries a time-series operator (L./F./D./S.).
bool varlist_has_ts(const std::vector<std::string>& names);

// The E1 routing decision for one `--keepif`-bearing estimator call.
struct KeepifPlan {
    std::string data;
    std::vector<char> mask;
    bool active = false;
};
KeepifPlan plan_keepif(const std::string& data, const std::string& keepif, const TsMeta* ts,
                       std::initializer_list<const std::vector<std::string>*> token_lists);

// ---- e(sample): the fit's admitted rows, emitted on request (esample T1a) ------
// `--esample-out PATH [--esample-index COL --esample-n N]`. Opt-in: without
// --esample-out nothing is written and the JSON carries no new key, so every
// existing response stays byte-identical.
struct EsampleRequest {
    std::string path;       // --esample-out (empty = not requested)
    std::string index_col;  // --esample-index (empty = rows of the --data frame)
    int64_t n_target = 0;   // --esample-n (meaningful only with index_col)
    bool active() const { return !path.empty(); }
};
// Reads (and so consumes) the three flags. A half-given pair, an index/n without
// --esample-out, or a non-integer / negative N is bad_input.
EsampleRequest esample_request(const std::vector<std::string>& a, const char* cmd);

// What a runner hands back for emit_esample: the kept row indices (ModelData::keep /
// IvModelData::keep — AFTER listwise deletion, zero/missing weights, missing
// cluster rows, TS-operator loss and singleton drops) and the Dataset they index.
// `loaded` borrows the runner's acquire_data() reference; consume it before the
// next acquire_data call.
struct EsampleRows {
    std::vector<int64_t> keep;
    const Dataset* loaded = nullptr;
};

// Writes req.path (one `1`/`0` per line — the headerless payload
// Frame::read_csv_column takes for `cobra setcol`) and sets out["esample_n"] to
// the number of 1s. The file's rows are the `--data` frame's rows BEFORE the
// command's --keepif; how `rows.keep` maps onto them:
//   keepif empty            -> identity (the fit loaded `data` itself);
//   keepif_masked (E1 path) -> identity (plan_keepif kept the FULL file);
//   otherwise               -> the fit loaded maybe_keepif's physically reduced
//                              scratch, whose row j is the j-th row satisfying the
//                              predicate; eval_keepif_mask(data, keepif) recovers it.
// With req.index_col the file instead has req.n_target lines and loaded row i lands
// on line COL[i] (1-based); every loaded row's COL must be an integer in [1, N],
// each used once, else bad_input and no file.
void emit_esample(const EsampleRequest& req, const char* cmd, const std::string& data,
                  const std::string& keepif, bool keepif_masked, const EsampleRows& rows,
                  json& out);

// Shared loader for binary/count models with an explicit intercept: returns the
// listwise-kept design [x..., 1] (col-major), y, and the kept indices. Callers span
// cmds_mle.cpp, cmds_postest.cpp (margins), and cmds_diag.cpp (hettest/ovtest/
// dwatson/durbinalt/bgodfrey/archlm) — needs external linkage across those TUs.
struct GlmInputs {
    int64_t n = 0, n_dropped = 0;
    int k = 0;                       // # x + 1 (intercept)
    std::vector<double> X, y;        // X col-major n*k, intercept LAST
    std::vector<std::string> names;  // x names then "(intercept)"
    std::vector<int64_t> keep;
    std::vector<std::string> warnings;  // factor-spec notes (absent-base, etc.)
    // P5 (pweight logit/cluster-robust): populated only when the caller passes
    // cluster_names / weight_var to load_glm below. Empty/None on every existing
    // call site — those stay byte-identical to before these fields existed.
    std::vector<cecon::Factor> cluster;
    std::vector<double> weights;                 // raw (UN-normalized), length n; empty = unweighted
    cecon::WeightKind weight_kind = cecon::WeightKind::None;
    // BUG-331: Stata `offset()` — a term added to the linear predictor with its
    // coefficient fixed at 1. Populated only when the caller names one; empty on
    // every pre-existing call site, which keeps their design byte-identical.
    std::vector<double> offset;
};
// cluster_names/weight_var/weight_kind (P5): optional, threaded straight through
// to build_model's own cluster/weight machinery (same single listwise-deletion
// pass as y/x). Every existing caller passes none of them and gets the exact
// prior behavior — build_model is invoked with the exact same trailing defaults
// ("", false, nullptr, nullptr, "", WeightKind::None) it always was.
// ts / row_mask (BUG-1484): the --xtset/--tsset metadata and plan_keepif's `if`
// mask, threaded to build_model exactly as cmd_feols passes them, so an L./F./D./S.
// token materializes against the FULL series before the `if` applies. nullptr (the
// default) keeps every existing caller byte-identical.
GlmInputs load_glm(const Dataset& ds, const char* cmd, const std::string& y,
                   const std::vector<std::string>& xs,
                   const std::vector<std::string>& cluster_names = {},
                   const std::string& weight_var = "",
                   cecon::WeightKind weight_kind = cecon::WeightKind::None,
                   const std::string& offset_var = "",
                   const cecon::TsMeta* ts = nullptr,
                   const std::vector<char>* row_mask = nullptr);

// Stata `noconstant` on a load_glm design: drops the trailing intercept column
// and its name; throws bad_input when no regressor would remain. Callers span
// cmds_mle.cpp (logit/probit fits) and cmds_postest.cpp (postest/margins refit).
void drop_glm_intercept(GlmInputs& g, const char* cmd);

// Callers span cmds_mle.cpp (logit/probit) and cmds_postest.cpp (margins).
bool check_binary(const std::vector<double>& y, const char* cmd, const std::string& nm);

// Shared core for feols / run. Callers span cmds_linear.cpp (cmd_feols) and
// cmds_data.cpp (cmd_run / grid_run_group) — needs external linkage.
// keep_nested_fe_dof (BUG-568) is feols_fit's switch of the same name: Stata
// areg's clustered dof, which charges an FE nested in the cluster (cmd_feols
// `--keep-nested-fe-dof`). Default false = reghdfe/xtreg,fe.
// multiway_subset_factors (FU-365) is feols_fit's switch of the same name: Stata
// regress/areg's built-in multiway cluster convention (cmd_feols
// `--multiway-subset-factors`). Default false = reghdfe's Gmin convention. Under it
// the reply gains `vcov_rank` (e(rank)) and `kcluster` (e(kcluster) / e(cluster#)).
// regress_exact_fit (cmd_feols `--regress-exact-fit`): Stata `regress`'s exact-fit
// rule (feols_fit). When it fires the reply's tstat/pvalue/ci_lo/ci_hi are null for
// every coefficient with se 0 (Stata's r(table) prints `.`; StataBE `regress yx x`
// on an exact line). Default false.
// collinear_forward (BUG-1067, cmd_feols `--collinear-forward`): ModelData's switch
// of the same name -- the earlier term of a collinear set is kept (reghdfe, areg,
// xtreg,fe) instead of the norm-pivoted basis (regress). Default false.
// fe_nested_in_clusters (FU-401, cmd_feols `--fe-nested-in-clusters`): xtreg,fe's
// nesting rule -- every cluster variable must be constant within each level
// of the first absorbed effect on the estimation sample, else EngineError bad_input
// with detail "panels_not_nested" (Stata r(498)). Default false: no check.
json run_feols(const std::string& data, const std::string& y,
               const std::vector<std::string>& xs, const std::vector<std::string>& fes,
               const std::string& vcov_type, const std::vector<std::string>& clusters,
               bool drop_singleton = true, const cecon::TsMeta* ts = nullptr,
               const std::vector<char>* if_mask = nullptr,
               bool no_intercept = false, double level = 95.0, bool level_set = false,
               const std::string& weight_var = "",
               cecon::WeightKind weight_kind = cecon::WeightKind::None,
               EsampleRows* esample = nullptr,
               bool keep_nested_fe_dof = false, bool emit_v = false,
               bool multiway_subset_factors = false, bool fe_nested_in_clusters = false,
               bool regress_exact_fit = false, bool collinear_forward = false);

// P7 — `svy: regress`. Same WLS point estimates as the pweighted feols path; what
// differs is the variance and the reference distribution: a stratified linearized
// (Taylor) meat over PSU totals, design dof = #PSU − #strata, t(dof) inference,
// and an adjusted Wald F instead of the usual F(k, N−k). Kept as its own entry
// point rather than another flag inside run_feols so the frozen non-svy output
// stays byte-identical.
json run_feols_svy(const std::string& data, const std::string& y,
                   const std::vector<std::string>& xs, const std::string& weight_var,
                   const std::string& psu_var, const std::string& strata_var,
                   const cecon::TsMeta* ts = nullptr,
                   const std::vector<char>* if_mask = nullptr, double level = 95.0,
                   EsampleRows* esample = nullptr);

// Shared core for iv / run. Callers span cmds_linear.cpp (cmd_iv) and
// cmds_data.cpp (cmd_run / grid_run_group).
// multiway_subset_factors (FU-387, cmd_iv `--multiway-subset-factors`, with --small
// only): Stata ivregress's `small` multiway variance, the built-in convention's
// per-combination factors. Under any
// multiway VCE the reply gains `vcov_rank` and `kcluster`.
json run_iv(const std::string& data, const std::string& y,
            const std::vector<std::string>& exog, const std::vector<std::string>& endog,
            const std::vector<std::string>& instruments,
            const std::vector<std::string>& fes, const std::string& vcov_type,
            const std::vector<std::string>& clusters, bool gmm = false,
            const cecon::TsMeta* ts = nullptr, bool drop_singleton = true,
            const std::vector<char>* if_mask = nullptr, bool small = false,
            bool savefirst = false, const std::string& savefprefix = "",
            EsampleRows* esample = nullptr, bool emit_v = false,
            bool multiway_subset_factors = false, bool no_intercept = false,
            const std::string& weight_var = "",
            cecon::WeightKind weight_kind = cecon::WeightKind::None,
            bool multiway_no_psd_fix = false, bool dofminus_fe = false);

// Multi-outcome 2SLS batch. Callers span cmds_linear.cpp (cmd_iv) and
// cmds_data.cpp (grid_run_group).
json run_iv_multi(const std::string& data, const std::vector<std::string>& ys,
                  const std::vector<std::string>& exog, const std::vector<std::string>& endog,
                  const std::vector<std::string>& instruments,
                  const std::vector<std::string>& fes, const std::string& vcov_type,
                  const std::vector<std::string>& clusters, bool gmm = false,
                  const cecon::TsMeta* ts = nullptr, bool drop_singleton = true,
                  const std::vector<char>* if_mask = nullptr, bool small = false,
                  bool savefirst = false, const std::string& savefprefix = "",
                  EsampleRows* esample = nullptr, const std::string& weight_var = "",
                  cecon::WeightKind weight_kind = cecon::WeightKind::None);

// Multi-outcome FE-OLS batch. Callers span cmds_linear.cpp (cmd_feols) and
// cmds_data.cpp (grid_run_group).
json run_feols_multi(const std::string& data, const std::vector<std::string>& ys,
                     const std::vector<std::string>& xs, const std::vector<std::string>& fes,
                     const std::string& vcov_type, const std::vector<std::string>& clusters,
                     bool drop_singleton, const cecon::TsMeta* ts = nullptr,
                     const std::vector<char>* if_mask = nullptr,
                     bool no_intercept = false, double level = 95.0, bool level_set = false,
                     EsampleRows* esample = nullptr, bool collinear_forward = false);

// Shared core for fepois / run. Callers span cmds_linear.cpp (cmd_fepois) and
// cmds_data.cpp (cmd_run / grid_run_group).
// weight_var/weight_kind (P5, pweight): threaded straight through to
// build_model, same as run_feols's trailing weight params. Defaulted so every
// existing caller (none of which pass them) gets byte-identical behavior.
json run_fepois(const std::string& data, const std::string& y,
                const std::vector<std::string>& xs, const std::vector<std::string>& fes,
                const std::string& offset, const std::string& vcov_type,
                const std::vector<std::string>& clusters, bool drop_zero_fe = true,
                bool scale_x2 = false, const std::string& weight_var = "",
                cecon::WeightKind weight_kind = cecon::WeightKind::None);

// Multi-outcome FE-Poisson batch. Callers span cmds_linear.cpp (cmd_fepois) and
// cmds_data.cpp (grid_run_group).
json run_fepois_multi(const std::string& data, const std::vector<std::string>& ys,
                      const std::vector<std::string>& xs, const std::vector<std::string>& fes,
                      const std::string& offset, const std::string& vcov_type,
                      const std::vector<std::string>& clusters, bool drop_zero_fe, bool scale_x2);

}  // namespace cli
}  // namespace cecon
