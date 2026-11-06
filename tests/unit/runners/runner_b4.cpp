// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// B4/W6 rider tests. Same one-test-per-invocation dispatcher as runner01..08
// (see CMakeLists.txt): argv[1] is the ctest name, so consolidation changes the
// number of BINARIES, not the isolation model. Kept as its OWN runner rather
// than folded into runner04 so the B4 wave's additions are purely additive lines
// in CMakeLists.txt — two lanes were editing this tree concurrently.
#include <cstdio>
#include <cstring>

int test_b4_merge_update_main();
int test_b4_egen_main();
int test_b4_concat_main();
int test_b4_savefirst_main();

int main(int argc, char** argv) {
    struct Ent { const char* name; int (*fn)(); };
    static const Ent kTests[] = {
        {"b4_merge_update", &test_b4_merge_update_main},
        {"b4_egen", &test_b4_egen_main},
        {"b4_concat", &test_b4_concat_main},
        {"b4_savefirst", &test_b4_savefirst_main},
    };
    if (argc != 2) {
        std::fprintf(stderr, "usage: runner_b4 <testname>\n");
        for (const Ent& e : kTests) std::fprintf(stderr, "  %s\n", e.name);
        return 2;
    }
    for (const Ent& e : kTests)
        if (std::strcmp(argv[1], e.name) == 0) return e.fn();
    std::fprintf(stderr, "runner_b4: unknown test '%s'\n", argv[1]);
    return 2;
}
