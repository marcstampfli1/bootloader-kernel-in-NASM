/*
 * dsotest -- a minimal no-import shared object for the dlopen (Phase 2)
 * milestone.  Built with `ld -shared` -> ET_DYN + dynsym + one R_X86_64_RELATIVE
 * (the s_name pointer).  Freestanding: it imports nothing, so the loader only
 * has to map it and apply the RELATIVE reloc.
 */
static const char *volatile s_name = "libdso";   /* -> R_X86_64_RELATIVE */

int dso_answer(void) { return 42; }
const char* dso_name(void) { return s_name; }     /* returns the relocated pointer */
