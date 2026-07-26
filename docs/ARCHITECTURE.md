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
| `0x0000000000000000`–`0x00007FFFFFFFFFFF` | user space (per-process), unused for now |
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
   │  HAL console → GDT/TSS → IDT → banner → idle
```

## Executive components

The directory names match NT's internal prefixes, so a symbol like
`MmAllocatePages` lives in `mm/`, `ObCreateObject` in `ob/`, and so on.

| Prefix | Directory | Responsibility                                             | State |
| ------ | --------- | ---------------------------------------------------------- | ----- |
| `Ke`   | `ke/`     | CPU control, interrupts/traps, scheduling, synchronization | early |
| `Hal`  | `hal/`    | port I/O, serial, VGA, timers, interrupt controllers       | early |
| `Mm`   | `mm/`     | physical & virtual memory, direct map, page tables         | working |
| `Ob`   | `ob/`     | object manager: object types, handles, namespace           | stub  |
| `Ps`   | `ps/`     | processes and threads                                      | stub  |
| `Io`   | `io/`     | I/O manager, device/driver model, IRPs                     | stub  |
| `Ex`   | `ex/`     | executive support: pool allocator, sync primitives         | early |
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
