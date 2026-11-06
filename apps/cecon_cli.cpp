// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// cobra — command-line front end.
//   cobra info  <file.hbin>
//   cobra ols   --data f.hbin --y Y --x X1,X2 [--no-intercept] [--out f.json]
//   cobra feols --data f.hbin --y Y --x X1,X2 [--fe F1,F2] [--vcov iid|hc1|cluster]
//               [--cluster C] [--out f.json]
//   cobra run   <spec.json> [--out f.json]
#ifdef _WIN32
// BUG-050: pull real Unicode argv from the wide command line in main() (below).
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shellapi.h>  // CommandLineToArgvW
#endif

// apps/cmds/common.h: shared helpers + global state (g_argv_consumed, g_format_tidy,
// g_argv, g_data_path, g_provenance, keepif_cleanup, opt/has/...) that main() and
// dispatch_guard() below touch directly. apps/cmds/commands.h: the 66 cmd_*
// prototypes main()'s dispatch if-chain calls. Both declared in namespace
// cecon::cli — see the `using namespace cecon::cli;` below.
#include "cmds/common.h"
#include "cmds/commands.h"
#include "core/eng_counters.h"  // W0: engine mechanism counters, read by the `counters` frame op
#include "core/verb_diag.h"  // COBRA_VERB_DIAG: preserve/restore (stash/unstash) phase timing (measurement-only)

#include <filesystem>
#include <sstream>
#include <set>
#include <unordered_map>

// FIX:PLEVEL-WORKER-RSS-SCOPING — allocator hygiene for the resident serve loop.
// vmmap on a live worker (2026-08-14 decomposition) showed 270–398 MB of a serve
// cobra's ~500 MB private footprint was MALLOC "(empty)" — pages already free()d
// that the allocator retained dirty, which the kernel then reclaims page-by-page
// under pressure (the measured 4.1× fault tax at N=2 fan-out). Marking them
// reusable hands them back for free instead: no compression, no copy, and malloc
// takes them back without a fault if the kernel never needed them.
#if defined(__APPLE__)
#include <malloc/malloc.h>
#include <mach/mach.h>
#include <mach/task_info.h>
#elif defined(__GLIBC__)
#include <malloc.h>
#endif

using namespace cecon::cli;

// E6 dispatch wrapper: run the unconsumed-flag check (check_unconsumed_flags)
// after a command RETURNS SUCCESSFULLY (rc==0) — a command that already failed on
// its own (a usage blurb + rc==2, or an EngineError that unwound past this call)
// is already loud about something being wrong, so this doesn't pile a second,
// possibly-confusing error onto an already-reported one. Every `cmd_XXX` dispatch
// line below is routed through this, `eval` included: cmd_eval's hand-rolled argv
// scanner (its chainable --gen/--egen/--xtset/... mini-language, applied in
// command-line order) does not go through opt()/has()/opt_all(), so it marks the
// ops it applies consumed itself (BUG-1181; it used to be exempt, and an unknown
// flag on `eval` was silently ignored).
// BUG-182: the COBRA_OPTIONS_STRICT drift half is NOT subject to that reasoning and
// must run on the failure paths too, or a flag whose handler rejects it can never be
// checked against its options_table() declaration. rc != 0 is handled here; the
// throwing path is handled in main() (see the catch(...) around dispatch_command).
int dispatch_guard(const std::vector<std::string>& a, int rc) {
    if (rc == 0) check_unconsumed_flags(a);
    else check_options_table_drift(a);
    return rc;
}

// The subcommand dispatch chain, factored out of main() so main() can run it with
// stdout BUFFERED (see the buffering note in main()). Every branch routes through
// dispatch_guard (the E6 unknown-flag net).
int dispatch_command(const std::vector<std::string>& a) {
    if (a[0] == "version" || a[0] == "--version") return dispatch_guard(a, cmd_version(a));
    if (a[0] == "options") return dispatch_guard(a, cmd_options(a));
    if (a[0] == "functions") return dispatch_guard(a, cmd_functions(a));
    if (a[0] == "info" && a.size() >= 2) return dispatch_guard(a, cmd_info(a[1]));
    if (a[0] == "convert") return dispatch_guard(a, cmd_convert(a));
    if (a[0] == "eval") return dispatch_guard(a, cmd_eval(a));
    if (a[0] == "ols") return dispatch_guard(a, cmd_ols(a));
    if (a[0] == "feols") return dispatch_guard(a, cmd_feols(a));
    if (a[0] == "iv") return dispatch_guard(a, cmd_iv(a));
    if (a[0] == "fepois") return dispatch_guard(a, cmd_fepois(a));
    if (a[0] == "wildboot") return dispatch_guard(a, cmd_wildboot(a));
    if (a[0] == "csdid") return dispatch_guard(a, cmd_csdid(a));
    if (a[0] == "xtpois") return dispatch_guard(a, cmd_xtpois(a));
    if (a[0] == "xtreg") return dispatch_guard(a, cmd_xtreg(a));
    if (a[0] == "nbreg") return dispatch_guard(a, cmd_nbreg(a));
    if (a[0] == "xtlogit") return dispatch_guard(a, cmd_xtlogit(a));
    if (a[0] == "test") return dispatch_guard(a, cmd_test(a));
    if (a[0] == "nlcom") return dispatch_guard(a, cmd_nlcom(a));
    if (a[0] == "lincom") return dispatch_guard(a, cmd_lincom(a));
    if (a[0] == "corr" || a[0] == "pwcorr") return dispatch_guard(a, cmd_corr(a));
    if (a[0] == "vif") return dispatch_guard(a, cmd_vif(a));
    if (a[0] == "sdtest") return dispatch_guard(a, cmd_sdtest(a));
    if (a[0] == "ttest") return dispatch_guard(a, cmd_ttest(a));
    if (a[0] == "tabstat") return dispatch_guard(a, cmd_tabstat(a));
    if (a[0] == "tabulate" || a[0] == "tab") return dispatch_guard(a, cmd_tabulate(a));
    if (a[0] == "codebook") return dispatch_guard(a, cmd_codebook(a));
    if (a[0] == "qreg") return dispatch_guard(a, cmd_qreg(a));
    // --- P7 wave ---
    if (a[0] == "ologit") return dispatch_guard(a, cmd_ologit(a));
    if (a[0] == "oprobit") return dispatch_guard(a, cmd_oprobit(a));
    if (a[0] == "mlogit") return dispatch_guard(a, cmd_mlogit(a));
    if (a[0] == "zip") return dispatch_guard(a, cmd_zip(a));
    if (a[0] == "zinb") return dispatch_guard(a, cmd_zinb(a));
    if (a[0] == "fracreg") return dispatch_guard(a, cmd_fracreg(a));
    if (a[0] == "ivprobit") return dispatch_guard(a, cmd_ivprobit(a));
    if (a[0] == "ivtobit") return dispatch_guard(a, cmd_ivtobit(a));
    if (a[0] == "mixed") return dispatch_guard(a, cmd_mixed(a));
    if (a[0] == "stset") return dispatch_guard(a, cmd_stset(a));
    if (a[0] == "stcox") return dispatch_guard(a, cmd_stcox(a));
    if (a[0] == "streg") return dispatch_guard(a, cmd_streg(a));
    if (a[0] == "var") return dispatch_guard(a, cmd_var(a));
    if (a[0] == "xtabond") return dispatch_guard(a, cmd_xtabond(a));
    if (a[0] == "nl") return dispatch_guard(a, cmd_nl(a));
    if (a[0] == "gmm") return dispatch_guard(a, cmd_gmm(a));
    if (a[0] == "mi-impute-regress") return dispatch_guard(a, cmd_mi_impute_regress(a));
    if (a[0] == "mediate") return dispatch_guard(a, cmd_mediate(a));
    if (a[0] == "chow") return dispatch_guard(a, cmd_chow(a));
    if (a[0] == "logit") return dispatch_guard(a, cmd_logit(a));
    if (a[0] == "probit") return dispatch_guard(a, cmd_probit(a));
    if (a[0] == "cloglog") return dispatch_guard(a, cmd_cloglog(a));
    if (a[0] == "tobit") return dispatch_guard(a, cmd_tobit(a));
    if (a[0] == "heckman") return dispatch_guard(a, cmd_heckman(a));
    if (a[0] == "reg3") return dispatch_guard(a, cmd_reg3(a));
    if (a[0] == "margins") return dispatch_guard(a, cmd_margins(a));
    if (a[0] == "predict") return dispatch_guard(a, cmd_predict(a));
    if (a[0] == "hausman") return dispatch_guard(a, cmd_hausman(a));
    if (a[0] == "hettest") return dispatch_guard(a, cmd_hettest(a));
    if (a[0] == "ovtest") return dispatch_guard(a, cmd_ovtest(a));
    if (a[0] == "dwatson") return dispatch_guard(a, cmd_dwatson(a));
    if (a[0] == "durbinalt") return dispatch_guard(a, cmd_durbinalt(a));
    if (a[0] == "bgodfrey") return dispatch_guard(a, cmd_bgodfrey(a));
    if (a[0] == "archlm") return dispatch_guard(a, cmd_archlm(a));
    if (a[0] == "wntestq") return dispatch_guard(a, cmd_wntestq(a));
    if (a[0] == "newey") return dispatch_guard(a, cmd_newey(a));
    if (a[0] == "prais") return dispatch_guard(a, cmd_prais(a));
    if (a[0] == "iv-diag") return dispatch_guard(a, cmd_ivdiag(a));
    if (a[0] == "liml") return dispatch_guard(a, cmd_liml(a));
    if (a[0] == "dfuller") return dispatch_guard(a, cmd_dfuller(a));
    if (a[0] == "pperron") return dispatch_guard(a, cmd_pperron(a));
    if (a[0] == "tsops") return dispatch_guard(a, cmd_tsops(a));
    if (a[0] == "collapse") return dispatch_guard(a, cmd_collapse(a));
    if (a[0] == "mean") return dispatch_guard(a, cmd_mean(a));
    if (a[0] == "proportion") return dispatch_guard(a, cmd_proportion(a));
    if (a[0] == "tsfill") return dispatch_guard(a, cmd_tsfill(a));
    if (a[0] == "import-dta") return dispatch_guard(a, cmd_import_dta(a));
    if (a[0] == "export-dta") return dispatch_guard(a, cmd_export_dta(a));
    if (a[0] == "datasignature") return dispatch_guard(a, cmd_datasignature(a));
    if (a[0] == "gsort") return dispatch_guard(a, cmd_gsort(a));
    if (a[0] == "order") return dispatch_guard(a, cmd_order(a));
    if (a[0] == "rename") return dispatch_guard(a, cmd_rename(a));
    if (a[0] == "clonevar") return dispatch_guard(a, cmd_clonevar(a));
    if (a[0] == "append") return dispatch_guard(a, cmd_append(a));
    if (a[0] == "joinby") return dispatch_guard(a, cmd_joinby(a));
    if (a[0] == "expand") return dispatch_guard(a, cmd_expand(a));
    if (a[0] == "fillin") return dispatch_guard(a, cmd_fillin(a));
    if (a[0] == "cross") return dispatch_guard(a, cmd_cross(a));
    if (a[0] == "contract") return dispatch_guard(a, cmd_contract(a));
    if (a[0] == "merge") return dispatch_guard(a, cmd_merge(a));
    if (a[0] == "duplicates") return dispatch_guard(a, cmd_duplicates(a));
    if (a[0] == "recode") return dispatch_guard(a, cmd_recode(a));
    if (a[0] == "encode") return dispatch_guard(a, cmd_encode(a));
    if (a[0] == "setcol") return dispatch_guard(a, cmd_setcol(a));
    if (a[0] == "reshape") return dispatch_guard(a, cmd_reshape(a));
    if (a[0] == "run") return dispatch_guard(a, cmd_run(a));
    if (a[0] == "grid") return dispatch_guard(a, cmd_grid(a));
    // --- P-QE wave: RD/QE and RD-adjacent estimators ---
    if (a[0] == "rdrobust") return dispatch_guard(a, cmd_rdrobust(a));
    if (a[0] == "rddensity") return dispatch_guard(a, cmd_rddensity(a));
    if (a[0] == "rdplot") return dispatch_guard(a, cmd_rdplot(a));
    if (a[0] == "binscatter") return dispatch_guard(a, cmd_binscatter(a));
    if (a[0] == "eventstudyinteract") return dispatch_guard(a, cmd_eventstudyinteract(a));
    if (a[0] == "did_imputation") return dispatch_guard(a, cmd_did_imputation(a));
    if (a[0] == "sdid") return dispatch_guard(a, cmd_sdid(a));
    // --- AER bundle H1 ---
    if (a[0] == "hpfilter") return dispatch_guard(a, cmd_hpfilter(a));
    if (a[0] == "lpoly") return dispatch_guard(a, cmd_lpoly(a));
    if (a[0] == "lowess") return dispatch_guard(a, cmd_lowess(a));
    if (a[0] == "oaxaca") return dispatch_guard(a, cmd_oaxaca(a));
    if (a[0] == "nnmatch") return dispatch_guard(a, cmd_nnmatch(a));
    if (a[0] == "pca") return dispatch_guard(a, cmd_pca(a));
    std::fprintf(stderr, "unknown command: %s\n", a[0].c_str());
    return 2;
}

