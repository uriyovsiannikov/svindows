/*
 * ke/thread.c - kernel threads and the preemptive round-robin scheduler.
 *
 * Threads are cooperatively AND preemptively scheduled: a thread may call
 * KeYield, and the timer interrupt (KeClockTick) forces a reschedule when a
 * thread's quantum expires. Switching is done by KiSwitchContext, which swaps
 * kernel stacks; a brand-new thread's stack is fabricated so its first switch
 * lands in KiThreadStartup -> KiThreadBootstrap -> the thread body.
 *
 * Single-CPU only for now, so short critical sections just disable interrupts
 * (KiIrqSave/KiIrqRestore) instead of taking spinlocks.
 */
#include <ntos/ke.h>
#include <ntos/ex.h>
#include <ntos/ob.h>
#include <ntos/rtl.h>
#include <nt/peb.h>

#define DEFAULT_QUANTUM 2   /* timer ticks per scheduling slice */
#define THREAD_STACK_SIZE 0x4000 /* 16 KiB kernel stack per thread */

extern void KiSwitchContext(UINT64 *save_rsp, UINT64 load_rsp);
extern void KiThreadStartup(void);                              /* asm trampoline    */
extern void KiEnterUserMode(UINT64 entry, UINT64 stack, UINT64 arg); /* asm ring-3 */

static KPROCESS       g_system_process;
static KTHREAD        g_idle_thread;
static PKTHREAD       g_current_thread;
static LIST_ENTRY     g_ready_queue;
static ULONG          g_next_thread_id;
static volatile UINT64 g_tick_count;

PKTHREAD KeGetCurrentThread(void) { return g_current_thread; }
UINT64   KeGetTickCount(void)     { return g_tick_count; }

void KeInitializeScheduler(void)
{
    InitializeListHead(&g_ready_queue);

    UINT64 cr3;
    __asm__ volatile("mov %%cr3, %0" : "=r"(cr3));

    g_system_process.DirectoryTableBase = cr3;
    g_system_process.ProcessId = 0;
    g_system_process.Name = "System";
    InitializeListHead(&g_system_process.ThreadListHead);

    /* The currently executing boot context becomes the idle thread. */
    memset(&g_idle_thread, 0, sizeof(g_idle_thread));
    g_idle_thread.State = ThreadStateRunning;
    g_idle_thread.Name = "Idle";
    g_idle_thread.Process = &g_system_process;
    InsertTailList(&g_system_process.ThreadListHead, &g_idle_thread.ProcessEntry);

    g_current_thread = &g_idle_thread;
    KeSetCurrentThread(&g_idle_thread);
    g_next_thread_id = 1;

    /* Give the idle thread a valid FPU/SSE image for the first context switch. */
    KiSaveFpuState(g_idle_thread.FpuState);

    KeLog("[ke]   scheduler ready: System process, idle thread\n");
}

/* Allocate a thread and its kernel stack, fabricating the initial stack so the
 * first KiSwitchContext to it 'ret's into KiThreadStartup. Not yet enqueued. */
static PKTHREAD KepAllocThread(const char *name, LONG priority)
{
    PKTHREAD t = ExAllocatePoolWithTag(NonPagedPool, sizeof(KTHREAD), 'rhtK');
    if (!t)
        return NULL;
    memset(t, 0, sizeof(*t));

    void *stack = ExAllocatePoolWithTag(NonPagedPool, THREAD_STACK_SIZE, 'ktsK');
    if (!stack) {
        ExFreePool(t);
        return NULL;
    }
    t->KernelStackBase = (UINT64)stack;
    t->KernelStackSize = THREAD_STACK_SIZE;

    UINT64 *sp = (UINT64 *)((UINT64)stack + THREAD_STACK_SIZE);
    *--sp = (UINT64)KiThreadStartup; /* return address for the final 'ret' */
    *--sp = 0;                       /* rbx */
    *--sp = 0;                       /* rbp */
    *--sp = 0;                       /* r12 */
    *--sp = 0;                       /* r13 */
    *--sp = 0;                       /* r14 */
    *--sp = 0;                       /* r15 (popped first) */
    t->KernelStackPointer = (UINT64)sp;

    t->State = ThreadStateReady;
    t->Priority = priority;
    t->Quantum = DEFAULT_QUANTUM;
    t->ThreadId = g_next_thread_id++;
    t->Name = name;
    t->Process = &g_system_process;

    /* Seed a valid FPU/SSE image (captures the default MXCSR) so the first
     * FXRSTOR into this thread doesn't fault or load garbage. */
    KiSaveFpuState(t->FpuState);
    return t;
}

