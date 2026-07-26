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

**The argument ABI.** The entry marshals the full Windows syscall argument set
into an array on the kernel stack — arguments 1-4 from `R10`/`RDX`/`R8`/`R9`,
and arguments 5+ from the user stack at `[user_rsp+0x28]` (past the ntdll stub's
return address and the four-slot home space), up to 11 — and passes its address
to `KiSystemServiceDispatch(number, args)`. That lets a service carry its true
NT signature: `NtCreateFile` reads a `POBJECT_ATTRIBUTES` (whose `UNICODE_STRING`
names the target) and reports via an `IO_STATUS_BLOCK`, `NtReadFile`/
`NtWriteFile` take the 9-argument form, and `NtAllocateVirtualMemory` the
6-argument form. The service table (`ke/syscall.c`) is a sparse array indexed by
the **real Windows 7 SP1 x64 syscall numbers** — the same numbers the ntdll
stubs issue — so aligning to a real build's numbering is what would let a
genuine `ntdll` drive this kernel. NTOS-only services (no Windows equivalent)
sit above the real range at `0xF0+`.

**GS and the TEB/PEB.** In ring 3 the GS base points at the thread's TEB, so
native code finds its environment at the usual offsets (`gs:[0x30]` = TEB self,
`gs:[0x60]` = PEB, whose `ImageBaseAddress` is at +0x10). In the kernel the GS
base is the per-CPU block (KPCR). The invariant "GS = KPCR while in ring 0" is
held by `swapgs`: the syscall stub swaps unconditionally, and interrupt entry/
exit swap only when the saved CS shows a ring-3 origin. `KERNEL_GS_BASE` (what
the next kernel-exit swapgs restores) is set to the incoming thread's TEB on
every context switch, so the model stays correct across preemption between user
and kernel threads. `Ps` (`ps/process.c`) builds the PEB and TEB and launches
the main thread via `PsCreateUserProcess`.

## Synchronization

Waitable kernel objects — events, semaphores, mutants, and threads — begin their
body with a `DISPATCHER_HEADER` (a signal state plus a wait list). A thread that
cannot immediately acquire an object links its wait block into that list, marks
itself `Waiting`, and reschedules; a signal wakes every waiter to re-test the
object. Because a syscall can block this way, the user `RSP` is saved on the
per-thread kernel stack (not the shared per-CPU slot) so it survives a context
switch to another thread mid-syscall. `NtCreateThreadEx`, `NtCreateEvent`,
`NtSetEvent`, and `NtWaitForSingleObject` (with alertable + timeout parameters)
expose this to ring 3.

**Validating user pointers.** A service must not trust the pointers ring 3 hands
it. `MmProbeForRead`/`MmProbeForWrite` (`mm/vmm.c`) check that a buffer lies
wholly in user space (`< 0x0000800000000000`) with every page present, and
`MmCaptureUnicodeName` copies an object name out of a user `UNICODE_STRING` into
kernel memory so the service works on a stable copy. `NtCreateFile`, the registry
services, and the thread/event services probe their inputs and capture their
names up front, returning `STATUS_ACCESS_VIOLATION` on a bad pointer rather than
faulting the kernel. There is no kernel SEH yet, so these are range +
page-presence checks, not fault-safe probes; adding SEH is future work.

## Loading executables

`Ldr` (`ldr/pe.c`) loads PE32+ images the way Windows does: it maps the image at
its preferred base, then **resolves imports** — for each imported DLL it loads
the module (from the mounted filesystem, on demand), looks each imported routine
up in that module's export directory, and patches the executable's Import
Address Table. Resolution recurses, so a full `app.exe -> kernel32.dll ->
ntdll.dll` chain links up automatically.

At the bottom, `ntdll.dll` is the native library: each `Nt*` export is a stub
(`mov r10, rcx; mov eax, <n>; syscall`) that crosses into the kernel.
`kernel32.dll` sits on top and implements the Win32 API (`WriteFile`,
`CreateThread`, `ExitProcess`, ...) by calling those native routines, so a
normal Win32 program links only against kernel32 and never issues a raw syscall.
`ntdll.dll` is `nasm`-assembled; `kernel32.dll` and the Win32 test `.exe` are C,
compiled with `clang --target=x86_64-pc-windows-msvc` and linked with
`lld-link` — ordinary PE files.

