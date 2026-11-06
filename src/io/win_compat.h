// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
#pragma once
// Windows file-I/O portability shims for the cobra engine (and reused by
// piet/native, which has src/ on its include path). Header-only and written so
// POSIX builds are BYTE-IDENTICAL: to_native_path() forwards to
// std::filesystem::u8path (same bytes on a UTF-8 filesystem) and the native_*
// helpers forward to the same POSIX calls they replaced (::open, ::unlink,
// ::stat, std::rename, std::fopen, ::getpid); wide Win32/CRT operations exist
// only under _WIN32.
//
// Solves two Windows-only problems that a macOS/Linux cross-compile could never
// surface:
//   1. Non-ASCII (Korean) paths — BUG-050. A UTF-8 std::string opened through a
//      narrow ANSI API is decoded as the active codepage (CP949 on KR Windows),
//      so the real UTF-16 file is never found. to_native_path() / native_open()
//      decode UTF-8 -> UTF-16 and open wide.
//   2. MinGW-w64 has no pread/pwrite — src/data/frame.cpp's patch_hbin() calls
//      ::pread/::pwrite, which fails to even compile natively. Supplied here.
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <system_error>

namespace cecon {

// Build a std::filesystem::path from a UTF-8 path string. On Windows this
// decodes UTF-8 -> UTF-16 so a Korean path resolves to the real file; on POSIX
// u8path copies the bytes verbatim, identical to the prior std::string open (no
// behavior change). std::filesystem::u8path is C++17 (deprecated-in-C++20; the
// build already passes -Wno-deprecated-declarations).
inline std::filesystem::path to_native_path(const std::string& utf8) {
    return std::filesystem::u8path(utf8);
}

// Inverse of to_native_path(): a native path back as UTF-8 bytes. On Windows the
// UTF-16 path is re-encoded to UTF-8; on POSIX u8string() hands back the same
// bytes to_native_path() was given (no behavior change).
inline std::string from_native_path(const std::filesystem::path& path) {
    auto u8 = path.u8string();
    return std::string(u8.begin(), u8.end());
}

// Join a directory and a leaf into a UTF-8 path using the platform separator.
// Replaces the hand-rolled `dir + "/" + leaf` concatenations, which produce a
// mixed-separator path on Windows.
inline std::string native_join_path(const std::string& dir, const std::string& leaf) {
    return from_native_path(to_native_path(dir) / to_native_path(leaf));
}

// A scratch path under the platform temp directory. POSIX keeps the historical
// hard-coded /tmp; Windows resolves %TMP%/%TEMP% (wide, via std::filesystem) so a
// Korean username does not land the temp root in the ANSI codepage.
inline std::string native_temp_path(const std::string& leaf) {
#ifdef _WIN32
    std::error_code ec;
    std::filesystem::path dir = std::filesystem::temp_directory_path(ec);
    if (ec) dir = std::filesystem::path(".");
    return from_native_path(dir / to_native_path(leaf));
#else
    return native_join_path("/tmp", leaf);
#endif
}

}  // namespace cecon

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>   // ReplaceFileW, CreateFileW
#include <fcntl.h>     // _O_BINARY and the O_* flag aliases
#include <io.h>        // _wopen, _wunlink, _lseeki64, _read, _write, _setmode
#include <process.h>   // _getpid
#include <sys/stat.h>  // _fstati64
#include <sys/types.h> // ssize_t

