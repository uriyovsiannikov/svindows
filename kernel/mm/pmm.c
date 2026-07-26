/*
 * mm/pmm.c - the physical memory manager: a bitmap page-frame allocator.
 *
 * One bit per 4 KiB frame (1 = in use, 0 = free). The bitmap itself is parked
 * in physical RAM immediately above the kernel image. Everything is reached
 * through MmPhysToVirt, so the allocator keeps working across the switch from
 * the identity map to the high-half direct map.
 */
#include <ntos/mm.h>
#include <ntos/ke.h>
#include <ntos/rtl.h>

/* Physical end of the loaded kernel image (from the linker script). */
extern char __kernel_phys_end[];

typedef struct _PMM_STATE {
    UINT64 bitmap_phys;   /* physical base of the bitmap                */
    UINT64 bitmap_bytes;  /* size of the bitmap in bytes                */
    UINT64 total_pages;   /* frames covered (highest_address / PAGE)    */
    UINT64 free_pages;
    UINT64 used_pages;
    UINT64 next_hint;     /* frame index to start the next search from  */
} PMM_STATE;

static PMM_STATE g_pmm;

static ALWAYS_INLINE UINT8 *pmm_bitmap(void)
{
    return (UINT8 *)MmPhysToVirt(g_pmm.bitmap_phys);
}

static ALWAYS_INLINE void bit_set(UINT64 frame)   /* mark used */
{
    pmm_bitmap()[frame >> 3] |= (UINT8)(1u << (frame & 7));
}

static ALWAYS_INLINE void bit_clear(UINT64 frame) /* mark free */
{
    pmm_bitmap()[frame >> 3] &= (UINT8)~(1u << (frame & 7));
}

static ALWAYS_INLINE BOOLEAN bit_test(UINT64 frame) /* TRUE if used */
{
    return (BOOLEAN)((pmm_bitmap()[frame >> 3] >> (frame & 7)) & 1);
}

/* Reserve [base, base+length): mark frames used (idempotent for accounting). */
static void reserve_range(UINT64 base, UINT64 length)
{
    UINT64 first = base >> PAGE_SHIFT;
    UINT64 last = (base + length + PAGE_MASK) >> PAGE_SHIFT;
    for (UINT64 f = first; f < last && f < g_pmm.total_pages; f++) {
        if (!bit_test(f)) {
            bit_set(f);
            if (g_pmm.free_pages)
                g_pmm.free_pages--;
            g_pmm.used_pages++;
        }
    }
}

/* Release an available region: mark frames free. */
static void free_range(UINT64 base, UINT64 length)
{
    UINT64 first = (base + PAGE_MASK) >> PAGE_SHIFT; /* don't free a partial head */
    UINT64 last = (base + length) >> PAGE_SHIFT;     /* nor a partial tail        */
    for (UINT64 f = first; f < last && f < g_pmm.total_pages; f++) {
        if (bit_test(f)) {
            bit_clear(f);
            g_pmm.free_pages++;
            if (g_pmm.used_pages)
                g_pmm.used_pages--;
        }
    }
}

void MmInitializePhysicalMemory(const MM_MEMORY_MAP *map)
{
    g_pmm.total_pages = map->highest_address >> PAGE_SHIFT;
    g_pmm.bitmap_bytes = (g_pmm.total_pages + 7) / 8;

    /* Park the bitmap just past the kernel image. */
    UINT64 kernel_end = (UINT64)(ULONG_PTR)__kernel_phys_end;
    g_pmm.bitmap_phys = PAGE_ALIGN_UP(kernel_end);
    UINT64 bitmap_pages = BYTES_TO_PAGES(g_pmm.bitmap_bytes);

    /* Start with everything marked used, then free the available regions. */
    memset(pmm_bitmap(), 0xFF, g_pmm.bitmap_bytes);
    g_pmm.free_pages = 0;
    g_pmm.used_pages = g_pmm.total_pages;

    for (UINT32 i = 0; i < map->count; i++) {
        const MM_MEMORY_REGION *r = &map->regions[i];
        if (r->type == MmRegionAvailable)
            free_range(r->base, r->length);
    }

    /* Reserve what must never be handed out. */
    reserve_range(0, 0x100000);                                  /* low 1 MiB      */
    reserve_range(KERNEL_PHYS_BASE, kernel_end - KERNEL_PHYS_BASE); /* kernel image */
    reserve_range(g_pmm.bitmap_phys, bitmap_pages << PAGE_SHIFT);   /* the bitmap   */

    g_pmm.next_hint = 0x100000 >> PAGE_SHIFT;

    KeLog("[mm]   pmm: %lu MiB usable, %lu frames free (bitmap @ 0x%lx, %lu KiB)\n",
          (unsigned long)((g_pmm.free_pages << PAGE_SHIFT) >> 20),
          (unsigned long)g_pmm.free_pages,
          (unsigned long)g_pmm.bitmap_phys,
          (unsigned long)(g_pmm.bitmap_bytes >> 10));
}

UINT64 MmAllocatePage(void)
{
    for (UINT64 scan = 0; scan < g_pmm.total_pages; scan++) {
        UINT64 f = g_pmm.next_hint + scan;
        if (f >= g_pmm.total_pages)
            f -= g_pmm.total_pages; /* wrap */

        if (!bit_test(f)) {
            bit_set(f);
            g_pmm.free_pages--;
            g_pmm.used_pages++;
            g_pmm.next_hint = f + 1;
            return f << PAGE_SHIFT;
        }
    }
    return MM_INVALID_PHYS;
}

UINT64 MmAllocatePages(SIZE_T count)
{
    if (count == 0)
        return MM_INVALID_PHYS;
    if (count == 1)
        return MmAllocatePage();

    /* First-fit search for `count` contiguous free frames. */
    UINT64 run = 0, start = 0;
    for (UINT64 f = 0; f < g_pmm.total_pages; f++) {
        if (!bit_test(f)) {
            if (run == 0)
                start = f;
            if (++run == count) {
                for (UINT64 k = start; k < start + count; k++)
                    bit_set(k);
                g_pmm.free_pages -= count;
                g_pmm.used_pages += count;
                return start << PAGE_SHIFT;
            }
        } else {
            run = 0;
        }
    }
    return MM_INVALID_PHYS;
}

void MmFreePages(UINT64 phys, SIZE_T count)
{
    UINT64 first = phys >> PAGE_SHIFT;
    for (UINT64 f = first; f < first + count && f < g_pmm.total_pages; f++) {
        if (bit_test(f)) {
            bit_clear(f);
            g_pmm.free_pages++;
            g_pmm.used_pages--;
        }
    }
    if (first < g_pmm.next_hint)
        g_pmm.next_hint = first;
}

UINT64 MmTotalPages(void)     { return g_pmm.total_pages; }
UINT64 MmFreePageCount(void)  { return g_pmm.free_pages; }
