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
- Drops to **ring 3 (user mode)** and services **`syscall`/`sysret`** system
  calls through a `KiServiceTable` of `Nt*` routines — the same mechanism a
  native `ntdll` uses.
- **Loads and runs a real PE (`.exe`) executable** with **dynamic linking**: the
  loader parses the PE32+ headers, maps sections with per-section permissions,
  loads the `ntdll.dll` it imports, resolves the imports against ntdll's export
  table, patches the executable's IAT, and runs it in ring 3 — where its calls
  to `Nt*` go through the import table into ntdll's syscall stubs. Both images
  are built by the standard toolchain (`nasm -f win64` + `lld-link`). This is
  the authentic Windows load-and-link flow; the syscall ABI matches Windows.
- Sets up the **PEB and TEB** at the Windows x64 layout: ring-3 code reads its
  TEB through `gs:[0x30]` and its PEB through `gs:[0x60]` (a correct swapgs
  model keeps GS consistent across ring transitions and preemption), and can
  request memory with a real `NtAllocateVirtualMemory` service.
- **Loads executables from a disk**: a polled **ATA PIO** driver and a read-only
  **FAT32** filesystem read `testapp.exe` and its `ntdll.dll` dependency off a
  disk image at runtime — no longer embedded in the kernel.
- **Handle-based file I/O**: a `File` object type and a `\Device\Console` device
  in the object namespace back `NtCreateFile` / `NtReadFile` / `NtWriteFile` /
  `NtClose`. The test program opens the console and a file, reads the file, and
  echoes it to the console entirely through NT handles.
- **User-mode multithreading**: waitable dispatcher objects (events, semaphores,
  mutants; threads signal on exit) with `KeWaitForSingleObject`, exposed as
  `NtCreateThread` / `NtCreateEvent` / `NtSetEvent` / `NtWaitForSingleObject`.
- **Runs a normal Win32 program.** A `kernel32.dll` (built from C) implements
  the Win32 API — `GetStdHandle`, `WriteFile`, `ReadFile`, `CreateFileA`,
  `CloseHandle`, `CreateThread`, `WaitForSingleObject`, `ExitProcess` — on top
  of the native `ntdll`. The test `.exe` uses only the Win32 API (no raw
  syscalls), and the loader resolves the full `app.exe → kernel32.dll →
  ntdll.dll → syscall` chain off the disk. All three are real PEs built by the
  standard toolchain (`clang --target=x86_64-pc-windows-msvc` + `lld-link`).
- **Draws a graphical desktop.** Requests a 32-bpp **linear framebuffer** from
  GRUB via Multiboot2, maps it into the direct map, and provides graphics
  primitives (pixels, filled rectangles, an 8×16 bitmap font). The kernel
  composes a simple desktop — a title bar, a scrolling text console, and a
  taskbar — and the kernel log is rendered on-screen (in addition to serial),
  which is the first visible step toward a GUI shell.
- **Reads keyboard and mouse.** A **PS/2 keyboard** driver (IRQ1) translates
  scancodes to ASCII into a ring buffer, and a **PS/2 mouse** driver (IRQ12)
  decodes movement packets into a screen-clamped cursor position. A live input
  thread echoes typed characters and draws an arrow **mouse cursor** that
  follows the mouse, saving and restoring the pixels underneath it (and hiding
  itself while the console draws) so the desktop stays intact.
- **Runs a Windows dynamic runtime.** The kernel loader builds a real
  **`PEB->Ldr` module list** (`LDR_DATA_TABLE_ENTRY` per loaded module, with the
  Windows x64 field layout), and kernel32 implements **`GetModuleHandleA`**
  (walking that list), **`GetProcAddress`** (parsing a module's PE export
  directory), and a **process heap** (`GetProcessHeap` / `HeapCreate` /
  `HeapAlloc` / `HeapFree`, a coalescing free list over
  `NtAllocateVirtualMemory`). The test program looks a module up by name,
  resolves a function from it by name, calls the resolved pointer, and allocates
  from the heap — the runtime backbone almost every Windows binary relies on.
  **`LoadLibraryA`** loads a DLL from disk **at runtime** (via an `NtLoadLibrary`
  service) and links it into `PEB->Ldr`: the test program loads a standalone
  `extra.dll` that isn't in its import chain, resolves its exports, and calls
  them — the way a Windows program loads a plugin. **SSE** is enabled at boot
  (mandatory on x86-64 and for real Windows code), and the context switch
  preserves each thread's x87/SSE state with **FXSAVE/FXRSTOR**.
- **Has a registry.** A Configuration Manager (`Cm`) keeps a hierarchical
  key/value store in memory (rooted at `\Registry`, with keys exposed as `Key`
  objects so `NtClose` releases them), reached through `NtCreateKey` /
  `NtOpenKey` / `NtSetValueKey` / `NtQueryValueKey`. An **`advapi32.dll`**
  implements the classic `RegOpenKeyExA` / `RegCreateKeyExA` / `RegSetValueExA` /
  `RegQueryValueExA` / `RegCloseKey` on top; the test program reads a preset
  value under `HKLM\Software\NTOS` and creates a key, writes a value, and reads
  it back under `HKCU`.
