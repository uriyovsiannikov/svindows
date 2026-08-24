/*
 * ps/psobj.c - Event and Thread objects, and the Nt* services that create,
 * signal, wait on, and spawn them.
 *
 * Event and Thread objects both begin their body with a DISPATCHER_HEADER, so
 * NtWaitForSingleObject can wait on either uniformly. Creating a thread here
 * spins up a new ring-3 thread in the current process (shared address space and
 * PEB) with its own stack and TEB, wrapped in a waitable Thread object that is
 * signaled when the thread exits.
 */
#include <ntos/ps.h>
#include <ntos/ob.h>
#include <ntos/ke.h>
#include <ntos/mm.h>
#include <ntos/rtl.h>
#include <nt/ntstatus.h>
#include <nt/ntobject.h>
#include <nt/peb.h>

typedef struct _THREAD_OBJECT {
    DISPATCHER_HEADER Header;   /* signaled on thread exit */
    PKTHREAD          Thread;
} THREAD_OBJECT;

static POBJECT_TYPE g_event_type;
static POBJECT_TYPE g_thread_type;
static POBJECT_TYPE g_semaphore_type;

/* Bump allocator for per-thread user stacks and TEBs. */
static UINT64 g_thread_va = 0x0000000030000000ULL;
#define THREAD_STACK_PAGES 16

void PsInitialize(void)
{
    g_event_type = ObCreateObjectType("Event", NULL);
    g_thread_type = ObCreateObjectType("Thread", NULL);
    g_semaphore_type = ObCreateObjectType("Semaphore", NULL);
}

/* ------------------------------------------------------------------ */
/* Events                                                             */
/* ------------------------------------------------------------------ */

/* NtCreateEvent(PHANDLE, ACCESS_MASK, POBJECT_ATTRIBUTES, EVENT_TYPE, InitialState).
 * EVENT_TYPE 0 = notification (manual reset), 1 = synchronization (auto). */
UINT64 NtCreateEvent(UINT64 *a)
{
    PHANDLE out = (PHANDLE)a[0];
    UINT64 event_type = a[3], initial = a[4];
    if (!MmProbeForWrite((UINT64)out, sizeof(HANDLE)))
        return (UINT64)STATUS_ACCESS_VIOLATION;

    POBJECT obj;
    if (!NT_SUCCESS(ObCreateObject(g_event_type, sizeof(KEVENT), &obj)))
        return (UINT64)STATUS_NO_MEMORY;
    KeInitializeEvent((PKEVENT)obj, (BOOLEAN)(event_type == 0), (BOOLEAN)initial);

    HANDLE h;
    if (!NT_SUCCESS(ObCreateHandle(obj, GENERIC_ALL, &h))) {
        ObDereferenceObject(obj);
        return (UINT64)STATUS_NO_MEMORY;
    }
    ObDereferenceObject(obj); /* the handle keeps it alive */
    *out = h;
    KeLog("[ps]   NtCreateEvent -> handle %p (thread %u)\n", (void *)h,
          KeGetCurrentThread() ? KeGetCurrentThread()->ThreadId : 0);
    return (UINT64)STATUS_SUCCESS;
}

/*
 * Kernel-owned notification event with a ring-3 handle. USER needs one per GUI
 * thread: MsgWaitForMultipleObjects waits on the thread's "input event", which
 * win32k signals when a message lands in that thread's queue. The kernel keeps
 * the object pointer so it can signal without a handle lookup.
 */
NTSTATUS PsCreateNotificationEvent(PKEVENT *event_out, HANDLE *handle_out)
{
    POBJECT obj;
    if (!NT_SUCCESS(ObCreateObject(g_event_type, sizeof(KEVENT), &obj)))
        return STATUS_NO_MEMORY;
    KeInitializeEvent((PKEVENT)obj, TRUE /* notification */, FALSE);

    HANDLE h;
    if (!NT_SUCCESS(ObCreateHandle(obj, GENERIC_ALL, &h))) {
        ObDereferenceObject(obj);
        return STATUS_NO_MEMORY;
    }
    /* The handle holds the only reference; it is never closed (the event lives
     * as long as the thread's queue does). */
    ObDereferenceObject(obj);
    *event_out = (PKEVENT)obj;
    *handle_out = h;
    return STATUS_SUCCESS;
}

