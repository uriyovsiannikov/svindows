; ============================================================================
; arch/x86_64/switch.asm - kernel-thread context switch and thread bootstrap.
;
; A context switch here is deliberately minimal: only the callee-saved registers
; defined by the System V AMD64 ABI need to be preserved across a C function
; call, because KiSwitchContext IS such a call from the scheduler's point of
; view. The caller-saved registers are already spilled by the compiler around
; the call, and the full interrupted state (for a preempted thread) lives in the
; KTRAP_FRAME further down that thread's own kernel stack.
; ============================================================================
bits 64
section .text

; void KiSwitchContext(UINT64 *save_rsp, UINT64 load_rsp);
;   rdi = address to store the outgoing thread's RSP
;   rsi = the incoming thread's saved RSP
;
; Saves callee-saved registers on the current (outgoing) stack, records RSP,
; loads the incoming stack, restores its callee-saved registers, and returns
; into wherever that thread last left off.
global KiSwitchContext
KiSwitchContext:
    push    rbx
    push    rbp
    push    r12
    push    r13
    push    r14
    push    r15

    mov     [rdi], rsp        ; save outgoing RSP
    mov     rsp, rsi          ; load incoming RSP

    pop     r15
    pop     r14
    pop     r13
    pop     r12
    pop     rbp
    pop     rbx
    ret

; void KiThreadStartup(void);
;   The synthetic return address at the bottom of a brand-new thread's stack.
;   Reached the first time such a thread is switched to. Interrupts were
;   disabled by the scheduler; enable them, then run the thread body.
global KiThreadStartup
extern KiThreadBootstrap
KiThreadStartup:
    sti                       ; new threads start with interrupts enabled
    xor     rbp, rbp          ; end of the call-chain for backtraces
    and     rsp, -16          ; ABI stack alignment before the call
    call    KiThreadBootstrap
.hang:
    cli
    hlt
    jmp     .hang
