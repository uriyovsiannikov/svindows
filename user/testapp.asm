; ============================================================================
; user/testapp.asm - a native NTOS test program, built as a real PE executable.
;
; Assembled with `nasm -f win64` and linked with `lld-link` into a PE32+ image,
; then embedded in the kernel and loaded by the NTOS PE loader. It talks to the
; kernel only through the NTOS syscall ABI (no Windows runtime), so it needs no
; import table and no CRT.
;
;   syscall ABI: number in RAX, args in RDI/RSI/RDX/R10/R8
;     0 = NtDisplayString(rdi = pointer)
;     1 = NtDisplayNumber(rdi = value)
;     2 = NtTerminateThread()
; ============================================================================
bits 64
default rel

section .text
global Start
Start:
    lea     rdi, [message]        ; RIP-relative; valid once mapped at ImageBase
    xor     eax, eax              ; NtDisplayString
    syscall

    mov     edi, 0x00ABCDEF
    mov     eax, 1                ; NtDisplayNumber
    syscall

    mov     eax, 2                ; NtTerminateThread (does not return)
    syscall

.hang:
    jmp     .hang

section .rdata
message:
    db "Hello from a PE executable running in ring 3!", 0
