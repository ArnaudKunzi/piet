// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// R4: grouped test runner — generated 2026-09-01 (see CMakeLists.txt).
// One invocation runs ONE test (argv[1] = the ctest name), so consolidation
// changes the number of BINARIES, not the isolation model.
#include <cstdio>
#include <cstring>

int test_csdid_missing_x_main();
int test_drdid_main();
int test_glm_main();
int test_iv_multi_main();
int test_mediate_main();
int test_nlcom_main();
int test_rdlocal_main();
int test_ts_expr_main();
int test_xtipo_main();

int main(int argc, char** argv) {
    struct Ent { const char* name; int (*fn)(); };
    static const Ent kTests[] = {
    {"csdid_missing_x", &test_csdid_missing_x_main},
    {"drdid", &test_drdid_main},
    {"glm", &test_glm_main},
    {"iv_multi", &test_iv_multi_main},
    {"mediate", &test_mediate_main},
    {"nlcom", &test_nlcom_main},
    {"rdlocal", &test_rdlocal_main},
    {"ts_expr", &test_ts_expr_main},
    {"xtipo", &test_xtipo_main},
    };
    if (argc != 2) {
        std::fprintf(stderr, "usage: runner08 <testname>\n");
        for (const Ent& e : kTests) std::fprintf(stderr, "  %s\n", e.name);
        return 2;
    }
    for (const Ent& e : kTests)
        if (std::strcmp(argv[1], e.name) == 0) return e.fn();
    std::fprintf(stderr, "runner08: unknown test '%s'\n", argv[1]);
    return 2;
}
