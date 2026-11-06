// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// MAXPERF Stage 3 — morsel-parallel determinism gate (①-gate proof obligation).
//
// The Stage-3 kernels (Frame::collapse aggregation loop; merge_frames column
// materialization) are dispatched across ThreadPool workers. Gate ① requires the
// output to be BIT-IDENTICAL run-to-run AND across worker counts on a given build.
// This test proves exactly that: it runs each kernel under CECON_NUM_THREADS=1 and
// =4 and asserts the emitted column bytes are byte-for-byte identical.
//
// Mechanism: the ThreadPool worker count is a process-wide singleton read ONCE from
// CECON_NUM_THREADS (threads.cpp default_threads()), so two distinct counts require
// two distinct PROCESSES. The parent starts one child per count; each child sets the
// env, builds the synthetic input, runs the kernels, and dumps every output column's
// raw bytes (row count, names, dtype, f64 bits / factor codes + level strings) to a
// file. The parent then byte-compares the 1-thread vs 4-thread files. A raw double
// dump (not a value tolerance) is deliberate — ANY reassociated float sum would flip
// low bits and fail, which is precisely the ① contract for the frozen summation order.
//
// The parent touches NOTHING thread-related before spawning, so each child
// initializes the pool fresh from its own CECON_NUM_THREADS. POSIX uses
// fork/waitpid; Windows has no fork, so the parent re-executes ITSELF with a
// `--child <nthreads> <base>` argv and waits — same two-process shape, same
// assertions, nothing is skipped.
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "data/frame.h"
#include "io/win_compat.h"

using namespace cecon;

#if defined(_WIN32)
#include <process.h>   // _wspawnl, _P_WAIT, _putenv_s
#include <windows.h>   // GetModuleFileNameW
#else
#include <sys/wait.h>
#include <unistd.h>
#endif

static int fail = 0;
static void check(bool cond, const std::string& msg) {
    if (!cond) { std::printf("  FAIL: %s\n", msg.c_str()); fail = 1; }
    else std::printf("  ok: %s\n", msg.c_str());
}

// ---- synthetic inputs (deterministic; groups + f64 + factor + missing) -------

static Frame make_collapse_input() {
    const int64_t n = 4096;
    Frame f;
    f.set_n_rows_hint(n);
    // f64 by-keys here: Frame::collapse ALSO accepts factor by-keys (L1 M3 native-verb
    // campaign — grouped on the underlying int32 code, same K-matrix every other
    // by-group helper uses), but that path's OWN determinism/value coverage lives in
    // test_data_verbs.cpp (collapse factor-by vs pre-converted-f64 equivalence), not
    // here. This fork-based bit-identical gate stays f64-only so it keeps exercising
    // the float-accumulation kernels (mean/sum/sd reassociation) it was built for.
    // Factor-column coverage of the Stage-3 parallel kernels ALSO lives in the merge
    // cases below (factor payload gather + factor key gather + u_remap), where
    // factors genuinely flow through that kernel.
    // g: 4 group values + NaN -> ~20 {g,h} groups, ~150-200 rows each, so a within-group
    // sum spans many values — a hypothetical within-group split WOULD reassociate.
    std::vector<double> g(n);
    for (int64_t i = 0; i < n; ++i) g[i] = (i % 5 == 0) ? NAN : static_cast<double>(i % 4);
    f.append_f64_column("g", g);
    // h: second f64 by-key with missing (NaN keys form their own group, key_eq: NaN==NaN)
    std::vector<double> h(n);
    for (int64_t i = 0; i < n; ++i) h[i] = (i % 11 == 0) ? NAN : static_cast<double>(i % 7);
    f.append_f64_column("h", h);
    // value columns with scattered missing
    std::vector<double> x(n), y(n), w(n);
    for (int64_t i = 0; i < n; ++i) {
        x[i] = (i % 13 == 0) ? NAN : static_cast<double>((i * 2654435761u) % 1000) * 0.5 - 250.0;
        y[i] = (i % 17 == 0) ? NAN : static_cast<double>((i * 40503u) % 997) * 0.25 - 100.0;
        w[i] = (i % 23 == 0) ? NAN : static_cast<double>(1 + (i % 5));  // fweight: positive ints
    }
    f.append_f64_column("x", x);
    f.append_f64_column("y", y);
    f.append_f64_column("w", w);
    f.finalize_after_bulk_load();
    return f;
}

