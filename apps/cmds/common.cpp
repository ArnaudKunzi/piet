// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
#include "common.h"

// Estimator headers needed only by the definitions in this TU (run_feols/run_iv/
// run_iv_multi/run_feols_multi/run_fepois/run_fepois_multi and the cragg_donald_F
// call inside run_iv/run_iv_multi). common.h itself stays to the curated
// STL + core/data/io include set per the split plan; these are additional.
#include "core/eng_counters.h"  // W0: provenance-hash + write-plane ledger (P2PERF §W0)
#include "core/verb_diag.h"  // COBRA_VERB_DIAG: maybe_keepif scratch-write phase timing (measurement-only)
#include "io/dta_reader.h"  // read_dta — open_using_frame's .dta branch (WS-D2)
#include <algorithm>  // std::min/std::max (fe_group_sizes)
#include <cstring>  // strlen — open_using_frame's extension match
#include "estimators/feols.h"
#include "estimators/fepois.h"
#include "estimators/iv2sls.h"
#include "estimators/iv_weakid.h"
#include "vcov/svy_vcov.h"
#include "vcov/vcov.h"

namespace cecon {
namespace cli {

// --- provenance plumbing (set in main, consumed by emit) -----------------------
std::vector<std::string> g_argv;     // full invocation (cobra + args)
std::string g_data_path;             // value of --data (original, pre-keepif)
bool g_provenance = true;            // attach provenance{} to estimator JSON

// FNV-1a 64-bit over the raw file bytes — a cheap content fingerprint so a run's
// provenance pins the EXACT input (irrecoverable after the fact). Cached by path.
// Hashed over a read-only mmap instead of a buffered ifstream sweep: the estimator
// already has these pages mapped (Dataset), so this adds no disk I/O and skips a
// second userspace copy of the whole file. Same bytes in the same fold order ⇒ the
// fnv1a64 value is bit-identical to the historical ifstream path. (A fresh mapping
// rather than the Dataset's own span because the runners' Datasets are function-local
// and already unmapped by the time the caller emits provenance.)
// The hash_file_hex path→hash memo — function-local static hoisted to TU scope so
// hash_cache_clear() (below) can drop it. One-shot lifecycle is unchanged: nothing
// on the one-shot path ever calls the clear, so the cache still lives per-process.
static std::unordered_map<std::string, std::string> g_hash_cache;

// serve-mode per-request reset (contract_A §25): a long-lived process must re-hash a
// path that may have been rewritten between requests, or provenance.fnv1a64 goes stale.
void hash_cache_clear() { g_hash_cache.clear(); }

// serve resident-run mode (§26): seed the memo with the root's use-time hash so
// per-run estimator provenance doesn't re-scan the (possibly large) root file.
void hash_cache_put(const std::string& path, const std::string& hex) { g_hash_cache[path] = hex; }

// The FNV-1a-64 kernel, ONE implementation (A11 — it used to live only inside
// hash_file_hex; ENG-ADOPTION-AVOIDANCE needed the same bytes→hex mapping for an
// already-mapped region, and two copies of a golden-pinned hash is exactly the
// divergence class this codebase writes rules about). Byte-for-byte the loop and
// the %016llx formatting hash_file_hex has always used, so every pinned
// provenance.fnv1a64 string is unchanged by construction.
std::string hash_bytes_hex(const void* data, size_t len) {
    uint64_t h = 1469598103934665603ULL;
    const unsigned char* p = static_cast<const unsigned char*>(data);
    for (size_t i = 0; i < len; ++i) {
        h ^= p[i];
        h *= 1099511628211ULL;
    }
    char hx[24];
    std::snprintf(hx, sizeof(hx), "%016llx", static_cast<unsigned long long>(h));
    return hx;
}

std::string hash_file_hex(const std::string& path) {
    auto it = g_hash_cache.find(path);
    if (it != g_hash_cache.end()) {
        engctr::addHashFileMemoHit();
        return it->second;
    }
    // ENG-ADOPTION-AVOIDANCE (§26 bridge): when `path` IS the resident session's
    // root, the engine ALREADY holds that file's bytes mapped — the adoption
    // mmap. Hash those instead of re-opening and re-reading the file: identical
    // value (Dataset::open maps the whole file at offset 0), no second pass, and
    // strictly more faithful — it attests the bytes this session actually
    // adopted rather than whatever is at `path` at provenance time. Empty
    // outside a serve session, for a view-backed root, or for any other path,
    // and then the file path below runs verbatim.
    if (std::string mapped = serve_root_hash(path); !mapped.empty()) {
        g_hash_cache[path] = mapped;
        engctr::addHashFileMemoHit();  // answered off the adoption mmap, no file pass
        return mapped;
    }
    // Past this point the whole file really is read and FNV'd — the one site the
    // ~263 ms/adoption measurement (cobra-frameload-materialization-tax) belongs to.
    engctr::addHashFileCall();
    std::string out = "unavailable";
    int fd = native_open(path, O_RDONLY);
    if (fd >= 0) {
        struct stat st;
        if (::fstat(fd, &st) == 0 && S_ISREG(st.st_mode)) {
            bool ok = true;
            std::string hex;
            if (st.st_size > 0) {
                void* m = ::mmap(nullptr, static_cast<size_t>(st.st_size), PROT_READ,
                                 MAP_PRIVATE, fd, 0);
                if (m != MAP_FAILED) {
                    hex = hash_bytes_hex(m, static_cast<size_t>(st.st_size));
                    ::munmap(m, static_cast<size_t>(st.st_size));
                } else {
                    ok = false;
                }
            } else {
                hex = hash_bytes_hex(nullptr, 0);  // empty file: the bare FNV offset basis
            }
            if (ok) out = hex;
        }
        ::close(fd);
    }
    g_hash_cache[path] = out;
    return out;
}

// ---- contract_A §26: primary-dataset acquisition + mutator commit ----------------
// ENG-SETCOL-DEDUP holder — see acquire_data / frame_from_acquired.
static std::shared_ptr<Dataset> g_acquired_sp;  // NOLINT: single-threaded serve/one-shot

const Dataset& acquire_data(const std::string& data) {
    // Resident-run serve mode with no explicit --data (data resolved to empty): hand
    // back the session's cached zero-copy view of the resident frame — no re-read.
    if (serve_resident_active() && data.empty()) return serve_resident_view();
    // One-shot / stateless §25 / explicit path: open exactly as before. The result is
    // parked in a process-static holder whose lifetime spans the reference's use — the
    // offline law is single-threaded and every command does exactly ONE primary open,
    // so a later acquire_data (next command / grid iteration) safely replaces it. Same
    // error text as the historical `Dataset::open(data)` (throws on "" or a bad path).
    // ENG-SETCOL-DEDUP: the holder is a shared_ptr so a command can build its Frame
    // over THIS mmap (frame_from_acquired, below) instead of re-opening the file.
    // Lifetime is strictly safer than the old by-value static: a Frame that took a
    // share keeps the Dataset alive even after a later acquire_data replaces the
    // holder, which the by-value version could not promise.
    g_acquired_sp = std::make_shared<Dataset>(Dataset::open(data));
    return *g_acquired_sp;
}

std::shared_ptr<const Dataset> acquired_handle() { return g_acquired_sp; }

Frame frame_from_acquired(const Dataset& ds) {
    // Identity, not a guess: only when `ds` IS the handle we are holding do we hand the
    // Frame a share of it. Anything else (the resident §26 view, a `using` file opened
    // separately, any future caller) falls through to Frame::from_dataset unchanged —
    // so this can never shorten a lifetime it does not own.
    if (g_acquired_sp && &ds == g_acquired_sp.get()) return Frame::from_dataset_shared(g_acquired_sp);
    return Frame::from_dataset(ds);
}

// WS-D2 / S06 ARC-MRG. The SECOND input of a two-file verb (merge/append/joinby/
// cross's `using`), acquired with exactly ONE physical open.
//
// The old idiom at those four call sites was
//     Dataset uds = Dataset::open(usingp);
//     Frame usingf = frame_from_acquired(uds);
// and it opened the file TWICE. `frame_from_acquired` hands out a shared Frame only
// for the ONE dataset acquire_data is holding; `uds` is a local, so it fell through
// to `Frame::from_dataset`, which — given a bare `const Dataset&` whose lifetime it
// cannot extend — re-opens the path to own an independent mmap
// (frame.cpp `from_dataset`, the `owns_mmap()` branch). Measured, not deduced: an
// open-site backtrace on `cobra merge --data mm.hbin --using lk.hbin` showed
// `lk.hbin` mapped twice, once from cmd_merge and once from
// `Frame::from_dataset`, for merge_file_opens = 3. That is the "merge opens its
// files three times" the P2-PERF close named.
//
// Holding the Dataset in a shared_ptr and going through `from_dataset_shared`
// removes the second open outright: the Frame co-owns the ONE mapping, which is
// also strictly safer than the old local `uds` (that only survived because
// from_dataset happened to re-open). Same idiom acquire_data/frame_from_acquired
// already use for the master — this is the missing counterpart for the `using`
// side, not a new mechanism. The .dta / .csv branches are folded in so all four
// verbs share one spelling of "what may be a `using` file".
Frame open_using_frame(const std::string& path) {
    std::string lower = path;
    for (auto& c : lower) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    auto ends_with = [&lower](const char* suf) {
        const size_t n = std::strlen(suf);
        return lower.size() >= n && lower.compare(lower.size() - n, n, suf) == 0;
    };
    if (ends_with(".dta")) return read_dta(path).frame;
    if (ends_with(".csv")) return Frame::from_csv(path, ',', {});
    if (ends_with(".tsv")) return Frame::from_csv(path, '\t', {});
    return Frame::from_dataset_shared(std::make_shared<Dataset>(Dataset::open(path)));
}

bool commit_frame(Frame&& fr, const std::string& out) {
    if (serve_resident_active()) { serve_stage_pending(std::move(fr)); return true; }
    if (!out.empty()) fr.write_hbin(out);
    return false;
}

std::vector<std::string> split_csv(const std::string& s) {
    std::vector<std::string> out;
    std::string cur;
    std::stringstream ss(s);
    while (std::getline(ss, cur, ',')) {
        // trim spaces
        size_t a = cur.find_first_not_of(" \t");
        size_t b = cur.find_last_not_of(" \t");
        if (a != std::string::npos) out.push_back(cur.substr(a, b - a + 1));
    }
    return out;
}

// ---- E6: unknown-flag rejection net --------------------------------------------
// opt()/has()/opt_all() are the sanctioned way a cmd_XXX() reads its flags out of
// `a` (the per-invocation argv slice; a[0] is the subcommand name). g_argv_consumed
// records, per invocation, WHICH INDICES of `a` a real match actually touched —
// both the flag token and (for opt/opt_all) its value token, so a legitimate
// option's VALUE is never itself mistaken for a leftover unrecognized flag even
// when that value happens to start with "--". main() resets this before dispatch
// and, after the command returns successfully, rejects any "--xxx"-shaped token
// nothing ever matched (see check_unconsumed_flags) — previously such a token was
// silently ignored. Reused: EngineError("bad_input", ...), the SAME code/exit-status
// path every other CLI-argument error already uses (no contract_A change).
std::vector<char> g_argv_consumed;

void mark_consumed(const std::vector<std::string>& a, size_t i) {
    if (g_argv_consumed.size() != a.size()) g_argv_consumed.assign(a.size(), 0);
    if (i < g_argv_consumed.size()) g_argv_consumed[i] = 1;
}

std::string opt(const std::vector<std::string>& a, const std::string& flag,
                const std::string& def) {
    // Scan the WHOLE array (not just up to the first hit) so a flag the caller
    // happens to pass more than once still has every occurrence marked consumed —
    // otherwise a redundant repeat (e.g. a caller-side "--data ... --data ...")
    // would leave its second copy unmarked and E6 would wrongly reject it as
    // unrecognized. Return value is unchanged: first occurrence still wins.
    std::string result = def;
    bool found = false;
    for (size_t i = 0; i + 1 < a.size(); ++i)
        if (a[i] == flag) {
            mark_consumed(a, i);
            mark_consumed(a, i + 1);
            if (!found) { result = a[i + 1]; found = true; }
        }
    return result;
}
bool has(const std::vector<std::string>& a, const std::string& flag) {
    bool found = false;  // scan the whole array — see opt()'s comment above
    for (size_t i = 0; i < a.size(); ++i)
        if (a[i] == flag) { mark_consumed(a, i); found = true; }
    return found;
}

// All values for a repeatable flag (e.g. multiple --gen).
std::vector<std::string> opt_all(const std::vector<std::string>& a, const std::string& flag) {
    std::vector<std::string> out;
    for (size_t i = 0; i + 1 < a.size(); ++i)
        if (a[i] == flag) { mark_consumed(a, i); mark_consumed(a, i + 1); out.push_back(a[i + 1]); }
    return out;
}

// After a command has run to completion, reject any argv token shaped like a flag
// ("--xxx") that no opt()/has()/opt_all() call ever matched — reusing the SAME
// bad_input mechanism every other CLI-argument error already goes through. Scoped
// to the caller's discretion (see main(): only invoked when the command itself
// reported success) so an EARLIER usage/argument error — already loud, a
// fprintf usage blurb plus a nonzero exit — is not ALSO piled onto with a second,
// possibly-misleading error about flags the early return never got to look up.
void check_unconsumed_flags(const std::vector<std::string>& a) {
    for (size_t i = 1; i < a.size(); ++i) {  // skip a[0], the subcommand name itself
        const std::string& tok = a[i];
        bool looks_like_flag = tok.size() > 2 && tok[0] == '-' && tok[1] == '-';
        bool was_consumed = i < g_argv_consumed.size() && g_argv_consumed[i];
        if (looks_like_flag && !was_consumed)
            throw EngineError("bad_input",
                "unrecognized option '" + tok + "' for '" + a[0] + "'", tok);
    }
    check_options_table_drift(a);
}

// Drift detector (B.0.3): when COBRA_OPTIONS_STRICT is set (ctest / run_eval under
// the drift gate), assert that every flag a command actually CONSUMED is declared in
// options_table() — so `cobra options --json` can never silently under-report a real
// flag to PIET. Off by default (zero cost in the shipped path).
//
// BUG-182 (2026-08-06): this is deliberately SEPARATE from the unrecognized-option net
// above, and callable on its own, because the two halves have opposite reachability
// needs. The unrecognized-option net must stay success-path-only — piling "you passed
// an unknown flag" on top of an error the command already reported is misleading, since
// an early return may simply never have got as far as reading the later flags. The DRIFT
// half is the opposite: the flags that most need it are the **refuse-by-name** flags —
// the ones a command consumes SOLELY so it can reject them with a named message instead
// of ignoring them silently (`--fuzzy`/`--covs`/`--cluster` on the three RD commands,
// `--controls`/`--absorb` on binscatter, `--weight`/`--cluster` on did_imputation,
// `--x`/`--method` on sdid). Every one of those paths THROWS, so under the old
// success-path-only wiring the check unwound past and the whole honesty mechanism's
// declarations were verifiable by code inspection only. main() now also calls this from
// the throwing path and dispatch_guard() from the rc != 0 path.
//
// It is safe to call twice for one invocation (idempotent: a pure read of
// g_argv_consumed + options_table()), and safe to call while an exception is in flight
// — main()'s call site is an ordinary catch block, not a destructor.
void check_options_table_drift(const std::vector<std::string>& a) {
    static const bool strict = std::getenv("COBRA_OPTIONS_STRICT") != nullptr;
    if (!strict || a.empty()) return;
    for (size_t i = 1; i < a.size(); ++i) {
        const std::string& tok = a[i];
        bool looks_like_flag = tok.size() > 2 && tok[0] == '-' && tok[1] == '-';
        bool was_consumed = i < g_argv_consumed.size() && g_argv_consumed[i];
        if (!looks_like_flag || !was_consumed) continue;
        const auto& T = options_table();
        auto it = T.find(a[0]);
        // P7: a command with NO row at all used to be silently exempt from the
        // whole drift check — the one shape the detector could not see. That is
        // backwards: an undeclared command under-reports EVERY one of its flags
        // to PIET, not just one. Missing rows are now the same error as a missing
        // flag. The check is self-limiting: it can only fire for a command that
        // actually consumed a `--flag`, so genuinely flag-less verbs (serve,
        // shutdown, --version) still need no row. Found while adding the P7 rows —
        // `mean`/`proportion` had shipped since P3 with no row and no gate.
        if (it == T.end())
            throw EngineError("internal",
                "options-table drift: '" + std::string(a[0]) +
                "' consumed '" + tok + "' but has NO entry in options_table() "
                "(apps/cmds/options_table.cpp)", tok);
        bool listed = false;
        for (const auto& s : it->second)
            if (tok == s.flag) { listed = true; break; }
        if (!listed)
            throw EngineError("internal",
                "options-table drift: '" + tok + "' consumed by '" + a[0] +
                "' but not declared in options_table() (apps/cmds/options_table.cpp)", tok);
    }
}

// Parse time-series metadata flags for the L./F./D./S. varlist operators. PIET (and
// any frontend) tracks the user's tsset/xtset and passes it per call, since .hbin
// does not persist it. `--tsset T` = single time series; `--xtset ID,T` = panel
// (id,time). `--tsdelta D` overrides the step (default 1). Returns an empty timevar
// (TS ops then error if used) when neither flag is present.
cecon::TsMeta parse_tsmeta(const std::vector<std::string>& a) {
    cecon::TsMeta m;
    std::string xt = opt(a, "--xtset");
    if (!xt.empty()) {
        auto p = split_csv(xt);
        if (p.size() == 2) { m.panelvar = p[0]; m.timevar = p[1]; }
        else if (p.size() == 1) { m.timevar = p[0]; }
    }
    std::string ts = opt(a, "--tsset");
    if (!ts.empty()) m.timevar = ts;  // single time series (no panel)
    std::string d = opt(a, "--tsdelta");
    if (!d.empty()) m.delta = std::stod(d);
    return m;
}

// --format tidy: add a uniform coeftable [{name,b,se,t,p,ci_lo,ci_hi}] alongside
// the native fields, so a driver can parse ONE schema regardless of whether the
// estimator emits a coef{} dict (ols/feols/iv/fepois) or a coefs[] array
// (xt*/nbreg/qreg/nlcom/lincom). Purely additive (existing keys are unchanged).
bool g_format_tidy = false;

json tidy_coeftable(const json& o) {
    json table = json::array();
    auto numn = [](const json& j) -> json { return j.is_number() ? j : json(nullptr); };
    auto pick = [&](const json& c, std::initializer_list<const char*> keys) -> json {
        for (const char* k : keys) if (c.contains(k)) return numn(c[k]);
        return json(nullptr);
    };
    if (o.contains("coef") && o["coef"].is_object()) {
        const json& coef = o["coef"];
        auto field = [&](const char* k, const std::string& nm) -> json {
            return (o.contains(k) && o[k].contains(nm)) ? numn(o[k][nm]) : json(nullptr);
        };
        for (auto it = coef.begin(); it != coef.end(); ++it) {
            const std::string& nm = it.key();
            table.push_back({{"name", nm}, {"b", numn(it.value())}, {"se", field("se", nm)},
                             {"t", field("tstat", nm)}, {"p", field("pvalue", nm)},
                             {"ci_lo", field("ci_lo", nm)}, {"ci_hi", field("ci_hi", nm)}});
        }
    } else if (o.contains("coefs") && o["coefs"].is_array()) {
        for (const auto& c : o["coefs"]) {
            json nm = c.contains("name") ? c["name"] : (c.contains("exp") ? c["exp"] : json(nullptr));
            table.push_back({{"name", nm}, {"b", pick(c, {"beta", "b"})}, {"se", pick(c, {"se"})},
                             {"t", pick(c, {"t", "z"})}, {"p", pick(c, {"p"})},
                             {"ci_lo", pick(c, {"ci_lo"})}, {"ci_hi", pick(c, {"ci_hi"})}});
        }
    }
    return table;
}

// Attach tidy coeftables in place, descending into multi-outcome / multi-equation
// containers (feols_multi, iv2sls_multi, reg3).
void apply_tidy(json& o) {
    for (const char* key : {"outcomes", "equations"}) {
        if (o.contains(key) && o[key].is_array()) {
            for (auto& sub : o[key]) { json t = tidy_coeftable(sub); if (!t.empty()) sub["coeftable"] = t; }
            return;
        }
    }
    json t = tidy_coeftable(o);
    if (!t.empty()) o["coeftable"] = t;
}

// provenance{} — what the engine alone can attest to: the exact build, the exact
// invocation, the exact input (path + content hash), and the sample/convergence
// accounting at read time. Pulled from globals + whatever the estimator already
// emitted (n_obs, drops, convergence). Purely additive.
json build_provenance(const json& out) {
    json p;
    p["cobra_version"] = COBRA_VERSION;
    if (!g_argv.empty()) p["argv"] = g_argv;
    if (!g_data_path.empty())
        p["data"] = {{"path", g_data_path}, {"fnv1a64", hash_file_hex(g_data_path)}};
    for (const char* k : {"n_obs", "n_dropped", "n_dropped_singletons",
                          "n_dropped_separation", "converged", "demean_converged"})
        if (out.contains(k)) p[k] = out[k];
    if (out.contains("diagnostics") && out["diagnostics"].is_object() &&
        out["diagnostics"].contains("demean_converged"))
        p["demean_converged"] = out["diagnostics"]["demean_converged"];
    if (out.contains("omitted")) p["omitted"] = out["omitted"];
    return p;
}

void emit(const json& out, const std::string& outpath) {
    json mut;
    const json* p = &out;
    bool prov = g_provenance && out.is_object() && out.contains("estimator");
    if (g_format_tidy || prov) { mut = out; p = &mut; }
    if (g_format_tidy) apply_tidy(mut);
    if (prov) mut["provenance"] = build_provenance(out);
    // Compact, streamed directly into the sink (nlohmann operator<< with stream
    // width 0 == dump()): pretty dump(2) materialized a second full copy of the
    // output and cost ~1.6x the bytes on large results (csdid). Every known
    // consumer parses this JSON (eval harness, ctest validators, bench gate,
    // piet cobra_runner) — none is byte/format sensitive.
    if (!outpath.empty()) {
        // binary so --out matches stdout byte-for-byte on Windows too: stdout is
        // already put in binary mode at startup, and a text-mode sink here would
        // silently CRLF-translate the same bytes.
        std::ofstream f(to_native_path(outpath), std::ios::binary);
        f << *p << "\n";
        std::fprintf(stderr, "wrote %s\n", outpath.c_str());
    } else {
        std::cout << *p << "\n";
    }
}

double tstat(double b, double se) { return se > 0 ? b / se : 0.0; }

// Lower-tail F CDF P(F_{d1,d2} <= f) — betai directly so both tails stay accurate.
double f_cdf(double f, double d1, double d2) {
    if (f <= 0) return 0.0;
    return betai(d1 / 2.0, d2 / 2.0, d1 * f / (d1 * f + d2));
}
// Student-t CDF P(T_dof <= t).
double t_cdf(double t, double dof) {
    double x = dof / (dof + t * t);
    double tail = 0.5 * betai(dof / 2.0, 0.5, x);  // = P(T > |t|)
    return t >= 0 ? 1.0 - tail : tail;
}
// Non-missing values of a numeric column.
std::vector<double> nonmissing(const double* x, int64_t n) {
    std::vector<double> v;
    v.reserve(static_cast<size_t>(n));
    for (int64_t i = 0; i < n; ++i) if (!std::isnan(x[i])) v.push_back(x[i]);
    return v;
}
// mean + sample (n-1) sd of a vector.
void mean_sd(const std::vector<double>& v, double& mean, double& sd) {
    double s = 0; for (double x : v) s += x; mean = v.empty() ? 0 : s / v.size();
    double ss = 0; for (double x : v) ss += (x - mean) * (x - mean);
    sd = v.size() > 1 ? std::sqrt(ss / (v.size() - 1)) : 0.0;
}

// Attach two-sided t p-values + 95% CI (dicts keyed by coef name) to an estimator
// JSON. dof = residual dof for iid/HC1, or (#clusters − 1) for cluster/twoway
// (Stata's t(G−1) convention).
void add_inference(json& out, const std::vector<std::string>& names,
                   const std::vector<double>& beta, const std::vector<double>& se,
                   double dof, bool asymptotic, double level) {
    json pval = json::object(), lo = json::object(), hi = json::object();
    // level==95 must reproduce the historical literal 0.05 EXACTLY: 1.0-95.0/100.0
    // differs from 0.05 by one ulp, which moves ci_lo/ci_hi by a last-digit byte
    // (caught by the PIET freeze harness, 0.25.0 → 0.25.1).
    double alpha = (level == 95.0) ? 0.05 : 1.0 - level / 100.0;
    // asymptotic (z, Stata `ivregress` default) uses the fixed 95% normal quantile
    // — iv is the only asymptotic caller and doesn't expose --level. The t branch
    // takes its CI quantile from `level` (feols --level; default 95 = old bytes).
    double tc = asymptotic ? 1.959963984540054
                           : ((dof > 0) ? t_critical(alpha, dof) : 0.0);
    for (size_t j = 0; j < names.size(); ++j) {
        double b = beta[j], s = se[j];
        double t = (s > 0) ? b / s : 0.0;
        pval[names[j]] = asymptotic
            ? (s > 0 ? z_twosided_pvalue(t) : 1.0)
            : ((s > 0 && dof > 0) ? t_twosided_pvalue(t, dof) : 1.0);
        lo[names[j]] = b - tc * s;
        hi[names[j]] = b + tc * s;
    }
    out["pvalue"] = pval;
    out["ci_lo"] = lo;
    out["ci_hi"] = hi;
    out["t_dof"] = dof;
}

// Append Stata-style omitted (collinear) terms to a coef object. Each gets coef 0
// and parallel entries in whatever inference maps already exist (se/tstat 0,
// pvalue 1, CI 0), and the names are listed under a top-level "omitted" array.
// No-op (and no "omitted" key added) when nothing was dropped — full-rank output
// is byte-identical to before. Call AFTER add_inference so the parallel maps exist.
//
// `full_names` (BUG-112(6)): the pre-collinearity-drop design-matrix column
// order, if the caller threaded it through. Every existing inference map is
// keyed by ordered_json insertion order, and up to here that order is
// [survivors][omitted] (tail-appended) rather than Stata's natural position.
// When full_names is non-empty AND accounts for every name in `omitted`
// (std::find sanity check — a caller/estimator mismatch would otherwise
// silently drop an entry), rebuild each map wholesale in full_names order.
// Empty full_names (caller didn't thread it through) keeps the old tail-append
// shape — this is an additive fallback, not a behavior change for such callers.
// BUG-112(8). Absent-key-when-empty is the whole point: see common.h.
void add_warnings(json& out, const std::vector<std::string>& warnings) {
    if (warnings.empty()) return;
    out["warnings"] = warnings;
}

void add_e_v(json& out, const std::vector<std::string>& names, const std::vector<double>& V,
             const std::vector<size_t>* order) {
    const size_t k = names.size();
    if (V.size() != k * k)
        throw EngineError("internal",
            "--emit-v: fitted covariance has " + std::to_string(V.size()) + " entries for " +
            std::to_string(k) + " coefficients");
    json nm = json::array(), rows = json::array();
    for (size_t i = 0; i < k; ++i) {
        const size_t ri = order ? (*order)[i] : i;
        nm.push_back(names[ri]);
        json row = json::array();
        for (size_t j = 0; j < k; ++j) {
            const size_t cj = order ? (*order)[j] : j;
            row.push_back(V[ri + cj * k]);
        }
        rows.push_back(std::move(row));
    }
    out["e_V"] = {{"names", std::move(nm)}, {"matrix", std::move(rows)}};
}

void add_omitted(json& out, const std::vector<std::string>& omitted_in,
                 const std::vector<std::string>& full_names) {
    // BUG-1067: a continuous regressor repeated in the design (`x x`, or the c.x of
    // `c.x##i.t c.x##i.s`) is two columns under ONE name; the collinearity pass
    // keeps the first and omits the second (Stata: `x o.x`). The omitted copy must
    // not overwrite the kept coefficient with 0 under the shared key, so a name a
    // survivor already holds is left to the survivor.
    std::vector<std::string> omitted;
    for (const auto& nm : omitted_in)
        if (!(out.contains("coef") && out["coef"].contains(nm))) omitted.push_back(nm);
    if (omitted.empty()) return;
    for (const auto& nm : omitted) {
        if (out.contains("coef")) out["coef"][nm] = 0.0;
        if (out.contains("se")) out["se"][nm] = 0.0;
        if (out.contains("tstat")) out["tstat"][nm] = 0.0;
        if (out.contains("pvalue")) out["pvalue"][nm] = 1.0;
        if (out.contains("ci_lo")) out["ci_lo"][nm] = 0.0;
        if (out.contains("ci_hi")) out["ci_hi"][nm] = 0.0;
    }
    out["omitted"] = omitted;

    if (full_names.empty()) return;
    for (const auto& nm : omitted) {
        if (std::find(full_names.begin(), full_names.end(), nm) == full_names.end()) return;
    }
    static const char* kMaps[] = {"coef", "se", "tstat", "pvalue", "ci_lo", "ci_hi"};
    for (const char* key : kMaps) {
        if (!out.contains(key)) continue;
        json reordered = json::object();
        for (const auto& nm : full_names) {
            auto it = out[key].find(nm);
            if (it != out[key].end()) reordered[nm] = *it;
        }
        out[key] = std::move(reordered);
    }
}

// Emit the model-level overall-F and its p-value (Prob>F). f is the Wald F over the
// slopes (NaN when undefined: no slopes / non-PD slope vcov); df1=df_m, df2=df_r (the
// Stata e(df_r): G-1 under cluster, else residual dof). Emits JSON null for both when
// undefined (JSON has no NaN). Used by the OLS family (feols single + multi).
void add_model_f(json& out, double f, int df1, double df2) {
    // null (= Stata MISSING) when F is undefined: NaN, no slopes (df1<=0), or a degenerate
    // F reference dof (df2<=0, e.g. a single cluster G=1 → G-1=0).
    if (std::isnan(f) || df1 <= 0 || df2 <= 0) {
        out["F"] = nullptr;
        out["p_F"] = nullptr;
        return;
    }
    out["F"] = f;
    out["p_F"] = f_pvalue(f, static_cast<double>(df1), df2);
}

// Emit MLE binary-model fit scalars (Stata logit/probit header): the fitted log-likelihood
// `ll`, the intercept-only (null) log-likelihood `ll_0`, McFadden pseudo-R² `r2_pseudo`, and
// the LR chi² test `chi2` (df `df_chi2`, p-value `p_chi2`) that all slopes are zero. `p` =
// fitted probabilities, `y` in {0,1} (both length n); `df` = number of slope coefficients
// (excludes the intercept). For BOTH logit and probit the intercept-only MLE fits p̂ = ȳ, so
// ll_0 = n[ȳ·ln ȳ + (1−ȳ)·ln(1−ȳ)]. A degenerate outcome (all 0 / all 1) → null ll undefined.
void add_mle_fit(json& out, const std::vector<double>& p, const std::vector<double>& y,
                 int64_t n, int df) {
    const double eps = 1e-12;
    double ll = 0.0, sy = 0.0;
    for (int64_t i = 0; i < n; ++i) {
        double pi = std::min(1.0 - eps, std::max(eps, p[static_cast<size_t>(i)]));
        double yi = y[static_cast<size_t>(i)];
        ll += yi * std::log(pi) + (1.0 - yi) * std::log(1.0 - pi);
        sy += yi;
    }
    out["ll"] = ll;
    out["df_chi2"] = df;
    double ybar = (n > 0) ? sy / static_cast<double>(n) : 0.0;
    if (ybar <= 0.0 || ybar >= 1.0) {  // degenerate outcome — null model undefined
        out["ll_0"] = nullptr;
        out["r2_pseudo"] = nullptr;
        out["chi2"] = nullptr;
        out["p_chi2"] = nullptr;
        return;
    }
    double ll0 = static_cast<double>(n) * (ybar * std::log(ybar) + (1.0 - ybar) * std::log(1.0 - ybar));
    double chi2 = 2.0 * (ll - ll0);
    out["ll_0"] = ll0;
    out["r2_pseudo"] = (ll0 != 0.0) ? 1.0 - ll / ll0 : 0.0;
    out["chi2"] = chi2;
    if (df > 0) out["p_chi2"] = chi2_pvalue(chi2, static_cast<double>(df));
    else out["p_chi2"] = nullptr;
}

std::string trimx(const std::string& s) {
    size_t a = s.find_first_not_of(" \t");
    if (a == std::string::npos) return "";
    return s.substr(a, s.find_last_not_of(" \t") - a + 1);
}
// split on commas AND whitespace (for by(h_pid year) style varlists)
std::vector<std::string> split_vars(const std::string& s) {
    std::vector<std::string> out;
    std::string cur;
    for (char ch : s) {
        if (ch == ',' || ch == ' ' || ch == '\t') { if (!cur.empty()) { out.push_back(cur); cur.clear(); } }
        else cur += ch;
    }
    if (!cur.empty()) out.push_back(cur);
    return out;
}
// index of the assignment '=' (a lone '=', not part of == <= >= !=)
size_t find_assign(const std::string& s) {
    for (size_t i = 0; i < s.size(); ++i)
        if (s[i] == '=') {
            char nx = (i + 1 < s.size()) ? s[i + 1] : 0, pv = (i > 0) ? s[i - 1] : 0;
            if (nx != '=' && pv != '=' && pv != '<' && pv != '>' && pv != '!') return i;
        }
    return std::string::npos;
}
// contents of key(...) inside s, paren-matched ("" if absent)
std::string paren_arg(const std::string& s, const std::string& key) {
    size_t k = s.find(key + "(");
    if (k == std::string::npos) return "";
    size_t p = k + key.size() + 1, st = p;
    int depth = 1;
    for (; p < s.size() && depth > 0; ++p) { if (s[p] == '(') ++depth; else if (s[p] == ')') --depth; }
    return s.substr(st, p - 1 - st);
}

// --- Shared loaders for the estimator commands (csdid / xt* / nbreg) ---------
// Validate a column exists AND is numeric (f64). A factor column stores int32
// codes, so reading it as double* yields garbage — for panel keys (ivar/time/
// gvar) that garbage explodes downstream into huge allocations / crashes. Fail
// loudly with an actionable message instead.
const double* require_num(const Dataset& ds, const char* cmd, const std::string& name) {
    if (!ds.has(name))
        throw EngineError("missing_column",
                          std::string(cmd) + ": column '" + name + "' not found in dataset", name);
    const Column& c = ds.col(name);
    if (c.dtype != DType::F64)
        throw EngineError("factor_misuse",
            std::string(cmd) + ": column '" + name + "' is a FACTOR but must be numeric (f64); "
            "panel keys (ivar/time/gvar) and y/x must stay numeric — re-run to_hbin WITHOUT "
            "--factor on '" + name + "'", name);
    return c.f64();
}

// Row indices where every listed column is non-missing (listwise deletion).
std::vector<int64_t> listwise_keep(const std::vector<const double*>& cols, int64_t n) {
    std::vector<int64_t> keep;
    keep.reserve(static_cast<size_t>(n));
    for (int64_t i = 0; i < n; ++i) {
        bool ok = true;
        for (const double* c : cols)
            if (c && std::isnan(c[i])) { ok = false; break; }
        if (ok) keep.push_back(i);
    }
    return keep;
}

// Compact a column to the kept rows.
std::vector<double> gather_rows(const double* src, const std::vector<int64_t>& keep) {
    std::vector<double> out(keep.size());
    for (size_t i = 0; i < keep.size(); ++i) out[i] = src[keep[i]];
    return out;
}

// Indices of covariates (cols of col-major X, n*k) that vary WITHIN at least one
// panel. A conditional fixed-effects estimator (xtpoisson,fe / xtlogit,fe)
// cannot identify a covariate that is constant within every panel — it is
// collinear with the unit effect. Stata silently omits such variables; we do the
// same instead of letting Newton-Raphson diverge to +/-1e15.
std::vector<int> within_varying_cols(const std::vector<double>& X, const double* ivar,
                                     int64_t n, int k) {
    std::vector<int> kept;
    for (int c = 0; c < k; ++c) {
        std::unordered_map<double, double> first;
        first.reserve(static_cast<size_t>(n) / 4 + 16);
        bool varies = false;
        const double* col = X.data() + static_cast<int64_t>(c) * n;
        for (int64_t i = 0; i < n; ++i) {
            auto it = first.find(ivar[i]);
            if (it == first.end()) first.emplace(ivar[i], col[i]);
            else if (std::fabs(col[i] - it->second) > 1e-12) { varies = true; break; }
        }
        if (varies) kept.push_back(c);
    }
    return kept;
}

// Scratch .hbin files created by maybe_keepif this process. PIET spawns a fresh
// cobra per command, so without cleanup every --keepif invocation leaks a
// ~100-160MB hbin. Removed by keepif_cleanup() before each command returns and
// via atexit (registered in main), so the normal success AND error-return paths
// both unlink. A leaked temp on a hard crash (SIGKILL) is acceptable.
std::vector<std::string> g_keepif_scratch;

void keepif_cleanup() {
    for (const auto& p : g_keepif_scratch) native_unlink(p);
    g_keepif_scratch.clear();
}

// Resolve the scratch directory honoring $TMPDIR -> $TMP -> /tmp on POSIX and
// %TMPDIR% -> %TMP% -> %TEMP% on Windows, so a machine whose boot disk is full
// but whose TMPDIR points at a roomy volume doesn't ENOSPC. Strips a trailing
// slash; falls back to /tmp (POSIX) or the CRT temp directory (Windows) if no
// candidate is a writable directory. The Windows values are read WIDE (env_utf8)
// and probed through native_open, which keeps a Korean username/path out of the
// ANSI-codepage trap (BUG-050).
std::string keepif_scratch_dir() {
    namespace fs = std::filesystem;
#if defined(_WIN32)
    const char* vars[] = {"TMPDIR", "TMP", "TEMP"};
#else
    const char* vars[] = {"TMPDIR", "TMP"};
#endif
    for (const char* var : vars) {
        std::string d = env_utf8(var);
        if (d.empty()) continue;
        while (d.size() > 1 && d.back() == '/') d.pop_back();
        std::error_code ec;
        if (fs::is_directory(to_native_path(d), ec)) {
            // Probe writability with a uniquely-named temp file. O_CREAT|O_TRUNC
            // (not O_EXCL) so the POSIX outcome stays exactly what the previous
            // std::ofstream probe produced, including for a stale same-pid leftover.
            std::string probe = native_join_path(
                d, ".cecon_wtest_" + std::to_string(native_process_id()));
            int fd = native_open(probe, O_WRONLY | O_CREAT | O_TRUNC, 0600);
            if (fd >= 0) {
                ::close(fd);
                native_unlink(probe);
                return d;
            }
        }
    }
#if defined(_WIN32)
    std::error_code ec;
    fs::path fallback = fs::temp_directory_path(ec);
    return ec ? std::string(".") : from_native_path(fallback);
#else
    return "/tmp";
#endif
}

// If keepif is non-empty, load the data into a Frame, apply the row filter, and
// write a temp .hbin whose path is returned (reuses the same evaluator as
// `cobra eval --keepif`, so estimators get inline `if`-sample control). Returns
// the original path when keepif is empty. The scratch honors the platform temp
// environment described above and is tracked in g_keepif_scratch for cleanup
// (see keepif_cleanup). The process id + a per-process counter keeps the name
// unique across concurrent processes and multiple --keepif calls in one process.
std::string maybe_keepif(const std::string& data, const std::string& keepif) {
    if (keepif.empty()) return data;
    // COBRA_VERB_DIAG (measurement-only, see src/core/verb_diag.h): this is the
    // #1 unmeasured scratch source for any --keepif-bearing verb (joinby, merge,
    // expand, fillin, cross, contract, gsort, duplicates, ...) — every call
    // materializes a full filtered Frame AND writes it straight back out as a
    // throwaway .hbin before the real verb ever opens its data. Phases: load
    // (open the source), eval (the Frame::keep_if kernel call — same kernel
    // `eval --keepif`'s inline path times in cmds_data.cpp, but here it feeds a
    // write instead of staying in memory), write (the scratch write itself,
    // whose byte size is stat'd below). OFF path costs one bool check.
    const bool kVerbDiag = cecon::verb_diag_enabled();
    const auto t_req0 = cecon::verb_diag_now(kVerbDiag);

    // WS-D2: one open, not two — `frame_from_acquired` only shares acquire_data's OWN
    // handle, so a local Dataset fell through to Frame::from_dataset, which re-opens
    // the path to own an independent mmap. Same defect as the four `using` sites; the
    // fix is the same shared-handle idiom (see open_using_frame's note). Measured on
    // `merge --keepif`: merge_file_opens 4 -> 3.
    const auto t_load0 = cecon::verb_diag_now(kVerbDiag);
    Frame fr = Frame::from_dataset_shared(std::make_shared<Dataset>(Dataset::open(data)));
    const double load_ms = cecon::verb_diag_ms(kVerbDiag, t_load0);

    const int64_t rows_in = kVerbDiag ? fr.n_rows() : 0;
    const auto t_eval0 = cecon::verb_diag_now(kVerbDiag);
    fr.keep_if(keepif);
    const double eval_ms = cecon::verb_diag_ms(kVerbDiag, t_eval0);
    const int64_t rows_out = kVerbDiag ? fr.n_rows() : 0;

    static int seq = 0;
    std::string tmp = native_join_path(
        keepif_scratch_dir(), "cecon_keepif_" + std::to_string(native_process_id()) +
                                  "_" + std::to_string(seq++) + ".hbin");
    const auto t_write0 = cecon::verb_diag_now(kVerbDiag);
    fr.write_hbin(tmp);
    const double write_ms = cecon::verb_diag_ms(kVerbDiag, t_write0);
    g_keepif_scratch.push_back(tmp);

    if (kVerbDiag) {
        const int64_t scratch_bytes = cecon::verb_diag_file_size(to_native_path(tmp));
        const double total_ms = cecon::verb_diag_ms(kVerbDiag, t_req0);
        const double other_ms = total_ms - load_ms - eval_ms - write_ms;
        cecon::verb_diag_emit(
            "[verb-diag] verb=keepif_scratch total_ms=%.3f load_ms=%.3f eval_ms=%.3f write_ms=%.3f "
            "other_ms=%.3f scratch_bytes=%lld rows_in=%lld rows_out=%lld\n",
            total_ms, load_ms, eval_ms, write_ms, other_ms,
            static_cast<long long>(scratch_bytes), static_cast<long long>(rows_in),
            static_cast<long long>(rows_out));
    }
    return tmp;
}

// ---- E1: time-series operators must see the FULL tsset series, THEN `if` -------
// Stata's L./F./D./S. operators are resolved against the whole dataset in memory;
// `if` only selects which of those already-computed rows enter the estimation
// sample (verified: `regress y L.x if t>=15` keeps n=16 in Stata, with the t=15
// boundary row's L.x sourced from the t=14 row — a row `if` itself excludes from
// the sample). maybe_keepif() above instead writes a physically row-REDUCED scratch
// .hbin before the estimator's own TS pre-pass (build_model's ts_prepass) ever
// runs, so a boundary row's lag/lead lookup misses a neighbor that's already been
// deleted and spuriously comes back missing (observed: n=15). The fix below keeps
// maybe_keepif's cheap physical pre-filter for every case that does NOT mix `if`
// with a TS-operator varlist (the overwhelming majority — byte-identical to
// before), and for the case that does, evaluates `if` into a full-length keep MASK
// over the UNFILTERED dataset instead, so build_model/build_iv_model can apply it
// AFTER their internal ts_prepass has already materialized L./F./D./S. against the
// complete series (see the row_mask parameter added to both in core/model.h/.cpp).

// Evaluate `keepif` against the FULL dataset WITHOUT removing any rows, returning a
// length-n_rows() mask (element 1 = row satisfies the `if`). Reuses Frame::keep_if
// VERBATIM (via a synthetic row-index column that keep_if compacts like every other
// column) instead of re-implementing its missing/relational semantics here — those
// stay single-sourced in frame.cpp. This function only changes WHEN a TS operator
// sees the data; it can never change WHICH rows an `if` expression selects.
std::vector<char> eval_keepif_mask(const std::string& data_path, const std::string& keepif) {
    Frame fr = Frame::from_dataset_shared(  // WS-D2: one open, not two (see maybe_keepif)
        std::make_shared<Dataset>(Dataset::open(data_path)));
    const int64_t n = fr.n_rows();
    std::vector<double> idx(static_cast<size_t>(n));
    for (int64_t i = 0; i < n; ++i) idx[static_cast<size_t>(i)] = static_cast<double>(i);
    fr.set_f64_column("__cecon_keepif_rowidx", std::move(idx));
    fr.keep_if(keepif);  // same expression, same evaluator, same row-selection rule as maybe_keepif's plain path
    std::vector<char> mask(static_cast<size_t>(n), 0);
    const double* kept = fr.col("__cecon_keepif_rowidx").f64.data();
    const int64_t m = fr.n_rows();
    for (int64_t i = 0; i < m; ++i) mask[static_cast<size_t>(kept[i])] = 1;
    return mask;
}

// True if any token in `names` carries a time-series operator (L./F./D./S.), e.g.
// "L.x" or the factor-prefixed "i.L.gdp". Pure string parsing (cecon::split_ts) —
// safe to call before any data is opened.
bool varlist_has_ts(const std::vector<std::string>& names) {
    for (const auto& t : names)
        if (cecon::split_ts(t).has_ts) return true;
    return false;
}

// The E1 routing decision for one `--keepif`-bearing estimator call. `data` stays
// the ORIGINAL path and `active`/`mask` carry the `if` restriction when TS tokens
// are actually in play (tsset known, keepif non-empty, and at least one of
// `token_lists` — the TS-eligible varlists for this estimator, e.g. {&xs, &ys} for
// feols or {&exog,&endog,&instruments,&ys} for iv; build_model/build_iv_model take
// the depvar through ts_prepass too, BUG-1277) carries an operator. Every other case (no keepif, no
// TS token, or no tsset — the last mirrors ts_prepass's own ts_no_tsset guard, so
// that error still surfaces exactly as before) falls back to maybe_keepif's plain
// physical pre-filter, byte-identical to pre-E1 cobra.
KeepifPlan plan_keepif(const std::string& data, const std::string& keepif, const TsMeta* ts,
                       std::initializer_list<const std::vector<std::string>*> token_lists) {
    KeepifPlan plan;
    bool has_ts_tok = false;
    if (ts && !ts->timevar.empty() && !keepif.empty())
        for (const auto* vl : token_lists)
            if (vl && varlist_has_ts(*vl)) { has_ts_tok = true; break; }
    if (!has_ts_tok) {
        plan.data = maybe_keepif(data, keepif);
        return plan;
    }
    plan.data = data;  // keep the FULL dataset — TS operators must see it whole
    plan.mask = eval_keepif_mask(data, keepif);
    plan.active = true;
    return plan;
}

// ---- e(sample) T1a: emit the admitted rows -------------------------------------
// The engine owns every admission rule (the `if` restriction, listwise deletion over
// y/x/fe/cluster/weight, zero weights, TS-operator loss, singleton drops), so the
// rows a fit used are exactly the runner's `keep` — nothing is recomputed here. The
// only work is mapping those indices back onto the `--data` frame's rows (or, with
// --esample-index, onto the caller's own N-row frame) and writing them out.
EsampleRequest esample_request(const std::vector<std::string>& a, const char* cmd) {
    EsampleRequest req;
    const std::string c(cmd);
    // has() first so a flag given with an EMPTY value is caught instead of reading as
    // "not requested" — that would silently skip the file the caller asked for.
    const bool out_given = has(a, "--esample-out");
    const bool idx_given = has(a, "--esample-index");
    const bool n_given = has(a, "--esample-n");
    req.path = opt(a, "--esample-out");
    req.index_col = opt(a, "--esample-index");
    const std::string nstr = opt(a, "--esample-n");
    if (out_given && req.path.empty())
        throw EngineError("bad_input", c + ": --esample-out requires a PATH", "--esample-out");
    if (idx_given && req.index_col.empty())
        throw EngineError("bad_input", c + ": --esample-index requires a column name",
                          "--esample-index");
    if (n_given && nstr.empty())
        throw EngineError("bad_input", c + ": --esample-n requires a row count", "--esample-n");
    if (!out_given && (idx_given || n_given))
        throw EngineError("bad_input",
            c + ": --esample-index/--esample-n are only meaningful with --esample-out",
            idx_given ? "--esample-index" : "--esample-n");
    if (idx_given != n_given)
        throw EngineError("bad_input",
            c + ": --esample-index and --esample-n must be given together",
            idx_given ? "--esample-index" : "--esample-n");
    if (n_given) {
        size_t pos = 0;
        long long v = -1;
        try { v = std::stoll(nstr, &pos); } catch (const std::exception&) { pos = 0; }
        if (pos != nstr.size() || v < 0)
            throw EngineError("bad_input",
                c + ": --esample-n must be a non-negative integer (got '" + nstr + "')", nstr);
        req.n_target = static_cast<int64_t>(v);
    }
    return req;
}

void emit_esample(const EsampleRequest& req, const char* cmd, const std::string& data,
                  const std::string& keepif, bool keepif_masked, const EsampleRows& rows,
                  json& out) {
    if (!req.active()) return;
    const std::string c(cmd);
    if (!rows.loaded)
        throw EngineError("internal", c + ": --esample-out: the fit did not report its rows");
    const Dataset& ld = *rows.loaded;
    const int64_t n_loaded = ld.n_rows();
    for (int64_t k : rows.keep)
        if (k < 0 || k >= n_loaded)
            throw EngineError("internal", c + ": --esample-out: kept row index out of range");

    std::vector<char> es;
    if (!req.index_col.empty()) {
        // Scatter through the caller's position column. Validated on EVERY loaded row
        // (not only the admitted ones) and before anything is written, so a bad column
        // is a refusal, never a truncated or misplaced mask.
        const double* col = require_num(ld, cmd, req.index_col);
        const int64_t N = req.n_target;
        std::vector<int64_t> pos(static_cast<size_t>(n_loaded));
        std::vector<char> used(static_cast<size_t>(N), 0);
        for (int64_t i = 0; i < n_loaded; ++i) {
            const double v = col[i];
            if (!(std::isfinite(v) && v == std::floor(v) && v >= 1.0 &&
                  v <= static_cast<double>(N)))
                throw EngineError("bad_input",
                    c + ": --esample-index '" + req.index_col + "' row " + std::to_string(i + 1) +
                    " is not an integer position in [1, " + std::to_string(N) + "]",
                    req.index_col);
            const int64_t p = static_cast<int64_t>(v) - 1;
            if (used[static_cast<size_t>(p)])
                throw EngineError("bad_input",
                    c + ": --esample-index '" + req.index_col + "' repeats position " +
                    std::to_string(p + 1), req.index_col);
            used[static_cast<size_t>(p)] = 1;
            pos[static_cast<size_t>(i)] = p;
        }
        es.assign(static_cast<size_t>(N), 0);
        for (int64_t k : rows.keep) es[static_cast<size_t>(pos[static_cast<size_t>(k)])] = 1;
    } else if (!keepif.empty() && !keepif_masked) {
        // maybe_keepif loaded a physically reduced scratch; keep_if compacts in order,
        // so scratch row j is the j-th source row the predicate admits. Re-evaluate the
        // SAME predicate with the SAME evaluator to recover that map (paid only here).
        const std::vector<char> mask = eval_keepif_mask(data, keepif);
        std::vector<int64_t> src;
        src.reserve(static_cast<size_t>(n_loaded));
        for (size_t i = 0; i < mask.size(); ++i)
            if (mask[i]) src.push_back(static_cast<int64_t>(i));
        if (static_cast<int64_t>(src.size()) != n_loaded)
            throw EngineError("internal",
                c + ": --esample-out: the --keepif mask admits " + std::to_string(src.size()) +
                " rows but the fit loaded " + std::to_string(n_loaded));
        es.assign(mask.size(), 0);
        for (int64_t k : rows.keep) es[static_cast<size_t>(src[static_cast<size_t>(k)])] = 1;
    } else {
        // No --keepif, or the E1 mask path: the fit loaded the --data frame itself.
        es.assign(static_cast<size_t>(n_loaded), 0);
        for (int64_t k : rows.keep) es[static_cast<size_t>(k)] = 1;
    }

    int64_t ones = 0;
    std::string buf;
    buf.reserve(es.size() * 2);
    for (char v : es) {
        buf += v ? "1\n" : "0\n";
        ones += v ? 1 : 0;
    }
    // Sibling temp + rename: a reader never sees a half-written mask, and a failed
    // write leaves any previous file at PATH untouched.
    const std::string tmp = req.path + ".tmp" + std::to_string(native_process_id());
    std::error_code ec;
    {
        std::ofstream f(to_native_path(tmp), std::ios::binary | std::ios::trunc);
        if (!f)
            throw EngineError("data_io", c + ": cannot create --esample-out file " + req.path,
                              req.path);
        f.write(buf.data(), static_cast<std::streamsize>(buf.size()));
        f.flush();
        if (!f) {
            f.close();
            std::filesystem::remove(to_native_path(tmp), ec);
            throw EngineError("data_io",
                c + ": failed writing --esample-out file " + req.path + " (disk full or I/O error)",
                req.path);
        }
    }
    std::filesystem::rename(to_native_path(tmp), to_native_path(req.path), ec);
    if (ec) {
        std::error_code rm;
        std::filesystem::remove(to_native_path(tmp), rm);
        throw EngineError("data_io",
            c + ": failed committing --esample-out file " + req.path + ": " + ec.message(),
            req.path);
    }
    out["esample_n"] = ones;
}

// Shared loader for binary/count models with an explicit intercept: returns the
// listwise-kept design [expanded x..., 1] (col-major), y, and the kept indices.
// build_model is the canonical factor-variable preprocessor, but it places the
// intercept first.  Rotate that one column to the end so every historical plain
// varlist keeps its coefficient ordering byte-for-byte ([x..., intercept]).
GlmInputs load_glm(const Dataset& ds, const char* cmd, const std::string& y,
                   const std::vector<std::string>& xs,
                   const std::vector<std::string>& cluster_names,
                   const std::string& weight_var, cecon::WeightKind weight_kind,
                   const std::string& offset_var, const cecon::TsMeta* ts,
                   const std::vector<char>* row_mask) {
    GlmInputs g;
    // Keep the established command-qualified diagnostics for ordinary y/x names.
    // Factor/interaction/TS expressions are intentionally left to build_model,
    // which resolves their operand names before validating them.
    (void)require_num(ds, cmd, y);
    for (const auto& x : xs)
        if (x.find_first_of(".#()") == std::string::npos)
            (void)require_num(ds, cmd, x);
    // BUG-331: offset_var joins the SAME single listwise-deletion pass as y/x/
    // weights (build_model's own machinery) — a missing offset drops the row,
    // exactly like a missing regressor, which is what Stata does.
    if (!offset_var.empty()) (void)require_num(ds, cmd, offset_var);
    ModelData md = build_model(ds, y, xs, {}, cluster_names, offset_var, false, ts, row_mask,
                               weight_var, weight_kind);
    g.n = md.dm.n;
    g.n_dropped = md.dm.n_dropped;
    g.k = md.dm.k;
    g.X.resize(md.dm.X.size());
    g.y = std::move(md.dm.y);
    g.keep = std::move(md.keep);
    g.warnings = std::move(md.warnings);
    g.cluster = std::move(md.cluster);
    g.weights = std::move(md.weights);
    g.weight_kind = md.weight_kind;
    g.offset = std::move(md.offset);  // BUG-331

    // md.dm is [intercept, expanded x...]; GlmInputs' public contract is
    // [expanded x..., intercept].  Keep that contract because the MLE/diagnostic
    // callers and all existing JSON goldens rely on it.
    for (int c = 1; c < g.k; ++c) {
        if (g.n > 0) {
            const double* src = md.dm.X.data() + static_cast<int64_t>(c) * g.n;
            double* dst = g.X.data() + static_cast<int64_t>(c - 1) * g.n;
            std::copy(src, src + g.n, dst);
        }
        g.names.push_back(md.dm.xnames[static_cast<size_t>(c)]);
    }
    if (g.k > 0) {
        if (g.n > 0)
            std::copy(md.dm.X.data(), md.dm.X.data() + g.n,
                      g.X.data() + static_cast<int64_t>(g.k - 1) * g.n);
        g.names.push_back("(intercept)");
    }
    return g;
}

bool check_binary(const std::vector<double>& y, const char* cmd, const std::string& nm) {
    for (double v : y)
        if (v != 0.0 && v != 1.0) {
            std::fprintf(stderr, "%s: outcome '%s' is not binary (found %g); requires y in {0,1}.\n",
                         cmd, nm.c_str(), v);
            return false;
        }
    return true;
}

namespace {

// --- Stata `xtreg, fe` panel header scalars (contract A §35) ---------------------
// The SIX numbers `xtreg, fe` prints outside the coefficient table:
//   r2_between / r2_overall   (r2_within is already the engine's r.r2_within)
//   sigma_u / sigma_e / rho / corr_u_xb
// PIET routes `xtreg y x, fe` to a plain `feols --fe panelid` (§8), so they all have
// to come out of the feols path.
//
// ONE producer for all six. The previous between/overall R² helper reconstructed the
// predictor from the DATASET by variable name, which forced it to bail on any factor
// expansion (`r.names[i] != xs[i]`) — so `xtreg y x1 x2 i.t, fe`, the very model the
// P2 oracle pins, silently got no r2_between/r2_overall at all (BUG-156). This
// version reads the DESIGN MATRIX instead, so factor-expanded terms are just ordinary
// columns and every scalar comes off the same xb/y reconstruction. Keeping two
// producers for the same quantity is exactly the divergence trap; there is now one.
//
// The one thing the fit destroys is the raw scale: feols_fit demeans md.dm in place.
// Hence the two-phase split — capture_panel_means() runs BEFORE feols_fit,
// add_panel_scalars() after. For a SINGLE FE the within transform is exact in one
// sweep, so the raw values come back exactly:
//     y_it  = md.dm.y[i]      + ȳ_i
//     Xβ_it = (X_dm)_it·β     + X̄_i·β
//
// Cost: one O(n·k) accumulation pass and an n_groups×k buffer (bounded above by the
// design matrix already resident). Engine-side feols_fit is untouched, so every
// non-CLI consumer (benchmarks, piet_native) pays nothing.
struct PanelMeans {
    bool ok = false;
    int kd = 0;                  // design width at capture (pre collinear compaction)
    std::vector<int64_t> cnt;    // rows per FE level (0 ⇒ level absent from the sample)
    std::vector<double> ybar;    // per level: mean of RAW y
    std::vector<double> xbar;    // per level: mean of each RAW X column, [lvl*kd + c]
};

// md must be pre-feols_fit (md.dm still raw). Single absorbed FE only.
PanelMeans capture_panel_means(const ModelData& md) {
    PanelMeans pm;
    if (md.fe.size() != 1) return pm;
    const Factor& F = md.fe[0];
    const int64_t n = md.dm.n;
    const int k = md.dm.k;
    const int ng = F.n_levels;
    if (n <= 0 || k <= 0 || ng <= 0 || static_cast<int64_t>(F.codes.size()) != n) return pm;
    // BUG-1292: `xtreg, fe` takes only a weight constant within the panel
    // (r(199) otherwise), and then computes its panel scalars
    // from the weighted fit's xb with UNweighted summaries (`summ U`, `corr`): the
    // panel means below are the same either way. A weight varying within a level
    // (areg/reghdfe) has no xtreg scalars: emit nothing, as before.
    if (!md.weights.empty()) {
        std::vector<double> wg(static_cast<size_t>(ng), std::nan(""));
        for (int64_t i = 0; i < n; ++i) {
            const int g = F.codes[static_cast<size_t>(i)];
            if (g < 0 || g >= ng) return pm;
            double& v = wg[static_cast<size_t>(g)];
            if (std::isnan(v)) v = md.weights[static_cast<size_t>(i)];
            else if (v != md.weights[static_cast<size_t>(i)]) return pm;
        }
    }
    pm.kd = k;
    pm.cnt.assign(static_cast<size_t>(ng), 0);
    pm.ybar.assign(static_cast<size_t>(ng), 0.0);
    pm.xbar.assign(static_cast<size_t>(ng) * static_cast<size_t>(k), 0.0);
    for (int64_t i = 0; i < n; ++i) {
        const int g = F.codes[static_cast<size_t>(i)];
        if (g < 0 || g >= ng) return pm;  // malformed codes → emit nothing
        pm.cnt[static_cast<size_t>(g)] += 1;
        pm.ybar[static_cast<size_t>(g)] += md.dm.y[static_cast<size_t>(i)];
    }
    for (int c = 0; c < k; ++c) {
        const double* col = md.dm.X.data() + static_cast<int64_t>(c) * n;
        for (int64_t i = 0; i < n; ++i)
            pm.xbar[static_cast<size_t>(F.codes[static_cast<size_t>(i)]) * static_cast<size_t>(k) +
                    static_cast<size_t>(c)] += col[i];
    }
    for (int g = 0; g < ng; ++g) {
        const int64_t T = pm.cnt[static_cast<size_t>(g)];
        if (T == 0) continue;
        const double invT = 1.0 / static_cast<double>(T);
        pm.ybar[static_cast<size_t>(g)] *= invT;
        for (int c = 0; c < k; ++c)
            pm.xbar[static_cast<size_t>(g) * static_cast<size_t>(k) + static_cast<size_t>(c)] *= invT;
    }
    pm.ok = true;
    return pm;
}

// md must be post-feols_fit (md.dm.X is the DEMEANED, collinear-compacted design).
void add_panel_scalars(json& out, const ModelData& md, const PanelMeans& pm, const FeolsResult& r) {
    if (!pm.ok || md.fe.size() != 1) return;
    // Collinear omission renumbers the design columns after the capture; the xbar
    // buffer is pre-drop, so defer that case rather than mis-pair columns (same
    // deferral GAP B's _cons reconstruction already takes in feols.cpp).
    if (!r.omitted.empty() || md.dm.k != pm.kd) return;
    const int k = pm.kd;
    const int64_t n = md.dm.n;
    // r.beta is the k design slopes, optionally followed by GAP B's appended "_cons"
    // row (r.k == k+1). Only the first k enter Xβ.
    if (static_cast<int>(r.beta.size()) < k) return;
    const Factor& F = md.fe[0];
    const int ng_levels = F.n_levels;
    if (static_cast<int64_t>(F.codes.size()) != n) return;

    // û_i = mean_i(y − Xβ) = ȳ_i − X̄_i·β   (the within residual averages to 0 in-group,
    // exactly, so the two forms are algebraically identical).
    std::vector<double> u(static_cast<size_t>(ng_levels), 0.0);
    std::vector<double> xbg(static_cast<size_t>(ng_levels), 0.0);  // X̄_i·β
    int64_t ng = 0;
    double su = 0;
    for (int g = 0; g < ng_levels; ++g) {
        if (pm.cnt[static_cast<size_t>(g)] == 0) continue;
        double xb = 0;
        for (int c = 0; c < k; ++c)
            xb += pm.xbar[static_cast<size_t>(g) * static_cast<size_t>(k) + static_cast<size_t>(c)] *
                  r.beta[static_cast<size_t>(c)];
        xbg[static_cast<size_t>(g)] = xb;
        u[static_cast<size_t>(g)] = pm.ybar[static_cast<size_t>(g)] - xb;
        su += u[static_cast<size_t>(g)];
        ++ng;
    }
    if (ng < 2) return;

    // sigma_u: the PER-GROUP (unweighted, one observation per panel) sample sd of û_i
    // with divisor n_g−1. Measured against the P2 oracle (`xtreg y x1 x2 i.t, fe
    // vce(cluster id)` → sigma_u = .31448074): the per-group /(n_g−1) form reproduces
    // Stata exactly, while every per-OBSERVATION (T_i-weighted) variant misses —
    // /(N−1) gives .31353552, /N gives .31350180, /n_g gives .31369355.
    const double ubar = su / static_cast<double>(ng);
    double s2 = 0;
    for (int g = 0; g < ng_levels; ++g) {
        if (pm.cnt[static_cast<size_t>(g)] == 0) continue;
        const double d = u[static_cast<size_t>(g)] - ubar;
        s2 += d * d;
    }
    const double sigma_u2 = s2 / static_cast<double>(ng - 1);

    // sigma_e: RMSE of the within regression, dof = N − n_g − k (xtreg.cpp's
    // convention, feols.cpp line 158's twin). Oracle .77958951 — exact. Weighted, N is
    // Stata's e(N) (rows under aw/pw, sum(w) under fw) and r.ssr the weighted RSS.
    const double n_obs = r.n_eff > 0.0 ? r.n_eff : static_cast<double>(n);
    const double dof_e = n_obs - static_cast<double>(ng) - static_cast<double>(k);
    if (!(dof_e > 0)) return;
    const double sigma_e2 = r.ssr / dof_e;
    // BUG-1292: a weighted fit left md.dm demeaned AND scaled by sqrt(w~) (feols_fit,
    // w~ = w * n_eff/sum_w); undo the scale to get the demeaned rows back. The weight
    // is constant within the panel here (capture_panel_means).
    std::vector<double> inv_s;
    if (!md.weights.empty()) {
        if (!(r.sum_w > 0.0) || !(r.n_eff > 0.0)) return;
        const double c = r.n_eff / r.sum_w;
        inv_s.resize(static_cast<size_t>(n));
        for (int64_t i = 0; i < n; ++i)
            inv_s[static_cast<size_t>(i)] = 1.0 / std::sqrt(md.weights[static_cast<size_t>(i)] * c);
    }

    // corr(u_i, Xb): Stata computes this at the OBSERVATION level — û_i repeated
    // across the panel's rows against the per-ROW Xβ_it — NOT as a per-group
    // correlation. Measured on the same oracle line (corr(u_i,Xb) = −0.1166):
    // per-observation gives −0.11659761 (match); the per-group form gives −0.20255728
    // and the T-weighted per-group form −0.20176792 (both wrong). The difference is
    // the denominator: sd(Xβ) over rows carries the WITHIN-panel variation of Xβ,
    // which the group-mean form throws away.
    // Xβ_it is reconstructed as (demeaned X)_it·β + X̄_i·β — md.dm.X is demeaned by
    // now, and for a single FE the demeaning subtracts exactly the group mean.
    double sa = 0, sb = 0;
    std::vector<double> xrow(static_cast<size_t>(n), 0.0);
    for (int c = 0; c < k; ++c) {
        const double* col = md.dm.X.data() + static_cast<int64_t>(c) * n;
        const double bc = r.beta[static_cast<size_t>(c)];
        for (int64_t i = 0; i < n; ++i) xrow[static_cast<size_t>(i)] += col[i] * bc;
    }
    for (int64_t i = 0; i < n; ++i) {
        const int g = F.codes[static_cast<size_t>(i)];
        if (!inv_s.empty()) xrow[static_cast<size_t>(i)] *= inv_s[static_cast<size_t>(i)];
        xrow[static_cast<size_t>(i)] += xbg[static_cast<size_t>(g)];
        sa += u[static_cast<size_t>(g)];
        sb += xrow[static_cast<size_t>(i)];
    }
    const double ma = sa / static_cast<double>(n), mb = sb / static_cast<double>(n);
    double sab = 0, saa = 0, sbb = 0;
    for (int64_t i = 0; i < n; ++i) {
        const double da = u[static_cast<size_t>(F.codes[static_cast<size_t>(i)])] - ma;
        const double db = xrow[static_cast<size_t>(i)] - mb;
        sab += da * db; saa += da * da; sbb += db * db;
    }

    out["sigma_u"] = std::sqrt(std::max(0.0, sigma_u2));
    out["sigma_e"] = std::sqrt(std::max(0.0, sigma_e2));
    out["rho"] = (sigma_u2 + sigma_e2 > 0) ? sigma_u2 / (sigma_u2 + sigma_e2) : 0.0;
    out["corr_u_xb"] = (saa > 0 && sbb > 0) ? sab / std::sqrt(saa * sbb) : 0.0;

    // --- between / overall R² (BUG-156) -------------------------------------------
    // Stata reports all three panel R² as SQUARED CORRELATIONS of the slope-only
    // predictor Xβ against y — overall over all rows, between over the group means,
    // within group-demeaned (already emitted as r2_within by the engine).
    // `xrow` above is Xβ_it back on the raw scale; raw y comes back the same way,
    // y_it = (demeaned y)_it + ȳ_i, exact for a single FE.
    auto corr2 = [](const std::vector<double>& av, const std::vector<double>& bv) -> double {
        const size_t m = av.size();
        if (m == 0) return 0.0;
        double ma2 = 0, mb2 = 0;
        for (size_t i = 0; i < m; ++i) { ma2 += av[i]; mb2 += bv[i]; }
        ma2 /= static_cast<double>(m); mb2 /= static_cast<double>(m);
        double sab2 = 0, saa2 = 0, sbb2 = 0;
        for (size_t i = 0; i < m; ++i) {
            const double da = av[i] - ma2, db = bv[i] - mb2;
            sab2 += da * db; saa2 += da * da; sbb2 += db * db;
        }
        const double den = saa2 * sbb2;
        return (den > 0.0) ? (sab2 * sab2) / den : 0.0;
    };
    std::vector<double> yrow(static_cast<size_t>(n));
    for (int64_t i = 0; i < n; ++i)
        yrow[static_cast<size_t>(i)] =
            (inv_s.empty() ? md.dm.y[static_cast<size_t>(i)]
                           : md.dm.y[static_cast<size_t>(i)] * inv_s[static_cast<size_t>(i)]) +
            pm.ybar[static_cast<size_t>(F.codes[static_cast<size_t>(i)])];
    out["r2_overall"] = corr2(xrow, yrow);

    std::vector<double> mxb, myy;
    mxb.reserve(static_cast<size_t>(ng)); myy.reserve(static_cast<size_t>(ng));
    for (int g = 0; g < ng_levels; ++g) {
        if (pm.cnt[static_cast<size_t>(g)] == 0) continue;
        mxb.push_back(xbg[static_cast<size_t>(g)]);
        myy.push_back(pm.ybar[static_cast<size_t>(g)]);
    }
    out["r2_between"] = corr2(mxb, myy);
}

}  // namespace

// --- P7: `svy: regress` --------------------------------------------------------
//
// The point estimates are the ordinary pweighted WLS fit — svy changes the
// VARIANCE and the reference distribution, nothing else:
//   V   = (X'WX)^-1 · [stratified linearized meat over PSU totals] · (X'WX)^-1
//   dof = #PSU − #strata                       (46 on the P7 fixture: 50 − 4)
//   inference on t(dof), CI at t_critical(dof)
//   model test = adjusted Wald F = W·(d−q+1)/(q·d) reported as F(q, d−q+1)
//
// Everything is invariant to a rescaling of the weights (bread scales 1/c, meat
// c²), so the RAW pweights are used throughout and Σw is the population size.
json run_feols_svy(const std::string& data, const std::string& y,
                   const std::vector<std::string>& xs, const std::string& weight_var,
                   const std::string& psu_var, const std::string& strata_var,
                   const cecon::TsMeta* ts, const std::vector<char>* if_mask, double level,
                   EsampleRows* esample) {
    const Dataset& ds = acquire_data(data);
    // PSU and stratum ride in as clustering dimensions so they join the SINGLE
    // listwise-deletion pass — a design column that is missing must drop the row
    // from the estimation sample, exactly as it does for a regressor.
    // An empty strata_var is a design without strata(): Stata treats it as ONE
    // stratum (`svy` header "Number of strata = 1", design df = #PSU - 1;
    // w2t_boottest_family.log:2455 H1). The constant stratum is appended after
    // build_model, so it drops no row and the PSU alone joins the deletion pass.
    const bool one_stratum = strata_var.empty();
    std::vector<std::string> design_cols = {psu_var};
    if (!one_stratum) design_cols.push_back(strata_var);
    ModelData md = build_model(ds, y, xs, /*fenames=*/{}, design_cols, "", /*drop_singleton=*/true, ts,
                               if_mask, weight_var, cecon::WeightKind::Probability);
    if (esample) { esample->keep = md.keep; esample->loaded = &ds; }  // e(sample) T1a
    if (one_stratum && !md.cluster.empty()) {
        Factor st;
        st.n_levels = 1;
        st.codes.assign(static_cast<size_t>(md.dm.n), 0);
        st.count.assign(1, static_cast<double>(md.dm.n));
        md.cluster.push_back(std::move(st));
    }
    if (md.cluster.size() < 2)
        throw EngineError("bad_input", "svy: could not resolve both --psu and --strata columns");

    FeolsResult r = feols_fit(md, Vcov::IID, 0, /*no_intercept=*/false);
    const int64_t n = md.dm.n;
    const int k = md.dm.k;
    const double* X = md.dm.X.data();

    // build_model ALREADY row-scales the design by sqrt(w̃) for aw/pw (w̃ = the
    // weights normalized to mean 1), so md.dm.X is x̃ = sqrt(w̃)·x and md.dm.y is
    // ỹ. Multiplying by the raw weight again here would form Σ w·w̃·x x' — the
    // shape that first came out of this function, wrong by 2-10% per coefficient
    // and in a way no single scalar factor explains. Working directly in the
    // scaled metric is exact, because ẽ·x̃ = w̃·e·x and X̃'X̃ = Σ w̃ x x': that is
    // the same sandwich as the raw-weight one up to a constant Σw/n that cancels
    // between bread and meat (the linearized variance is invariant to rescaling
    // the weights). pop_size below still comes from the RAW weights, since Σw is
    // a population total and is NOT scale-invariant.
    std::vector<double> scores(static_cast<size_t>(n) * k, 0.0);
    for (int64_t i = 0; i < n; ++i) {
        double xb = 0.0;
        for (int j = 0; j < k; ++j) xb += X[static_cast<size_t>(j) * n + i] * r.beta[j];
        const double e = md.dm.y[static_cast<size_t>(i)] - xb;
        for (int j = 0; j < k; ++j)
            scores[static_cast<size_t>(j) * n + i] = e * X[static_cast<size_t>(j) * n + i];
    }
    std::vector<double> bread(static_cast<size_t>(k) * k, 0.0);
    la::xtx(X, n, k, bread.data());
    if (la::spd_inverse(bread.data(), k) != 0)
        throw EngineError("bad_input", "svy: the weighted cross-product matrix is singular");

    const double* wptr = md.weights.empty() ? nullptr : md.weights.data();
    SvyMeatResult sm =
        cecon::svy_linearized_meat(scores.data(), n, k, md.cluster[0], md.cluster[1], wptr);
    if (!sm.singleton_stratum.empty())
        throw EngineError("bad_input",
                          "svy: stratum variable '" + sm.singleton_stratum +
                              "' has a stratum with a single sampling unit; the linearized "
                              "variance is undefined there (Stata reports missing)");
    if (!sm.ok)
        throw EngineError("bad_input",
                          "svy: sampling units are not nested within strata (a PSU appears "
                          "under more than one stratum)");

    std::vector<double> V(static_cast<size_t>(k) * k, 0.0);
    la::sandwich(bread.data(), sm.meat.data(), k, 1.0, V.data(), nullptr);

    const int design_df = sm.n_psu - sm.n_strata;
    if (design_df <= 0)
        throw EngineError("bad_input", "svy: design degrees of freedom (#PSU − #strata) is not positive");

    std::vector<double> se(static_cast<size_t>(k), 0.0);
    for (int j = 0; j < k; ++j) {
        const double v = V[static_cast<size_t>(j) * k + j];
        se[static_cast<size_t>(j)] = v > 0.0 ? std::sqrt(v) : 0.0;
    }

    json coef, seo, tst;
    for (int j = 0; j < k; ++j) {
        coef[r.names[static_cast<size_t>(j)]] = r.beta[static_cast<size_t>(j)];
        seo[r.names[static_cast<size_t>(j)]] = se[static_cast<size_t>(j)];
        tst[r.names[static_cast<size_t>(j)]] = tstat(r.beta[static_cast<size_t>(j)], se[static_cast<size_t>(j)]);
    }
    json out = {{"estimator", "feols"},
                {"depvar", y},
                {"n_obs", n},
                {"n_dropped", r.n_dropped},
                {"coef", coef},
                {"se", seo},
                {"tstat", tst},
                {"vcov_type", "svy_linearized"},
                {"r2", r.r2},
                {"rmse", r.rmse}};
    out["svy"] = {{"strata_var", strata_var}, {"psu_var", psu_var},
                  {"n_strata", sm.n_strata},  {"n_psu", sm.n_psu},
                  {"design_df", design_df},   {"pop_size", sm.pop_size}};
    add_inference(out, r.names, r.beta, se, static_cast<double>(design_df), /*asymptotic=*/false,
                  level);

    // Adjusted Wald F over the slopes (every coefficient but the intercept).
    // Stata reports F(q, d − q + 1): on the P7 fixture q = 6 and d = 46, hence the
    // oracle's F(6, 41) — which is what pins this convention rather than d − q.
    std::vector<int> slopes;
    for (int j = 0; j < k; ++j) {
        // feols names the constant "(intercept)"; "_cons" is the Stata-facing
        // spelling other surfaces use. Match both — testing only for "_cons" here
        // silently left the constant IN the joint test, which changes q and so
        // both F and its degrees of freedom.
        const std::string& nm = r.names[static_cast<size_t>(j)];
        if (nm != "_cons" && nm != "(intercept)") slopes.push_back(j);
    }
    const int q = static_cast<int>(slopes.size());
    if (q > 0 && design_df - q + 1 > 0) {
        std::vector<double> R(static_cast<size_t>(q) * k, 0.0), qv(static_cast<size_t>(q), 0.0);
        for (int j = 0; j < q; ++j) R[static_cast<size_t>(j) * k + slopes[static_cast<size_t>(j)]] = 1.0;
        WaldResult w = cecon::wald_test(r.beta.data(), V.data(), k, R.data(), qv.data(), q);
        if (w.ok) {
            const double f = w.chi2 * static_cast<double>(design_df - q + 1) /
                             (static_cast<double>(q) * static_cast<double>(design_df));
            add_model_f(out, f, q, static_cast<double>(design_df - q + 1));
            // add_model_f emits only F/p_F (its shape is frozen for every existing
            // caller), but the survey header prints "F(q, d−q+1)", so the two dof
            // ride in the svy block where they are new keys and break no golden.
            out["svy"]["f_df1"] = q;
            out["svy"]["f_df2"] = design_df - q + 1;
        }
    }
    add_warnings(out, md.warnings);
    return out;
}

// e(kcluster) / e(cluster1..): one entry per multiway cluster combination
// (multiway_cluster_combos order) under `kcluster`, {"name", "levels"} each. Absent
// key when empty, so every other reply is byte-identical.
static void add_kcluster(json& out, const std::vector<ClusterCombo>& combos) {
    if (combos.empty()) return;
    json arr = json::array();
    for (const auto& c : combos) arr.push_back({{"name", c.name}, {"levels", c.levels}});
    out["kcluster"] = std::move(arr);
}

json run_feols(const std::string& data, const std::string& y,
               const std::vector<std::string>& xs, const std::vector<std::string>& fes,
               const std::string& vcov_type, const std::vector<std::string>& clusters,
               bool drop_singleton, const cecon::TsMeta* ts,
               const std::vector<char>* if_mask, bool no_intercept, double level,
               bool level_set, const std::string& weight_var,
               cecon::WeightKind weight_kind, EsampleRows* esample,
               bool keep_nested_fe_dof, bool emit_v, bool multiway_subset_factors,
               bool fe_nested_in_clusters, bool regress_exact_fit, bool collinear_forward) {
    auto t0 = std::chrono::steady_clock::now();
    const Dataset& ds = acquire_data(data);
    auto t1 = std::chrono::steady_clock::now();
    ModelData md = build_model(ds, y, xs, fes, clusters, "", drop_singleton, ts, if_mask,
                               weight_var, weight_kind);
    md.collinear_forward = collinear_forward;  // BUG-1067
    if (esample) { esample->keep = md.keep; esample->loaded = &ds; }  // e(sample) T1a
    auto t2 = std::chrono::steady_clock::now();
    // FU-401/FU-407: xtreg,fe checks every cluster variable, in the
    // order given, over the estimation sample: a cluster
    // that varies within a panel is "panels are not nested within clusters", r(498),
    // and with two or more cluster variables the failing one is named
    // (w2s_multiway_class.log:630-632 `vce(cluster id id2)`, :774 `vce(cluster id2)`).
    // The cluster variable equal to the panel variable passes.
    if (fe_nested_in_clusters) {
        if (md.fe.empty() || md.cluster.empty())
            throw EngineError("bad_input",
                "feols: --fe-nested-in-clusters requires --fe and a cluster VCE");
        for (const Factor& c : md.cluster) {
            if (c.name == md.fe[0].name || fe_nested_in_cluster(md.fe[0], c)) continue;
            std::string msg = "panels are not nested within clusters";
            if (md.cluster.size() > 1) msg += "\ncluster variable " + c.name;
            throw EngineError("bad_input", msg, "panels_not_nested");
        }
    }

    Vcov vt = Vcov::IID;
    if (vcov_type == "hc1" || vcov_type == "HC1") vt = Vcov::HC1;
    else if (vcov_type == "cluster") vt = Vcov::Cluster;
    else if (vcov_type == "twoway" || vcov_type == "multiway") vt = Vcov::Multiway;
    // §35 xtreg,fe variance components: the raw-scale group means must be taken
    // before feols_fit demeans md.dm in place. No-op unless there is exactly one
    // absorbed FE and any weight is constant within its levels (xtreg,fe's own
    // requirement; BUG-1292).
    const PanelMeans panel_means = capture_panel_means(md);
    FeolsResult r = feols_fit(md, vt, 0, no_intercept, keep_nested_fe_dof, multiway_subset_factors,
                              regress_exact_fit);
    auto t3 = std::chrono::steady_clock::now();

    auto ms = [](auto x, auto y2) {
        return std::chrono::duration<double, std::milli>(y2 - x).count();
    };

    json coef, se, tst;
    for (int j = 0; j < r.k; ++j) {
        coef[r.names[j]] = r.beta[j];
        se[r.names[j]] = r.se[j];
        tst[r.names[j]] = tstat(r.beta[j], r.se[j]);
    }
    json absorbed = json::object();
    for (size_t g = 0; g < md.fe.size(); ++g) absorbed[md.fe[g].name] = r.dof.fe_levels[g];

    // Stata e(N): the row count unweighted and under aw/pw, Σw under fw/iw. Emitted
    // through the SAME "n_obs" key either way — that is the number the header prints
    // as "Number of obs", and reporting rows there under fw would contradict Stata.
    json n_obs = r.n;
    if (r.n_eff > 0.0) n_obs = static_cast<int64_t>(std::floor(r.n_eff + 1e-9));  // Stata truncates
    json out = {
        {"estimator", "feols"},
        {"depvar", y},
        {"n_obs", n_obs},
        {"n_dropped", r.n_dropped},
        {"n_dropped_singletons", md.n_singletons_dropped},
        {"coef", coef},
        {"se", se},
        {"tstat", tst},
        {"vcov_type", r.vcov_type},
        {"n_clusters", r.n_clusters},
        {"cluster_dof_k", r.cluster_dof_k},
        {"nested_fe", r.nested_fe},
        {"vcov_npd_fix", r.vcov_npd_fix},
        {"absorbed", absorbed},
        {"k_vars", r.dof.k_vars},
        {"k_absorbed", r.dof.k_fe},
        {"fe_redundant", r.dof.fe_redundant},
        {"fe_exact", r.dof.fe_exact},
        {"dof_resid", r.dof.dof_resid},
        {"r2_within", r.r2_within},
        {"sigma2", r.sigma2},
        // Model-fit scalars (Stata `regress` e()); since contract A 1.5.0 (§17).
        {"r2", r.r2},
        {"adj_r2", r.r2_a},
        {"rmse", r.rmse},
        {"mss", r.mss},
        {"rss", r.rss},
        {"df_m", r.df_m},
        {"df_r", r.df_r},
        {"diagnostics",
         {{"demean_iters", r.demean_iters},
          {"demean_resid", r.demean_resid},
          {"demean_converged", r.demean_converged}}},
        {"timing_ms", {{"load", ms(t0, t1)}, {"build", ms(t1, t2)}, {"fit", ms(t2, t3)}}},
    };
    // Weighted extras. Emitted ONLY when weights were actually used, so an unweighted
    // response stays byte-identical to before weights existed (the freeze pins hold).
    if (r.n_eff > 0.0) {
        const char* wt = "aw";
        switch (r.weight_kind) {
            case cecon::WeightKind::Frequency:   wt = "fw"; break;
            case cecon::WeightKind::Importance:  wt = "iw"; break;
            case cecon::WeightKind::Probability: wt = "pw"; break;
            default: break;
        }
        out["weight"] = {{"var", weight_var}, {"type", wt}, {"sum_w", r.sum_w}};
        out["n_rows"] = r.n;                        // rows kept, when N is Σw instead
        out["dof_resid_exact"] = r.dof_resid_exact;  // fractional under iw
    }
    // Multiway clustering: each dimension's own G, in --cluster order (reghdfe's
    // e(N_clust1..d); "n_clusters" above stays the smallest, which drives df_r = G-1).
    // Emitted only under --vcov twoway/multiway, so every other reply is byte-identical.
    if (vt == Vcov::Multiway) {
        json by = json::array();
        for (const auto& c : md.cluster) by.push_back(c.n_levels);
        out["n_clusters_by"] = by;
    }
    // Stata's built-in multiway convention only (FU-388): e(rank) of the clipped
    // variance and the e(kcluster) combinations, in Stata's column order.
    if (r.vcov_rank >= 0) out["vcov_rank"] = r.vcov_rank;
    add_kcluster(out, r.kcluster);
    // The rest of that built-in multiway e() (new keys, multiway_subset_factors fits
    // only; e_Vmb also after a one-way robust/cluster fit, BUG-1043 -- unweighted or
    // fw without FE, unweighted with FE): e(V_modelbased) as `e_Vmb` in e_V's shape
    // (under --emit-v), and under a
    // single absorbed FE the sums of squares and group sizes areg/xtreg,fe post
    // (e(tss)/e(tss_w)/e(mss_w), and
    // e(Tbar)/e(g_min)/e(g_avg)/e(g_max) over the panel sizes).
    if (emit_v && !r.vcov_modelbased.empty()) {
        json tmp;
        add_e_v(tmp, r.names, r.vcov_modelbased);
        out["e_Vmb"] = std::move(tmp["e_V"]);
    }
    // BUG-1041: every absorbed fit carries both sums, because areg and reghdfe
    // post e(mss) = e(tss) - e(rss) about the overall mean and e(tss) (reghdfe also
    // e(tss_within)) under every VCE, not only the built-in multiway one.
    if (!md.fe.empty()) {
        out["tss_total"] = r.tss_total;
        out["tss_within"] = r.tss_within;
        // The panel group sizes come from the one producer below (fe_group_sizes,
        // estimation-sample row counts); a second writer here used to be overwritten
        // by it, silently, with different keys.
    }
    // Stata `regress`'s e(beta) (unweighted no-FE OLS only; absent otherwise).
    if (!r.beta_std.empty()) {
        json b = json::object();
        for (const auto& [nm, v] : r.beta_std) b[nm] = v;
        out["beta_std"] = std::move(b);
    }
    add_model_f(out, r.f_stat, r.df_m, r.df_r_exact);  // {"F", ...},{"p_F", ...} (null if undefined)
    // Single-FE panel models (Stata xtreg,fe / areg): the six header scalars —
    // r2_between/r2_overall (since 1.5.1, §19; factor-expansion hole closed in §35 /
    // BUG-156) and sigma_u/sigma_e/rho/corr_u_xb (since §35). One producer for all six.
    if (fes.size() == 1) add_panel_scalars(out, md, panel_means, r);
    // xtreg, fe's e(g_min)/e(g_avg)/e(g_max): the row counts of the panels in the
    // estimation sample (observations per panel). One
    // absorbed factor only; the consumer (PIET's xtreg, fe route) copies them to
    // e(). Additive key, absent otherwise.
    if (fes.size() == 1 && md.fe.size() == 1 && md.fe[0].n_levels > 0) {
        const Factor& f = md.fe[0];
        std::vector<int64_t> cnt(static_cast<size_t>(f.n_levels), 0);
        for (int32_t c : f.codes)
            if (c >= 0 && c < f.n_levels) ++cnt[static_cast<size_t>(c)];
        int64_t gmin = -1, gmax = 0, tot = 0, ng = 0;
        for (int64_t c : cnt) {
            if (c <= 0) continue;
            gmin = gmin < 0 ? c : std::min(gmin, c);
            gmax = std::max(gmax, c);
            tot += c;
            ++ng;
        }
        if (ng > 0)
            out["fe_group_sizes"] = {{"min", gmin}, {"avg", static_cast<double>(tot) / static_cast<double>(ng)},
                                     {"max", gmax}};
    }
    if (level_set) out["level"] = level;  // emitted only when --level was passed (byte-stable default)
    {
        double dof = (r.n_clusters > 0) ? (r.n_clusters - 1.0)
                                        : static_cast<double>(r.dof.dof_resid);
        add_inference(out, r.names, r.beta, r.se, dof, false, level);
    }
    // Stata `regress` on an exact fit (feols_fit's exact-fit rule): V is 0, and
    // the coefficient table carries t, P>|t| and the interval as missing
    // (StataBE `regress yx x` with yx = 3 + 2x: r(table) se/t/pvalue/ll/ul `.`).
    // tstat() and add_inference would post t 0, p 1 and ll = ul = b instead.
    if (r.exact_fit) {
        for (int j = 0; j < r.k; ++j) {
            if (r.se[static_cast<size_t>(j)] != 0.0) continue;
            const std::string& nm = r.names[static_cast<size_t>(j)];
            out["tstat"][nm] = nullptr;
            out["pvalue"][nm] = nullptr;
            out["ci_lo"][nm] = nullptr;
            out["ci_hi"][nm] = nullptr;
        }
    }
    add_omitted(out, r.omitted, r.full_names);
    if (emit_v) add_e_v(out, r.names, r.vcov_matrix);  // opt-in: absent key by default
    add_warnings(out, md.warnings);  // BUG-112(8): absent-key when empty
    return out;
}

// BUG-100 (IV completion): Stata's ivregress/ivgmm display lists ENDOGENOUS
// regressors before exogenous ones; cobra's iv2sls builds coefs in internal
// [exog,endog] design-matrix order. Permute the DISPLAY only — vcov, first-
// stage F, KP/Hansen/Cragg-Donald diagnostics all keep the untouched internal
// order. Row order: (intercept)/_cons first (cobra's own convention, D1b), then
// endogenous (r.names order restricted to the endog list), then the exogenous.
// Plain OLS/feols never call this path, so their typed-varlist order is intact.
static std::vector<size_t> iv_display_order(const std::vector<std::string>& names,
                                            const std::vector<std::string>& endog) {
    auto is_cons = [](const std::string& s) { return s == "(intercept)" || s == "_cons"; };
    auto is_endog = [&](const std::string& s) {
        return std::find(endog.begin(), endog.end(), s) != endog.end();
    };
    std::vector<size_t> ord;
    ord.reserve(names.size());
    for (size_t j = 0; j < names.size(); ++j) if (is_cons(names[j])) ord.push_back(j);
    for (size_t j = 0; j < names.size(); ++j)
        if (!is_cons(names[j]) && is_endog(names[j])) ord.push_back(j);
    for (size_t j = 0; j < names.size(); ++j)
        if (!is_cons(names[j]) && !is_endog(names[j])) ord.push_back(j);
    return ord;
}

// BUG-1225: the included regressors the FE absorption annihilated, as omitted rows
// (b 0, se 0) at their varlist place in the display order -- StataBE ivreghdfe's
// e(b) is `x1 x2 x3` with x2 "(omitted)". Call after add_inference. No-op otherwise.
static void add_iv_omitted_exog(json& out, const cecon::IvResult& r,
                                const std::vector<std::string>& endog) {
    if (r.omitted_exog.empty()) return;
    std::vector<std::string> full;
    for (size_t idx : iv_display_order(r.full_names, endog)) full.push_back(r.full_names[idx]);
    add_omitted(out, r.omitted_exog, full);
}

// ---- ivreg2 `savefirst` JSON ------------------------------------------------
// The stored-estimate NAME ivreg2 mints for endogenous regressor `v`
// (as ivreg2 mints it), measured by tests/eval/golden/aer_b4/ivreg2-savefirst.log:
//   * with savefprefix(fs_)     -> "fs_mpg"                (arm A: e(firsteqs) = "fs_mpg fs_turn")
//   * with no prefix            -> "_ivreg2_" + v          (arm B: "_ivreg2_L_mpg")
//   * every "." becomes "_"     -> `L.mpg` -> `L_mpg`      (arm B, same cell)
//   * longer than 32 characters -> Stata's `est store` is `capture`d by ivreg2, so the
//     equation is SILENTLY NOT STORED and drops out of e(firsteqs) (arm G: a 30-char
//     prefix + "mpg" = 33 gives e(firsteqs) EMPTY and rc 111 on restore).
// The length rule is reproduced rather than papered over: a consumer that registered
// a 33-character name would diverge from Stata on the very next `estimates restore`.
std::string ivreg2_first_eqname(const std::string& prefix, const std::string& endog) {
    std::string nm = (prefix.empty() ? std::string("_ivreg2_") : prefix) + endog;
    for (char& c : nm) if (c == '.') c = '_';
    return nm;
}

// Serialize IvResult::first_stage into the JSON a consumer registers as stored
// estimates, plus the `firsteqs` name list (short exactly where Stata's is short).
void add_first_stage(json& out, const cecon::IvResult& r, const std::string& prefix) {
    if (r.first_stage.empty()) return;   // absent key unless savefirst was asked for
    json eqs = json::array();
    json fs = json::object();
    for (const cecon::IvFirstStage& f : r.first_stage) {
        const std::string eqname = ivreg2_first_eqname(prefix, f.depvar);
        json coef = json::object(), se = json::object(), tst = json::object();
        for (size_t j = 0; j < f.names.size(); ++j) {
            coef[f.names[j]] = f.beta[j];
            se[f.names[j]] = f.se[j];
            tst[f.names[j]] = tstat(f.beta[j], f.se[j]);
        }
        json e = json{
            {"depvar", f.depvar},
            {"eqname", eqname},
            {"stored", eqname.size() <= 32},   // arm G: >32 chars is silently NOT stored
            {"names", f.names},
            {"coef", coef},
            {"se", se},
            {"tstat", tst},
            {"n_obs", f.n},
            {"df_r", f.df_r},
            {"df_m", f.df_m},
            {"k_eq", f.k_eq},
            {"rmse", f.rmse},
            {"rss", f.rss},
            {"vcov_type", f.vcov_type},
            {"n_clusters", f.n_clusters},
            // ivreg2 metadata, so a consumer can post an estimate that
            // answers e(cmd)/e(model) the way a restored ivreg2 first stage does.
            {"cmd", "ivreg2"},
            {"model", "first"},
        };
        // t/p/CI off df_r — which under cluster() is N-K, NOT M-1 (as ivreg2 posts it;
        // arm E measured dfr_fs = 70 against `regress, cluster()`'s 8 on the same fit).
        add_inference(e, f.names, f.beta, f.se, static_cast<double>(f.df_r), /*use_z=*/false);
        // The stored estimate's e(V), in `names` order, and ivreg2's e(first) column
        // (ivreg2_first_stat_rows() order; absent where it was not computed).
        add_e_v(e, f.names, f.vcov_matrix);
        if (!f.ffirst.empty()) e["ffirst"] = f.ffirst;
        fs[f.depvar] = std::move(e);
        if (eqname.size() <= 32) eqs.push_back(eqname);
    }
    out["first_stage"] = std::move(fs);
    out["firsteqs"] = std::move(eqs);
}

// The iv weight column and its sample mask (see run_iv); nullptr when unweighted.
static const double* iv_weight_mask(const Dataset& ds, const std::string& weight_var,
                                    cecon::WeightKind weight_kind,
                                    const std::vector<char>*& if_mask, std::vector<char>& wmask) {
    if (weight_var.empty()) return nullptr;
    if (weight_kind != cecon::WeightKind::Analytic && weight_kind != cecon::WeightKind::Probability)
        throw EngineError("bad_input", "iv: only --wtype aw and pw are implemented");
    if (!ds.has(weight_var))
        throw EngineError("bad_input", "iv: weight variable '" + weight_var + "' not found");
    const cecon::Column& wc = ds.col(weight_var);
    if (wc.dtype != cecon::DType::F64)
        throw EngineError("bad_input", "iv: weight variable '" + weight_var + "' must be numeric");
    const double* wcol = wc.f64();
    const int64_t nr = ds.n_rows();
    wmask.assign(static_cast<size_t>(nr), 1);
    for (int64_t i = 0; i < nr; ++i) {
        if (if_mask && !(*if_mask)[static_cast<size_t>(i)]) { wmask[static_cast<size_t>(i)] = 0; continue; }
        const double w = wcol[i];
        if (std::isnan(w) || w == 0.0) { wmask[static_cast<size_t>(i)] = 0; continue; }
        if (w < 0)
            throw EngineError("bad_input", "iv: weight '" + weight_var + "' has a negative value");
    }
    if_mask = &wmask;
    return wcol;
}

static void iv_attach_weights(IvModelData& md, const double* wcol, cecon::WeightKind kind) {
    if (!wcol) return;
    md.weights.resize(static_cast<size_t>(md.n));
    for (int64_t r = 0; r < md.n; ++r)
        md.weights[static_cast<size_t>(r)] = wcol[md.keep[static_cast<size_t>(r)]];
    md.weight_kind = kind;
}

json run_iv(const std::string& data, const std::string& y,
            const std::vector<std::string>& exog, const std::vector<std::string>& endog,
            const std::vector<std::string>& instruments,
            const std::vector<std::string>& fes, const std::string& vcov_type,
            const std::vector<std::string>& clusters, bool gmm,
            const cecon::TsMeta* ts, bool drop_singleton,
            const std::vector<char>* if_mask, bool small, bool savefirst,
            const std::string& savefprefix, EsampleRows* esample, bool emit_v,
            bool multiway_subset_factors, bool no_intercept, const std::string& weight_var,
            cecon::WeightKind weight_kind, bool multiway_no_psd_fix, bool dofminus_fe) {
    auto t0 = std::chrono::steady_clock::now();
    const Dataset& ds = acquire_data(data);
    auto t1 = std::chrono::steady_clock::now();
    // [aw=]/[pw=]: a missing or zero weight leaves the row out of the sample (Stata's
    // marksample), a negative one is an error, and the surviving weights ride on md
    // for iv2sls_fit. The mask joins --keepif's, so e(sample) and every count follow.
    std::vector<char> wmask;
    const double* wcol = iv_weight_mask(ds, weight_var, weight_kind, if_mask, wmask);
    IvModelData md = build_iv_model(ds, y, exog, endog, instruments, fes, clusters, ts,
                                    drop_singleton, if_mask, no_intercept);
    iv_attach_weights(md, wcol, weight_kind);
    md.multiway_no_psd_fix = multiway_no_psd_fix;
    md.dofminus_fe = dofminus_fe;
    if (esample) { esample->keep = md.keep; esample->loaded = &ds; }  // e(sample) T1a
    auto t2 = std::chrono::steady_clock::now();
    Vcov vt = Vcov::IID;
    if (vcov_type == "hc1" || vcov_type == "HC1") vt = Vcov::HC1;
    else if (vcov_type == "cluster") vt = Vcov::Cluster;
    else if (vcov_type == "twoway" || vcov_type == "multiway") vt = Vcov::Multiway;
    // xtivreg2: "cluster option not supported if a panel spans more than one
    // cluster" (r(198)), checked on the estimation sample (after singletons).
    if (dofminus_fe && vt == Vcov::Cluster && !md.cluster.empty())
        for (const auto& f : md.fe)
            if (!cecon::fe_nested_in_cluster(f, md.cluster[0]))
                throw EngineError("cluster_spans_panel",
                                  "cluster option not supported if a panel spans more than one cluster");
    // e(tss), which ivregress posts hidden: the centred sum of squares of the depvar
    // over the estimation sample. Only for the plain fit (no absorbed effect, no
    // weights), the one PIET posts it for, and read here because the fit may
    // transform md.y in place. PIET used to sum it from its working frame, writing
    // the whole frame to disk to read two columns (BUG-1088). Sent as
    // `tss_centred`, not `tss`: PIET falls back from e(name) to the result key of
    // the same name, so a `tss` key would surface as e(tss) after the fits PIET
    // posts none for (noconstant takes the uncentred sum; ivreg2 posts none).
    double tss = std::numeric_limits<double>::quiet_NaN();
    if (md.fe.empty() && md.weights.empty() && md.n > 0) {
        double sum = 0;
        for (double v : md.y) sum += v;
        const double ybar = sum / static_cast<double>(md.n);
        tss = 0;
        for (double v : md.y) tss += (v - ybar) * (v - ybar);
    }
    IvResult r = iv2sls_fit(md, vt, 0, gmm, small, savefirst, multiway_subset_factors);
    auto t3 = std::chrono::steady_clock::now();
    auto ms = [](auto x, auto y2) {
        return std::chrono::duration<double, std::milli>(y2 - x).count();
    };

    json coef, se, tst;
    std::vector<size_t> ord = iv_display_order(r.names, endog);
    std::vector<std::string> pnames; std::vector<double> pbeta, pse;
    pnames.reserve(ord.size()); pbeta.reserve(ord.size()); pse.reserve(ord.size());
    for (size_t idx : ord) {
        coef[r.names[idx]] = r.beta[idx];
        se[r.names[idx]] = r.se[idx];
        tst[r.names[idx]] = tstat(r.beta[idx], r.se[idx]);
        pnames.push_back(r.names[idx]); pbeta.push_back(r.beta[idx]); pse.push_back(r.se[idx]);
    }
    json absorbed = json::object();
    for (size_t g = 0; g < md.fe.size(); ++g) absorbed[md.fe[g].name] = r.dof.fe_levels[g];
    json fstage = json::object();
    for (size_t j = 0; j < r.endog_names.size(); ++j)
        fstage[r.endog_names[j]] = r.first_stage_F[j];

    json out = json{
        {"estimator", gmm ? "ivgmm" : "iv2sls"},
        {"depvar", y},
        {"endogenous", r.endog_names},
        {"n_obs", r.n},
        {"n_dropped", r.n_dropped},
        {"n_dropped_singletons", md.n_singletons_dropped},
        {"coef", coef},
        {"se", se},
        {"tstat", tst},
        {"vcov_type", r.vcov_type},
        {"n_clusters", r.n_clusters},
        {"cluster_dof_k", r.cluster_dof_k},
        {"nested_fe", r.nested_fe},
        {"vcov_npd_fix", r.vcov_npd_fix},
        {"absorbed", absorbed},
        {"k_vars", r.dof.k_vars},
        {"k_absorbed", r.dof.k_fe},
        {"dof_resid", r.dof.dof_resid},
        {"first_stage_F", fstage},      // homoskedastic (iid) first-stage F per endog
        {"first_stage_F_iid", fstage},  // explicit alias; kp_wald_F below is the robust/cluster F
        {"kp_wald_F", r.kp_wald_F},  // Kleibergen-Paap rk Wald F (cluster/robust), matches ivreghdfe e(widstat)
        {"small", small},
        {"diagnostics",
         {{"demean_iters", r.demean_iters},
          {"demean_resid", r.demean_resid},
          {"demean_converged", r.demean_converged}}},
        {"timing_ms", {{"load", ms(t0, t1)}, {"build", ms(t1, t2)}, {"fit", ms(t2, t3)}}},
    };
    // §35: excluded instruments omitted because the FE absorption annihilated them
    // (ivreghdfe drops them silently). Absent-key when nothing was omitted, so a fit
    // with a full-rank instrument set stays byte-identical. These are INSTRUMENT
    // names, not coefficients — deliberately NOT routed through add_omitted(), which
    // would fabricate zero coef/se rows for something that never had a coefficient.
    if (!r.omitted.empty()) out["omitted"] = r.omitted;
    // Model Wald chi2 + rmse (Stata ivregress e(chi2)/e(rmse)); NaN-guarded like
    // hansen_j/kp_lm/eff_F below.
    if (!std::isnan(r.rmse)) out["rmse"] = r.rmse;
    if (!std::isnan(tss)) out["tss_centred"] = tss;
    if (!std::isnan(r.chi2)) { out["chi2"] = r.chi2; out["chi2_df"] = r.chi2_df; }
    // Hansen J overidentification test (efficient 2-step GMM objective), matches
    // ivreg2/ivreghdfe e(j)/e(jdf)/e(jp) to ~1e-12 for robust and cluster vcov.
    if (!std::isnan(r.hansen_j)) {
        out["hansen_j"] = r.hansen_j;
        out["hansen_j_df"] = r.hansen_j_df;
        out["hansen_j_p"] = r.hansen_j_p;
    }
    // Kleibergen-Paap rk LM underidentification (kendog==1), matches ranktest e(idstat).
    if (!std::isnan(r.kp_lm)) {
        out["kp_lm"] = r.kp_lm;
        out["kp_lm_df"] = r.kp_lm_df;
        out["kp_lm_p"] = r.kp_lm_p;
    }
    // Montiel-Olea-Pflueger effective F (kendog==1), matches weakivtest r(F_eff).
    if (!std::isnan(r.eff_F)) out["eff_F"] = r.eff_F;
    // Multi-endogenous (>=2) homoskedastic weak-IV F = Cragg-Donald min-eigenvalue F
    // (ivreg2 e(widstat) under no robust/cluster). No-FE only (validated exact).
    if (md.kendog >= 2 && md.fe.empty()) {
        double cd = cragg_donald_F(md.Xendog, md.kendog, md.Xexog, md.kexog, md.Zinst, md.kinst, md.n);
        if (cd >= 0) out["cragg_donald_F"] = cd;
    }
    // FU-387/FU-403: ivregress's multiway e(rank) (the
    // rank of the clipped variance) and e(kcluster). Multiway only, new keys only.
    // Each dimension's own G, in --cluster order (ivreg2 e(N_clust1), e(N_clust2)),
    // as feols emits it; "n_clusters" stays the smallest. Multiway only, a new key.
    if (vt == Vcov::Multiway) {
        json by = json::array();
        for (const auto& c : md.cluster) by.push_back(c.n_levels);
        out["n_clusters_by"] = by;
    }
    if (vt == Vcov::Multiway && !gmm) {
        out["vcov_rank"] = multiway_vcov_rank(r.vcov_matrix, r.k);
        add_kcluster(out, multiway_cluster_combos(md.cluster));
        // e(p) of the multiway Wald chi2 (StataBE e(p) 1.5107017713e-183 on the
        // w2s_multiway_class fixture). New key, multiway only.
        if (!std::isnan(r.chi2)) out["chi2_p"] = chi2_pvalue(r.chi2, r.chi2_df);
    }
    {
        double dof = (r.n_clusters > 0) ? (r.n_clusters - 1.0)
                                        : static_cast<double>(r.dof.dof_resid);
        // GMM's SEs are always the asymptotic sandwich regardless of --small, so
        // z inference is the coherent match; 2SLS is asymptotic unless --small.
        add_inference(out, pnames, pbeta, pse, dof, gmm || !small);
    }
    add_iv_omitted_exog(out, r, endog);
    // ivreghdfe counts such a regressor among the dropped instruments too, so the
    // §35 list above keeps its instruments and gains the regressors.
    if (!r.omitted_exog.empty() && !r.omitted.empty()) {
        std::vector<std::string> all = r.omitted;
        all.insert(all.end(), r.omitted_exog.begin(), r.omitted_exog.end());
        out["omitted"] = all;
    }
    add_first_stage(out, r, savefprefix);   // absent key unless --savefirst
    if (emit_v) add_e_v(out, r.names, r.vcov_matrix, &ord);  // reply's coef order; absent by default
    add_warnings(out, md.warnings);  // BUG-112(8): absent-key when empty
    return out;
}

// Multi-outcome 2SLS batch: same endog/instruments/exog/FE design, several
// outcomes, sharing the FE absorption and instrument algebra (only the per-y
// solve/vcov/Hansen-J are recomputed). Shared instrument diagnostics (first-stage
// F, KP rk Wald F, effective F, KP rk LM, Cragg-Donald) sit at the top level
// since they depend only on the instruments, not the outcome.
json run_iv_multi(const std::string& data, const std::vector<std::string>& ys,
                  const std::vector<std::string>& exog, const std::vector<std::string>& endog,
                  const std::vector<std::string>& instruments,
                  const std::vector<std::string>& fes, const std::string& vcov_type,
                  const std::vector<std::string>& clusters, bool gmm,
                  const cecon::TsMeta* ts, bool drop_singleton,
                  const std::vector<char>* if_mask, bool small, bool savefirst,
                  const std::string& savefprefix, EsampleRows* esample,
                  const std::string& weight_var, cecon::WeightKind weight_kind) {
    auto t0 = std::chrono::steady_clock::now();
    const Dataset& ds = acquire_data(data);
    auto t1 = std::chrono::steady_clock::now();
    std::vector<char> wmask;  // as in run_iv
    const double* wcol = iv_weight_mask(ds, weight_var, weight_kind, if_mask, wmask);
    IvModelData md = build_iv_model(ds, ys[0], exog, endog, instruments, fes, clusters, ts, drop_singleton, if_mask);
    iv_attach_weights(md, wcol, weight_kind);
    // The batch refuses (common_sample) unless every outcome is present on these rows,
    // so ys[0]'s kept rows ARE the sample of every outcome in the response.
    if (esample) { esample->keep = md.keep; esample->loaded = &ds; }  // e(sample) T1a
    // Gather every outcome aligned to the common (ys[0]) sample.
    std::vector<std::vector<double>> yvecs;
    for (const auto& yn : ys) {
        const double* yp = require_num(ds, "iv", yn);
        std::vector<double> v(static_cast<size_t>(md.n));
        for (int64_t i = 0; i < md.n; ++i) {
            double val = yp[md.keep[static_cast<size_t>(i)]];
            if (std::isnan(val))
                throw EngineError("common_sample",
                    "iv --y batch: outcome '" + yn + "' is missing on rows kept for '" + ys[0] +
                    "'. Multi-outcome batch needs a common sample; run outcomes separately or pre-filter.",
                    yn);
            v[static_cast<size_t>(i)] = val;
        }
        yvecs.push_back(std::move(v));
    }
    auto t2 = std::chrono::steady_clock::now();
    Vcov vt = Vcov::IID;
    if (vcov_type == "hc1" || vcov_type == "HC1") vt = Vcov::HC1;
    else if (vcov_type == "cluster") vt = Vcov::Cluster;
    else if (vcov_type == "twoway" || vcov_type == "multiway") vt = Vcov::Multiway;
    auto results = iv2sls_fit_multi(md, yvecs, vt, 0, gmm, small, savefirst);
    auto t3 = std::chrono::steady_clock::now();
    auto ms = [](auto x, auto y2) {
        return std::chrono::duration<double, std::milli>(y2 - x).count();
    };
    const IvResult& r0 = results[0];

    json absorbed = json::object();
    for (size_t g = 0; g < md.fe.size(); ++g) absorbed[md.fe[g].name] = r0.dof.fe_levels[g];
    json fstage = json::object();
    for (size_t j = 0; j < r0.endog_names.size(); ++j)
        fstage[r0.endog_names[j]] = r0.first_stage_F[j];

    json outcomes = json::array();
    for (size_t o = 0; o < results.size(); ++o) {
        const IvResult& r = results[o];
        json coef = json::object(), se = json::object(), tst = json::object();
        std::vector<size_t> ord = iv_display_order(r.names, endog);
        std::vector<std::string> pnames; std::vector<double> pbeta, pse;
        pnames.reserve(ord.size()); pbeta.reserve(ord.size()); pse.reserve(ord.size());
        for (size_t idx : ord) {
            coef[r.names[idx]] = r.beta[idx];
            se[r.names[idx]] = r.se[idx];
            tst[r.names[idx]] = tstat(r.beta[idx], r.se[idx]);
            pnames.push_back(r.names[idx]); pbeta.push_back(r.beta[idx]); pse.push_back(r.se[idx]);
        }
        json oj = {{"depvar", ys[o]}, {"coef", coef}, {"se", se}, {"tstat", tst},
                   {"n_obs", r.n}, {"vcov_type", r.vcov_type}, {"n_clusters", r.n_clusters},
                   {"dof_resid", r.dof.dof_resid}};
        if (!std::isnan(r.rmse)) oj["rmse"] = r.rmse;
        if (!std::isnan(r.chi2)) { oj["chi2"] = r.chi2; oj["chi2_df"] = r.chi2_df; }
        if (!std::isnan(r.hansen_j)) {
            oj["hansen_j"] = r.hansen_j;
            oj["hansen_j_df"] = r.hansen_j_df;
            oj["hansen_j_p"] = r.hansen_j_p;
        }
        double dof = (r.n_clusters > 0) ? (r.n_clusters - 1.0) : static_cast<double>(r.dof.dof_resid);
        add_inference(oj, pnames, pbeta, pse, dof, gmm || !small);
        add_iv_omitted_exog(oj, r, endog);  // BUG-1225
        outcomes.push_back(oj);
    }

    json out = {
        {"estimator", gmm ? "ivgmm_multi" : "iv2sls_multi"},
        {"n_outcomes", static_cast<int>(ys.size())},
        {"endogenous", r0.endog_names},
        {"n_obs", r0.n},
        {"n_dropped", r0.n_dropped},
        {"n_dropped_singletons", md.n_singletons_dropped},
        {"shared_absorption", true},
        {"absorbed", absorbed},
        {"k_vars", r0.dof.k_vars},
        {"k_absorbed", r0.dof.k_fe},
        {"dof_resid", r0.dof.dof_resid},
        {"vcov_type", r0.vcov_type},
        {"n_clusters", r0.n_clusters},
        {"first_stage_F", fstage},
        {"first_stage_F_iid", fstage},
        {"kp_wald_F", r0.kp_wald_F},
        {"small", small},
        {"outcomes", outcomes},
        {"timing_ms", {{"load", ms(t0, t1)}, {"build", ms(t1, t2)}, {"fit", ms(t2, t3)}}},
    };
    if (!std::isnan(r0.kp_lm)) {
        out["kp_lm"] = r0.kp_lm;
        out["kp_lm_df"] = r0.kp_lm_df;
        out["kp_lm_p"] = r0.kp_lm_p;
    }
    if (!std::isnan(r0.eff_F)) out["eff_F"] = r0.eff_F;
    // §35: the instrument set is SHARED across outcomes, so the omission is a
    // top-level fact, not a per-outcome one. Absent-key when nothing was omitted.
    if (!r0.omitted.empty()) out["omitted"] = r0.omitted;
    if (md.kendog >= 2 && md.fe.empty()) {
        double cd = cragg_donald_F(md.Xendog, md.kendog, md.Xexog, md.kexog, md.Zinst, md.kinst, md.n);
        if (cd >= 0) out["cragg_donald_F"] = cd;
    }
    // Shared across outcomes: the first stage never involves y (ivreg2 stores ONE
    // set of first-stage equations per parent command, whatever the outcome).
    add_first_stage(out, r0, savefprefix);   // absent key unless --savefirst
    add_warnings(out, md.warnings);  // BUG-112(8): absent-key when empty
    return out;
}

// Multi-outcome FE-OLS batch: share the FE absorption of X across outcomes.
json run_feols_multi(const std::string& data, const std::vector<std::string>& ys,
                     const std::vector<std::string>& xs, const std::vector<std::string>& fes,
                     const std::string& vcov_type, const std::vector<std::string>& clusters,
                     bool drop_singleton, const cecon::TsMeta* ts,
                     const std::vector<char>* if_mask, bool no_intercept, double level,
                     bool level_set, EsampleRows* esample, bool collinear_forward) {
    const Dataset& ds = acquire_data(data);
    ModelData md = build_model(ds, ys[0], xs, fes, clusters, "", drop_singleton, ts, if_mask);
    md.collinear_forward = collinear_forward;  // BUG-1067
    // Same common-sample rule as run_iv_multi: ys[0]'s kept rows are every outcome's.
    if (esample) { esample->keep = md.keep; esample->loaded = &ds; }  // e(sample) T1a
    int64_t n = md.dm.n;
    std::vector<std::vector<double>> yvecs;
    for (const auto& yn : ys) {
        const double* yp = require_num(ds, "feols", yn);
        std::vector<double> v(static_cast<size_t>(n));
        for (int64_t i = 0; i < n; ++i) {
            double val = yp[md.keep[static_cast<size_t>(i)]];
            if (std::isnan(val))
                throw EngineError("common_sample",
                    "feols --y batch: outcome '" + yn + "' is missing on rows kept for '" + ys[0] +
                    "'. Multi-outcome batch needs a common sample; run outcomes separately or "
                    "pre-filter to a shared sample.", yn);
            v[static_cast<size_t>(i)] = val;
        }
        yvecs.push_back(std::move(v));
    }
    Vcov vt = Vcov::IID;
    if (vcov_type == "hc1" || vcov_type == "HC1") vt = Vcov::HC1;
    else if (vcov_type == "cluster") vt = Vcov::Cluster;
    else if (vcov_type == "twoway" || vcov_type == "multiway") vt = Vcov::Multiway;
    auto t0 = std::chrono::steady_clock::now();
    auto results = feols_fit_multi(md, yvecs, vt, 0, no_intercept);
    auto t1 = std::chrono::steady_clock::now();
    json absorbed = json::object();
    for (size_t gI = 0; gI < md.fe.size(); ++gI) absorbed[md.fe[gI].name] = md.fe[gI].n_levels;
    json arr = json::array();
    for (size_t o = 0; o < results.size(); ++o) {
        const FeolsResult& r = results[o];
        json coef = json::object(), se = json::object(), tst = json::object();
        for (int j = 0; j < r.k; ++j) {
            coef[r.names[static_cast<size_t>(j)]] = r.beta[static_cast<size_t>(j)];
            se[r.names[static_cast<size_t>(j)]] = r.se[static_cast<size_t>(j)];
            tst[r.names[static_cast<size_t>(j)]] = tstat(r.beta[static_cast<size_t>(j)], r.se[static_cast<size_t>(j)]);
        }
        json oj = {{"depvar", ys[o]}, {"coef", coef}, {"se", se}, {"tstat", tst},
                   {"n_obs", r.n}, {"r2_within", r.r2_within}, {"sigma2", r.sigma2},
                   {"vcov_type", r.vcov_type}, {"n_clusters", r.n_clusters},
                   {"dof_resid", r.dof.dof_resid},
                   // Model-fit scalars (Stata regress e()); since contract A 1.5.0 (§17).
                   {"r2", r.r2}, {"adj_r2", r.r2_a}, {"rmse", r.rmse},
                   {"mss", r.mss}, {"rss", r.rss}, {"df_m", r.df_m}, {"df_r", r.df_r}};
        add_model_f(oj, r.f_stat, r.df_m, r.df_r);
        double dof = (r.n_clusters > 0) ? (r.n_clusters - 1.0) : static_cast<double>(r.dof.dof_resid);
        add_inference(oj, r.names, r.beta, r.se, dof, false, level);
        add_omitted(oj, r.omitted, r.full_names);
        arr.push_back(oj);
    }
    json out = {{"estimator", "feols_multi"}, {"n_outcomes", static_cast<int>(ys.size())},
                {"n_obs", n}, {"shared_absorption", true}, {"absorbed", absorbed},
                {"outcomes", arr},
                {"time_ms", std::chrono::duration<double, std::milli>(t1 - t0).count()}};
    if (level_set) out["level"] = level;  // emitted only when --level was passed (byte-stable default)
    add_warnings(out, md.warnings);  // BUG-112(8): absent-key when empty
    return out;
}

json run_fepois_emit_v(const std::string& data, const std::string& y,
                       const std::vector<std::string>& xs, const std::vector<std::string>& fes,
                       const std::string& offset, const std::string& vcov_type,
                       const std::vector<std::string>& clusters, bool drop_zero_fe,
                       bool scale_x2, const std::string& weight_var, cecon::WeightKind weight_kind,
                       bool emit_v, const std::string& d_out = std::string(),
                       const std::string& d_name = std::string(),
                       const cecon::TsMeta* ts = nullptr,
                       const std::vector<char>* if_mask = nullptr);

json run_fepois(const std::string& data, const std::string& y,
                const std::vector<std::string>& xs, const std::vector<std::string>& fes,
                const std::string& offset, const std::string& vcov_type,
                const std::vector<std::string>& clusters, bool drop_zero_fe,
                bool scale_x2, const std::string& weight_var, cecon::WeightKind weight_kind) {
    return run_fepois_emit_v(data, y, xs, fes, offset, vcov_type, clusters, drop_zero_fe, scale_x2,
                             weight_var, weight_kind, false);
}

// run_fepois plus the opt-in `--emit-v` (FU-859, the T-E1/BUG-808 shape): with
// emit_v the reply carries `e_V`, the fitted covariance (after the scale(x2)
// dispersion, so it is the matrix the reported se came from), over the design's
// coefficient names. Without it the key is absent and the reply is byte-identical
// to run_fepois's. Only cmd_fepois passes emit_v (cmds_linear.cpp declares this
// function locally; common.h is outside its change set). Absorbed fits retain
// the recovered `_cons` covariance in full_vcov_matrix, including its slope
// cross-covariances; pooled fits keep their original vcov_matrix.
json run_fepois_emit_v(const std::string& data, const std::string& y,
                       const std::vector<std::string>& xs, const std::vector<std::string>& fes,
                       const std::string& offset, const std::string& vcov_type,
                       const std::vector<std::string>& clusters, bool drop_zero_fe,
                       bool scale_x2, const std::string& weight_var, cecon::WeightKind weight_kind,
                       bool emit_v, const std::string& d_out, const std::string& d_name,
                       const cecon::TsMeta* ts, const std::vector<char>* if_mask) {
    auto t0 = std::chrono::steady_clock::now();
    const Dataset& ds = acquire_data(data);
    auto t1 = std::chrono::steady_clock::now();
    // ts / if_mask (BUG-1484): `poisson cnt L.x1 x2 [if]` after xtset -- the operators
    // read the full series, the `if` applies after (cmd_fepois's plan_keepif).
    ModelData md = build_model(ds, y, xs, fes, clusters, offset, true, ts, if_mask,
                               weight_var, weight_kind);
    // ppmlhdfe-parity "fe" separation: drop all-zero FE cells (default on).
    int64_t n_dropped_separation = drop_zero_fe ? drop_zero_outcome_fe_cells(md) : 0;
    auto t2 = std::chrono::steady_clock::now();
    Vcov vt = Vcov::IID;
    if (vcov_type == "hc1" || vcov_type == "HC1" || vcov_type == "robust") vt = Vcov::HC1;
    else if (vcov_type == "cluster") vt = Vcov::Cluster;
    else if (vcov_type == "twoway" || vcov_type == "multiway") vt = Vcov::Multiway;
    // --d-out: ppmlhdfe's `d` (the sum of the absorbed effects) on the fit's rows.
    std::vector<double> fe_sum;
    FepoisResult r = fepois_fit(md, vt, 0, d_out.empty() || md.fe.empty() ? nullptr : &fe_sum);
    if (!d_out.empty()) {
        std::vector<double> dcol(static_cast<size_t>(ds.n_rows()), std::nan(""));
        for (int64_t i = 0; i < md.dm.n; ++i)
            dcol[static_cast<size_t>(md.keep[static_cast<size_t>(i)])] =
                md.fe.empty() ? 0.0 : fe_sum[static_cast<size_t>(i)];
        Frame fr = frame_from_acquired(ds);
        fr.set_f64_column(d_name, std::move(dcol));
        // Always a real file, never commit_frame's resident stage: a fit is a
        // read-only call, and staging would swap the caller's resident head.
        fr.write_hbin(d_out);
    }
    // glm family(poisson) scale(x2): quasi-Poisson — same coefs, SEs scaled by the
    // Pearson dispersion phi = Σ(y-μ)²/μ / (n-k). Pooled only (μ reconstructable).
    double pearson_scale = 1.0;
    if (scale_x2) {
        if (!md.fe.empty()) {
            std::fprintf(stderr, "fepois --scale x2: only supported without --fe (pooled glm).\n");
            return json{{"error", "scale(x2) requires no fixed effects"}};
        }
        // P5: the Pearson-dispersion loop below reconstructs mu from md.dm.X/beta
        // without any weight, so it is not weight-aware — reject rather than
        // silently mis-scale under pweight.
        if (!md.weights.empty())
            throw EngineError("unsupported", "fepois --scale x2: not supported together with weights");
        const int64_t n = md.dm.n;
        const int kk = md.dm.k;
        double pearson = 0;
        for (int64_t i = 0; i < n; ++i) {
            double eta = md.offset.empty() ? 0.0 : md.offset[i];
            for (int j = 0; j < kk; ++j) eta += md.dm.X[static_cast<int64_t>(j) * n + i] * r.beta[j];
            double mu = std::exp(eta);
            double e = md.dm.y[i] - mu;
            if (mu > 0) pearson += e * e / mu;
        }
        double dofp = static_cast<double>(n - r.k);
        pearson_scale = (dofp > 0) ? pearson / dofp : 1.0;
        double sq = std::sqrt(pearson_scale);
        for (auto& v : r.se) v *= sq;
        for (auto& v : r.vcov_matrix) v *= pearson_scale;
    }
    auto t3 = std::chrono::steady_clock::now();
    auto ms = [](auto x, auto y2) {
        return std::chrono::duration<double, std::milli>(y2 - x).count();
    };
    json coef = json::object(), se = json::object(), tst = json::object();  // --no-x --fe: k = 0
    for (int j = 0; j < r.k; ++j) {
        coef[r.names[j]] = r.beta[j];
        se[r.names[j]] = r.se[j];
        tst[r.names[j]] = tstat(r.beta[j], r.se[j]);
    }
    if (r.has_cons) {  // ppmlhdfe recovers a _cons even under absorb(); emit it
        coef["_cons"] = r.cons_b;
        if (r.cons_se_valid) {
            se["_cons"] = r.cons_se;
            tst["_cons"] = tstat(r.cons_b, r.cons_se);
        }
    }
    json absorbed = json::object();
    for (size_t g = 0; g < md.fe.size(); ++g) absorbed[md.fe[g].name] = r.dof.fe_levels[g];
    // e(N) under an fweight is the observations the rows stand for (BUG-1538:
    // StataBE 19.5 `poisson cnt x z [fw=wi]` e(N) 150 on 60 rows).
    int64_t n_obs = r.n;
    if (md.weight_kind == cecon::WeightKind::Frequency && !md.weights.empty()) {
        double sw = 0;
        for (double w : md.weights) sw += w;
        n_obs = static_cast<int64_t>(sw);
    }
    json out = json{
        {"estimator", "fepois"},
        {"depvar", y},
        {"n_obs", n_obs},
        {"n_dropped", r.n_dropped},
        {"n_dropped_singletons", md.n_singletons_dropped},
        {"n_dropped_separation", n_dropped_separation},
        {"has_offset", r.has_offset},
        {"coef", coef},
        {"se", se},
        {"tstat", tst},
        {"vcov_type", r.vcov_type},
        {"n_clusters", r.n_clusters},
        {"cluster_dof_k", r.cluster_dof_k},
        {"nested_fe", r.nested_fe},
        {"absorbed", absorbed},
        {"k_vars", r.dof.k_vars},
        {"k_absorbed", r.dof.k_fe},
        {"dof_resid", r.dof.dof_resid},
        {"deviance", r.deviance},
        // §35 additive: Stata `poisson` e(ll)/e(ll_0)/e(chi2)/e(df_m)/e(r2_p).
        {"ll", r.ll},
        {"ll_0", r.ll_0},
        {"chi2", r.chi2},
        {"chi2_df", r.chi2_df},
        {"r2_p", r.r2_p},
        {"scale_x2", pearson_scale},
        {"irls_iters", r.irls_iters},
        {"converged", r.converged},
        {"separation", r.separation},
        {"diagnostics",
         {{"demean_iters", r.demean_iters}, {"demean_converged", r.demean_converged}}},
        {"timing_ms", {{"load", ms(t0, t1)}, {"build", ms(t1, t2)}, {"fit", ms(t2, t3)}}},
    };
    {
        double dof = (r.n_clusters > 0) ? (r.n_clusters - 1.0) : static_cast<double>(r.dof.dof_resid);
        add_inference(out, r.names, r.beta, r.se, dof);
    }
    add_warnings(out, md.warnings);  // BUG-112(8): absent-key when empty
    // Multiway clustering: each dimension's own G, in --cluster order (ppmlhdfe's
    // e(N_clust1..k), BUG-1481), as feols and iv emit it; "n_clusters" stays the
    // smallest. Emitted only under --vcov twoway/multiway, so every other reply is
    // byte-identical.
    if (vt == Vcov::Multiway) {
        json by = json::array();
        for (const auto& c : md.cluster) by.push_back(c.n_levels);
        out["n_clusters_by"] = by;
    }
    if (emit_v) {
        if (r.has_cons) {
            std::vector<std::string> names = r.names;
            names.push_back("_cons");
            add_e_v(out, names, r.full_vcov_matrix);
        } else {
            add_e_v(out, r.names, r.vcov_matrix);
        }
    }
    return out;
}

// Multi-outcome FE-Poisson batch. Unlike feols/iv, the FE absorption lives
// INSIDE the IRLS loop (weights depend on mu = exp(Xb+offset), hence on y), so it
// cannot be shared across outcomes; what IS shared is the data load, the listwise
// deletion, and the factor construction (build_model once). The all-zero-cell
// separation drop is y-specific, so each outcome works on its own copy.
json run_fepois_multi(const std::string& data, const std::vector<std::string>& ys,
                      const std::vector<std::string>& xs, const std::vector<std::string>& fes,
                      const std::string& offset, const std::string& vcov_type,
                      const std::vector<std::string>& clusters, bool drop_zero_fe, bool scale_x2) {
    auto t0 = std::chrono::steady_clock::now();
    const Dataset& ds = acquire_data(data);
    auto t1 = std::chrono::steady_clock::now();
    ModelData base = build_model(ds, ys[0], xs, fes, clusters, offset);
    std::vector<std::vector<double>> yvecs;
    for (const auto& yn : ys) {
        const double* yp = require_num(ds, "fepois", yn);
        std::vector<double> v(static_cast<size_t>(base.dm.n));
        for (int64_t i = 0; i < base.dm.n; ++i) {
            double val = yp[base.keep[static_cast<size_t>(i)]];
            if (std::isnan(val))
                throw EngineError("common_sample",
                    "fepois --y batch: outcome '" + yn + "' is missing on rows kept for '" + ys[0] +
                    "'. Multi-outcome batch needs a common sample; run outcomes separately or pre-filter.", yn);
            v[static_cast<size_t>(i)] = val;
        }
        yvecs.push_back(std::move(v));
    }
    auto t2 = std::chrono::steady_clock::now();
    Vcov vt = Vcov::IID;
    if (vcov_type == "hc1" || vcov_type == "HC1" || vcov_type == "robust") vt = Vcov::HC1;
    else if (vcov_type == "cluster") vt = Vcov::Cluster;
    else if (vcov_type == "twoway" || vcov_type == "multiway") vt = Vcov::Multiway;

    json arr = json::array();
    for (size_t o = 0; o < ys.size(); ++o) {
        ModelData md = base;          // separation drop + fit mutate per-outcome state
        md.dm.y = yvecs[o];
        int64_t nsep = drop_zero_fe ? drop_zero_outcome_fe_cells(md) : 0;
        FepoisResult r = fepois_fit(md, vt);
        double pearson_scale = 1.0;
        if (scale_x2 && md.fe.empty()) {
            const int64_t n = md.dm.n; const int kk = md.dm.k; double pearson = 0;
            for (int64_t i = 0; i < n; ++i) {
                double eta = md.offset.empty() ? 0.0 : md.offset[i];
                for (int j = 0; j < kk; ++j) eta += md.dm.X[static_cast<int64_t>(j) * n + i] * r.beta[j];
                double mu = std::exp(eta), e = md.dm.y[i] - mu;
                if (mu > 0) pearson += e * e / mu;
            }
            double dofp = static_cast<double>(n - r.k);
            pearson_scale = (dofp > 0) ? pearson / dofp : 1.0;
            double sq = std::sqrt(pearson_scale);
            for (auto& v : r.se) v *= sq;
        }
        json coef = json::object(), se = json::object(), tst = json::object();
        for (int j = 0; j < r.k; ++j) {
            coef[r.names[static_cast<size_t>(j)]] = r.beta[static_cast<size_t>(j)];
            se[r.names[static_cast<size_t>(j)]] = r.se[static_cast<size_t>(j)];
            tst[r.names[static_cast<size_t>(j)]] = tstat(r.beta[static_cast<size_t>(j)], r.se[static_cast<size_t>(j)]);
        }
        json absorbed = json::object();
        for (size_t g = 0; g < md.fe.size(); ++g) absorbed[md.fe[g].name] = r.dof.fe_levels[g];
        json oj = {{"depvar", ys[o]}, {"coef", coef}, {"se", se}, {"tstat", tst},
                   {"n_obs", r.n}, {"n_dropped_singletons", md.n_singletons_dropped},
                   {"n_dropped_separation", nsep}, {"deviance", r.deviance},
                   // §35 additive — kept consistent with the single-outcome run_fepois.
                   {"ll", r.ll}, {"ll_0", r.ll_0}, {"chi2", r.chi2},
                   {"chi2_df", r.chi2_df}, {"r2_p", r.r2_p},
                   {"scale_x2", pearson_scale}, {"irls_iters", r.irls_iters},
                   {"converged", r.converged}, {"separation", r.separation},
                   {"vcov_type", r.vcov_type}, {"n_clusters", r.n_clusters},
                   {"absorbed", absorbed}, {"dof_resid", r.dof.dof_resid}};
        double dof = (r.n_clusters > 0) ? (r.n_clusters - 1.0) : static_cast<double>(r.dof.dof_resid);
        add_inference(oj, r.names, r.beta, r.se, dof);
        arr.push_back(oj);
    }
    auto t3 = std::chrono::steady_clock::now();
    auto ms = [](auto x, auto y2) { return std::chrono::duration<double, std::milli>(y2 - x).count(); };
    json out = {{"estimator", "fepois_multi"}, {"n_outcomes", static_cast<int>(ys.size())},
                {"shared_load", true}, {"outcomes", arr},
                {"timing_ms", {{"load", ms(t0, t1)}, {"build", ms(t1, t2)}, {"fit", ms(t2, t3)}}}};
    add_warnings(out, base.warnings);  // BUG-112(8): absent-key when empty
    return out;
}

}  // namespace cli
}  // namespace cecon
