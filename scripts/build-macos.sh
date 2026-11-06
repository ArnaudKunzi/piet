#!/bin/sh
# Build the CoBrA engine and the PIET native app from a fresh clone on macOS,
# then run the engine unit tests and the self-contained public smoke test.
#
# Requirements: Xcode Command Line Tools (C++17), CMake 3.20+, Python 3.
# `piet_native --selftest` is the maintainers' suite; most of its arms read
# private oracle fixtures that the public tree does not carry, so it is not
# part of this script (see README "Build").
set -eu
cd "$(dirname "$0")/.."
source_root=$(pwd -P)
# Keep the builder's local path out of the binaries (including __FILE__).
path_map="-ffile-prefix-map=$source_root=."
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DCMAKE_OSX_DEPLOYMENT_TARGET=13.3 "-DCMAKE_CXX_FLAGS=$path_map"
cmake --build build --parallel "${BUILD_JOBS:-2}"
cmake -S piet/native -B piet/native/build -DCMAKE_BUILD_TYPE=Release -DCMAKE_OSX_DEPLOYMENT_TARGET=13.3 "-DCMAKE_CXX_FLAGS=$path_map"
cmake --build piet/native/build --parallel "${BUILD_JOBS:-2}"
ctest --test-dir build --output-on-failure
python3 tests/public_smoke.py
