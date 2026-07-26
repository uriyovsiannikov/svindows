/*
 * io/file.c - File objects and the handle-based file services.
 *
 * Ties the object manager (Ob) to the I/O subsystem: NtCreateFile produces a
 * File object and a handle, NtReadFile/NtWriteFile operate through that handle,
 * and NtClose releases it (the File object's delete routine frees any cached
 * contents). A single console File object lives in the namespace at
 * \Device\Console; other names are read from the FAT filesystem.
 *
 * Simplifications vs. real NT: names are ASCII (no OBJECT_ATTRIBUTES /
 * UNICODE_STRING), whole files are cached on open, the filesystem is read-only,
 * and user pointers are trusted rather than probed/captured.
 */
#include <ntos/io.h>
#include <ntos/ob.h>
#include <ntos/ex.h>
#include <ntos/ke.h>
#include <ntos/mm.h>
#include <ntos/hal.h>
#include <ntos/rtl.h>
#include <nt/ntobject.h>

typedef enum _FILE_KIND {
    FileKindDisk = 0,
    FileKindConsole,
} FILE_KIND;

typedef struct _FILE_OBJECT {
    FILE_KIND Kind;
    UINT8    *Data;     /* cached contents for disk files (NULL for console) */
    SIZE_T    Size;
    SIZE_T    Position;
} FILE_OBJECT;

static POBJECT_TYPE g_file_type;
static POBJECT      g_console_file;

static void IopFileDelete(POBJECT object)
{
    FILE_OBJECT *f = (FILE_OBJECT *)object;
    if (f->Data)
        ExFreePool(f->Data);
}

void IoInitializeObjects(void)
{
    g_file_type = ObCreateObjectType("File", IopFileDelete);

    /* \Device directory + a shared console File object at \Device\Console. */
    struct _OBJECT_DIRECTORY *devdir;
    if (!NT_SUCCESS(ObCreateDirectory(ObRootDirectory, "Device", &devdir)))
        KeBugCheck(KE_PHASE0_INITIALIZATION_FAILED, "cannot create \\Device");

    if (!NT_SUCCESS(ObCreateObject(g_file_type, sizeof(FILE_OBJECT),
                                   &g_console_file)))
        KeBugCheck(KE_PHASE0_INITIALIZATION_FAILED, "cannot create console");

    FILE_OBJECT *cf = (FILE_OBJECT *)g_console_file;
    cf->Kind = FileKindConsole;
    cf->Data = NULL;
    cf->Size = 0;
    cf->Position = 0;

    ObInsertObjectByName(devdir, "Console", g_console_file);
    ObDereferenceObject(g_console_file); /* the namespace owns it now */

    KeLog("[io]   \\Device\\Console ready; File object type registered\n");
}

/*
 * NtCreateFile with the real Windows signature (through the argument array):
 *   a0 PHANDLE FileHandle (out)   a1 ACCESS_MASK DesiredAccess
 *   a2 POBJECT_ATTRIBUTES         a3 PIO_STATUS_BLOCK (out)
 *   a4 PLARGE_INTEGER Alloc       a5 ULONG FileAttributes
 *   a6 ULONG ShareAccess          a7 ULONG CreateDisposition
 *   a8 ULONG CreateOptions        a9 PVOID EaBuffer   a10 ULONG EaLength
 * User pointers are validated (and the name captured) before use.
 */
