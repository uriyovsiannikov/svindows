/*
 * ke/ke_main.c - KiSystemStartup, the kernel's C entry point.
 *
 * Reached from the boot trampoline once the CPU is in 64-bit long mode and the
 * kernel is running out of the higher half. Brings up the console, installs the
 * CPU descriptor tables, reports what it found, and idles.
 */
#include <nt/ntdef.h>
#include <nt/ntstatus.h>
#include <ntos/ke.h>
#include <ntos/hal.h>

#define NTOS_VERSION "0.1.0"

static void print_banner(void)
{
    HalVgaSetColor(VGA_COLOR(VGA_LCYAN, VGA_BLACK));
    KeLog("\n");
    KeLog("  NTOS - NT-compatible operating system for x86-64\n");
    KeLog("  version %s\n", NTOS_VERSION);
    HalVgaSetColor(VGA_COLOR(VGA_LGRAY, VGA_BLACK));
    KeLog("  --------------------------------------------------\n\n");
}

/*
 * KiSystemStartup - phase 0 kernel initialization.
 *
 * @magic:    the Multiboot2 bootloader magic reported by GRUB.
 * @mbi_phys: physical address of the Multiboot2 information structure.
 */
void KiSystemStartup(UINT32 magic, UINT32 mbi_phys)
{
    /* Console first, so everything after this is observable. */
    KeLogInit();
    print_banner();

    KeLog("[boot] entered long mode, running in the higher half\n");
    KeLog("[boot] KiSystemStartup at %p\n", (void *)&KiSystemStartup);
    KeLog("[boot] multiboot2 magic=0x%08x info=0x%08x\n", magic, mbi_phys);

    if (magic != MULTIBOOT2_BOOTLOADER_MAGIC) {
        KeLog("[boot] WARNING: unexpected multiboot magic (got 0x%08x, "
              "want 0x%08x)\n", magic, MULTIBOOT2_BOOTLOADER_MAGIC);
    }

    KeLog("[ke]   installing GDT + TSS...\n");
    KeInitializeGdt();

    KeLog("[ke]   installing IDT (256 vectors)...\n");
    KeInitializeIdt();

    KeLog("[ke]   CPU descriptor tables active; traps are now handled.\n");

    /* Quick sanity check of the formatter and arithmetic paths. */
    KeLog("[test] signed=%d unsigned=%u hex=0x%x ptr=%p\n",
          -12345, 42u, 0xDEADBEEFu, (void *)0xFFFFFFFF80000000ULL);

    HalVgaSetColor(VGA_COLOR(VGA_LGREEN, VGA_BLACK));
    KeLog("\n[ok]   phase 0 initialization complete. Halting (idle).\n");
    HalVgaSetColor(VGA_COLOR(VGA_LGRAY, VGA_BLACK));

    /* No scheduler yet: park the boot processor. */
    for (;;)
        __halt();
}
