/*
 * ldr/pe.c - the PE/COFF image loader.
 *
 * Validates a PE32+ image, maps SizeOfImage worth of user pages at the image
 * base, copies the headers and each section to its RVA, zero-fills the rest,
 * applies base relocations if the load base differs from the preferred one, and
 * tightens page permissions per section. The result is an image ready to run in
 * ring 3.
 */
#include <ntos/ldr.h>
#include <ntos/mm.h>
#include <ntos/ke.h>
#include <ntos/rtl.h>
#include <nt/pe.h>

/* Set the page permissions for [start, start+size) (page-granular). */
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

static void apply_relocations(UINT64 base, INT64 delta,
                              const IMAGE_OPTIONAL_HEADER64 *opt,
                              const UINT8 *file)
{
    if (delta == 0)
        return;

    IMAGE_DATA_DIRECTORY reloc_dir =
        opt->DataDirectory[IMAGE_DIRECTORY_ENTRY_BASERELOC];
    if (reloc_dir.Size == 0)
        return;

    /* The relocation blocks live in the mapped image at ImageBase + RVA. */
    UINT64 cursor = base + reloc_dir.VirtualAddress;
    UINT64 end = cursor + reloc_dir.Size;
    (void)file;

    while (cursor < end) {
        IMAGE_BASE_RELOCATION *block = (IMAGE_BASE_RELOCATION *)cursor;
        if (block->SizeOfBlock < sizeof(IMAGE_BASE_RELOCATION))
            break;

        UINT32 count =
            (block->SizeOfBlock - sizeof(IMAGE_BASE_RELOCATION)) / sizeof(UINT16);
        UINT16 *entries = (UINT16 *)(cursor + sizeof(IMAGE_BASE_RELOCATION));

        for (UINT32 i = 0; i < count; i++) {
            UINT16 type = entries[i] >> 12;
            UINT16 offset = entries[i] & 0x0FFF;
            if (type == IMAGE_REL_BASED_DIR64) {
                UINT64 *patch = (UINT64 *)(base + block->VirtualAddress + offset);
                *patch += (UINT64)delta;
            }
            /* IMAGE_REL_BASED_ABSOLUTE (0) is padding; ignore others. */
        }
        cursor += block->SizeOfBlock;
    }
}

NTSTATUS LdrLoadPeImage(const void *file, SIZE_T file_size,
                        UINT64 *entry_out, UINT64 *base_out)
{
    const UINT8 *bytes = (const UINT8 *)file;

    if (file_size < sizeof(IMAGE_DOS_HEADER))
        return STATUS_INVALID_IMAGE_FORMAT;

    const IMAGE_DOS_HEADER *dos = (const IMAGE_DOS_HEADER *)bytes;
    if (dos->e_magic != IMAGE_DOS_SIGNATURE)
        return STATUS_INVALID_IMAGE_FORMAT;

    if (dos->e_lfanew + sizeof(IMAGE_NT_HEADERS64) > file_size)
        return STATUS_INVALID_IMAGE_FORMAT;

    const IMAGE_NT_HEADERS64 *nt =
        (const IMAGE_NT_HEADERS64 *)(bytes + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE)
        return STATUS_INVALID_IMAGE_FORMAT;
    if (nt->FileHeader.Machine != IMAGE_FILE_MACHINE_AMD64)
        return STATUS_INVALID_IMAGE_FORMAT;
    if (nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC)
        return STATUS_INVALID_IMAGE_FORMAT;

    const IMAGE_OPTIONAL_HEADER64 *opt = &nt->OptionalHeader;
    UINT64 base = opt->ImageBase; /* load at the preferred base */
    UINT64 image_size = PAGE_ALIGN_UP(opt->SizeOfImage);

    KeLog("[ldr]  PE: base=%p entry_rva=0x%x size=0x%lx sections=%u\n",
          (void *)base, opt->AddressOfEntryPoint,
          (unsigned long)image_size, nt->FileHeader.NumberOfSections);

    /* Reserve and map the whole image as writable+user so we can lay it out. */
    for (UINT64 off = 0; off < image_size; off += PAGE_SIZE) {
        UINT64 pa = MmAllocatePage();
        if (pa == MM_INVALID_PHYS)
            return STATUS_NO_MEMORY;
        if (!MmMapPage(base + off, pa, PTE_USER | PTE_WRITE))
            return STATUS_CONFLICTING_ADDRESSES;
    }

    /* Everything starts zeroed (gives uninitialized sections their BSS). */
    memset((void *)base, 0, image_size);

    /* Copy the headers. */
    if (opt->SizeOfHeaders <= file_size)
        memcpy((void *)base, bytes, opt->SizeOfHeaders);

    /* Copy each section's raw data to its virtual address. */
    const IMAGE_SECTION_HEADER *sections =
        (const IMAGE_SECTION_HEADER *)((const UINT8 *)opt +
                                       nt->FileHeader.SizeOfOptionalHeader);

    for (UINT16 i = 0; i < nt->FileHeader.NumberOfSections; i++) {
        const IMAGE_SECTION_HEADER *s = &sections[i];
        if (s->SizeOfRawData == 0)
            continue;
        if ((UINT64)s->PointerToRawData + s->SizeOfRawData > file_size)
            continue;
        if ((UINT64)s->VirtualAddress + s->SizeOfRawData > image_size)
            continue;

        memcpy((void *)(base + s->VirtualAddress),
               bytes + s->PointerToRawData, s->SizeOfRawData);
    }

    /* Relocate if we could not honour the preferred base (delta is 0 here). */
    apply_relocations(base, (INT64)(base - opt->ImageBase), opt, bytes);

    /* Tighten permissions: read-only for everything by default, writable only
     * where a section asks for it. (Execute is always permitted; no NX yet.) */
    set_range_perms(base, image_size, PTE_USER);
    for (UINT16 i = 0; i < nt->FileHeader.NumberOfSections; i++) {
        const IMAGE_SECTION_HEADER *s = &sections[i];
        UINT64 flags = PTE_USER;
        if (s->Characteristics & IMAGE_SCN_MEM_WRITE)
            flags |= PTE_WRITE;
        set_range_perms(base + s->VirtualAddress, s->VirtualSize, flags);

        KeLog("[ldr]  section %-8s -> %p size=0x%x %s%s%s\n",
              (const char *)s->Name, (void *)(base + s->VirtualAddress),
              s->VirtualSize,
              (s->Characteristics & IMAGE_SCN_MEM_READ) ? "R" : "-",
              (s->Characteristics & IMAGE_SCN_MEM_WRITE) ? "W" : "-",
              (s->Characteristics & IMAGE_SCN_MEM_EXECUTE) ? "X" : "-");
    }

    *base_out = base;
    *entry_out = base + opt->AddressOfEntryPoint;

    KeLog("[ldr]  image loaded; entry at %p\n", (void *)*entry_out);
    return STATUS_SUCCESS;
}
