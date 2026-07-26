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

#endif /* _NTOS_KE_H_ */