// ---- `cobra serve` — MAXPERF Stage 1a (contract_A §25) --------------------------
// Offline law: stdin/stdout line-JSON ONLY — no sockets, no network, no threads.
// Kills the per-command process-spawn tax by running every request IN-PROCESS
// through dispatch_command. Stage 1a = PROTOCOL + LOOP only; the resident-Frame
// data plane (skipping the per-request hbin re-read) is Stage 1b, out of scope here.

// The no-args usage blurb — ONE definition shared verbatim by one-shot main() and a
// serve request whose argv is empty, so the two paths are byte-equal by construction.
// (No '%' in the text; both sites print it through "%s".)
static const char kUsageBlurb[] =
    "CoBrA — C++ Oriented Bootstrap & Regression Accelerator\n"
    "  cobra info  <file.hbin>\n"
    "  cobra ols   --data f.hbin --y Y --x X1,X2 [--no-intercept]\n"
    "  cobra feols --data f.hbin --y Y --x X1,X2 [--fe F1,F2] "
    "[--vcov iid|hc1|cluster] [--cluster C]\n"
    "  cobra csdid --data f.hbin --y Y --ivar ID --time T --gvar G "
    "[--x X1,X2,...] [--window LO,HI]\n"
    "  cobra run   <spec.json>\n";

// Lossless byte-level JSON string escaper for the response. nlohmann's dump()
// VALIDATES UTF-8 and THROWS (type_error.316) on any non-UTF-8 byte in a captured
// buffer — a single stray byte would abort a response mid-session — so response
// strings are escaped by hand instead: the two structural bytes (", \) and all C0
// controls (0x00-0x1F) get JSON escapes; every byte >= 0x20 passes through VERBATIM
// (bytes preserved exactly; a strict-UTF-8 reader is out of scope for 1a). Request
// PARSING still reuses nlohmann (json::parse, exceptions off) — only WRITE is hand-rolled.
static void serve_escape(std::string& dst, const std::string& s) {
    static const char* HEX = "0123456789abcdef";
    for (unsigned char c : s) {
        switch (c) {
            case '"':  dst += "\\\""; break;
            case '\\': dst += "\\\\"; break;
            case '\b': dst += "\\b";  break;
            case '\f': dst += "\\f";  break;
            case '\n': dst += "\\n";  break;
            case '\r': dst += "\\r";  break;
            case '\t': dst += "\\t";  break;
            default:
                if (c < 0x20) { dst += "\\u00"; dst += HEX[c >> 4]; dst += HEX[c & 0xf]; }
                else dst += static_cast<char>(c);
        }
    }
}

// Slurp a capture file back through its still-open fd (rewind + read to EOF).
static std::string read_capture(int fd) {
    std::string s;
    if (::lseek(fd, 0, SEEK_SET) == -1) return s;
    char buf[1 << 16];
    for (ssize_t n; (n = ::read(fd, buf, sizeof buf)) > 0;)
        s.append(buf, static_cast<size_t>(n));
    return s;
}

// ---- `cobra serve` resident-frame session — MAXPERF Stage 1b (contract_A §26) ----
// The worker holds ONE working dataset in RAM across requests; verbs mutate it in
// place; hbin files become snapshot devices. Additive over §25: stateless {argv}
// requests are byte-identical to before and never see or clobber this session.
// A captured session data-state (serve preserve / tempfile — contract_A §27). The
// Frame COPY shares every column payload with the live resident (column-granularity
// CoW, Stage 2) and carries its own root-mmap keepalive (Frame::src_), so a snapshot
// is O(columns) — a pointer capture, not a data copy — and survives even a resident
// swap or a dropped resident. A later mutation of either side clones only the one
// column it touches, so the snapshot never sees the mutation.
struct FrameSnapshot {
    Frame frame;
    std::shared_ptr<const Dataset> root_sp;
    std::string root_path;
    std::string root_hash;
    std::set<std::string> dirty_names;
    bool structure_dirty = false;
};

// RAMDIET 2026-08-15: the process's OWN physical footprint, and its high-water mark.
//
// Why the process has to report this itself. A 1 s `vmmap -summary` sampler was the
// instrument for two arms of a relief-cadence A/B and produced a clean, apparently
// reproducible story (406.4 MB twice at cadence=1 against 653.4 MB twice at 32) that
// a wider sweep destroyed: cadence=1 came back at 653.5 and cadence=8 gave both
// 461.6 and 653.7. 653.4 repeating to the decimal is the real peak; the lower numbers
// were samples that missed it. A sampler cannot bound a peak, so it cannot answer a
// question about peaks, however stable its output looks.
//
// `phys_footprint` is the same number vmmap prints as "Physical footprint" — dirty +
// compressed + IOKit, i.e. what the process is actually charged for, and NOT the
// file-backed mmap pages that make RSS misleading here.
static uint64_t phys_footprint_now() {
#if defined(__APPLE__)
    task_vm_info_data_t info{};
    mach_msg_type_number_t count = TASK_VM_INFO_COUNT;
    if (task_info(mach_task_self(), TASK_VM_INFO,
                  reinterpret_cast<task_info_t>(&info), &count) == KERN_SUCCESS)
        return info.phys_footprint;
#endif
    return 0;
}
uint64_t g_phys_peak = 0;

// What the allocator HOLDS versus what it USES, sampled at the moment the footprint
// sets a new high-water mark. This is the fork the campaign is standing on: if
// `used` tracks `held` at the peak then the bytes are live data and the hunt is a
// data question; if `held` far exceeds `used` then they are retained pages and it is
// an allocator question. vmmap can show the same split but only from outside, at a
// moment nobody chose -- and a sampler cannot be at a peak on purpose.
uint64_t g_peak_zone_held = 0, g_peak_zone_used = 0, g_peak_zone_blocks = 0;

static void zone_stats_at_peak() {
#if defined(__APPLE__)
    vm_address_t* zones = nullptr;
    unsigned count = 0;
    if (malloc_get_all_zones(mach_task_self(), nullptr, &zones, &count) != KERN_SUCCESS) return;
    uint64_t held = 0, used = 0, blocks = 0;
    for (unsigned i = 0; i < count; ++i) {
        malloc_statistics_t st{};
        malloc_zone_statistics(reinterpret_cast<malloc_zone_t*>(zones[i]), &st);
        held += st.size_allocated;
        used += st.size_in_use;
        blocks += st.blocks_in_use;
    }
    g_peak_zone_held = held;
    g_peak_zone_used = used;
    g_peak_zone_blocks = blocks;
#endif
}

static void phys_footprint_track() {
    const uint64_t now = phys_footprint_now();
    if (now > g_phys_peak) {
        g_phys_peak = now;
        zone_stats_at_peak();
    }
}


struct ServeSession {
    std::shared_ptr<const Dataset> root_sp;  // root .hbin mmap (shared with resident.src_)
    std::string root_path;                   // g_data_path for resident runs; clone-patch source
    std::string root_hash;                   // fnv1a64 of the root, hashed ONCE at `use`
    Frame resident;                          // the working dataset held in RAM
    Dataset view;                            // cached zero-copy Dataset over `resident`
    bool active = false;                     // a frame is loaded

    Frame pending;                           // staged mutator result (applied post clean dispatch)
    bool has_pending = false;

    std::set<std::string> dirty_names;       // cumulative changed/added vs root (since `use`)
    bool structure_dirty = false;            // cumulative: row/col-set/order change or bulk-build

    // §27 session snapshot registries. Both are O(columns) ref-snapshots of the whole
    // data-state; the disk hbin chain is untouched (no write on stash / save_virtual).
    std::unordered_map<std::string, FrameSnapshot> stashes;        // preserve name  -> snapshot
    std::unordered_map<std::string, FrameSnapshot> virtual_files;  // tempfile path  -> snapshot

    // §31 estimation store: handle -> the fitted model behind it. A stored model is a
    // self-contained (names, beta, V) tuple that references NO data, so it cannot be
    // invalidated by a later data change — matching Stata, where post-estimation reads
    // the stored e(b)/e(V) and does not care that the data moved underneath. It is
    // cleared only by `drop` (Stata `clear`, which likewise clears e()).
    // `model_order` is mint order, used solely for the capacity eviction below.
    std::unordered_map<std::string, PostestModel> models;
    std::vector<std::string> model_order;
    size_t model_bytes = 0;  // running Σ of postest_bytes() over `models` (see the cap note)

    void rebuild_view() {
        // MECHOPT W2: patch-in-place when the view is an incremental step of
        // the same shape (same rows / source path / sort claim) — unchanged
        // entries are MOVED (O(1), dictionary rides along), only changed
        // columns are rebuilt. The full rebuild deep-copies every factor level
        // dictionary (135,603 strings on the acceptance corpus) and fired on
        // every committed frame run, every use and every unstash. All four
        // call sites go through here, so all four inherit the patch.
        if (resident.patch_dataset_view(view)) {
            engctr::addViewPatch();
            return;
        }
        engctr::addViewRebuild();
        view = resident.as_dataset_view();
    }


    // Capture the current data-state (Stata preserve / tempfile save). O(columns).
    FrameSnapshot capture_state() const {
        return FrameSnapshot{resident, root_sp, root_path, root_hash, dirty_names, structure_dirty};
    }
    // ONESHOT-FIXMEM M1: the session's own residency census. Answers, in bytes the
    // process is actually charged for (Frame::owned_payload_bytes — borrowed mmap
    // columns count 0), the question a heap profile can only answer as one lump:
    // WHICH holder is keeping a `keep_if` generation alive. The registries are the
    // suspects — neither `virtual_files` nor `stashes` has any eviction — so they
    // are reported with both a count and a byte total, and the live resident is
    // reported beside them as the denominator that legitimately exists.
    //
    // Env-gated to a FILE path, not stderr: PIET discards engine stderr on every
    // success path it owns ([[cobra-piet-warning-channel]]), so an stderr census is
    // unreachable from behind the product — the same lesson the csdid census cost.
    void census(const char* path, unsigned long long served) const {
        int64_t stash_b = 0;
        for (const auto& kv : stashes) stash_b += kv.second.frame.owned_payload_bytes();
        int64_t vf_b = 0;
        for (const auto& kv : virtual_files) vf_b += kv.second.frame.owned_payload_bytes();
        if (std::FILE* f = std::fopen(path, "a")) {
            std::fprintf(f,
                         "[session-memlog] req=%llu resident=%.1fMB pending=%.1fMB "
                         "stashes=%zu/%.1fMB virtual_files=%zu/%.1fMB "
                         "phys=%.1fMB physpeak=%.1fMB peakzone_held=%.1fMB "
                         "peakzone_used=%.1fMB peakzone_blocks=%llu\n",
                         served, resident.owned_payload_bytes() / 1048576.0,
                         pending.owned_payload_bytes() / 1048576.0, stashes.size(),
                         stash_b / 1048576.0, virtual_files.size(), vf_b / 1048576.0,
                         phys_footprint_now() / 1048576.0, g_phys_peak / 1048576.0,
                         g_peak_zone_held / 1048576.0, g_peak_zone_used / 1048576.0,
                         static_cast<unsigned long long>(g_peak_zone_blocks));
            std::fclose(f);
        }
    }

    // Restore a captured data-state wholesale (Stata restore / tempfile use). O(columns).
    void restore_state(const FrameSnapshot& s) {
        root_sp = s.root_sp;
        root_path = s.root_path;
        root_hash = s.root_hash;
        resident = s.frame;              // Frame copy: shares columns (CoW), keeps root alive via src_
        dirty_names = s.dirty_names;
        structure_dirty = s.structure_dirty;
        has_pending = false;
        pending = Frame();
        active = true;
        rebuild_view();                  // the cached view must point at the restored buffers
    }
};

// Non-null ONLY during a frame-run "run" dispatch: the bridge the shared data plane
// (acquire_data / commit_frame, apps/cmds/common.cpp) consults. Outside a frame-run it
// is null, so one-shot and stateless §25 paths behave exactly as before.
static ServeSession* g_active_session = nullptr;
// ENG-ADOPTION-AVOIDANCE: the serve loop's session for the WHOLE loop lifetime (not
// just a resident-run dispatch) — the deferred root-hash bridge reads it. Null in
// one-shot mode; see serve_root_hash().
static ServeSession* g_hash_root_session = nullptr;

