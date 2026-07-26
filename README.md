# NTOS — an NT-compatible operating system for x86-64

NTOS is a from-scratch operating system for the `x86_64` architecture whose
long-term goal is **binary compatibility with the Windows NT family** — the
ability to load and execute native NT `PE/COFF` executables (`.exe` / `.dll`).

The project is being built bottom-up and deliberately mirrors the internal
architecture of the Windows NT executive (the `Ke`, `Mm`, `Ob`, `Ps`, `Io`,
`Ex`, `Rtl`, `Hal` components) so that the native API and the PE loader can be
layered on top of a faithful kernel foundation.

> This is an educational / research OS in the same spirit as ReactOS. It is not
> affiliated with, nor does it contain code from, Microsoft.

## Current status

The kernel currently:

- Boots on BIOS machines via **Multiboot2** (GRUB) as an `ELF64` image.
- Transitions from 32-bit protected mode into **64-bit long mode** and relocates
  itself into the higher half of the address space (`0xFFFFFFFF80000000`).
- Brings up an early console over the **COM1 serial port** and **VGA text mode**
  with a `printf`-style formatter.
- Installs a 64-bit **GDT/TSS** and a full **IDT** with CPU exception handlers
  that dump a trap frame.
- Exposes NT-style base types (`NTSTATUS`, `LIST_ENTRY`, …) and a small `Rtl`
  runtime library.
- Runs a real **memory manager**: parses the firmware memory map, allocates
  physical frames from a bitmap **PMM**, builds its own page tables with a
  full-RAM **direct map** (dropping the identity map), and serves kernel
  allocations from a growable **pool** (`ExAllocatePool`).
- Provides an **object manager** (`Ob`): reference-counted objects with typed
  delete procedures, a handle table with access masks, and a `\`-rooted object
  namespace with directories and path lookup.
- Runs multiple **kernel threads** under a **preemptive round-robin scheduler**
  driven by the PIT timer (8259 PIC remapped, IRQ0 tick), with context
  switching, per-thread quanta, `KeCreateThread` / `KeYield` /
  `KeTerminateThread`.

See [`docs/ROADMAP.md`](docs/ROADMAP.md) for what comes next (physical/virtual
memory manager, object manager, threads & scheduler, system-call boundary, and
finally the PE loader that runs the first `.exe`).

## Building

You need a Linux host with:

- `gcc` (or `clang`), `ld`, `nasm`
- `grub-mkrescue` + `xorriso` (to build the bootable ISO)
- `qemu-system-x86_64` (to run it)

```sh
make            # build kernel/ntoskrnl.elf
make iso        # build build/ntos.iso
make run        # build the ISO and boot it in QEMU (serial on stdout)
make run-gui    # same, but with a QEMU graphical window
```

`make run` boots headless and prints the kernel log to your terminal via the
emulated serial port, which is the easiest way to see what the kernel is doing.

## Layout

```
boot/            GRUB configuration
kernel/
  arch/x86_64/   architecture-specific boot & CPU code (boot.asm, gdt, idt, …)
  include/       public kernel headers
    nt/          NT ABI types and status codes (ntdef.h, ntstatus.h)
    ntos/        executive component interfaces (ke.h, hal.h, rtl.h, …)
  ke/            Kernel core (KiSystemStartup, threads, scheduler)
  hal/           Hardware Abstraction Layer (serial, VGA, PIC, PIT, port I/O)
  rtl/           Runtime Library (memory, string, formatted print, lists)
  mm/            Memory Manager (multiboot map, PMM, page tables, direct map)
  ex/            Executive support (pool allocator)
  ob/            Object Manager (types, handles, namespace)
  ps/ io/        Process / I/O managers (stubs, being filled in)
docs/            architecture notes and roadmap
scripts/         helper scripts
```

## License

See [`LICENSE`](LICENSE).
