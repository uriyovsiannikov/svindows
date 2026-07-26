/*
 * mm/multiboot.c - turn the Multiboot2 information block into an MM_MEMORY_MAP.
 *
 * Called while the low 1 GiB is still identity-mapped, so the physical info
 * pointer GRUB gave us can be dereferenced directly. Everything we need is
 * copied into the caller's MM_MEMORY_MAP, after which the boot block can be
 * reused as ordinary RAM.
 */
#include <ntos/mm.h>
#include <ntos/multiboot2.h>
#include <ntos/gfx.h>

const char *MmRegionTypeName(MM_REGION_TYPE type)
{
    switch (type) {
    case MmRegionAvailable:       return "available";
    case MmRegionReserved:        return "reserved";
    case MmRegionAcpiReclaimable: return "ACPI-reclaim";
    case MmRegionAcpiNvs:         return "ACPI-NVS";
    case MmRegionBad:             return "bad";
    default:                      return "unknown";
    }
}

static MM_REGION_TYPE translate_type(UINT32 mb_type)
{
    switch (mb_type) {
    case MB_MEMORY_AVAILABLE:        return MmRegionAvailable;
    case MB_MEMORY_ACPI_RECLAIMABLE: return MmRegionAcpiReclaimable;
    case MB_MEMORY_NVS:              return MmRegionAcpiNvs;
    case MB_MEMORY_BADRAM:           return MmRegionBad;
    default:                         return MmRegionReserved;
    }
}

BOOLEAN MmParseMultibootMemoryMap(UINT64 mb_info_phys, MM_MEMORY_MAP *out)
{
    out->count = 0;
    out->highest_address = 0;
    out->total_available = 0;

    if (mb_info_phys == 0)
        return FALSE;

    MB_INFO_HEADER *hdr = (MB_INFO_HEADER *)MmPhysToVirt(mb_info_phys);
    UINT64 end = mb_info_phys + hdr->total_size;

    /* Walk the tag list. Tags are 8-byte aligned; the list ends at MB_TAG_END. */
    UINT64 cursor = mb_info_phys + sizeof(MB_INFO_HEADER);
    while (cursor + sizeof(MB_TAG) <= end) {
        MB_TAG *tag = (MB_TAG *)MmPhysToVirt(cursor);
        if (tag->type == MB_TAG_TYPE_END)
            break;

        if (tag->type == MB_TAG_TYPE_FRAMEBUFFER) {
            MB_TAG_FRAMEBUFFER *fb = (MB_TAG_FRAMEBUFFER *)tag;
            GfxFramebuffer.PhysAddr = fb->addr;
            GfxFramebuffer.Pitch = fb->pitch;
            GfxFramebuffer.Width = fb->width;
            GfxFramebuffer.Height = fb->height;
            GfxFramebuffer.Bpp = fb->bpp;
            if (fb->fb_type == MB_FRAMEBUFFER_TYPE_RGB) {
                GfxFramebuffer.RedShift = fb->red_field_position;
                GfxFramebuffer.GreenShift = fb->green_field_position;
                GfxFramebuffer.BlueShift = fb->blue_field_position;
            } else {
                GfxFramebuffer.RedShift = 16;
                GfxFramebuffer.GreenShift = 8;
                GfxFramebuffer.BlueShift = 0;
            }
            GfxFramebuffer.Present = TRUE;
        }

        if (tag->type == MB_TAG_TYPE_MMAP) {
            MB_TAG_MMAP *mmap = (MB_TAG_MMAP *)tag;
            UINT32 entry_size = mmap->entry_size;
            UINT64 entries_end = cursor + tag->size;

            for (UINT64 e = cursor + 16;
                 e + entry_size <= entries_end && out->count < MM_MAX_REGIONS;
                 e += entry_size) {
                MB_MMAP_ENTRY *me = (MB_MMAP_ENTRY *)MmPhysToVirt(e);

                MM_MEMORY_REGION *r = &out->regions[out->count++];
                r->base = me->addr;
                r->length = me->len;
                r->type = translate_type(me->type);

                if (r->type == MmRegionAvailable) {
                    out->total_available += r->length;
                    UINT64 region_end = r->base + r->length;
                    if (region_end > out->highest_address)
                        out->highest_address = region_end;
                }
            }
        }

        /* Advance to the next 8-byte-aligned tag. */
        cursor += (tag->size + 7) & ~7ULL;
    }

    return (BOOLEAN)(out->count > 0);
}