// ---- §31 session estimation store: process-level state --------------------------
// The handle counter and the nonce are PROCESS-global, deliberately NOT session
// members: `drop` resets the ServeSession (clearing the store, Stata `clear`), and a
// session-local counter would then rewind to 0 and re-mint a handle string that a
// client might still be holding from before the drop — silently addressing a DIFFERENT
// model. Keeping the counter monotonic for the life of the process makes that
// wrong-model answer unrepresentable: a handle either resolves to exactly the model it
// was minted for, or it misses and errors.
static uint64_t g_est_counter = 0;
// Post-estimation model FITS this process has performed (as opposed to reusing a stored
// model). Reported on the `est` response envelope: this is the number a mechanism gate
// asserts against — it must NOT increase across handle-addressed calls. Also
// process-monotonic so a mid-run `drop` cannot reset a gate's baseline.
static uint64_t g_est_fits = 0;
// Bounds on the stored-model set. An evicted handle is an honest exit-3 miss (the client
// re-runs the full specification); it is never a silent re-fit.
//
// TWO bounds, because a count cap ALONE cannot bound memory here: the per-model payload
// is a k*k covariance and k is unbounded, so "32 models" is 32*k²*8 bytes — not a
// bounded quantity. (This is the count-cap-vs-byte-budget trap; the count cap bounds the
// bookkeeping, the byte budget bounds the actual memory, and both are enforced below.)
// The store is exercised hard — PIET's fitting calls arrive as frame-runs, so every
// lincom/nlcom/test mints: ~54 per dependent variable, ~8.6k across the full analysis.
static const size_t kEstStoreCap = 32;                        // map/bookkeeping bound
static const size_t kEstStoreBytes = 64ull * 1024 * 1024;     // resident-payload bound

// Resident footprint of one stored model. The k*k covariance dominates; beta and the
// coefficient names are O(k). capacity() (not size()) is what the allocator is actually
// holding, and a stored model is never mutated, so this is stable between the insert
// that adds it and the eviction that subtracts it.
static size_t postest_bytes(const PostestModel& m) {
    size_t b = m.vcov_matrix.capacity() * sizeof(double)
             + m.beta.capacity() * sizeof(double)
             + m.vcov_type.capacity()
             + m.names.capacity() * sizeof(std::string);
    for (const std::string& s : m.names) b += s.capacity();
    return b;
}

// Per-process handle salt. A handle minted by a PREVIOUS serve process can therefore
// never resolve in a new one even though the counter restarts — a replayed stale handle
// misses and errors instead of hitting an unrelated model.
static const std::string& est_nonce() {
    static const std::string n = [] {
        uint64_t s = static_cast<uint64_t>(native_process_id()) * 0x9E3779B97F4A7C15ull;
        s ^= static_cast<uint64_t>(
            std::chrono::system_clock::now().time_since_epoch().count());
        char buf[16];
        std::snprintf(buf, sizeof buf, "%08x", static_cast<unsigned>(s ^ (s >> 32)));
        return std::string(buf);
    }();
    return n;
}

// The §31 bridge, mirroring the §26 one above. `g_active_model` is set ONLY for the
// span of a `postest` dispatch; `g_publish_enabled` is true ONLY for the span of a
// frame-mode dispatch that is allowed to mint (so one-shot and stateless §25 runs keep
// zero session footprint and are byte-untouched).
static const PostestModel* g_active_model = nullptr;
static PostestModel g_published_model;
static bool g_has_published = false;
static bool g_publish_enabled = false;

// GA sweep 2026-08-22 — see serve_publish_merge_counts in apps/cmds/common.h.
struct MergeCounts { bool valid = false; int64_t master_only = 0, using_only = 0, matched = 0; };
static MergeCounts g_merge_counts;

// RAII teardown for the handle path. g_active_model MUST be cleared on EVERY exit path,
// including an exception that escapes serve_capture_dispatch's catch net — which catches
// EngineError and std::exception but NOT `...`. A plain assignment after the dispatch is
// skipped by such an escape, and the leaked pointer is not merely a stale flag: the next
// FULL-SPECIFICATION frame-run lincom/nlcom/test would see a non-null g_active_model,
// take the handle branch, and answer from the PREVIOUS model instead of fitting its own
// — a silent wrong-model answer on the specification path, i.e. a gate ① violation.
struct ActiveModelGuard {
    explicit ActiveModelGuard(const PostestModel* m) { g_active_model = m; }
    ~ActiveModelGuard() { g_active_model = nullptr; }
    ActiveModelGuard(const ActiveModelGuard&) = delete;
    ActiveModelGuard& operator=(const ActiveModelGuard&) = delete;
};

namespace cecon {
namespace cli {
bool serve_resident_active() { return g_active_session != nullptr; }
// ENG-ADOPTION-AVOIDANCE. Deliberately keyed off g_hash_root_session, NOT
// g_active_session: the latter is ON only during a frame-run "run" dispatch (that
// is §26's contract and must not widen), while a §25 stateless request can also
// carry --data pointing at the very root this session has mapped, and it deserves
// the same free hash. g_hash_root_session is simply "the serve loop's session, if
// a serve loop is running" — read-only here apart from memoising the hash it
// computes. One-shot mode leaves it null, so nothing outside serve changes.
std::string serve_root_hash(const std::string& path) {
    ServeSession* s = g_hash_root_session;
    if (s == nullptr || !s->active || path.empty() || path != s->root_path) return "";
    if (s->root_hash.empty()) {
        // A view-backed root owns no mapping (make_view) — no bytes to hash here;
        // return empty and let the caller read the file, exactly as before.
        if (!s->root_sp || !s->root_sp->owns_mmap() || s->root_sp->map_base() == nullptr) return "";
        s->root_hash = hash_bytes_hex(s->root_sp->map_base(), s->root_sp->map_size());
    }
    return s->root_hash;
}
const Dataset& serve_resident_view() { return g_active_session->view; }
void serve_stage_pending(Frame&& fr) {
    g_active_session->pending = std::move(fr);
    g_active_session->has_pending = true;
}
// See common.h. Reset per request alongside the other per-request globals, so a
// response can never carry a previous merge's counts.
void serve_publish_merge_counts(int64_t master_only, int64_t using_only, int64_t matched) {
    g_merge_counts = MergeCounts{true, master_only, using_only, matched};
}
const PostestModel* serve_active_model() { return g_active_model; }
void serve_publish_model(const PostestModel& m) {
    // Count EVERY post-estimation fit, whatever transport asked for it — including a
    // stateless §25 request, which mints nothing. `fits` is the mechanism signal, so it
    // must count the thing itself (a fit happened) and not merely the addressable
    // subset; otherwise a gate could be fooled by re-fits arriving on the other path.
    ++g_est_fits;
    if (!g_publish_enabled) return;  // one-shot / stateless §25: no store write, no handle
    g_published_model = m;
    g_has_published = true;
}
}  // namespace cli
}  // namespace cecon

// Raw data pointer of a frame column (borrowed or owned) — identity across a swap
// tells "unchanged passthrough" (same ptr) from "mutated/new" (fresh allocation).
static const void* fcol_ptr(const FrameColumn& c) {
    return c.is_factor ? static_cast<const void*>(c.codes.data())
                       : static_cast<const void*>(c.f64.data());
}

// n_missing for one column — MAXPERF Stage 2 Wave β2: reads through FrameColumn's
// lazily-computed, mutation-invalidated cache (frame.h) instead of re-scanning the
// column on every serve response that asks for it (`use`/`unstash`'s full descriptor,
// and a mutating verb's vars_delta for its changed/added columns).
static int64_t frame_col_n_missing(const Frame& fr, const FrameColumn& c) {
    return c.n_missing(fr.n_rows());
}

// ENG-ADOPTDESC-NMISS. Seed `fr`'s per-column missing-count caches from the write
// chain's memo for `path` (frame.h). Name AND dtype must match per column; the memo
// itself already required the file to be byte-for-byte the one measured. Columns it
// cannot vouch for keep a cold cache and are counted the old way, so a partial memo
// is a partial win and never a wrong answer.
static void seed_frame_nmiss_from_memo(const Frame& fr, const std::string& path) {
    std::vector<NmissMemoCol> known = frame_nmiss_memo_get(path);
    if (known.empty()) return;
    for (const NmissMemoCol& m : known) {
        if (m.n_missing < 0 || !fr.has(m.name)) continue;
        const FrameColumn& c = fr.col(m.name);
        const std::string dtype = c.is_factor ? "factor" : "f64";
        if (dtype != m.dtype) continue;
        c.seed_n_missing(m.n_missing);
    }
}

// ENG-ADOPTDESC-NMISS: publish what an adoption just computed, so the files this
// session's writes derive from it inherit the counts instead of re-scanning. Called
// AFTER the full descriptor (which warms every cache) — the values are already in
// hand, so this costs a vector copy, not a scan. This is the chain's ROOT: the
// corpus is counted once per process, and every scratch file downstream carries it.
static void publish_frame_nmiss(const Frame& fr, const std::string& path) {
    std::vector<NmissMemoCol> cols;
    cols.reserve(static_cast<size_t>(fr.n_cols()));
    for (const FrameColumn& c : fr.columns()) {
        cols.push_back({c.name, c.is_factor ? "factor" : "f64", c.n_missing(fr.n_rows())});
    }
    frame_nmiss_memo_put(path, std::move(cols));
}

// {name,type("f64"|"factor"),n_missing[,n_levels]} — the §26 var descriptor for
// frame.vars (use) and frame.vars_delta (run). n_levels is added for factor columns
// (PIET's shadow varlist card renders without re-mmapping). Column names come from an
// hbin header (already valid UTF-8), so building via nlohmann + dump() is safe.
static json frame_var_desc(const Frame& fr, const std::string& name) {
    const FrameColumn& c = fr.col(name);
    json v = {{"name", name}, {"type", c.is_factor ? "factor" : "f64"},
              {"n_missing", frame_col_n_missing(fr, c)}};
    if (c.is_factor) v["n_levels"] = static_cast<int64_t>(c.levels.size());
    // BUG-365 (contract_A additive, absent when unstated): identity metadata has to
    // ride the RESIDENT response too. A column PIET learns about only from here — a
    // using-side column of a resident joinby/merge — had no other source for its
    // display format, so routing those verbs onto the resident plane silently reset
    // %10s to %9.0g. Same absent-vs-empty discipline as the hbin header: "" means
    // the source stated none, and the key is omitted rather than sent empty.
    if (!c.format.empty()) v["format"] = c.format;
    if (!c.label.empty()) v["label"] = c.label;
    return v;
}

// The full frame descriptor {rows,cols,vars:[...]} — the §26 `use` response body, reused
// verbatim by the §27 tempfile-`use` hit and by `unstash` (both re-establish the entire
// resident frame, so the client resyncs its whole varlist). Insertion order (rows, cols,
// vars) is fixed so the dump is byte-identical to the pre-§27 inline `use` builder.
static json frame_full_desc(const Frame& fr) {
    json frame;
    frame["rows"] = fr.n_rows();
    frame["cols"] = fr.n_cols();
    json vars = json::array();
    for (const FrameColumn& c : fr.columns()) vars.push_back(frame_var_desc(fr, c.name));
    frame["vars"] = vars;
    return frame;
}

// One serve response line: standard §25 shape {id,exit,stdout,stderr} + optional
// "frame" object (raw JSON text, "" = omit). stdout/stderr are hand-escaped for the
// same reason §25 does it (a captured buffer may hold non-UTF-8 bytes nlohmann rejects).
// `est_json` ("" = omit) is the §31 estimation-store member; it is written LAST, after
// `frame`, so every pre-1.20.0 member keeps its position and an existing consumer that
// reads by key is unaffected.
static void serve_write_response(long long id, bool have_id, int exit_code,
                                 const std::string& out_bytes, const std::string& err_bytes,
                                 const std::string& frame_json,
                                 const std::string& est_json = "") {
    std::string resp = "{\"id\":";
    resp += have_id ? std::to_string(id) : std::string("null");
    resp += ",\"exit\":";
    resp += std::to_string(exit_code);
    resp += ",\"stdout\":\"";
    serve_escape(resp, out_bytes);
    resp += "\",\"stderr\":\"";
    serve_escape(resp, err_bytes);
    resp += "\"";
    if (!frame_json.empty()) { resp += ",\"frame\":"; resp += frame_json; }
    if (!est_json.empty()) { resp += ",\"est\":"; resp += est_json; }
    resp += "}\n";
    std::cout << resp;
    std::cout.flush();
}

