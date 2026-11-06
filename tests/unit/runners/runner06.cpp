// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// R4: grouped test runner — generated 2026-09-01 (see CMakeLists.txt).
// One invocation runs ONE test (argv[1] = the ctest name), so consolidation
// changes the number of BINARIES, not the isolation model.
#include <cstdio>
#include <cstring>

int test_csdid_estat_main();
int test_demean_aa_main();
int test_frame_prewave_main();
int test_iv_diagnostics_main();
int test_logit_main();
int test_nbreg_main();
int test_qreg_main();
int test_tobit_main();
int test_wildboot_main();

int main(int argc, char** argv) {
    struct Ent { const char* name; int (*fn)(); };
    static const Ent kTests[] = {
    {"csdid_estat", &test_csdid_estat_main},
    {"demean_aa", &test_demean_aa_main},
    {"frame_prewave", &test_frame_prewave_main},
    {"iv_diagnostics", &test_iv_diagnostics_main},
    {"logit", &test_logit_main},
    {"nbreg", &test_nbreg_main},
    {"qreg", &test_qreg_main},
    {"tobit", &test_tobit_main},
    {"wildboot", &test_wildboot_main},
    };
    if (argc != 2) {
        std::fprintf(stderr, "usage: runner06 <testname>\n");
        for (const Ent& e : kTests) std::fprintf(stderr, "  %s\n", e.name);
        return 2;
    }
    for (const Ent& e : kTests)
        if (std::strcmp(argv[1], e.name) == 0) return e.fn();
    std::fprintf(stderr, "runner06: unknown test '%s'\n", argv[1]);
    return 2;
}
