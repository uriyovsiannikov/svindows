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

## Phase 2 — Object manager (`Ob`) (in progress)

- [x] Object manager: object types, object headers, reference counting with
      per-type delete procedures.
- [x] Handle table with granted-access masks (`ObCreateHandle`,
      `ObReferenceObjectByHandle`, `ObCloseHandle`). `Nt`-style handle APIs on
      top of it arrive with the system-call layer in Phase 4.
- [x] Object namespace: `Directory` objects, the root `\`, insert-by-name and
      absolute-path lookup (e.g. `\Device\TestEvent`).
- [ ] Synchronization objects: events, mutexes, semaphores (needs `Ke` waits).

## Phase 3 — Threads, processes, scheduler (`Ke`, `Ps`) (in progress)

- [x] `KTHREAD` / `KPROCESS` and the context switch (`KiSwitchContext`).
- [x] Preemptive scheduler driven by the 8254 PIT via the remapped 8259 PIC;
      round-robin with per-thread quanta, `KeCreateThread` / `KeYield` /
      `KeTerminateThread`.
- [ ] Priority-based ordering (the `Priority` field exists but scheduling is
      currently round-robin) and thread synchronization/wait.
- [ ] `ETHREAD` / `EPROCESS` executive wrappers and a terminated-thread reaper.
- [ ] Kernel-mode threads work today; the user/kernel privilege split is Phase 4.

## Phase 4 — User mode and system calls (in progress)

- [x] User mode (ring 3): GDT user descriptors ordered for SYSRET, `iretq`
      transition, per-thread kernel stack via TSS.RSP0 + KPCR.
- [x] `syscall`/`sysret` fast path: EFER.SCE, STAR/LSTAR/SFMASK, a swapgs +
      stack-switch entry stub, and the ring-3 register ABI.
- [x] The `Nt*` system service dispatch table (`KiServiceTable`) with the first
      services (`NtDisplayString`, `NtDisplayNumber`, `NtTerminateThread`),
      exercised by an in-kernel ring-3 test program.
- [ ] APIC + IOAPIC bring-up; keyboard interrupt (still on the 8259 PIC / PIT).
- [ ] Capture/validate user-mode pointers (SEH-style probing) instead of
      trusting them.

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
