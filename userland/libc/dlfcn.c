// ── dlfcn.c — runtime dynamic loader (dlopen/dlsym/dlclose/dlerror) ─────────
//
// MakaOS has no separate ld.so: dlopen is a libc routine that maps a PIC .so
// into the calling process, base-relocates it, runs its init, and resolves
// dlsym via the .so's own dynamic symbol table (SysV hash).  Executables are
// PIEs and the kernel already applies THEIR relocations; this handles the .so
// side.  A .so's symbol imports bind against the loaded-.so scope (own defined
// symbols + previously dlopen'd objects); binding against the main exe's
// exported .dynsym is layered on top later.
//
// Segment mapping mirrors how the kernel maps the exe: each PT_LOAD is mmap'd
// file-backed MAP_PRIVATE with its own protection (text R+X, data R+W), so no
// mprotect is needed and W^X holds.  The BSS tail is zeroed / anon-mapped.

#include <dlfcn.h>
#include <string.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdint.h>
#include <sys/mman.h>

// ── Minimal ELF64 (userland has no <elf.h>) ────────────────────────────────
typedef struct { uint8_t e_ident[16]; uint16_t e_type, e_machine; uint32_t e_version;
    uint64_t e_entry, e_phoff, e_shoff; uint32_t e_flags;
    uint16_t e_ehsize, e_phentsize, e_phnum, e_shentsize, e_shnum, e_shstrndx; } Elf64_Ehdr;
typedef struct { uint32_t p_type, p_flags; uint64_t p_offset, p_vaddr, p_paddr,
    p_filesz, p_memsz, p_align; } Elf64_Phdr;
typedef struct { int64_t d_tag; uint64_t d_un; } Elf64_Dyn;
typedef struct { uint32_t st_name; uint8_t st_info, st_other; uint16_t st_shndx;
    uint64_t st_value, st_size; } Elf64_Sym;
typedef struct { uint64_t r_offset, r_info; int64_t r_addend; } Elf64_Rela;

#define PT_LOAD 1
#define PT_DYNAMIC 2
#define PT_PHDR 6
#define PF_X 1
#define PF_W 2
#define PF_R 4
#define ET_DYN 3
#define SHN_UNDEF 0
#define DT_NULL 0
#define DT_NEEDED 1
#define DT_HASH 4
#define DT_STRTAB 5
#define DT_SYMTAB 6
#define DT_RELA 7
#define DT_RELASZ 8
#define DT_STRSZ 10
#define ELF_ST_BIND(i) ((i) >> 4)
#define STB_WEAK 2
#define DT_INIT_ARRAY 25
#define DT_INIT_ARRAYSZ 27
#define DT_FINI_ARRAY 26
#define DT_FINI_ARRAYSZ 28
#define DT_JMPREL 23
#define DT_PLTRELSZ 2
#define MAX_NEEDED 24
#define R_X86_64_64 1
#define R_X86_64_GLOB_DAT 6
#define R_X86_64_JUMP_SLOT 7
#define R_X86_64_RELATIVE 8
#define ELF64_R_SYM(i)  ((uint32_t)((i) >> 32))
#define ELF64_R_TYPE(i) ((uint32_t)((i) & 0xffffffffu))

// ── Loaded-object handle ───────────────────────────────────────────────────
typedef struct dso {
    struct dso*      next;       // global list of loaded objects (dlsym scope)
    unsigned long    base;       // load bias: runtime addr = base + link vaddr
    unsigned long    map_start;  // reserved span, for munmap on dlclose
    unsigned long    map_len;
    const Elf64_Sym* symtab;     // DT_SYMTAB (runtime addr)
    const char*      strtab;     // DT_STRTAB
    uint64_t         strsz;      // DT_STRSZ -- bounds st_name (fail closed on corrupt .so)
    const uint32_t*  hash;       // DT_HASH (SysV): [nbucket, nchain, bucket[], chain[]]
    const unsigned long* fini_arr; // DT_FINI_ARRAY (run in reverse on dlclose)
    uint64_t         fini_sz;
    char             path[96];   // opened path -- dedup key for re-dlopen / shared deps
    int              refcnt;
} dso_t;

