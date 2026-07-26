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

/*
 * Physical <-> virtual translation for low memory.
 *
 * During early boot the first 1 GiB of physical memory is identity-mapped, so
 * a physical address equals its virtual address (the direct-map base is 0).
 * When Mm later installs a proper high-half direct map, only this base changes
 * and every caller keeps working.
 */
#define KERNEL_DIRECT_MAP_BASE 0x0ULL

static ALWAYS_INLINE void *MmPhysToVirt(UINT64 phys)
{
    return (void *)(ULONG_PTR)(phys + KERNEL_DIRECT_MAP_BASE);
}

static ALWAYS_INLINE UINT64 MmVirtToPhysDirect(void *virt)
{
    return (UINT64)(ULONG_PTR)virt - KERNEL_DIRECT_MAP_BASE;
}

#endif /* _NTOS_MM_H_ */