static Frame run_collapse(const Frame& in) {
    // Broad spec set: float-accum (mean/sum/sd) + order-free (count/min/max/first/last/
    // firstnm/lastnm) + percentile (median/p25). Distinct out names to keep columns clean.
    std::vector<Frame::CollapseSpec> specs = {
        {"mean", "x", "mean_x"}, {"sum", "x", "sum_x"},   {"sd", "y", "sd_y"},
        {"count", "x", "cnt_x"}, {"min", "y", "min_y"},   {"max", "y", "max_y"},
        {"first", "x", "first_x"}, {"last", "x", "last_x"}, {"median", "x", "med_x"},
        {"p25", "y", "p25_y"},   {"firstnm", "x", "fnm_x"}, {"lastnm", "y", "lnm_y"}};
    return in.collapse(specs, {"g", "h"}, "");
}

static Frame run_wcollapse(const Frame& in) {
    // Weighted (fweight) float accumulation per group — the strictest bit path.
    std::vector<Frame::CollapseSpec> specs = {
        {"mean", "x", "wmean_x"}, {"sum", "x", "wsum_x"},
        {"sd", "x", "wsd_x"},     {"count", "x", "wcnt_x"}};
    return in.collapse(specs, {"g"}, "w");
}

// merge #1 — numeric key, factor + f64 payloads with missing, 1:m expansion.
static Frame run_merge_numkey() {
    const int64_t nm = 1500, nu = 4000;
    Frame master, usingf;
    master.set_n_rows_hint(nm);
    usingf.set_n_rows_hint(nu);
    std::vector<double> mid(nm), mx(nm);
    std::vector<std::string> mfac(nm);
    const char* ML[3] = {"red", "green", "blue"};
    for (int64_t i = 0; i < nm; ++i) {
        mid[i] = static_cast<double>(i);  // unique -> 1:m master side
        mfac[i] = (i % 9 == 0) ? std::string("") : std::string(ML[i % 3]);
        mx[i] = (i % 7 == 0) ? NAN : static_cast<double>(i) * 1.5 - 10.0;
    }
    master.append_f64_column("id", mid);
    master.append_factor_column("mfac", mfac);
    master.append_f64_column("mx", mx);
    master.finalize_after_bulk_load();

    std::vector<double> uid(nu), uy(nu);
    std::vector<std::string> ufac(nu);
    const char* UL[4] = {"x", "y", "z", "w"};
    for (int64_t j = 0; j < nu; ++j) {
        uid[j] = static_cast<double>(j % 2000);  // ids 0..1999, each twice; 1500..1999 using-only
        ufac[j] = (j % 8 == 0) ? std::string("") : std::string(UL[j % 4]);
        uy[j] = (j % 6 == 0) ? NAN : static_cast<double>(j) * 0.25;
    }
    usingf.append_f64_column("id", uid);
    usingf.append_factor_column("ufac", ufac);
    usingf.append_f64_column("uy", uy);
    usingf.finalize_after_bulk_load();

    MergeOpts opts;
    opts.type = "1:m";
    opts.keylist = {"id"};
    return merge_frames(master, usingf, opts).frame;
}

