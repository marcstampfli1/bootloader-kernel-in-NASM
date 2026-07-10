/*
 * dltest -- exercises the real dlopen loader end to end.  Headless serial can't
 * see stdout, so the result is signalled via a sentinel page fault whose low 12
 * bits encode independent checks; the PF-KILL dump prints CR2 with comm=dltest.
 * Base 0x5EC00000 has a clean low 16 bits so every bit is a real signal.
 * Full pass => CR2=0x000000005EC00FFF.
 *
 *   bit0  dlopen mapped + relocated /lib/libdso.so
 *   bit1  dlsym found dso_answer
 *   bit2  calling into the .so returns 42
 *   bit3  the .so's own RELATIVE reloc applied (dso_name() == "libdso")
 *   bit4  dlclose returned 0
 *   bit5  open(/etc/passwd) succeeded (deny-by-default test is live)
 *   bit6  SECURITY (ASVS V4): PROT_EXEC mmap on the non-exec passwd is REFUSED
 *   bit7  the .so's libc import (strlen) bound to the exe's exported .dynsym
 *   bit8  a WEAK undefined symbol resolves to 0 (dlopen still succeeds)
 *   bit9  a .so importing a STRONG undefined symbol is REJECTED + cleaned up
 *   bit10 DT_NEEDED: libdso.so pulls in libdso2.so; dso_dep() == 100
 *   bit11 dlopen(NULL) global scope: dlsym finds the exe's strlen
 */
extern void* dlopen(const char*, int);
extern void* dlsym(void*, const char*);
extern int   dlclose(void*);
extern int   open(const char*, int, ...);
extern void* mmap(void*, unsigned long, int, int, int, long);

int main(void) {
    unsigned long r = 0x5EC00000UL;

    void* h = dlopen("/lib/libdso.so", 2 /*RTLD_NOW*/);
    if (h) {
        r |= 0x001UL;
        int (*answer)(void) = (int (*)(void))dlsym(h, "dso_answer");
        if (answer) r |= 0x002UL;
        if (answer && answer() == 42) r |= 0x004UL;
        const char* (*nm)(void) = (const char* (*)(void))dlsym(h, "dso_name");
        if (nm) { const char* n = nm(); if (n && n[0]=='l' && n[3]=='d' && n[5]=='o') r |= 0x008UL; }
        int (*dslen)(void) = (int (*)(void))dlsym(h, "dso_strlen");
        if (dslen && dslen() == 4) r |= 0x080UL;               /* libc import bound to exe */
        void* (*wk)(void) = (void* (*)(void))dlsym(h, "dso_weak");
        if (wk && wk() == (void*)0) r |= 0x100UL;              /* weak undefined -> 0 */
        int (*dep)(void) = (int (*)(void))dlsym(h, "dso_dep");
        if (dep && dep() == 100) r |= 0x400UL;                 /* DT_NEEDED dep called */
        if (dlclose(h) == 0) r |= 0x010UL;
    }

    /* SECURITY: PROT_EXEC on a non-executable file must be refused (ASVS V4). */
    int pf = open("/etc/passwd", 0 /*O_RDONLY*/, 0);
    if (pf >= 0) {
        r |= 0x020UL;
        void* m = mmap((void*)0, 4096, 1 /*PROT_READ*/ | 4 /*PROT_EXEC*/, 2 /*MAP_PRIVATE*/, pf, 0);
        if (m == (void*)-1) r |= 0x040UL;
    }

    /* reject: a .so with a strong undefined import must fail (return NULL). */
    if (dlopen("/lib/libdsobad.so", 2) == (void*)0) r |= 0x200UL;

    /* dlopen(NULL): the global scope resolves the exe's exported symbols. */
    void* g = dlopen((const char*)0, 2);
    if (g && dlsym(g, "strlen") != (void*)0) r |= 0x800UL;

    *(volatile unsigned char*)r = 0;   /* PF-KILL: CR2=0x5EC00FFF on full pass */
    return 0;
}
