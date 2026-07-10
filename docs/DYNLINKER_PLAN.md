# MakaOS dynamic loader (dlopen) -- implementation plan

Goal: real `dlopen`/`dlsym`/`dlclose`/`dlerror` loading actual PIC `.so`s, so
sdl2-compat / DarkPlaces / Mesa DRI / (later) LWJGL+JVM work unmodified and the
static-bind hacks (SDL static-ANGLE EGL, planned sdl2-compat patch) get deleted.

## Architecture: PIE executables + in-libc runtime loader (no separate ld.so)
Decided against `-no-pie`: a `-no-pie` ET_EXEC cannot carry a usable dynsym on
this toolchain (verified: every `--export-dynamic` variant gave `PT_DYNAMIC=0`).
PIE is the correct base -- executables become ET_DYN, naturally get `.dynsym` +
`PT_DYNAMIC` + relocations, and exe + every `.so` are uniform base-relocated
objects. Load-base math is the correct mechanism, not a cost to avoid.
Executables stay self-contained (no PT_INTERP, no ld.so bootstrap); `dlopen` is
a libc routine that loads `.so` plugins and binds their imports to the PIE exe's
exported `.dynsym` (base + st_value) + previously-loaded objects.

## Toolchain reality (Phase 0 is the real gate)
The cross-toolchain was purpose-built for static `-no-pie` ELF:
`--disable-shared`, no `Scrt1.o`/dynamic-linker, and a default linker script
that forces a fixed base (so `-pie` still emitted `ET_EXEC`, `PT_DYNAMIC=0`).
`libc.a` has non-PIC `R_X86_64_32S` relocs. So Phase 0 = teach it dynamic/PIE
ELF. Most `scripts/port-*.sh` libs are already `-fPIC`.

## Phases (each with a provable milestone)
0. Toolchain emits PIE + `.so`:
   - PIE/ET_DYN linker script (base-0, PT_DYNAMIC, .dynsym/.hash) + a `-shared`
     script; provide via `-T` or fix binutils ld scripts if the PIE (.xd) script
     is missing.
   - Recompile libc + crt0 (`entry.asm` -> RIP-relative) + userland `-fPIC/-fPIE`.
   - Reconfigure/re-spec gcc only if specs keep forcing no-pie.
   - DONE: `cc -fPIE -pie -Wl,--export-dynamic hi.c` -> ET_DYN + host sym in
     `.dynsym` + `R_X86_64_RELATIVE`; `cc -shared` -> a real `.so`.
1. Kernel ET_DYN exec: load base, map PT_LOADs, apply exe `RELATIVE`/`IRELATIVE`
   relocs, jump `base+entry`; aux vector (AT_PHDR/PHENT/PHNUM/BASE/ENTRY/EXECFN).
   DONE: a PIE hello runs.
2. libc `dlopen`/`dlsym`: map `.so`, base-relocate (`RELATIVE`/`GLOB_DAT`/
   `JUMP_SLOT`/`64`, BIND_NOW), resolve host imports vs the PIE exe `.dynsym` +
   loaded `.so`s, RELRO mprotect, gnu-hash lookup. DONE: no-import `.so`, then
   libc-import `.so`.
3. init/fini + `DT_NEEDED` deps + `dlopen(NULL)` (self scope) + `dlerror` (TLS) +
   `dlclose` (refcount). DONE: C++ ctor `.so`; dlopen(NULL) finds host syms.
4. Dynamic TLS: `__tls_get_addr` + per-thread DTV + `DTPMOD64`/`DTPOFF64`/
   `TPOFF64`, integrated with `__makaos_tls_init` + pthreads. THE hard part;
   SDL3/Mesa need it. DONE: a `__thread` `.so` works across threads.
5. Build `libSDL3.so`; sdl2-compat builds unmodified (its dlopen(libSDL3) works);
   revert SDL/Mesa static-bind hacks; then DarkPlaces dynamic loading + Xonotic;
   foundation for LWJGL/JVM. DONE: sdl2-compat runs a trivial SDL2 app.

## Risks
- Phase 0 toolchain surgery (PIE script may be missing from ld; PIC libc/crt).
- TLS (Phase 4) is the mountain.
- Relocation/ifunc coverage: audit `libSDL3.so`'s `.rela` early
  (IRELATIVE/COPY/ordering).
- Multi-session; each phase surfaces new libc gaps.
