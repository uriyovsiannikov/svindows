; ============================================================================
; arch/x86_64/cpu.asm - small 64-bit helpers for loading descriptor tables.
;
; These use the System V AMD64 ABI (first argument in RDI) since they are
; called from the C kernel.
; ============================================================================
bits 64
section .text

; void KiLoadGdtr(const void *gdtr);
;   Loads the GDT, reloads the data segment registers, and reloads CS with the
;   kernel code selector (0x08) via a far return.
global KiLoadGdtr
KiLoadGdtr:
    lgdt    [rdi]

    mov     ax, 0x10          ; kernel data selector
    mov     ds, ax
    mov     es, ax
    mov     ss, ax
    mov     fs, ax
    mov     gs, ax

    pop     rax               ; return address
    push    qword 0x08        ; kernel code selector (new CS)
    push    rax               ; RIP to return to
    o64 retf                  ; far return -> reloads CS:RIP

; void KiLoadTr(UINT16 selector);
global KiLoadTr
KiLoadTr:
    ltr     di
    ret

; void KiLoadIdtr(const void *idtr);
global KiLoadIdtr
KiLoadIdtr:
    lidt    [rdi]
    ret
