; ============================================================================
; kernel/ldr/testpe.asm - embed the user-space PE images into the kernel.
;
; build/testapp.exe and build/ntdll.dll are produced from user/*.asm by the
; Makefile (nasm -f win64 + lld-link). Their raw bytes are included here so the
; loader has images to work with before there is a filesystem to read them from.
; ============================================================================
bits 64
section .rodata

global TestappImageStart
global TestappImageEnd
global NtdllImageStart
global NtdllImageEnd

align 16
TestappImageStart:
    incbin "build/testapp.exe"
TestappImageEnd:

align 16
NtdllImageStart:
    incbin "build/ntdll.dll"
NtdllImageEnd:
