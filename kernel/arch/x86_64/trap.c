/*
 * arch/x86_64/trap.c - the C trap dispatcher.
 *
 * Called by the common assembly stub with a fully populated KTRAP_FRAME. For
 * now every CPU exception (vector < 32) is fatal: we dump the frame and bring
 * the system down through KeBugCheck. Device interrupts (>= 32) are not enabled
 * yet, so anything landing here is unexpected and merely reported.
 */
#include <nt/ntdef.h>
#include <ntos/ke.h>

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
}

void KiDispatchTrap(PKTRAP_FRAME frame)
{
    dump_frame(frame);

    if (frame->vector < 32)
        KeBugCheck(KE_UNEXPECTED_KERNEL_MODE_TRAP,
                   "Unhandled CPU exception in kernel mode");

    /* Not reached under current configuration (no external IRQs enabled). */
    KeLog("Ignoring unexpected external interrupt %lu\n",
          (unsigned long)frame->vector);
}
