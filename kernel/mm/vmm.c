/*
 * mm/vmm.c - the virtual memory manager: real 4-level page tables.
 *
 * Boot left us on the trampoline's tables (low 1 GiB identity + a 1 GiB
 * higher-half window). Here we build the kernel's own PML4 from scratch:
 *
 *   - the kernel image window at -2 GiB (0xFFFFFFFF80000000), and
 *   - a direct map of all physical RAM at 0xFFFF800000000000,
 *
 * then switch CR3 to it and abandon the identity map. From that point on
 * MmPhysToVirt resolves through the direct map. New fine-grained mappings (for
 * example the kernel heap) are added with MmMapPage.
 */
#include <ntos/mm.h>
#include <ntos/ke.h>
#include <ntos/hal.h>
#include <ntos/rtl.h>

static UINT64 g_kernel_pml4_phys;

#define ENTRIES_PER_TABLE 512
#define PML4_INDEX(va) (((va) >> 39) & 0x1FF)
#define PDPT_INDEX(va) (((va) >> 30) & 0x1FF)
#define PD_INDEX(va)   (((va) >> 21) & 0x1FF)
#define PT_INDEX(va)   (((va) >> 12) & 0x1FF)

static ALWAYS_INLINE UINT64 *table_at(UINT64 phys)
{
    return (UINT64 *)MmPhysToVirt(phys);
}

static ALWAYS_INLINE void invlpg(UINT64 va)
{
    __asm__ volatile("invlpg (%0)" : : "r"(va) : "memory");
}

static ALWAYS_INLINE void load_cr3(UINT64 pml4_phys)
{
    __asm__ volatile("mov %0, %%cr3" : : "r"(pml4_phys) : "memory");
}

static UINT64 alloc_zeroed_table(void)
{
    UINT64 phys = MmAllocatePage();
    if (phys == MM_INVALID_PHYS)
        KeBugCheck(KE_PHASE0_INITIALIZATION_FAILED,
                   "out of physical memory building page tables");
    memset(table_at(phys), 0, PAGE_SIZE);
    return phys;
}

/*
 * Return the phys of the next-level table under *entry, allocating it if absent.
 * Intermediate entries are marked writable and user-accessible; the actual
 * privilege of any page is decided by the leaf PTE (access requires U/S set at
 * every level, so a supervisor leaf stays kernel-only regardless).
 */
static UINT64 next_table(UINT64 *entry)
{
    if (!(*entry & PTE_PRESENT)) {
        UINT64 t = alloc_zeroed_table();
        *entry = t | PTE_PRESENT | PTE_WRITE | PTE_USER;
        return t;
    }
    /* Ensure an existing intermediate also permits user access below it. */
    *entry |= PTE_USER;
    return *entry & PTE_ADDR_MASK;
}

/* Map a single 2 MiB page. Used only while building the initial address space. */
static void map_large(UINT64 pml4_phys, UINT64 va, UINT64 pa, UINT64 flags)
{
    UINT64 *pml4 = table_at(pml4_phys);
    UINT64 pdpt_phys = next_table(&pml4[PML4_INDEX(va)]);
    UINT64 *pdpt = table_at(pdpt_phys);
    UINT64 pd_phys = next_table(&pdpt[PDPT_INDEX(va)]);
    UINT64 *pd = table_at(pd_phys);

    pd[PD_INDEX(va)] = (pa & ~(LARGE_PAGE_SIZE - 1)) | flags | PTE_LARGE;
}

