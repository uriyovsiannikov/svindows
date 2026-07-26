/*
 * mm/mm_init.c - Memory Manager bring-up.
 *
 * Owns the direct-map base (0 while the identity map is live, MM_DIRECT_MAP_BASE
 * afterwards) and sequences the three stages: parse the firmware memory map,
 * start the physical allocator, then install the kernel's own page tables.
 */
#include <ntos/mm.h>
#include <ntos/ke.h>

/* 0 => identity map (early boot); becomes MM_DIRECT_MAP_BASE once VMM is up. */
UINT64 MmDirectMapBase = 0;

static MM_MEMORY_MAP g_memory_map;

void MmInitialize(UINT64 mb_info_phys)
{
    KeLog("[mm]   parsing Multiboot2 memory map...\n");
    if (!MmParseMultibootMemoryMap(mb_info_phys, &g_memory_map))
        KeBugCheck(KE_PHASE0_INITIALIZATION_FAILED,
                   "bootloader supplied no usable memory map");

    for (UINT32 i = 0; i < g_memory_map.count; i++) {
        const MM_MEMORY_REGION *r = &g_memory_map.regions[i];
        KeLog("[mm]   e820[%2u] 0x%016lx..0x%016lx  %s\n",
              i, (unsigned long)r->base,
              (unsigned long)(r->base + r->length),
              MmRegionTypeName(r->type));
    }
    KeLog("[mm]   %lu MiB available, top of RAM at 0x%lx\n",
          (unsigned long)(g_memory_map.total_available >> 20),
          (unsigned long)g_memory_map.highest_address);

    MmInitializePhysicalMemory(&g_memory_map);
    MmInitializeVirtualMemory(&g_memory_map);
}
