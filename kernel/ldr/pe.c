/*
 * ldr/pe.c - the PE/COFF image loader with import/export linking.
 *
 * Loading an executable is three steps:
 *   1. map the image at its preferred base and lay out its sections,
 *   2. resolve its imports - load each dependency DLL (e.g. ntdll) on demand
 *      and patch this image's Import Address Table with the resolved routine
 *      addresses,
 *   3. tighten page permissions per section.
 *
 * Dependency DLLs are found in a small in-kernel module registry backed by the
 * images embedded in the kernel (kernel/ldr/testpe.asm), standing in for a
 * filesystem until there is one.
 */
#include <ntos/ldr.h>
#include <ntos/mm.h>
#include <ntos/ke.h>
#include <ntos/rtl.h>
#include <nt/pe.h>

/* Embedded module images. */
extern char NtdllImageStart[];
extern char NtdllImageEnd[];

typedef struct _LDR_MODULE {
    const char *Name;
    const char *Start;
    const char *End;
    UINT64      Base;
    BOOLEAN     Loaded;
} LDR_MODULE;

static LDR_MODULE g_modules[] = {
    { "ntdll.dll", NtdllImageStart, NtdllImageEnd, 0, FALSE },
};
#define MODULE_COUNT (sizeof(g_modules) / sizeof(g_modules[0]))

/* Case-insensitive ASCII compare (DLL names are matched loosely). */
static int ci_strcmp(const char *a, const char *b)
{
    for (;; a++, b++) {
        char ca = *a, cb = *b;
        if (ca >= 'A' && ca <= 'Z') ca += 32;
        if (cb >= 'A' && cb <= 'Z') cb += 32;
        if (ca != cb || ca == 0)
            return (int)(UCHAR)ca - (int)(UCHAR)cb;
    }
}

static PIMAGE_NT_HEADERS64 nt_headers(UINT64 base)
{
    IMAGE_DOS_HEADER *dos = (IMAGE_DOS_HEADER *)base;
    if (dos->e_magic != IMAGE_DOS_SIGNATURE)
        return NULL;
    PIMAGE_NT_HEADERS64 nt = (PIMAGE_NT_HEADERS64)(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE)
        return NULL;
    return nt;
}

static PIMAGE_SECTION_HEADER first_section(PIMAGE_NT_HEADERS64 nt)
{
    return (PIMAGE_SECTION_HEADER)((UINT8 *)&nt->OptionalHeader +
                                   nt->FileHeader.SizeOfOptionalHeader);
}

/* ------------------------------------------------------------------ */
/* Mapping and protection                                             */
/* ------------------------------------------------------------------ */

static void set_range_perms(UINT64 start, UINT64 size, UINT64 flags)
{
    UINT64 first = PAGE_ALIGN(start);
    UINT64 last = PAGE_ALIGN_UP(start + size);
    for (UINT64 va = first; va < last; va += PAGE_SIZE) {
        UINT64 pa = MmGetPhysicalAddress(va);
        if (pa != MM_INVALID_PHYS)
            MmMapPage(va, pa, flags);
    }
}

static void apply_relocations(UINT64 base, INT64 delta, PIMAGE_NT_HEADERS64 nt)
{
    if (delta == 0)
        return;
    IMAGE_DATA_DIRECTORY dir =
        nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_BASERELOC];
    if (dir.Size == 0)
        return;

    UINT64 cursor = base + dir.VirtualAddress;
    UINT64 end = cursor + dir.Size;
    while (cursor < end) {
        IMAGE_BASE_RELOCATION *block = (IMAGE_BASE_RELOCATION *)cursor;
        if (block->SizeOfBlock < sizeof(IMAGE_BASE_RELOCATION))
            break;
        UINT32 count =
            (block->SizeOfBlock - sizeof(IMAGE_BASE_RELOCATION)) / sizeof(UINT16);
        UINT16 *entries = (UINT16 *)(cursor + sizeof(IMAGE_BASE_RELOCATION));
        for (UINT32 i = 0; i < count; i++) {
            if ((entries[i] >> 12) == IMAGE_REL_BASED_DIR64) {
                UINT64 *p = (UINT64 *)(base + block->VirtualAddress +
                                       (entries[i] & 0x0FFF));
                *p += (UINT64)delta;
            }
        }
        cursor += block->SizeOfBlock;
    }
}