static void KepEnqueueThread(PKTHREAD t)
{
    UINT64 flags = KiIrqSave();
    InsertTailList(&g_ready_queue, &t->ReadyEntry);
    InsertTailList(&g_system_process.ThreadListHead, &t->ProcessEntry);
    KiIrqRestore(flags);
}

PKTHREAD KeCreateThread(const char *name, PKSTART_ROUTINE routine,
                        PVOID context, LONG priority)
{
    PKTHREAD t = KepAllocThread(name, priority);
    if (!t)
        return NULL;

    t->StartRoutine = routine;
    t->StartContext = context;
    KepEnqueueThread(t);

    KeLog("[ke]   created thread '%s' (id %u), stack %p\n",
          name, t->ThreadId, (void *)t->KernelStackBase);
    return t;
}

PKTHREAD KeCreateUserThread(const char *name, UINT64 user_entry,
                            UINT64 user_stack, UINT64 user_gs_base,
                            UINT64 user_arg, LONG priority)
{
    PKTHREAD t = KepAllocThread(name, priority);
    if (!t)
        return NULL;

    t->UserMode = TRUE;
    t->UserEntry = user_entry;
    t->UserStack = user_stack;
    t->UserGsBase = user_gs_base;
    t->UserArg = user_arg;

    /* Ps builds the TEB before Ke allocates the KTHREAD, so this is the first
     * point at which the real thread id is known.  Native code reads the id
     * directly from gs:[0x48]; leaving child TEBs at zero makes every created
     * thread look identical and breaks owner-based user-mode synchronization. */
    if (user_gs_base)
        ((PTEB)user_gs_base)->ClientId.UniqueThread =
            (HANDLE)(ULONG_PTR)t->ThreadId;
    KepEnqueueThread(t);

    KeLog("[ke]   created user thread '%s' (id %u): entry %p, ustack %p, teb %p\n",
          name, t->ThreadId, (void *)user_entry, (void *)user_stack,
          (void *)user_gs_base);
    return t;
}

/*
 * KiSchedule - pick the next runnable thread and switch to it.
 * Must be called with interrupts disabled.
 */
static void KiSchedule(void)
{
    PKTHREAD prev = g_current_thread;
    PKTHREAD next;

    if (IsListEmpty(&g_ready_queue)) {
        next = &g_idle_thread;
    } else {
        PLIST_ENTRY e = RemoveHeadList(&g_ready_queue);
        next = CONTAINING_RECORD(e, KTHREAD, ReadyEntry);
    }

    /* Return the outgoing thread to the ready queue if it can still run. The
     * idle thread is the fallback and is never queued. */
    if (prev != &g_idle_thread && prev->State == ThreadStateRunning) {
        prev->State = ThreadStateReady;
        InsertTailList(&g_ready_queue, &prev->ReadyEntry);
    }

    if (prev == next) {
        prev->State = ThreadStateRunning;
        return;
    }

    next->State = ThreadStateRunning;
    next->Quantum = DEFAULT_QUANTUM;
    g_current_thread = next;
    KeSetCurrentThread(next);

    /* Point the CPU's ring-0 entry stack at the incoming thread's kernel stack,
     * so a syscall or interrupt taken from ring 3 lands on the right stack, and
     * set the GS base it will see in ring 3 (its TEB). */
    if (next->KernelStackBase)
        KeSetKernelStack(next->KernelStackBase + next->KernelStackSize);
    KeSetUserGsBase(next->UserGsBase);

    /* Preserve SSE/x87 state across the switch: save ours, load the incoming
     * thread's. Done here (not in the asm) because the incoming thread's area
     * must be restored with a pointer that survives the stack swap. */
    KiSaveFpuState(prev->FpuState);
    KiRestoreFpuState(next->FpuState);

    KiSwitchContext(&prev->KernelStackPointer, next->KernelStackPointer);
    /* Control returns here only when `prev` is scheduled again. */
}

