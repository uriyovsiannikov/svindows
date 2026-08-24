/*
 * ob/ob_handle.c - the kernel handle table.
 *
 * A handle is an index into a table of (object, granted-access) pairs, encoded
 * as (index + 1) * 4 so that the NULL handle (0) is always invalid and handle
 * values are multiples of four, matching NT. There is one global table for now;
 * per-process handle tables arrive with the process manager (Ps).
 *
 * A handle holds one pointer reference on its object, plus a handle count.
 * Closing the handle (or the object's last handle going away) releases them.
 */
#include <ntos/ob.h>
#include <ntos/ke.h>
#include <ntos/rtl.h>
#include "obp.h"

#define OB_MAX_HANDLES 4096

typedef struct _HANDLE_ENTRY {
    POBJECT     Object;   /* NULL when the slot is free */
    ACCESS_MASK GrantedAccess;
} HANDLE_ENTRY;

static HANDLE_ENTRY g_handles[OB_MAX_HANDLES];

static ALWAYS_INLINE HANDLE index_to_handle(UINT32 index)
{
    return (HANDLE)(ULONG_PTR)((index + 1) * 4);
}

/* Returns TRUE and the index on success. */
static BOOLEAN handle_to_index(HANDLE handle, UINT32 *out_index)
{
    ULONG_PTR value = (ULONG_PTR)handle;
    if (value == 0 || (value & 3) != 0)
        return FALSE;
    ULONG_PTR index = value / 4 - 1;
    if (index >= OB_MAX_HANDLES)
        return FALSE;
    *out_index = (UINT32)index;
    return TRUE;
}

void ObInitializeHandleTable(void)
{
    memset(g_handles, 0, sizeof(g_handles));
    KeLog("[ob]   handle table: %u slots\n", (unsigned)OB_MAX_HANDLES);
}

NTSTATUS ObCreateHandle(POBJECT object, ACCESS_MASK access, HANDLE *out_handle)
{
    if (!object || !out_handle)
        return STATUS_INVALID_PARAMETER;

    for (UINT32 i = 0; i < OB_MAX_HANDLES; i++) {
        if (g_handles[i].Object == NULL) {
            POBJECT_HEADER header = ObHeaderFromObject(object);

            g_handles[i].Object = object;
            g_handles[i].GrantedAccess = access;

            ObReferenceObject(object);                 /* handle's pointer ref */
            InterlockedIncrement(&header->HandleCount);
            header->Type->TotalHandles++;

            *out_handle = index_to_handle(i);
            return STATUS_SUCCESS;
        }
    }
    /* Exhaustion is a silent killer: every later Nt*Create/Open returns a NULL
     * handle and the caller behaves as if the object never existed. Report it
     * once, with a per-type census so the leaking object type is obvious. */
    static BOOLEAN reported;
    if (!reported) {
        reported = TRUE;
        KeLog("[ob]   handle table exhausted (%u slots); census:\n",
              (unsigned)OB_MAX_HANDLES);
        for (UINT32 i = 0; i < OB_MAX_HANDLES; i++) {
            POBJECT_TYPE type = ObHeaderFromObject(g_handles[i].Object)->Type;
            BOOLEAN counted = FALSE;
            for (UINT32 j = 0; j < i; j++)
                if (ObHeaderFromObject(g_handles[j].Object)->Type == type)
                    counted = TRUE;
            if (counted)
                continue;
            UINT32 n = 0;
            for (UINT32 j = 0; j < OB_MAX_HANDLES; j++)
                if (ObHeaderFromObject(g_handles[j].Object)->Type == type)
                    n++;
            KeLog("[ob]     %s: %u handle(s)\n", type->Name, (unsigned)n);
        }
    }
    return STATUS_INSUFFICIENT_RESOURCES;
}

NTSTATUS ObReferenceObjectByHandle(HANDLE handle, ACCESS_MASK desired,
                                   POBJECT_TYPE type, POBJECT *out_object)
{
    UINT32 index;
    if (!out_object || !handle_to_index(handle, &index))
        return STATUS_INVALID_HANDLE;

    HANDLE_ENTRY *entry = &g_handles[index];
    if (entry->Object == NULL)
        return STATUS_INVALID_HANDLE;

    POBJECT_HEADER header = ObHeaderFromObject(entry->Object);
    if (type && header->Type != type)
        return STATUS_OBJECT_TYPE_MISMATCH;

    /* Until per-type GENERIC_MAPPING exists, treat GENERIC_ALL as full access. */
    ACCESS_MASK granted = entry->GrantedAccess;
    if (granted & GENERIC_ALL)
        granted = (ACCESS_MASK)~0u;
    if (desired && (granted & desired) != desired)
        return STATUS_ACCESS_DENIED;

    ObReferenceObject(entry->Object);
    *out_object = entry->Object;
    return STATUS_SUCCESS;
}

NTSTATUS ObCloseHandle(HANDLE handle)
{
    UINT32 index;
    if (!handle_to_index(handle, &index))
        return STATUS_INVALID_HANDLE;

    HANDLE_ENTRY *entry = &g_handles[index];
    POBJECT object = entry->Object;
    if (object == NULL)
        return STATUS_INVALID_HANDLE;

    POBJECT_HEADER header = ObHeaderFromObject(object);

    entry->Object = NULL;
    entry->GrantedAccess = 0;

    InterlockedDecrement(&header->HandleCount);
    if (header->Type->TotalHandles)
        header->Type->TotalHandles--;

    ObDereferenceObject(object); /* release the handle's pointer ref */
    return STATUS_SUCCESS;
}