After loading, the kernel builds the process's **`PEB->Ldr` module list** —
a `PEB_LDR_DATA` with a `LDR_DATA_TABLE_ENTRY` (DllBase, EntryPoint,
SizeOfImage, a wide BaseDllName) per loaded module, threaded onto the
load/memory/init-order lists at the Windows x64 offsets — in a user-readable
region `PEB->Ldr` points at (`LdrBuildProcessModuleList`). That is what lets the
**dynamic runtime** work in ring 3 without the kernel's help: kernel32's
`GetModuleHandleA` walks the load-order list, `GetProcAddress` parses the target
module's PE export directory, and the process heap (`GetProcessHeap` /
`HeapAlloc` / `HeapFree`, a coalescing free list) runs over
`NtAllocateVirtualMemory`. `LoadLibraryA` loads a DLL from disk **at runtime**:
it calls the kernel loader through an `NtLoadLibrary` service, which maps the
image, resolves its imports, and appends a new `LDR_DATA_TABLE_ENTRY` to the
same `PEB->Ldr` region — after which `GetProcAddress` resolves the freshly
loaded module's exports, exactly as a plugin load works on Windows.

SSE is enabled at boot (`CR0.MP`/`CR4.OSFXSR`), since compiler-emitted XMM use —
pervasive in real Windows binaries — would otherwise fault, and the context
switch preserves each thread's x87/SSE state with FXSAVE/FXRSTOR (`KiSchedule`
saves the outgoing thread's state and restores the incoming thread's, each from
a 16-byte-aligned per-thread area, so concurrent SSE users don't clobber one
another).

## Graphics

GRUB is asked for a 32-bpp linear framebuffer through a Multiboot2 framebuffer
request tag (`arch/x86_64/boot.asm`). At boot, `mm/multiboot.c` reads the
framebuffer info tag — physical address, pitch, dimensions, and the RGB field
positions — into `GfxFramebuffer`. `GfxInitialize` (`hal/framebuffer.c`) maps
the framebuffer (which lives above RAM) into the direct map, and the graphics
primitives — put-pixel, filled rectangle, screen clear, and an 8×16 bitmap font
(`hal/font8x16.c`, generated from a monospace TTF) — draw straight into it.

A scrolling **framebuffer text console** sits on top of the primitives, and
`ke/ke_main.c` composes a simple **desktop**: a title bar, the console area, and
a taskbar. Once graphics are up, `HalConsolePutChar` (`hal/console.c`) fans the
kernel log out to the serial port *and* the framebuffer console, so boot output
appears on screen.

## Input and the cursor

The 8042 PS/2 controller (`hal/ps2.c`) multiplexes a keyboard (IRQ1) and the
mouse (IRQ12). The keyboard driver (`hal/keyboard.c`) translates set-1
scancodes to ASCII, tracking shift/caps, into a ring buffer that `KbdReadChar`
drains. The mouse driver (`hal/mouse.c`) reassembles the 3-byte movement
packets into a screen-clamped `MouseState` (position + buttons), bumping a
sequence counter on each change.

A kernel **input thread** (`InputWorker` in `ke/ke_main.c`) is the desktop's
live input loop: it echoes typed characters and, when the mouse sequence
changes, moves the arrow **cursor**. `GfxMoveCursor` (`hal/framebuffer.c`) saves
the pixels under the sprite and restores them on the next move. Because the
cursor is a software overlay, any console drawing must not run while it is
present: `HalConsolePutChar` lifts the cursor (`GfxHideCursor`) around each
character and restores it after (`GfxShowCursor`), all with interrupts masked so
the input thread can't repaint it mid-update. That keeps a console scroll from
smearing the cursor across the screen. A window/compositor model comes next
(see the roadmap).

## The registry