/* NtSetEvent(HANDLE, PLONG PreviousState). */
UINT64 NtSetEvent(UINT64 *a)
{
    UINT64 handle = a[0];
    LONG  *prev_out = (LONG *)a[1];

    POBJECT obj;
    if (!NT_SUCCESS(ObReferenceObjectByHandle((HANDLE)(ULONG_PTR)handle, 0,
                                              g_event_type, &obj)))
        return (UINT64)STATUS_INVALID_HANDLE;

    KeLog("[ps]   NtSetEvent(handle %p, thread %u)\n", (void *)handle,
          KeGetCurrentThread() ? KeGetCurrentThread()->ThreadId : 0);
    LONG previous = KeSetEvent((PKEVENT)obj);
    ObDereferenceObject(obj);
    if (prev_out && MmProbeForWrite((UINT64)prev_out, sizeof(LONG)))
        *prev_out = previous;
    return (UINT64)STATUS_SUCCESS;
}

UINT64 NtResetEvent(UINT64 *a)
{
    POBJECT obj;
    if (!NT_SUCCESS(ObReferenceObjectByHandle((HANDLE)(ULONG_PTR)a[0], 0,
                                              g_event_type, &obj)))
        return (UINT64)STATUS_INVALID_HANDLE;
    PKEVENT event = (PKEVENT)obj;
    LONG previous = event->Header.SignalState;
    KeResetEvent(event);
    LONG *previous_out = (LONG *)a[1];
    if (previous_out && MmProbeForWrite((UINT64)previous_out, sizeof(LONG)))
        *previous_out = previous;
    ObDereferenceObject(obj);
    return (UINT64)STATUS_SUCCESS;
}

/* ------------------------------------------------------------------ */
/* Semaphores                                                         */
/* ------------------------------------------------------------------ */

UINT64 NtCreateSemaphore(UINT64 *a)
{
    PHANDLE out = (PHANDLE)a[0];
    LONG initial = (LONG)a[3];
    LONG limit = (LONG)a[4];
    if (!MmProbeForWrite((UINT64)out, sizeof(HANDLE)))
        return (UINT64)STATUS_ACCESS_VIOLATION;
    if (limit <= 0 || initial < 0 || initial > limit)
        return (UINT64)STATUS_INVALID_PARAMETER;

    POBJECT obj;
    if (!NT_SUCCESS(ObCreateObject(g_semaphore_type, sizeof(KSEMAPHORE), &obj)))
        return (UINT64)STATUS_NO_MEMORY;
    KeInitializeSemaphore((PKSEMAPHORE)obj, initial, limit);

    HANDLE handle;
    NTSTATUS status = ObCreateHandle(obj, GENERIC_ALL, &handle);
    ObDereferenceObject(obj);
    if (!NT_SUCCESS(status))
        return (UINT64)status;
    *out = handle;
    return (UINT64)STATUS_SUCCESS;
}

UINT64 NtReleaseSemaphore(UINT64 *a)
{
    LONG release = (LONG)a[1];
    if (release <= 0)
        return (UINT64)STATUS_INVALID_PARAMETER;

    POBJECT obj;
    if (!NT_SUCCESS(ObReferenceObjectByHandle((HANDLE)(ULONG_PTR)a[0], 0,
                                              g_semaphore_type, &obj)))
        return (UINT64)STATUS_INVALID_HANDLE;
    PKSEMAPHORE semaphore = (PKSEMAPHORE)obj;
    if (semaphore->Header.SignalState > semaphore->Limit - release) {
        ObDereferenceObject(obj);
        return (UINT64)STATUS_INVALID_PARAMETER;
    }
    LONG previous = KeReleaseSemaphore(semaphore, release);
    LONG *previous_out = (LONG *)a[2];
    if (previous_out && MmProbeForWrite((UINT64)previous_out, sizeof(LONG)))
        *previous_out = previous;
    ObDereferenceObject(obj);
    return (UINT64)STATUS_SUCCESS;
}

/* ------------------------------------------------------------------ */
/* Waiting                                                            */
/* ------------------------------------------------------------------ */

