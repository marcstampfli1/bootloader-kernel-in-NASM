// glibc_compat.c - symbols that PREBUILT glibc shared objects import.
//
// MakaOS's dlopen rejects a .so with any unresolvable strong-undefined symbol,
// so a prebuilt glibc native library (LWJGL's liblwjgl.so, and others) needs
// every symbol it imports defined here. These are the glibc-ABI names our libc
// did not already export: the *64 large-file aliases (identical to the base
// functions under LP64), the old __xstat/__fxstat stat ABI, __getdelim, and
// aligned_alloc. Follows the wrapper-.c convention: sysroot headers only.

#include <sys/mman.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/uio.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdarg.h>
#include <errno.h>

// ── Large-file (*64) aliases -- LP64: off_t is already 64-bit ────────────────
void* mmap64(void* addr, size_t len, int prot, int flags, int fd, off_t off) {
    return mmap(addr, len, prot, flags, fd, off);
}
int open64(const char* path, int flags, ...) {
    va_list ap; va_start(ap, flags); int mode = va_arg(ap, int); va_end(ap);
    return open(path, flags, mode);
}
int creat64(const char* path, mode_t mode) { return creat(path, mode); }

// openat: MakaOS has no SYS_OPENAT. AT_FDCWD or an absolute path routes to open;
// a real dirfd-relative open is unsupported.
int openat(int dirfd, const char* path, int flags, ...) {
    va_list ap; va_start(ap, flags); int mode = va_arg(ap, int); va_end(ap);
    if (dirfd == AT_FDCWD || (path && path[0] == '/')) return open(path, flags, mode);
    errno = ENOSYS; return -1;
}
int openat64(int dirfd, const char* path, int flags, ...) {
    va_list ap; va_start(ap, flags); int mode = va_arg(ap, int); va_end(ap);
    return openat(dirfd, path, flags, mode);
}

// ── Old glibc stat ABI: a version arg precedes (path/fd, buf) ────────────────
int __xstat64(int ver, const char* path, struct stat* buf)  { (void)ver; return stat(path, buf); }
int __fxstat64(int ver, int fd, struct stat* buf)           { (void)ver; return fstat(fd, buf); }

// ── aligned_alloc (C11) ─────────────────────────────────────────────────────
void* aligned_alloc(size_t alignment, size_t size) {
    void* p = 0;
    if (posix_memalign(&p, alignment, size) != 0) return 0;
    return p;
}

// ── preadv / pwritev (+ *64): emulate over pread/pwrite ─────────────────────
ssize_t preadv(int fd, const struct iovec* iov, int iovcnt, off_t off) {
    ssize_t total = 0;
    for (int i = 0; i < iovcnt; i++) {
        ssize_t r = pread(fd, iov[i].iov_base, iov[i].iov_len, off);
        if (r < 0) return total ? total : -1;
        total += r; off += r;
        if ((size_t)r < iov[i].iov_len) break;   // short read -> stop
    }
    return total;
}
ssize_t pwritev(int fd, const struct iovec* iov, int iovcnt, off_t off) {
    ssize_t total = 0;
    for (int i = 0; i < iovcnt; i++) {
        ssize_t w = pwrite(fd, iov[i].iov_base, iov[i].iov_len, off);
        if (w < 0) return total ? total : -1;
        total += w; off += w;
        if ((size_t)w < iov[i].iov_len) break;
    }
    return total;
}
ssize_t preadv64(int fd, const struct iovec* iov, int iovcnt, off_t off)  { return preadv(fd, iov, iovcnt, off); }
ssize_t pwritev64(int fd, const struct iovec* iov, int iovcnt, off_t off) { return pwritev(fd, iov, iovcnt, off); }

// ── getdelim / __getdelim ────────────────────────────────────────────────────
ssize_t getdelim(char** lineptr, size_t* n, int delim, FILE* stream) {
    if (!lineptr || !n || !stream) { errno = EINVAL; return -1; }
    if (!*lineptr || *n == 0) { *n = 128; *lineptr = (char*)malloc(*n); if (!*lineptr) return -1; }
    size_t pos = 0; int c;
    while ((c = fgetc(stream)) != EOF) {
        if (pos + 1 >= *n) {
            size_t nn = *n * 2;
            char* np = (char*)realloc(*lineptr, nn);
            if (!np) return -1;
            *lineptr = np; *n = nn;
        }
        (*lineptr)[pos++] = (char)c;
        if (c == delim) break;
    }
    if (pos == 0 && c == EOF) return -1;
    (*lineptr)[pos] = '\0';
    return (ssize_t)pos;
}
ssize_t __getdelim(char** lineptr, size_t* n, int delim, FILE* stream) {
    return getdelim(lineptr, n, delim, stream);
}

// ── process_vm_readv/writev: no cross-process memory access ─────────────────
ssize_t process_vm_readv(pid_t pid, const struct iovec* lvec, unsigned long liovcnt,
                         const struct iovec* rvec, unsigned long riovcnt, unsigned long flags) {
    (void)pid; (void)lvec; (void)liovcnt; (void)rvec; (void)riovcnt; (void)flags;
    errno = ENOSYS; return -1;
}
ssize_t process_vm_writev(pid_t pid, const struct iovec* lvec, unsigned long liovcnt,
                          const struct iovec* rvec, unsigned long riovcnt, unsigned long flags) {
    (void)pid; (void)lvec; (void)liovcnt; (void)rvec; (void)riovcnt; (void)flags;
    errno = ENOSYS; return -1;
}
