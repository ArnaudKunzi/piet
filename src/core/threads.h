// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// cecon — worker concurrency. Two layers:
//   * parallel_for(): coarse one-shot fan-out (spawn threads, join). For
//     embarrassingly-parallel OUTER loops with many tasks (e.g. bootstrap reps).
//   * ThreadPool: a persistent pool for fine-grained, repeated INNER passes (the
//     demean group-sum/subtract loops), so we don't pay thread-spawn cost on every
//     iteration. The calling thread participates as partition 0.
#pragma once
#include <algorithm>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

namespace cecon {

// Desired worker concurrency. Honors $CECON_NUM_THREADS (>0) first; otherwise
// the effective core count: hardware_concurrency() capped by the CPU affinity
// mask and the cgroup CPU quota on Linux (so a constrained sandbox doesn't get
// host-core-count workers), or 1 if unknown. Cached on first call.
//
// The variable is the CORE BUDGET this process was handed. PIET exports it from
// the Settings panel's "Compute cores" (piet/native/src/settings.cpp,
// applyComputeCores): the whole budget for the interactive session, a 1/N share
// for each worker of a `parallel` block (parallel.cpp spawnWorker). Every
// dispatch site in the engine — parallel_for, the ThreadPool singleton,
// threads_for_work — sizes itself from here, so there is exactly one place the
// setting has to reach.
int default_threads();

// The MEMORY BUDGET this process was handed, in bytes, or 0 when none was
// declared. Read once from $CECON_MEMORY_BUDGET_MB (>0). PIET exports it from
// the Settings panel's memory budget (the recommended half-of-RAM when the user
// left it at auto; a `parallel` worker receives its 1/N share). A bare CLI run
// has none and is unbounded, exactly as before the variable existed.
//
// It bounds FAN-OUT only — how many per-thread scratch buffers may exist at
// once — and never what a thread computes, so an output is the same bits under
// any budget (the parallel-bitneutral law that CECON_NUM_THREADS=1 arms prove).
int64_t memory_budget_bytes();

// Thread count for a fan-out whose total work is `units` elementary operations
// and whose per-thread scratch is `per_thread_bytes` (0 = negligible): 1 when
// the work is not worth splitting, else default_threads() capped so that the
// threads' scratch together fits inside a quarter of memory_budget_bytes() (the
// other three quarters are the resident data and the estimator's own state —
// a conservative split, chosen to err toward fewer threads). Always >= 1. Pass
// the result as parallel_for's `nthreads` — at 1 it runs the loop on the calling
// thread and spawns nothing, which is byte-identical to the parallel path (that
// identity is what the CECON_NUM_THREADS=1 arm of the estimator probes checks).
//
// The work threshold is the measured break-even, not a guess: spawning and
// joining a worker costs tens of microseconds, and on a 74-row fixture a
// numeric-Hessian fit is ~5 ms end to end, so the spawn was showing up as a
// 7-13% regression on small fits while paying for itself several times over on
// real ones.
inline int threads_for_work(int64_t units, int64_t per_thread_bytes = 0) {
  constexpr int64_t kParallelWorthIt = 100000;
  if (units < kParallelWorthIt) return 1;
  int t = default_threads();
  if (per_thread_bytes > 0) {
    const int64_t budget = memory_budget_bytes();
    if (budget > 0) {
      const int64_t fit = (budget / 4) / per_thread_bytes;
      if (fit < t) t = static_cast<int>(std::max<int64_t>(1, fit));
    }
  }
  return std::max(1, t);
}

// Pin the BLAS backend to a single thread on Linux (OpenBLAS) so it doesn't
// nest its own thread pool inside cecon's, unless the user set
// OPENBLAS_NUM_THREADS. No-op on Apple/Accelerate. Call once at startup.
void init_blas_threads();

// Persistent worker pool for fine-grained parallel passes. Sized to
// default_threads(); run() splits [0,N) into size() contiguous partitions and
// runs body(lo,hi,partition) on each (the caller runs partition 0), blocking
// until all finish. Nested use (a worker calling run) runs serially on the caller
// to avoid deadlock.
class ThreadPool {
 public:
  static ThreadPool& global();
  int size() const { return n_; }
  void run(int64_t N, const std::function<void(int64_t, int64_t, int)>& body);

  ThreadPool(const ThreadPool&) = delete;
  ThreadPool& operator=(const ThreadPool&) = delete;

 private:
  explicit ThreadPool(int n);
  ~ThreadPool();
  void worker_loop(int idx);
  void run_partition(int idx);

  int n_;
  std::vector<std::thread> workers_;
  const std::function<void(int64_t, int64_t, int)>* body_ = nullptr;
  int64_t N_ = 0;
  std::mutex m_;
  std::condition_variable cv_work_;
  std::condition_variable cv_done_;
  uint64_t gen_ = 0;
  int remaining_ = 0;
  bool stop_ = false;
};

// Coarse one-shot parallel: spawns `nthreads` threads for a single call and joins.
// Independent of ThreadPool to avoid nested-pool contention from outer loops.
template <class F>
void parallel_for(int64_t n, F&& fn, int nthreads = 0) {
  if (n <= 0) return;
  if (nthreads <= 0) nthreads = default_threads();
  nthreads = std::max<int>(1, std::min<int64_t>(nthreads, n));
  if (nthreads == 1) {
    for (int64_t i = 0; i < n; ++i) fn(i);
    return;
  }
  std::vector<std::thread> pool;
  pool.reserve(nthreads);
  const int64_t chunk = (n + nthreads - 1) / nthreads;
  for (int t = 0; t < nthreads; ++t) {
    int64_t lo = static_cast<int64_t>(t) * chunk;
    int64_t hi = std::min(n, lo + chunk);
    if (lo >= hi) break;
    pool.emplace_back([lo, hi, &fn] {
      for (int64_t i = lo; i < hi; ++i) fn(i);
    });
  }
  for (auto& th : pool) th.join();
}

}  // namespace cecon
