/*
 * ntos/ob.h - the Object Manager (Ob).
 *
 * Every shareable kernel entity (a directory, an event, later a process, a
 * file, ...) is an "object": a type-specific body preceded by a common
 * OBJECT_HEADER that carries the reference/handle counts, the object type, and
 * its optional place in the object namespace. Objects are reference-counted;
 * when the last pointer reference goes away the type's delete routine runs and
 * the memory is returned to the pool.
 *
 * Handles are the user-mode-facing references: an entry in a handle table that
 * pairs an object with a granted ACCESS_MASK.
 */
#ifndef _NTOS_OB_H_
#define _NTOS_OB_H_

#include <nt/ntdef.h>
#include <nt/ntstatus.h>

typedef PVOID POBJECT;
struct _OBJECT_TYPE;
struct _OBJECT_DIRECTORY;

/* Type-specific teardown, invoked when an object's last reference is released. */
typedef VOID (*OB_DELETE_METHOD)(POBJECT Object);

/*
 * OBJECT_TYPE - describes a class of objects. Unlike Windows, a type is not
 * itself an object here (that avoids a bootstrap cycle); it is a plain pool
 * allocation linked into a global list.
 */
typedef struct _OBJECT_TYPE {
    LIST_ENTRY       TypeListEntry;
    const char      *Name;            /* e.g. "Directory", "Event" */
    ULONG            TotalObjects;
    ULONG            TotalHandles;
    SIZE_T           DefaultBodySize; /* informational */
    OB_DELETE_METHOD DeleteProcedure; /* may be NULL */
} OBJECT_TYPE, *POBJECT_TYPE;

/*
 * OBJECT_HEADER - the common prefix in front of every object body. The body
 * pointer that callers hold is (header + 1).
 */
typedef struct _OBJECT_HEADER {
    volatile LONG            PointerCount; /* live references (incl. handles) */
    volatile LONG            HandleCount;  /* open handles                    */
    POBJECT_TYPE             Type;
    const char              *Name;         /* leaf name if in the namespace   */
    LIST_ENTRY               NamespaceEntry;
    struct _OBJECT_DIRECTORY *ParentDirectory;
} OBJECT_HEADER, *POBJECT_HEADER;

/* Header <-> body conversions. */
static ALWAYS_INLINE POBJECT_HEADER ObHeaderFromObject(POBJECT o)
{
    return (POBJECT_HEADER)o - 1;
}
static ALWAYS_INLINE POBJECT ObObjectFromHeader(POBJECT_HEADER h)
{
    return (POBJECT)(h + 1);
}

/* ------------------------------------------------------------------ */
/* Types and objects                                                  */
/* ------------------------------------------------------------------ */

void         ObInitialize(void);
POBJECT_TYPE ObCreateObjectType(const char *name, OB_DELETE_METHOD del);

NTSTATUS ObCreateObject(POBJECT_TYPE type, SIZE_T body_size, POBJECT *out_object);
void     ObReferenceObject(POBJECT object);
void     ObDereferenceObject(POBJECT object);
LONG     ObGetReferenceCount(POBJECT object);

/* ------------------------------------------------------------------ */
/* Namespace (directories)                                            */
/* ------------------------------------------------------------------ */

extern struct _OBJECT_DIRECTORY *ObRootDirectory;

NTSTATUS ObCreateDirectory(struct _OBJECT_DIRECTORY *parent, const char *name,
                           struct _OBJECT_DIRECTORY **out_dir);

/* Insert `object` under `dir` with leaf name `name` (the object gains a ref
 * held by the namespace). */
NTSTATUS ObInsertObjectByName(struct _OBJECT_DIRECTORY *dir, const char *name,
                              POBJECT object);

/* Look up an absolute path ("\Foo\Bar"). On success the returned object has an
 * extra reference the caller must release with ObDereferenceObject. */
NTSTATUS ObLookupObjectByName(const char *path, POBJECT *out_object);

/* ------------------------------------------------------------------ */
/* Handles                                                            */
/* ------------------------------------------------------------------ */

void     ObInitializeHandleTable(void);

/* Create a handle referring to `object` (the object gains a handle + pointer
 * reference). */
NTSTATUS ObCreateHandle(POBJECT object, ACCESS_MASK access, HANDLE *out_handle);

/* Resolve a handle to its object. On success the object gains a pointer
 * reference the caller releases with ObDereferenceObject. */
NTSTATUS ObReferenceObjectByHandle(HANDLE handle, ACCESS_MASK desired,
                                   POBJECT_TYPE type, POBJECT *out_object);

/* Close a handle (drops its references; may delete the object). */
NTSTATUS ObCloseHandle(HANDLE handle);

#endif /* _NTOS_OB_H_ */
