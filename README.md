# SVindows — an NT-compatible operating system for x86-64

SVindows is a from-scratch `x86_64` operating system aiming for **binary compatibility with the Windows NT family**: loading and executing native NT `PE/COFF` executables (`.exe` / `.dll`).

The project is built bottom-up around NT-style components (`Ke`, `Mm`, `Ob`, `Ps`, `Io`, `Ex`, `Rtl`, `Hal`) and is intended for educational/research use. It is not affiliated with Microsoft and contains no Microsoft code.

## Current status

- Boots via **Multiboot2/GRUB** and enters 64-bit long mode.
- Has GDT/TSS, IDT, CPU exception handling, serial/VGA console, and a higher-half kernel.
- Provides a real memory manager, physical-frame allocator, page tables, direct map, and kernel pool.
- Implements an NT-style object manager, handles, namespaces, kernel threads, and a preemptive scheduler.
- Runs ring-3 processes through `syscall`/`sysret` and an NT-style `Nt*` service table.
- Loads real PE32+ executables and DLLs from a FAT32 disk image, resolves imports, and runs them in user mode.
- Provides Windows-compatible PEB/TEB structures, NT file I/O, events, semaphores, threads, waits, registry APIs, and user-pointer validation.
- Includes `kernel32.dll`, `advapi32.dll`, `msvcrt.dll`, and a small CRT for normal C/Win32 programs.
- Supports Win32 TLS, process/thread identity, virtual memory APIs, timing, synchronization, and dynamic DLL loading.
- Can run an **unmodified Windows 11 x64 `hostname.exe`** from `C:\Windows\System32`.
- Provides a 32-bpp framebuffer plus PS/2 keyboard/mouse input without a kernel-owned desktop shell.

See [`docs/ROADMAP.md`](docs/ROADMAP.md) for the next planned steps.

## Building

You need a Linux host with:

- `gcc` or `clang`, `ld`, `nasm`
- `grub-mkrescue`, `xorriso`
- `mtools`
- `qemu-system-x86_64`

```sh
make            # build kernel/svwindowskrnl.elf
make iso        # build build/svindows.iso
make run        # boot in QEMU
make run-gui    # boot with a QEMU graphical window
```

`make run` boots from the ISO and prints the kernel log through the emulated serial port. User-space executables are stored on the FAT32 disk image `build/disk.img`.

## Layout

```text
boot/            GRUB configuration
kernel/
  arch/x86_64/   architecture-specific boot & CPU code
  include/       public kernel headers
    nt/          NT ABI types and status codes
    svindows/    executive component interfaces
  ke/            Kernel core, threads, scheduler
  hal/           HAL, serial, VGA, framebuffer, PS/2, PIC, PIT
  rtl/           Runtime Library
  mm/            Memory Manager
  ex/            Executive support
  ob/            Object Manager
  ldr/            PE/COFF image loader
  ps/            Process manager, PEB/TEB
  io/            ATA/FAT32 I/O
  cm/            Configuration Manager / registry
user/            User-space DLLs, CRT, and test programs
docs/            Architecture notes and roadmap
scripts/         Helper scripts

## License

See [`LICENSE`](LICENSE).
