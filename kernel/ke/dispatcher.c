/*
 * ke/dispatcher.c - waitable (dispatcher) objects and the wait machinery.
 *
 * Events, semaphores, mutants, and threads all embed a DISPATCHER_HEADER. A
 * thread waits by adding its wait block to the header's list and blocking; a
 * signal wakes every waiter to re-test the object (a simple, correct model: for
 * an auto-reset event or a semaphore, only as many waiters as there are signals
 * succeed, and the rest block again).
 *
 * Single-CPU: short critical sections just disable interrupts.
 */
#include <ntos/ke.h>
#include <ntos/rtl.h>
#include <nt/ntstatus.h>

/* Try to satisfy a wait without blocking, consuming the signal as appropriate. */
static BOOLEAN KiCanAcquire(PDISPATCHER_HEADER h)
{
    switch (h->Type) {
    case EventNotificationObject:
    case ThreadObject:
    case EventSynchronizationObject:
    case SemaphoreObject:
    case MutantObject:
        return (BOOLEAN)(h->SignalState > 0);
    }
    return FALSE;
}

static void KiConsumeSignal(PDISPATCHER_HEADER h)
{
    switch (h->Type) {
    case EventNotificationObject:
    case ThreadObject:
        break; /* level-triggered, never consumed */
    case EventSynchronizationObject:
        h->SignalState = 0;
        break;
    case SemaphoreObject:
    case MutantObject:
        h->SignalState--;
        break;
    }
}

/* Remove every outstanding wait block owned by a thread. Each entry is made a
 * self-list when detached so cancellation and signaling can safely race on the
 * single CPU without removing the same entry twice. */
static void KiUnlinkThreadWait(PKTHREAD thread)
{
    for (ULONG i = 0; i < thread->WaitBlockCount; i++) {
        PLIST_ENTRY entry = &thread->WaitBlocks[i].WaitListEntry;
        if (entry->Flink != entry) {
            RemoveEntryList(entry);
            InitializeListHead(entry);
        }
    }
}

static void KiWakeWaitingThread(PKTHREAD thread, NTSTATUS status)
{
    if (thread->State != ThreadStateWaiting)
        return;
    KiUnlinkThreadWait(thread);
    thread->WaitStatus = status;
    thread->WaitDeadline = 0;
    KiReadyThread(thread);
}

/* Wake every thread waiting on this header so it can re-test the object. */
void KiSignalObject(PDISPATCHER_HEADER h)
{
    while (!IsListEmpty(&h->WaitListHead)) {
        PLIST_ENTRY e = RemoveHeadList(&h->WaitListHead);
        InitializeListHead(e);
        PKWAIT_BLOCK wb = CONTAINING_RECORD(e, KWAIT_BLOCK, WaitListEntry);
        KiWakeWaitingThread(wb->Thread, STATUS_SUCCESS);
    }
}

void KiTimeoutThreadWait(PKTHREAD thread)
{
    KiWakeWaitingThread(thread, STATUS_TIMEOUT);
}

