// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// R4: grouped test runner — generated 2026-09-01 (see CMakeLists.txt).
// One invocation runs ONE test (argv[1] = the ctest name), so consolidation
// changes the number of BINARIES, not the isolation model.
#include <cstdio>
#include <cstring>

int test_csdid_long2_grid_main();
int test_determinism_main();
int test_fvops_main();
int test_iv_hansenj_main();
int test_margins_at_main();
int test_neweyprais_main();
int test_radix_sort_main();
int test_ts_diag_main();
int test_xtabond_main();

int main(int argc, char** argv) {
    struct Ent { const char* name; int (*fn)(); };
    static const Ent kTests[] = {
    {"csdid_long2_grid", &test_csdid_long2_grid_main},
    {"determinism", &test_determinism_main},
    {"fvops", &test_fvops_main},
    {"iv_hansenj", &test_iv_hansenj_main},
    {"margins_at", &test_margins_at_main},
    {"neweyprais", &test_neweyprais_main},
    {"radix_sort", &test_radix_sort_main},
    {"ts_diag", &test_ts_diag_main},
    {"xtabond", &test_xtabond_main},
    };
    if (argc != 2) {
        std::fprintf(stderr, "usage: runner07 <testname>\n");
        for (const Ent& e : kTests) std::fprintf(stderr, "  %s\n", e.name);
        return 2;
    }
    for (const Ent& e : kTests)
        if (std::strcmp(argv[1], e.name) == 0) return e.fn();
    std::fprintf(stderr, "runner07: unknown test '%s'\n", argv[1]);
    return 2;
}