static UINT64 timeout_to_ticks(const INT64 *timeout)
{
    if (!timeout)
        return ~(UINT64)0; /* infinite */
    if (!MmProbeForRead((UINT64)timeout, sizeof(*timeout)))
        return ~(UINT64)0;

    INT64 value = *timeout;
    if (value == 0)
        return 0;
    if (value < 0) {
        UINT64 units = (UINT64)(-(value + 1)) + 1; /* handles INT64_MIN */
        return (units + 99999ULL) / 100000ULL;     /* 100 ns -> 10 ms */
    }

    /* Absolute deadlines use the same since-boot 100 ns clock currently
     * exposed through KUSER_SHARED_DATA. */
    UINT64 now = KeGetTickCount() * 100000ULL;
    if ((UINT64)value <= now)
        return 0;
    return ((UINT64)value - now + 99999ULL) / 100000ULL;
}

static NTSTATUS reference_waitable(HANDLE handle, POBJECT *object,
                                   PDISPATCHER_HEADER *header)
{
    POBJECT obj;
    NTSTATUS status = ObReferenceObjectByHandle(handle, 0, NULL, &obj);
    if (!NT_SUCCESS(status))
        return STATUS_INVALID_HANDLE;

    POBJECT_TYPE type = ObHeaderFromObject(obj)->Type;
    if (type != g_event_type && type != g_thread_type &&
        type != g_semaphore_type) {
        ObDereferenceObject(obj);
        return STATUS_OBJECT_TYPE_MISMATCH;
    }
    *object = obj;
    *header = (PDISPATCHER_HEADER)obj;
    return STATUS_SUCCESS;
}

/* NtWaitForSingleObject(HANDLE, BOOLEAN Alertable, PLARGE_INTEGER Timeout). */
UINT64 NtWaitForSingleObject(UINT64 *a)
{
    (void)a[1]; /* alertable APC delivery is not implemented yet */
    POBJECT obj;
    PDISPATCHER_HEADER header;
    NTSTATUS st = reference_waitable((HANDLE)(ULONG_PTR)a[0], &obj, &header);
    if (!NT_SUCCESS(st))
        return (UINT64)st;

    /* Diagnostics: an indefinite wait is the signature of a stuck user
     * thread; report each begin/end pair. */
    BOOLEAN indefinite = !a[2];
    UINT64 tid = 0;
    PKTHREAD thread = KeGetCurrentThread();
    if (thread)
        tid = thread->ThreadId;
    if (indefinite) {
        static UINT64 logged[64];
        static ULONG logged_count;
        BOOLEAN seen = FALSE;
        for (ULONG i = 0; i < logged_count; i++)
            if (logged[i] == (tid << 32 | (UINT32)(ULONG_PTR)a[0]))
                seen = TRUE;
        if (!seen && logged_count < 64) {
            logged[logged_count++] = tid << 32 | (UINT32)(ULONG_PTR)a[0];
            KeLog("[ps]   thread %llu begins indefinite wait on handle %p\n",
                  (unsigned long long)tid, (void *)(ULONG_PTR)a[0]);
        }
    }

    st = KeWaitForSingleObjectTimeout(header,
                                      timeout_to_ticks((const INT64 *)a[2]));
    if (indefinite)
        KeLog("[ps]   thread %llu resumed from indefinite wait on %p "
              "(status 0x%08lx)\n", (unsigned long long)tid,
              (void *)(ULONG_PTR)a[0], (unsigned long)st);
    ObDereferenceObject(obj);
    return (UINT64)st;
}

/* NTOS-private service used by kernel32 until the native system-service table
 * is switched to a specific Windows build's NtWaitForMultipleObjects number.
 * Arguments mirror the native routine closely: count, handle array, WaitType
 * (0=all, 1=any), alertable, timeout. */