// Run one dispatch with fds 1/2 captured, using the SAME mechanics §25/§26 document
// (see run_serve's long note). Used only by the §31 `postest` path: the two existing
// copies of this block (the stateless loop and the frame-`run` path) are shipped and
// gated, so they are deliberately left byte-untouched rather than refactored into this
// helper — a third literal copy would have been the worse of the two evils.
// `setup_failed` reports a capture-setup failure; rc/out/err are meaningless then.
static int serve_capture_dispatch(const std::vector<std::string>& a,
                                  std::string& out_bytes, std::string& err_bytes,
                                  bool& setup_failed) {
    setup_failed = false;
    std::cout.flush();
    std::fflush(stdout);
    std::fflush(stderr);
    std::string cap_base = native_join_path(
        keepif_scratch_dir(), "cecon_serve_" + std::to_string(native_process_id()));
    int cap1 = native_open(cap_base + "_out.tmp", O_RDWR | O_CREAT | O_TRUNC, 0600);
    int cap2 = native_open(cap_base + "_err.tmp", O_RDWR | O_CREAT | O_TRUNC, 0600);
    int sav1 = ::dup(1), sav2 = ::dup(2);
    if (cap1 < 0 || cap2 < 0 || sav1 < 0 || sav2 < 0) {
        for (int f : {cap1, cap2, sav1, sav2}) if (f >= 0) ::close(f);
        native_unlink(cap_base + "_out.tmp");
        native_unlink(cap_base + "_err.tmp");
        setup_failed = true;
        return 1;
    }
    native_unlink(cap_base + "_out.tmp");
    native_unlink(cap_base + "_err.tmp");
    ::dup2(cap1, 1);
    ::dup2(cap2, 2);

    int rc;
    bool discard_out = false;
    try {
        rc = dispatch_command(a);
    } catch (const cecon::EngineError& e) {
        discard_out = true;
        json err = {{"error", {{"code", e.code}, {"message", e.what()}}}};
        if (!e.detail.empty()) err["error"]["detail"] = e.detail;
        std::cerr << err.dump(2) << "\n";
        rc = cecon::exit_code_for(e.code);
    } catch (const std::exception& e) {
        discard_out = true;
        json err = {{"error", {{"code", "internal"}, {"message", e.what()}}}};
        std::cerr << err.dump(2) << "\n";
        rc = 1;
    }

    std::cout.flush();
    std::cerr.flush();
    std::fflush(stdout);
    std::fflush(stderr);
    ::dup2(sav1, 1);
    ::dup2(sav2, 2);
    ::close(sav1);
    ::close(sav2);
    out_bytes = discard_out ? std::string() : read_capture(cap1);
    err_bytes = read_capture(cap2);
    ::close(cap1);
    ::close(cap2);
    native_unlink(cap_base + "_out.tmp");
    native_unlink(cap_base + "_err.tmp");
    keepif_cleanup();
    return rc;
}

// Mint a handle for a just-published fit and store it (contract_A §31). Returns the
// `est` response-envelope object. Eviction is in mint order against BOTH bounds.
static json serve_mint_model(ServeSession& session) {
    ++g_est_counter;
    std::string handle = "est:" + est_nonce() + ":" + std::to_string(g_est_counter);

    // MOVE into the store, then explicitly release the static. g_published_model is a
    // process-static that would otherwise keep a full second copy of the model alive
    // indefinitely — outliving the store's own eviction and quietly becoming the
    // dominant holder of a model the store believes it has released. That secondary
    // holder is exactly what a naive audit of the cap misses, so it is cut here.
    PostestModel& stored = (session.models[handle] = std::move(g_published_model));
    g_published_model = PostestModel();

    // Envelope fields are read BEFORE eviction: `stored` must not be consulted after the
    // loop below, which may erase map entries.
    json est;
    est["handle"] = handle;
    est["k"] = static_cast<int64_t>(stored.names.size());
    est["n_obs"] = stored.n;
    est["vcov_type"] = stored.vcov_type;
    est["fits"] = static_cast<int64_t>(g_est_fits);

    session.model_order.push_back(handle);
    session.model_bytes += postest_bytes(stored);
    // Evict oldest-first until under BOTH bounds. The newest model is never evicted: a
    // single model larger than the whole budget must still be addressable, or minting
    // would hand back a handle that is dead on arrival.
    while (session.model_order.size() > 1 &&
           (session.model_order.size() > kEstStoreCap || session.model_bytes > kEstStoreBytes)) {
        auto it = session.models.find(session.model_order.front());
        if (it != session.models.end()) {
            // std::min guards the running total against any drift between the insert-time
            // and evict-time measurement (there should be none — stored models are immutable).
            session.model_bytes -= std::min(session.model_bytes, postest_bytes(it->second));
            session.models.erase(it);
        }
        session.model_order.erase(session.model_order.begin());
    }
    return est;
}

