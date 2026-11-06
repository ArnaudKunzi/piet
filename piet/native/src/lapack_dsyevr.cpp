// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// Portions derived from LAPACK 3.12.0 (BSD-3-Clause). See THIRD_PARTY_NOTICES.
//
// lapack_dsyevr.cpp -- Netlib LAPACK 3.12.0 DSYEVR(JOBZ='V', RANGE='A',
// UPLO='L') and the part of its call tree that path reaches, with the
// reference BLAS routines it calls, translated to C++ routine by routine.
//
// Why bit-exact: Mata's symeigensystem() in StataBE 19.5 is DSYEVR from the
// bundled liblapack.3.12.0.dylib (gfortran-built for arm64, linked against the
// bundled reference libblas.3.12.0.dylib), and ml's `difficult` step is
// sensitive to the last bit of its eigenvectors.
//
// Translation rules, so the file can be audited against the Fortran:
//  - each function is named after its routine and keeps its argument order;
//    arrays are passed as pointers to element (1) and re-based with `--p`
//    (f2c style), so every index below is the Fortran index;
//  - only the branches the (V,A,L) path can execute are carried; a branch
//    that is left out is named in a comment where it would sit;
//  - this TU is compiled with -ffp-contract=off and every fused multiply-add
//    of the StataBE build is written as std::fma in the same place. The sites
//    were read from `otool -tV` of the two dylibs: gfortran fuses a product
//    into the add/subtract that consumes it whenever that is the product's
//    only use; with two products in one sum the first one is fused. The
//    per-routine FMA instruction count of the dylib is noted at each routine;
//  - MAX/MIN are fmaxnm/fminnm there (std::fmax/std::fmin), SIGN is copysign,
//    X**2 is X*X, LOG/SQRT/ABS are the platform's.
//
// Not carried: RANGE='V'/'I' and JOBZ='N' branches, UPLO='U'. Carried
// besides the DSTEMR path: the blocked DSYTRD (DLATRD + DSYR2K, order > 32
// with DSYEVR's own optimal workspace), the blocked DORMQR (DLARFT + DLARFB,
// order >= 144) and DSYEVR's DSTEBZ/DLAEBZ + DSTEIN/DLAGTF/DLAGTS fallback
// when DSTEMR fails.
#include "lapack_dsyevr.h"

#include <cfloat>
#include <cmath>
#include <cstring>

