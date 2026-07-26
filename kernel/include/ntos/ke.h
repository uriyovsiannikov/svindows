/*
 * ntos/ke.h - Kernel core (Ke).
 *
 * Entry point, CPU structure init (GDT/IDT), the trap frame, panic/bugcheck,
 * and the kernel-wide formatted logger.
 */
#ifndef _NTOS_KE_H_
#define _NTOS_KE_H_

#include <nt/ntdef.h>
#include <stdarg.h>

/* ------------------------------------------------------------------ */
/* Atomic (interlocked) operations                                    */
/* ------------------------------------------------------------------ */

static ALWAYS_INLINE LONG InterlockedIncrement(volatile LONG *value)
{
    return __atomic_add_fetch(value, 1, __ATOMIC_SEQ_CST);
}

static ALWAYS_INLINE LONG InterlockedDecrement(volatile LONG *value)
{
    return __atomic_sub_fetch(value, 1, __ATOMIC_SEQ_CST);
}

static ALWAYS_INLINE LONG InterlockedExchange(volatile LONG *target, LONG value)
{
    return __atomic_exchange_n(target, value, __ATOMIC_SEQ_CST);
}

/* Save RFLAGS and disable interrupts; restore later. Used to make short
 * critical sections atomic against preemption on the local CPU. */
static ALWAYS_INLINE UINT64 KiIrqSave(void)
{
    UINT64 flags;
    __asm__ volatile("pushfq; pop %0; cli" : "=r"(flags) : : "memory");
    return flags;
}

static ALWAYS_INLINE void KiIrqRestore(UINT64 flags)
{
    __asm__ volatile("push %0; popfq" : : "r"(flags) : "memory", "cc");
}

/* Save / restore the x87+SSE state (XMM registers, MXCSR, ...) to a 512-byte,
 * 16-byte-aligned area. The context switch uses these so threads that use SSE
 * (all user code does) don't clobber each other's XMM registers. */
static ALWAYS_INLINE void KiSaveFpuState(void *area)
{
    __asm__ volatile("fxsave (%0)" : : "r"(area) : "memory");
}

static ALWAYS_INLINE void KiRestoreFpuState(const void *area)
{
    __asm__ volatile("fxrstor (%0)" : : "r"(area) : "memory");
}

/* ------------------------------------------------------------------ */
/* Boot information handed to the kernel by the boot trampoline        */
/* ------------------------------------------------------------------ */

/*
 * Physical pointer to the Multiboot2 information structure and its magic
 * value, captured by boot.asm and passed to KiSystemStartup. The structures
 * live in low physical memory, which early boot keeps identity-mapped.
 */
#define MULTIBOOT2_BOOTLOADER_MAGIC 0x36D76289u

/* ------------------------------------------------------------------ */
/* Kernel logger                                                      */
/* ------------------------------------------------------------------ */

