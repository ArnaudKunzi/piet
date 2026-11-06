// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// test_cow_frame: MAXPERF Stage 2 column-CoW store (contract_A §27). Verifies:
//   (a) derived frames SHARE untouched column payloads (order / clonevar = O(1) ref);
//   (b) a mutation isolates via column-CoW (clone iff shared → the sibling is untouched);
//   (c) a stash / unstash round-trip (Frame copy = preserve, copy-back = restore) is
//       byte-equal and independent of later resident mutation;
//   (d) a virtual save -> use round-trip returns the saved values after the resident
//       has moved on;
//   (e) a borrowed-root column survives a stash ACROSS a resident swap
//       (adopt_resident_predecessor keeps the root mmap alive);
//   (f) MAXPERF Stage 2 Wave β1 — a factor column's levels BORROW the Dataset's own
//       level strings (no copy) at from_dataset_shared; a mutation detaches (materializes
//       into an owned copy) and leaves the Dataset's own dictionary untouched;
//   (g) Wave β2 — the n_missing cache is correct on first ask, stays cached on repeat
//       asks, and is correctly recomputed (never stale) after a mutable access through
//       EACH of the three funnels (f64, codes, levels) and across a column-CoW clone.
//
// The serve ops (stash / unstash / save_virtual / use) are thin wrappers over exactly
// these Frame-level primitives — a Frame COPY captures/restores, FrameColumn::detach
// does the CoW, adopt_resident_predecessor reattaches across a swap. ServeSession
// itself lives in the cobra app TU, so its wire protocol is gated end-to-end by the
// serve_session dual-run eval leg; this unit pins the data-plane invariants they rely on.
//
// NOTE: reads that must OBSERVE sharing use the const FrameColumn::payload() (a bare
// non-const proxy access would itself detach); mutations go through the real verb path.
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <limits>
#include <string>
#include <utility>
#include <vector>

#include "data/frame.h"
#include "io/columnar.h"
#include "io/win_compat.h"
#include "nlohmann/json.hpp"

using namespace cecon;
using nlohmann::json;
static int g_fail = 0;
static void check(const char* what, bool ok) {
    if (!ok) { std::printf("  FAIL %s\n", what); ++g_fail; }
}

// Tiny all-f64 .hbin writer + loader for the borrowed-root path (mirrors test_d2).
static void write_hbin_f64(const std::string& path, int64_t n,
                           const std::vector<std::pair<std::string, std::vector<double>>>& cols) {
    constexpr int64_t A = 64;
    auto up = [](int64_t x) { return (x + A - 1) / A * A; };
    json h; h["magic"] = "CECONHB1"; h["version"] = 1; h["n_rows"] = n;
    json jc = json::array(); int64_t rel = 0;
    for (auto& c : cols) {
        int64_t nb = (int64_t)c.second.size() * 8;
        jc.push_back({{"name", c.first}, {"dtype", "f64"}, {"offset", rel}, {"nbytes", nb}});
        rel = up(rel + nb);
    }
    h["columns"] = jc;
    std::string hj = h.dump();
    std::ofstream f(to_native_path(path), std::ios::binary);
    f.write("CECONHB1", 8);
    uint64_t hl = hj.size(); f.write((const char*)&hl, 8); f.write(hj.data(), hj.size());
    int64_t ds = up(16 + (int64_t)hl), pos = 16 + (int64_t)hl;
    std::vector<char> pad(A, 0);
    if (pos < ds) { f.write(pad.data(), ds - pos); pos = ds; }
    for (size_t i = 0; i < cols.size(); ++i) {
        int64_t want = ds + jc[i]["offset"].get<int64_t>();
        if (pos < want) { f.write(pad.data(), want - pos); pos = want; }
        f.write((const char*)cols[i].second.data(), (int64_t)cols[i].second.size() * 8);
        pos += (int64_t)cols[i].second.size() * 8;
    }
}

// Tiny single-factor-column .hbin writer (codes + levels dictionary) for the Wave β1
// levels-borrow test (f).
static void write_hbin_factor(const std::string& path, int64_t n, const std::string& name,
                              const std::vector<int32_t>& codes,
                              const std::vector<std::string>& levels) {
    constexpr int64_t A = 64;
    auto up = [](int64_t x) { return (x + A - 1) / A * A; };
    json h; h["magic"] = "CECONHB1"; h["version"] = 1; h["n_rows"] = n;
    int64_t nb = (int64_t)codes.size() * 4;
    json jc = json::array();
    jc.push_back({{"name", name}, {"dtype", "factor"}, {"offset", 0}, {"nbytes", nb}, {"levels", levels}});
    h["columns"] = jc;
    std::string hj = h.dump();
    std::ofstream f(to_native_path(path), std::ios::binary);
    f.write("CECONHB1", 8);
    uint64_t hl = hj.size(); f.write((const char*)&hl, 8); f.write(hj.data(), hj.size());
    int64_t ds = up(16 + (int64_t)hl), pos = 16 + (int64_t)hl;
    std::vector<char> pad(A, 0);
    if (pos < ds) { f.write(pad.data(), ds - pos); pos = ds; }
    f.write((const char*)codes.data(), nb);
}

