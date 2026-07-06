// elf.h -- ELF types + auxiliary-vector definitions.
// Focused subset: the ELFxx integer types, the auxv_t entries, and the AT_*
// tags that portable userland code (Mesa's CPU detection, loaders) reads.
#ifndef _ELF_H
#define _ELF_H 1

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef uint16_t Elf32_Half;   typedef uint16_t Elf64_Half;
typedef uint32_t Elf32_Word;   typedef uint32_t Elf64_Word;
typedef int32_t  Elf32_Sword;  typedef int32_t  Elf64_Sword;
typedef uint64_t Elf32_Xword;  typedef uint64_t Elf64_Xword;
typedef int64_t  Elf32_Sxword; typedef int64_t  Elf64_Sxword;
typedef uint32_t Elf32_Addr;   typedef uint64_t Elf64_Addr;
typedef uint32_t Elf32_Off;    typedef uint64_t Elf64_Off;

// Auxiliary vector entry (the kernel-provided auxv the loader passes at exec).
typedef struct {
    uint32_t a_type;
    union { uint32_t a_val; } a_un;
} Elf32_auxv_t;

typedef struct {
    uint64_t a_type;
    union { uint64_t a_val; } a_un;
} Elf64_auxv_t;

// AT_* auxv tags (Linux/x86-64 values).
#define AT_NULL     0
#define AT_IGNORE   1
#define AT_EXECFD   2
#define AT_PHDR     3
#define AT_PHENT    4
#define AT_PHNUM    5
#define AT_PAGESZ   6
#define AT_BASE     7
#define AT_FLAGS    8
#define AT_ENTRY    9
#define AT_NOTELF   10
#define AT_UID      11
#define AT_EUID     12
#define AT_GID      13
#define AT_EGID     14
#define AT_PLATFORM 15
#define AT_HWCAP    16
#define AT_CLKTCK   17
#define AT_SECURE   23
#define AT_RANDOM   25
#define AT_HWCAP2   26
#define AT_EXECFN   31

#ifdef __cplusplus
}
#endif

#endif // _ELF_H
