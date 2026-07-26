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
#include <ntos/mm.h>
#include <ntos/ex.h>
#include <ntos/rtl.h>

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

    /* Phase 1: memory management. */
    MmInitialize((UINT64)mbi_phys);

    /* Sanity-check the new address space: translate a kernel address and a
     * direct-map address back to physical. */
    UINT64 va = (UINT64)&KiSystemStartup;
    KeLog("[test] MmGetPhysicalAddress(%p) = 0x%lx\n",
          (void *)va, (unsigned long)MmGetPhysicalAddress(va));
    KeLog("[test] free physical memory: %lu MiB (%lu frames)\n",
          (unsigned long)((MmFreePageCount() << PAGE_SHIFT) >> 20),
          (unsigned long)MmFreePageCount());

    /* Kernel pool. */
    ExInitializePool();

    /* Exercise the allocator: allocate, use, free, and reuse. */
    void *a = ExAllocatePoolWithTag(NonPagedPool, 128, 'tset');
    void *b = ExAllocatePoolWithTag(NonPagedPool, 4096, 'tset');
    void *c = ExAllocatePoolWithTag(NonPagedPool, 32, 'tset');
    KeLog("[test] pool alloc: a=%p b=%p c=%p (in use %lu bytes)\n",
          a, b, c, (unsigned long)ExPoolBytesInUse());
    memset(a, 0xAB, 128);
    memset(b, 0xCD, 4096);
    ExFreePool(b);
    void *d = ExAllocatePoolWithTag(NonPagedPool, 2048, 'tset');
    KeLog("[test] after free(b), alloc d=%p (in use %lu bytes)\n",
          d, (unsigned long)ExPoolBytesInUse());
    ExFreePool(a);
    ExFreePool(c);
    ExFreePool(d);
    KeLog("[test] after freeing all, in use %lu bytes\n",
          (unsigned long)ExPoolBytesInUse());

    HalVgaSetColor(VGA_COLOR(VGA_LGREEN, VGA_BLACK));
    KeLog("\n[ok]   phase 1 (memory) initialization complete. Halting (idle).\n");
    HalVgaSetColor(VGA_COLOR(VGA_LGRAY, VGA_BLACK));

    /* No scheduler yet: park the boot processor. */
    for (;;)
        __halt();
}
