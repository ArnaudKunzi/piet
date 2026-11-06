// Benchmark harness for the HDFE demean: reports iteration count + wall time for
// a synthetic many-FE panel. Honors $CECON_NUM_THREADS (pool size) and
// $CECON_NO_AA (force plain alternating projections). Not a ctest.
//
//   bench_demean [N] [nFE] [ncols]
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "core/model.h"
#include "core/threads.h"
#include "hdfe/demean.h"

using namespace cecon;

static uint64_t rng = 0x243f6a8885a308d3ULL;
static double urand() {
  rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17;
  return (double)(rng >> 11) / (double)(1ULL << 53);
}

static Factor mkf(std::vector<int32_t> codes, int K) {
  Factor f;
  f.codes = std::move(codes);
  f.n_levels = K;
  f.count.assign(K, 0.0);
  for (int32_t c : f.codes) f.count[c] += 1.0;
  return f;
}

int main(int argc, char** argv) {
  int64_t N = argc > 1 ? std::atoll(argv[1]) : 1000000;
  int nFE = argc > 2 ? std::atoi(argv[2]) : 6;
  int ncols = argc > 3 ? std::atoi(argv[3]) : 3;

  // FE level counts (a mix of large and small cardinalities)
  int Ks[6] = {0};
  int base[6] = {50000, 9973, 1009, 211, 53, 17};
  for (int f = 0; f < nFE && f < 6; ++f) Ks[f] = base[f];

  // mode: "rand" (default, well-conditioned) or "hard" (AKM worker-firm mobility:
  // two correlated high-cardinality FE -> slow alternating-projections mixing).
  const char* mode = argc > 4 ? argv[4] : "rand";
  bool hard = (std::string(mode) == "hard");

  std::vector<Factor> fe;
  if (hard) {
    int spell = 8;                          // obs per worker
    int nworkers = (int)std::max<int64_t>(2, N / spell);
    int nfirms = (int)std::max<int64_t>(2, N / 40);
    std::vector<int32_t> wk(N), fm(N);
    for (int64_t i = 0; i < N; ++i) {
      int w = (int)((i / spell) % nworkers);
      wk[i] = w;
      int home = w % nfirms;
      fm[i] = (urand() < 0.85) ? home : (int)(urand() * nfirms);  // 15% mobility
    }
    fe.push_back(mkf(std::move(wk), nworkers));
    fe.push_back(mkf(std::move(fm), nfirms));
    for (int f = 2; f < nFE; ++f) {  // remaining FE: random low-card (year etc.)
      std::vector<int32_t> c(N);
      int K = Ks[f] ? Ks[f] : 101;
      for (int64_t i = 0; i < N; ++i) c[i] = (int32_t)(urand() * K);
      fe.push_back(mkf(std::move(c), K));
    }
  } else {
    for (int f = 0; f < nFE; ++f) {
      std::vector<int32_t> c(N);
      int K = Ks[f] ? Ks[f] : 101;
      for (int64_t i = 0; i < N; ++i) c[i] = (int32_t)(urand() * K);
      fe.push_back(mkf(std::move(c), K));
    }
  }

  // ncols of correlated-with-FE data
  std::vector<double> M((size_t)N * ncols);
  std::vector<std::vector<double>> effs(nFE);
  for (int f = 0; f < nFE; ++f) {
    effs[f].resize(fe[f].n_levels);
    for (auto& x : effs[f]) x = 10.0 * (urand() - 0.5);
  }
  for (int c = 0; c < ncols; ++c)
    for (int64_t i = 0; i < N; ++i) {
      double v = 2.0 * (urand() - 0.5);
      for (int f = 0; f < nFE; ++f) v += effs[f][fe[f].codes[i]];
      M[(size_t)c * N + i] = v;
    }

  int threads = ThreadPool::global().size();
  const char* noaa = std::getenv("CECON_NO_AA");
  bool aa = !(noaa && noaa[0] && noaa[0] != '0') && nFE >= 3;

  auto t0 = std::chrono::steady_clock::now();
  DemeanReport rep = demean(M.data(), N, ncols, fe);
  auto t1 = std::chrono::steady_clock::now();
  double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();

  std::printf("N=%lld nFE=%d ncols=%d threads=%d aa=%d | iters=%d resid=%.2e conv=%d | %.1f ms\n",
              (long long)N, nFE, ncols, threads, (int)aa, rep.iters, rep.resid, (int)rep.converged, ms);
  return 0;
}
