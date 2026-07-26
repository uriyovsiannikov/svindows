/*
 * ob/ob_namespace.c - the object namespace: Directory objects and path lookup.
 *
 * Directories are ordinary objects whose body is a list of the objects they
 * contain. The root directory ("\") anchors the tree; absolute paths like
 * "\Device\Serial0" are resolved segment by segment.
 */
#include <ntos/ob.h>
#include <ntos/ke.h>
#include <ntos/rtl.h>
#include "obp.h"

POBJECT_TYPE ObpDirectoryType;
struct _OBJECT_DIRECTORY *ObRootDirectory;

static void ObpDirectoryDelete(POBJECT object)
{
    OBJECT_DIRECTORY *dir = (OBJECT_DIRECTORY *)object;
    if (!IsListEmpty(&dir->Entries))
        KeLog("[ob]   WARNING: deleting a non-empty directory\n");
}

void ObpInitializeNamespace(void)
{
    ObpDirectoryType = ObCreateObjectType("Directory", ObpDirectoryDelete);

    POBJECT root;
    if (!NT_SUCCESS(ObCreateObject(ObpDirectoryType, sizeof(OBJECT_DIRECTORY),
                                   &root)))
        KeBugCheck(KE_PHASE0_INITIALIZATION_FAILED,
                   "cannot create object namespace root");

    OBJECT_DIRECTORY *dir = (OBJECT_DIRECTORY *)root;
    InitializeListHead(&dir->Entries);
    ObHeaderFromObject(root)->Name = "\\";
    ObRootDirectory = dir;

    KeLog("[ob]   namespace root '\\' created\n");
}

NTSTATUS ObCreateDirectory(struct _OBJECT_DIRECTORY *parent, const char *name,
                           struct _OBJECT_DIRECTORY **out_dir)
{
    POBJECT obj;
    NTSTATUS status = ObCreateObject(ObpDirectoryType, sizeof(OBJECT_DIRECTORY),
                                     &obj);
    if (!NT_SUCCESS(status))
        return status;

    OBJECT_DIRECTORY *dir = (OBJECT_DIRECTORY *)obj;
    InitializeListHead(&dir->Entries);

    if (parent) {
        status = ObInsertObjectByName(parent, name, obj);
        if (!NT_SUCCESS(status)) {
            ObDereferenceObject(obj);
            return status;
        }
    }

    *out_dir = dir;
    return STATUS_SUCCESS;
}

/* Find a leaf named [seg, seg+seglen) directly inside `dir`. */
static POBJECT_HEADER ObpFindInDirectory(OBJECT_DIRECTORY *dir,
                                         const char *seg, SIZE_T seglen)
{
    for (PLIST_ENTRY e = dir->Entries.Flink; e != &dir->Entries; e = e->Flink) {
        POBJECT_HEADER h = CONTAINING_RECORD(e, OBJECT_HEADER, NamespaceEntry);
        if (h->Name && strlen(h->Name) == seglen &&
            memcmp(h->Name, seg, seglen) == 0)
            return h;
    }
    return NULL;
}

NTSTATUS ObInsertObjectByName(struct _OBJECT_DIRECTORY *dir, const char *name,
                              POBJECT object)
{
    if (!dir || !name || !object)
        return STATUS_INVALID_PARAMETER;

    if (ObpFindInDirectory(dir, name, strlen(name)))
        return STATUS_OBJECT_NAME_COLLISION;

    POBJECT_HEADER h = ObHeaderFromObject(object);
    h->Name = name;
    h->ParentDirectory = dir;
    InsertTailList(&dir->Entries, &h->NamespaceEntry);

    /* The namespace holds a reference for as long as the object is linked. */
    ObReferenceObject(object);
    return STATUS_SUCCESS;
}

NTSTATUS ObLookupObjectByName(const char *path, POBJECT *out_object)
{
    if (!path || path[0] != '\\' || !out_object)
        return STATUS_OBJECT_NAME_INVALID;

    OBJECT_DIRECTORY *dir = ObRootDirectory;
    const char *p = path + 1; /* skip the leading '\' */

    /* "\" alone resolves to the root directory. */
    if (*p == '\0') {
        POBJECT root = ObObjectFromHeader(ObHeaderFromObject(dir));
        ObReferenceObject(root);
        *out_object = root;
        return STATUS_SUCCESS;
    }

    for (;;) {
        const char *seg = p;
        while (*p && *p != '\\')
            p++;
        SIZE_T seglen = (SIZE_T)(p - seg);
        if (seglen == 0)
            return STATUS_OBJECT_NAME_INVALID;

        POBJECT_HEADER h = ObpFindInDirectory(dir, seg, seglen);
        if (!h)
            return (*p == '\\') ? STATUS_OBJECT_PATH_NOT_FOUND
                                : STATUS_OBJECT_NAME_NOT_FOUND;

        if (*p == '\0') {
            /* Final segment: return the object with an extra reference. */
            POBJECT obj = ObObjectFromHeader(h);
            ObReferenceObject(obj);
            *out_object = obj;
            return STATUS_SUCCESS;
        }

        /* Intermediate segment must be a directory to descend into. */
        if (h->Type != ObpDirectoryType)
            return STATUS_OBJECT_PATH_NOT_FOUND;
        dir = (OBJECT_DIRECTORY *)ObObjectFromHeader(h);
        p++; /* skip the '\' */
    }
}
