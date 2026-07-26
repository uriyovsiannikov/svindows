/*
 * ntos/ldr.h - the image loader (Ldr).
 *
 * Loads PE/COFF images into the current address space's user region and links
 * them together by resolving imports against loaded modules (a real ntdll).
 */
#ifndef _NTOS_LDR_H_
#define _NTOS_LDR_H_

#include <nt/ntdef.h>
#include <nt/ntstatus.h>

/*
 * LdrLoadExecutable - load a PE executable and everything it imports, ready to
 * run in ring 3.
 *
 * @file:      raw image bytes (kernel-readable).
 * @file_size: size of the raw image.
 * @entry_out: receives the user virtual address of the entry point.
 *
 * The image is mapped at its preferred base, its imports are resolved (loading
 * dependency DLLs such as ntdll on demand and patching the IAT), and per-section
 * page permissions are applied. @base_out (optional) receives the load base.
 */
NTSTATUS LdrLoadExecutable(const void *file, SIZE_T file_size,
                           UINT64 *entry_out, UINT64 *base_out);

/* Resolve an exported routine's address in a loaded module. */
UINT64 LdrGetProcAddress(UINT64 module_base, const char *name);

#endif /* _NTOS_LDR_H_ */
