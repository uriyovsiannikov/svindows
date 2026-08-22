/*
 * arch/x86_64/trap.c - the C trap dispatcher.
 *
 * Called by the common assembly stub with a fully populated KTRAP_FRAME. For
 * Kernel-mode CPU exceptions remain fatal. A fault whose saved CS is ring 3,
 * however, belongs to the current process: dump it for diagnostics and
 * terminate only that user thread, just as NT turns an unhandled user exception
 * into process/thread teardown rather than a kernel bugcheck.
 */
#include <nt/ntdef.h>
#include <ntos/ke.h>
#include <ntos/hal.h>
#include <ntos/mm.h>
#include <ntos/ldr.h>

static const char *const g_exception_names[32] = {
    "#DE Divide-by-Zero",
    "#DB Debug",
    "NMI",
    "#BP Breakpoint",
    "#OF Overflow",
    "#BR BOUND Range Exceeded",
    "#UD Invalid Opcode",
    "#NM Device Not Available",
    "#DF Double Fault",
    "Coprocessor Segment Overrun",
    "#TS Invalid TSS",
    "#NP Segment Not Present",
    "#SS Stack-Segment Fault",
    "#GP General Protection Fault",
    "#PF Page Fault",
    "(reserved 15)",
    "#MF x87 Floating-Point",
    "#AC Alignment Check",
    "#MC Machine Check",
    "#XM SIMD Floating-Point",
    "#VE Virtualization",
    "#CP Control Protection",
    "(reserved 22)", "(reserved 23)", "(reserved 24)", "(reserved 25)",
    "(reserved 26)", "(reserved 27)", "(reserved 28)", "(reserved 29)",
    "#SX Security", "(reserved 31)",
};

static UINT64 read_cr2(void)
{
    UINT64 v;
    __asm__ volatile("mov %%cr2, %0" : "=r"(v));
    return v;
}

static void dump_frame(PKTRAP_FRAME f)
{
    const char *name = f->vector < 32 ? g_exception_names[f->vector]
                                      : "(external interrupt)";

    KeLog("\n=== TRAP: vector %lu (%s), error_code=0x%lx ===\n",
          (unsigned long)f->vector, name, (unsigned long)f->error_code);

    KeLog("RIP=%p  CS=0x%lx  RFLAGS=0x%lx\n",
          (void *)f->rip, (unsigned long)f->cs, (unsigned long)f->rflags);
    KeLog("RSP=%p  SS=0x%lx\n", (void *)f->rsp, (unsigned long)f->ss);

    if (f->vector == 14) /* #PF: CR2 holds the faulting linear address */
        KeLog("CR2=%p (faulting address)\n", (void *)read_cr2());

    KeLog("RAX=%p RBX=%p RCX=%p RDX=%p\n",
          (void *)f->rax, (void *)f->rbx, (void *)f->rcx, (void *)f->rdx);
    KeLog("RSI=%p RDI=%p RBP=%p\n",
          (void *)f->rsi, (void *)f->rdi, (void *)f->rbp);
    KeLog("R8 =%p R9 =%p R10=%p R11=%p\n",
          (void *)f->r8, (void *)f->r9, (void *)f->r10, (void *)f->r11);
    KeLog("R12=%p R13=%p R14=%p R15=%p\n",
          (void *)f->r12, (void *)f->r13, (void *)f->r14, (void *)f->r15);

    /* Debug aid: raw bytes at the faulting RIP and the top of the stack. */
    if (f->rip && MmIsUserAddress(f->rip) && MmProbeForRead(f->rip, 16)) {
        const volatile UINT8 *ip = (const volatile UINT8 *)f->rip;
        KeLog("code@RIP:");
        for (int i = 0; i < 16; i++)
            KeLog(" %02x", ip[i]);
        KeLog("\n");
    }
    if (f->rsp && MmIsUserAddress(f->rsp) && MmProbeForRead(f->rsp, 32)) {
        const volatile UINT64 *sp = (const volatile UINT64 *)f->rsp;
        KeLog("stack@RSP: %p %p %p %p\n",
              (void *)sp[0], (void *)sp[1], (void *)sp[2], (void *)sp[3]);
    }

    /* Annotate the user stack: values landing inside loaded module images are
     * return addresses, giving an exit/fault backtrace without full unwind
     * support. */
    if ((f->cs & 3) == 3 && f->rsp && MmIsUserAddress(f->rsp)) {
        KeLog("[user] stack walk:\n");
        for (UINT64 i = 0; i < 64; i++) {
            UINT64 va = f->rsp + i * 8;
            if (!MmProbeForRead(va, sizeof(UINT64)))
                break;
            UINT64 value = *(volatile UINT64 *)va;
            const char *name = NULL;
            UINT64 base = 0;
            if (value && LdrDescribeUserAddress(value, &name, &base))
                KeLog("  [%02lu] %p  %s+0x%lx\n", (unsigned long)i,
                      (void *)value, name,
                      (unsigned long)(value - base));
        }
    }
}

void KiDispatchTrap(PKTRAP_FRAME frame)
{
    /* Hardware IRQs (remapped to vectors 32..47) go to the HAL, which EOIs the
     * PIC and runs the registered handler (e.g. the scheduler tick). */
    if (frame->vector >= IRQ_BASE_VECTOR && frame->vector < IRQ_BASE_VECTOR + 16) {
        HalDispatchIrq((UINT8)(frame->vector - IRQ_BASE_VECTOR));
        return;
    }

    /* Never let a bad or incomplete Win32 component take down the kernel. We
     * do not have user-mode SEH dispatch yet, so an unhandled ring-3 exception
     * terminates the faulting thread after preserving a full diagnostic dump. */
    if (frame->vector < 32) {
        dump_frame(frame);
        if ((frame->cs & 3) == 3) {
            const UINT64 cfg_trace_va = 0x0000000000088ff8ULL;
            if (MmProbeForRead(cfg_trace_va, sizeof(UINT64)))
                KeLog("[user] last CFG indirect target: %p\n",
                      (void *)*(volatile UINT64 *)cfg_trace_va);
            PKTHREAD thread = KeGetCurrentThread();
            KeLog("[user] unhandled exception in thread '%s' (id %u); "
                  "terminating thread only\n",
                  thread && thread->Name ? thread->Name : "?",
                  thread ? thread->ThreadId : 0);
            KeTerminateThread();
        }
        KeBugCheck(KE_UNEXPECTED_KERNEL_MODE_TRAP,
                   "Unhandled CPU exception in kernel mode");
    }

    /* Any other vector is unexpected. */
    dump_frame(frame);
    KeLog("Ignoring unexpected interrupt %lu\n", (unsigned long)frame->vector);
}
