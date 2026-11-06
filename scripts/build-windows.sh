#!/bin/sh
# Build the CoBrA engine and the PIET native app from a fresh clone ON Windows,
# then run the engine unit tests and the self-contained public smoke test. This
# is the Windows counterpart of scripts/build-macos.sh.
#
# Run it from an MSYS2 MINGW64 shell, with:
#   pacman -S --needed mingw-w64-x86_64-gcc mingw-w64-x86_64-gcc-fortran \
#     mingw-w64-x86_64-cmake mingw-w64-x86_64-ninja mingw-w64-x86_64-openblas \
#     mingw-w64-x86_64-libiconv mingw-w64-x86_64-python
#
# Both executables link statically: the only DLLs they load are parts of Windows
# itself. WebView2 is the one runtime the app needs from the machine; the
# installer (piet/packaging/piet_setup.iss) takes care of it.
#
# Environment:
#   BUILD_JOBS   parallel compile jobs (default 2)
#   SKIP_TESTS   set to 1 to build without running ctest and the smoke test
set -eu
cd "$(dirname "$0")/.."

case "${MSYSTEM:-}" in
  MINGW64) ;;
  *) echo "FATAL: run this from an MSYS2 MINGW64 shell (MSYSTEM is '${MSYSTEM:-unset}')" >&2; exit 1 ;;
esac

# The C:/... spelling, not MSYS's /c/...: CMake, Ninja and GCC are native Windows
# programs, and __FILE__ carries this form, so -ffile-prefix-map must match it.
source_root=$(pwd -W)

jobs="${BUILD_JOBS:-2}"
# Keep the builder's local path out of the binaries (including __FILE__).
path_map="-ffile-prefix-map=$source_root=."
compat="$source_root/docker/windows-compat"

# The mmap shim columnar.cpp links against: MinGW-w64 has no <sys/mman.h>. It is
# built with the same _FILE_OFFSET_BITS as the engine, because mmap takes an
# off_t and MinGW's off_t changes width with that define.
mkdir -p build-compat
gcc -O2 -D_FILE_OFFSET_BITS=64 -I"$compat" -c "$compat/mman.c" -o build-compat/mman.o
ar rcs build-compat/libmman_compat.a build-compat/mman.o
mman_lib="$source_root/build-compat/libmman_compat.a"

# Static BLAS/LAPACK and the runtimes it needs. MSYS2's libopenblas.a selects its
# kernels for the running CPU at startup and is threaded with OpenMP.
need_lib() {
  found=$(gcc -print-file-name="$1")
  if [ ! -f "$found" ]; then
    echo "FATAL: $1 not found by gcc; see the pacman line at the top of this script" >&2
    exit 1
  fi
  printf '%s' "$found"
}
la_libs="$(need_lib libopenblas.a);$mman_lib;$(need_lib libgfortran.a);$(need_lib libgomp.a)"
openblas_include=$(cygpath -m /mingw64/include/openblas)
# Newer GCC folds libquadmath into libgfortran, so it is optional here.
quadmath=$(gcc -print-file-name=libquadmath.a)
if [ -f "$quadmath" ]; then
  la_libs="$la_libs;$quadmath"
fi

# x86-64-v2 (SSE4.2, 2009 onward) is the documented Windows floor: the engine's
# default of v3 would crash with an illegal instruction on a CPU without AVX2.
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DCECON_X86_ARCH=x86-64-v2 \
  "-DCECON_STATIC_LIBS=$la_libs" \
  "-DCMAKE_CXX_FLAGS=-I$compat -I$openblas_include $path_map" \
  -DCMAKE_EXE_LINKER_FLAGS=-static
cmake --build build --parallel "$jobs"

# Name the static libiconv outright: left to itself, FindIconv picks the import
# library, and the app would then need libiconv-2.dll on every user's machine.
cmake -S piet/native -B piet/native/build -G Ninja -DCMAKE_BUILD_TYPE=Release \
  "-DIconv_LIBRARY=$(need_lib libiconv.a)" \
  "-DCMAKE_CXX_FLAGS=-I$compat $path_map" \
  -DCMAKE_EXE_LINKER_FLAGS=-static \
  "-DCMAKE_CXX_STANDARD_LIBRARIES=$mman_lib"
cmake --build piet/native/build --parallel "$jobs"

if [ "${SKIP_TESTS:-0}" = 1 ]; then
  exit 0
fi
# The whole suite takes seconds, so a test still running after five minutes is hung.
ctest --test-dir build --output-on-failure --timeout 300
python tests/public_smoke.py
