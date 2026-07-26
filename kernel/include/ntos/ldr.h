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
 * LdrLoadExecutable - load a PE executable (by filename, read from the mounted
 * filesystem) and everything it imports, ready to run in ring 3.
 *
 * @filename:  name of the executable in the filesystem root (8.3).
 * @entry_out: receives the user virtual address of the entry point.
 * @base_out:  (optional) receives the load base.
 *
 * The image is mapped at its preferred base, its imports are resolved (loading
 * dependency DLLs such as ntdll from disk on demand and patching the IAT), and
 * per-section page permissions are applied.
 */
NTSTATUS LdrLoadExecutable(const char *filename, UINT64 *entry_out,
                           UINT64 *base_out);

/* Resolve an exported routine's address in a loaded module. */
UINT64 LdrGetProcAddress(UINT64 module_base, const char *name);

#endif /* _NTOS_LDR_H_ */