- **Speaks the real NT system-call ABI.** The syscall entry marshals the full
  Windows argument set (four registers plus stack arguments at `[rsp+0x28]`, up
  to 11), so services carry their true NT signatures: `NtCreateFile` takes a
  `POBJECT_ATTRIBUTES` (with a `UNICODE_STRING` name) and reports through an
  `IO_STATUS_BLOCK`, `NtReadFile`/`NtWriteFile` use the 9-argument form, and
  `NtAllocateVirtualMemory` the 6-argument form. The service table is indexed by
  the **real Windows 7 SP1 x64 syscall numbers**, and kernel32 builds these
  structures exactly as the real one does — a concrete step toward one day
  driving the kernel with an unmodified `ntdll`.
- **Broader Win32 + a mini-CRT.** A **`KUSER_SHARED_DATA`** page at the fixed
  user address `0x7FFE0000` (kernel-updated each tick) backs `GetTickCount` /
  `GetTickCount64` / `GetSystemTimeAsFileTime` with no system call, exactly as on
  Windows. `Sleep` blocks through `NtDelayExecution`; `GetCommandLineA` reads the
  command line from `PEB->ProcessParameters`; and kernel32 gains `lstrlenA` /
  `lstrcpyA` / `lstrcatA` / `wsprintfA`. A small **`msvcrt.dll`** (`printf`,
  `malloc`/`free`, `strlen`/`strcpy`/`strcmp`, `memcpy`/`memset`, `puts`) layers
  a C runtime over the Win32 API, so a normal C program links against it instead
  of a host libc.
- **Real signatures throughout, and validated user pointers.** The registry
  (`NtOpenKey`/`NtCreateKey` via `OBJECT_ATTRIBUTES`, `NtQueryValueKey` returning
  `KEY_VALUE_PARTIAL_INFORMATION`), threads (`NtCreateThreadEx`), and events/waits
  now carry their true NT signatures, and advapi32/kernel32 build the structures
  as the real DLLs do. The kernel no longer trusts ring-3 pointers blindly:
  `MmProbeForRead`/`MmProbeForWrite` validate that a user buffer is in user space
  with its pages present, and object names are **captured**
  (`MmCaptureUnicodeName`) into kernel memory before use — so a bad pointer
  returns `STATUS_ACCESS_VIOLATION` instead of faulting the kernel.
- **Runs a standard `int main()` program.** A CRT startup (`crt0.c`'s
  `mainCRTStartup`, the linker's default console entry) parses `argc`/`argv` from
  the command line, calls `main`, and exits with its return value — so a plain,
  portable C program (`int main()`, `<stdio.h>`/`<stdlib.h>`/`<string.h>`, *no*
  OS-specific code) compiles and runs on NTOS unchanged. `hello.exe` is exactly
  that; `testapp.exe` now enters through the same startup too. This is the base
  for eventually dropping in ready-made programs.

See [`docs/ROADMAP.md`](docs/ROADMAP.md) for what comes next (physical/virtual
memory manager, object manager, threads & scheduler, system-call boundary, and
finally the PE loader that runs the first `.exe`).

## Building

You need a Linux host with:

- `gcc` (or `clang`), `ld`, `nasm`
- `grub-mkrescue` + `xorriso` (to build the bootable ISO)
- `mtools` (to build the FAT disk image), `qemu-system-x86_64` (to run it)

```sh
make            # build kernel/ntoskrnl.elf
make iso        # build build/ntos.iso
make run        # build the ISO + FAT disk and boot in QEMU (serial on stdout)
make run-gui    # same, but with a QEMU graphical window
```

`make run` boots headless from the ISO and prints the kernel log to your
terminal via the emulated serial port. The user-space executables live on a
separate FAT32 disk image (`build/disk.img`, built from `user/*.asm`) that the
kernel's ATA + FAT drivers read at runtime.

## Layout

```
boot/            GRUB configuration
kernel/
  arch/x86_64/   architecture-specific boot & CPU code (boot.asm, gdt, idt, …)
  include/       public kernel headers
    nt/          NT ABI types and status codes (ntdef.h, ntstatus.h)
    ntos/        executive component interfaces (ke.h, hal.h, rtl.h, …)
  ke/            Kernel core (KiSystemStartup, threads, scheduler)
  hal/           HAL (serial, VGA, framebuffer, PS/2 keyboard+mouse, PIC, PIT)
  rtl/           Runtime Library (memory, string, formatted print, lists)
  mm/            Memory Manager (multiboot map, PMM, page tables, direct map)
  ex/            Executive support (pool allocator)
  ob/            Object Manager (types, handles, namespace)
  ldr/           Image loader (PE/COFF, imports/exports, loads from disk)
  ps/            Process manager (PEB/TEB, user process creation)
  io/            I/O manager (ATA PIO block driver, FAT32 filesystem)
  cm/            Configuration Manager (the registry)
user/            User-space sources (ntdll, kernel32, advapi32, msvcrt, crt0, extra, testapp, hello)
  include/       minimal libc headers (stdio.h, stdlib.h, string.h) for portable programs
docs/            architecture notes and roadmap
scripts/         helper scripts
```

## License

See [`LICENSE`](LICENSE).
