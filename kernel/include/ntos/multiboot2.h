/*
 * ntos/multiboot2.h - the subset of the Multiboot2 information format we parse.
 *
 * GRUB hands the kernel a pointer to a "boot information" block: a header
 * (total_size, reserved) followed by a sequence of 8-byte-aligned tags, ending
 * with a tag of type 0. Only the tags NTOS currently consumes are described.
 */
#ifndef _NTOS_MULTIBOOT2_H_
#define _NTOS_MULTIBOOT2_H_

#include <nt/ntdef.h>

#define MULTIBOOT2_MAGIC 0x36D76289u /* value GRUB passes in EAX */

/* Tag types (kept distinct from the struct names below). */
#define MB_TAG_TYPE_END           0
#define MB_TAG_TYPE_CMDLINE       1
#define MB_TAG_TYPE_BOOT_LOADER   2
#define MB_TAG_TYPE_MODULE        3
#define MB_TAG_TYPE_BASIC_MEMINFO 4
#define MB_TAG_TYPE_MMAP          6
#define MB_TAG_TYPE_FRAMEBUFFER   8

/* Memory-map entry types (mirrors the E820 conventions). */
#define MB_MEMORY_AVAILABLE        1
#define MB_MEMORY_RESERVED         2
#define MB_MEMORY_ACPI_RECLAIMABLE 3
#define MB_MEMORY_NVS              4
#define MB_MEMORY_BADRAM           5

typedef struct PACKED _MB_INFO_HEADER {
    UINT32 total_size;
    UINT32 reserved;
} MB_INFO_HEADER;

typedef struct PACKED _MB_TAG {
    UINT32 type;
    UINT32 size;
} MB_TAG;

typedef struct PACKED _MB_TAG_BASIC_MEMINFO {
    UINT32 type;
    UINT32 size;
    UINT32 mem_lower; /* KiB below 1 MiB */
    UINT32 mem_upper; /* KiB above 1 MiB */
} MB_TAG_BASIC_MEMINFO;

typedef struct PACKED _MB_MMAP_ENTRY {
    UINT64 addr;
    UINT64 len;
    UINT32 type;
    UINT32 zero;
} MB_MMAP_ENTRY;

typedef struct PACKED _MB_TAG_MMAP {
    UINT32 type;
    UINT32 size;
    UINT32 entry_size;
    UINT32 entry_version;
    MB_MMAP_ENTRY entries[]; /* (size - 16) / entry_size entries follow */
} MB_TAG_MMAP;

#define MB_FRAMEBUFFER_TYPE_RGB 1

typedef struct PACKED _MB_TAG_FRAMEBUFFER {
    UINT32 type;
    UINT32 size;
    UINT64 addr;      /* physical address of the framebuffer */
    UINT32 pitch;     /* bytes per scanline */
    UINT32 width;
    UINT32 height;
    UINT8  bpp;       /* bits per pixel */
    UINT8  fb_type;   /* 1 = direct RGB */
    UINT16 reserved;
    /* RGB color-field layout follows for fb_type == RGB */
    UINT8  red_field_position;
    UINT8  red_mask_size;
    UINT8  green_field_position;
    UINT8  green_mask_size;
    UINT8  blue_field_position;
    UINT8  blue_mask_size;
} MB_TAG_FRAMEBUFFER;

#endif /* _NTOS_MULTIBOOT2_H_ */
