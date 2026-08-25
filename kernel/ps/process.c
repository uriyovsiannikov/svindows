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
#include <ntos/win32k.h>
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

PKTHREAD PsCreateUserProcess(const char *name, const char *command_line,
                             UINT64 entry, UINT64 image_base,
                             UINT64 stack_base, UINT64 stack_top,
                             UINT64 start_argument)
{
    /* PEB: what the image is and where it loaded. */
    PPEB peb = map_user_rw(USER_PEB_VA);
    memset(peb, 0, sizeof(*peb));
    peb->ImageBaseAddress = (PVOID)image_base;

    /* GDI32 validates kernel GDI handles through the shared handle table at
     * PEB+0xF8. Map the complete 16-bit table now; entries remain empty until
     * win32k creates actual brushes, fonts, surfaces, and DCs. */
    for (UINT64 off = 0; off < PROCESS_GDI_SHARED_TABLE_SIZE;
         off += PAGE_SIZE) {
        void *page = map_user_rw(PROCESS_GDI_SHARED_TABLE_VA + off);
        memset(page, 0, PAGE_SIZE);
    }
    peb->GdiSharedHandleTable = (PVOID)PROCESS_GDI_SHARED_TABLE_VA;

    /* The attribute blocks a GDI cell's pUserInfo points at. GDI32 writes DC
     * state into them (SetTextColor and SetBkColor never enter the kernel), so
     * unlike the handle table itself this arena stays user-writable. */
    for (UINT64 off = 0; off < PROCESS_GDI_ATTR_SIZE; off += PAGE_SIZE) {
        void *page = map_user_rw(PROCESS_GDI_ATTR_VA + off);
        memset(page, 0, PAGE_SIZE);
    }
    W32kInitializeGdi();

    /* USER32 validates HWND values through SHAREDINFO.aheList without entering
     * the kernel. Keep the native 24-byte HANDLEENTRY array and initial shared
     * window-object arena mapped at the addresses published by ntdll. */
    for (UINT64 off = 0; off < PROCESS_USER_SHARED_TABLE_SIZE;
         off += PAGE_SIZE) {
        void *page = map_user_rw(PROCESS_USER_SHARED_TABLE_VA + off);
        memset(page, 0, PAGE_SIZE);
    }
    /* The handle table is win32k-owned: ring 3 only reads it. Write access is
     * enabled transiently around kernel-side fills (see NtUserCreateWindowEx),
     * which also turns any client that misbehaves into a named trap instead
     * of silent corruption. */
    MmProtectRange(PROCESS_USER_SHARED_TABLE_VA, PROCESS_USER_SHARED_TABLE_SIZE,
                   FALSE);
    for (UINT64 off = 0; off < PROCESS_USER_OBJECT_ARENA_SIZE;
         off += PAGE_SIZE) {
        void *page = map_user_rw(PROCESS_USER_OBJECT_ARENA_VA + off);
        memset(page, 0, PAGE_SIZE);
    }

    /* USER32's client-side HWND validator (IsWindow and every window API's
     * fast path) checks the object head against a per-thread desktop-heap
     * range held in the TEB's Win32ClientInfo (+0x800 block): [+0x820] points
     * at a {start, end} pair and [+0x828] is the heap delta subtracted from
     * heads (kept at zero so heads stay absolute). Publish the window arena
     * as that heap; child threads inherit the block via psobj's memcpy. */
    UINT64 *range = map_user_rw(PROCESS_USER_OBJECT_ARENA_VA +
                                PROCESS_USER_OBJECT_ARENA_SIZE);
    range[0] = PROCESS_USER_OBJECT_ARENA_VA;
    range[1] = PROCESS_USER_OBJECT_ARENA_VA + PROCESS_USER_OBJECT_ARENA_SIZE;

    /* Loader module list, so ring-3 code can enumerate loaded modules. */
    for (UINT64 off = 0; off < PROCESS_LDR_SIZE; off += PAGE_SIZE)
        map_user_rw(PROCESS_LDR_VA + off);
    LdrBuildProcessModuleList(peb, PROCESS_LDR_VA, PROCESS_LDR_SIZE);

    /* Process parameters: a minimal RTL_USER_PROCESS_PARAMETERS carrying the
     * command line (ImagePathName at 0x60, CommandLine at 0x70), so GetCommandLine
     * works. The wide strings live past the struct in the same page. */
    UINT8 *params = map_user_rw(PROCESS_PARAMS_VA);
    memset(params, 0, PAGE_SIZE);
    UINT16 *imagew = (UINT16 *)(params + 0x200);
    UINT16 image_n = 0;
    for (; name[image_n] && image_n < 200; image_n++)
        imagew[image_n] = (UINT16)(UCHAR)name[image_n];
    imagew[image_n] = 0;
    *(UINT16 *)(params + 0x60) = (UINT16)(image_n * 2);
    *(UINT16 *)(params + 0x62) = (UINT16)(image_n * 2 + 2);
    *(void **)(params + 0x68) = imagew;

    UINT16 *cmdw = (UINT16 *)(params + 0x400);
    UINT16 cmd_n = 0;
    for (; command_line[cmd_n] && cmd_n < 500; cmd_n++)
        cmdw[cmd_n] = (UINT16)(UCHAR)command_line[cmd_n];
    cmdw[cmd_n] = 0;
    *(UINT16 *)(params + 0x70) = (UINT16)(cmd_n * 2);
    *(UINT16 *)(params + 0x72) = (UINT16)(cmd_n * 2 + 2);
    *(void **)(params + 0x78) = cmdw;
    peb->ProcessParameters = params;

    /* An ANSI copy of the same command line, past the wide one, for the CRT's
     * `_acmdln` data export (see LdrSeedCrtCommandLine). */
    char *cmda = (char *)(params + 0x800);
    for (UINT16 i = 0; i <= cmd_n; i++)
        cmda[i] = command_line[i];
    LdrSeedCrtCommandLine((UINT64)cmda, (UINT64)cmdw);

    /* TEB: the per-thread block GS resolves to in ring 3. */
    for (UINT64 off = 0; off < PROCESS_TEB_SIZE; off += PAGE_SIZE)
        map_user_rw(USER_TEB_VA + off);
    PTEB teb = (PTEB)USER_TEB_VA;
    memset(teb, 0, PROCESS_TEB_SIZE);
    teb->NtTib.Self = (struct _NT_TIB *)USER_TEB_VA;
    {
        UINT64 *client = (UINT64 *)((UINT8 *)teb + 0x800);
        /* CLIENTINFO.pDeskInfo (+0x20) and .ulClientDelta (+0x28) -- the
         * DESKTOPINFO USER32's client-side window fast paths use, starting with
         * { pvDesktopBase, pvDesktopLimit }. Deliberately left NULL: the page
         * above the window arena carries that range pair and publishing it does
         * make USER32 take those paths, but they then reach for the rest of the
         * structure (DESKTOPINFO.spwnd, the shell/Progman windows) and end up
         * calling a NULL window procedure -- a ring-3 #PF at USER32+0x121e4
         * with RIP=0 through the CFG dispatch. Publishing a DESKTOPINFO is the
         * step that needs a real desktop window behind it. USER32 reads only
         * +0x20/+0x28 here, never +0x100/+0x108, which an earlier version of
         * this code wrote. */
        client[4] = 0;
        client[5] = 0;
        /* CLIENTINFO.dwExpWinVer (+0x10). USER32 compares it against 0x400 on
         * the way into GetSystemMetrics and other entry points and diverts
         * every caller to its Windows-3.x compatibility path when it is lower;
         * zero (a freshly cleared TEB) is lower. Report Windows 10. */
        *(UINT32 *)((UINT8 *)teb + 0x810) = 0x0A00;
    }
    teb->NtTib.StackBase = (PVOID)stack_top;
    teb->NtTib.StackLimit = (PVOID)stack_base;
    teb->ProcessEnvironmentBlock = (PVOID)USER_PEB_VA;
    teb->ClientId.UniqueProcess = (HANDLE)(ULONG_PTR)PROCESS_CLIENT_ID;
    teb->ClientId.UniqueThread = (HANDLE)(ULONG_PTR)1;

    KeLog("[ps]   process '%s': PEB @ %p (ImageBase %p), TEB @ %p\n",
          name, (void *)USER_PEB_VA, (void *)image_base, (void *)USER_TEB_VA);

    return KeCreateUserThread(name, entry, stack_top, USER_TEB_VA,
                              start_argument, 8);
}
