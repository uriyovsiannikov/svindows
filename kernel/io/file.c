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
#include <ntos/hal.h>
#include <ntos/rtl.h>

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

/* Copy a NUL-terminated ASCII name out of user memory into a bounded buffer. */
static void capture_name(UINT64 user_ptr, char *out, SIZE_T out_size)
{
    const char *src = (const char *)user_ptr;
    SIZE_T i = 0;
    for (; i < out_size - 1 && src[i]; i++)
        out[i] = src[i];
    out[i] = '\0';
}

UINT64 NtCreateFile(UINT64 name_ptr, UINT64 a2, UINT64 a3, UINT64 a4)
{
    (void)a2; (void)a3; (void)a4;

    char name[128];
    capture_name(name_ptr, name, sizeof(name));

    /* A named object in the namespace (e.g. the console) opens directly. */
    POBJECT named;
    if (NT_SUCCESS(ObLookupObjectByName(name, &named))) {
        HANDLE h;
        NTSTATUS st = ObCreateHandle(named, GENERIC_ALL, &h);
        ObDereferenceObject(named); /* release the lookup reference */
        if (!NT_SUCCESS(st))
            return 0;
        KeLog("[io]   NtCreateFile('%s') -> handle %p (namespace)\n", name, h);
        return (UINT64)(ULONG_PTR)h;
    }

    /* Otherwise treat the last path component as a filesystem name. */
    const char *fname = name;
    for (int k = 0; name[k]; k++)
        if (name[k] == '\\' || name[k] == '/')
            fname = &name[k + 1];

    void *data;
    SIZE_T size;
    if (!NT_SUCCESS(FatLoadFile(fname, &data, &size))) {
        KeLog("[io]   NtCreateFile: '%s' not found\n", fname);
        return 0;
    }

    POBJECT obj;
    if (!NT_SUCCESS(ObCreateObject(g_file_type, sizeof(FILE_OBJECT), &obj))) {
        ExFreePool(data);
        return 0;
    }
    FILE_OBJECT *f = (FILE_OBJECT *)obj;
    f->Kind = FileKindDisk;
    f->Data = data;
    f->Size = size;
    f->Position = 0;

    HANDLE h;
    NTSTATUS st = ObCreateHandle(obj, GENERIC_ALL, &h);
    ObDereferenceObject(obj); /* creator reference; the handle keeps it alive */
    if (!NT_SUCCESS(st))
        return 0;

    KeLog("[io]   NtCreateFile('%s') -> handle %p (%lu bytes)\n",
          fname, h, (unsigned long)size);
    return (UINT64)(ULONG_PTR)h;
}

UINT64 NtReadFile(UINT64 handle, UINT64 buffer, UINT64 length, UINT64 a4)
{
    (void)a4;

    POBJECT obj;
    if (!NT_SUCCESS(ObReferenceObjectByHandle((HANDLE)(ULONG_PTR)handle, 0,
                                              g_file_type, &obj)))
        return 0;

    FILE_OBJECT *f = (FILE_OBJECT *)obj;
    UINT64 n = 0;
    if (f->Kind == FileKindDisk && f->Position < f->Size) {
        SIZE_T avail = f->Size - f->Position;
        n = length < avail ? length : avail;
        memcpy((void *)buffer, f->Data + f->Position, n);
        f->Position += n;
    }

    ObDereferenceObject(obj);
    return n;
}

UINT64 NtWriteFile(UINT64 handle, UINT64 buffer, UINT64 length, UINT64 a4)
{
    (void)a4;

    POBJECT obj;
    if (!NT_SUCCESS(ObReferenceObjectByHandle((HANDLE)(ULONG_PTR)handle, 0,
                                              g_file_type, &obj)))
        return 0;

    FILE_OBJECT *f = (FILE_OBJECT *)obj;
    UINT64 n = 0;
    if (f->Kind == FileKindConsole) {
        const char *p = (const char *)buffer;
        for (UINT64 i = 0; i < length; i++)
            HalConsolePutChar(p[i]);
        n = length;
    }

    ObDereferenceObject(obj);
    return n;
}

UINT64 NtClose(UINT64 handle, UINT64 a2, UINT64 a3, UINT64 a4)
{
    (void)a2; (void)a3; (void)a4;
    return (UINT64)ObCloseHandle((HANDLE)(ULONG_PTR)handle);
}
