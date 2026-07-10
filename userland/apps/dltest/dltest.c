/*
 * dltest -- Phase 2 milestone: a PIE that uses the real dlopen loader.
 *
 * It dlopen()s /lib/libdso.so, dlsym()s two functions, calls them, and checks
 * the results -- exercising the whole loader: segment mmap, RELATIVE
 * relocation, SysV-hash symbol lookup, and dlclose.
 *
 * Headless serial can't see stdout, so the result is signalled via a sentinel
 * page fault whose low byte encodes the checks -- PF-KILL prints
 * CR2=0x000000005EC0DEFF with comm=dltest on full pass (bit7 = the .so's libc
 * import bound to the exe's exported strlen):
 *   bit0 dlopen mapped+relocated the .so    bit1 dlsym found dso_answer
 *   bit2 calling into the .so returns 42     bit3 the .so's own RELATIVE reloc
 *        applied (dso_name() returns "libdso")
 *   bit4 dlclose returned
 *   bit5 SECURITY (ASVS V4 deny-by-default): a NON-executable file (/etc/passwd,
 *        mode 0644) is correctly REFUSED an mmap PROT_EXEC -- the exec-mmap right
 *        is granted only for files the caller may execute, so this must fail.
 *   bit7 the .so's libc import (strlen) bound to the exe's exported .dynsym
 *   bit8 loader hardening: a WEAK undefined symbol resolves to 0 (dlopen still
 *        succeeds), so dso_weak() returns NULL
 *   bit9 loader hardening: a .so importing a STRONG undefined symbol is REJECTED
 *        (dlopen returns NULL) and cleaned up, not half-loaded
 * Full pass: CR2=0x000000005EC0DFFF.
 */
extern void* dlopen(const char*, int);
extern void* dlsym(void*, const char*);
extern int   dlclose(void*);
extern int   open(const char*, int, ...);
extern void* mmap(void*, unsigned long, int, int, int, long);

int main(void) {
    unsigned long r = 0x5EC0DE00UL;
    void* h = dlopen("/lib/libdso.so", 2 /*RTLD_NOW*/);
    if (h) {
        r |= 0x01UL;
        int (*answer)(void) = (int (*)(void))dlsym(h, "dso_answer");
        if (answer) r |= 0x02UL;
        if (answer && answer() == 42) r |= 0x04UL;
        const char* (*nm)(void) = (const char* (*)(void))dlsym(h, "dso_name");
        if (nm) { const char* n = nm(); if (n && n[0] == 'l' && n[3] == 'd' && n[5] == 'o') r |= 0x08UL; }
        /* bit7: the .so imports libc strlen, bound against the exe's exported
         * .dynsym (milestone 2); dso_strlen() must return strlen("abcd") == 4. */
        int (*dslen)(void) = (int (*)(void))dlsym(h, "dso_strlen");
        if (dslen && dslen() == 4) r |= 0x80UL;
        /* bit8: a weak-undefined symbol resolved to 0, dlopen still succeeded. */
        void* (*wk)(void) = (void* (*)(void))dlsym(h, "dso_weak");
        if (wk && wk() == (void*)0) r |= 0x100UL;
        if (dlclose(h) == 0) r |= 0x10UL;
    }
    /* deny-by-default check: PROT_EXEC on a non-executable file must be refused. */
    int pf = open("/etc/passwd", 0 /*O_RDONLY*/, 0);
    if (pf >= 0) {
        r |= 0x20UL;                        /* bit5: open succeeded (test is live) */
        void* m = mmap((void*)0, 4096, 1 /*PROT_READ*/ | 4 /*PROT_EXEC*/,
                       2 /*MAP_PRIVATE*/, pf, 0);
        if (m == (void*)-1) r |= 0x40UL;    /* bit6: PROT_EXEC correctly DENIED */
    }
    /* bit9: a .so with a strong undefined import must be REJECTED (returns NULL). */
    void* hb = dlopen("/lib/libdsobad.so", 2 /*RTLD_NOW*/);
    if (hb == (void*)0) r |= 0x200UL;
    *(volatile unsigned char*)r = 0;   /* PF-KILL: CR2=0x5EC0DFFF on full pass */
    return 0;
}
