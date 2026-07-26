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
;   arguments 5.. are on the user stack at [user_rsp + 0x28] (past the stub's
;     return address and the 4-slot home space), exactly as in a Windows call
;   return value in RAX
;
; The entry gathers all arguments into an 11-entry array on the kernel stack and
; passes its address to KiSystemServiceDispatch(number, args), so services with
; the real (up to 11-argument) NT signatures get every parameter.
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

    ; Put the four register arguments in an array on the kernel stack, and hand
    ; the C dispatcher the user RSP so it can gather any stack arguments safely
    ; (probing each, since a low-argument call may leave RSP near the top of the
    ; stack where [RSP+0x28] is unmapped). Interrupts are masked (SFMASK clears
    ; IF), so [gs:0] is a stable copy of the user RSP.
    sub     rsp, 32               ; 4 * 8 bytes
    mov     [rsp+0x00], r10       ; arg1
    mov     [rsp+0x08], rdx       ; arg2
    mov     [rsp+0x10], r8        ; arg3
    mov     [rsp+0x18], r9        ; arg4

    mov     rdi, rax              ; num       -> arg1
    mov     rsi, rsp              ; reg args  -> arg2 (pointer)
    mov     rdx, [gs:0]           ; user RSP  -> arg3

    ; Realign to 16: 5 pushes (40) + 32 = 72, so drop 8 more.
    sub     rsp, 8
    call    KiSystemServiceDispatch
    add     rsp, 8
    add     rsp, 32               ; drop the register-argument array
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

    ; The entry is treated like a called function. Present RSP % 16 == 8 (as if
    ; a return address had been pushed onto a 16-aligned stack) AND leave the
    ; 32-byte "home"/shadow space above RSP mapped: a real Win64 prologue homes
    ; its non-volatile registers into [rsp+8]..[rsp+0x20] (the caller's shadow
    ; space) before allocating its own frame, so those slots must be writable.
    ; Reserving 0x28 (0x20 home + one return slot) keeps RSP % 16 == 8 and keeps
    ; the whole home space inside the mapped stack.
    and     rsi, -16
    sub     rsi, 0x28
    mov     qword [rsi], 0        ; return slot: a stray `ret` from the entry
                                  ; faults at RIP=0 (caught) instead of running
                                  ; off into mapped code. The CRT exits via
                                  ; ExitProcess and never returns here.

    ; Switch the active GS base to this thread's TEB (held in KERNEL_GS_BASE by
    ; the scheduler) before entering ring 3.
    swapgs

    push    0x1B                  ; SS  = user data | 3
    push    rsi                   ; RSP = user stack
    push    0x202                 ; RFLAGS: IF=1, reserved bit set
    push    0x23                  ; CS  = user code | 3
    push    rdi                   ; RIP = user entry
    iretq
