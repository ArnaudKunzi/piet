// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// R4: grouped test runner — generated 2026-09-01 (see CMakeLists.txt).
// One invocation runs ONE test (argv[1] = the ctest name), so consolidation
// changes the number of BINARIES, not the isolation model.
#include <cstdio>
#include <cstring>

int test_cli_prewave_main();
int test_csv_main();
int test_dta_missing_roundtrip_main();
int test_hdfe_main();
int test_ivml_main();
int test_missing_main();
int test_oglm_main();
int test_rowegen_main();
int test_tsma_main();
int test_xtpois_main();

int main(int argc, char** argv) {
    struct Ent { const char* name; int (*fn)(); };
    static const Ent kTests[] = {
    {"cli_prewave", &test_cli_prewave_main},
    {"csv", &test_csv_main},
    {"dta_missing_roundtrip", &test_dta_missing_roundtrip_main},
    {"hdfe", &test_hdfe_main},
    {"ivml", &test_ivml_main},
    {"missing", &test_missing_main},
    {"oglm", &test_oglm_main},
    {"rowegen", &test_rowegen_main},
    {"tsma", &test_tsma_main},
    {"xtpois", &test_xtpois_main},
    };
    if (argc != 2) {
        std::fprintf(stderr, "usage: runner02 <testname>\n");
        for (const Ent& e : kTests) std::fprintf(stderr, "  %s\n", e.name);
        return 2;
    }
    for (const Ent& e : kTests)
        if (std::strcmp(argv[1], e.name) == 0) return e.fn();
    std::fprintf(stderr, "runner02: unknown test '%s'\n", argv[1]);
    return 2;
}
