/*
 * pietest -- minimal PIE self-relocation smoke test (Phase 0/1 of the dynamic
 * loader).  Freestanding, no libc.  A .data pointer -> .rodata forces an
 * R_X86_64_RELATIVE the kernel ELF loader must fix up; if it doesn't, write(2)
 * gets a base-0 pointer and prints garbage (or faults).  Prints the marker and
 * exits 42, so a headless serial run confirms: ET_DYN loaded at a bias, RELATIVE
 * relocs applied, entry reached, syscalls work.
 *
 * Built with: cc -fPIC -ffreestanding -c ; ld -pie --export-dynamic -e _start
 */
static const char msg[] = "PIE-RELOC-OK\n";
static const char *volatile pmsg = msg;   /* absolute ptr -> R_X86_64_RELATIVE */

static long sc3(long n, long a, long b, long c) {
    long r;
    __asm__ volatile("syscall" : "=a"(r)
                     : "a"(n), "D"(a), "S"(b), "d"(c)
                     : "rcx", "r11", "memory");
    return r;
}

void _start(void) {
    sc3(0 /*SYS_WRITE*/, 1, (long)pmsg, 13);   /* fd 1, "PIE-RELOC-OK\n", 13 */
    sc3(1 /*SYS_EXIT*/, 42, 0, 0);
    for (;;) { }
}
