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

; Process bootstrap used by the kernel loader. RCX points to:
;   +0  final executable entry
;   +8  DLL count
;   +16 repeated { module base, DLL entry point }
; Dependencies are supplied in initialization order. Each entry is called as
; DllMain(base, DLL_PROCESS_ATTACH, NULL), then control jumps to the EXE entry
; with the original process-entry stack restored.
extern LdrpRegisterWowHandlers

global LdrInitializeProcess
LdrInitializeProcess:
    push    rbx
    push    rsi
    push    rdi
    mov     rbx, rcx
    mov     rsi, [rbx + 8]
    lea     rdi, [rbx + 16]
.dll_loop:
    test    rsi, rsi
    jz      .start_exe
    mov     rax, [rdi + 8]
    test    rax, rax
    jz      .next_dll
    mov     rcx, [rdi]
    mov     edx, 1              ; DLL_PROCESS_ATTACH
    xor     r8d, r8d
    sub     rsp, 0x20           ; Win64 shadow space; stack stays aligned
    call    rax
    add     rsp, 0x20
    test    eax, eax
    jnz     .next_dll
    ; A failed DLL_PROCESS_ATTACH is fatal on Windows. Log the failing module
    ; base while the bootstrap still continues, so dependency bring-up can be
    ; diagnosed without losing the original fault site.
    mov     rcx, [rdi]
    mov     r10, rcx
    mov     eax, 0xF1           ; NTOS-private NtDisplayNumber
    syscall
.next_dll:
    add     rdi, 16
    dec     rsi
    jmp     .dll_loop
.start_exe:
    ; With no win32k, nobody performs user32's WOW-handler registration
    ; handshake; do it here so RegisterClassW passes its validation.
    call    LdrpRegisterWowHandlers
    mov     rax, [rbx]
    pop     rdi
    pop     rsi
    pop     rbx
    xor     ecx, ecx
    jmp     rax

; PIMAGE_NT_HEADERS RtlImageNtHeader(PVOID Base)
global RtlImageNtHeader
RtlImageNtHeader:
    xor     eax, eax
    test    rcx, rcx
    jz      .image_done
    cmp     word [rcx], 0x5a4d       ; MZ
    jne     .image_done
    movsxd  rdx, dword [rcx + 0x3c]
    test    rdx, rdx
    js      .image_done
    add     rcx, rdx
    cmp     dword [rcx], 0x00004550  ; PE\0\0
    jne     .image_done
    mov     rax, rcx
.image_done:
    ret

; USER32 registers three client callback tables during process attach and then
; retrieves them through these ntdll helpers. Keeping the pointers in ntdll is
; sufficient for the single-process model and lets the real USER32 finish its
; internal function-table initialization.
global RtlInitializeNtUserPfn
RtlInitializeNtUserPfn:
    mov     [rel ntuser_pfn1], rcx
    mov     [rel ntuser_pfn2], r8
    mov     rax, [rsp + 0x28]       ; fifth argument: third table
    mov     [rel ntuser_pfn3], rax
    xor     eax, eax                ; STATUS_SUCCESS
    ret

global RtlRetrieveNtUserPfn
RtlRetrieveNtUserPfn:
    test    rcx, rcx
    jz      .retrieve_second
    mov     rax, [rel ntuser_pfn1]
    mov     [rcx], rax
.retrieve_second:
    test    rdx, rdx
    jz      .retrieve_third
    mov     rax, [rel ntuser_pfn2]
    mov     [rdx], rax
.retrieve_third:
    test    r8, r8
    jz      .retrieve_done
    mov     rax, [rel ntuser_pfn3]
    mov     [r8], rax
.retrieve_done:
    xor     eax, eax
    ret

global RtlResetNtUserPfn
RtlResetNtUserPfn:
    mov     qword [rel ntuser_pfn1], 0
    mov     qword [rel ntuser_pfn2], 0
    mov     qword [rel ntuser_pfn3], 0
    xor     eax, eax
    ret

; USER32 forwards DefWindowProcA/W here (NTDLL.NtdllDefWindowProc_W). The real
; routine answers the few messages whose defaults live in ntdll (the CTLCOLOR
; family returns system brushes); every other message takes the standard
; "not handled" result of zero, which is what an empty USER system should
; report until win32k exists.
global NtdllDefWindowProc_W
NtdllDefWindowProc_W:
global NtdllDefWindowProc_A
NtdllDefWindowProc_A:
    xor     eax, eax
    ret

; USER32 connects to the user CSR server during DLL_PROCESS_ATTACH and asks it
; to fill a 0x240-byte USERCONNECT block.  On Windows the first member of that
; block is a pointer to the read-only SERVERINFO shared by win32k and USER32.
; We do not have CSRSS/win32k yet, but returning a real, stable zero-initialized
; SERVERINFO is enough to establish the ABI correctly (and is deliberately not
; a fake desktop implementation).  The kernel-side USER subsystem will grow
; this shared page as individual fields become necessary.
;
; NTSTATUS CsrClientConnectToServer(
;     PWSTR ObjectDirectory, ULONG ServerId, PVOID ConnectionInfo,
;     PULONG ConnectionInfoLength, PBOOLEAN CalledFromServer)
global CsrClientConnectToServer
CsrClientConnectToServer:
    test    r8, r8
    jz      .csr_invalid_parameter
    lea     rax, [rel ntuser_server_info]
    ; The first eight bytes are the CSR connection header. USER32 copies the
    ; build-specific 0x238-byte payload starting at ConnectionInfo+8; its first
    ; payload member is psi (the shared SERVERINFO pointer).
    mov     [r8 + 8], rax
    ; SHAREDINFO.aheList and HeEntrySize. PsCreateUserProcess maps the native
    ; 24-byte USER HANDLEENTRY array at this fixed client-visible address.
    mov     rax, 0x0000000001180000
    mov     [r8 + 16], rax
    mov     dword [r8 + 24], 24
    ; USER32 supplied a writable 0x240-byte block which was already cleared by
    ; its caller.  Preserve the remaining build-specific fields as zero.
    xor     eax, eax                ; STATUS_SUCCESS
    ret
.csr_invalid_parameter:
    mov     eax, 0xc000000d         ; STATUS_INVALID_PARAMETER
    ret

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
SYSCALL_STUB NtQuerySystemInformation,0x33
SYSCALL_STUB NtCreateFile,            0x52
SYSCALL_STUB NtSetValueKey,           0x5D
SYSCALL_STUB NtDisplayString,         0xF0
SYSCALL_STUB NtDisplayNumber,         0xF1
SYSCALL_STUB NtLoadLibrary,           0xF2
SYSCALL_STUB NtEnumerateRootFiles,    0xF3
SYSCALL_STUB NtQueryFileInfo,         0xF4
SYSCALL_STUB NtSetFilePosition,       0xF5
SYSCALL_STUB NtWaitForMultipleObjects,0xF6
SYSCALL_STUB NtResetEvent,            0xF7
SYSCALL_STUB NtCreateSemaphore,       0xF8
SYSCALL_STUB NtReleaseSemaphore,      0xF9
SYSCALL_STUB NtQueryInformationProcess,0xFA

section .data
align 8
ntuser_pfn1: dq 0
ntuser_pfn2: dq 0
ntuser_pfn3: dq 0
align 16
; Initial SERVERINFO prefix. USER32 currently reads byte 0 (feature flags);
; reserve a full page so later compatible fields can be populated in place.
ntuser_server_info: times 4096 db 0
