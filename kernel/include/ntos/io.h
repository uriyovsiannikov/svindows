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

/* Bring up the I/O subsystem (ATA + mount FAT). */
NTSTATUS IoInitialize(void);

#endif /* _NTOS_IO_H_ */
