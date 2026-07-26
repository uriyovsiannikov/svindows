# NTOS architecture

NTOS is organized to mirror the Windows NT executive. This keeps a clear path
toward NT binary compatibility: each NT subsystem has a home, and the public
native API (`Nt*` / `Zw*` system services) can eventually be implemented on top
of the same component boundaries the real kernel uses.

## Address space

64-bit canonical addressing, 4-level paging (PML4). The kernel lives in the
higher half:

| Region                                  | Purpose                                    |
| --------------------------------------- | ------------------------------------------ |
| `0x0000000000000000`–`0x00007FFFFFFFFFFF` | user space (ring 3 code/stack mapped here) |
| `0xFFFF800000000000`–…                   | direct map of all physical RAM (2 MiB pages) |
| `0xFFFFFFFF80000000`–`0xFFFFFFFFBFFFFFFF` | kernel image + kernel data (`-2 GiB`)     |
| `0xFFFFFFFFC0000000`–…                   | kernel pool heap (`-1 GiB`, grows on demand) |

The kernel is linked at virtual base `0xFFFFFFFF80000000` and loaded at physical
`0x100000` (1 MiB). Early boot maps the first 1 GiB both identity-mapped and at
the kernel's higher-half base so the trampoline can survive the jump into the
higher half. `Mm` then builds the kernel's own page tables — the kernel window
plus a direct map of all RAM — switches `CR3`, and drops the identity map. After
that, any physical page is reachable at `MM_DIRECT_MAP_BASE + phys` via
`MmPhysToVirt`, and `ExAllocatePool` hands out memory from the growable heap.

## Boot flow

```
BIOS ──▶ GRUB (Multiboot2) ──▶ _start (32-bit, arch/x86_64/boot.asm)
   │
   │  1. save Multiboot2 info pointer + magic
   │  2. build boot page tables (identity + higher half, 2 MiB pages)
   │  3. enable PAE, set EFER.LME, enable paging  → IA-32e mode
   │  4. load GDT64, far-jump into 64-bit code
   │  5. jump to the higher-half virtual address
   ▼
KiSystemStartup (C, ke/ke_main.c)
   │  HAL console → GDT/TSS → IDT → syscall MSRs → Mm (memory)
   │  → Ob (objects) → Ke scheduler + threads (kernel + ring 3)
   │  → PIC/PIT → sti → idle thread
```

## System calls and ring 3

A user thread enters ring 3 by `iretq`. It calls back into the kernel with the
`syscall` instruction, whose entry (`arch/x86_64/syscall_entry.asm`) does the
`swapgs` + kernel-stack switch through a per-CPU block (KPCR) reached via GS,
then dispatches through `KiServiceTable` in `ke/syscall.c`. The user ABI mirrors
System V with R10 replacing RCX (RCX/R11 are consumed by `syscall`): service
number in RAX, arguments in RDI/RSI/RDX/R10/R8, result in RAX. `SYSRET` returns
to ring 3. On every context switch the scheduler repoints TSS.RSP0 and the
KPCR's kernel stack at the incoming thread, so a syscall or interrupt taken from
ring 3 always lands on that thread's own kernel stack.

## Loading executables

`Ldr` (`ldr/pe.c`) loads PE32+ images the way Windows does: it maps the image at
its preferred base, then **resolves imports** — for each imported DLL it loads
the module (today from an in-kernel registry of embedded images; a filesystem
later), looks each imported routine up in that module's export directory, and
patches the executable's Import Address Table. So a program's `NtWriteFile`-style
call compiles to an indirect call through the IAT into `ntdll.dll`, whose stub
(`mov r10, rcx; mov eax, <n>; syscall`) crosses into the kernel. `ntdll.dll` and
the test `.exe` are ordinary PE files produced by `nasm -f win64` + `lld-link`.

## Executive components

The directory names match NT's internal prefixes, so a symbol like
`MmAllocatePages` lives in `mm/`, `ObCreateObject` in `ob/`, and so on.

| Prefix | Directory | Responsibility                                             | State |
| ------ | --------- | ---------------------------------------------------------- | ----- |
| `Ke`   | `ke/`     | CPU control, interrupts/traps, scheduling, synchronization | working |
| `Hal`  | `hal/`    | port I/O, serial, VGA, PIC, PIT timer, IRQ dispatch        | working |
| `Mm`   | `mm/`     | physical & virtual memory, direct map, page tables         | working |
| `Ob`   | `ob/`     | object manager: object types, handles, namespace           | working |
| `Ps`   | `ps/`     | processes and threads                                      | stub  |
| `Io`   | `io/`     | I/O manager, device/driver model, IRPs                     | stub  |
| `Ex`   | `ex/`     | executive support: pool allocator, sync primitives         | early |
| `Ldr`  | `ldr/`    | PE/COFF image loader                                       | early |
| `Rtl`  | `rtl/`    | runtime library: strings, memory, lists, formatting        | early |

## Calling conventions and the NT ABI

Internally the kernel is compiled with the System V AMD64 ABI (the toolchain
default). The boundary that must match Windows exactly — the system-service
interface consumed by native PE binaries — uses the Microsoft x64 calling
convention. `NTAPI` is defined as `__attribute__((ms_abi))` for exactly those
functions so that, when the PE loader and `ntdll` shim arrive, native binaries
see a Windows-compatible ABI.

## Design principles

- **Freestanding C.** No host libc. `-ffreestanding`, no red zone, kernel code
  model, general registers only (no implicit SSE/FP in the kernel).
- **NT-faithful naming.** Types, status codes, and structures follow the public
  NT conventions so the eventual native API is a natural fit, not a rewrite.
- **Testable at every step.** Every milestone boots in QEMU and prints evidence
  of what it did over the serial port.
