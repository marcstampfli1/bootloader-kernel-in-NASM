# Porting a JVM to MakaOS — plan

Goal: run real Java on MakaOS. MakaOS has a **custom syscall ABI** (write=0,
exit=1, ... — not Linux-numbered), so the JVM is **ported from source** against
MakaOS's Linux-flavored libc, exactly like every other port. We are NOT running
precompiled Linux JDK binaries (that would need a Linux syscall personality —
out of scope).

## Readiness assessment (verified 2026-07-11)

Ready:
- **Threads** — pthreads (`pthread_create/join`, C11 threads). ✓
- **mmap + PROT_EXEC** — anonymous + file-backed executable mappings (dlopen
  already loads + runs `libSDL3.so`); needed for JIT code pages. ✓
- **dlopen** — real dynamic loading of `.so` (SDL3, sdl2-compat). JNI native
  libs + `libjvm.so` load this way. ✓
- **libffi** — already ported (`scripts/port-libffi.sh`, incl. `java_raw_api.c`).
  This is the JNI/foreign-call substrate. ✓
- **libstdc++** — ported, PIC. ✓
- **Filesystem** — ext2 rootfs for the JDK's `lib/modules` / class files. ✓
- **TLS** — `__thread` works; errno is now a POSIX macro. ✓
- **Time** — `clock_gettime` / monotonic. ✓ (confirm CLOCK_MONOTONIC resolution)

Gaps (the real work):
- **⚠ SA_SIGINFO signal delivery — THE gate.** MakaOS delivers signals as a
  simple `sa_handler(int)`: the kernel sets `rdi=signum` but does NOT populate
  `rsi=siginfo*` / `rdx=ucontext*`. HotSpot *requires* `SA_SIGINFO` — it reads
  `siginfo->si_addr` (fault address for implicit null checks) and
  `ucontext->uc_mcontext.gregs[]` (to read/advance the faulting PC for
  safepoint-poll and null-check resumption). The register-save mechanism already
  exists (`kernel/proc/signal.h sigframe_t` saves rip/rsp/rflags/GPRs/FPU and
  restores them on sigreturn), so the work is bounded: deliver a real
  `siginfo_t` in rsi and a **glibc-compatible `ucontext_t`** in rdx, populate
  `uc_mcontext.gregs[]` from the sigframe, and honor handler modifications on
  sigreturn. Kernel + libc change.
- **⚠ os_linux assumptions in OpenJDK.** `/proc/self/maps` (memory probing),
  `sysconf` (page size / nproc — partly present), NPTL pthread internals, vDSO
  (`clock_gettime` fast path). Each surfaces during the os-layer port; most map
  to existing MakaOS facilities or small additions.
- **Custom syscall ABI** — recompile from source (a port). ✓ by design.

## Phased plan

### Phase 0 — Prerequisites / de-risk (kernel + libc)
1. **SA_SIGINFO + glibc `ucontext`.** Wire `siginfo_t*` (rsi) and `ucontext_t*`
   (rdx) into signal delivery; define glibc-layout `ucontext_t` / `mcontext_t` /
   `gregs[REG_*]` in libc; map `sigframe_t <-> uc_mcontext.gregs`; make
   `sys_sigreturn` restore from the (possibly handler-modified) context. Add a
   selftest: a SIGSEGV handler reads `si_addr`, advances `gregs[REG_RIP]` past
   the faulting instruction, and execution resumes. This is generally correct
   POSIX, not JVM-specific.
2. Confirm thread stack **guard pages**, `clock_gettime(CLOCK_MONOTONIC)`, large
   anon mmap, and `mprotect` (for the safepoint polling page).

### Phase 1 — First light with a SMALL JVM (fast milestone, de-risk)
Port **Avian** (preferred: small, C++, bundles its own class library, minimal OS
deps, near-static binary) or **JamVM**. These do **explicit** null checks +
cooperative safepoints, so they likely run on MakaOS's *current* simple-signal
support — proving the OS can host a JVM (threads + class loading + GC +
interpreter/JIT) before the OpenJDK marathon. Goal: `Hello.class` prints.

### Phase 2 — OpenJDK Zero (the real JDK, no JIT)
Cross-compile OpenJDK with the **Zero** interpreter to `x86_64-pc-makaos`.
- Create the OS/CPU layer: `os_makaos.cpp` + `os_cpu/makaos_x86` derived from
  `os_linux*` (thread mgmt, signals -> our SA_SIGINFO/ucontext, memory, time,
  dlopen).
- Build `java.base` + native libs (`libjava`, `libjvm`, `libnet`, `libnio`,
  `libzip`) against MakaOS libc.
- Goal: `java -version`, then `java Hello`. Zero sidesteps the JIT port entirely.

