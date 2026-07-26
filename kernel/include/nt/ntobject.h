/*
 * nt/ntobject.h - the object/IO parameter structures the native API uses.
 *
 * OBJECT_ATTRIBUTES (with its UNICODE_STRING name) is how NtCreateFile/NtOpenKey
 * name their target, and IO_STATUS_BLOCK is how the I/O services report a status
 * plus a result count. Field offsets match the Windows x64 layout so native
 * callers (and, eventually, a real ntdll) see exactly what they expect.
 */
#ifndef _NT_NTOBJECT_H_
#define _NT_NTOBJECT_H_

#include <nt/ntdef.h>

typedef ULONG ACCESS_MASK;

typedef struct _OBJECT_ATTRIBUTES {
    ULONG           Length;                   /* 0x00 */
    HANDLE          RootDirectory;            /* 0x08 */
    PUNICODE_STRING ObjectName;               /* 0x10 */
    ULONG           Attributes;               /* 0x18 */
    PVOID           SecurityDescriptor;       /* 0x20 */
    PVOID           SecurityQualityOfService; /* 0x28 */
} OBJECT_ATTRIBUTES, *POBJECT_ATTRIBUTES;

typedef struct _IO_STATUS_BLOCK {
    union {
        NTSTATUS Status;
        PVOID    Pointer;
    };
    ULONG_PTR Information; /* bytes transferred, disposition, ... */
} IO_STATUS_BLOCK, *PIO_STATUS_BLOCK;

/* CreateDisposition values for NtCreateFile. */
#define FILE_SUPERSEDE    0x00000000
#define FILE_OPEN         0x00000001
#define FILE_CREATE       0x00000002
#define FILE_OPEN_IF      0x00000003
#define FILE_OVERWRITE    0x00000004
#define FILE_OVERWRITE_IF 0x00000005

/* IoStatusBlock.Information results for a create/open. */
#define FILE_SUPERSEDED 0x00000000
#define FILE_OPENED     0x00000001
#define FILE_CREATED    0x00000002

#endif /* _NT_NTOBJECT_H_ */
