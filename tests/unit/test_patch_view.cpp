// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// test_patch_view: MECHOPT W2 (PATCH_VIEW). Frame::patch_dataset_view updates a
// cached make_view() Dataset in place, MOVING unchanged entries and rebuilding
// only columns whose payload identity moved. The serve session's rebuild_view()
// rides this on every committed frame run / use / unstash, and a wrong entry
// there is a silent wrong answer in every resident-mode verb — so this gate
// compares PAYLOAD AND DICTIONARY CONTENT field-for-field against a fresh
// as_dataset_view() after every step of a mutation sequence, not just the card.
//
// The three holes the design review named, each pinned here:
//   (a) equivalence: patched view == fresh view, every field, every step;
//   (b) the mechanism actually engages: an untouched factor column's dictionary
//       buffer is MOVED, not re-copied (pointer identity across the patch) —
//       the mutant kill for "patch quietly always rebuilds";
//   (c) the ABA hole: drop a factor column and mint another with the same name,
//       same rows, same dictionary SIZE — the generation stamp must force a
//       rebuild of that entry (a pointer-keyed patcher can reuse the freed
//       address and serve the OLD dictionary).
#include <cstdint>
#include <cstdio>
#include <string>
#include <utility>
#include <vector>

#include "data/frame.h"
#include "io/columnar.h"

using namespace cecon;

static int g_fail = 0;
#define CHECK(cond, msg)                                                         \
    do {                                                                         \
        if (!(cond)) {                                                           \
            std::fprintf(stderr, "FAIL: %s (%s:%d)\n", msg, __FILE__, __LINE__); \
            ++g_fail;                                                            \
        }                                                                        \
    } while (0)

// Field-for-field equality of two view Datasets — payloads by pointer (both
// views of the SAME frame must hand out the same buffer), dictionaries by
// CONTENT (the fresh view owns a fresh copy, so pointers differ by design).
static void check_views_equal(const Dataset& a, const Dataset& b, const char* where) {
    CHECK(a.n_rows() == b.n_rows(), where);
    CHECK(a.n_cols() == b.n_cols(), where);
    CHECK(a.path() == b.path(), where);
    CHECK(a.sorted_by() == b.sorted_by(), where);
    if (a.n_cols() != b.n_cols()) return;
    for (int i = 0; i < a.n_cols(); ++i) {
        const Column& ca = a.columns()[i];
        const Column& cb = b.columns()[i];
        CHECK(ca.name == cb.name, where);
        CHECK(ca.dtype == cb.dtype, where);
        CHECK(ca.data == cb.data, where);      // same frame ⇒ same live buffer
        CHECK(ca.nbytes == cb.nbytes, where);
        CHECK(ca.levels == cb.levels, where);  // dictionary CONTENT
        CHECK(ca.format == cb.format, where);
        CHECK(ca.label == cb.label, where);
    }
}

static Frame make_frame() {
    Frame f;
    f.set_n_rows_hint(4);
    f.append_f64_column("x", {1.0, 2.0, 3.0, 4.0});
    f.append_f64_column("y", {10.0, 20.0, 30.0, 40.0});
    f.append_factor_column_coded("team", {0, 1, 0, 1}, {"red", "blue"});
    f.append_factor_column_coded("city", {1, 0, 1, 0}, {"seoul", "busan"});
    f.finalize_after_bulk_load();
    return f;
}

int test_patch_view_main() {
    std::printf("test_patch_view:\n");

    // ---- (a) equivalence across a mutation sequence -------------------------
    Frame f = make_frame();
    Dataset view = f.as_dataset_view();

    auto step = [&](const char* what) {
        Dataset fresh = f.as_dataset_view();
        bool ok = f.patch_dataset_view(view);
        CHECK(ok, what);
        check_views_equal(view, fresh, what);
    };

    step("noop patch");

    // in-place numeric write on a uniquely-held column (address may not move —
    // the generation bump is what makes this visible)
    f.mutable_col("x").f64.mut()[2] = 99.0;
    step("inplace f64 write");

    // factor mutation: level dictionary changes
    {
        FrameColumn& c = f.mutable_col("team");
        c.levels.mut().push_back("green");
        c.codes.mut()[0] = 2;
    }
    step("factor level append");

    // metadata-only mutations: payload untouched
    f.mutable_col("y").format = "%9.2f";
    step("format change");
    f.mutable_col("y").label = "yield";
    step("label change");

    // add + drop + reorder
    f.append_f64_column("z", {5.0, 6.0, 7.0, 8.0});
    f.finalize_after_bulk_load();
    step("add column");
    f.drop("x");
    step("drop column");
    f = std::move(f).order({"z"}, "first", "");
    step("reorder");

    // ---- (b) the mechanism engages: unchanged dictionary is MOVED ----------
    {
        Frame g = make_frame();
        Dataset v = g.as_dataset_view();
        const std::string* levels_before = v.col("city").levels.data();
        g.mutable_col("x").f64.mut()[0] = -1.0;  // touch something ELSE
        bool ok = g.patch_dataset_view(v);
        CHECK(ok, "mechanism patch ok");
        const std::string* levels_after = v.col("city").levels.data();
        CHECK(levels_before == levels_after,
              "untouched factor dictionary must be MOVED (same buffer), not re-copied "
              "— if this fails the patch is silently a full rebuild");
    }

    // ---- (c) the ABA hole: same name, same rows, same dictionary size ------
    {
        Frame g;
        g.set_n_rows_hint(4);
        g.append_f64_column("k", {1, 2, 3, 4});
        g.append_factor_column_coded("t", {0, 1, 0, 1}, {"old_a", "old_b"});
        g.finalize_after_bulk_load();
        Dataset v = g.as_dataset_view();
        g.drop("t");
        g.append_factor_column_coded("t", {1, 0, 1, 0}, {"new_a", "new_b"});
        g.finalize_after_bulk_load();
        bool ok = g.patch_dataset_view(v);
        CHECK(ok, "aba patch ok");
        Dataset fresh = g.as_dataset_view();
        check_views_equal(v, fresh, "aba");
        CHECK(v.col("t").levels == std::vector<std::string>({"new_a", "new_b"}),
              "recreated factor must carry the NEW dictionary — a pointer-keyed patcher "
              "can serve the OLD one off a recycled allocation");
    }

    // ---- precondition: row-count mismatch refuses the patch ----------------
    {
        Frame g = make_frame();
        Dataset v = g.as_dataset_view();
        Frame h;
        h.set_n_rows_hint(1);
        h.append_f64_column("only", {1.0});
        h.finalize_after_bulk_load();
        CHECK(!h.patch_dataset_view(v), "row-count mismatch must refuse the patch");
    }

    if (g_fail == 0) std::printf("test_patch_view: ALL OK\n");
    return g_fail == 0 ? 0 : 1;
}