// Dispatch a frame request (object member "frame" instead of "argv") — contract_A §26.
static void run_frame_request(const json& req, ServeSession& session,
                              const std::string& serve_argv0) {
    const json& F = req["frame"];
    bool have_id = req.contains("id") && req["id"].is_number_integer();
    long long id = have_id ? req["id"].get<long long>() : 0;
    if (!F.contains("op") || !F["op"].is_string()) {
        serve_write_response(id, have_id, 2, "", "serve: frame request needs a string \"op\"", "");
        return;
    }
    const std::string op = F["op"].get<std::string>();

    if (op == "use") {
        if (!F.contains("path") || !F["path"].is_string()) {
            serve_write_response(id, have_id, 2, "", "serve: frame use needs a string \"path\"", "");
            return;
        }
        const std::string path = F["path"].get<std::string>();
        // §27: a tempfile virtual-save shadows the disk file — an O(1) in-RAM restore,
        // no re-mmap/re-read. Checked FIRST; a path never save_virtual'd falls through to
        // the disk open below (byte-identical to the pre-§27 `use`).
        auto vit = session.virtual_files.find(path);
        if (vit != session.virtual_files.end()) {
            session.restore_state(vit->second);
            serve_write_response(id, have_id, 0, "", "", frame_full_desc(session.resident).dump());
            return;
        }
        std::shared_ptr<Dataset> sp;
        try {
            sp = std::make_shared<Dataset>(Dataset::open(path));  // hbin ONLY: bad magic / missing → throw
        } catch (const std::exception& e) {
            // Non-hbin or unreadable → exit 2, session UNCHANGED (no clobber of a prior frame).
            serve_write_response(id, have_id, 2, "",
                std::string("serve: frame use could not open '") + path + "' as .hbin: " + e.what(), "");
            return;
        }
        session.root_path = path;
        // ENG-ADOPTION-AVOIDANCE: DEFERRED, not dropped. This used to be an eager
        // whole-file FNV on every adoption — and an adoption happens on every
        // resident-chain break, which the FLOOR campaign measured at 2,241 per
        // analysis.do run (~270 ms each on the 164 MB corpus ≈ 10 of the engine's
        // 19 minutes). The hash's only consumer is estimator provenance, so it is
        // now computed on first demand by serve_root_hash() — from THIS mmap
        // (session.root_sp below), so the value is identical and attests exactly
        // the bytes adopted here. An empty root_hash means "not computed yet",
        // never "unknown": the seeds below skip an empty one and hash_file_hex's
        // bridge call fills it.
        session.root_hash.clear();
        session.root_sp = sp;
        session.resident = Frame::from_dataset_shared(sp);
        // ENG-ADOPTDESC-NMISS: seed the per-column missing counts the write chain
        // already knows for this exact file, so the full descriptor below does not
        // re-scan every column. Measured: 190 ms of a 202 ms adoption on the
        // campaign corpus. Only exact (path, size, mtime) + (name, dtype) matches
        // seed; anything else falls through to the ordinary scan (frame.h).
        seed_frame_nmiss_from_memo(session.resident, path);
        session.rebuild_view();
        session.dirty_names.clear();
        session.structure_dirty = false;
        session.has_pending = false;
        session.active = true;
        std::string desc = frame_full_desc(session.resident).dump();
        // ENG-ADOPTDESC-NMISS: the descriptor above just warmed every column's count
        // (or read the seeded ones back) — publish them so files written FROM this
        // frame inherit instead of re-scanning. Root of the write-chain carry.
        publish_frame_nmiss(session.resident, path);
        serve_write_response(id, have_id, 0, "", "", desc);
        return;
    }

    // §28 getcols — hand back a column SUBSET of the resident frame as a real .hbin.
    //
    // Why this op exists. PIET's row-wise expression evaluator needs actual values,
    // and its only way to get them was to make the engine flush the WHOLE resident
    // frame to disk and mmap it back. Measured on the reference slice: 154 of 350
    // head materializations come from that entrance alone, ~161 MB each, ~24.8 GB
    // of the run's 45.7 GB of scratch writes. The exit side (resident `setcol`,
    // which writes no --out) was optimized long ago; this is the entrance.
    //
    // Deliberately NOT a new payload format. The projection is written by the same
    // `write_hbin` the frame already uses, so there is no new serializer, no new
    // reader, and the bytes stay under the two gates that already pin them (the
    // freeze corpus and tools/lint_hbin_header_bytes.py). The projection shares
    // payloads with the resident (Frame::project), so this costs one header plus
    // the requested columns' bytes — never a frame copy.
    //
    // Refusals are honest and total: no resident frame, an empty list, or ANY name
    // the frame does not have is exit 2 with no file written. A partial projection
    // would be the worse failure — PIET would evaluate against columns it did not
    // ask for and never know.
    if (op == "getcols") {
        if (!session.active) {
            serve_write_response(id, have_id, 2, "", "serve: getcols with no resident frame", "");
            return;
        }
        if (!F.contains("cols") || !F["cols"].is_array() || F["cols"].empty()) {
            serve_write_response(id, have_id, 2, "",
                                 "serve: getcols needs a non-empty string array \"cols\"", "");
            return;
        }
        if (!F.contains("out") || !F["out"].is_string() || F["out"].get<std::string>().empty()) {
            serve_write_response(id, have_id, 2, "", "serve: getcols needs a string \"out\"", "");
            return;
        }
        std::vector<std::string> want;
        want.reserve(F["cols"].size());
        for (const auto& c : F["cols"]) {
            if (!c.is_string()) {
                serve_write_response(id, have_id, 2, "",
                                     "serve: getcols \"cols\" must hold strings only", "");
                return;
            }
            want.push_back(c.get<std::string>());
        }
        const std::string out = F["out"].get<std::string>();
        try {
            Frame sub = session.resident.project(want);
            sub.write_hbin(out);
        } catch (const std::exception& e) {
            // Same discipline as every other write path here: never leave a torn
            // file behind for a later mmap to walk off the end of (BUG-124).
            std::error_code rmEc;
            std::filesystem::remove(cecon::to_native_path(out), rmEc);
            serve_write_response(id, have_id, 2, "", std::string("serve: getcols: ") + e.what(), "");
            return;
        }
        serve_write_response(id, have_id, 0, "", "", "");
        return;
    }

    if (op == "drop") {  // free resident state, exit 0 (root mmap released with the session reset)
        session = ServeSession();
        serve_write_response(id, have_id, 0, "", "", "");
        return;
    }

    // W0 (P2PERF-WAVE-PLAN §W0) — read the engine's mechanism counters out of the
    // worker. They are process-wide integers that ALWAYS accumulate; this op is
    // the only reader, so a run that never asks is byte-identical to a build
    // without them. Additive: a client that does not know the op never sends it,
    // and the response is an ordinary stdout payload. Purely observational — it
    // touches no session state, so it can be sent at any point in a session
    // (PIET sends it once, at emit time).
    // MUTRES-WIRE (2026-08-22): the PULL half of the card-rebuild wire. `vars`
    // hands back the resident frame's varlist and NOTHING else — the same
    // {rows, cols, vars} body `use` and `unstash` already return, via the same
    // frame_full_desc builder, so no new shape enters the protocol.
    //
    // Why a second mechanism when the run response already pushes a varlist on
    // !card_stable: the push covers the refusals that KNOW they are structural
    // (measured 320 of 640 — the resident joinby/merge deltas). The other 320 are
    // `plan-declined`, where a card-fold PLAN was attempted and the engine's own
    // card_stable said the card was fine; those responses carry no full list and
    // never will, because from the engine's side nothing was wrong. This op lets
    // PIET ask for the list at the moment it discovers it cannot fold — one ~40 KB
    // round trip in place of materializing a 128.6 MB head. It also makes the
    // coverage structural rather than enumerated: any FUTURE refusal reason is
    // covered without touching the engine again.
    //
    // Read-only and session-safe: it touches no session state, stages nothing,
    // and does not advance the resident. Additive, so a client that never sends
    // it sees a byte-identical engine.
    if (op == "vars") {
        if (!session.active) { serve_write_response(id, have_id, 3, "", "serve: no frame in session", ""); return; }
        serve_write_response(id, have_id, 0, "", "", frame_full_desc(session.resident).dump());
        return;
    }

    if (op == "counters") {
        json c;
        c["patch_hits"] = engctr::patchHits();
        c["patch_fulls"] = engctr::patchFulls();
        c["patch_fallbacks"] = engctr::patchFallbacks();
        c["hash_file_calls"] = engctr::hashFileCalls();
        c["hash_file_memo_hits"] = engctr::hashFileMemoHits();
        c["data_file_opens"] = engctr::dataFileOpens();   // WS-D2 (S06 ARC-MRG)
        c["merge_file_opens"] = engctr::mergeFileOpens();  // last cmd_merge invocation
        c["open_full_parse"] = Dataset::fullParseCount();
        c["open_carried"] = Dataset::carriedOpenCount();
        c["est_fits"] = g_est_fits;
        // COBRA-DD-ACCUM (TOLERANCE §5) — appended LAST so key order stays
        // append-only for every already-captured fixture. Mechanism witness:
        // how many compensated-accumulation kernel calls this process made.
        c["dd_accum_used"] = engctr::ddAccumUsed();
        // MECHOPT W1 — appended after dd_accum_used, same append-only law. The
        // PATCH_VIEW witness pair: per-call view work, not request counts.
        c["view_rebuilds"] = engctr::viewRebuilds();
        c["view_patches"] = engctr::viewPatches();
        serve_write_response(id, have_id, 0, c.dump(), "", "");
        return;
    }

    if (op == "snapshot") {
        if (!session.active) { serve_write_response(id, have_id, 3, "", "serve: no frame in session", ""); return; }
        if (!F.contains("path") || !F["path"].is_string()) {
            serve_write_response(id, have_id, 2, "", "serve: frame snapshot needs a string \"path\"", "");
            return;
        }
        const std::string path = F["path"].get<std::string>();
        try {
            // write_hbin's dispatcher clones the root + patches the (owned = cumulative
            // dirty) columns when structure-clean AND the root still exists; otherwise a
            // full rewrite. A clone failure of any kind (e.g. the client deleted the root)
            // auto-falls back to full write. structure_dirty was stamped onto `resident`
            // at each swap, so the clean/rewrite choice here reflects CUMULATIVE state.
            // MECHOPT W1: attribute this snapshot's write plane. write_hbin
            // dispatches to patch_hbin (CoW clone + header rewrite) or a full
            // re-serialize; the census's p50 25ms / p90 342ms bimodality was
            // hypothesised to be exactly that split. Read the counters around
            // the call and say which way it went — env-gated stderr, the
            // OPEN_PHASE_DIAG idiom, nothing on stdout.
            static const bool kSnapDiag = std::getenv("COBRA_OPEN_PHASE_DIAG") != nullptr;
            const int64_t sp0 = engctr::patchHits(), sf0 = engctr::patchFulls();
            // GA sweep, 2026-08-22: frame:snapshot became the single largest engine
            // op (50.9% of engine wall) the moment joinby stopped writing its own
            // --out, which is the signature of a write that MOVED rather than went
            // away. It had no byte meter, so the storage half of that question could
            // not be answered at all. It has one now: bytes written and which write
            // plane wrote them, on the same COBRA_VERB_DIAG sink as every other
            // per-request row, so time and bytes are read from one ledger.
            const bool kVerbDiag = cecon::verb_diag_enabled();
            const auto t_snap0 = cecon::verb_diag_now(kVerbDiag);
            session.resident.write_hbin(path);
            const double snap_ms = cecon::verb_diag_ms(kVerbDiag, t_snap0);
            if (kVerbDiag) {
                const char* plane = engctr::patchHits() > sp0 ? "patch"
                                  : (engctr::patchFulls() > sf0 ? "full" : "other");
                cecon::verb_diag_emit(
                    "[verb-diag] verb=snapshot total_ms=%.3f write_ms=%.3f other_ms=0.000 "
                    "scratch_bytes=%lld rows_in=%lld rows_out=%lld plane=%s\n",
                    snap_ms, snap_ms,
                    static_cast<long long>(cecon::verb_diag_file_size(cecon::to_native_path(path))),
                    static_cast<long long>(session.resident.n_rows()),
                    static_cast<long long>(session.resident.n_rows()), plane);
            }
            if (kSnapDiag) {
                std::fprintf(stderr, "SNAPSHOT_DIAG outcome=%s\n",
                             engctr::patchHits() > sp0
                                 ? "patch"
                                 : (engctr::patchFulls() > sf0 ? "full" : "other"));
            }
        } catch (const std::exception& e) {
            serve_write_response(id, have_id, 1, "",
                std::string("serve: snapshot write failed: ") + e.what(), "");
            return;
        }
        // ENG-ADOPTDESC-NMISS, MEASURED AND REJECTED: publishing the counts here
        // (so the snapshot file's adoption could seed) is a NET LOSS — it forces
        // every cold column to be counted at snapshot time, and the adoption it
        // would help has already been served by the patch writer's own memo.
        // a2: frame:use 76.4->61.8ms but frame:snapshot 169.7->230.8ms, engine-busy
        // 40.0->42.5s. Counting is not free just because the frame is hot.
        json frame;
        frame["rows"] = session.resident.n_rows();
        frame["cols"] = session.resident.n_cols();
        frame["path"] = path;
        serve_write_response(id, have_id, 0, "", "", frame.dump());
        return;
    }

    // ---- §27 session snapshots (Stata preserve / restore / tempfile) --------------
    if (op == "stash") {  // preserve: O(1) ref-snapshot of the resident under `name`
        if (!session.active) { serve_write_response(id, have_id, 3, "", "serve: no frame in session", ""); return; }
        if (!F.contains("name") || !F["name"].is_string()) {
            serve_write_response(id, have_id, 2, "", "serve: frame stash needs a string \"name\"", "");
            return;
        }
        const std::string name = F["name"].get<std::string>();
        // COBRA_VERB_DIAG (measurement-only, see src/core/verb_diag.h): `preserve`
        // is DOCUMENTED as an O(1) ref-snapshot with no witness (see FrameSnapshot's
        // doc comment above) — this times capture_state() and reports how many
        // bytes the resulting snapshot Frame independently OWNS beyond what the
        // live resident already owned pre-capture (owned_payload_bytes() counts a
        // borrowed/shared mmap column as 0, per the census note above). A true
        // pointer/refcount snapshot leaves that delta at 0; a silent deep copy
        // would show up here as `deep_bytes` > 0. OFF path costs one bool check.
        const bool kVerbDiag = cecon::verb_diag_enabled();
        const int64_t bytes_before = kVerbDiag ? session.resident.owned_payload_bytes() : 0;
        const auto t0 = cecon::verb_diag_now(kVerbDiag);
        session.stashes[name] = session.capture_state();
        const double capture_ms = cecon::verb_diag_ms(kVerbDiag, t0);
        if (kVerbDiag) {
            const int64_t bytes_after = session.stashes[name].frame.owned_payload_bytes();
            const int64_t deep_bytes = bytes_after > bytes_before ? bytes_after - bytes_before : 0;
            const int64_t rows = session.resident.n_rows();
            cecon::verb_diag_emit(
                "[verb-diag] verb=preserve total_ms=%.3f capture_ms=%.3f other_ms=0.000 "
                "scratch_bytes=0 rows_in=%lld rows_out=%lld deep_bytes=%lld\n",
                capture_ms, capture_ms, static_cast<long long>(rows),
                static_cast<long long>(rows), static_cast<long long>(deep_bytes));
        }
        json frame;
        frame["rows"] = session.resident.n_rows();
        frame["cols"] = session.resident.n_cols();
        frame["name"] = name;
        serve_write_response(id, have_id, 0, "", "", frame.dump());
        return;
    }

    if (op == "unstash") {  // restore: O(1) ref-restore of a named preserve snapshot
        if (!F.contains("name") || !F["name"].is_string()) {
            serve_write_response(id, have_id, 2, "", "serve: frame unstash needs a string \"name\"", "");
            return;
        }
        const std::string name = F["name"].get<std::string>();
        auto it = session.stashes.find(name);
        if (it == session.stashes.end()) {
            serve_write_response(id, have_id, 3, "", std::string("serve: no stash named '") + name + "'", "");
            return;
        }
        // keep=true retains the snapshot for reuse (Stata `restore, preserve`); the
        // default drops it (plain Stata `restore`).
        const bool keep = F.contains("keep") && F["keep"].is_boolean() && F["keep"].get<bool>();
        FrameSnapshot snap = it->second;  // copy out before a possible erase
        if (!keep) session.stashes.erase(it);
        // COBRA_VERB_DIAG: `restore`'s counterpart to the `preserve` probe above —
        // times restore_state() and reports growth in the resident's owned bytes
        // over the snapshot's own owned bytes as `deep_bytes` (0 for a genuine
        // O(1) ref-restore). OFF path costs one bool check.
        const bool kVerbDiag = cecon::verb_diag_enabled();
        const int64_t bytes_before = kVerbDiag ? snap.frame.owned_payload_bytes() : 0;
        const auto t0 = cecon::verb_diag_now(kVerbDiag);
        session.restore_state(snap);
        const double restore_ms = cecon::verb_diag_ms(kVerbDiag, t0);
        if (kVerbDiag) {
            const int64_t bytes_after = session.resident.owned_payload_bytes();
            const int64_t deep_bytes = bytes_after > bytes_before ? bytes_after - bytes_before : 0;
            const int64_t rows = session.resident.n_rows();
            cecon::verb_diag_emit(
                "[verb-diag] verb=restore total_ms=%.3f restore_ms=%.3f other_ms=0.000 "
                "scratch_bytes=0 rows_in=%lld rows_out=%lld deep_bytes=%lld\n",
                restore_ms, restore_ms, static_cast<long long>(rows),
                static_cast<long long>(rows), static_cast<long long>(deep_bytes));
        }
        serve_write_response(id, have_id, 0, "", "", frame_full_desc(session.resident).dump());
        return;
    }

    if (op == "stashdrop") {  // §40 (2026-09-02, BUG-390/392): forget a named stash WITHOUT restoring it
        // The client's `use` memo (K1b) and `clear all` need to release a stash's
        // RAM; until now the only way was `unstash` (which restores it as a side
        // effect) or a same-name re-stash. Idempotent: an absent name is exit 0
        // with dropped=false, so a client can call it unconditionally at teardown.
        if (!F.contains("name") || !F["name"].is_string()) {
            serve_write_response(id, have_id, 2, "", "serve: frame stashdrop needs a string \"name\"", "");
            return;
        }
        const std::string name = F["name"].get<std::string>();
        const bool dropped = session.stashes.erase(name) > 0;
        // The count rides on stdout as JSON (the `keepif` idiom), so a client reads it
        // from CobraRunResult::stdout_text without a new frame-meta key.
        json out;
        out["name"] = name;
        out["dropped"] = dropped;
        out["stashes"] = static_cast<int64_t>(session.stashes.size());
        serve_write_response(id, have_id, 0, out.dump() + "\n", "", "");
        return;
    }

    if (op == "save_virtual") {  // tempfile save: register path -> O(1) ref-snapshot (no disk write)
        if (!session.active) { serve_write_response(id, have_id, 3, "", "serve: no frame in session", ""); return; }
        if (!F.contains("path") || !F["path"].is_string()) {
            serve_write_response(id, have_id, 2, "", "serve: frame save_virtual needs a string \"path\"", "");
            return;
        }
        const std::string path = F["path"].get<std::string>();
        session.virtual_files[path] = session.capture_state();
        json frame;
        frame["rows"] = session.resident.n_rows();
        frame["cols"] = session.resident.n_cols();
        frame["path"] = path;
        serve_write_response(id, have_id, 0, "", "", frame.dump());
        return;
    }

    if (op == "vdrop") {  // BUG-384: forget a virtual-save registration
        // Every `tempfile` save registered an O(1) ref-snapshot here — and NOTHING
        // ever erased it. Each snapshot's root_sp keeps a whole root .hbin mmapped,
        // and the client's eager scratch GC unlinks those roots while mapped, so the
        // blocks stayed allocated INVISIBLY (df shrinks, du/census see nothing). On
        // a 47-artifact corpus the worker accumulated hundreds of mappings; on a
        // volume with ~21 GB free the hidden retention crossed the ceiling at a
        // deterministic op count and every later snapshot write failed with
        // "disk full or I/O error" — the internal-volume failure signature.
        // Stata's own contract is the eraser: tempfiles die at program/do exit
        // ([P] macro), so the client calls this from exactly that reap.
        if (!F.contains("path") || !F["path"].is_string()) {
            serve_write_response(id, have_id, 2, "", "serve: frame vdrop needs a string \"path\"", "");
            return;
        }
        session.virtual_files.erase(F["path"].get<std::string>());
        serve_write_response(id, have_id, 0, "", "", "");
        return;
    }

    // ---- §31 post-estimation against a stored model -------------------------------
    // `lincom`/`nlcom`/`test` addressed by HANDLE: no dataset is read and no model is
    // re-fitted — the stored fit's own beta/V answer the combination. This is the whole
    // point of the section (54 lincoms per dependent variable were each re-reading a
    // 164 MB dataset and re-running HDFE to recompute a fit that already existed).
    if (op == "postest") {
        if (!F.contains("est") || !F["est"].is_string()) {
            serve_write_response(id, have_id, 2, "", "serve: frame postest needs a string \"est\" handle", "");
            return;
        }
        const std::string handle = F["est"].get<std::string>();
        if (!F.contains("argv") || !F["argv"].is_array()) {
            serve_write_response(id, have_id, 2, "", "serve: frame postest needs an array \"argv\"", "");
            return;
        }
        std::vector<std::string> a;
        bool argv_ok = true;
        for (const auto& e : F["argv"]) {
            if (!e.is_string()) { argv_ok = false; break; }
            a.push_back(e.get<std::string>());
        }
        if (!argv_ok || a.empty()) {
            serve_write_response(id, have_id, 2, "",
                "serve: frame postest argv must be a non-empty string array", "");
            return;
        }
        if (a[0] != "lincom" && a[0] != "nlcom" && a[0] != "test") {
            serve_write_response(id, have_id, 2, "",
                "serve: frame postest argv[0] must be lincom, nlcom or test (got '" + a[0] + "')", "");
            return;
        }
        for (const std::string& t : a)
            if (t == "--data" || t == "--out") {
                serve_write_response(id, have_id, 2, "",
                    "serve: frame postest argv may not contain --data or --out (the stored model is the input)", "");
                return;
            }
        // Unknown or evicted handle → an EXPLICIT error. Never a silent re-fit, and —
        // because handles are process-monotonic and nonce-salted — never a different
        // model. The client's contract on this rc is to re-run the full specification.
        auto mit = session.models.find(handle);
        if (mit == session.models.end()) {
            serve_write_response(id, have_id, 3, "",
                "serve: no stored model '" + handle +
                "' (expired or never minted; re-run the full model specification)", "");
            return;
        }

        // Per-request global reset — same shape as the `run` path. The root path is
        // pinned for provenance when a resident frame happens to be loaded, but a
        // postest call needs NO data, so it is equally valid with no resident at all.
        g_argv_consumed.assign(a.size(), 0);
        // BUG-168: `||` short-circuits, so with `--tidy` present `opt(a,"--format")`
        // never ran and --format was never marked consumed — the unconsumed-flag net
        // then rejected the perfectly legal `--tidy --format tidy` pair. Read both
        // sides unconditionally; the mark is the side effect that matters.
        {
            const bool tidy_flag = has(a, "--tidy");
            const bool tidy_fmt = (opt(a, "--format") == "tidy");
            g_format_tidy = tidy_flag || tidy_fmt;
        }
        g_argv.assign(1, serve_argv0);
        g_argv.insert(g_argv.end(), a.begin(), a.end());
        g_data_path = session.active ? session.root_path : std::string();
        g_provenance = !has(a, "--no-provenance");
        hash_cache_clear();
        // Seed only what is ALREADY computed (ENG-ADOPTION-AVOIDANCE): an empty
        // root_hash means the deferred hash has not been demanded yet, and seeding
        // "" would poison the memo with a bogus answer. When this request does
        // need it, hash_file_hex's serve_root_hash() bridge computes it from the
        // root mmap and memoizes it here itself.
        if (session.active && !session.root_hash.empty())
            hash_cache_put(session.root_path, session.root_hash);

        // Handle path ON (RAII — see ActiveModelGuard; the clear must not be skippable by
        // an exception), minting OFF: a handle-addressed call consumes a stored model and
        // must never publish a new one (it performs no fit to publish).
        ActiveModelGuard model_guard(&mit->second);
        g_publish_enabled = false;
        g_has_published = false;
        std::string out_bytes, err_bytes;
        bool setup_failed = false;
        int rc = serve_capture_dispatch(a, out_bytes, err_bytes, setup_failed);
        if (setup_failed) {
            serve_write_response(id, have_id, 1, "", "serve: capture setup failed", "");
            return;
        }
        // `fits` is the mechanism signal: it is the count of model FITS this process has
        // performed, so it must be IDENTICAL across every handle-addressed call. A gate
        // that watches it going up has caught the re-fit coming back.
        json est;
        est["handle"] = handle;
        est["fits"] = static_cast<int64_t>(g_est_fits);
        serve_write_response(id, have_id, rc, out_bytes, err_bytes, "", est.dump());
        return;
    }

    if (op != "run") {
        serve_write_response(id, have_id, 2, "", std::string("serve: unknown frame op '") + op + "'", "");
        return;
    }

    // ---- op == "run": one verb against the resident frame -------------------------
    if (!F.contains("argv") || !F["argv"].is_array()) {
        serve_write_response(id, have_id, 2, "", "serve: frame run needs an array \"argv\"", "");
        return;
    }
    std::vector<std::string> a;
    bool ok = true;
    for (const auto& e : F["argv"]) {
        if (!e.is_string()) { ok = false; break; }
        a.push_back(e.get<std::string>());
    }
    if (!ok || a.empty()) {
        serve_write_response(id, have_id, 2, "", "serve: frame run argv must be a non-empty string array", "");
        return;
    }
    if (!session.active) { serve_write_response(id, have_id, 3, "", "serve: no frame in session", ""); return; }
    // argv MUST NOT carry --data or --out (the resident frame IS the data source/sink)
    // → exit 2, session state untouched.
    for (const std::string& t : a)
        if (t == "--data" || t == "--out") {
            serve_write_response(id, have_id, 2, "",
                "serve: frame run argv may not contain --data or --out (resident frame is the data source/sink)", "");
            return;
        }

    // Per-request global reset — mirrors §25 EXACTLY, except g_data_path pins the ROOT
    // (resident runs read the in-RAM frame; provenance still attests the source file) and
    // the root's use-time hash is re-seeded so provenance doesn't re-scan the root file.
    g_argv_consumed.assign(a.size(), 0);
    // BUG-168: `||` short-circuits, so with `--tidy` present `opt(a,"--format")`
        // never ran and --format was never marked consumed — the unconsumed-flag net
        // then rejected the perfectly legal `--tidy --format tidy` pair. Read both
        // sides unconditionally; the mark is the side effect that matters.
        {
            const bool tidy_flag = has(a, "--tidy");
            const bool tidy_fmt = (opt(a, "--format") == "tidy");
            g_format_tidy = tidy_flag || tidy_fmt;
        }
    g_argv.assign(1, serve_argv0);
    g_argv.insert(g_argv.end(), a.begin(), a.end());
    g_data_path = session.root_path;
    g_provenance = !has(a, "--no-provenance");
    hash_cache_clear();
    if (!session.root_hash.empty()) hash_cache_put(session.root_path, session.root_hash);
    session.has_pending = false;  // clear any stale stage

    // FD-level capture (identical mechanics to §25 — see run_serve's long note).
    std::cout.flush();
    std::fflush(stdout);
    std::fflush(stderr);
    std::string cap_base = native_join_path(
        keepif_scratch_dir(), "cecon_serve_" + std::to_string(native_process_id()));
    int cap1 = native_open(cap_base + "_out.tmp", O_RDWR | O_CREAT | O_TRUNC, 0600);
    int cap2 = native_open(cap_base + "_err.tmp", O_RDWR | O_CREAT | O_TRUNC, 0600);
    int sav1 = ::dup(1), sav2 = ::dup(2);
    if (cap1 < 0 || cap2 < 0 || sav1 < 0 || sav2 < 0) {
        for (int f : {cap1, cap2, sav1, sav2}) if (f >= 0) ::close(f);
        native_unlink(cap_base + "_out.tmp");
        native_unlink(cap_base + "_err.tmp");
        serve_write_response(id, have_id, 1, "", "serve: capture setup failed", "");
        return;
    }
    native_unlink(cap_base + "_out.tmp");
    native_unlink(cap_base + "_err.tmp");
    ::dup2(cap1, 1);
    ::dup2(cap2, 2);

    int rc;
    bool discard_out = false;
    g_merge_counts = MergeCounts{};  // per-request: a response never carries a prior merge's counts
    g_active_session = &session;  // resident-run mode ON: acquire_data → view, commit_frame → stage
    // §31: a frame-mode run MAY mint. A post-estimation verb that fits from a full model
    // specification publishes that fit here, and the response hands back a handle the
    // client can address on every subsequent combination instead of re-fitting.
    g_publish_enabled = true;
    g_has_published = false;
    try {
        rc = dispatch_command(a);
    } catch (const cecon::EngineError& e) {
        discard_out = true;
        json err = {{"error", {{"code", e.code}, {"message", e.what()}}}};
        if (!e.detail.empty()) err["error"]["detail"] = e.detail;
        std::cerr << err.dump(2) << "\n";
        rc = cecon::exit_code_for(e.code);
    } catch (const std::exception& e) {
        discard_out = true;
        json err = {{"error", {{"code", "internal"}, {"message", e.what()}}}};
        std::cerr << err.dump(2) << "\n";
        rc = 1;
    }
    g_active_session = nullptr;  // resident-run mode OFF (before ANY swap / response work)
    g_publish_enabled = false;   // §31: minting window closed with the dispatch

    std::cout.flush();
    std::cerr.flush();
    std::fflush(stdout);
    std::fflush(stderr);
    ::dup2(sav1, 1);
    ::dup2(sav2, 2);
    ::close(sav1);
    ::close(sav2);
    std::string out_bytes = discard_out ? std::string() : read_capture(cap1);
    std::string err_bytes = read_capture(cap2);
    ::close(cap1);
    ::close(cap2);
    native_unlink(cap_base + "_out.tmp");
    native_unlink(cap_base + "_err.tmp");
    keepif_cleanup();

    // Commit semantics: the resident swap happens ONLY on rc==0 with no error unwind.
    // A nonzero rc (incl. check_unconsumed_flags via dispatch_guard) or a thrown error
    // leaves the session EXACTLY as before — the staged pending frame is discarded.
    std::string frame_json;
    if (rc == 0 && session.has_pending) {
        Frame& N = session.pending;
        Frame& R = session.resident;
        // Per-run delta, computed BEFORE the steal moves any buffer pointer.
        std::vector<std::string> changed, added, dropped;
        for (const FrameColumn& nc : N.columns()) {
            if (!R.has(nc.name)) { added.push_back(nc.name); continue; }
            if (fcol_ptr(nc) != fcol_ptr(R.col(nc.name))) changed.push_back(nc.name);
        }
        for (const FrameColumn& rcol : R.columns())
            if (!N.has(rcol.name)) dropped.push_back(rcol.name);
        // Two different questions share these checks — do not re-merge them.
        // `this_structural` (write-path, engine-internal): can a later `save` byte-patch the
        // source file, or must it fully rewrite? A Frame with no source_path (e.g. gsort's
        // result, built via the generic take_rows, which never carries src_path_ forward)
        // genuinely can't be patched even when nothing else changed, so source_path().empty()
        // belongs in this answer.
        // `card_stable` (read-path, over the wire): is PIET's cached per-column metadata card
        // (names/types/n_missing/n_levels) still valid? That depends only on row count and
        // column set — Frame::structure_dirty()'s own contract (frame.h) — NOT on whether this
        // Frame happens to carry a source path, so the empty-path term is deliberately excluded.
        const bool this_structural =
            N.structure_dirty() || N.source_path().empty() || N.n_rows() != R.n_rows();
        const bool card_stable = !(N.structure_dirty() || N.n_rows() != R.n_rows());
        // Steal outgoing owned buffers pending still borrows, then swap; O(touched).
        N.adopt_resident_predecessor(R);
        session.resident = std::move(session.pending);
        session.has_pending = false;
        session.structure_dirty = session.structure_dirty || this_structural;
        if (session.structure_dirty) session.resident.mark_structure_dirty();
        for (const auto& nm : changed) session.dirty_names.insert(nm);
        for (const auto& nm : added) session.dirty_names.insert(nm);
        session.rebuild_view();  // buffer pointers moved → the cached view must be rebuilt
        json frame;
        frame["rows"] = session.resident.n_rows();
        frame["cols"] = session.resident.n_cols();
        frame["changed"] = changed;
        frame["added"] = added;
        frame["dropped"] = dropped;
        frame["structure"] = this_structural;
        frame["card_stable"] = card_stable;
        // contract_A additive: present ONLY when this run staged a merge, so every
        // existing consumer and every captured fixture is byte-unchanged. This is what
        // lets `merge` leave the stateless leg without dropping the counts PIET shows
        // the user (they used to exist only in the one-shot stderr notice).
        if (g_merge_counts.valid) {
            json mc;
            mc["master_only"] = g_merge_counts.master_only;
            mc["using_only"]  = g_merge_counts.using_only;
            mc["matched"]     = g_merge_counts.matched;
            frame["merge_counts"] = std::move(mc);
        }
        json vd = json::array();
        for (const auto& nm : changed) vd.push_back(frame_var_desc(session.resident, nm));
        for (const auto& nm : added) vd.push_back(frame_var_desc(session.resident, nm));
        frame["vars_delta"] = vd;
        // MUTRES-WIRE, 2026-08-22 — the PUSH half was built here and REMOVED the
        // same day, by its own measurement. The idea was to save PIET a round trip
        // by attaching the full varlist to every response whose card_stable is
        // false. But card_stable is false for every row-changing run, and this
        // corpus makes 9,440 of those (7,840 `drop` alone) to serve 640 actual
        // card-fold refusals — a 14.8:1 waste ratio, each instance serialising 192
        // columns and forcing frame_col_n_missing on every one of them. Measured
        // cost: frame:run 477 s -> 648 s at an IDENTICAL 22,245 calls.
        //
        // The `vars` op (the PULL half, above) covers every refusal for one ~40 KB
        // round trip AT the moment of refusal, so the push bought nothing it does
        // not already have. Removed rather than narrowed: any trigger precise
        // enough to fire only on real refusals would have to know what PIET's fold
        // planner will decide, which is exactly the knowledge the engine does not
        // have — that is why the pull exists.
        frame_json = frame.dump();
    } else if (rc == 0) {  // read-only verb: no staged frame → resident unchanged
        json frame;
        frame["rows"] = session.resident.n_rows();
        frame["cols"] = session.resident.n_cols();
        frame["changed"] = json::array();
        frame["added"] = json::array();
        frame["dropped"] = json::array();
        frame["structure"] = false;
        frame["card_stable"] = true;  // read-only verb: resident unchanged, card trivially valid
        frame["vars_delta"] = json::array();
        frame_json = frame.dump();
    } else {  // failure: discard the stage, leave the session untouched, omit the frame object
        session.has_pending = false;
        session.pending = Frame();
    }
    // §31: mint a handle for a fit published by this run. Gated on rc==0 for the same
    // reason the resident swap is — a run that failed or unwound leaves no committed
    // state behind, and must not leave an addressable model behind either.
    std::string est_json;
    if (rc == 0 && g_has_published) est_json = serve_mint_model(session).dump();
    g_has_published = false;
    serve_write_response(id, have_id, rc, out_bytes, err_bytes, frame_json, est_json);
}