// In-memory frame built from OWNED columns (no mmap) for the pure-sharing cases.
static Frame owned_frame(int64_t n,
                         const std::vector<std::pair<std::string, std::vector<double>>>& cols) {
    Frame f;
    f.set_n_rows_hint(n);
    for (auto& c : cols) f.append_f64_column(c.first, c.second);
    f.finalize_after_bulk_load();
    return f;
}

int test_cow_frame_main() {
    std::printf("test_cow_frame:\n");

    // ---------- (a) derived frames SHARE untouched column payloads ----------
    {
        Frame a = owned_frame(4, {{"x", {1, 2, 3, 4}}, {"y", {5, 6, 7, 8}}, {"z", {9, 10, 11, 12}}});
        // `order` is a pure column permutation → it must ref-copy every column, not
        // deep-copy it: each payload pointer stays identical across the derived frame.
        Frame b = a.order({"z"}, "first", "");
        check("order shares x payload", a.col("x").payload() == b.col("x").payload());
        check("order shares y payload", a.col("y").payload() == b.col("y").payload());
        check("order shares z payload", a.col("z").payload() == b.col("z").payload());
        check("order permuted column list", b.n_cols() == 3 && b.columns()[0].name == "z");

        // clonevar = a SECOND ref to the same ColumnData (a FrameColumn copy shares it).
        FrameColumn dup = a.col("x");
        check("clonevar shares payload", dup.payload() == a.col("x").payload());
    }

    // ---------- (b) column-CoW isolates a mutation (sibling unchanged) ----------
    {
        Frame a = owned_frame(3, {{"x", {1, 2, 3}}});
        Frame b = a;  // copy: b shares a's x payload
        check("pre-CoW shared", a.col("x").payload() == b.col("x").payload());
        b.generate("x", "x + 10");  // mutate b's x → column-CoW clones it first
        check("post-CoW payloads diverged", a.col("x").payload() != b.col("x").payload());
        check("original x[0] intact", a.col("x").f64[0] == 1);
        check("original x[2] intact", a.col("x").f64[2] == 3);
        check("mutated x[0]", b.col("x").f64[0] == 11);
        check("mutated x[2]", b.col("x").f64[2] == 13);
    }

    // ---------- (c) stash / unstash round-trip: byte-equal + independent ----------
    {
        Frame resident = owned_frame(5, {{"a", {1, 2, 3, 4, 5}}, {"b", {10, 20, 30, 40, 50}}});
        Frame stash = resident;  // = ServeSession::capture_state()'s Frame copy (preserve)
        // The resident moves on after the snapshot: replace a column, drop one, add one.
        resident.generate("a", "a * 100");
        resident.drop("b");
        resident.append_f64_column("c", {7, 7, 7, 7, 7});
        resident.finalize_after_bulk_load();
        // The stash must be byte-unchanged by all of that.
        check("stash keeps a[0]", stash.col("a").f64[0] == 1);
        check("stash keeps a[4]", stash.col("a").f64[4] == 5);
        check("stash keeps b", stash.has("b") && stash.col("b").f64[2] == 30);
        check("stash has no c", !stash.has("c"));
        check("stash n_cols == 2", stash.n_cols() == 2);
        // Restore (unstash): copy the snapshot back; it is independent of the resident.
        Frame restored = stash;  // = ServeSession::restore_state()'s Frame copy (restore)
        check("restore a[3]", restored.col("a").f64[3] == 4);
        check("restore b[3]", restored.col("b").f64[3] == 40);
        check("resident diverged from stash", resident.col("a").f64[3] == 400);
    }

    // ---------- (d) virtual-file save -> use round-trip ----------
    {
        Frame resident = owned_frame(3, {{"v", {1, 2, 3}}});
        Frame saved = resident;   // save_virtual: register path -> snapshot (tempfile save)
        resident.generate("v", "v + 1");  // the resident advances
        Frame used = saved;       // use of the virtual path returns the saved snapshot
        check("virtual use restores v[0]", used.col("v").f64[0] == 1);
        check("virtual use restores v[2]", used.col("v").f64[2] == 3);
        check("resident advanced v[0]", resident.col("v").f64[0] == 2);
        check("virtual shares saved payload", used.col("v").payload() == saved.col("v").payload());
    }

    // ---------- (e) borrowed-root column survives a stash across a resident swap ----------
    {
        const std::string root_path = native_temp_path("cecon_cow_root.hbin");
        write_hbin_f64(root_path, 4, {{"p", {1, 2, 3, 4}}, {"q", {5, 6, 7, 8}}});
        Dataset ds = Dataset::open(root_path);
        Frame resident = Frame::from_dataset(ds);
        check("root column p borrowed", resident.col("p").borrowed());
        const void* p_root = resident.col("p").payload();  // the mmap span pointer

        Frame stash = resident;  // preserve while p/q still BORROW the root mmap
        check("stash shares borrowed p", stash.col("p").payload() == p_root);

        // Simulate the serve §26 commit path: build a pending frame that BORROWS the
        // resident's view spans, add a derived column, adopt, then swap by moving —
        // which destroys the old resident.
        Dataset view = resident.as_dataset_view();
        Frame pending = Frame::from_dataset(view);     // borrows the resident's buffers
        pending.generate("r", "p + q");                // touches only the NEW column
        pending.adopt_resident_predecessor(resident);  // reattach borrowed passthrough cols
        resident = std::move(pending);                 // old resident destroyed here

        // The stash must still read valid data: p/q borrow the root mmap, which the
        // stash keeps alive via its own src_, so they survive the swap.
        check("stash p intact after swap", stash.col("p").f64[0] == 1 && stash.col("p").f64[3] == 4);
        check("stash q intact after swap", stash.col("q").f64[1] == 6);
        check("stash still borrows the root span", stash.col("p").payload() == p_root);
        // The new resident carries the derived column and the passthrough columns.
        check("resident has derived r", resident.has("r") && resident.col("r").f64[0] == 6);
        check("resident p passthrough", resident.col("p").f64[2] == 3);
    }

    // ---------- (f) Wave β1: factor levels BORROW the Dataset's own strings ----------
    {
        const std::string levels_path = native_temp_path("cecon_cow_levels.hbin");
        write_hbin_factor(levels_path, 4, "obs_id",
                          {0, 1, 2, 0}, {"alice", "bob", "carol"});
        // from_dataset_shared (the exact path serve's `use` takes, contract_A §26) keeps
        // the SAME Dataset object alive via src_ — so the Dataset accessed through `sp`
        // below is literally what the Frame borrowed from (from_dataset's owns_mmap()
        // branch re-opens an independent mmap instead, which would defeat the pointer
        // check: same bytes, different object).
        auto sp = std::make_shared<Dataset>(Dataset::open(levels_path));
        Frame f = Frame::from_dataset_shared(sp);
        // NOTE (mirrors the file-header caution): `col` is a const FrameColumn&, so
        // `col.levels` (the LevelCol PROXY) resolves to its const, non-materializing
        // overloads. Going through `col.data->levels` instead would NOT be const-safe —
        // shared_ptr's constness doesn't propagate to the pointee, so a bare `.data()`
        // there picks the MUTABLE CowVec overload and detaches on the spot.
        const FrameColumn& col = f.col("obs_id");
        check("levels borrowed (no copy) right after from_dataset_shared", col.levels.borrowed());
        check("levels pointer == Dataset's own strings (no copy)",
              col.levels.data() == sp->col("obs_id").levels.data());
        check("levels content correct", col.levels.size() == 3 && col.levels[0] == "alice" &&
                                        col.levels[1] == "bob" && col.levels[2] == "carol");

        // Mutation detaches: push_back funnels through LevelCol::mut() -> detach() ->
        // CowVec<string>::materialize(), so the Frame's own dictionary grows while the
        // Dataset's copy (still reachable via `sp`) must stay byte-unchanged.
        f.mutable_col("obs_id").levels.push_back("dave");
        check("frame levels grew after mutation", f.col("obs_id").levels.size() == 4 &&
                                                  f.col("obs_id").levels[3] == "dave");
        check("frame levels now owned (detached)", !f.col("obs_id").levels.borrowed());
        check("Dataset levels untouched (size)", sp->col("obs_id").levels.size() == 3);
        check("Dataset levels untouched (content)", sp->col("obs_id").levels[0] == "alice" &&
                                                    sp->col("obs_id").levels[2] == "carol");
    }

    // ---------- (g) Wave β2: n_missing cache, invalidated at every mutable-access path ----------
    {
        const double NA = std::numeric_limits<double>::quiet_NaN();

        // -- f64 mutable-access path (F64Col::mut() -> detach()) --
        Frame f = owned_frame(4, {{"x", {1, NA, 3, 4}}});
        check("n_missing f64 initial", f.col("x").n_missing(f.n_rows()) == 1);
        check("n_missing f64 stays cached on repeat ask", f.col("x").n_missing(f.n_rows()) == 1);
        f.mutable_col("x").f64[1] = 20;  // un-missing row 1
        f.mutable_col("x").f64[3] = NA;  // row 3 becomes missing
        check("n_missing f64 recomputed after f64 mutable access (not stale)",
              f.col("x").n_missing(f.n_rows()) == 1);

        // -- codes mutable-access path (CodeCol::mut() -> detach()) on a factor column --
        Frame ff; ff.set_n_rows_hint(3);
        ff.append_factor_column("g", {"a", "", "b"});  // "" -> missing code -1
        ff.finalize_after_bulk_load();
        check("n_missing factor initial", ff.col("g").n_missing(ff.n_rows()) == 1);
        ff.mutable_col("g").codes[0] = -1;  // row 0 now ALSO missing
        check("n_missing recomputed after codes mutable access (not stale)",
              ff.col("g").n_missing(ff.n_rows()) == 2);

        // -- levels mutable-access path (LevelCol::mut() -> detach()) --
        // Pushing a new level can't itself change which codes are missing, but the
        // funnel invalidates unconditionally (Correctness law: any doubt, invalidate) —
        // this proves the pessimistic invalidation still recomputes the CORRECT value,
        // not a stale-but-accidentally-right one and not a corrupted one.
        int64_t before = ff.col("g").n_missing(ff.n_rows());  // re-primes the cache
        ff.mutable_col("g").levels.push_back("c");
        check("n_missing correct (unchanged) after levels mutable access",
              ff.col("g").n_missing(ff.n_rows()) == before);

        // -- column-CoW sibling isolation: a shared cache must not cross-contaminate --
        Frame g1 = owned_frame(3, {{"y", {1, NA, 3}}});
        Frame g2 = g1;  // copy: shares y's ColumnData (and whatever cache state it holds)
        check("n_missing g1 initial", g1.col("y").n_missing(g1.n_rows()) == 1);
        check("g1/g2 share payload pre-mutation", g1.col("y").payload() == g2.col("y").payload());
        g2.generate("y", "y + 100");  // mutates g2's y -> detach() clones -> separate caches
        check("n_missing g1 unaffected by g2's mutation", g1.col("y").n_missing(g1.n_rows()) == 1);
        check("n_missing g2 recomputed correctly (NaN+100 stays missing)",
              g2.col("y").n_missing(g2.n_rows()) == 1);
    }

    // ---------- (h) L4-FIX-B: order -> write_hbin is a CoW-clone + header reorder ----------
    // A pure column reorder of a Frame loaded from an .hbin must NOT re-emit the payload:
    // write_hbin takes the patch path (clone the source + rewrite the column directory in
    // the new order, reusing every payload offset). This pins the VALUE-IDENTITY + the new
    // column ORDER of the written file — the observable contract the fast path must keep.
    {
        const std::string src_path = native_temp_path("cecon_reorder_src.hbin");
        const std::string out_path = native_temp_path("cecon_reorder_out.hbin");
        const std::string rename_path = native_temp_path("cecon_rename_out.hbin");
        write_hbin_f64(src_path, 4,
                       {{"p", {1, 2, 3, 4}}, {"q", {5, 6, 7, 8}}, {"r", {9, 10, 11, 12}}});
        Dataset ds = Dataset::open(src_path);
        Frame src = Frame::from_dataset(ds);
        Frame ordered = std::move(src).order({"r"}, "first", "");  // -> [r, p, q]
        ordered.write_hbin(out_path);

        Dataset out_ds = Dataset::open(out_path);
        Frame out = Frame::from_dataset(out_ds);
        check("reorder wrote 3 cols", out.n_cols() == 3);
        check("reorder new order [r,p,q]", out.columns()[0].name == "r" &&
              out.columns()[1].name == "p" && out.columns()[2].name == "q");
        check("reorder r values intact", out.col("r").f64[0] == 9 && out.col("r").f64[3] == 12);
        check("reorder p values intact", out.col("p").f64[0] == 1 && out.col("p").f64[3] == 4);
        check("reorder q values intact", out.col("q").f64[1] == 6 && out.col("q").f64[2] == 7);

        // Bijection precheck: a RENAME is not a reorder (a source name goes missing), so it
        // must stay on the value-correct full-rewrite path — verify it still round-trips.
        Frame src2 = Frame::from_dataset(ds);
        src2.rename({{"q", "q2"}});
        src2.write_hbin(rename_path);
        Frame rn = Frame::from_dataset(Dataset::open(rename_path));
        check("rename round-trips values", rn.has("q2") && !rn.has("q") &&
              rn.col("q2").f64[0] == 5 && rn.col("p").f64[2] == 3 && rn.col("r").f64[3] == 12);
    }

    if (g_fail) { std::printf("FAILED (%d)\n", g_fail); return 1; }
    std::printf("  ok — sharing, column-CoW, stash/unstash, virtual, borrowed-swap, "
                "levels-borrow, n_missing cache, order-reorder write\nPASSED\n");
    return 0;
}
