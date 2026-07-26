; ============================================================================
; kernel/ldr/testpe.asm - embed the test PE executable into the kernel image.
;
; build/testapp.exe is produced from user/testapp.asm by the Makefile (nasm
; -f win64 + lld-link). Its raw bytes are included here so the PE loader has an
; image to load before there is a filesystem to read one from.
; ============================================================================
bits 64
section .rodata

global TestPeStart
global TestPeEnd

align 16
TestPeStart:
    incbin "build/testapp.exe"
TestPeEnd:
