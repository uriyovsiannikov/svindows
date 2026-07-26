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

; Service numbers target Windows 7 SP1 x64 (public NT syscall-number tables);
; the kernel's KiServiceTable is indexed by the same values. Private NTOS-only
; services (no Windows equivalent) sit above the real range at 0xF0+.
SYSCALL_STUB NtWaitForSingleObject,   0x01
SYSCALL_STUB NtSetEvent,              0x02
SYSCALL_STUB NtReadFile,              0x03
SYSCALL_STUB NtWriteFile,             0x05
SYSCALL_STUB NtClose,                 0x0C
SYSCALL_STUB NtOpenKey,               0x0F
SYSCALL_STUB NtAllocateVirtualMemory, 0x15
SYSCALL_STUB NtProtectVirtualMemory,  0x4D
SYSCALL_STUB NtQueryValueKey,         0x17
SYSCALL_STUB NtCreateKey,             0x1A
SYSCALL_STUB NtCreateEvent,           0x48
SYSCALL_STUB NtCreateThreadEx,        0xA5
SYSCALL_STUB NtTerminateThread,       0x50
SYSCALL_STUB NtDelayExecution,        0x31
SYSCALL_STUB NtCreateFile,            0x52
SYSCALL_STUB NtSetValueKey,           0x5D
SYSCALL_STUB NtDisplayString,         0xF0
SYSCALL_STUB NtDisplayNumber,         0xF1
SYSCALL_STUB NtLoadLibrary,           0xF2
