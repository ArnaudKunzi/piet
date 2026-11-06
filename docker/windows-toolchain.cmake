# CMake toolchain: cross-compile to Windows x64 using MinGW-w64.
# Tested on Debian 12 (g++-mingw-w64-x86-64 package).
set(CMAKE_SYSTEM_NAME    Windows)
set(CMAKE_SYSTEM_PROCESSOR x86_64)

set(CMAKE_C_COMPILER   x86_64-w64-mingw32-gcc)
set(CMAKE_CXX_COMPILER x86_64-w64-mingw32-g++)
set(CMAKE_RC_COMPILER  x86_64-w64-mingw32-windres)

# Restrict library/header search to the MinGW sysroot so CMake never picks
# up host (Linux) libraries by accident.
# Debian: /usr/x86_64-w64-mingw32
# Fedora: /usr/x86_64-w64-mingw32/sys-root/mingw  (both listed; CMake tries in order)
set(CMAKE_FIND_ROOT_PATH
    /usr/x86_64-w64-mingw32
    /usr/x86_64-w64-mingw32/sys-root/mingw)
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
