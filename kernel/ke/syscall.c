/*
 * ke/syscall.c - the system-call boundary: MSR setup, the per-CPU block, the
 * service dispatcher, and the first handful of Nt* services.
 *
 * This is the interface native (ring 3) code will eventually reach through
 * ntdll. For now a tiny in-kernel user stub exercises it directly.
 */
#include <ntos/ke.h>
#include <ntos/mm.h>
#include <ntos/io.h>
#include <ntos/ps.h>
#include <ntos/ldr.h>
#include <nt/ntstatus.h>

/* --- Model-specific registers we program. --- */
#define MSR_EFER          0xC0000080 /* bit 0 = SCE (SYSCALL enable)      */
#define MSR_STAR          0xC0000081 /* segment selectors for syscall/ret */
#define MSR_LSTAR         0xC0000082 /* 64-bit SYSCALL entry RIP          */
#define MSR_SFMASK        0xC0000084 /* RFLAGS bits cleared on SYSCALL    */
#define MSR_GS_BASE       0xC0000101
#define MSR_KERNEL_GS_BASE 0xC0000102

/*
 * KPCR - per-CPU block reached via GS after swapgs. The assembly entry relies
 * on the exact offsets of the first two fields (0 and 8).
 */
typedef struct _KPCR {
    UINT64   UserRspScratch; /* +0: user RSP saved during syscall entry */
    UINT64   KernelRsp;      /* +8: kernel stack loaded on syscall entry */
    PKTHREAD CurrentThread;  /* +16 */
} KPCR;

static KPCR g_kpcr;

extern void KiSystemCallEntry(void); /* arch/x86_64/syscall_entry.asm */

static ALWAYS_INLINE void wrmsr(UINT32 msr, UINT64 value)
{
    __asm__ volatile("wrmsr"
                     :
                     : "c"(msr), "a"((UINT32)value), "d"((UINT32)(value >> 32)));
}

static ALWAYS_INLINE UINT64 rdmsr(UINT32 msr)
{
    UINT32 lo, hi;
    __asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
    return ((UINT64)hi << 32) | lo;
}

void KeSetKernelStack(UINT64 kernel_rsp)
{
    g_kpcr.KernelRsp = kernel_rsp;
    KeSetTssRsp0(kernel_rsp);
}

void KeSetUserGsBase(UINT64 teb)
{
    /* KERNEL_GS_BASE holds what swapgs will make the active GS on the next
     * kernel exit. For a user thread that is its TEB; for a kernel thread we
     * park the per-CPU block there. Kept correct on every context switch, so a
     * thread that terminates mid-syscall can't leave it stale. */
    wrmsr(MSR_KERNEL_GS_BASE, teb ? teb : (UINT64)&g_kpcr);
}

void KiInitializeSystemCalls(void)
{
    /* Invariant: in ring 0 the active GS base is the per-CPU block (KPCR). The
     * "other" base holds the value swapgs will restore on the next kernel exit;
     * it starts as the KPCR too and is repointed at each thread's TEB by the
     * scheduler (KeSetUserGsBase). */
    wrmsr(MSR_GS_BASE, (UINT64)&g_kpcr);
    wrmsr(MSR_KERNEL_GS_BASE, (UINT64)&g_kpcr);

    /* Enable the SYSCALL/SYSRET instructions. */
    wrmsr(MSR_EFER, rdmsr(MSR_EFER) | 1);

    /*
     * STAR selectors:
     *   [47:32] = 0x08 -> SYSCALL sets CS=0x08 (kernel code), SS=0x10.
     *   [63:48] = 0x10 -> SYSRET sets SS=0x18 (user data|3), CS=0x20 (user code|3).
     */
    wrmsr(MSR_STAR, ((UINT64)0x10 << 48) | ((UINT64)0x08 << 32));
    wrmsr(MSR_LSTAR, (UINT64)&KiSystemCallEntry);

    /* Clear IF, DF, TF on entry: syscalls run non-preemptible for now. */
    wrmsr(MSR_SFMASK, 0x700);

    KeLog("[ke]   syscall path armed (LSTAR=%p)\n", (void *)&KiSystemCallEntry);
}

/* ------------------------------------------------------------------ */
/* Nt* system services                                                */
/* ------------------------------------------------------------------ */

