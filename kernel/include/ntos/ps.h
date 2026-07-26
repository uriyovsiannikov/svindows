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

#endif /* _NTOS_PS_H_ */
