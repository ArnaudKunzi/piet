// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// R4: grouped test runner — generated 2026-09-01 (see CMakeLists.txt).
// One invocation runs ONE test (argv[1] = the ctest name), so consolidation
// changes the number of BINARIES, not the isolation model.
#include <cstdio>
#include <cstring>

int test_csdid_main();
int test_dd_accum_main();
int test_fepois_ll_main();
int test_iv_main();
int test_lincom_main();
int test_mlogit_main();
int test_posthest_main();
int test_svy_main();
int test_wald_main();

int main(int argc, char** argv) {
    struct Ent { const char* name; int (*fn)(); };
    static const Ent kTests[] = {
    {"csdid", &test_csdid_main},
    {"dd_accum", &test_dd_accum_main},
    {"fepois_ll", &test_fepois_ll_main},
    {"iv", &test_iv_main},
    {"lincom", &test_lincom_main},
    {"mlogit", &test_mlogit_main},
    {"posthest", &test_posthest_main},
    {"svy", &test_svy_main},
    {"wald", &test_wald_main},
    };
    if (argc != 2) {
        std::fprintf(stderr, "usage: runner05 <testname>\n");
        for (const Ent& e : kTests) std::fprintf(stderr, "  %s\n", e.name);
        return 2;
    }
    for (const Ent& e : kTests)
        if (std::strcmp(argv[1], e.name) == 0) return e.fn();
    std::fprintf(stderr, "runner05: unknown test '%s'\n", argv[1]);
    return 2;
}
