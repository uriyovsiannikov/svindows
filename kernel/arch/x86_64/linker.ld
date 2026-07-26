/*
 * arch/x86_64/linker.ld - higher-half kernel image layout.
 *
 * The kernel is linked at the virtual base 0xFFFFFFFF80000000 but physically
 * loaded at 0x100000 (1 MiB). The `.boot*` sections that run before paging is
 * enabled are linked at their low physical address (VMA == LMA) so 32-bit code
 * can reference them directly; everything after is linked high with its load
 * address (LMA) set back down into low physical memory via AT().
 */

ENTRY(_start)

KERNEL_VBASE = 0xFFFFFFFF80000000;
KERNEL_LMA   = 0x00100000;

SECTIONS
{
    . = KERNEL_LMA;

    /* Low, identity-addressed boot code/data (VMA == LMA). */
    .boot ALIGN(4K) :
    {
        __boot_start = .;
        KEEP(*(.multiboot))
        *(.boot.text)
        *(.boot.data)
    }

    .boot_bss (NOLOAD) : ALIGN(4K)
    {
        *(.boot.bss)
    }
    __boot_end = .;

    /* Shift into the higher half for the kernel proper. */
    . += KERNEL_VBASE;
    __kernel_start = .;

    .text ALIGN(4K) : AT(ADDR(.text) - KERNEL_VBASE)
    {
        *(.text .text.*)
    }

    .rodata ALIGN(4K) : AT(ADDR(.rodata) - KERNEL_VBASE)
    {
        *(.rodata .rodata.*)
    }

    .data ALIGN(4K) : AT(ADDR(.data) - KERNEL_VBASE)
    {
        *(.data .data.*)
    }

    .bss (NOLOAD) : AT(ADDR(.bss) - KERNEL_VBASE)
    {
        __bss_start = .;
        *(COMMON)
        *(.bss .bss.*)
        __bss_end = .;
    }

    __kernel_end = .;

    /* Physical end of the loaded image, handy for early allocators. */
    __kernel_phys_end = . - KERNEL_VBASE;

    /DISCARD/ :
    {
        *(.eh_frame)
        *(.note .note.*)
        *(.comment)
    }
}
