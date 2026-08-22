# ============================================================================
# NTOS top-level build
#
#   make            build kernel/ntoskrnl.elf (build/ntoskrnl.elf)
#   make iso        build the bootable ISO (build/ntos.iso)
#   make run        boot the ISO in QEMU, kernel log on the serial console
#   make run-gui    same, but keep QEMU's graphical window
#   make clean      remove build artifacts
# ============================================================================

CROSS   ?=
CC      := $(CROSS)gcc
LD      := $(CROSS)ld
NASM    := nasm

BUILD   := build
KERNEL  := $(BUILD)/ntoskrnl.elf
ISO     := $(BUILD)/ntos.iso

.DEFAULT_GOAL := all

# The PE to start after boot.  Keep the default demo, but permit an externally
# supplied, unmodified PE in the project root, for example:
#   make clean && make run PROGRAM=where.exe
# The FAT driver currently supports root-level 8.3 names.
PROGRAM ?= testapp.exe
CMDLINE ?= $(PROGRAM)

# Freestanding, higher-half (-mcmodel=kernel), no FP/SIMD in the kernel, no red
# zone (interrupts run on the same stack), position-dependent.
CFLAGS := -std=gnu11 -ffreestanding \
          -fno-stack-protector -fno-stack-clash-protection \
          -fno-pic -fno-pie -no-pie \
          -mno-red-zone -mcmodel=kernel -mgeneral-regs-only \
          -fno-asynchronous-unwind-tables \
          -Wall -Wextra -Wno-unused-parameter -Wno-multichar \
          -O2 -g -Ikernel/include -DNTOS_BOOT_PROGRAM=\"$(PROGRAM)\" \
          -DNTOS_BOOT_COMMAND_LINE='"$(CMDLINE)"'

NASMFLAGS := -f elf64 -g -F dwarf

LDFLAGS := -n -nostdlib -static -no-pie -z noexecstack \
           -z max-page-size=0x1000 --no-warn-rwx-segments \
           -T kernel/arch/x86_64/linker.ld

C_SRC   := $(shell find kernel -name '*.c')
ASM_SRC := $(shell find kernel -name '*.asm')
OBJ     := $(patsubst %,$(BUILD)/%.o,$(C_SRC) $(ASM_SRC))

# User-space images, built as real PE32+ files and placed on a FAT disk image
# the kernel reads at runtime. The dependency chain is
#   testapp.exe -> kernel32.dll -> ntdll.dll -> syscall
# ntdll (asm) exports the Nt* syscall stubs; kernel32 (C) implements Win32 on
# top of them; testapp (C) is a normal Win32 program.
TESTAPP    := $(BUILD)/testapp.exe
NTDLL      := $(BUILD)/ntdll.dll
NTDLLLIB   := $(BUILD)/ntdll.lib
KERNEL32   := $(BUILD)/kernel32.dll
KERNEL32LIB := $(BUILD)/kernel32.lib
ADVAPI32   := $(BUILD)/advapi32.dll
ADVAPI32LIB := $(BUILD)/advapi32.lib
MSVCRT     := $(BUILD)/msvcrt.dll
MSVCRTLIB  := $(BUILD)/msvcrt.lib
CRT0       := $(BUILD)/crt0.obj
HELLO      := $(BUILD)/hello.exe
EXTRA      := $(BUILD)/extra.dll
WS2_32     := $(BUILD)/ws2_32.dll
DISK       := $(BUILD)/disk.img
LLDLINK    := lld-link
LLVM_DLLTOOL := $(or $(shell command -v llvm-dlltool 2>/dev/null),\
                     $(shell command -v llvm-dlltool-19 2>/dev/null),\
                     llvm-dlltool)
CLANGWIN   := clang --target=x86_64-pc-windows-msvc -ffreestanding \
              -fno-stack-protector -fno-stack-check -O2

# Built samples live under build/, while a real program is supplied at the
# project root.  Add more built-in aliases here as they become boot choices.
ifeq ($(PROGRAM),testapp.exe)
BOOT_IMAGE := $(TESTAPP)
else ifeq ($(PROGRAM),hello.exe)
BOOT_IMAGE := $(HELLO)
else
BOOT_IMAGE := $(PROGRAM)
endif

