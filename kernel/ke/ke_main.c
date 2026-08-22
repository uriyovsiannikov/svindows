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
#include <ntos/ob.h>
#include <ntos/ldr.h>
#include <ntos/ps.h>
#include <ntos/io.h>
#include <ntos/cm.h>
#include <ntos/gfx.h>
#include <ntos/input.h>
#include <ntos/rtl.h>

#define NTOS_VERSION "0.6.0"

/* Set by the top-level Makefile (PROGRAM=...).  This keeps the boot path the
 * same for built-in test PEs and a real PE supplied in the repository root. */
#ifndef NTOS_BOOT_PROGRAM
#define NTOS_BOOT_PROGRAM "testapp.exe"
#endif
#ifndef NTOS_BOOT_COMMAND_LINE
#define NTOS_BOOT_COMMAND_LINE NTOS_BOOT_PROGRAM
#endif

/* User stack for the loaded program (grows down from the top). */
#define USER_STACK_TOP   0x0000000010010000ULL
#define USER_STACK_PAGES 16

/* A demo kernel thread: print a few iterations with busy work in between so the
 * timer preempts it and it interleaves with the user program. */
static void DemoWorker(PVOID context)
{
    const char *name = (const char *)context;
    for (int i = 0; i < 3; i++) {
        KeLog("   [kthread %s] iteration %d at tick %lu\n",
              name, i, (unsigned long)KeGetTickCount());
        for (volatile UINT64 spin = 0; spin < 8000000ULL; spin++)
            ;
    }
    KeLog("   [kthread %s] finished\n", name);
}

/* The input thread keeps the hardware queues alive for the future USER32 input
 * path. The framebuffer cursor remains a low-level pointer primitive, not a
 * kernel-drawn shell or desktop. */
static void InputWorker(PVOID context)
{
    (void)context;

    UINT32 last_seq = (UINT32)-1;

    KeLog("[input] keyboard + mouse ready; move the mouse and type.\n");

    for (;;) {
        /* Echo any typed characters. */
        while (KbdDataAvailable()) {
            char c = KbdReadChar();
            if (c) {
                KeLog("%c", c);
                KiUserQueueCharacter((UINT8)c);
            }
        }

        /* Redraw the cursor when the mouse has moved. Mask interrupts for the
         * draw so a preemption can't split the save/restore of the pixels
         * under the cursor. */
        UINT32 seq = MouseState.Seq;
        if (seq != last_seq) {
            last_seq = seq;
            KiUserQueueMouse(MouseState.X, MouseState.Y, MouseState.Buttons);
            UINT64 flags = KiIrqSave();
            GfxMoveCursor(MouseState.X, MouseState.Y);
            KiIrqRestore(flags);
        }

        KeYield();
    }
}

/* Map a user stack and return its (16-byte aligned) top. */
static UINT64 SetupUserStack(void)
{
    UINT64 base = USER_STACK_TOP - USER_STACK_PAGES * PAGE_SIZE;
    for (UINT64 i = 0; i < USER_STACK_PAGES; i++) {
        UINT64 pa = MmAllocatePage();
        if (pa == MM_INVALID_PHYS)
            KeBugCheck(KE_PHASE0_INITIALIZATION_FAILED, "no memory for user stack");
        MmMapPage(base + i * PAGE_SIZE, pa, PTE_USER | PTE_WRITE);
    }
    return USER_STACK_TOP;
}

/* Delete procedure for the demo "Event" object type. */
static void DemoEventDelete(POBJECT object)
{
    KeLog("[test]   -> Event delete procedure ran for object %p\n", object);
}

