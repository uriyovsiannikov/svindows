; ============================================================================
; user/ntdll.asm - the NTOS user-mode system library (ntdll.dll).
;
; Each exported Nt* function is the canonical Windows syscall stub: move the
; first argument from RCX (Windows calling convention) to R10 (because SYSCALL
; clobbers RCX), load the service number into EAX, and issue SYSCALL. The kernel
; returns via SYSRET with the status in RAX.
;
; Built into a real ntdll.dll (with an export table) by the Makefile; user
; programs import these instead of emitting raw syscalls.
; ============================================================================
bits 64
default rel

section .text

%macro SYSCALL_STUB 2   ; %1 = name, %2 = service number
global %1
%1:
    mov     r10, rcx
    mov     eax, %2
    syscall
    ret
%endmacro

SYSCALL_STUB NtDisplayString,         0
SYSCALL_STUB NtDisplayNumber,         1
SYSCALL_STUB NtTerminateThread,       2
SYSCALL_STUB NtAllocateVirtualMemory, 3
SYSCALL_STUB NtCreateFile,            4
SYSCALL_STUB NtReadFile,              5
SYSCALL_STUB NtWriteFile,             6
SYSCALL_STUB NtClose,                 7
