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
#include <ntos/cm.h>
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

static UINT32 g_next_registered_message = 0xC000;

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
} USER_MSG_QUEUE;

static USER_MSG_QUEUE g_user_thread_queues[USER_MAX_THREAD_QUEUES];

static USER_MSG_QUEUE *KiUserQueueForThread(UINT32 thread_id)
{
    if (!thread_id || thread_id > USER_MAX_THREAD_QUEUES)
        return NULL;
    return &g_user_thread_queues[thread_id - 1];
}

/* Owner thread of each created window slot: input goes to the queue of the
 * thread that created the window, as in win32k. */
static UINT32 g_window_owner[256];

static UINT32 KiUserWindowOwner(UINT64 hwnd)
{
    UINT16 slot = (UINT16)(hwnd & 0xFFFF);
    if (!slot || slot >= 256)
        return 0;
    return g_window_owner[slot];
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

static BOOLEAN KiUserTakeMessage(USER_MSG_LOCAL *out, BOOLEAN remove)
{
    PKTHREAD thread = KeGetCurrentThread();
    USER_MSG_QUEUE *queue =
        KiUserQueueForThread(thread ? thread->ThreadId : 0);
    if (!queue)
        return FALSE;
    UINT64 flags = KiIrqSave();
    if (queue->Tail == queue->Head) {
        KiIrqRestore(flags);
        return FALSE;
    }
    *out = queue->Msgs[queue->Tail];
    if (remove)
        queue->Tail = (queue->Tail + 1) % USER_MESSAGE_QUEUE_CAPACITY;
    KiIrqRestore(flags);
    return TRUE;
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
    /* Registered window-message IDs occupy 0xC000..0xFFFF.  Keep allocation
     * stable for the process; string interning will be added with the USER
     * atom table. */
    UINT32 atom = g_next_registered_message;
    if (g_next_registered_message < 0xFFFF)
        g_next_registered_message++;
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
    KeLog("[user] NtUserPostThreadMessage(tid=%u, msg=0x%x, wp=%p, lp=%p)\n",
          thread_id, message, (void *)a[2], (void *)a[3]);

    /* The message is delivered to the target thread's own queue; only that
     * thread's GetMessage/PeekMessage can consume it, preserving per-thread
     * message semantics for explorer's desktop hand-off. */
    return KiUserEnqueueThreadMessage(thread_id, message, a[2],
                                      (INT64)a[3]);
}

static UINT64 NtUserCallOneParam(UINT64 *a)
{
    KeLog("[user] NtUserCallOneParam(value=%p, routine=0x%lx)\n",
          (void *)a[0], (unsigned long)a[1]);
    return 0;
}

static UINT64 NtUserGetDC(UINT64 *a)
{
    /* No visible/user DC has been created by win32k yet. Returning NULL is a
     * valid failed GetDC result; returning STATUS_NOT_IMPLEMENTED here is not,
     * because GDI32 would interpret it as a kernel GDI handle. */
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
    UINT16 atom = (UINT16)a[0];
    UINT16 *buffer = (UINT16 *)a[1];
    UINT32 capacity = (UINT32)a[2];
    KeLog("[user] NtUserGetAtomName(atom=0x%x, out=%p, cap=%u)\n",
          atom, buffer, capacity);
    if (buffer && capacity && MmProbeForWrite((UINT64)buffer, sizeof(UINT16)))
        buffer[0] = 0;
    return 0;
}

/* ------------------------------------------------------------------ */
/* Window classes and the USER atom table                              */
/* ------------------------------------------------------------------ */

#define USER_MAX_CLASSES 64
#define USER_ATOM_FIRST  0xC000

typedef struct _USER_CLASS_LOCAL {
    WCHAR Name[32];
    UINT64 WndProc;
    UINT64 Instance;
    UINT32 Style;
    UINT16 Atom;
    BOOLEAN Used;
} USER_CLASS_LOCAL;

static USER_CLASS_LOCAL g_user_classes[USER_MAX_CLASSES];

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
        g_user_classes[i].WndProc =
            *(const UINT64 *)(wcex + WNDCLASSEX_WNDPROC);
        g_user_classes[i].Instance =
            *(const UINT64 *)(wcex + WNDCLASSEX_HINSTANCE);
        g_user_classes[i].Atom = (UINT16)(USER_ATOM_FIRST + i);
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

static INT64 KiGetWindowLong(UINT64 hwnd, INT32 index)
{
    UINT16 slot = (UINT16)(hwnd & 0xFFFF);
    if (!slot || slot >= 256 || index < -WINDOW_LONG_COUNT)
        return 0;
    INT64 *longs = (INT64 *)(PROCESS_USER_OBJECT_ARENA_VA +
                             (UINT64)slot * 0x100 + WINDOW_LONG_BASE +
                             (UINT64)(-index - 1) * 8);
    if (!MmProbeForRead((UINT64)longs, 8))
        return 0;
    return *longs;
}

static BOOLEAN KiSetWindowLong(UINT64 hwnd, INT32 index, INT64 value,
                               INT64 *old)
{
    UINT16 slot = (UINT16)(hwnd & 0xFFFF);
    if (!slot || slot >= 256 || index < -WINDOW_LONG_COUNT)
        return FALSE;
    INT64 *longs = (INT64 *)(PROCESS_USER_OBJECT_ARENA_VA +
                             (UINT64)slot * 0x100 + WINDOW_LONG_BASE +
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

static UINT64 NtUserShowWindow(UINT64 *a) { return 1; }
static UINT64 NtUserSetWindowPos(UINT64 *a) { return 1; }
static UINT64 NtUserMoveWindow(UINT64 *a) { return 1; }
static UINT64 NtUserInvalidateRect(UINT64 *a) { return 1; }
static UINT64 NtUserUpdateWindow(UINT64 *a) { return 1; }
static UINT64 NtUserPostMessage(UINT64 *a)
{
    return KiUserEnqueueMessage(a[0], (UINT32)a[1], a[2], (INT64)a[3],
                                0, 0);
}
static UINT64 NtUserMessageCall(UINT64 *a) { return 0; }
static UINT64 NtUserDispatchMessage(UINT64 *a) { return 0; }
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
static UINT64 NtUserBeginPaint(UINT64 *a)
{
    UINT8 *ps = (UINT8 *)a[1];
    if (ps && MmProbeForWrite((UINT64)ps, 104))
        memset(ps, 0, 104);
    return 0;
}
static UINT64 NtUserEndPaint(UINT64 *a) { return 1; }

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

    if (next_window_index >= 256)
        return 0;

    UINT16 index = next_window_index++;
    UINT16 generation = 1;
    UINT64 hwnd = ((UINT64)generation << 16) | index;
    USER_HANDLE_ENTRY_LOCAL *entries =
        (USER_HANDLE_ENTRY_LOCAL *)PROCESS_USER_SHARED_TABLE_VA;
    USER_WINDOW_HEAD_LOCAL *window = (USER_WINDOW_HEAD_LOCAL *)(
        PROCESS_USER_OBJECT_ARENA_VA + (UINT64)index * 0x100);

    memset(window, 0, 0x100);
    window->Handle = hwnd;
    window->LockCount = 1;
    window->Self = (UINT64)window;

    entries[index].Object = (UINT64)window;
    entries[index].Owner = 0;
    entries[index].Type = 1; /* TYPE_WINDOW */
    entries[index].Flags = 0;
    entries[index].Generation = generation;

    PKTHREAD creator = KeGetCurrentThread();
    g_window_owner[index] = creator ? creator->ThreadId : 0;

    /* Do not focus Explorer's zero-sized hidden coordination window. Hardware
     * input remains a thread message until USER creates a visible top-level
     * window and establishes foreground/focus state. */
    if (((UINT32)a[4] & 0x10000000U) && (INT32)a[7] > 0 && (INT32)a[8] > 0)
        g_user_active_hwnd = hwnd;

    KeLog("[user]   created HWND %p, shared object %p\n",
          (void *)hwnd, (void *)window);
    return hwnd;
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
    if (slot && slot < 256) {
        g_window_owner[slot] = 0;
        if (g_user_active_hwnd == a[0])
            g_user_active_hwnd = 0;
    }
    return 0;
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
    while (!KiUserTakeMessage(&msg, TRUE))
        KeYield();
    *user_msg = msg;
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
    KiServiceTable[SN_NtUserCallNoParam] = NtUserCallOneParam;
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
    KiServiceTable[SN_NtUserCallTwoParam] = NtUserCallTwoParam;
    KiServiceTable[SN_NtUserSetWindowLong] = NtUserSetWindowLong;
    KiServiceTable[SN_NtUserSetWindowLongPtr] = NtUserSetWindowLong;
    KiServiceTable[SN_NtUserChangeWindowMessageFilterEx] =
        NtUserChangeWindowMessageFilterEx;
}

UINT64 KiSystemServiceDispatch(UINT64 number, UINT64 *reg_args, UINT64 user_rsp)
{
    g_service_user_rsp = user_rsp;
    static UINT8 seen_services[NTOS_MAX_SYSCALL];
    if (number < NTOS_MAX_SYSCALL && number >= 0x1000 &&
        !seen_services[number]) {
        seen_services[number] = 1;
        KeLog("[user] first win32u service 0x%lx\n", (unsigned long)number);
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