// merge #2 — FACTOR key (exercises the key-column factor gather + u_remap unification).
static Frame run_merge_factkey() {
    const int64_t nm = 600, nu = 1600;
    Frame master, usingf;
    master.set_n_rows_hint(nm);
    usingf.set_n_rows_hint(nu);
    std::vector<std::string> mk(nm);
    std::vector<double> m2x(nm);
    for (int64_t i = 0; i < nm; ++i) {
        mk[i] = "k" + std::to_string(i);  // 600 unique labels
        m2x[i] = (i % 5 == 0) ? NAN : static_cast<double>(i) * 2.0;
    }
    master.append_factor_column("k", mk);
    master.append_f64_column("m2x", m2x);
    master.finalize_after_bulk_load();

    std::vector<std::string> uk(nu);
    std::vector<double> u2y(nu);
    for (int64_t j = 0; j < nu; ++j) {
        uk[j] = "k" + std::to_string(j % 800);  // k0..k799 (k600..k799 = using-only), each twice
        u2y[j] = (j % 4 == 0) ? NAN : static_cast<double>(j) * 0.5;
    }
    usingf.append_factor_column("k", uk);
    usingf.append_f64_column("u2y", u2y);
    usingf.finalize_after_bulk_load();

    MergeOpts opts;
    opts.type = "1:m";
    opts.keylist = {"k"};
    return merge_frames(master, usingf, opts).frame;
}

// merge #3 — NARROW master (BUG-395, 2026-09-02). keep_if's gather keeps a narrow
// column narrow (f32 payload, no f64 cache), and the parallel gather in merge_frames
// used to read it through F64Col::ref() -> ColumnData::ensure_wide(), a const method
// that MUTATES the shared cache — from every worker at once. Before the fix the
// 4-worker child dies with "pointer being freed was not allocated" (SIGABRT), so this
// case is RED by crash, not by diff; after it, the bytes must also equal the 1-worker
// run (the reader's float->double cast is the same cast ensure_wide performs).
static Frame run_merge_narrow() {
    // Sized so the widening race has a real window: 24 narrow payload columns of
    // 60k rows. The engine crash reproduced on 29 columns x 3600 rows; a 2-column
    // toy passed the un-fixed build by luck, which is not a certificate.
    const int64_t nm = 60000, nu = 90000;
    Frame master, usingf;
    master.set_n_rows_hint(nm);
    usingf.set_n_rows_hint(nu);
    std::vector<float> mid(nm);
    std::vector<double> mz(nm);
    for (int64_t i = 0; i < nm; ++i) {
        mid[i] = static_cast<float>(i);                                   // narrow KEY
        mz[i] = (i % 11 == 0) ? NAN : static_cast<double>(i) * 0.125;      // one wide payload beside it
    }
    master.append_f64_column_narrow("id", std::move(mid));
    for (int c = 0; c < 24; ++c) {
        std::vector<float> mx(nm);
        for (int64_t i = 0; i < nm; ++i)
            mx[i] = (i % (7 + c) == 0) ? NAN : static_cast<float>(i) * (1.5f + c) - 10.f;  // narrow payload
        master.append_f64_column_narrow("mx" + std::to_string(c), std::move(mx));
    }
    master.append_f64_column("mz", mz);
    master.finalize_after_bulk_load();

    std::vector<double> uid(nu);
    std::vector<float> uy(nu);
    for (int64_t j = 0; j < nu; ++j) {
        uid[j] = static_cast<double>(j % 70000);                           // 60000..69999 using-only
        uy[j] = (j % 6 == 0) ? NAN : static_cast<float>(j) * 0.25f;        // narrow USING payload
    }
    usingf.append_f64_column("id", uid);
    usingf.append_f64_column_narrow("uy", std::move(uy));
    usingf.finalize_after_bulk_load();

    MergeOpts opts;
    opts.type = "1:m";
    opts.keylist = {"id"};
    return merge_frames(master, usingf, opts).frame;
}

// ---- raw byte dump of a frame's output columns (the "output file") -----------

