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
static BOOLEAN KiTryAcquire(PDISPATCHER_HEADER h)
{
    switch (h->Type) {
    case EventNotificationObject:
    case ThreadObject:
        return (BOOLEAN)(h->SignalState != 0); /* level-triggered, no consume */
    case EventSynchronizationObject:
        if (h->SignalState) {
            h->SignalState = 0;
            return TRUE;
        }
        return FALSE;
    case SemaphoreObject:
    case MutantObject:
        if (h->SignalState > 0) {
            h->SignalState--;
            return TRUE;
        }
        return FALSE;
    }
    return FALSE;
}

/* Wake every thread waiting on this header so it can re-test the object. */
void KiSignalObject(PDISPATCHER_HEADER h)
{
    while (!IsListEmpty(&h->WaitListHead)) {
        PLIST_ENTRY e = RemoveHeadList(&h->WaitListHead);
        PKWAIT_BLOCK wb = CONTAINING_RECORD(e, KWAIT_BLOCK, WaitListEntry);
        KiReadyThread(wb->Thread);
    }
}

NTSTATUS KeWaitForSingleObject(PDISPATCHER_HEADER h)
{
    UINT64 flags = KiIrqSave();

    while (!KiTryAcquire(h)) {
        PKTHREAD t = KeGetCurrentThread();
        t->WaitBlock.Thread = t;
        t->WaitBlock.Object = h;
        InsertTailList(&h->WaitListHead, &t->WaitBlock.WaitListEntry);
        t->State = ThreadStateWaiting;
        KiBlockCurrentThread(); /* reschedule; returns once signalled */
    }

    KiIrqRestore(flags);
    return STATUS_SUCCESS;
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