NTSTATUS KeWaitForMultipleObjects(ULONG count, PDISPATCHER_HEADER *objects,
                                  BOOLEAN wait_all, UINT64 timeout_ticks)
{
    if (!count || count > KE_MAXIMUM_WAIT_OBJECTS || !objects)
        return STATUS_INVALID_PARAMETER;

    UINT64 flags = KiIrqSave();
    PKTHREAD thread = KeGetCurrentThread();
    UINT64 deadline = timeout_ticks == ~(UINT64)0
                          ? 0 : KeGetTickCount() + timeout_ticks;

    for (;;) {
        if (wait_all) {
            BOOLEAN ready = TRUE;
            for (ULONG i = 0; i < count; i++)
                if (!KiCanAcquire(objects[i])) {
                    ready = FALSE;
                    break;
                }
            if (ready) {
                for (ULONG i = 0; i < count; i++)
                    KiConsumeSignal(objects[i]);
                KiIrqRestore(flags);
                return STATUS_WAIT_0;
            }
        } else {
            for (ULONG i = 0; i < count; i++) {
                if (KiCanAcquire(objects[i])) {
                    KiConsumeSignal(objects[i]);
                    KiIrqRestore(flags);
                    return (NTSTATUS)(STATUS_WAIT_0 + i);
                }
            }
        }

        if (timeout_ticks == 0 || (deadline && KeGetTickCount() >= deadline)) {
            KiIrqRestore(flags);
            return STATUS_TIMEOUT;
        }

        thread->WaitBlockCount = count;
        thread->WaitStatus = STATUS_PENDING;
        thread->WaitDeadline = deadline;
        for (ULONG i = 0; i < count; i++) {
            PKWAIT_BLOCK block = &thread->WaitBlocks[i];
            InitializeListHead(&block->WaitListEntry);
            block->Thread = thread;
            block->Object = objects[i];
            InsertTailList(&objects[i]->WaitListHead, &block->WaitListEntry);
        }
        thread->State = ThreadStateWaiting;
        KiBlockCurrentThread(); /* reschedule; returns once signalled */

        /* Signaling and timeout paths normally unlink all blocks before making
         * us ready. This is also safe as defensive cleanup. */
        KiUnlinkThreadWait(thread);
        if (thread->WaitStatus == STATUS_TIMEOUT) {
            thread->WaitBlockCount = 0;
            KiIrqRestore(flags);
            return STATUS_TIMEOUT;
        }
        thread->WaitBlockCount = 0;
    }
}

NTSTATUS KeWaitForSingleObjectTimeout(PDISPATCHER_HEADER h,
                                      UINT64 timeout_ticks)
{
    PDISPATCHER_HEADER objects[1] = { h };
    return KeWaitForMultipleObjects(1, objects, FALSE, timeout_ticks);
}

NTSTATUS KeWaitForSingleObject(PDISPATCHER_HEADER h)
{
    return KeWaitForSingleObjectTimeout(h, ~(UINT64)0);
}

/* ------------------------------------------------------------------ */
/* Events                                                             */
/* ------------------------------------------------------------------ */

void KeInitializeEvent(PKEVENT event, BOOLEAN notification, BOOLEAN signaled)
{
    event->Header.Type = notification ? EventNotificationObject
                                      : EventSynchronizationObject;
    event->Header.SignalState = signaled ? 1 : 0;
    InitializeListHead(&event->Header.WaitListHead);
}

LONG KeSetEvent(PKEVENT event)
{
    UINT64 flags = KiIrqSave();
    LONG previous = event->Header.SignalState;
    event->Header.SignalState = 1;
    KiSignalObject(&event->Header);
    KiIrqRestore(flags);
    return previous;
}

void KeResetEvent(PKEVENT event)
{
    UINT64 flags = KiIrqSave();
    event->Header.SignalState = 0;
    KiIrqRestore(flags);
}

/* ------------------------------------------------------------------ */
/* Semaphores                                                         */
/* ------------------------------------------------------------------ */

void KeInitializeSemaphore(PKSEMAPHORE sem, LONG initial, LONG limit)
{
    sem->Header.Type = SemaphoreObject;
    sem->Header.SignalState = initial;
    sem->Limit = limit;
    InitializeListHead(&sem->Header.WaitListHead);
}

LONG KeReleaseSemaphore(PKSEMAPHORE sem, LONG count)
{
    UINT64 flags = KiIrqSave();
    LONG previous = sem->Header.SignalState;
    sem->Header.SignalState += count;
    if (sem->Limit && sem->Header.SignalState > sem->Limit)
        sem->Header.SignalState = sem->Limit;
    KiSignalObject(&sem->Header);
    KiIrqRestore(flags);
    return previous;
}

/* ------------------------------------------------------------------ */
/* Mutants (mutexes) - non-recursive, ownership not yet tracked        */
/* ------------------------------------------------------------------ */

void KeInitializeMutant(PKMUTANT mutant, BOOLEAN initially_owned)
{
    mutant->Header.Type = MutantObject;
    mutant->Header.SignalState = initially_owned ? 0 : 1;
    InitializeListHead(&mutant->Header.WaitListHead);
}

LONG KeReleaseMutant(PKMUTANT mutant)
{
    UINT64 flags = KiIrqSave();
    LONG previous = mutant->Header.SignalState;
    mutant->Header.SignalState = 1;
    KiSignalObject(&mutant->Header);
    KiIrqRestore(flags);
    return previous;
}
