// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
#include "core/threads.h"

#include <algorithm>
#include <cstdlib>

#ifdef __linux__
#include <sched.h>
#include <cstdio>
#endif

namespace cecon {

#ifdef __linux__
// CPUs this process is actually allowed to run on (respects taskset /
// docker --cpuset-cpus / k8s cpuset). 0 = unknown.
static int affinity_cpus() {
  cpu_set_t set;
  CPU_ZERO(&set);
  if (sched_getaffinity(0, sizeof(set), &set) == 0) {
    int c = CPU_COUNT(&set);
    if (c > 0) return c;
  }
  return 0;
}

// Effective whole-core CPU quota from the cgroup (respects docker --cpus and
// k8s CPU limits). 0 = unlimited / unreadable. Rounds a fractional quota up so
// we never fall below 1 (e.g. --cpus=1.5 -> 2).
static int cgroup_cpu_quota() {
  // cgroup v2: "/sys/fs/cgroup/cpu.max" = "<quota> <period>" or "max <period>".
  if (FILE* f = std::fopen("/sys/fs/cgroup/cpu.max", "r")) {
    char q[64] = {0};
    long period = 0;
    int n = std::fscanf(f, "%63s %ld", q, &period);
    std::fclose(f);
    if (n >= 1 && q[0] != 'm') {  // not "max"
      long quota = std::atol(q);
      if (quota > 0 && period > 0)
        return static_cast<int>((quota + period - 1) / period);
    }
    return 0;  // "max" => unlimited
  }
  // cgroup v1.
  long quota = -1, period = 0;
  if (FILE* f = std::fopen("/sys/fs/cgroup/cpu/cpu.cfs_quota_us", "r")) {
    if (std::fscanf(f, "%ld", &quota) != 1) quota = -1;
    std::fclose(f);
  }
  if (FILE* f = std::fopen("/sys/fs/cgroup/cpu/cpu.cfs_period_us", "r")) {
    if (std::fscanf(f, "%ld", &period) != 1) period = 0;
    std::fclose(f);
  }
  if (quota > 0 && period > 0)
    return static_cast<int>((quota + period - 1) / period);
  return 0;
}

#endif

#if defined(__linux__) || defined(_WIN32)
// OpenBLAS thread control, resolved from the (statically) linked libopenblas.
extern "C" void openblas_set_num_threads(int);
#endif

int default_threads() {
  static int cached = [] {
    if (const char* e = std::getenv("CECON_NUM_THREADS")) {
      int v = std::atoi(e);
      if (v > 0) return v;
    }
    unsigned hc = std::thread::hardware_concurrency();
    int n = hc == 0 ? 1 : static_cast<int>(hc);
#ifdef __linux__
    // In a constrained sandbox (Cowork, k8s, docker --cpus) hardware_concurrency
    // reports the HOST core count, not what we may actually use — spawning that
    // many workers oversubscribes the quota. Cap to the cgroup quota and CPU
    // affinity so we right-size to the box we're really on.
    if (int aff = affinity_cpus()) n = std::min(n, aff);
    if (int q = cgroup_cpu_quota()) n = std::min(n, q);
#endif
    return std::max(1, n);
  }();
  return cached;
}

int64_t memory_budget_bytes() {
  static const int64_t cached = [] {
    if (const char* e = std::getenv("CECON_MEMORY_BUDGET_MB")) {
      char* end = nullptr;
      const long long mb = std::strtoll(e, &end, 10);
      if (end != e && mb > 0) return static_cast<int64_t>(mb) * 1024 * 1024;
    }
    return static_cast<int64_t>(0);  // undeclared = unbounded (pre-2026-09-02 behaviour)
  }();
  return cached;
}

void init_blas_threads() {
#ifdef __linux__
  // cecon owns parallelism through its own ThreadPool (the HDFE demean sweep).
  // A nested multi-threaded OpenBLAS inside that pool just oversubscribes — 2-2.6x
  // slower on a capped sandbox, and it can deadlock under a low pid limit. Pin
  // BLAS to a single thread unless the user explicitly asked otherwise.
  //
  // The ENV var is the reliable lever: OpenBLAS initializes its thread pool
  // lazily on the first BLAS call and re-reads OPENBLAS_NUM_THREADS there, which
  // overrides an earlier openblas_set_num_threads() call. We set it before any
  // BLAS runs (this is invoked at the very top of main()); overwrite=0 respects a
  // user-provided value. OMP_NUM_THREADS covers an OpenMP-threaded build. The API
  // call is a harmless backstop. (On Apple we use Accelerate — no-op.)
  const bool user_set = std::getenv("OPENBLAS_NUM_THREADS") != nullptr;
  setenv("OPENBLAS_NUM_THREADS", "1", 0);
  setenv("OMP_NUM_THREADS", "1", 0);
  if (!user_set) openblas_set_num_threads(1);
#elif defined(_WIN32)
  // The Windows distribution also links OpenBLAS (docker/Dockerfile.windows,
  // NUM_THREADS=64). Same reasoning as the Linux arm: keep cecon's ThreadPool as
  // the sole parallel layer, while preserving an explicit user override. There is
  // no setenv() in the MSVCRT/MinGW CRT — _putenv_s is the equivalent, so the
  // "don't overwrite" semantics of setenv's overwrite=0 are spelled out here.
  const bool user_set = std::getenv("OPENBLAS_NUM_THREADS") != nullptr;
  if (!user_set) ::_putenv_s("OPENBLAS_NUM_THREADS", "1");
  if (!std::getenv("OMP_NUM_THREADS")) ::_putenv_s("OMP_NUM_THREADS", "1");
  if (!user_set) openblas_set_num_threads(1);
#endif
}

// True while executing inside a pool worker (or the caller during run()), so a
// nested run() falls back to serial instead of deadlocking on the pool.
static thread_local bool tl_in_pool = false;

ThreadPool& ThreadPool::global() {
  static ThreadPool pool(default_threads());
  return pool;
}

ThreadPool::ThreadPool(int n) : n_(std::max(1, n)) {
  for (int i = 1; i < n_; ++i) workers_.emplace_back([this, i] { worker_loop(i); });
}

ThreadPool::~ThreadPool() {
  {
    std::unique_lock<std::mutex> lk(m_);
    stop_ = true;
    ++gen_;
    cv_work_.notify_all();
  }
  for (auto& w : workers_)
    if (w.joinable()) w.join();
}

void ThreadPool::run_partition(int idx) {
  const int64_t chunk = (N_ + n_ - 1) / n_;
  int64_t lo = static_cast<int64_t>(idx) * chunk;
  int64_t hi = std::min(N_, lo + chunk);
  if (lo < hi) (*body_)(lo, hi, idx);
}

void ThreadPool::worker_loop(int idx) {
  tl_in_pool = true;
  uint64_t mygen = 0;
  for (;;) {
    std::unique_lock<std::mutex> lk(m_);
    cv_work_.wait(lk, [&] { return stop_ || gen_ != mygen; });
    if (stop_) return;
    mygen = gen_;
    lk.unlock();
    run_partition(idx);
    lk.lock();
    if (--remaining_ == 0) cv_done_.notify_one();
  }
}

// Direct-call bypass for the degenerate single-unit dispatch. N here is the number of
// parallel UNITS, not n·k: for the HDFE demean the reduction passes get N = the fixed
// lane count (~n/16384), so N stays small (e.g. 38 at 620k rows). Measurement shows the
// per-lane group-sum is ~2.4x faster threaded even at N=38, and that win scales with any
// multi-lane reduction — so a large "rows·cols" floor here would serialize the parallel
// reduction and REGRESS mid-size problems. Only N <= 1 is universally safe to bypass:
// one partition does all the work while the other workers idle, making the pool pure
// dispatch overhead. Bit-identity holds for any floor because the reductions sum over a
// fixed lane count (reduction_lanes()), independent of the worker partition count.
static constexpr int64_t kRunDirectFloor = 2;

void ThreadPool::run(int64_t N, const std::function<void(int64_t, int64_t, int)>& body) {
  if (N <= 0) return;
  if (N < kRunDirectFloor || n_ <= 1 || tl_in_pool) {  // single-unit / serial / nested
    body(0, N, 0);
    return;
  }
  std::unique_lock<std::mutex> lk(m_);
  body_ = &body;
  N_ = N;
  remaining_ = n_ - 1;  // workers 1..n_-1; partition 0 runs on the caller
  ++gen_;
  cv_work_.notify_all();
  lk.unlock();

  tl_in_pool = true;
  run_partition(0);
  tl_in_pool = false;

  lk.lock();
  cv_done_.wait(lk, [&] { return remaining_ == 0; });
  body_ = nullptr;
}

}  // namespace cecon