static int run_serve(const std::string& serve_argv0) {
    // Handshake: ONE line the kernel reads before sending requests.
    std::cout << "{\"serve\":1,\"cobra_version\":\"" << COBRA_VERSION
              << "\",\"contract_a\":\"" << CONTRACT_VERSION << "\"}\n";
    std::cout.flush();

    // Resident-frame session state (contract_A §26). Lives for the whole serve loop;
    // frame requests build it up, stateless {argv} requests below never touch it.
    ServeSession session;
    // ENG-ADOPTION-AVOIDANCE: publish it for the deferred root-hash bridge, for the
    // loop's lifetime. RAII-reset so an early return/throw cannot leave a dangling
    // pointer to this stack object.
    struct HashRootScope {
        explicit HashRootScope(ServeSession* s) { g_hash_root_session = s; }
        ~HashRootScope() { g_hash_root_session = nullptr; }
    } hash_root_scope(&session);

    std::string line;
    unsigned long long served = 0;  // relief throttle counter (below)
    while (std::getline(std::cin, line)) {
        phys_footprint_track();   // every request: a peak cannot be sampled from outside
        // Allocator hygiene (see the <malloc/malloc.h> include note): every 32nd
        // request, ask the allocator to mark its free-but-still-dirty pages
        // OS-reclaimable. Top-of-loop means the previous request's frees have all
        // landed. The 32-step throttle is request-count keyed (deterministic, never
        // wall clock); the call never touches a live allocation, so emitted bytes are
        // identical by construction.
        //
        // MEASURED 2026-08-15, AND IT RETURNS NOTHING HERE. On the reference slice all
        // twelve calls freed 0.0 MB by BOTH accounts that exist — the phys_footprint
        // delta across the call and malloc_zone_pressure_relief's own return value —
        // and `MallocNanoZone=0` does not change that. At the same run's peak the
        // allocator held 444.0 MB against 177.5 MB in use, so there was 266 MB it
        // could have given back and did not. The call is kept because it costs a zone
        // walk every 32nd request and a workload where it DOES free something would
        // now say so in the census; it is not kept because it is known to help. Do not
        // cite it as mitigation.
        // RAMDIET 2026-08-15: the cadence is env-tunable so the arms are ONE branch
        // apart in ONE binary (same discipline as COBRA_NO_CARRY). The question was
        // worth re-asking after the header-dictionary churn landed — the live small-
        // block population fell 278,337 -> 2,155 and the SMALL regions went 89% empty,
        // so relief finally had something it was ALLOWED to return.
        //
        // MEASURED, AND IT DOES NOT MOVE THE PEAK. Cadence 32 produced both 653.8 MB
        // and 406.4 MB on the same slice; cadence 1 produced 406.4 and 370.0; cadence
        // 8 produced 359.9. The run-to-run spread swamps the knob. The default stays
        // 32; this knob exists so the next person re-measures instead of re-arguing.
        //
        // (An earlier reading of this same A/B said -37.8%, from a 1 s external
        // sampler. It was wrong -- see phys_footprint_now above for why a sampler
        // cannot answer a question about peaks.)
        static const unsigned long long kReliefEvery = [] {
            if (const char* e = std::getenv("COBRA_RELIEF_EVERY")) {
                const long long v = std::atoll(e);
                if (v > 0) return static_cast<unsigned long long>(v);
            }
            return 32ull;
        }();
        if (++served % kReliefEvery == 0) {
            // RAMDIET 2026-08-15: does relief return anything AT ALL? The peak sits at
            // 407 MB dirty over 49 MB live, with 344 MB of it in regions vmmap marks
            // "(empty)", and the cadence sweep could not move that. Before designing
            // anything further, the call gets measured on both sides of itself.
            const uint64_t relief_before = phys_footprint_now();
            size_t relief_ret = 0;
#if defined(__APPLE__)
            // The return value is the allocator's OWN account of what it gave back,
            // which is the cross-check on the footprint delta: the two disagreeing
            // would mean the pages moved but the process is still charged for them.
            relief_ret = malloc_zone_pressure_relief(nullptr, 0);
#elif defined(__GLIBC__)
            relief_ret = static_cast<size_t>(malloc_trim(0));
#endif
            if (const char* mp = std::getenv("COBRA_SESSION_MEMLOG"); mp && *mp) {
                const uint64_t after = phys_footprint_now();
                if (std::FILE* f = std::fopen(mp, "a")) {
                    std::fprintf(f, "[relief] req=%llu before=%.1fMB after=%.1fMB freed=%.1fMB reported=%.1fMB\n",
                                 served, relief_before / 1048576.0, after / 1048576.0,
                                 (static_cast<double>(relief_before) - static_cast<double>(after)) / 1048576.0,
                                 relief_ret / 1048576.0);
                    std::fclose(f);
                }
            }
            // Same 32-request boundary, and on purpose: the census is most honest
            // read right AFTER a relief, where what remains is what is genuinely
            // held rather than what merely has not been returned yet.
            if (const char* mp = std::getenv("COBRA_SESSION_MEMLOG"); mp && *mp)
                session.census(mp, served);
            // RAMDIET A: the storage-ladder prize, taken on the live resident at the
            // same boundary (last write wins — a SIGTERM'd worker still leaves one).
            if (const char* tc = std::getenv("COBRA_TYPE_CENSUS"); tc && *tc)
                session.resident.type_census(tc);
            // Same boundary, same reason as COBRA_HDRLOG's cadence flush: PIET
            // SIGTERMs its workers, so atexit never runs and a flush that waits
            // for a clean exit loses the whole run. Delta-flushed; a reader sums.
            cecon::verb_diag_flush_requests();
        }
        // Parse the request line (reuse nlohmann, exceptions OFF). Strict shape:
        // {"id":<int>, "argv":[<string>, ...]} for a stateless §25 request, or
        // {"id":<int>, "frame":{"op":...}} for a resident-frame §26 request.
        json req = json::parse(line, nullptr, /*allow_exceptions=*/false);
        // COBRA_VERB_DIAG whole-request partition (src/core/verb_diag.h): bill
        // EVERY request to its own op, so the engine's wall has no residual to
        // hide in. The four-verb probes covered 41.4% of it; this covers all of
        // it by construction.
        const bool kReqDiag = cecon::verb_diag_enabled();
        const auto req_t0 = cecon::verb_diag_now(kReqDiag);
        std::string req_op;
        if (kReqDiag) {
            if (req.is_object() && req.contains("frame") && req["frame"].is_object()
                && req["frame"].contains("op") && req["frame"]["op"].is_string()) {
                req_op = "frame:" + req["frame"]["op"].get<std::string>();
            } else if (req.is_object() && req.contains("argv") && req["argv"].is_array()
                       && !req["argv"].empty() && req["argv"][0].is_string()) {
                req_op = "argv:" + req["argv"][0].get<std::string>();
            } else {
                req_op = "malformed";
            }
        }
        struct ReqBill {
            bool on; const std::string& op; cecon::VerbDiagClock::time_point t0;
            ~ReqBill() { if (on) cecon::verb_diag_bill(op, cecon::verb_diag_ms(on, t0)); }
        } req_bill{kReqDiag, req_op, req_t0};
        // §26: a well-formed `frame` object routes to the resident-session handler
        // (which writes its own response); everything else stays the §25 argv path.
        if (req.is_object() && req.contains("frame") && req["frame"].is_object()) {
            run_frame_request(req, session, serve_argv0);
            continue;
        }
        bool ok = req.is_object() && req.contains("argv") && req["argv"].is_array();
        std::vector<std::string> a;
        if (ok)
            for (const auto& e : req["argv"]) {
                if (!e.is_string()) { ok = false; break; }
                a.push_back(e.get<std::string>());
            }
        bool have_id = ok && req.contains("id") && req["id"].is_number_integer();
        long long id = have_id ? req["id"].get<long long>() : 0;

        if (!ok) {  // malformed line → id null, exit 2, usage-style stderr; CONTINUE
            std::cout << "{\"id\":null,\"exit\":2,\"stdout\":\"\",\"stderr\":"
                         "\"serve: malformed request line (need {id:<int>, argv:[<string>,...]})\"}\n";
            std::cout.flush();
            continue;
        }
        if (a.size() == 1 && a[0] == "shutdown") return 0;  // clean shutdown

        // Per-request global reset — mirrors main()'s one-shot per-process setup
        // EXACTLY, so each request is independent modulo process-global leakage.
        // hash_cache_clear drops the hash_file_hex path→hash memo (a path rewritten
        // between requests must re-hash or provenance.fnv1a64 goes stale). g_argv
        // gets the REAL process argv[0] so provenance{argv} is byte-identical to a
        // one-shot run invoked via the same binary path (dual-run parity leg law;
        // the synthetic "cobra" placeholder RED-flagged 62/101 provenance cases).
        g_argv_consumed.assign(a.size(), 0);
        // BUG-168: `||` short-circuits, so with `--tidy` present `opt(a,"--format")`
        // never ran and --format was never marked consumed — the unconsumed-flag net
        // then rejected the perfectly legal `--tidy --format tidy` pair. Read both
        // sides unconditionally; the mark is the side effect that matters.
        {
            const bool tidy_flag = has(a, "--tidy");
            const bool tidy_fmt = (opt(a, "--format") == "tidy");
            g_format_tidy = tidy_flag || tidy_fmt;
        }
        g_argv.assign(1, serve_argv0);
        g_argv.insert(g_argv.end(), a.begin(), a.end());
        g_data_path = opt(a, "--data");
        g_provenance = !has(a, "--no-provenance");
        hash_cache_clear();

        // FD-LEVEL capture: dup2 fds 1/2 onto per-request scratch files (same tmp-dir
        // convention as the keepif scratch) for the span of the dispatch, so C-stdio
        // bytes (handlers' printf / fprintf(stderr,…)) and iostream bytes land in the
        // SAME two sinks, in order — sync_with_stdio is never disabled in this
        // codebase, so cout/cerr write through stdout/stderr's FILE streams. Protocol
        // lines are written only AFTER the fds are restored, so no handler byte can
        // reach the protocol stream. Each scratch file is unlinked the moment it is
        // open (anonymous fd, read back via lseek): no exit path can leak it.
        std::cout.flush();    // drain pending protocol bytes through BOTH buffering
        std::fflush(stdout);  // layers to the REAL stdout, pre-redirect
        std::fflush(stderr);
        std::string cap_base = native_join_path(
            keepif_scratch_dir(), "cecon_serve_" + std::to_string(native_process_id()));
        int cap1 = native_open(cap_base + "_out.tmp", O_RDWR | O_CREAT | O_TRUNC, 0600);
        int cap2 = native_open(cap_base + "_err.tmp", O_RDWR | O_CREAT | O_TRUNC, 0600);
        int sav1 = ::dup(1), sav2 = ::dup(2);
        if (cap1 < 0 || cap2 < 0 || sav1 < 0 || sav2 < 0) {
            for (int f : {cap1, cap2, sav1, sav2})
                if (f >= 0) ::close(f);
            native_unlink(cap_base + "_out.tmp");
            native_unlink(cap_base + "_err.tmp");
            std::cout << "{\"id\":" << (have_id ? std::to_string(id) : std::string("null"))
                      << ",\"exit\":1,\"stdout\":\"\",\"stderr\":"
                         "\"serve: capture setup failed\"}\n";
            std::cout.flush();
            continue;
        }
        native_unlink(cap_base + "_out.tmp");
        native_unlink(cap_base + "_err.tmp");
        ::dup2(cap1, 1);
        ::dup2(cap2, 2);

        // Dispatch, replicating main()'s policy: the empty-argv usage blurb (rc 2,
        // shared kUsageBlurb) and the two catch arms are main()'s own, and the error
        // envelope goes to std::cerr WHILE fd 2 is still captured — so it lands AFTER
        // the handler's own stderr output, the same single-stream order one-shot
        // emits (handler stderr streams live during dispatch; main()'s catch prints
        // the envelope after). BUG-110 discard-on-throw moves to the response layer:
        // on a throw the captured fd-1 bytes are dropped (stdout field = ""), never
        // emitted — one-shot flushes its buffered stdout only on a clean return.
        int rc;
        bool discard_out = false;
        try {
            if (a.empty()) {
                std::fprintf(stderr, "%s", kUsageBlurb);
                rc = 2;
            } else {
                rc = dispatch_command(a);
            }
        } catch (const cecon::EngineError& e) {
            discard_out = true;
            json err = {{"error", {{"code", e.code}, {"message", e.what()}}}};
            if (!e.detail.empty()) err["error"]["detail"] = e.detail;
            std::cerr << err.dump(2) << "\n";
            rc = cecon::exit_code_for(e.code);
        } catch (const std::exception& e) {
            discard_out = true;
            json err = {{"error", {{"code", "internal"}, {"message", e.what()}}}};
            std::cerr << err.dump(2) << "\n";
            rc = 1;
        }
        // Drain both buffering layers INTO the capture fds, THEN swap the real fds
        // back. Order matters: flush before dup2-restore, or buffered handler bytes
        // would flush later into the restored (protocol) stdout.
        std::cout.flush();
        std::cerr.flush();
        std::fflush(stdout);
        std::fflush(stderr);
        ::dup2(sav1, 1);
        ::dup2(sav2, 2);
        ::close(sav1);
        ::close(sav2);
        std::string out_bytes = discard_out ? std::string() : read_capture(cap1);
        std::string err_bytes = read_capture(cap2);
        ::close(cap1);
        ::close(cap2);
        // Windows refuses unlink-while-open, so the eager unlinks above are no-ops
        // there — this post-close pass is the one that lands (ENOENT on POSIX, fine).
        native_unlink(cap_base + "_out.tmp");
        native_unlink(cap_base + "_err.tmp");
        keepif_cleanup();  // per-request temp unlink (one-shot relies on atexit at process end)

        // Response: ONE line, written to the REAL stdout only now.
        std::string resp = "{\"id\":";
        if (have_id) resp += std::to_string(id); else resp += "null";
        resp += ",\"exit\":";
        resp += std::to_string(rc);
        resp += ",\"stdout\":\"";
        serve_escape(resp, out_bytes);
        resp += "\",\"stderr\":\"";
        serve_escape(resp, err_bytes);
        resp += "\"}\n";
        std::cout << resp;
        std::cout.flush();
    }
    // Final flush for a clean stdin-close exit (SIGTERM path is covered by the
    // cadence flush above). Idempotent.
    cecon::verb_diag_flush_requests();
    return 0;  // EOF on stdin → clean exit 0
}