/* 0: print a NUL-terminated string that lives in user memory. */
static UINT64 NtDisplayString(UINT64 user_ptr, UINT64 a2, UINT64 a3, UINT64 a4)
{
    (void)a2; (void)a3; (void)a4;
    /* Ring 0 may read the user page directly (same address space). A real
     * implementation would validate and capture the buffer first. */
    KeLog("[user] %s\n", (const char *)user_ptr);
    return 0; /* STATUS_SUCCESS */
}

/* 1: print an integer argument. */
static UINT64 NtDisplayNumber(UINT64 value, UINT64 a2, UINT64 a3, UINT64 a4)
{
    (void)a2; (void)a3; (void)a4;
    KeLog("[user] NtDisplayNumber: %lu (0x%lx)\n",
          (unsigned long)value, (unsigned long)value);
    return 0;
}

/* 2: terminate the calling thread; does not return to the caller. */
static UINT64 NtTerminateThread(UINT64 a1, UINT64 a2, UINT64 a3, UINT64 a4)
{
    (void)a1; (void)a2; (void)a3; (void)a4;
    KeLog("[user] NtTerminateThread requested; ending user thread\n");
    KeTerminateThread();
    return 0; /* unreachable */
}

/* Simple bump allocator for user-mode virtual memory, well clear of the image,
 * stack, and TEB/PEB regions. */
static UINT64 g_user_alloc_next = 0x0000000020000000ULL;

/* 3: allocate `size` bytes of user memory; returns the base address (or 0). A
 * placeholder for the real NtAllocateVirtualMemory (which takes a base pointer,
 * region size, type, and protection). */
static UINT64 NtAllocateVirtualMemory(UINT64 size, UINT64 a2, UINT64 a3,
                                      UINT64 a4)
{
    (void)a2; (void)a3; (void)a4;
    if (size == 0)
        return 0;

    UINT64 pages = BYTES_TO_PAGES(size);
    UINT64 base = g_user_alloc_next;
    for (UINT64 i = 0; i < pages; i++) {
        UINT64 pa = MmAllocatePage();
        if (pa == MM_INVALID_PHYS)
            return 0;
        MmMapPage(base + i * PAGE_SIZE, pa, PTE_USER | PTE_WRITE);
    }
    g_user_alloc_next += pages * PAGE_SIZE;

    KeLog("[user] NtAllocateVirtualMemory(%lu) -> %p\n",
          (unsigned long)size, (void *)base);
    return base;
}

/* 12: load a DLL by name (user string) at runtime; returns its base or 0. */
static UINT64 NtLoadLibrary(UINT64 name_ptr, UINT64 a2, UINT64 a3, UINT64 a4)
{
    (void)a2; (void)a3; (void)a4;
    if (name_ptr == 0)
        return 0;
    /* Same address space: ring 0 can read the user name string directly. */
    UINT64 base = LdrLoadLibrary((const char *)name_ptr);
    KeLog("[user] NtLoadLibrary('%s') -> %p\n", (const char *)name_ptr,
          (void *)base);
    return base;
}

typedef UINT64 (*KI_SERVICE)(UINT64, UINT64, UINT64, UINT64);

static KI_SERVICE KiServiceTable[] = {
    NtDisplayString,         /* 0 */
    NtDisplayNumber,         /* 1 */
    NtTerminateThread,       /* 2 */
    NtAllocateVirtualMemory, /* 3 */
    NtCreateFile,            /* 4 */
    NtReadFile,              /* 5 */
    NtWriteFile,             /* 6 */
    NtClose,                 /* 7 */
    NtCreateEvent,           /* 8 */
    NtSetEvent,              /* 9 */
    NtWaitForSingleObject,   /* 10 */
    NtCreateThread,          /* 11 */
    NtLoadLibrary,           /* 12 */
};

#define KI_SERVICE_COUNT (sizeof(KiServiceTable) / sizeof(KiServiceTable[0]))

UINT64 KiSystemServiceDispatch(UINT64 number, UINT64 a1, UINT64 a2, UINT64 a3,
                               UINT64 a4)
{
    if (number >= KI_SERVICE_COUNT) {
        KeLog("[ke]   invalid system service %lu\n", (unsigned long)number);
        return (UINT64)STATUS_NOT_IMPLEMENTED;
    }
    return KiServiceTable[number](a1, a2, a3, a4);
}
