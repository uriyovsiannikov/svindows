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
 * Executables and their dependency DLLs (ntdll) are read from the mounted
 * filesystem by name; a small cache tracks what is already loaded.
 */
#include <ntos/ldr.h>
#include <ntos/mm.h>
#include <ntos/ke.h>
#include <ntos/rtl.h>
#include <ntos/io.h>
#include <ntos/ex.h>
#include <nt/pe.h>
#include <nt/peb.h>

/* Cache of already-loaded modules (name -> load base). Satisfies repeat imports
 * and breaks import cycles. */
typedef struct _LDR_LOADED {
    char    Name[32];
    UINT64  Base;
    BOOLEAN Valid;
    BOOLEAN Linked; /* already threaded onto the process PEB->Ldr list */
} LDR_LOADED;

#define MAX_LOADED_MODULES 16
static LDR_LOADED g_loaded[MAX_LOADED_MODULES];

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

/* A per-import stub arena in low user memory. Each unresolved import gets its
 * own 16-byte slot, so a single arena serves imports that are functions AND
 * imports that are data:
 *
 *   - A missing FUNCTION is pointed at its slot and called: the slot's first
 *     bytes are `xor eax, eax; ret`, so the call harmlessly returns 0.
 *   - A missing DATA export (e.g. msvcrt's `_fmode`, `_commode`) has an IAT slot
 *     that is a *pointer to the variable*; the image dereferences it and may
 *     read or WRITE through it. Pointing it at its own slot means such a write
 *     lands in that slot alone and cannot corrupt any other stub.
 *
 * The arena is therefore mapped writable *and* executable (a deliberate W^X
 * exception for this bootstrap shim). Giving every import a distinct slot is
 * what makes the two uses coexist: a slot used as data is never executed, and a
 * slot used as code is never written. */
#define LDR_STUB_ARENA_VA    0x0000000000069000ULL
#define LDR_STUB_ARENA_PAGES 4      /* 4 * 4096 / 16 = 1024 stub slots */
#define LDR_STUB_SLOT_SIZE   16

static UINT64 g_stub_arena;   /* base VA once mapped */
static UINT64 g_stub_next;    /* bump cursor for the next free slot */
static UINT64 g_stub_end;     /* one past the arena */

static UINT64 LdrpImportStub(void)
{
    if (!g_stub_arena) {
        for (UINT64 i = 0; i < LDR_STUB_ARENA_PAGES; i++) {
            UINT64 pa = MmAllocatePage();
            if (pa == MM_INVALID_PHYS)
                return 0;
            MmMapPage(LDR_STUB_ARENA_VA + i * PAGE_SIZE, pa,
                      PTE_USER | PTE_WRITE);
        }
        memset((void *)LDR_STUB_ARENA_VA, 0,
               LDR_STUB_ARENA_PAGES * PAGE_SIZE);
        g_stub_arena = LDR_STUB_ARENA_VA;
        g_stub_next = LDR_STUB_ARENA_VA;
        g_stub_end = LDR_STUB_ARENA_VA + LDR_STUB_ARENA_PAGES * PAGE_SIZE;
    }

    /* Out of slots: fall back to the first one. Sharing degrades correctness for
     * data imports but keeps the image loadable rather than failing outright. */
    if (g_stub_next + LDR_STUB_SLOT_SIZE > g_stub_end)
        return g_stub_arena;

    UINT64 slot = g_stub_next;
    g_stub_next += LDR_STUB_SLOT_SIZE;
    UINT8 *code = (UINT8 *)slot;
    code[0] = 0x31; /* xor eax, eax */
    code[1] = 0xC0;
    code[2] = 0xC3; /* ret          */
    /* bytes 3..15 stay zero: scratch for a data-import dereference. */
    return slot;
}

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
        /* A dependency DLL we don't provide no longer fails the load: every
         * import from it is stubbed (and logged), just like an individual
         * missing function. */
        if (!dll_base)
            KeLog("[ldr]  dependency '%s' not found -> stubbing its imports\n",
                  dll);

        UINT32 int_rva = desc->OriginalFirstThunk ? desc->OriginalFirstThunk
                                                   : desc->FirstThunk;
        UINT64 *names = (UINT64 *)(base + int_rva);
        UINT64 *iat = (UINT64 *)(base + desc->FirstThunk);

        UINT32 stubbed = 0, linked = 0;
        for (UINT32 i = 0; names[i]; i++) {
            UINT64 thunk = names[i];
            UINT64 addr = 0;
            if (thunk & IMAGE_ORDINAL_FLAG64) {
                UINT16 ord = (UINT16)(thunk & 0xFFFF);
                if (dll_base)
                    addr = LdrpProcByOrdinal(dll_base, ord);
                if (!addr)
                    KeLog("[ldr]  STUB %s!#%u\n", dll, ord);
            } else {
                IMAGE_IMPORT_BY_NAME *ibn =
                    (IMAGE_IMPORT_BY_NAME *)(base + (thunk & 0x7FFFFFFF));
                if (dll_base)
                    addr = LdrGetProcAddress(dll_base, ibn->Name);
                if (!addr)
                    KeLog("[ldr]  STUB %s!%s\n", dll, ibn->Name);
            }
            /* Unresolved imports get a return-0 stub so the image still loads;
             * the log above is the to-do list of what a binary actually needs. */
            if (!addr) {
                addr = LdrpImportStub();
                stubbed++;
            } else {
                linked++;
            }
            iat[i] = addr;
        }
        if (stubbed)
            KeLog("[ldr]  %s: %u linked, %u stubbed\n", dll, linked, stubbed);
        else
            KeLog("[ldr]  linked imports from %s (base %p)\n", dll,
                  (void *)dll_base);
    }
    return STATUS_SUCCESS;
}

