; ============================================================================
; arch/x86_64/syscall_entry.asm - the SYSCALL fast-path entry and the ring-3
; transition.
;
; SYSCALL does not switch stacks, so the entry must do it by hand: swapgs to
; reach the per-CPU block (KPCR) via GS, stash the user RSP, and load the
; current thread's kernel stack. The user syscall ABI matches Windows x64: the
; caller's first argument (RCX in the Windows calling convention) is moved to
; R10 by the ntdll stub, since SYSCALL clobbers RCX (and R11):
;
;   RAX = service number
;   R10, RDX, R8, R9 = arguments 1..4
;   return value in RAX
;
; KPCR layout (see ke/syscall.c): +0 = user RSP scratch, +8 = kernel RSP.
; ============================================================================
bits 64
section .text

extern KiSystemServiceDispatch

global KiSystemCallEntry
KiSystemCallEntry:
    swapgs                        ; GS -> kernel KPCR
    mov     [gs:0], rsp           ; momentary scratch for the user RSP
    mov     rsp, [gs:8]           ; load this thread's kernel stack top

    ; Save the user RSP on THIS thread's kernel stack, not the shared KPCR slot:
    ; a syscall may block and switch to another thread whose own syscall entry
    ; would clobber the per-CPU scratch. Keeping it on the kernel stack makes it
    ; per-thread and safe across a block.
    push    qword [gs:0]          ; user RSP
    push    rcx                   ; user RIP  (SYSCALL saved it in RCX)
    push    r11                   ; user RFLAGS (SYSCALL saved it in R11)

    ; RDI and RSI are non-volatile in the Windows calling convention but scratch
    ; in the kernel's SysV ABI, so preserve the caller's values. (RBX, RBP, and
    ; R12-R15 are preserved automatically by the C dispatcher.)
    push    rdi
    push    rsi

    ; Marshal the Windows syscall ABI (num=RAX, a1=R10, a2=RDX, a3=R8, a4=R9)
    ; into the SysV argument registers for
    ; KiSystemServiceDispatch(num, a1, a2, a3, a4).
    mov     rdi, rax              ; num -> arg1
    mov     rsi, r10              ; a1  -> arg2
    mov     rcx, r8               ; a3  -> arg4 (read R8 before it is overwritten)
    mov     r8,  r9               ; a4  -> arg5
    ;   arg3 (RDX) already holds a2

    sub     rsp, 8                ; realign to 16 (five 8-byte pushes above)
    call    KiSystemServiceDispatch
    add     rsp, 8
    ; return value already in RAX for the user

    pop     rsi                   ; restore caller's RSI
    pop     rdi                   ; restore caller's RDI
    pop     r11                   ; user RFLAGS
    pop     rcx                   ; user RIP
    pop     rsp                   ; user RSP (from this thread's kernel stack)
    swapgs                        ; GS -> user
    o64 sysret                    ; back to ring 3 (RIP=RCX, RFLAGS=R11)

; void KiEnterUserMode(UINT64 entry /*rdi*/, UINT64 user_stack /*rsi*/,
;                      UINT64 arg /*rdx*/);
;   Builds an IRETQ frame and drops to ring 3, passing `arg` in RCX (the Windows
;   first-argument register). Never returns.
global KiEnterUserMode
KiEnterUserMode:
    mov     ax, 0x1B              ; user data selector (RPL 3)
    mov     ds, ax
    mov     es, ax

    mov     rcx, rdx              ; entry-point argument (Windows: first arg = RCX)

    ; The entry is treated like a called function, so present RSP % 16 == 8 (as
    ; if a return address had been pushed onto a 16-aligned stack). C entry
    ; points rely on this.
    and     rsi, -16
    sub     rsi, 8

    ; Switch the active GS base to this thread's TEB (held in KERNEL_GS_BASE by
    ; the scheduler) before entering ring 3.
    swapgs

    push    0x1B                  ; SS  = user data | 3
    push    rsi                   ; RSP = user stack
    push    0x202                 ; RFLAGS: IF=1, reserved bit set
    push    0x23                  ; CS  = user code | 3
    push    rdi                   ; RIP = user entry
    iretq