namespace piet::lapack {
namespace {

using std::copysign;
using std::fabs;
using std::fma;
using std::fmax;
using std::fmin;
using std::sqrt;

// LOG(TWO) as gfortran folds it at compile time (correctly rounded ln 2).
constexpr double kLn2 = 0x1.62e42fefa39efp-1;

// ---------------------------------------------------------------- machine
// INSTALL/dlamch.f (RND = ONE, so EPS = EPSILON(ZERO)*0.5).
double dlamch(char cmach) {
  const double eps = DBL_EPSILON * 0.5;
  switch (cmach) {
    case 'E': return eps;
    case 'S': {
      double sfmin = DBL_MIN;
      const double small = 1.0 / DBL_MAX;
      if (small >= sfmin) sfmin = small * (1.0 + eps);
      return sfmin;
    }
    case 'B': return 2.0;
    case 'P': return eps * 2.0;
    case 'N': return 53.0;
    case 'R': return 1.0;
    case 'M': return -1021.0;
    case 'U': return DBL_MIN;
    case 'L': return 1024.0;
    case 'O': return DBL_MAX;
  }
  return 0.0;
}

bool disnan(double din) { return din != din; }  // DISNAN -> DLAISNAN(DIN,DIN)

// ieeeck.f (REAL arithmetic).
int ieeeck(int ispec, float zero, float one) {
  float posinf = one / zero;
  if (posinf <= one) return 0;
  float neginf = -one / zero;
  if (neginf >= zero) return 0;
  const float negzro = one / (neginf + one);
  if (negzro != zero) return 0;
  neginf = one / negzro;
  if (neginf >= zero) return 0;
  const float newzro = negzro + zero;
  if (newzro != zero) return 0;
  posinf = one / newzro;
  if (posinf <= one) return 0;
  neginf = neginf * posinf;
  if (neginf >= zero) return 0;
  posinf = posinf * posinf;
  if (posinf <= one) return 0;
  if (ispec == 0) return 1;
  const float nan1 = posinf + neginf, nan2 = posinf / neginf, nan3 = posinf / posinf;
  const float nan4 = posinf * zero, nan5 = neginf * negzro, nan6 = nan5 * zero;
  if (nan1 == nan1 || nan2 == nan2 || nan3 == nan3 || nan4 == nan4 || nan5 == nan5 ||
      nan6 == nan6)
    return 0;
  return 1;
}

// ilaenv.f, the ISPEC values and routine families this file asks about
// (xSYTRD, xORMTR/xORMQR; ISPEC=10 for xSYEVR). Same answers as the full
// function for those names; other names get ilaenv's defaults (1, 2, 0).
int ilaenv(int ispec, const char* name) {
  if (ispec == 10) return ieeeck(1, 0.0f, 1.0f);
  if (ispec < 1 || ispec > 3) return -1;
  const char c1 = name[0];
  const bool sname = c1 == 'S' || c1 == 'D';
  if (!(sname || c1 == 'C' || c1 == 'Z')) return 1;
  const auto eq = [&](int at, const char* s) { return std::strncmp(name + at, s, std::strlen(s)) == 0; };
  const bool sy = eq(1, "SY"), orr = eq(1, "OR");
  const bool c4ok = eq(4, "QR") || eq(4, "RQ") || eq(4, "LQ") || eq(4, "QL") || eq(4, "HR") ||
                    eq(4, "TR") || eq(4, "BR");
  if (ispec == 1) {
    if (sy && sname && eq(3, "TRD")) return 32;
    if (orr && sname && (name[3] == 'G' || name[3] == 'M') && c4ok) return 32;
    return 1;
  }
  if (ispec == 2) return 2;  // NBMIN is 2 for every SY TRD / OR G,M family member
  if (sy && sname && eq(3, "TRD")) return 32;
  if (orr && sname && name[3] == 'G' && c4ok) return 128;
  return 0;
}

// ------------------------------------------------------------ BLAS level 1
// ddot.f (7 fmadd: every DTEMP + DX*DY).
double ddot(int n, const double* dx, int incx, const double* dy, int incy) {
  --dx; --dy;
  double dtemp = 0.0;
  if (n <= 0) return 0.0;
  if (incx == 1 && incy == 1) {
    const int m = n % 5;
    if (m != 0) {
      for (int i = 1; i <= m; ++i) dtemp = fma(dx[i], dy[i], dtemp);
      if (n < 5) return dtemp;
    }
    for (int i = m + 1; i <= n; i += 5) {
      dtemp = fma(dx[i], dy[i], dtemp);
      dtemp = fma(dx[i + 1], dy[i + 1], dtemp);
      dtemp = fma(dx[i + 2], dy[i + 2], dtemp);
      dtemp = fma(dx[i + 3], dy[i + 3], dtemp);
      dtemp = fma(dx[i + 4], dy[i + 4], dtemp);
    }
  } else {
    int ix = 1, iy = 1;
    if (incx < 0) ix = (-n + 1) * incx + 1;
    if (incy < 0) iy = (-n + 1) * incy + 1;
    for (int i = 1; i <= n; ++i) {
      dtemp = fma(dx[ix], dy[iy], dtemp);
      ix += incx;
      iy += incy;
    }
  }
  return dtemp;
}

// daxpy.f (6 fmadd).
void daxpy(int n, double da, const double* dx, int incx, double* dy, int incy) {
  --dx; --dy;
  if (n <= 0) return;
  if (da == 0.0) return;
  if (incx == 1 && incy == 1) {
    const int m = n % 4;
    if (m != 0)
      for (int i = 1; i <= m; ++i) dy[i] = fma(da, dx[i], dy[i]);
    if (n < 4) return;
    for (int i = m + 1; i <= n; i += 4) {
      dy[i] = fma(da, dx[i], dy[i]);
      dy[i + 1] = fma(da, dx[i + 1], dy[i + 1]);
      dy[i + 2] = fma(da, dx[i + 2], dy[i + 2]);
      dy[i + 3] = fma(da, dx[i + 3], dy[i + 3]);
    }
  } else {
    int ix = 1, iy = 1;
    if (incx < 0) ix = (-n + 1) * incx + 1;
    if (incy < 0) iy = (-n + 1) * incy + 1;
    for (int i = 1; i <= n; ++i) {
      dy[iy] = fma(da, dx[ix], dy[iy]);
      ix += incx;
      iy += incy;
    }
  }
}

// dscal.f (no FMA).
void dscal(int n, double da, double* dx, int incx) {
  --dx;
  if (n <= 0 || incx <= 0 || da == 1.0) return;
  if (incx == 1) {
    const int m = n % 5;
    if (m != 0) {
      for (int i = 1; i <= m; ++i) dx[i] = da * dx[i];
      if (n < 5) return;
    }
    for (int i = m + 1; i <= n; i += 5) {
      dx[i] = da * dx[i];
      dx[i + 1] = da * dx[i + 1];
      dx[i + 2] = da * dx[i + 2];
      dx[i + 3] = da * dx[i + 3];
      dx[i + 4] = da * dx[i + 4];
    }
  } else {
    const int nincx = n * incx;
    for (int i = 1; i <= nincx; i += incx) dx[i] = da * dx[i];
  }
}

// dcopy.f
void dcopy(int n, const double* dx, int incx, double* dy, int incy) {
  --dx; --dy;
  if (n <= 0) return;
  int ix = 1, iy = 1;
  if (incx < 0) ix = (-n + 1) * incx + 1;
  if (incy < 0) iy = (-n + 1) * incy + 1;
  for (int i = 1; i <= n; ++i) {
    dy[iy] = dx[ix];
    ix += incx;
    iy += incy;
  }
}

// dswap.f
void dswap(int n, double* dx, int incx, double* dy, int incy) {
  --dx; --dy;
  if (n <= 0) return;
  int ix = 1, iy = 1;
  if (incx < 0) ix = (-n + 1) * incx + 1;
  if (incy < 0) iy = (-n + 1) * incy + 1;
  for (int i = 1; i <= n; ++i) {
    const double dtemp = dx[ix];
    dx[ix] = dy[iy];
    dy[iy] = dtemp;
    ix += incx;
    iy += incy;
  }
}

// dnrm2.f90 (Blue's algorithm; 5 fmadd: the three accumulations,
// abig + (amed*sbig)*sbig and one + (ymin/ymax)**2).
double dnrm2(int n, const double* x, int incx) {
  --x;
  const double tsml = 0x1p-511, tbig = 0x1p486, ssml = 0x1p537, sbig = 0x1p-538;
  const double maxN = DBL_MAX;
  if (n <= 0) return 0.0;
  double scl = 1.0, sumsq = 0.0;
  bool notbig = true;
  double asml = 0.0, amed = 0.0, abig = 0.0;
  int ix = 1;
  if (incx < 0) ix = 1 - (n - 1) * incx;
  for (int i = 1; i <= n; ++i) {
    const double ax = fabs(x[ix]);
    if (ax > tbig) {
      const double t = ax * sbig;
      abig = fma(t, t, abig);
      notbig = false;
    } else if (ax < tsml) {
      if (notbig) {
        const double t = ax * ssml;
        asml = fma(t, t, asml);
      }
    } else {
      amed = fma(ax, ax, amed);
    }
    ix += incx;
  }
  if (abig > 0.0) {
    if (amed > 0.0 || amed > maxN || amed != amed) abig = fma(amed * sbig, sbig, abig);
    scl = 1.0 / sbig;
    sumsq = abig;
  } else if (asml > 0.0) {
    if (amed > 0.0 || amed > maxN || amed != amed) {
      amed = sqrt(amed);
      asml = sqrt(asml) / ssml;
      double ymin, ymax;
      if (asml > amed) {
        ymin = amed;
        ymax = asml;
      } else {
        ymin = asml;
        ymax = amed;
      }
      scl = 1.0;
      const double r = ymin / ymax;
      sumsq = (ymax * ymax) * fma(r, r, 1.0);
    } else {
      scl = 1.0 / ssml;
      sumsq = asml;
    }
  } else {
    scl = 1.0;
    sumsq = amed;
  }
  return scl * sqrt(sumsq);
}

// ------------------------------------------------------------ BLAS level 2
// Only the UPLO/TRANS values and unit strides the (V,A,L) path passes.
#define A_(i, j) a[(i) + (j) * lda]

// dsymv.f, UPLO='L', INCX=INCY=1 (4 fmadd in this branch).
void dsymvL(int n, double alpha, double* a, int lda, const double* x, double beta, double* y) {
  a -= 1 + lda; --x; --y;
  if (n == 0 || (alpha == 0.0 && beta == 1.0)) return;
  if (beta != 1.0) {
    if (beta == 0.0)
      for (int i = 1; i <= n; ++i) y[i] = 0.0;
    else
      for (int i = 1; i <= n; ++i) y[i] = beta * y[i];
  }
  if (alpha == 0.0) return;
  for (int j = 1; j <= n; ++j) {
    const double temp1 = alpha * x[j];
    double temp2 = 0.0;
    y[j] = fma(temp1, A_(j, j), y[j]);
    for (int i = j + 1; i <= n; ++i) {
      y[i] = fma(temp1, A_(i, j), y[i]);
      temp2 = fma(A_(i, j), x[i], temp2);
    }
    y[j] = fma(alpha, temp2, y[j]);
  }
}

// dsyr2.f, UPLO='L', INCX=INCY=1 (A + X*TEMP1 + Y*TEMP2: two fmadd).
void dsyr2L(int n, double alpha, const double* x, const double* y, double* a, int lda) {
  a -= 1 + lda; --x; --y;
  if (n == 0 || alpha == 0.0) return;
  for (int j = 1; j <= n; ++j) {
    if (x[j] != 0.0 || y[j] != 0.0) {
      const double temp1 = alpha * y[j];
      const double temp2 = alpha * x[j];
      for (int i = j; i <= n; ++i) A_(i, j) = fma(y[i], temp2, fma(x[i], temp1, A_(i, j)));
    }
  }
}

// dgemv.f, TRANS='T', INCX=INCY=1 (TEMP + A*X and Y + ALPHA*TEMP fused).
void dgemvT(int m, int n, double alpha, const double* a, int lda, const double* x, double beta,
            double* y) {
  a -= 1 + lda; --x; --y;
  if (m == 0 || n == 0 || (alpha == 0.0 && beta == 1.0)) return;
  const int leny = n;
  if (beta != 1.0) {
    if (beta == 0.0)
      for (int i = 1; i <= leny; ++i) y[i] = 0.0;
    else
      for (int i = 1; i <= leny; ++i) y[i] = beta * y[i];
  }
  if (alpha == 0.0) return;
  int jy = 1;
  for (int j = 1; j <= n; ++j) {
    double temp = 0.0;
    for (int i = 1; i <= m; ++i) temp = fma(A_(i, j), x[i], temp);
    y[jy] = fma(alpha, temp, y[jy]);
    jy += 1;
  }
}

// dgemv.f, TRANS='N', INCY=1, any INCX>0 (Y + TEMP*A fused).
void dgemvN(int m, int n, double alpha, const double* a, int lda, const double* x, int incx,
            double beta, double* y) {
  a -= 1 + lda; --x; --y;
  if (m == 0 || n == 0 || (alpha == 0.0 && beta == 1.0)) return;
  const int leny = m;
  const int kx = 1;
  if (beta != 1.0) {
    if (beta == 0.0)
      for (int i = 1; i <= leny; ++i) y[i] = 0.0;
    else
      for (int i = 1; i <= leny; ++i) y[i] = beta * y[i];
  }
  if (alpha == 0.0) return;
  int jx = kx;
  for (int j = 1; j <= n; ++j) {
    const double temp = alpha * x[jx];
    for (int i = 1; i <= m; ++i) y[i] = fma(temp, A_(i, j), y[i]);
    jx += incx;
  }
}

// dsyr2k.f, UPLO='L', TRANS='N' (C + A*TEMP1 + B*TEMP2: two fmadd).
void dsyr2kLN(int n, int k, double alpha, const double* a, int lda, const double* b, int ldb,
              double beta, double* c, int ldc) {
  a -= 1 + lda; b -= 1 + ldb; c -= 1 + ldc;
#define B_(i, j) b[(i) + (j) * ldb]
#define C_(i, j) c[(i) + (j) * ldc]
  if (n == 0 || ((alpha == 0.0 || k == 0) && beta == 1.0)) return;
  if (alpha == 0.0) {
    for (int j = 1; j <= n; ++j)
      for (int i = j; i <= n; ++i) C_(i, j) = beta == 0.0 ? 0.0 : beta * C_(i, j);
    return;
  }
  for (int j = 1; j <= n; ++j) {
    if (beta == 0.0) {
      for (int i = j; i <= n; ++i) C_(i, j) = 0.0;
    } else if (beta != 1.0) {
      for (int i = j; i <= n; ++i) C_(i, j) = beta * C_(i, j);
    }
    for (int l = 1; l <= k; ++l) {
      if (A_(j, l) != 0.0 || B_(j, l) != 0.0) {
        const double temp1 = alpha * B_(j, l);
        const double temp2 = alpha * A_(j, l);
        for (int i = j; i <= n; ++i) C_(i, j) = fma(B_(i, l), temp2, fma(A_(i, l), temp1, C_(i, j)));
      }
    }
  }
#undef B_
#undef C_
}

// dtrmv.f, UPLO='U', TRANS='N', DIAG='N', INCX=1 (X + TEMP*A fused).
void dtrmvUNN(int n, const double* a, int lda, double* x) {
  a -= 1 + lda; --x;
  if (n == 0) return;
  for (int j = 1; j <= n; ++j) {
    if (x[j] != 0.0) {
      const double temp = x[j];
      for (int i = 1; i <= j - 1; ++i) x[i] = fma(temp, A_(i, j), x[i]);
      x[j] = x[j] * A_(j, j);
    }
  }
}

// dtrmm.f, SIDE='R' (B := alpha*B*op(A)); the three UPLO/TRANSA/DIAG
// combinations DLARFB uses. B + TEMP*B(.,K) is fused.
void dtrmmR(char uplo, char transa, char diag, int m, int n, double alpha, const double* a, int lda,
            double* b, int ldb) {
  a -= 1 + lda; b -= 1 + ldb;
#define B_(i, j) b[(i) + (j) * ldb]
  const bool nounit = diag == 'N', upper = uplo == 'U';
  if (m == 0 || n == 0) return;
  if (alpha == 0.0) {
    for (int j = 1; j <= n; ++j)
      for (int i = 1; i <= m; ++i) B_(i, j) = 0.0;
    return;
  }
  double temp;
  if (transa == 'N') {
    if (upper) {
      for (int j = n; j >= 1; --j) {
        temp = alpha;
        if (nounit) temp = temp * A_(j, j);
        for (int i = 1; i <= m; ++i) B_(i, j) = temp * B_(i, j);
        for (int k = 1; k <= j - 1; ++k) {
          if (A_(k, j) != 0.0) {
            temp = alpha * A_(k, j);
            for (int i = 1; i <= m; ++i) B_(i, j) = fma(temp, B_(i, k), B_(i, j));
          }
        }
      }
    } else {
      for (int j = 1; j <= n; ++j) {
        temp = alpha;
        if (nounit) temp = temp * A_(j, j);
        for (int i = 1; i <= m; ++i) B_(i, j) = temp * B_(i, j);
        for (int k = j + 1; k <= n; ++k) {
          if (A_(k, j) != 0.0) {
            temp = alpha * A_(k, j);
            for (int i = 1; i <= m; ++i) B_(i, j) = fma(temp, B_(i, k), B_(i, j));
          }
        }
      }
    }
  } else {
    if (upper) {
      for (int k = 1; k <= n; ++k) {
        for (int j = 1; j <= k - 1; ++j) {
          if (A_(j, k) != 0.0) {
            temp = alpha * A_(j, k);
            for (int i = 1; i <= m; ++i) B_(i, j) = fma(temp, B_(i, k), B_(i, j));
          }
        }
        temp = alpha;
        if (nounit) temp = temp * A_(k, k);
        if (temp != 1.0)
          for (int i = 1; i <= m; ++i) B_(i, k) = temp * B_(i, k);
      }
    } else {
      for (int k = n; k >= 1; --k) {
        for (int j = k + 1; j <= n; ++j) {
          if (A_(j, k) != 0.0) {
            temp = alpha * A_(j, k);
            for (int i = 1; i <= m; ++i) B_(i, j) = fma(temp, B_(i, k), B_(i, j));
          }
        }
        temp = alpha;
        if (nounit) temp = temp * A_(k, k);
        if (temp != 1.0)
          for (int i = 1; i <= m; ++i) B_(i, k) = temp * B_(i, k);
      }
    }
  }
#undef B_
}

// dgemm.f, (TRANSA,TRANSB) = ('T','N') and ('N','T'): TEMP + A*B and
// C + TEMP*A fused; ALPHA*TEMP + BETA*C fuses its first product.
void dgemm(char transa, char transb, int m, int n, int k, double alpha, const double* a, int lda,
           const double* b, int ldb, double beta, double* c, int ldc) {
  a -= 1 + lda; b -= 1 + ldb; c -= 1 + ldc;
#define B_(i, j) b[(i) + (j) * ldb]
#define C_(i, j) c[(i) + (j) * ldc]
  if (m == 0 || n == 0 || ((alpha == 0.0 || k == 0) && beta == 1.0)) return;
  if (alpha == 0.0) {
    for (int j = 1; j <= n; ++j)
      for (int i = 1; i <= m; ++i) C_(i, j) = beta == 0.0 ? 0.0 : beta * C_(i, j);
    return;
  }
  if (transa == 'T' && transb == 'N') {
    for (int j = 1; j <= n; ++j)
      for (int i = 1; i <= m; ++i) {
        double temp = 0.0;
        for (int l = 1; l <= k; ++l) temp = fma(A_(l, i), B_(l, j), temp);
        if (beta == 0.0) C_(i, j) = alpha * temp;
        else C_(i, j) = fma(alpha, temp, beta * C_(i, j));
      }
  } else {  // 'N','T'
    for (int j = 1; j <= n; ++j) {
      if (beta == 0.0) {
        for (int i = 1; i <= m; ++i) C_(i, j) = 0.0;
      } else if (beta != 1.0) {
        for (int i = 1; i <= m; ++i) C_(i, j) = beta * C_(i, j);
      }
      for (int l = 1; l <= k; ++l) {
        const double temp = alpha * B_(j, l);
        for (int i = 1; i <= m; ++i) C_(i, j) = fma(temp, A_(i, l), C_(i, j));
      }
    }
  }
#undef B_
#undef C_
}

// dger.f, INCX=INCY=1 (A + X*TEMP fused).
void dger(int m, int n, double alpha, const double* x, const double* y, double* a, int lda) {
  a -= 1 + lda; --x; --y;
  if (m == 0 || n == 0 || alpha == 0.0) return;
  int jy = 1;
  for (int j = 1; j <= n; ++j) {
    if (y[jy] != 0.0) {
      const double temp = alpha * y[jy];
      for (int i = 1; i <= m; ++i) A_(i, j) = fma(x[i], temp, A_(i, j));
    }
    jy += 1;
  }
}

// ------------------------------------------------- small LAPACK auxiliaries
// dlapy2.f (1 fmadd: ONE + (Z/W)**2).
double dlapy2(double x, double y) {
  const bool x_is_nan = disnan(x), y_is_nan = disnan(y);
  double r = 0.0;
  if (x_is_nan) r = x;
  if (y_is_nan) r = y;
  const double hugeval = dlamch('O');
  if (!(x_is_nan || y_is_nan)) {
    const double xabs = fabs(x), yabs = fabs(y);
    const double w = fmax(xabs, yabs), z = fmin(xabs, yabs);
    if (z == 0.0 || w > hugeval) {
      r = w;
    } else {
      const double q = z / w;
      r = w * sqrt(fma(q, q, 1.0));
    }
  }
  return r;
}

// dlansy.f, NORM='M', UPLO='L'.
double dlansyML(int n, const double* a, int lda) {
  a -= 1 + lda;
  double value = 0.0;
  if (n == 0) return 0.0;
  for (int j = 1; j <= n; ++j)
    for (int i = j; i <= n; ++i) {
      const double sum = fabs(A_(i, j));
      if (value < sum || disnan(sum)) value = sum;
    }
  return value;
}

// dlanst.f, NORM='M'.
double dlanstM(int n, const double* d, const double* e) {
  --d; --e;
  if (n <= 0) return 0.0;
  double anorm = fabs(d[n]);
  for (int i = 1; i <= n - 1; ++i) {
    double sum = fabs(d[i]);
    if (anorm < sum || disnan(sum)) anorm = sum;
    sum = fabs(e[i]);
    if (anorm < sum || disnan(sum)) anorm = sum;
  }
  return anorm;
}

// dlaset.f, UPLO='Full'.
void dlasetFull(int m, int n, double alpha, double beta, double* a, int lda) {
  a -= 1 + lda;
  for (int j = 1; j <= n; ++j)
    for (int i = 1; i <= m; ++i) A_(i, j) = alpha;
  for (int i = 1; i <= (m < n ? m : n); ++i) A_(i, i) = beta;
}

// dlasrt.f (ID = 'D' or 'I').
int dlasrt(char id, int n, double* d) {
  --d;
  int dir = -1;
  if (id == 'D') dir = 0;
  else if (id == 'I') dir = 1;
  if (dir == -1) return -1;
  if (n < 0) return -2;
  if (n <= 1) return 0;
  const int select = 20;
  int stack[2][33];
  int stkpnt = 1;
  stack[0][1] = 1;
  stack[1][1] = n;
  do {
    const int start = stack[0][stkpnt];
    const int endd = stack[1][stkpnt];
    --stkpnt;
    if (endd - start <= select && endd - start > 0) {
      if (dir == 0) {
        for (int i = start + 1; i <= endd; ++i)
          for (int j = i; j >= start + 1; --j) {
            if (d[j] > d[j - 1]) {
              const double dmnmx = d[j];
              d[j] = d[j - 1];
              d[j - 1] = dmnmx;
            } else {
              break;
            }
          }
      } else {
        for (int i = start + 1; i <= endd; ++i)
          for (int j = i; j >= start + 1; --j) {
            if (d[j] < d[j - 1]) {
              const double dmnmx = d[j];
              d[j] = d[j - 1];
              d[j - 1] = dmnmx;
            } else {
              break;
            }
          }
      }
    } else if (endd - start > select) {
      const double d1 = d[start], d2 = d[endd];
      int i = (start + endd) / 2;
      const double d3 = d[i];
      double dmnmx;
      if (d1 < d2) {
        if (d3 < d1) dmnmx = d1;
        else if (d3 < d2) dmnmx = d3;
        else dmnmx = d2;
      } else {
        if (d3 < d2) dmnmx = d2;
        else if (d3 < d1) dmnmx = d3;
        else dmnmx = d1;
      }
      int j;
      if (dir == 0) {
        i = start - 1;
        j = endd + 1;
        for (;;) {
          do --j; while (d[j] < dmnmx);
          do ++i; while (d[i] > dmnmx);
          if (i < j) {
            const double tmp = d[i];
            d[i] = d[j];
            d[j] = tmp;
          } else {
            break;
          }
        }
      } else {
        i = start - 1;
        j = endd + 1;
        for (;;) {
          do --j; while (d[j] > dmnmx);
          do ++i; while (d[i] < dmnmx);
          if (i < j) {
            const double tmp = d[i];
            d[i] = d[j];
            d[j] = tmp;
          } else {
            break;
          }
        }
      }
      if (j - start > endd - j - 1) {
        ++stkpnt; stack[0][stkpnt] = start; stack[1][stkpnt] = j;
        ++stkpnt; stack[0][stkpnt] = j + 1; stack[1][stkpnt] = endd;
      } else {
        ++stkpnt; stack[0][stkpnt] = j + 1; stack[1][stkpnt] = endd;
        ++stkpnt; stack[0][stkpnt] = start; stack[1][stkpnt] = j;
      }
    }
  } while (stkpnt > 0);
  return 0;
}

// dlaev2.f (6 fmadd/fnmsub: the two ONE+q**2 under SQRT, RT2 twice, and
// ONE+CT*CT / ONE+TN*TN). In RT2 = (ACMX/RT1)*ACMN - (B/RT1)*B the first
// product is the fused one.
void dlaev2(double a, double b, double c, double& rt1, double& rt2, double& cs1, double& sn1) {
  const double sm = a + c, df = a - c, adf = fabs(df), tb = b + b, ab = fabs(tb);
  double acmx, acmn, rt;
  if (fabs(a) > fabs(c)) {
    acmx = a;
    acmn = c;
  } else {
    acmx = c;
    acmn = a;
  }
  if (adf > ab) {
    const double q = ab / adf;
    rt = adf * sqrt(fma(q, q, 1.0));
  } else if (adf < ab) {
    const double q = adf / ab;
    rt = ab * sqrt(fma(q, q, 1.0));
  } else {
    rt = ab * sqrt(2.0);
  }
  int sgn1, sgn2;
  if (sm < 0.0) {
    rt1 = 0.5 * (sm - rt);
    sgn1 = -1;
    rt2 = fma(acmx / rt1, acmn, -((b / rt1) * b));
  } else if (sm > 0.0) {
    rt1 = 0.5 * (sm + rt);
    sgn1 = 1;
    rt2 = fma(acmx / rt1, acmn, -((b / rt1) * b));
  } else {
    rt1 = 0.5 * rt;
    rt2 = -0.5 * rt;
    sgn1 = 1;
  }
  double cs;
  if (df >= 0.0) {
    cs = df + rt;
    sgn2 = 1;
  } else {
    cs = df - rt;
    sgn2 = -1;
  }
  const double acs = fabs(cs);
  if (acs > ab) {
    const double ct = -tb / cs;
    sn1 = 1.0 / sqrt(fma(ct, ct, 1.0));
    cs1 = ct * sn1;
  } else {
    if (ab == 0.0) {
      cs1 = 1.0;
      sn1 = 0.0;
    } else {
      const double tn = -cs / tb;
      cs1 = 1.0 / sqrt(fma(tn, tn, 1.0));
      sn1 = tn * cs1;
    }
  }
  if (sgn1 == sgn2) {
    const double tn = cs1;
    cs1 = -sn1;
    sn1 = tn;
  }
}

// ------------------------------------------- Householder reduction (DSYTRD)
// dlarfg.f (no FMA of its own; DLAPY2's one).
void dlarfg(int n, double& alpha, double* x, int incx, double& tau) {
  if (n <= 1) {
    tau = 0.0;
    return;
  }
  double xnorm = dnrm2(n - 1, x, incx);
  if (xnorm == 0.0) {
    tau = 0.0;
  } else {
    double beta = -copysign(dlapy2(alpha, xnorm), alpha);
    const double safmin = dlamch('S') / dlamch('E');
    int knt = 0;
    if (fabs(beta) < safmin) {
      const double rsafmn = 1.0 / safmin;
      do {
        ++knt;
        dscal(n - 1, rsafmn, x, incx);
        beta = beta * rsafmn;
        alpha = alpha * rsafmn;
      } while (fabs(beta) < safmin && knt < 20);
      xnorm = dnrm2(n - 1, x, incx);
      beta = -copysign(dlapy2(alpha, xnorm), alpha);
    }
    tau = (beta - alpha) / beta;
    dscal(n - 1, 1.0 / (alpha - beta), x, incx);
    for (int j = 1; j <= knt; ++j) beta = beta * safmin;
    alpha = beta;
  }
}

// dsytd2.f, UPLO='L'. ALPHA = -HALF*TAUI*DDOT(...) is (-HALF*TAUI)*DDOT.
void dsytd2L(int n, double* a, int lda, double* d, double* e, double* tau) {
  a -= 1 + lda; --d; --e; --tau;
  if (n <= 0) return;
  for (int i = 1; i <= n - 1; ++i) {
    double taui;
    dlarfg(n - i, A_(i + 1, i), &A_(i + 2 < n ? i + 2 : n, i), 1, taui);
    e[i] = A_(i + 1, i);
    if (taui != 0.0) {
      A_(i + 1, i) = 1.0;
      dsymvL(n - i, taui, &A_(i + 1, i + 1), lda, &A_(i + 1, i), 0.0, &tau[i]);
      const double alpha = (-0.5 * taui) * ddot(n - i, &tau[i], 1, &A_(i + 1, i), 1);
      daxpy(n - i, alpha, &A_(i + 1, i), 1, &tau[i], 1);
      dsyr2L(n - i, -1.0, &A_(i + 1, i), &tau[i], &A_(i + 1, i + 1), lda);
      A_(i + 1, i) = e[i];
    }
    d[i] = A_(i, i);
    tau[i] = taui;
  }
  d[n] = A_(n, n);
}

// dlatrd.f, UPLO='L' (no FMA of its own; ALPHA is (-HALF*TAU)*DDOT).
void dlatrdL(int n, int nb, double* a, int lda, double* e, double* tau, double* w, int ldw) {
  a -= 1 + lda; --e; --tau; w -= 1 + ldw;
#define W_(i, j) w[(i) + (j) * ldw]
  if (n <= 0) return;
  for (int i = 1; i <= nb; ++i) {
    dgemvN(n - i + 1, i - 1, -1.0, &A_(i, 1), lda, &W_(i, 1), ldw, 1.0, &A_(i, i));
    dgemvN(n - i + 1, i - 1, -1.0, &W_(i, 1), ldw, &A_(i, 1), lda, 1.0, &A_(i, i));
    if (i < n) {
      dlarfg(n - i, A_(i + 1, i), &A_(i + 2 < n ? i + 2 : n, i), 1, tau[i]);
      e[i] = A_(i + 1, i);
      A_(i + 1, i) = 1.0;
      dsymvL(n - i, 1.0, &A_(i + 1, i + 1), lda, &A_(i + 1, i), 0.0, &W_(i + 1, i));
      dgemvT(n - i, i - 1, 1.0, &W_(i + 1, 1), ldw, &A_(i + 1, i), 0.0, &W_(1, i));
      dgemvN(n - i, i - 1, -1.0, &A_(i + 1, 1), lda, &W_(1, i), 1, 1.0, &W_(i + 1, i));
      dgemvT(n - i, i - 1, 1.0, &A_(i + 1, 1), lda, &A_(i + 1, i), 0.0, &W_(1, i));
      dgemvN(n - i, i - 1, -1.0, &W_(i + 1, 1), ldw, &W_(1, i), 1, 1.0, &W_(i + 1, i));
      dscal(n - i, tau[i], &W_(i + 1, i), 1);
      const double alpha = (-0.5 * tau[i]) * ddot(n - i, &W_(i + 1, i), 1, &A_(i + 1, i), 1);
      daxpy(n - i, alpha, &A_(i + 1, i), 1, &W_(i + 1, i), 1);
    }
  }
#undef W_
}

// dsytrd.f, UPLO='L': ILAENV's block-size decision (NB from LWORK when the
// workspace is short), DLATRD + DSYR2K panels, DSYTD2 on the trailing block.
int dsytrdL(int n, double* a, int lda, double* d, double* e, double* tau, double* work,
            int lwork) {
  if (n < 0) return -2;
  if (lda < (n > 1 ? n : 1)) return -4;
  if (lwork < 1) return -9;
  int nb = ilaenv(1, "DSYTRD");
  if (n == 0) return 0;
  int nx = n;
  int ldwork = 1;
  if (nb > 1 && nb < n) {
    const int il3 = ilaenv(3, "DSYTRD");
    nx = nb > il3 ? nb : il3;
    if (nx < n) {
      ldwork = n;
      const int iws = ldwork * nb;
      if (lwork < iws) {
        nb = lwork / ldwork > 1 ? lwork / ldwork : 1;
        const int nbmin = ilaenv(2, "DSYTRD");
        if (nb < nbmin) nx = n;
      }
    } else {
      nx = n;
    }
  } else {
    nb = 1;
  }
  a -= 1 + lda; --d; --e; --tau; --work;
  int i;
  for (i = 1; i <= n - nx; i += nb) {
    dlatrdL(n - i + 1, nb, &A_(i, i), lda, &e[i], &tau[i], &work[1], ldwork);
    dsyr2kLN(n - i - nb + 1, nb, -1.0, &A_(i + nb, i), lda, &work[nb + 1], ldwork, 1.0,
             &A_(i + nb, i + nb), lda);
    for (int j = i; j <= i + nb - 1; ++j) {
      A_(j + 1, j) = e[j];
      d[j] = A_(j, j);
    }
  }
  dsytd2L(n - i + 1, &A_(i, i), lda, &d[i], &e[i], &tau[i]);
  return 0;
}

// --------------------------------------------------- back-transformation
// iladlc.f
int iladlc(int m, int n, const double* a, int lda) {
  a -= 1 + lda;
  if (n == 0) return n;
  if (A_(1, n) != 0.0 || A_(m, n) != 0.0) return n;
  for (int c = n; c >= 1; --c)
    for (int i = 1; i <= m; ++i)
      if (A_(i, c) != 0.0) return c;
  return 0;
}

// dlarf.f, SIDE='L' (FMA only inside DGEMV/DGER).
void dlarfL(int m, int n, double* v, int incv, double tau, double* c, int ldc, double* work) {
  --v;
  int lastv = 0, lastc = 0;
  if (tau != 0.0) {
    lastv = m;
    int i = incv > 0 ? 1 + (lastv - 1) * incv : 1;
    while (lastv > 0 && v[i] == 0.0) {
      --lastv;
      i -= incv;
    }
    lastc = iladlc(lastv, n, c, ldc);
  }
  if (lastv > 0) {
    dgemvT(lastv, lastc, 1.0, c, ldc, v + 1, 0.0, work);
    dger(lastv, lastc, -tau, v + 1, work, c, ldc);
  }
}

// dlarft.f, DIRECT='F', STOREV='C'.
void dlarftFC(int n, int k, const double* v, int ldv, const double* tau, double* t, int ldt) {
  const double* v1 = v;
  double* t1 = t;
  v -= 1 + ldv; --tau; t -= 1 + ldt;
#define V_(i, j) v[(i) + (j) * ldv]
#define T_(i, j) t[(i) + (j) * ldt]
  if (n == 0) return;
  int prevlastv = n;
  for (int i = 1; i <= k; ++i) {
    prevlastv = prevlastv > i ? prevlastv : i;
    if (tau[i] == 0.0) {
      for (int j = 1; j <= i; ++j) T_(j, i) = 0.0;
    } else {
      int lastv;
      for (lastv = n; lastv >= i + 1; --lastv)
        if (V_(lastv, i) != 0.0) break;
      for (int j = 1; j <= i - 1; ++j) T_(j, i) = -tau[i] * V_(i, j);
      const int j = lastv < prevlastv ? lastv : prevlastv;
      dgemvT(j - i, i - 1, -tau[i], &V_(i + 1, 1), ldv, &V_(i + 1, i), 1.0, &T_(1, i));
      dtrmvUNN(i - 1, t1, ldt, &T_(1, i));
      T_(i, i) = tau[i];
      if (i > 1) prevlastv = prevlastv > lastv ? prevlastv : lastv;
      else prevlastv = lastv;
    }
  }
  (void)v1;
#undef V_
#undef T_
}

// dlarfb.f, SIDE='L', TRANS='N' (TRANST='T'), DIRECT='F', STOREV='C'.
void dlarfbLNFC(int m, int n, int k, const double* v, int ldv, const double* t, int ldt, double* c,
                int ldc, double* work, int ldwork) {
  const double* v1 = v;
  double* c1 = c;
  double* work1 = work;
  c -= 1 + ldc; work -= 1 + ldwork;
#define C_(i, j) c[(i) + (j) * ldc]
#define WK_(i, j) work[(i) + (j) * ldwork]
  if (m <= 0 || n <= 0) return;
  for (int j = 1; j <= k; ++j) dcopy(n, &C_(j, 1), ldc, &WK_(1, j), 1);
  dtrmmR('L', 'N', 'U', n, k, 1.0, v1, ldv, work1, ldwork);
  if (m > k) dgemm('T', 'N', n, k, m - k, 1.0, &C_(k + 1, 1), ldc, v1 + k, ldv, 1.0, work1, ldwork);
  dtrmmR('U', 'T', 'N', n, k, 1.0, t, ldt, work1, ldwork);
  if (m > k) dgemm('N', 'T', m - k, n, k, -1.0, v1 + k, ldv, work1, ldwork, 1.0, &C_(k + 1, 1), ldc);
  dtrmmR('L', 'T', 'U', n, k, 1.0, v1, ldv, work1, ldwork);
  for (int j = 1; j <= k; ++j)
    for (int i = 1; i <= n; ++i) C_(j, i) = C_(j, i) - WK_(i, j);
  (void)c1;
#undef C_
#undef WK_
}

// dorm2r.f, SIDE='L', TRANS='N' (I runs K, K-1, ..., 1).
void dorm2rLN(int m, int n, int k, double* a, int lda, const double* tau, double* c, int ldc,
              double* work) {
  a -= 1 + lda; --tau;
  if (m == 0 || n == 0 || k == 0) return;
  const int ni = n, jc = 1;
  for (int i = k; i >= 1; --i) {
    const int mi = m - i + 1, ic = i;
    const double aii = A_(i, i);
    A_(i, i) = 1.0;
    dlarfL(mi, ni, &A_(i, i), 1, tau[i], c + (ic - 1) + (jc - 1) * ldc, ldc, work);
    A_(i, i) = aii;
  }
}

// dormqr.f, SIDE='L', TRANS='N': ILAENV/LWORK decide between DORM2R and the
// blocked DLARFT/DLARFB loop.
int dormqrLN(int m, int n, int k, double* a, int lda, const double* tau, double* c, int ldc,
             double* work, int lwork) {
  const int nbmax = 64, ldt = nbmax + 1, tsize = ldt * nbmax;
  const int nw = n > 1 ? n : 1;
  int nb = ilaenv(1, "DORMQR");
  if (nb > nbmax) nb = nbmax;
  const int lwkopt = nw * nb + tsize;
  if (m == 0 || n == 0 || k == 0) return 0;
  int nbmin = 2;
  const int ldwork = nw;
  if (nb > 1 && nb < k) {
    if (lwork < lwkopt) {
      nb = (lwork - tsize) / ldwork;
      const int il2 = ilaenv(2, "DORMQR");
      nbmin = il2 > 2 ? il2 : 2;
    }
  }
  if (nb < nbmin || nb >= k) {
    dorm2rLN(m, n, k, a, lda, tau, c, ldc, work);
    return 0;
  }
  // Blocked: LEFT and NOTRAN, so I runs from ((K-1)/NB)*NB+1 down to 1.
  const int nq = m;
  const int iwt = 1 + nw * nb;
  const int i1 = ((k - 1) / nb) * nb + 1, i3 = -nb;
  const int ni = n, jc = 1;
  for (int i = i1; i >= 1; i += i3) {
    const int ib = nb < k - i + 1 ? nb : k - i + 1;
    double* aii = a + (i - 1) + (i - 1) * lda;
    dlarftFC(nq - i + 1, ib, aii, lda, tau + (i - 1), work + (iwt - 1), ldt);
    const int mi = m - i + 1, ic = i;
    dlarfbLNFC(mi, ni, ib, aii, lda, work + (iwt - 1), ldt, c + (ic - 1) + (jc - 1) * ldc, ldc, work,
               ldwork);
  }
  return 0;
}

// dormtr.f, SIDE='L', UPLO='L', TRANS='N' -> DORMQR on A(2,1), C(2,1).
int dormtrLLN(int m, int n, double* a, int lda, const double* tau, double* c, int ldc,
              double* work, int lwork) {
  const int nq = m;
  if (m == 0 || n == 0 || nq == 1) return 0;
  const int mi = m - 1, ni = n;
  return dormqrLN(mi, ni, nq - 1, a + 1, lda, tau, c + 1, ldc, work, lwork);
}

// ------------------------------------------------ MRRR (DSTEMR) machinery
// dlaruv.f (3 fmadd in the Horner form; R = 2**-12 makes them exact).
const int kMM[128][4] = {
#include "lapack_dlaruv_mm.inc"
};

void dlaruv(int* iseed, int n, double* x) {
  --iseed; --x;
  const int ipw2 = 4096;
  const double r = 1.0 / ipw2;
  if (n < 1) return;
  int i1 = iseed[1], i2 = iseed[2], i3 = iseed[3], i4 = iseed[4];
  int it1 = 0, it2 = 0, it3 = 0, it4 = 0;
  for (int i = 1; i <= (n < 128 ? n : 128); ++i) {
    const int* mm = kMM[i - 1];  // mm[0..3] = MM(I,1..4)
    for (;;) {
      it4 = i4 * mm[3];
      it3 = it4 / ipw2;
      it4 = it4 - ipw2 * it3;
      it3 = it3 + i3 * mm[3] + i4 * mm[2];
      it2 = it3 / ipw2;
      it3 = it3 - ipw2 * it2;
      it2 = it2 + i2 * mm[3] + i3 * mm[2] + i4 * mm[1];
      it1 = it2 / ipw2;
      it2 = it2 - ipw2 * it1;
      it1 = it1 + i1 * mm[3] + i2 * mm[2] + i3 * mm[1] + i4 * mm[0];
      it1 = it1 % ipw2;
      x[i] = r * fma(r, fma(r, fma(r, double(it4), double(it3)), double(it2)), double(it1));
      if (x[i] == 1.0) {
        i1 += 2;
        i2 += 2;
        i3 += 2;
        i4 += 2;
        continue;
      }
      break;
    }
  }
  iseed[1] = it1;
  iseed[2] = it2;
  iseed[3] = it3;
  iseed[4] = it4;
}

// dlarnv.f, IDIST=2 (uniform (-1,1)): TWO*U - ONE, fused (exact either way).
void dlarnv2(int* iseed, int n, double* x) {
  --x;
  const int lv = 128;
  double u[lv];
  for (int iv = 1; iv <= n; iv += lv / 2) {
    const int il = lv / 2 < n - iv + 1 ? lv / 2 : n - iv + 1;
    dlaruv(iseed, il, u);
    for (int i = 1; i <= il; ++i) x[iv + i - 1] = fma(2.0, u[i - 1], -1.0);
  }
}

// dlarrr.f
int dlarrr(int n, const double* d, const double* e) {
  --d; --e;
  if (n <= 0) return 0;
  const double relcond = 0.999;
  const double safmin = dlamch('S'), eps = dlamch('P');
  const double smlnum = safmin / eps, rmin = sqrt(smlnum);
  bool yesrel = true;
  double offdig = 0.0;
  double tmp = sqrt(fabs(d[1]));
  if (tmp < rmin) yesrel = false;
  if (yesrel) {
    for (int i = 2; i <= n; ++i) {
      const double tmp2 = sqrt(fabs(d[i]));
      if (tmp2 < rmin) yesrel = false;
      if (!yesrel) break;
      const double offdig2 = fabs(e[i - 1]) / (tmp * tmp2);
      if (offdig + offdig2 >= relcond) yesrel = false;
      if (!yesrel) break;
      tmp = tmp2;
      offdig = offdig2;
    }
  }
  return yesrel ? 0 : 1;
}

// dlarra.f
void dlarra(int n, const double* d, double* e, double* e2, double spltol, double tnrm,
            int& nsplit, int* isplit) {
  --d; --e; --e2; --isplit;
  nsplit = 1;
  if (n <= 0) return;
  if (spltol < 0.0) {
    const double tmp1 = fabs(spltol) * tnrm;
    for (int i = 1; i <= n - 1; ++i) {
      const double eabs = fabs(e[i]);
      if (eabs <= tmp1) {
        e[i] = 0.0;
        e2[i] = 0.0;
        isplit[nsplit] = i;
        ++nsplit;
      }
    }
  } else {
    for (int i = 1; i <= n - 1; ++i) {
      const double eabs = fabs(e[i]);
      if (eabs <= spltol * sqrt(fabs(d[i])) * sqrt(fabs(d[i + 1]))) {
        e[i] = 0.0;
        e2[i] = 0.0;
        isplit[nsplit] = i;
        ++nsplit;
      }
    }
  }
  isplit[nsplit] = n;
}

// dlarrc.f, JOBT='T' (the FMA pair of the dylib is in the 'L' branch).
void dlarrcT(int n, double vl, double vu, const double* d, const double* e, int& eigcnt,
             int& lcnt, int& rcnt) {
  --d; --e;
  lcnt = 0;
  rcnt = 0;
  eigcnt = 0;
  if (n <= 0) return;
  double lpivot = d[1] - vl, rpivot = d[1] - vu;
  if (lpivot <= 0.0) ++lcnt;
  if (rpivot <= 0.0) ++rcnt;
  for (int i = 1; i <= n - 1; ++i) {
    const double tmp = e[i] * e[i];
    lpivot = (d[i + 1] - vl) - tmp / lpivot;
    rpivot = (d[i + 1] - vu) - tmp / rpivot;
    if (lpivot <= 0.0) ++lcnt;
    if (rpivot <= 0.0) ++rcnt;
  }
  eigcnt = rcnt - lcnt;
}

// dlarrk.f (2 fmadd/fmsub: GL - (FUDGE*TNORM*EPS)*N and GU + ...; the
// FUDGE*TWO*PIVMIN term is shared with ATOLI, so it is rounded).
int dlarrk(int n, int iw, double gl, double gu, const double* d, const double* e2,
           double pivmin, double reltol, double& w, double& werr) {
  --d; --e2;
  if (n <= 0) return 0;
  const double fudge = 2.0, two = 2.0, half = 0.5;
  const double eps = dlamch('P');
  const double tnorm = fmax(fabs(gl), fabs(gu));
  const double rtoli = reltol;
  const double atoli = fudge * two * pivmin;
  const int itmax = int((std::log(tnorm + pivmin) - std::log(pivmin)) / kLn2) + 2;
  int info = -1;
  const double t = fudge * tnorm * eps;
  double left = fma(-t, double(n), gl) - atoli;
  double right = fma(t, double(n), gu) + atoli;
  int it = 0;
  for (;;) {
    double tmp1 = fabs(right - left);
    const double tmp2 = fmax(fabs(right), fabs(left));
    if (tmp1 < fmax(fmax(atoli, pivmin), rtoli * tmp2)) {
      info = 0;
      break;
    }
    if (it > itmax) break;
    ++it;
    const double mid = half * (left + right);
    int negcnt = 0;
    tmp1 = d[1] - mid;
    if (fabs(tmp1) < pivmin) tmp1 = -pivmin;
    if (tmp1 <= 0.0) ++negcnt;
    for (int i = 2; i <= n; ++i) {
      tmp1 = d[i] - e2[i - 1] / tmp1 - mid;
      if (fabs(tmp1) < pivmin) tmp1 = -pivmin;
      if (tmp1 <= 0.0) ++negcnt;
    }
    if (negcnt >= iw) right = mid;
    else left = mid;
  }
  w = half * (left + right);
  werr = half * fabs(right - left);
  return info;
}

// dlaneg.f (4 fnmsub: T = TMP*LLD(J) - SIGMA and P = TMP*D(J) - SIGMA).
int dlaneg(int n, const double* d, const double* lld, double sigma, double pivmin, int r) {
  --d; --lld;
  (void)pivmin;
  const int blklen = 128;
  int negcnt = 0;
  double t = -sigma;
  for (int bj = 1; bj <= r - 1; bj += blklen) {
    int neg1 = 0;
    const double bsav = t;
    const int jend = bj + blklen - 1 < r - 1 ? bj + blklen - 1 : r - 1;
    for (int j = bj; j <= jend; ++j) {
      const double dplus = d[j] + t;
      if (dplus < 0.0) ++neg1;
      const double tmp = t / dplus;
      t = fma(tmp, lld[j], -sigma);
    }
    if (disnan(t)) {
      neg1 = 0;
      t = bsav;
      for (int j = bj; j <= jend; ++j) {
        const double dplus = d[j] + t;
        if (dplus < 0.0) ++neg1;
        double tmp = t / dplus;
        if (disnan(tmp)) tmp = 1.0;
        t = fma(tmp, lld[j], -sigma);
      }
    }
    negcnt += neg1;
  }
  double p = d[n] - sigma;
  for (int bj = n - 1; bj >= r; bj -= blklen) {
    int neg2 = 0;
    const double bsav = p;
    const int jend = bj - blklen + 1 > r ? bj - blklen + 1 : r;
    for (int j = bj; j >= jend; --j) {
      const double dminus = lld[j] + p;
      if (dminus < 0.0) ++neg2;
      const double tmp = p / dminus;
      p = fma(tmp, d[j], -sigma);
    }
    if (disnan(p)) {
      neg2 = 0;
      p = bsav;
      for (int j = bj; j >= jend; --j) {
        const double dminus = lld[j] + p;
        if (dminus < 0.0) ++neg2;
        double tmp = p / dminus;
        if (disnan(tmp)) tmp = 1.0;
        p = fma(tmp, d[j], -sigma);
      }
    }
    negcnt += neg2;
  }
  const double gamma = (t + sigma) + p;
  if (gamma < 0.0) ++negcnt;
  return negcnt;
}

// dlarrb.f (no FMA).
int dlarrb(int n, const double* d, const double* lld, int ifirst, int ilast, double rtol1,
           double rtol2, int offset, double* w, double* wgap, double* werr, double* work,
           int* iwork, double pivmin, double spdiam, int twist) {
  --w; --wgap; --werr; --work; --iwork;
  if (n <= 0) return 0;
  const double two = 2.0, half = 0.5;
  const int maxitr = int((std::log(spdiam + pivmin) - std::log(pivmin)) / kLn2) + 2;
  const double mnwdth = two * pivmin;
  int r = twist;
  if (r < 1 || r > n) r = n;
  int i1 = ifirst, nint = 0, prev = 0;
  double rgap = wgap[i1 - offset];
  const int i1start = i1;
  for (int i = i1start; i <= ilast; ++i) {
    const int k = 2 * i, ii = i - offset;
    double left = w[ii] - werr[ii];
    double right = w[ii] + werr[ii];
    const double lgap = rgap;
    rgap = wgap[ii];
    const double gap = fmin(lgap, rgap);
    double back = werr[ii];
    int negcnt;
    for (;;) {
      negcnt = dlaneg(n, d, lld, left, pivmin, r);
      if (negcnt > i - 1) {
        left = left - back;
        back = two * back;
      } else {
        break;
      }
    }
    back = werr[ii];
    for (;;) {
      negcnt = dlaneg(n, d, lld, right, pivmin, r);
      if (negcnt < i) {
        right = right + back;
        back = two * back;
      } else {
        break;
      }
    }
    const double width = half * fabs(left - right);
    const double tmp = fmax(fabs(left), fabs(right));
    const double cvrgd = fmax(rtol1 * gap, rtol2 * tmp);
    if (width <= cvrgd || width <= mnwdth) {
      iwork[k - 1] = -1;
      if (i == i1 && i < ilast) i1 = i + 1;
      if (prev >= i1 && i <= ilast) iwork[2 * prev - 1] = i + 1;
    } else {
      prev = i;
      ++nint;
      iwork[k - 1] = i + 1;
      iwork[k] = negcnt;
    }
    work[k - 1] = left;
    work[k] = right;
  }
  int iter = 0;
  do {
    prev = i1 - 1;
    int i = i1;
    const int olnint = nint;
    for (int ip = 1; ip <= olnint; ++ip) {
      const int k = 2 * i, ii = i - offset;
      const double rgap2 = wgap[ii];
      double lgap = rgap2;
      if (ii > 1) lgap = wgap[ii - 1];
      const double gap = fmin(lgap, rgap2);
      const int next = iwork[k - 1];
      const double left = work[k - 1], right = work[k];
      const double mid = half * (left + right);
      const double width = right - mid;
      const double tmp = fmax(fabs(left), fabs(right));
      const double cvrgd = fmax(rtol1 * gap, rtol2 * tmp);
      if (width <= cvrgd || width <= mnwdth || iter == maxitr) {
        --nint;
        iwork[k - 1] = 0;
        if (i1 == i) {
          i1 = next;
        } else {
          if (prev >= i1) iwork[2 * prev - 1] = next;
        }
        i = next;
        continue;
      }
      prev = i;
      const int negcnt = dlaneg(n, d, lld, mid, pivmin, r);
      if (negcnt <= i - 1) work[k - 1] = mid;
      else work[k] = mid;
      i = next;
    }
    ++iter;
  } while (nint > 0 && iter <= maxitr);
  for (int i = ifirst; i <= ilast; ++i) {
    const int k = 2 * i, ii = i - offset;
    if (iwork[k - 1] == 0) {
      w[ii] = half * (work[k - 1] + work[k]);
      werr[ii] = work[k] - w[ii];
    }
  }
  for (int i = ifirst + 1; i <= ilast; ++i) {
    const int ii = i - offset;
    wgap[ii - 1] = fmax(0.0, w[ii] - werr[ii] - w[ii - 1] - werr[ii - 1]);
  }
  return 0;
}

// dlasq4.f (12 fused sites, marked).
void dlasq4(int i0, int n0, const double* z, int pp, int n0in, double dmin, double dmin1,
            double dmin2, double dn, double dn1, double dn2, double& tau, int& ttype, double& g) {
  --z;
  const double cnst1 = 0.5630, cnst2 = 1.010, cnst3 = 1.050;
  const double qurtr = 0.250, third = 0.3330, half = 0.50, one = 1.0, two = 2.0, hundrd = 100.0;
  if (dmin <= 0.0) {
    tau = -dmin;
    ttype = -1;
    return;
  }
  const int nn = 4 * n0 + pp;
  double s = 0.0, a2, b1, b2, gam, gap1, gap2;
  int np;
  if (n0in == n0) {
    if (dmin == dn || dmin == dn1) {
      b1 = sqrt(z[nn - 3]) * sqrt(z[nn - 5]);
      b2 = sqrt(z[nn - 7]) * sqrt(z[nn - 9]);
      a2 = z[nn - 7] + z[nn - 5];
      if (dmin == dn && dmin1 == dn1) {
        gap2 = fma(-dmin2, qurtr, dmin2 - a2);  // fused
        if (gap2 > 0.0 && gap2 > b2) gap1 = fma(-(b2 / gap2), b2, a2 - dn);  // fused
        else gap1 = a2 - dn - (b1 + b2);
        if (gap1 > 0.0 && gap1 > b1) {
          s = fmax(fma(-(b1 / gap1), b1, dn), half * dmin);  // fused
          ttype = -2;
        } else {
          s = 0.0;
          if (dn > b1) s = dn - b1;
          if (a2 > (b1 + b2)) s = fmin(s, a2 - (b1 + b2));
          s = fmax(s, third * dmin);
          ttype = -3;
        }
      } else {
        ttype = -4;
        s = qurtr * dmin;
        if (dmin == dn) {
          gam = dn;
          a2 = 0.0;
          if (z[nn - 5] > z[nn - 7]) return;
          b2 = z[nn - 5] / z[nn - 7];
          np = nn - 9;
        } else {
          np = nn - 2 * pp;
          gam = dn1;
          if (z[np - 4] > z[np - 2]) return;
          a2 = z[np - 4] / z[np - 2];
          if (z[nn - 9] > z[nn - 11]) return;
          b2 = z[nn - 9] / z[nn - 11];
          np = nn - 13;
        }
        a2 = a2 + b2;
        for (int i4 = np; i4 >= 4 * i0 - 1 + pp; i4 -= 4) {
          if (b2 == 0.0) break;
          b1 = b2;
          if (z[i4] > z[i4 - 2]) return;
          b2 = b2 * (z[i4] / z[i4 - 2]);
          a2 = a2 + b2;
          if (hundrd * fmax(b2, b1) < a2 || cnst1 < a2) break;
        }
        a2 = cnst3 * a2;
        if (a2 < cnst1) s = gam * (one - sqrt(a2)) / (one + a2);
      }
    } else if (dmin == dn2) {
      ttype = -5;
      s = qurtr * dmin;
      np = nn - 2 * pp;
      b1 = z[np - 2];
      b2 = z[np - 6];
      gam = dn2;
      if (z[np - 8] > b2 || z[np - 4] > b1) return;
      a2 = (z[np - 8] / b2) * (one + z[np - 4] / b1);
      if (n0 - i0 > 2) {
        b2 = z[nn - 13] / z[nn - 15];
        a2 = a2 + b2;
        for (int i4 = nn - 17; i4 >= 4 * i0 - 1 + pp; i4 -= 4) {
          if (b2 == 0.0) break;
          b1 = b2;
          if (z[i4] > z[i4 - 2]) return;
          b2 = b2 * (z[i4] / z[i4 - 2]);
          a2 = a2 + b2;
          if (hundrd * fmax(b2, b1) < a2 || cnst1 < a2) break;
        }
        a2 = cnst3 * a2;
      }
      if (a2 < cnst1) s = gam * (one - sqrt(a2)) / (one + a2);
    } else {
      if (ttype == -6) g = fma(third, one - g, g);  // fused
      else if (ttype == -18) g = qurtr * third;
      else g = qurtr;
      s = g * dmin;
      ttype = -6;
    }
  } else if (n0in == n0 + 1) {
    if (dmin1 == dn1 && dmin2 == dn2) {
      ttype = -7;
      s = third * dmin1;
      if (z[nn - 5] > z[nn - 7]) return;
      b1 = z[nn - 5] / z[nn - 7];
      b2 = b1;
      if (b2 != 0.0) {
        for (int i4 = 4 * n0 - 9 + pp; i4 >= 4 * i0 - 1 + pp; i4 -= 4) {
          a2 = b1;
          if (z[i4] > z[i4 - 2]) return;
          b1 = b1 * (z[i4] / z[i4 - 2]);
          b2 = b2 + b1;
          if (hundrd * fmax(b1, a2) < b2) break;
        }
      }
      b2 = sqrt(cnst3 * b2);
      a2 = dmin1 / fma(b2, b2, one);  // fused
      gap2 = fma(half, dmin2, -a2);    // fused
      if (gap2 > 0.0 && gap2 > b2 * a2) {
        s = fmax(s, a2 * fma(-((cnst2 * a2) * (b2 / gap2)), b2, one));  // fused
      } else {
        s = fmax(s, a2 * fma(-cnst2, b2, one));  // fused
        ttype = -8;
      }
    } else {
      s = qurtr * dmin1;
      if (dmin1 == dn1) s = half * dmin1;
      ttype = -9;
    }
  } else if (n0in == n0 + 2) {
    if (dmin2 == dn2 && two * z[nn - 5] < z[nn - 7]) {
      ttype = -10;
      s = third * dmin2;
      if (z[nn - 5] > z[nn - 7]) return;
      b1 = z[nn - 5] / z[nn - 7];
      b2 = b1;
      if (b2 != 0.0) {
        for (int i4 = 4 * n0 - 9 + pp; i4 >= 4 * i0 - 1 + pp; i4 -= 4) {
          if (z[i4] > z[i4 - 2]) return;
          b1 = b1 * (z[i4] / z[i4 - 2]);
          b2 = b2 + b1;
          if (hundrd * b1 < b2) break;
        }
      }
      b2 = sqrt(cnst3 * b2);
      a2 = dmin2 / fma(b2, b2, one);  // fused
      gap2 = fma(-sqrt(z[nn - 11]), sqrt(z[nn - 9]), z[nn - 7] + z[nn - 9]) - a2;  // fused
      if (gap2 > 0.0 && gap2 > b2 * a2) {
        s = fmax(s, a2 * fma(-((cnst2 * a2) * (b2 / gap2)), b2, one));  // fused
      } else {
        s = fmax(s, a2 * fma(-cnst2, b2, one));  // fused
      }
    } else {
      s = qurtr * dmin2;
      ttype = -11;
    }
  } else if (n0in > n0 + 2) {
    s = 0.0;
    ttype = -12;
  }
  tau = s;
}

// dlasq5.f (D*TEMP - TAU and Z*(DNM/Z) - TAU are fnmsub).
void dlasq5(int i0, int n0, double* z, int pp, double& tau, double sigma, double& dmin,
            double& dmin1, double& dmin2, double& dn, double& dnm1, double& dnm2, bool ieee,
            double eps) {
  --z;
  const double half = 0.5;
  if ((n0 - i0 - 1) <= 0) return;
  const double dthresh = eps * (sigma + tau);
  if (tau < dthresh * half) tau = 0.0;
  int j4, j4p2;
  double d, emin, temp;
  if (tau != 0.0) {
    j4 = 4 * i0 + pp - 3;
    emin = z[j4 + 4];
    d = z[j4] - tau;
    dmin = d;
    dmin1 = -z[j4];
    if (ieee) {
      if (pp == 0) {
        for (j4 = 4 * i0; j4 <= 4 * (n0 - 3); j4 += 4) {
          z[j4 - 2] = d + z[j4 - 1];
          temp = z[j4 + 1] / z[j4 - 2];
          d = fma(d, temp, -tau);
          dmin = fmin(dmin, d);
          z[j4] = z[j4 - 1] * temp;
          emin = fmin(z[j4], emin);
        }
      } else {
        for (j4 = 4 * i0; j4 <= 4 * (n0 - 3); j4 += 4) {
          z[j4 - 3] = d + z[j4];
          temp = z[j4 + 2] / z[j4 - 3];
          d = fma(d, temp, -tau);
          dmin = fmin(dmin, d);
          z[j4 - 1] = z[j4] * temp;
          emin = fmin(z[j4 - 1], emin);
        }
      }
      dnm2 = d;
      dmin2 = dmin;
      j4 = 4 * (n0 - 2) - pp;
      j4p2 = j4 + 2 * pp - 1;
      z[j4 - 2] = dnm2 + z[j4p2];
      z[j4] = z[j4p2 + 2] * (z[j4p2] / z[j4 - 2]);
      dnm1 = fma(z[j4p2 + 2], dnm2 / z[j4 - 2], -tau);
      dmin = fmin(dmin, dnm1);
      dmin1 = dmin;
      j4 += 4;
      j4p2 = j4 + 2 * pp - 1;
      z[j4 - 2] = dnm1 + z[j4p2];
      z[j4] = z[j4p2 + 2] * (z[j4p2] / z[j4 - 2]);
      dn = fma(z[j4p2 + 2], dnm1 / z[j4 - 2], -tau);
      dmin = fmin(dmin, dn);
    } else {
      if (pp == 0) {
        for (j4 = 4 * i0; j4 <= 4 * (n0 - 3); j4 += 4) {
          z[j4 - 2] = d + z[j4 - 1];
          if (d < 0.0) return;
          z[j4] = z[j4 + 1] * (z[j4 - 1] / z[j4 - 2]);
          d = fma(z[j4 + 1], d / z[j4 - 2], -tau);
          dmin = fmin(dmin, d);
          emin = fmin(emin, z[j4]);
        }
      } else {
        for (j4 = 4 * i0; j4 <= 4 * (n0 - 3); j4 += 4) {
          z[j4 - 3] = d + z[j4];
          if (d < 0.0) return;
          z[j4 - 1] = z[j4 + 2] * (z[j4] / z[j4 - 3]);
          d = fma(z[j4 + 2], d / z[j4 - 3], -tau);
          dmin = fmin(dmin, d);
          emin = fmin(emin, z[j4 - 1]);
        }
      }
      dnm2 = d;
      dmin2 = dmin;
      j4 = 4 * (n0 - 2) - pp;
      j4p2 = j4 + 2 * pp - 1;
      z[j4 - 2] = dnm2 + z[j4p2];
      if (dnm2 < 0.0) return;
      z[j4] = z[j4p2 + 2] * (z[j4p2] / z[j4 - 2]);
      dnm1 = fma(z[j4p2 + 2], dnm2 / z[j4 - 2], -tau);
      dmin = fmin(dmin, dnm1);
      dmin1 = dmin;
      j4 += 4;
      j4p2 = j4 + 2 * pp - 1;
      z[j4 - 2] = dnm1 + z[j4p2];
      if (dnm1 < 0.0) return;
      z[j4] = z[j4p2 + 2] * (z[j4p2] / z[j4 - 2]);
      dn = fma(z[j4p2 + 2], dnm1 / z[j4 - 2], -tau);
      dmin = fmin(dmin, dn);
    }
  } else {
    j4 = 4 * i0 + pp - 3;
    emin = z[j4 + 4];
    d = z[j4] - tau;
    dmin = d;
    dmin1 = -z[j4];
    if (ieee) {
      if (pp == 0) {
        for (j4 = 4 * i0; j4 <= 4 * (n0 - 3); j4 += 4) {
          z[j4 - 2] = d + z[j4 - 1];
          temp = z[j4 + 1] / z[j4 - 2];
          d = fma(d, temp, -tau);
          if (d < dthresh) d = 0.0;
          dmin = fmin(dmin, d);
          z[j4] = z[j4 - 1] * temp;
          emin = fmin(z[j4], emin);
        }
      } else {
        for (j4 = 4 * i0; j4 <= 4 * (n0 - 3); j4 += 4) {
          z[j4 - 3] = d + z[j4];
          temp = z[j4 + 2] / z[j4 - 3];
          d = fma(d, temp, -tau);
          if (d < dthresh) d = 0.0;
          dmin = fmin(dmin, d);
          z[j4 - 1] = z[j4] * temp;
          emin = fmin(z[j4 - 1], emin);
        }
      }
      dnm2 = d;
      dmin2 = dmin;
      j4 = 4 * (n0 - 2) - pp;
      j4p2 = j4 + 2 * pp - 1;
      z[j4 - 2] = dnm2 + z[j4p2];
      z[j4] = z[j4p2 + 2] * (z[j4p2] / z[j4 - 2]);
      dnm1 = fma(z[j4p2 + 2], dnm2 / z[j4 - 2], -tau);
      dmin = fmin(dmin, dnm1);
      dmin1 = dmin;
      j4 += 4;
      j4p2 = j4 + 2 * pp - 1;
      z[j4 - 2] = dnm1 + z[j4p2];
      z[j4] = z[j4p2 + 2] * (z[j4p2] / z[j4 - 2]);
      dn = fma(z[j4p2 + 2], dnm1 / z[j4 - 2], -tau);
      dmin = fmin(dmin, dn);
    } else {
      if (pp == 0) {
        for (j4 = 4 * i0; j4 <= 4 * (n0 - 3); j4 += 4) {
          z[j4 - 2] = d + z[j4 - 1];
          if (d < 0.0) return;
          z[j4] = z[j4 + 1] * (z[j4 - 1] / z[j4 - 2]);
          d = fma(z[j4 + 1], d / z[j4 - 2], -tau);
          if (d < dthresh) d = 0.0;
          dmin = fmin(dmin, d);
          emin = fmin(emin, z[j4]);
        }
      } else {
        for (j4 = 4 * i0; j4 <= 4 * (n0 - 3); j4 += 4) {
          z[j4 - 3] = d + z[j4];
          if (d < 0.0) return;
          z[j4 - 1] = z[j4 + 2] * (z[j4] / z[j4 - 3]);
          d = fma(z[j4 + 2], d / z[j4 - 3], -tau);
          if (d < dthresh) d = 0.0;
          dmin = fmin(dmin, d);
          emin = fmin(emin, z[j4 - 1]);
        }
      }
      dnm2 = d;
      dmin2 = dmin;
      j4 = 4 * (n0 - 2) - pp;
      j4p2 = j4 + 2 * pp - 1;
      z[j4 - 2] = dnm2 + z[j4p2];
      if (dnm2 < 0.0) return;
      z[j4] = z[j4p2 + 2] * (z[j4p2] / z[j4 - 2]);
      dnm1 = fma(z[j4p2 + 2], dnm2 / z[j4 - 2], -tau);
      dmin = fmin(dmin, dnm1);
      dmin1 = dmin;
      j4 += 4;
      j4p2 = j4 + 2 * pp - 1;
      z[j4 - 2] = dnm1 + z[j4p2];
      if (dnm1 < 0.0) return;
      z[j4] = z[j4p2 + 2] * (z[j4p2] / z[j4 - 2]);
      dn = fma(z[j4p2 + 2], dnm1 / z[j4 - 2], -tau);
      dmin = fmin(dmin, dn);
    }
  }
  z[j4 + 2] = dn;
  z[4 * n0 - pp] = emin;
}

// dlasq6.f (no FMA).
void dlasq6(int i0, int n0, double* z, int pp, double& dmin, double& dmin1, double& dmin2,
            double& dn, double& dnm1, double& dnm2) {
  --z;
  if ((n0 - i0 - 1) <= 0) return;
  const double safmin = dlamch('S');
  int j4 = 4 * i0 + pp - 3;
  double emin = z[j4 + 4];
  double d = z[j4];
  double temp;
  dmin = d;
  if (pp == 0) {
    for (j4 = 4 * i0; j4 <= 4 * (n0 - 3); j4 += 4) {
      z[j4 - 2] = d + z[j4 - 1];
      if (z[j4 - 2] == 0.0) {
        z[j4] = 0.0;
        d = z[j4 + 1];
        dmin = d;
        emin = 0.0;
      } else if (safmin * z[j4 + 1] < z[j4 - 2] && safmin * z[j4 - 2] < z[j4 + 1]) {
        temp = z[j4 + 1] / z[j4 - 2];
        z[j4] = z[j4 - 1] * temp;
        d = d * temp;
      } else {
        z[j4] = z[j4 + 1] * (z[j4 - 1] / z[j4 - 2]);
        d = z[j4 + 1] * (d / z[j4 - 2]);
      }
      dmin = fmin(dmin, d);
      emin = fmin(emin, z[j4]);
    }
  } else {
    for (j4 = 4 * i0; j4 <= 4 * (n0 - 3); j4 += 4) {
      z[j4 - 3] = d + z[j4];
      if (z[j4 - 3] == 0.0) {
        z[j4 - 1] = 0.0;
        d = z[j4 + 2];
        dmin = d;
        emin = 0.0;
      } else if (safmin * z[j4 + 2] < z[j4 - 3] && safmin * z[j4 - 3] < z[j4 + 2]) {
        temp = z[j4 + 2] / z[j4 - 3];
        z[j4 - 1] = z[j4] * temp;
        d = d * temp;
      } else {
        z[j4 - 1] = z[j4 + 2] * (z[j4] / z[j4 - 3]);
        d = z[j4 + 2] * (d / z[j4 - 3]);
      }
      dmin = fmin(dmin, d);
      emin = fmin(emin, z[j4 - 1]);
    }
  }
  dnm2 = d;
  dmin2 = dmin;
  j4 = 4 * (n0 - 2) - pp;
  int j4p2 = j4 + 2 * pp - 1;
  z[j4 - 2] = dnm2 + z[j4p2];
  if (z[j4 - 2] == 0.0) {
    z[j4] = 0.0;
    dnm1 = z[j4p2 + 2];
    dmin = dnm1;
    emin = 0.0;
  } else if (safmin * z[j4p2 + 2] < z[j4 - 2] && safmin * z[j4 - 2] < z[j4p2 + 2]) {
    temp = z[j4p2 + 2] / z[j4 - 2];
    z[j4] = z[j4p2] * temp;
    dnm1 = dnm2 * temp;
  } else {
    z[j4] = z[j4p2 + 2] * (z[j4p2] / z[j4 - 2]);
    dnm1 = z[j4p2 + 2] * (dnm2 / z[j4 - 2]);
  }
  dmin = fmin(dmin, dnm1);
  dmin1 = dmin;
  j4 += 4;
  j4p2 = j4 + 2 * pp - 1;
  z[j4 - 2] = dnm1 + z[j4p2];
  if (z[j4 - 2] == 0.0) {
    z[j4] = 0.0;
    dn = z[j4p2 + 2];
    dmin = dn;
    emin = 0.0;
  } else if (safmin * z[j4p2 + 2] < z[j4 - 2] && safmin * z[j4 - 2] < z[j4p2 + 2]) {
    temp = z[j4p2 + 2] / z[j4 - 2];
    z[j4] = z[j4p2] * temp;
    dn = dnm1 * temp;
  } else {
    z[j4] = z[j4p2 + 2] * (z[j4p2] / z[j4 - 2]);
    dn = z[j4p2 + 2] * (dnm1 / z[j4 - 2]);
  }
  dmin = fmin(dmin, dn);
  z[j4 + 2] = dn;
  z[4 * n0 - pp] = emin;
}

// dlasq3.f (2 fused: T + SQRT(T)*SQRT(T+S) and ONE - TWO*EPS).
void dlasq3(int i0, int& n0, double* z, int& pp, double& dmin, double& sigma, double& desig,
            double& qmax, int& nfail, int& iter, int& ndiv, bool ieee, int& ttype, double& dmin1,
            double& dmin2, double& dn, double& dn1, double& dn2, double& g, double& tau) {
  double* zz = z;  // Z(1) for the callees
  --z;
  const double cbias = 1.50, qurtr = 0.250, half = 0.5, one = 1.0, two = 2.0, hundrd = 100.0;
  const int n0in = n0;
  const double eps = dlamch('P');
  const double tol = eps * hundrd;
  const double tol2 = tol * tol;
  double s, t, temp;
  int nn;
  // label 10
  for (;;) {
    if (n0 < i0) return;
    bool deflate1 = false;  // GO TO 20 (otherwise GO TO 40)
    if (n0 == i0) {
      deflate1 = true;
    } else {
      nn = 4 * n0 + pp;
      if (n0 == i0 + 1) {
        // GO TO 40
      } else if (z[nn - 5] > tol2 * (sigma + z[nn - 3]) && z[nn - 2 * pp - 4] > tol2 * z[nn - 7]) {
        // label 30
        if (z[nn - 9] > tol2 * sigma && z[nn - 2 * pp - 8] > tol2 * z[nn - 11]) break;  // -> 50
      } else {
        deflate1 = true;
      }
    }
    if (deflate1) {
      // label 20
      z[4 * n0 - 3] = z[4 * n0 + pp - 3] + sigma;
      n0 = n0 - 1;
      continue;
    }
    // label 40
    nn = 4 * n0 + pp;
    if (z[nn - 3] > z[nn - 7]) {
      s = z[nn - 3];
      z[nn - 3] = z[nn - 7];
      z[nn - 7] = s;
    }
    t = half * ((z[nn - 7] - z[nn - 3]) + z[nn - 5]);
    if (z[nn - 5] > z[nn - 3] * tol2 && t != 0.0) {
      s = z[nn - 3] * (z[nn - 5] / t);
      if (s <= t) {
        s = z[nn - 3] * (z[nn - 5] / (t * (one + sqrt(one + s / t))));
      } else {
        s = z[nn - 3] * (z[nn - 5] / fma(sqrt(t), sqrt(t + s), t));
      }
      t = z[nn - 7] + (s + z[nn - 5]);
      z[nn - 3] = z[nn - 3] * (z[nn - 7] / t);
      z[nn - 7] = t;
    }
    z[4 * n0 - 7] = z[nn - 7] + sigma;
    z[4 * n0 - 3] = z[nn - 3] + sigma;
    n0 = n0 - 2;
  }
  // label 50
  if (pp == 2) pp = 0;
  if (dmin <= 0.0 || n0 < n0in) {
    if (cbias * z[4 * i0 + pp - 3] < z[4 * n0 + pp - 3]) {
      const int ipn4 = 4 * (i0 + n0);
      for (int j4 = 4 * i0; j4 <= 2 * (i0 + n0 - 1); j4 += 4) {
        temp = z[j4 - 3]; z[j4 - 3] = z[ipn4 - j4 - 3]; z[ipn4 - j4 - 3] = temp;
        temp = z[j4 - 2]; z[j4 - 2] = z[ipn4 - j4 - 2]; z[ipn4 - j4 - 2] = temp;
        temp = z[j4 - 1]; z[j4 - 1] = z[ipn4 - j4 - 5]; z[ipn4 - j4 - 5] = temp;
        temp = z[j4]; z[j4] = z[ipn4 - j4 - 4]; z[ipn4 - j4 - 4] = temp;
      }
      if (n0 - i0 <= 4) {
        z[4 * n0 + pp - 1] = z[4 * i0 + pp - 1];
        z[4 * n0 - pp] = z[4 * i0 - pp];
      }
      dmin2 = fmin(dmin2, z[4 * n0 + pp - 1]);
      z[4 * n0 + pp - 1] = fmin(fmin(z[4 * n0 + pp - 1], z[4 * i0 + pp - 1]), z[4 * i0 + pp + 3]);
      z[4 * n0 - pp] = fmin(fmin(z[4 * n0 - pp], z[4 * i0 - pp]), z[4 * i0 - pp + 4]);
      qmax = fmax(fmax(qmax, z[4 * i0 + pp - 3]), z[4 * i0 + pp + 1]);
      dmin = -0.0;
    }
  }
  dlasq4(i0, n0, zz, pp, n0in, dmin, dmin1, dmin2, dn, dn1, dn2, tau, ttype, g);
  for (;;) {
    // label 70
    dlasq5(i0, n0, zz, pp, tau, sigma, dmin, dmin1, dmin2, dn, dn1, dn2, ieee, eps);
    ndiv = ndiv + (n0 - i0 + 2);
    iter = iter + 1;
    if (dmin >= 0.0 && dmin1 >= 0.0) {
      break;  // -> 90
    } else if (dmin < 0.0 && dmin1 > 0.0 && z[4 * (n0 - 1) - pp] < tol * (sigma + dn1) &&
               fabs(dn) < tol * sigma) {
      z[4 * (n0 - 1) - pp + 2] = 0.0;
      dmin = 0.0;
      break;  // -> 90
    } else if (dmin < 0.0) {
      nfail = nfail + 1;
      if (ttype < -22) {
        tau = 0.0;
      } else if (dmin1 > 0.0) {
        tau = (tau + dmin) * fma(-two, eps, one);
        ttype = ttype - 11;
      } else {
        tau = qurtr * tau;
        ttype = ttype - 12;
      }
      continue;  // -> 70
    } else if (disnan(dmin)) {
      if (tau == 0.0) {
        // -> 80
      } else {
        tau = 0.0;
        continue;  // -> 70
      }
    }
    // label 80
    dlasq6(i0, n0, zz, pp, dmin, dmin1, dmin2, dn, dn1, dn2);
    ndiv = ndiv + (n0 - i0 + 2);
    iter = iter + 1;
    tau = 0.0;
    break;
  }
  // label 90
  if (tau < sigma) {
    desig = desig + tau;
    t = sigma + desig;
    desig = desig - (t - sigma);
  } else {
    t = sigma + tau;
    desig = sigma - (t - tau) + desig;
  }
  sigma = t;
}

// dlasq2.f (2 fused: T + SQRT(T)*SQRT(T+S) and QMIN - TWO*SQRT(QMIN)*SQRT(EMAX)).
int dlasq2(int n, double* z) {
  double* zz = z;
  --z;
  const double cbias = 1.50, half = 0.5, one = 1.0, two = 2.0, four = 4.0, hundrd = 100.0;
  const double eps = dlamch('P');
  const double safmin = dlamch('S');
  const double tol = eps * hundrd;
  const double tol2 = tol * tol;
  double d, e, s, t, temp;
  if (n < 0) return -1;
  if (n == 0) return 0;
  if (n == 1) {
    if (z[1] < 0.0) return -201;
    return 0;
  }
  if (n == 2) {
    if (z[1] < 0.0) return -201;
    if (z[2] < 0.0) return -202;
    if (z[3] < 0.0) return -203;
    if (z[3] > z[1]) {
      d = z[3];
      z[3] = z[1];
      z[1] = d;
    }
    z[5] = z[1] + z[2] + z[3];
    if (z[2] > z[3] * tol2) {
      t = half * ((z[1] - z[3]) + z[2]);
      s = z[3] * (z[2] / t);
      if (s <= t) s = z[3] * (z[2] / (t * (one + sqrt(one + s / t))));
      else s = z[3] * (z[2] / fma(sqrt(t), sqrt(t + s), t));
      t = z[1] + (s + z[2]);
      z[3] = z[3] * (z[1] / t);
      z[1] = t;
    }
    z[2] = z[3];
    z[6] = z[2] + z[1];
    return 0;
  }
  z[2 * n] = 0.0;
  double emin = z[2];
  double qmax = 0.0, zmax = 0.0;
  d = 0.0;
  e = 0.0;
  for (int k = 1; k <= 2 * (n - 1); k += 2) {
    if (z[k] < 0.0) return -(200 + k);
    if (z[k + 1] < 0.0) return -(200 + k + 1);
    d = d + z[k];
    e = e + z[k + 1];
    qmax = fmax(qmax, z[k]);
    emin = fmin(emin, z[k + 1]);
    zmax = fmax(fmax(qmax, zmax), z[k + 1]);
  }
  if (z[2 * n - 1] < 0.0) return -(200 + 2 * n - 1);
  d = d + z[2 * n - 1];
  qmax = fmax(qmax, z[2 * n - 1]);
  zmax = fmax(qmax, zmax);
  if (e == 0.0) {
    for (int k = 2; k <= n; ++k) z[k] = z[2 * k - 1];
    dlasrt('D', n, zz);
    z[2 * n - 1] = d;
    return 0;
  }
  const double trace = d + e;
  if (trace == 0.0) {
    z[2 * n - 1] = 0.0;
    return 0;
  }
  const bool ieee = ilaenv(10, "DLASQ2") == 1;
  for (int k = 2 * n; k >= 2; k -= 2) {
    z[2 * k] = 0.0;
    z[2 * k - 1] = z[k];
    z[2 * k - 2] = 0.0;
    z[2 * k - 3] = z[k - 1];
  }
  int i0 = 1, n0 = n;
  if (cbias * z[4 * i0 - 3] < z[4 * n0 - 3]) {
    const int ipn4 = 4 * (i0 + n0);
    for (int i4 = 4 * i0; i4 <= 2 * (i0 + n0 - 1); i4 += 4) {
      temp = z[i4 - 3]; z[i4 - 3] = z[ipn4 - i4 - 3]; z[ipn4 - i4 - 3] = temp;
      temp = z[i4 - 1]; z[i4 - 1] = z[ipn4 - i4 - 5]; z[ipn4 - i4 - 5] = temp;
    }
  }
  int pp = 0;
  for (int k = 1; k <= 2; ++k) {
    d = z[4 * n0 + pp - 3];
    for (int i4 = 4 * (n0 - 1) + pp; i4 >= 4 * i0 + pp; i4 -= 4) {
      if (z[i4 - 1] <= tol2 * d) {
        z[i4 - 1] = -0.0;
        d = z[i4 - 3];
      } else {
        d = z[i4 - 3] * (d / (d + z[i4 - 1]));
      }
    }
    emin = z[4 * i0 + pp + 1];
    d = z[4 * i0 + pp - 3];
    for (int i4 = 4 * i0 + pp; i4 <= 4 * (n0 - 1) + pp; i4 += 4) {
      z[i4 - 2 * pp - 2] = d + z[i4 - 1];
      if (z[i4 - 1] <= tol2 * d) {
        z[i4 - 1] = -0.0;
        z[i4 - 2 * pp - 2] = d;
        z[i4 - 2 * pp] = 0.0;
        d = z[i4 + 1];
      } else if (safmin * z[i4 + 1] < z[i4 - 2 * pp - 2] && safmin * z[i4 - 2 * pp - 2] < z[i4 + 1]) {
        temp = z[i4 + 1] / z[i4 - 2 * pp - 2];
        z[i4 - 2 * pp] = z[i4 - 1] * temp;
        d = d * temp;
      } else {
        z[i4 - 2 * pp] = z[i4 + 1] * (z[i4 - 1] / z[i4 - 2 * pp - 2]);
        d = z[i4 + 1] * (d / z[i4 - 2 * pp - 2]);
      }
      emin = fmin(emin, z[i4 - 2 * pp]);
    }
    z[4 * n0 - pp - 2] = d;
    qmax = z[4 * i0 - pp - 2];
    for (int i4 = 4 * i0 - pp + 2; i4 <= 4 * n0 - pp - 2; i4 += 4) qmax = fmax(qmax, z[i4]);
    pp = 1 - pp;
  }
  int ttype = 0;
  double dmin1 = 0.0, dmin2 = 0.0, dn = 0.0, dn1 = 0.0, dn2 = 0.0, g = 0.0, tau = 0.0;
  int iter = 2, nfail = 0;
  int ndiv = 2 * (n0 - i0);
  double sigma = 0.0, desig, dmin, qmin, emax, oldemn, dee, deemin;
  bool finished = false;  // GO TO 170
  for (int iwhila = 1; iwhila <= n + 1; ++iwhila) {
    if (n0 < 1) {
      finished = true;
      break;
    }
    desig = 0.0;
    if (n0 == n) sigma = 0.0;
    else sigma = -z[4 * n0 - 1];
    if (sigma < 0.0) return 1;
    emax = 0.0;
    if (n0 > i0) emin = fabs(z[4 * n0 - 5]);
    else emin = 0.0;
    qmin = z[4 * n0 - 3];
    qmax = qmin;
    int i4;
    bool hit = false;
    for (i4 = 4 * n0; i4 >= 8; i4 -= 4) {
      if (z[i4 - 5] <= 0.0) {
        hit = true;
        break;
      }
      if (qmin >= four * emax) {
        qmin = fmin(qmin, z[i4 - 3]);
        emax = fmax(emax, z[i4 - 5]);
      }
      qmax = fmax(qmax, z[i4 - 7] + z[i4 - 5]);
      emin = fmin(emin, z[i4 - 5]);
    }
    if (!hit) i4 = 4;
    // label 100
    i0 = i4 / 4;
    pp = 0;
    if (n0 - i0 > 1) {
      dee = z[4 * i0 - 3];
      deemin = dee;
      int kmin = i0;
      for (int j = 4 * i0 + 1; j <= 4 * n0 - 3; j += 4) {
        dee = z[j] * (dee / (dee + z[j - 2]));
        if (dee <= deemin) {
          deemin = dee;
          kmin = (j + 3) / 4;
        }
      }
      if ((kmin - i0) * 2 < n0 - kmin && deemin <= half * z[4 * n0 - 3]) {
        const int ipn4 = 4 * (i0 + n0);
        pp = 2;
        for (int j = 4 * i0; j <= 2 * (i0 + n0 - 1); j += 4) {
          temp = z[j - 3]; z[j - 3] = z[ipn4 - j - 3]; z[ipn4 - j - 3] = temp;
          temp = z[j - 2]; z[j - 2] = z[ipn4 - j - 2]; z[ipn4 - j - 2] = temp;
          temp = z[j - 1]; z[j - 1] = z[ipn4 - j - 5]; z[ipn4 - j - 5] = temp;
          temp = z[j]; z[j] = z[ipn4 - j - 4]; z[ipn4 - j - 4] = temp;
        }
      }
    }
    dmin = -fmax(0.0, fma(-(two * sqrt(qmin)), sqrt(emax), qmin));
    const int nbig = 100 * (n0 - i0 + 1);
    bool exited = false;  // GO TO 150
    for (int iwhilb = 1; iwhilb <= nbig; ++iwhilb) {
      if (i0 > n0) {
        exited = true;
        break;
      }
      dlasq3(i0, n0, zz, pp, dmin, sigma, desig, qmax, nfail, iter, ndiv, ieee, ttype, dmin1,
             dmin2, dn, dn1, dn2, g, tau);
      pp = 1 - pp;
      if (pp == 0 && n0 - i0 >= 3) {
        if (z[4 * n0] <= tol2 * qmax || z[4 * n0 - 1] <= tol2 * sigma) {
          int splt = i0 - 1;
          qmax = z[4 * i0 - 3];
          emin = z[4 * i0 - 1];
          oldemn = z[4 * i0];
          for (int j = 4 * i0; j <= 4 * (n0 - 3); j += 4) {
            if (z[j] <= tol2 * z[j - 3] || z[j - 1] <= tol2 * sigma) {
              z[j - 1] = -sigma;
              splt = j / 4;
              qmax = 0.0;
              emin = z[j + 3];
              oldemn = z[j + 4];
            } else {
              qmax = fmax(qmax, z[j + 1]);
              emin = fmin(emin, z[j - 1]);
              oldemn = fmin(oldemn, z[j]);
            }
          }
          z[4 * n0 - 1] = emin;
          z[4 * n0] = oldemn;
          i0 = splt + 1;
        }
      }
    }
    if (exited) continue;  // label 150 -> next IWHILA
    // INFO = 2: the inner loop ran out (restore the unconverged blocks).
    int i1 = i0, n1 = n0;
    for (;;) {
      double tempq = z[4 * i0 - 3];
      z[4 * i0 - 3] = z[4 * i0 - 3] + sigma;
      for (int k = i0 + 1; k <= n0; ++k) {
        const double tempe = z[4 * k - 5];
        z[4 * k - 5] = z[4 * k - 5] * (tempq / z[4 * k - 7]);
        tempq = z[4 * k - 3];
        z[4 * k - 3] = z[4 * k - 3] + sigma + tempe - z[4 * k - 5];
      }
      if (i1 > 1) {
        n1 = i1 - 1;
        while (i1 >= 2 && z[4 * i1 - 5] >= 0.0) i1 = i1 - 1;
        sigma = -z[4 * n1 - 1];
        continue;
      }
      break;
    }
    for (int k = 1; k <= n; ++k) {
      z[2 * k - 1] = z[4 * k - 3];
      if (k < n0) z[2 * k] = z[4 * k - 1];
      else z[2 * k] = 0;
    }
    return 2;
  }
  if (!finished) return 3;
  // label 170
  for (int k = 2; k <= n; ++k) z[k] = z[4 * k - 3];
  dlasrt('D', n, zz);
  e = 0.0;
  for (int k = n; k >= 1; --k) e = e + z[k];
  z[2 * n + 1] = trace;
  z[2 * n + 2] = e;
  z[2 * n + 3] = double(iter);
  z[2 * n + 4] = double(ndiv) / double(n * n);
  z[2 * n + 5] = hundrd * nfail / double(iter);
  return 0;
}

// dlarre.f, RANGE='A' (IRANGE = ALLRNG, FORCEB = .FALSE., so USEDQD and the
// DLASQ2 branch; the DLARRD/DLARRB branch is RANGE='V'/'I' only). 17 FMA
// sites in the dylib; the ones on this path are marked.
int dlarreA(int n, double& vl, double& vu, double* d, double* e, double* e2, double rtol1,
            double rtol2, double spltol, int& nsplit, int* isplit, int& m, double* w,
            double* werr, double* wgap, int* iblock, int* indexw, double* gers, double& pivmin,
            double* work, int* iwork) {
  double* d1 = d;
  double* e21 = e2;
  double* work1 = work;
  --d; --e; --e2; --isplit; --w; --werr; --wgap; --iblock; --indexw; --gers; --work;
  (void)rtol1; (void)rtol2; (void)iwork;
  const double fac = 0.5, four = 4.0, fourth = 1.0 / 4.0, fudge = 2.0, half = 0.5,
               hndrd = 100.0, maxgrowth = 64.0, one = 1.0, pert = 8.0, two = 2.0;
  const int maxtry = 6;
  (void)fac;
  nsplit = 0;
  m = 0;
  if (n <= 0) return 0;
  const double safmin = dlamch('S');
  const double eps = dlamch('P');
  const double rtl = sqrt(eps);
  if (n == 1) {
    m = 1;
    w[1] = d[1];
    werr[1] = 0.0;
    wgap[1] = 0.0;
    iblock[1] = 1;
    indexw[1] = 1;
    gers[1] = d[1];
    gers[2] = d[1];
    e[1] = 0.0;
    return 0;
  }
  double gl = d[1], gu = d[1], eold = 0.0, emax = 0.0;
  e[n] = 0.0;
  for (int i = 1; i <= n; ++i) {
    werr[i] = 0.0;
    wgap[i] = 0.0;
    const double eabs = fabs(e[i]);
    if (eabs >= emax) emax = eabs;
    const double tmp1 = eabs + eold;
    gers[2 * i - 1] = d[i] - tmp1;
    gl = fmin(gl, gers[2 * i - 1]);
    gers[2 * i] = d[i] + tmp1;
    gu = fmax(gu, gers[2 * i]);
    eold = eabs;
  }
  pivmin = safmin * fmax(one, emax * emax);
  double spdiam = gu - gl;
  dlarra(n, d1, e + 1, e21, spltol, spdiam, nsplit, isplit + 1);
  // IRANGE = ALLRNG and .NOT.FORCEB:
  vl = gl;
  vu = gu;
  int ibegin = 1, wbegin = 1;
  for (int jblk = 1; jblk <= nsplit; ++jblk) {
    const int iend = isplit[jblk];
    const int in = iend - ibegin + 1;
    if (in == 1) {
      m = m + 1;
      w[m] = d[ibegin];
      werr[m] = 0.0;
      wgap[m] = 0.0;
      iblock[m] = jblk;
      indexw[m] = 1;
      wbegin = wbegin + 1;
      e[iend] = 0.0;
      ibegin = iend + 1;
      continue;
    }
    e[iend] = 0.0;
    gl = d[ibegin];
    gu = d[ibegin];
    for (int i = ibegin; i <= iend; ++i) {
      gl = fmin(gers[2 * i - 1], gl);
      gu = fmax(gers[2 * i], gu);
    }
    spdiam = gu - gl;
    double tmp, tmp1;
    if (dlarrk(in, 1, gl, gu, &d[ibegin], &e2[ibegin], pivmin, rtl, tmp, tmp1) != 0) return -1;
    const double isleft = fmax(gl, fma(-(hndrd * eps), fabs(tmp - tmp1), tmp - tmp1));  // fused
    if (dlarrk(in, in, gl, gu, &d[ibegin], &e2[ibegin], pivmin, rtl, tmp, tmp1) != 0) return -1;
    const double isrght = fmin(gu, fma(hndrd * eps, fabs(tmp + tmp1), tmp + tmp1));  // fused
    spdiam = isrght - isleft;
    const int indl = 1, indu = in, mb = in;
    const int wend = wbegin + mb - 1;
    const double s1 = fma(fourth, spdiam, isleft);    // fused
    const double s2 = fma(-fourth, spdiam, isrght);   // fused
    int cnt = 0, cnt1 = 0, cnt2 = 0;
    if (mb > 1) dlarrcT(in, s1, s2, &d[ibegin], &e[ibegin], cnt, cnt1, cnt2);
    double sigma, sgndef;
    if (mb == 1) {
      sigma = gl;
      sgndef = one;
    } else if (cnt1 - indl >= indu - cnt2) {
      sigma = fmax(isleft, gl);
      sgndef = one;
    } else {
      sigma = fmin(isrght, gu);
      sgndef = -one;
    }
    // USEDQD
    double tau = fma(spdiam * eps, double(n), two * pivmin);  // fused
    tau = fmax(tau, two * eps * fabs(sigma));
    bool found = false;
    for (int idum = 1; idum <= maxtry; ++idum) {
      double dpivot = d[ibegin] - sigma;
      work[1] = dpivot;
      double dmax = fabs(work[1]);
      int j = ibegin;
      for (int i = 1; i <= in - 1; ++i) {
        work[2 * in + i] = one / work[i];
        tmp = e[j] * work[2 * in + i];
        work[in + i] = tmp;
        dpivot = fma(-tmp, e[j], d[j + 1] - sigma);  // fused
        work[i + 1] = dpivot;
        dmax = fmax(dmax, fabs(dpivot));
        j = j + 1;
      }
      bool norep = dmax > maxgrowth * spdiam;
      if (!norep) {
        for (int i = 1; i <= in; ++i) {
          tmp = sgndef * work[i];
          if (tmp < 0.0) norep = true;
        }
      }
      if (norep) {
        if (idum == maxtry - 1) {
          const double t = fudge * spdiam * eps;
          if (sgndef == one) sigma = fma(-t, double(n), gl) - fudge * two * pivmin;   // fused
          else sigma = fma(t, double(n), gu) + fudge * two * pivmin;                  // fused
        } else {
          sigma = fma(-sgndef, tau, sigma);  // fused
          tau = two * tau;
        }
      } else {
        found = true;
        break;
      }
    }
    if (!found) return 2;
    // label 83
    e[iend] = sigma;
    dcopy(in, work1, 1, &d[ibegin], 1);
    dcopy(in - 1, &work[in + 1], 1, &e[ibegin], 1);
    if (mb > 1) {
      int iseed[4] = {1, 1, 1, 1};
      dlarnv2(iseed, 2 * in - 1, work1);
      for (int i = 1; i <= in - 1; ++i) {
        d[ibegin + i - 1] = d[ibegin + i - 1] * fma(eps * pert, work[i], one);       // fused
        e[ibegin + i - 1] = e[ibegin + i - 1] * fma(eps * pert, work[in + i], one);  // fused
      }
      d[iend] = d[iend] * fma(eps * four, work[in], one);  // fused
    }
    // USEDQD: dqds on the (perturbed) block.
    const double rtol = std::log(double(in)) * four * eps;
    int j = ibegin;
    for (int i = 1; i <= in - 1; ++i) {
      work[2 * i - 1] = fabs(d[j]);
      work[2 * i] = e[j] * e[j] * work[2 * i - 1];
      j = j + 1;
    }
    work[2 * in - 1] = fabs(d[iend]);
    work[2 * in] = 0.0;
    if (dlasq2(in, work1) != 0) return -5;
    for (int i = 1; i <= in; ++i)
      if (work[i] < 0.0) return -6;
    if (sgndef > 0.0) {
      for (int i = indl; i <= indu; ++i) {
        m = m + 1;
        w[m] = work[in - i + 1];
        iblock[m] = jblk;
        indexw[m] = i;
      }
    } else {
      for (int i = indl; i <= indu; ++i) {
        m = m + 1;
        w[m] = -work[i];
        iblock[m] = jblk;
        indexw[m] = i;
      }
    }
    for (int i = m - mb + 1; i <= m; ++i) werr[i] = rtol * fabs(w[i]);
    for (int i = m - mb + 1; i <= m - 1; ++i)
      wgap[i] = fmax(0.0, w[i + 1] - werr[i + 1] - (w[i] + werr[i]));
    wgap[m] = fmax(0.0, (vu - sigma) - (w[m] + werr[m]));
    ibegin = iend + 1;
    wbegin = wend + 1;
  }
  (void)half;
  return 0;
}

// dlar1v.f (6 fused: WORK(INDP+I)*TMP - LAMBDA twice, ZTZ + Z*Z four times).
void dlar1v(int n, int b1, int bn, double lambda, const double* d, const double* l,
            const double* ld, const double* lld, double pivmin, double gaptol, double* z,
            bool wantnc, int& negcnt, double& ztz, double& mingma, int& r, int* isuppz,
            double& nrminv, double& resid, double& rqcorr, double* work) {
  --d; --l; --ld; --lld; --z; --isuppz; --work;
  const double eps = dlamch('P');
  int r1, r2;
  if (r == 0) {
    r1 = b1;
    r2 = bn;
  } else {
    r1 = r;
    r2 = r;
  }
  const int indlpl = 0, indumn = n, inds = 2 * n + 1, indp = 3 * n + 1;
  if (b1 == 1) work[inds] = 0.0;
  else work[inds + b1 - 1] = lld[b1 - 1];
  bool sawnan1 = false;
  int neg1 = 0;
  double s = work[inds + b1 - 1] - lambda;
  double dplus, dminus, tmp;
  for (int i = b1; i <= r1 - 1; ++i) {
    dplus = d[i] + s;
    work[indlpl + i] = ld[i] / dplus;
    if (dplus < 0.0) ++neg1;
    work[inds + i] = s * work[indlpl + i] * l[i];
    s = work[inds + i] - lambda;
  }
  sawnan1 = disnan(s);
  if (!sawnan1) {
    for (int i = r1; i <= r2 - 1; ++i) {
      dplus = d[i] + s;
      work[indlpl + i] = ld[i] / dplus;
      work[inds + i] = s * work[indlpl + i] * l[i];
      s = work[inds + i] - lambda;
    }
    sawnan1 = disnan(s);
  }
  if (sawnan1) {
    neg1 = 0;
    s = work[inds + b1 - 1] - lambda;
    for (int i = b1; i <= r1 - 1; ++i) {
      dplus = d[i] + s;
      if (fabs(dplus) < pivmin) dplus = -pivmin;
      work[indlpl + i] = ld[i] / dplus;
      if (dplus < 0.0) ++neg1;
      work[inds + i] = s * work[indlpl + i] * l[i];
      if (work[indlpl + i] == 0.0) work[inds + i] = lld[i];
      s = work[inds + i] - lambda;
    }
    for (int i = r1; i <= r2 - 1; ++i) {
      dplus = d[i] + s;
      if (fabs(dplus) < pivmin) dplus = -pivmin;
      work[indlpl + i] = ld[i] / dplus;
      work[inds + i] = s * work[indlpl + i] * l[i];
      if (work[indlpl + i] == 0.0) work[inds + i] = lld[i];
      s = work[inds + i] - lambda;
    }
  }
  bool sawnan2 = false;
  int neg2 = 0;
  work[indp + bn - 1] = d[bn] - lambda;
  for (int i = bn - 1; i >= r1; --i) {
    dminus = lld[i] + work[indp + i];
    tmp = d[i] / dminus;
    if (dminus < 0.0) ++neg2;
    work[indumn + i] = l[i] * tmp;
    work[indp + i - 1] = fma(work[indp + i], tmp, -lambda);  // fused
  }
  tmp = work[indp + r1 - 1];
  sawnan2 = disnan(tmp);
  if (sawnan2) {
    neg2 = 0;
    for (int i = bn - 1; i >= r1; --i) {
      dminus = lld[i] + work[indp + i];
      if (fabs(dminus) < pivmin) dminus = -pivmin;
      tmp = d[i] / dminus;
      if (dminus < 0.0) ++neg2;
      work[indumn + i] = l[i] * tmp;
      work[indp + i - 1] = fma(work[indp + i], tmp, -lambda);  // fused
      if (tmp == 0.0) work[indp + i - 1] = d[i] - lambda;
    }
  }
  mingma = work[inds + r1 - 1] + work[indp + r1 - 1];
  if (mingma < 0.0) ++neg1;
  if (wantnc) negcnt = neg1 + neg2;
  else negcnt = -1;
  if (fabs(mingma) == 0.0) mingma = eps * work[inds + r1 - 1];
  r = r1;
  for (int i = r1; i <= r2 - 1; ++i) {
    tmp = work[inds + i] + work[indp + i];
    if (tmp == 0.0) tmp = eps * work[inds + i];
    if (fabs(tmp) <= fabs(mingma)) {
      mingma = tmp;
      r = i + 1;
    }
  }
  isuppz[1] = b1;
  isuppz[2] = bn;
  z[r] = 1.0;
  ztz = 1.0;
  if (!sawnan1 && !sawnan2) {
    for (int i = r - 1; i >= b1; --i) {
      z[i] = -(work[indlpl + i] * z[i + 1]);
      if ((fabs(z[i]) + fabs(z[i + 1])) * fabs(ld[i]) < gaptol) {
        z[i] = 0.0;
        isuppz[1] = i + 1;
        break;
      }
      ztz = fma(z[i], z[i], ztz);  // fused
    }
  } else {
    for (int i = r - 1; i >= b1; --i) {
      if (z[i + 1] == 0.0) z[i] = -(ld[i + 1] / ld[i]) * z[i + 2];
      else z[i] = -(work[indlpl + i] * z[i + 1]);
      if ((fabs(z[i]) + fabs(z[i + 1])) * fabs(ld[i]) < gaptol) {
        z[i] = 0.0;
        isuppz[1] = i + 1;
        break;
      }
      ztz = fma(z[i], z[i], ztz);  // fused
    }
  }
  if (!sawnan1 && !sawnan2) {
    for (int i = r; i <= bn - 1; ++i) {
      z[i + 1] = -(work[indumn + i] * z[i]);
      if ((fabs(z[i]) + fabs(z[i + 1])) * fabs(ld[i]) < gaptol) {
        z[i + 1] = 0.0;
        isuppz[2] = i;
        break;
      }
      ztz = fma(z[i + 1], z[i + 1], ztz);  // fused
    }
  } else {
    for (int i = r; i <= bn - 1; ++i) {
      if (z[i] == 0.0) z[i + 1] = -(ld[i - 1] / ld[i]) * z[i - 1];
      else z[i + 1] = -(work[indumn + i] * z[i]);
      if ((fabs(z[i]) + fabs(z[i + 1])) * fabs(ld[i]) < gaptol) {
        z[i + 1] = 0.0;
        isuppz[2] = i;
        break;
      }
      ztz = fma(z[i + 1], z[i + 1], ztz);  // fused
    }
  }
  tmp = 1.0 / ztz;
  nrminv = sqrt(tmp);
  resid = fabs(mingma) * nrminv;
  rqcorr = mingma * tmp;
}

// dlarrf.f (7 fused sites, marked; LDMAX and RDMAX are the same expression).
int dlarrf(int n, const double* d, const double* l, const double* ld, int clstrt, int clend,
           const double* w, const double* wgap, const double* werr, double spdiam, double clgapl,
           double clgapr, double pivmin, double& sigma, double* dplus, double* lplus,
           double* work) {
  double* dplus1 = dplus;
  double* lplus1 = lplus;
  double* work1 = work;
  --d; --l; --ld; --w; --wgap; --werr; --dplus; --lplus; --work;
  const double four = 4.0, maxgrowth1 = 8.0, maxgrowth2 = 8.0, one = 1.0, quart = 0.25, two = 2.0;
  const int ktrymax = 1, sleft = 1, sright = 2;
  if (n <= 0) return 0;
  const double fact = double(1 << ktrymax);
  const double eps = dlamch('P');
  int shift = 0;
  bool forcer = false;
  const bool nofail = false;
  const double clwdth = fabs(w[clend] - w[clstrt]) + werr[clend] + werr[clstrt];
  const double avgap = clwdth / double(clend - clstrt);
  const double mingap = fmin(clgapl, clgapr);
  double lsigma = fmin(w[clstrt], w[clend]) - werr[clstrt];
  double rsigma = fmax(w[clstrt], w[clend]) + werr[clend];
  lsigma = fma(-(fabs(lsigma) * four), eps, lsigma);  // fused
  rsigma = fma(fabs(rsigma) * four, eps, rsigma);     // fused
  const double ldmax = fma(quart, mingap, two * pivmin);  // fused
  const double rdmax = fma(quart, mingap, two * pivmin);  // fused (same site)
  double ldelta = fmax(avgap, wgap[clstrt]) / fact;
  double rdelta = fmax(avgap, wgap[clend - 1]) / fact;
  double s = dlamch('S');
  double smlgrowth = one / s;
  const double fail = double(n - 1) * mingap / (spdiam * eps);
  const double fail2 = double(n - 1) * mingap / (spdiam * sqrt(eps));
  double bestshift = lsigma;
  int ktry = 0;
  const double growthbound = maxgrowth1 * spdiam;
  int indx = 0;
  for (;;) {
    // label 5
    bool sawnan1 = false, sawnan2 = false;
    ldelta = fmin(ldmax, ldelta);
    rdelta = fmin(rdmax, rdelta);
    s = -lsigma;
    dplus[1] = d[1] + s;
    if (fabs(dplus[1]) < pivmin) {
      dplus[1] = -pivmin;
      sawnan1 = true;
    }
    double max1 = fabs(dplus[1]);
    for (int i = 1; i <= n - 1; ++i) {
      lplus[i] = ld[i] / dplus[i];
      s = fma(s * lplus[i], l[i], -lsigma);  // fused
      dplus[i + 1] = d[i + 1] + s;
      if (fabs(dplus[i + 1]) < pivmin) {
        dplus[i + 1] = -pivmin;
        sawnan1 = true;
      }
      max1 = fmax(max1, fabs(dplus[i + 1]));
    }
    sawnan1 = sawnan1 || disnan(max1);
    if (forcer || (max1 <= growthbound && !sawnan1)) {
      sigma = lsigma;
      shift = sleft;
      break;  // -> 100
    }
    s = -rsigma;
    work[1] = d[1] + s;
    if (fabs(work[1]) < pivmin) {
      work[1] = -pivmin;
      sawnan2 = true;
    }
    double max2 = fabs(work[1]);
    for (int i = 1; i <= n - 1; ++i) {
      work[n + i] = ld[i] / work[i];
      s = fma(s * work[n + i], l[i], -rsigma);  // fused
      work[i + 1] = d[i + 1] + s;
      if (fabs(work[i + 1]) < pivmin) {
        work[i + 1] = -pivmin;
        sawnan2 = true;
      }
      max2 = fmax(max2, fabs(work[i + 1]));
    }
    sawnan2 = sawnan2 || disnan(max2);
    if (forcer || (max2 <= growthbound && !sawnan2)) {
      sigma = rsigma;
      shift = sright;
      break;  // -> 100
    }
    bool to50 = false;
    if (sawnan1 && sawnan2) {
      to50 = true;
    } else {
      if (!sawnan1) {
        indx = 1;
        if (max1 <= smlgrowth) {
          smlgrowth = max1;
          bestshift = lsigma;
        }
      }
      if (!sawnan2) {
        if (sawnan1 || max2 <= max1) indx = 2;
        if (max2 <= smlgrowth) {
          smlgrowth = max2;
          bestshift = rsigma;
        }
      }
    }
    if (!to50) {
      const bool dorrr1 = clwdth < mingap / double(128) && fmin(max1, max2) < fail2 && !sawnan1 &&
                          !sawnan2;
      const bool tryrrr1 = true;
      if (tryrrr1 && dorrr1) {
        if (indx == 1) {
          double tmp = fabs(dplus[n]);
          double znm2 = one, prod = one, oldp = one;
          for (int i = n - 1; i >= 1; --i) {
            if (prod <= eps) prod = ((dplus[i + 1] * work[n + i + 1]) / (dplus[i] * work[n + i])) * oldp;
            else prod = prod * fabs(work[n + i]);
            oldp = prod;
            znm2 = fma(prod, prod, znm2);  // fused
            tmp = fmax(tmp, fabs(dplus[i] * prod));
          }
          const double rrr1 = tmp / (spdiam * sqrt(znm2));
          if (rrr1 <= maxgrowth2) {
            sigma = lsigma;
            shift = sleft;
            break;  // -> 100
          }
        } else if (indx == 2) {
          double tmp = fabs(work[n]);
          double znm2 = one, prod = one, oldp = one;
          for (int i = n - 1; i >= 1; --i) {
            if (prod <= eps) prod = ((work[i + 1] * lplus[i + 1]) / (work[i] * lplus[i])) * oldp;
            else prod = prod * fabs(lplus[i]);
            oldp = prod;
            znm2 = fma(prod, prod, znm2);  // fused
            tmp = fmax(tmp, fabs(work[i] * prod));
          }
          const double rrr2 = tmp / (spdiam * sqrt(znm2));
          if (rrr2 <= maxgrowth2) {
            sigma = rsigma;
            shift = sright;
            break;  // -> 100
          }
        }
      }
    }
    // label 50
    if (ktry < ktrymax) {
      lsigma = fmax(lsigma - ldelta, lsigma - ldmax);
      rsigma = fmin(rsigma + rdelta, rsigma + rdmax);
      ldelta = two * ldelta;
      rdelta = two * rdelta;
      ++ktry;
      continue;
    }
    if (smlgrowth < fail || nofail) {
      lsigma = bestshift;
      rsigma = bestshift;
      forcer = true;
      continue;
    }
    return 1;
  }
  // label 100
  if (shift == sright) {
    dcopy(n, work1, 1, dplus1, 1);
    dcopy(n - 1, work1 + n, 1, lplus1, 1);
  }
  return 0;
}

// dlarrv.f (1 fused site: FUDGE = THREE*EPS*|W| + FOUR*EPS*|W-TAU|, the first
// product fused).
int dlarrv(int n, double vl, double vu, double* d, double* l, double pivmin, const int* isplit,
           int m, int dol, int dou, double minrgp, double& rtol1, double& rtol2, double* w,
           double* werr, double* wgap, const int* iblock, const int* indexw, const double* gers,
           double* z, int ldz, int* isuppz, double* work, int* iwork) {
  (void)vu;
  double* z0 = z;
  --d; --l; --isplit; --w; --werr; --wgap; --iblock; --indexw; --gers; --isuppz; --work; --iwork;
  z -= 1 + ldz;
#define Z_(i, j) z[(i) + (j) * ldz]
  const int maxitr = 10;
  const double one = 1.0, two = 2.0, three = 3.0, four = 4.0, half = 0.5;
  if (n <= 0 || m <= 0) return 0;
  const int indld = n + 1, indlld = 2 * n + 1, indwrk = 3 * n + 1;
  const int minwsize = 12 * n;
  for (int i = 1; i <= minwsize; ++i) work[i] = 0.0;
  const int iindr = 0, iindc1 = n, iindc2 = 2 * n, iindwk = 3 * n + 1;
  const int miniwsize = 7 * n;
  for (int i = 1; i <= miniwsize; ++i) iwork[i] = 0;
  int zusedl = 1;
  if (dol > 1) zusedl = dol - 1;
  int zusedu = m;
  if (dou < m) zusedu = dou + 1;
  const int zusedw = zusedu - zusedl + 1;
  dlasetFull(n, zusedw, 0.0, 0.0, z0 + (zusedl - 1) * ldz, ldz);
  const double eps = dlamch('P');
  const double rqtol = two * eps;
  const bool tryrqc = true;
  if (!(dol == 1 && dou == m)) {
    rtol1 = four * eps;
    rtol2 = four * eps;
  }
  int done = 0;
  int ibegin = 1, wbegin = 1;
  const int nblk = iblock[m];
  for (int jblk = 1; jblk <= nblk; ++jblk) {
    const int iend = isplit[jblk];
    double sigma = l[iend];
    int wend = wbegin - 1;
    while (wend < m && iblock[wend + 1] == jblk) ++wend;
    if (wend < wbegin) {
      ibegin = iend + 1;
      continue;
    } else if (wend < dol || wbegin > dou) {
      ibegin = iend + 1;
      wbegin = wend + 1;
      continue;
    }
    double gl = gers[2 * ibegin - 1], gu = gers[2 * ibegin];
    for (int i = ibegin + 1; i <= iend; ++i) {
      gl = fmin(gers[2 * i - 1], gl);
      gu = fmax(gers[2 * i], gu);
    }
    const double spdiam = gu - gl;
    const int oldien = ibegin - 1;
    const int in = iend - ibegin + 1;
    const int im = wend - wbegin + 1;
    if (ibegin == iend) {
      ++done;
      Z_(ibegin, wbegin) = one;
      isuppz[2 * wbegin - 1] = ibegin;
      isuppz[2 * wbegin] = ibegin;
      w[wbegin] = w[wbegin] + sigma;
      work[wbegin] = w[wbegin];
      ibegin = iend + 1;
      ++wbegin;
      continue;
    }
    dcopy(im, &w[wbegin], 1, &work[wbegin], 1);
    for (int i = 1; i <= im; ++i) w[wbegin + i - 1] = w[wbegin + i - 1] + sigma;
    int ndepth = 0, parity = 1, nclus = 1;
    iwork[iindc1 + 1] = 1;
    iwork[iindc1 + 2] = im;
    int idone = 0;
    while (idone < im) {
      if (ndepth > m) return -2;
      const int oldncl = nclus;
      nclus = 0;
      parity = 1 - parity;
      int oldcls, newcls;
      if (parity == 0) {
        oldcls = iindc1;
        newcls = iindc2;
      } else {
        oldcls = iindc2;
        newcls = iindc1;
      }
      for (int i = 1; i <= oldncl; ++i) {
        int j = oldcls + 2 * i;
        const int oldfst = iwork[j - 1];
        const int oldlst = iwork[j];
        if (ndepth > 0) {
          if (dol == 1 && dou == m) {
            j = wbegin + oldfst - 1;
          } else {
            if (wbegin + oldfst - 1 < dol) j = dol - 1;
            else if (wbegin + oldfst - 1 > dou) j = dou;
            else j = wbegin + oldfst - 1;
          }
          dcopy(in, &Z_(ibegin, j), 1, &d[ibegin], 1);
          dcopy(in - 1, &Z_(ibegin, j + 1), 1, &l[ibegin], 1);
          sigma = Z_(iend, j + 1);
          dlasetFull(in, 2, 0.0, 0.0, &Z_(ibegin, j), ldz);
        }
        for (j = ibegin; j <= iend - 1; ++j) {
          const double tmp = d[j] * l[j];
          work[indld - 1 + j] = tmp;
          work[indlld - 1 + j] = tmp * l[j];
        }
        if (ndepth > 0) {
          const int p = indexw[wbegin - 1 + oldfst];
          const int q = indexw[wbegin - 1 + oldlst];
          const int offset = indexw[wbegin] - 1;
          if (dlarrb(in, &d[ibegin], &work[indlld + ibegin - 1], p, q, rtol1, rtol2, offset,
                     &work[wbegin], &wgap[wbegin], &werr[wbegin], &work[indwrk], &iwork[iindwk],
                     pivmin, spdiam, in) != 0)
            return -1;
          if (oldfst > 1)
            wgap[wbegin + oldfst - 2] =
                fmax(wgap[wbegin + oldfst - 2], w[wbegin + oldfst - 1] - werr[wbegin + oldfst - 1] -
                                                    w[wbegin + oldfst - 2] - werr[wbegin + oldfst - 2]);
          if (wbegin + oldlst - 1 < wend)
            wgap[wbegin + oldlst - 1] =
                fmax(wgap[wbegin + oldlst - 1], w[wbegin + oldlst] - werr[wbegin + oldlst] -
                                                    w[wbegin + oldlst - 1] - werr[wbegin + oldlst - 1]);
          for (j = oldfst; j <= oldlst; ++j) w[wbegin + j - 1] = work[wbegin + j - 1] + sigma;
        }
        int newfst = oldfst;
        for (j = oldfst; j <= oldlst; ++j) {
          int newlst;
          if (j == oldlst) {
            newlst = j;
          } else if (wgap[wbegin + j - 1] >= minrgp * fabs(work[wbegin + j - 1])) {
            newlst = j;
          } else {
            continue;  // GOTO 140
          }
          const int newsiz = newlst - newfst + 1;
          int newftt;
          if (dol == 1 && dou == m) {
            newftt = wbegin + newfst - 1;
          } else {
            if (wbegin + newfst - 1 < dol) newftt = dol - 1;
            else if (wbegin + newfst - 1 > dou) newftt = dou;
            else newftt = wbegin + newfst - 1;
          }
          if (newsiz > 1) {
            double lgap;
            if (newfst == 1) lgap = fmax(0.0, w[wbegin] - werr[wbegin] - vl);
            else lgap = wgap[wbegin + newfst - 2];
            const double rgap = wgap[wbegin + newlst - 1];
            for (int k = 1; k <= 2; ++k) {
              const int p = k == 1 ? indexw[wbegin - 1 + newfst] : indexw[wbegin - 1 + newlst];
              const int offset = indexw[wbegin] - 1;
              dlarrb(in, &d[ibegin], &work[indlld + ibegin - 1], p, p, rqtol, rqtol, offset,
                     &work[wbegin], &wgap[wbegin], &werr[wbegin], &work[indwrk], &iwork[iindwk],
                     pivmin, spdiam, in);
            }
            if (wbegin + newlst - 1 < dol || wbegin + newfst - 1 > dou) {
              idone = idone + newlst - newfst + 1;
              newfst = j + 1;  // label 139
              continue;
            }
            double tau;
            const int iinfo = dlarrf(in, &d[ibegin], &l[ibegin], &work[indld + ibegin - 1], newfst,
                                     newlst, &work[wbegin], &wgap[wbegin], &werr[wbegin], spdiam,
                                     lgap, rgap, pivmin, tau, &Z_(ibegin, newftt),
                                     &Z_(ibegin, newftt + 1), &work[indwrk]);
            if (iinfo == 0) {
              const double ssigma = sigma + tau;
              Z_(iend, newftt + 1) = ssigma;
              for (int k = newfst; k <= newlst; ++k) {
                const double fudge0 = three * eps;  // THREE*EPS*ABS(WORK) is fused below
                const double wold = fabs(work[wbegin + k - 1]);
                work[wbegin + k - 1] = work[wbegin + k - 1] - tau;
                const double fudge = fma(fudge0, wold, four * eps * fabs(work[wbegin + k - 1]));
                werr[wbegin + k - 1] = werr[wbegin + k - 1] + fudge;
              }
              ++nclus;
              const int k = newcls + 2 * nclus;
              iwork[k - 1] = newfst;
              iwork[k] = newlst;
            } else {
              return -2;
            }
          } else {
            int iter = 0;
            const double tol = four * std::log(double(in)) * eps;
            const int k = newfst;
            const int windex = wbegin + k - 1;
            const int windmn = windex - 1 > 1 ? windex - 1 : 1;
            const int windpl = windex + 1 < m ? windex + 1 : m;
            double lambda = work[windex];
            ++done;
            bool eskip;
            double savgap = 0.0;
            if (windex < dol || windex > dou) {
              eskip = true;
            } else {
              eskip = false;
              double left = work[windex] - werr[windex];
              double right = work[windex] + werr[windex];
              const int indeig = indexw[windex];
              double lgap, rgap;
              if (k == 1) lgap = eps * fmax(fabs(left), fabs(right));
              else lgap = wgap[windmn];
              if (k == im) rgap = eps * fmax(fabs(left), fabs(right));
              else rgap = wgap[windex];
              const double gap = fmin(lgap, rgap);
              double gaptol;
              if (k == 1 || k == im) gaptol = 0.0;
              else gaptol = gap * eps;
              int isupmn = in, isupmx = 1;
              savgap = wgap[windex];
              wgap[windex] = gap;
              bool usedbs = false, usedrq = false, needbs = !tryrqc;
              double bstres = 0.0, bstw = 0.0, ztz, mingma, nrminv, resid, rqcorr;
              int negcnt;
              for (;;) {
                // label 120
                if (needbs) {
                  usedbs = true;
                  const int itmp1 = iwork[iindr + windex];
                  const int offset = indexw[wbegin] - 1;
                  if (dlarrb(in, &d[ibegin], &work[indlld + ibegin - 1], indeig, indeig, 0.0,
                             two * eps, offset, &work[wbegin], &wgap[wbegin], &werr[wbegin],
                             &work[indwrk], &iwork[iindwk], pivmin, spdiam, itmp1) != 0)
                    return -3;
                  lambda = work[windex];
                  iwork[iindr + windex] = 0;
                }
                dlar1v(in, 1, in, lambda, &d[ibegin], &l[ibegin], &work[indld + ibegin - 1],
                       &work[indlld + ibegin - 1], pivmin, gaptol, &Z_(ibegin, windex), !usedbs,
                       negcnt, ztz, mingma, iwork[iindr + windex], &isuppz[2 * windex - 1], nrminv,
                       resid, rqcorr, &work[indwrk]);
                if (iter == 0) {
                  bstres = resid;
                  bstw = lambda;
                } else if (resid < bstres) {
                  bstres = resid;
                  bstw = lambda;
                }
                isupmn = isupmn < isuppz[2 * windex - 1] ? isupmn : isuppz[2 * windex - 1];
                isupmx = isupmx > isuppz[2 * windex] ? isupmx : isuppz[2 * windex];
                ++iter;
                if (resid > tol * gap && fabs(rqcorr) > rqtol * fabs(lambda) && !usedbs) {
                  double sgndef;
                  if (indeig <= negcnt) sgndef = -one;
                  else sgndef = one;
                  if (rqcorr * sgndef >= 0.0 && lambda + rqcorr <= right &&
                      lambda + rqcorr >= left) {
                    usedrq = true;
                    if (sgndef == one) left = lambda;
                    else right = lambda;
                    work[windex] = half * (right + left);
                    lambda = lambda + rqcorr;
                    werr[windex] = half * (right - left);
                  } else {
                    needbs = true;
                  }
                  if (right - left < rqtol * fabs(lambda)) {
                    usedbs = true;
                    continue;
                  } else if (iter < maxitr) {
                    continue;
                  } else if (iter == maxitr) {
                    needbs = true;
                    continue;
                  } else {
                    return 5;
                  }
                } else {
                  bool stp2ii = false;
                  if (usedrq && usedbs && bstres <= resid) {
                    lambda = bstw;
                    stp2ii = true;
                  }
                  if (stp2ii) {
                    dlar1v(in, 1, in, lambda, &d[ibegin], &l[ibegin], &work[indld + ibegin - 1],
                           &work[indlld + ibegin - 1], pivmin, gaptol, &Z_(ibegin, windex),
                           !usedbs, negcnt, ztz, mingma, iwork[iindr + windex],
                           &isuppz[2 * windex - 1], nrminv, resid, rqcorr, &work[indwrk]);
                  }
                  work[windex] = lambda;
                }
                break;
              }
              isuppz[2 * windex - 1] = isuppz[2 * windex - 1] + oldien;
              isuppz[2 * windex] = isuppz[2 * windex] + oldien;
              const int zfrom = isuppz[2 * windex - 1];
              const int zto = isuppz[2 * windex];
              isupmn = isupmn + oldien;
              isupmx = isupmx + oldien;
              if (isupmn < zfrom)
                for (int ii = isupmn; ii <= zfrom - 1; ++ii) Z_(ii, windex) = 0.0;
              if (isupmx > zto)
                for (int ii = zto + 1; ii <= isupmx; ++ii) Z_(ii, windex) = 0.0;
              dscal(zto - zfrom + 1, nrminv, &Z_(zfrom, windex), 1);
            }
            // label 125
            w[windex] = lambda + sigma;
            if (!eskip) {
              if (k > 1)
                wgap[windmn] = fmax(wgap[windmn],
                                    w[windex] - werr[windex] - w[windmn] - werr[windmn]);
              if (windex < wend)
                wgap[windex] = fmax(savgap, w[windpl] - werr[windpl] - w[windex] - werr[windex]);
            }
            ++idone;
          }
          // label 139
          newfst = j + 1;
        }
      }
      ++ndepth;
    }
    ibegin = iend + 1;
    wbegin = wend + 1;
  }
#undef Z_
  (void)done;
  return 0;
}

// dlarrj.f (2 fused: LEFT - WERR*FAC, RIGHT + WERR*FAC).
void dlarrj(int n, const double* d, const double* e2, int ifirst, int ilast, double rtol,
            int offset, double* w, double* werr, double* work, int* iwork, double pivmin,
            double spdiam) {
  --d; --e2; --w; --werr; --work; --iwork;
  const double one = 1.0, two = 2.0, half = 0.5;
  if (n <= 0) return;
  const int maxitr = int((std::log(spdiam + pivmin) - std::log(pivmin)) / kLn2) + 2;
  int i1 = ifirst;
  const int i2 = ilast;
  int nint = 0, prev = 0;
  const int i1start = i1;
  for (int i = i1start; i <= i2; ++i) {
    const int k = 2 * i, ii = i - offset;
    double left = w[ii] - werr[ii];
    const double mid = w[ii];
    double right = w[ii] + werr[ii];
    const double width = right - mid;
    const double tmp = fmax(fabs(left), fabs(right));
    if (width < rtol * tmp) {
      iwork[k - 1] = -1;
      if (i == i1 && i < i2) i1 = i + 1;
      if (prev >= i1 && i <= i2) iwork[2 * prev - 1] = i + 1;
    } else {
      prev = i;
      double fac = one;
      int cnt;
      for (;;) {
        cnt = 0;
        const double s = left;
        double dplus = d[1] - s;
        if (dplus < 0.0) ++cnt;
        for (int j = 2; j <= n; ++j) {
          dplus = d[j] - s - e2[j - 1] / dplus;
          if (dplus < 0.0) ++cnt;
        }
        if (cnt > i - 1) {
          left = fma(-werr[ii], fac, left);  // fused
          fac = two * fac;
        } else {
          break;
        }
      }
      fac = one;
      for (;;) {
        cnt = 0;
        const double s = right;
        double dplus = d[1] - s;
        if (dplus < 0.0) ++cnt;
        for (int j = 2; j <= n; ++j) {
          dplus = d[j] - s - e2[j - 1] / dplus;
          if (dplus < 0.0) ++cnt;
        }
        if (cnt < i) {
          right = fma(werr[ii], fac, right);  // fused
          fac = two * fac;
        } else {
          break;
        }
      }
      ++nint;
      iwork[k - 1] = i + 1;
      iwork[k] = cnt;
    }
    work[k - 1] = left;
    work[k] = right;
  }
  const int savi1 = i1;
  int iter = 0;
  do {
    prev = i1 - 1;
    int i = i1;
    const int olnint = nint;
    for (int p = 1; p <= olnint; ++p) {
      const int k = 2 * i;
      const int next = iwork[k - 1];
      const double left = work[k - 1], right = work[k];
      const double mid = half * (left + right);
      const double width = right - mid;
      const double tmp = fmax(fabs(left), fabs(right));
      if (width < rtol * tmp || iter == maxitr) {
        --nint;
        iwork[k - 1] = 0;
        if (i1 == i) {
          i1 = next;
        } else {
          if (prev >= i1) iwork[2 * prev - 1] = next;
        }
        i = next;
        continue;
      }
      prev = i;
      int cnt = 0;
      const double s = mid;
      double dplus = d[1] - s;
      if (dplus < 0.0) ++cnt;
      for (int j = 2; j <= n; ++j) {
        dplus = d[j] - s - e2[j - 1] / dplus;
        if (dplus < 0.0) ++cnt;
      }
      if (cnt <= i - 1) work[k - 1] = mid;
      else work[k] = mid;
      i = next;
    }
    ++iter;
  } while (nint > 0 && iter <= maxitr);
  for (int i = savi1; i <= ilast; ++i) {
    const int k = 2 * i, ii = i - offset;
    if (iwork[k - 1] == 0) {
      w[ii] = half * (work[k - 1] + work[k]);
      werr[ii] = work[k] - w[ii];
    }
  }
}

// dstemr.f, JOBZ='V', RANGE='A' (no FMA).
int dstemrVA(int n, double* d, double* e, int& m, double* w, double* z, int ldz, int nzc,
             int* isuppz, bool& tryrac, double* work, int lwork, int* iwork, int liwork) {
  double* d1 = d;
  double* e1 = e;
  double* w1 = w;
  double* z1 = z;
  --d; --e; --w; --isuppz; --work; --iwork;
  z -= 1 + ldz;
#define Z_(i, j) z[(i) + (j) * ldz]
  const double one = 1.0, four = 4.0, minrgp = 1.0e-3;
  const bool lquery = lwork == -1 || liwork == -1;
  const bool zquery = nzc == -1;
  bool laeswap = false;
  const int lwmin = 18 * n, liwmin = 10 * n;
  double wl = 0.0, wu = 0.0;
  int iil = 0, iiu = 0, nsplit = 0;
  int info = 0;
  (void)iil; (void)iiu;
  if (n < 0) info = -3;
  else if (ldz < 1 || ldz < n) info = -13;
  else if (lwork < lwmin && !lquery) info = -17;
  else if (liwork < liwmin && !lquery) info = -19;
  const double safmin = dlamch('S');
  const double eps = dlamch('P');
  const double smlnum = safmin / eps;
  const double bignum = one / smlnum;
  const double rmin = sqrt(smlnum);
  const double rmax = fmin(sqrt(bignum), one / sqrt(sqrt(safmin)));
  if (info == 0) {
    work[1] = lwmin;
    iwork[1] = liwmin;
    const int nzcmin = n;
    if (zquery) Z_(1, 1) = nzcmin;
    else if (nzc < nzcmin) info = -14;
  }
  if (info != 0) return info;
  if (lquery || zquery) return 0;
  m = 0;
  if (n == 0) return 0;
  if (n == 1) {
    m = 1;
    w[1] = d[1];
    Z_(1, 1) = one;
    isuppz[1] = 1;
    isuppz[2] = 1;
    return 0;
  }
  if (n == 2) {
    double r1, r2, cs, sn;
    dlaev2(d[1], e[1], d[2], r1, r2, cs, sn);
    if (r1 < r2) {
      e[2] = r1;
      r1 = r2;
      r2 = e[2];
      laeswap = true;
    }
    const auto supp = [&](int mm) {
      if (sn != 0.0) {
        if (cs != 0.0) {
          isuppz[2 * mm - 1] = 1;
          isuppz[2 * mm] = 2;
        } else {
          isuppz[2 * mm - 1] = 1;
          isuppz[2 * mm] = 1;
        }
      } else {
        isuppz[2 * mm - 1] = 2;
        isuppz[2 * mm] = 2;
      }
    };
    ++m;
    w[m] = r2;
    if (laeswap) {
      Z_(1, m) = cs;
      Z_(2, m) = sn;
    } else {
      Z_(1, m) = -sn;
      Z_(2, m) = cs;
    }
    supp(m);
    ++m;
    w[m] = r1;
    if (laeswap) {
      Z_(1, m) = -sn;
      Z_(2, m) = cs;
    } else {
      Z_(1, m) = cs;
      Z_(2, m) = sn;
    }
    supp(m);
  } else {
    const int indgrs = 1, inderr = 2 * n + 1, indgp = 3 * n + 1, indd = 4 * n + 1,
              inde2 = 5 * n + 1, indwrk = 6 * n + 1;
    const int iinspl = 1, iindbl = n + 1, iindw = 2 * n + 1, iindwk = 3 * n + 1;
    double scale = one;
    double tnrm = dlanstM(n, d1, e1);
    if (tnrm > 0.0 && tnrm < rmin) scale = rmin / tnrm;
    else if (tnrm > rmax) scale = rmax / tnrm;
    if (scale != one) {
      dscal(n, scale, d1, 1);
      dscal(n - 1, scale, e1, 1);
      tnrm = tnrm * scale;
    }
    int iinfo;
    if (tryrac) iinfo = dlarrr(n, d1, e1);
    else iinfo = -1;
    double thresh;
    if (iinfo == 0) {
      thresh = eps;
    } else {
      thresh = -eps;
      tryrac = false;
    }
    if (tryrac) dcopy(n, d1, 1, &work[indd], 1);
    for (int j = 1; j <= n - 1; ++j) work[inde2 + j - 1] = e[j] * e[j];
    double rtol1 = sqrt(eps);
    double rtol2 = fmax(sqrt(eps) * 5.0e-3, four * eps);
    double pivmin;
    iinfo = dlarreA(n, wl, wu, d1, e1, &work[inde2], rtol1, rtol2, thresh, nsplit, &iwork[iinspl],
                    m, w1, &work[inderr], &work[indgp], &iwork[iindbl], &iwork[iindw],
                    &work[indgrs], pivmin, &work[indwrk], &iwork[iindwk]);
    if (iinfo != 0) return 10 + (iinfo < 0 ? -iinfo : iinfo);
    iinfo = dlarrv(n, wl, wu, d1, e1, pivmin, &iwork[iinspl], m, 1, m, minrgp, rtol1, rtol2, w1,
                   &work[inderr], &work[indgp], &iwork[iindbl], &iwork[iindw], &work[indgrs], z1,
                   ldz, isuppz + 1, &work[indwrk], &iwork[iindwk]);
    if (iinfo != 0) return 20 + (iinfo < 0 ? -iinfo : iinfo);
    if (tryrac) {
      int ibegin = 1, wbegin = 1;
      const int nblk = iwork[iindbl + m - 1];
      for (int jblk = 1; jblk <= nblk; ++jblk) {
        const int iend = iwork[iinspl + jblk - 1];
        const int in = iend - ibegin + 1;
        int wend = wbegin - 1;
        while (wend < m && iwork[iindbl + wend] == jblk) ++wend;
        if (wend < wbegin) {
          ibegin = iend + 1;
          continue;
        }
        const int offset = iwork[iindw + wbegin - 1] - 1;
        const int ifirst = iwork[iindw + wbegin - 1];
        const int ilast = iwork[iindw + wend - 1];
        rtol2 = four * eps;
        dlarrj(in, &work[indd + ibegin - 1], &work[inde2 + ibegin - 1], ifirst, ilast, rtol2,
               offset, &w[wbegin], &work[inderr + wbegin - 1], &work[indwrk], &iwork[iindwk],
               pivmin, tnrm);
        ibegin = iend + 1;
        wbegin = wend + 1;
      }
    }
    if (scale != one) dscal(m, one / scale, w1, 1);
  }
  if (nsplit > 1 || n == 2) {
    for (int j = 1; j <= m - 1; ++j) {
      int i = 0;
      double tmp = w[j];
      for (int jj = j + 1; jj <= m; ++jj) {
        if (w[jj] < tmp) {
          i = jj;
          tmp = w[jj];
        }
      }
      if (i != 0) {
        w[i] = w[j];
        w[j] = tmp;
        dswap(n, &Z_(1, i), 1, &Z_(1, j), 1);
        int itmp = isuppz[2 * i - 1];
        isuppz[2 * i - 1] = isuppz[2 * j - 1];
        isuppz[2 * j - 1] = itmp;
        itmp = isuppz[2 * i];
        isuppz[2 * i] = isuppz[2 * j];
        isuppz[2 * j] = itmp;
      }
    }
  }
  work[1] = lwmin;
  iwork[1] = liwmin;
#undef Z_
  return 0;
}

// ----------------------------------- DSYEVR's fallback: DSTEBZ + DSTEIN
// idamax.f, INCX=1.
int idamax(int n, const double* dx) {
  --dx;
  if (n < 1) return 0;
  int r = 1;
  if (n == 1) return r;
  double dmax = fabs(dx[1]);
  for (int i = 2; i <= n; ++i) {
    if (fabs(dx[i]) > dmax) {
      r = i;
      dmax = fabs(dx[i]);
    }
  }
  return r;
}

// dlaebz.f, IJOB = 1 and 2 with NBMIN = 0 (DSTEBZ's NB is 0), the scalar
// bisection loop; no FMA.
int dlaebz(int ijob, int nitmax, int n, int mmax, int minp, double abstol, double reltol,
           double pivmin, const double* d, const double* e2, double* ab, double* c, int& mout,
           int* nab) {
  --d; --e2; --c;
  ab -= 1 + mmax; nab -= 1 + mmax;
#define AB_(i, j) ab[(i) + (j) * mmax]
#define NAB_(i, j) nab[(i) + (j) * mmax]
  const double half = 0.5;
  if (ijob == 1) {
    mout = 0;
    for (int ji = 1; ji <= minp; ++ji) {
      for (int jp = 1; jp <= 2; ++jp) {
        double tmp1 = d[1] - AB_(ji, jp);
        if (fabs(tmp1) < pivmin) tmp1 = -pivmin;
        NAB_(ji, jp) = 0;
        if (tmp1 <= 0.0) NAB_(ji, jp) = 1;
        for (int j = 2; j <= n; ++j) {
          tmp1 = d[j] - e2[j - 1] / tmp1 - AB_(ji, jp);
          if (fabs(tmp1) < pivmin) tmp1 = -pivmin;
          if (tmp1 <= 0.0) NAB_(ji, jp) = NAB_(ji, jp) + 1;
        }
      }
      mout = mout + NAB_(ji, 2) - NAB_(ji, 1);
    }
    return 0;
  }
  int kf = 1, kl = minp;
  for (int ji = 1; ji <= minp; ++ji) c[ji] = half * (AB_(ji, 1) + AB_(ji, 2));
  for (int jit = 1; jit <= nitmax; ++jit) {
    int klnew = kl;
    for (int ji = kf; ji <= kl; ++ji) {
      const double tmp1 = c[ji];
      double tmp2 = d[1] - tmp1;
      int itmp1 = 0;
      if (tmp2 <= pivmin) {
        itmp1 = 1;
        tmp2 = fmin(tmp2, -pivmin);
      }
      for (int j = 2; j <= n; ++j) {
        tmp2 = d[j] - e2[j - 1] / tmp2 - tmp1;
        if (tmp2 <= pivmin) {
          ++itmp1;
          tmp2 = fmin(tmp2, -pivmin);
        }
      }
      itmp1 = NAB_(ji, 2) < (NAB_(ji, 1) > itmp1 ? NAB_(ji, 1) : itmp1)
                  ? NAB_(ji, 2)
                  : (NAB_(ji, 1) > itmp1 ? NAB_(ji, 1) : itmp1);
      if (itmp1 == NAB_(ji, 2)) {
        AB_(ji, 2) = tmp1;
      } else if (itmp1 == NAB_(ji, 1)) {
        AB_(ji, 1) = tmp1;
      } else if (klnew < mmax) {
        ++klnew;
        AB_(klnew, 2) = AB_(ji, 2);
        NAB_(klnew, 2) = NAB_(ji, 2);
        AB_(klnew, 1) = tmp1;
        NAB_(klnew, 1) = itmp1;
        AB_(ji, 2) = tmp1;
        NAB_(ji, 2) = itmp1;
      } else {
        return mmax + 1;
      }
    }
    kl = klnew;
    int kfnew = kf;
    for (int ji = kf; ji <= kl; ++ji) {
      double tmp1 = fabs(AB_(ji, 2) - AB_(ji, 1));
      double tmp2 = fmax(fabs(AB_(ji, 2)), fabs(AB_(ji, 1)));
      if (tmp1 < fmax(fmax(abstol, pivmin), reltol * tmp2) || NAB_(ji, 1) >= NAB_(ji, 2)) {
        if (ji > kfnew) {
          tmp1 = AB_(ji, 1);
          tmp2 = AB_(ji, 2);
          const int itmp1 = NAB_(ji, 1), itmp2 = NAB_(ji, 2);
          AB_(ji, 1) = AB_(kfnew, 1);
          AB_(ji, 2) = AB_(kfnew, 2);
          NAB_(ji, 1) = NAB_(kfnew, 1);
          NAB_(ji, 2) = NAB_(kfnew, 2);
          AB_(kfnew, 1) = tmp1;
          AB_(kfnew, 2) = tmp2;
          NAB_(kfnew, 1) = itmp1;
          NAB_(kfnew, 2) = itmp2;
        }
        ++kfnew;
      }
    }
    kf = kfnew;
    for (int ji = kf; ji <= kl; ++ji) c[ji] = half * (AB_(ji, 1) + AB_(ji, 2));
    if (kf > kl) break;
  }
  mout = kl;
#undef AB_
#undef NAB_
  return kl + 1 - kf > 0 ? kl + 1 - kf : 0;
}

// dstebz.f, RANGE='A', ORDER='B' (5 fused sites on this path: the split
// test |D(J)*D(J-1)|*ULP**2 + SAFEMN, and GL/GU's two subtracted/added
// products each).
int dstebzAB(int n, double abstol, const double* d, const double* e, int& m, int& nsplit, double* w,
             int* iblock, int* isplit, double* work, int* iwork) {
  --d; --e; --w; --iblock; --isplit; --work; --iwork;
  const double relfac = 2.0, fudge = 2.1, half = 0.5;
  bool ncnvrg = false;
  m = 0;
  if (n == 0) return 0;
  const double safemn = dlamch('S');
  const double ulp = dlamch('P');
  const double rtoli = ulp * relfac;
  int nb = ilaenv(1, "DSTEBZ");
  if (nb <= 1) nb = 0;
  (void)nb;  // NB = 0: DLAEBZ runs its scalar loop
  if (n == 1) {
    nsplit = 1;
    isplit[1] = 1;
    w[1] = d[1];
    iblock[1] = 1;
    m = 1;
    return 0;
  }
  nsplit = 1;
  work[n] = 0.0;
  double pivmin = 1.0;
  for (int j = 2; j <= n; ++j) {
    const double tmp1 = e[j - 1] * e[j - 1];
    if (fma(fabs(d[j] * d[j - 1]), ulp * ulp, safemn) > tmp1) {
      isplit[nsplit] = j - 1;
      ++nsplit;
      work[j - 1] = 0.0;
    } else {
      work[j - 1] = tmp1;
      pivmin = fmax(pivmin, tmp1);
    }
  }
  isplit[nsplit] = n;
  pivmin = pivmin * safemn;
  // IRANGE = 1: TNORM and ATOLI are recomputed per block below.
  m = 0;
  int iend = 0;
  int nwl = 0, nwu = 0;
  for (int jb = 1; jb <= nsplit; ++jb) {
    const int ioff = iend;
    const int ibegin = ioff + 1;
    iend = isplit[jb];
    const int in = iend - ioff;
    if (in == 1) {
      ++nwl;
      ++nwu;
      ++m;
      w[m] = d[ibegin];
      iblock[m] = jb;
    } else {
      double gu = d[ibegin], gl = d[ibegin], tmp1 = 0.0;
      for (int j = ibegin; j <= iend - 1; ++j) {
        const double tmp2 = fabs(e[j]);
        gu = fmax(gu, d[j] + tmp1 + tmp2);
        gl = fmin(gl, d[j] - tmp1 - tmp2);
        tmp1 = tmp2;
      }
      gu = fmax(gu, d[iend] + tmp1);
      gl = fmin(gl, d[iend] - tmp1);
      const double bnorm = fmax(fabs(gl), fabs(gu));
      const double t = fudge * bnorm * ulp;
      gl = fma(-fudge, pivmin, fma(-t, double(in), gl));
      gu = fma(fudge, pivmin, fma(t, double(in), gu));
      const double atoli = abstol <= 0.0 ? ulp * fmax(fabs(gl), fabs(gu)) : abstol;
      work[n + 1] = gl;
      work[n + in + 1] = gu;
      int im = 0, iout = 0;
      dlaebz(1, 0, in, in, 1, atoli, rtoli, pivmin, &d[ibegin], &work[ibegin], &work[n + 1],
             &work[n + 2 * in + 1], im, &iwork[1]);
      nwl += iwork[1];
      nwu += iwork[in + 1];
      const int iwoff = m - iwork[1];
      const int itmax = int((std::log(gu - gl + pivmin) - std::log(pivmin)) / kLn2) + 2;
      const int iinfo = dlaebz(2, itmax, in, in, 1, atoli, rtoli, pivmin, &d[ibegin], &work[ibegin],
                               &work[n + 1], &work[n + 2 * in + 1], iout, &iwork[1]);
      for (int j = 1; j <= iout; ++j) {
        const double tmp = half * (work[j + n] + work[j + in + n]);
        int ib;
        if (j > iout - iinfo) {
          ncnvrg = true;
          ib = -jb;
        } else {
          ib = jb;
        }
        for (int je = iwork[j] + 1 + iwoff; je <= iwork[j + in] + iwoff; ++je) {
          w[je] = tmp;
          iblock[je] = ib;
        }
      }
      m += im;
    }
  }
  (void)nwl; (void)nwu;
  return ncnvrg ? 1 : 0;
}

// dlagtf.f (2 fused: A(K+1) - C(K)*B(K) and B(K) - MULT*TEMP).
void dlagtf(int n, double* a, double lambda, double* b, double* c, double tol, double* d, int* in) {
  --a; --b; --c; --d; --in;
  if (n == 0) return;
  a[1] = a[1] - lambda;
  in[n] = 0;
  if (n == 1) {
    if (a[1] == 0.0) in[1] = 1;
    return;
  }
  const double eps = dlamch('E');
  const double tl = fmax(tol, eps);
  double scale1 = fabs(a[1]) + fabs(b[1]);
  for (int k = 1; k <= n - 1; ++k) {
    a[k + 1] = a[k + 1] - lambda;
    double scale2 = fabs(c[k]) + fabs(a[k + 1]);
    if (k < n - 1) scale2 = scale2 + fabs(b[k + 1]);
    double piv1, piv2;
    if (a[k] == 0.0) piv1 = 0.0;
    else piv1 = fabs(a[k]) / scale1;
    if (c[k] == 0.0) {
      in[k] = 0;
      piv2 = 0.0;
      scale1 = scale2;
      if (k < n - 1) d[k] = 0.0;
    } else {
      piv2 = fabs(c[k]) / scale2;
      if (piv2 <= piv1) {
        in[k] = 0;
        scale1 = scale2;
        c[k] = c[k] / a[k];
        a[k + 1] = fma(-c[k], b[k], a[k + 1]);
        if (k < n - 1) d[k] = 0.0;
      } else {
        in[k] = 1;
        const double mult = a[k] / c[k];
        a[k] = c[k];
        const double temp = a[k + 1];
        a[k + 1] = fma(-mult, temp, b[k]);
        if (k < n - 1) {
          d[k] = b[k + 1];
          b[k + 1] = -mult * d[k];
        }
        b[k] = temp;
        c[k] = mult;
      }
    }
    if (fmax(piv1, piv2) <= tl && in[n] == 0) in[n] = k;
  }
  if (fabs(a[n]) <= scale1 * tl && in[n] == 0) in[n] = n;
}

// dlagts.f, JOB = -1 (the forward elimination's Y - C*Y and the back
// substitution's Y - B*Y1 - D*Y2 are fused).
int dlagtsM1(int n, const double* a, const double* b, const double* c, const double* d,
             const int* in, double* y, double& tol) {
  --a; --b; --c; --d; --in; --y;
  if (n == 0) return 0;
  const double eps = dlamch('E');
  const double sfmin = dlamch('S');
  const double bignum = 1.0 / sfmin;
  if (tol <= 0.0) {
    tol = fabs(a[1]);
    if (n > 1) tol = fmax(fmax(tol, fabs(a[2])), fabs(b[1]));
    for (int k = 3; k <= n; ++k) tol = fmax(fmax(fmax(tol, fabs(a[k])), fabs(b[k - 1])), fabs(d[k - 2]));
    tol = tol * eps;
    if (tol == 0.0) tol = eps;
  }
  for (int k = 2; k <= n; ++k) {
    if (in[k - 1] == 0) {
      y[k] = fma(-c[k - 1], y[k - 1], y[k]);
    } else {
      const double temp = y[k - 1];
      y[k - 1] = y[k];
      y[k] = fma(-c[k - 1], y[k], temp);
    }
  }
  for (int k = n; k >= 1; --k) {
    double temp;
    if (k <= n - 2) temp = fma(-d[k], y[k + 2], fma(-b[k], y[k + 1], y[k]));
    else if (k == n - 1) temp = fma(-b[k], y[k + 1], y[k]);
    else temp = y[k];
    double ak = a[k];
    double pert = copysign(tol, ak);
    for (;;) {
      const double absak = fabs(ak);
      if (absak < 1.0) {
        if (absak < sfmin) {
          if (absak == 0.0 || fabs(temp) * sfmin > absak) {
            ak = ak + pert;
            pert = 2 * pert;
            continue;
          } else {
            temp = temp * bignum;
            ak = ak * bignum;
          }
        } else if (fabs(temp) > absak * bignum) {
          ak = ak + pert;
          pert = 2 * pert;
          continue;
        }
      }
      break;
    }
    y[k] = temp / ak;
  }
  return 0;
}

// dstein.f (no FMA of its own).
int dstein(int n, const double* d, const double* e, int m, const double* w, const int* iblock,
           const int* isplit, double* z, int ldz, double* work, int* iwork, int* ifail) {
  --d; --e; --w; --iblock; --isplit; --work; --ifail;
  z -= 1 + ldz;
#define Z_(i, j) z[(i) + (j) * ldz]
  const double one = 1.0, ten = 10.0, odm3 = 1.0e-3, odm1 = 1.0e-1;
  const int maxits = 5, extra = 2;
  int info = 0;
  for (int i = 1; i <= m; ++i) ifail[i] = 0;
  if (n < 0) return -1;
  if (m < 0 || m > n) return -4;
  if (ldz < (n > 1 ? n : 1)) return -9;
  for (int j = 2; j <= m; ++j) {
    if (iblock[j] < iblock[j - 1]) return -6;
    if (iblock[j] == iblock[j - 1] && w[j] < w[j - 1]) return -5;
  }
  if (n == 0 || m == 0) return 0;
  if (n == 1) {
    Z_(1, 1) = one;
    return 0;
  }
  const double eps = dlamch('P');
  int iseed[4] = {1, 1, 1, 1};
  const int indrv1 = 0, indrv2 = indrv1 + n, indrv3 = indrv2 + n, indrv4 = indrv3 + n,
            indrv5 = indrv4 + n;
  int j1 = 1;
  double onenrm = 0.0, ortol = 0.0, dtpcrt = 0.0, xjm = 0.0;
  const int nblks = iblock[m];
  for (int nblk = 1; nblk <= nblks; ++nblk) {
    const int b1 = nblk == 1 ? 1 : isplit[nblk - 1] + 1;
    const int bn = isplit[nblk];
    const int blksiz = bn - b1 + 1;
    int gpind = 0;
    if (blksiz != 1) {
      gpind = j1;
      onenrm = fabs(d[b1]) + fabs(e[b1]);
      onenrm = fmax(onenrm, fabs(d[bn]) + fabs(e[bn - 1]));
      for (int i = b1 + 1; i <= bn - 1; ++i)
        onenrm = fmax(onenrm, fabs(d[i]) + fabs(e[i - 1]) + fabs(e[i]));
      ortol = odm3 * onenrm;
      dtpcrt = sqrt(odm1 / blksiz);
    }
    int jblk = 0;
    for (int j = j1; j <= m; ++j) {
      if (iblock[j] != nblk) {
        j1 = j;
        break;  // GO TO 160
      }
      ++jblk;
      double xj = w[j];
      if (blksiz == 1) {
        work[indrv1 + 1] = one;
      } else {
        if (jblk > 1) {
          const double eps1 = fabs(eps * xj);
          const double pertol = ten * eps1;
          const double sep = xj - xjm;
          if (sep < pertol) xj = xjm + pertol;
        }
        int its = 0, nrmchk = 0;
        dlarnv2(iseed, blksiz, &work[indrv1 + 1]);
        dcopy(blksiz, &d[b1], 1, &work[indrv4 + 1], 1);
        dcopy(blksiz - 1, &e[b1], 1, &work[indrv2 + 2], 1);
        dcopy(blksiz - 1, &e[b1], 1, &work[indrv3 + 1], 1);
        double tol = 0.0;
        dlagtf(blksiz, &work[indrv4 + 1], xj, &work[indrv2 + 2], &work[indrv3 + 1], tol,
               &work[indrv5 + 1], iwork);
        bool failed = false;
        for (;;) {
          // label 70
          ++its;
          if (its > maxits) {
            failed = true;
            break;
          }
          int jmax = idamax(blksiz, &work[indrv1 + 1]);
          const double scl = blksiz * onenrm * fmax(eps, fabs(work[indrv4 + blksiz])) /
                             fabs(work[indrv1 + jmax]);
          dscal(blksiz, scl, &work[indrv1 + 1], 1);
          dlagtsM1(blksiz, &work[indrv4 + 1], &work[indrv2 + 2], &work[indrv3 + 1],
                   &work[indrv5 + 1], iwork, &work[indrv1 + 1], tol);
          if (jblk != 1) {
            if (fabs(xj - xjm) > ortol) gpind = j;
            if (gpind != j) {
              for (int i = gpind; i <= j - 1; ++i) {
                const double ztr = -ddot(blksiz, &work[indrv1 + 1], 1, &Z_(b1, i), 1);
                daxpy(blksiz, ztr, &Z_(b1, i), 1, &work[indrv1 + 1], 1);
              }
            }
          }
          // label 90
          jmax = idamax(blksiz, &work[indrv1 + 1]);
          const double nrm = fabs(work[indrv1 + jmax]);
          if (nrm < dtpcrt) continue;
          ++nrmchk;
          if (nrmchk < extra + 1) continue;
          break;  // GO TO 110
        }
        if (failed) {
          // label 100
          ++info;
          ifail[info] = j;
        }
        // label 110
        double scl = one / dnrm2(blksiz, &work[indrv1 + 1], 1);
        const int jmax = idamax(blksiz, &work[indrv1 + 1]);
        if (work[indrv1 + jmax] < 0.0) scl = -scl;
        dscal(blksiz, scl, &work[indrv1 + 1], 1);
      }
      // label 120
      for (int i = 1; i <= n; ++i) Z_(i, j) = 0.0;
      for (int i = 1; i <= blksiz; ++i) Z_(b1 + i - 1, j) = work[indrv1 + i];
      xjm = xj;
    }
  }
#undef Z_
  return info;
}

// dsyevr.f, JOBZ='V', RANGE='A', UPLO='L' (no FMA of its own).
int dsyevrVAL_(int n, double* a, int lda, double abstol, int& m, double* w, double* z, int ldz,
               int* isuppz, double* work, int lwork, int* iwork, int liwork) {
  double* a1 = a;
  double* w1 = w;
  --w; --isuppz; --work; --iwork;
  a -= 1 + lda;
  z -= 1 + ldz;
#define Z_(i, j) z[(i) + (j) * ldz]
  const double one = 1.0, two = 2.0;
  const int ieeeok = ilaenv(10, "DSYEVR");
  const bool lquery = lwork == -1 || liwork == -1;
  const int lwmin = 26 * n > 1 ? 26 * n : 1;
  const int liwmin = 10 * n > 1 ? 10 * n : 1;
  int info = 0;
  if (n < 0) info = -4;
  else if (lda < (n > 1 ? n : 1)) info = -6;
  if (info == 0) {
    if (ldz < 1 || ldz < n) info = -15;
    else if (lwork < lwmin && !lquery) info = -18;
    else if (liwork < liwmin && !lquery) info = -20;
  }
  int lwkopt = 0;
  if (info == 0) {
    int nb = ilaenv(1, "DSYTRD");
    const int nb2 = ilaenv(1, "DORMTR");
    nb = nb > nb2 ? nb : nb2;
    lwkopt = (nb + 1) * n > lwmin ? (nb + 1) * n : lwmin;
    work[1] = lwkopt;
    iwork[1] = liwmin;
  }
  if (info != 0) return info;
  if (lquery) return 0;
  m = 0;
  if (n == 0) {
    work[1] = 1;
    return 0;
  }
  if (n == 1) {
    work[1] = 7;
    m = 1;
    w[1] = A_(1, 1);
    Z_(1, 1) = one;
    isuppz[1] = 1;
    isuppz[2] = 1;
    return 0;
  }
  const double safmin = dlamch('S');
  const double eps = dlamch('P');
  const double smlnum = safmin / eps;
  const double bignum = one / smlnum;
  const double rmin = sqrt(smlnum);
  const double rmax = fmin(sqrt(bignum), one / sqrt(sqrt(safmin)));
  int iscale = 0;
  double sigma = one;
  const double anrm = dlansyML(n, a1, lda);
  if (anrm > 0.0 && anrm < rmin) {
    iscale = 1;
    sigma = rmin / anrm;
  } else if (anrm > rmax) {
    iscale = 1;
    sigma = rmax / anrm;
  }
  double abstll = abstol;
  if (iscale == 1) {
    for (int j = 1; j <= n; ++j) dscal(n - j + 1, sigma, &A_(j, j), 1);
    if (abstol > 0.0) abstll = abstol * sigma;
  }
  const int indtau = 1, indd = indtau + n, inde = indd + n, inddd = inde + n, indee = inddd + n,
            indwk = indee + n;
  const int indibl = 1, indisp = indibl + n, indifl = indisp + n, indiwo = indifl + n;
  const int llwork = lwork - indwk + 1;
  dsytrdL(n, a1, lda, &work[indd], &work[inde], &work[indtau], &work[indwk], llwork);
  if (ieeeok == 1) {
    dcopy(n - 1, &work[inde], 1, &work[indee], 1);
    dcopy(n, &work[indd], 1, &work[inddd], 1);
    bool tryrac = abstol <= two * n * eps;
    info = dstemrVA(n, &work[inddd], &work[indee], m, w1, z + 1 + ldz, ldz, n, isuppz + 1, tryrac,
                    &work[indwk], lwork, iwork + 1, liwork);
    if (info == 0) {
      const int indwkn = inde;
      const int llwrkn = lwork - indwkn + 1;
      dormtrLLN(n, m, a1, lda, &work[indtau], z + 1 + ldz, ldz, &work[indwkn], llwrkn);
    }
    if (info == 0) m = n;
  }
  if (!(ieeeok == 1 && info == 0)) {
    // DSTEMR failed (or arithmetic is not IEEE): DSTEBZ + DSTEIN on DSYTRD's
    // own D and E, then the same back-transformation.
    if (ieeeok == 1) info = 0;
    int nsplit = 0;
    info = dstebzAB(n, abstll, &work[indd], &work[inde], m, nsplit, w1, iwork + indibl,
                    iwork + indisp, &work[indwk], iwork + indiwo);
    info = dstein(n, &work[indd], &work[inde], m, w1, iwork + indibl, iwork + indisp, z + 1 + ldz,
                  ldz, &work[indwk], iwork + indiwo, iwork + indifl);
    const int indwkn = inde;
    const int llwrkn = lwork - indwkn + 1;
    dormtrLLN(n, m, a1, lda, &work[indtau], z + 1 + ldz, ldz, &work[indwkn], llwrkn);
  }
  // label 30
  if (iscale == 1) dscal(info == 0 ? m : info - 1, one / sigma, w1, 1);
  for (int j = 1; j <= m - 1; ++j) {
    int i = 0;
    double tmp1 = w[j];
    for (int jj = j + 1; jj <= m; ++jj) {
      if (w[jj] < tmp1) {
        i = jj;
        tmp1 = w[jj];
      }
    }
    if (i != 0) {
      w[i] = w[j];
      w[j] = tmp1;
      dswap(n, &Z_(1, i), 1, &Z_(1, j), 1);
    }
  }
  work[1] = lwkopt;
  iwork[1] = liwmin;
#undef Z_
  return info;
}

#undef A_

}  // namespace

int dsyevrVAL(int n, std::vector<double> a, double abstol, std::vector<double>& w,
              std::vector<double>& z) {
  if (n < 0 || a.size() < static_cast<size_t>(n) * static_cast<size_t>(n)) return -4;
  const int ld = n > 1 ? n : 1;
  w.assign(static_cast<size_t>(n > 0 ? n : 1), 0.0);
  z.assign(static_cast<size_t>(ld) * static_cast<size_t>(n > 0 ? n : 1), 0.0);
  std::vector<int> isuppz(static_cast<size_t>(2 * (n > 0 ? n : 1)), 0);
  // Workspace query (LWORK = LIWORK = -1), then the sizes it returns.
  double wq = 0.0;
  int iwq = 0, m = 0;
  int info = dsyevrVAL_(n, a.data(), ld, abstol, m, w.data(), z.data(), ld, isuppz.data(), &wq,
                        -1, &iwq, -1);
  if (info != 0) return info;
  const int lwork = static_cast<int>(wq), liwork = iwq;
  std::vector<double> work(static_cast<size_t>(lwork > 1 ? lwork : 1), 0.0);
  std::vector<int> iwork(static_cast<size_t>(liwork > 1 ? liwork : 1), 0);
  info = dsyevrVAL_(n, a.data(), ld, abstol, m, w.data(), z.data(), ld, isuppz.data(), work.data(),
                    lwork, iwork.data(), liwork);
  w.resize(static_cast<size_t>(n));
  z.resize(static_cast<size_t>(n) * static_cast<size_t>(n));
  return info;
}

}  // namespace piet::lapack