namespace cecon {

// Open a file descriptor by UTF-8 path, wide so non-ASCII paths resolve. Force
// binary — every fd our engine opens (hbin/dta/csv) is binary; text-mode CRLF
// translation would corrupt reads/writes.
inline int native_open(const std::string& utf8, int flags, int mode = 0) {
    // Dataset mappings must not pin the source directory entry against an
    // atomic save,replace. The CRT's default sharing omits FILE_SHARE_DELETE;
    // open read-only inputs with it explicitly, then wrap the owned handle in
    // a CRT fd for the existing mmap/read/close call sites.
    if ((flags & (_O_WRONLY | _O_RDWR | _O_CREAT | _O_TRUNC | _O_APPEND)) == 0) {
        HANDLE handle = ::CreateFileW(to_native_path(utf8).c_str(), GENERIC_READ,
                                      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                      nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (handle == INVALID_HANDLE_VALUE) return -1;
        int fd = ::_open_osfhandle(reinterpret_cast<intptr_t>(handle), _O_RDONLY | _O_BINARY);
        if (fd < 0) ::CloseHandle(handle);
        return fd;
    }
    return ::_wopen(to_native_path(utf8).c_str(), flags | _O_BINARY, mode);
}

// Existence test by UTF-8 path, wide (_waccess) so a non-ASCII path is not
// misread through the ANSI codepage — the narrow access() gate is what actually
// emits BUG-050's "File not found" before any open is even attempted.
inline bool native_exists(const std::string& utf8) {
    return ::_waccess(to_native_path(utf8).c_str(), 0) == 0;
}

// Remove by UTF-8 path, wide. Same return convention as ::unlink (0 on success).
inline int native_unlink(const std::string& utf8) {
    return ::_wunlink(to_native_path(utf8).c_str());
}

// Same-directory replacement is one ReplaceFileW operation. In particular, do
// not emulate POSIX rename by deleting the destination first: a failed replace
// must leave the original save target present.
inline bool native_replace_file(const std::string& replacement, const std::string& destination) {
    return ::ReplaceFileW(to_native_path(destination).c_str(),
                          to_native_path(replacement).c_str(), nullptr,
                          REPLACEFILE_WRITE_THROUGH, nullptr, nullptr) != 0;
}

// std::fopen by UTF-8 path, wide. Callers pass the usual narrow mode string
// (ASCII), widened here; add "b" yourself — this does NOT force binary, because
// unlike native_open() some callers legitimately want text mode.
inline std::FILE* native_fopen(const std::string& utf8, const char* mode) {
    std::wstring wmode(mode, mode + std::char_traits<char>::length(mode));
    return ::_wfopen(to_native_path(utf8).c_str(), wmode.c_str());
}

// Process id for temp-file uniquing. long (not pid_t) so both platforms agree.
inline long native_process_id() {
    return static_cast<long>(::_getpid());
}

// Size of an OPEN fd. _fstati64 (not fstat) so a >2 GiB .hbin is not truncated
// by MinGW's 32-bit unsuffixed off_t.
inline bool native_fd_size(int fd, int64_t& size) {
    struct _stati64 st {};
    if (::_fstati64(fd, &st) != 0) return false;
    size = static_cast<int64_t>(st.st_size);
    return true;
}

// (size, mtime_ns) by UTF-8 path. Routed through native_open() rather than a
// wide _wstati64 so the FILE_SHARE_DELETE sharing above applies here too — the
// memo stamp must not pin a file another process is replacing. Windows st_mtime
// is whole-seconds only, so the ns field is second-granular there.
inline bool native_file_stamp(const std::string& utf8, int64_t& size, int64_t& mtime_ns) {
    int fd = native_open(utf8, O_RDONLY);
    if (fd < 0) return false;
    struct _stati64 st {};
    const int rc = ::_fstati64(fd, &st);
    ::_close(fd);
    if (rc != 0) return false;
    size = static_cast<int64_t>(st.st_size);
    mtime_ns = static_cast<int64_t>(st.st_mtime) * 1000000000LL;
    return true;
}

// Put stdin/stdout/stderr in binary mode. The Windows CRT otherwise translates
// LF -> CRLF on console/pipe descriptors, which would (a) corrupt the serve
// line-JSON protocol against the byte counts the client expects and (b) make
// the fd-level capture files (read back as binary) disagree with what was
// written. The port is validated by BYTE-IDENTICAL fixture comparison, so this
// is not cosmetic.
inline void native_set_standard_streams_binary() {
    ::_setmode(::_fileno(stdin), _O_BINARY);
    ::_setmode(::_fileno(stdout), _O_BINARY);
    ::_setmode(::_fileno(stderr), _O_BINARY);
}

// Read an environment variable as UTF-8. On a Korean-username machine
// %USERPROFILE% / %TEMP% / %APPDATA% hold Hangul; narrow getenv returns those
// bytes in the ANSI codepage (CP949), and feeding CP949 bytes into u8path /
// CreateProcessW-as-UTF-8 mis-decodes every ~-relative, cwd and scratch path.
// Read WIDE (_wgetenv, a plain CRT call) and hand back UTF-8 — the one encoding
// the rest of piet uses. (env NAMES are ASCII.)
inline std::string env_utf8(const char* name) {
    std::wstring wname(name, name + std::char_traits<char>::length(name));
    const wchar_t* w = ::_wgetenv(wname.c_str());
    if (w == nullptr) return std::string();
    auto u8 = std::filesystem::path(w).u8string();
    return std::string(u8.begin(), u8.end());
}

}  // namespace cecon

// MinGW-w64 provides no pread/pwrite; frame.cpp's patch_hbin() calls ::pread /
// ::pwrite. Emulate positionally with seek + _read/_write and restore the fd
// offset (engine use is single-threaded — the fd is never shared across
// threads, so the transient seek is invisible).
inline ssize_t pread(int fd, void* buf, size_t n, long long off) {
    long long cur = _lseeki64(fd, 0, SEEK_CUR);
    if (cur < 0) return -1;
    if (_lseeki64(fd, off, SEEK_SET) < 0) return -1;
    int r = _read(fd, buf, static_cast<unsigned>(n));
    _lseeki64(fd, cur, SEEK_SET);
    return r;
}
inline ssize_t pwrite(int fd, const void* buf, size_t n, long long off) {
    long long cur = _lseeki64(fd, 0, SEEK_CUR);
    if (cur < 0) return -1;
    if (_lseeki64(fd, off, SEEK_SET) < 0) return -1;
    int r = _write(fd, buf, static_cast<unsigned>(n));
    _lseeki64(fd, cur, SEEK_SET);
    return r;
}

#else  // POSIX — byte-identical passthrough
#include <fcntl.h>     // ::open
#include <sys/stat.h>  // ::stat, ::fstat
#include <unistd.h>    // ::access, ::unlink, ::getpid, F_OK

namespace cecon {

inline int native_open(const std::string& path, int flags, int mode = 0) {
    return ::open(path.c_str(), flags, mode);
}

inline bool native_exists(const std::string& path) {
    return ::access(path.c_str(), F_OK) == 0;
}

inline int native_unlink(const std::string& path) {
    return ::unlink(path.c_str());
}

inline bool native_replace_file(const std::string& replacement, const std::string& destination) {
    return std::rename(replacement.c_str(), destination.c_str()) == 0;
}

inline std::FILE* native_fopen(const std::string& path, const char* mode) {
    return std::fopen(path.c_str(), mode);
}

inline long native_process_id() {
    return static_cast<long>(::getpid());
}

inline bool native_fd_size(int fd, int64_t& size) {
    struct stat st {};
    if (::fstat(fd, &st) != 0) return false;
    size = static_cast<int64_t>(st.st_size);
    return true;
}

inline bool native_file_stamp(const std::string& path, int64_t& size, int64_t& mtime_ns) {
    struct stat st {};
    if (::stat(path.c_str(), &st) != 0) return false;
    size = static_cast<int64_t>(st.st_size);
#if defined(__APPLE__)
    mtime_ns = static_cast<int64_t>(st.st_mtimespec.tv_sec) * 1000000000LL +
               st.st_mtimespec.tv_nsec;
#else
    mtime_ns = static_cast<int64_t>(st.st_mtim.tv_sec) * 1000000000LL +
               st.st_mtim.tv_nsec;
#endif
    return true;
}

inline void native_set_standard_streams_binary() {}

inline std::string env_utf8(const char* name) {
    const char* v = std::getenv(name);
    return v ? std::string(v) : std::string();
}

}  // namespace cecon
#endif
