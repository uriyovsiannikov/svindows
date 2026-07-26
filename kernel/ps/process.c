/*
 * ps/process.c - user process creation: PEB, TEB, and the main thread.
 *
 * Lays out the process environment the way native code expects to find it:
 * a PEB carrying the image base, and a TEB (reached via GS in ring 3) whose
 * self-pointer and PEB pointer sit at the canonical offsets (0x30 and 0x60).
 */
#include <ntos/ps.h>
#include <ntos/mm.h>
#include <ntos/ke.h>
#include <ntos/rtl.h>
#include <ntos/ldr.h>
#include <nt/peb.h>

/* Fixed user addresses for the single process we currently support (shared with
 * ps.h so additional threads can find the PEB). */
#define USER_PEB_VA PROCESS_PEB_VA
#define USER_TEB_VA PROCESS_MAIN_TEB_VA

static void *map_user_rw(UINT64 va)
{
    UINT64 pa = MmAllocatePage();
    if (pa == MM_INVALID_PHYS)
        KeBugCheck(KE_PHASE0_INITIALIZATION_FAILED, "no memory for PEB/TEB");
    MmMapPage(va, pa, PTE_USER | PTE_WRITE);
    return (void *)va;
}

PKTHREAD PsCreateUserProcess(const char *name, UINT64 entry, UINT64 image_base,
                             UINT64 stack_base, UINT64 stack_top)
{
    /* PEB: what the image is and where it loaded. */
    PPEB peb = map_user_rw(USER_PEB_VA);
    memset(peb, 0, sizeof(*peb));
    peb->ImageBaseAddress = (PVOID)image_base;

    /* Loader module list, so ring-3 code can enumerate loaded modules. */
    for (UINT64 off = 0; off < PROCESS_LDR_SIZE; off += PAGE_SIZE)
        map_user_rw(PROCESS_LDR_VA + off);
    LdrBuildProcessModuleList(peb, PROCESS_LDR_VA, PROCESS_LDR_SIZE);

    /* TEB: the per-thread block GS resolves to in ring 3. */
    PTEB teb = map_user_rw(USER_TEB_VA);
    memset(teb, 0, sizeof(*teb));
    teb->NtTib.Self = (struct _NT_TIB *)USER_TEB_VA;
    teb->NtTib.StackBase = (PVOID)stack_top;
    teb->NtTib.StackLimit = (PVOID)stack_base;
    teb->ProcessEnvironmentBlock = (PVOID)USER_PEB_VA;
    teb->ClientId.UniqueProcess = (HANDLE)(ULONG_PTR)1;
    teb->ClientId.UniqueThread = (HANDLE)(ULONG_PTR)1;

    KeLog("[ps]   process '%s': PEB @ %p (ImageBase %p), TEB @ %p\n",
          name, (void *)USER_PEB_VA, (void *)image_base, (void *)USER_TEB_VA);

    return KeCreateUserThread(name, entry, stack_top, USER_TEB_VA, 0, 8);
}
