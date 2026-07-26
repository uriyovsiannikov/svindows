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
#include <ntos/cm.h>
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

    KiInitializeServiceTable();

    KeLog("[ke]   syscall path armed (LSTAR=%p)\n", (void *)&KiSystemCallEntry);
}

/* ------------------------------------------------------------------ */
/* Nt* system services                                                */
/* ------------------------------------------------------------------ */

/* Services receive the full argument array (see the syscall entry), so those
 * with the real, up-to-11-argument NT signatures can read every parameter. */

/* NtDisplayString(PUNICODE-ish ptr) - print a NUL-terminated user string. */
static UINT64 NtDisplayString(UINT64 *a)
{
    /* Ring 0 may read the user page directly (same address space). A real
     * implementation would validate and capture the buffer first. */
    KeLog("[user] %s\n", (const char *)a[0]);
    return 0; /* STATUS_SUCCESS */
}

/* NtDisplayNumber(value) - print an integer argument. */
static UINT64 NtDisplayNumber(UINT64 *a)
{
    KeLog("[user] NtDisplayNumber: %lu (0x%lx)\n",
          (unsigned long)a[0], (unsigned long)a[0]);
    return 0;
}

/* NtTerminateThread - end the calling thread; does not return. */
static UINT64 NtTerminateThread(UINT64 *a)
{
    (void)a;
    KeLog("[user] NtTerminateThread requested; ending user thread\n");
    KeTerminateThread();
    return 0; /* unreachable */
}

/* Simple bump allocator for user-mode virtual memory, well clear of the image,
 * stack, and TEB/PEB regions. */
static UINT64 g_user_alloc_next = 0x0000000020000000ULL;

/*
 * NtAllocateVirtualMemory(ProcessHandle, *BaseAddress, ZeroBits, *RegionSize,
 *                         AllocationType, Protect) -> NTSTATUS.
 * The real signature: the base and size are in/out pointers. We honor a
 * requested base of 0 (choose one) and update *BaseAddress; the region grows
 * from a bump pointer.
 */
static UINT64 NtAllocateVirtualMemory(UINT64 *a)
{
    PVOID   *base_ptr = (PVOID *)a[1];
    SIZE_T  *size_ptr = (SIZE_T *)a[3];
    if (!size_ptr || *size_ptr == 0)
        return (UINT64)STATUS_INVALID_PARAMETER;

    UINT64 size = *size_ptr;
    UINT64 pages = BYTES_TO_PAGES(size);
    UINT64 base = g_user_alloc_next;
    for (UINT64 i = 0; i < pages; i++) {
        UINT64 pa = MmAllocatePage();
        if (pa == MM_INVALID_PHYS)
            return (UINT64)STATUS_NO_MEMORY;
        MmMapPage(base + i * PAGE_SIZE, pa, PTE_USER | PTE_WRITE);
    }
    g_user_alloc_next += pages * PAGE_SIZE;

    if (base_ptr)
        *base_ptr = (PVOID)base;
    *size_ptr = pages * PAGE_SIZE;

    KeLog("[user] NtAllocateVirtualMemory(%lu) -> %p\n",
          (unsigned long)size, (void *)base);
    return (UINT64)STATUS_SUCCESS;
}

/*
 * NtDelayExecution(BOOLEAN Alertable, PLARGE_INTEGER Interval) - block the
 * caller for the (relative, negative, 100 ns units) interval. Implemented as a
 * cooperative yield loop against the tick count (10 ms resolution).
 */
static UINT64 NtDelayExecution(UINT64 *a)
{
    INT64 *interval = (INT64 *)a[1];
    if (!interval)
        return (UINT64)STATUS_SUCCESS;

    INT64 iv = *interval;
    UINT64 ticks_100ns = (iv < 0) ? (UINT64)(-iv) : 0; /* only relative delays */
    UINT64 wait = ticks_100ns / 100000ULL;             /* 100 ns -> 10 ms ticks */
    if (wait == 0)
        wait = 1;

    UINT64 target = KeGetTickCount() + wait;
    while (KeGetTickCount() < target)
        KeYield();
    return (UINT64)STATUS_SUCCESS;
}

