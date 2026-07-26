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

# Freestanding, higher-half (-mcmodel=kernel), no FP/SIMD in the kernel, no red
# zone (interrupts run on the same stack), position-dependent.
CFLAGS := -std=gnu11 -ffreestanding \
          -fno-stack-protector -fno-stack-clash-protection \
          -fno-pic -fno-pie -no-pie \
          -mno-red-zone -mcmodel=kernel -mgeneral-regs-only \
          -fno-asynchronous-unwind-tables \
          -Wall -Wextra -Wno-unused-parameter -Wno-multichar \
          -O2 -g -Ikernel/include

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
DISK       := $(BUILD)/disk.img
LLDLINK    := lld-link
CLANGWIN   := clang --target=x86_64-pc-windows-msvc -ffreestanding \
              -fno-stack-protector -fno-stack-check -O2

QEMU        := qemu-system-x86_64
# -boot d forces booting from the CD-ROM (the ISO); the hard disk is data only.
QEMUFLAGS   := -m 256M -no-reboot -no-shutdown -boot d
QEMUDISK    := -drive file=$(DISK),format=raw,if=ide,index=0,media=disk

.PHONY: all iso run run-gui clean

all: $(KERNEL)

$(BUILD)/%.c.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/%.asm.o: %.asm
	@mkdir -p $(dir $@)
	$(NASM) $(NASMFLAGS) $< -o $@

# ntdll.dll: the syscall-stub library, with an export table + import library.
# Each user DLL gets a distinct preferred base so the loader (which loads at the
# preferred base) never has two images collide.
$(NTDLL): user/ntdll.asm user/ntdll.def
	@mkdir -p $(BUILD)
	$(NASM) -f win64 user/ntdll.asm -o $(BUILD)/ntdll.obj
	$(LLDLINK) /dll /noentry /machine:x64 /nodefaultlib /base:0x180000000 \
	           /def:user/ntdll.def /out:$(NTDLL) /implib:$(NTDLLLIB) \
	           $(BUILD)/ntdll.obj
	@echo "  DLL   $(NTDLL)"

# kernel32.dll: the Win32 subsystem library, built from C over ntdll.
$(KERNEL32): user/kernel32.c $(NTDLL)
	@mkdir -p $(BUILD)
	$(CLANGWIN) -c user/kernel32.c -o $(BUILD)/kernel32.obj
	$(LLDLINK) /dll /noentry /machine:x64 /nodefaultlib /base:0x1C0000000 \
	           /out:$(KERNEL32) /implib:$(KERNEL32LIB) \
	           $(BUILD)/kernel32.obj $(NTDLLLIB)
	@echo "  DLL   $(KERNEL32)"

# testapp.exe: a normal Win32 program (C), linked against kernel32.
$(TESTAPP): user/testapp.c $(KERNEL32)
	@mkdir -p $(BUILD)
	$(CLANGWIN) -c user/testapp.c -o $(BUILD)/testapp.obj
	$(LLDLINK) /subsystem:console /entry:Start /nodefaultlib /machine:x64 \
	           /out:$@ $(BUILD)/testapp.obj $(KERNEL32LIB)
	@echo "  PE    $@"

# FAT32 disk image holding the user-space executables, read by the kernel's
# ATA + FAT drivers at runtime.
$(DISK): $(TESTAPP) $(KERNEL32) $(NTDLL) user/message.txt
	@mkdir -p $(BUILD)
	dd if=/dev/zero of=$(DISK) bs=1M count=64 status=none
	mformat -i $(DISK) -F -v NTOSDISK ::
	mcopy -i $(DISK) $(TESTAPP) ::TESTAPP.EXE
	mcopy -i $(DISK) $(KERNEL32) ::KERNEL32.DLL
	mcopy -i $(DISK) $(NTDLL) ::NTDLL.DLL
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
	grub-mkrescue -o $@ $(BUILD)/isodir 2>/dev/null
	@echo "  ISO   $@"

run: $(ISO) $(DISK)
	$(QEMU) $(QEMUFLAGS) -cdrom $(ISO) $(QEMUDISK) -serial stdio -display none

run-gui: $(ISO) $(DISK)
	$(QEMU) $(QEMUFLAGS) -cdrom $(ISO) $(QEMUDISK) -serial stdio

clean:
	rm -rf $(BUILD)
