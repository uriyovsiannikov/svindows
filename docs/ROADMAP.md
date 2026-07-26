# NTOS roadmap

The destination is **running a native NT `.exe`**. That is a long road; this
document breaks it into milestones that each end in something bootable and
observable. Checked items are done and verified in QEMU.

## Phase 0 — Foundation (in progress)

- [x] Multiboot2 boot as an ELF64 image (GRUB).
- [x] 32-bit → 64-bit long mode transition, higher-half kernel.
- [x] Serial (COM1) + VGA text-mode console with `printf`-style formatting.
- [x] NT base types (`NTSTATUS`, `LIST_ENTRY`, …) and `Rtl` string/memory helpers.
- [x] 64-bit GDT + TSS.
- [x] IDT with CPU exception handlers that dump a trap frame.
- [ ] Serial-driven kernel debugger hooks (`DbgPrint`, breakpoints).

## Phase 1 — Memory management (`Mm`) (in progress)

- [x] Parse the Multiboot2 memory map into a physical memory map.
- [x] Physical page allocator (`MmAllocatePage`/`MmAllocatePages`, bitmap).
- [x] Kernel virtual address space + real page tables; drop the identity map
      (kernel window at -2 GiB, direct map of all RAM at `0xFFFF800000000000`).
- [x] Pool allocator (`ExAllocatePool` / `ExFreePool`), growable kernel heap.
- [ ] Per-process address spaces (`MmCreateAddressSpace`) — deferred to Phase 3,
      alongside process creation, since it only becomes useful there.

Also done in this phase: the executive pool allocator (`Ex`) that the rest of
the kernel allocates from.

## Phase 2 — Executive & Object manager (`Ex`, `Ob`)

- [ ] Object manager: object types, headers, reference counting.
- [ ] Handle tables and `Nt`-style handle-based APIs.
- [ ] Object namespace (`\`, `\Device`, `\??`, …).
- [ ] Synchronization objects: events, mutexes, semaphores.

## Phase 3 — Threads, processes, scheduler (`Ke`, `Ps`)

- [ ] `KTHREAD` / `KPROCESS`, context switch.
- [ ] Priority-based, preemptive scheduler driven by the timer (APIC/PIT).
- [ ] `ETHREAD` / `EPROCESS` and process/thread creation.
- [ ] Kernel-mode threads first, then a user/kernel privilege split.

## Phase 4 — Traps, interrupts, system calls

- [ ] APIC + IOAPIC bring-up, timer and keyboard interrupts.
- [ ] User mode (ring 3), `syscall`/`sysret` fast system-call path.
- [ ] The `Nt*` system service dispatch table (`KiServiceTable`).

## Phase 5 — I/O manager (`Io`)

- [ ] Device / driver objects, IRP model.
- [ ] A RAM-disk and a simple file system (FAT) for loading binaries.

## Phase 6 — The NT user-mode boundary

- [ ] PE/COFF image loader in the kernel.
- [ ] A minimal `ntdll` that thunks native calls to `syscall`.
- [ ] Load and run the first native PE `.exe` (a program that calls
      `NtWriteFile` / `NtTerminateProcess`).
- [ ] Grow the native API surface; later, a Win32 personality
      (`kernel32`/`user32`) — the ReactOS/Wine-scale endgame.

## Guiding rule

Every milestone must boot in QEMU and print evidence of success over the serial
port before it is marked done. Correctness and clarity first — speed later.
