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
          -Wall -Wextra -Wno-unused-parameter \
          -O2 -g -Ikernel/include

NASMFLAGS := -f elf64 -g -F dwarf

LDFLAGS := -n -nostdlib -static -no-pie -z noexecstack \
           -z max-page-size=0x1000 --no-warn-rwx-segments \
           -T kernel/arch/x86_64/linker.ld

C_SRC   := $(shell find kernel -name '*.c')
ASM_SRC := $(shell find kernel -name '*.asm')
OBJ     := $(patsubst %,$(BUILD)/%.o,$(C_SRC) $(ASM_SRC))

QEMU        := qemu-system-x86_64
QEMUFLAGS   := -m 256M -no-reboot -no-shutdown

.PHONY: all iso run run-gui clean

all: $(KERNEL)

$(BUILD)/%.c.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/%.asm.o: %.asm
	@mkdir -p $(dir $@)
	$(NASM) $(NASMFLAGS) $< -o $@

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

run: $(ISO)
	$(QEMU) $(QEMUFLAGS) -cdrom $(ISO) -serial stdio -display none

run-gui: $(ISO)
	$(QEMU) $(QEMUFLAGS) -cdrom $(ISO) -serial stdio

clean:
	rm -rf $(BUILD)
