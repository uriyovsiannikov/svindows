; ============================================================================
; user/testapp.asm - a native NTOS test program.
;
; Demonstrates the ring-3 environment the way a real Windows program sees it:
;   * imports Nt* from ntdll.dll (resolved through the IAT by the loader),
;   * reads its TEB and PEB through the GS segment (gs:[0x30], gs:[0x60]),
;   * allocates memory with NtAllocateVirtualMemory and uses it.
;
; Windows x64 calling convention: first integer argument in RCX; RAX returns.
; ============================================================================
bits 64
default rel

extern NtDisplayString
extern NtDisplayNumber
extern NtTerminateThread
extern NtAllocateVirtualMemory

section .text
global Start
Start:
    and     rsp, -16
    sub     rsp, 32               ; 32-byte shadow space, stays 16-aligned

    lea     rcx, [message]        ; announce ourselves
    call    NtDisplayString

    mov     rcx, [gs:0x30]        ; TEB self-pointer (NtTib.Self)
    call    NtDisplayNumber       ; expect the TEB base

    mov     rax, [gs:0x60]        ; TEB.ProcessEnvironmentBlock
    mov     rcx, [rax + 0x10]     ; PEB.ImageBaseAddress
    call    NtDisplayNumber       ; expect the executable's load base

    mov     ecx, 0x1000           ; allocate one page
    call    NtAllocateVirtualMemory
    mov     edx, 0x0DEADBEE       ; write a marker...
    mov     [rax], rdx
    mov     rcx, [rax]            ; ...and read it back
    call    NtDisplayNumber       ; expect 0x0DEADBEE

    call    NtTerminateThread     ; does not return

.hang:
    jmp     .hang

section .rdata
message:
    db "Native PE: reading TEB/PEB via GS and allocating memory.", 0