void KeLogInit(void);
int  KeVLog(const char *fmt, va_list ap);
int  KeLog(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

/* Familiar spellings used throughout NT-style code. */
#define DbgPrint  KeLog
#define kprintf   KeLog

/* ------------------------------------------------------------------ */
/* Fatal errors (bugcheck / KeBugCheck)                               */
/* ------------------------------------------------------------------ */

NORETURN void KeBugCheck(ULONG code, const char *message);

/* A few bugcheck codes borrowed from NT semantics. */
#define KE_PHASE0_INITIALIZATION_FAILED 0x00000031u
#define KE_TRAP_UNHANDLED               0x0000007Fu
#define KE_UNEXPECTED_KERNEL_MODE_TRAP  0x0000007Fu

/* ------------------------------------------------------------------ */
/* Descriptor tables                                                  */
/* ------------------------------------------------------------------ */

void KeInitializeGdt(void);
void KeInitializeIdt(void);

/*
 * KTRAP_FRAME - the register state pushed by our interrupt/exception stubs.
 * The layout is produced by arch/x86_64/isr.asm and consumed by the C trap
 * dispatcher, so the two must stay in lock-step.
 */
typedef struct _KTRAP_FRAME {
    /* Pushed by the common stub (in this order via `push`), so they appear
     * here from the last-pushed (r15) to the first-pushed (rax) reversed by
     * the stack growing down — see isr.asm for the exact sequence. */
    UINT64 r15, r14, r13, r12, r11, r10, r9, r8;
    UINT64 rbp, rdi, rsi, rdx, rcx, rbx, rax;

    UINT64 vector;      /* interrupt/exception vector number */
    UINT64 error_code;  /* CPU error code, or 0 if none      */

    /* Pushed automatically by the CPU on interrupt entry. */
    UINT64 rip;
    UINT64 cs;
    UINT64 rflags;
    UINT64 rsp;
    UINT64 ss;
} KTRAP_FRAME, *PKTRAP_FRAME;

/* Called from the assembly stubs with a pointer to the trap frame. */
void KiDispatchTrap(PKTRAP_FRAME frame);

/* ------------------------------------------------------------------ */
/* Threads, processes, and the scheduler                              */
/* ------------------------------------------------------------------ */

typedef enum _KTHREAD_STATE {
    ThreadStateInitialized = 0,
    ThreadStateReady,
    ThreadStateRunning,
    ThreadStateWaiting,
    ThreadStateTerminated,
} KTHREAD_STATE;

typedef void (*PKSTART_ROUTINE)(PVOID StartContext);

struct _KPROCESS;
struct _KTHREAD;

/* ------------------------------------------------------------------ */
/* Dispatcher (waitable) objects                                       */
/* ------------------------------------------------------------------ */

typedef enum _DISPATCHER_TYPE {
    EventNotificationObject = 0, /* stays signaled until reset */
    EventSynchronizationObject,  /* auto-reset: one waiter consumes it */
    SemaphoreObject,             /* SignalState is the count */
    MutantObject,                /* 1 = free, 0 = held */
    ThreadObject,                /* signaled when the thread terminates */
} DISPATCHER_TYPE;

/*
 * DISPATCHER_HEADER - the common prefix of every waitable kernel object. A
 * thread waits on an object by casting the object body to this header, so every
 * waitable object body must begin with one.
 */
typedef struct _DISPATCHER_HEADER {
    LONG         Type;         /* DISPATCHER_TYPE */
    volatile LONG SignalState;
    LIST_ENTRY   WaitListHead; /* KWAIT_BLOCKs of waiting threads */
} DISPATCHER_HEADER, *PDISPATCHER_HEADER;

typedef struct _KWAIT_BLOCK {
    LIST_ENTRY        WaitListEntry;
    struct _KTHREAD  *Thread;
    PDISPATCHER_HEADER Object;
} KWAIT_BLOCK, *PKWAIT_BLOCK;

typedef struct _KEVENT {
    DISPATCHER_HEADER Header;
} KEVENT, *PKEVENT;

typedef struct _KSEMAPHORE {
    DISPATCHER_HEADER Header;
    LONG              Limit;
} KSEMAPHORE, *PKSEMAPHORE;

typedef struct _KMUTANT {
    DISPATCHER_HEADER Header;
} KMUTANT, *PKMUTANT;

/*
 * KTHREAD - a schedulable thread of execution. Each thread owns a kernel stack;
 * KernelStackPointer holds its saved RSP while it is not the running thread.
 */
typedef struct _KTHREAD {
    UINT64          KernelStackPointer; /* saved RSP when switched out       */
    UINT64          KernelStackBase;    /* allocation base (for teardown)    */
    SIZE_T          KernelStackSize;
    KTHREAD_STATE   State;
    LONG            Priority;
    LONG            Quantum;            /* ticks left in the current slice   */
    PKSTART_ROUTINE StartRoutine;
    PVOID           StartContext;
    LIST_ENTRY      ReadyEntry;         /* link in the scheduler ready queue */
    LIST_ENTRY      ProcessEntry;       /* link in the owning process        */
    ULONG           ThreadId;
    const char     *Name;
    struct _KPROCESS *Process;

    /* User-mode threads start life in ring 3 at UserEntry with stack UserStack
     * instead of calling a kernel StartRoutine. UserGsBase is the TEB address
     * the GS segment resolves to while the thread runs in ring 3. UserArg is
     * passed to the entry point in RCX (Windows convention). */
    BOOLEAN         UserMode;
    UINT64          UserEntry;
    UINT64          UserStack;
    UINT64          UserGsBase;
    UINT64          UserArg;

    /* Synchronization: the block used while this thread waits, and the
     * dispatcher header (if any) that is signaled when it terminates. */
    KWAIT_BLOCK        WaitBlock;
    PDISPATCHER_HEADER TerminationObject;

    /* x87+SSE state saved across context switches (FXSAVE image; 16-aligned). */
    __attribute__((aligned(16))) UINT8 FpuState[512];
} KTHREAD, *PKTHREAD;

/*
 * KPROCESS - a container for threads and (later) an address space. For now all
 * kernel threads share the kernel's page tables.
 */
typedef struct _KPROCESS {
    UINT64     DirectoryTableBase; /* CR3 for this process                    */
    LIST_ENTRY ThreadListHead;
    ULONG      ProcessId;
    const char *Name;
} KPROCESS, *PKPROCESS;

void      KeInitializeScheduler(void);
PKTHREAD  KeCreateThread(const char *name, PKSTART_ROUTINE routine,
                         PVOID context, LONG priority);
PKTHREAD  KeCreateUserThread(const char *name, UINT64 user_entry,
                            UINT64 user_stack, UINT64 user_gs_base,
                            UINT64 user_arg, LONG priority);
PKTHREAD  KeGetCurrentThread(void);
void      KeYield(void);
NORETURN void KeTerminateThread(void);

/* Called from the timer interrupt to drive preemption. */
void      KeClockTick(void);
UINT64    KeGetTickCount(void);

/* Scheduler hooks used by the dispatcher (interrupts must be disabled). */
void      KiReadyThread(PKTHREAD thread);   /* move a waiting thread to Ready */
void      KiBlockCurrentThread(void);       /* current is Waiting -> reschedule */

/* ------------------------------------------------------------------ */
/* Synchronization primitives                                          */
/* ------------------------------------------------------------------ */

void KeInitializeEvent(PKEVENT event, BOOLEAN notification, BOOLEAN signaled);
LONG KeSetEvent(PKEVENT event);   /* returns the previous state */
void KeResetEvent(PKEVENT event);

void KeInitializeSemaphore(PKSEMAPHORE sem, LONG initial, LONG limit);
LONG KeReleaseSemaphore(PKSEMAPHORE sem, LONG count);

void KeInitializeMutant(PKMUTANT mutant, BOOLEAN initially_owned);
LONG KeReleaseMutant(PKMUTANT mutant);

/* Block until `object` (a DISPATCHER_HEADER) is signaled and acquired. */
NTSTATUS KeWaitForSingleObject(PDISPATCHER_HEADER object);

/* Signal a header and wake its waiters (interrupts must be disabled). */
void KiSignalObject(PDISPATCHER_HEADER header);

/* ------------------------------------------------------------------ */
/* System calls and user mode (ring 3)                                */
/* ------------------------------------------------------------------ */

/* Program the syscall MSRs and per-CPU block. Call once during boot after the
 * GDT is installed. */
void KiInitializeSystemCalls(void);

/* Point the CPU's ring-0 entry stack (TSS.RSP0 and the syscall entry's kernel
 * stack) at `kernel_rsp`. The scheduler calls this on every context switch. */
void KeSetKernelStack(UINT64 kernel_rsp);

/* Set the GS base a thread will see in ring 3 (its TEB); 0 selects the kernel
 * per-CPU block. The scheduler calls this on every context switch so the
 * kernel-exit swapgs restores the right per-thread GS. */
void KeSetUserGsBase(UINT64 teb);

/* Set the TSS ring-0 stack pointer (implemented in gdt.c). */
void KeSetTssRsp0(UINT64 rsp0);

/* Assembly: drop to ring 3 at `entry` (arg in RCX) with `user_stack`. */
void KiEnterUserMode(UINT64 entry, UINT64 user_stack, UINT64 arg);

/* The C half of the system-service dispatcher, called from KiSystemCallEntry
 * with the number and a pointer to the marshaled argument array. */
UINT64 KiSystemServiceDispatch(UINT64 number, UINT64 *args);
void   KiInitializeServiceTable(void);

#endif /* _NTOS_KE_H_ */
