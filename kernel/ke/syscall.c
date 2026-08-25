/*
 * ke/syscall.c - the system-call boundary: MSR setup, the per-CPU block, the
 * service dispatcher, and the first handful of Nt* services.
 *
 * This is the interface native (ring 3) code will eventually reach through
 * ntdll. For now a tiny in-kernel user stub exercises it directly.
 */
#include <ntos/ke.h>
#include <ntos/trace.h>
#include <ntos/mm.h>
#include <ntos/io.h>
#include <ntos/ps.h>
#include <ntos/ldr.h>
#include <ntos/io.h>
#include <ntos/cm.h>
#include <ntos/gfx.h>
#include <ntos/win32k.h>
#include <ntos/rtl.h>
#include <nt/ntdef.h>
#include <nt/ntstatus.h>

#ifndef _WCHAR_DEFINED
#define WCHAR UINT16
#endif

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

/* The syscall entry stub reaches the current KTHREAD through KPCR+16 (GS
 * after swapgs), so the scheduler must publish it there on every switch. */
void KeSetCurrentThread(PKTHREAD thread)
{
    g_kpcr.CurrentThread = thread;
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

/* User RSP of the syscall currently being serviced (set by the dispatcher
 * before the service runs; used for diagnostics such as stack walks). */
static UINT64 g_service_user_rsp;

/* Walk the caller's user stack and annotate values that land inside loaded
 * module images (return addresses), giving a poor-man's exit backtrace. */
static void KiDumpUserStack(UINT64 rsp)
{
    KeLog("[user] user stack walk (rsp=%p, module-relative return sites):\n",
          (void *)rsp);
    for (UINT64 i = 0; i < 64; i++) {
        UINT64 va = rsp + i * 8;
        if (!MmProbeForRead(va, sizeof(UINT64)))
            break;
        UINT64 value = *(volatile UINT64 *)va;
        const char *name = 0;
        UINT64 base = 0;
        if (value && LdrDescribeUserAddress(value, &name, &base))
            KeLog("  [%02lu] %p  %s+0x%lx\n", (unsigned long)i,
                  (void *)value, name, (unsigned long)(value - base));
    }

    /* Frame-pointer chain: with the trap frame unavailable here, reconstruct
     * the first rbp by scanning for a slot whose value points further up the
     * same stack and whose own target again points upward; then walk [rbp] /
     * [rbp+8] pairs for the true caller chain. */
    UINT64 stack_max = rsp + 0x8000;
    UINT64 rbp = 0;
    for (UINT64 va = rsp + 8; va < rsp + 0x400; va += 8) {
        if (!MmProbeForRead(va, 8) || !MmProbeForRead(*(UINT64 *)va, 8))
            continue;
        UINT64 candidate = *(volatile UINT64 *)va;
        if (candidate <= va || candidate >= stack_max)
            continue;
        UINT64 next = *(volatile UINT64 *)candidate;
        if (next > candidate && next < stack_max) {
            rbp = candidate;
            break;
        }
    }
    if (rbp) {
        KeLog("[user] frame chain:\n");
        for (int level = 0; level < 12; level++) {
            if (!MmProbeForRead(rbp + 8, 8))
                break;
            UINT64 ret = *(volatile UINT64 *)(rbp + 8);
            const char *name = 0;
            UINT64 base = 0;
            if (ret && LdrDescribeUserAddress(ret, &name, &base))
                KeLog("  #%d %s+0x%lx\n", level, name,
                      (unsigned long)(ret - base));
            if (!MmProbeForRead(rbp, 8))
                break;
            UINT64 next = *(volatile UINT64 *)rbp;
            if (next <= rbp || next >= stack_max)
                break;
            rbp = next;
        }
    }
}

/* NtTraceCall - loader trampoline notification (see ntos/trace.h). Args keep
 * their positions (r10 holds the original RCX); the trampoline's tag sits at
 * NTOS_TRACE_TAG_VA and the caller's return address at [user_rsp]. */
static UINT64 NtTraceCall(UINT64 *a)
{
    static const char *const names[] = {
        NULL,
        "RegisterClassW",
        "RegisterClassExW",
        "CreateWindowExW",
        "DestroyWindow",
        "PostThreadMessageW",
        "PostMessageW",
        "DefWindowProcW",
        "DispatchMessageW",
        "RegisterClassW@entry",
    };
    UINT32 tag = 0;
    if (MmProbeForRead(NTOS_TRACE_TAG_VA, sizeof(UINT32)))
        tag = *(volatile UINT32 *)NTOS_TRACE_TAG_VA;
    const char *name = tag < sizeof(names) / sizeof(names[0]) && names[tag]
                           ? names[tag]
                           : "?";
    UINT64 ret = 0;
    if (MmProbeForRead(g_service_user_rsp, sizeof(UINT64)))
        ret = *(volatile UINT64 *)g_service_user_rsp;
    const char *mod = 0;
    UINT64 base = 0;
    if (ret && LdrDescribeUserAddress(ret, &mod, &base))
        KeLog("[trace] %s(a1=%p, a2=%p, a3=%p, a4=%p) from %s+0x%lx\n",
              name, (void *)a[0], (void *)a[1], (void *)a[2], (void *)a[3],
              mod, (unsigned long)(ret - base));
    else
        KeLog("[trace] %s(a1=%p, a2=%p, a3=%p, a4=%p) from %p\n", name,
              (void *)a[0], (void *)a[1], (void *)a[2], (void *)a[3],
              (void *)ret);
    /* For class registrations, dump the WNDCLASS/EX the caller built: the
     * class-name pointer (+0x40) is the value the whole client-side pipeline
     * hinges on. */
    if ((tag == 1 || tag == 2) && MmProbeForRead(a[0] + 0x40, 8)) {
        UINT64 cls = *(volatile UINT64 *)(a[0] + 0x40);
        UINT64 inst = 0, brush = 0;
        UINT32 style = 0;
        if (MmProbeForRead(a[0] + 0x18, 8))
            inst = *(volatile UINT64 *)(a[0] + 0x18);
        if (MmProbeForRead(a[0] + 0x30, 8))
            brush = *(volatile UINT64 *)(a[0] + 0x30);
        if (MmProbeForRead(a[0] + 0x4, 4))
            style = *(volatile UINT32 *)(a[0] + 0x4);
        KeLog("[trace]   wcx: style=%lx hInstance=%p hbrBackground=%p lpszClassName=%p\n",
              (unsigned long)style, (void *)inst, (void *)brush, (void *)cls);
        if (MmProbeForRead(a[0], 0x50)) {
            KeLog("[trace]   wcx raw:");
            for (int off = 0; off < 0x50; off += 8)
                KeLog(" +%x=%p", off,
                      (void *)*( (volatile UINT64 *)(a[0] + off)));
            KeLog("\n");
        }
        if (MmProbeForRead(cls, 16)) {
            WCHAR wname[24];
            for (int i = 0; i < 23; i++) {
                UINT16 c = *(volatile UINT16 *)(cls + i * 2);
                wname[i] = c;
                if (!c)
                    break;
            }
            wname[23] = 0;
            char ascii[24];
            for (int i = 0; i < 23; i++)
                ascii[i] = wname[i] && wname[i] < 128 ? (char)wname[i] : '?';
            ascii[wname[23] ? 23 : 0] = 0;
            KeLog("[trace]   class name: %s\n", ascii);
        }
    }
    return 0;
}

/* NtTerminateThread - end the calling thread; does not return. */
static UINT64 NtTerminateThread(UINT64 *a)
{
    KeLog("[user] NtTerminateThread(handle=%p, status=0x%08lx); ending user thread\n",
          (void *)a[0], (unsigned long)(UINT32)a[1]);
    KiDumpUserStack(g_service_user_rsp);
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

/*
 * NtQuerySystemInformation(SystemInformationClass, SystemInformation,
 *                          SystemInformationLength, ReturnLength).
 *
 * Explorer asks for SystemPolicyInformation (class 134) while constructing
 * the desktop.  This is an opaque CLIP/WarBird licensing channel, not a plain
 * list of policy values: both its request and response contain encrypted and
 * checksummed blobs.  Expose the native entry point and validate its public
 * descriptor, but report that the licensing provider is absent rather than
 * returning a fabricated blob that user mode would reject as corrupt.
 */
static UINT64 NtQuerySystemInformation(UINT64 *a)
{
    typedef struct _SYSTEM_BASIC_INFORMATION_LOCAL {
        UINT32 Reserved;
        UINT32 TimerResolution;
        UINT32 PageSize;
        UINT32 NumberOfPhysicalPages;
        UINT32 LowestPhysicalPageNumber;
        UINT32 HighestPhysicalPageNumber;
        UINT32 AllocationGranularity;
        UINT32 Padding;
        UINT64 MinimumUserModeAddress;
        UINT64 MaximumUserModeAddress;
        UINT64 ActiveProcessorsAffinityMask;
        UINT8  NumberOfProcessors;
        UINT8  Reserved2[7];
    } SYSTEM_BASIC_INFORMATION_LOCAL;

    const UINT32 SystemBasicInformation = 0;
    const UINT32 SystemPolicyInformation = 134;
    const UINT32 policy_info_size = 32;
    UINT32 info_class = (UINT32)a[0];
    UINT64 buffer = a[1];
    UINT32 length = (UINT32)a[2];
    UINT32 *return_length = (UINT32 *)a[3];

    if (info_class == SystemBasicInformation) {
        UINT32 required = sizeof(SYSTEM_BASIC_INFORMATION_LOCAL);
        if (return_length &&
            MmProbeForWrite((UINT64)return_length, sizeof(*return_length)))
            *return_length = required;
        if (length < required)
            return (UINT64)STATUS_INFO_LENGTH_MISMATCH;
        if (!MmProbeForWrite(buffer, required))
            return (UINT64)STATUS_ACCESS_VIOLATION;
        SYSTEM_BASIC_INFORMATION_LOCAL *info =
            (SYSTEM_BASIC_INFORMATION_LOCAL *)buffer;
        memset(info, 0, sizeof(*info));
        info->TimerResolution = 100000; /* 10 ms in 100 ns units */
        info->PageSize = PAGE_SIZE;
        info->NumberOfPhysicalPages = (UINT32)MmFreePageCount();
        info->HighestPhysicalPageNumber = info->NumberOfPhysicalPages - 1;
        info->AllocationGranularity = 0x10000;
        info->MinimumUserModeAddress = 0x10000;
        info->MaximumUserModeAddress = 0x00007FFFFFFEFFFFULL;
        info->ActiveProcessorsAffinityMask = 1;
        info->NumberOfProcessors = 1;
        return (UINT64)STATUS_SUCCESS;
    }

    if (info_class != SystemPolicyInformation)
        return (UINT64)STATUS_INVALID_INFO_CLASS;
    if (return_length && MmProbeForWrite((UINT64)return_length,
                                         sizeof(*return_length)))
        *return_length = policy_info_size;
    if (length < policy_info_size)
        return (UINT64)STATUS_INFO_LENGTH_MISMATCH;
    if (!MmProbeForWrite(buffer, policy_info_size))
        return (UINT64)STATUS_ACCESS_VIOLATION;

    UINT32 *descriptor_status = (UINT32 *)(buffer + 0x1c);
    if (!MmProbeForWrite((UINT64)descriptor_status, sizeof(*descriptor_status)))
        return (UINT64)STATUS_INVALID_PARAMETER;
    *descriptor_status = (UINT32)STATUS_NOT_SUPPORTED;
    KeLog("[user] NtQuerySystemInformation(SystemPolicyInformation): no licensing provider\n");
    return (UINT64)STATUS_NOT_SUPPORTED;
}

/*
 * NtProtectVirtualMemory(ProcessHandle, *BaseAddress, *RegionSize, NewProtect,
 *                        *OldProtect) - change page protection on a range.
 */
static UINT64 NtProtectVirtualMemory(UINT64 *a)
{
    PVOID  *base_ptr = (PVOID *)a[1];
    SIZE_T *size_ptr = (SIZE_T *)a[2];
    UINT32  protect  = (UINT32)a[3];
    UINT32 *old_ptr  = (UINT32 *)a[4];

    if (!MmProbeForRead((UINT64)base_ptr, sizeof(PVOID)) ||
        !MmProbeForRead((UINT64)size_ptr, sizeof(SIZE_T)))
        return (UINT64)STATUS_ACCESS_VIOLATION;

    UINT64 base = (UINT64)*base_ptr;
    UINT64 size = *size_ptr;
    /* Writable protections: PAGE_READWRITE|WRITECOPY|EXECUTE_READWRITE|
     * EXECUTE_WRITECOPY = 0x04|0x08|0x40|0x80. */
    BOOLEAN writable = (protect & 0xCC) != 0;
    MmProtectRange(base, size, writable);

    if (old_ptr && MmProbeForWrite((UINT64)old_ptr, sizeof(UINT32)))
        *old_ptr = 0x04; /* report the previous protection as PAGE_READWRITE */
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

/* ------------------------------------------------------------------ */
/* Minimal win32k/USER services used by the genuine win32u.dll.        */
/* ------------------------------------------------------------------ */

/* Offset of USER32's `gpsi` global (the SERVERINFO pointer) in the build in
 * win/: read out of GetSysColor, which is `mov gpsi,%rax; mov
 * 0x1360(%rax,%rdx,4),%eax`. Diagnostics only. */
#define USER32_GPSI_OFFSET 0xBC8D8

typedef struct _USER_MSG_LOCAL {
    UINT64 Hwnd;
    UINT32 Message;
    UINT32 Padding;
    UINT64 WParam;
    INT64  LParam;
    UINT32 Time;
    INT32  PtX;
    INT32  PtY;
    UINT32 Private;
} USER_MSG_LOCAL;

#define USER_MESSAGE_QUEUE_CAPACITY 64
#define WM_CHAR        0x0102
#define WM_MOUSEMOVE   0x0200
#define WM_LBUTTONDOWN 0x0201
#define WM_LBUTTONUP   0x0202
#define WM_RBUTTONDOWN 0x0204
#define WM_RBUTTONUP   0x0205

/*
 * Per-thread posted-message queues. Real USER owns a queue per GUI thread;
 * cross-thread hand-offs (explorer's desktop bootstrap posts to the creating
 * thread) only work if the posted message cannot be consumed by another
 * thread's GetMessage. Threads are few, so queues are keyed by thread id.
 */
#define USER_MESSAGE_QUEUE_CAPACITY 64
#define USER_MAX_THREAD_QUEUES 64

typedef struct _USER_MSG_QUEUE {
    USER_MSG_LOCAL Msgs[USER_MESSAGE_QUEUE_CAPACITY];
    UINT32 Head;
    UINT32 Tail;
    /* The thread's USER "input event": a notification event ring 3 waits on
     * through MsgWaitForMultipleObjects. Created lazily the first time the
     * thread asks for it, signaled while the queue is non-empty. */
    PKEVENT InputEvent;
    HANDLE  InputEventHandle;
} USER_MSG_QUEUE;

static USER_MSG_QUEUE g_user_thread_queues[USER_MAX_THREAD_QUEUES];

static USER_MSG_QUEUE *KiUserQueueForThread(UINT32 thread_id)
{
    if (!thread_id || thread_id > USER_MAX_THREAD_QUEUES)
        return NULL;
    return &g_user_thread_queues[thread_id - 1];
}

/* Keep a thread's input event in step with its queue. Called with the queue
 * already updated; safe from interrupt context (KeSetEvent masks interrupts
 * and only readies threads). A pending update region counts as work: the
 * WM_PAINT it produces is synthesized by the message loop, not queued, so the
 * event has to fire for it too or the loop sleeps through the repaint. */
static UINT16 KiUserNextUpdateWindow(UINT32 thread_id);

static void KiUserSyncInputEvent(USER_MSG_QUEUE *queue)
{
    if (!queue->InputEvent)
        return;
    UINT32 tid = (UINT32)(queue - g_user_thread_queues) + 1;
    if (queue->Head != queue->Tail || KiUserNextUpdateWindow(tid))
        KeSetEvent(queue->InputEvent);
    else
        KeResetEvent(queue->InputEvent);
}

/* Owner thread of each created window slot: input goes to the queue of the
 * thread that created the window, as in win32k. */
static UINT32 g_window_owner[128];

static UINT32 KiUserWindowOwner(UINT64 hwnd)
{
    UINT16 slot = (UINT16)(hwnd & 0xFFFF);
    if (!slot || slot >= 128)
        return 0;
    return g_window_owner[slot];
}

/*
 * Update regions and WM_PAINT.
 *
 * win32k never queues WM_PAINT: a window carries an update region, and the
 * owning thread's message loop synthesizes the message for as long as that
 * region is non-empty. The same model here, with the region simplified to
 * "the whole window": g_window_update[slot] counts the WM_PAINTs still owed,
 * BeginPaint/ValidateRect clear it, and the count bounds redelivery so a
 * client that ignores the message cannot spin its loop forever.
 */
#define USER_PAINT_SLOTS 128
#define USER_PAINT_REDELIVERY 4
#define WM_PAINT_ 0x000F

static UINT8 g_window_update[USER_PAINT_SLOTS];

/* First window owned by `tid` that still owes a WM_PAINT, in creation order
 * (parents before children, which is the order win32k paints in). */
static UINT16 KiUserNextUpdateWindow(UINT32 tid)
{
    if (!tid)
        return 0;
    for (UINT16 slot = 1; slot < USER_PAINT_SLOTS; slot++)
        if (g_window_update[slot] && g_window_owner[slot] == tid)
            return slot;
    return 0;
}

static BOOLEAN KiUserEnqueueMessage(UINT64 hwnd, UINT32 message,
                                    UINT64 wparam, INT64 lparam,
                                    INT32 x, INT32 y)
{
    /* A posted message is delivered to its window's owner thread; a thread
     * message (hwnd == 0) goes to the posting thread's own target queue via
     * KiUserEnqueueThreadMessage. */
    UINT32 tid = hwnd ? KiUserWindowOwner(hwnd) : 0;
    if (!tid) {
        PKTHREAD thread = KeGetCurrentThread();
        tid = thread ? thread->ThreadId : 0;
    }
    USER_MSG_QUEUE *queue = KiUserQueueForThread(tid);
    if (!queue)
        return FALSE;
    UINT64 flags = KiIrqSave();
    UINT32 next = (queue->Head + 1) % USER_MESSAGE_QUEUE_CAPACITY;
    if (next == queue->Tail) {
        KiIrqRestore(flags);
        return FALSE;
    }

    USER_MSG_LOCAL *msg = &queue->Msgs[queue->Head];
    memset(msg, 0, sizeof(*msg));
    msg->Hwnd = hwnd;
    msg->Message = message;
    msg->WParam = wparam;
    msg->LParam = lparam;
    msg->Time = (UINT32)(KeGetTickCount() * 10);
    msg->PtX = x;
    msg->PtY = y;
    queue->Head = next;
    KiUserSyncInputEvent(queue);
    KiIrqRestore(flags);
    return TRUE;
}

static BOOLEAN KiUserEnqueueThreadMessage(UINT32 target_tid, UINT32 message,
                                          UINT64 wparam, INT64 lparam)
{
    USER_MSG_QUEUE *queue = KiUserQueueForThread(target_tid);
    if (!queue)
        return FALSE;
    UINT64 flags = KiIrqSave();
    UINT32 next = (queue->Head + 1) % USER_MESSAGE_QUEUE_CAPACITY;
    if (next == queue->Tail) {
        KiIrqRestore(flags);
        return FALSE;
    }
    USER_MSG_LOCAL *msg = &queue->Msgs[queue->Head];
    memset(msg, 0, sizeof(*msg));
    msg->Message = message;
    msg->WParam = wparam;
    msg->LParam = lparam;
    msg->Time = (UINT32)(KeGetTickCount() * 10);
    queue->Head = next;
    KiUserSyncInputEvent(queue);
    KiIrqRestore(flags);
    return TRUE;
}

static UINT64 g_user_active_hwnd;
static UINT8 g_user_mouse_buttons;

void KiUserQueueCharacter(UINT16 character)
{
    if (!g_user_active_hwnd)
        return;
    KiUserEnqueueMessage(g_user_active_hwnd, WM_CHAR, character, 1, 0, 0);
}

void KiUserQueueMouse(INT32 x, INT32 y, UINT8 buttons)
{
    if (!g_user_active_hwnd) {
        g_user_mouse_buttons = buttons;
        return;
    }
    UINT64 wparam = 0;
    if (buttons & 1) wparam |= 0x0001; /* MK_LBUTTON */
    if (buttons & 2) wparam |= 0x0002; /* MK_RBUTTON */
    INT64 lparam = (INT64)(UINT32)(((UINT16)y << 16) | (UINT16)x);

    KiUserEnqueueMessage(g_user_active_hwnd, WM_MOUSEMOVE, wparam, lparam, x, y);
    if ((buttons ^ g_user_mouse_buttons) & 1)
        KiUserEnqueueMessage(g_user_active_hwnd,
                             (buttons & 1) ? WM_LBUTTONDOWN : WM_LBUTTONUP,
                             wparam, lparam, x, y);
    if ((buttons ^ g_user_mouse_buttons) & 2)
        KiUserEnqueueMessage(g_user_active_hwnd,
                             (buttons & 2) ? WM_RBUTTONDOWN : WM_RBUTTONUP,
                             wparam, lparam, x, y);
    g_user_mouse_buttons = buttons;
}

/* Mark a window as needing repaint and wake its owner thread's message loop. */
static void KiUserInvalidateWindow(UINT16 slot)
{
    if (!slot || slot >= USER_PAINT_SLOTS)
        return;
    UINT64 flags = KiIrqSave();
    g_window_update[slot] = USER_PAINT_REDELIVERY;
    USER_MSG_QUEUE *queue = KiUserQueueForThread(g_window_owner[slot]);
    if (queue)
        KiUserSyncInputEvent(queue);
    KiIrqRestore(flags);
}

/* Drop a window's update region: it has been painted (or explicitly
 * validated), so no further WM_PAINT is owed. */
static void KiUserValidateWindow(UINT16 slot)
{
    if (!slot || slot >= USER_PAINT_SLOTS)
        return;
    UINT64 flags = KiIrqSave();
    g_window_update[slot] = 0;
    USER_MSG_QUEUE *queue = KiUserQueueForThread(g_window_owner[slot]);
    if (queue)
        KiUserSyncInputEvent(queue);
    KiIrqRestore(flags);
}

static BOOLEAN KiUserTakeMessage(USER_MSG_LOCAL *out, BOOLEAN remove)
{
    PKTHREAD thread = KeGetCurrentThread();
    USER_MSG_QUEUE *queue =
        KiUserQueueForThread(thread ? thread->ThreadId : 0);
    if (!queue)
        return FALSE;
    UINT64 flags = KiIrqSave();
    if (queue->Tail == queue->Head) {
        /* Nothing posted. An invalid window becomes WM_PAINT right here, which
         * is where win32k generates it: paint messages live in the window's
         * update region, never in the queue, so they are always produced last
         * and always after everything that was actually posted. */
        UINT16 slot = KiUserNextUpdateWindow(thread ? thread->ThreadId : 0);
        if (!slot) {
            KiIrqRestore(flags);
            return FALSE;
        }
        memset(out, 0, sizeof(*out));
        out->Hwnd = 0x10000u | slot;
        out->Message = WM_PAINT_;
        out->Time = (UINT32)(KeGetTickCount() * 10);
        /* Windows keeps producing WM_PAINT until the window is validated.
         * Count every delivery, not just the removing ones, so a client that
         * neither paints nor validates cannot livelock its own message loop
         * on a window we hand it forever. */
        if (g_window_update[slot] && !--g_window_update[slot]) {
            KeLog("[user] WM_PAINT for HWND %p went unpainted; validating the "
                  "window to keep the message loop live\n", (void *)out->Hwnd);
            KiUserSyncInputEvent(queue);
        }
        KiIrqRestore(flags);
        return TRUE;
    }
    *out = queue->Msgs[queue->Tail];
    if (remove) {
        queue->Tail = (queue->Tail + 1) % USER_MESSAGE_QUEUE_CAPACITY;
        KiUserSyncInputEvent(queue);
    }
    KiIrqRestore(flags);
    return TRUE;
}

/* ------------------------------------------------------------------ */
/* The USER atom table                                                */
/* ------------------------------------------------------------------ */
/*
 * Window classes, registered window messages and GlobalAddAtom-style names all
 * draw from one 0xC000..0xFFFF atom space per window station, exactly as in
 * win32k: RegisterClassEx interns the class name, RegisterWindowMessage interns
 * the message name (so two components asking for the same string get the same
 * id), and GetAtomName maps an atom back to its string. Keeping them in
 * separate counters -- which is what this file did -- hands the same number to
 * a class and a message, and leaves GetAtomName unable to answer at all, which
 * is what stopped the shell from creating its worker window.
 */
#define USER_MAX_ATOMS   192
#define USER_ATOM_CHARS  64
#define USER_ATOM_FIRST  0xC000

typedef struct _USER_ATOM_ENTRY {
    WCHAR  Name[USER_ATOM_CHARS];
    UINT16 Atom;
} USER_ATOM_ENTRY;

static USER_ATOM_ENTRY g_user_atoms[USER_MAX_ATOMS];
static UINT32 g_user_atom_count;

/* Copy a NUL-terminated wide string from user memory (bounded). */
static UINT32 KiCopyWideFromUser(const WCHAR *src, WCHAR *dst, UINT32 cap)
{
    if (!src || !MmProbeForRead((UINT64)src, sizeof(WCHAR)))
        return 0;
    for (UINT32 i = 0; i + 1 < cap; i++) {
        if (!MmProbeForRead((UINT64)(src + i), sizeof(WCHAR))) {
            dst[i] = 0;
            return i;
        }
        dst[i] = src[i];
        if (!src[i])
            return i;
    }
    dst[cap - 1] = 0;
    return cap - 1;
}

static BOOLEAN KiWideEqual(const WCHAR *a, const WCHAR *b)
{
    while (*a && *b) {
        WCHAR ca = *a++, cb = *b++;
        if (ca >= 'A' && ca <= 'Z') ca += 32;
        if (cb >= 'A' && cb <= 'Z') cb += 32;
        if (ca != cb)
            return FALSE;
    }
    return *a == *b;
}

/* Intern `name`, returning its (stable) atom. 0 when the table is full. */
static UINT16 KiUserAddAtom(const WCHAR *name)
{
    if (!name || !name[0])
        return 0;
    for (UINT32 i = 0; i < g_user_atom_count; i++)
        if (KiWideEqual(g_user_atoms[i].Name, name))
            return g_user_atoms[i].Atom;
    if (g_user_atom_count >= USER_MAX_ATOMS)
        return 0;
    USER_ATOM_ENTRY *entry = &g_user_atoms[g_user_atom_count];
    UINT32 n = 0;
    for (; n + 1 < USER_ATOM_CHARS && name[n]; n++)
        entry->Name[n] = name[n];
    entry->Name[n] = 0;
    entry->Atom = (UINT16)(USER_ATOM_FIRST + g_user_atom_count);
    g_user_atom_count++;
    return entry->Atom;
}

static const WCHAR *KiUserAtomName(UINT16 atom)
{
    for (UINT32 i = 0; i < g_user_atom_count; i++)
        if (g_user_atoms[i].Atom == atom)
            return g_user_atoms[i].Name;
    return NULL;
}

/* Capture the atom name a win32u service was handed: modern win32u passes a
 * counted UNICODE_STRING, older forms a bare pointer. */
static UINT32 KiCaptureAtomName(UINT64 arg, WCHAR *out, UINT32 cap)
{
    if (!arg)
        return 0;
    /* A UNICODE_STRING starts with two USHORTs whose values are small and
     * whose Buffer is a valid pointer; a bare string starts with characters. */
    if (MmProbeForRead(arg, sizeof(UNICODE_STRING))) {
        const UNICODE_STRING *ustr = (const UNICODE_STRING *)arg;
        UINT32 chars = ustr->Length / sizeof(WCHAR);
        if (chars && chars < cap && ustr->MaximumLength >= ustr->Length &&
            ustr->Buffer &&
            MmProbeForRead((UINT64)ustr->Buffer,
                           (UINT64)chars * sizeof(WCHAR))) {
            memcpy(out, ustr->Buffer, (UINT64)chars * sizeof(WCHAR));
            out[chars] = 0;
            return chars;
        }
    }
    return KiCopyWideFromUser((const WCHAR *)arg, out, cap);
}

static UINT64 NtUserGetThreadState(UINT64 *a)
{
    /* No USER message queue/window state exists yet.  A zero state is the
     * correct empty-subsystem answer and, unlike STATUS_NOT_IMPLEMENTED, is a
     * valid value for every query currently made during process startup. */
    return 0;
}

static UINT64 NtUserRegisterWindowMessage(UINT64 *a)
{
    /* NtUserRegisterWindowMessage(PUNICODE_STRING MessageName): the id must be
     * the same for every caller that asks for the same string, which is what
     * makes cross-process shell messages (Shell_TrayWnd's "TaskbarCreated" and
     * friends) work at all. Interning gives that for free. */
    WCHAR name[USER_ATOM_CHARS];
    UINT32 n = KiCaptureAtomName(a[0], name, USER_ATOM_CHARS);
    if (!n)
        return 0;
    UINT16 atom = KiUserAddAtom(name);
    char ascii[USER_ATOM_CHARS];
    UINT32 j = 0;
    for (; j < n && j + 1 < sizeof(ascii); j++)
        ascii[j] = (char)name[j];
    ascii[j] = 0;
    KeLog("[user] RegisterWindowMessage('%s') -> 0x%x\n", ascii, atom);
    return atom;
}

static UINT64 NtUserSystemParametersInfo(UINT64 *a)
{
    KeLog("[user] NtUserSystemParametersInfo(action=0x%lx, param=%lu, out=%p, flags=0x%lx)\n",
          (unsigned long)a[0], (unsigned long)a[1], (void *)a[2],
          (unsigned long)a[3]);
    /* SPI_GETDRAGFULLWINDOWS (0x26) is Explorer's first query. Supply the
     * disabled policy value through pvParam. This is a genuine empty USER
     * policy state, not framebuffer desktop emulation. */
    if ((UINT32)a[0] == 0x26 &&
        MmProbeForWrite(a[2], sizeof(UINT32))) {
        *(UINT32 *)a[2] = 0;
        return 1;
    }

    /* SPI_GETCLIENTAREAANIMATION (0x1042). Explorer's SHCore worker asks for
     * this after its startup hand-off. We currently have no compositor
     * animations, so publish the disabled BOOL as a successful query. */
    if ((UINT32)a[0] == 0x1042 &&
        MmProbeForWrite(a[2], sizeof(UINT32))) {
        *(UINT32 *)a[2] = 0;
        return 1;
    }

    /* SPI_GETHIGHCONTRAST (0x42), HIGHCONTRASTW is 16 bytes on x64.
     * Accessibility high-contrast mode is disabled and no scheme is active. */
    if ((UINT32)a[0] == 0x42 && a[1] >= 16 &&
        MmProbeForWrite(a[2], 16)) {
        UINT32 *hc = (UINT32 *)a[2];
        hc[0] = 16; /* cbSize */
        hc[1] = 0;  /* dwFlags */
        *(UINT64 *)((UINT8 *)hc + 8) = 0; /* lpszDefaultScheme */
        return 1;
    }

    /* Report other settings as unavailable until the USER system metrics and
     * settings block is backed by win32k. This is a Win32 BOOL, not an
     * NTSTATUS. */
    return 0;
}

static UINT64 NtUserFindWindowEx(UINT64 *a)
{
    /* Empty window tree: no matching HWND. */
    return 0;
}

static UINT64 NtUserEnableMouseInPointer(UINT64 *a)
{
    /* Input is already delivered by the kernel PS/2 path. Accepting the mode
     * switch lets USER32 complete initialization without inventing windows. */
    return 1;
}

static UINT64 NtUserSetProcessUIAccessZorder(UINT64 *a)
{
    /* The single interactive process owns the only desktop and therefore has
     * no cross-integrity z-order boundary to enforce. */
    return 1;
}

static UINT64 NtUserPeekMessage(UINT64 *a)
{
    USER_MSG_LOCAL *user_msg = (USER_MSG_LOCAL *)a[0];
    USER_MSG_LOCAL msg;
    BOOLEAN remove = ((UINT32)a[4] & 1) != 0; /* PM_REMOVE */
    if (!user_msg || !MmProbeForWrite((UINT64)user_msg, sizeof(*user_msg)))
        return 0;
    if (!KiUserTakeMessage(&msg, remove))
        return 0;
    *user_msg = msg;
    return 1;
}

static UINT64 NtUserPostThreadMessage(UINT64 *a)
{
    UINT32 thread_id = (UINT32)a[0];
    UINT32 message = (UINT32)a[1];
    const char *mod = 0;
    UINT64 base = 0, ret = 0;
    if (MmProbeForRead(g_service_user_rsp, sizeof(UINT64)))
        ret = *(volatile UINT64 *)g_service_user_rsp;
    if (ret && LdrDescribeUserAddress(ret, &mod, &base))
        KeLog("[user] NtUserPostThreadMessage(tid=%u, msg=0x%x, wp=%p, lp=%p) from %s+0x%lx\n",
              thread_id, message, (void *)a[2], (void *)a[3], mod,
              (unsigned long)(ret - base));
    else
        KeLog("[user] NtUserPostThreadMessage(tid=%u, msg=0x%x, wp=%p, lp=%p)\n",
              thread_id, message, (void *)a[2], (void *)a[3]);

    /* The message is delivered to the target thread's own queue; only that
     * thread's GetMessage/PeekMessage can consume it, preserving per-thread
     * message semantics for explorer's desktop hand-off. */
    return KiUserEnqueueThreadMessage(thread_id, message, a[2],
                                      (INT64)a[3]);
}

/* NtUserCallOneParam routine indices used by the shell (the win32u
 * apfnSimpleCall table of the supplied build). */
#define ONEPARAM_GETINPUTEVENT 0x32

/*
 * The thread's USER input event. user32's MsgWaitForMultipleObjectsEx checks
 * the client-side wake bits first and, when nothing is pending, asks win32k
 * for the handle of the event that fires when a message matching the wake mask
 * arrives, then waits on it together with the caller's handles. Returning NULL
 * (the old stub) makes that function fail, and the shell retries forever.
 *
 * The wake mask is packed into the argument as (flags << 16) | mask; the
 * per-thread queue is not filtered by message class yet, so any queued message
 * wakes the wait -- a superset, which a message loop handles correctly because
 * it re-checks its own queue after waking.
 */
static UINT64 KiUserGetInputEvent(void)
{
    PKTHREAD thread = KeGetCurrentThread();
    USER_MSG_QUEUE *queue =
        KiUserQueueForThread(thread ? thread->ThreadId : 0);
    if (!queue)
        return 0;
    if (!queue->InputEvent) {
        PKEVENT event = NULL;
        HANDLE handle = NULL;
        if (!NT_SUCCESS(PsCreateNotificationEvent(&event, &handle)))
            return 0;
        queue->InputEvent = event;
        queue->InputEventHandle = handle;
        KeLog("[user] input event for thread %lu -> handle %p\n",
              (unsigned long)(thread ? thread->ThreadId : 0), (void *)handle);
    }
    /* Publish the current queue state before the caller waits, so a message
     * that arrived before this call is not missed. */
    UINT64 flags = KiIrqSave();
    KiUserSyncInputEvent(queue);
    KiIrqRestore(flags);
    return (UINT64)queue->InputEventHandle;
}

/* NtUserCallNoParam(routine): the routine index is the only argument, so it
 * cannot share NtUserCallOneParam's handler (which reads it from a[1]). */
static UINT64 NtUserCallNoParam(UINT64 *a)
{
    static UINT8 seen[256];
    UINT64 routine = a[0];
    if (routine >= 256 || !seen[routine]) {
        if (routine < 256)
            seen[routine] = 1;
        KeLog("[user] NtUserCallNoParam(routine=0x%lx) -> 0\n",
              (unsigned long)routine);
    }
    return 0;
}

static UINT64 NtUserCallOneParam(UINT64 *a)
{
    UINT64 routine = a[1];

    if (routine == ONEPARAM_GETINPUTEVENT)
        return KiUserGetInputEvent();

    /* Log each unimplemented routine once: repeated calls come from polling
     * loops and would bury the rest of the log. */
    static UINT8 seen[256];
    if (routine >= 256 || !seen[routine]) {
        if (routine < 256)
            seen[routine] = 1;
        KeLog("[user] NtUserCallOneParam(value=%p, routine=0x%lx) -> 0\n",
              (void *)a[0], (unsigned long)routine);
    }
    return 0;
}

static UINT64 NtUserGetCaretBlinkTime(UINT64 *a)
{
    /* Conventional Windows default in milliseconds. This is a scalar query;
     * no caret object needs to exist yet. */
    return 530;
}

static UINT64 NtUserGetAtomName(UINT64 *a)
{
    /* (atom, PUNICODE_STRING out) in the modern form; the buffer/capacity pair
     * is the older one. The shell resolves its own class atom back to a name
     * before creating the worker window, so answering with an empty string
     * (what this used to do) silently loses that window. */
    UINT16 atom = (UINT16)a[0];
    const WCHAR *name = KiUserAtomName(atom);
    if (!name) {
        KeLog("[user] GetAtomName(0x%x) -> unknown\n", atom);
        return 0;
    }
    UINT32 chars = 0;
    while (name[chars])
        chars++;

    WCHAR *buffer = (WCHAR *)a[1];
    UINT32 capacity = (UINT32)a[2];
    /* UNICODE_STRING form: a[1] points at { Length, MaximumLength, Buffer }
     * and the count comes from MaximumLength. */
    if (buffer && !capacity && MmProbeForRead(a[1], sizeof(UNICODE_STRING))) {
        UNICODE_STRING *ustr = (UNICODE_STRING *)a[1];
        if (ustr->Buffer && ustr->MaximumLength >= sizeof(WCHAR) &&
            MmProbeForWrite(a[1], sizeof(UNICODE_STRING))) {
            UINT32 room = ustr->MaximumLength / sizeof(WCHAR);
            UINT32 n = chars + 1 > room ? room - 1 : chars;
            if (!MmProbeForWrite((UINT64)ustr->Buffer,
                                 (UINT64)(n + 1) * sizeof(WCHAR)))
                return 0;
            for (UINT32 i = 0; i < n; i++)
                ustr->Buffer[i] = name[i];
            ustr->Buffer[n] = 0;
            ustr->Length = (UINT16)(n * sizeof(WCHAR));
            return n;
        }
    }

    if (!buffer || !capacity)
        return 0;
    UINT32 n = chars + 1 > capacity ? capacity - 1 : chars;
    if (!MmProbeForWrite((UINT64)buffer, (UINT64)(n + 1) * sizeof(WCHAR)))
        return 0;
    for (UINT32 i = 0; i < n; i++)
        buffer[i] = name[i];
    buffer[n] = 0;
    return n;
}

/* ------------------------------------------------------------------ */
/* Window classes and the USER atom table                              */
/* ------------------------------------------------------------------ */

#define USER_MAX_CLASSES 64

typedef struct _USER_CLASS_LOCAL {
    WCHAR Name[32];
    UINT64 WndProc;
    UINT64 Instance;
    UINT32 Style;
    UINT32 WndExtra;
    UINT16 Atom;
    BOOLEAN Used;
} USER_CLASS_LOCAL;

static USER_CLASS_LOCAL g_user_classes[USER_MAX_CLASSES];

static USER_CLASS_LOCAL *KiClassByAtom(UINT16 atom)
{
    for (int i = 0; i < USER_MAX_CLASSES; i++)
        if (g_user_classes[i].Used && g_user_classes[i].Atom == atom)
            return &g_user_classes[i];
    return NULL;
}

/* WNDCLASSEXW x64 offsets. */
#define WNDCLASSEX_CBSIZE      0x00
#define WNDCLASSEX_STYLE       0x04
#define WNDCLASSEX_WNDPROC     0x08
#define WNDCLASSEX_HINSTANCE   0x18
#define WNDCLASSEX_CLASSNAME   0x40

static UINT64 NtUserRegisterClassExWOW(UINT64 *a)
{
    /* Win10 signature: (WNDCLASSEXW *lpwcx, PUNICODE_STRING ClassName,
     * PCLSMENUNAME MenuName, DWORD FnId, DWORD Flags, WORD Wow). The client
     * name arrives as a captured UNICODE_STRING in a[1]; the WNDCLASSEX copy
     * in a[0] is the fallback source. */
    const UINT8 *wcex = (const UINT8 *)a[0];
    const UNICODE_STRING *ustr = (const UNICODE_STRING *)a[1];
    WCHAR local[32];
    UINT32 n = 0;

    KeLog("[user] RegisterClassExWOW(wcex=%p, name_ustr=%p, fnid=%lu, "
          "flags=%lx)\n", (void *)wcex, (void *)ustr,
          (unsigned long)(UINT32)a[3], (unsigned long)(UINT32)a[4]);

    if (ustr && MmProbeForRead((UINT64)ustr, sizeof(*ustr))) {
        UINT32 chars = ustr->Length / 2;
        if (chars > 31)
            chars = 31;
        if (chars && ustr->Buffer &&
            MmProbeForRead((UINT64)ustr->Buffer,
                           (UINT64)chars * sizeof(WCHAR))) {
            memcpy(local, ustr->Buffer, (UINT64)chars * sizeof(WCHAR));
            local[chars] = 0;
            n = chars;
        }
    }
    if (!n && wcex && MmProbeForRead((UINT64)wcex, 80))
        n = KiCopyWideFromUser(*(const WCHAR **)(wcex + WNDCLASSEX_CLASSNAME),
                               local, 32);
    if (!n) {
        KeLog("[user] RegisterClassExWOW: no usable class name\n");
        return 0;
    }

    /* Re-registration returns the existing class atom. */
    for (int i = 0; i < USER_MAX_CLASSES; i++)
        if (g_user_classes[i].Used &&
            KiWideEqual(g_user_classes[i].Name, local))
            return g_user_classes[i].Atom;

    for (int i = 0; i < USER_MAX_CLASSES; i++) {
        if (g_user_classes[i].Used)
            continue;
        memcpy(g_user_classes[i].Name, local, (n + 1) * sizeof(WCHAR));
        g_user_classes[i].Style = *(const UINT32 *)(wcex + WNDCLASSEX_STYLE);
        if (MmProbeForRead((UINT64)(wcex + 0x14), sizeof(UINT32)))
            g_user_classes[i].WndExtra =
                *(const UINT32 *)(wcex + 0x14);
        g_user_classes[i].WndProc =
            *(const UINT64 *)(wcex + WNDCLASSEX_WNDPROC);
        g_user_classes[i].Instance =
            *(const UINT64 *)(wcex + WNDCLASSEX_HINSTANCE);
        /* The class name is interned in the station atom table, so
         * GetAtomName/FindAtom answer for it and no registered window message
         * can be handed the same number. */
        g_user_classes[i].Atom = KiUserAddAtom(local);
        g_user_classes[i].Used = TRUE;
        char ascii[33];
        UINT32 j = 0;
        for (; j < n; j++)
            ascii[j] = (char)local[j];
        ascii[j] = 0;
        KeLog("[user] RegisterClass('%s') -> atom 0x%x\n", ascii,
              g_user_classes[i].Atom);
        return g_user_classes[i].Atom;
    }
    return 0;
}

static UINT64 NtUserUnregisterClass(UINT64 *a)
{
    const WCHAR *name = *(const WCHAR **)a[1];
    WCHAR local[32];
    UINT32 n = KiCopyWideFromUser(name, local, 32);
    (void)n;
    for (int i = 0; i < USER_MAX_CLASSES; i++)
        if (g_user_classes[i].Used &&
            KiWideEqual(g_user_classes[i].Name, local)) {
            memset(&g_user_classes[i], 0, sizeof(g_user_classes[i]));
            return 1;
        }
    return 0;
}

static UINT64 NtUserGetClassInfoEx(UINT64 *a)
{
    /* (HInstance, PUNICODE_STRING name, WNDCLASSEXW *out, ..., BOOL ansi) */
    const UNICODE_STRING *ustr = (const UNICODE_STRING *)a[1];
    UINT8 *out = (UINT8 *)a[2];
    if (!ustr || !MmProbeForRead((UINT64)ustr, sizeof(*ustr)) ||
        !out || !MmProbeForWrite((UINT64)out, 80))
        return 0;

    WCHAR stackname[32];
    UINT32 chars = ustr->Length / 2;
    if (chars > 31)
        chars = 31;
    for (UINT32 i = 0; i < chars; i++) {
        if (!MmProbeForRead((UINT64)(ustr->Buffer + i), sizeof(WCHAR)))
            return 0;
        stackname[i] = ustr->Buffer[i];
    }
    stackname[chars] = 0;

    for (int i = 0; i < USER_MAX_CLASSES; i++) {
        if (!g_user_classes[i].Used ||
            !KiWideEqual(g_user_classes[i].Name, stackname))
            continue;
        memset(out, 0, 80);
        *(UINT32 *)(out + WNDCLASSEX_STYLE) = g_user_classes[i].Style;
        *(UINT64 *)(out + WNDCLASSEX_WNDPROC) = g_user_classes[i].WndProc;
        *(UINT64 *)(out + WNDCLASSEX_HINSTANCE) = g_user_classes[i].Instance;
        *(const WCHAR **)(out + WNDCLASSEX_CLASSNAME) = ustr->Buffer;
        return g_user_classes[i].Atom;
    }
    return 0;
}

/* Window properties: a small fixed table keyed by (hwnd, name pointer). */
#define USER_MAX_PROPS 128
typedef struct _USER_PROP_LOCAL {
    UINT64 Hwnd;
    UINT64 Name;   /* MAKEINTATOM or string pointer as passed */
    UINT64 Value;
    BOOLEAN Used;
} USER_PROP_LOCAL;

static USER_PROP_LOCAL g_user_props[USER_MAX_PROPS];

static UINT64 NtUserSetProp(UINT64 *a)
{
    UINT64 hwnd = a[0];
    UINT64 name = a[1];
    UINT64 value = a[2];
    for (int i = 0; i < USER_MAX_PROPS; i++) {
        if (g_user_props[i].Used && g_user_props[i].Hwnd == hwnd &&
            g_user_props[i].Name == name) {
            g_user_props[i].Value = value;
            return 1;
        }
    }
    for (int i = 0; i < USER_MAX_PROPS; i++) {
        if (g_user_props[i].Used)
            continue;
        g_user_props[i].Used = TRUE;
        g_user_props[i].Hwnd = hwnd;
        g_user_props[i].Name = name;
        g_user_props[i].Value = value;
        return 1;
    }
    return 0;
}

static UINT64 NtUserGetProp(UINT64 *a)
{
    UINT64 hwnd = a[0];
    UINT64 name = a[1];
    for (int i = 0; i < USER_MAX_PROPS; i++)
        if (g_user_props[i].Used && g_user_props[i].Hwnd == hwnd &&
            g_user_props[i].Name == name)
            return g_user_props[i].Value;
    return 0;
}

static UINT64 NtUserRemoveProp(UINT64 *a)
{
    UINT64 hwnd = a[0];
    UINT64 name = a[1];
    for (int i = 0; i < USER_MAX_PROPS; i++)
        if (g_user_props[i].Used && g_user_props[i].Hwnd == hwnd &&
            g_user_props[i].Name == name) {
            memset(&g_user_props[i], 0, sizeof(g_user_props[i]));
            return 1;
        }
    return 0;
}

/* Per-window longs live behind the window head inside its arena slot. */
#define WINDOW_LONG_BASE 0x60
#define WINDOW_LONG_COUNT 16

/* Offsets inside the client-visible window head that the genuine user32
 * dereferences directly (decoded from its GWL getter and IsWindowVisible):
 * style/+0x30, exStyle/+0x34, fnid-state/+0x42, cbWndExtra/+0xE8,
 * hWndParent/+0x100, WndProc/+0x148, and the window bytes at +0x168. */
#define WND_STYLE_OFF     0x30
#define WND_EXSTYLE_OFF   0x34
#define WND_FNID_OFF      0x42
#define WND_WND_EXTRA_OFF 0xE8
#define WND_PARENT_OFF    0x100
#define WND_WNDPROC_OFF   0x148
#define WND_WINBYTES_OFF  0x168
#define WS_VISIBLE_       0x10000000u

/*
 * NtUserCreateWindowEx receives the class name as a LARGE_STRING (not a
 * UNICODE_STRING): { ULONG Length; ULONG MaximumLength:31, bAnsi:1;
 * PVOID Buffer; }. When the caller passed an atom rather than a string,
 * user32 sets Length = 0 and stuffs the atom into Buffer, which is exactly
 * what win32k tests. Resolve either form against the class registry.
 */
static USER_CLASS_LOCAL *KiClassForCreate(UINT64 class_arg)
{
    if (!class_arg)
        return NULL;

    /* An atom may also arrive directly in the argument slot. */
    UINT16 atom = 0;
    if ((class_arg & ~0xFFFFULL) == 0) {
        atom = (UINT16)class_arg;
    } else {
        if (!MmProbeForRead(class_arg, 16))
            return NULL;
        UINT32 length = *(const UINT32 *)class_arg;
        UINT64 buffer = *(const UINT64 *)(class_arg + 8);
        UINT32 ansi = (*(const UINT32 *)(class_arg + 4)) >> 31;

        if (buffer && (buffer & ~0xFFFFULL) == 0) {
            atom = (UINT16)buffer; /* atom form: Buffer holds the atom */
        } else if (length && buffer) {
            /* String form: match the registered class by name. */
            WCHAR local[32];
            UINT32 chars = 0;
            if (ansi) {
                UINT32 n = length > 31 ? 31 : length;
                if (!MmProbeForRead(buffer, n))
                    return NULL;
                for (UINT32 i = 0; i < n; i++)
                    local[i] = (WCHAR)(UINT8)((const char *)buffer)[i];
                chars = n;
            } else {
                UINT32 n = length / 2;
                if (n > 31)
                    n = 31;
                if (!MmProbeForRead(buffer, (UINT64)n * sizeof(WCHAR)))
                    return NULL;
                memcpy(local, (const void *)buffer, (UINT64)n * sizeof(WCHAR));
                chars = n;
            }
            local[chars] = 0;
            for (int i = 0; i < USER_MAX_CLASSES; i++)
                if (g_user_classes[i].Used &&
                    KiWideEqual(g_user_classes[i].Name, local))
                    return &g_user_classes[i];
            return NULL;
        }
    }

    if (!atom)
        return NULL;
    for (int i = 0; i < USER_MAX_CLASSES; i++)
        if (g_user_classes[i].Used && g_user_classes[i].Atom == atom)
            return &g_user_classes[i];
    return NULL;
}

/* Publish the fields client-side user32 reads without a syscall. */
static void KiPublishWindowState(UINT64 window, UINT32 style, UINT32 exstyle,
                                 UINT64 parent, USER_CLASS_LOCAL *cls)
{
    *(UINT32 *)(window + WND_STYLE_OFF) = style;
    *(UINT32 *)(window + WND_EXSTYLE_OFF) = exstyle;
    *(UINT16 *)(window + WND_FNID_OFF) = 0;
    if (cls) {
        *(UINT32 *)(window + WND_WND_EXTRA_OFF) = cls->WndExtra;
        *(UINT64 *)(window + WND_WNDPROC_OFF) = cls->WndProc;
    } else {
        *(UINT32 *)(window + WND_WND_EXTRA_OFF) = 0x40;
    }
    *(UINT64 *)(window + WND_PARENT_OFF) = parent;
}

static INT64 KiGetWindowLong(UINT64 hwnd, INT32 index)
{
    UINT16 slot = (UINT16)(hwnd & 0xFFFF);
    if (!slot || slot >= 128 || index < -WINDOW_LONG_COUNT)
        return 0;
    INT64 *longs = (INT64 *)(PROCESS_USER_OBJECT_ARENA_VA +
                             (UINT64)slot * 0x200 + WINDOW_LONG_BASE +
                             (UINT64)(-index - 1) * 8);
    if (!MmProbeForRead((UINT64)longs, 8))
        return 0;
    return *longs;
}

static BOOLEAN KiSetWindowLong(UINT64 hwnd, INT32 index, INT64 value,
                               INT64 *old)
{
    UINT16 slot = (UINT16)(hwnd & 0xFFFF);
    if (!slot || slot >= 128 || index < -WINDOW_LONG_COUNT)
        return FALSE;
    INT64 *longs = (INT64 *)(PROCESS_USER_OBJECT_ARENA_VA +
                             (UINT64)slot * 0x200 + WINDOW_LONG_BASE +
                             (UINT64)(-index - 1) * 8);
    if (!MmProbeForWrite((UINT64)longs, 8))
        return FALSE;
    *old = *longs;
    *longs = value;
    return TRUE;
}

static UINT64 NtUserSetWindowLong(UINT64 *a)
{
    INT64 old = 0;
    if (KiSetWindowLong(a[0], (INT32)a[1], (INT64)a[2], &old))
        return (UINT64)old;
    return 0;
}
/* ------------------------------------------------------------------ */
/* win32k-lite: paint the shell windows on the framebuffer            */
/* ------------------------------------------------------------------ */
/* There is no compositor yet; the kernel classifies the shell's own
 * windows by class name and paints simple rectangles for them. Enough to
 * make "the desktop is up" visible: WorkerW fills the screen (wallpaper),
 * Shell_TrayWnd becomes the taskbar strip, everything else is a plain
 * surface. Painting happens on create/show/hide/move so ring-3 state
 * changes become visible without any client-side drawing. */

#define WINPAINT_NONE     0
#define WINPAINT_DESKTOP  1
#define WINPAINT_TASKBAR  2
#define WINPAINT_GENERIC  3
#define WINPAINT_SLOTS    128

static UINT8  g_win_kind[WINPAINT_SLOTS];
static INT32  g_win_x[WINPAINT_SLOTS], g_win_y[WINPAINT_SLOTS];
static UINT32 g_win_w[WINPAINT_SLOTS], g_win_h[WINPAINT_SLOTS];

static BOOLEAN KiClassNameIs(const WCHAR *name, const char *ascii)
{
    UINT32 i = 0;
    for (; ascii[i]; i++)
        if (name[i] != (WCHAR)(UINT8)ascii[i])
            return FALSE;
    return name[i] == 0;
}

static void KiPaintWindowBySlot(UINT16 slot)
{
    UINT32 style = *(volatile UINT32 *)(PROCESS_USER_OBJECT_ARENA_VA +
                                        (UINT64)slot * 0x200 + WND_STYLE_OFF);
    if (!(style & WS_VISIBLE_))
        return;

    UINT32 sw = GfxFramebuffer.Width, sh = GfxFramebuffer.Height;

    if (g_win_kind[slot] == WINPAINT_DESKTOP) {
        GfxFillRect(0, 0, sw, sh, GfxColor(0x0e, 0x2a, 0x47));
        return;
    }
    if (g_win_kind[slot] == WINPAINT_TASKBAR) {
        UINT32 h = g_win_h[slot] ? g_win_h[slot] : 40;
        if (h > sh / 3)
            h = 40;
        UINT32 y = sh - h;
        GfxFillRect(0, y, sw, h, GfxColor(0x1f, 0x1f, 0x1f));
        GfxFillRect(0, y, sw, 2, GfxColor(0x00, 0x78, 0xd7));
        GfxFillRect(0, y + 2, 48, h - 2, GfxColor(0x2d, 0x2d, 0x2d));
        GfxDrawString(14, y + (h - GFX_FONT_H) / 2 + 1, "NTOS",
                      GfxColor(0xff, 0xff, 0xff), GfxColor(0x2d, 0x2d, 0x2d));
        return;
    }

    /* Generic top-level window: a plain surface with a border. */
    {
    INT32 x = g_win_x[slot], y = g_win_y[slot];
    UINT32 w = g_win_w[slot], h = g_win_h[slot];
    if ((UINT32)x >= sw || (UINT32)y >= sh || !w || !h)
        return;
    if (x + (INT32)w > (INT32)sw)
        w = sw - (UINT32)x;
    if (y + (INT32)h > (INT32)sh)
        h = sh - (UINT32)y;
    GfxFillRect((UINT32)x, (UINT32)y, w, h, GfxColor(0xf0, 0xf0, 0xf0));
    GfxFillRect((UINT32)x, (UINT32)y, w, 1, GfxColor(0x10, 0x10, 0x10));
    GfxFillRect((UINT32)x, (UINT32)y, 1, h, GfxColor(0x10, 0x10, 0x10));
    }
}

static void KiRepaintAll(void);
static UINT64 NtUserShowWindow(UINT64 *a)
{
    UINT16 slot = (UINT16)(a[0] & 0xFFFF);
    UINT32 cmd = (UINT32)a[1] & 0xF;
    if (slot && slot < 256) {
        UINT64 window = PROCESS_USER_OBJECT_ARENA_VA + (UINT64)slot * 0x200;
        if (MmProbeForWrite(window + WND_STYLE_OFF, sizeof(UINT32))) {
            UINT32 *style = (UINT32 *)(window + WND_STYLE_OFF);
            if (cmd == 0) /* SW_HIDE */
                *style &= ~WS_VISIBLE_;
            else
                *style |= WS_VISIBLE_;
        }
    }
    /* Becoming visible makes the whole window invalid, which is what turns
     * into the window's first WM_PAINT. */
    if (cmd == 0)
        KiUserValidateWindow(slot);
    else
        KiUserInvalidateWindow(slot);
    KeLog("[user] ShowWindow(HWND %p, cmd %lu)\n", (void *)a[0],
          (unsigned long)cmd);
    KiRepaintAll();
    return 1;
}

static UINT64 NtUserSetWindowPos(UINT64 *a)
{
    /* Genuine win32k argument order: (hwnd, hWndInsertAfter, x, y,
     * cx, cy, flags). SWP_NOMOVE=0x2, SWP_NOSIZE=0x1, SWP_SHOWWINDOW=0x40,
     * SWP_HIDEWINDOW=0x80. */
    UINT32 flags = (UINT32)a[6];
    UINT16 slot = (UINT16)(a[0] & 0xFFFF);
    if (slot && slot < WINPAINT_SLOTS && g_win_kind[slot]) {
        if (!(flags & 0x2)) {
            g_win_x[slot] = (INT32)a[2];
            g_win_y[slot] = (INT32)a[3];
        }
        if (!(flags & 0x1)) {
            g_win_w[slot] = (UINT32)a[4];
            g_win_h[slot] = (UINT32)a[5];
        }
    }
    if (flags & 0xC0) {
        if (slot && slot < 256) {
            UINT64 window = PROCESS_USER_OBJECT_ARENA_VA +
                            (UINT64)slot * 0x200;
            if (MmProbeForWrite(window + WND_STYLE_OFF, sizeof(UINT32))) {
                UINT32 *style = (UINT32 *)(window + WND_STYLE_OFF);
                if (flags & 0x40)
                    *style |= WS_VISIBLE_;
                if (flags & 0x80)
                    *style &= ~WS_VISIBLE_;
            }
        }
        if (flags & 0x80)
            KiUserValidateWindow(slot);
    }
    /* Anything but a pure no-op move invalidates: a resized or newly shown
     * window has to repaint. SWP_NOREDRAW (0x8) is the caller opting out. */
    if (!(flags & 0x8) && (flags & 0x40 || !(flags & 0x3)))
        KiUserInvalidateWindow(slot);
    KeLog("[user] SetWindowPos(HWND %p, %ld,%ld %lux%lu, flags 0x%lx)\n",
          (void *)a[0], (long)(INT32)a[2], (long)(INT32)a[3],
          (unsigned long)(UINT32)a[4], (unsigned long)(UINT32)a[5],
          (unsigned long)flags);
    KiRepaintAll();
    return 1;
}

/* Full-screen repaint in creation order, with the desktop underneath. */
static void KiRepaintAll(void)
{
    if (!GfxAvailable())
        return;
    for (UINT16 slot = 1; slot < WINPAINT_SLOTS; slot++) {
        if (g_win_kind[slot] == WINPAINT_DESKTOP &&
            (*(volatile UINT32 *)(PROCESS_USER_OBJECT_ARENA_VA +
                                  (UINT64)slot * 0x200 + WND_STYLE_OFF)) &
                WS_VISIBLE_)
            KiPaintWindowBySlot(slot);
    }
    for (UINT16 slot = 1; slot < WINPAINT_SLOTS; slot++)
        KiPaintWindowBySlot(slot);
}


static UINT64 NtUserMoveWindow(UINT64 *a) { return 1; }

/* (hwnd, const RECT *rect, BOOL erase). The update region is whole-window, so
 * a partial rectangle just marks the window dirty. */
static UINT64 NtUserInvalidateRect(UINT64 *a)
{
    KiUserInvalidateWindow((UINT16)(a[0] & 0xFFFF));
    return 1;
}

/* (hwnd, const RECT *rect) */
static UINT64 NtUserValidateRect(UINT64 *a)
{
    KiUserValidateWindow((UINT16)(a[0] & 0xFFFF));
    return 1;
}

/* (hwnd, const RECT *rect, HRGN rgn, UINT flags): RDW_INVALIDATE=0x1,
 * RDW_VALIDATE=0x8. */
static UINT64 NtUserRedrawWindow(UINT64 *a)
{
    UINT32 flags = (UINT32)a[3];
    if (flags & 0x8)
        KiUserValidateWindow((UINT16)(a[0] & 0xFFFF));
    else if (flags & 0x1)
        KiUserInvalidateWindow((UINT16)(a[0] & 0xFFFF));
    return 1;
}

/* (hwnd, RECT *out, BOOL erase): the update rectangle in client coordinates,
 * empty when the window is clean. */
static UINT64 NtUserGetUpdateRect(UINT64 *a)
{
    UINT16 slot = (UINT16)(a[0] & 0xFFFF);
    BOOLEAN dirty = slot && slot < USER_PAINT_SLOTS && g_window_update[slot];
    INT32 *rect = (INT32 *)a[1];
    if (rect && MmProbeForWrite((UINT64)rect, 16)) {
        rect[0] = 0;
        rect[1] = 0;
        rect[2] = dirty && slot < WINPAINT_SLOTS ? (INT32)g_win_w[slot] : 0;
        rect[3] = dirty && slot < WINPAINT_SLOTS ? (INT32)g_win_h[slot] : 0;
    }
    return dirty ? 1 : 0;
}

static UINT64 NtUserUpdateWindow(UINT64 *a) { return 1; }
static UINT64 NtUserPostMessage(UINT64 *a)
{
    return KiUserEnqueueMessage(a[0], (UINT32)a[1], a[2], (INT64)a[3],
                                0, 0);
}

/* Continuation codes carried in KTHREAD.CallbackContinue. */
#define CB_CONT_CREATE_NCCREATE 1 /* next: WM_NCCREATE result handling   */
#define CB_CONT_CREATE_WMCREATE 2 /* next: WM_CREATE result handling     */
#define CB_CONT_SIMPLE          3 /* hand the WndProc result to the caller */

/* The kernel->user callback machinery is defined further down (it needs the
 * loader's stub arena); the message services here are its first users. */
static BOOLEAN KiStartUserCallback(UINT64 target, UINT64 a1, UINT64 a2,
                                   UINT64 a3, UINT64 a4, UINT64 continuation,
                                   UINT64 window, UINT64 msg, UINT64 lparam);

/*
 * Synchronous message delivery: SendMessage (NtUserMessageCall) and the message
 * pump's DispatchMessage both have to run the target window's procedure and
 * hand its LRESULT back to the caller. Both used to return 0 without calling
 * anything, which silently voided every SendMessage the shell makes -- and a
 * message pump whose DispatchMessage does nothing can never advance a window's
 * state machine. Both now go through the kernel->user callback path that
 * window creation already uses.
 */
static UINT64 KiWindowProcOf(UINT64 hwnd)
{
    UINT16 slot = (UINT16)(hwnd & 0xFFFF);
    if (!slot || slot >= 128)
        return 0;
    UINT64 window = PROCESS_USER_OBJECT_ARENA_VA + (UINT64)slot * 0x200;
    if (!MmProbeForRead(window + WND_WNDPROC_OFF, sizeof(UINT64)))
        return 0;
    return *(volatile UINT64 *)(window + WND_WNDPROC_OFF);
}

/* Returns TRUE when the callback was armed; the result then reaches the caller
 * through NtCallbackReturn, so the service's own return value is unused. */
static BOOLEAN KiStartUserCallback(UINT64 target, UINT64 a1, UINT64 a2,
                                   UINT64 a3, UINT64 a4, UINT64 continuation,
                                   UINT64 window, UINT64 msg, UINT64 lparam);
static BOOLEAN KiSendToWindowProc(UINT64 hwnd, UINT32 message, UINT64 wparam,
                                  INT64 lparam)
{
    UINT64 proc = KiWindowProcOf(hwnd);
    if (!proc)
        return FALSE;
    return KiStartUserCallback(proc, hwnd, message, wparam, (UINT64)lparam,
                               CB_CONT_SIMPLE, hwnd, message, (UINT64)lparam);
}

static UINT64 NtUserMessageCall(UINT64 *a)
{
    UINT64 hwnd = a[0];
    UINT32 message = (UINT32)a[1];
    UINT32 type = (UINT32)a[5];

    static UINT8 logged;
    if (logged < 32) {
        logged++;
        KeLog("[user] MessageCall(hwnd=%p, msg=0x%x, wp=%p, lp=%p, "
              "type=0x%lx)\n", (void *)hwnd, (unsigned)message, (void *)a[2],
              (void *)a[3], (unsigned long)type);
    }
    if (KiSendToWindowProc(hwnd, message, a[2], (INT64)a[3]))
        return 0; /* the WndProc's result is returned by NtCallbackReturn */
    return 0;
}

static UINT64 NtUserDispatchMessage(UINT64 *a)
{
    const USER_MSG_LOCAL *msg = (const USER_MSG_LOCAL *)a[0];
    if (!msg || !MmProbeForRead((UINT64)msg, sizeof(*msg)))
        return 0;
    /* A thread message (hwnd == 0) has no window procedure to run. */
    if (!msg->Hwnd)
        return 0;
    if (KiSendToWindowProc(msg->Hwnd, msg->Message, msg->WParam, msg->LParam))
        return 0;
    return 0;
}
static UINT64 NtUserTranslateMessage(UINT64 *a) { return 0; }
static UINT64 NtUserTranslateAccelerator(UINT64 *a) { return 0; }
static UINT64 NtUserSetTimer(UINT64 *a) { return 1; }
static UINT64 NtUserKillTimer(UINT64 *a) { return 1; }
static UINT64 NtUserSetFocus(UINT64 *a) { return 0; }
static UINT64 NtUserSetActiveWindow(UINT64 *a) { return 0; }
static UINT64 NtUserSetCapture(UINT64 *a) { return 0; }
static UINT64 NtUserReleaseCapture(UINT64 *a) { return 1; }
static UINT64 NtUserGetForegroundWindow(UINT64 *a)
{
    return g_user_active_hwnd;
}
static UINT64 NtUserQueryWindow(UINT64 *a)
{
    /* QueryWindow(hwnd, type): type 4 = window thread process id. */
    if ((UINT32)a[1] == 4) {
        PKTHREAD thread = KeGetCurrentThread();
        if (thread && thread->Process)
            return thread->Process->ProcessId;
    }
    return 0;
}
static UINT64 NtUserGetAncestor(UINT64 *a)
{
    /* GA_ROOT(2)/GA_ROOTOWNER(3): the single top-level window is itself. */
    return a[0];
}
static UINT64 NtUserWaitMessage(UINT64 *a)
{
    USER_MSG_LOCAL msg;
    while (!KiUserTakeMessage(&msg, FALSE))
        KeYield();
    return 0;
}
static UINT64 NtUserGetProcessWindowStation(UINT64 *a) { return 0x1; }
static UINT64 NtUserGetThreadDesktop(UINT64 *a) { return 0x2; }
static UINT64 NtUserSetThreadDesktop(UINT64 *a) { return 1; }
static UINT64 NtUserOpenInputDesktop(UINT64 *a) { return 0x3; }
static UINT64 NtUserCloseDesktop(UINT64 *a) { return 1; }
static UINT64 NtUserOpenDesktop(UINT64 *a) { return 0x3; }
static UINT64 NtUserOpenWindowStation(UINT64 *a) { return 0x4; }
static UINT64 NtUserSetProcessWindowStation(UINT64 *a) { return 1; }
static UINT64 NtUserCloseWindowStation(UINT64 *a) { return 1; }
static UINT64 NtUserNotifyWinEvent(UINT64 *a) { return 0; }
static UINT64 NtUserCallHwnd(UINT64 *a) { return 0; }
static UINT64 NtUserCallHwndLock(UINT64 *a) { return 0; }
static UINT64 NtUserCallHwndParam(UINT64 *a) { return 0; }
static UINT64 NtUserCallTwoParam(UINT64 *a) { return 0; }
static UINT64 NtUserEnumDisplayMonitors(UINT64 *a) { return 0; }
static UINT64 NtUserGetDC(UINT64 *a)
{
    /* GetDC(NULL) means the whole screen; GetDC(hwnd) is clipped to the
     * window. Non-client areas are not modelled, so the client rectangle is
     * the window rectangle. */
    UINT64 hwnd = a[0];
    INT32 x = 0, y = 0;
    INT32 w = (INT32)GfxFramebuffer.Width, h = (INT32)GfxFramebuffer.Height;
    UINT16 slot = (UINT16)(hwnd & 0xFFFF);
    if (hwnd && slot && slot < WINPAINT_SLOTS && g_win_kind[slot]) {
        x = g_win_x[slot];
        y = g_win_y[slot];
        w = (INT32)g_win_w[slot];
        h = (INT32)g_win_h[slot];
    }
    return W32kAcquireWindowDc(hwnd, x, y, w, h);
}

/* GetDCEx(hwnd, hrgnClip, flags) and GetWindowDC(hwnd) reach the same cached
 * device context; the clipping region and the non-client distinction are not
 * modelled yet. */
static UINT64 NtUserGetDCEx(UINT64 *a)
{
    return NtUserGetDC(a);
}


/* ------------------------------------------------------------------ */
/* USER objects other than windows                                     */
/*                                                                     */
/* USER32 validates every handle it is given against the shared         */
/* HANDLEENTRY table, so a cursor or an accelerator table cannot be a   */
/* fabricated number: it needs a real entry with the right type byte and */
/* an object head in the arena. Types follow the native ordering        */
/* (TYPE_WINDOW 1, TYPE_MENU 2, TYPE_CURSOR 3, ..., TYPE_ACCELTABLE 8). */
/* ------------------------------------------------------------------ */

#define USER_TYPE_CURSOR 3
#define USER_TYPE_ACCEL  8

#define USER_OBJECT_SLOTS \
    (PROCESS_USER_OBJECT_ARENA_SIZE / 0x200)

static UINT16 g_user_next_object = 200; /* above the window index range */

static UINT64 KiUserAllocateObject(UINT8 type, UINT64 *head_out)
{
    if (g_user_next_object >= USER_OBJECT_SLOTS)
        return 0;
    UINT16 index = g_user_next_object++;
    UINT64 head = PROCESS_USER_OBJECT_ARENA_VA + (UINT64)index * 0x200;
    memset((void *)head, 0, 0x200);
    UINT64 handle = (1ULL << 16) | index;
    *(UINT64 *)head = handle;          /* the head carries its own handle */
    *(UINT32 *)(head + 8) = 1;         /* lock count */

    MmProtectRange(PROCESS_USER_SHARED_TABLE_VA,
                   PROCESS_USER_SHARED_TABLE_SIZE, TRUE);
    UINT8 *entry = (UINT8 *)(PROCESS_USER_SHARED_TABLE_VA +
                             (UINT64)index * 24);
    *(UINT64 *)(entry + 0) = head;     /* Object   */
    *(UINT64 *)(entry + 8) = 0;        /* Owner    */
    entry[16] = type;                  /* Type     */
    entry[17] = 0;                     /* Flags    */
    *(UINT16 *)(entry + 18) = 1;       /* Generation */
    MmProtectRange(PROCESS_USER_SHARED_TABLE_VA,
                   PROCESS_USER_SHARED_TABLE_SIZE, FALSE);

    if (head_out)
        *head_out = head;
    return handle;
}

/*
 * HCURSOR NtUserFindExistingCursorIcon(PUNICODE_STRING module,
 *                                      PUNICODE_STRING resource, void *param)
 *
 * win32k caches cursors and icons per (module, resource) and hands the existing
 * object back; in a real session the stock IDC_* cursors are always already
 * there. Answering NULL sends USER32 down a creation path that this build only
 * reaches through NtUserSetCursorIconData, so the cache is what makes
 * LoadCursorW succeed -- and a program that checks its cursor (notepad does,
 * before it will register its window class) stops dead without it.
 */
#define USER_CURSOR_CACHE 32

static UINT64 NtUserFindExistingCursorIcon(UINT64 *a)
{
    typedef struct _CURSOR_KEY {
        UINT64 Module;
        UINT64 Resource;
        UINT64 Handle;
    } CURSOR_KEY;
    static CURSOR_KEY cache[USER_CURSOR_CACHE];
    static UINT32 count;

    /* The resource id is what distinguishes stock cursors; for a string name
     * the pointer to the captured name serves as the key. */
    UINT64 module = a[0], resource = a[1];
    UINT64 key = resource;
    if (resource && MmProbeForRead(resource, 16))
        key = *(volatile UINT64 *)(resource + 8); /* UNICODE_STRING.Buffer */

    for (UINT32 i = 0; i < count; i++)
        if (cache[i].Module == module && cache[i].Resource == key)
            return cache[i].Handle;

    UINT64 head = 0;
    UINT64 handle = KiUserAllocateObject(USER_TYPE_CURSOR, &head);
    if (!handle)
        return 0;
    if (count < USER_CURSOR_CACHE) {
        cache[count].Module = module;
        cache[count].Resource = key;
        cache[count].Handle = handle;
        count++;
    }
    KeLog("[user] cursor for module %p resource %p -> %p\n", (void *)module,
          (void *)key, (void *)handle);
    return handle;
}

/* HACCEL NtUserCreateAcceleratorTable(LPACCEL entries, ULONG count) */
static UINT64 NtUserCreateAcceleratorTable(UINT64 *a)
{
    UINT32 entries = (UINT32)a[1];
    UINT64 head = 0;
    UINT64 handle = KiUserAllocateObject(USER_TYPE_ACCEL, &head);
    if (!handle)
        return 0;
    /* The table itself is only consulted by TranslateAccelerator, which is a
     * no-op here; the handle is what the caller checks. */
    KeLog("[user] accelerator table (%lu entries) -> %p\n",
          (unsigned long)entries, (void *)handle);
    return handle;
}

static UINT64 NtUserDestroyAcceleratorTable(UINT64 *a) { return 1; }
static UINT64 NtUserDestroyCursor(UINT64 *a) { return 1; }
static UINT64 NtUserSetCursor(UINT64 *a) { return 0; }

/* PAINTSTRUCT on x64: HDC hdc (0), BOOL fErase (8), RECT rcPaint (12..28),
 * BOOL fRestore (28), BOOL fIncUpdate (32), BYTE rgbReserved[32] (36). */
#define PAINTSTRUCT_SIZE 72

static UINT64 NtUserBeginPaint(UINT64 *a)
{
    UINT16 slot = (UINT16)(a[0] & 0xFFFF);
    UINT8 *ps = (UINT8 *)a[1];
    if (!ps || !MmProbeForWrite((UINT64)ps, PAINTSTRUCT_SIZE))
        return 0;
    memset(ps, 0, PAINTSTRUCT_SIZE);

    INT32 w = 0, h = 0;
    if (slot && slot < WINPAINT_SLOTS) {
        w = (INT32)g_win_w[slot];
        h = (INT32)g_win_h[slot];
    }
    *(UINT32 *)(ps + 8) = 1;  /* fErase: nothing has drawn the background */
    *(INT32 *)(ps + 12) = 0;  /* rcPaint, in client coordinates */
    *(INT32 *)(ps + 16) = 0;
    *(INT32 *)(ps + 20) = w;
    *(INT32 *)(ps + 24) = h;

    KiUserValidateWindow(slot);
    UINT64 hdc = W32kAcquireWindowDc(a[0], slot < WINPAINT_SLOTS ?
                                     g_win_x[slot] : 0,
                                     slot < WINPAINT_SLOTS ?
                                     g_win_y[slot] : 0, w, h);
    *(UINT64 *)(ps + 0) = hdc;
    KeLog("[user] BeginPaint(HWND %p) rcPaint 0,0,%ld,%ld -> hdc %p\n",
          (void *)a[0], (long)w, (long)h, (void *)hdc);
    return hdc;
}

static UINT64 NtUserEndPaint(UINT64 *a) { return 1; }

/* ------------------------------------------------------------------ */
/* Kernel -> user callbacks (the KiUserCallbackDispatcher equivalent)  */
/* ------------------------------------------------------------------ */

#define SN_NtCallbackReturn 0xFC

/* The entry stub (syscall_entry.asm) hardcodes these KTHREAD offsets; fail
 * the build rather than silently corrupting the wrong fields when the struct
 * changes. */
#define KI_OFFSETOF(type, field) __builtin_offsetof(type, field)
_Static_assert(KI_OFFSETOF(KTHREAD, UserRip) == 0xAC0, "KTH_USER_RIP");
_Static_assert(KI_OFFSETOF(KTHREAD, UserRsp) == 0xAC8, "KTH_USER_RSP");
_Static_assert(KI_OFFSETOF(KTHREAD, UserRflags) == 0xAD0, "KTH_USER_RFLAGS");
_Static_assert(KI_OFFSETOF(KTHREAD, CallbackRip) == 0xAD8, "KTH_CB_RIP");
_Static_assert(KI_OFFSETOF(KTHREAD, CallbackRsp) == 0xAE0, "KTH_CB_RSP");
_Static_assert(KI_OFFSETOF(KTHREAD, CallbackFlags) == 0xAE8, "KTH_CB_FLAGS");
_Static_assert(KI_OFFSETOF(KTHREAD, RedirectPending) == 0xB29, "KTH_REDIRECT");

/* Dedicated stacks for WndProc callbacks. The user thread stacks are only
 * 64 KiB and a shell WndProc's call chain is far deeper than the space below
 * the arming syscall's RSP, so each callback chain claims its own 128 KiB
 * slot here (lazily mapped). Slots are never returned: windows are few and
 * creations are clustered at bootstrap. */
#define CALLBACK_STACK_ARENA_VA  0x0000000001400000ULL
#define CALLBACK_STACK_SLOTS     16
#define CALLBACK_STACK_SLOT_SIZE 0x0000000000020000ULL
static UINT8 g_callback_stack_next;

/* Claim a dedicated callback stack; returns its top (0 when out of slots). */
static UINT64 KiClaimCallbackStack(void)
{
    if (g_callback_stack_next >= CALLBACK_STACK_SLOTS)
        return 0;
    UINT64 slot = g_callback_stack_next++;
    UINT64 base = CALLBACK_STACK_ARENA_VA + slot * CALLBACK_STACK_SLOT_SIZE;
    for (UINT64 off = 0; off < CALLBACK_STACK_SLOT_SIZE; off += PAGE_SIZE) {
        UINT64 pa = MmAllocatePage();
        if (pa == MM_INVALID_PHYS)
            return 0;
        MmMapPage(base + off, pa, PTE_USER | PTE_WRITE);
    }
    return base + CALLBACK_STACK_SLOT_SIZE;
}


/*
 * Build a per-call trampoline in the stub arena that loads the four Win64
 * argument registers with immediates, provides home space, calls the target
 * (WndProc), and re-enters the kernel with the result:
 *   movabs rax, imm ; mov rcx/rdx/r8/r9, rax  (x4)
 *   sub rsp, 0x28 ; movabs rax, target ; call rax
 *   mov r10, rax ; mov eax, 0xFC ; syscall ; int3
 */
static UINT64 LdrpBuildCallbackThunk(UINT64 target, UINT64 a1, UINT64 a2,
                                     UINT64 a3, UINT64 a4)
{
    extern BOOLEAN LdrpEnsureStubArenaPub(void);
    if (!LdrpEnsureStubArenaPub())
        return 0;
    extern UINT64 LdrStubArenaBase(void), LdrStubArenaNext(UINT64 bytes),
        LdrStubArenaEnd(void);
    UINT64 need = 5 * 16;
    if (LdrStubArenaNext(0) + need > LdrStubArenaEnd())
        return 0;
    UINT64 slot = LdrStubArenaNext(need);
    UINT8 *code = (UINT8 *)slot;
    UINT64 args[4] = {a1, a2, a3, a4};
    int o = 0;
    for (int i = 0; i < 4; i++) {
        code[o++] = 0x48; code[o++] = 0xB8; /* movabs rax, imm64 */
        *(UINT64 *)(code + o) = args[i];
        o += 8;
        /* mov rcx/rdx, rax is 48 89 C1/C2; r8/r9 need REX.B: 49 89 C0/C1. */
        code[o++] = (UINT8)(i < 2 ? 0x48 : 0x49);
        code[o++] = 0x89;
        code[o++] = (UINT8)(i < 2 ? 0xC1 + i : 0xC0 + (i - 2));
    }
    code[o++] = 0x48; code[o++] = 0x83; code[o++] = 0xEC; /* sub rsp, 0x28 */
    code[o++] = 0x28;
    code[o++] = 0x48; code[o++] = 0xB8; /* movabs rax, target */
    *(UINT64 *)(code + o) = target;
    o += 8;
    code[o++] = 0xFF; code[o++] = 0xD0; /* call rax */
    /* mov r10, rax: REX.W|REX.B (49) with reg=rax, rm=r10. (0x4C 0x89 0xC2
     * would be `mov rdx, r8` -- the result would never reach the kernel.) */
    code[o++] = 0x49; code[o++] = 0x89; code[o++] = 0xC2;
    code[o++] = 0xB8; *(UINT32 *)(code + o) = SN_NtCallbackReturn; o += 4;
    code[o++] = 0x0F; code[o++] = 0x05; /* syscall */
    code[o++] = 0xCC;
    return slot;
}

/*
 * Arm a kernel->user callback for this thread: the current sysret (belonging
 * to the service calling this) is redirected into a trampoline that invokes
 * the WndProc. The first callback of a chain captures the original caller's
 * frame in Orig*; the final redirect (KiFinishCallbackToCaller) restores it
 * with the callback's result in RAX. Returns FALSE when a callback chain is
 * already in flight (nested callbacks are refused; the caller falls back to a
 * default result).
 */
static BOOLEAN KiStartUserCallback(UINT64 target, UINT64 a1, UINT64 a2,
                                   UINT64 a3, UINT64 a4, UINT64 continuation,
                                   UINT64 window, UINT64 msg, UINT64 lparam)
{
    PKTHREAD t = KeGetCurrentThread();
    if (!t || t->CallbackActive || t->RedirectPending)
        return FALSE;

    UINT64 thunk = LdrpBuildCallbackThunk(target, a1, a2, a3, a4);
    UINT64 stack = thunk ? KiClaimCallbackStack() : 0;
    if (!thunk || !stack)
        return FALSE;

    t->OrigRip = t->UserRip;
    t->OrigRsp = t->UserRsp;
    t->OrigRflags = t->UserRflags;
    /* Snapshot the caller's return address so the final redirect can put it
     * back even if ring-3 scribbled on the (now dead) stack slot meanwhile. */
    t->SavedReturnAddress =
        MmProbeForRead(t->UserRsp, sizeof(UINT64))
            ? *(volatile UINT64 *)t->UserRsp : 0;
    t->CallbackActive = 1;

    /* RSP%16==8 at the trampoline entry: its `sub rsp,0x28` + `call` then put
     * the WndProc entry at the Win64-conventional RSP%16==8 with the home
     * space mapped. */
    t->CallbackRip = thunk;
    t->CallbackRsp = ((stack - 0x100) & ~0xFULL) | 8;
    t->CallbackFlags = 0x202; /* IF set */
    t->CallbackContinue = continuation;
    t->CallbackWindow = window;
    t->CallbackMsg = msg;
    t->CallbackLParam = lparam;
    t->RedirectPending = 1;
    KeLog("[user]   callback -> proc %p msg 0x%lx hwnd %p (thunk %p rsp %p, "
          "caller %p)\n", (void *)target, (unsigned long)a2, (void *)a1,
          (void *)thunk, (void *)t->CallbackRsp, (void *)t->OrigRip);
    return TRUE;
}

/*
 * Chain the next callback of a creation (WM_NCCREATE -> WM_CREATE) from inside
 * NtCallbackReturn's dispatch. The original frame stays captured in Orig*;
 * only the redirect target changes.
 */
static BOOLEAN KiChainUserCallback(UINT64 target, UINT64 a1, UINT64 a2,
                                   UINT64 a3, UINT64 a4, UINT64 continuation,
                                   UINT64 window, UINT64 msg, UINT64 lparam)
{
    PKTHREAD t = KeGetCurrentThread();
    if (!t || !t->CallbackActive || t->RedirectPending)
        return FALSE;

    UINT64 thunk = LdrpBuildCallbackThunk(target, a1, a2, a3, a4);
    if (!thunk)
        return FALSE;

    t->CallbackRip = thunk;
    t->CallbackRsp = (t->CallbackRsp & ~0xFULL) | 8; /* reuse the stack slot */
    t->CallbackContinue = continuation;
    t->CallbackWindow = window;
    t->CallbackMsg = msg;
    t->CallbackLParam = lparam;
    t->RedirectPending = 1;
    return TRUE;
}

/* End of the chain: the next (and last) redirect sends the sysret back to the
 * original caller with `result` in RAX, exactly as the arming service would
 * have returned it. */
static void KiFinishCallbackToCaller(PKTHREAD t, UINT64 result)
{
    (void)result;
    t->CallbackRip = t->OrigRip;
    t->CallbackRsp = t->OrigRsp;
    t->CallbackFlags = t->OrigRflags;
    /* Enforce the win32k guarantee: the interrupted frame is returned to
     * exactly as it was left, including the slot its stub will `ret`
     * through. Ring-3 code running between arming and here (this thread's
     * own WndProc or a sibling thread's stray write) must not be able to
     * derail the return path. */
    if (t->SavedReturnAddress &&
        MmProbeForWrite(t->OrigRsp, sizeof(UINT64)))
        *(volatile UINT64 *)t->OrigRsp = t->SavedReturnAddress;
    t->CallbackContinue = 0;
    t->CallbackActive = 0;
    t->RedirectPending = 1;
}

/* NtCallbackReturn: the trampoline re-entered the kernel with the WndProc
 * result in R10. Drive the kernel-side continuation state machine. */
static UINT64 NtCallbackReturn(UINT64 *a)
{
    PKTHREAD t = KeGetCurrentThread();
    if (!t || !t->CallbackActive)
        return 0;
    UINT64 result = a[0]; /* r10 */
    KeLog("[user]   callback return: msg 0x%lx -> %p (cont %lu)\n",
          (unsigned long)t->CallbackMsg, (void *)result,
          (unsigned long)t->CallbackContinue);

    if (t->CallbackContinue == CB_CONT_CREATE_NCCREATE) {
        if (result == 0) {
            /* WM_NCCREATE refused: fail the creation. */
            UINT16 slot = (UINT16)(t->CallbackWindow & 0xFFFF);
            if (slot && slot < 128)
                g_window_owner[slot] = 0;
            KiFinishCallbackToCaller(t, 0);
            return 0; /* CreateWindowExW sees NULL */
        }
        /* Continue into WM_CREATE: the window head carries the WndProc. */
        UINT16 slot = (UINT16)(t->CallbackWindow & 0xFFFF);
        UINT64 window = PROCESS_USER_OBJECT_ARENA_VA + (UINT64)slot * 0x200;
        UINT64 proc = 0;
        if (MmProbeForRead(window + WND_WNDPROC_OFF, 8))
            proc = *(volatile UINT64 *)(window + WND_WNDPROC_OFF);
        if (proc && KiChainUserCallback(proc, t->CallbackWindow, 0x0001, 0,
                                        t->CallbackLParam,
                                        CB_CONT_CREATE_WMCREATE,
                                        t->CallbackWindow, 0x0001,
                                        t->CallbackLParam))
            return 0; /* redirected again; value unused */
        KiFinishCallbackToCaller(t, t->CallbackWindow);
        return t->CallbackWindow;
    }

    if (t->CallbackContinue == CB_CONT_CREATE_WMCREATE) {
        UINT16 slot = (UINT16)(t->CallbackWindow & 0xFFFF);
        if ((INT64)result == -1) {
            /* WM_CREATE refused: destroy the half-made window. */
            if (slot && slot < 128)
                g_window_owner[slot] = 0;
            KiFinishCallbackToCaller(t, 0);
            return 0;
        }
        KiFinishCallbackToCaller(t, t->CallbackWindow);
        return t->CallbackWindow;
    }

    KiFinishCallbackToCaller(t, result);
    return result;
}

/* Build the CREATESTRUCTW a WM_NCCREATE/WM_CREATE lParam points at, from the
 * NtUserCreateWindowEx argument block: (exStyle, class, version, name, style,
 * x, y, cx, cy, parent, menu, instance, lpParam, ...). x64 layout. */
static UINT64 KiBuildCreateStruct(const UINT64 *a, UINT64 hwnd)
{
    extern BOOLEAN LdrpEnsureStubArenaPub(void);
    extern UINT64 LdrStubArenaNext(UINT64 bytes), LdrStubArenaEnd(void);
    if (!LdrpEnsureStubArenaPub() ||
        LdrStubArenaNext(0) + 0x60 > LdrStubArenaEnd())
        return 0;
    UINT64 p = LdrStubArenaNext(0x60);
    UINT8 *cs = (UINT8 *)p;
    memset(cs, 0, 0x60);
    *(UINT64 *)(cs + 0x00) = a[12];      /* lpCreateParams */
    *(UINT64 *)(cs + 0x08) = a[11];      /* hInstance      */
    *(UINT64 *)(cs + 0x10) = a[10];      /* hMenu          */
    *(UINT64 *)(cs + 0x18) = a[9];       /* hwndParent     */
    *(UINT32 *)(cs + 0x20) = (UINT32)a[8]; /* cy           */
    *(UINT32 *)(cs + 0x24) = (UINT32)a[7]; /* cx           */
    *(UINT32 *)(cs + 0x28) = (UINT32)a[6]; /* y            */
    *(UINT32 *)(cs + 0x2C) = (UINT32)a[5]; /* x            */
    *(UINT32 *)(cs + 0x30) = (UINT32)a[4]; /* style        */
    *(UINT64 *)(cs + 0x38) = a[3];       /* lpszName       */
    *(UINT64 *)(cs + 0x40) = a[1];       /* lpszClass (or atom) */
    *(UINT64 *)(cs + 0x48) = hwnd;       /* helper: creator's own HWND */
    KeLog("[user]   CREATESTRUCT @ %p for HWND %p\n", (void *)p,
          (void *)hwnd);
    return p;
}

static UINT64 NtUserCreateWindowEx(UINT64 *a)
{
    typedef struct _USER_HANDLE_ENTRY_LOCAL {
        UINT64 Object;
        UINT64 Owner;
        UINT8 Type;
        UINT8 Flags;
        UINT16 Generation;
        UINT32 Padding;
    } USER_HANDLE_ENTRY_LOCAL;

    typedef struct _USER_WINDOW_HEAD_LOCAL {
        UINT64 Handle;
        UINT32 LockCount;
        UINT32 Flags;
        UINT64 ThreadInfo;
        UINT64 Desktop;
        UINT64 Self;
    } USER_WINDOW_HEAD_LOCAL;

    static UINT16 next_window_index = 1;

    /* Modern win32u passes class-version separately from the window name:
     * exStyle, class, classVersion, name, style, x, y, cx, cy, ... */
    KeLog("[user] NtUserCreateWindowEx(exStyle=0x%lx, class=%p, version=%p, name=%p, style=0x%lx, xy=%ld,%ld size=%ldx%ld)\n",
          (unsigned long)a[0], (void *)a[1], (void *)a[2], (void *)a[3],
          (unsigned long)a[4], (long)a[5], (long)a[6], (long)a[7],
          (long)a[8]);

    if (next_window_index >= 128)
        return 0;

    UINT16 index = next_window_index++;
    UINT16 generation = 1;
    UINT64 hwnd = ((UINT64)generation << 16) | index;
    USER_HANDLE_ENTRY_LOCAL *entries =
        (USER_HANDLE_ENTRY_LOCAL *)PROCESS_USER_SHARED_TABLE_VA;
    USER_WINDOW_HEAD_LOCAL *window = (USER_WINDOW_HEAD_LOCAL *)(
        PROCESS_USER_OBJECT_ARENA_VA + (UINT64)index * 0x200);

    memset(window, 0, 0x200);
    window->Handle = hwnd;
    window->LockCount = 1;
    /* Flip the shared handle table writable for the kernel-side fill; ring 3
     * normally sees it read-only (see PsCreateUserProcess). */
    MmProtectRange(PROCESS_USER_SHARED_TABLE_VA, PROCESS_USER_SHARED_TABLE_SIZE,
                   TRUE);
    entries[index].Object = (UINT64)window;
    entries[index].Owner = 0;
    entries[index].Type = 1; /* TYPE_WINDOW */
    entries[index].Flags = 0;
    entries[index].Generation = generation;
    MmProtectRange(PROCESS_USER_SHARED_TABLE_VA, PROCESS_USER_SHARED_TABLE_SIZE,
                   FALSE);

    PKTHREAD creator = KeGetCurrentThread();
    g_window_owner[index] = creator ? creator->ThreadId : 0;

    USER_CLASS_LOCAL *cls = KiClassForCreate(a[1]);
    KiPublishWindowState((UINT64)window, (UINT32)a[4], (UINT32)a[0],
                         a[9], cls);
    {
        char ascii[33];
        UINT32 j = 0;
        if (cls)
            for (; j < 32 && cls->Name[j]; j++)
                ascii[j] = (char)cls->Name[j];
        ascii[j] = 0;
        KeLog("[user]   created HWND %p head %p class '%s' atom 0x%x "
              "wndproc %p extra %lu\n", (void *)hwnd, (void *)window,
              cls ? ascii : "<unresolved>", cls ? cls->Atom : 0,
              (void *)(cls ? cls->WndProc : 0),
              (unsigned long)(cls ? cls->WndExtra : 0));
    }
    if (index < WINPAINT_SLOTS) {
        g_win_kind[index] = WINPAINT_GENERIC;
        if (cls && KiClassNameIs(cls->Name, "WorkerW"))
            g_win_kind[index] = WINPAINT_DESKTOP;
        else if (cls && KiClassNameIs(cls->Name, "Shell_TrayWnd"))
            g_win_kind[index] = WINPAINT_TASKBAR;
        g_win_x[index] = (INT32)a[5];
        g_win_y[index] = (INT32)a[6];
        g_win_w[index] = (UINT32)a[7];
        g_win_h[index] = (UINT32)a[8];
    }
    /* Bring-up shortcut: the desktop and taskbar are painted the moment
     * they exist. The shell's own ShowWindow/SetWindowPos traffic does not
     * reach those services yet, so waiting for a visibility bit would keep
     * the screen black. */
    if (index < WINPAINT_SLOTS &&
        (g_win_kind[index] == WINPAINT_DESKTOP ||
         g_win_kind[index] == WINPAINT_TASKBAR)) {
        *(volatile UINT32 *)(PROCESS_USER_OBJECT_ARENA_VA +
                             (UINT64)index * 0x200 + WND_STYLE_OFF) |=
            WS_VISIBLE_;
        KiRepaintAll();
    }
    /* A window that is created visible owes its first WM_PAINT immediately. */
    if (((UINT32)a[4] & WS_VISIBLE_) || g_win_kind[index] == WINPAINT_DESKTOP ||
        g_win_kind[index] == WINPAINT_TASKBAR)
        KiUserInvalidateWindow(index);
    if (cls && cls->WndProc) {
        UINT64 cs = KiBuildCreateStruct(a, hwnd);
        if (KiStartUserCallback(cls->WndProc, hwnd, 0x0081 /* WM_NCCREATE */,
                                0, cs, CB_CONT_CREATE_NCCREATE, hwnd,
                                0x0081, cs))
            return 0; /* value unused; the callback path returns the HWND */
    }

    return hwnd;
}

/* RegisterHotKey(hwnd, id, modifiers, vk): the shell registers its Win-key
 * hotkeys during taskbar bring-up. No hotkey table exists yet, so nothing is
 * delivered, but the registration itself must succeed -- FALSE is a failure
 * the shell treats as a broken window station. */
static UINT64 NtUserRegisterHotKey(UINT64 *a)
{
    KeLog("[user] RegisterHotKey(hwnd=%p, id=%ld, mods=0x%lx, vk=0x%lx)\n",
          (void *)a[0], (long)(INT32)a[1], (unsigned long)(UINT32)a[2],
          (unsigned long)(UINT32)a[3]);
    return 1;
}

static UINT64 NtUserGetObjectInformation(UINT64 *a)
{
    /* (handle, index, pvInfo, cbInfo, pcbNeeded) -- GetUserObjectInformationW
     * reads the name/type/flags of the window station or desktop. Callers
     * compare the desktop name against L"Default" and expect wide strings; a
     * NULL pvInfo with pcbNeeded is the documented length-query pattern. */
    UINT64 handle = a[0];
    UINT32 index = (UINT32)a[1];
    WCHAR *out = (WCHAR *)a[2];
    UINT32 size = (UINT32)a[3];
    UINT32 *needed = (UINT32 *)a[4];

    KeLog("[user] GetObjectInformation(h=%lu, idx=%lu, out=%p, size=%lu)\n",
          (unsigned long)(UINT32)handle, (unsigned long)index, (void *)out,
          (unsigned long)size);
    BOOLEAN station = (handle == 1 || handle == 4);
    static const WCHAR name_desktop[] = {'D','e','f','a','u','l','t',0};
    static const WCHAR name_station[] = {'W','i','n','S','t','a','0',0};
    static const WCHAR type_desktop[] = {'D','e','s','k','t','o','p',0};
    static const WCHAR type_station[] =
        {'W','i','n','d','o','w','S','t','a','t','i','o','n',0};
    const WCHAR *text = 0;
    UINT32 bytes = 0;
    UINT32 flags_word = 0; /* for UOI_FLAGS */

    if (index == 1) { /* UOI_FLAGS: USEROBJECTFLAGS {inherit, reserved, flags} */
        bytes = 12;
        flags_word = 1; /* WSF_VISIBLE */
    } else if (index == 2) { /* UOI_NAME */
        text = station ? name_station : name_desktop;
        for (UINT32 i = 0; text[i]; i++)
            bytes += sizeof(WCHAR);
        bytes += sizeof(WCHAR);
    } else if (index == 3) { /* UOI_TYPE */
        text = station ? type_station : type_desktop;
        for (UINT32 i = 0; text[i]; i++)
            bytes += sizeof(WCHAR);
        bytes += sizeof(WCHAR);
    } else {
        return 0;
    }

    if (needed && MmProbeForWrite((UINT64)needed, sizeof(UINT32)))
        *needed = bytes;
    if (!out)
        return 1; /* length query */
    if (size < bytes || !MmProbeForWrite((UINT64)out, bytes))
        return 0;
    if (index == 1) {
        UINT32 *words = (UINT32 *)out;
        words[0] = 0;
        words[1] = 0;
        words[2] = flags_word;
    } else {
        for (UINT32 i = 0; i * sizeof(WCHAR) < bytes; i++)
            out[i] = text[i];
    }
    return 1;
}

static UINT64 NtUserChangeWindowMessageFilterEx(UINT64 *a)
{
    /* (hwnd, message, action, pCHANGEFILTERSTRUCT): the shell asserts the
     * taskbar/desktop messages through this gate right after creating the
     * WorkerW. Grant the request and report the filtered-allowed state. */
    UINT64 *out = (UINT64 *)a[3];
    if (out && MmProbeForWrite((UINT64)out, sizeof(UINT64))) {
        out[0] = 4; /* cbSize (DWORD) + status STATUS_SUCCESS (DWORD) */
    }
    return 1;
}

static UINT64 NtUserDestroyWindow(UINT64 *a)
{
    UINT16 slot = (UINT16)(a[0] & 0xFFFF);
    if (slot && slot < 128) {
        g_window_owner[slot] = 0;
        if (slot < WINPAINT_SLOTS)
            g_win_kind[slot] = WINPAINT_NONE;
        if (g_user_active_hwnd == a[0])
            g_user_active_hwnd = 0;
    }
    KiRepaintAll();
    return 1;
}

static UINT64 NtUserDdeInitialize(UINT64 *a)
{
    /* DDE is optional for the shell startup path. Zero is the native success
     * status used by its USER wrapper. */
    return 0;
}

static UINT64 NtUserSetWinEventHook(UINT64 *a)
{
    /* No hook object/callback delivery exists yet. A NULL hook is the valid
     * failure sentinel and prevents USER32 from treating an NTSTATUS as an
     * H-WINEVENTHOOK. */
    return 0;
}

static UINT64 NtUserUnhookWinEvent(UINT64 *a)
{
    return 0;
}

static UINT64 NtUserGetMessage(UINT64 *a)
{
    USER_MSG_LOCAL *user_msg = (USER_MSG_LOCAL *)a[0];
    if (!user_msg || !MmProbeForWrite((UINT64)user_msg, sizeof(*user_msg)))
        return (UINT64)-1;

    USER_MSG_LOCAL msg;
    PKTHREAD caller = KeGetCurrentThread();
    UINT32 tid = caller ? caller->ThreadId : 0;
    if (!KiUserTakeMessage(&msg, TRUE)) {
        /* Report the first time each thread parks in GetMessage: an empty
         * queue with nothing to wake it is the signature of a shell that is
         * waiting on USER machinery we have not built yet. */
        static UINT8 parked[USER_MAX_THREAD_QUEUES + 1];
        if (tid <= USER_MAX_THREAD_QUEUES && !parked[tid]) {
            parked[tid] = 1;
            KeLog("[user] GetMessage: thread %lu queue empty, waiting\n",
                  (unsigned long)tid);
        }
        do
            KeYield();
        while (!KiUserTakeMessage(&msg, TRUE));
    }
    *user_msg = msg;
    KeLog("[user] GetMessage -> msg 0x%x hwnd %p (tid %lu)\n",
          (unsigned)msg.Message, (void *)msg.Hwnd, (unsigned long)tid);
    return msg.Message == 0x0012 /* WM_QUIT */ ? 0 : 1;
}

/*
 * The system service table, indexed by real Windows 7 SP1 x64 syscall numbers
 * (from the public NT syscall-number tables). Sparse: most slots are unused.
 * Our ntdll stubs issue these same numbers, and a service's slot holds the
 * routine implementing it. Aligning to a real build's numbering is what would
 * let a genuine ntdll drive this kernel; the exact values target Win7 SP1 x64.
 */
#define NTOS_MAX_SYSCALL 0x2000
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
#define SN_NtCreateThreadEx        0xA5
#define SN_NtCreateFile            0x52
#define SN_NtSetValueKey           0x5D
#define SN_NtTerminateThread       0x50
#define SN_NtDelayExecution        0x31
#define SN_NtQuerySystemInformation 0x33
#define SN_NtProtectVirtualMemory  0x4D
/* NTOS-private services (no Windows equivalent) live above the real range. */
#define SN_NtDisplayString         0xF0
#define SN_NtDisplayNumber         0xF1
#define SN_NtLoadLibrary           0xF2
#define SN_NtEnumerateRootFiles    0xF3
#define SN_NtQueryFileInfo         0xF4
#define SN_NtSetFilePosition       0xF5
#define SN_NtWaitForMultipleObjects 0xF6
#define SN_NtResetEvent             0xF7
#define SN_NtCreateSemaphore        0xF8
#define SN_NtReleaseSemaphore       0xF9
#define SN_NtQueryInformationProcess 0xFA
#define SN_NtTraceCall           0xFB

/* win32u.dll service numbers from the matching Windows 10 user-mode build. */
#define SN_NtUserGetThreadState         0x1003
#define SN_NtUserPeekMessage            0x1004
#define SN_NtUserCallOneParam           0x1005
#define SN_NtUserGetMessage             0x1009
#define SN_NtUserGetDC                  0x100D
#define SN_NtUserPostThreadMessage      0x1061
#define SN_NtUserGetCaretBlinkTime      0x10F5
#define SN_NtUserGetAtomName            0x10AB
#define SN_NtUserRegisterWindowMessage  0x103A
#define SN_NtUserSystemParametersInfo   0x1045
#define SN_NtUserFindWindowEx           0x1070
#define SN_NtUserCreateWindowEx         0x1078
#define SN_NtUserDestroyWindow          0x109F
#define SN_NtUserDdeInitialize          0x110E
#define SN_NtUserSetWinEventHook        0x1105
#define SN_NtUserUnhookWinEvent         0x1106
#define SN_NtUserEnableMouseInPointer   0x1372
#define SN_NtUserSetProcessUIAccessZorder 0x143C
/* Service numbers extracted from the matching win32u.dll build. */
#define SN_NtUserCallNoParam            0x1008
#define SN_NtUserMessageCall            0x100A
#define SN_NtUserPostMessage            0x1012
#define SN_NtUserQueryWindow            0x1013
#define SN_NtUserTranslateAccelerator   0x1014
#define SN_NtUserBeginPaint             0x101A
#define SN_NtUserSetTimer               0x101B
#define SN_NtUserEndPaint               0x101C
#define SN_NtUserKillTimer              0x101E
#define SN_NtUserSetWindowPos           0x1027
#define SN_NtUserDispatchMessage        0x1039
#define SN_NtUserGetForegroundWindow    0x103F
#define SN_NtUserSetCapture             0x104C
#define SN_NtUserEnumDisplayMonitors    0x104D
#define SN_NtUserSetProp                0x104F
#define SN_NtUserShowWindow             0x105A
#define SN_NtUserMoveWindow             0x1060
#define SN_NtUserInvalidateRect         0x1007
#define SN_NtUserUpdateWindow           0x10E9
#define SN_NtUserGetAncestor            0x10B4
#define SN_NtUserRegisterClassExWOW     0x10B2
#define SN_NtUserUnregisterClass        0x10BD
#define SN_NtUserGetClassInfoEx         0x10BB
#define SN_NtUserRemoveProp             0x1049
#define SN_NtUserGetProp                0x1011
#define SN_NtUserWaitMessage            0x100F
#define SN_NtUserTranslateMessage       0x1010
#define SN_NtUserSetActiveWindow        0x10E1
#define SN_NtUserReleaseCapture         0x1017
#define SN_NtUserGetProcessWindowStation 0x1025
#define SN_NtUserGetThreadDesktop       0x1085
#define SN_NtUserSetThreadDesktop       0x1093
#define SN_NtUserOpenInputDesktop       0x13F0
#define SN_NtUserCloseDesktop           0x10A8
#define SN_NtUserOpenDesktop            0x10A9
#define SN_NtUserOpenWindowStation      0x10A2
#define SN_NtUserSetProcessWindowStation 0x10AA
#define SN_NtUserCloseWindowStation     0x10B7
#define SN_NtUserNotifyWinEvent         0x1030
#define SN_NtUserCallHwnd               0x110D
#define SN_NtUserCallHwndLock           0x1024
#define SN_NtUserCallHwndParam          0x10A0
#define SN_NtUserCallTwoParam           0x102D
#define SN_NtUserSetWindowLong          0x105E
#define SN_NtUserSetWindowLongPtr       0x1471
#define SN_NtUserChangeWindowMessageFilterEx 0x134A
#define SN_NtUserGetObjectInformation     0x106E
#define SN_NtUserRegisterHotKey           0x1403
#define SN_NtUserValidateRect             0x10CF
#define SN_NtUserRedrawWindow             0x1016
#define SN_NtUserGetUpdateRect            0x1056
#define SN_NtUserFindExistingCursorIcon   0x1041
#define SN_NtUserCreateAcceleratorTable   0x10F2
#define SN_NtUserDestroyAcceleratorTable  0x10FF
#define SN_NtUserDestroyCursor            0x109E
#define SN_NtUserSetCursor                0x101D
#define SN_NtGdiGetEntry                  0x12AE
#define SN_NtGdiCreateSolidBrush          0x10BA
#define SN_NtGdiCreatePen                 0x1059
#define SN_NtGdiHfontCreate               0x105F
#define SN_NtGdiCreateRectRgn             0x1086
#define SN_NtGdiSelectBrush               0x12F6
#define SN_NtGdiSelectPen                 0x12F8
#define SN_NtGdiSelectFont                0x103C
#define SN_NtGdiSelectBitmap              0x100E
#define SN_NtGdiDeleteObjectApp           0x1026
#define SN_NtGdiPatBlt                    0x105C
#define SN_NtGdiBitBlt                    0x100B
#define SN_NtGdiExtTextOutW               0x103B
#define SN_NtGdiGetTextMetricsW           0x1077
#define SN_NtGdiGetTextExtent             0x1096
#define SN_NtGdiGetTextExtentExW          0x12CB
#define SN_NtGdiSetPixel                  0x10B1
#define SN_NtGdiRectangle                 0x1092
#define SN_NtGdiMoveTo                    0x12DC
#define SN_NtGdiLineTo                    0x1044
#define SN_NtGdiCreateCompatibleDC        0x1057
#define SN_NtGdiCreateCompatibleBitmap    0x104E
#define SN_NtGdiCreateBitmap              0x106F
#define SN_NtGdiGetDCObject               0x1038
#define SN_NtGdiFlush                     0x1015
#define SN_NtGdiGetDeviceCaps             0x12A6
#define SN_NtGdiGetCurrentDpiInfo         0x12A5
#define SN_NtUserGetDCEx                  0x1094
#define SN_NtUserGetWindowDC              0x1066

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
    KiServiceTable[SN_NtCreateThreadEx]        = NtCreateThreadEx;
    KiServiceTable[SN_NtCreateFile]            = NtCreateFile;
    KiServiceTable[SN_NtSetValueKey]           = NtSetValueKey;
    KiServiceTable[SN_NtTerminateThread]       = NtTerminateThread;
    KiServiceTable[SN_NtDelayExecution]        = NtDelayExecution;
    KiServiceTable[SN_NtQuerySystemInformation] = NtQuerySystemInformation;
    KiServiceTable[SN_NtProtectVirtualMemory]  = NtProtectVirtualMemory;
    KiServiceTable[SN_NtDisplayString]         = NtDisplayString;
    KiServiceTable[SN_NtDisplayNumber]         = NtDisplayNumber;
    KiServiceTable[SN_NtLoadLibrary]           = NtLoadLibrary;
    KiServiceTable[SN_NtEnumerateRootFiles]    = NtEnumerateRootFiles;
    KiServiceTable[SN_NtQueryFileInfo]         = NtQueryFileInfo;
    KiServiceTable[SN_NtSetFilePosition]       = NtSetFilePosition;
    KiServiceTable[SN_NtWaitForMultipleObjects] = NtWaitForMultipleObjects;
    KiServiceTable[SN_NtResetEvent]             = NtResetEvent;
    KiServiceTable[SN_NtCreateSemaphore]        = NtCreateSemaphore;
    KiServiceTable[SN_NtReleaseSemaphore]       = NtReleaseSemaphore;
    KiServiceTable[SN_NtQueryInformationProcess] = NtQueryInformationProcess;
    KiServiceTable[SN_NtTraceCall] = NtTraceCall;
    KiServiceTable[SN_NtCallbackReturn] = NtCallbackReturn;
    KiServiceTable[SN_NtUserGetThreadState] = NtUserGetThreadState;
    KiServiceTable[SN_NtUserPeekMessage] = NtUserPeekMessage;
    KiServiceTable[SN_NtUserCallOneParam] = NtUserCallOneParam;
    KiServiceTable[SN_NtUserGetMessage] = NtUserGetMessage;
    KiServiceTable[SN_NtUserGetDC] = NtUserGetDC;
    KiServiceTable[SN_NtUserPostThreadMessage] = NtUserPostThreadMessage;
    KiServiceTable[SN_NtUserGetCaretBlinkTime] = NtUserGetCaretBlinkTime;
    KiServiceTable[SN_NtUserGetAtomName] = NtUserGetAtomName;
    KiServiceTable[SN_NtUserRegisterWindowMessage] = NtUserRegisterWindowMessage;
    KiServiceTable[SN_NtUserSystemParametersInfo] = NtUserSystemParametersInfo;
    KiServiceTable[SN_NtUserFindWindowEx] = NtUserFindWindowEx;
    KiServiceTable[SN_NtUserCreateWindowEx] = NtUserCreateWindowEx;
    KiServiceTable[SN_NtUserDestroyWindow] = NtUserDestroyWindow;
    KiServiceTable[SN_NtUserDdeInitialize] = NtUserDdeInitialize;
    KiServiceTable[SN_NtUserSetWinEventHook] = NtUserSetWinEventHook;
    KiServiceTable[SN_NtUserUnhookWinEvent] = NtUserUnhookWinEvent;
    KiServiceTable[SN_NtUserEnableMouseInPointer] = NtUserEnableMouseInPointer;
    KiServiceTable[SN_NtUserSetProcessUIAccessZorder] =
        NtUserSetProcessUIAccessZorder;
    KiServiceTable[SN_NtUserCallNoParam] = NtUserCallNoParam;
    KiServiceTable[SN_NtUserMessageCall] = NtUserMessageCall;
    KiServiceTable[SN_NtUserPostMessage] = NtUserPostMessage;
    KiServiceTable[SN_NtUserQueryWindow] = NtUserQueryWindow;
    KiServiceTable[SN_NtUserTranslateAccelerator] = NtUserTranslateAccelerator;
    KiServiceTable[SN_NtUserBeginPaint] = NtUserBeginPaint;
    KiServiceTable[SN_NtUserSetTimer] = NtUserSetTimer;
    KiServiceTable[SN_NtUserEndPaint] = NtUserEndPaint;
    KiServiceTable[SN_NtUserKillTimer] = NtUserKillTimer;
    KiServiceTable[SN_NtUserSetWindowPos] = NtUserSetWindowPos;
    KiServiceTable[SN_NtUserDispatchMessage] = NtUserDispatchMessage;
    KiServiceTable[SN_NtUserGetForegroundWindow] = NtUserGetForegroundWindow;
    KiServiceTable[SN_NtUserSetCapture] = NtUserSetCapture;
    KiServiceTable[SN_NtUserEnumDisplayMonitors] = NtUserEnumDisplayMonitors;
    KiServiceTable[SN_NtUserSetProp] = NtUserSetProp;
    KiServiceTable[SN_NtUserShowWindow] = NtUserShowWindow;
    KiServiceTable[SN_NtUserMoveWindow] = NtUserMoveWindow;
    KiServiceTable[SN_NtUserInvalidateRect] = NtUserInvalidateRect;
    KiServiceTable[SN_NtUserUpdateWindow] = NtUserUpdateWindow;
    KiServiceTable[SN_NtUserGetAncestor] = NtUserGetAncestor;
    KiServiceTable[SN_NtUserRegisterClassExWOW] = NtUserRegisterClassExWOW;
    KiServiceTable[SN_NtUserUnregisterClass] = NtUserUnregisterClass;
    KiServiceTable[SN_NtUserGetClassInfoEx] = NtUserGetClassInfoEx;
    KiServiceTable[SN_NtUserRemoveProp] = NtUserRemoveProp;
    KiServiceTable[SN_NtUserGetProp] = NtUserGetProp;
    KiServiceTable[SN_NtUserWaitMessage] = NtUserWaitMessage;
    KiServiceTable[SN_NtUserTranslateMessage] = NtUserTranslateMessage;
    KiServiceTable[SN_NtUserSetActiveWindow] = NtUserSetActiveWindow;
    KiServiceTable[SN_NtUserReleaseCapture] = NtUserReleaseCapture;
    KiServiceTable[SN_NtUserGetProcessWindowStation] =
        NtUserGetProcessWindowStation;
    KiServiceTable[SN_NtUserGetThreadDesktop] = NtUserGetThreadDesktop;
    KiServiceTable[SN_NtUserSetThreadDesktop] = NtUserSetThreadDesktop;
    KiServiceTable[SN_NtUserOpenInputDesktop] = NtUserOpenInputDesktop;
    KiServiceTable[SN_NtUserCloseDesktop] = NtUserCloseDesktop;
    KiServiceTable[SN_NtUserOpenDesktop] = NtUserOpenDesktop;
    KiServiceTable[SN_NtUserOpenWindowStation] = NtUserOpenWindowStation;
    KiServiceTable[SN_NtUserSetProcessWindowStation] =
        NtUserSetProcessWindowStation;
    KiServiceTable[SN_NtUserCloseWindowStation] = NtUserCloseWindowStation;
    KiServiceTable[SN_NtUserNotifyWinEvent] = NtUserNotifyWinEvent;
    KiServiceTable[SN_NtUserCallHwnd] = NtUserCallHwnd;
    KiServiceTable[SN_NtUserCallHwndLock] = NtUserCallHwndLock;
    KiServiceTable[SN_NtUserCallHwndParam] = NtUserCallHwndParam;
    KiServiceTable[SN_NtUserGetObjectInformation] = NtUserGetObjectInformation;
    KiServiceTable[SN_NtUserRegisterHotKey] = NtUserRegisterHotKey;
    KiServiceTable[SN_NtUserCallTwoParam] = NtUserCallTwoParam;
    KiServiceTable[SN_NtUserSetWindowLong] = NtUserSetWindowLong;
    KiServiceTable[SN_NtUserSetWindowLongPtr] = NtUserSetWindowLong;
    KiServiceTable[SN_NtUserChangeWindowMessageFilterEx] =
        NtUserChangeWindowMessageFilterEx;
    KiServiceTable[SN_NtUserValidateRect] = NtUserValidateRect;
    KiServiceTable[SN_NtUserRedrawWindow] = NtUserRedrawWindow;
    KiServiceTable[SN_NtUserGetUpdateRect] = NtUserGetUpdateRect;
    KiServiceTable[SN_NtUserFindExistingCursorIcon] =
        NtUserFindExistingCursorIcon;
    KiServiceTable[SN_NtUserCreateAcceleratorTable] =
        NtUserCreateAcceleratorTable;
    KiServiceTable[SN_NtUserDestroyAcceleratorTable] =
        NtUserDestroyAcceleratorTable;
    KiServiceTable[SN_NtUserDestroyCursor] = NtUserDestroyCursor;
    KiServiceTable[SN_NtUserSetCursor] = NtUserSetCursor;
    KiServiceTable[SN_NtGdiGetEntry] = NtGdiGetEntry;
    KiServiceTable[SN_NtGdiCreateSolidBrush] = NtGdiCreateSolidBrush;
    KiServiceTable[SN_NtGdiCreatePen] = NtGdiCreatePen;
    KiServiceTable[SN_NtGdiHfontCreate] = NtGdiHfontCreate;
    KiServiceTable[SN_NtGdiCreateRectRgn] = NtGdiCreateRectRgn;
    KiServiceTable[SN_NtGdiSelectBrush] = NtGdiSelectBrush;
    KiServiceTable[SN_NtGdiSelectPen] = NtGdiSelectPen;
    KiServiceTable[SN_NtGdiSelectFont] = NtGdiSelectFont;
    KiServiceTable[SN_NtGdiSelectBitmap] = NtGdiSelectBitmap;
    KiServiceTable[SN_NtGdiDeleteObjectApp] = NtGdiDeleteObjectApp;
    KiServiceTable[SN_NtGdiPatBlt] = NtGdiPatBlt;
    KiServiceTable[SN_NtGdiBitBlt] = NtGdiBitBlt;
    KiServiceTable[SN_NtGdiExtTextOutW] = NtGdiExtTextOutW;
    KiServiceTable[SN_NtGdiGetTextMetricsW] = NtGdiGetTextMetricsW;
    KiServiceTable[SN_NtGdiGetTextExtent] = NtGdiGetTextExtent;
    KiServiceTable[SN_NtGdiGetTextExtentExW] = NtGdiGetTextExtentExW;
    KiServiceTable[SN_NtGdiSetPixel] = NtGdiSetPixel;
    KiServiceTable[SN_NtGdiRectangle] = NtGdiRectangle;
    KiServiceTable[SN_NtGdiMoveTo] = NtGdiMoveTo;
    KiServiceTable[SN_NtGdiLineTo] = NtGdiLineTo;
    KiServiceTable[SN_NtGdiCreateCompatibleDC] = NtGdiCreateCompatibleDC;
    KiServiceTable[SN_NtGdiCreateCompatibleBitmap] = NtGdiCreateCompatibleBitmap;
    KiServiceTable[SN_NtGdiCreateBitmap] = NtGdiCreateBitmap;
    KiServiceTable[SN_NtGdiGetDCObject] = NtGdiGetDCObject;
    KiServiceTable[SN_NtGdiFlush] = NtGdiFlush;
    KiServiceTable[SN_NtGdiGetDeviceCaps] = NtGdiGetDeviceCaps;
    KiServiceTable[SN_NtGdiGetCurrentDpiInfo] = NtGdiGetCurrentDpiInfo;
    KiServiceTable[SN_NtUserGetDCEx] = NtUserGetDCEx;
    KiServiceTable[SN_NtUserGetWindowDC] = NtUserGetDCEx;
}

UINT64 KiSystemServiceDispatch(UINT64 number, UINT64 *reg_args, UINT64 user_rsp)
{
    g_service_user_rsp = user_rsp;
    static UINT8 seen_services[NTOS_MAX_SYSCALL];
    if (number < NTOS_MAX_SYSCALL && number >= 0x1000 &&
        !seen_services[number]) {
        seen_services[number] = 1;
        PKTHREAD caller = KeGetCurrentThread();
        KeLog("[user] first win32u service 0x%lx (tid %lu)\n",
              (unsigned long)number,
              caller ? (unsigned long)caller->ThreadId : 0ul);
        /* The first win32k-range call means USER32 finished its process
         * attach, so its `gpsi` is set. Report where it points: SERVERINFO is
         * the page GetSystemMetrics and GetSysColor answer out of without ever
         * entering the kernel, so a wrong pointer here is invisible in every
         * other log. */
        static UINT8 gpsi_reported;
        if (!gpsi_reported) {
            gpsi_reported = 1;
            UINT64 user32 = LdrGetModuleBase("user32.dll");
            if (user32 && MmProbeForRead(user32 + USER32_GPSI_OFFSET, 8)) {
                UINT64 gpsi = *(volatile UINT64 *)(user32 +
                                                   USER32_GPSI_OFFSET);
                KeLog("[user] USER32 gpsi = %p (SERVERINFO is %p) -> %s\n",
                      (void *)gpsi, (void *)PROCESS_SERVER_INFO_VA,
                      gpsi == PROCESS_SERVER_INFO_VA ? "connected"
                                                     : "NOT ours");
                if (gpsi == PROCESS_SERVER_INFO_VA)
                    KeLog("[user]   screen from gpsi: %ldx%ld\n",
                          (long)*(volatile INT32 *)(gpsi + 0x758),
                          (long)*(volatile INT32 *)(gpsi + 0x75C));
            }
            /* GDI32 owns the client side of the handle table: it caches the
             * table pointer and the handle count in its own data, and every
             * handle check goes through them. If its initialization did not
             * publish them, nothing GDI does can succeed, so fill them in. */
            UINT64 gdi32 = LdrGetModuleBase("gdi32.dll");
            if (gdi32) {
                UINT64 table = LdrGetProcAddress(gdi32,
                                                 "pGdiSharedHandleTable");
                UINT64 count = LdrGetProcAddress(gdi32,
                                                 "gMaxGdiHandleCount");
                if (table && MmProbeForWrite(table, sizeof(UINT64))) {
                    if (*(volatile UINT64 *)table != PROCESS_GDI_SHARED_TABLE_VA)
                        *(UINT64 *)table = PROCESS_GDI_SHARED_TABLE_VA;
                }
                if (count && MmProbeForWrite(count, sizeof(UINT32))) {
                    if (*(volatile UINT32 *)count == 0)
                        *(UINT32 *)count = 0x10000;
                }
                /* Every GDI handle check ends with
                 *   (cell.ProcessId & ~1) == *(DWORD *)(gdi32 + 0x2E04C)
                 * -- gdi32's cached process id, read out of GetDeviceCaps. The
                 * cells win32k publishes carry this process's client id, so the
                 * cache has to agree or every DC is rejected before any syscall
                 * happens, which looks exactly like "GDI does nothing".
                 */
                UINT64 pid_cache = gdi32 + 0x2E04C;
                if (MmProbeForWrite(pid_cache, sizeof(UINT32))) {
                    UINT32 cached = *(volatile UINT32 *)pid_cache;
                    if (cached != PROCESS_CLIENT_ID) {
                        *(UINT32 *)pid_cache = PROCESS_CLIENT_ID;
                        KeLog("[user] GDI32 cached process id %lu -> %u\n",
                              (unsigned long)cached,
                              (unsigned)PROCESS_CLIENT_ID);
                    }
                }
                KeLog("[user] GDI32 pGdiSharedHandleTable=%p "
                      "gMaxGdiHandleCount=%lu\n",
                      table ? (void *)*(volatile UINT64 *)table : 0,
                      count ? (unsigned long)*(volatile UINT32 *)count : 0ul);
            }
        }
    }
    if (number >= NTOS_MAX_SYSCALL || KiServiceTable[number] == NULL) {
        /* Win32k-range services (win32u) that we do not implement yet return
         * the neutral failure value 0 (NULL handle / FALSE) instead of an
         * NTSTATUS, mirroring the loader's return-0 import stubs: real USER32
         * code paths treat an NTSTATUS here as a valid kernel GDI handle. */
        if (number >= 0x1000 && number < NTOS_MAX_SYSCALL) {
            static UINT8 seen_missing[NTOS_MAX_SYSCALL];
            if (!seen_missing[number]) {
                seen_missing[number] = 1;
                KeLog("[user] win32u service 0x%lx unimplemented -> 0\n",
                      (unsigned long)number);
            }
            return 0;
        }
        KeLog("[ke]   invalid system service 0x%lx\n", (unsigned long)number);
        return (UINT64)STATUS_NOT_IMPLEMENTED;
    }

    /* Build the full argument array: the four register arguments, then up to
     * twelve stack arguments read from the user stack above the ntdll stub's
     * return address (+0x28). NtUserCreateWindowEx uses fifteen arguments on
     * current x64 Windows. Each stack slot is probed, so a call that passes
     * fewer arguments (leaving RSP near the top of the stack) yields zeros
     * instead of faulting the kernel on an unmapped read. */
    UINT64 args[16];
    args[0] = reg_args[0];
    args[1] = reg_args[1];
    args[2] = reg_args[2];
    args[3] = reg_args[3];
    for (int i = 0; i < 12; i++) {
        UINT64 slot = user_rsp + 0x28 + (UINT64)i * 8;
        args[4 + i] = MmProbeForRead(slot, 8) ? *(volatile UINT64 *)slot : 0;
    }

    return KiServiceTable[number](args);
}