int main(int argc, char** argv) {
#ifdef _WIN32
    // Make the one-shot streams, the serve line-JSON protocol, and the fd-level
    // captures obey one explicit byte contract. Without this the Windows CRT
    // translates LF <-> CRLF on console/pipe descriptors while the capture files
    // are read back as binary — and the port's gate is a BYTE-identical fixture
    // comparison, so a stray \r is a hard failure, not cosmetic.
    native_set_standard_streams_binary();

    // BUG-050: a narrow main() receives argv re-decoded through the ANSI
    // codepage, which turns a non-ANSI (Korean) path into '?' on a non-CP949
    // system and mismatches the UTF-8 paths piet passes via CreateProcessW's
    // UTF-16 command line. Recover the true args from GetCommandLineW and
    // re-expose them as UTF-8 so every downstream path is UTF-8 and opens wide
    // (io/win_compat.h to_native_path/native_open). u8store/u8argv live for the
    // whole of main(), so argv stays valid.
    std::vector<std::string> u8store;
    std::vector<char*> u8argv;
    if (int wargc = 0; LPWSTR* wv = CommandLineToArgvW(GetCommandLineW(), &wargc)) {
        u8store.reserve(static_cast<size_t>(wargc));
        for (int i = 0; i < wargc; ++i) {
            int n = WideCharToMultiByte(CP_UTF8, 0, wv[i], -1, nullptr, 0, nullptr, nullptr);
            std::string s;
            if (n > 0) {
                s.resize(static_cast<size_t>(n));
                WideCharToMultiByte(CP_UTF8, 0, wv[i], -1, s.data(), n, nullptr, nullptr);
                s.resize(static_cast<size_t>(n - 1));  // drop the trailing NUL
            }
            u8store.push_back(std::move(s));
        }
        LocalFree(wv);
        for (auto& s : u8store) u8argv.push_back(s.data());
        u8argv.push_back(nullptr);
        argc = wargc;
        argv = u8argv.data();
    }
#endif
#ifdef __linux__
    // Pin OpenBLAS to a single thread BEFORE it initializes. OpenBLAS reads
    // OPENBLAS_NUM_THREADS in a load-time constructor that runs before main(), so
    // setting it here in-process is too late — the only reliable way to inject it
    // from inside the binary is to set it and re-exec once. cobra owns parallelism
    // through its own ThreadPool; a nested multi-threaded BLAS just oversubscribes
    // a CPU-capped sandbox (2-2.6x slower) and can deadlock under a pid limit. A
    // user who sets OPENBLAS_NUM_THREADS themselves opts out (no re-exec).
    if (!std::getenv("OPENBLAS_NUM_THREADS")) {
        setenv("OPENBLAS_NUM_THREADS", "1", 1);
        setenv("OMP_NUM_THREADS", "1", 1);
        execv("/proc/self/exe", argv);  // returns only on failure; then fall through
    }
#endif
    cecon::init_blas_threads();  // backstop (runtime API) if re-exec was unavailable
    // Unlink any --keepif scratch on every exit path (normal return, error return,
    // or an EngineError that escapes to the catch below and returns). atexit fires
    // after main returns regardless of which path was taken.
    std::atexit(keepif_cleanup);
    std::vector<std::string> a(argv + 1, argv + argc);
    // `serve` is a new top-level subcommand: session mode (contract_A §25, MAXPERF
    // Stage 1a). Handled here — before the one-shot per-process global setup and the
    // buffered single-dispatch below — so the one-shot path stays byte-untouched
    // (`serve` was previously just `unknown command: serve`, exit 2). run_serve()
    // does its own per-request global setup + buffered capture (mirroring main's).
    if (!a.empty() && a[0] == "serve") return run_serve(argv[0]);
    // E6: fresh per-process tracking for the unknown-flag net (see opt()/has()/
    // opt_all()/check_unconsumed_flags above) — explicit rather than relying on
    // mark_consumed's lazy same-size-skips-reinit check, so a future caller that
    // ever invokes main()'s dispatch more than once in a process can't inherit
    // stale marks from a same-length-but-different `a`.
    g_argv_consumed.assign(a.size(), 0);
    // Global output-format switch: `--format tidy` (or `--tidy`) adds a uniform
    // coeftable to every estimator's JSON, and `--no-provenance` disables the
    // provenance{} block. Read here (unconditionally, before dispatch) so every
    // command accepts them transparently — this also means they're always marked
    // "consumed" for the E6 net below regardless of which command runs.
    // BUG-168: `||` short-circuits, so with `--tidy` present `opt(a,"--format")`
        // never ran and --format was never marked consumed — the unconsumed-flag net
        // then rejected the perfectly legal `--tidy --format tidy` pair. Read both
        // sides unconditionally; the mark is the side effect that matters.
        {
            const bool tidy_flag = has(a, "--tidy");
            const bool tidy_fmt = (opt(a, "--format") == "tidy");
            g_format_tidy = tidy_flag || tidy_fmt;
        }
    g_argv.assign(argv, argv + argc);
    g_data_path = opt(a, "--data");
    g_provenance = !has(a, "--no-provenance");
    try {
        if (a.empty()) {
            // kUsageBlurb (above): one definition, shared with serve's empty-argv
            // request path — identical bytes on both, by construction.
            std::fprintf(stderr, "%s", kUsageBlurb);
            return 2;
        }
        // Buffer stdout across dispatch (BUG-110 fix): the E6 unknown-flag net
        // (dispatch_guard → check_unconsumed_flags) can only tell a flag was unread
        // AFTER the command has already produced its result. By capturing that result
        // into a buffer and flushing it only on a clean return — discarding it if an
        // unrecognized flag (or any error) unwinds — a silently-wrong result for an
        // ignored option can never reach stdout (it errors on stderr, exit 3, instead).
        // Uses the flags ACTUALLY consumed, so no options_table() imperfection can
        // reintroduce the skew. `--out` writes go to their file (unbuffered), and
        // stderr (warnings, usage) always streams live.
        std::ostringstream captured;
        std::streambuf* real_cout = std::cout.rdbuf(captured.rdbuf());
        int rc;
        try {
            rc = dispatch_command(a);
        } catch (...) {
            std::cout.rdbuf(real_cout);  // restore real stdout; DISCARD the captured buffer
            // BUG-182: the handler threw, so dispatch_guard was never reached and the
            // options-table drift check unwound past. Run it HERE, on the throwing path,
            // before the error below is reported — this is the ONLY way a refuse-by-name
            // flag (the flags a command consumes solely in order to reject them) can be
            // verified against its declaration, and refusing by name is how this codebase
            // satisfies gate ① for everything it does not implement. No-op unless
            // COBRA_OPTIONS_STRICT is set, so the shipped path is byte-unchanged; under
            // the gate a drift REPLACES the handler's own error, which is correct — an
            // undeclared flag is the more fundamental defect and the one no other check
            // can see.
            check_options_table_drift(a);
            throw;                        // → the EngineError/std::exception catches below
        }
        std::cout.rdbuf(real_cout);
        std::cout << captured.str();      // clean run: emit the buffered result verbatim
        return rc;
    } catch (const cecon::EngineError& e) {
        // Structured, machine-parseable error: stable code + message (+ detail),
        // emitted as JSON on stderr with a code-derived exit status (see error.h).
        json err = {{"error", {{"code", e.code}, {"message", e.what()}}}};
        if (!e.detail.empty()) err["error"]["detail"] = e.detail;
        std::cerr << err.dump(2) << "\n";
        return cecon::exit_code_for(e.code);
    } catch (const std::exception& e) {
        json err = {{"error", {{"code", "internal"}, {"message", e.what()}}}};
        std::cerr << err.dump(2) << "\n";
        return 1;
    }
}
