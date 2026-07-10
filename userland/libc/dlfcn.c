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
#define DT_HASH 4
#define DT_STRTAB 5
#define DT_SYMTAB 6
#define DT_RELA 7
#define DT_RELASZ 8
#define DT_INIT_ARRAY 25
#define DT_INIT_ARRAYSZ 27
#define DT_JMPREL 23
#define DT_PLTRELSZ 2
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
    const uint32_t*  hash;       // DT_HASH (SysV): [nbucket, nchain, bucket[], chain[]]
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
    if (!nbucket) return NULL;
    const uint32_t* bucket = d->hash + 2;
    const uint32_t* chain  = bucket + nbucket;
    for (uint32_t i = bucket[elf_hash(name) % nbucket]; i != 0; i = chain[i]) {
        const Elf64_Sym* s = &d->symtab[i];
        if (s->st_shndx != SHN_UNDEF && strcmp(name, d->strtab + s->st_name) == 0)
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
        uint64_t* where = (uint64_t*)(d->base + r->r_offset);
        if (type == R_X86_64_RELATIVE) { *where = d->base + (uint64_t)r->r_addend; continue; }

        const Elf64_Sym* s = &d->symtab[ELF64_R_SYM(r->r_info)];
        const char* name = d->strtab + s->st_name;
        const Elf64_Sym* def = dso_lookup(d, name);
        unsigned long val = def ? d->base + def->st_value : resolve_sym(name);
        if (!val && s->st_shndx == SHN_UNDEF) { set_err("dlopen: undefined symbol", name); return -1; }
        switch (type) {
            case R_X86_64_GLOB_DAT:
            case R_X86_64_JUMP_SLOT: *where = val; break;
            case R_X86_64_64:        *where = val + (uint64_t)r->r_addend; break;
            default: set_err("dlopen: unsupported reloc type", name); return -1;
        }
    }
    return 0;
}

void* dlopen(const char* path, int flags) {
    (void)flags;
    s_err_set = 0;
    if (!path) { set_err("dlopen: NULL path (self-scope not yet supported)", 0); return NULL; }

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

    // Parse the dynamic section (resident at base + dyn_vaddr).
    const Elf64_Dyn* dyn = (const Elf64_Dyn*)(base + dyn_vaddr);
    const Elf64_Rela* rela = NULL; uint64_t rela_sz = 0;
    const Elf64_Rela* jmprel = NULL; uint64_t jmprel_sz = 0;
    const unsigned long* init_arr = NULL; uint64_t init_sz = 0;
    for (const Elf64_Dyn* e = dyn; e->d_tag != DT_NULL; e++) {
        switch (e->d_tag) {
            case DT_SYMTAB:       d->symtab = (const Elf64_Sym*)(base + e->d_un); break;
            case DT_STRTAB:       d->strtab = (const char*)(base + e->d_un); break;
            case DT_HASH:         d->hash   = (const uint32_t*)(base + e->d_un); break;
            case DT_RELA:         rela      = (const Elf64_Rela*)(base + e->d_un); break;
            case DT_RELASZ:       rela_sz   = e->d_un; break;
            case DT_JMPREL:       jmprel    = (const Elf64_Rela*)(base + e->d_un); break;
            case DT_PLTRELSZ:     jmprel_sz = e->d_un; break;
            case DT_INIT_ARRAY:   init_arr  = (const unsigned long*)(base + e->d_un); break;
            case DT_INIT_ARRAYSZ: init_sz   = e->d_un; break;
        }
    }

    // Publish into the global scope BEFORE relocating so the object's own
    // defined symbols (and previously-loaded objects) are visible.
    d->next = s_loaded; s_loaded = d;

    if (apply_rela(d, rela, rela_sz) != 0 || apply_rela(d, jmprel, jmprel_sz) != 0) {
        s_loaded = d->next;                    // unpublish
        munmap(res, span); free(d); return NULL;
    }

    // Constructors (DT_INIT_ARRAY).
    if (init_arr && init_sz) {
        for (uint64_t i = 0; i < init_sz / sizeof(unsigned long); i++) {
            void (*fn)(void) = (void (*)(void))init_arr[i];
            if (fn && (unsigned long)fn != ~0UL) fn();
        }
    }
    return d;
}

void* dlsym(void* handle, const char* name) {
    s_err_set = 0;
    if (!handle || !name) { set_err("dlsym: bad argument", 0); return NULL; }
    const Elf64_Sym* s = dso_lookup((dso_t*)handle, name);
    if (!s) { set_err("dlsym: symbol not found", name); return NULL; }
    return (void*)(((dso_t*)handle)->base + s->st_value);
}

int dlclose(void* handle) {
    if (!handle) return 0;
    dso_t* d = (dso_t*)handle;
    if (--d->refcnt > 0) return 0;
    for (dso_t** pp = &s_loaded; *pp; pp = &(*pp)->next)
        if (*pp == d) { *pp = d->next; break; }
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