/* NtLoadLibrary(name) - load a DLL by name at runtime; returns its base or 0.
 * (Not a real NT service name; our loader hook for kernel32's LoadLibraryA.) */
static UINT64 NtLoadLibrary(UINT64 *a)
{
    if (a[0] == 0)
        return 0;
    UINT64 base = LdrLoadLibrary((const char *)a[0]);
    KeLog("[user] NtLoadLibrary('%s') -> %p\n", (const char *)a[0], (void *)base);
    return base;
}

typedef UINT64 (*KI_SERVICE)(UINT64 *args);

/*
 * The system service table, indexed by real Windows 7 SP1 x64 syscall numbers
 * (from the public NT syscall-number tables). Sparse: most slots are unused.
 * Our ntdll stubs issue these same numbers, and a service's slot holds the
 * routine implementing it. Aligning to a real build's numbering is what would
 * let a genuine ntdll drive this kernel; the exact values target Win7 SP1 x64.
 */
#define NTOS_MAX_SYSCALL 0x100
static KI_SERVICE KiServiceTable[NTOS_MAX_SYSCALL];

/* Number assignments (Win7 SP1 x64). Kept together as the single source of
 * truth, mirrored by the ntdll stubs. */
#define SN_NtWaitForSingleObject   0x01
#define SN_NtWriteFile             0x05
#define SN_NtReadFile              0x03
#define SN_NtClose                 0x0C
#define SN_NtOpenKey               0x0F
#define SN_NtAllocateVirtualMemory 0x15
#define SN_NtQueryValueKey         0x17
#define SN_NtCreateKey             0x1A
#define SN_NtSetEvent              0x02
#define SN_NtCreateEvent           0x48
#define SN_NtCreateThread          0x4B
#define SN_NtCreateFile            0x52
#define SN_NtSetValueKey           0x5D
#define SN_NtTerminateThread       0x50
#define SN_NtDelayExecution        0x31
/* NTOS-private services (no Windows equivalent) live above the real range. */
#define SN_NtDisplayString         0xF0
#define SN_NtDisplayNumber         0xF1
#define SN_NtLoadLibrary           0xF2

void KiInitializeServiceTable(void)
{
    KiServiceTable[SN_NtWaitForSingleObject]   = NtWaitForSingleObject;
    KiServiceTable[SN_NtWriteFile]             = NtWriteFile;
    KiServiceTable[SN_NtReadFile]              = NtReadFile;
    KiServiceTable[SN_NtClose]                 = NtClose;
    KiServiceTable[SN_NtOpenKey]               = NtOpenKey;
    KiServiceTable[SN_NtAllocateVirtualMemory] = NtAllocateVirtualMemory;
    KiServiceTable[SN_NtQueryValueKey]         = NtQueryValueKey;
    KiServiceTable[SN_NtCreateKey]             = NtCreateKey;
    KiServiceTable[SN_NtSetEvent]              = NtSetEvent;
    KiServiceTable[SN_NtCreateEvent]           = NtCreateEvent;
    KiServiceTable[SN_NtCreateThread]          = NtCreateThread;
    KiServiceTable[SN_NtCreateFile]            = NtCreateFile;
    KiServiceTable[SN_NtSetValueKey]           = NtSetValueKey;
    KiServiceTable[SN_NtTerminateThread]       = NtTerminateThread;
    KiServiceTable[SN_NtDelayExecution]        = NtDelayExecution;
    KiServiceTable[SN_NtDisplayString]         = NtDisplayString;
    KiServiceTable[SN_NtDisplayNumber]         = NtDisplayNumber;
    KiServiceTable[SN_NtLoadLibrary]           = NtLoadLibrary;
}

UINT64 KiSystemServiceDispatch(UINT64 number, UINT64 *args)
{
    if (number >= NTOS_MAX_SYSCALL || KiServiceTable[number] == NULL) {
        KeLog("[ke]   invalid system service 0x%lx\n", (unsigned long)number);
        return (UINT64)STATUS_NOT_IMPLEMENTED;
    }
    return KiServiceTable[number](args);
}
