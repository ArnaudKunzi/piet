// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// Portions derived from LAPACK 3.12.0 (BSD-3-Clause). See THIRD_PARTY_NOTICES.
//
// DSYEVR(JOBZ='V', RANGE='A', UPLO='L') from Netlib LAPACK 3.12.0, translated
// routine by routine (lapack_dsyevr.cpp). Mata's symeigensystem() in StataBE
// 19.5 is this routine from the bundled liblapack.3.12.0.dylib (gfortran,
// arm64, reference BLAS); the translation reproduces that build's bits,
// fused multiply-adds included.
#pragma once

#include <vector>

namespace piet::lapack {

// All eigenvalues and eigenvectors of the symmetric n x n matrix `a`
// (column-major, a[i + j*n]; only the lower triangle is read, as UPLO='L').
// On success returns 0 with `w` ascending (LAPACK order) and `z` the
// column-major eigenvectors (column j belongs to w[j]). Workspace is the
// size DSYEVR's own workspace query returns (LWORK=-1), as LAPACKE does.
// A non-zero return is DSYEVR's INFO (an eigenvector DSTEIN could not
// converge after a DSTEMR failure, or an invalid argument).
int dsyevrVAL(int n, std::vector<double> a, double abstol,
              std::vector<double>& w, std::vector<double>& z);

}  // namespace piet::lapack