The Configuration Manager (`Cm`, `cm/registry.c`) keeps a hierarchical
key/value store in memory: keys form a tree under an anonymous root whose child
`Registry` anchors absolute paths (`\Registry\Machine\...`), and each key holds
a list of named, typed values (`REG_SZ`, `REG_DWORD`, ...). Keys are handed to
ring 3 as **`Key` objects** — `NtCreateKey`/`NtOpenKey` wrap the persistent
tree node in an Ob object and return a handle, so `NtClose` releases a key like
any other handle while the tree itself persists. The services carry their real
NT signatures: `NtOpenKey`/`NtCreateKey` name the key through an
`OBJECT_ATTRIBUTES` (whose `RootDirectory` anchors a relative name), and
`NtQueryValueKey` returns a `KEY_VALUE_PARTIAL_INFORMATION`. `advapi32.dll`
layers the classic `Reg*` API on
top: predefined roots (`HKEY_LOCAL_MACHINE`, ...) map to absolute paths opened
from the root, and a real key handle passes through as the parent for a relative
open — so `app.exe → advapi32 → ntdll → syscall → Cm` is the full path a
registry call takes.

## The user-mode runtime

Above `ntdll`, the DLLs a Windows program expects are reproduced in miniature.
`kernel32.dll` carries the process runtime (module/symbol lookup, the heap,
threads, files) plus time and string helpers; `advapi32.dll` the registry
`Reg*` API; and `msvcrt.dll` a small C runtime (`printf`, `malloc`/`free`,
`str*`/`mem*`) layered on the Win32 API. Time reads avoid a system call the way
Windows does: the kernel maps a read-only **`KUSER_SHARED_DATA`** page at the
fixed user address `0x7FFE0000` and refreshes its tick count / system time on
every clock tick, so `GetTickCount` just reads and scales the shared value. The
command line lives in a `RTL_USER_PROCESS_PARAMETERS` block that `PEB->
ProcessParameters` points at, where `GetCommandLine` finds it.

kernel32 covers a widening slice of the API real programs import:
`GetCurrentProcessId`/`GetCurrentThreadId`/`GetLastError`/`SetLastError` read
straight from the TEB via GS (`ClientId` at 0x40/0x48, `LastErrorValue` at 0x68);
`VirtualAlloc`/`VirtualProtect` run over `NtAllocateVirtualMemory` and a
`NtProtectVirtualMemory` service that re-applies page protection (`MmProtectRange`
keeps each frame, flips its writable bit); `QueryPerformanceCounter` reads the
shared-data clock; and `Interlocked*` plus recursive critical sections give the
atomics and locking a threaded program expects.

## Running a standard program

A program does not need NTOS-specific glue to run. The linker's default console
entry is the CRT startup `mainCRTStartup` (`user/crt0.c`), which parses
`argc`/`argv` from the command line, calls `int main(argc, argv, envp)`, and
calls `ExitProcess` with its return value. So a plain, portable C program —
`int main()`, `<stdio.h>`/`<stdlib.h>`/`<string.h>` (minimal headers under
`user/include/` that declare the `msvcrt` exports), no OS-specific code —
compiles the ordinary way and runs unchanged (`user/hello.c` → `hello.exe`).
`testapp.exe` is entered the same way. This is the foundation for eventually
loading ready-made Windows binaries: grow the reimplemented DLLs and the loader
(TLS, load-config, SEH, ...) until real programs' imports and startup are all
satisfied.

## Executive components

The directory names match NT's internal prefixes, so a symbol like
`MmAllocatePages` lives in `mm/`, `ObCreateObject` in `ob/`, and so on.

| Prefix | Directory | Responsibility                                             | State |
| ------ | --------- | ---------------------------------------------------------- | ----- |
| `Ke`   | `ke/`     | CPU control, interrupts/traps, scheduling, synchronization | working |
| `Hal`  | `hal/`    | port I/O, serial, VGA, framebuffer, PIC, PIT, IRQ dispatch  | working |
| `Mm`   | `mm/`     | physical & virtual memory, direct map, page tables         | working |
| `Ob`   | `ob/`     | object manager: object types, handles, namespace           | working |
| `Ps`   | `ps/`     | processes and threads (PEB/TEB, user process creation)     | early |
| `Io`   | `io/`     | ATA PIO, FAT32, File objects, handle-based file services    | early |
| `Cm`   | `cm/`     | Configuration Manager: the registry (keys, values, hive)    | early |
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