void KeYield(void)
{
    UINT64 flags = KiIrqSave();
    KiSchedule();
    KiIrqRestore(flags);
}

/* Move a waiting thread back to the ready queue (called by the dispatcher with
 * interrupts disabled). */
void KiReadyThread(PKTHREAD thread)
{
    if (thread->State == ThreadStateWaiting) {
        thread->State = ThreadStateReady;
        InsertTailList(&g_ready_queue, &thread->ReadyEntry);
    }
}

/* The current thread has marked itself Waiting; switch away until readied. */
void KiBlockCurrentThread(void)
{
    KiSchedule(); /* won't re-enqueue a non-Running thread */
}

/* Entered from KiThreadStartup (assembly) the first time a new thread runs. */
void KiThreadBootstrap(void)
{
    PKTHREAD t = g_current_thread;

    if (t->UserMode) {
        /* Drop to ring 3; the thread lives in user mode from here and only
         * re-enters the kernel via syscalls and interrupts. Never returns. */
        KiEnterUserMode(t->UserEntry, t->UserStack, t->UserArg);
    } else if (t->StartRoutine) {
        t->StartRoutine(t->StartContext);
    }

    KeTerminateThread();
}

NORETURN void KeTerminateThread(void)
{
    (void)KiIrqSave(); /* disable preemption for good on this thread */

    PKTHREAD t = g_current_thread;
    t->State = ThreadStateTerminated;
    KeLog("[ke]   thread '%s' (id %u) terminated\n", t->Name, t->ThreadId);

    /* Signal the thread object so anyone waiting on this thread wakes up, then
     * release the reference the thread held on it (see NtCreateThreadEx). The
     * dispatcher header is the object body, so the body pointer is the same
     * address. */
    if (t->TerminationObject) {
        PDISPATCHER_HEADER termination = t->TerminationObject;
        termination->SignalState = 1;
        KiSignalObject(termination);
        t->TerminationObject = NULL;
        ObDereferenceObject((POBJECT)termination);
    }

    /* NOTE: the thread's stack and KTHREAD are intentionally leaked for now;
     * a reaper in the idle thread will reclaim terminated threads once there
     * is somewhere safe to free them from. */
    KiSchedule();

    for (;;)
        __asm__ volatile("hlt");
}

void KeClockTick(void)
{
    g_tick_count++;
    KeUpdateSharedData(g_tick_count);

    /* Dispatcher timeouts are absolute tick deadlines stored on blocked
     * threads. The process list is stable while interrupts are disabled. */
    PLIST_ENTRY head = &g_system_process.ThreadListHead;
    for (PLIST_ENTRY entry = head->Flink; entry != head; ) {
        PKTHREAD waiting = CONTAINING_RECORD(entry, KTHREAD, ProcessEntry);
        entry = entry->Flink;
        if (waiting->State == ThreadStateWaiting && waiting->WaitDeadline &&
            g_tick_count >= waiting->WaitDeadline)
            KiTimeoutThreadWait(waiting);
    }

    PKTHREAD t = g_current_thread;
    if (t->Quantum > 0)
        t->Quantum--;

    if (t->Quantum <= 0)
        KiSchedule(); /* interrupts already disabled (we're in the IRQ) */
}