static dso_t* s_loaded = NULL;   // head of loaded-object list
static char   s_err[160];        // last error (dlerror)
static int    s_err_set = 0;

// Main-executable symbol scope: the exe exports its symbols (libc etc.) via
// --export-dynamic, so a .so's imports resolve against it.  Derived once from
// the auxv program headers libc saved (base via PT_PHDR, dynsym via PT_DYNAMIC).
extern unsigned long __libc_phdr, __libc_phent, __libc_phnum;
static dso_t s_exe;
static int   s_exe_ready = 0;

static void set_err(const char* a, const char* b) {
    // Compose "a: b" into s_err (no snprintf dependency in this TU).
    size_t i = 0;
    for (const char* p = a; *p && i < sizeof(s_err) - 1; p++) s_err[i++] = *p;
    if (b) { if (i < sizeof(s_err) - 2) { s_err[i++] = ':'; s_err[i++] = ' '; }
             for (const char* p = b; *p && i < sizeof(s_err) - 1; p++) s_err[i++] = *p; }
    s_err[i] = 0;
    s_err_set = 1;
}

static unsigned long page_down(unsigned long x) { return x & ~(unsigned long)0xFFF; }
static unsigned long page_up(unsigned long x)   { return (x + 0xFFF) & ~(unsigned long)0xFFF; }


// SysV ELF hash.
static unsigned long elf_hash(const char* s) {
    unsigned long h = 0, g;
    for (const unsigned char* p = (const unsigned char*)s; *p; p++) {
        h = (h << 4) + *p;
        if ((g = h & 0xf0000000UL)) h ^= g >> 24;
        h &= ~g;
    }
    return h;
}

// Look up `name` in one object's dynsym (SysV hash chain).  Returns the defined
// symbol (st_shndx != UNDEF) or NULL.
static const Elf64_Sym* dso_lookup(const dso_t* d, const char* name) {
    if (!d->hash || !d->symtab || !d->strtab) return NULL;
    uint32_t nbucket = d->hash[0];
    uint32_t nchain  = d->hash[1];          // == symtab entry count; bounds every index
    if (!nbucket || !nchain) return NULL;
    const uint32_t* bucket = d->hash + 2;
    const uint32_t* chain  = bucket + nbucket;
    // Bound every step by nchain -> no OOB read, and cap iterations so a cyclic
    // chain in a corrupt .so cannot spin forever (fail closed, V12).
    uint32_t i = bucket[elf_hash(name) % nbucket];
    for (uint32_t steps = 0; i != 0 && i < nchain && steps < nchain; i = chain[i], steps++) {
        const Elf64_Sym* s = &d->symtab[i];
        if (s->st_shndx != SHN_UNDEF && s->st_name < d->strsz
            && strcmp(name, d->strtab + s->st_name) == 0)
            return s;
    }
    return NULL;
}

// Populate the main-executable scope from the auxv program headers (once).
// exe_base = AT_PHDR - the PT_PHDR link vaddr; the exe's dynsym/strtab/hash come
// from PT_DYNAMIC.  The exe's .dynamic d_ptr fields are link-relative (the
// kernel does not relocate them), so add exe_base.
static void init_exe_scope(void) {
    if (s_exe_ready) return;
    s_exe_ready = 1;                       // attempt once; stays zeroed on failure
    if (!__libc_phdr || !__libc_phent || !__libc_phnum) return;
    unsigned long base = 0, dyn_v = 0; int have_base = 0;
    for (unsigned long i = 0; i < __libc_phnum; i++) {
        const Elf64_Phdr* p = (const Elf64_Phdr*)(__libc_phdr + i * __libc_phent);
        if (p->p_type == PT_PHDR)    { base = __libc_phdr - p->p_vaddr; have_base = 1; }
        if (p->p_type == PT_DYNAMIC) dyn_v = p->p_vaddr;
    }
    if (!have_base || !dyn_v) return;
    for (const Elf64_Dyn* e = (const Elf64_Dyn*)(base + dyn_v); e->d_tag != DT_NULL; e++) {
        switch (e->d_tag) {
            case DT_SYMTAB: s_exe.symtab = (const Elf64_Sym*)(base + e->d_un); break;
            case DT_STRTAB: s_exe.strtab = (const char*)(base + e->d_un); break;
            case DT_STRSZ:  s_exe.strsz  = e->d_un; break;
            case DT_HASH:   s_exe.hash   = (const uint32_t*)(base + e->d_un); break;
        }
    }
    s_exe.base = base;
}