void MmInitializeVirtualMemory(const MM_MEMORY_MAP *map)
{
    g_kernel_pml4_phys = alloc_zeroed_table();

    /* Kernel image window: virtual -2 GiB..-1 GiB -> physical 0..1 GiB. */
    for (UINT64 off = 0; off < 0x40000000ULL; off += LARGE_PAGE_SIZE)
        map_large(g_kernel_pml4_phys, KERNEL_VIRT_BASE + off, off,
                  PTE_PRESENT | PTE_WRITE | PTE_GLOBAL);

    /* Direct map: cover all physical RAM at MM_DIRECT_MAP_BASE with 2 MiB pages. */
    UINT64 top = (map->highest_address + LARGE_PAGE_SIZE - 1) & ~(LARGE_PAGE_SIZE - 1);
    if (top < 0x40000000ULL)
        top = 0x40000000ULL; /* always map at least the low 1 GiB */
    for (UINT64 pa = 0; pa < top; pa += LARGE_PAGE_SIZE)
        map_large(g_kernel_pml4_phys, MM_DIRECT_MAP_BASE + pa, pa,
                  PTE_PRESENT | PTE_WRITE | PTE_GLOBAL);

    /* Commit: switch to the new tables and start resolving physical addresses
     * through the direct map. These two lines must stay adjacent - after the
     * CR3 load the identity map is gone, so MmDirectMapBase must be updated
     * before any further MmPhysToVirt call. */
    load_cr3(g_kernel_pml4_phys);
    MmDirectMapBase = MM_DIRECT_MAP_BASE;

    /* The VGA framebuffer pointer was computed against the identity map; move
     * it onto the direct map. */
    HalVgaRelocate();

    KeLog("[mm]   vmm: kernel PML4 @ 0x%lx, direct map covers %lu MiB\n",
          (unsigned long)g_kernel_pml4_phys, (unsigned long)(top >> 20));
    KeLog("[mm]   vmm: identity map dropped; direct map base 0x%lx\n",
          (unsigned long)MM_DIRECT_MAP_BASE);
}

BOOLEAN MmMapPage(UINT64 virt, UINT64 phys, UINT64 flags)
{
    UINT64 *pml4 = table_at(g_kernel_pml4_phys);
    UINT64 pdpt_phys = next_table(&pml4[PML4_INDEX(virt)]);
    UINT64 *pdpt = table_at(pdpt_phys);
    UINT64 pd_phys = next_table(&pdpt[PDPT_INDEX(virt)]);
    UINT64 *pd = table_at(pd_phys);

    if (pd[PD_INDEX(virt)] & PTE_LARGE)
        return FALSE; /* would have to split a 2 MiB page; unsupported */

    UINT64 pt_phys = next_table(&pd[PD_INDEX(virt)]);
    UINT64 *pt = table_at(pt_phys);

    pt[PT_INDEX(virt)] = (phys & PTE_ADDR_MASK) | flags | PTE_PRESENT;
    invlpg(virt);
    return TRUE;
}

BOOLEAN MmUnmapPage(UINT64 virt)
{
    UINT64 *pml4 = table_at(g_kernel_pml4_phys);
    if (!(pml4[PML4_INDEX(virt)] & PTE_PRESENT))
        return FALSE;
    UINT64 *pdpt = table_at(pml4[PML4_INDEX(virt)] & PTE_ADDR_MASK);
    if (!(pdpt[PDPT_INDEX(virt)] & PTE_PRESENT))
        return FALSE;
    UINT64 *pd = table_at(pdpt[PDPT_INDEX(virt)] & PTE_ADDR_MASK);
    if (!(pd[PD_INDEX(virt)] & PTE_PRESENT) || (pd[PD_INDEX(virt)] & PTE_LARGE))
        return FALSE;
    UINT64 *pt = table_at(pd[PD_INDEX(virt)] & PTE_ADDR_MASK);

    pt[PT_INDEX(virt)] = 0;
    invlpg(virt);
    return TRUE;
}

UINT64 MmGetPhysicalAddress(UINT64 virt)
{
    UINT64 *pml4 = table_at(g_kernel_pml4_phys);
    if (!(pml4[PML4_INDEX(virt)] & PTE_PRESENT))
        return MM_INVALID_PHYS;
    UINT64 *pdpt = table_at(pml4[PML4_INDEX(virt)] & PTE_ADDR_MASK);
    if (!(pdpt[PDPT_INDEX(virt)] & PTE_PRESENT))
        return MM_INVALID_PHYS;
    UINT64 *pd = table_at(pdpt[PDPT_INDEX(virt)] & PTE_ADDR_MASK);
    UINT64 pde = pd[PD_INDEX(virt)];
    if (!(pde & PTE_PRESENT))
        return MM_INVALID_PHYS;
    if (pde & PTE_LARGE)
        return (pde & ~(LARGE_PAGE_SIZE - 1)) | (virt & (LARGE_PAGE_SIZE - 1));

    UINT64 *pt = table_at(pde & PTE_ADDR_MASK);
    UINT64 pte = pt[PT_INDEX(virt)];
    if (!(pte & PTE_PRESENT))
        return MM_INVALID_PHYS;
    return (pte & PTE_ADDR_MASK) | (virt & PAGE_MASK);
}