static void wr(std::FILE* fp, const void* p, size_t n) {
    if (std::fwrite(p, 1, n, fp) != n) { std::perror("fwrite"); _exit(4); }
}
static void dump_frame(const Frame& f, const std::string& path) {
    std::FILE* fp = native_fopen(path, "wb");
    if (!fp) { std::perror(("fopen " + path).c_str()); _exit(3); }
    int64_t nr = f.n_rows();
    int32_t nc = f.n_cols();
    wr(fp, &nr, sizeof nr);
    wr(fp, &nc, sizeof nc);
    for (const FrameColumn& c : f.columns()) {
        uint32_t nlen = static_cast<uint32_t>(c.name.size());
        wr(fp, &nlen, 4);
        wr(fp, c.name.data(), nlen);
        uint8_t isf = c.is_factor ? 1u : 0u;
        wr(fp, &isf, 1);
        if (c.is_factor) {
            for (int64_t i = 0; i < nr; ++i) { int32_t v = c.codes[i]; wr(fp, &v, 4); }
            uint32_t nl = static_cast<uint32_t>(c.levels.size());
            wr(fp, &nl, 4);
            for (uint32_t l = 0; l < nl; ++l) {
                const std::string& s = c.levels[l];
                uint32_t sl = static_cast<uint32_t>(s.size());
                wr(fp, &sl, 4);
                wr(fp, s.data(), sl);
            }
        } else {
            // Raw f64 BITS (not a formatted value): a reassociated sum flips low bits and
            // this dump differs — exactly the ① byte-equality we are gating.
            for (int64_t i = 0; i < nr; ++i) { double v = c.f64[i]; wr(fp, &v, 8); }
        }
    }
    if (std::fclose(fp) != 0) { std::perror("fclose"); _exit(5); }
}

// ---- driver ------------------------------------------------------------------

struct Case { const char* tag; Frame (*run)(); };

static Frame case_collapse()  { return run_collapse(make_collapse_input()); }
static Frame case_wcollapse() { return run_wcollapse(make_collapse_input()); }
static Frame case_merge_num() { return run_merge_numkey(); }
static Frame case_merge_fac() { return run_merge_factkey(); }
static Frame case_merge_narrow() { return run_merge_narrow(); }

static std::string path_for(const std::string& base, const char* tag, int nthr) {
    return base + "_" + tag + "_" + std::to_string(nthr) + ".bin";
}

static bool files_equal(const std::string& a, const std::string& b) {
    std::FILE* fa = native_fopen(a, "rb");
    std::FILE* fb = native_fopen(b, "rb");
    if (!fa || !fb) { if (fa) std::fclose(fa); if (fb) std::fclose(fb); return false; }
    bool eq = true;
    for (;;) {
        char ba[65536], bb[65536];
        size_t na = std::fread(ba, 1, sizeof ba, fa);
        size_t nb = std::fread(bb, 1, sizeof bb, fb);
        if (na != nb) { eq = false; break; }
        if (na == 0) break;
        if (std::memcmp(ba, bb, na) != 0) { eq = false; break; }
    }
    std::fclose(fa);
    std::fclose(fb);
    return eq;
}

static const std::vector<Case>& all_cases() {
    static const std::vector<Case> cases = {
        {"collapse", case_collapse},
        {"wcollapse", case_wcollapse},
        {"merge_num", case_merge_num},
        {"merge_fac", case_merge_fac},
        {"merge_narrow", case_merge_narrow},
    };
    return cases;
}

// The whole of the child's work, shared by the POSIX fork child and the Windows
// re-exec child so both sides run byte-for-byte the same kernels.
static int run_child(int nthr, const std::string& base) {
#if defined(_WIN32)
    ::_putenv_s("CECON_NUM_THREADS", std::to_string(nthr).c_str());
#else
    setenv("CECON_NUM_THREADS", std::to_string(nthr).c_str(), 1);
#endif
    try {
        for (const Case& c : all_cases())
            dump_frame(c.run(), path_for(base, c.tag, nthr));
    } catch (const std::exception& e) {
        std::fprintf(stderr, "child(%d) threw: %s\n", nthr, e.what());
        return 2;
    }
    return 0;
}