UINT64 NtWaitForMultipleObjects(UINT64 *a)
{
    ULONG count = (ULONG)a[0];
    HANDLE *handles = (HANDLE *)a[1];
    ULONG wait_type = (ULONG)a[2];
    (void)a[3]; /* alertable */
    const INT64 *timeout = (const INT64 *)a[4];

    if (!count || count > KE_MAXIMUM_WAIT_OBJECTS || wait_type > 1)
        return (UINT64)STATUS_INVALID_PARAMETER;
    if (!MmProbeForRead((UINT64)handles, count * sizeof(HANDLE)))
        return (UINT64)STATUS_ACCESS_VIOLATION;

    POBJECT objects[KE_MAXIMUM_WAIT_OBJECTS];
    PDISPATCHER_HEADER headers[KE_MAXIMUM_WAIT_OBJECTS];
    ULONG referenced = 0;
    NTSTATUS status = STATUS_SUCCESS;
    for (; referenced < count; referenced++) {
        status = reference_waitable(handles[referenced], &objects[referenced],
                                    &headers[referenced]);
        if (!NT_SUCCESS(status))
            break;
    }

    if (NT_SUCCESS(status)) {
        /* Same diagnostic as the single-object wait: a blocking multi-object
         * wait is where a stuck GUI thread ends up (MsgWaitForMultipleObjects
         * waits on the caller's handles plus the USER input event), so report
         * the handle set once per (thread, count, first handle). */
        UINT64 tid = 0;
        PKTHREAD thread = KeGetCurrentThread();
        if (thread)
            tid = thread->ThreadId;
        static UINT64 logged[64];
        static ULONG logged_count;
        UINT64 key = tid << 40 | (UINT64)count << 32 |
                     (UINT32)(ULONG_PTR)handles[0];
        BOOLEAN seen = FALSE;
        for (ULONG i = 0; i < logged_count; i++)
            if (logged[i] == key)
                seen = TRUE;
        if (!seen && logged_count < 64) {
            logged[logged_count++] = key;
            KeLog("[ps]   thread %llu waits on %lu object(s) (%s, %s): "
                  "%p %p %p %p\n", (unsigned long long)tid,
                  (unsigned long)count, wait_type == 0 ? "all" : "any",
                  timeout ? "timed" : "indefinite",
                  (void *)(ULONG_PTR)handles[0],
                  (void *)(count > 1 ? (ULONG_PTR)handles[1] : 0),
                  (void *)(count > 2 ? (ULONG_PTR)handles[2] : 0),
                  (void *)(count > 3 ? (ULONG_PTR)handles[3] : 0));
        }
        status = KeWaitForMultipleObjects(count, headers,
                                          (BOOLEAN)(wait_type == 0),
                                          timeout_to_ticks(timeout));
    }
    while (referenced)
        ObDereferenceObject(objects[--referenced]);
    return (UINT64)status;
}

/* NtQueryInformationProcess: the first class required by real SHCORE during
 * DLL initialization is ProcessBasicInformation (0). */
UINT64 NtQueryInformationProcess(UINT64 *a)
{
    typedef struct _PROCESS_BASIC_INFORMATION_LOCAL {
        NTSTATUS ExitStatus;
        UINT32 Padding;
        PVOID PebBaseAddress;
        UINT64 AffinityMask;
        LONG BasePriority;
        UINT32 Padding2;
        UINT64 UniqueProcessId;
        UINT64 InheritedFromUniqueProcessId;
    } PROCESS_BASIC_INFORMATION_LOCAL;

    ULONG info_class = (ULONG)a[1];
    void *buffer = (void *)a[2];
    ULONG length = (ULONG)a[3];
    ULONG *return_length = (ULONG *)a[4];
    if (info_class != 0)
        return (UINT64)STATUS_INVALID_INFO_CLASS;
    if (return_length && MmProbeForWrite((UINT64)return_length, sizeof(ULONG)))
        *return_length = sizeof(PROCESS_BASIC_INFORMATION_LOCAL);
    if (length < sizeof(PROCESS_BASIC_INFORMATION_LOCAL))
        return (UINT64)STATUS_INFO_LENGTH_MISMATCH;
    if (!MmProbeForWrite((UINT64)buffer,
                         sizeof(PROCESS_BASIC_INFORMATION_LOCAL)))
        return (UINT64)STATUS_ACCESS_VIOLATION;

    PROCESS_BASIC_INFORMATION_LOCAL *info = buffer;
    memset(info, 0, sizeof(*info));
    info->ExitStatus = STATUS_PENDING;
    info->PebBaseAddress = (PVOID)PROCESS_PEB_VA;
    info->AffinityMask = 1;
    info->BasePriority = 8;
    info->UniqueProcessId = 1;
    return (UINT64)STATUS_SUCCESS;
}

/* ------------------------------------------------------------------ */
/* Thread creation                                                    */
/* ------------------------------------------------------------------ */

static UINT64 map_user_pages(SIZE_T pages)
{
    UINT64 base = g_thread_va;
    for (SIZE_T i = 0; i < pages; i++) {
        UINT64 pa = MmAllocatePage();
        if (pa == MM_INVALID_PHYS)
            return 0;
        MmMapPage(base + i * PAGE_SIZE, pa, PTE_USER | PTE_WRITE);
    }
    g_thread_va += pages * PAGE_SIZE;
    return base;
}

