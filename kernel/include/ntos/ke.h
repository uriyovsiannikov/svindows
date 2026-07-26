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
    ThreadStateTerminated,
} KTHREAD_STATE;

typedef void (*PKSTART_ROUTINE)(PVOID StartContext);

struct _KPROCESS;

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
     * instead of calling a kernel StartRoutine. */
    BOOLEAN         UserMode;
    UINT64          UserEntry;
    UINT64          UserStack;
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
                            UINT64 user_stack, LONG priority);
PKTHREAD  KeGetCurrentThread(void);
void      KeYield(void);
NORETURN void KeTerminateThread(void);

/* Called from the timer interrupt to drive preemption. */
void      KeClockTick(void);
UINT64    KeGetTickCount(void);

/* ------------------------------------------------------------------ */
/* System calls and user mode (ring 3)                                */
/* ------------------------------------------------------------------ */

/* Program the syscall MSRs and per-CPU block. Call once during boot after the
 * GDT is installed. */
void KiInitializeSystemCalls(void);

/* Point the CPU's ring-0 entry stack (TSS.RSP0 and the syscall entry's kernel
 * stack) at `kernel_rsp`. The scheduler calls this on every context switch. */
void KeSetKernelStack(UINT64 kernel_rsp);

/* Set the TSS ring-0 stack pointer (implemented in gdt.c). */
void KeSetTssRsp0(UINT64 rsp0);

/* Assembly: drop to ring 3 at `entry` with stack `user_stack` (never returns). */
void KiEnterUserMode(UINT64 entry, UINT64 user_stack);

/* The C half of the system-service dispatcher, called from KiSystemCallEntry. */
UINT64 KiSystemServiceDispatch(UINT64 number, UINT64 a1, UINT64 a2, UINT64 a3,
                               UINT64 a4);

#endif /* _NTOS_KE_H_ */