/* Map an image at its preferred base and lay out headers + sections (writable). */
static NTSTATUS LdrpMapImage(const void *file, SIZE_T file_size, UINT64 *base_out)
{
    const UINT8 *bytes = (const UINT8 *)file;
    if (file_size < sizeof(IMAGE_DOS_HEADER))
        return STATUS_INVALID_IMAGE_FORMAT;

    const IMAGE_DOS_HEADER *dos = (const IMAGE_DOS_HEADER *)bytes;
    if (dos->e_magic != IMAGE_DOS_SIGNATURE ||
        dos->e_lfanew + sizeof(IMAGE_NT_HEADERS64) > file_size)
        return STATUS_INVALID_IMAGE_FORMAT;

    const IMAGE_NT_HEADERS64 *nt =
        (const IMAGE_NT_HEADERS64 *)(bytes + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE ||
        nt->FileHeader.Machine != IMAGE_FILE_MACHINE_AMD64 ||
        nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC)
        return STATUS_INVALID_IMAGE_FORMAT;

    const IMAGE_OPTIONAL_HEADER64 *opt = &nt->OptionalHeader;
    UINT64 base = opt->ImageBase;
    UINT64 image_size = PAGE_ALIGN_UP(opt->SizeOfImage);

    for (UINT64 off = 0; off < image_size; off += PAGE_SIZE) {
        UINT64 pa = MmAllocatePage();
        if (pa == MM_INVALID_PHYS)
            return STATUS_NO_MEMORY;
        if (!MmMapPage(base + off, pa, PTE_USER | PTE_WRITE))
            return STATUS_CONFLICTING_ADDRESSES;
    }

    memset((void *)base, 0, image_size);
    if (opt->SizeOfHeaders <= file_size)
        memcpy((void *)base, bytes, opt->SizeOfHeaders);

    const IMAGE_SECTION_HEADER *sec =
        (const IMAGE_SECTION_HEADER *)((const UINT8 *)opt +
                                       nt->FileHeader.SizeOfOptionalHeader);
    for (UINT16 i = 0; i < nt->FileHeader.NumberOfSections; i++) {
        const IMAGE_SECTION_HEADER *s = &sec[i];
        if (s->SizeOfRawData == 0)
            continue;
        if ((UINT64)s->PointerToRawData + s->SizeOfRawData > file_size)
            continue;
        if ((UINT64)s->VirtualAddress + s->SizeOfRawData > image_size)
            continue;
        memcpy((void *)(base + s->VirtualAddress),
               bytes + s->PointerToRawData, s->SizeOfRawData);
    }

    apply_relocations(base, (INT64)(base - opt->ImageBase),
                      (PIMAGE_NT_HEADERS64)(base + dos->e_lfanew));

    *base_out = base;
    return STATUS_SUCCESS;
}

static void LdrpProtectImage(UINT64 base)
{
    PIMAGE_NT_HEADERS64 nt = nt_headers(base);
    UINT64 image_size = PAGE_ALIGN_UP(nt->OptionalHeader.SizeOfImage);

    set_range_perms(base, image_size, PTE_USER); /* read-only + user by default */

    PIMAGE_SECTION_HEADER sec = first_section(nt);
    for (UINT16 i = 0; i < nt->FileHeader.NumberOfSections; i++) {
        UINT64 flags = PTE_USER;
        if (sec[i].Characteristics & IMAGE_SCN_MEM_WRITE)
            flags |= PTE_WRITE;
        set_range_perms(base + sec[i].VirtualAddress, sec[i].VirtualSize, flags);
    }
}

/* ------------------------------------------------------------------ */
/* Exports and imports                                                */
/* ------------------------------------------------------------------ */

UINT64 LdrGetProcAddress(UINT64 base, const char *name)
{
    PIMAGE_NT_HEADERS64 nt = nt_headers(base);
    if (!nt)
        return 0;
    IMAGE_DATA_DIRECTORY dir =
        nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
    if (dir.Size == 0)
        return 0;

    IMAGE_EXPORT_DIRECTORY *ed =
        (IMAGE_EXPORT_DIRECTORY *)(base + dir.VirtualAddress);
    UINT32 *names = (UINT32 *)(base + ed->AddressOfNames);
    UINT16 *ordinals = (UINT16 *)(base + ed->AddressOfNameOrdinals);
    UINT32 *funcs = (UINT32 *)(base + ed->AddressOfFunctions);

    for (UINT32 i = 0; i < ed->NumberOfNames; i++) {
        const char *export_name = (const char *)(base + names[i]);
        if (strcmp(export_name, name) == 0)
            return base + funcs[ordinals[i]];
    }
    return 0;
}

