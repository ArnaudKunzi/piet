/*
 * POSIX mmap / munmap / mprotect for Windows via Win32 file mapping API.
 * Used by src/io/columnar.cpp which maps .hbin files read-only.
 *
 * cobra usage pattern: mmap(NULL, len, PROT_READ, MAP_PRIVATE, fd, 0)
 * where fd is a CRT file descriptor opened with open(path, O_RDONLY).
 */
#include "sys/mman.h"
#include <windows.h>
#include <io.h>      /* _get_osfhandle */
#include <errno.h>
#include <stdint.h>

static DWORD page_flags(int prot) {
    if (prot & PROT_EXEC) {
        if (prot & PROT_WRITE) return PAGE_EXECUTE_READWRITE;
        return PAGE_EXECUTE_READ;
    }
    if (prot & PROT_WRITE) return PAGE_READWRITE;
    if (prot & PROT_READ)  return PAGE_READONLY;
    return PAGE_NOACCESS;
}

static DWORD view_flags(int prot, int flags) {
    if (prot & PROT_WRITE) {
        if (flags & MAP_PRIVATE) return FILE_MAP_COPY;
        return FILE_MAP_WRITE;
    }
    if (prot & PROT_READ) return FILE_MAP_READ;
    return 0;
}

void *mmap(void *addr, size_t len, int prot, int flags, int fd, off_t offset) {
    (void)addr; /* address hints not supported */

    if (len == 0) { errno = EINVAL; return MAP_FAILED; }

    HANDLE fh;
    if (fd == -1) {
        /* anonymous mapping */
        fh = INVALID_HANDLE_VALUE;
    } else {
        fh = (HANDLE)_get_osfhandle(fd);
        if (fh == INVALID_HANDLE_VALUE) { errno = EBADF; return MAP_FAILED; }
    }

    /* CreateFileMapping size = offset + len (cover the whole mapped range). */
    uint64_t map_end = (uint64_t)(uint64_t)offset + (uint64_t)len;
    HANDLE hmap = CreateFileMapping(fh, NULL, page_flags(prot),
                                    (DWORD)(map_end >> 32),
                                    (DWORD)(map_end & 0xFFFFFFFFU),
                                    NULL);
    if (!hmap) { errno = ENOMEM; return MAP_FAILED; }

    void *ptr = MapViewOfFile(hmap,
                              view_flags(prot, flags),
                              (DWORD)((uint64_t)offset >> 32),
                              (DWORD)((uint64_t)offset & 0xFFFFFFFFU),
                              len);
    CloseHandle(hmap);
    if (!ptr) { errno = ENOMEM; return MAP_FAILED; }
    return ptr;
}

int munmap(void *addr, size_t len) {
    (void)len;
    if (!UnmapViewOfFile(addr)) { errno = EINVAL; return -1; }
    return 0;
}

int mprotect(void *addr, size_t len, int prot) {
    DWORD old;
    if (!VirtualProtect(addr, len, page_flags(prot), &old)) {
        errno = EACCES; return -1;
    }
    return 0;
}