static UINT64 lookup_module(const char *name)
{
    for (int i = 0; i < MAX_LOADED_MODULES; i++)
        if (g_loaded[i].Valid && ci_strcmp(name, g_loaded[i].Name) == 0)
            return g_loaded[i].Base;
    return 0;
}

static void remember_module(const char *name, UINT64 base)
{
    for (int i = 0; i < MAX_LOADED_MODULES; i++) {
        if (g_loaded[i].Valid)
            continue;
        int j = 0;
        for (; j < 31 && name[j]; j++)
            g_loaded[i].Name[j] = name[j];
        g_loaded[i].Name[j] = 0;
        g_loaded[i].Base = base;
        g_loaded[i].Valid = TRUE;
        return;
    }
}

/* Load a dependency module by name from disk (or return the cached base). */
static UINT64 LdrpLoadModule(const char *name)
{
    UINT64 existing = lookup_module(name);
    if (existing)
        return existing;

    void *buf;
    SIZE_T size;
    if (!NT_SUCCESS(FatLoadFile(name, &buf, &size))) {
        KeLog("[ldr]  dependency '%s' not found on disk\n", name);
        return 0;
    }

    UINT64 base;
    NTSTATUS status = LdrpMapImage(buf, size, &base);
    ExFreePool(buf); /* mapped into user pages; the file buffer is done */
    if (!NT_SUCCESS(status))
        return 0;

    remember_module(name, base); /* cache before resolving to break cycles */

    if (!NT_SUCCESS(LdrResolveImports(base)))
        return 0;
    LdrpProtectImage(base);

    KeLog("[ldr]  loaded module %s at %p\n", name, (void *)base);
    return base;
}

/* ------------------------------------------------------------------ */
/* Process module list (PEB->Ldr)                                     */
/* ------------------------------------------------------------------ */

/* Persistent state for the process module list, so modules loaded later (via
 * LoadLibrary) can be appended to the same region. */
static PPEB_LDR_DATA g_peb_ldr;
static UINT8 *g_ldr_cursor;
static UINT8 *g_ldr_end;

/* Append one cached module to the PEB->Ldr lists (bump-allocated from the Ldr
 * region). Marks it linked so it isn't added twice. */
