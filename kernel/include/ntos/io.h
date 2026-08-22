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

typedef struct _FAT_FIND_DATA {
    char   Name[260];
    UINT32 Size;
    UINT32 Attributes;
    UINT16 WriteDate;
    UINT16 WriteTime;
} FAT_FIND_DATA;

/* Read a root file into a freshly allocated pool buffer. When supplied,
 * out_info receives the directory metadata used to create its File object. */
NTSTATUS FatLoadFile(const char *name, void **out_buffer, SIZE_T *out_size,
                     FAT_FIND_DATA *out_info);

/* Enumerate visible root entries (VFAT long names when present) by zero-based
 * index. This is the directory-query primitive used by Win32 find. */
NTSTATUS FatEnumerateRoot(UINT32 index, FAT_FIND_DATA *out);

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
UINT64 NtEnumerateRootFiles(UINT64 *args); /* private NTOS loader/runtime hook */

typedef struct _NTOS_FILE_INFO {
    UINT64 Size;
    UINT64 Position;
    UINT32 Attributes;
    UINT32 IsConsole;
    UINT16 FatWriteDate;
    UINT16 FatWriteTime;
    UINT32 Reserved;
} NTOS_FILE_INFO;

UINT64 NtQueryFileInfo(UINT64 *args); /* private NTOS loader/runtime hook */
UINT64 NtSetFilePosition(UINT64 *args); /* private NTOS loader/runtime hook */

#endif /* _NTOS_IO_H_ */
