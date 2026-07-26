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
# the kernel reads at runtime: ntdll.dll (exports the Nt* syscall stubs) and
# testapp.exe (imports them).
TESTAPP  := $(BUILD)/testapp.exe
NTDLL    := $(BUILD)/ntdll.dll
NTDLLLIB := $(BUILD)/ntdll.lib
DISK     := $(BUILD)/disk.img
LLDLINK  := lld-link

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
$(NTDLL): user/ntdll.asm user/ntdll.def
	@mkdir -p $(BUILD)
	$(NASM) -f win64 user/ntdll.asm -o $(BUILD)/ntdll.obj
	$(LLDLINK) /dll /noentry /machine:x64 /nodefaultlib /def:user/ntdll.def \
	           /out:$(NTDLL) /implib:$(NTDLLLIB) $(BUILD)/ntdll.obj
	@echo "  DLL   $(NTDLL)"

# testapp.exe: imports Nt* from ntdll (links against the import library).
$(TESTAPP): user/testapp.asm $(NTDLL)
	@mkdir -p $(BUILD)
	$(NASM) -f win64 user/testapp.asm -o $(BUILD)/testapp.obj
	$(LLDLINK) /subsystem:console /entry:Start /nodefaultlib /machine:x64 \
	           /out:$@ $(BUILD)/testapp.obj $(NTDLLLIB)
	@echo "  PE    $@"

# FAT32 disk image holding the user-space executables, read by the kernel's
# ATA + FAT drivers at runtime.
$(DISK): $(TESTAPP) $(NTDLL)
	@mkdir -p $(BUILD)
	dd if=/dev/zero of=$(DISK) bs=1M count=64 status=none
	mformat -i $(DISK) -F -v NTOSDISK ::
	mcopy -i $(DISK) $(TESTAPP) ::TESTAPP.EXE
	mcopy -i $(DISK) $(NTDLL) ::NTDLL.DLL
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