#if defined(_WIN32)
// Windows has no fork. The parent re-executes its OWN image with `--child <n>
// <base>`; the child sets CECON_NUM_THREADS before anything touches the pool,
// so it still gets a fresh first-call-cached worker count — the property this
// gate depends on. _wspawnl (not system()/popen) keeps the exe path wide, so a
// build living under a Hangul directory still finds itself.
int main(int argc, char** argv) {
    if (argc == 4 && std::strcmp(argv[1], "--child") == 0) {
        return run_child(std::atoi(argv[2]), argv[3]);
    }

    std::printf("test_morsel_determinism:\n");
    const std::string base =
        "./morsel_det_" + std::to_string(static_cast<long>(native_process_id()));
    const std::wstring wbase = to_native_path(base).wstring();

    wchar_t exe[32768];
    constexpr DWORD exe_cap = static_cast<DWORD>(sizeof(exe) / sizeof(exe[0]));
    const DWORD exe_len = ::GetModuleFileNameW(nullptr, exe, exe_cap);
    auto spawn = [&](int nthr) -> int {
        if (exe_len == 0 || exe_len >= exe_cap) return -1;
        const std::wstring wn = std::to_wstring(nthr);
        const intptr_t rc = ::_wspawnl(_P_WAIT, exe, exe, L"--child", wn.c_str(), wbase.c_str(),
                                       static_cast<wchar_t*>(nullptr));
        return rc < 0 ? -1 : static_cast<int>(rc);
    };

    const int st1 = spawn(1);
    const int st4 = spawn(4);
    check(st1 >= 0 && st4 >= 0, "spawned both worker-count children");
    check(st1 == 0, "child CECON_NUM_THREADS=1 exited cleanly");
    check(st4 == 0, "child CECON_NUM_THREADS=4 exited cleanly");

    if (st1 == 0 && st4 == 0) {
        for (const Case& c : all_cases()) {
            std::string a = path_for(base, c.tag, 1), b = path_for(base, c.tag, 4);
            check(files_equal(a, b),
                  std::string(c.tag) + ": output byte-identical across 1 vs 4 workers");
        }
    }

    // cleanup (best-effort)
    for (const Case& c : all_cases()) {
        native_unlink(path_for(base, c.tag, 1));
        native_unlink(path_for(base, c.tag, 4));
    }

    std::printf("%s\n", fail ? "FAILED" : "PASSED");
    return fail;
}
#else
int main() {
    std::printf("test_morsel_determinism:\n");
    const std::vector<Case>& cases = all_cases();
    const std::string base =
        "./morsel_det_" + std::to_string(static_cast<long>(native_process_id()));

    // IMPORTANT: do nothing thread-related before forking, so each child initializes the
    // ThreadPool fresh from its own CECON_NUM_THREADS (a process-wide, first-call-cached
    // singleton). The children do ALL the compute.
    auto spawn = [&](int nthr) -> pid_t {
        pid_t pid = fork();
        if (pid < 0) { std::perror("fork"); return -1; }
        if (pid == 0) {
            _exit(run_child(nthr, base));
        }
        return pid;
    };

    pid_t p1 = spawn(1);
    pid_t p4 = spawn(4);
    check(p1 > 0 && p4 > 0, "forked both worker-count children");

    int st1 = 0, st4 = 0;
    if (p1 > 0) waitpid(p1, &st1, 0);
    if (p4 > 0) waitpid(p4, &st4, 0);
    check(p1 > 0 && WIFEXITED(st1) && WEXITSTATUS(st1) == 0, "child CECON_NUM_THREADS=1 exited cleanly");
    check(p4 > 0 && WIFEXITED(st4) && WEXITSTATUS(st4) == 0, "child CECON_NUM_THREADS=4 exited cleanly");

    if (p1 > 0 && p4 > 0 && WIFEXITED(st1) && WEXITSTATUS(st1) == 0 &&
        WIFEXITED(st4) && WEXITSTATUS(st4) == 0) {
        for (const Case& c : cases) {
            std::string a = path_for(base, c.tag, 1), b = path_for(base, c.tag, 4);
            check(files_equal(a, b),
                  std::string(c.tag) + ": output byte-identical across 1 vs 4 workers");
        }
    }

    // cleanup (best-effort)
    for (const Case& c : cases) {
        native_unlink(path_for(base, c.tag, 1));
        native_unlink(path_for(base, c.tag, 4));
    }

    std::printf("%s\n", fail ? "FAILED" : "PASSED");
    return fail;
}
#endif  // _WIN32 (parent driver: re-exec on Windows, fork on POSIX)