UINT64 NtCreateFile(UINT64 *a)
{
    PHANDLE            out_handle = (PHANDLE)a[0];
    POBJECT_ATTRIBUTES oa         = (POBJECT_ATTRIBUTES)a[2];
    PIO_STATUS_BLOCK   iosb       = (PIO_STATUS_BLOCK)a[3];

    if (!MmProbeForWrite((UINT64)out_handle, sizeof(HANDLE)) ||
        !MmProbeForWrite((UINT64)iosb, sizeof(IO_STATUS_BLOCK)) ||
        !MmProbeForRead((UINT64)oa, sizeof(OBJECT_ATTRIBUTES)))
        return (UINT64)STATUS_ACCESS_VIOLATION;

    char name[128];
    if (!MmCaptureUnicodeName((UINT64)oa->ObjectName, name, sizeof(name)))
        return (UINT64)STATUS_ACCESS_VIOLATION;

    HANDLE  h = NULL;
    NTSTATUS status = STATUS_UNSUCCESSFUL;
    ULONG_PTR info = FILE_OPENED;

    /* A named object in the namespace (e.g. the console) opens directly. */
    POBJECT named;
    if (NT_SUCCESS(ObLookupObjectByName(name, &named))) {
        status = ObCreateHandle(named, GENERIC_ALL, &h);
        ObDereferenceObject(named);
        KeLog("[io]   NtCreateFile('%s') -> handle %p (namespace)\n", name, h);
    } else {
        /* Otherwise treat the last path component as a filesystem name. */
        const char *fname = name;
        for (int k = 0; name[k]; k++)
            if (name[k] == '\\' || name[k] == '/')
                fname = &name[k + 1];

        void *data;
        SIZE_T size;
        if (!NT_SUCCESS(FatLoadFile(fname, &data, &size))) {
            KeLog("[io]   NtCreateFile: '%s' not found\n", fname);
            status = STATUS_OBJECT_NAME_NOT_FOUND;
        } else {
            POBJECT obj;
            if (!NT_SUCCESS(ObCreateObject(g_file_type, sizeof(FILE_OBJECT),
                                           &obj))) {
                ExFreePool(data);
                status = STATUS_NO_MEMORY;
            } else {
                FILE_OBJECT *f = (FILE_OBJECT *)obj;
                f->Kind = FileKindDisk;
                f->Data = data;
                f->Size = size;
                f->Position = 0;
                status = ObCreateHandle(obj, GENERIC_ALL, &h);
                ObDereferenceObject(obj);
                KeLog("[io]   NtCreateFile('%s') -> handle %p (%lu bytes)\n",
                      fname, h, (unsigned long)size);
            }
        }
    }

    if (NT_SUCCESS(status) && out_handle)
        *out_handle = h;
    if (iosb) {
        iosb->Status = status;
        iosb->Information = NT_SUCCESS(status) ? info : 0;
    }
    return (UINT64)status;
}

/*
 * NtReadFile / NtWriteFile real signature (through the argument array):
 *   a0 HANDLE File   a1 HANDLE Event   a2 PIO_APC_ROUTINE   a3 PVOID ApcContext
 *   a4 PIO_STATUS_BLOCK (out; Information = bytes)   a5 PVOID Buffer
 *   a6 ULONG Length  a7 PLARGE_INTEGER ByteOffset   a8 PULONG Key
 */
UINT64 NtReadFile(UINT64 *a)
{
    HANDLE           handle = (HANDLE)a[0];
    PIO_STATUS_BLOCK iosb   = (PIO_STATUS_BLOCK)a[4];
    void            *buffer = (void *)a[5];
    UINT64           length = a[6];

    if (!MmProbeForWrite((UINT64)iosb, sizeof(IO_STATUS_BLOCK)) ||
        !MmProbeForWrite((UINT64)buffer, length))
        return (UINT64)STATUS_ACCESS_VIOLATION;

    POBJECT obj;
    if (!NT_SUCCESS(ObReferenceObjectByHandle(handle, 0, g_file_type, &obj)))
        return (UINT64)STATUS_INVALID_HANDLE;

    FILE_OBJECT *f = (FILE_OBJECT *)obj;
    UINT64 n = 0;
    if (f->Kind == FileKindDisk && f->Position < f->Size) {
        SIZE_T avail = f->Size - f->Position;
        n = length < avail ? length : avail;
        memcpy(buffer, f->Data + f->Position, n);
        f->Position += n;
    }

    ObDereferenceObject(obj);
    if (iosb) {
        iosb->Status = STATUS_SUCCESS;
        iosb->Information = n;
    }
    return (UINT64)STATUS_SUCCESS;
}

UINT64 NtWriteFile(UINT64 *a)
{
    HANDLE           handle = (HANDLE)a[0];
    PIO_STATUS_BLOCK iosb   = (PIO_STATUS_BLOCK)a[4];
    const void      *buffer = (const void *)a[5];
    UINT64           length = a[6];

    if (!MmProbeForWrite((UINT64)iosb, sizeof(IO_STATUS_BLOCK)) ||
        !MmProbeForRead((UINT64)buffer, length))
        return (UINT64)STATUS_ACCESS_VIOLATION;

    POBJECT obj;
    if (!NT_SUCCESS(ObReferenceObjectByHandle(handle, 0, g_file_type, &obj)))
        return (UINT64)STATUS_INVALID_HANDLE;

    FILE_OBJECT *f = (FILE_OBJECT *)obj;
    UINT64 n = 0;
    if (f->Kind == FileKindConsole) {
        const char *p = (const char *)buffer;
        for (UINT64 i = 0; i < length; i++)
            HalConsolePutChar(p[i]);
        n = length;
    }

    ObDereferenceObject(obj);
    if (iosb) {
        iosb->Status = STATUS_SUCCESS;
        iosb->Information = n;
    }
    return (UINT64)STATUS_SUCCESS;
}

UINT64 NtClose(UINT64 *a)
{
    return (UINT64)ObCloseHandle((HANDLE)a[0]);
}
