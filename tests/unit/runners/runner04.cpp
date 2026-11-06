// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// R4: grouped test runner — generated 2026-09-01 (see CMakeLists.txt).
// One invocation runs ONE test (argv[1] = the ctest name), so consolidation
// changes the number of BINARIES, not the isolation model.
#include <cstdio>
#include <cstring>

int test_cow_frame_main();
int test_data_verbs_main();
int test_eventstudy_main();
int test_hettest_main();
int test_liml_main();
int test_mkspline_main();
int test_patch_view_main();
int test_survival_main();
int test_varmodel_main();
int test_zicount_main();

int main(int argc, char** argv) {
    struct Ent { const char* name; int (*fn)(); };
    static const Ent kTests[] = {
    {"cow_frame", &test_cow_frame_main},
    {"data_verbs", &test_data_verbs_main},
    {"eventstudy", &test_eventstudy_main},
    {"hettest", &test_hettest_main},
    {"liml", &test_liml_main},
    {"mkspline", &test_mkspline_main},
    {"patch_view", &test_patch_view_main},
    {"survival", &test_survival_main},
    {"varmodel", &test_varmodel_main},
    {"zicount", &test_zicount_main},
    };
    if (argc != 2) {
        std::fprintf(stderr, "usage: runner04 <testname>\n");
        for (const Ent& e : kTests) std::fprintf(stderr, "  %s\n", e.name);
        return 2;
    }
    for (const Ent& e : kTests)
        if (std::strcmp(argv[1], e.name) == 0) return e.fn();
    std::fprintf(stderr, "runner04: unknown test '%s'\n", argv[1]);
    return 2;
}