static void ObjectManagerDemo(void)
{
    KeLog("[test] --- object manager demo ---\n");

    POBJECT_TYPE event_type = ObCreateObjectType("Event", DemoEventDelete);

    /* Lifetime via references and a handle. */
    POBJECT e1;
    ObCreateObject(event_type, 32, &e1);
    KeLog("[test] created Event e1=%p refs=%d\n", e1, ObGetReferenceCount(e1));

    HANDLE h1;
    ObCreateHandle(e1, GENERIC_ALL, &h1);
    KeLog("[test] opened handle %p to e1; refs=%d\n", h1, ObGetReferenceCount(e1));

    POBJECT resolved;
    if (NT_SUCCESS(ObReferenceObjectByHandle(h1, GENERIC_READ, event_type,
                                             &resolved))) {
        KeLog("[test] handle resolves to %p (refs=%d), releasing\n",
              resolved, ObGetReferenceCount(resolved));
        ObDereferenceObject(resolved);
    }

    ObDereferenceObject(e1); /* drop the creator's reference; handle still holds */
    KeLog("[test] dropped creator ref; refs=%d (handle keeps it alive)\n",
          ObGetReferenceCount(e1));
    KeLog("[test] closing the handle should delete the object:\n");
    ObCloseHandle(h1);

    /* Namespace: \BaseNamedObjects\TestEvent (\Device is owned by Io). */
    struct _OBJECT_DIRECTORY *bno_dir;
    ObCreateDirectory(ObRootDirectory, "BaseNamedObjects", &bno_dir);

    POBJECT e2;
    ObCreateObject(event_type, 32, &e2);
    ObInsertObjectByName(bno_dir, "TestEvent", e2);
    ObDereferenceObject(e2); /* namespace now owns it */
    KeLog("[test] inserted \\BaseNamedObjects\\TestEvent\n");

    POBJECT found;
    NTSTATUS st = ObLookupObjectByName("\\BaseNamedObjects\\TestEvent", &found);
    KeLog("[test] lookup \\BaseNamedObjects\\TestEvent -> status=0x%08x obj=%p\n",
          (unsigned)st, NT_SUCCESS(st) ? found : NULL);
    if (NT_SUCCESS(st))
        ObDereferenceObject(found);

    st = ObLookupObjectByName("\\BaseNamedObjects\\Missing", &found);
    KeLog("[test] lookup \\BaseNamedObjects\\Missing -> status=0x%08x (not found)\n",
          (unsigned)st);
}

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

    KeLog("[ke]   arming syscall/sysret path...\n");
    KiInitializeSystemCalls();

    KeLog("[ke]   CPU descriptor tables active; traps are now handled.\n");

    /* Phase 1: memory management. */
    MmInitialize((UINT64)mbi_phys);

    /* Map the shared user-data page (tick count / system time for ring 3). */
    KeInitializeSharedData();

    /* Bring up the raw framebuffer, but deliberately do not draw a kernel
     * desktop, taskbar, window or graphical log console. The visible shell is
     * now exclusively the responsibility of the Windows-compatible user-mode
     * stack we are building for explorer.exe. */
    GfxInitialize();
    if (GfxAvailable())
        GfxClear(GfxColor(0, 0, 0));
    KeLog("[gfx]  raw framebuffer ready; no built-in desktop\n");

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

    /* Phase 2: object manager, then the Ps object types. */
    ObInitialize();
    ObjectManagerDemo();
    PsInitialize();

    /* Configuration manager (registry). */
    CmInitialize();

    /* Phase 5/6: mount the disk, then load a PE from it and run it. */
    KeLog("[io]   bringing up disk and filesystem...\n");
    NTSTATUS io = IoInitialize();
    if (!NT_SUCCESS(io))
        KeLog("[io]   WARNING: no filesystem (status 0x%08x)\n", (unsigned)io);

    KeLog("[test] --- loading %s from disk ---\n", NTOS_BOOT_PROGRAM);
    KeInitializeScheduler();

    UINT64 pe_entry, pe_base, pe_arg;
    NTSTATUS st = LdrLoadExecutable(NTOS_BOOT_PROGRAM, &pe_entry, &pe_base,
                                    &pe_arg);
    if (NT_SUCCESS(st)) {
        UINT64 stack_top = SetupUserStack();
        UINT64 stack_base = USER_STACK_TOP - USER_STACK_PAGES * PAGE_SIZE;
        PsCreateUserProcess(NTOS_BOOT_PROGRAM, NTOS_BOOT_COMMAND_LINE,
                            pe_entry, pe_base, stack_base, stack_top, pe_arg);
    } else {
        KeLog("[test] failed to load %s: status 0x%08x\n",
              NTOS_BOOT_PROGRAM, (unsigned)st);
    }

    KeCreateThread("KWorker", DemoWorker, (PVOID)"KWorker", 8);

    HalInitializePic();
    HalRegisterIrqHandler(0, KeClockTick);
    HalInitializeTimer(100); /* 100 Hz preemption tick */

    /* Input: PS/2 keyboard + mouse, and a thread that drives the cursor. */
    HalInitializeKeyboard();
    HalInitializeMouse();
    if (GfxAvailable())
        HalMouseSetBounds((INT32)GfxFramebuffer.Width, (INT32)GfxFramebuffer.Height);
    KeCreateThread("Input", InputWorker, NULL, 8);

    HalVgaSetColor(VGA_COLOR(VGA_LGREEN, VGA_BLACK));
    KeLog("\n[ok]   PE loaded; running it in ring 3 alongside a kernel thread.\n");
    HalVgaSetColor(VGA_COLOR(VGA_LGRAY, VGA_BLACK));

    /* Become the idle thread. The scheduler runs the ring-3 PE (which makes
     * syscalls) and the kernel worker. */
    __sti();
    for (;;)
        __halt();
}
