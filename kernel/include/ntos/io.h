/*
 * ntos/io.h - I/O manager (Io): block device + filesystem access.
 *
 * A pragmatic first cut: a polled ATA PIO driver for the primary disk and a
 * read-only FAT32 driver on top of it, enough to read executables off a disk
 * image instead of embedding them in the kernel.
 */
#ifndef _NTOS_IO_H_
#define _NTOS_IO_H_

#include <nt/ntdef.h>
#include <nt/ntstatus.h>

/* ------------------------------------------------------------------ */
/* ATA PIO block device (primary bus, master)                         */
/* ------------------------------------------------------------------ */

#define ATA_SECTOR_SIZE 512

BOOLEAN AtaInitialize(void);
/* Read `count` (1..255) 512-byte sectors starting at LBA into `buffer`. */
BOOLEAN AtaReadSectors(UINT32 lba, UINT8 count, void *buffer);

/* ------------------------------------------------------------------ */
/* FAT32 read-only filesystem                                         */
/* ------------------------------------------------------------------ */

NTSTATUS FatMount(void);

/*
 * FatLoadFile - read a file from the root directory into a freshly allocated
 * pool buffer. The caller frees *out_buffer with ExFreePool.
 */
NTSTATUS FatLoadFile(const char *name, void **out_buffer, SIZE_T *out_size);

/* Bring up the I/O subsystem (ATA + mount FAT + I/O objects). */
NTSTATUS IoInitialize(void);

/* ------------------------------------------------------------------ */
/* File objects and the handle-based file services                    */
/* ------------------------------------------------------------------ */

/* Register the File object type and create \Device\Console. Requires Ob. */
void IoInitializeObjects(void);

/*
 * Handle-based Nt* file services (simplified: names are ASCII, no
 * OBJECT_ATTRIBUTES/UNICODE_STRING yet). Signatures match the syscall
 * dispatcher: (a1, a2, a3, a4), result in the return value.
 *
 *   NtCreateFile(name)                 -> HANDLE (0 on failure)
 *   NtReadFile(handle, buffer, length) -> bytes read
 *   NtWriteFile(handle, buffer, length)-> bytes written
 *   NtClose(handle)                    -> NTSTATUS
 */
UINT64 NtCreateFile(UINT64 name, UINT64 a2, UINT64 a3, UINT64 a4);
UINT64 NtReadFile(UINT64 handle, UINT64 buffer, UINT64 length, UINT64 a4);
UINT64 NtWriteFile(UINT64 handle, UINT64 buffer, UINT64 length, UINT64 a4);
UINT64 NtClose(UINT64 handle, UINT64 a2, UINT64 a3, UINT64 a4);

#endif /* _NTOS_IO_H_ */
