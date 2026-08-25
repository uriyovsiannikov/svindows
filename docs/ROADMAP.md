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
- [x] Synchronization objects: events, mutants, and counting semaphores over
      the common dispatcher header and wait lists.

## Phase 3 — Threads, processes, scheduler (`Ke`, `Ps`) (in progress)

- [x] `KTHREAD` / `KPROCESS` and the context switch (`KiSwitchContext`).
- [x] Preemptive scheduler driven by the 8254 PIT via the remapped 8259 PIC;
      round-robin with per-thread quanta, `KeCreateThread` / `KeYield` /
      `KeTerminateThread`.
- [x] Thread synchronization: dispatcher (waitable) objects with a
      DISPATCHER_HEADER, thread blocking/waking, and `KeWaitForSingleObject`.
      Events (notification + auto-reset), semaphores, and mutants share the
      wait/wake machinery; threads are waitable and signal on exit.
- [x] `KeWaitForMultipleObjects` with wait-any/wait-all signal consumption,
      poll/infinite/relative/absolute timeouts, and clock-driven wakeup.
- [ ] Priority-based ordering (the `Priority` field exists but scheduling is
      currently round-robin).
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
- [x] Per-thread **TEB** and per-process **PEB** at the Windows x64 offsets,
      reached through the GS segment in ring 3 (`gs:[0x30]` = TEB self,
      `gs:[0x60]` = PEB). Correct swapgs model (conditional on ring transition,
      per-thread `KERNEL_GS_BASE`) so it survives preemption between user and
      kernel threads.
- [x] `Ps` bring-up: `PsCreateUserProcess` builds the PEB/TEB and launches the
      main thread.
- [x] `NtAllocateVirtualMemory` — the first `Nt*` service with real (memory)
      semantics rather than a debug helper.
- [ ] APIC + IOAPIC bring-up; keyboard interrupt (still on the 8259 PIC / PIT).
- [ ] Capture/validate user-mode pointers (SEH-style probing) instead of
      trusting them.
- [x] `NtCreateThread` (spawn a ring-3 thread in the current process, waitable
      on exit) and `NtCreateEvent` / `NtSetEvent` / `NtWaitForSingleObject`
      (wait on an event or a thread handle) — user-mode multithreading.
- [ ] Match the real Windows `Nt*` numbers/signatures and grow the set
      (`NtCreateFile`, `NtWriteFile`, ... with proper parameter blocks).

## Phase 5 — I/O manager (`Io`) (in progress)

- [x] A polled **ATA PIO** block driver (primary bus, 28-bit LBA).
- [x] A read-only **FAT32** driver (BPB, FAT chain, root directory, 8.3 names).
- [x] The loader reads `testapp.exe` and its `ntdll.dll` dependency **from a
      disk image** instead of embedded blobs.
- [x] A **File object type** (Ob) and a console device at `\Device\Console`;
      handle-based **`NtCreateFile` / `NtReadFile` / `NtWriteFile` / `NtClose`**
      that open FAT files or the console and read/write through handles.
- [x] The syscall entry preserves the caller's `RDI`/`RSI` (Windows
      non-volatile), so the full Windows non-volatile set survives a syscall.
- [ ] Device / driver objects and the IRP model (currently direct calls).
- [ ] Write support to disk; `OBJECT_ATTRIBUTES`/`UNICODE_STRING` names and
      user-pointer probing/capture instead of trusting user pointers.

## Phase 6 — The NT user-mode boundary (in progress)

- [x] PE/COFF image loader in the kernel (`LdrLoadPeImage`): validates the
      MZ/PE headers, maps `SizeOfImage`, lays out sections at their RVAs with
      per-section permissions, zero-fills BSS, and applies base relocations.
- [x] Load and run the first native PE `.exe` in ring 3 — an image built by the
      standard toolchain (`nasm -f win64` + `lld-link`), embedded in the kernel.
- [x] A real `ntdll.dll` (built as a PE with an export table) providing the
      Windows-form syscall stubs (`mov r10, rcx; syscall`), plus PE import (IAT)
      resolution: the loader parses the import directory, loads `ntdll` on
      demand, resolves each name against its export directory, and patches the
      executable's IAT — the authentic Windows dynamic-linking flow. The syscall
      ABI now matches Windows (args in `R10`/`RDX`/`R8`/`R9`).
