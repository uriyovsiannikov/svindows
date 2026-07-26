; ============================================================================
; arch/x86_64/syscall_entry.asm - the SYSCALL fast-path entry and the ring-3
; transition.
;
; SYSCALL does not switch stacks, so the entry must do it by hand: swapgs to
; reach the per-CPU block (KPCR) via GS, stash the user RSP, and load the
; current thread's kernel stack. The user syscall ABI mirrors the SysV register
; layout with R10 replacing RCX (RCX and R11 are clobbered by SYSCALL itself):
;
;   RAX = service number
;   RDI, RSI, RDX, R10, R8 = arguments 1..5
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
    mov     [gs:0], rsp           ; save user RSP
    mov     rsp, [gs:8]           ; load this thread's kernel stack top

    push    rcx                   ; user RIP  (SYSCALL saved it in RCX)
    push    r11                   ; user RFLAGS (SYSCALL saved it in R11)

    ; Marshal (num=RAX, a1=RDI, a2=RSI, a3=RDX, a4=R10) into the SysV argument
    ; registers for KiSystemServiceDispatch(num, a1, a2, a3, a4).
    mov     r8, r10               ; a4 -> arg5
    mov     rcx, rdx              ; a3 -> arg4
    mov     rdx, rsi              ; a2 -> arg3
    mov     rsi, rdi              ; a1 -> arg2
    mov     rdi, rax              ; num -> arg1

    ; RSP is 16-byte aligned here (kernel top, minus the two 8-byte pushes).
    call    KiSystemServiceDispatch
    ; return value already in RAX for the user

    pop     r11                   ; user RFLAGS
    pop     rcx                   ; user RIP
    mov     rsp, [gs:0]           ; restore user RSP
    swapgs                        ; GS -> user
    o64 sysret                    ; back to ring 3 (RIP=RCX, RFLAGS=R11)

; void KiEnterUserMode(UINT64 entry /*rdi*/, UINT64 user_stack /*rsi*/);
;   Builds an IRETQ frame and drops to ring 3. Never returns.
global KiEnterUserMode
KiEnterUserMode:
    mov     ax, 0x1B              ; user data selector (RPL 3)
    mov     ds, ax
    mov     es, ax

    push    0x1B                  ; SS  = user data | 3
    push    rsi                   ; RSP = user stack
    push    0x202                 ; RFLAGS: IF=1, reserved bit set
    push    0x23                  ; CS  = user code | 3
    push    rdi                   ; RIP = user entry
    iretq