// Resolve `name` to a runtime address: loaded objects first (global scope), then
// the main executable's exported symbols.  Returns 0 if unresolved.
static unsigned long resolve_sym(const char* name) {
    for (dso_t* d = s_loaded; d; d = d->next) {
        const Elf64_Sym* s = dso_lookup(d, name);
        if (s) return d->base + s->st_value;
    }
    init_exe_scope();
    if (s_exe.hash) {
        const Elf64_Sym* s = dso_lookup(&s_exe, name);
        if (s) return s_exe.base + s->st_value;
    }
    return 0;
}

// Map every PT_LOAD from `fd` at load bias `base`.  Handles the BSS tail (zero
// the last file page, anon-map the extra pages).  Returns 0 / -1.
static int map_segments(int fd, unsigned long base, const Elf64_Phdr* ph, int phnum) {
    for (int i = 0; i < phnum; i++) {
        if (ph[i].p_type != PT_LOAD || ph[i].p_memsz == 0) continue;
        int prot = 0;
        if (ph[i].p_flags & PF_R) prot |= PROT_READ;
        if (ph[i].p_flags & PF_W) prot |= PROT_WRITE;
        if (ph[i].p_flags & PF_X) prot |= PROT_EXEC;

        unsigned long vaddr      = base + ph[i].p_vaddr;
        unsigned long seg_beg    = page_down(vaddr);
        unsigned long file_end   = vaddr + ph[i].p_filesz;
        unsigned long mem_end    = vaddr + ph[i].p_memsz;
        unsigned long file_pg_end = page_up(file_end);
        unsigned long mem_pg_end  = page_up(mem_end);
        uint64_t      file_off   = page_down(ph[i].p_offset);

        // File-backed part [seg_beg, file_pg_end), MAP_FIXED over the reservation.
        if (file_pg_end > seg_beg) {
            void* r = mmap((void*)seg_beg, file_pg_end - seg_beg, prot,
                           MAP_PRIVATE | MAP_FIXED, fd, (long)file_off);
            if (r == MAP_FAILED) return -1;
        }
        // Zero the BSS bytes sharing the last file page (writable segments).
        if ((prot & PROT_WRITE) && file_end < file_pg_end && ph[i].p_memsz > ph[i].p_filesz)
            memset((void*)file_end, 0, file_pg_end - file_end);
        // Extra whole BSS pages beyond the file-backed part: anon zero.
        if (mem_pg_end > file_pg_end) {
            void* r = mmap((void*)file_pg_end, mem_pg_end - file_pg_end, prot,
                           MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
            if (r == MAP_FAILED) return -1;
        }
    }
    return 0;
}

// Apply one Rela table.  Returns 0 / -1 (missing symbol or unsupported type).
static int apply_rela(dso_t* d, const Elf64_Rela* rela, uint64_t sz) {
    if (!rela || !sz) return 0;
    for (uint64_t o = 0; o + sizeof(Elf64_Rela) <= sz; o += sizeof(Elf64_Rela)) {
        const Elf64_Rela* r = (const Elf64_Rela*)((const uint8_t*)rela + o);
        uint32_t type = ELF64_R_TYPE(r->r_info);
        // The reloc target must lie inside this object's own mapped span, or a
        // corrupt .so would scribble the exe/heap.  Fail closed (V12).
        unsigned long w = d->base + r->r_offset;
        if (w < d->map_start || w + sizeof(uint64_t) > d->map_start + d->map_len) {
            set_err("dlopen: reloc offset out of range", 0); return -1;
        }
        uint64_t* where = (uint64_t*)w;
        if (type == R_X86_64_RELATIVE) { *where = d->base + (uint64_t)r->r_addend; continue; }

        // Bound the symbol index + name against the .so's tables (OOB guard).
        uint32_t symi = ELF64_R_SYM(r->r_info);
        if (!d->hash || symi >= d->hash[1]) { set_err("dlopen: bad reloc symbol index", 0); return -1; }
        const Elf64_Sym* s = &d->symtab[symi];
        if (s->st_name >= d->strsz) { set_err("dlopen: bad reloc symbol name", 0); return -1; }
        const char* name = d->strtab + s->st_name;
        const Elf64_Sym* def = dso_lookup(d, name);
        unsigned long val = def ? d->base + def->st_value : resolve_sym(name);
        // A WEAK undefined symbol legitimately resolves to 0; only a STRONG
        // undefined that no object provides is fatal.
        if (!val && s->st_shndx == SHN_UNDEF && ELF_ST_BIND(s->st_info) != STB_WEAK) {
            set_err("dlopen: undefined symbol", name); return -1;
        }
        switch (type) {
            case R_X86_64_GLOB_DAT:
            case R_X86_64_JUMP_SLOT: *where = val; break;
            case R_X86_64_64:        *where = val + (uint64_t)r->r_addend; break;
            default: set_err("dlopen: unsupported reloc type", name); return -1;
        }
    }
    return 0;
}

// Run an init/fini function-pointer array (DT_INIT_ARRAY / DT_FINI_ARRAY),
// bounded to the object's own mapped span so a corrupt array pointer in a bad
// .so cannot make us fetch + call through wild memory (V12).  `reverse` runs
// destructors last-to-first.
static void run_array(const dso_t* d, const unsigned long* arr, uint64_t sz, int reverse) {
    if (!arr || !sz) return;
    unsigned long a = (unsigned long)arr;
    if (a < d->map_start || a + sz > d->map_start + d->map_len) return;   // corrupt -> skip
    uint64_t n = sz / sizeof(unsigned long);
    for (uint64_t k = 0; k < n; k++) {
        unsigned long fp = arr[reverse ? (n - 1 - k) : k];
        if (fp && fp != ~0UL) ((void (*)(void))fp)();
    }
}

// dlopen(NULL) returns this handle for the global scope (main exe + all loaded
// objects); dlsym on it searches everything via resolve_sym.
static dso_t s_global;

// Resolve a DT_NEEDED name to a path: absolute if it has a '/', else /lib/<name>.
static void dep_path(const char* name, char* out, unsigned outsz) {
    unsigned i = 0; int slash = 0;
    for (const char* p = name; *p; p++) if (*p == '/') slash = 1;
    if (!slash) for (const char* p = "/lib/"; *p && i + 1 < outsz; p++) out[i++] = *p;
    for (const char* p = name; *p && i + 1 < outsz; p++) out[i++] = *p;
    out[i] = 0;
}

// Remove `d` from the loaded list (used on a failed load).
static void unpublish(dso_t* d) {
    for (dso_t** pp = &s_loaded; *pp; pp = &(*pp)->next)
        if (*pp == d) { *pp = d->next; break; }
}

void* dlopen(const char* path, int flags) {
    (void)flags;
    s_err_set = 0;
    if (!path) return &s_global;   // global-scope handle (RTLD_DEFAULT-like)

    // Dedup: a re-dlopen of an already-loaded object bumps its refcount and
    // returns the same handle (also breaks circular DT_NEEDED chains below).
    for (dso_t* e = s_loaded; e; e = e->next)
        if (strcmp(e->path, path) == 0) { e->refcnt++; return e; }

    int fd = open(path, O_RDONLY, 0);
    if (fd < 0) { set_err("dlopen: cannot open", path); return NULL; }

    Elf64_Ehdr eh;
    if (lseek(fd, 0, 0) != 0 || read(fd, &eh, sizeof eh) != (long)sizeof eh
        || eh.e_ident[0] != 0x7f || eh.e_ident[1] != 'E'
        || eh.e_type != ET_DYN || eh.e_phnum == 0) {
        set_err("dlopen: not a shared object", path); close(fd); return NULL;
    }
    int phnum = eh.e_phnum;
    Elf64_Phdr* ph = (Elf64_Phdr*)malloc((size_t)phnum * sizeof(Elf64_Phdr));
    if (!ph) { set_err("dlopen: out of memory", 0); close(fd); return NULL; }
    size_t phsz = (size_t)phnum * sizeof(Elf64_Phdr);
    if (lseek(fd, (long)eh.e_phoff, 0) != (long)eh.e_phoff
        || read(fd, ph, phsz) != (long)phsz) {
        set_err("dlopen: short phdr read", path); free(ph); close(fd); return NULL;
    }

    // Span of all PT_LOADs (link-time vaddrs) + the PT_DYNAMIC vaddr.
    unsigned long lo = ~0UL, hi = 0, dyn_vaddr = 0;
    for (int i = 0; i < phnum; i++) {
        if (ph[i].p_type == PT_DYNAMIC) dyn_vaddr = ph[i].p_vaddr;
        if (ph[i].p_type != PT_LOAD || ph[i].p_memsz == 0) continue;
        if (page_down(ph[i].p_vaddr) < lo) lo = page_down(ph[i].p_vaddr);
        if (page_up(ph[i].p_vaddr + ph[i].p_memsz) > hi) hi = page_up(ph[i].p_vaddr + ph[i].p_memsz);
    }
    if (lo == ~0UL || hi <= lo || !dyn_vaddr) {
        set_err("dlopen: malformed program headers", path); free(ph); close(fd); return NULL;
    }

    // Reserve the whole span, then MAP_FIXED each segment over it.
    size_t span = hi - lo;
    void* res = mmap(NULL, span, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (res == MAP_FAILED) { set_err("dlopen: address reservation failed", path); free(ph); close(fd); return NULL; }
    unsigned long base = (unsigned long)res - lo;   // base + p_vaddr lands in [res, res+span)

    if (map_segments(fd, base, ph, phnum) != 0) {
        set_err("dlopen: segment mmap failed", path);
        munmap(res, span); free(ph); close(fd); return NULL;
    }
    free(ph);
    close(fd);   // segments are mapped; their VMAs hold their own file refs

    dso_t* d = (dso_t*)malloc(sizeof(dso_t));
    if (!d) { set_err("dlopen: out of memory", 0); munmap(res, span); return NULL; }
    memset(d, 0, sizeof *d);
    d->base = base; d->map_start = (unsigned long)res; d->map_len = span; d->refcnt = 1;
    { unsigned i = 0; for (const char* p = path; *p && i < sizeof(d->path) - 1; p++) d->path[i++] = *p;
      d->path[i] = 0; }

    // Parse the dynamic section (resident at base + dyn_vaddr).
    const Elf64_Dyn* dyn = (const Elf64_Dyn*)(base + dyn_vaddr);
    const Elf64_Rela* rela = NULL; uint64_t rela_sz = 0;
    const Elf64_Rela* jmprel = NULL; uint64_t jmprel_sz = 0;
    const unsigned long* init_arr = NULL; uint64_t init_sz = 0;
    uint64_t needed[MAX_NEEDED]; int n_needed = 0, needed_overflow = 0;
    for (const Elf64_Dyn* e = dyn; e->d_tag != DT_NULL; e++) {
        switch (e->d_tag) {
            case DT_NEEDED:       if (n_needed < MAX_NEEDED) needed[n_needed++] = e->d_un;
                                  else needed_overflow = 1; break;
            case DT_SYMTAB:       d->symtab = (const Elf64_Sym*)(base + e->d_un); break;
            case DT_STRTAB:       d->strtab = (const char*)(base + e->d_un); break;
            case DT_STRSZ:        d->strsz  = e->d_un; break;
            case DT_HASH:         d->hash   = (const uint32_t*)(base + e->d_un); break;
            case DT_RELA:         rela      = (const Elf64_Rela*)(base + e->d_un); break;
            case DT_RELASZ:       rela_sz   = e->d_un; break;
            case DT_JMPREL:       jmprel    = (const Elf64_Rela*)(base + e->d_un); break;
            case DT_PLTRELSZ:     jmprel_sz = e->d_un; break;
            case DT_INIT_ARRAY:   init_arr  = (const unsigned long*)(base + e->d_un); break;
            case DT_INIT_ARRAYSZ: init_sz   = e->d_un; break;
            case DT_FINI_ARRAY:   d->fini_arr = (const unsigned long*)(base + e->d_un); break;
            case DT_FINI_ARRAYSZ: d->fini_sz  = e->d_un; break;
        }
    }
    if (needed_overflow) {   // never silently drop deps -> unresolved symbols; fail closed
        set_err("dlopen: too many DT_NEEDED", path);
        munmap(res, span); free(d); return NULL;
    }

    // Publish BEFORE loading dependencies + relocating so the object's own
    // symbols are visible to its deps' relocations, and a circular DT_NEEDED
    // chain dedups against the already-published object instead of recursing.
    d->next = s_loaded; s_loaded = d;

    // Load DT_NEEDED dependencies (recursively); each publishes itself, so this
    // object's relocations below can bind against them.
    for (int i = 0; i < n_needed; i++) {
        if (!d->strtab || needed[i] >= d->strsz) continue;
        char dp[128]; dep_path(d->strtab + needed[i], dp, sizeof dp);
        if (!dlopen(dp, flags)) {
            set_err("dlopen: dependency failed", dp);
            unpublish(d); munmap(res, span); free(d); return NULL;
        }
    }

    if (apply_rela(d, rela, rela_sz) != 0 || apply_rela(d, jmprel, jmprel_sz) != 0) {
        unpublish(d); munmap(res, span); free(d); return NULL;
    }

    run_array(d, init_arr, init_sz, 0);   // constructors (DT_INIT_ARRAY), bounded
    return d;
}

void* dlsym(void* handle, const char* name) {
    s_err_set = 0;
    if (!handle || !name) { set_err("dlsym: bad argument", 0); return NULL; }
    if (handle == &s_global) {              // dlopen(NULL): search the global scope
        unsigned long v = resolve_sym(name);
        if (!v) { set_err("dlsym: symbol not found", name); return NULL; }
        return (void*)v;
    }
    const Elf64_Sym* s = dso_lookup((dso_t*)handle, name);
    if (!s) { set_err("dlsym: symbol not found", name); return NULL; }
    return (void*)(((dso_t*)handle)->base + s->st_value);
}

int dlclose(void* handle) {
    if (!handle || handle == &s_global) return 0;
    dso_t* d = (dso_t*)handle;
    if (--d->refcnt > 0) return 0;
    run_array(d, d->fini_arr, d->fini_sz, 1);   // destructors (DT_FINI_ARRAY), reverse, bounded
    // Note: DT_NEEDED deps are left loaded (no per-object dep list yet); a small
    // leak, not a correctness bug.
    unpublish(d);
    munmap((void*)d->map_start, d->map_len);
    free(d);
    return 0;
}

char* dlerror(void) {
    if (!s_err_set) return NULL;
    s_err_set = 0;
    return s_err;
}

int dladdr(const void* addr, Dl_info* info) {
    (void)addr;
    if (info) { info->dli_fname = 0; info->dli_fbase = 0; info->dli_sname = 0; info->dli_saddr = 0; }
    return 0;
}