- [x] Load PE images from a filesystem (needs Phase 5) rather than an embedded
      blob; a proper module list (`PEB_LDR_DATA` with a `LDR_DATA_TABLE_ENTRY`
      per module at the Windows x64 layout, built by `LdrBuildProcessModuleList`).
- [ ] Forwarded exports, bound imports, and TLS callbacks.
- [x] A **Win32 subsystem library `kernel32.dll`** (built from C, over ntdll)
      with `GetStdHandle` / `WriteFile` / `ReadFile` / `CreateFileA` /
      `CloseHandle` / `CreateThread` / `WaitForSingleObject` / `ExitProcess`,
      and a **normal Win32 program** that links against it — the loader resolves
      the full `app.exe -> kernel32.dll -> ntdll.dll` import chain from disk.
- [x] The **dynamic runtime**: `GetModuleHandleA` (walks `PEB->Ldr`),
      `GetProcAddress` (parses the PE export directory), and a process heap
      (`GetProcessHeap` / `HeapCreate` / `HeapAlloc` / `HeapFree`) over
      `NtAllocateVirtualMemory`. SSE enabled at boot for compiler-emitted XMM.
- [x] `LoadLibraryA` — load a DLL from disk at runtime (`NtLoadLibrary`) and
      link it into `PEB->Ldr`; the app loads a standalone `extra.dll` (not in
      its import chain), resolves its exports, and calls them. XMM/FPU state is
      now preserved across context switches (FXSAVE/FXRSTOR).
- [x] A **registry**: a Configuration Manager (`Cm`) with an in-memory
      key/value tree (keys are `Key` objects), `NtCreateKey` / `NtOpenKey` /
      `NtSetValueKey` / `NtQueryValueKey`, and an `advapi32.dll` with the
      `RegOpenKeyExA` / `RegCreateKeyExA` / `RegSetValueExA` / `RegQueryValueExA`
      / `RegCloseKey` surface. (On-disk hives are still future work.)
- [x] Match the real Windows `Nt*` **signatures and numbers** for the core:
      the syscall entry marshals up to 11 arguments (registers + user stack),
      `NtCreateFile`/`NtReadFile`/`NtWriteFile` use `OBJECT_ATTRIBUTES` /
      `UNICODE_STRING` / `IO_STATUS_BLOCK` at their true arities,
      `NtAllocateVirtualMemory` is the 6-argument form, and the service table is
      indexed by Windows 7 SP1 x64 numbers. (Registry/thread services still use
      simplified signatures; the exact numbers should be checked against a
      reference table before driving a real ntdll.)
- [x] Broaden the Win32 surface + a mini-CRT: a `KUSER_SHARED_DATA` page at
      `0x7FFE0000` backs `GetTickCount`/`GetSystemTimeAsFileTime` with no
      syscall; `Sleep` (`NtDelayExecution`); `GetCommandLineA` from
      `PEB->ProcessParameters`; `lstrlenA`/`lstrcpyA`/`lstrcatA`/`wsprintfA`; and
      an `msvcrt.dll` (`printf`/`malloc`/`free`/`str*`/`mem*`) over the Win32 API.
- [x] Extend real signatures to the rest and probe/capture user pointers:
      `NtOpenKey`/`NtCreateKey` take `OBJECT_ATTRIBUTES`, `NtQueryValueKey`
      returns `KEY_VALUE_PARTIAL_INFORMATION`, threads use `NtCreateThreadEx`,
      and `NtWaitForSingleObject` takes alertable+timeout; the kernel validates
      user buffers (`MmProbeForRead`/`MmProbeForWrite`) and captures object names
      (`MmCaptureUnicodeName`) before use.
