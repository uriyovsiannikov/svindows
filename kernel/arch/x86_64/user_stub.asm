; ============================================================================
; arch/x86_64/user_stub.asm - a tiny ring-3 test program.
;
; This code is NOT executed in place. The kernel copies the bytes between
; UserStubStart and UserStubEnd into a freshly allocated user page and jumps to
; them in ring 3. It must therefore be position independent: it only uses
; immediates, RIP-relative addressing, and the `syscall` instruction.
;
; Syscall ABI: number in RAX, args in RDI/RSI/RDX/R10/R8; RCX and R11 are
; clobbered by `syscall` itself.
;   0 = NtDisplayString(rdi = pointer)
;   1 = NtDisplayNumber(rdi = value)
;   2 = NtTerminateThread()
; ============================================================================
bits 64
section .rodata

global UserStubStart
global UserStubEnd

UserStubStart:
    lea     rdi, [rel .message]   ; RIP-relative -> correct user VA once copied
    mov     eax, 0                ; NtDisplayString
    syscall

    mov     edi, 0x00C0FFEE       ; some value to print
    mov     eax, 1                ; NtDisplayNumber
    syscall

    mov     eax, 2                ; NtTerminateThread (does not return)
    syscall

.spin:
    jmp     .spin                 ; safety net if the kernel ever returns here

.message:
    db      "Hello from ring 3 -- a user-mode syscall!", 0
UserStubEnd:
