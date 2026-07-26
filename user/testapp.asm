; ============================================================================
; user/testapp.asm - a native NTOS test program exercising handle-based file I/O.
;
; Opens the console and a file on the FAT disk through NtCreateFile, reads the
; file with NtReadFile, echoes its contents to the console with NtWriteFile, and
; closes both handles - the same object/handle model Windows uses.
;
; Windows x64 calling convention: args in RCX, RDX, R8, R9; RAX returns; callers
; reserve 32 bytes of shadow space. RBX/R12-R15 are non-volatile, so handles and
; buffers kept there survive the syscalls.
; ============================================================================
bits 64
default rel

extern NtCreateFile
extern NtReadFile
extern NtWriteFile
extern NtClose
extern NtAllocateVirtualMemory
extern NtTerminateThread

section .text
global Start
Start:
    and     rsp, -16
    sub     rsp, 32               ; shadow space, 16-aligned

    ; console = NtCreateFile("\Device\Console")
    lea     rcx, [console_name]
    call    NtCreateFile
    mov     rbx, rax              ; rbx = console handle

    ; NtWriteFile(console, greeting, greeting_len)
    mov     rcx, rbx
    lea     rdx, [greeting]
    mov     r8d, greeting_len
    call    NtWriteFile

    ; buffer = NtAllocateVirtualMemory(512)
    mov     ecx, 512
    call    NtAllocateVirtualMemory
    mov     r13, rax              ; r13 = read buffer

    ; file = NtCreateFile("message.txt")
    lea     rcx, [file_name]
    call    NtCreateFile
    mov     r12, rax              ; r12 = file handle

    ; bytes = NtReadFile(file, buffer, 512)
    mov     rcx, r12
    mov     rdx, r13
    mov     r8d, 512
    call    NtReadFile
    mov     r14, rax              ; r14 = bytes read

    ; NtWriteFile(console, buffer, bytes) - echo the file to the console
    mov     rcx, rbx
    mov     rdx, r13
    mov     r8, r14
    call    NtWriteFile

    ; close both handles
    mov     rcx, r12
    call    NtClose
    mov     rcx, rbx
    call    NtClose

    call    NtTerminateThread     ; does not return

.hang:
    jmp     .hang

section .rdata
console_name:
    db "\Device\Console", 0
file_name:
    db "message.txt", 0
greeting:
    db "testapp.exe: reading a file through NT handles ->", 10
greeting_len equ $ - greeting
