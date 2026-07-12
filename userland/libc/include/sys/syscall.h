// sys/syscall.h -- conventional home of the SYS_* numbers for syscall(2).
// Deliberately MINIMAL: it does NOT pull in <makaos/syscall.h>, whose inline
// `asm(...)` wrappers use the bare `asm` keyword and break under the strict
// -std=c11 that ported third-party code (Mesa) compiles with.  Portable code
// guards every raw syscall behind `#ifdef SYS_<name>`, so leaving the Linux-
// only numbers undefined here simply routes those callers to their fallbacks.
// Add specific plain-integer SYS_<name> defines below only if a consumer needs
// one AND MakaOS actually implements it.
#ifndef _SYS_SYSCALL_H
#define _SYS_SYSCALL_H

#include <unistd.h>   // syscall() prototype

// Linux syscall-number TAGS recognised by libc's syscall() compat shim (which
// maps them to MakaOS libc calls, NOT to raw kernel numbers -- MakaOS is
// source- but not ABI-number-compatible). Values match Linux/x86-64 so ported
// code that hardcodes them works. Add a tag here only when the shim handles it.
#define SYS_gettid 186
#define SYS_futex  202

#endif // _SYS_SYSCALL_H
