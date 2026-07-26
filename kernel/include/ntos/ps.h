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
PKTHREAD PsCreateUserProcess(const char *name, UINT64 entry, UINT64 image_base,
                             UINT64 stack_base, UINT64 stack_top);

/* The (single) user process's PEB virtual address, shared by its threads. */
#define PROCESS_PEB_VA 0x0000000000061000ULL
#define PROCESS_MAIN_TEB_VA 0x0000000000060000ULL

/* User region for the loader's module list (PEB_LDR_DATA + module entries +
 * name buffers), which PEB.Ldr points into. Two pages, below the image. */
#define PROCESS_LDR_VA 0x0000000000062000ULL
#define PROCESS_LDR_SIZE 0x2000ULL

/* User page for RTL_USER_PROCESS_PARAMETERS (command line, image path). */
#define PROCESS_PARAMS_VA 0x0000000000068000ULL

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
UINT64 NtCreateThreadEx(UINT64 *args);

#endif /* _NTOS_PS_H_ */
