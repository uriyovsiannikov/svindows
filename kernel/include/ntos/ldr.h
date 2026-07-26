/*
 * ntos/ldr.h - the image loader (Ldr).
 *
 * Loads a PE/COFF executable from a memory buffer into the current address
 * space's user region, ready to be run in ring 3.
 */
#ifndef _NTOS_LDR_H_
#define _NTOS_LDR_H_

#include <nt/ntdef.h>
#include <nt/ntstatus.h>

/*
 * LdrLoadPeImage - map a PE32+ image into user memory.
 *
 * @file:       pointer to the raw image bytes (kernel-readable).
 * @file_size:  size of the raw image.
 * @entry_out:  receives the user virtual address of the entry point.
 * @base_out:   receives the user virtual address the image was loaded at.
 *
 * Sections are placed at ImageBase + RVA, zero-filled beyond their raw data,
 * base-relocated if loaded away from the preferred base, and given per-section
 * page permissions.
 */
NTSTATUS LdrLoadPeImage(const void *file, SIZE_T file_size,
                        UINT64 *entry_out, UINT64 *base_out);

#endif /* _NTOS_LDR_H_ */
