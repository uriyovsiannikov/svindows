; ============================================================================
; user/testapp.asm - a native NTOS test program that imports from ntdll.
;
; Unlike the earlier version, this calls the Nt* functions provided by ntdll.dll
; using the Windows x64 calling convention (first argument in RCX). The linker
; records these as imports (an import directory + IAT); the NTOS PE loader
; resolves the IAT against the loaded ntdll before the program runs.
; ============================================================================
bits 64
default rel

extern NtDisplayString
extern NtDisplayNumber
extern NtTerminateThread

section .text
global Start
Start:
    sub     rsp, 40               ; 32-byte shadow space + 16-byte alignment

    lea     rcx, [message]        ; arg1 in RCX (Windows calling convention)
    call    NtDisplayString

    mov     ecx, 0x00ABCDEF       ; arg1
    call    NtDisplayNumber

    call    NtTerminateThread     ; does not return

.hang:
    jmp     .hang

section .rdata
message:
    db "Hello from a PE .exe, calling Nt* through ntdll imports!", 0
