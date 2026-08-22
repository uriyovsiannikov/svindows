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
 * @arg_out:   receives the bootstrap argument passed in RCX.
 *
 * The image is mapped at its preferred base, its imports are resolved (loading
 * dependency DLLs such as ntdll from disk on demand and patching the IAT), and
 * per-section page permissions are applied.
 */
NTSTATUS LdrLoadExecutable(const char *filename, UINT64 *entry_out,
                           UINT64 *base_out, UINT64 *arg_out);

/* Resolve an exported routine's address in a loaded module. */
UINT64 LdrGetProcAddress(UINT64 module_base, const char *name);

/*
 * Build the process's loader module list (PEB_LDR_DATA + one
 * LDR_DATA_TABLE_ENTRY per loaded module) in a user-readable region and point
 * @peb->Ldr at it. @ldr_va / @ldr_size describe that region (already mapped
 * user-writable). Lets ring-3 kernel32 walk loaded modules the Windows way.
 */
struct _PEB;
void LdrBuildProcessModuleList(struct _PEB *peb, UINT64 ldr_va, UINT64 ldr_size);

/* Load a DLL by name at runtime and link it into PEB->Ldr; returns the load
 * base (0 on failure). Backs kernel32's LoadLibraryA. */
UINT64 LdrLoadLibrary(const char *name);

/* If @addr falls inside a loaded module's image, report the module's cache
 * name and base (for user-stack symbolization in diagnostics). */
BOOLEAN LdrDescribeUserAddress(UINT64 addr, const char **name_out,
                               UINT64 *base_out);

#endif /* _NTOS_LDR_H_ */
