// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// Settings-derived compute envelope (2026-09-02) — the engine's thread resolver
// must follow the two budgets PIET exports from its Settings panel:
//
//   $CECON_NUM_THREADS      the core budget   -> default_threads()
//   $CECON_MEMORY_BUDGET_MB the memory budget -> memory_budget_bytes()
//
// and threads_for_work(units, per_thread_bytes) must (a) run serial below the
// work break-even, (b) fan out to default_threads() above it, (c) cap the fan-out
// so per-thread scratch fits inside a quarter of the memory budget, and (d) treat
// an undeclared or unparseable budget as unbounded (the pre-existing behaviour).
//
// Both resolvers are first-call-cached process singletons, so every environment
// below is its own child process (fork on POSIX, self-spawn on Windows) — the
// same shape as test_thread_determinism. The estimator-facing resolvers
// (xtlogit_fanout_threads, wildboot_fanout_threads) are probed the same way, at
// exactly the sizes test_thread_determinism uses, so that test's multi-thread arm
// is proven non-vacuous here as well as there.
//
// RED certificate (2026-09-02): with the memory cap in threads_for_work removed,
// cases C and D went RED (3 != 2, 3 != 1) while every other case stayed GREEN;
// with default_threads()'s env read removed, case B went RED. Seeds reverted.
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "core/threads.h"
#include "estimators/xtlogit.h"
#include "inference/wildboot.h"

#if defined(_WIN32)
#include <process.h>
#include <windows.h>
#else
#include <sys/wait.h>
#include <unistd.h>
#endif

using namespace cecon;

static void put(const char* k, const char* v) {
#if defined(_WIN32)
    ::_putenv_s(k, v ? v : "");
#else
    if (v) setenv(k, v, 1); else unsetenv(k);
#endif
}

struct Case {
    const char* tag;
    const char* threads;  // nullptr = unset
    const char* budget;   // nullptr = unset
    int (*body)();
};

static int expect(int got, int want, const char* what) {
    if (got != want) { std::fprintf(stderr, "    %s: got %d, want %d\n", what, got, want); return 1; }
    return 0;
}

static const int64_t MiB = 1024 * 1024;

// A: nothing declared — hardware width, no memory cap, small work serial.
static int caseA() {
    int bad = 0;
    bad += expect(default_threads() >= 1, 1, "default_threads() >= 1");
    bad += expect(memory_budget_bytes() == 0, 1, "undeclared budget reads 0");
    bad += expect(threads_for_work(1000000, 0), default_threads(), "big work, no scratch -> width");
    bad += expect(threads_for_work(1000000, 64 * MiB), default_threads(), "big scratch, no budget -> width");
    bad += expect(threads_for_work(10, MiB), 1, "small work -> serial");
    return bad;
}
// B: core budget 3, no memory budget.
static int caseB() {
    int bad = 0;
    bad += expect(default_threads(), 3, "CECON_NUM_THREADS=3 honoured");
    bad += expect(threads_for_work(1000000, 0), 3, "big work -> 3");
    bad += expect(threads_for_work(1000000, 1 << 30), 3, "no budget -> uncapped even at 1 GiB/thread");
    return bad;
}
// C: core budget 3, memory budget 8 MB, 1 MiB/thread -> quarter = 2 MiB -> 2 threads.
static int caseC() {
    int bad = 0;
    bad += expect(memory_budget_bytes() == 8 * MiB, 1, "CECON_MEMORY_BUDGET_MB=8 reads 8 MiB");
    bad += expect(threads_for_work(1000000, MiB), 2, "8 MB budget / 4 / 1 MiB -> 2");
    bad += expect(threads_for_work(1000000, 0), 3, "no scratch declared -> not capped");
    return bad;
}
// D: memory budget 1 MB, 1 MiB/thread -> floor of 1.
static int caseD() {
    return expect(threads_for_work(1000000, MiB), 1, "1 MB budget -> floor 1");
}
// E: unparseable budget -> unbounded.
static int caseE() {
    int bad = 0;
    bad += expect(memory_budget_bytes() == 0, 1, "garbage budget reads 0");
    bad += expect(threads_for_work(1000000, 1 << 30), 3, "garbage budget -> uncapped");
    return bad;
}
// F: the estimator resolvers at test_thread_determinism's case sizes.
static int caseF() {
    int bad = 0;
    bad += expect(xtlogit_fanout_threads(400, 6, 2) >= 2, 1, "xtlogit determinism case fans out at 4");
    bad += expect(wildboot_fanout_threads(999, 24) >= 2, 1, "wildboot determinism case fans out at 4");
    bad += expect(xtlogit_fanout_threads(20, 5, 2), 1, "a 20-panel fit stays serial");
    bad += expect(wildboot_fanout_threads(99, 8), 1, "99 reps over 8 clusters stays serial");
    return bad;
}
// G: the estimator resolvers under a tight memory budget — xtlogit's per-thread
// tables at Tmax=2000 are ~64 MB each thread, so a 64 MB budget (quarter = 16 MB)
// admits one thread; the same fit under no budget takes the full width.
static int caseG() {
    return expect(xtlogit_fanout_threads(5000, 2000, 2), 1, "64 MB budget caps long-panel xtlogit to 1");
}

static const std::vector<Case>& all_cases() {
    static const std::vector<Case> c = {
        {"A", nullptr, nullptr, caseA},
        {"B", "3", nullptr, caseB},
        {"C", "3", "8", caseC},
        {"D", "3", "1", caseD},
        {"E", "3", "not-a-number", caseE},
        {"F", "4", nullptr, caseF},
        {"G", "4", "64", caseG},
    };
    return c;
}

static int run_child(const std::string& tag) {
    for (const Case& c : all_cases()) {
        if (tag != c.tag) continue;
        put("CECON_NUM_THREADS", c.threads);
        put("CECON_MEMORY_BUDGET_MB", c.budget);
        return c.body();
    }
    return 3;
}

#if defined(_WIN32)
int main(int argc, char** argv) {
    if (argc == 3 && std::strcmp(argv[1], "--child") == 0) return run_child(argv[2]);
    std::printf("test_threads_budget:\n");
    int fail = 0;
    for (const Case& c : all_cases()) {
        const intptr_t rc = _spawnl(_P_WAIT, argv[0], argv[0], "--child", c.tag, nullptr);
        const bool ok = rc == 0;
        std::printf("  %s: case %s\n", ok ? "ok" : "FAIL", c.tag);
        if (!ok) fail = 1;
    }
    return fail;
}
#else
int main() {
    std::printf("test_threads_budget:\n");
    int fail = 0;
    for (const Case& c : all_cases()) {
        pid_t pid = fork();
        if (pid == 0) _exit(run_child(c.tag));
        int status = 0;
        waitpid(pid, &status, 0);
        const bool ok = WIFEXITED(status) && WEXITSTATUS(status) == 0;
        std::printf("  %s: case %s\n", ok ? "ok" : "FAIL", c.tag);
        if (!ok) fail = 1;
    }
    return fail;
}
#endif