### Phase 3 — HotSpot JIT (performance)
Enable C1 (client) then C2, emitting x86-64 into PROT_EXEC mmap. Depends on
Phase 0's SA_SIGINFO/ucontext being solid (implicit null checks + SEGV-based
safepoint polling). Goal: real-speed Java.

### Phase 4 — JDK completeness
Fill shared libc/syscall gaps as OpenJDK hits them (in the shared libc/kernel per
the `userland/compat/linux/` discipline — never per-port). Timezone DB, fonts
(fontconfig/freetype already ported), networking (NIO), crypto.

## Recommended immediate order
Phase 0 (SA_SIGINFO/ucontext) -> Phase 1 (Avian first-light) in parallel, then
commit to Phase 2 (OpenJDK Zero). Phase 0 is the highest-leverage, most-reusable
piece (it is correct POSIX signal handling regardless of the JVM).

## Biggest risks
- The `os_linux -> os_makaos` port (signals/ucontext, thread management) is the
  crux of Phase 2.
- OpenJDK's build system cross-compiling to a novel target is finicky (expect
  `--openjdk-target`, a bootstrap JDK on the host, and configure hacking).
- Latent glibc-specific assumptions surfacing one at a time.

---

## Update (2026-07-11): recon done, ordering confirmed, Phase-0 design locked

Avian recon (source cloned to build/third_party/avian; host `javac 25` + `java`
present; zlib/make/g++ present) found that **Avian's own posix.cpp uses
`SA_SIGINFO` + `ucontext`** (`src/system/posix.cpp:653` sets `sa_flags=SA_SIGINFO`;
`:965` casts the 3rd arg to `ucontext_t*`) to coordinate its stop-the-world GC
(signal a thread, read its ucontext to walk the stack) and for SEGV null-checks.
So the signal layer is NOT skippable for Avian -- it **is** "the missing stuff,"
and it's the same as the OpenJDK Phase 0.  Confirmed order: **signal layer ->
Avian -> OpenJDK.**

Missing-stuff checklist (verified):
- [x] libc `<ucontext.h>` -- glibc-layout `ucontext_t`/`mcontext_t`/`gregs[REG_*]`
      (done: userland/libc/include/ucontext.h).
- [x] Kernel: `SA_SIGINFO` delivery -- **DONE + verified** (sigtest PASS, both
      paths).  The rt_sigframe `{ pretcode, ucontext, siginfo }` is built by a
      single source-neutral builder (`build_rt_frame`/`build_simple_frame` in
      kernel/proc/signal.c) that reads a `sig_uctx_t` and emits a
      `sig_redirect_t`, fed from EITHER a syscall kframe (syscall-return
      delivery) OR an exception trap frame.  `sys_sigreturn` restores from
      `uc.uc_mcontext.gregs[]` so a handler's `gregs[REG_RIP]` edit takes effect.
      Crucially, synchronous faults now deliver to handlers too:
        - `isr_common_entry` (isr_stubs.asm) passes the C handler a pointer to
          the REAL on-stack `trap_frame_t` (idt.h), not a copy, so edits to
          ip/sp/flags + GPRs take effect on POP_GPRS + iretq.
        - `signal_deliver_fault(sig, iframe)` (signal.c) redirects the trap frame
          into a catchable, unblocked handler, else forces SIG_DFL + terminates
          (Linux force_sig semantics).  Wired into every synchronous exception
          (#PF via vmm.c, #DE/#UD/#GP/... via idt.c).  The #PF path suppresses
          its PF-KILL banner for a deliverable fault (`signal_fault_deliverable`).
        - `sys_sigaction` now permits catching SIGSEGV/SIGBUS/SIGFPE/SIGILL/
          SIGTRAP (only SIGKILL/SIGSTOP uncatchable, POSIX).
      Regression test: userland/apps/sigtest (build `SIGTEST=1`) exercises both
      the syscall-return path (raise SIGUSR1) and the fault path (NULL deref,
      si_addr==0, gregs[REG_RIP] redirect resumes at recovery()).
- [ ] `pthread_kill` + a kernel per-thread signal syscall (tkill/tgkill-style)
      -- Avian GC signals a specific thread.
- [ ] `sigaltstack` (libc + kernel) -- SIGSEGV on an alt stack for stack-overflow.
      libc `stack_t` + SS_ONSTACK/SS_DISABLE already added (ucontext.h).

Then Phase 1: add a `scripts/port-avian.sh` that cross-builds Avian via its
posix layer as `platform=linux` with `cxx/cc/ranlib` overridden to the makaos
toolchain + `--sysroot`, host `javac` for the classpath/bootimage, and iterate
on remaining missing libc symbols.