static void ldr_link_module(LDR_LOADED *m)
{
    if (!g_peb_ldr || m->Linked)
        return;
    if (g_ldr_cursor + sizeof(LDR_DATA_TABLE_ENTRY) + 128 > g_ldr_end)
        return; /* out of room */

    PLDR_DATA_TABLE_ENTRY e = (PLDR_DATA_TABLE_ENTRY)g_ldr_cursor;
    g_ldr_cursor += sizeof(LDR_DATA_TABLE_ENTRY);
    memset(e, 0, sizeof(*e));

    PIMAGE_NT_HEADERS64 nt = nt_headers(m->Base);
    e->DllBase = (PVOID)m->Base;
    e->EntryPoint = nt ? (PVOID)(m->Base + nt->OptionalHeader.AddressOfEntryPoint)
                       : NULL;
    e->SizeOfImage = nt ? nt->OptionalHeader.SizeOfImage : 0;

    /* Widen the ASCII module name into a UTF-16 buffer for BaseDllName. */
    UINT16 *wname = (UINT16 *)g_ldr_cursor;
    int n = 0;
    for (; m->Name[n] && n < 63; n++)
        wname[n] = (UINT16)(UCHAR)m->Name[n];
    wname[n] = 0;
    g_ldr_cursor += (n + 1) * sizeof(UINT16);
    g_ldr_cursor = (UINT8 *)(((UINT64)g_ldr_cursor + 7) & ~7ULL); /* realign */

    e->BaseDllName.Length = (USHORT)(n * 2);
    e->BaseDllName.MaximumLength = (USHORT)((n + 1) * 2);
    e->BaseDllName.Buffer = wname;
    e->FullDllName = e->BaseDllName;

    InsertTailList(&g_peb_ldr->InLoadOrderModuleList, &e->InLoadOrderLinks);
    InsertTailList(&g_peb_ldr->InMemoryOrderModuleList, &e->InMemoryOrderLinks);
    InsertTailList(&g_peb_ldr->InInitializationOrderModuleList,
                   &e->InInitializationOrderLinks);
    m->Linked = TRUE;
}

/* Link any cached-but-not-yet-listed modules (used after a runtime load). */
static void ldr_sync_module_list(void)
{
    for (int i = 0; i < MAX_LOADED_MODULES; i++)
        if (g_loaded[i].Valid && !g_loaded[i].Linked)
            ldr_link_module(&g_loaded[i]);
}

void LdrBuildProcessModuleList(struct _PEB *peb_opaque, UINT64 ldr_va,
                               UINT64 ldr_size)
{
    PPEB peb = (PPEB)peb_opaque;

    /* Carve the PEB_LDR_DATA out of the head of the user Ldr region; the rest
     * of the region bump-allocates the module entries. */
    PPEB_LDR_DATA ldr = (PPEB_LDR_DATA)ldr_va;
    memset(ldr, 0, sizeof(*ldr));
    ldr->Length = sizeof(PEB_LDR_DATA);
    ldr->Initialized = 1;
    InitializeListHead(&ldr->InLoadOrderModuleList);
    InitializeListHead(&ldr->InMemoryOrderModuleList);
    InitializeListHead(&ldr->InInitializationOrderModuleList);

    g_peb_ldr = ldr;
    g_ldr_cursor = (UINT8 *)(ldr_va + sizeof(PEB_LDR_DATA));
    g_ldr_end = (UINT8 *)(ldr_va + ldr_size);

    ldr_sync_module_list();

    peb->Ldr = ldr;
    KeLog("[ldr]  PEB->Ldr ready @ %p\n", (void *)ldr);
}

/*
 * LdrLoadLibrary - load a DLL by name at runtime and link it into PEB->Ldr,
 * returning its load base (0 on failure). Repeat loads return the cached base.
 * This is what kernel32's LoadLibraryA reaches through a syscall.
 */
UINT64 LdrLoadLibrary(const char *name)
{
    UINT64 base = LdrpLoadModule(name);
    if (base)
        ldr_sync_module_list(); /* thread the newly-loaded module(s) into Ldr */
    return base;
}

/* ------------------------------------------------------------------ */
/* Public entry point                                                 */
/* ------------------------------------------------------------------ */

NTSTATUS LdrLoadExecutable(const char *filename, UINT64 *entry_out,
                           UINT64 *base_out)
{
    void *buf;
    SIZE_T size;
    NTSTATUS status = FatLoadFile(filename, &buf, &size);
    if (!NT_SUCCESS(status))
        return status;

    UINT64 base;
    status = LdrpMapImage(buf, size, &base);
    ExFreePool(buf);
    if (!NT_SUCCESS(status))
        return status;

    PIMAGE_NT_HEADERS64 nt = nt_headers(base);
    KeLog("[ldr]  exe base=%p entry_rva=0x%x size=0x%x sections=%u\n",
          (void *)base, nt->OptionalHeader.AddressOfEntryPoint,
          nt->OptionalHeader.SizeOfImage, nt->FileHeader.NumberOfSections);

    /* Register the exe itself as the first module (load-order head), before its
     * dependencies get pulled in by import resolution. */
    remember_module(filename, base);

    status = LdrResolveImports(base);
    if (!NT_SUCCESS(status))
        return status;

    LdrpProtectImage(base);

    *entry_out = base + nt->OptionalHeader.AddressOfEntryPoint;
    if (base_out)
        *base_out = base;
    KeLog("[ldr]  executable ready; entry at %p\n", (void *)*entry_out);
    return STATUS_SUCCESS;
}
