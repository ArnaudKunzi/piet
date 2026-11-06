// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// R4: grouped test runner — generated 2026-09-01 (see CMakeLists.txt).
// One invocation runs ONE test (argv[1] = the ctest name), so consolidation
// changes the number of BINARIES, not the isolation model.
#include <cstdio>
#include <cstring>

int test_binscatter_main();
int test_csdid_separation_main();
int test_dta114_main();
int test_glmqp_main();
int test_iv_singleton_main();
int test_miimpute_main();
int test_nlgmm_main();
int test_reg3_main();
int test_ts_operators_main();
int test_xtlogit_main();

int main(int argc, char** argv) {
    struct Ent { const char* name; int (*fn)(); };
    static const Ent kTests[] = {
    {"binscatter", &test_binscatter_main},
    {"csdid_separation", &test_csdid_separation_main},
    {"dta114", &test_dta114_main},
    {"glmqp", &test_glmqp_main},
    {"iv_singleton", &test_iv_singleton_main},
    {"miimpute", &test_miimpute_main},
    {"nlgmm", &test_nlgmm_main},
    {"reg3", &test_reg3_main},
    {"ts_operators", &test_ts_operators_main},
    {"xtlogit", &test_xtlogit_main},
    };
    if (argc != 2) {
        std::fprintf(stderr, "usage: runner01 <testname>\n");
        for (const Ent& e : kTests) std::fprintf(stderr, "  %s\n", e.name);
        return 2;
    }
    for (const Ent& e : kTests)
        if (std::strcmp(argv[1], e.name) == 0) return e.fn();
    std::fprintf(stderr, "runner01: unknown test '%s'\n", argv[1]);
    return 2;
}