/*
 * NtCreateThreadEx(PHANDLE ThreadHandle, ACCESS_MASK, POBJECT_ATTRIBUTES,
 *   HANDLE ProcessHandle, PVOID StartRoutine, PVOID Argument, ULONG CreateFlags,
 *   SIZE_T ZeroBits, SIZE_T StackSize, SIZE_T MaximumStackSize, PVOID AttrList)
 * The modern (Vista+) thread-creation service kernel32's CreateThread uses.
 */
UINT64 NtCreateThreadEx(UINT64 *a)
{
    PHANDLE out = (PHANDLE)a[0];
    UINT64 entry = a[4], arg = a[5];
    if (!MmProbeForWrite((UINT64)out, sizeof(HANDLE)))
        return (UINT64)STATUS_ACCESS_VIOLATION;

    /* Stack + TEB for the new thread. */
    UINT64 stack_base = map_user_pages(THREAD_STACK_PAGES);
    UINT64 teb_va = map_user_pages(PROCESS_TEB_SIZE / PAGE_SIZE);
    if (!stack_base || !teb_va)
        return (UINT64)STATUS_NO_MEMORY;
    UINT64 stack_top = stack_base + THREAD_STACK_PAGES * PAGE_SIZE;

    PTEB teb = (PTEB)teb_va;
    memset(teb, 0, PROCESS_TEB_SIZE);
    /* USER32 keeps its per-thread client callback/cache block in the extended
     * TEB starting at 0x800. The kernel normally initializes this as a thread
     * joins win32k. Until that path is split out, inherit the process template
     * established in the main TEB during USER32 process attach. */
    memcpy((UINT8 *)teb + 0x800,
           (const UINT8 *)PROCESS_MAIN_TEB_VA + 0x800,
           PROCESS_TEB_SIZE - 0x800);
    teb->NtTib.Self = (struct _NT_TIB *)teb_va;
    teb->NtTib.StackBase = (PVOID)stack_top;
    teb->NtTib.StackLimit = (PVOID)stack_base;
    teb->ProcessEnvironmentBlock = (PVOID)PROCESS_PEB_VA;
    teb->ClientId.UniqueProcess = (HANDLE)(ULONG_PTR)1;

    /* Waitable thread object (signaled on exit). */
    POBJECT obj;
    if (!NT_SUCCESS(ObCreateObject(g_thread_type, sizeof(THREAD_OBJECT), &obj)))
        return (UINT64)STATUS_NO_MEMORY;
    THREAD_OBJECT *to = (THREAD_OBJECT *)obj;
    to->Header.Type = ThreadObject;
    to->Header.SignalState = 0;
    InitializeListHead(&to->Header.WaitListHead);

    /* Interrupts are masked during a syscall, so the new thread cannot run
     * before we finish wiring it up. */
    PKTHREAD kt = KeCreateUserThread("userthread", entry, stack_top, teb_va, arg,
                                     8);
    if (!kt) {
        ObDereferenceObject(obj);
        return (UINT64)STATUS_NO_MEMORY;
    }
    to->Thread = kt;
    kt->TerminationObject = &to->Header;

    HANDLE h;
    if (!NT_SUCCESS(ObCreateHandle(obj, GENERIC_ALL, &h))) {
        ObDereferenceObject(obj);
        return (UINT64)STATUS_NO_MEMORY;
    }
    ObDereferenceObject(obj); /* the handle keeps it alive */

    *out = h;
    UINT64 user_start = 0;
    if (arg && MmProbeForRead(arg, sizeof(UINT64)))
        user_start = *(const UINT64 *)arg; /* kernel32 THREAD_INFO.Start */
    KeLog("[ps]   NtCreateThreadEx(entry=%p, start=%p) -> handle %p\n",
          (void *)entry, (void *)user_start, h);

    /* A newly-created Windows thread is eligible to run before
     * NtCreateThreadEx returns. SHCore relies on that scheduling point when it
     * hands a worker a short-lived stack context and waits for the worker to
     * copy it. Let the child execute now instead of allowing the creator to
     * reuse that stack storage first. */
    KeYield();
    return (UINT64)STATUS_SUCCESS;
}