static UINT64 LdrpProcByOrdinal(UINT64 base, UINT16 ordinal)
{
    PIMAGE_NT_HEADERS64 nt = nt_headers(base);
    IMAGE_DATA_DIRECTORY dir =
        nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
    if (dir.Size == 0)
        return 0;
    IMAGE_EXPORT_DIRECTORY *ed =
        (IMAGE_EXPORT_DIRECTORY *)(base + dir.VirtualAddress);
    UINT32 index = ordinal - ed->Base;
    if (index >= ed->NumberOfFunctions)
        return 0;
    UINT32 *funcs = (UINT32 *)(base + ed->AddressOfFunctions);
    return base + funcs[index];
}

/* Forward declaration: importing an image may pull in dependency modules. */
static UINT64 LdrpLoadModule(const char *name);

static NTSTATUS LdrResolveImports(UINT64 base)
{
    PIMAGE_NT_HEADERS64 nt = nt_headers(base);
    IMAGE_DATA_DIRECTORY dir =
        nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (dir.Size == 0)
        return STATUS_SUCCESS; /* nothing to import */

    IMAGE_IMPORT_DESCRIPTOR *desc =
        (IMAGE_IMPORT_DESCRIPTOR *)(base + dir.VirtualAddress);

    for (; desc->Name != 0; desc++) {
        const char *dll = (const char *)(base + desc->Name);
        UINT64 dll_base = LdrpLoadModule(dll);
        if (!dll_base) {
            KeLog("[ldr]  import: dependency '%s' not found\n", dll);
            return STATUS_INVALID_IMAGE_FORMAT;
        }

        UINT32 int_rva = desc->OriginalFirstThunk ? desc->OriginalFirstThunk
                                                   : desc->FirstThunk;
        UINT64 *names = (UINT64 *)(base + int_rva);
        UINT64 *iat = (UINT64 *)(base + desc->FirstThunk);

        for (UINT32 i = 0; names[i]; i++) {
            UINT64 thunk = names[i];
            UINT64 addr;
            if (thunk & IMAGE_ORDINAL_FLAG64) {
                addr = LdrpProcByOrdinal(dll_base, (UINT16)(thunk & 0xFFFF));
            } else {
                IMAGE_IMPORT_BY_NAME *ibn =
                    (IMAGE_IMPORT_BY_NAME *)(base + (thunk & 0x7FFFFFFF));
                addr = LdrGetProcAddress(dll_base, ibn->Name);
                if (!addr)
                    KeLog("[ldr]  import: '%s' not exported by %s\n",
                          ibn->Name, dll);
            }
            if (!addr)
                return STATUS_INVALID_IMAGE_FORMAT;
            iat[i] = addr;
        }
        KeLog("[ldr]  linked imports from %s (base %p)\n", dll, (void *)dll_base);
    }
    return STATUS_SUCCESS;
}

static UINT64 LdrpLoadModule(const char *name)
{
    for (SIZE_T i = 0; i < MODULE_COUNT; i++) {
        LDR_MODULE *m = &g_modules[i];
        if (ci_strcmp(name, m->Name) != 0)
            continue;
        if (m->Loaded)
            return m->Base;

        UINT64 base;
        SIZE_T size = (SIZE_T)(m->End - m->Start);
        if (!NT_SUCCESS(LdrpMapImage(m->Start, size, &base)))
            return 0;

        /* Mark loaded before resolving to break any import cycles. */
        m->Base = base;
        m->Loaded = TRUE;

        if (!NT_SUCCESS(LdrResolveImports(base)))
            return 0;
        LdrpProtectImage(base);

        KeLog("[ldr]  loaded module %s at %p\n", m->Name, (void *)base);
        return base;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* Public entry point                                                 */
/* ------------------------------------------------------------------ */

NTSTATUS LdrLoadExecutable(const void *file, SIZE_T file_size, UINT64 *entry_out)
{
    UINT64 base;
    NTSTATUS status = LdrpMapImage(file, file_size, &base);
    if (!NT_SUCCESS(status))
        return status;

    PIMAGE_NT_HEADERS64 nt = nt_headers(base);
    KeLog("[ldr]  exe base=%p entry_rva=0x%x size=0x%x sections=%u\n",
          (void *)base, nt->OptionalHeader.AddressOfEntryPoint,
          nt->OptionalHeader.SizeOfImage, nt->FileHeader.NumberOfSections);

    status = LdrResolveImports(base);
    if (!NT_SUCCESS(status))
        return status;

    LdrpProtectImage(base);

    *entry_out = base + nt->OptionalHeader.AddressOfEntryPoint;
    KeLog("[ldr]  executable ready; entry at %p\n", (void *)*entry_out);
    return STATUS_SUCCESS;
}
