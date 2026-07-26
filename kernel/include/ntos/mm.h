/*
 * ntos/mm.h - Memory Manager (Mm) - address space layout and helpers.
 *
 * The full physical/virtual memory manager is still to come (see the roadmap);
 * for now this header pins down the address-space constants shared across the
 * kernel and the physical<->virtual translation used by early drivers.
 */
#ifndef _NTOS_MM_H_
#define _NTOS_MM_H_

#include <nt/ntdef.h>

/* The kernel image is linked here and loaded at physical KERNEL_PHYS_BASE. */
#define KERNEL_VIRT_BASE 0xFFFFFFFF80000000ULL
#define KERNEL_PHYS_BASE 0x0000000000100000ULL

/* Standard x86-64 page geometry. */
#define PAGE_SHIFT       12
#define PAGE_SIZE        0x1000ULL
#define PAGE_MASK        (PAGE_SIZE - 1)
#define LARGE_PAGE_SHIFT 21
#define LARGE_PAGE_SIZE  0x200000ULL   /* 2 MiB */

#define PAGE_ALIGN(x)      ((ULONG_PTR)(x) & ~PAGE_MASK)
#define PAGE_ALIGN_UP(x)   (((ULONG_PTR)(x) + PAGE_MASK) & ~PAGE_MASK)
#define BYTES_TO_PAGES(x)  (((ULONG_PTR)(x) + PAGE_MASK) >> PAGE_SHIFT)

/* Base of the physical-memory direct map, once Mm installs it. */
#define MM_DIRECT_MAP_BASE 0xFFFF800000000000ULL

/* Kernel heap arena (used by the pool allocator), in the -1 GiB window that the
 * kernel image mapping (-2 GiB) leaves free. */
#define MM_KERNEL_HEAP_BASE 0xFFFFFFFFC0000000ULL

/*
 * Physical <-> virtual translation for the direct map.
 *
 * During early boot the first 1 GiB of physical memory is identity-mapped, so a
 * physical address equals its virtual address; MmDirectMapBase is 0. Once Mm
 * installs the high-half direct map and drops identity, MmDirectMapBase becomes
 * MM_DIRECT_MAP_BASE and every caller keeps working unchanged.
 */
extern UINT64 MmDirectMapBase;

static ALWAYS_INLINE void *MmPhysToVirt(UINT64 phys)
{
    return (void *)(ULONG_PTR)(phys + MmDirectMapBase);
}

static ALWAYS_INLINE UINT64 MmVirtToPhysDirect(void *virt)
{
    return (UINT64)(ULONG_PTR)virt - MmDirectMapBase;
}

/* Page-table entry flags. */
#define PTE_PRESENT   0x001ULL
#define PTE_WRITE     0x002ULL
#define PTE_USER      0x004ULL
#define PTE_PWT       0x008ULL
#define PTE_PCD       0x010ULL
#define PTE_ACCESSED  0x020ULL
#define PTE_DIRTY     0x040ULL
#define PTE_LARGE     0x080ULL   /* PS: 2 MiB page at PD level */
#define PTE_GLOBAL    0x100ULL
#define PTE_ADDR_MASK 0x000FFFFFFFFFF000ULL

/* ------------------------------------------------------------------ */
/* Physical memory map (parsed from Multiboot2)                       */
/* ------------------------------------------------------------------ */

#define MM_MAX_REGIONS 64

typedef enum _MM_REGION_TYPE {
    MmRegionAvailable = 1,
    MmRegionReserved,
    MmRegionAcpiReclaimable,
    MmRegionAcpiNvs,
    MmRegionBad,
} MM_REGION_TYPE;

typedef struct _MM_MEMORY_REGION {
    UINT64         base;
    UINT64         length;
    MM_REGION_TYPE type;
} MM_MEMORY_REGION;

typedef struct _MM_MEMORY_MAP {
    MM_MEMORY_REGION regions[MM_MAX_REGIONS];
    UINT32           count;
    UINT64           highest_address; /* end of the highest available region */
    UINT64           total_available; /* sum of available bytes             */
} MM_MEMORY_MAP;

/* Parse the Multiboot2 info block (physical pointer) into `out`. */
BOOLEAN MmParseMultibootMemoryMap(UINT64 mb_info_phys, MM_MEMORY_MAP *out);
const char *MmRegionTypeName(MM_REGION_TYPE type);

/* ------------------------------------------------------------------ */
/* Physical page allocator (PMM)                                      */
/* ------------------------------------------------------------------ */

#define MM_INVALID_PHYS ((UINT64)-1)

void   MmInitializePhysicalMemory(const MM_MEMORY_MAP *map);
UINT64 MmAllocatePage(void);                 /* one 4 KiB frame, or MM_INVALID_PHYS */
UINT64 MmAllocatePages(SIZE_T count);        /* `count` contiguous frames          */
void   MmFreePages(UINT64 phys, SIZE_T count);

UINT64 MmTotalPages(void);
UINT64 MmFreePageCount(void);

/* ------------------------------------------------------------------ */
/* Virtual memory (VMM)                                               */
/* ------------------------------------------------------------------ */

void    MmInitializeVirtualMemory(const MM_MEMORY_MAP *map);
BOOLEAN MmMapPage(UINT64 virt, UINT64 phys, UINT64 flags);
BOOLEAN MmUnmapPage(UINT64 virt);
UINT64  MmGetPhysicalAddress(UINT64 virt);   /* MM_INVALID_PHYS if not mapped */

/* ------------------------------------------------------------------ */
/* User-pointer validation                                            */
/* ------------------------------------------------------------------ */

/* User space is the low canonical half (below the non-canonical hole). */
#define MM_USER_MAX 0x0000800000000000ULL

/* TRUE if the whole range [va, va+len) lies in user space. */
BOOLEAN MmIsUserAddress(UINT64 va);

/* Validate that a user buffer is safe for the kernel to read / write: the whole
 * range is in user space and every page is currently mapped. There is no kernel
 * SEH yet, so this is a range + page-presence check (it catches null, kernel,
 * and unmapped pointers) rather than a fault-safe probe. */
BOOLEAN MmProbeForRead(UINT64 va, UINT64 len);
BOOLEAN MmProbeForWrite(UINT64 va, UINT64 len);

/* Probe and copy a user UNICODE_STRING's name (at `ustr_va`) into a kernel
 * ASCII buffer, so the service works on a captured copy rather than a user
 * pointer that could change or fault. FALSE if any pointer fails to probe. */
BOOLEAN MmCaptureUnicodeName(UINT64 ustr_va, char *out, SIZE_T out_size);

/* Re-apply page protection to a range of already-mapped pages (keeps their
 * physical frames, changes the writable bit). Backs VirtualProtect. */
void MmProtectRange(UINT64 va, UINT64 size, BOOLEAN writable);

/* Top-level Mm bring-up: parse map, start PMM, install real paging. */
void MmInitialize(UINT64 mb_info_phys);

#endif /* _NTOS_MM_H_ */