- [x] A **CRT startup** (`crt0.c`'s `mainCRTStartup`) + minimal libc headers, so
      a plain portable C program (`int main()`, `<stdio.h>`/`<stdlib.h>`/
      `<string.h>`, no OS-specific code) compiles and runs unchanged (`hello.exe`).
- [x] A wider Win32 surface real binaries import: `GetCurrentProcessId`/
      `GetCurrentThreadId`/`GetLastError`/`SetLastError` (TEB), `GetModuleFileNameA`,
      `VirtualAlloc`/`VirtualFree`/`VirtualProtect` (over a new
      `NtProtectVirtualMemory`), `QueryPerformanceCounter`/`QueryPerformanceFrequency`,
      the `Interlocked*` family, and recursive critical sections.
- [x] Loader tolerance for real binaries: an image that imports functions we
      don't provide yet **loads anyway** — the loader logs each missing import
      and points the IAT at a return-0 stub (the load-and-report loop that
      bootstraps toward unmodified binaries).
- [x] Run a stock, unmodified Windows 11 x64 console binary: `hostname.exe`
      resolves its API-set imports, enters through the Microsoft CRT startup,
      calls the NTOS Winsock facade, prints `NTOS`, and exits normally.
- [x] Win32 thread-local storage (`TlsAlloc`/`TlsFree`/`TlsGetValue`/
      `TlsSetValue`) backed by each thread's TEB, with real per-thread IDs in
      child TEBs so owner-based synchronization and TLS remain isolated.
- [x] A usable Win32 dispatcher/thread-pool base: timed single and multiple
      waits, events (`SetEvent`/`ResetEvent`), semaphores, a bounded persistent
      four-worker work queue, and one shared monitor for thread-pool waits
      instead of allocating a new OS thread for every wait arm.
- [x] Ring-3 faults no longer bugcheck the operating system. Until user-mode
      SEH is implemented, the trap dispatcher records the complete fault and
      terminates only the offending user thread; kernel faults remain fatal.
- [ ] Keep growing toward *unmodified, ready-made* Windows binaries: the PE TLS
      directory and callbacks (plus static TEB TLS slots/expansion slots),
      a fuller CRT/`msvcrt`, load-config (security cookie) handling, kernel SEH
      (`.pdata`/`RUNTIME_FUNCTION`) so exceptions and fault-safe probes work, and
      eventually `user32`/`gdi32` for GUI programs — the ReactOS/Wine-scale
      endgame that leads to a real desktop shell.

## Phase 7 — Graphics and the road to a desktop (in progress)

The north star is a graphical shell — eventually a real Windows desktop
(`explorer.exe`). That is a ReactOS-scale endeavour; this phase starts at the
bottom of the graphics stack and builds up.

- [x] Request a 32-bpp **linear framebuffer** from GRUB via the Multiboot2
      framebuffer tag, parse the framebuffer info tag, and map the framebuffer
      into the kernel direct map.
- [x] **Graphics primitives**: put-pixel, filled rectangles, screen clear, and
      an 8×16 **bitmap font** with glyph and string drawing.
- [x] The early framebuffer console and mock desktop were useful bring-up
      milestones and were verified in QEMU, then deliberately removed from the
      boot path. The framebuffer now starts blank: only USER/GDI and
      `explorer.exe` are allowed to become the visible desktop.
- [x] A **PS/2 keyboard** (IRQ1, scancode→ASCII ring buffer) and **PS/2 mouse**
      (IRQ12, movement packets) input stack, with a software **mouse cursor**
      that follows the mouse (save/restore under the sprite).
- [x] **The stock Windows 10/11 `explorer.exe` boots and initializes as the
      shell.** It resolves its ~950 imports across 60 DLLs (the inbox USER32,
      GDI32, SHELL32, SHCORE, combase, DWrite, dwmapi, ... from `win/` plus
      NTOS kernel32/advapi32/msvcrt), runs all 27 DLL entry points, decides it
      *is* the registered shell (`HKLM\...\Winlogon\AlternateShells\
      AvailableShells`), initializes the accent-color palette (reading and
      writing real registry keys), writes `StartMenuInit`, spawns its worker
      threads, and only then stops at the desktop-thread bootstrap hand-off.
      Supporting work done along the way: `NtdllDefWindowProc_W/A` (USER32's
      forwarded DefWindowProc), ~60 new kernel32 exports (PE resources,
      Global/Local memory, Fls*, code pages, file mappings, VirtualQuery,
      timer queues...), advapi32 SID/token APIs and Reg*W variants, a
      kernel-side win32k service layer keyed by the *real* win32u.dll syscall
      numbers extracted from the binary itself (window classes + atoms,
      properties, window longs, post/show/setwindowpos/timers, desktop and
      window-station stubs) with a return-0 fallback for unimplemented ones,
      sign-extended-HKEY handling in advapi32, GetNativeSystemInfo reporting
      PROCESSOR_ARCHITECTURE_AMD64 (dwmapi requires it before wiring its
      function table), and user-stack symbolization in trap/terminate dumps.
- [x] **Window classes register end-to-end and the shell windows exist.**
      The desktop-thread bootstrap now runs the full USER pipeline:
      `RegisterClassExWOW` fires with real class names — `Worker Window`,
      `Shell_TrayWnd` (the taskbar), `WorkerW` (the desktop host) — each
      receiving a `0xC000`-range atom, and `NtUserCreateWindowEx` creates
      their windows (taskbar HWND 0x10002, WorkerW 0x10003). Unblocking work:
      the loader routes every `*ntuser*` API-set contract (regular and delay
      imports) to the genuine user32.dll; kernel32 grew the full atom family
      (`GlobalAddAtom/Find/GetAtomName` A+W, integer atoms, `*Ex*`) that
      user32's class-name capture consumes; ntdll answers
      `RtlFindActivationContextSectionString` with the canonical
      `STATUS_SXS_SECTION_NOT_FOUND` (any other negative status made user32
      silently fail every registration); user32's class-flag pair
      (`0xBC920/0xBD130`) is synced at load so the client class-list walk is
      skipped; `gdi32!GdiValidateHandle` answers TRUE; per-thread posted-
      message queues stopped the desktop thread stealing the main thread's
      hand-off messages; msvcrt gained the CRT leaf surface the inbox shell
      links (`realloc`, `_lock/_unlock`, math/time, `_s` strings, and
      `std::exception` under its MSVC-mangled names); and
      `NtUserChangeWindowMessageFilterEx` grants the taskbar's message
      filter assertion after WorkerW creation.
- [x] **The desktop-thread bootstrap runs to completion with no faults.**
      After the WorkerW is created the SHCORE desktop thread asserts its
      message filter (`NtUserChangeWindowMessageFilterEx`), checks the
      App Paths key, reads the window-station/desktop identity
      (`NtUserGetObjectInformation` answering `WinSta0`/`Default`),
      posts its hand-off messages to the main thread, and exits cleanly —
      no trap. The NULL crash that used to kill it here was
      `PathFindExtensionW`: kernel32 now hosts the shlwapi path helpers
      (`PathFindFileName/Extension` A+W) instead of stubbing them to 0
      (mapping the whole shlwapi contract family to the genuine
      shlwapi.dll was tried first and destabilized other paths, so the
      two needed functions are implemented directly). msvcrt also gained
      `wcsncmp/strncmp/_wcsnicmp/_strnicmp`.
- [x] **The shell's main thread survives window creation and reaches its
      message pump.** With the kernel→user callback machinery in place the
      first `CreateWindowExW` ('Worker Window') still ended in a ring-3
      page fault whose return address was stack garbage. GDB over the QEMU
      gdbstub traced it: the genuine user32 validates every HWND purely in
      user mode against the shared `HANDLEENTRY` table (type byte must be
      1/TYPE_WINDOW), and that table — mapped user-writable — was being
      corrupted between creation and validation, so CreateWindowExW took
      its ERROR_INVALID_WINDOW_HANDLE path and smashed its own frame.
      Fix: the client-visible handle table is now kernel-owned read-only
      for ring 3 (as win32k's is), with write access enabled transiently
      around kernel-side fills (`MmProtectRange`); any client that writes
      it now traps with a nameable RIP instead of corrupting state. The
      full bootstrap now runs clean: Worker Window → Shell_TrayWnd taskbar
      → WorkerW desktop host, each receiving WM_NCCREATE/WM_CREATE through
      the callback dispatcher, ~60 Win-key hotkeys registered on the
      taskbar, per-thread USER input events backing
      MsgWaitForMultipleObjects, and tid 1 parked in GetMessage waiting
      for work — the outcome in the majority of boots.
      Residual, ~30% of boots: a timing-dependent ring-3 fault lands right
      after the first `CreateWindowEx` callback chain completes (fingerprint:
      RIP=RBP inside the thread stack, CR2 = the new HWND, RCX/RDX still
      holding the redirect's OrigRip/OrigRsp). The kernel-side return path is
      verified correct under GDB — the final redirect lands with the exact
      expected user frame — so the divergence is inside genuine user32's own
      post-create code (a function-pointer call near USER32+0xd455 whose
      target comes out as stack garbage in bad runs), most likely sensitive
      to loader relocation/delay-import handling or client state we do not
      model yet. Even in crashing runs the shell's other threads finish the
      taskbar bring-up (Shell_TrayWnd + WorkerW created with full callback
      chains, ~71 hotkeys registered); the crash kills only tid 1.
- [x] **The desktop paints.** A minimal win32k-lite layer in the USER
      syscall path classifies the shell's own windows by class name and
      draws them on the linear framebuffer: WorkerW fills the screen with
      a wallpaper color, Shell_TrayWnd becomes a bottom taskbar strip
      (accent line, start box, "NTOS" label), generic windows get a plain
      bordered surface. Painting fires on create/show/hide/move/destroy;
      desktop and taskbar are painted immediately at creation since the
      shell's ShowWindow/SetWindowPos traffic does not reach those services
      yet. Verified by QEMU `screendump` pixel probes (wallpaper
      #0e2a47 across the screen, #1f1f1f taskbar strip in the bottom 40
      rows). This is bring-up scaffolding: the real milestone remains a
      gdi32 DC surface so explorer paints itself through WM_PAINT.
- [x] **The shell bootstrap is deterministic.** One use-after-free was behind
      both the late-boot `STATUS_NO_MEMORY` and the timing-dependent ring-3
      fault above. `NtCreateThreadEx` handed the KTHREAD a raw pointer to the
      waitable thread object (`KTHREAD.TerminationObject`) without a
      reference, and the shell's thread pool closes a thread handle long
      before that thread exits; the object was freed, its 48 bytes were
      recycled and zeroed by the next `ObCreateObject`, and thread exit then
      ran `KiSignalObject` over a stale wait list — writing list pointers into
      whatever now owned that memory. When the victim was a pool block header
      the free list stopped being reusable, so the arena grew to ~188 MiB with
      the in-use figure frozen and `NtSetValueKey` started failing at
      `StartMenuInit`; when it was live user state, tid 1 died after its first
      `CreateWindowEx`. The KTHREAD now holds a reference for as long as it
      keeps the pointer and releases it right after signaling. Alongside the
      fix the pool grew self-checking: a `'POOL'` header magic, a 16-byte
      guard past every payload that names the overrunning allocation by tag on
      free, and a periodic walk asserting that blocks still tile the arena.
      Boots are now byte-identical run to run: 3 arena growths, no traps, no
      corruption, `StartMenuInit` written successfully.
- [x] **Update regions and WM_PAINT.** Windows carry an update region and the
      owning thread's message loop synthesizes `WM_PAINT` from it, as win32k
      does (paint messages are never queued): create-visible, `ShowWindow`,
      `SetWindowPos` and `NtUserInvalidateRect`/`NtUserRedrawWindow` mark a
      window dirty and signal the owner's USER input event, `GetMessage` /
      `PeekMessage` / `WaitMessage` produce the message from it, and
      `NtUserBeginPaint` (with a correctly sized x64 PAINTSTRUCT and a real
      `rcPaint`) / `NtUserValidateRect` clear it. Redelivery is capped so a
      client that ignores the message cannot livelock its own loop. The shell
      does receive the taskbar's `WM_PAINT` and does not yet act on it: it
      never reaches `NtUserDispatchMessage`, and its windows are still created
      0x0 because the `SetWindowPos` that would size them never arrives — the
      next thing to trace.
- [ ] `NtUserBeginPaint` still returns a NULL HDC: there is no GDI surface to
      hand out. The GDI shared handle table is mapped and published at
      `PEB->GdiSharedHandleTable`, but no cell is ever filled, so the genuine
      gdi32 has nothing to validate. Next: kernel DC/surface objects behind
      real GDICELL64 entries, then `NtGdiPatBlt`/`BitBlt`/`ExtTextOutW` onto
      the framebuffer, so `BeginPaint` returns a DC the shell can draw into.
- [ ] A window/compositor model (drawing windows, z-order, dirty rectangles).
- [ ] `win32k`-style kernel graphics + a `gdi32`/`user32` surface so Win32 GUI
      programs can create windows and paint — the bridge from console programs
      to a real desktop shell.

## Guiding rule

Every milestone must boot in QEMU and print evidence of success over the serial
port before it is marked done. Correctness and clarity first — speed later.
