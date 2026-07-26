; ============================================================================
; arch/x86_64/boot.asm - Multiboot2 entry and the 32-bit -> 64-bit trampoline.
;
; GRUB loads this ELF64 image and enters `_start` in 32-bit protected mode with
; paging disabled. We then:
;   1. save the Multiboot2 magic + info pointer,
;   2. build boot page tables that map the low 1 GiB both identity-mapped and
;      at the kernel's higher-half base (-2 GiB),
;   3. enable PAE, set EFER.LME, turn on paging  -> IA-32e mode,
;   4. load a 64-bit GDT and far-jump into 64-bit code,
;   5. jump to the higher-half virtual address and call KiSystemStartup.
; ============================================================================

KERNEL_VBASE      equ 0xFFFFFFFF80000000

; Multiboot2 header constants.
MB2_MAGIC         equ 0xE85250D6
MB2_ARCH_I386     equ 0

; Page/table flags.
PTE_PRESENT       equ 0x1
PTE_WRITE         equ 0x2
PTE_HUGE          equ 0x80          ; 2 MiB page (PS bit) in a PD entry

; ---------------------------------------------------------------------------
; Multiboot2 header - must be 8-byte aligned and within the first 32 KiB.
; ---------------------------------------------------------------------------
section .multiboot progbits alloc noexec nowrite align=8
mb_header_start:
    dd  MB2_MAGIC
    dd  MB2_ARCH_I386
    dd  mb_header_end - mb_header_start
    dd  -(MB2_MAGIC + MB2_ARCH_I386 + (mb_header_end - mb_header_start))

    ; Framebuffer request tag (type = 5): ask GRUB for a linear framebuffer.
    align 8
    dw  5                       ; type = framebuffer
    dw  1                       ; flags: optional (fall back if unavailable)
    dd  20                      ; size
    dd  1024                    ; preferred width
    dd  768                     ; preferred height
    dd  32                      ; preferred bits per pixel

    ; End tag (type = 0, size = 8).
    align 8
    dw  0
    dw  0
    dd  8
mb_header_end:

; ---------------------------------------------------------------------------
; 32-bit trampoline.
; ---------------------------------------------------------------------------
section .boot.text progbits alloc exec nowrite align=16
bits 32
global _start
extern KiSystemStartup

_start:
    cld
    cli
    mov     esp, boot_stack_top       ; a stack in low identity memory

    ; Preserve what GRUB handed us (eax = magic, ebx = info pointer).
    mov     [saved_magic], eax
    mov     [saved_mbi], ebx

    ; --- Zero the three boot paging structures. ------------------------------
    mov     edi, boot_pml4
    xor     eax, eax
    mov     ecx, (3 * 4096) / 4
    rep     stosd

    ; --- Top-level entries. --------------------------------------------------
    ; PML4[0]   -> PDPT   (identity, low canonical half)
    ; PML4[511] -> PDPT   (kernel higher half, -2 GiB)
    mov     eax, boot_pdpt
    or      eax, PTE_PRESENT | PTE_WRITE
    mov     [boot_pml4 + 0 * 8], eax
    mov     [boot_pml4 + 511 * 8], eax

    ; PDPT[0]   -> PD     (identity   0..1 GiB)
    ; PDPT[510] -> PD     (higher half maps to PDPT index 510)
    mov     eax, boot_pd
    or      eax, PTE_PRESENT | PTE_WRITE
    mov     [boot_pdpt + 0 * 8], eax
    mov     [boot_pdpt + 510 * 8], eax

    ; --- Fill the PD with 512 * 2 MiB identity pages (0 .. 1 GiB). -----------
    mov     edi, boot_pd
    mov     eax, PTE_PRESENT | PTE_WRITE | PTE_HUGE   ; phys 0 | flags
    mov     ecx, 512
.fill_pd:
    mov     [edi], eax                ; low dword: phys | flags
    mov     dword [edi + 4], 0        ; high dword: 0 for the low 1 GiB
    add     eax, 0x200000             ; next 2 MiB frame
    add     edi, 8
    loop    .fill_pd

    ; --- Turn on long mode. --------------------------------------------------
    mov     eax, boot_pml4
    mov     cr3, eax

    mov     eax, cr4
    or      eax, 1 << 5               ; CR4.PAE
    mov     cr4, eax

    mov     ecx, 0xC0000080           ; IA32_EFER
    rdmsr
    or      eax, 1 << 8               ; EFER.LME
    wrmsr

    mov     eax, cr0
    or      eax, 1 << 31              ; CR0.PG
    mov     cr0, eax                  ; paging on -> IA-32e compatibility mode

    lgdt    [gdt64_ptr]
    jmp     GDT64_CODE:long_mode_start

; ---------------------------------------------------------------------------
; 64-bit stub, still executing in low identity memory.
; ---------------------------------------------------------------------------
bits 64
long_mode_start:
    mov     ax, GDT64_DATA
    mov     ds, ax
    mov     es, ax
    mov     ss, ax
    mov     fs, ax
    mov     gs, ax

    ; Absolute (64-bit) jump into the higher half.
    mov     rax, qword higher_half_start
    jmp     rax

; ---------------------------------------------------------------------------
; Boot data: GDT + saved bootloader hand-off values (low identity memory).
; ---------------------------------------------------------------------------
section .boot.data progbits alloc write align=16

align 16
gdt64:
    dq  0                              ; null descriptor
GDT64_CODE equ 0x08
    dq  0x00AF9A000000FFFF             ; 64-bit code: P,S,exec,read, L=1
GDT64_DATA equ 0x10
    dq  0x00AF92000000FFFF             ; data: P,S,read/write
gdt64_ptr:
    dw  gdt64_ptr - gdt64 - 1
    dq  gdt64

saved_magic: dd 0
saved_mbi:   dd 0

; ---------------------------------------------------------------------------
; Boot BSS: page tables + the trampoline stack (low identity memory).
; ---------------------------------------------------------------------------
section .boot.bss nobits alloc write align=4096
global boot_pml4
boot_pml4:  resb 4096
boot_pdpt:  resb 4096
boot_pd:    resb 4096

align 16
boot_stack:      resb 4096
boot_stack_top:

; ---------------------------------------------------------------------------
; Higher-half 64-bit entry (linked at the kernel's virtual base).
; ---------------------------------------------------------------------------
section .text progbits alloc exec nowrite align=16
bits 64
global higher_half_start
higher_half_start:
    lea     rsp, [rel kernel_stack_top]    ; switch to the real kernel stack
    xor     rbp, rbp                        ; terminate call-chain backtraces

    mov     edi, [saved_magic]              ; arg1: Multiboot2 magic
    mov     esi, [saved_mbi]                ; arg2: Multiboot2 info (phys)
    call    KiSystemStartup

.hang:
    cli
    hlt
    jmp     .hang

; ---------------------------------------------------------------------------
; Kernel stack (higher-half BSS).
; ---------------------------------------------------------------------------
section .bss nobits alloc write align=16
global kernel_stack
kernel_stack:
    resb    0x4000                          ; 16 KiB
kernel_stack_top:
