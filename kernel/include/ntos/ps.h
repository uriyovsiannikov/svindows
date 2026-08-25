/*
 * ntos/ps.h - Process/thread manager (Ps).
 *
 * For now this covers creating a user "process": setting up its PEB and the
 * main thread's TEB and launching it in ring 3. Full EPROCESS/ETHREAD objects
 * and multi-process address spaces come later.
 */
#ifndef _NTOS_PS_H_
#define _NTOS_PS_H_

#include <nt/ntdef.h>
#include <ntos/ke.h>

/*
 * PsCreateUserProcess - build the user environment (PEB + TEB) and start the
 * program's main thread in ring 3.
 *
 * @entry:      user virtual address of the entry point.
 * @image_base: load base of the executable (stored in PEB.ImageBaseAddress).
 * @stack_base: lowest address of the user stack (TEB StackLimit).
 * @stack_top:  top of the user stack (TEB StackBase, initial RSP).
 */
PKTHREAD PsCreateUserProcess(const char *name, const char *command_line,
                             UINT64 entry, UINT64 image_base,
                             UINT64 stack_base, UINT64 stack_top,
                             UINT64 start_argument);

/*
 * The single user process's client id. It has to be even: a GDI handle's cell
 * carries the owning process id in a field whose low bit is a flag, and every
 * gdi32 handle check compares (cell.ProcessId & ~1) against its own cached
 * copy. An odd id therefore fails every check -- with an id of 1, gdi32
 * rejected every device context before it made a single syscall. Real Windows
 * process ids are multiples of four for the same reason.
 */
#define PROCESS_CLIENT_ID 4

/* The (single) user process's PEB virtual address, shared by its threads. */
#define PROCESS_PEB_VA 0x0000000000061000ULL
#define PROCESS_MAIN_TEB_VA 0x0000000000050000ULL
#define PROCESS_TEB_SIZE    0x0000000000002000ULL

/* User region for the loader's module list (PEB_LDR_DATA + module entries +
 * name buffers), which PEB.Ldr points into. Six pages, below the process
 * parameters, are enough for the real Explorer dependency graph. */
#define PROCESS_LDR_VA 0x0000000000062000ULL
#define PROCESS_LDR_SIZE 0x6000ULL

/* User page for RTL_USER_PROCESS_PARAMETERS (command line, image path). */
#define PROCESS_PARAMS_VA 0x0000000000068000ULL

/* The native GDI client indexes this table directly through PEB+0xF8. Each
 * GDI handle entry is 24 bytes and the handle index is 16 bits. */
#define PROCESS_GDI_SHARED_TABLE_VA   0x0000000001000000ULL
#define PROCESS_GDI_SHARED_TABLE_SIZE (0x10000ULL * 24ULL)

/* USER32's SHAREDINFO points at a parallel 16-bit handle table. */
#define PROCESS_USER_SHARED_TABLE_VA   0x0000000001180000ULL
#define PROCESS_USER_SHARED_TABLE_SIZE (0x10000ULL * 24ULL)
#define PROCESS_USER_OBJECT_ARENA_VA   0x0000000001300000ULL
/* 0x200 bytes per USER object head. Windows take indices 1..127;
 * cursors, accelerator tables and the other USER object types are
 * allocated above them, so the arena has to be larger than the window
 * range alone. */
#define PROCESS_USER_OBJECT_ARENA_SIZE 0x0000000000040000ULL

/* win32k's SERVERINFO ("gpsi"), reached through SHAREDINFO.psi: read-only
 * shared memory USER32 answers GetSystemMetrics/GetSysColor out of. The
 * supplied user32 build reads as far as gpsi+0x21F4, so four pages. */
#define PROCESS_SERVER_INFO_VA   0x0000000001320000ULL
#define PROCESS_SERVER_INFO_SIZE 0x0000000000004000ULL

/* User-writable arena for the per-object GDI attribute blocks (DC_ATTR and
 * friends) that a GDI cell's pUserInfo points at. GDI32 caches DC state there
 * -- SetTextColor and SetBkColor never enter the kernel -- so win32k has to
 * read the drawing colours back out of it. */
#define PROCESS_GDI_ATTR_VA   0x0000000001340000ULL
#define PROCESS_GDI_ATTR_SIZE 0x0000000000010000ULL

/* Register the Event and Thread object types. Requires Ob. */
void PsInitialize(void);

/*
 * Handle-based synchronization + thread services (signatures match the syscall
 * dispatcher: (a1, a2, a3, a4), result in the return value).
 *
 *   NtCreateEvent(notification, initial_state) -> HANDLE
 *   NtSetEvent(handle)                         -> previous state
 *   NtWaitForSingleObject(handle)              -> NTSTATUS
 *   NtCreateThread(entry, arg)                 -> thread HANDLE
 */
UINT64 NtCreateEvent(UINT64 *args);
UINT64 NtSetEvent(UINT64 *args);
UINT64 NtWaitForSingleObject(UINT64 *args);
UINT64 NtWaitForMultipleObjects(UINT64 *args);
UINT64 NtResetEvent(UINT64 *args);
UINT64 NtCreateSemaphore(UINT64 *args);
UINT64 NtReleaseSemaphore(UINT64 *args);
UINT64 NtQueryInformationProcess(UINT64 *args);
UINT64 NtCreateThreadEx(UINT64 *args);

/* Create a notification (manual-reset) Event object and a handle to it in the
 * current process, for kernel-owned events ring 3 must be able to wait on --
 * the per-thread USER input event backing MsgWaitForMultipleObjects. The
 * caller keeps `event_out` to signal it without going through the handle. */
NTSTATUS PsCreateNotificationEvent(PKEVENT *event_out, HANDLE *handle_out);

#endif /* _NTOS_PS_H_ */
