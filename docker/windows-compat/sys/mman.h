/*
 * POSIX mmap/munmap declarations for Windows (MinGW-w64).
 * Backed by Win32 CreateFileMapping / MapViewOfFile.
 * Only the subset used by src/io/columnar.cpp is needed, but a general
 * implementation is provided for correctness.
 */
#ifndef CECON_MMAN_WIN32_H
#define CECON_MMAN_WIN32_H

#include <sys/types.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Protection flags */
#define PROT_NONE  0x00
#define PROT_READ  0x01
#define PROT_WRITE 0x02
#define PROT_EXEC  0x04

/* Mapping flags */
#define MAP_SHARED    0x01
#define MAP_PRIVATE   0x02
#define MAP_FIXED     0x10
#define MAP_ANONYMOUS 0x20
#define MAP_ANON      MAP_ANONYMOUS

#define MAP_FAILED ((void *)-1)

void *mmap(void *addr, size_t len, int prot, int flags, int fd, off_t offset);
int   munmap(void *addr, size_t len);
int   mprotect(void *addr, size_t len, int prot);

#ifdef __cplusplus
}
#endif

#endif /* CECON_MMAN_WIN32_H */
