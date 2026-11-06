# piet/packaging/licenses/windows/ — Windows-bundle-only notice parts

**Owner of this subdirectory: Porting Op (WP-W6).** These three files are the
"activation" of the `../staged/` placeholders for the Windows static build,
per `../staged/README.md`'s instruction ("at the first static Linux/Windows
build, copy the verbatim license text into a top-level NN-*.txt file").

## Why a separate subdirectory instead of the shared top-level `licenses/`

`piet/packaging/package_native_app.sh` (the macOS packager, Conductor-owned)
concatenates **every** `licenses/NN-*.txt` file — unconditionally — into the
shipped `THIRD_PARTY_NOTICES.txt`. The macOS build links **Apple Accelerate**
(no OpenBLAS) and is compiled with **clang** (no GCC/MinGW runtime, no
winpthreads). Dropping these three files into the shared top level would make
every macOS bundle falsely claim components it does not actually ship.

So: these three files are **activated but scoped** — they belong in the
generated notices for a *Windows* bundle only. The intended concatenation
order for a Windows `THIRD_PARTY_NOTICES.txt` is:

```
../00-ABOUT-AND-DISCLAIMER.txt      (shared: StataCorp disclaimer)
../10-webview-MIT.txt               (shared: webview/webview, vendored on Windows too)
../20-nlohmann-json-MIT.txt         (shared: nlohmann/json, vendored on Windows too)
./10-openblas-BSD-3.txt             (Windows-only: cobra.exe links OpenBLAS statically)
./20-gcc-runtime-exception.txt      (Windows-only: cobra.exe + piet_native.exe are
                                      static-linked by gcc-mingw-w64 — libgcc/
                                      libstdc++/libgfortran/libquadmath)
./30-mingw-winpthreads.txt          (Windows-only: posix threading model —
                                      winpthreads backs <pthread.h>/std::thread)
../30-DATA-SOURCES.txt              (shared: Fatalities sample dataset)
```

A future Windows packaging/assembly script (the WP-W6 analogue of
`package_native_app.sh` — not yet written; see
`piet/packaging/piet_setup.iss`'s header comment) should concatenate exactly
this shared-plus-windows set, mirroring the mac script's lexical-order
`NN-*.txt` concatenation but scoped to `../*.txt` + `./*.txt`.

**Coordinate before generalizing the shared generator.** If Conductor wants
one shared, platform-aware `THIRD_PARTY_NOTICES.txt` generator instead of two
separate concatenation lists, that is a `package_native_app.sh` change
(Conductor-owned) — flagged, not made here, to avoid silently changing what
ships in the macOS bundle. See porting-op → piet-conductor messenger note,
WP-W6 (2026-07-08).

## Provenance (toolchain versions actually linked, per the Porting Op Docker
## recipes — re-verify these at the next OpenBLAS/MinGW version bump)

| File | Component | Exact version verified against |
|---|---|---|
| `10-openblas-BSD-3.txt` | OpenBLAS | `v0.3.28` tag (`docker/Dockerfile.windows`'s `OPENBLAS_TAG`) — LICENSE fetched from the tagged source tree |
| `20-gcc-runtime-exception.txt` | GCC runtime libs (libgcc/libstdc++/libgfortran/libquadmath) | Debian 12 `gcc-mingw-w64-base` 12.2.0-14+deb12u1+25.2+b1 (posix variant) — verbatim from that package's `/usr/share/doc/gcc-mingw-w64-base/copyright` |
| `30-mingw-winpthreads.txt` | MinGW-w64 winpthreads | Debian 12 `mingw-w64-x86-64-dev` 10.0.0-3 — verbatim from that package's `/usr/share/doc/mingw-w64-x86-64-dev/copyright` (winpthreads paragraphs) |
