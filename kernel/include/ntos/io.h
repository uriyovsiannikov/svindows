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
 * Handle-based Nt* file services with the real Windows signatures (via the
 * argument array the syscall entry passes). See io/file.c for the parameter
 * layouts (OBJECT_ATTRIBUTES / IO_STATUS_BLOCK).
 */
UINT64 NtCreateFile(UINT64 *args);
UINT64 NtReadFile(UINT64 *args);
UINT64 NtWriteFile(UINT64 *args);
UINT64 NtClose(UINT64 *args);

#endif /* _NTOS_IO_H_ */
