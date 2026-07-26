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

/* Bump allocator for per-thread user stacks and TEBs. */
static UINT64 g_thread_va = 0x0000000030000000ULL;
#define THREAD_STACK_PAGES 16

void PsInitialize(void)
{
    g_event_type = ObCreateObjectType("Event", NULL);
    g_thread_type = ObCreateObjectType("Thread", NULL);
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
    return (UINT64)STATUS_SUCCESS;
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

    LONG previous = KeSetEvent((PKEVENT)obj);
    ObDereferenceObject(obj);
    if (prev_out && MmProbeForWrite((UINT64)prev_out, sizeof(LONG)))
        *prev_out = previous;
    return (UINT64)STATUS_SUCCESS;
}

/* ------------------------------------------------------------------ */
/* Waiting                                                            */
/* ------------------------------------------------------------------ */

/* NtWaitForSingleObject(HANDLE, BOOLEAN Alertable, PLARGE_INTEGER Timeout).
 * Alertable and Timeout are accepted but not yet honored (waits are infinite). */
UINT64 NtWaitForSingleObject(UINT64 *a)
{
    UINT64 handle = a[0];

    POBJECT obj;
    if (!NT_SUCCESS(ObReferenceObjectByHandle((HANDLE)(ULONG_PTR)handle, 0, NULL,
                                              &obj)))
        return (UINT64)STATUS_INVALID_HANDLE;

    /* Only events and threads are waitable, and both bodies start with a
     * DISPATCHER_HEADER. */
    POBJECT_TYPE type = ObHeaderFromObject(obj)->Type;
    if (type != g_event_type && type != g_thread_type) {
        ObDereferenceObject(obj);
        return (UINT64)STATUS_OBJECT_TYPE_MISMATCH;
    }

    NTSTATUS st = KeWaitForSingleObject((PDISPATCHER_HEADER)obj);
    ObDereferenceObject(obj);
    return (UINT64)st;
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
    UINT64 teb_va = map_user_pages(1);
    if (!stack_base || !teb_va)
        return (UINT64)STATUS_NO_MEMORY;
    UINT64 stack_top = stack_base + THREAD_STACK_PAGES * PAGE_SIZE;

    PTEB teb = (PTEB)teb_va;
    memset(teb, 0, sizeof(*teb));
    teb->NtTib.Self = (struct _NT_TIB *)teb_va;
    teb->NtTib.StackBase = (PVOID)stack_top;
    teb->NtTib.StackLimit = (PVOID)stack_base;
    teb->ProcessEnvironmentBlock = (PVOID)PROCESS_PEB_VA;

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
    KeLog("[ps]   NtCreateThreadEx(entry=%p) -> handle %p\n", (void *)entry, h);
    return (UINT64)STATUS_SUCCESS;
}