# Keep externally supplied PE test programs available to the program being
# booted.  This lets utilities such as where.exe inspect another real Windows
# executable without requiring a special copy rule for every new sample.
EXTRA_PE_IMAGES := $(filter-out $(BOOT_IMAGE),$(sort $(wildcard *.exe) $(wildcard *.EXE)))
WIN_FILES       := $(sort $(wildcard win/*))

QEMU        := qemu-system-x86_64
# -boot d forces booting from the CD-ROM (the ISO); the hard disk is data only.
QEMUFLAGS   := -m 256M -no-reboot -no-shutdown -boot d
QEMUDISK    := -drive file=$(DISK),format=raw,if=ide,index=0,media=disk

.PHONY: all iso run run-gui clean FORCE

FORCE:

all: $(KERNEL)

$(BUILD)/%.c.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

# PROGRAM is supplied through CFLAGS, which make does not otherwise track.
# Recompile just the boot selector so switching PROGRAM never reuses a kernel
# containing the previous executable name.
$(BUILD)/kernel/ke/ke_main.c.o: kernel/ke/ke_main.c FORCE
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/%.asm.o: %.asm
	@mkdir -p $(dir $@)
	$(NASM) $(NASMFLAGS) $< -o $@

# ntdll.dll: the syscall-stub library, with an export table + import library.
# Each user DLL gets a distinct preferred base so the loader (which loads at the
# preferred base) never has two images collide. ntdllrtl.c provides the C side
# (Rtl heap, critical sections, string helpers) real binaries import.
$(NTDLL): user/ntdll.asm user/ntdll.def user/ntdllrtl.c
	@mkdir -p $(BUILD)
	$(NASM) -f win64 user/ntdll.asm -o $(BUILD)/ntdll.obj
	$(CLANGWIN) -c user/ntdllrtl.c -o $(BUILD)/ntdllrtl.obj
	$(LLDLINK) /dll /noentry /machine:x64 /nodefaultlib /base:0x180000000 \
	           /def:user/ntdll.def /out:$(NTDLL) /implib:$(NTDLLLIB) \
	           $(BUILD)/ntdll.obj $(BUILD)/ntdllrtl.obj
	@echo "  DLL   $(NTDLL)"

# kernel32.dll: the Win32 subsystem library, built from C over ntdll.
$(KERNEL32): user/kernel32.c $(NTDLL)
	@mkdir -p $(BUILD)
	$(CLANGWIN) -c user/kernel32.c -o $(BUILD)/kernel32.obj
	$(LLDLINK) /dll /noentry /machine:x64 /nodefaultlib /base:0x1C0000000 \
	           /out:$(KERNEL32) /implib:$(KERNEL32LIB) \
	           $(BUILD)/kernel32.obj $(NTDLLLIB)
	@echo "  DLL   $(KERNEL32)"

# advapi32.dll: the registry Reg* API (C), built over ntdll's Nt*Key services.
$(ADVAPI32): user/advapi32.c $(NTDLL)
	@mkdir -p $(BUILD)
	$(CLANGWIN) -c user/advapi32.c -o $(BUILD)/advapi32.obj
	$(LLDLINK) /dll /noentry /machine:x64 /nodefaultlib /base:0x1D0000000 \
	           /out:$(ADVAPI32) /implib:$(ADVAPI32LIB) \
	           $(BUILD)/advapi32.obj $(NTDLLLIB)
	@echo "  DLL   $(ADVAPI32)"

# msvcrt.dll: a tiny C runtime (printf/malloc/str*) over kernel32.
$(MSVCRT): user/msvcrt.c $(KERNEL32)
	@mkdir -p $(BUILD)
	$(CLANGWIN) -c user/msvcrt.c -o $(BUILD)/msvcrt.obj
	$(LLDLINK) /dll /noentry /machine:x64 /nodefaultlib /base:0x1E0000000 \
	           /out:$(MSVCRT) /implib:$(MSVCRTLIB) \
	           $(BUILD)/msvcrt.obj $(KERNEL32LIB)
	@echo "  DLL   $(MSVCRT)"

# extra.dll: a standalone DLL loaded at runtime via LoadLibraryA (not linked
# into the app's import chain). Distinct preferred base so it never collides.
$(EXTRA): user/extra.c
	@mkdir -p $(BUILD)
	$(CLANGWIN) -c user/extra.c -o $(BUILD)/extra.obj
	$(LLDLINK) /dll /noentry /machine:x64 /nodefaultlib /base:0x200000000 \
	           /out:$(EXTRA) /implib:$(BUILD)/extra.lib $(BUILD)/extra.obj
	@echo "  DLL   $(EXTRA)"

# Minimal Winsock facade used by real console utilities.  It intentionally
# starts with startup + hostname only; import logs expose the next API needed.
$(WS2_32): user/ws2_32.c user/ws2_32.def $(KERNEL32)
	@mkdir -p $(BUILD)
	$(CLANGWIN) -c user/ws2_32.c -o $(BUILD)/ws2_32.obj
	$(LLDLINK) /dll /noentry /machine:x64 /nodefaultlib /base:0x210000000 \
	           /def:user/ws2_32.def /out:$(WS2_32) \
	           $(BUILD)/ws2_32.obj $(KERNEL32LIB)
	@echo "  DLL   $(WS2_32)"

# crt0.obj: the C runtime startup (mainCRTStartup). Linked into every program
# so a standard `int main()` is entered without a custom /entry.
$(CRT0): user/crt0.c
	@mkdir -p $(BUILD)
	$(CLANGWIN) -c user/crt0.c -o $(CRT0)

# stubtest.lib: an import library (built by llvm-dlltool from a .def) that
# promises a kernel32 export the real kernel32.dll does not provide, so testapp
# can exercise the loader's unimplemented-import stubbing.
STUBTESTLIB := $(BUILD)/stubtest.lib
$(STUBTESTLIB): user/stubtest.def
	@mkdir -p $(BUILD)
	$(LLVM_DLLTOOL) -m i386:x86-64 -d user/stubtest.def -l $(STUBTESTLIB) -D kernel32.dll

# testapp.exe: a Win32 program (C) entered through the CRT startup (int main),
# linked against kernel32 + advapi32 + msvcrt (+ the stubtest import lib).
$(TESTAPP): user/testapp.c $(CRT0) $(KERNEL32) $(ADVAPI32) $(MSVCRT) $(STUBTESTLIB)
	@mkdir -p $(BUILD)
	$(CLANGWIN) -c user/testapp.c -o $(BUILD)/testapp.obj
	$(LLDLINK) /subsystem:console /nodefaultlib /machine:x64 \
	           /out:$@ $(BUILD)/testapp.obj $(CRT0) $(KERNEL32LIB) \
	           $(ADVAPI32LIB) $(MSVCRTLIB) $(STUBTESTLIB)
	@echo "  PE    $@"

# hello.exe: a plain portable C program (int main, stdio/stdlib/string), built
# the ordinary way -- no custom entry, no OS-specific code.
$(HELLO): user/hello.c $(CRT0) $(KERNEL32) $(MSVCRT)
	@mkdir -p $(BUILD)
	$(CLANGWIN) -Iuser/include -c user/hello.c -o $(BUILD)/hello.obj
	$(LLDLINK) /subsystem:console /nodefaultlib /machine:x64 \
	           /out:$@ $(BUILD)/hello.obj $(CRT0) $(KERNEL32LIB) $(MSVCRTLIB)
	@echo "  PE    $@"

# FAT32 disk image holding the user-space executables, read by the kernel's
# ATA + FAT drivers at runtime.
$(DISK): FORCE $(TESTAPP) $(HELLO) $(KERNEL32) $(NTDLL) $(ADVAPI32) $(MSVCRT) $(EXTRA) $(WS2_32) user/message.txt $(BOOT_IMAGE) $(EXTRA_PE_IMAGES) $(WIN_FILES)
	@mkdir -p $(BUILD)
	dd if=/dev/zero of=$(DISK) bs=1M count=96 status=none
	mformat -i $(DISK) -F -v NTOSDISK ::
	mcopy -i $(DISK) $(TESTAPP) ::TESTAPP.EXE
	mcopy -i $(DISK) $(HELLO) ::HELLO.EXE
	mcopy -i $(DISK) $(KERNEL32) ::KERNEL32.DLL
	mcopy -i $(DISK) $(NTDLL) ::NTDLL.DLL
	mcopy -i $(DISK) $(ADVAPI32) ::ADVAPI32.DLL
	mcopy -i $(DISK) $(MSVCRT) ::MSVCRT.DLL
	mcopy -i $(DISK) $(EXTRA) ::EXTRA.DLL
	mcopy -i $(DISK) $(WS2_32) ::WS2_32.DLL
	if [ "$(PROGRAM)" != "testapp.exe" ]; then mcopy -o -i $(DISK) $(BOOT_IMAGE) ::$(PROGRAM); fi
	$(foreach image,$(EXTRA_PE_IMAGES),mcopy -i $(DISK) $(image) ::$(notdir $(image));)
	$(foreach file,$(WIN_FILES),mcopy -o -i $(DISK) $(file) ::$(notdir $(file));)
	mcopy -i $(DISK) user/message.txt ::MESSAGE.TXT
	@echo "  DISK  $(DISK)"

$(KERNEL): $(OBJ) kernel/arch/x86_64/linker.ld
	@mkdir -p $(dir $@)
	$(LD) $(LDFLAGS) -o $@ $(OBJ)
	@echo "  LD    $@"

iso: $(ISO)

$(ISO): $(KERNEL) boot/grub.cfg
	@mkdir -p $(BUILD)/isodir/boot/grub
	@cp $(KERNEL) $(BUILD)/isodir/boot/ntoskrnl.elf
	@cp boot/grub.cfg $(BUILD)/isodir/boot/grub/grub.cfg
	grub2-mkrescue -o $@ $(BUILD)/isodir 2>/dev/null
	@echo "  ISO   $@"

run: $(ISO) $(DISK)
	$(QEMU) $(QEMUFLAGS) -cdrom $(ISO) $(QEMUDISK) -serial stdio -display none

run-gui: $(ISO) $(DISK)
	$(QEMU) $(QEMUFLAGS) -cdrom $(ISO) $(QEMUDISK) -serial stdio

clean:
	rm -rf $(BUILD)
