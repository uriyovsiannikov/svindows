; ============================================================================
; arch/x86_64/isr.asm - the 256 interrupt/exception entry stubs.
;
; Each vector gets a tiny stub that normalises the stack (pushing a dummy error
; code for vectors that don't supply one, then the vector number) and jumps to
; a common path. The common path saves the general registers into a KTRAP_FRAME
; and calls the C dispatcher KiDispatchTrap(frame).
;
; The order of the pushes here MUST match struct KTRAP_FRAME in ntos/ke.h.
; ============================================================================
bits 64
section .text
extern KiDispatchTrap

; ---- per-vector stubs -----------------------------------------------------

%macro ISR_NOERR 1
isr%1:
    push    qword 0           ; dummy error code
    push    qword %1          ; vector number
    jmp     isr_common
%endmacro

%macro ISR_ERR 1
isr%1:
    ; the CPU has already pushed a real error code
    push    qword %1          ; vector number
    jmp     isr_common
%endmacro

%assign i 0
%rep 256
    ; Vectors 8, 10-14, 17, 21 push a hardware error code.
    %if i == 8 || i == 10 || i == 11 || i == 12 || i == 13 || i == 14 || i == 17 || i == 21
        ISR_ERR i
    %else
        ISR_NOERR i
    %endif
    %assign i i+1
%endrep

; ---- common trap path -----------------------------------------------------
isr_common:
    ; If we came from ring 3, swap to the kernel GS base so the invariant
    ; "GS = KPCR while in the kernel" holds regardless of entry path. The saved
    ; CS is at [rsp+24] here: vector(0) error(8) rip(16) cs(24).
    test    byte [rsp + 24], 3
    jz      .from_kernel
    swapgs
.from_kernel:
    ; Save general-purpose registers. Pushed rax-first so that after the last
    ; push (r15) the in-memory order low->high is r15..rax, matching KTRAP_FRAME.
    push    rax
    push    rbx
    push    rcx
    push    rdx
    push    rsi
    push    rdi
    push    rbp
    push    r8
    push    r9
    push    r10
    push    r11
    push    r12
    push    r13
    push    r14
    push    r15

    mov     rdi, rsp          ; arg1 = pointer to the trap frame

    ; Align the stack to 16 bytes for the C call, remembering the frame pointer.
    mov     rbp, rsp
    and     rsp, -16
    call    KiDispatchTrap
    mov     rsp, rbp          ; restore

    ; Restore general-purpose registers.
    pop     r15
    pop     r14
    pop     r13
    pop     r12
    pop     r11
    pop     r10
    pop     r9
    pop     r8
    pop     rbp
    pop     rdi
    pop     rsi
    pop     rdx
    pop     rcx
    pop     rbx
    pop     rax

    add     rsp, 16           ; discard vector + error code

    ; Symmetric to entry: if returning to ring 3, restore the user GS base.
    ; After the add, the saved CS is at [rsp+8]: rip(0) cs(8).
    test    byte [rsp + 8], 3
    jz      .to_kernel
    swapgs
.to_kernel:
    iretq

; ---- table of stub addresses, consumed by KeInitializeIdt ------------------
section .rodata
global isr_stub_table
isr_stub_table:
%assign i 0
%rep 256
    dq  isr %+ i
    %assign i i+1
%endrep
