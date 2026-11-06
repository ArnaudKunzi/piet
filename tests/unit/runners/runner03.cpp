// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// R4: grouped test runner — generated 2026-09-01 (see CMakeLists.txt).
// One invocation runs ONE test (argv[1] = the ctest name), so consolidation
// changes the number of BINARIES, not the isolation model.
#include <cstdio>
#include <cstring>

int test_coll_tsf_main();
int test_d2_main();
int test_dta_reader_main();
int test_heckman_main();
int test_kprk2_main();
int test_mixedlm_main();
int test_ols_synth_main();
int test_sdid_main();
int test_unitroot_main();
int test_xtreg_main();

int main(int argc, char** argv) {
    struct Ent { const char* name; int (*fn)(); };
    static const Ent kTests[] = {
    {"coll_tsf", &test_coll_tsf_main},
    {"d2", &test_d2_main},
    {"dta_reader", &test_dta_reader_main},
    {"heckman", &test_heckman_main},
    {"kprk2", &test_kprk2_main},
    {"mixedlm", &test_mixedlm_main},
    {"ols_synth", &test_ols_synth_main},
    {"sdid", &test_sdid_main},
    {"unitroot", &test_unitroot_main},
    {"xtreg", &test_xtreg_main},
    };
    if (argc != 2) {
        std::fprintf(stderr, "usage: runner03 <testname>\n");
        for (const Ent& e : kTests) std::fprintf(stderr, "  %s\n", e.name);
        return 2;
    }
    for (const Ent& e : kTests)
        if (std::strcmp(argv[1], e.name) == 0) return e.fn();
    std::fprintf(stderr, "runner03: unknown test '%s'\n", argv[1]);
    return 2;
}
